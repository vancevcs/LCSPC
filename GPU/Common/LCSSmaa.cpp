// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cstdlib>
#include <cstring>

#include "Common/Log.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/ShaderWriter.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSSettings.h"
#include "GPU/GPUState.h"
#include "GPU/Common/LCSSmaa.h"

namespace LCSSmaa {

using namespace Draw;

static bool s_setupFailed = false;
static int s_debugView = 0;

// Armed by 3D drawn into the game's own framebuffer, fired by the 2D draw after it. Not "once per
// host frame": VCS sometimes gets a second frame into one host frame, the whole scene drawn again
// into the same framebuffer after the HUD of the first, and a pass that had already fired would
// leave the frame actually shown unsmoothed. Nor "the first 2D draw of the frame into the game's
// framebuffer", which is what VCS's first did, and in play nothing of it reached the screen,
// because the world was drawn over it afterwards.
static bool s_worldDrawn = false;
static int s_runsThisFrame = 0;
// LCS draws its world in pieces with 2D between them: three 3D-to-2D turns every host frame, steady
// (measured: 900 runs in 300 frames before this), and the HUD comes after the last. Running at each
// turn smoothed two half-drawn worlds and the first pieces twice. So the turns are counted, and the
// pass runs from the turn the previous frame ended on: once a frame, at the HUD seam, and still at
// the last turn of a frame that has more.
static int s_turnsThisFrame = 0;
static int s_turnsLastFrame = 0;
static int s_multiRunFrames = 0;
// With LCS_SMAA_DEBUG set, the runs over each 300 host frames go to the log: how often the world
// comes in pieces, which decides what the pass costs.
static bool s_logRate = false;
static int s_rateFrames = 0;
static int s_rateRuns = 0;
static int s_loggedWidth = 0;
static int s_loggedHeight = 0;

static Framebuffer *s_colorFbo;   // the frame as the game left it at the seam
static Framebuffer *s_edgeFbo;    // r: an edge against the left neighbour, g: against the one above
static Framebuffer *s_blendFbo;   // the four areas each edge hands out - see the blend pass
static int s_width;
static int s_height;

static Pipeline *s_edgePipeline;
static Pipeline *s_blendPipeline;
static Pipeline *s_resolvePipeline;
static SamplerState *s_pointSampler;

// SMAA's own defaults for the two edge-detection numbers: a luma step of a tenth, and an edge kept
// only if it is at least half the strongest step around it - the local contrast adaptation that
// stops the soft side of a hard edge from being found as an edge of its own.
static constexpr float kThreshold = 0.1f;
static constexpr float kContrastAdaptation = 2.0f;
// How far along an edge the search walks for its ends, in pixels, each way. At the high internal
// resolutions this fork usually runs at, a step of a staircase is several pixels long, so this is
// SMAA HIGH's distance rather than LOW's.
static constexpr int kMaxSearch = 24;

struct SmaaUB {
	float texel[4];   // 1/width, 1/height, width, height
	float params[4];  // threshold, contrast adaptation, max search, debug view
};
static const UniformBufferDesc s_ubDesc{ sizeof(SmaaUB), {
	{ "u_texel", 0, 0, UniformType::FLOAT4, 0 },
	{ "u_params", 1, 1, UniformType::FLOAT4, 16 },
} };
static const UniformDef kUniforms[] = {
	{ "vec4", "u_texel", 0 },
	{ "vec4", "u_params", 1 },
};
static const VaryingDef kVaryings[] = {
	{ "vec2", "v_uv", SEM_TEXCOORD0, 0, "highp" },
};
static const SamplerDef kEdgeSamplers[] = {
	{ 0, "colorTex", SamplerFlags(0) },
};
static const SamplerDef kBlendSamplers[] = {
	{ 0, "edgeTex", SamplerFlags(0) },
};
static const SamplerDef kResolveSamplers[] = {
	{ 0, "colorTex", SamplerFlags(0) },
	{ 1, "blendTex", SamplerFlags(0) },
};

static void ReleaseSized() {
	for (Framebuffer **fb : { &s_colorFbo, &s_edgeFbo, &s_blendFbo }) {
		if (*fb) {
			(*fb)->Release();
			*fb = nullptr;
		}
	}
	s_width = 0;
	s_height = 0;
}

static void ReleasePipelines() {
	for (Pipeline **p : { &s_edgePipeline, &s_blendPipeline, &s_resolvePipeline }) {
		if (*p) {
			(*p)->Release();
			*p = nullptr;
		}
	}
	if (s_pointSampler) {
		s_pointSampler->Release();
		s_pointSampler = nullptr;
	}
}

static ShaderModule *Compile(DrawContext *draw, ShaderStage stage, const char *code, const char *tag) {
	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	ShaderModule *module = draw->CreateShaderModule(stage, lang.shaderLanguage,
		(const uint8_t *)code, strlen(code), tag);
	if (!module) {
		ERROR_LOG(Log::G3D, "LCS SMAA: %s failed to compile:\n%s", tag, code);
	}
	return module;
}

// The area a reconstructed silhouette covers inside this pixel, for an edge whose ends lie d1 pixels
// before it and d2 after, with the line at height h1 at the first end and h2 at the second - +0.5
// where the end turns away into the OTHER pixel's side, -0.5 into this one's, 0 where it turns both
// ways or neither. The MLAA cases: Z (opposite signs) and L (one zero) are one straight line across
// the whole length; U (the same sign both ends) is two, meeting at zero in the middle, or the line
// would sit half a pixel off the edge all the way along it.
//
// Integrated at four points across the pixel rather than in closed form: it is only ever run on
// edge pixels, it handles the U split and the zero crossing with no cases, and four samples of a
// straight line are exact to within a sixty-fourth.
//
// x is how much of THIS pixel takes the other side's colour; y how much of the OTHER pixel takes
// this one's.
static void WriteAreaFunction(ShaderWriter &writer) {
	writer.C("vec2 smaaArea(float h1, float h2, float d1, float d2) {\n");
	writer.C("  float len = d1 + d2 + 1.0;\n");
	writer.C("  float mid = 0.5 * len;\n");
	writer.C("  bool ushape = h1 == h2 && h1 != 0.0;\n");
	writer.C("  vec2 a = vec2(0.0);\n");
	writer.C("  for (int k = 0; k < 4; k++) {\n");
	writer.C("    float s = d1 + (float(k) + 0.5) * 0.25;\n");
	writer.C("    float h = h1 + (h2 - h1) * (s / len);\n");
	writer.C("    if (ushape) { h = s < mid ? h1 * (1.0 - s / mid) : h2 * ((s - mid) / mid); }\n");
	writer.C("    a += vec2(max(-h, 0.0), max(h, 0.0));\n");
	writer.C("  }\n");
	writer.C("  return a * 0.25;\n");
	writer.C("}\n");
}

static bool EnsurePipelines(DrawContext *draw) {
	if (s_edgePipeline && s_blendPipeline && s_resolvePipeline) {
		return true;
	}
	if (s_setupFailed) {
		return false;
	}
	ReleasePipelines();
	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	const size_t kSize = 12288;
	char *code = new char[kSize];

	// One full-screen triangle for every stage.
	{
		ShaderWriter writer(code, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec2", "a_position", SEM_POSITION } };
		writer.BeginVSMain(inputs, kUniforms, kVaryings);
		writer.C("  v_uv = a_position * 0.5 + 0.5;\n");
		writer.C("  gl_Position = vec4(a_position, 0.0, 1.0);\n");
		writer.EndVSMain(kVaryings);
	}
	ShaderModule *vs = Compile(draw, ShaderStage::Vertex, code, "lcs_smaa_vs");

	// 1. EDGES, on luma, against the left and the upper neighbour. SMAA's local contrast adaptation:
	// an edge survives only if its step is at least half the strongest step in the neighbourhood.
	{
		ShaderWriter writer(code, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(kEdgeSamplers);
		writer.BeginFSMain(kUniforms, kVaryings);
		writer.C("  vec3 W = vec3(0.2126, 0.7152, 0.0722);\n");
		writer.C("  vec2 t = u_texel.xy;\n");
		writer.C("  float L = dot(").SampleTexture2D("colorTex", "v_uv").C(".rgb, W);\n");
		writer.C("  float Ll = dot(").SampleTexture2D("colorTex", "v_uv + vec2(-t.x, 0.0)").C(".rgb, W);\n");
		writer.C("  float Lt = dot(").SampleTexture2D("colorTex", "v_uv + vec2(0.0, -t.y)").C(".rgb, W);\n");
		writer.C("  vec2 d = abs(vec2(L) - vec2(Ll, Lt));\n");
		writer.C("  vec2 e = step(vec2(u_params.x), d);\n");
		writer.C("  vec4 outColor = vec4(0.0);\n");
		writer.C("  if (e.x + e.y > 0.0) {\n");
		writer.C("    float Lr = dot(").SampleTexture2D("colorTex", "v_uv + vec2(t.x, 0.0)").C(".rgb, W);\n");
		writer.C("    float Lb = dot(").SampleTexture2D("colorTex", "v_uv + vec2(0.0, t.y)").C(".rgb, W);\n");
		writer.C("    vec2 m = max(d, abs(vec2(L) - vec2(Lr, Lb)));\n");
		writer.C("    float Lll = dot(").SampleTexture2D("colorTex", "v_uv + vec2(-2.0 * t.x, 0.0)").C(".rgb, W);\n");
		writer.C("    float Ltt = dot(").SampleTexture2D("colorTex", "v_uv + vec2(0.0, -2.0 * t.y)").C(".rgb, W);\n");
		writer.C("    m = max(m, abs(vec2(Ll, Lt) - vec2(Lll, Ltt)));\n");
		writer.C("    float fm = max(m.x, m.y);\n");
		writer.C("    e *= step(vec2(fm), u_params.y * d);\n");
		writer.C("    outColor = vec4(e, 0.0, 1.0);\n");
		writer.C("  }\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *edgeFS = Compile(draw, ShaderStage::Fragment, code, "lcs_smaa_edge_fs");

	// 2. WEIGHTS. For each edge this pixel has, walk along it both ways to its ends, look at which
	// way each end turns, and hand out the area the silhouette line covers. xy for the edge above:
	// x is what this pixel takes from the one above, y what the one above takes from this. zw the
	// same for the edge to the left.
	{
		ShaderWriter writer(code, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(kBlendSamplers);
		WriteAreaFunction(writer);
		writer.BeginFSMain(kUniforms, kVaryings);
		writer.C("  vec2 t = u_texel.xy;\n");
		writer.C("  int maxSearch = int(u_params.z);\n");
		writer.C("  vec2 e = ").SampleTexture2D("edgeTex", "v_uv").C(".rg;\n");
		writer.C("  vec4 w = vec4(0.0);\n");
		// The edge above: runs left-right.
		writer.C("  if (e.y > 0.5) {\n");
		writer.C("    float d1 = 0.0; float d2 = 0.0;\n");
		writer.C("    for (int i = 1; i <= 64; i++) {\n");
		writer.C("      if (i > maxSearch) break;\n");
		writer.C("      if (").SampleTexture2D("edgeTex", "v_uv + vec2(-float(i) * t.x, 0.0)").C(".g < 0.5) break;\n");
		writer.C("      d1 = float(i);\n");
		writer.C("    }\n");
		writer.C("    for (int i = 1; i <= 64; i++) {\n");
		writer.C("      if (i > maxSearch) break;\n");
		writer.C("      if (").SampleTexture2D("edgeTex", "v_uv + vec2(float(i) * t.x, 0.0)").C(".g < 0.5) break;\n");
		writer.C("      d2 = float(i);\n");
		writer.C("    }\n");
		// Which way each end turns: the vertical edge at the end, in this row (into this pixel's
		// side, -0.5) or the row above (into the other side, +0.5). An end the search ran out on
		// is taken as turning neither way.
		writer.C("    vec2 lo = v_uv + vec2(-d1 * t.x, 0.0);\n");
		writer.C("    vec2 ro = v_uv + vec2((d2 + 1.0) * t.x, 0.0);\n");
		writer.C("    float h1 = 0.5 * (").SampleTexture2D("edgeTex", "lo + vec2(0.0, -t.y)").C(".r - ").SampleTexture2D("edgeTex", "lo").C(".r);\n");
		writer.C("    float h2 = 0.5 * (").SampleTexture2D("edgeTex", "ro + vec2(0.0, -t.y)").C(".r - ").SampleTexture2D("edgeTex", "ro").C(".r);\n");
		writer.C("    if (d1 >= float(maxSearch)) h1 = 0.0;\n");
		writer.C("    if (d2 >= float(maxSearch)) h2 = 0.0;\n");
		writer.C("    w.xy = smaaArea(h1, h2, d1, d2);\n");
		writer.C("  }\n");
		// The edge to the left: runs up-down.
		writer.C("  if (e.x > 0.5) {\n");
		writer.C("    float d1 = 0.0; float d2 = 0.0;\n");
		writer.C("    for (int i = 1; i <= 64; i++) {\n");
		writer.C("      if (i > maxSearch) break;\n");
		writer.C("      if (").SampleTexture2D("edgeTex", "v_uv + vec2(0.0, -float(i) * t.y)").C(".r < 0.5) break;\n");
		writer.C("      d1 = float(i);\n");
		writer.C("    }\n");
		writer.C("    for (int i = 1; i <= 64; i++) {\n");
		writer.C("      if (i > maxSearch) break;\n");
		writer.C("      if (").SampleTexture2D("edgeTex", "v_uv + vec2(0.0, float(i) * t.y)").C(".r < 0.5) break;\n");
		writer.C("      d2 = float(i);\n");
		writer.C("    }\n");
		writer.C("    vec2 to = v_uv + vec2(0.0, -d1 * t.y);\n");
		writer.C("    vec2 bo = v_uv + vec2(0.0, (d2 + 1.0) * t.y);\n");
		writer.C("    float h1 = 0.5 * (").SampleTexture2D("edgeTex", "to + vec2(-t.x, 0.0)").C(".g - ").SampleTexture2D("edgeTex", "to").C(".g);\n");
		writer.C("    float h2 = 0.5 * (").SampleTexture2D("edgeTex", "bo + vec2(-t.x, 0.0)").C(".g - ").SampleTexture2D("edgeTex", "bo").C(".g);\n");
		writer.C("    if (d1 >= float(maxSearch)) h1 = 0.0;\n");
		writer.C("    if (d2 >= float(maxSearch)) h2 = 0.0;\n");
		writer.C("    w.zw = smaaArea(h1, h2, d1, d2);\n");
		writer.C("  }\n");
		writer.C("  vec4 outColor = w;\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *blendFS = Compile(draw, ShaderStage::Fragment, code, "lcs_smaa_blend_fs");

	// 3. RESOLVE. Gather the four areas that concern this pixel - its own two, the one below's
	// "above" share and the one to the right's "left" share - and blend along whichever direction
	// carries more, as SMAA does: mixing both at a corner smears it.
	{
		ShaderWriter writer(code, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(kResolveSamplers);
		writer.BeginFSMain(kUniforms, kVaryings);
		writer.C("  vec2 t = u_texel.xy;\n");
		writer.C("  vec4 own = ").SampleTexture2D("blendTex", "v_uv").C(";\n");
		writer.C("  float aUp = own.x;\n");
		writer.C("  float aLeft = own.z;\n");
		writer.C("  float aDown = ").SampleTexture2D("blendTex", "v_uv + vec2(0.0, t.y)").C(".y;\n");
		writer.C("  float aRight = ").SampleTexture2D("blendTex", "v_uv + vec2(t.x, 0.0)").C(".w;\n");
		writer.C("  vec3 c0 = ").SampleTexture2D("colorTex", "v_uv").C(".rgb;\n");
		writer.C("  vec3 c = c0;\n");
		writer.C("  float hm = max(aUp, aDown);\n");
		writer.C("  float vm = max(aLeft, aRight);\n");
		writer.C("  if (hm >= vm && hm > 0.0) {\n");
		writer.C("    float s = max(aUp + aDown, 1.0);\n");
		writer.C("    vec3 up = ").SampleTexture2D("colorTex", "v_uv + vec2(0.0, -t.y)").C(".rgb;\n");
		writer.C("    vec3 dn = ").SampleTexture2D("colorTex", "v_uv + vec2(0.0, t.y)").C(".rgb;\n");
		writer.C("    c = c * (1.0 - (aUp + aDown) / s) + up * (aUp / s) + dn * (aDown / s);\n");
		writer.C("  } else if (vm > 0.0) {\n");
		writer.C("    float s = max(aLeft + aRight, 1.0);\n");
		writer.C("    vec3 lf = ").SampleTexture2D("colorTex", "v_uv + vec2(-t.x, 0.0)").C(".rgb;\n");
		writer.C("    vec3 rt = ").SampleTexture2D("colorTex", "v_uv + vec2(t.x, 0.0)").C(".rgb;\n");
		writer.C("    c = c * (1.0 - (aLeft + aRight) / s) + lf * (aLeft / s) + rt * (aRight / s);\n");
		writer.C("  }\n");
		writer.C("  if (u_params.w > 0.5 && u_params.w < 1.5 && v_uv.x > 0.5) c = c0;\n");
		writer.C("  if (u_params.w > 1.5) c = 2.0 * vec3(aUp + aDown, aLeft + aRight, 0.0);\n");
		writer.C("  vec4 outColor = vec4(c, 1.0);\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *resolveFS = Compile(draw, ShaderStage::Fragment, code, "lcs_smaa_resolve_fs");
	delete[] code;

	if (!vs || !edgeFS || !blendFS || !resolveFS) {
		for (ShaderModule *m : { vs, edgeFS, blendFS, resolveFS }) {
			if (m) {
				m->Release();
			}
		}
		s_setupFailed = true;
		return false;
	}

	static const InputLayoutDesc quadLayout = {
		8,
		{ { SEM_POSITION, DataFormat::R32G32_FLOAT, 0 } },
	};
	InputLayout *quadIL = draw->CreateInputLayout(quadLayout);
	DepthStencilStateDesc noDepthDesc{};
	noDepthDesc.depthCompare = Comparison::ALWAYS;
	DepthStencilState *noDepth = draw->CreateDepthStencilState(noDepthDesc);
	BlendState *opaque = draw->CreateBlendState({ false, 0xF });
	// Colour only on the way back into the game's framebuffer: the game keeps things in its alpha.
	BlendState *colourOnly = draw->CreateBlendState({ false, 0x7 });
	RasterState *raster = draw->CreateRasterState({});

	SamplerStateDesc point{};
	point.magFilter = TextureFilter::NEAREST;
	point.minFilter = TextureFilter::NEAREST;
	point.mipFilter = TextureFilter::NEAREST;
	point.wrapU = TextureAddressMode::CLAMP_TO_EDGE;
	point.wrapV = TextureAddressMode::CLAMP_TO_EDGE;
	point.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
	s_pointSampler = draw->CreateSamplerState(point);

	PipelineDesc edgeDesc{ Primitive::TRIANGLE_LIST, { vs, edgeFS }, quadIL, noDepth, opaque, raster,
		&s_ubDesc, kEdgeSamplers };
	s_edgePipeline = draw->CreateGraphicsPipeline(edgeDesc, "lcs_smaa_edge");
	PipelineDesc blendDesc{ Primitive::TRIANGLE_LIST, { vs, blendFS }, quadIL, noDepth, opaque, raster,
		&s_ubDesc, kBlendSamplers };
	s_blendPipeline = draw->CreateGraphicsPipeline(blendDesc, "lcs_smaa_blend");
	PipelineDesc resolveDesc{ Primitive::TRIANGLE_LIST, { vs, resolveFS }, quadIL, noDepth, colourOnly,
		raster, &s_ubDesc, kResolveSamplers };
	s_resolvePipeline = draw->CreateGraphicsPipeline(resolveDesc, "lcs_smaa_resolve");

	for (ShaderModule *m : { vs, edgeFS, blendFS, resolveFS }) {
		m->Release();
	}
	quadIL->Release();
	noDepth->Release();
	opaque->Release();
	colourOnly->Release();
	raster->Release();

	if (!s_edgePipeline || !s_blendPipeline || !s_resolvePipeline || !s_pointSampler) {
		ERROR_LOG(Log::G3D, "LCS SMAA: pipelines failed to create");
		ReleasePipelines();
		s_setupFailed = true;
		return false;
	}
	return true;
}

static bool EnsureSized(DrawContext *draw, int width, int height) {
	if (s_colorFbo && s_edgeFbo && s_blendFbo && s_width == width && s_height == height) {
		return true;
	}
	ReleaseSized();
	s_colorFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, false, "lcs_smaa_color" });
	s_edgeFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, false, "lcs_smaa_edges" });
	s_blendFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, false, "lcs_smaa_weights" });
	if (!s_colorFbo || !s_edgeFbo || !s_blendFbo) {
		ReleaseSized();
		return false;
	}
	s_width = width;
	s_height = height;
	return true;
}

static void DrawFullscreen(DrawContext *draw) {
	static const float kTriangle[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	draw->DrawUP(kTriangle, 3);
}

bool OnFlush(DrawContext *draw, bool through, Framebuffer *target, int bufferWidth, int bufferHeight) {
	if (!IsActive() || !draw || !target) {
		return false;
	}
	// The game's own framebuffer, not one of the small targets it renders reflections and the like
	// into.
	if (gstate_c.curRTWidth < 480 || gstate_c.curRTHeight < 272) {
		return false;
	}
	if (!through) {
		s_worldDrawn = true;
		return false;
	}
	if (!s_worldDrawn) {
		return false;
	}
	s_worldDrawn = false;
	s_turnsThisFrame++;
	if (s_turnsThisFrame < s_turnsLastFrame) {
		return false;
	}
	if (!LCS::IsActive() || !EnsurePipelines(draw)) {
		return false;
	}
	int width = 0, height = 0;
	draw->GetFramebufferDimensions(target, &width, &height);
	if (width <= 0 || height <= 0 || !EnsureSized(draw, width, height)) {
		return false;
	}

	SmaaUB ub{};
	ub.texel[0] = 1.0f / (float)width;
	ub.texel[1] = 1.0f / (float)height;
	ub.texel[2] = (float)width;
	ub.texel[3] = (float)height;
	ub.params[0] = kThreshold;
	ub.params[1] = kContrastAdaptation;
	ub.params[2] = (float)kMaxSearch;
	ub.params[3] = (float)s_debugView;

	const Viewport viewport{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };

	draw->BlitFramebuffer(target, 0, 0, width, height, s_colorFbo, 0, 0, width, height,
		Aspect::COLOR_BIT, FB_BLIT_NEAREST, "lcs_smaa_copy");

	draw->BindFramebufferAsRenderTarget(s_edgeFbo,
		{ RPAction::CLEAR, RPAction::DONT_CARE, RPAction::DONT_CARE, 0, 1.0f, 0, "lcs_smaa_edges" },
		"lcs_smaa_edges");
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, width, height);
	draw->BindPipeline(s_edgePipeline);
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	draw->BindFramebufferAsTexture(s_colorFbo, 0, Aspect::COLOR_BIT, 0);
	draw->BindSamplerStates(0, 1, &s_pointSampler);
	DrawFullscreen(draw);

	draw->BindFramebufferAsRenderTarget(s_blendFbo,
		{ RPAction::CLEAR, RPAction::DONT_CARE, RPAction::DONT_CARE, 0, 1.0f, 0, "lcs_smaa_weights" },
		"lcs_smaa_weights");
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, width, height);
	draw->BindPipeline(s_blendPipeline);
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	draw->BindFramebufferAsTexture(s_edgeFbo, 0, Aspect::COLOR_BIT, 0);
	draw->BindSamplerStates(0, 1, &s_pointSampler);
	DrawFullscreen(draw);

	draw->BindFramebufferAsRenderTarget(target,
		{ RPAction::KEEP, RPAction::KEEP, RPAction::KEEP, 0, 0.0f, 0, "lcs_smaa_resolve" },
		"lcs_smaa_resolve");
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, width, height);
	draw->BindPipeline(s_resolvePipeline);
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	draw->BindFramebufferAsTexture(s_colorFbo, 0, Aspect::COLOR_BIT, 0);
	draw->BindFramebufferAsTexture(s_blendFbo, 1, Aspect::COLOR_BIT, 0);
	SamplerState *samplers[2] = { s_pointSampler, s_pointSampler };
	draw->BindSamplerStates(0, 2, samplers);
	DrawFullscreen(draw);

	s_runsThisFrame++;
	if (width != s_loggedWidth || height != s_loggedHeight) {
		s_loggedWidth = width;
		s_loggedHeight = height;
		NOTICE_LOG(Log::G3D, "LCS SMAA: running at %dx%d", width, height);
	}
	return true;
}

void Init() {
	s_worldDrawn = false;
	s_setupFailed = false;
	s_loggedWidth = 0;
	s_loggedHeight = 0;
	if (const char *view = getenv("LCS_SMAA_DEBUG")) {
		s_debugView = atoi(view);
		s_logRate = true;
		NOTICE_LOG(Log::G3D, "LCS SMAA: debug view %d", s_debugView);
	}
}

void Shutdown() {
	ReleasePipelines();
	ReleaseSized();
}

void DeviceLost() {
	ReleasePipelines();
	ReleaseSized();
	s_setupFailed = false;
}

void BeginFrame() {
	// A run that is not the frame's only one is the second-frame case above, or a world drawn in
	// pieces with 2D between them, which would smooth the first piece twice. Counted so which of
	// the two it is can be read off the log rather than assumed.
	if (s_runsThisFrame > 1) {
		s_multiRunFrames++;
		if (s_multiRunFrames <= 3 || s_multiRunFrames % 1000 == 0) {
			NOTICE_LOG(Log::G3D, "LCS SMAA: ran %d times in one host frame (%d such frames)",
				s_runsThisFrame, s_multiRunFrames);
		}
	}
	s_turnsLastFrame = s_turnsThisFrame;
	s_turnsThisFrame = 0;
	if (s_logRate) {
		s_rateFrames++;
		s_rateRuns += s_runsThisFrame;
		if (s_rateFrames == 300) {
			NOTICE_LOG(Log::G3D, "LCS SMAA: %d runs in 300 host frames", s_rateRuns);
			s_rateFrames = 0;
			s_rateRuns = 0;
		}
	}
	s_runsThisFrame = 0;
	s_worldDrawn = false;
}

bool IsActive() {
	return LCS::IsActive() && LCS::Settings().antiAliasing;
}

int &DebugView() {
	return s_debugView;
}

}  // namespace LCSSmaa
