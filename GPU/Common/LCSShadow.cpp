// Copyright (c) 2026- PPSSPP Project.

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

// LCS: ported from the VCS fork's GPU/Common/VCSShadow (its docs/vcs/shadows.md has the design and
// every measurement). The notes below are VCS's, made on Vice City Stories, unless they say LCS:
// the passes only read the GE (the sun is the light the game hands it), so they carried over, and
// what they read of the game itself goes through Core/LCS (LCS::ReadWeather, LCS::CameraWorld).
// docs/lcs/shadows-and-water.md has what was measured on Liberty City Stories.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/CommonFuncs.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/ShaderWriter.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/LCS/LCSState.h"
#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Common/LCSShadow.h"
#include "GPU/Common/LCSWater.h"
#include "GPU/Common/VertexDecoderCommon.h"

namespace LCSShadow {

bool g_available = false;
bool g_active = false;
// Off until switched on. This is an experiment inside a working emulator: the cost of it
// being on by default is paid by someone who did not ask for it.
static bool s_enabled = false;

static FrameStats s_current;
static FrameStats s_published;

// Luminance of the brightest directional light seen so far this frame. Kept out of FrameStats
// because it is scratch for picking the sun, not something worth showing.
static float s_sunLuminance;

// Roughly three degrees. Below this there is no useful sun and no useful shadow.
static const float kMinSunElevation = 0.05f;

static ShadowView s_view;
static Settings s_settings = {
	// Designated by position, so the ORDER HERE MUST MATCH THE STRUCT. It did not, once, and the
	// symptom was not a compile error: cacheHoldSeconds took a bool and the cache expired every
	// frame while every count in the panel looked healthy.
	85.0f,    // cascadeRadius, world units. GTA is roughly one unit to the metre, so this is a
	          // couple of blocks around the player. It was 50, which is fine on foot and too
	          // close in a car: shadows arrived a second before you reached them. 70 while the box
	          // sat 40 units ahead of the view; 85 now it is centred on the camera at rest - see
	          // CascadeLead - so a street seen from standing still keeps most of its reach.
	40.0f,    // centreDistance - the most the box may lead the camera along its TRAVEL
	18.0f,    // nearCascadeRadius - the sharp half, about 0.9cm a texel at 4096. 0 disables
	          // the split and halves the atlas back to one tile
	300.0f,   // casterReach - how far up-sun a caster can be and still reach the cascade
	4096,     // mapSize. 70 units across 4096 texels is about 3.4cm a texel - and it is a
	          // depth-only pass over geometry that is already in a buffer, so the cost of
	          // doubling it is rasterisation nobody was short of rather than memory.
	true,     // forwardIsNegativeZ
	1.0f,     // maskScale - the game's own render resolution
	0.0008f,  // depthBias - small, because the back-face rule does the work a bias used to
	1.5f,     // slopeBias
	1,        // pcfRadius
	0.15f,    // edgeFade
	0.7f,     // strength
	{ 0.35f, 0.38f, 0.48f },  // tint - blue, because what fills a shadow outdoors is the sky
	true,     // cacheCasters
	0.0f,     // cacheHoldSeconds - forever. Nothing that moves is remembered, so the radius is
	          // the only thing that has to bound this cache
	350.0f,   // cacheRadius - has to hold everything that can cast INTO the cascade, which
	          // reaches 300 units up-sun, not just what is near the camera
	150.0f,   // cacheReplaceRadius - comfortably inside the ~266 units of full detail the level
	          // archives bake around each streamed cell, so what the game draws this close is
	          // the whole building rather than its stand-in
	true,     // castBackFacesOnly
	false,    // flipCasterWinding
	0.35f,    // moonStrength
	0.26f,    // lowSunFade - about 15 degrees
	false,    // hideInRain - the puddles mirroring the shadows, which is what it hid, is fixed in
	          // LCSWater (the road pass divides the composite back out of its reflection)
	2.0f,     // rainFadeSeconds
	0.30f,    // rainShadowsReturnWetness - was effectively 0.1, and read as "only once every puddle is gone"
	true,     // hideBlobShadows
	2.0f,     // nearCameraCutoff
	400.0f,   // maxCasterSpan
	4.0f,     // propMaxSpan, and this one is TUNED BY EYE rather than measured. Six metres
	          // put 283 props among 307 casters on one street, which is not bins, it is
	          // building segments getting through; 2.5 caught nothing at all. The count is
	          // a poor guide either way because AddCaster sees one FLUSH, not one object,
	          // so a batch of three lamp posts has a box three lamp posts wide. Drag
	          // 'Prop footprint' on the Shadows tab and watch the prop count: hundreds
	          // means buildings, zero means nothing qualifies, a few dozen is a street.
	2.0f,     // propMinHeight - taller than any decal, shorter than the shortest lamp post
	false,    // entityCastersOnly - everything casts, which is what a PC port is for
	false,    // propCasters - only consulted while entityCastersOnly is on
	true,     // cutoutEntitiesCast - a wheel is a disc, not a card. See TestDraw.
	true,     // texturedCutouts - palms cast fronds, through the texture. See AddCutoutCaster.
	0.003f,   // pedReceiverBias - about a metre, which clears a body and not a building
	false,    // flipShadowV
	0,        // debugView
	false,    // showMask
	false,    // countCascadeCoverage
	false,    // ambientOcclusion - a prototype, so off until somebody has looked at it
	1.5f,     // aoRadius - a metre and a half: a kerb, the foot of a wall, the gap under a car
	0.6f,     // aoStrength
	1.0f,     // aoIntensity
	0.35f,    // aoInSun
	0.3f,     // aoOnPeople
	0.002f,   // aoBias, per unit of camera distance
	50.0f,    // aoFadeStart
	80.0f,    // aoFadeEnd
	1.0f,     // aoScale - the mask's own resolution
	12,       // aoSamples
	true,     // aoBlur
	8.0f,     // aoBlurSharpness
	false,    // showAO
	false,    // aoSplit
};


// gstate's view matrix as of the first caster of the frame. Captured rather than read at end of
// frame because the game leaves whatever matrix the last draw used in place, and the last draw is
// usually 2D.
static float s_frameViewMatrix[12];
static float s_frameProjMatrix[16];
static bool s_haveViewMatrix;

// Everything between the projection matrix and the pixel, captured from the same draw.
//
// This is what makes the mask line up with the frame instead of merely resembling it. PPSSPP's
// vertex shader does not stop at proj: it divides, applies the PSP's own viewport scale and
// offset, subtracts the raster offset, and remaps into the render target's NDC - and all of that
// is linear in the clip vector, so it folds into one more 4x4 rather than having to be replayed.
// See VertexShaderGenerator.cpp, the block from `float recip` to the `u_xywh` line.
struct ViewportCapture {
	float scale[3];
	float offset[3];
	float rasterOffset[2];
	float rtWidth, rtHeight;          // PSP pixels
	int rtOffsetX, rtOffsetY;
};
static ViewportCapture s_frameViewport;

// The camera, recovered as soon as the view matrix is captured rather than at the end of the
// frame, because the capture itself has to test against it - see nearCameraCutoff.
static float s_frameCameraPos[3];
// ...and the way it faces, for the caster-horizon measure. Taken from the same matrix on the same
// frame rather than from s_view, which is not filled until the seam, long after the draws it would
// have to describe.
static float s_frameCameraFwd[3];

// The direction light travels, as far as the capture knows it so far. The back-face test needs it
// while draws are arriving, and the projection is not built until the frame turns to 2D - so this
// is this frame's sun once one has been seen and last frame's until then. It moves slowly enough
// that the difference cannot decide which side of a triangle faces the light.
static float s_captureLightDir[3];
static bool s_haveCaptureLightDir;
static void NoteGameSun();
static float s_lastCameraPos[3];
static bool s_haveLastCameraPos;
// The game's own camera in WORLD space, as of the same frame - what tells a rebase of the space the
// GE is fed from a camera that really jumped. See RebaseCachedCasters.
static float s_lastCameraWorld[2];
static bool s_haveLastCameraWorld;
static int s_cacheRebases;
static int s_cacheClears;

// The frame's caster geometry. Kept as std::vector so the capacity settles after a few frames
// and then stops allocating - clear() keeps the storage.
static std::vector<float> s_positions;

// thin3d's DrawIndexed is 16-bit only, so the triangle list is cut into batches of under 64k
// vertices with indices relative to each batch's first vertex. A typical frame here captures
// ~30k vertices and so produces exactly one batch; a busy one produces two.
//
// Two index streams over one vertex buffer, because a caster and a receiver are not the same set.
// Everything captured receives; only geometry small enough to be a building casts - see
// maxCasterSpan, and the sky it exists to keep out of the depth map.
struct Batch {
	u32 firstVertex;
	int vertexCount;
	u32 firstCasterIndex;
	int casterIndexCount;
	u32 firstReceiverIndex;
	int receiverIndexCount;
	// People are shaded in a second pass with a heavier bias - see pedReceiverBias. They are
	// a separate index stream rather than a separate vertex buffer, so this costs one more
	// draw call and not one more copy of the geometry.
	u32 firstPedReceiverIndex;
	int pedReceiverIndexCount;
};
static const int kMaxBatchVertices = 65535;
static std::vector<u16> s_casterIndices;
static std::vector<u16> s_receiverIndices;
static std::vector<u16> s_pedReceiverIndices;
static std::vector<Batch> s_batches;
static CaptureStats s_capture;
static CaptureStats s_capturePublished;

// Set once the frame's passes have run, so a frame produces exactly one shadow composite and the
// capture stops growing behind it.
static bool s_frameComposited;

// Host frames since boot. The cache ages in these rather than in seconds so that nothing here
// needs a clock, and a stalled emulator does not silently expire the whole cache.
static int s_frameIndex;

// The weather's share of the shadow strength - see Settings::hideInRain. Starts at 1, so a boot in
// the dry does not fade in.
static float s_weatherFade = 1.0f;
static float s_weatherWetness;
static double s_lastWeatherTime;

static void UpdateWeatherFade() {
	const double now = time_now_d();
	float dt = s_lastWeatherTime > 0.0 ? (float)(now - s_lastWeatherTime) : 0.0f;
	s_lastWeatherTime = now;
	// A loading screen or a breakpoint leaves an arbitrary gap, as it does for LCSWater's soak.
	if (dt < 0.0f) dt = 0.0f;
	if (dt > 0.25f) dt = 0.25f;

	float wet = 0.0f;
	if (LCSWater::IsActive()) {
		// Last frame's - LCSWater's BeginFrame runs after this one. The lagged wetness is the point:
		// the puddles are still reflecting for a minute and a half after the rain stops.
		const LCSWater::FrameStats &w = LCSWater::LastFrameStats();
		wet = std::max(w.rainNorm, w.wetness);
	} else {
		// 0.5 is the heaviest Rain the game produces - LCSWater's kRainFullScale, measured there.
		const LCS::WeatherState w = LCS::ReadWeather();
		wet = w.valid ? w.rain / 0.5f : 0.0f;
	}
	if (!(wet > 0.0f)) {
		wet = 0.0f;   // also catches NaN
	} else if (wet > 1.0f) {
		wet = 1.0f;
	}
	s_weatherWetness = wet;

	// Out above rainShadowsReturnWetness, all the way back at half of it, a straight ramp between.
	float target = 1.0f;
	const float back = s_settings.rainShadowsReturnWetness * 0.5f;
	if (s_settings.hideInRain && wet > back) {
		target = back > 0.0f ? std::max(0.0f, 1.0f - (wet - back) / back) : 0.0f;
	}
	const float step = s_settings.rainFadeSeconds > 0.01f ? dt / s_settings.rainFadeSeconds : 1.0f;
	const float before = s_weatherFade;
	if (s_weatherFade < target) {
		s_weatherFade = std::min(s_weatherFade + step, target);
	} else {
		s_weatherFade = std::max(s_weatherFade - step, target);
	}

	// Said when the shadows are all the way gone and all the way back, so a run can show the rain
	// turned them off rather than leaving it to a screenshot that has no shadow in it.
	static int s_weatherLogs = 0;
	if (s_weatherLogs < 64) {
		if (before > 0.0f && s_weatherFade <= 0.0f) {
			s_weatherLogs++;
			NOTICE_LOG(Log::G3D, "LCS shadows: off for the rain (wetness %.2f)", wet);
		} else if (before < 1.0f && s_weatherFade >= 1.0f) {
			s_weatherLogs++;
			NOTICE_LOG(Log::G3D, "LCS shadows: back, the roads are dry (wetness %.2f)", wet);
		}
	}
}

float WeatherFade() {
	return s_weatherFade;
}

float WeatherWetness() {
	return s_weatherWetness;
}

// Texture alpha scans allowed this frame - see kTexScansPerFrame, far below, for why there is a
// budget at all. Declared up here because BeginFrame resets it.
static int s_texScansThisFrame;

// The textures the game's own blob shadows are drawn with, learned rather than named.
//
// A texture is not believed on one sighting. A quad happening to pass under something once is a
// coincidence; the same texture doing it repeatedly is the game drawing a shadow under everything
// that moves. Without the count the learner filled its whole table in a few seconds, which is
// what over-learning looks like from outside.
static const int kMaxBlobTextures = 16;
static const int kBlobSightingsToLearn = 8;
static u32 s_blobTextures[kMaxBlobTextures];
static int s_blobTextureCount;

static const int kMaxBlobCandidates = 24;
struct BlobCandidate {
	u32 addr;
	int sightings;
};
static BlobCandidate s_blobCandidates[kMaxBlobCandidates];
static int s_blobCandidateCount;

// Set by NoteGroundQuad when the draw it was just handed passes the positional test, and consumed
// by ShouldSkipDraw a moment later on the same draw.
//
// Suppression used to wait for the texture to be LEARNED, which takes kBlobSightingsToLearn
// frames - and every texture address moves when a chunk streams in, so each load showed the
// vanilla blob under the player for those frames before the learner caught up again. Reported as
// "when loading the new chunk, the static shadow sprite shows up for a split second". The
// positional test already knows the answer for THIS draw on the first frame it sees it; the
// learned set is what carries that knowledge to frames where the object above it was not
// captured.
static bool s_blobThisDraw;

// Cumulative, because the per-frame count samples one frame and the question is whether the
// positional path fires AT ALL - it is the one that covers the frames right after a chunk
// load, when every texture address has moved and the learned set is empty.
static int s_blobByPosition;
static int s_blobByTexture;

// This frame's object-sized casters, for the test that decides what a flat quad is lying under.
struct ObjectBounds {
	float min[3];
	float max[3];
};
static std::vector<ObjectBounds> s_objectBounds;

// Scratch for one draw, so a rejected draw costs no growth in the capture buffers. Two index
// lists: everything, which receives, and the half turned away from the light, which casts.
static std::vector<float> s_drawPos;
static std::vector<u16> s_drawIdx;
static std::vector<u16> s_drawCastIdx;
static bool s_drawFlipWinding;

// --- cut-out casters: what the plain pass cannot draw -----------------------------------------
//
// A palm frond is a card whose shape lives in its texture's alpha, and the plain depth pass carries
// no textures - so it could only ever write the card. That was the square shadow, and rejecting
// cut-outs outright was the "fix" that left palms casting nothing. These go through a second
// pipeline that samples the texture and discards the holes.

// What a cut-out piece samples. The view is only trusted inside the frame the game bound it in;
// the address and dimensions are what find the texture again later - see ResolveCutoutTexture.
struct CutoutTex {
	void *view;
	u32 addr;
	u16 dim;
};

// One draw call of the cut-out pass: this frame's pieces sharing a texture, an alpha threshold and
// a wrap mode. Five floats a vertex - position and texture coordinate - and indices relative to the
// group, so a group holds under 64k vertices and a busy texture simply gets a second group.
struct CutoutGroup {
	CutoutTex tex;
	float alphaRef;
	u8 wrap;   // bit 0 clamps S, bit 1 clamps T
	std::vector<float> verts;
	std::vector<u16> idx;
};
static std::vector<CutoutGroup> s_cutGroups;

// Below this much alpha a texel is a hole whatever the game's own test says. A frond drawn with a
// test of "greater than zero" would otherwise cast every faint fringe pixel as solid leaf.
static const float kMinCutoutAlpha = 0.25f;

// The draw baked by AddCutoutCaster, waiting for NoteCutoutTexture to say what it samples.
static std::vector<float> s_cutDrawVerts;
static std::vector<u16> s_cutDrawIdx;
static bool s_cutPending;
static bool s_cutPendingRemember;
static u32 s_cutPendingAddr;
static u16 s_cutPendingDim;
static float s_cutPendingRef;
static u8 s_cutPendingWrap;
static float s_cutPendingCentre[3];

// Scratch for splitting a cut-out draw into its connected pieces - see AddCutoutCaster.
static std::vector<int> s_cutParent;
static std::vector<float> s_cutBox;
static std::vector<u8> s_cutVerdict;

// A frame that wants more than this is not a frame we can help. At 12 bytes a vertex this is
// about 12 MB of positions, against a measured ~38k vertices a frame - roughly 25x headroom, so
// hitting it means something has gone wrong rather than that the scene got busy.
static const size_t kMaxCapturedVertices = 1024 * 1024;

namespace {

inline float Dot(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline void Cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

inline bool Normalize(float v[3]) {
	const float lengthSq = Dot(v, v);
	if (lengthSq < 1e-12f) {
		return false;
	}
	const float invLength = 1.0f / sqrtf(lengthSq);
	v[0] *= invLength;
	v[1] *= invLength;
	v[2] *= invLength;
	return true;
}

// Recovers the camera's world-space position and axes from gstate's 4x3 view matrix.
//
// The view matrix is row-vector and world-to-view, so viewPos = worldPos * R + t with R at
// m[0..8] and t at m[9..11]. R is a rotation, so its inverse is its transpose: the world-space
// direction of view axis c is column c of R, and the camera position is -t * R^T. Getting this
// transposed is the classic way to lose a day, which is why the panel prints the result next to
// the player position rather than trusting it.
void DecomposeViewMatrix(const float m[12], ShadowView *view) {
	view->cameraRight[0] = m[0];
	view->cameraRight[1] = m[3];
	view->cameraRight[2] = m[6];

	view->cameraUp[0] = m[1];
	view->cameraUp[1] = m[4];
	view->cameraUp[2] = m[7];

	float viewZ[3] = { m[2], m[5], m[8] };
	const float sign = s_settings.forwardIsNegativeZ ? -1.0f : 1.0f;
	view->cameraForward[0] = viewZ[0] * sign;
	view->cameraForward[1] = viewZ[1] * sign;
	view->cameraForward[2] = viewZ[2] * sign;

	const float t[3] = { m[9], m[10], m[11] };
	view->cameraPos[0] = -(t[0] * m[0] + t[1] * m[1] + t[2] * m[2]);
	view->cameraPos[1] = -(t[0] * m[3] + t[1] * m[4] + t[2] * m[5]);
	view->cameraPos[2] = -(t[0] * m[6] + t[1] * m[7] + t[2] * m[8]);
}

// Row-major, row-vector: C = A * B, so a point transformed by C is the point through A then B.
void Mul4x4(const float a[16], const float b[16], float out[16]) {
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += a[row * 4 + k] * b[k * 4 + col];
			}
			out[row * 4 + col] = sum;
		}
	}
}

// Builds world-to-light-clip as one row-vector 4x4, so a caster's world position becomes shadow
// clip space with a single multiply and nothing downstream has to know how it was assembled.
//
// The light half is an orthographic box: the sun is far enough away that perspective is wrong.
// Depth maps to [0, 1] rather than [-1, 1] because that is Vulkan's clip convention.
void BuildLightViewProj(ShadowView *view) {
	const float *right = view->lightRight;
	const float *up = view->lightUp;
	const float *forward = view->lightDir;

	// Pull back along the light far enough that everything which could cast INTO the cascade is
	// in front of the near plane - a much longer way than the cascade is wide. See casterReach.
	const float reach = view->casterReach;
	const float origin[3] = {
		view->centre[0] - forward[0] * reach,
		view->centre[1] - forward[1] * reach,
		view->centre[2] - forward[2] * reach,
	};

	const float invRadius = 1.0f / view->radius;
	const float invDepth = 1.0f / (reach + view->radius);

	float *m = view->lightViewProj;
	m[0]  = right[0] * invRadius;  m[1]  = up[0] * invRadius;  m[2]  = forward[0] * invDepth;  m[3]  = 0.0f;
	m[4]  = right[1] * invRadius;  m[5]  = up[1] * invRadius;  m[6]  = forward[1] * invDepth;  m[7]  = 0.0f;
	m[8]  = right[2] * invRadius;  m[9]  = up[2] * invRadius;  m[10] = forward[2] * invDepth;  m[11] = 0.0f;
	m[12] = -Dot(origin, right) * invRadius;
	m[13] = -Dot(origin, up) * invRadius;
	m[14] = -Dot(origin, forward) * invDepth;
	m[15] = 1.0f;
}

// The rest of PPSSPP's vertex path, as one matrix. See ViewportCapture for why this can be a
// matrix at all: every step between the projection and gl_Position is linear in (x, y, z, w),
// because the perspective divide the viewport transform needs is undone again by the multiply
// back up into clip space at the end of it.
void BuildPostProjMatrix(const ViewportCapture &vp, float out[16]) {
	memset(out, 0, sizeof(float) * 16);
	out[15] = 1.0f;
	if (vp.rtWidth <= 0.0f || vp.rtHeight <= 0.0f) {
		out[0] = out[5] = out[10] = 1.0f;
		return;
	}
	const float invW = 2.0f / vp.rtWidth;
	const float invH = 2.0f / vp.rtHeight;
	const float invZ = 1.0f / 65536.0f;

	// Depth is the one axis that is NOT copied faithfully, and it has to be inverted here.
	//
	// VCS runs a reversed depth buffer: measured, the viewport Z scale is -32755.5 against an
	// offset of +32763.5, which maps the near plane to 65519 and the far plane to 8. That is the
	// game's business - it sets a matching depth compare and PPSSPP passes both through - but
	// this pass owns its OWN depth buffer and tests LESS against it, so copying the mapping
	// verbatim makes the FARTHEST surface win every pixel. The mask then samples the sky, which
	// is nowhere near the cascade, and comes back uniformly lit. That is exactly what it did.
	//
	// Nothing downstream cares which way camera depth runs - the shadow comparison happens in
	// light space - so the fix is to make it increase with distance and stop there.
	float scaleZ = vp.scale[2] * invZ;
	float offsetZ = vp.offset[2] * invZ;
	if (scaleZ < 0.0f) {
		scaleZ = -scaleZ;
		offsetZ = 1.0f - offsetZ;
	}

	out[0]  = vp.scale[0] * invW;
	out[5]  = vp.scale[1] * invH;
	out[10] = scaleZ;
	out[12] = (vp.offset[0] - vp.rasterOffset[0] + (float)vp.rtOffsetX) * invW - 1.0f;
	out[13] = (vp.offset[1] - vp.rasterOffset[1] + (float)vp.rtOffsetY) * invH - 1.0f;
	out[14] = offsetZ;
}

// Row-major on the CPU, column-major in a GLSL uniform. Everything that leaves this file for a
// shader goes through here; uploading a matrix raw scrambles the basis, which draws as long thin
// triangles radiating from a point rather than as a scene. PPSSPP hits the same wall and answers
// it the same way - ConvertMatrix4x3To3x4Transposed, on every matrix it hands its own shaders.
void Transpose4x4(const float in[16], float out[16]) {
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			out[col * 4 + row] = in[row * 4 + col];
		}
	}
}

// Gauss-Jordan with partial pivoting, in double. The matrix it is asked to invert is the camera's
// whole path to the pixel, whose depth row is a perspective projection squeezed into [0, 1] - in
// float the far half of the depth range comes back as noise.
bool Invert4x4(const float in[16], double out[16]) {
	double a[4][8];
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			a[row][col] = in[row * 4 + col];
			a[row][col + 4] = row == col ? 1.0 : 0.0;
		}
	}
	for (int col = 0; col < 4; col++) {
		int pivot = col;
		for (int row = col + 1; row < 4; row++) {
			if (fabs(a[row][col]) > fabs(a[pivot][col])) {
				pivot = row;
			}
		}
		if (fabs(a[pivot][col]) < 1e-30) {
			return false;
		}
		if (pivot != col) {
			for (int k = 0; k < 8; k++) {
				std::swap(a[col][k], a[pivot][k]);
			}
		}
		const double invPivot = 1.0 / a[col][col];
		for (int k = 0; k < 8; k++) {
			a[col][k] *= invPivot;
		}
		for (int row = 0; row < 4; row++) {
			if (row == col) {
				continue;
			}
			const double factor = a[row][col];
			for (int k = 0; k < 8; k++) {
				a[row][k] -= factor * a[col][k];
			}
		}
	}
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			out[row * 4 + col] = a[row][col + 4];
		}
	}
	return true;
}

}  // namespace

// The last sun this pass was given, kept so that a frame which happens not to contain one does
// not blank the shadows.
//
// The sun is read off whatever draw hands the hardware an enabled directional light, so it is
// found only if such a draw reaches the GPU that frame - and the scenery in this game is prelit
// and unlit, so the lit draws are the traffic and the pedestrians. Drive somewhere with neither in
// frame and the sun is simply not there, `ComputeShadowView` bails, and every shadow on screen
// vanishes for that frame. Reported as shadows flickering while driving.
//
// A sun that is a frame or two stale is worth vastly more than no sun: it moves with the time of
// day, which is to say a few thousandths of a degree per frame.
// Flicker, counted rather than described. See the blink block in BeginFrame.
static bool s_lastComposited;
static int s_blinks;
static int s_blinkNoSun;
static int s_blinkNoView;
static int s_blinkNoCasters;
static int s_blinkOther;

static bool s_haveHeldSun;
static float s_heldSunDir[3];
static float s_heldSunDiffuse[3];
static bool s_heldSunIsMoon;
static int s_framesOnHeldSun;

// At sunset the game's light sinks below the horizon and the pick swaps it for the mirrored moon,
// lifted to at least 0.35 - measured as a 16-degree jump in one frame, with every shadow on screen
// swinging through it, and sunrise is the same jump backwards. The game also moves its sun in steps
// of about 2.7 degrees every half second at dusk. So the direction the shadows are cast along turns
// towards the pick at a limited rate instead of snapping to it. A change too big for the clock to
// have made - a save loaded at another hour - still snaps, rather than sweeping across the sky.
static const float kSunTurnDegreesPerSecond = 8.0f;
static const float kSunSnapDegrees = 60.0f;
static bool s_haveSmoothSun;
static float s_smoothSunDir[3];
static double s_lastSunTurnTime;

static void TurnTowardsSun(float dir[3]) {
	const double now = time_now_d();
	float dt = s_lastSunTurnTime > 0.0 ? (float)(now - s_lastSunTurnTime) : 0.0f;
	s_lastSunTurnTime = now;
	// A load, a pause or a breakpoint leaves an arbitrary gap, and letting it through would snap.
	if (dt < 0.0f) dt = 0.0f;
	if (dt > 0.25f) dt = 0.25f;

	static const float kRadians = 3.14159265f / 180.0f;
	float cosAngle = 1.0f;
	if (s_haveSmoothSun) {
		cosAngle = s_smoothSunDir[0] * dir[0] + s_smoothSunDir[1] * dir[1] + s_smoothSunDir[2] * dir[2];
		if (cosAngle > 1.0f) cosAngle = 1.0f;
		if (cosAngle < -1.0f) cosAngle = -1.0f;
	}
	const float angle = acosf(cosAngle);
	const float maxStep = kSunTurnDegreesPerSecond * kRadians * dt;

	// Said when a turn starts and when a change is snapped, so a log shows the transition happened
	// the way it was meant to rather than leaving that to a screenshot.
	static bool s_turning = false;
	static int s_turnLogs = 0;
	if (!s_haveSmoothSun || angle <= maxStep || angle > kSunSnapDegrees * kRadians) {
		if (s_haveSmoothSun && angle > kSunSnapDegrees * kRadians && s_turnLogs < 32) {
			s_turnLogs++;
			NOTICE_LOG(Log::G3D, "LCS shadows: the sun moved %.0f degrees at once - snapped rather than turned", angle / kRadians);
		}
		memcpy(s_smoothSunDir, dir, sizeof(s_smoothSunDir));
		s_haveSmoothSun = true;
		s_turning = false;
		return;
	}
	if (!s_turning && angle > 3.0f * kRadians && s_turnLogs < 32) {
		s_turnLogs++;
		NOTICE_LOG(Log::G3D, "LCS shadows: turning the shadows through %.1f degrees over %.1f s instead of jumping",
			angle / kRadians, angle / (kSunTurnDegreesPerSecond * kRadians));
	}
	s_turning = true;

	// Spherical interpolation by a fixed angle, so the rate is the same however far there is to go.
	const float t = maxStep / angle;
	const float sinAngle = sinf(angle);
	const float a = sinf((1.0f - t) * angle) / sinAngle;
	const float b = sinf(t * angle) / sinAngle;
	for (int c = 0; c < 3; c++) {
		s_smoothSunDir[c] = a * s_smoothSunDir[c] + b * dir[c];
	}
	Normalize(s_smoothSunDir);
	memcpy(dir, s_smoothSunDir, sizeof(s_smoothSunDir));
}

// The strength has the same seam: the sun's is its own brightness and the moon's is moonStrength, so
// the swap at sunset changed how dark every shadow is in the same frame as their direction. Eased at a
// rate that crosses the default strength in about two seconds.
static const float kStrengthPerSecond = 0.35f;
static bool s_haveEasedStrength;
static float s_easedStrength;
static double s_lastStrengthTime;

static float EaseShadowStrength(float target) {
	const double now = time_now_d();
	float dt = s_lastStrengthTime > 0.0 ? (float)(now - s_lastStrengthTime) : 0.0f;
	s_lastStrengthTime = now;
	if (dt < 0.0f) dt = 0.0f;
	if (dt > 0.25f) dt = 0.25f;
	if (!s_haveEasedStrength) {
		s_easedStrength = target;
		s_haveEasedStrength = true;
		return target;
	}
	const float step = kStrengthPerSecond * dt;
	if (s_easedStrength < target) {
		s_easedStrength = std::min(s_easedStrength + step, target);
	} else {
		s_easedStrength = std::max(s_easedStrength - step, target);
	}
	return s_easedStrength;
}

// Recomputes where the shadow projection is looking, from the view matrix and sun direction the
// frame has left behind by the time it turns to 2D.
// Where the cascades sit relative to the camera: ahead of where it is GOING, never of where it is
// LOOKING.
//
// They used to sit `centreDistance` along the view direction, so that a car would not outrun its
// shadows - and that made every turn of the camera swing the whole box 80 units, which on ULTRA
// is buildings' shadows coming and going: the casters at the box's edge enter and leave the map,
// and the remembered cells' replay test is measured from the same centre. Reported as ULTRA
// flickering with camera movement while HIGH did not - HIGH's casters are people, cars and props a
// few units from the player, always inside the box wherever it points.
//
// The lead the box was for is a lead along the TRAVEL, so that is what it follows now: the
// camera's horizontal velocity, smoothed over half a second and taken 1.2 seconds ahead, clamped
// to centreDistance. Standing and turning, it is zero and nothing about the shadow map changes. A
// jump of more than fifteen units in a frame is a rebase of the GE space or a teleport, not travel,
// and carries the velocity over rather than spiking it.
static float s_leadVelocity[2];
static float s_leadLastPos[2];
static double s_leadLastTime;
static bool s_leadHave;

static void CascadeLead(const float *cameraPos, float maxLead, float out[3]) {
	const double now = time_now_d();
	if (s_leadHave) {
		const double dt = now - s_leadLastTime;
		const float dx = cameraPos[0] - s_leadLastPos[0];
		const float dy = cameraPos[1] - s_leadLastPos[1];
		if (dt > 0.0005 && dt < 0.5 && dx * dx + dy * dy < 15.0f * 15.0f) {
			const float k = 1.0f - expf(-(float)dt / 0.5f);
			s_leadVelocity[0] += (dx / (float)dt - s_leadVelocity[0]) * k;
			s_leadVelocity[1] += (dy / (float)dt - s_leadVelocity[1]) * k;
		}
	}
	s_leadLastPos[0] = cameraPos[0];
	s_leadLastPos[1] = cameraPos[1];
	s_leadLastTime = now;
	s_leadHave = true;

	out[0] = s_leadVelocity[0] * 1.2f;
	out[1] = s_leadVelocity[1] * 1.2f;
	out[2] = 0.0f;
	const float len = sqrtf(out[0] * out[0] + out[1] * out[1]);
	if (len > maxLead && len > 0.0f) {
		out[0] *= maxLead / len;
		out[1] *= maxLead / len;
	}
}

static void ComputeShadowView(FrameStats stats) {
	s_view.valid = false;
	if (!s_haveViewMatrix) {
		return;
	}
	if (stats.sunValid) {
		s_haveHeldSun = true;
		memcpy(s_heldSunDir, stats.sunDir, sizeof(s_heldSunDir));
		memcpy(s_heldSunDiffuse, stats.sunDiffuse, sizeof(s_heldSunDiffuse));
		s_heldSunIsMoon = stats.sunIsMoon;
		s_framesOnHeldSun = 0;
	} else if (s_haveHeldSun) {
		stats.sunValid = true;
		memcpy(stats.sunDir, s_heldSunDir, sizeof(s_heldSunDir));
		memcpy(stats.sunDiffuse, s_heldSunDiffuse, sizeof(s_heldSunDiffuse));
		stats.sunIsMoon = s_heldSunIsMoon;
		s_framesOnHeldSun++;
	} else {
		return;
	}

	DecomposeViewMatrix(s_frameViewMatrix, &s_view);

	// Push the recovered position back through the view matrix exactly as the vertex shader
	// would push a vertex: out.c = sum_r p.r * m[r*3+c] + m[9+c].
	{
		const float *m = s_frameViewMatrix;
		const float *p = s_view.cameraPos;
		for (int c = 0; c < 3; c++) {
			s_view.viewResidual[c] = p[0] * m[c] + p[1] * m[3 + c] + p[2] * m[6 + c] + m[9 + c];
		}
	}

	TurnTowardsSun(stats.sunDir);

	// The sun vector points towards the light; light travels the other way.
	s_view.lightDir[0] = -stats.sunDir[0];
	s_view.lightDir[1] = -stats.sunDir[1];
	s_view.lightDir[2] = -stats.sunDir[2];
	if (!Normalize(s_view.lightDir)) {
		return;
	}

	// Any up vector will do as long as it is not parallel to the light. World up is the natural
	// choice and only fails with the sun directly overhead, which is what the fallback is for.
	float up[3] = { 0.0f, 0.0f, 1.0f };
	if (fabsf(Dot(s_view.lightDir, up)) > 0.99f) {
		up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
	}
	Cross(up, s_view.lightDir, s_view.lightRight);
	if (!Normalize(s_view.lightRight)) {
		return;
	}
	Cross(s_view.lightDir, s_view.lightRight, s_view.lightUp);
	Normalize(s_view.lightUp);

	s_view.radius = s_settings.cascadeRadius;
	s_view.casterReach = s_settings.casterReach > s_view.radius
		? s_settings.casterReach : s_view.radius;
	s_view.texelWorldSize = (2.0f * s_view.radius) / (float)s_settings.mapSize;

	float lead[3];
	CascadeLead(s_view.cameraPos, s_settings.centreDistance, lead);
	float centre[3] = {
		s_view.cameraPos[0] + lead[0],
		s_view.cameraPos[1] + lead[1],
		s_view.cameraPos[2] + lead[2],
	};

	// Snap the centre to whole shadow-map texels along the light's own axes. Without this the
	// map's sampling grid slides under the geometry as the camera moves and every shadow edge
	// crawls - the single most obvious artefact in a cascade that is otherwise correct.
	const float texel = s_view.texelWorldSize;
	if (texel > 0.0f) {
		const float x = floorf(Dot(centre, s_view.lightRight) / texel) * texel;
		const float y = floorf(Dot(centre, s_view.lightUp) / texel) * texel;
		const float z = Dot(centre, s_view.lightDir);
		for (int i = 0; i < 3; i++) {
			centre[i] = s_view.lightRight[i] * x + s_view.lightUp[i] * y + s_view.lightDir[i] * z;
		}
	}
	s_view.centre[0] = centre[0];
	s_view.centre[1] = centre[1];
	s_view.centre[2] = centre[2];

	// The near cascade: the same construction at a smaller radius, anchored on the camera
	// rather than well ahead of it, because what it is for is the ground under your feet and
	// the car you are standing next to. Snapped on its OWN texel size - sharing the far
	// cascade's would leave it crawling, which is the artefact snapping exists to remove.
	s_view.nearRadius = s_settings.nearCascadeRadius > 1.0f ? s_settings.nearCascadeRadius : 0.0f;
	if (s_view.nearRadius > 0.0f && s_view.nearRadius < s_view.radius) {
		s_view.nearTexelWorldSize = (2.0f * s_view.nearRadius) / (float)s_settings.mapSize;
		// The same lead, scaled to a third of this box's own radius at most - and for the same
		// reason: led by the view, turning moved things between the sharp cascade and the soft one.
		const float nearLead = s_settings.centreDistance > 0.0f
			? s_view.nearRadius * 0.35f / s_settings.centreDistance : 0.0f;
		float nearCentre[3] = {
			s_view.cameraPos[0] + lead[0] * nearLead,
			s_view.cameraPos[1] + lead[1] * nearLead,
			s_view.cameraPos[2],
		};
		const float nearTexel = s_view.nearTexelWorldSize;
		if (nearTexel > 0.0f) {
			const float nx = floorf(Dot(nearCentre, s_view.lightRight) / nearTexel) * nearTexel;
			const float ny = floorf(Dot(nearCentre, s_view.lightUp) / nearTexel) * nearTexel;
			const float nz = Dot(nearCentre, s_view.lightDir);
			for (int i = 0; i < 3; i++) {
				nearCentre[i] = s_view.lightRight[i] * nx + s_view.lightUp[i] * ny +
					s_view.lightDir[i] * nz;
			}
		}
		for (int i = 0; i < 3; i++) {
			s_view.nearCentre[i] = nearCentre[i];
		}
	} else {
		s_view.nearRadius = 0.0f;
		s_view.nearTexelWorldSize = 0.0f;
	}

	BuildLightViewProj(&s_view);
	if (s_view.nearRadius > 0.0f) {
		// Same builder, near numbers. Swapped in and out rather than copied into a second
		// function, so the two cascades cannot drift apart in how their box is constructed.
		const float farRadius = s_view.radius;
		float farCentre[3] = { s_view.centre[0], s_view.centre[1], s_view.centre[2] };
		s_view.radius = s_view.nearRadius;
		for (int i = 0; i < 3; i++) {
			s_view.centre[i] = s_view.nearCentre[i];
		}
		BuildLightViewProj(&s_view);
		memcpy(s_view.nearLightViewProj, s_view.lightViewProj, sizeof(s_view.lightViewProj));
		s_view.radius = farRadius;
		for (int i = 0; i < 3; i++) {
			s_view.centre[i] = farCentre[i];
		}
		BuildLightViewProj(&s_view);
	}

	// World to the pixel the game drew. The 4x3 view matrix widens to 4x4 with the translation
	// staying in the last row, exactly as ConvertMatrix4x3To4x4 does it, then the projection, then
	// everything PPSSPP's own vertex shader does after it.
	{
		const float *v = s_frameViewMatrix;
		const float view4[16] = {
			v[0], v[1], v[2],  0.0f,
			v[3], v[4], v[5],  0.0f,
			v[6], v[7], v[8],  0.0f,
			v[9], v[10], v[11], 1.0f,
		};
		float viewProj[16];
		Mul4x4(view4, s_frameProjMatrix, viewProj);

		float post[16];
		BuildPostProjMatrix(s_frameViewport, post);
		Mul4x4(viewProj, post, s_view.cameraViewProj);
	}

	s_view.valid = true;
}

// Appends one piece of world-space geometry to the frame's streams, batching it so the 16-bit
// indices stay in range. Shared by the live capture and by the caster cache, which is the whole
// reason it is a function: a remembered caster has to enter the depth pass by exactly the same
// road a fresh one does.
static void AppendGeometry(const float *pos, int vertCount, const u16 *castIdx, int castCount,
	const u16 *recvIdx, int recvCount, bool pedReceiver = false) {
	if (vertCount <= 0 || (castCount < 3 && recvCount < 3)) {
		return;
	}
	if (s_positions.size() / 3 + (size_t)vertCount > kMaxCapturedVertices) {
		s_capture.overflowed = true;
		return;
	}

	// A single caster never approaches 64k vertices - the largest measured here is about a
	// thousand - so starting a fresh batch whenever this one would overflow is always enough.
	if (s_batches.empty() || s_batches.back().vertexCount + vertCount > kMaxBatchVertices) {
		Batch fresh;
		fresh.firstVertex = (u32)(s_positions.size() / 3);
		fresh.vertexCount = 0;
		fresh.firstCasterIndex = (u32)s_casterIndices.size();
		fresh.casterIndexCount = 0;
		fresh.firstReceiverIndex = (u32)s_receiverIndices.size();
		fresh.receiverIndexCount = 0;
		fresh.firstPedReceiverIndex = (u32)s_pedReceiverIndices.size();
		fresh.pedReceiverIndexCount = 0;
		s_batches.push_back(fresh);
	}
	Batch &batch = s_batches.back();

	// Two different bases, and mixing them up costs a frame of confusion: indices are relative to
	// the batch because they are 16-bit, while the position write goes to the end of the shared
	// buffer. They coincide while there is only one batch, which is most frames - so this only
	// ever breaks once a scene gets busy enough to need a second one.
	const u16 base = (u16)batch.vertexCount;
	s_positions.insert(s_positions.end(), pos, pos + (size_t)vertCount * 3);

	std::vector<u16> &recvStream = pedReceiver ? s_pedReceiverIndices : s_receiverIndices;
	for (int i = 0; i < recvCount; i++) {
		recvStream.push_back((u16)(base + recvIdx[i]));
	}
	for (int i = 0; i < castCount; i++) {
		s_casterIndices.push_back((u16)(base + castIdx[i]));
	}

	batch.vertexCount += vertCount;
	batch.casterIndexCount = (int)(s_casterIndices.size() - batch.firstCasterIndex);
	batch.receiverIndexCount = (int)(s_receiverIndices.size() - batch.firstReceiverIndex);
	batch.pedReceiverIndexCount = (int)(s_pedReceiverIndices.size() - batch.firstPedReceiverIndex);

	s_capture.batches = (int)s_batches.size();
	s_capture.vertices = (int)(s_positions.size() / 3);
	s_capture.casterIndices = (int)s_casterIndices.size();
	s_capture.receiverIndices = (int)(s_receiverIndices.size() + s_pedReceiverIndices.size());
	s_capture.indices = s_capture.receiverIndices;
	s_capture.bytes = s_positions.size() * sizeof(float)
		+ (s_casterIndices.size() + s_receiverIndices.size()
			+ s_pedReceiverIndices.size()) * sizeof(u16);
}

// One triangle of the draw being baked, in the winding the game asked for, split into the two
// streams by which way it faces the light.
//
// The facing test is the whole answer to self-shadowing here, and it is worth more than a bias
// because both passes read these same vertices: a surface is compared against ITSELF, so the only
// error is where the shadow map's texel grid happens to fall, and no bias small enough to keep
// shadows attached is reliably bigger than that. Storing the far side of every object instead puts
// a whole object's thickness between the two. It also halves what the depth pass draws.
static inline void EmitTriangle(u16 a, u16 b, u16 c) {
	if (s_drawFlipWinding) {
		const u16 t = b;
		b = c;
		c = t;
	}
	s_drawIdx.push_back(a);
	s_drawIdx.push_back(b);
	s_drawIdx.push_back(c);

	if (!s_settings.castBackFacesOnly || !s_haveCaptureLightDir) {
		s_drawCastIdx.push_back(a);
		s_drawCastIdx.push_back(b);
		s_drawCastIdx.push_back(c);
		return;
	}

	const float *p0 = s_drawPos.data() + (size_t)a * 3;
	const float *p1 = s_drawPos.data() + (size_t)b * 3;
	const float *p2 = s_drawPos.data() + (size_t)c * 3;
	const float e0[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
	const float e1[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
	const float n[3] = {
		e0[1] * e1[2] - e0[2] * e1[1],
		e0[2] * e1[0] - e0[0] * e1[2],
		e0[0] * e1[1] - e0[1] * e1[0],
	};
	// Light travels along s_captureLightDir, so a face whose normal points the same way is the
	// far side of whatever it belongs to. Degenerate triangles fall out on the >= and cost
	// nothing either way.
	if (n[0] * s_captureLightDir[0] + n[1] * s_captureLightDir[1] + n[2] * s_captureLightDir[2] > 0.0f) {
		s_drawCastIdx.push_back(a);
		s_drawCastIdx.push_back(b);
		s_drawCastIdx.push_back(c);
		s_current.trianglesFacingAway++;
	} else {
		s_current.trianglesFacingLight++;
	}
}

// Every draw is captured in the space of the frame's FIRST caster - its view matrix is what the mask
// projects with and what the cascade is built around. A draw that arrives with a different view matrix
// is on screen correctly, because the GE puts it through its own; baked with its world matrix alone it
// lands somewhere else in that shared space.
//
// That is the shadow that vanished around chunk loads. The game rebases the space it feeds the GE by a
// whole streaming cell - 125 units - as you travel, and around the swap not every draw in a frame
// carries the new camera: the player can be 125 units from the ground under them in the capture while
// looking perfectly placed on screen. The shadow was cast and had nothing to land on, for the few frames
// the swap took - lined up in play against the logged rebase to within a tenth of a second.
//
// So a draw whose view matrix differs is moved into the frame's space: world, through its own view,
// back through the inverse of the frame's. Rigid or not, that is exact; the same matrix costs a compare.
static bool s_frameViewInvValid;
static float s_frameViewInvR[9];
static float s_viewCorrA[9];
static float s_viewCorrB[3];
static int s_viewCorrectedLogs;
static int s_viewCorrectedDraws;

static bool Invert3x3(const float m[9], float out[9]) {
	const float c00 = m[4] * m[8] - m[5] * m[7];
	const float c01 = m[5] * m[6] - m[3] * m[8];
	const float c02 = m[3] * m[7] - m[4] * m[6];
	const float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
	if (fabsf(det) < 1e-12f) {
		return false;
	}
	const float inv = 1.0f / det;
	out[0] = c00 * inv;
	out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
	out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
	out[3] = c01 * inv;
	out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
	out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
	out[6] = c02 * inv;
	out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
	out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
	return true;
}

// Fills s_viewCorrA / s_viewCorrB for the draw about to be baked, when its view matrix is not the
// frame's. Row vectors throughout, as everywhere in this file: frame = world * A + B.
static bool PrepareViewCorrection() {
	if (!s_haveViewMatrix || !s_frameViewInvValid) {
		return false;
	}
	if (memcmp(gstate.viewMatrix, s_frameViewMatrix, sizeof(s_frameViewMatrix)) == 0) {
		return false;
	}
	const float *d = gstate.viewMatrix;
	const float *f = s_frameViewMatrix;
	const float *inv = s_frameViewInvR;
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			s_viewCorrA[r * 3 + c] = d[r * 3] * inv[c] + d[r * 3 + 1] * inv[3 + c] + d[r * 3 + 2] * inv[6 + c];
		}
	}
	const float dt[3] = { d[9] - f[9], d[10] - f[10], d[11] - f[11] };
	for (int c = 0; c < 3; c++) {
		s_viewCorrB[c] = dt[0] * inv[c] + dt[1] * inv[3 + c] + dt[2] * inv[6 + c];
	}
	s_viewCorrectedDraws++;
	const float shift = sqrtf(s_viewCorrB[0] * s_viewCorrB[0] + s_viewCorrB[1] * s_viewCorrB[1] +
		s_viewCorrB[2] * s_viewCorrB[2]);
	if (shift > 1.0f && (s_viewCorrectedLogs < 6 || s_viewCorrectedDraws % 5000 == 0)) {
		s_viewCorrectedLogs++;
		NOTICE_LOG(Log::G3D,
			"LCS shadows: a draw used a different camera matrix than the frame's (%.1f %.1f %.1f units apart) - moved into the frame's space (%d draws so far)",
			s_viewCorrB[0], s_viewCorrB[1], s_viewCorrB[2], s_viewCorrectedDraws);
	}
	return true;
}

// Transforms a draw's decoded vertices into `s_drawPos` and expands its topology into
// `s_drawIdx` as a plain triangle list.
//
// Row-vector, matching the vertex shader: world = vec4(pos, 1.0) * m, with the 4x3 matrix holding
// its translation in the last row. The decoded position is always three floats - the decoder
// guarantees that regardless of how the game encoded it - and for a skinned mesh the bones have
// already been folded in here, which is what gets peds into pose for free.
static void BakeDraw(const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, GEPrimitiveType prim, const float world[12],
	float drawMin[3], float drawMax[3]) {
	s_drawPos.resize((size_t)numDecodedVerts * 3);
	s_drawIdx.clear();
	s_drawCastIdx.clear();

	for (int c = 0; c < 3; c++) {
		drawMin[c] = 1e30f;
		drawMax[c] = -1e30f;
	}

	const bool correctView = PrepareViewCorrection();
	const float *ca = s_viewCorrA;
	const float *cb = s_viewCorrB;
	float *out = s_drawPos.data();
	for (int i = 0; i < numDecodedVerts; i++) {
		const float *p = (const float *)(decoded + (size_t)i * stride + posOffset);
		float v[3] = {
			p[0] * world[0] + p[1] * world[3] + p[2] * world[6] + world[9],
			p[0] * world[1] + p[1] * world[4] + p[2] * world[7] + world[10],
			p[0] * world[2] + p[1] * world[5] + p[2] * world[8] + world[11],
		};
		if (correctView) {
			const float x = v[0], y = v[1], z = v[2];
			v[0] = x * ca[0] + y * ca[3] + z * ca[6] + cb[0];
			v[1] = x * ca[1] + y * ca[4] + z * ca[7] + cb[1];
			v[2] = x * ca[2] + y * ca[5] + z * ca[8] + cb[2];
		}
		for (int c = 0; c < 3; c++) {
			out[i * 3 + c] = v[c];
			if (v[c] < drawMin[c]) drawMin[c] = v[c];
			if (v[c] > drawMax[c]) drawMax[c] = v[c];
		}
	}

	// Everything becomes a triangle list, because the whole cascade is going out as a single
	// draw and one draw cannot carry three topologies. Strips and fans are cheap to expand and
	// the alternative is a draw call per caster.
	const auto index = [&](int i) -> u16 {
		return (u16)(indices ? indices[i] : i);
	};

	// The winding the game asked for. PPSSPP culls with the game's own cull mode rather than
	// normalising it away, so a draw with mode 1 has the opposite front face from one with mode
	// 0, and a facing test that ignored that would be right half the time.
	s_drawFlipWinding = (gstate.getCullMode() != 0) != s_settings.flipCasterWinding;
	switch (prim) {
	case GE_PRIM_TRIANGLES:
		for (int i = 0; i + 2 < indexCount; i += 3) {
			EmitTriangle(index(i), index(i + 1), index(i + 2));
		}
		break;
	case GE_PRIM_TRIANGLE_STRIP:
		// Every other triangle in a strip has reversed winding. Keeping that consistent matters
		// because the depth pass culls faces to halve the acne problem.
		for (int i = 0; i + 2 < indexCount; i++) {
			if (i & 1) {
				EmitTriangle(index(i + 1), index(i), index(i + 2));
			} else {
				EmitTriangle(index(i), index(i + 1), index(i + 2));
			}
		}
		break;
	case GE_PRIM_TRIANGLE_FAN:
		for (int i = 1; i + 1 < indexCount; i++) {
			EmitTriangle(index(0), index(i), index(i + 1));
		}
		break;
	default:
		break;
	}
}

// --- remembering casters the game has stopped drawing --------------------------------------
//
// The game culls to its own camera frustum, so a building a few degrees off the edge of the
// screen is not drawn, is not captured, and stops casting the shadow that was lying across the
// road in front of you. What is remembered here is re-submitted to the DEPTH pass alone - a
// receiver has to be on screen to be shaded.
//
// It remembers PLACES, not draws, and that is the second design. The first kept one entry per
// draw, keyed on what the draw was and where: it looked right and cached nothing, measured at
// fifteen hits against fourteen hundred inserts a frame while standing still. The reason is that
// `AddCaster` sees one FLUSH rather than one object - PPSSPP merges draw calls until some piece
// of state changes - and where those merges fall moves from frame to frame even when the scene is
// identical. So no key built out of a flush can repeat, however stable the world is.
//
// A grid does not care. Whatever was captured inside a cell replaces whatever was there before,
// and a cell nobody has seen lately keeps what it had. The geometry is already in world space, and
// a static object holds still there to four decimal places - measured, on a 3609-vertex building
// across several seconds - so a remembered cell needs no transform to be correct.

static const float kBucketSize = 32.0f;
static const int kMaxBucketVertices = 60000;   // indices into a bucket are 16-bit
// How far the light may move, cos(10 degrees), before a remembered cell may be refreshed with a
// smaller capture - see the replace rule in SubmitCachedCasters.
static const float kCacheLightRefreshCos = 0.985f;

// A remembered cut-out: which texture, how it is sampled, and where its vertices and indices sit
// in the cell's flat arrays. Indices are relative to the piece's first vertex.
struct CutPiece {
	CutoutTex tex;
	float alphaRef;
	u8 wrap;
	u32 firstVert;
	u32 vertCount;
	u32 firstIdx;
	u32 idxCount;
};

struct Bucket {
	std::vector<float> pos;
	std::vector<u16> idx;
	std::vector<float> pendingPos;
	std::vector<u16> pendingIdx;
	// The cell's cut-outs, which need their texture as well as their shape. Flat arrays rather
	// than a vector per piece, so clear() keeps the storage frame to frame the way the plain
	// half's does.
	std::vector<float> cutVerts;
	std::vector<u16> cutIdx;
	std::vector<CutPiece> cutPieces;
	std::vector<float> pendingCutVerts;
	std::vector<u16> pendingCutIdx;
	std::vector<CutPiece> pendingCutPieces;
	int lastSeenFrame;
	int touchedFrame;
	float centre[3];
	// How far the camera was when these contents were COMMITTED, which is what decides whether
	// they are the real building or its LOD stand-in. A cell only takes full detail from a
	// sighting inside cacheReplaceRadius; an empty one takes whatever it is offered, however far
	// away, and then keeps it. So a cell whose fillDist is large is casting a blocky
	// approximation for as long as it lives, and nothing in the counts so far could see that.
	float fillDist;
	// The light the plain half was committed under. Only faces turned from the light are kept,
	// so a memory taken under a light that has since moved holds the wrong faces.
	float commitLight[3];
};

static std::unordered_map<u64, Bucket> s_buckets;

static u64 BucketKey(const float centre[3]) {
	const s64 x = (s64)floorf(centre[0] / kBucketSize);
	const s64 y = (s64)floorf(centre[1] / kBucketSize);
	const s64 z = (s64)floorf(centre[2] / kBucketSize);
	return ((u64)(x & 0x1FFFFF) << 42) | ((u64)(y & 0x1FFFFF) << 21) | (u64)(z & 0x1FFFFF);
}

// The cell a draw centred here belongs to, emptied of last frame's pending capture the first time
// this frame touches it. Shared by both kinds of caster, so a cell that holds a building and a
// palm starts each frame clean on both halves.
static Bucket &TouchBucket(const float centre[3]) {
	Bucket &b = s_buckets[BucketKey(centre)];
	if (b.touchedFrame != s_frameIndex) {
		b.touchedFrame = s_frameIndex;
		b.pendingPos.clear();
		b.pendingIdx.clear();
		b.pendingCutVerts.clear();
		b.pendingCutIdx.clear();
		b.pendingCutPieces.clear();
		for (int c = 0; c < 3; c++) {
			b.centre[c] = floorf(centre[c] / kBucketSize) * kBucketSize + kBucketSize * 0.5f;
		}
	}
	return b;
}

// Scratch for RememberCaster: which cell each of this draw's vertices was last copied into, and
// where in it.
static std::vector<const Bucket *> s_rememberCell;
static std::vector<u16> s_rememberIdx;

// Adds this draw's casting triangles to the cells they sit in, each TRIANGLE to the cell of its
// own centroid.
//
// Whole draws used to go to the cell of the draw's centre, and a draw is a flush, not an object:
// PPSSPP merges consecutive draws with the same state, and which ones it merges depends on what
// the game culled - so it changes as the camera turns. A building drawn alone went to its own
// cell; a moment later, merged with a neighbour that had come into view, it went to a cell
// between the two. Its own cell, still on screen, was then REPLACED without it, the cell between
// was half off screen and kept what it had, and the building was in neither. Reported as a
// building's shadow going out after turning your back on it - buildings worst, palms fine,
// because a frond flush is so wide its centre lands in a cell too far away ever to be replaced.
//
// A triangle's centroid does not care how draws were grouped. It does NOT make one frame's capture
// of a cell complete - drawn is not captured - which is why SubmitCachedCasters keeps whichever
// capture held the most, rather than whichever came last.
static void RememberCaster(int vertCount) {
	if (!s_settings.cacheCasters || s_drawCastIdx.size() < 3) {
		return;
	}
	s_rememberCell.assign((size_t)vertCount, nullptr);
	s_rememberIdx.resize((size_t)vertCount);

	const float *pos = s_drawPos.data();
	const size_t count = s_drawCastIdx.size() - s_drawCastIdx.size() % 3;
	Bucket *cell = nullptr;
	const Bucket *first = nullptr;
	u64 cellKey = 0;
	bool split = false;
	for (size_t t = 0; t < count; t += 3) {
		const u16 *tri = s_drawCastIdx.data() + t;
		float centroid[3];
		for (int c = 0; c < 3; c++) {
			centroid[c] = (pos[tri[0] * 3 + c] + pos[tri[1] * 3 + c] + pos[tri[2] * 3 + c]) * (1.0f / 3.0f);
		}
		// Consecutive triangles nearly always share a cell, so the map is only asked on a change.
		const u64 key = BucketKey(centroid);
		if (!cell || key != cellKey) {
			cell = &TouchBucket(centroid);
			cellKey = key;
			if (!first) {
				first = cell;
			} else if (cell != first) {
				split = true;
			}
		}
		if (cell->pendingPos.size() / 3 + 3 > (size_t)kMaxBucketVertices) {
			s_capture.cacheCapDrops++;
			continue;
		}
		for (int k = 0; k < 3; k++) {
			const u16 v = tri[k];
			if (s_rememberCell[v] != cell) {
				s_rememberCell[v] = cell;
				s_rememberIdx[v] = (u16)(cell->pendingPos.size() / 3);
				cell->pendingPos.insert(cell->pendingPos.end(), pos + v * 3, pos + v * 3 + 3);
			}
			cell->pendingIdx.push_back(s_rememberIdx[v]);
		}
	}
	if (split) {
		s_capture.cacheSplitDraws++;
	}
}

// Adds one cut-out piece to this frame's cut-out pass, in the group for its texture.
static void AppendCutout(const CutoutTex &tex, float alphaRef, u8 wrap, const float *verts,
	u32 vertCount, const u16 *idx, u32 idxCount) {
	if (vertCount == 0 || idxCount < 3 || vertCount > (u32)kMaxBatchVertices) {
		return;
	}
	CutoutGroup *group = nullptr;
	CutoutGroup *spare = nullptr;
	for (CutoutGroup &g : s_cutGroups) {
		if (g.verts.empty()) {
			if (!spare) {
				spare = &g;
			}
			continue;
		}
		if (g.tex.view == tex.view && g.alphaRef == alphaRef && g.wrap == wrap &&
			g.verts.size() / 5 + vertCount <= (size_t)kMaxBatchVertices) {
			group = &g;
			break;
		}
	}
	if (!group) {
		if (!spare) {
			s_cutGroups.emplace_back();
			spare = &s_cutGroups.back();
		}
		group = spare;
		group->tex = tex;
		group->alphaRef = alphaRef;
		group->wrap = wrap;
	}
	const u16 base = (u16)(group->verts.size() / 5);
	group->verts.insert(group->verts.end(), verts, verts + (size_t)vertCount * 5);
	for (u32 i = 0; i < idxCount; i++) {
		group->idx.push_back((u16)(base + idx[i]));
	}
	s_capture.cutoutIndices += (int)idxCount;
}

// Every built texture's view, keyed both ways, rebuilt at the seam of any frame with remembered
// cut-outs to replay.
//
// A remembered piece cannot simply keep its view. The texture cache frees textures it has not used
// in a while and rebuilds one when its HD replacement lands, and a view that has gone - handed to
// Vulkan in a descriptor set - is a crash rather than a wrong shadow. A view that is in this table
// THIS frame belongs to a texture that is alive; one whose address and size still match is still
// the same texture. Otherwise the address finds whatever lives there now, and failing that the
// piece is skipped for the frame.
static std::unordered_map<void *, u64> s_liveViewIdentity;
static std::unordered_map<u64, void *> s_liveIdentityView;

static inline u64 TexIdentity(u32 addr, u16 dim) {
	return ((u64)addr << 16) | dim;
}

static void NoteLiveTextureView(void *ctx, void *view, u32 addr, u16 dim) {
	const u64 id = TexIdentity(addr, dim);
	s_liveViewIdentity[view] = id;
	s_liveIdentityView.emplace(id, view);
}

static void *ResolveCutoutTexture(const CutoutTex &tex) {
	const u64 id = TexIdentity(tex.addr, tex.dim);
	auto byView = s_liveViewIdentity.find(tex.view);
	if (byView != s_liveViewIdentity.end() && byView->second == id) {
		return tex.view;
	}
	auto byIdentity = s_liveIdentityView.find(id);
	return byIdentity != s_liveIdentityView.end() ? byIdentity->second : nullptr;
}

// Remembers the committed cut-out in the cell it sits in, beside that cell's plain geometry.
static void RememberCutout(const CutoutTex &tex, float alphaRef, u8 wrap) {
	if (!s_settings.cacheCasters || s_cutDrawIdx.size() < 3) {
		return;
	}
	Bucket &b = TouchBucket(s_cutPendingCentre);
	const u32 vertCount = (u32)(s_cutDrawVerts.size() / 5);
	if (b.pendingCutVerts.size() / 5 + vertCount > (size_t)kMaxBucketVertices) {
		return;
	}
	CutPiece piece;
	piece.tex = tex;
	piece.alphaRef = alphaRef;
	piece.wrap = wrap;
	piece.firstVert = (u32)(b.pendingCutVerts.size() / 5);
	piece.vertCount = vertCount;
	piece.firstIdx = (u32)b.pendingCutIdx.size();
	piece.idxCount = (u32)s_cutDrawIdx.size();
	b.pendingCutVerts.insert(b.pendingCutVerts.end(), s_cutDrawVerts.begin(), s_cutDrawVerts.end());
	b.pendingCutIdx.insert(b.pendingCutIdx.end(), s_cutDrawIdx.begin(), s_cutDrawIdx.end());
	b.pendingCutPieces.push_back(piece);
}

// How far round from the way the camera faces this point lies, in degrees, measured flat on the
// ground plane. 0 is straight ahead and 180 is straight behind.
//
// The whole of the caster-horizon instrument - see CaptureStats for what it is for and how to read
// it. Flattened because the camera is normally pitched down at the player: on a full solid-angle
// measure the pavement at his feet reads as being off to one side, which is true and useless.
//
// Returns -1 for a point the measure cannot describe: one standing on the camera's own axis, where
// there is no direction to report, or a frame with no camera yet.
static float YawFromForward(const float point[3]) {
	if (!s_haveViewMatrix) {
		return -1.0f;
	}
	const float fx = s_frameCameraFwd[0], fy = s_frameCameraFwd[1];
	const float dx = point[0] - s_frameCameraPos[0], dy = point[1] - s_frameCameraPos[1];
	const float fLen = sqrtf(fx * fx + fy * fy);
	const float dLen = sqrtf(dx * dx + dy * dy);
	// Two units is about where the game's own full-screen overlays sit, and anything inside that
	// is too close for a bearing to mean anything anyway.
	if (fLen < 0.001f || dLen < 2.0f) {
		return -1.0f;
	}
	float c = (fx * dx + fy * dy) / (fLen * dLen);
	c = c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);
	return acosf(c) * (180.0f / 3.14159265f);
}

// LCS_HORIZON_TRACE=1 - the same population as the horizon counts, but bucketed by WORLD bearing
// rather than by bearing relative to the camera, and printed once a second with the camera's own
// bearing beside it.
//
// That is the control the three counts cannot be: a distribution that stays peaked around whatever
// the camera happens to be pointing at is the game culling to its view, and one that stays put in
// the world while the camera turns is the game drawing what the streamer holds. Turn the camera
// between samples - the chase camera's block takes a look command - and read the two off each other.
static const int kBearingBuckets = 24;
static int s_bearingHist[kBearingBuckets];
static int s_bearingFrames;
static double s_bearingLast;
static bool s_horizonTrace;

// LCS_CACHE_TRACE=1 - what the caster cache is holding and replaying, once a second, WHILE THE
// PLAYER MOVES. Standing still is what hid the last failure in this area: a fixed spot holds
// still exactly the variable the streamer cares about, so a cache that collapses at every chunk
// swap reads as a healthy one. The line carries the three numbers that separate the failures -
// cells held (is it retaining), replayed (is any of it reaching the depth pass) and replayed
// BEHIND (is any of THAT the geometry a shadow behind the player needs) - beside the rebase and
// clear counts, because a cache that looks empty is usually a cache that was just emptied.
// LCS_STANDIN_SWITCH=<path> - a dev hatch for LOOKING at what stand-in cells contribute, which
// no counter can answer. While that file exists, a cell whose contents were committed beyond
// cacheReplaceRadius does not cast at all, so whatever disappears from the frame is exactly what
// the stand-ins were drawing. A file rather than an env var because the two pictures have to come
// from ONE session - a second run moves the clock, and the sun with it, and then the difference
// between the shots is the time of day rather than the setting.
static std::string s_standInSwitchPath;
static bool s_standInCasters = true;
static double s_standInSwitchLast;

static bool s_cacheTrace;
static double s_cacheTraceLast;
static int s_cacheTraceFrames;
static int s_cacheTraceCellsSum;
static int s_cacheTraceReplaySum;
static int s_cacheTraceBehindSum;
static int s_cacheTraceLiveBehindSum;
static int s_cacheTraceReplayMax;
static int s_cacheTraceBehindMax;
static int s_cacheTraceHeldStandInSum;
static int s_cacheTraceStandInSum;
static int s_cacheTraceStandInBehindSum;
static int s_cacheTraceStandInVertSum;
static int s_cacheTraceVertSum;
static int s_cacheTraceLastRebases;
static int s_cacheTraceLastClears;
static int s_cacheTraceSplitSum;
static int s_cacheTraceCapDropSum;

// Beyond this, an object's own width stops deciding what angle it is at.
//
// THE HISTOGRAM IS MEASURED ONLY OUT HERE, and the first version was not, which made it useless
// for the thing it was built to show. A frustum keeps whatever OVERLAPS it, so an object is kept
// while its centre is up to a frustum half-angle plus its own angular radius off forward - and
// close to the camera that radius is enormous. A bin three units to the player's left is at 80
// degrees whether the cull is 70 degrees wide or 160. Measured: opening the cull to 140 doubled
// the geometry the game handed over and moved the 99th percentile of the whole population from
// 97 degrees to 99, because the tail was the lamp posts either side of the player rather than
// anything the setting touches. At thirty units a ten-metre building is worth about nine degrees.
static const float kHorizonFarDistance = 30.0f;

// Files one object-sized draw into the horizon histogram.
static void NoteHorizon(const float centre[3]) {
	const float deg = YawFromForward(centre);
	if (deg < 0.0f) {
		return;
	}
	const float dx = centre[0] - s_frameCameraPos[0], dy = centre[1] - s_frameCameraPos[1];
	const bool far = dx * dx + dy * dy >= kHorizonFarDistance * kHorizonFarDistance;
	s_capture.thingDraws++;
	if (deg > s_capture.widestThingDeg) {
		s_capture.widestThingDeg = deg;
	}
	if (deg <= 45.0f) {
		s_capture.thingsAhead++;
	} else if (deg <= 90.0f) {
		s_capture.thingsBeside++;
	} else {
		s_capture.thingsBehind++;
	}
	if (far) {
		int bucket = (int)(deg / 15.0f);
		if (bucket < 0) bucket = 0;
		if (bucket >= CaptureStats::kAngleBuckets) bucket = CaptureStats::kAngleBuckets - 1;
		s_capture.angleHist[bucket]++;
		s_capture.farThingDraws++;
	}

	if (s_horizonTrace) {
		const float dx = centre[0] - s_frameCameraPos[0], dy = centre[1] - s_frameCameraPos[1];
		float bearing = atan2f(dy, dx) * (180.0f / 3.14159265f);
		if (bearing < 0.0f) bearing += 360.0f;
		int b = (int)(bearing / (360.0f / kBearingBuckets));
		if (b < 0) b = 0;
		if (b >= kBearingBuckets) b = kBearingBuckets - 1;
		s_bearingHist[b]++;
	}
}

// The share of far objects lying more than 60 degrees off the way the camera faces - the one
// number that moves with the cull and does not move with anything else.
//
// A PERCENTILE WAS TRIED FIRST AND IS USELESS HERE, which is worth a line because it looks like
// the obvious summary. The distribution is a plateau out to the cull edge and then a long thin
// tail of wide, near-ish surfaces, so the 99th percentile sits at about 100 degrees whatever the
// setting is - measured at 100, 96 and 97 across three positions whose geometry counts differ by
// a factor of nearly four. The plateau is the signal; the tail is not.
//
// 60 degrees sits just past the game's own edge rather than on it: a 70 degree view puts its
// shoulder at about 45 once an object's own width is allowed for. Measured shares at one spot -
// 2.7% with the row off, 4.6% at its widest, and 23.9% at a 140 degree cull that the engine turned
// out not to tolerate (see "It stops at 2x"). The SHAPE printed beside it is the better read of
// the two: the shoulder moves from bucket 2 to bucket 4 and that is unmistakable, while this one
// number only separates them by a couple of points at the widths actually offered.
static float ShareBeyond60(const int *hist, int total) {
	if (total <= 0) {
		return 0.0f;
	}
	int wide = 0;
	for (int i = 4; i < CaptureStats::kAngleBuckets; i++) {
		wide += hist[i];
	}
	return 100.0f * (float)wide / (float)total;
}

// See s_standInSwitchPath. Costs a stat() a second, and only when the hatch is armed.
static void PollStandInSwitch() {
	if (s_standInSwitchPath.empty()) {
		return;
	}
	const double now = time_now_d();
	if (now - s_standInSwitchLast < 1.0) {
		return;
	}
	s_standInSwitchLast = now;
	bool suppress = false;
	if (FILE *f = fopen(s_standInSwitchPath.c_str(), "rb")) {
		suppress = true;
		fclose(f);
	}
	if (suppress == s_standInCasters) {
		s_standInCasters = !suppress;
		NOTICE_LOG(Log::G3D, "LCS shadows: stand-in cells %s",
			s_standInCasters ? "cast again" : "are NOT casting (the dev hatch is set)");
	}
}

// Prints the world-bearing histogram once a second while LCS_HORIZON_TRACE is set.
static void ReportBearings() {
	if (!s_horizonTrace || !s_haveViewMatrix) {
		return;
	}
	s_bearingFrames++;
	const double now = time_now_d();
	if (now - s_bearingLast < 1.0) {
		return;
	}
	s_bearingLast = now;
	float camBearing = atan2f(s_frameCameraFwd[1], s_frameCameraFwd[0]) * (180.0f / 3.14159265f);
	if (camBearing < 0.0f) camBearing += 360.0f;
	int total = 0;
	for (int i = 0; i < kBearingBuckets; i++) {
		total += s_bearingHist[i];
	}
	char line[512];
	int at = 0;
	for (int i = 0; i < kBearingBuckets && at < (int)sizeof(line) - 8; i++) {
		at += snprintf(line + at, sizeof(line) - at, "%d ",
			total ? (int)(1000.0f * s_bearingHist[i] / total) : 0);
	}
	NOTICE_LOG(Log::G3D, "LCS horizon trace: camera at %.0f deg, %d draws over %d frames, per mille "
		"by world bearing from 0 in 15 deg steps: %s",
		camBearing, total, s_bearingFrames, line);
	memset(s_bearingHist, 0, sizeof(s_bearingHist));
	s_bearingFrames = 0;
}

// The cache's half of the same question ReportBearings asks about the game's own geometry.
static void ReportCache() {
	if (!s_cacheTrace) {
		return;
	}
	s_cacheTraceFrames++;
	s_cacheTraceCellsSum += s_capture.cachedEntries;
	s_cacheTraceReplaySum += s_capture.cachedDraws;
	s_cacheTraceBehindSum += s_capture.cachedBehind;
	s_cacheTraceLiveBehindSum += s_capture.thingsBehind;
	s_cacheTraceHeldStandInSum += s_capture.heldStandIn;
	s_cacheTraceStandInSum += s_capture.cachedStandIn;
	s_cacheTraceStandInBehindSum += s_capture.cachedStandInBehind;
	s_cacheTraceStandInVertSum += s_capture.cachedStandInVerts;
	s_cacheTraceVertSum += s_capture.cachedVertices;
	s_cacheTraceSplitSum += s_capture.cacheSplitDraws;
	s_cacheTraceCapDropSum += s_capture.cacheCapDrops;
	if (s_capture.cachedDraws > s_cacheTraceReplayMax) s_cacheTraceReplayMax = s_capture.cachedDraws;
	if (s_capture.cachedBehind > s_cacheTraceBehindMax) s_cacheTraceBehindMax = s_capture.cachedBehind;
	const double now = time_now_d();
	if (now - s_cacheTraceLast < 1.0 || s_cacheTraceFrames == 0) {
		return;
	}
	s_cacheTraceLast = now;
	const float f = (float)s_cacheTraceFrames;
	const float heldPct = s_cacheTraceCellsSum ? 100.0f * s_cacheTraceHeldStandInSum / (float)s_cacheTraceCellsSum : 0.0f;
	const float castPct = s_cacheTraceReplaySum ? 100.0f * s_cacheTraceStandInSum / (float)s_cacheTraceReplaySum : 0.0f;
	const float behindPct = s_cacheTraceBehindSum ? 100.0f * s_cacheTraceStandInBehindSum / (float)s_cacheTraceBehindSum : 0.0f;
	const float vertPct = s_cacheTraceVertSum ? 100.0f * s_cacheTraceStandInVertSum / (float)s_cacheTraceVertSum : 0.0f;
	NOTICE_LOG(Log::G3D,
		"LCS cache trace: %d frames | cells held %.0f | replayed %.1f (max %d) | "
		"replayed BEHIND %.1f (max %d) | live things behind %.1f | rebases +%d clears +%d | "
		"draws split across cells %.1f | triangles a full cell dropped %d "
		"|| STAND-IN: held %.0f%% | cast %.0f%% | cast behind %.0f%% | of the geometry %.0f%%",
		s_cacheTraceFrames, s_cacheTraceCellsSum / f, s_cacheTraceReplaySum / f,
		s_cacheTraceReplayMax, s_cacheTraceBehindSum / f, s_cacheTraceBehindMax,
		s_cacheTraceLiveBehindSum / f,
		s_cacheRebases - s_cacheTraceLastRebases, s_cacheClears - s_cacheTraceLastClears,
		s_cacheTraceSplitSum / f, s_cacheTraceCapDropSum,
		heldPct, castPct, behindPct, vertPct);
	s_cacheTraceLastRebases = s_cacheRebases;
	s_cacheTraceLastClears = s_cacheClears;
	s_cacheTraceFrames = 0;
	s_cacheTraceCellsSum = s_cacheTraceReplaySum = s_cacheTraceBehindSum = 0;
	s_cacheTraceLiveBehindSum = 0;
	s_cacheTraceReplayMax = s_cacheTraceBehindMax = 0;
	s_cacheTraceHeldStandInSum = s_cacheTraceStandInSum = s_cacheTraceStandInBehindSum = 0;
	s_cacheTraceStandInVertSum = s_cacheTraceVertSum = 0;
	s_cacheTraceSplitSum = s_cacheTraceCapDropSum = 0;
}

// Is this cell entirely inside the frame, so that what the game drew in it is the whole of it?
//
// All eight corners, not the centre. A 32-unit cell next to the camera has its centre on screen while
// half of it is past the edge, and the game draws only that half - which then REPLACED the whole
// cell's memory, so the shadows of props beside and behind the player went out as soon as they left
// the view. Reported as shadows that disappear although they had been cast once. The camera matrix is
// the one the mask rasterises with, so "in view" means exactly what it means to the game.
static bool CellIsWhollyInView(const float centre[3]) {
	if (!s_view.valid) {
		return false;
	}
	const float *m = s_view.cameraViewProj;
	const float half = kBucketSize * 0.5f;
	for (int i = 0; i < 8; i++) {
		const float p[3] = {
			centre[0] + ((i & 1) ? half : -half),
			centre[1] + ((i & 2) ? half : -half),
			centre[2] + ((i & 4) ? half : -half),
		};
		const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
		const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
		const float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
		const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
		if (w <= 0.0f || x < -w || x > w || y < -w || y > w || z < 0.0f || z > w) {
			return false;
		}
	}
	return true;
}

// Moves everything remembered by `shift` in X and Y, for a rebase of the space the GE is fed.
//
// The cache used to be thrown away on any camera jump over 40 units, on the reasoning that a rebase,
// a teleport and a load all look alike from the GE. They do not look alike from the game: its own
// camera, read in world space on the same frame, stays put across a rebase. The game rebases as you
// travel, so every rebase used to put out the remembered shadow of everything already passed. A
// rebase now shifts the cache by how far the space moved, and only a camera that really jumped
// clears it.
static void RebaseCachedCasters(const float shift[2]) {
	const auto shiftVerts = [shift](std::vector<float> &verts, size_t stride) {
		for (size_t i = 0; i + stride <= verts.size(); i += stride) {
			verts[i] += shift[0];
			verts[i + 1] += shift[1];
		}
	};
	std::unordered_map<u64, Bucket> moved;
	moved.reserve(s_buckets.size());
	for (auto &it : s_buckets) {
		Bucket &b = it.second;
		shiftVerts(b.pos, 3);
		shiftVerts(b.pendingPos, 3);
		shiftVerts(b.cutVerts, 5);
		shiftVerts(b.pendingCutVerts, 5);
		b.centre[0] += shift[0];
		b.centre[1] += shift[1];
		const u64 key = BucketKey(b.centre);
		moved.emplace(key, std::move(b));
	}
	s_buckets.swap(moved);
}

// Promotes this frame's cells over what they held, re-submits the cells nobody drew, and drops
// what has gone stale or out of range. Runs once, immediately before the passes.
static void SubmitCachedCasters(TextureCacheCommon *textureCache) {
	s_capture.cachedDraws = 0;
	s_capture.cachedVertices = 0;
	s_capture.cachedBytes = 0;
	if (!s_settings.cacheCasters || !s_haveViewMatrix) {
		s_capture.cachedEntries = (int)s_buckets.size();
		return;
	}

	const int holdFrames = (int)(s_settings.cacheHoldSeconds * 60.0f);
	const float radius = s_settings.cacheRadius;
	const float replaceRadius = s_settings.cacheReplaceRadius;
	int keptOverLiveDraw = 0;
	bool liveTexturesListed = false;

	for (auto it = s_buckets.begin(); it != s_buckets.end(); ) {
		Bucket &b = it->second;

		float distSq = 0.0f;
		for (int c = 0; c < 3; c++) {
			const float d = b.centre[c] - s_frameCameraPos[c];
			distSq += d * d;
		}

		if (b.touchedFrame == s_frameIndex) {
			// Drawn by the game this frame, so it is already in the streams. What was captured
			// replaces what was remembered, wholesale - which is the property that makes a grid
			// immune to the game re-batching its draws.
			//
			// But only when the whole cell was on screen to be captured. This is the bug that
			// survived two rounds of looking for it: as a building slides off the edge, the part
			// still visible keeps its cell alive and OVERWRITES the memory of the rest with just
			// that sliver - so the shadow shrank away exactly as the caster left the view, which
			// is the symptom the cache was built to remove.
			//
			// And only when it was seen from close up, for the same reason in a different
			// direction. Past cacheReplaceRadius the game draws this place as its LOD stand-in, or
			// as part of a building whose other parts it has stopped drawing, because the full-
			// detail radius is baked into the level archives. What was captured there is a lesser
			// version of what is remembered, and replacing one with the other is how a shadow came
			// apart as you walked away from its building.
			//
			// A cell that keeps its memory also REPLAYS it this frame, alongside the live draw.
			// Before, it only kept it for later, so while half a building was on screen it cast
			// half a shadow. Depth keeps the nearest surface, so the two agree wherever they
			// overlap and the remembered one fills in wherever the live one is short.
			//
			// An empty cell always takes what it is offered, or a cell that is never fully seen
			// would never hold anything at all.
			//
			// The plain half no longer trusts one frame's capture to be complete. Since every
			// triangle goes to the cell of its own centroid, a cell is touched by whatever else
			// was drawn there - and a building missing from THIS frame's capture (its flush merged
			// past maxCasterSpan, a resource still streaming) was wiped from memory by the
			// neighbours that were present, which made the shadows flicker more than before.
			// Static scenery does not shrink, so a capture with fewer triangles than the memory is
			// an incomplete one: the memory is kept and replayed over it. Cells nearer than about
			// forty units are never wholly on screen, so this is also the only way they improve.
			//
			// And it is a quality ladder, which is the LOD rule asked for: a close capture is full
			// detail and beats a far one, and between two captures from the same side of
			// cacheReplaceRadius the one with more triangles is the more detailed. The one way a
			// memory may get smaller is the light moving, since it changes which faces are kept.
			// Cut-outs still go by the flush, so they keep the older rule.
			const bool close = distSq <= replaceRadius * replaceRadius;
			const bool empty = b.pos.empty() && b.cutPieces.empty();
			const size_t heldTris = b.idx.size() / 3;
			const size_t seenTris = b.pendingIdx.size() / 3;
			const bool heldFar = b.fillDist > replaceRadius;
			const bool lightMoved = s_view.valid && Dot(b.commitLight, s_view.lightDir) < kCacheLightRefreshCos;
			bool replacePlain;
			if (empty || b.pos.empty()) {
				replacePlain = true;
			} else if (!close) {
				replacePlain = heldFar && seenTris >= heldTris;
			} else if (heldFar) {
				replacePlain = seenTris >= heldTris || CellIsWhollyInView(b.centre);
			} else {
				replacePlain = seenTris >= heldTris ||
					(lightMoved && (CellIsWhollyInView(b.centre) || seenTris * 10 >= heldTris * 9));
			}
			const bool replaceCut = empty || (close && CellIsWhollyInView(b.centre));
			if (replacePlain) {
				b.fillDist = sqrtf(distSq);
				b.pos.swap(b.pendingPos);
				b.idx.swap(b.pendingIdx);
				if (s_view.valid) {
					memcpy(b.commitLight, s_view.lightDir, sizeof(b.commitLight));
				}
			}
			if (replaceCut) {
				b.cutVerts.swap(b.pendingCutVerts);
				b.cutIdx.swap(b.pendingCutIdx);
				b.cutPieces.swap(b.pendingCutPieces);
			}
			b.pendingPos.clear();
			b.pendingIdx.clear();
			b.pendingCutVerts.clear();
			b.pendingCutIdx.clear();
			b.pendingCutPieces.clear();
			b.lastSeenFrame = s_frameIndex;
			const bool replace = replacePlain && replaceCut;
			if (replace) {
				s_capture.cachedBytes += b.pos.size() * sizeof(float) + b.idx.size() * sizeof(u16) +
					b.cutVerts.size() * sizeof(float) + b.cutIdx.size() * sizeof(u16);
				++it;
				continue;
			}
			keptOverLiveDraw++;
		} else if (distSq > radius * radius ||
				(holdFrames > 0 && s_frameIndex - b.lastSeenFrame > holdFrames)) {
			it = s_buckets.erase(it);
			continue;
		}

		// Kept because it may be needed a moment from now, but only submitted if it can reach the
		// cascade this frame - and "reach" is measured in the LIGHT's frame, not the camera's.
		// Across the light a cell has to be inside the box, because a shadow lands where its
		// caster is; along the light it can be most of a street away and still throw into it.
		bool reaches = true;
		if (s_view.valid) {
			float rel[3];
			for (int c = 0; c < 3; c++) {
				rel[c] = b.centre[c] - s_view.centre[c];
			}
			const float along = Dot(rel, s_view.lightDir);
			float acrossSq = 0.0f;
			for (int c = 0; c < 3; c++) {
				const float a = rel[c] - along * s_view.lightDir[c];
				acrossSq += a * a;
			}
			const float across = s_view.radius + kBucketSize;
			reaches = acrossSq < across * across &&
				along > -(s_view.casterReach + kBucketSize) && along < s_view.radius + kBucketSize;
		}
		// Is what this cell casts the real thing, or the stand-in it was holding the only time it
		// was ever seen from anywhere? See CaptureStats, and s_standInSwitchPath for the hatch
		// that stops them casting so the difference can be looked at rather than counted.
		const bool standIn = b.fillDist > replaceRadius;
		if (standIn && !s_standInCasters) {
			reaches = false;
		}

		if (reaches && b.idx.size() >= 3) {
			AppendGeometry(b.pos.data(), (int)(b.pos.size() / 3), b.idx.data(), (int)b.idx.size(),
				nullptr, 0);
			s_capture.cachedDraws++;
			s_capture.cachedVertices += (int)(b.pos.size() / 3);
			if (standIn) {
				s_capture.cachedStandIn++;
				s_capture.cachedStandInVerts += (int)(b.pos.size() / 3);
			}
			// Only cells the game is NOT drawing reach here, so this counts exactly the shadows
			// behind the player that exist because they were remembered. Kept apart from the live
			// half of the horizon measure - see CaptureStats.
			const float deg = YawFromForward(b.centre);
			if (deg > 90.0f) {
				s_capture.cachedBehind++;
				if (standIn) {
					s_capture.cachedStandInBehind++;
				}
			}
		}
		if (reaches && !b.cutPieces.empty() && s_settings.texturedCutouts) {
			if (!liveTexturesListed) {
				liveTexturesListed = true;
				s_liveViewIdentity.clear();
				s_liveIdentityView.clear();
				// LCS: every built texture's view, through the cache's public accessors (VCS added a
				// ForEachNativeTextureView to TextureCacheCommon for this).
				if (textureCache) {
					for (const TexCache *cache : { &textureCache->Cache(), &textureCache->SecondCache() }) {
						for (const auto &entryIt : *cache) {
							const TexCacheEntry *entry = entryIt.second.get();
							if (entry && entry->texturePtr) {
								NoteLiveTextureView(nullptr, textureCache->GetNativeTextureView(entry, true),
									entry->addr, entry->dim);
							}
						}
					}
				}
			}
			for (const CutPiece &piece : b.cutPieces) {
				const CutoutTex live{ ResolveCutoutTexture(piece.tex), piece.tex.addr, piece.tex.dim };
				if (!live.view) {
					s_capture.cutoutUnresolved++;
					continue;
				}
				AppendCutout(live, piece.alphaRef, piece.wrap, b.cutVerts.data() + (size_t)piece.firstVert * 5,
					piece.vertCount, b.cutIdx.data() + piece.firstIdx, piece.idxCount);
				s_capture.cutoutCachedDraws++;
			}
		}
		s_capture.cachedBytes += b.pos.size() * sizeof(float) + b.idx.size() * sizeof(u16) +
			b.cutVerts.size() * sizeof(float) + b.cutIdx.size() * sizeof(u16);
		++it;
	}

	s_capture.cachedEntries = (int)s_buckets.size();
	for (const auto &cell : s_buckets) {
		if (cell.second.fillDist > replaceRadius) {
			s_capture.heldStandIn++;
		}
	}

	// Once per boot, so a build where the cache silently never engages says so in the log rather
	// than only in a panel the Release build cannot open.
	// And the same for the rule that keeps a remembered cell over what the game drew there, which
	// is the one that makes a building's shadow survive the walk away from it.
	static bool s_saidKeptOverLive = false;
	if (!s_saidKeptOverLive && keptOverLiveDraw > 0) {
		s_saidKeptOverLive = true;
		NOTICE_LOG(Log::G3D, "LCS shadows: %d cells the game drew only in part or in low detail are casting their remembered shadow",
			keptOverLiveDraw);
	}

	static bool s_saidCacheWorks = false;
	if (!s_saidCacheWorks && s_capture.cachedDraws > 0) {
		s_saidCacheWorks = true;
		WARN_LOG(Log::G3D, "LCS shadows: the caster cache is carrying %d cells the game no longer draws (%d held)",
			s_capture.cachedDraws, s_capture.cachedEntries);
	}

	// THE CASTER HORIZON, said out loud once per boot. This is the measurement that decides
	// whether the request "make the game draw 360 degrees" has anything to aim at: if the game
	// never submits an object more than about 40 degrees off the way the camera faces, there is a
	// view-dependent cull to find and widen, and four address hunts have looked in the wrong
	// places. If it routinely submits things behind the player, there is not, and a shadow that
	// goes out behind you is this module's cache rather than the game's culling.
	//
	// Ten seconds of whatever the player happens to be doing, because one frame in a corridor
	// proves nothing either way - and NOTICE, because this build's log level drops WARNING.
	PollStandInSwitch();
	ReportBearings();
	ReportCache();

	static int s_horizonFrames = 0;
	static int s_horizonHist[CaptureStats::kAngleBuckets] = {};
	static int s_horizonThings = 0;
	static bool s_saidHorizon = false;
	if (!s_saidHorizon && s_capture.thingDraws > 0) {
		s_horizonFrames++;
		s_horizonThings += s_capture.farThingDraws;
		for (int i = 0; i < CaptureStats::kAngleBuckets; i++) {
			s_horizonHist[i] += s_capture.angleHist[i];
		}
		if (s_horizonFrames >= 600) {
			s_saidHorizon = true;
			char shape[128];
			int at = 0;
			for (int i = 0; i < CaptureStats::kAngleBuckets; i++) {
				at += snprintf(shape + at, sizeof(shape) - at, "%d ",
					s_horizonThings ? (int)(1000.0f * s_horizonHist[i] / s_horizonThings) : 0);
			}
			NOTICE_LOG(Log::G3D,
				"LCS shadows: caster horizon over %d frames - %d object draws past %.0f units, "
				"%.1f%% of them past 60 degrees off forward. Per mille by angle in 15 deg steps: %s"
				"(the SHOULDER is where the cull ends - about 45 deg on the retail cull, because a "
				"frustum keeps whatever overlaps it and a near object overlaps a lot of it)",
				s_horizonFrames, s_horizonThings, kHorizonFarDistance,
				ShareBeyond60(s_horizonHist, s_horizonThings), shape);
		}
	}
}

void AddCaster(const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, GEPrimitiveType prim, const float world[12]) {
	if (numDecodedVerts <= 0 || indexCount < 3 || s_frameComposited) {
		return;
	}

	float drawMin[3], drawMax[3];
	BakeDraw(decoded, numDecodedVerts, indices, indexCount, stride, posOffset, prim, world,
		drawMin, drawMax);
	if (s_drawIdx.size() < 3) {
		return;
	}

	// A draw sitting on top of the camera is one of the game's full-screen overlays, drawn as
	// world geometry. Dropped whole - it is neither a caster nor a receiver, and left in it wins
	// every pixel of the mask.
	if (s_haveViewMatrix && s_settings.nearCameraCutoff > 0.0f) {
		float furthest = 0.0f;
		for (int c = 0; c < 3; c++) {
			const float lo = fabsf(drawMin[c] - s_frameCameraPos[c]);
			const float hi = fabsf(drawMax[c] - s_frameCameraPos[c]);
			const float d = lo > hi ? lo : hi;
			furthest += d * d;
		}
		// ... but never a person, and never a vehicle part bigger than the overlay. The overlays are
		// flat unlit quads 0.6 units across; a camera pulled in behind the player put pieces of the
		// player and the car inside the same two units, and they lost their shadows for exactly the
		// frames the camera was close - measured at 5 to 10 such draws a frame while playing.
		const bool skinned = (gstate.vertType & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE;
		const bool withNormals = (gstate.vertType & GE_VTYPE_NRM_MASK) != GE_VTYPE_NRM_NONE;
		float extent = 0.0f;
		for (int c = 0; c < 3; c++) {
			const float span = drawMax[c] - drawMin[c];
			if (span > extent) extent = span;
		}
		if (furthest < s_settings.nearCameraCutoff * s_settings.nearCameraCutoff &&
			!skinned && !(withNormals && extent > 1.0f)) {
			s_capture.nearCameraDraws++;
			return;
		}
	}

	for (int c = 0; c < 3; c++) {
		if (!s_capture.boundsValid || drawMin[c] < s_capture.min[c]) s_capture.min[c] = drawMin[c];
		if (!s_capture.boundsValid || drawMax[c] > s_capture.max[c]) s_capture.max[c] = drawMax[c];
	}
	s_capture.boundsValid = true;

	// 500 units is far larger than any building here and far smaller than the map, so this
	// separates "one map-spanning ground or water quad" from "positions are being read at the
	// wrong stride". A bounding box cannot tell those apart; this count can.
	const float spanX = drawMax[0] - drawMin[0];
	const float spanY = drawMax[1] - drawMin[1];
	if (spanX > 500.0f || spanY > 500.0f) {
		s_capture.largeDraws++;
		if (numDecodedVerts > s_capture.largestDrawVerts) {
			s_capture.largestDrawVerts = numDecodedVerts;
		}
	}

	// Receivers, always. Casters, only if this draw is small enough to be a thing in the world
	// rather than the world itself - a sky or a map-spanning quad covers the whole cascade, and
	// one surface across the whole shadow map puts the entire city in its own shadow.
	//
	// In entity mode the same decision also asks whether this draw is a thing that moves, which
	// this game answers with vertex normals - see the setting for why that is a fact rather than
	// a guess. Receivers are untouched either way: the road still has to darken under the car.
	const bool isEntity = (gstate.vertType & GE_VTYPE_NRM_MASK) != GE_VTYPE_NRM_NONE;

	// ... and props, which are scenery by every test the game offers and are still things
	// rather than the world. Small on the ground, tall against it - see propMaxSpan.
	const float spanZ = drawMax[2] - drawMin[2];
	const bool isProp = !isEntity && s_settings.propCasters &&
		spanX <= s_settings.propMaxSpan && spanY <= s_settings.propMaxSpan &&
		spanZ >= s_settings.propMinHeight;

	// Thing-sized, before the mode filter has an opinion: the horizon measure is asking what the
	// GAME submitted, and answering it with our own caster setting would be answering a different
	// question. The span test alone is what drops the sky and the map-spanning quads, whose centre
	// is not a place anything is.
	const bool thingSized = spanX <= s_settings.maxCasterSpan && spanY <= s_settings.maxCasterSpan;
	if (thingSized) {
		const float centre[3] = {
			(drawMin[0] + drawMax[0]) * 0.5f,
			(drawMin[1] + drawMax[1]) * 0.5f,
			(drawMin[2] + drawMax[2]) * 0.5f,
		};
		NoteHorizon(centre);
	}

	const bool casts = spanX <= s_settings.maxCasterSpan && spanY <= s_settings.maxCasterSpan &&
		(!s_settings.entityCastersOnly || isEntity || isProp);
	if (!casts) {
		s_capture.receiverOnlyDraws++;
	} else if (isProp) {
		s_capture.casterPropDraws++;
	}

	// Object-sized draws are what a flat quad has to be lying under to be a blob shadow.
	//
	// Deliberately NOT gated on `casts`. This list exists only for the blob learner, and
	// in people-and-vehicles mode `casts` is false for all the scenery - so the list lost
	// everything a blob actually sits on and the learner stopped matching entirely:
	// measured at 400 ground quads, 96 of them flat and small, and 0 over an object. The
	// vanilla blob then stayed under every ped and car in the one mode whose whole point
	// is that those things have real shadows instead.
	if (s_settings.hideBlobShadows && spanX <= 20.0f && spanY <= 20.0f) {
		ObjectBounds b;
		for (int c = 0; c < 3; c++) {
			b.min[c] = drawMin[c];
			b.max[c] = drawMax[c];
		}
		s_objectBounds.push_back(b);
	}

	if (casts && (gstate.vertType & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE) {
		s_capture.casterSkinnedDraws++;
	}

	// Skinned means a person, which is the one receiver that needs the heavier bias - and the one
	// the ambient occlusion goes easy on, which needs them in their own draw to mark them.
	const bool isSkinned = (gstate.vertType & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE;
	AppendGeometry(s_drawPos.data(), numDecodedVerts,
		casts ? s_drawCastIdx.data() : nullptr, casts ? (int)s_drawCastIdx.size() : 0,
		s_drawIdx.data(), (int)s_drawIdx.size(),
		isSkinned && (s_settings.pedReceiverBias > 0.0f || s_settings.ambientOcclusion));
	s_capture.draws++;

	// What may be REMEMBERED is narrower than what casts, and the rule is one question: can
	// this thing move? A cell is replayed for cacheHoldSeconds after the thing left the view,
	// so anything that can drive away would be left standing in the road.
	//
	// Skinned meshes are people and are never remembered - their vertices arrive already in
	// pose, so a remembered copy is somebody frozen mid-stride. A vehicle is not skinned, which
	// is why people-and-vehicles mode used to remember nothing at all. A PROP is the exception
	// that makes the cache worth having in that mode: it is the only thing there that cannot
	// move, so a lamp post keeps casting after you have driven past it.
	//
	// In the everything mode a vehicle used to be remembered as well, and the 25-second hold was
	// what stopped a parked car's shadow outliving the car. Scenery is kept for good now, so that
	// clock is gone - and anything with vertex normals, which in this game is exactly the set of
	// things that move, stays out of the cache in every mode instead.
	const bool canMove = s_settings.entityCastersOnly ? !isProp : isEntity;
	if (casts && !canMove &&
		(gstate.vertType & GE_VTYPE_WEIGHT_MASK) == GE_VTYPE_WEIGHT_NONE) {
		RememberCaster(numDecodedVerts);
	}
}

// A surface drawn with a blend or a cut-out texture that still writes depth - a pavement edge, a road
// decal, a dirt overlay - is somewhere a shadow lands, even though it can cast nothing as a solid.
//
// These used to be dropped whole by the caster filter, receivers included. The mask decides what a
// pixel is by the nearest captured receiver, so where the ground itself was one of these draws there
// was no receiver at all and the pixel read as lit.
//
// Added while chasing a shadow that vanished beside a pavement seam, and it was NOT that bug - the seam
// was a newly streamed chunk in a different camera space; see PrepareViewCorrection. Kept because a
// shadow landing on a pavement edge or a decal is right regardless.
//
// Only surfaces lying roughly flat qualify. A blended plane standing up in front of the camera - haze,
// glare, a window - would cover everything behind it in the mask and put the shadows out instead.
// People and vehicles are left out: a fading ped is blended, and they have their own receiver stream.
void AddReceiver(const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, GEPrimitiveType prim, const float world[12]) {
	if (numDecodedVerts <= 0 || indexCount < 3 || s_frameComposited || gstate.isModeThrough()) {
		return;
	}
	if ((gstate.vertType & (GE_VTYPE_NRM_MASK | GE_VTYPE_WEIGHT_MASK)) != 0) {
		return;
	}
	float drawMin[3], drawMax[3];
	BakeDraw(decoded, numDecodedVerts, indices, indexCount, stride, posOffset, prim, world,
		drawMin, drawMax);
	if (s_drawIdx.size() < 3) {
		return;
	}
	const float spanX = drawMax[0] - drawMin[0];
	const float spanY = drawMax[1] - drawMin[1];
	const float spanZ = drawMax[2] - drawMin[2];
	const float footprint = spanX > spanY ? spanX : spanY;
	if (footprint < 0.5f || spanZ > 3.0f || spanZ > 0.25f * footprint) {
		return;
	}
	// The full-screen overlays sit on the camera; a blended one would win every pixel of the mask.
	if (s_haveViewMatrix && s_settings.nearCameraCutoff > 0.0f) {
		float furthest = 0.0f;
		for (int c = 0; c < 3; c++) {
			const float lo = fabsf(drawMin[c] - s_frameCameraPos[c]);
			const float hi = fabsf(drawMax[c] - s_frameCameraPos[c]);
			const float d = lo > hi ? lo : hi;
			furthest += d * d;
		}
		if (furthest < s_settings.nearCameraCutoff * s_settings.nearCameraCutoff) {
			return;
		}
	}
	AppendGeometry(s_drawPos.data(), numDecodedVerts, nullptr, 0, s_drawIdx.data(), (int)s_drawIdx.size());
	s_capture.blendedReceiverDraws++;
	static bool s_saidBlendedReceivers = false;
	if (!s_saidBlendedReceivers) {
		s_saidBlendedReceivers = true;
		NOTICE_LOG(Log::G3D, "LCS shadows: a flat blended or cut-out surface is receiving shadows (%.1f x %.1f, %.2f high)",
			spanX, spanY, spanZ);
	}
}

void AddCutoutCaster(const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, int uvFormat, int uvOffset, GEPrimitiveType prim, const float world[12]) {
	s_cutPending = false;
	if (!s_settings.texturedCutouts || s_frameComposited || numDecodedVerts <= 0 ||
		numDecodedVerts > kMaxBatchVertices || indexCount < 3 || uvFormat != DEC_FLOAT_2) {
		return;
	}
	// Texture coordinates as the vertices carry them, and only that. The decoder has already
	// applied the game's UV scale and offset in this mode, so what it hands over is exactly what
	// the game samples with; a projected or environment-mapped cut-out would need the texture
	// matrix and the lights replayed, and nothing in this game draws foliage that way.
	const GETexMapMode uvGen = gstate.getUVGenMode();
	if (uvGen != GE_TEXMAP_TEXTURE_COORDS && uvGen != GE_TEXMAP_UNKNOWN) {
		return;
	}

	float drawMin[3], drawMax[3];
	BakeDraw(decoded, numDecodedVerts, indices, indexCount, stride, posOffset, prim, world,
		drawMin, drawMax);
	if (s_drawIdx.size() < 3) {
		return;
	}

	if (s_haveViewMatrix && s_settings.nearCameraCutoff > 0.0f) {
		float furthest = 0.0f;
		for (int c = 0; c < 3; c++) {
			const float lo = fabsf(drawMin[c] - s_frameCameraPos[c]);
			const float hi = fabsf(drawMax[c] - s_frameCameraPos[c]);
			const float d = lo > hi ? lo : hi;
			furthest += d * d;
		}
		if (furthest < s_settings.nearCameraCutoff * s_settings.nearCameraCutoff) {
			return;
		}
	}

	const float spanX = drawMax[0] - drawMin[0];
	const float spanY = drawMax[1] - drawMin[1];
	const float spanZ = drawMax[2] - drawMin[2];
	// A flat cut-out is a decal - a grate, leaves painted on a path - and lies on the very surface
	// it would shadow.
	if (spanZ < 0.5f) {
		return;
	}
	const bool isEntity = (gstate.vertType & GE_VTYPE_NRM_MASK) != GE_VTYPE_NRM_NONE;
	const bool isSkinned = (gstate.vertType & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE;

	// The size rule is judged per connected PIECE of the draw, not per draw.
	//
	// A flush is not an object. The game hands over every copy of a texture across a wide area in
	// one go - draws measured here span 500 to 3000 units - so a row of palms sharing a frond texture
	// arrived as one draw far wider than any palm, and the whole row cast nothing while a lone palm
	// beside it cast. Reported exactly that way: some palms fixed, others not. A card or a crown is
	// its own connected mesh, and that is what the rule was always meant to measure.
	//
	// Flatness stays a question about the whole draw: a single frond card can lie nearly level, and
	// judging that per piece would throw away the crown.
	const int vertCount = numDecodedVerts;
	s_cutParent.resize((size_t)vertCount);
	for (int i = 0; i < vertCount; i++) {
		s_cutParent[i] = i;
	}
	const auto findRoot = [](int v) {
		while (s_cutParent[v] != v) {
			s_cutParent[v] = s_cutParent[s_cutParent[v]];
			v = s_cutParent[v];
		}
		return v;
	};
	const size_t triIndices = s_drawIdx.size() - s_drawIdx.size() % 3;
	for (size_t t = 0; t < triIndices; t += 3) {
		const int a = findRoot(s_drawIdx[t]);
		const int b = findRoot(s_drawIdx[t + 1]);
		if (b != a) {
			s_cutParent[b] = a;
		}
		const int c = findRoot(s_drawIdx[t + 2]);
		if (c != a) {
			s_cutParent[c] = a;
		}
	}
	s_cutBox.resize((size_t)vertCount * 6);
	s_cutVerdict.assign((size_t)vertCount, 0);
	for (size_t t = 0; t < triIndices; t += 3) {
		const int r = findRoot(s_drawIdx[t]);
		float *box = s_cutBox.data() + (size_t)r * 6;
		if (s_cutVerdict[r] == 0) {
			s_cutVerdict[r] = 1;
			box[0] = box[1] = box[2] = 1e30f;
			box[3] = box[4] = box[5] = -1e30f;
		}
		for (int k = 0; k < 3; k++) {
			const float *p = s_drawPos.data() + (size_t)s_drawIdx[t + k] * 3;
			for (int c = 0; c < 3; c++) {
				if (p[c] < box[c]) box[c] = p[c];
				if (p[c] > box[3 + c]) box[3 + c] = p[c];
			}
		}
	}
	for (int r = 0; r < vertCount; r++) {
		if (s_cutVerdict[r] != 1) {
			continue;
		}
		const float *box = s_cutBox.data() + (size_t)r * 6;
		const float pieceX = box[3] - box[0];
		const float pieceY = box[4] - box[1];
		// On HIGH, cut-out scenery counts as a prop at ANY size a caster may be at all. The prop
		// footprint is there because a wide solid draw on that setting is a building; a wide cut-out
		// is still foliage or a fence. A palm's crown is wider than any lamp post, and measuring it
		// against one is how the fronds cast on ULTRA and nothing on HIGH.
		const bool casts = pieceX <= s_settings.maxCasterSpan && pieceY <= s_settings.maxCasterSpan &&
			(!s_settings.entityCastersOnly || isEntity || s_settings.propCasters);
		s_cutVerdict[r] = casts ? 2 : 3;
	}
	s_cutDrawIdx.clear();
	for (size_t t = 0; t < triIndices; t += 3) {
		if (s_cutVerdict[findRoot(s_drawIdx[t])] == 2) {
			s_cutDrawIdx.push_back(s_drawIdx[t]);
			s_cutDrawIdx.push_back(s_drawIdx[t + 1]);
			s_cutDrawIdx.push_back(s_drawIdx[t + 2]);
		}
	}
	if (s_cutDrawIdx.size() < 3) {
		return;
	}

	// Every triangle, not only the half facing away from the light. The back-face rule works by
	// putting an object's thickness between caster and receiver, and a frond is a single card
	// with no thickness: half of the crown would simply vanish.
	s_cutDrawVerts.resize((size_t)numDecodedVerts * 5);
	float *out = s_cutDrawVerts.data();
	const float *pos = s_drawPos.data();
	for (int i = 0; i < numDecodedVerts; i++) {
		const float *uv = (const float *)(decoded + (size_t)i * stride + uvOffset);
		out[0] = pos[0];
		out[1] = pos[1];
		out[2] = pos[2];
		out[3] = uv[0];
		out[4] = uv[1];
		out += 5;
		pos += 3;
	}

	// The game's own threshold where it has one, so the shadow's silhouette is the drawn one - and
	// the middle where it blends instead, which is where a soft frond edge reads as leaf.
	float ref = 0.5f;
	if (gstate.isAlphaTestEnabled()) {
		const GEComparison fn = gstate.getAlphaTestFunction();
		if (fn == GE_COMP_GREATER) {
			ref = ((float)gstate.getAlphaTestRef() + 0.5f) / 255.0f;
		} else if (fn == GE_COMP_GEQUAL) {
			ref = ((float)gstate.getAlphaTestRef() - 0.5f) / 255.0f;
		}
	}
	if (ref < kMinCutoutAlpha) ref = kMinCutoutAlpha;
	if (ref > 0.98f) ref = 0.98f;

	s_cutPendingRef = ref;
	s_cutPendingWrap = (u8)((gstate.isTexCoordClampedS() ? 1 : 0) | (gstate.isTexCoordClampedT() ? 2 : 0));
	s_cutPendingAddr = gstate.getTextureAddress(0);
	s_cutPendingDim = gstate.getTextureDimension(0);
	s_cutPendingRemember = !isEntity && !isSkinned;
	for (int c = 0; c < 3; c++) {
		s_cutPendingCentre[c] = (drawMin[c] + drawMax[c]) * 0.5f;
	}
	s_cutPending = true;
}

void NoteCutoutTexture(void *imageView) {
	if (!s_cutPending) {
		return;
	}
	s_cutPending = false;
	// A framebuffer bound as a texture has no cache entry to find again, and a palette the shader
	// expands leaves indices where the alpha should be.
	if (!imageView || gstate_c.textureIsFramebuffer || gstate_c.textureIsArray ||
		gstate_c.shaderDepalMode != ShaderDepalMode::OFF) {
		s_capture.cutoutSkipped++;
		return;
	}
	const CutoutTex tex{ imageView, s_cutPendingAddr, s_cutPendingDim };
	AppendCutout(tex, s_cutPendingRef, s_cutPendingWrap, s_cutDrawVerts.data(),
		(u32)(s_cutDrawVerts.size() / 5), s_cutDrawIdx.data(), (u32)s_cutDrawIdx.size());
	s_capture.cutoutDraws++;
	// Scenery cannot move, so it is remembered like any other; a cut-out on a vehicle is not.
	if (s_cutPendingRemember) {
		RememberCutout(tex, s_cutPendingRef, s_cutPendingWrap);
	}
}

// --- the game's own blob shadows ---------------------------------------------------------
//
// A flat, small, alpha-blended quad that writes no depth and lies directly underneath something
// the capture accepted is that object's shadow. That is the whole test, and it is positional
// rather than about render state, because the render state a blob uses is the render state a
// decal uses. A tyre mark on open road has nothing above it.

static bool IsLearnedBlobTexture(u32 textureAddr) {
	for (int i = 0; i < s_blobTextureCount; i++) {
		if (s_blobTextures[i] == textureAddr) {
			return true;
		}
	}
	return false;
}

// A blend that can only add light: the destination is kept whole and the source goes on top.
//
// GTA draws two kinds of decal through the same shadow code - SHADOWTYPE_DARK and INVCOLOR, which
// are the blob under a ped or a car, and SHADOWTYPE_ADDITIVE, which is the pool of light a street
// lamp or a headlight throws on the road. Both are small, flat, write no depth and lie under
// something, so the positional test could not tell them apart, and the lamp's pool was hidden for
// as long as the lamp post was on screen - and popped back the moment it left, which was reported
// as a hard-edged shadow appearing with the lamp just behind the camera.
static bool BlendOnlyBrightens() {
	if (!gstate.isAlphaBlendEnabled()) {
		return false;
	}
	if (gstate.getBlendEq() == GE_BLENDMODE_MAX) {
		return true;
	}
	return gstate.getBlendEq() == GE_BLENDMODE_MUL_AND_ADD &&
		gstate.getBlendFuncB() == GE_DSTBLEND_FIXB && gstate.getFixB() == 0xFFFFFF;
}

void NoteGroundQuad(const u8 *decoded, int numDecodedVerts, int stride, int posOffset,
	const float world[12], u32 textureAddr) {
	if (!g_active || !s_settings.hideBlobShadows || numDecodedVerts < 3 || !textureAddr) {
		return;
	}
	if (BlendOnlyBrightens()) {
		return;
	}
	if (IsLearnedBlobTexture(textureAddr)) {
		s_blobThisDraw = true;
		return;
	}

	float qMin[3] = { 1e30f, 1e30f, 1e30f };
	float qMax[3] = { -1e30f, -1e30f, -1e30f };
	for (int i = 0; i < numDecodedVerts; i++) {
		const float *p = (const float *)(decoded + (size_t)i * stride + posOffset);
		const float v[3] = {
			p[0] * world[0] + p[1] * world[3] + p[2] * world[6] + world[9],
			p[0] * world[1] + p[1] * world[4] + p[2] * world[7] + world[10],
			p[0] * world[2] + p[1] * world[5] + p[2] * world[8] + world[11],
		};
		for (int c = 0; c < 3; c++) {
			if (v[c] < qMin[c]) qMin[c] = v[c];
			if (v[c] > qMax[c]) qMax[c] = v[c];
		}
	}

	// Flat, and no bigger than a vehicle. A blob is a decal on the ground; anything with height
	// is something else entirely.
	if (qMax[2] - qMin[2] > 1.5f) {
		return;
	}
	if (qMax[0] - qMin[0] > 12.0f || qMax[1] - qMin[1] > 12.0f) {
		return;
	}

	const float cx = (qMin[0] + qMax[0]) * 0.5f;
	const float cy = (qMin[1] + qMax[1]) * 0.5f;
	for (const ObjectBounds &b : s_objectBounds) {
		if (cx < b.min[0] || cx > b.max[0] || cy < b.min[1] || cy > b.max[1]) {
			continue;
		}
		// Underneath it, not floating somewhere above it. A ped's own feet are the bottom of its
		// bounds, and the blob sits within a stride of them.
		if (qMax[2] > b.min[2] + 2.0f || qMax[2] < b.min[2] - 3.0f) {
			continue;
		}
		BlobCandidate *candidate = nullptr;
		for (int i = 0; i < s_blobCandidateCount; i++) {
			if (s_blobCandidates[i].addr == textureAddr) {
				candidate = &s_blobCandidates[i];
				break;
			}
		}
		if (!candidate) {
			if (s_blobCandidateCount >= kMaxBlobCandidates) {
				return;
			}
			candidate = &s_blobCandidates[s_blobCandidateCount++];
			candidate->addr = textureAddr;
			candidate->sightings = 0;
		}
		// Skip it NOW, whatever the sighting count says. Learning still matters - it is what
		// keeps a blob suppressed on frames where whatever is standing over it did not reach the
		// capture - but waiting for it before suppressing anything is what made every chunk load
		// flash a blob.
		s_blobThisDraw = true;
		if (++candidate->sightings < kBlobSightingsToLearn) {
			return;
		}
		if (s_blobTextureCount < kMaxBlobTextures) {
			s_blobTextures[s_blobTextureCount++] = textureAddr;
			WARN_LOG(Log::G3D, "LCS shadows: %08x is a blob shadow texture (%d known)",
				textureAddr, s_blobTextureCount);
		}
		return;
	}
}

bool ShouldSkipDraw() {
	const bool positional = s_blobThisDraw;
	s_blobThisDraw = false;
	if (!g_active || !s_settings.hideBlobShadows) {
		return false;
	}
	if (positional) {
		s_capture.blobDraws++;
		s_blobByPosition++;
		return true;
	}
	if (s_blobTextureCount == 0) {
		return false;
	}
	// The shape as well as the texture. A learned texture is one this game happens to use for
	// blobs, and nothing says it is used for nothing else - the through-mode HUD in particular
	// is not ours to edit.
	if (gstate.isModeThrough() || gstate.isDepthWriteEnabled() || !gstate.isAlphaBlendEnabled() ||
		BlendOnlyBrightens()) {
		return false;
	}
	if (!IsLearnedBlobTexture(gstate.getTextureAddress(0))) {
		return false;
	}
	s_capture.blobDraws++;
	s_blobByTexture++;
	return true;
}

// LCS: VCS's draw engine did this inline at both of its decode sites; one call keeps the upstream
// file's diff to a few lines.
bool CaptureDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount, const u8 *decoded,
		int numDecodedVerts, const u16 *indices, int stride, int posOffset, int uvFormat, int uvOffset,
		const float world[12], u32 textureAddr) {
	const Reject reject = ClassifyDraw(prim, vertTypeID, vertexCount);
	if (reject == Reject::None) {
		AddCaster(decoded, numDecodedVerts, indices, vertexCount, stride, posOffset, prim, world);
	} else if (reject == Reject::NoDepthWrite && numDecodedVerts <= 64) {
		// Small, writes no depth: a candidate for the game's own blob shadow. Offered with its
		// geometry, because where it is is the only thing that separates one from a decal.
		NoteGroundQuad(decoded, numDecodedVerts, stride, posOffset, world, textureAddr);
	} else if (reject == Reject::Cutout) {
		// Foliage: cast through its texture, which is only settled after this. A flat cut-out (a
		// pavement edge) is somewhere for shadows to land instead.
		AddReceiver(decoded, numDecodedVerts, indices, vertexCount, stride, posOffset, prim, world);
		AddCutoutCaster(decoded, numDecodedVerts, indices, vertexCount, stride, posOffset, uvFormat,
			uvOffset, prim, world);
	} else if (reject == Reject::Blended) {
		AddReceiver(decoded, numDecodedVerts, indices, vertexCount, stride, posOffset, prim, world);
	}
	return ShouldSkipDraw();
}

const CaptureStats &LastCapture() {
	return s_capturePublished;
}

Settings &GetSettings() {
	return s_settings;
}

const ShadowView &View() {
	return s_view;
}

// --- the depth pass ---------------------------------------------------------------------------
//
// One render pass, one pipeline, and one draw per 64k vertices. All of the complexity that a
// shadow pass normally carries - a pipeline per vertex format, per-draw world matrices, buffer
// lifetimes - was spent up front in the capture instead, which bakes everything to world space
// and flattens it to a single triangle list.

struct ShadowUB {
	float lightViewProj[16];
};

static const UniformBufferDesc s_shadowUBDesc{ sizeof(ShadowUB), {
	{ "u_lightViewProj", 0, -1, UniformType::MATRIX4X4, 0 },
} };

static Draw::Framebuffer *s_fbo;
static Draw::Pipeline *s_pipeline;
static int s_fboSize;
static bool s_depthSetupFailed;

static void ReleaseResources() {
	if (s_pipeline) {
		s_pipeline->Release();
		s_pipeline = nullptr;
	}
	if (s_fbo) {
		s_fbo->Release();
		s_fbo = nullptr;
	}
	s_fboSize = 0;
}

static bool EnsureResources(Draw::DrawContext *draw) {
	if (s_fbo && s_pipeline && s_fboSize == s_settings.mapSize) {
		return true;
	}
	// Setup that failed once will fail identically every frame. Retrying it rebuilds shaders at
	// frame rate, which costs far more than the feature it is trying to enable.
	if (s_depthSetupFailed) {
		return false;
	}
	ReleaseResources();

	using namespace Draw;
	const int size = s_settings.mapSize;

	// thin3d always gives a colour attachment alongside the depth one. We never write to it -
	// the blend state masks every channel off - so it costs memory and nothing else.
	// Two tiles side by side: the near cascade on the left, the far one on the right. One
	// texture rather than two keeps the mask to a single sampler, which is what makes the
	// split a change to the shader rather than to the pipeline's descriptor layout.
	s_fbo = draw->CreateFramebuffer({ size * 2, size, 1, 1, 0, true, "lcs_shadow" });
	if (!s_fbo) {
		return false;
	}
	s_fboSize = size;

	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();

	char *vsCode = new char[4096];
	{
		ShaderWriter writer(vsCode, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec3", "a_position", Draw::SEM_POSITION } };
		static const UniformDef uniforms[] = { { "mat4", "u_lightViewProj", 0 } };
		writer.BeginVSMain(inputs, uniforms, Slice<VaryingDef>::empty());
		// Row-vector, same as every other matrix in this file and the same as the game's own
		// vertex path. ShaderWriter's mul() is defined as (x * y), which in GLSL is exactly that.
		writer.C("  gl_Position = mul(vec4(a_position, 1.0), u_lightViewProj);\n");
		writer.EndVSMain(Slice<VaryingDef>::empty());
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)vsCode, strlen(vsCode), "lcs_shadow_vs");
	delete[] vsCode;

	char *fsCode = new char[4096];
	{
		ShaderWriter writer(fsCode, lang, ShaderStage::Fragment);
		writer.BeginFSMain(Slice<UniformDef>::empty(), Slice<VaryingDef>::empty());
		// Nothing is written here. The pass exists for the depth it leaves behind, but a pipeline
		// still needs a fragment stage.
		writer.C("  vec4 outColor = vec4(1.0, 1.0, 1.0, 1.0);\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *fs = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)fsCode, strlen(fsCode), "lcs_shadow_fs");
	delete[] fsCode;

	if (!vs || !fs) {
		if (vs) vs->Release();
		if (fs) fs->Release();
		ReleaseResources();
		s_depthSetupFailed = true;
		return false;
	}

	static const InputLayoutDesc layoutDesc = {
		12,
		{ { SEM_POSITION, DataFormat::R32G32B32_FLOAT, 0 } },
	};
	InputLayout *inputLayout = draw->CreateInputLayout(layoutDesc);

	DepthStencilStateDesc dsDesc{};
	dsDesc.depthTestEnabled = true;
	dsDesc.depthWriteEnabled = true;
	dsDesc.depthCompare = Comparison::LESS;
	DepthStencilState *depthStencil = draw->CreateDepthStencilState(dsDesc);

	// No colour writes at all.
	BlendState *blend = draw->CreateBlendState({ false, 0x0 });

	// Deliberately no face culling. Culling back faces is the usual first move against shadow
	// acne, and it assumes closed geometry - which a city built out of single-sided walls and
	// alpha-tested fences is not. The bias terms in the mask are where acne is answered instead.
	RasterState *raster = draw->CreateRasterState({});

	PipelineDesc desc{
		Primitive::TRIANGLE_LIST,
		{ vs, fs },
		inputLayout,
		depthStencil,
		blend,
		raster,
		&s_shadowUBDesc,
	};
	s_pipeline = draw->CreateGraphicsPipeline(desc, "lcs_shadow");

	vs->Release();
	fs->Release();
	inputLayout->Release();
	depthStencil->Release();
	blend->Release();
	raster->Release();

	if (!s_pipeline) {
		ReleaseResources();
		s_depthSetupFailed = true;
		return false;
	}
	return true;
}

// --- the cut-out depth pass ------------------------------------------------------------------
//
// The same depth map and the same light matrix, drawn a second way: position and texture
// coordinate in, the texture's alpha tested against the piece's threshold, the holes discarded.
// One draw call per texture, which is the price of the texture and why the plain pass does not work
// like this.

struct CutoutUB {
	float lightViewProj[16];
	float params[4];   // alpha threshold, unused x3
};

static const UniformBufferDesc s_cutoutUBDesc{ sizeof(CutoutUB), {
	{ "u_lightViewProj", 0, -1, UniformType::MATRIX4X4, 0 },
	{ "u_cutoutParams", 1, 0, UniformType::FLOAT4, 64 },
} };

static Draw::Pipeline *s_cutPipeline;
static Draw::SamplerState *s_cutSamplers[4];   // indexed by CutoutGroup::wrap
static bool s_cutSetupFailed;

static void ReleaseCutoutResources() {
	if (s_cutPipeline) {
		s_cutPipeline->Release();
		s_cutPipeline = nullptr;
	}
	for (Draw::SamplerState *&sampler : s_cutSamplers) {
		if (sampler) {
			sampler->Release();
			sampler = nullptr;
		}
	}
}

static bool EnsureCutoutResources(Draw::DrawContext *draw) {
	if (s_cutPipeline) {
		return true;
	}
	if (s_cutSetupFailed) {
		return false;
	}

	using namespace Draw;
	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	static const SamplerDef samplers[] = { { 0, "cutoutTex", SamplerFlags(0) } };
	static const VaryingDef varyings[] = {
		{ "vec2", "v_texcoord", Draw::SEM_TEXCOORD0, 0, "highp" },
	};
	// Declared identically in both stages - one block, one binding - for the reason the mask
	// pass gives.
	static const UniformDef uniforms[] = {
		{ "mat4", "u_lightViewProj", 0 },
		{ "vec4", "u_cutoutParams", 1 },
	};
	static const InputDef inputs[] = {
		{ "vec3", "a_position", Draw::SEM_POSITION },
		{ "vec2", "a_texcoord0", Draw::SEM_TEXCOORD0 },
	};

	char *vsCode = new char[4096];
	{
		ShaderWriter writer(vsCode, lang, ShaderStage::Vertex);
		writer.BeginVSMain(inputs, uniforms, varyings);
		writer.C("  v_texcoord = a_texcoord0;\n");
		writer.C("  gl_Position = mul(vec4(a_position, 1.0), u_lightViewProj);\n");
		writer.EndVSMain(varyings);
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)vsCode, strlen(vsCode), "lcs_shadow_cutout_vs");
	delete[] vsCode;

	char *fsCode = new char[4096];
	{
		ShaderWriter writer(fsCode, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(samplers);
		writer.BeginFSMain(uniforms, varyings);
		writer.C("  vec4 texColor = ").SampleTexture2D("cutoutTex", "v_texcoord").C(";\n");
		writer.C("  if (texColor.a < u_cutoutParams.x) {\n");
		writer.C("    discard;\n");
		writer.C("  }\n");
		writer.C("  vec4 outColor = vec4(1.0, 1.0, 1.0, 1.0);\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *fs = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)fsCode, strlen(fsCode), "lcs_shadow_cutout_fs");
	delete[] fsCode;

	if (!vs || !fs) {
		ERROR_LOG(Log::G3D, "LCS shadows: cut-out %s shader failed to compile - palms cast nothing",
			vs ? "fragment" : "vertex");
		if (vs) vs->Release();
		if (fs) fs->Release();
		s_cutSetupFailed = true;
		return false;
	}

	static const InputLayoutDesc layoutDesc = {
		20,
		{
			{ SEM_POSITION, DataFormat::R32G32B32_FLOAT, 0 },
			{ SEM_TEXCOORD0, DataFormat::R32G32_FLOAT, 12 },
		},
	};
	InputLayout *inputLayout = draw->CreateInputLayout(layoutDesc);

	DepthStencilStateDesc dsDesc{};
	dsDesc.depthTestEnabled = true;
	dsDesc.depthWriteEnabled = true;
	dsDesc.depthCompare = Comparison::LESS;
	DepthStencilState *depthStencil = draw->CreateDepthStencilState(dsDesc);
	BlendState *blend = draw->CreateBlendState({ false, 0x0 });
	RasterState *raster = draw->CreateRasterState({});

	PipelineDesc desc{
		Primitive::TRIANGLE_LIST,
		{ vs, fs },
		inputLayout,
		depthStencil,
		blend,
		raster,
		&s_cutoutUBDesc,
		samplers,
	};
	s_cutPipeline = draw->CreateGraphicsPipeline(desc, "lcs_shadow_cutout");

	vs->Release();
	fs->Release();
	inputLayout->Release();
	depthStencil->Release();
	blend->Release();
	raster->Release();

	if (!s_cutPipeline) {
		ERROR_LOG(Log::G3D, "LCS shadows: cut-out pipeline failed to create - palms cast nothing");
		s_cutSetupFailed = true;
		return false;
	}

	// Linear, so a frond's edge is cut where its alpha crosses the threshold rather than on the
	// game texture's texel grid. Wrap follows the game's own per-draw setting.
	for (int wrap = 0; wrap < 4; wrap++) {
		SamplerStateDesc sampDesc{};
		sampDesc.magFilter = TextureFilter::LINEAR;
		sampDesc.minFilter = TextureFilter::LINEAR;
		sampDesc.mipFilter = TextureFilter::NEAREST;
		sampDesc.wrapU = (wrap & 1) ? TextureAddressMode::CLAMP_TO_EDGE : TextureAddressMode::REPEAT;
		sampDesc.wrapV = (wrap & 2) ? TextureAddressMode::CLAMP_TO_EDGE : TextureAddressMode::REPEAT;
		sampDesc.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
		s_cutSamplers[wrap] = draw->CreateSamplerState(sampDesc);
	}
	return true;
}

// Draws this frame's cut-out groups into whichever tile is bound, with that tile's matrix.
static void DrawCutouts(Draw::DrawContext *draw, const float *transposedLightViewProj) {
	using namespace Draw;
	draw->BindPipeline(s_cutPipeline);
	CutoutUB ub;
	memcpy(ub.lightViewProj, transposedLightViewProj, sizeof(ub.lightViewProj));
	ub.params[1] = 0.0f;
	ub.params[2] = 0.0f;
	ub.params[3] = 0.0f;
	for (const CutoutGroup &group : s_cutGroups) {
		if (group.idx.size() < 3) {
			continue;
		}
		ub.params[0] = group.alphaRef;
		draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
		draw->BindNativeTexture(0, group.tex.view);
		SamplerState *sampler = s_cutSamplers[group.wrap & 3];
		draw->BindSamplerStates(0, 1, &sampler);
		draw->DrawIndexedUP(group.verts.data(), (int)(group.verts.size() / 5),
			group.idx.data(), (int)group.idx.size());
	}
	// Leave nothing of ours bound. thin3d writes whatever is bound into the descriptor set of the
	// next draw through ANY pipeline, and by a later frame this texture may be gone.
	draw->BindTexture(0, nullptr);
	SamplerState *none = nullptr;
	draw->BindSamplerStates(0, 1, &none);
}

// The depth map from the last frame that actually had something to put in it, and the light
// matrices it was rendered with.
//
// A frame with NO casters used to give up here, which meant no shadow map, no mask, no composite -
// every shadow on screen gone for that frame. Measured while driving: seven such frames in forty
// seconds, every one of them attributed to an empty caster set rather than to anything else, and
// that is what shadows flickering while driving actually is. The captured set is only what the
// game drew, and during a chunk swap it can briefly contain no vehicle, no pedestrian and no prop.
//
// Reusing the previous map is very nearly free and very nearly exact: it is anchored in LIGHT
// space, and over one or two frames nothing in it has moved far enough to matter. What must NOT be
// reused is the camera matrix - the camera has moved and the mask is screen space - so the two are
// held apart: fresh cameraViewProj, held light matrices.
static bool s_haveHeldMap;
static int s_framesOnHeldMap;
static bool s_usingHeldMap;
static float s_heldLightViewProj[16];
static float s_heldNearLightViewProj[16];
static float s_heldNearRadius;

// Beyond this the world really has changed and a stale map would put shadows under things that
// have gone.
//
// Twelve frames is 0.4s at this game's 30Hz, and the trade is worth stating: what goes stale is
// only the CASTERS - the receivers are this frame's geometry, so shadows still land on the ground
// that is actually there. The failure mode is a shadow outliving the car that cast it by a few
// tenths of a second, against the alternative of every shadow on screen blinking off for a frame.
// At six the measured blink count over a run went from seven to two; at twelve it goes to zero.
static const int kMaxHeldMapFrames = 12;

static void RenderCascade(Draw::DrawContext *draw) {
	s_capture.rendered = false;
	s_usingHeldMap = false;
	if (!draw || !s_view.valid) {
		return;
	}
	const bool haveCutouts = s_capture.cutoutIndices > 0;
	if ((s_batches.empty() || s_casterIndices.empty()) && !haveCutouts) {
		// Nothing to draw INTO the map - but the map from a moment ago is still good.
		if (s_haveHeldMap && s_fbo && s_framesOnHeldMap < kMaxHeldMapFrames) {
			s_framesOnHeldMap++;
			s_usingHeldMap = true;
			s_capture.rendered = true;
		}
		return;
	}
	if (!EnsureResources(draw)) {
		return;
	}
	const bool drawCutouts = haveCutouts && EnsureCutoutResources(draw);

	using namespace Draw;
	draw->BindFramebufferAsRenderTarget(s_fbo,
		{ RPAction::CLEAR, RPAction::CLEAR, RPAction::DONT_CARE, 0, 1.0f, 0, "lcs_shadow" },
		"lcs_shadow");

	const float size = (float)s_fboSize;

	// Count what actually falls in the box, before handing the same matrix to the GPU.
	s_capture.verticesInCascade = -1;
	if (s_settings.countCascadeCoverage) {
		const float *m = s_view.lightViewProj;
		int inside = 0;
		const size_t count = s_positions.size() / 3;
		for (size_t i = 0; i < count; i++) {
			const float *p = s_positions.data() + i * 3;
			const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
			const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
			const float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
			if (x >= -1.0f && x <= 1.0f && y >= -1.0f && y <= 1.0f && z >= 0.0f && z <= 1.0f) {
				inside++;
			}
		}
		s_capture.verticesInCascade = inside;
	}

	// Tile 0 is the near cascade and tile 1 the far one, drawn in that order so the far one
	// - the one that always exists - is what a machine that somehow fails mid-pass is left
	// showing. With the split off, only tile 1 is drawn and the mask never looks at tile 0.
	for (int tile = 0; tile < 2; tile++) {
		const bool isNear = tile == 0;
		if (isNear && s_view.nearRadius <= 0.0f) {
			continue;
		}
		Viewport viewport{ isNear ? 0.0f : size, 0.0f, size, size, 0.0f, 1.0f };
		draw->SetViewport(viewport);
		draw->SetScissorRect(isNear ? 0 : s_fboSize, 0, s_fboSize, s_fboSize);

		ShadowUB ub;
		Transpose4x4(isNear ? s_view.nearLightViewProj : s_view.lightViewProj,
			ub.lightViewProj);
		// Bound per tile, because the cut-out pass at the end of each one swaps its own in.
		draw->BindPipeline(s_pipeline);
		draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

		for (const Batch &batch : s_batches) {
			if (batch.casterIndexCount < 3) {
				continue;
			}
			draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3, batch.vertexCount,
				s_casterIndices.data() + batch.firstCasterIndex, batch.casterIndexCount);
		}
		if (drawCutouts) {
			DrawCutouts(draw, ub.lightViewProj);
		}
	}
	if (drawCutouts) {
		int groups = 0;
		for (const CutoutGroup &group : s_cutGroups) {
			if (group.idx.size() >= 3) {
				groups++;
			}
		}
		s_capture.cutoutGroups = groups;
		// Once per boot and positively: a pass that silently never runs looks exactly like a
		// street with no palms on it.
		static bool s_saidCutouts = false;
		if (!s_saidCutouts && groups > 0) {
			s_saidCutouts = true;
			NOTICE_LOG(Log::G3D,
				"LCS shadows: cut-out pass ran - %d textures, %d indices (%d live draws, %d remembered)",
				groups, s_capture.cutoutIndices, s_capture.cutoutDraws, s_capture.cutoutCachedDraws);
		}
	}
	// Remember what this map was rendered with, so a frame that has to reuse it samples it with
	// the matrices it was actually built from rather than with this frame's.
	memcpy(s_heldLightViewProj, s_view.lightViewProj, sizeof(s_heldLightViewProj));
	memcpy(s_heldNearLightViewProj, s_view.nearLightViewProj, sizeof(s_heldNearLightViewProj));
	s_heldNearRadius = s_view.nearRadius;
	s_haveHeldMap = true;
	s_framesOnHeldMap = 0;
	s_capture.rendered = true;
}

Draw::Framebuffer *ShadowMap() {
	return s_fbo;
}

// --- the shadow mask --------------------------------------------------------------------------
//
// A screen-space mask of what the sun reaches, rendered by drawing the captured geometry a second
// time from the camera. The obvious alternative - reconstruct world position from the game's depth
// buffer - was rejected on inspection: PPSSPP rewrites gl_Position.z through u_minZmaxZ and a
// further doubling step depending on backend and render state, so reconstruction would mean
// replicating a moving target and would break in ways that look like shadow bugs. Drawing the
// geometry again costs one extra pass over ~32k triangles, which is nothing, and hands the
// fragment shader an exact world position as a varying instead of a reconstructed one.
//
// It is built at the game's own render resolution and with the game's own viewport, because the
// next thing that happens to it is being multiplied into the frame pixel for pixel.

struct MaskUB {
	float cameraViewProj[16];
	float lightViewProj[16];
	float nearLightViewProj[16];
	float params[4];    // depthBias, flipV, debugView, slopeBias
	float params2[4];   // shadow texel size, pcf radius, edge fade, shadow map size
	float params3[4];   // near cascade on/off, this draw is people (alpha 0 for the AO), unused x2
};

static const UniformBufferDesc s_maskUBDesc{ sizeof(MaskUB), {
	{ "u_cameraViewProj", 0, -1, UniformType::MATRIX4X4, 0 },
	{ "u_lightViewProj", 1, 0, UniformType::MATRIX4X4, 64 },
	{ "u_nearLightViewProj", 2, 1, UniformType::MATRIX4X4, 128 },
	{ "u_shadowParams", 3, 2, UniformType::FLOAT4, 192 },
	{ "u_shadowParams2", 4, 3, UniformType::FLOAT4, 208 },
	{ "u_shadowParams3", 5, 4, UniformType::FLOAT4, 224 },
} };

static Draw::Framebuffer *s_maskFbo;
static Draw::Pipeline *s_maskPipeline;
static Draw::SamplerState *s_maskSampler;
static int s_maskWidth;
static int s_maskHeight;
static bool s_maskSetupFailed;

static void ReleaseMaskResources() {
	if (s_maskPipeline) {
		s_maskPipeline->Release();
		s_maskPipeline = nullptr;
	}
	if (s_maskSampler) {
		s_maskSampler->Release();
		s_maskSampler = nullptr;
	}
	if (s_maskFbo) {
		s_maskFbo->Release();
		s_maskFbo = nullptr;
	}
	s_maskWidth = 0;
	s_maskHeight = 0;
}

static bool EnsureMaskResources(Draw::DrawContext *draw, int width, int height) {
	if (s_maskFbo && s_maskPipeline && s_maskWidth == width && s_maskHeight == height) {
		s_capture.maskStep = CaptureStats::MaskStep::Ok;
		return true;
	}
	if (s_maskSetupFailed) {
		s_capture.maskStep = CaptureStats::MaskStep::Pipeline;
		return false;
	}
	// The pipeline does not depend on the size, so a window resize should not rebuild shaders -
	// but it is one allocation a resize, and keeping the two together keeps the release path
	// from having to know which half is stale.
	const bool hadPipeline = s_maskPipeline != nullptr;
	if (hadPipeline) {
		Draw::Pipeline *keepPipeline = s_maskPipeline;
		Draw::SamplerState *keepSampler = s_maskSampler;
		s_maskPipeline = nullptr;
		s_maskSampler = nullptr;
		ReleaseMaskResources();
		s_maskPipeline = keepPipeline;
		s_maskSampler = keepSampler;
	} else {
		ReleaseMaskResources();
	}

	using namespace Draw;
	s_maskFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, true, "lcs_shadow_mask" });
	if (!s_maskFbo) {
		s_capture.maskStep = CaptureStats::MaskStep::Framebuffer;
		s_maskSetupFailed = true;
		return false;
	}
	s_maskWidth = width;
	s_maskHeight = height;
	if (hadPipeline) {
		s_capture.maskStep = CaptureStats::MaskStep::Ok;
		return true;
	}

	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	static const SamplerDef samplers[] = { { 0, "shadowMap", SamplerFlags(0) } };
	// The light-space position is computed per vertex, not per fragment. Two reasons, one of them
	// forced: mul() is a ShaderWriter #define emitted in the vertex and geometry preambles but not
	// the fragment one, so a matrix multiply in a fragment shader simply will not compile on
	// Vulkan GLSL. The other is that it is cheaper - once a vertex rather than once a pixel - and
	// costs nothing in accuracy, because the light projection is orthographic and therefore
	// affine, so interpolating its output linearly is exact.
	static const VaryingDef varyings[] = {
		{ "vec3", "v_lightClip", Draw::SEM_TEXCOORD0, 0, "highp" },
	{ "vec3", "v_nearClip", Draw::SEM_TEXCOORD1, 1, "highp" },
	};

	// Every uniform is declared in both stages, even though each stage only reads some of them.
	// On Vulkan the block is a single descriptor shared by the two shaders, so declaring different
	// subsets generates two different block layouts for one binding and the pipeline will not
	// build. Unused members cost nothing; a mismatched block costs the whole pass.
	static const UniformDef uniforms[] = {
		{ "mat4", "u_cameraViewProj", 0 },
		{ "mat4", "u_lightViewProj", 1 },
		{ "mat4", "u_nearLightViewProj", 2 },
		{ "vec4", "u_shadowParams", 3 },
		{ "vec4", "u_shadowParams2", 4 },
		{ "vec4", "u_shadowParams3", 5 },
	};

	const size_t kShaderBufferSize = 8192;
	char *vsCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(vsCode, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec3", "a_position", Draw::SEM_POSITION } };
		writer.BeginVSMain(inputs, uniforms, varyings);
		writer.C("  v_lightClip = mul(vec4(a_position, 1.0), u_lightViewProj).xyz;\n");
		writer.C("  v_nearClip = mul(vec4(a_position, 1.0), u_nearLightViewProj).xyz;\n");
		writer.C("  gl_Position = mul(vec4(a_position, 1.0), u_cameraViewProj);\n");
		writer.EndVSMain(varyings);
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)vsCode, strlen(vsCode), "lcs_mask_vs");
	if (!vs) {
		ERROR_LOG(Log::G3D, "LCS mask VS (%d bytes) failed:\n%s", (int)strlen(vsCode), vsCode);
	}
	delete[] vsCode;

	char *fsCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(fsCode, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(samplers);
		writer.BeginFSMain(uniforms, varyings);
		// Pick the cascade first, then everything below is written against one of them.
		//
		// The near tile is used wherever this pixel falls inside it with a margin, and the
		// margin is what stops the seam being visible: at the very edge of the near box a
		// PCF tap would reach outside it and read whatever the far cascade left in the
		// neighbouring texels, so the handover happens a few texels early where both
		// cascades still agree.
		writer.C("  vec3 lightClip = v_lightClip;\n");
		writer.C("  float tile = 1.0;\n");
		writer.C("  if (u_shadowParams3.x > 0.5) {\n");
		writer.C("    vec2 nearEdge = abs(v_nearClip.xy);\n");
		writer.C("    if (max(nearEdge.x, nearEdge.y) < 0.92 && v_nearClip.z > 0.0 && v_nearClip.z < 1.0) {\n");
		writer.C("      lightClip = v_nearClip;\n");
		writer.C("      tile = 0.0;\n");
		writer.C("    }\n");
		writer.C("  }\n");
		writer.C("  vec2 shadowUV = lightClip.xy * 0.5 + 0.5;\n");
		// Which way up the shadow map's V axis runs depends on the backend's clip convention. It
		// should not need flipping on either of them - a framebuffer's first texel row and clip
		// -1 are the same end of the image in Vulkan and in GL both, because the two conventions
		// flip together - but getting it wrong puts the right shapes in the wrong places, which
		// is worth one checkbox rather than an argument.
		writer.C("  if (u_shadowParams.y > 0.5) { shadowUV.y = 1.0 - shadowUV.y; }\n");
		writer.C("  float refZ = lightClip.z;\n");
		// Slope-scaled bias, from the depth derivatives rather than from a normal. Most of this
		// city arrives with no vertex normals at all - only ~24 draws of 161 carry them, the peds
		// and the vehicles - so normal-offset bias is not available for the geometry that needs
		// it most. fwidth is: it is large exactly where the light grazes a surface, which is
		// where acne is, and near zero on a wall facing the sun.
		writer.C("  float bias = u_shadowParams.x + u_shadowParams.w * fwidth(refZ);\n");
		// Tile UV to atlas UV: each tile owns half the width, so u is halved and pushed into
		// its own half. Everything above works in TILE space, which is what lets the filter
		// below stay exactly the code that was written for a single map.
		writer.C("  vec2 clampedUV = vec2((clamp(shadowUV.x, 0.0, 1.0) + tile) * 0.5, clamp(shadowUV.y, 0.0, 1.0));\n");
		writer.C("  float mapDepth = ").SampleTexture2D("shadowMap", "clampedUV").C(".r;\n");
		// Nine taps, weighted by where the sample point actually falls between them.
		//
		// This is the whole of "the shadows are pixelated", and it was the FILTER rather than
		// the resolution. The old version averaged nine NEAREST samples, so the answer could
		// only ever be one of ten values and every one of them changed at a texel boundary -
		// a step every 7cm on the ground at 2048 texels across a 140-unit cascade, which is a
		// staircase however many taps are averaged. Weighting each tap by its distance from
		// the sample point makes the result continuous: the same nine fetches, read as a
		// surface instead of as a grid.
		//
		// The taps sit on a grid of whole texels `spacing` apart, so they always land on texel
		// centres and the weights are the ordinary bilinear ones evaluated in that grid's own
		// units. pcfRadius therefore still means "softer" - it widens the spacing and the
		// reconstruction stretches with it - rather than "more values to average".
		writer.C("  float spacing = 1.0 + u_shadowParams2.y;\n");
		writer.C("  vec2 gridPos = (shadowUV * u_shadowParams2.w - 0.5) / spacing;\n");
		writer.C("  vec2 node = floor(gridPos + 0.5);\n");
		writer.C("  vec2 nodeFrac = gridPos - node;\n");
		writer.C("  float sum = 0.0;\n");
		writer.C("  float weightSum = 0.0;\n");
		writer.C("  for (int iy = -1; iy <= 1; iy++) {\n");
		writer.C("    for (int ix = -1; ix <= 1; ix++) {\n");
		writer.C("      vec2 tapTexel = (node + vec2(float(ix), float(iy))) * spacing;\n");
		writer.C("      vec2 tapTile = clamp((tapTexel + 0.5) * u_shadowParams2.x, 0.0, 1.0);\n");
		writer.C("      vec2 tapUV = vec2((tapTile.x + tile) * 0.5, tapTile.y);\n");
		writer.C("      float tapDepth = ").SampleTexture2D("shadowMap", "tapUV").C(".r;\n");
		writer.C("      float tapLit = (refZ - bias > tapDepth) ? 0.0 : 1.0;\n");
		writer.C("      float wx = clamp(1.0 - abs(float(ix) - nodeFrac.x), 0.0, 1.0);\n");
		writer.C("      float wy = clamp(1.0 - abs(float(iy) - nodeFrac.y), 0.0, 1.0);\n");
		writer.C("      sum += tapLit * wx * wy;\n");
		writer.C("      weightSum += wx * wy;\n");
		writer.C("    }\n");
		writer.C("  }\n");
		writer.C("  float lit = sum / max(weightSum, 0.0001);\n");
		// The cascade has an edge, and without this it is a straight line ruled across the road.
		//
		// The FAR cascade's edge only. The near one's is where the far takes over, and fading there
		// thinned every shadow to about half just inside the 0.92 handover before the far cascade
		// put it back at full - a lit ring a couple of pixels wide, cut through every shadow at the
		// near box's radius and following the player: reported as "a 2px cut in the otherwise full
		// shadow that follows the player in distance, like an end of a radius".
		writer.C("  vec2 edge = abs(shadowUV * 2.0 - 1.0);\n");
		writer.C("  float edgeDist = max(edge.x, edge.y);\n");
		writer.C("  float fade = tile > 0.5 ? clamp((1.0 - edgeDist) / max(u_shadowParams2.z, 0.001), 0.0, 1.0) : 1.0;\n");
		writer.C("  lit = mix(1.0, lit, fade);\n");
		writer.C("  if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0 ||\n");
		writer.C("      refZ < 0.0 || refZ > 1.0) {\n");
		writer.C("    lit = 1.0;\n");
		writer.C("  }\n");
		// The debug views. An all-black mask has at least two causes that look identical from
		// outside - the sample returning nothing, or the comparison being the wrong way round -
		// and these separate them in one run rather than one per guess.
		// Alpha marks people, for the ambient occlusion's composite - see aoOnPeople. Nothing
		// else reads the mask's alpha, and the debug views below overwrite it harmlessly.
		writer.C("  vec4 outColor = vec4(lit, lit, lit, u_shadowParams3.y > 0.5 ? 0.0 : 1.0);\n");
		writer.C("  if (u_shadowParams.z > 0.5 && u_shadowParams.z < 1.5) {\n");
		writer.C("    outColor = vec4(mapDepth, mapDepth, mapDepth, 1.0);\n");
		writer.C("  } else if (u_shadowParams.z > 1.5 && u_shadowParams.z < 2.5) {\n");
		writer.C("    outColor = vec4(refZ, refZ, refZ, 1.0);\n");
		writer.C("  } else if (u_shadowParams.z > 2.5) {\n");
		writer.C("    outColor = vec4(clampedUV.x, clampedUV.y, 0.0, 1.0);\n");
		writer.C("  }\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *fs = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)fsCode, strlen(fsCode), "lcs_mask_fs");
	if (!fs) {
		ERROR_LOG(Log::G3D, "LCS mask FS (%d bytes) failed:\n%s", (int)strlen(fsCode), fsCode);
	}
	delete[] fsCode;

	if (!vs || !fs) {
		// Log the source, not just the failure - a shader that will not compile is only
		// diagnosable if you can see what was handed to the compiler.
		s_capture.maskStep = vs ? CaptureStats::MaskStep::FragmentShader
			: CaptureStats::MaskStep::VertexShader;
		ERROR_LOG(Log::G3D, "LCS shadow mask shader failed to compile (%s)",
			vs ? "fragment" : "vertex");
		if (vs) vs->Release();
		if (fs) fs->Release();
		ReleaseMaskResources();
		s_maskSetupFailed = true;
		return false;
	}

	static const InputLayoutDesc layoutDesc = {
		12,
		{ { SEM_POSITION, DataFormat::R32G32B32_FLOAT, 0 } },
	};
	InputLayout *inputLayout = draw->CreateInputLayout(layoutDesc);

	DepthStencilStateDesc dsDesc{};
	dsDesc.depthTestEnabled = true;
	dsDesc.depthWriteEnabled = true;
	dsDesc.depthCompare = Comparison::LESS;
	DepthStencilState *depthStencil = draw->CreateDepthStencilState(dsDesc);
	BlendState *blend = draw->CreateBlendState({ false, 0xF });
	RasterState *raster = draw->CreateRasterState({});

	SamplerStateDesc sampDesc{};
	sampDesc.magFilter = TextureFilter::NEAREST;
	sampDesc.minFilter = TextureFilter::NEAREST;
	sampDesc.mipFilter = TextureFilter::NEAREST;
	sampDesc.wrapU = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapV = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
	s_maskSampler = draw->CreateSamplerState(sampDesc);

	PipelineDesc desc{
		Primitive::TRIANGLE_LIST,
		{ vs, fs },
		inputLayout,
		depthStencil,
		blend,
		raster,
		&s_maskUBDesc,
		samplers,
	};
	s_maskPipeline = draw->CreateGraphicsPipeline(desc, "lcs_shadow_mask");

	vs->Release();
	fs->Release();
	inputLayout->Release();
	depthStencil->Release();
	blend->Release();
	raster->Release();

	if (!s_maskPipeline) {
		s_capture.maskStep = CaptureStats::MaskStep::Pipeline;
		ERROR_LOG(Log::G3D, "LCS shadow mask pipeline failed to create");
		ReleaseMaskResources();
		s_maskSetupFailed = true;
		return false;
	}
	s_capture.maskStep = CaptureStats::MaskStep::Ok;
	return true;
}

static void RenderMask(Draw::DrawContext *draw, int width, int height, int viewW, int viewH) {
	s_capture.maskRendered = false;
	s_capture.maskStep = CaptureStats::MaskStep::NotAttempted;
	if (!s_capture.rendered || !s_fbo || s_batches.empty()) {
		return;
	}
	if (!EnsureMaskResources(draw, width, height)) {
		return;
	}

	using namespace Draw;
	draw->BindFramebufferAsRenderTarget(s_maskFbo,
		{ RPAction::CLEAR, RPAction::CLEAR, RPAction::DONT_CARE, 0xFFFFFFFF, 1.0f, 0, "lcs_mask" },
		"lcs_mask");
	draw->BindFramebufferAsTexture(s_fbo, 0, Aspect::DEPTH_BIT, 0);
	draw->BindSamplerStates(0, 1, &s_maskSampler);

	// The game's own viewport, scaled to this mask. Anything outside it stays at the clear value,
	// which is "lit" - so the composite leaves those pixels exactly as the game drew them.
	Viewport viewport{ 0.0f, 0.0f, (float)viewW, (float)viewH, 0.0f, 1.0f };
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, viewW, viewH);
	draw->BindPipeline(s_maskPipeline);

	MaskUB ub;
	// The CAMERA matrix is always this frame's - the mask is screen space and the camera has
	// moved. The LIGHT matrices have to be the ones the depth map in front of us was rendered
	// with, which on a held frame is not the same thing. Mixing the two puts the right shadows in
	// the wrong places.
	const float *lightVP = s_usingHeldMap ? s_heldLightViewProj : s_view.lightViewProj;
	const float *nearVP = s_usingHeldMap ? s_heldNearLightViewProj : s_view.nearLightViewProj;
	const float nearRadius = s_usingHeldMap ? s_heldNearRadius : s_view.nearRadius;
	Transpose4x4(s_view.cameraViewProj, ub.cameraViewProj);
	Transpose4x4(lightVP, ub.lightViewProj);
	// With the split off this is the far matrix again rather than anything undefined: the
	// varying is still computed and still interpolated, it is simply never chosen.
	Transpose4x4(nearRadius > 0.0f ? nearVP : lightVP, ub.nearLightViewProj);
	ub.params[0] = s_settings.depthBias;
	ub.params[1] = s_settings.flipShadowV ? 1.0f : 0.0f;
	ub.params[2] = (float)s_settings.debugView;
	ub.params[3] = s_settings.slopeBias;
	ub.params2[0] = s_fboSize > 0 ? 1.0f / (float)s_fboSize : 0.0f;
	ub.params2[1] = (float)s_settings.pcfRadius;
	ub.params2[2] = s_settings.edgeFade;
	// The filter works in texel and tap-grid units, so it needs the size as well as its
	// reciprocal - and computing one from the other in the shader is a divide per pixel.
	ub.params2[3] = (float)s_fboSize;
	// Whether the near tile was drawn at all. The shader tests this rather than trying to
	// infer it from a matrix, so a disabled split costs one compare and reads the far
	// cascade exactly as it did before the atlas existed.
	ub.params3[0] = nearRadius > 0.0f ? 1.0f : 0.0f;
	ub.params3[1] = 0.0f;
	ub.params3[2] = 0.0f;
	ub.params3[3] = 0.0f;
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

	for (const Batch &batch : s_batches) {
		if (batch.receiverIndexCount < 3) {
			continue;
		}
		draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3, batch.vertexCount,
			s_receiverIndices.data() + batch.firstReceiverIndex, batch.receiverIndexCount);
	}

	// The people, with the bias raised so their own arms and legs stop landing on them. Same
	// pipeline and same vertex buffer - only the uniform and the index stream differ - so this
	// is one more draw call per batch rather than a second pass over the scene. Marked in alpha
	// while they are at it, for the ambient occlusion.
	if ((s_settings.pedReceiverBias > 0.0f || s_settings.ambientOcclusion) && !s_pedReceiverIndices.empty()) {
		ub.params[0] = s_settings.depthBias + s_settings.pedReceiverBias;
		ub.params3[1] = 1.0f;
		draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
		for (const Batch &batch : s_batches) {
			if (batch.pedReceiverIndexCount < 3) {
				continue;
			}
			draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3,
				batch.vertexCount,
				s_pedReceiverIndices.data() + batch.firstPedReceiverIndex,
				batch.pedReceiverIndexCount);
		}
	}
	s_capture.maskRendered = true;
	s_capture.maskWidth = width;
	s_capture.maskHeight = height;
}

Draw::Framebuffer *ShadowMask() {
	return s_maskFbo;
}

// --- ambient occlusion ------------------------------------------------------------------------
//
// Scalable Ambient Obscurance (McGuire, Mara and Luebke, 2012), read off the MASK's depth buffer.
// That buffer is the thing occlusion needs and the expensive thing to get: every captured receiver
// drawn through the camera's own transform, lined up with the frame to the pixel. The mask pass
// already pays for it, so this is one full-screen pass over it and a two-pass blur - nothing goes
// back through the city's geometry.
//
//   estimate   per pixel: its position rebuilt from depth, a normal from its neighbours, and a
//              spiral of nearby pixels asked how much of the hemisphere above it they fill
//   blur x2    separable, and stopped at depth edges so an outline is not smeared into the wall
//              behind it
//
// The composite then multiplies the result in alongside the shadows, before the HUD is drawn.
//
// Positions are rebuilt RELATIVE TO THE CAMERA rather than in the space the GE is fed. That space
// sits wherever the game last rebased it, so a position in it is a large number reached by
// subtracting two large numbers; relative to the camera it is small, and occlusion only ever asks
// about the difference between two nearby points.

struct AOUB {
	float inv[4][4];     // columns of NDC -> camera-relative position; see RenderAO
	float forward[4];    // camera forward, and w = pixels per world unit at one unit's distance
	float params[4];     // radius, intensity, bias per unit of distance, largest radius in pixels
	float params2[4];    // NDC scale x and y (mask texture over mask viewport), samples, spiral turns
	float depthSize[4];  // mask depth texture width, height, 1/width, 1/height
	float aoSize[4];     // 1/this target's width, 1/height, fade start, fade end
	float key[4];        // 1 / the distance at which the blur's depth key saturates, unused x3
};

static const UniformBufferDesc s_aoUBDesc{ sizeof(AOUB), {
	{ "u_aoInv0", 0, -1, UniformType::FLOAT4, 0 },
	{ "u_aoInv1", 1, 0, UniformType::FLOAT4, 16 },
	{ "u_aoInv2", 2, 1, UniformType::FLOAT4, 32 },
	{ "u_aoInv3", 3, 2, UniformType::FLOAT4, 48 },
	{ "u_aoForward", 4, 3, UniformType::FLOAT4, 64 },
	{ "u_aoParams", 5, 4, UniformType::FLOAT4, 80 },
	{ "u_aoParams2", 6, 5, UniformType::FLOAT4, 96 },
	{ "u_aoDepthSize", 7, 6, UniformType::FLOAT4, 112 },
	{ "u_aoSize", 8, 7, UniformType::FLOAT4, 128 },
	{ "u_aoKey", 9, 8, UniformType::FLOAT4, 144 },
} };

struct AOBlurUB {
	float dir[4];   // one tap's step in UV, x and y; edge sharpness; unused
};

static const UniformBufferDesc s_aoBlurUBDesc{ sizeof(AOBlurUB), {
	{ "u_aoBlurDir", 0, -1, UniformType::FLOAT4, 0 },
} };

static Draw::Framebuffer *s_aoFbo;       // the estimate, and where the blur finishes
static Draw::Framebuffer *s_aoBlurFbo;   // between the two blur passes
static Draw::Pipeline *s_aoPipeline;
static Draw::Pipeline *s_aoBlurPipeline;
// NEAREST for everything these passes read: a filtered depth is a position that is on no surface,
// and a filtered key is a depth nobody drew.
static Draw::SamplerState *s_aoSampler;
static int s_aoWidth;
static int s_aoHeight;
static bool s_aoSetupFailed;

static void ReleaseAOTargets() {
	if (s_aoFbo) {
		s_aoFbo->Release();
		s_aoFbo = nullptr;
	}
	if (s_aoBlurFbo) {
		s_aoBlurFbo->Release();
		s_aoBlurFbo = nullptr;
	}
	s_aoWidth = 0;
	s_aoHeight = 0;
}

static void ReleaseAOResources() {
	ReleaseAOTargets();
	if (s_aoPipeline) {
		s_aoPipeline->Release();
		s_aoPipeline = nullptr;
	}
	if (s_aoBlurPipeline) {
		s_aoBlurPipeline->Release();
		s_aoBlurPipeline = nullptr;
	}
	if (s_aoSampler) {
		s_aoSampler->Release();
		s_aoSampler = nullptr;
	}
}

// The vertex half of every full-screen pass here: one oversized triangle in clip space, and a UV
// that is that position remapped. It declares the pipeline's uniform block even though it reads
// none of it - see the note on the mask's uniforms for why the two stages must agree on Vulkan.
static Draw::ShaderModule *CompileFullscreenVS(Draw::DrawContext *draw, Slice<UniformDef> uniforms,
	Slice<VaryingDef> varyings, const char *tag) {
	using namespace Draw;
	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	char *code = new char[4096];
	{
		ShaderWriter writer(code, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec2", "a_position", Draw::SEM_POSITION } };
		writer.BeginVSMain(inputs, uniforms, varyings);
		writer.C("  v_uv = a_position * 0.5 + 0.5;\n");
		writer.C("  gl_Position = vec4(a_position, 0.0, 1.0);\n");
		writer.EndVSMain(varyings);
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)code, strlen(code), tag);
	if (!vs) {
		ERROR_LOG(Log::G3D, "LCS %s failed:\n%s", tag, code);
	}
	delete[] code;
	return vs;
}

// One position rebuilt from the mask's depth, as `vec3 <name>`. Emitted inline rather than as a
// GLSL function, because a function would have to be declared before the uniform block that
// BeginFSMain writes, and could not see it.
//
// The UV is snapped to the centre of the depth texel it lands in first. The depth read there is
// that texel's, so the position has to be that texel's too - rebuilding it at the unsnapped UV
// puts every point up to half a texel off its own surface, and on a slope that reads as noise.
static void EmitAOPosition(ShaderWriter &writer, const char *name, const char *uv) {
	char texelUV[64];
	snprintf(texelUV, sizeof(texelUV), "%s_t", name);
	writer.F("  vec2 %s_t = (floor((%s) * u_aoDepthSize.xy) + 0.5) * u_aoDepthSize.zw;\n", name, uv);
	writer.F("  float %s_d = ", name).SampleTexture2D("maskDepth", texelUV).C(".r;\n");
	writer.F("  vec4 %s_n = vec4((%s_t * u_aoParams2.xy) * 2.0 - 1.0, %s_d, 1.0);\n", name, name, name);
	writer.F("  vec4 %s_h = vec4(dot(%s_n, u_aoInv0), dot(%s_n, u_aoInv1), dot(%s_n, u_aoInv2), dot(%s_n, u_aoInv3));\n",
		name, name, name, name, name);
	writer.F("  vec3 %s = %s_h.xyz / %s_h.w;\n", name, name, name);
}

static bool EnsureAOResources(Draw::DrawContext *draw, int width, int height) {
	if (s_aoFbo && s_aoBlurFbo && s_aoPipeline && s_aoBlurPipeline && s_aoWidth == width && s_aoHeight == height) {
		s_capture.aoStep = CaptureStats::MaskStep::Ok;
		return true;
	}
	if (s_aoSetupFailed) {
		s_capture.aoStep = CaptureStats::MaskStep::Pipeline;
		return false;
	}

	using namespace Draw;
	ReleaseAOTargets();
	// Colour only. The estimate reads the mask's depth and writes none of its own.
	s_aoFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, false, "lcs_ao" });
	s_aoBlurFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, false, "lcs_ao_blur" });
	if (!s_aoFbo || !s_aoBlurFbo) {
		s_capture.aoStep = CaptureStats::MaskStep::Framebuffer;
		ReleaseAOTargets();
		s_aoSetupFailed = true;
		return false;
	}
	s_aoWidth = width;
	s_aoHeight = height;
	if (s_aoPipeline && s_aoBlurPipeline) {
		s_capture.aoStep = CaptureStats::MaskStep::Ok;
		return true;
	}

	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	static const VaryingDef varyings[] = {
		{ "vec2", "v_uv", Draw::SEM_TEXCOORD0, 0, "highp" },
	};

	// --- the estimate ---
	static const SamplerDef aoSamplers[] = { { 0, "maskDepth", SamplerFlags(0) } };
	static const UniformDef aoUniforms[] = {
		{ "vec4", "u_aoInv0", 0 },
		{ "vec4", "u_aoInv1", 1 },
		{ "vec4", "u_aoInv2", 2 },
		{ "vec4", "u_aoInv3", 3 },
		{ "vec4", "u_aoForward", 4 },
		{ "vec4", "u_aoParams", 5 },
		{ "vec4", "u_aoParams2", 6 },
		{ "vec4", "u_aoDepthSize", 7 },
		{ "vec4", "u_aoSize", 8 },
		{ "vec4", "u_aoKey", 9 },
	};
	ShaderModule *aoVS = CompileFullscreenVS(draw, aoUniforms, varyings, "lcs_ao_vs");

	const size_t kShaderBufferSize = 16384;
	char *fsCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(fsCode, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(aoSamplers);
		writer.BeginFSMain(aoUniforms, varyings);
		writer.C("  vec4 outColor = vec4(1.0, 1.0, 1.0, 1.0);\n");
		EmitAOPosition(writer, "P", "v_uv");
		writer.C("  float depth = dot(P, u_aoForward.xyz);\n");
		// The mask clears to the far plane and nothing it draws gets there - the game's own far
		// plane lands 8/65536 short of it. So a cleared texel is sky, or outside the game's
		// viewport, and nothing there is occluded.
		writer.C("  if (P_d < 0.99995 && depth > 0.0 && depth < u_aoSize.w) {\n");
		// A normal from the neighbours, taking whichever side is nearer in depth on each axis.
		// The nearer side is the one on the same surface: at a silhouette the far side is
		// whatever is behind it, and a normal built across that edge points nowhere real.
		writer.C("    vec2 texX = vec2(u_aoDepthSize.z, 0.0);\n");
		writer.C("    vec2 texY = vec2(0.0, u_aoDepthSize.w);\n");
		EmitAOPosition(writer, "Pr", "v_uv + texX");
		EmitAOPosition(writer, "Pl", "v_uv - texX");
		EmitAOPosition(writer, "Pd", "v_uv + texY");
		EmitAOPosition(writer, "Pu", "v_uv - texY");
		writer.C("    vec3 ex = abs(dot(Pr - P, u_aoForward.xyz)) < abs(dot(P - Pl, u_aoForward.xyz)) ? Pr - P : P - Pl;\n");
		writer.C("    vec3 ey = abs(dot(Pd - P, u_aoForward.xyz)) < abs(dot(P - Pu, u_aoForward.xyz)) ? Pd - P : P - Pu;\n");
		writer.C("    vec3 c = cross(ex, ey);\n");
		writer.C("    float cl = dot(c, c);\n");
		// The camera is the origin, so a surface the camera can see faces back along -P.
		writer.C("    vec3 N = cl > 1e-20 ? c * inversesqrt(cl) : -normalize(P);\n");
		writer.C("    if (dot(N, P) > 0.0) { N = -N; }\n");
		// How big the search disc is on screen, from how far away this pixel is. Capped, because
		// a wall at arm's length would otherwise ask for samples from half the screen away and
		// twelve of them cannot cover that.
		writer.C("    float radius = u_aoParams.x;\n");
		writer.C("    float r2 = radius * radius;\n");
		writer.C("    float discPx = min(radius * u_aoForward.w / depth, u_aoParams.w);\n");
		writer.C("    float ao = 1.0;\n");
		writer.C("    if (discPx >= 1.0) {\n");
		// A different spin on the spiral per pixel, from interleaved gradient noise - which is
		// what makes twelve samples cover a disc between neighbours, and what the blur is for.
		writer.C("      vec2 pixel = floor(v_uv / u_aoSize.xy);\n");
		writer.C("      float spin = 6.2831853 * fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));\n");
		writer.C("      int sampleCount = int(u_aoParams2.z);\n");
		writer.C("      float bias = u_aoParams.z * depth;\n");
		writer.C("      float sum = 0.0;\n");
		writer.C("      for (int i = 0; i < 32; i++) {\n");
		writer.C("        if (i >= sampleCount) { break; }\n");
		writer.C("        float alpha = (float(i) + 0.5) / float(sampleCount);\n");
		writer.C("        float angle = alpha * u_aoParams2.w * 6.2831853 + spin;\n");
		writer.C("        vec2 sampleUV = v_uv + vec2(cos(angle), sin(angle)) * (alpha * discPx) * u_aoSize.xy;\n");
		EmitAOPosition(writer, "Q", "sampleUV");
		// SAO's falloff: nothing past the radius counts at all, which is also what keeps a person
		// standing in front of a distant wall from casting a dark halo onto it.
		writer.C("        vec3 v = Q - P;\n");
		writer.C("        float vv = dot(v, v);\n");
		writer.C("        float vn = dot(v, N);\n");
		writer.C("        float f = max(r2 - vv, 0.0);\n");
		writer.C("        sum += f * f * f * max((vn - bias) / (vv + 0.01), 0.0);\n");
		writer.C("      }\n");
		writer.C("      ao = max(0.0, 1.0 - sum * u_aoParams.y * 5.0 / (float(sampleCount) * r2 * r2 * r2));\n");
		writer.C("    }\n");
		writer.C("    ao = mix(ao, 1.0, smoothstep(u_aoSize.z, u_aoSize.w, depth));\n");
		// Depth for the blur to key on, sixteen bits across green and blue. Packed as two whole
		// numbers out of 255 each, which an 8-bit channel stores exactly.
		writer.C("    float keyq = floor(clamp(depth * u_aoKey.x, 0.0, 1.0) * 65535.0);\n");
		writer.C("    float keyHi = floor(keyq / 256.0);\n");
		writer.C("    outColor = vec4(ao, keyHi / 255.0, (keyq - keyHi * 256.0) / 255.0, 1.0);\n");
		writer.C("  }\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *aoFS = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)fsCode, strlen(fsCode), "lcs_ao_fs");
	if (!aoFS) {
		ERROR_LOG(Log::G3D, "LCS AO FS (%d bytes) failed:\n%s", (int)strlen(fsCode), fsCode);
	}
	delete[] fsCode;

	// --- the blur ---
	static const SamplerDef blurSamplers[] = { { 0, "aoTex", SamplerFlags(0) } };
	static const UniformDef blurUniforms[] = {
		{ "vec4", "u_aoBlurDir", 0 },
	};
	ShaderModule *blurVS = CompileFullscreenVS(draw, blurUniforms, varyings, "lcs_ao_blur_vs");

	char *blurCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(blurCode, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(blurSamplers);
		writer.BeginFSMain(blurUniforms, varyings);
		writer.C("  vec4 centre = ").SampleTexture2D("aoTex", "v_uv").C(";\n");
		writer.C("  float keyC = (floor(centre.g * 255.0 + 0.5) * 256.0 + floor(centre.b * 255.0 + 0.5)) / 65535.0;\n");
		writer.C("  vec4 outColor = centre;\n");
		writer.C("  if (keyC < 0.99999) {\n");
		writer.C("    float sum = centre.r;\n");
		writer.C("    float weightSum = 1.0;\n");
		writer.C("    for (int i = -4; i <= 4; i++) {\n");
		writer.C("      if (i == 0) { continue; }\n");
		writer.C("      float fi = float(i);\n");
		writer.C("      vec4 tap = ").SampleTexture2D("aoTex", "v_uv + u_aoBlurDir.xy * fi").C(";\n");
		writer.C("      float key = (floor(tap.g * 255.0 + 0.5) * 256.0 + floor(tap.b * 255.0 + 0.5)) / 65535.0;\n");
		// Gaussian in the tap, and zero across a depth step. The step is measured relative to
		// the depth itself, because the ground's own depth changes faster per pixel the further
		// away it is - an absolute threshold would stop the blur dead on distant road.
		writer.C("      float w = exp(-fi * fi / 8.0);\n");
		writer.C("      w *= max(0.0, 1.0 - u_aoBlurDir.z * abs(key - keyC) / max(keyC, 0.0001));\n");
		writer.C("      sum += tap.r * w;\n");
		writer.C("      weightSum += w;\n");
		writer.C("    }\n");
		writer.C("    outColor = vec4(sum / weightSum, centre.g, centre.b, 1.0);\n");
		writer.C("  }\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *blurFS = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)blurCode, strlen(blurCode), "lcs_ao_blur_fs");
	if (!blurFS) {
		ERROR_LOG(Log::G3D, "LCS AO blur FS failed:\n%s", blurCode);
	}
	delete[] blurCode;

	if (!aoVS || !aoFS || !blurVS || !blurFS) {
		s_capture.aoStep = (!aoVS || !blurVS) ? CaptureStats::MaskStep::VertexShader
			: CaptureStats::MaskStep::FragmentShader;
		ERROR_LOG(Log::G3D, "LCS ambient occlusion shaders failed to compile");
		if (aoVS) aoVS->Release();
		if (aoFS) aoFS->Release();
		if (blurVS) blurVS->Release();
		if (blurFS) blurFS->Release();
		ReleaseAOResources();
		s_aoSetupFailed = true;
		return false;
	}

	static const InputLayoutDesc layoutDesc = {
		8,
		{ { SEM_POSITION, DataFormat::R32G32_FLOAT, 0 } },
	};
	InputLayout *inputLayout = draw->CreateInputLayout(layoutDesc);
	DepthStencilStateDesc dsDesc{};
	dsDesc.depthTestEnabled = false;
	dsDesc.depthWriteEnabled = false;
	dsDesc.depthCompare = Comparison::ALWAYS;
	DepthStencilState *depthStencil = draw->CreateDepthStencilState(dsDesc);
	BlendState *opaque = draw->CreateBlendState({ false, 0xF });
	RasterState *raster = draw->CreateRasterState({});

	SamplerStateDesc sampDesc{};
	sampDesc.magFilter = TextureFilter::NEAREST;
	sampDesc.minFilter = TextureFilter::NEAREST;
	sampDesc.mipFilter = TextureFilter::NEAREST;
	sampDesc.wrapU = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapV = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
	s_aoSampler = draw->CreateSamplerState(sampDesc);

	PipelineDesc aoDesc{
		Primitive::TRIANGLE_LIST,
		{ aoVS, aoFS },
		inputLayout,
		depthStencil,
		opaque,
		raster,
		&s_aoUBDesc,
		aoSamplers,
	};
	s_aoPipeline = draw->CreateGraphicsPipeline(aoDesc, "lcs_ao");
	PipelineDesc blurDesc{
		Primitive::TRIANGLE_LIST,
		{ blurVS, blurFS },
		inputLayout,
		depthStencil,
		opaque,
		raster,
		&s_aoBlurUBDesc,
		blurSamplers,
	};
	s_aoBlurPipeline = draw->CreateGraphicsPipeline(blurDesc, "lcs_ao_blur");

	aoVS->Release();
	aoFS->Release();
	blurVS->Release();
	blurFS->Release();
	inputLayout->Release();
	depthStencil->Release();
	opaque->Release();
	raster->Release();

	if (!s_aoPipeline || !s_aoBlurPipeline || !s_aoSampler) {
		s_capture.aoStep = CaptureStats::MaskStep::Pipeline;
		ERROR_LOG(Log::G3D, "LCS ambient occlusion pipelines failed to create");
		ReleaseAOResources();
		s_aoSetupFailed = true;
		return false;
	}
	s_capture.aoStep = CaptureStats::MaskStep::Ok;
	return true;
}

static void RenderAO(Draw::DrawContext *draw, int maskW, int maskH, int viewW, int viewH) {
	s_capture.aoRendered = false;
	s_capture.aoStep = CaptureStats::MaskStep::NotAttempted;
	if (!s_settings.ambientOcclusion || !s_capture.maskRendered || !s_maskFbo || viewW <= 0 || viewH <= 0) {
		return;
	}

	// NDC -> the space the GE is fed, then moved so the camera is the origin. In row-vector
	// terms that is inverse(cameraViewProj) * T(-camera), and folding the translation in on the
	// CPU keeps the shader's numbers small - see the note at the top of this section.
	double inv[16];
	if (!Invert4x4(s_view.cameraViewProj, inv)) {
		return;
	}
	const double cam[3] = { s_view.cameraPos[0], s_view.cameraPos[1], s_view.cameraPos[2] };
	double rel[16];
	for (int row = 0; row < 4; row++) {
		const double w = inv[row * 4 + 3];
		rel[row * 4 + 0] = inv[row * 4 + 0] - cam[0] * w;
		rel[row * 4 + 1] = inv[row * 4 + 1] - cam[1] * w;
		rel[row * 4 + 2] = inv[row * 4 + 2] - cam[2] * w;
		rel[row * 4 + 3] = w;
	}

	float scale = s_settings.aoScale;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 1.0f) scale = 1.0f;
	const int aoW = std::max(1, (int)((float)maskW * scale + 0.5f));
	const int aoH = std::max(1, (int)((float)maskH * scale + 0.5f));
	if (!EnsureAOResources(draw, aoW, aoH)) {
		return;
	}

	float forward[3] = { s_view.cameraForward[0], s_view.cameraForward[1], s_view.cameraForward[2] };
	float up[3] = { s_view.cameraUp[0], s_view.cameraUp[1], s_view.cameraUp[2] };
	if (!Normalize(forward) || !Normalize(up)) {
		return;
	}

	// Pixels per world unit at one unit's distance, in THIS target's pixels. Measured by pushing
	// a one-unit step through the same matrix the mask used rather than read off the projection,
	// because the matrix also holds the PSP viewport and the remap into the render target, and
	// every one of those scales the answer.
	float projScale = 0.0f;
	{
		const float dist = 10.0f;
		float a[3], b[3];
		for (int i = 0; i < 3; i++) {
			a[i] = s_view.cameraPos[i] + forward[i] * dist;
			b[i] = a[i] + up[i];
		}
		const float *m = s_view.cameraViewProj;
		float ca[4], cb[4];
		for (int col = 0; col < 4; col++) {
			ca[col] = a[0] * m[col] + a[1] * m[4 + col] + a[2] * m[8 + col] + m[12 + col];
			cb[col] = b[0] * m[col] + b[1] * m[4 + col] + b[2] * m[8 + col] + m[12 + col];
		}
		if (fabsf(ca[3]) < 1e-6f || fabsf(cb[3]) < 1e-6f) {
			return;
		}
		// NDC spans 2 across the mask's viewport, and this target covers the same fraction of
		// itself as the viewport does of the mask.
		const float pxW = (float)viewW * (float)aoW / (float)maskW * 0.5f;
		const float pxH = (float)viewH * (float)aoH / (float)maskH * 0.5f;
		const float dx = (cb[0] / cb[3] - ca[0] / ca[3]) * pxW;
		const float dy = (cb[1] / cb[3] - ca[1] / ca[3]) * pxH;
		projScale = sqrtf(dx * dx + dy * dy) * dist;
	}

	AOUB ub;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			ub.inv[col][row] = (float)rel[row * 4 + col];
		}
	}
	ub.forward[0] = forward[0];
	ub.forward[1] = forward[1];
	ub.forward[2] = forward[2];
	ub.forward[3] = projScale;
	ub.params[0] = std::max(s_settings.aoRadius, 0.05f);
	ub.params[1] = s_settings.aoIntensity;
	ub.params[2] = s_settings.aoBias;
	ub.params[3] = 0.1f * (float)aoH;
	ub.params2[0] = (float)maskW / (float)viewW;
	ub.params2[1] = (float)maskH / (float)viewH;
	ub.params2[2] = (float)std::clamp(s_settings.aoSamples, 4, 32);
	// Turns of the spiral. SAO's table picks the count that spreads the samples most evenly for
	// each sample count; seven is its answer for eleven and twelve, and nothing here is fussy.
	ub.params2[3] = 7.0f;
	ub.depthSize[0] = (float)maskW;
	ub.depthSize[1] = (float)maskH;
	ub.depthSize[2] = 1.0f / (float)maskW;
	ub.depthSize[3] = 1.0f / (float)maskH;
	ub.aoSize[0] = 1.0f / (float)aoW;
	ub.aoSize[1] = 1.0f / (float)aoH;
	ub.aoSize[2] = s_settings.aoFadeStart;
	ub.aoSize[3] = std::max(s_settings.aoFadeEnd, s_settings.aoFadeStart + 1.0f);
	// Saturates well past the fade, so nothing the estimate shades shares a key with the sky.
	ub.key[0] = 1.0f / (ub.aoSize[3] * 2.0f);
	ub.key[1] = 0.0f;
	ub.key[2] = 0.0f;
	ub.key[3] = 0.0f;

	static const float kFullscreenTriangle[6] = {
		-1.0f, -1.0f,
		 3.0f, -1.0f,
		-1.0f,  3.0f,
	};

	using namespace Draw;
	draw->BindFramebufferAsRenderTarget(s_aoFbo,
		{ RPAction::CLEAR, RPAction::DONT_CARE, RPAction::DONT_CARE, 0xFFFFFFFF, 1.0f, 0, "lcs_ao" },
		"lcs_ao");
	draw->BindFramebufferAsTexture(s_maskFbo, 0, Aspect::DEPTH_BIT, 0);
	draw->BindSamplerStates(0, 1, &s_aoSampler);
	Viewport viewport{ 0.0f, 0.0f, (float)aoW, (float)aoH, 0.0f, 1.0f };
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, aoW, aoH);
	draw->BindPipeline(s_aoPipeline);
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	draw->DrawUP(kFullscreenTriangle, 3);

	if (s_settings.aoBlur) {
		// Taps two texels apart at full resolution - the noise repeats over about four pixels,
		// and nine taps a texel apart would only average it with itself.
		const float stride = scale >= 0.75f ? 2.0f : 1.0f;
		AOBlurUB blur{};
		blur.dir[2] = s_settings.aoBlurSharpness;

		draw->BindFramebufferAsRenderTarget(s_aoBlurFbo,
			{ RPAction::DONT_CARE, RPAction::DONT_CARE, RPAction::DONT_CARE, 0, 1.0f, 0, "lcs_ao_blur_x" },
			"lcs_ao_blur_x");
		draw->BindFramebufferAsTexture(s_aoFbo, 0, Aspect::COLOR_BIT, 0);
		draw->BindSamplerStates(0, 1, &s_aoSampler);
		draw->SetViewport(viewport);
		draw->SetScissorRect(0, 0, aoW, aoH);
		draw->BindPipeline(s_aoBlurPipeline);
		blur.dir[0] = stride / (float)aoW;
		blur.dir[1] = 0.0f;
		draw->UpdateDynamicUniformBuffer(&blur, sizeof(blur));
		draw->DrawUP(kFullscreenTriangle, 3);

		draw->BindFramebufferAsRenderTarget(s_aoFbo,
			{ RPAction::DONT_CARE, RPAction::DONT_CARE, RPAction::DONT_CARE, 0, 1.0f, 0, "lcs_ao_blur_y" },
			"lcs_ao_blur_y");
		draw->BindFramebufferAsTexture(s_aoBlurFbo, 0, Aspect::COLOR_BIT, 0);
		draw->BindSamplerStates(0, 1, &s_aoSampler);
		draw->SetViewport(viewport);
		draw->SetScissorRect(0, 0, aoW, aoH);
		draw->BindPipeline(s_aoBlurPipeline);
		blur.dir[0] = 0.0f;
		blur.dir[1] = stride / (float)aoH;
		draw->UpdateDynamicUniformBuffer(&blur, sizeof(blur));
		draw->DrawUP(kFullscreenTriangle, 3);
	}

	s_capture.aoRendered = true;
	s_capture.aoWidth = aoW;
	s_capture.aoHeight = aoH;

	// Said positively, and again whenever the size changes - a latched setup failure and a pass
	// that is simply switched off look the same from the frame, and only this tells them apart.
	static int s_lastLoggedAO[3] = { -1, -1, -1 };
	static int s_aoLogs = 0;
	const int now[3] = { aoW, aoH, (int)projScale };
	if ((now[0] != s_lastLoggedAO[0] || now[1] != s_lastLoggedAO[1]) && s_aoLogs < 16) {
		memcpy(s_lastLoggedAO, now, sizeof(now));
		s_aoLogs++;
		NOTICE_LOG(Log::G3D, "LCS AO: ran at %dx%d (mask %dx%d, viewport %dx%d), %.0f px per unit at 1 unit, radius %.2f",
			aoW, aoH, maskW, maskH, viewW, viewH, projScale, ub.params[0]);
	}
}

Draw::Framebuffer *AmbientOcclusion() {
	return s_aoFbo;
}

// --- the composite ----------------------------------------------------------------------------
//
// One triangle over the game's own framebuffer, multiplying what is there by the mask. The blend
// is ZERO/SRC_COLOR, so the shader's output IS the multiplier and the shadow's colour and depth
// are settings rather than another pass.
//
// It runs at the moment the frame turns from 3D to 2D, which is what keeps it off the HUD: the
// radar and the wanted stars are drawn after this, over the top, unshadowed.

struct CompositeUB {
	float tint[4];     // rgb, and strength in a
	float params[4];   // showMask, stretch for the depth-map view, showAO, unused
	float ao[4];       // AO strength (0 when it did not run), its share in sunlight, how much of
	                   // the light is direct right now, left half only
	float ao2[4];      // AO share on people, unused x3
};

static const UniformBufferDesc s_compositeUBDesc{ sizeof(CompositeUB), {
	{ "u_shadowTint", 0, -1, UniformType::FLOAT4, 0 },
	{ "u_compositeParams", 1, 0, UniformType::FLOAT4, 16 },
	{ "u_compositeAO", 2, 1, UniformType::FLOAT4, 32 },
	{ "u_compositeAO2", 3, 2, UniformType::FLOAT4, 48 },
} };

static Draw::Pipeline *s_compositePipeline;   // multiply into the frame
static Draw::Pipeline *s_compositeOpaque;     // ...or replace it, for looking at the mask
static Draw::SamplerState *s_compositeSampler;
static bool s_compositeSetupFailed;

static void ReleaseCompositeResources() {
	if (s_compositePipeline) {
		s_compositePipeline->Release();
		s_compositePipeline = nullptr;
	}
	if (s_compositeOpaque) {
		s_compositeOpaque->Release();
		s_compositeOpaque = nullptr;
	}
	if (s_compositeSampler) {
		s_compositeSampler->Release();
		s_compositeSampler = nullptr;
	}
}

static bool EnsureCompositeResources(Draw::DrawContext *draw) {
	if (s_compositePipeline && s_compositeOpaque) {
		return true;
	}
	if (s_compositeSetupFailed) {
		return false;
	}
	ReleaseCompositeResources();

	using namespace Draw;
	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	// The occlusion always has a texture bound, even when it did not run - the mask again, with
	// the strength at zero - so there is one pipeline rather than one per combination.
	static const SamplerDef samplers[] = {
		{ 0, "shadowMask", SamplerFlags(0) },
		{ 1, "aoTex", SamplerFlags(0) },
	};
	static const VaryingDef varyings[] = {
		{ "vec2", "v_uv", Draw::SEM_TEXCOORD0, 0, "highp" },
	};
	static const UniformDef uniforms[] = {
		{ "vec4", "u_shadowTint", 0 },
		{ "vec4", "u_compositeParams", 1 },
		{ "vec4", "u_compositeAO", 2 },
		{ "vec4", "u_compositeAO2", 3 },
	};

	const size_t kShaderBufferSize = 4096;
	char *vsCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(vsCode, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec2", "a_position", Draw::SEM_POSITION } };
		writer.BeginVSMain(inputs, uniforms, varyings);
		// The vertices arrive in clip space already, and the texture coordinate is that same
		// position remapped. No flip: clip -1 and texel row 0 are the same end of the image on
		// both backends, because the NDC convention and the framebuffer convention flip together.
		writer.C("  v_uv = a_position * 0.5 + 0.5;\n");
		writer.C("  gl_Position = vec4(a_position, 0.0, 1.0);\n");
		writer.EndVSMain(varyings);
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)vsCode, strlen(vsCode), "lcs_composite_vs");
	if (!vs) {
		ERROR_LOG(Log::G3D, "LCS composite VS failed:\n%s", vsCode);
	}
	delete[] vsCode;

	char *fsCode = new char[kShaderBufferSize];
	{
		ShaderWriter writer(fsCode, lang, ShaderStage::Fragment);
		writer.HighPrecisionFloat();
		writer.DeclareSamplers(samplers);
		writer.BeginFSMain(uniforms, varyings);
		writer.C("  vec4 maskSample = ").SampleTexture2D("shadowMask", "v_uv").C(";\n");
		writer.C("  float lit = maskSample.r;\n");
		// Towards the tint where the sun does not reach, and exactly white where it does - so a
		// fully lit pixel multiplies by one and the frame is bit-for-bit what the game drew.
		writer.C("  vec3 shaded = mix(u_shadowTint.rgb, vec3(1.0, 1.0, 1.0), lit);\n");
		writer.C("  vec3 col = mix(vec3(1.0, 1.0, 1.0), shaded, u_shadowTint.a);\n");
		// Occlusion takes away ambient light and nothing else, but all that is here is the finished
		// colour - so where the sun is reaching, only a share of it applies. How much of the light
		// is sun right now comes from the shadows' own strength, which already follows the time of
		// day and the weather: at night nearly all of it applies, at noon in the open little does.
		writer.C("  float ao = ").SampleTexture2D("aoTex", "v_uv").C(".r;\n");
		writer.C("  float sunlit = lit * u_compositeAO.z;\n");
		writer.C("  float aoAmount = u_compositeAO.x * (1.0 - ao) * mix(1.0, u_compositeAO.y, sunlit);\n");
		// The mask's alpha is zero on people - see aoOnPeople.
		writer.C("  aoAmount *= mix(u_compositeAO2.x, 1.0, maskSample.a);\n");
		writer.C("  if (u_compositeAO.w > 0.5 && v_uv.x > 0.5) { aoAmount = 0.0; }\n");
		writer.C("  col *= 1.0 - aoAmount;\n");
		writer.C("  vec4 outColor = vec4(col, 1.0);\n");
		writer.C("  if (u_compositeParams.x > 0.5) {\n");
		writer.C("    float v = u_compositeParams.y > 0.5 ? clamp((1.0 - lit) * 6.0, 0.0, 1.0) : lit;\n");
		writer.C("    outColor = vec4(v, v, v, 1.0);\n");
		writer.C("  }\n");
		// The raw estimate, before the strength or the sunlight share - what the pass computed
		// rather than what reached the frame.
		writer.C("  if (u_compositeParams.z > 0.5) {\n");
		writer.C("    outColor = vec4(ao, ao, ao, 1.0);\n");
		writer.C("  }\n");
		writer.EndFSMain("outColor");
	}
	ShaderModule *fs = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
		(const uint8_t *)fsCode, strlen(fsCode), "lcs_composite_fs");
	if (!fs) {
		ERROR_LOG(Log::G3D, "LCS composite FS failed:\n%s", fsCode);
	}
	delete[] fsCode;

	if (!vs || !fs) {
		if (vs) vs->Release();
		if (fs) fs->Release();
		ReleaseCompositeResources();
		s_compositeSetupFailed = true;
		return false;
	}

	static const InputLayoutDesc layoutDesc = {
		8,
		{ { SEM_POSITION, DataFormat::R32G32_FLOAT, 0 } },
	};
	InputLayout *inputLayout = draw->CreateInputLayout(layoutDesc);

	// The game's depth buffer is not ours to touch, and this covers the screen anyway.
	DepthStencilStateDesc dsDesc{};
	dsDesc.depthTestEnabled = false;
	dsDesc.depthWriteEnabled = false;
	dsDesc.depthCompare = Comparison::ALWAYS;
	DepthStencilState *depthStencil = draw->CreateDepthStencilState(dsDesc);

	BlendStateDesc multiplyDesc{};
	multiplyDesc.enabled = true;
	multiplyDesc.colorMask = 0x7;   // leave alpha alone; the game keeps things in it
	multiplyDesc.srcCol = BlendFactor::ZERO;
	multiplyDesc.dstCol = BlendFactor::SRC_COLOR;
	multiplyDesc.eqCol = BlendOp::ADD;
	multiplyDesc.srcAlpha = BlendFactor::ZERO;
	multiplyDesc.dstAlpha = BlendFactor::ONE;
	multiplyDesc.eqAlpha = BlendOp::ADD;
	BlendState *multiply = draw->CreateBlendState(multiplyDesc);
	BlendState *opaque = draw->CreateBlendState({ false, 0x7 });
	RasterState *raster = draw->CreateRasterState({});

	SamplerStateDesc sampDesc{};
	sampDesc.magFilter = TextureFilter::LINEAR;
	sampDesc.minFilter = TextureFilter::LINEAR;
	sampDesc.mipFilter = TextureFilter::NEAREST;
	sampDesc.wrapU = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapV = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
	s_compositeSampler = draw->CreateSamplerState(sampDesc);

	PipelineDesc desc{
		Primitive::TRIANGLE_LIST,
		{ vs, fs },
		inputLayout,
		depthStencil,
		multiply,
		raster,
		&s_compositeUBDesc,
		samplers,
	};
	s_compositePipeline = draw->CreateGraphicsPipeline(desc, "lcs_shadow_composite");
	desc.blend = opaque;
	s_compositeOpaque = draw->CreateGraphicsPipeline(desc, "lcs_shadow_composite_opaque");

	vs->Release();
	fs->Release();
	inputLayout->Release();
	depthStencil->Release();
	multiply->Release();
	opaque->Release();
	raster->Release();

	if (!s_compositePipeline || !s_compositeOpaque) {
		ERROR_LOG(Log::G3D, "LCS shadow composite pipeline failed to create");
		ReleaseCompositeResources();
		s_compositeSetupFailed = true;
		return false;
	}
	return true;
}

// The frame's FIRST capture, as it stood when a restart threw it away - what a second capture has to
// measure up to before the passes run on it again. See OnFlush.
static bool s_recaptureThisFrame = false;
// Early seams: a 2D draw the game makes in the MIDDLE of its world, which the passes took for the seam
// between the world and the HUD. See OnFlush.
static bool s_recaptureRan = false;      // this frame's passes ran a second time, on a real second capture
static u32 s_firstSeamTex = 0;           // the texture of the 2D draw this frame's passes FIRST ran at
static u32 s_secondSeamTex = 0;          // ... and the one they ran at again, after the restart
static constexpr int kEarlySeamTextures = 4;
static u32 s_earlySeamTex[kEarlySeamTextures];  // learned: draws with these are not the end of the world
static int s_earlySeamIdleFrames = 0;    // frames since one of them was last passed over
static bool s_passedEarlySeam = false;
static int s_seamsDeferred = 0;
static int s_firstCasterIndices = 0;
static int s_recapturesSkipped = 0;

// What the last composite multiplied the frame by - see CompositedMask.
static bool s_maskComposited = false;
static float s_compositedTint[4];

Draw::Framebuffer *CompositedMask(float tintStrength[4]) {
	if (!s_maskComposited || !s_maskFbo) {
		return nullptr;
	}
	memcpy(tintStrength, s_compositedTint, sizeof(s_compositedTint));
	return s_maskFbo;
}

static void RenderComposite(Draw::DrawContext *draw, Draw::Framebuffer *target, int width, int height) {
	s_capture.composited = false;
	if (!s_capture.maskRendered || !s_maskFbo || !target) {
		return;
	}
	if (!EnsureCompositeResources(draw)) {
		return;
	}

	using namespace Draw;
	draw->BindFramebufferAsRenderTarget(target,
		{ RPAction::KEEP, RPAction::KEEP, RPAction::KEEP, 0, 0.0f, 0, "lcs_shadow_composite" },
		"lcs_shadow_composite");
	// Debug view 4 puts the depth map itself on the screen instead of the mask. It is the one
	// picture that says what the depth pass actually holds, which is how the cache was finally
	// shown to be working when three rounds of reasoning about it had not.
	if (s_settings.debugView == 4 && s_settings.showMask && s_fbo) {
		draw->BindFramebufferAsTexture(s_fbo, 0, Aspect::DEPTH_BIT, 0);
	} else {
		draw->BindFramebufferAsTexture(s_maskFbo, 0, Aspect::COLOR_BIT, 0);
	}
	const bool haveAO = s_capture.aoRendered && s_aoFbo;
	draw->BindFramebufferAsTexture(haveAO ? s_aoFbo : s_maskFbo, 1, Aspect::COLOR_BIT, 0);
	Draw::SamplerState *compositeSamplers[2] = { s_compositeSampler, s_compositeSampler };
	draw->BindSamplerStates(0, 2, compositeSamplers);

	Viewport viewport{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, width, height);
	const bool showAO = s_settings.showAO && haveAO;
	draw->BindPipeline(s_settings.showMask || showAO ? s_compositeOpaque : s_compositePipeline);

	CompositeUB ub;
	ub.tint[0] = s_settings.tint[0];
	ub.tint[1] = s_settings.tint[1];
	ub.tint[2] = s_settings.tint[2];
	// The sun's own brightness carries the strength, so shadows thin out towards dusk and are
	// gone at night without anything here having to know what time it is.
	{
		const bool live = s_current.sunValid;
		const float *d = live ? s_current.sunDiffuse
			: (s_haveHeldSun ? s_heldSunDiffuse : s_published.sunDiffuse);
		float luminance = d[0] * 0.299f + d[1] * 0.587f + d[2] * 0.114f;
		if (luminance > 1.0f) luminance = 1.0f;
		if (luminance < 0.0f) luminance = 0.0f;
		// A moon's light is dim by definition, and the game's night colour is dim with it, so
		// scaling by luminance alone would leave nothing at all. The floor is what makes night
		// shadows visible; moonStrength is what keeps them from looking like noon.
		const bool moon = live ? s_current.sunIsMoon
			: (s_haveHeldSun ? s_heldSunIsMoon : s_published.sunIsMoon);
		// A low sun fades its shadows out before it sets - see lowSunFade - so the swap to the moon
		// starts from nothing and the moon's shadows ease in from there.
		float lowSun = 1.0f;
		if (!moon) {
			const float *dir = live ? s_current.sunDir
				: (s_haveHeldSun ? s_heldSunDir : s_published.sunDir);
			const float top = std::max(s_settings.lowSunFade, kMinSunElevation + 0.01f);
			float k = std::clamp((dir[2] - kMinSunElevation) / (top - kMinSunElevation), 0.0f, 1.0f);
			lowSun = k * k * (3.0f - 2.0f * k);
		}
		ub.tint[3] = EaseShadowStrength(moon ? s_settings.strength * s_settings.moonStrength
			: s_settings.strength * luminance * lowSun);
		// And the weather's share - see hideInRain. Zero never gets this far; OnFlush skips it.
		ub.tint[3] *= s_weatherFade;
	}
	s_maskComposited = !(s_settings.showMask || showAO);
	memcpy(s_compositedTint, ub.tint, sizeof(s_compositedTint));
	ub.params[0] = s_settings.showMask ? 1.0f : 0.0f;
	// The depth map is almost all near-white at these ranges, so it is stretched to be legible.
	ub.params[1] = s_settings.debugView == 4 ? 1.0f : 0.0f;
	ub.params[2] = showAO ? 1.0f : 0.0f;
	ub.params[3] = 0.0f;
	ub.ao[0] = haveAO ? std::clamp(s_settings.aoStrength, 0.0f, 1.0f) : 0.0f;
	ub.ao[1] = std::clamp(s_settings.aoInSun, 0.0f, 1.0f);
	// How much of the light is direct sun, read back off the strength the shadows just settled
	// on - which is the sun's brightness, the moon's share and the rain's, already eased. Asking
	// for them again would advance the easing a second time in one frame.
	ub.ao[2] = s_settings.strength > 0.0f ? std::clamp(ub.tint[3] / s_settings.strength, 0.0f, 1.0f) : 0.0f;
	ub.ao[3] = s_settings.aoSplit ? 1.0f : 0.0f;
	ub.ao2[0] = std::clamp(s_settings.aoOnPeople, 0.0f, 1.0f);
	ub.ao2[1] = 0.0f;
	ub.ao2[2] = 0.0f;
	ub.ao2[3] = 0.0f;
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

	// One oversized triangle rather than two, so there is no seam down the diagonal and no index
	// buffer to keep alive.
	static const float kFullscreenTriangle[6] = {
		-1.0f, -1.0f,
		 3.0f, -1.0f,
		-1.0f,  3.0f,
	};
	draw->DrawUP(kFullscreenTriangle, 3);
	s_capture.composited = true;
}

// How much of the render target the game actually draws into, in the target's own pixels.
//
// The size the game draws at, over the size the framebuffer IS, times the pixels in it - which is
// exactly how PPSSPP sizes its own viewport (ConvertViewportAndScissor). This used to be the whole
// render target, and that is the same number only for as long as the two sizes agree. They stop
// agreeing for good the first time the framebuffer grows: PPSSPP enlarges a buffer as soon as
// something draws past its edge, and shrinks one only once it has more than halved.
static void DrawnArea(const ViewportCapture &vp, int bufferWidth, int bufferHeight,
	int fbWidth, int fbHeight, int *drawnW, int *drawnH) {
	*drawnW = fbWidth;
	*drawnH = fbHeight;
	if (bufferWidth > 0 && bufferHeight > 0 && vp.rtWidth > 0.0f && vp.rtHeight > 0.0f) {
		*drawnW = std::min(fbWidth, (int)(vp.rtWidth * (float)fbWidth / (float)bufferWidth + 0.5f));
		*drawnH = std::min(fbHeight, (int)(vp.rtHeight * (float)fbHeight / (float)bufferHeight + 0.5f));
	}

	static int s_lastLogged[4] = { -1, -1, -1, -1 };
	static int s_logs = 0;
	const int now[4] = { (int)vp.rtWidth, (int)vp.rtHeight, bufferWidth, bufferHeight };
	if (memcmp(now, s_lastLogged, sizeof(now)) != 0 && s_logs < 16) {
		memcpy(s_lastLogged, now, sizeof(now));
		s_logs++;
		NOTICE_LOG(Log::G3D, "LCS shadows: the game draws %dx%d into a %dx%d framebuffer (%dx%d pixels) - the mask covers %dx%d",
			now[0], now[1], bufferWidth, bufferHeight, fbWidth, fbHeight, *drawnW, *drawnH);
	}
}

bool OnFlush(Draw::DrawContext *draw, bool through, Draw::Framebuffer *target,
	int bufferWidth, int bufferHeight, TextureCacheCommon *textureCache) {
	if (!g_active || s_frameComposited || !through || !draw || !target) {
		return false;
	}
	// Nothing 3D has been captured, so this is not the seam between the world and the HUD - it is
	// a frame that is 2D from the start, like a loading screen or a menu.
	if (s_capture.draws == 0 || s_batches.empty()) {
		return false;
	}
	// A render-to-texture target is not the frame the player is looking at. Compositing onto one
	// would darken whatever the game is about to sample as a texture.
	if (gstate_c.curRTWidth < 480 || gstate_c.curRTHeight < 272) {
		return false;
	}
	// A restart that is NOT a second frame. The re-capture exists for the game drawing its whole
	// scene again after the HUD - measured with draw, caster and receiver counts within a few of
	// the first. In the rain something 3D arrives after the HUD on every frame, and the passes ran
	// a second time on a capture of little but that, compositing a second set of shadows, out of
	// place, over the frame's own: reported as shadows in the rain "very weirdly misplaced", and
	// what hideInRain was hiding. Logged as 1000 restarts in 40 seconds of rain. A second capture
	// with under a quarter of the first's casters keeps the first composite and runs nothing.
	if (s_recaptureThisFrame &&
			s_capture.casterIndices < std::max(300, s_firstCasterIndices / 4)) {
		s_frameComposited = true;
		s_recapturesSkipped++;
		if (s_recapturesSkipped <= 3 || s_recapturesSkipped % 1000 == 0) {
			NOTICE_LOG(Log::G3D, "LCS shadows: a re-capture of %d caster indices against the frame's %d "
				"is not a second frame - keeping the first composite (%d so far)",
				s_capture.casterIndices, s_firstCasterIndices, s_recapturesSkipped);
		}
		return false;
	}
	// A SEAM TOO EARLY. In the rain the game makes a blended, textured 2D draw a quarter of the way
	// through its world - measured: the passes fired with 10446 of a frame's 42258 caster indices in,
	// on the same texture every frame - and the rest of the world arrived after it. So the passes
	// ran on a quarter of the city, the restart above caught the rest and ran them again: two
	// composites a frame, each from part of the world. That, not the puddles, is what "shadows act
	// strange in the rain" was, and what hideInRain hid.
	//
	// So the texture of the draw the passes FIRST ran at, in a frame where they then had to run again,
	// is learned (see BeginFrame), and from then on a 2D draw with it is passed over: the capture
	// carries on and the passes run once, at the seam after the world. Not a share of the casters -
	// that was tried, and an early seam in the rain arrives with anywhere from a quarter to over half
	// of them, so no threshold separated it from the real one; the shadows blinked out on frames
	// that missed both. A frame with no such draw - every frame in clear weather - never learns one.
	const u32 seamTex = gstate.isTextureMapEnabled() ? gstate.getTextureAddress(0) : 0;
	if (seamTex != 0 && !s_recaptureThisFrame) {
		for (u32 t : s_earlySeamTex) {
			if (t == seamTex) {
				s_passedEarlySeam = true;
				s_seamsDeferred++;
				if (s_seamsDeferred <= 3 || s_seamsDeferred % 1000 == 0) {
					NOTICE_LOG(Log::G3D, "LCS shadows: passed over the early 2D draw (%08x) with %d caster "
						"indices in (%d so far)", seamTex, s_capture.casterIndices, s_seamsDeferred);
				}
				return false;
			}
		}
	}
	if (s_recaptureThisFrame) {
		s_recaptureRan = true;
		s_secondSeamTex = seamTex;
	} else if (s_firstSeamTex == 0) {
		s_firstSeamTex = seamTex;
	}
	// Faded all the way out for the rain, so skip all three passes rather than composite a mask at
	// zero strength. The capture carries on, which keeps the caster cache warm for when it dries.
	if (s_weatherFade <= 0.0f) {
		s_frameComposited = true;
		s_capture.rainSuppressed = true;
		return false;
	}

	// Everything the passes need belongs to THIS frame - the geometry, the view matrix, the
	// viewport and the sun - which is why they run here rather than at the top of the next frame
	// against the one that just ended. A screen-space mask a frame behind the frame it is
	// multiplied into slides across the picture every time the camera turns.
	s_frameComposited = true;
	ComputeShadowView(s_current);
	if (s_framesOnHeldSun == 1) {
		// Only the frame it starts, not every frame it continues, and only the first handful of
		// times - the point is to say the mechanism is being used at all, not to narrate every
		// use of it. Frequent lines here is what the flicker looked like from inside.
		static int s_heldSunLogs = 0;
		if (s_heldSunLogs < 8) {
			s_heldSunLogs++;
			NOTICE_LOG(Log::G3D,
				"LCS shadows: no sun in this frame, holding the last one (%d)", s_heldSunLogs);
		}
	}
	if (!s_view.valid) {
		return false;
	}

	int fbWidth = 0, fbHeight = 0;
	draw->GetFramebufferDimensions(target, &fbWidth, &fbHeight);
	if (fbWidth <= 0 || fbHeight <= 0) {
		return false;
	}

	float scale = s_settings.maskScale;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 1.0f) scale = 1.0f;
	const int maskW = (int)((float)fbWidth * scale + 0.5f) > 0 ? (int)((float)fbWidth * scale + 0.5f) : 1;
	const int maskH = (int)((float)fbHeight * scale + 0.5f) > 0 ? (int)((float)fbHeight * scale + 0.5f) : 1;

	// The mask's viewport has to cover the same fraction of the mask that the game's viewport
	// covers of its render target, or the composite - which maps the whole mask over the whole
	// target - lands offset. See DrawnArea for why that is not simply the whole target.
	int drawnW = fbWidth, drawnH = fbHeight;
	DrawnArea(s_frameViewport, bufferWidth, bufferHeight, fbWidth, fbHeight, &drawnW, &drawnH);
	int viewW = (int)((float)drawnW * scale + 0.5f);
	int viewH = (int)((float)drawnH * scale + 0.5f);
	if (viewW <= 0 || viewW > maskW) viewW = maskW;
	if (viewH <= 0 || viewH > maskH) viewH = maskH;

	SubmitCachedCasters(textureCache);
	RenderCascade(draw);
	RenderMask(draw, maskW, maskH, viewW, viewH);
	RenderAO(draw, maskW, maskH, viewW, viewH);
	RenderComposite(draw, target, fbWidth, fbHeight);
	return true;
}

// The compat flag, re-read rather than latched at Init.
//
// The draw engine is constructed before the PSP kernel starts, and the kernel is where the
// settings file is read - so at Init time the flag may not have been loaded for this disc yet,
// and latching it there is how the feature came back from the ini switched on and inert. Reading
// it again whenever the switch moves costs nothing and cannot be a frame late.
static void RefreshAvailability() {
	g_available = LCS::IsActive();
}

// LCS: the menu's Shadow quality and Ambient occlusion rows, followed from here at every BeginFrame
// rather than pushed by the menu (Core/LCS has no renderer dependency; LCSSmaa reads its row the
// same way). Applied when a row moves, so the debugger's edits to the same fields stand until then.
// The gate is read again every frame as well, not only when the switch moves: LCS::IsActive turns on
// when the EBOOT loads, after the draw engine is built, and off when the game stops.
static int s_followedShadows = -1;
static int s_followedAO = -1;
// LCS: LCS_SHADOW_STATS=<host frames> logs the last frame's capture and passes that often, for a
// run nobody is watching (the VCS fork read these off its debugger tab).
static int s_statsEvery = 0;

static void FollowSettings() {
	// Not LCS (or no longer): nothing to follow. Off if the game has just stopped.
	if (!LCS::IsActive()) {
		if (g_active) {
			SetEnabled(s_enabled);
		}
		return;
	}
	const LCS::LCSSettings &s = LCS::Settings();
	if (s.shadows != s_followedShadows) {
		s_followedShadows = s.shadows;
		s_settings.entityCastersOnly = s.shadows == LCS::kLCSShadowsEntities || s.shadows == LCS::kLCSShadowsProps;
		s_settings.propCasters = s.shadows == LCS::kLCSShadowsProps;
		SetEnabled(s.shadows != LCS::kLCSShadowsOff);
		NOTICE_LOG(Log::G3D, "LCS shadows: quality %d, %s", s.shadows, g_active ? "on" : "off");
	} else if (!g_available) {
		SetEnabled(s_enabled);
		NOTICE_LOG(Log::G3D, "LCS shadows: %s", g_active ? "on" : "off");
	}
	if ((int)s.ambientOcclusion != s_followedAO) {
		s_followedAO = s.ambientOcclusion;
		s_settings.ambientOcclusion = s.ambientOcclusion;
	}
}

void Init() {
	s_followedShadows = -1;
	s_followedAO = -1;
	// Read once, like the camera and water traces beside it.
	s_horizonTrace = getenv("LCS_HORIZON_TRACE") != nullptr;
	s_cacheTrace = getenv("LCS_CACHE_TRACE") != nullptr;
	if (const char *sw = getenv("LCS_STANDIN_SWITCH")) {
		s_standInSwitchPath = sw;
		s_standInSwitchLast = 0.0;
		s_standInCasters = true;
	}
	s_cacheTraceLast = 0.0;
	s_cacheTraceFrames = 0;
	memset(s_bearingHist, 0, sizeof(s_bearingHist));
	s_bearingFrames = 0;
	s_bearingLast = 0.0;
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	s_sunLuminance = 0.0f;
	memset(&s_view, 0, sizeof(s_view));
	memset(&s_frameViewport, 0, sizeof(s_frameViewport));
	s_haveViewMatrix = false;
	s_haveLastCameraPos = false;
	s_haveLastCameraWorld = false;
	s_frameComposited = false;
	s_buckets.clear();
	s_objectBounds.clear();
	s_blobTextureCount = 0;
	s_blobCandidateCount = 0;
	s_haveHeldSun = false;
	s_framesOnHeldSun = 0;
	s_haveHeldMap = false;
	s_framesOnHeldMap = 0;
	s_usingHeldMap = false;
	s_weatherFade = 1.0f;
	s_weatherWetness = 0.0f;
	s_lastWeatherTime = 0.0;
	s_haveSmoothSun = false;
	s_lastSunTurnTime = 0.0;
	s_haveEasedStrength = false;
	s_lastStrengthTime = 0.0;

	// LCS::IsActive is the disc, version and EBOOT check as well.
	RefreshAvailability();
	g_active = g_available && s_enabled;

	// The raw occlusion over the frame, for a run with nobody at the keyboard - the same lever as
	// LCSWater's LCS_WATER_DEBUG. Not persisted anywhere, so nothing loaded later overrides it.
	if (const char *show = getenv("LCS_AO_SHOW")) {
		s_settings.showAO = atoi(show) != 0;
		NOTICE_LOG(Log::G3D, "LCS AO: showing the raw estimate: %d", s_settings.showAO ? 1 : 0);
	}
	if (const char *stats = getenv("LCS_SHADOW_STATS")) {
		s_statsEvery = std::max(atoi(stats), 1);
	}
	// The mask drawn over the frame instead of multiplied into it (Settings::showMask).
	if (const char *mask = getenv("LCS_SHADOW_MASK")) {
		s_settings.showMask = atoi(mask) != 0;
		NOTICE_LOG(Log::G3D, "LCS shadows: showing the mask: %d", s_settings.showMask ? 1 : 0);
	}
	if (const char *split = getenv("LCS_AO_SPLIT")) {
		s_settings.aoSplit = atoi(split) != 0;
		NOTICE_LOG(Log::G3D, "LCS AO: left half only: %d", s_settings.aoSplit ? 1 : 0);
	}
}

void SetEnabled(bool enabled) {
	s_enabled = enabled;
	RefreshAvailability();
	g_active = g_available && s_enabled;
	if (enabled) {
		// Switching it back on is the one place a fresh attempt makes sense - the alternative is
		// a latched failure that can only be cleared by restarting the emulator.
		s_depthSetupFailed = false;
		s_maskSetupFailed = false;
		s_compositeSetupFailed = false;
		s_cutSetupFailed = false;
		s_aoSetupFailed = false;
	}
	if (!g_active) {
		// Drop the frame's work immediately rather than leaving a stale map on screen in the
		// debugger and a buffer full of geometry nobody will draw.
		s_positions.clear();
		s_casterIndices.clear();
		s_receiverIndices.clear();
		s_pedReceiverIndices.clear();
		s_batches.clear();
		s_buckets.clear();
		s_objectBounds.clear();
		s_cutGroups.clear();
		s_cutPending = false;
		memset(&s_capture, 0, sizeof(s_capture));
		memset(&s_capturePublished, 0, sizeof(s_capturePublished));
	}
}

bool IsEnabled() {
	return s_enabled;
}

void Shutdown() {
	ReleaseResources();
	ReleaseMaskResources();
	ReleaseAOResources();
	ReleaseCompositeResources();
	ReleaseCutoutResources();
	s_cutSetupFailed = false;
	s_aoSetupFailed = false;
	s_cutGroups.clear();
	s_cutPending = false;
	s_depthSetupFailed = false;
	s_maskSetupFailed = false;
	s_compositeSetupFailed = false;
	g_available = false;
	g_active = false;
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	memset(&s_view, 0, sizeof(s_view));
	memset(&s_frameViewport, 0, sizeof(s_frameViewport));
	s_sunLuminance = 0.0f;
	s_haveViewMatrix = false;
	s_haveLastCameraPos = false;
	s_haveLastCameraWorld = false;
	s_frameComposited = false;
	s_buckets.clear();
	s_objectBounds.clear();
	s_blobTextureCount = 0;
	s_blobCandidateCount = 0;
}

void DeviceLost() {
	ReleaseResources();
	ReleaseMaskResources();
	ReleaseAOResources();
	ReleaseCompositeResources();
	ReleaseCutoutResources();
}

static void LogStats() {
	const FrameStats &f = s_current;
	const CaptureStats &c = s_capture;
	NOTICE_LOG(Log::G3D, "LCS shadows stats: active %d | draws %d casters %d (skinned %d) | caster idx %d, receiver idx %d, in cascade %d | rendered %d, mask %d (step %d), AO %d, composited %d, rain-suppressed %d | view %d cam (%.1f %.1f %.1f) game cam (%.1f %.1f %.1f) | sun %d (%.2f %.2f %.2f) moon %d lum %.2f | fade %.2f wet %.2f",
		g_active ? 1 : 0, f.draws, f.casters, f.castersSkinned, c.casterIndices, c.receiverIndices, c.verticesInCascade,
		c.rendered ? 1 : 0, c.maskRendered ? 1 : 0, (int)c.maskStep, c.aoRendered ? 1 : 0, c.composited ? 1 : 0,
		c.rainSuppressed ? 1 : 0, s_view.valid ? 1 : 0, s_view.cameraPos[0], s_view.cameraPos[1], s_view.cameraPos[2],
		LCS::CameraWorld(0).value_or(0.0f), LCS::CameraWorld(1).value_or(0.0f), LCS::CameraWorld(2).value_or(0.0f), f.sunValid ? 1 : 0, f.sunDir[0], f.sunDir[1],
		f.sunDir[2], f.sunIsMoon ? 1 : 0, s_sunLuminance, s_weatherFade, s_weatherWetness);
	for (int i = 0; i < 4; i++) {
		const FrameStats::SunCandidate &k = f.sunCandidates[i];
		if (k.valid) {
			NOTICE_LOG(Log::G3D, "LCS shadows stats:   light %d (%.3f %.3f %.3f) diffuse (%.2f %.2f %.2f) draws %d changes %d",
				i, k.dir[0], k.dir[1], k.dir[2], k.diffuse[0], k.diffuse[1], k.diffuse[2], k.draws, k.directionChanges);
		}
	}
}

void BeginFrame(Draw::DrawContext *draw) {
	FollowSettings();
	static int s_statsFrames = 0;
	if (s_statsEvery > 0 && g_active && ++s_statsFrames >= s_statsEvery) {
		s_statsFrames = 0;
		LogStats();
	}
	// The frame just ended, judged before any of it is reset: did it have a seam too early? Either
	// the passes ran twice on comparable captures, or one was passed over and the passes ran later.
	{
		// The passes had to run twice, at two DIFFERENT draws: the first was too early, so learn its
		// texture. At the same draw twice it was a real second frame - the whole scene again, which
		// happens a few times a session in clear weather - and learning that texture, the HUD's,
		// would pass over the only seam there is and draw no shadows at all.
		if (s_recaptureRan && s_firstSeamTex != 0 && s_firstSeamTex != s_secondSeamTex) {
			bool known = false;
			for (u32 t : s_earlySeamTex) {
				known = known || t == s_firstSeamTex;
			}
			if (!known) {
				for (int i = kEarlySeamTextures - 1; i > 0; i--) {
					s_earlySeamTex[i] = s_earlySeamTex[i - 1];
				}
				s_earlySeamTex[0] = s_firstSeamTex;
				NOTICE_LOG(Log::G3D, "LCS shadows: a 2D draw with texture %08x comes before the world is "
					"finished - waiting past it from now on", s_firstSeamTex);
			}
		}
		// And forget them once they stop turning up - the rain has stopped, or the texture moved.
		if (s_passedEarlySeam) {
			s_earlySeamIdleFrames = 0;
		} else if (++s_earlySeamIdleFrames > 600) {
			memset(s_earlySeamTex, 0, sizeof(s_earlySeamTex));
			s_earlySeamIdleFrames = 0;
		}
		s_recaptureRan = false;
		s_passedEarlySeam = false;
		s_firstSeamTex = 0;
		s_secondSeamTex = 0;
	}
	// Before the gate: with the shadows off, last frame's mask must not be handed out as this one's.
	s_maskComposited = false;
	s_recaptureThisFrame = false;
	if (!g_active) {
		return;
	}
	// Turning entity mode ON has to throw the cache away rather than let it drain. Everything in
	// it is scenery, nothing captured from here on will replace any of it, and a held cell casts
	// for cacheHoldSeconds - so without this the setting appears not to work for the better part
	// of half a minute, which reads as the filter being broken.
	static bool s_cachedUnderEntityMode = false;
	if (s_cachedUnderEntityMode != s_settings.entityCastersOnly) {
		s_cachedUnderEntityMode = s_settings.entityCastersOnly;
		s_buckets.clear();
	}

	UpdateWeatherFade();

	// A BLINK: the composite ran last frame and did not run this one, which is exactly what the
	// player sees as shadows flickering. Counted with the reason, because "shadows flicker" has
	// several possible mechanisms and only a count can say which one is happening.
	//
	// The reasons are tested in the order the pass itself gives up in, so the first one that
	// applies is the one that stopped it. A frame skipped for the rain is not one: the shadows
	// had already faded to nothing before it.
	if (s_lastComposited && !s_capture.composited && !s_capture.rainSuppressed && s_capture.draws > 50) {
		s_blinks++;
		if (!s_current.sunValid && !s_haveHeldSun) {
			s_blinkNoSun++;
		} else if (!s_view.valid) {
			s_blinkNoView++;
		} else if (s_capture.casterIndices == 0) {
			s_blinkNoCasters++;
		} else {
			s_blinkOther++;
		}
		NOTICE_LOG(Log::G3D,
			"LCS shadows: BLINK %d (no sun %d, no view %d, no casters %d, other %d) - "
			"casters %d, draws %d",
			s_blinks, s_blinkNoSun, s_blinkNoView, s_blinkNoCasters, s_blinkOther,
			s_capture.casterIndices, s_capture.draws);
	}
	if (s_capture.draws > 50) {
		s_lastComposited = s_capture.composited;
	}

	// Every few seconds: the blink tally, and whether the vanilla blob is being suppressed by the
	// LEARNED texture set or by the positional test on the draw itself. The second number is what
	// says the chunk-load case is covered - right after a load the learned set is empty, so a
	// nonzero suppression count there can only be coming from the positional path.
	{
		// Only when the blink count has actually moved, plus once at the start. A line every few
		// seconds forever is noise in somebody's log; a line when the thing being watched changes
		// is a diagnostic.
		static int s_statusCountdown = 0;
		static int s_lastLoggedBlinks = -1;
		const bool changed = s_blinks != s_lastLoggedBlinks;
		if (s_capture.draws > 50 && (changed || --s_statusCountdown <= 0)) {
			s_statusCountdown = 3600;
			s_lastLoggedBlinks = s_blinks;
			NOTICE_LOG(Log::G3D,
				"LCS shadows: %d blinks (sun %d, view %d, casters %d, other %d) | blobs hidden: "
				"%d by position, %d by learned texture (%d learned) | %d frames on a held sun",
				s_blinks, s_blinkNoSun, s_blinkNoView, s_blinkNoCasters, s_blinkOther,
				s_blobByPosition, s_blobByTexture, s_capture.blobTextures, s_framesOnHeldSun);
		}
	}

	// Publish and reset, and nothing else. The passes themselves run inside the frame, at the
	// seam between the world and the HUD - see OnFlush.
	s_published = s_current;
	s_capturePublished = s_capture;

	// Once per boot, for the same reason the cache says so: entity mode rests on a claim about
	// this game's vertex formats, and a build where that claim stopped holding would show up as
	// shadows quietly missing rather than as anything an assert could catch. A count near zero
	// means nothing is being recognised as a person or a vehicle.
	static bool s_saidEntityMode = false;
	if (s_settings.entityCastersOnly && !s_saidEntityMode && s_capture.draws > 50) {
		s_saidEntityMode = true;
		WARN_LOG(Log::G3D, "LCS shadows: people-and-vehicles shadows: %d of %d captured draws cast, %d of them skinned, %d props",
			s_capture.draws - s_capture.receiverOnlyDraws, s_capture.draws,
			s_capture.casterSkinnedDraws, s_capture.casterPropDraws);
	}

	memset(&s_capture, 0, sizeof(s_capture));
	s_positions.clear();
	s_casterIndices.clear();
	s_receiverIndices.clear();
	s_pedReceiverIndices.clear();
	s_batches.clear();
	// clear() keeps the storage, and these settle after a few frames - but the first frames of a
	// scene reallocate a megabyte at a time, which is a hitch exactly when the world is streaming.
	s_positions.reserve(768 * 1024);
	s_casterIndices.reserve(768 * 1024);
	s_receiverIndices.reserve(768 * 1024);

	memset(&s_current, 0, sizeof(s_current));
	s_sunLuminance = 0.0f;
	NoteGameSun();
	s_haveViewMatrix = false;
	s_frameComposited = false;
	s_frameIndex++;
	s_objectBounds.clear();
	s_texScansThisFrame = 0;
	s_capture.blobTextures = s_blobTextureCount;

	// Emptied rather than erased, so each group keeps its storage for the texture that fills it
	// next - unless the count has run away, which would mean groups are never being reused.
	if (s_cutGroups.size() > 512) {
		s_cutGroups.clear();
	}
	for (CutoutGroup &group : s_cutGroups) {
		group.verts.clear();
		group.idx.clear();
	}
	s_cutPending = false;
}

// LCS: the sun, from the game's clock (LCS::ReadSun), for the whole frame. LCS lights its few lit
// draws with one fixed direction on channel 0 - (0.05, 1, 0) at every hour - and the world with
// vertex colours, so the GE carries no sun at all (measured over a day: NoteLights below found only
// that). Mirrored to a moon below the horizon exactly as NoteLights does it.
static bool s_sunFromGame = false;

static void NoteGameSun() {
	const LCS::SunState sun = LCS::ReadSun();
	s_sunFromGame = sun.valid;
	if (!sun.valid) {
		return;
	}
	float unit[3] = { sun.dir[0], sun.dir[1], sun.dir[2] };
	bool moon = false;
	if (unit[2] <= kMinSunElevation) {
		moon = true;
		unit[2] = fabsf(unit[2]);
		if (unit[2] < 0.35f) {
			unit[2] = 0.35f;
			const float xy = sqrtf(unit[0] * unit[0] + unit[1] * unit[1]);
			const float want = sqrtf(1.0f - unit[2] * unit[2]);
			if (xy > 1e-6f) {
				unit[0] *= want / xy;
				unit[1] *= want / xy;
			} else {
				unit[0] = want;
			}
		}
	}
	s_sunLuminance = sun.colour[0] * 0.299f + sun.colour[1] * 0.587f + sun.colour[2] * 0.114f;
	s_current.sunValid = true;
	s_current.sunIsMoon = moon;
	s_current.sunChannel = -1;
	for (int c = 0; c < 3; c++) {
		s_current.sunDir[c] = unit[c];
		s_current.sunDiffuse[c] = sun.colour[c];
	}
	s_captureLightDir[0] = -(s_haveSmoothSun ? s_smoothSunDir[0] : unit[0]);
	s_captureLightDir[1] = -(s_haveSmoothSun ? s_smoothSunDir[1] : unit[1]);
	s_captureLightDir[2] = -(s_haveSmoothSun ? s_smoothSunDir[2] : unit[2]);
	s_haveCaptureLightDir = true;
}

// Finds the sun among the four GE light channels. GTA hands the hardware its time-of-day directional
// on channel 0 and, near street lamps, per-object lamp directionals on the others - which are as
// bright or brighter, so the channel decides, not brightness. See the channel test below.
static void NoteLights() {
	if (!gstate.isLightingEnabled()) {
		return;
	}
	s_current.lightingDraws++;

	bool anyDirectional = false;
	for (int i = 0; i < 4; i++) {
		if (!gstate.isLightChanEnabled(i) || !gstate.isDirectionalLight(i)) {
			continue;
		}
		anyDirectional = true;

		// PSP colours are packed ABGR, so red is the low byte.
		const u32 diffuse = gstate.getDiffuseColor(i);
		const float r = (float)(diffuse & 0xFF) * (1.0f / 255.0f);
		const float g = (float)((diffuse >> 8) & 0xFF) * (1.0f / 255.0f);
		const float b = (float)((diffuse >> 16) & 0xFF) * (1.0f / 255.0f);
		const float luminance = r * 0.299f + g * 0.587f + b * 0.114f;
		if (s_current.sunValid && luminance <= s_sunLuminance) {
			continue;
		}

		float dir[3] = {
			getFloat24(gstate.lpos[i * 3 + 0]),
			getFloat24(gstate.lpos[i * 3 + 1]),
			getFloat24(gstate.lpos[i * 3 + 2]),
		};
		const float lengthSq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
		if (lengthSq < 1e-12f) {
			// A zero direction is a light that is enabled but not meaningfully set up. Taking it
			// would give the shadow projection no axis at all, so leave the previous pick alone.
			continue;
		}
		const float invLength = 1.0f / sqrtf(lengthSq);

		// Record every candidate, whether or not it wins, so the panel can show what the game
		// actually offered rather than only what got chosen.
		{
			FrameStats::SunCandidate &candidate = s_current.sunCandidates[i];
			candidate.valid = true;
			candidate.draws++;
			for (int c = 0; c < 3; c++) {
				const float normalized = dir[c] * invLength;
				if (candidate.draws > 1 && fabsf(normalized - candidate.dir[c]) > 0.01f) {
					candidate.directionChanges++;
				}
				candidate.dir[c] = normalized;
			}
			candidate.diffuse[0] = r;
			candidate.diffuse[1] = g;
			candidate.diffuse[2] = b;
			candidate.aboveHorizon = candidate.dir[2] > kMinSunElevation;
		}

		// Only channel 0 is the sun. VCS lights the things that move with its time-of-day directional
		// on channel 0 and adds the street lamps near each object as extra directionals on channels 1
		// to 3, each pointing from its lamp to that object: full white, a different direction for every
		// object, sweeping as you ride past. Picked by brightness they beat a dim moon every time, and
		// every shadow on screen swung towards whichever lamp was nearest. Measured on a night ride:
		// 35 jumps in 20 seconds, every one to channel 1 or 2, while channel 0 held the setting sun
		// (moving 2.7 degrees at a time) and then the moon. Still recorded above, for the panel.
		// LCS: and not even channel 0 when the game's own sun is known (NoteGameSun).
		if (i != 0 || s_sunFromGame) {
			continue;
		}

		float unit[3] = { dir[0] * invLength, dir[1] * invLength, dir[2] * invLength };

		// An axis-aligned channel at full white is what a channel holds when nobody has set it,
		// not a light. This used to be caught by the horizon test, which cannot stay - see below -
		// so it is named directly: no real solar or lunar direction lands on an axis.
		if (fabsf(unit[0]) > 0.999f || fabsf(unit[1]) > 0.999f || fabsf(unit[2]) > 0.999f) {
			continue;
		}

		// Z is up here, and after dark the game's own light is below the horizon - which is
		// correct for a sun and casts nothing. Refusing it meant no shadows at all for half of
		// every day, which is a worse answer than the approximate one: mirror it back above the
		// horizon, where the moon is, and light the night from there at a fraction of the
		// strength. A light that is merely low keeps its own direction.
		bool moon = false;
		if (unit[2] <= kMinSunElevation) {
			moon = true;
			unit[2] = fabsf(unit[2]);
			if (unit[2] < 0.35f) {
				// Nearly level, so mirroring alone leaves it grazing and every shadow a mile
				// long. Lift it to something a moon could plausibly be at and renormalise.
				unit[2] = 0.35f;
				const float xy = sqrtf(unit[0] * unit[0] + unit[1] * unit[1]);
				const float want = sqrtf(1.0f - unit[2] * unit[2]);
				if (xy > 1e-6f) {
					unit[0] *= want / xy;
					unit[1] *= want / xy;
				} else {
					unit[0] = want;
				}
			}
		}

		s_sunLuminance = luminance;
		s_current.sunValid = true;
		s_current.sunIsMoon = moon;
		s_current.sunChannel = i;
		s_current.sunDir[0] = unit[0];
		s_current.sunDir[1] = unit[1];
		s_current.sunDir[2] = unit[2];
		s_current.sunDiffuse[0] = r;
		s_current.sunDiffuse[1] = g;
		s_current.sunDiffuse[2] = b;
	}

	if (anyDirectional) {
		s_current.dirLightDraws++;
	}

	if (s_current.sunValid) {
		// The eased direction once there is one, so faces are chosen against the same light the map
		// is projected along - see TurnTowardsSun.
		const float *sun = s_haveSmoothSun ? s_smoothSunDir : s_current.sunDir;
		s_captureLightDir[0] = -sun[0];
		s_captureLightDir[1] = -sun[1];
		s_captureLightDir[2] = -sun[2];
		s_haveCaptureLightDir = true;
	}
}

// Does the texture this draw is using cut its own shape out?
//
// This exists because the obvious source for the answer is wrong here. PPSSPP tracks it as
// `gstate_c.textureSolidAlpha`, set from the decoded texture's alpha - and it is only ever set on
// the path that decodes the PSP's texture. With texture replacement on, which is this port's
// whole point, the replaced path leaves it at TextureAlpha::Any and every texture in the game
// reads as "might have holes". Believing it threw out the entire city: 0 casters, 58 of 58
// blendable draws rejected, and no shadows at all.
//
// So ask the game's own texture instead of the pack's. That is also the more correct question -
// whether a palm leaf is a cut-out is a fact about the game, not about which pack is installed -
// and the answer is cached per texture, so it costs one scan each and a hash lookup per draw.
namespace {

struct TexAlphaKey {
	u32 addr;
	u32 clutAddr;
	u32 shape;   // format, bufw and height packed - two textures at one address differ by these
};

// A texture is a cut-out if a real share of it is transparent. A threshold rather than "any
// non-opaque texel", because an antialiased edge or a stray pixel is not a hole, and a leaf card
// is mostly hole.
static const float kCutoutFraction = 0.02f;

static std::unordered_map<u64, bool> s_texCutout;

// New textures arrive in bursts as the streamer works, and scanning a 256x256 32-bit texture is a
// quarter of a megabyte of reads. Doing every new one the frame it appears is a stall you can feel
// while driving. Two a frame gets through the burst in well under a second, and a texture that has
// not been scanned yet is treated as solid - so the worst a delay costs is one frame of a leaf
// quad casting its box, rather than a frame that arrives late.
static const int kTexScansPerFrame = 2;

inline bool PaletteEntryIsClear(const u8 *clut, u32 index, GEPaletteFormat fmt) {
	switch (fmt) {
	case GE_CMODE_16BIT_ABGR5551:
		return (((const u16 *)clut)[index] & 0x8000) == 0;
	case GE_CMODE_16BIT_ABGR4444:
		return (((const u16 *)clut)[index] >> 12) < 0x8;
	case GE_CMODE_32BIT_ABGR8888:
		return (((const u32 *)clut)[index] >> 24) < 0x80;
	default:
		return false;   // 5650 has no alpha at all
	}
}

}  // namespace

// A cheap fingerprint of what a texture holds: 32 bytes sampled across its level 0, and the start of
// its palette.
//
// The scan cache used to be keyed on where a texture is and its shape alone, and that is not an
// identity in this game. The streamer frees textures and loads others at the same address, often at
// the same size and format, and a verdict keyed on the address then answers for whatever lived
// there before - a palm's fronds inheriting a wall's "solid" for the rest of the session.
static u64 TextureContentFingerprint(u32 addr, GETextureFormat fmt, int bufw, int h, u32 clutAddr) {
	int bitsPerTexel;
	switch (fmt) {
	case GE_TFMT_4444:
	case GE_TFMT_5551:
	case GE_TFMT_5650:
	case GE_TFMT_CLUT16:
		bitsPerTexel = 16;
		break;
	case GE_TFMT_8888:
	case GE_TFMT_CLUT32:
		bitsPerTexel = 32;
		break;
	case GE_TFMT_CLUT8:
		bitsPerTexel = 8;
		break;
	case GE_TFMT_CLUT4:
		bitsPerTexel = 4;
		break;
	default:
		return 0;
	}
	const u64 bytes = (u64)bufw * (u64)h * (u64)bitsPerTexel / 8;
	u64 hash = 1469598103934665603ULL;
	if (bytes >= 32 && bytes < 0x01000000 && Memory::IsValidRange(addr, (u32)bytes)) {
		const u8 *p = Memory::GetPointerUnchecked(addr);
		for (u64 i = 0; i < 32; i++) {
			hash ^= p[bytes * i / 32];
			hash *= 1099511628211ULL;
		}
	}
	if (clutAddr && Memory::IsValidRange(clutAddr, 64)) {
		const u8 *c = Memory::GetPointerUnchecked(clutAddr);
		for (int i = 0; i < 64; i++) {
			hash ^= c[i];
			hash *= 1099511628211ULL;
		}
	}
	return hash * 0x9E3779B97F4A7C15ULL;
}

// True when the texture is opaque enough to be believed as a solid caster. Unknown formats answer
// true, so a format nobody here has thought about keeps the behaviour the fork already had rather
// than silently deleting shadows.
static bool TextureIsSolid() {
	if (!gstate.isTextureMapEnabled()) {
		return true;
	}
	const u32 addr = gstate.getTextureAddress(0);
	if (!addr) {
		return true;
	}
	const GETextureFormat fmt = gstate.getTextureFormat();
	const int bufw = (int)GetTextureBufw(0, addr, fmt);
	const int h = gstate.getTextureHeight(0);
	if (bufw <= 0 || h <= 0) {
		return true;
	}

	const u32 clutAddr = (fmt >= GE_TFMT_CLUT4 && fmt <= GE_TFMT_CLUT32) ? gstate.getClutAddress() : 0;
	const u32 shape = ((u32)fmt << 28) ^ ((u32)bufw << 14) ^ (u32)h;
	const u64 key = ((u64)addr << 32) ^ ((u64)clutAddr << 8) ^ shape ^
		TextureContentFingerprint(addr, fmt, bufw, h, clutAddr);

	auto cached = s_texCutout.find(key);
	if (cached != s_texCutout.end()) {
		return !cached->second;
	}
	if (s_texScansThisFrame >= kTexScansPerFrame) {
		return true;
	}
	s_texScansThisFrame++;

	const int texels = bufw * h;
	int clear = 0;
	bool known = true;

	switch (fmt) {
	case GE_TFMT_5650:
		break;   // no alpha channel at all
	case GE_TFMT_5551:
	case GE_TFMT_4444: {
		if (!Memory::IsValidRange(addr, texels * 2)) { known = false; break; }
		const u16 *p = (const u16 *)Memory::GetPointerUnchecked(addr);
		if (fmt == GE_TFMT_5551) {
			for (int i = 0; i < texels; i++) { if (!(p[i] & 0x8000)) clear++; }
		} else {
			for (int i = 0; i < texels; i++) { if ((p[i] >> 12) < 0x8) clear++; }
		}
		break;
	}
	case GE_TFMT_8888: {
		if (!Memory::IsValidRange(addr, texels * 4)) { known = false; break; }
		const u32 *p = (const u32 *)Memory::GetPointerUnchecked(addr);
		for (int i = 0; i < texels; i++) { if ((p[i] >> 24) < 0x80) clear++; }
		break;
	}
	case GE_TFMT_CLUT4:
	case GE_TFMT_CLUT8: {
		// The indices decide which palette entries matter, so scan them rather than scanning the
		// palette: one transparent colour nobody uses is not a hole.
		const int bytes = fmt == GE_TFMT_CLUT4 ? texels / 2 : texels;
		if (!clutAddr || !Memory::IsValidRange(addr, bytes) ||
			!Memory::IsValidRange(clutAddr, 1024)) { known = false; break; }
		const u8 *p = Memory::GetPointerUnchecked(addr);
		const u8 *clut = Memory::GetPointerUnchecked(clutAddr);
		const GEPaletteFormat clutFmt = gstate.getClutPaletteFormat();
		const u32 start = gstate.getClutIndexStartPos();
		const u32 shift = gstate.getClutIndexShift();
		const u32 mask = gstate.getClutIndexMask();
		const u32 entries = clutFmt == GE_CMODE_32BIT_ABGR8888 ? 256u : 512u;
		for (int i = 0; i < texels; i++) {
			u32 index = fmt == GE_TFMT_CLUT4
				? ((p[i >> 1] >> ((i & 1) * 4)) & 0xF)
				: p[i];
			index = ((index >> shift) & mask) | start;
			if (index >= entries) { continue; }
			if (PaletteEntryIsClear(clut, index, clutFmt)) { clear++; }
		}
		break;
	}
	default:
		known = false;
		break;
	}

	const bool cutout = known && texels > 0 && (float)clear > kCutoutFraction * (float)texels;
	// Emptied when full rather than frozen. It used to stop storing at 4096, and a streaming city
	// passes that in a long session - after which every new texture was scanned again whenever the
	// budget allowed and read as solid in between.
	if (s_texCutout.size() >= 8192) {
		s_texCutout.clear();
	}
	s_texCutout[key] = cutout;
	return !cutout;
}

// Whether this draw's fragments are fully opaque - BOTH halves of it.
//
// The vertex colour is only half the answer, and the half that was being asked. The final alpha
// is vertex alpha times texture alpha, and PPSSPP's texture cache already knows the second half:
// `textureSolidAlpha` is set from the bound texture's own alpha status. GPUStateUtils asks the
// question in exactly this form when it decides whether a blend can be simplified away.
//
// This is what makes a palm cast a palm instead of a box. A leaf quad has fully opaque vertex
// colours over a texture that is mostly holes, so on vertex alpha alone it reads as solid and the
// depth pass writes the whole rectangle. The plain depth pass carries no textures, so the honest
// answer is to stop calling the quad a solid caster - TestDraw sends it to the cut-out pass instead.
static bool FinalAlphaIsOpaque() {
	if (!gstate_c.vertexFullAlpha) {
		return false;
	}
	return !gstate.isTextureAlphaUsed() || TextureIsSolid();
}

// Whether the current blend setup can actually change what is already in the framebuffer.
//
// This is the whole lesson of the first run of the counters. Testing gstate.isAlphaBlendEnabled()
// threw out 170 of 241 draws and left zero casters - the entire city. Bucketing them showed one
// setup, srcalpha/invsrcalpha/add, on 170 draws whose vertex colours were fully opaque all 170
// times. That expands to src*1 + dst*0: the game switches blending on for the opaque pass and
// specifies a blend that does nothing. Asking "is blending enabled" is the wrong question;
// asking "can this blend alter the destination" is the right one.
//
// This asked only about VERTEX alpha for a long time, and the note here used to say that erring
// towards calling a draw opaque was the cheap direction to be wrong in. It is not: it is what put
// solid rectangles in the sky where palm leaves are. See FinalAlphaIsOpaque.
static bool BlendAltersDestination() {
	// Anything other than a straight multiply-and-add is doing something deliberate.
	if (gstate.getBlendEq() != GE_BLENDMODE_MUL_AND_ADD) {
		return true;
	}

	const GEBlendSrcFactor funcA = gstate.getBlendFuncA();
	const GEBlendDstFactor funcB = gstate.getBlendFuncB();

	// Fixed factors: src*fixA + dst*fixB. White over black is a copy.
	if (funcA == GE_SRCBLEND_FIXA && funcB == GE_DSTBLEND_FIXB) {
		return !(gstate.getFixA() == 0xFFFFFF && gstate.getFixB() == 0x000000);
	}

	// The standard alpha blend, which is a copy whenever the source is fully opaque.
	if (funcA == GE_SRCBLEND_SRCALPHA && funcB == GE_DSTBLEND_INVSRCALPHA) {
		return !FinalAlphaIsOpaque();
	}

	return true;
}

// The caster filter. There is no "this is a car" bit in a display list, so everything here is
// render state, and the order matters: the first failure wins and gets the blame in the counts.
static Reject TestDraw(GEPrimitiveType prim, int vertexCount) {
	// Through mode is the 2D path - the HUD, the radar, menus. The vertices are already in
	// screen space, so there is no world transform to project from a light at all.
	if (gstate.isModeThrough()) {
		return Reject::Through;
	}

	// Not the frame the player is looking at. A render-to-texture pass has its own camera, and
	// mixing its geometry into the capture would project the frame's shadows from two of them.
	if (gstate_c.curRTWidth < 480 || gstate_c.curRTHeight < 272) {
		return Reject::OffscreenTarget;
	}

	switch (prim) {
	case GE_PRIM_TRIANGLES:
	case GE_PRIM_TRIANGLE_STRIP:
	case GE_PRIM_TRIANGLE_FAN:
		break;
	default:
		// Lines, points and sprites. Sprites are the interesting exclusion: they are how the
		// game draws billboards, and a camera-facing quad casts a shadow that swings as you
		// turn, which reads worse than no shadow at all.
		return Reject::Primitive;
	}

	// Depth write off means the game itself considers the draw not part of the solid scene.
	// That catches particles, coronas, and - usefully - the vanilla blob shadow, which has to
	// go anyway or every ped ends up with two shadows.
	if (!gstate.isDepthWriteEnabled()) {
		return Reject::NoDepthWrite;
	}

	// A cut-out drawn with a BLEND rather than the alpha test. The standard alpha blend over opaque
	// vertices blends only through the texture's alpha, and a texture that is mostly hole is a card
	// with a shape cut out of it - which is how palm fronds reach this filter. Filed as glass, they
	// cast nothing; filed as what they are, they cast through the texture.
	if (gstate.isAlphaBlendEnabled() && gstate.getBlendEq() == GE_BLENDMODE_MUL_AND_ADD &&
		gstate.getBlendFuncA() == GE_SRCBLEND_SRCALPHA && gstate.getBlendFuncB() == GE_DSTBLEND_INVSRCALPHA &&
		gstate_c.vertexFullAlpha && gstate.isTextureMapEnabled() && gstate.isTextureAlphaUsed() &&
		!TextureIsSolid()) {
		return Reject::Cutout;
	}

	// Blending that genuinely composites catches glass, smoke and water. A blend that cannot
	// change the destination is not transparency at all - see BlendAltersDestination, which
	// is where the entire city was being thrown away.
	if (gstate.isAlphaBlendEnabled() && BlendAltersDestination()) {
		return Reject::Blended;
	}

	// And the same question again for the draws that cut their shape out with the alpha TEST
	// rather than with a blend - foliage and chain-link. The plain depth pass carries no textures,
	// so it would write the rectangle; a cut-out is sent to the textured pass instead - see
	// AddCutoutCaster - which casts the shape the texture cuts.
	if (gstate.isAlphaTestEnabled() && gstate.getAlphaTestFunction() != GE_COMP_ALWAYS &&
		!FinalAlphaIsOpaque()) {
		// ... with one exception, and it is the reasoning above rather than a hole in it. "It
		// would write the rectangle" is only damning when the geometry IS a rectangle. A palm
		// frond is a flat card whose shape lives entirely in the texture; a motorcycle wheel is a
		// disc of real geometry that happens to cut its spokes out with the alpha test, and its
		// silhouette is very nearly what the depth pass would write anyway.
		//
		// The discriminator is the one this game answers honestly: vertex NORMALS. Scenery ships
		// prelit and carries none, so palms and chain-link are unaffected; a wheel belongs to a
		// vehicle and carries them. Same fact the caster mode rests on, used the other way round.
		if (!s_settings.cutoutEntitiesCast ||
			(gstate.vertType & GE_VTYPE_NRM_MASK) == GE_VTYPE_NRM_NONE) {
			return Reject::Cutout;
		}
	}

	if (vertexCount < 3) {
		return Reject::TooFewVerts;
	}

	return Reject::None;
}

// Records what the blend setup actually was on a draw the filter threw out for blending, so the
// question "is that really transparency?" gets answered with the equation rather than a guess.
static void NoteBlend(int vertexCount) {
	if (vertexCount >= 3) {
		s_current.castersIfBlendIgnored++;
	}

	const u8 funcA = (u8)gstate.getBlendFuncA();
	const u8 funcB = (u8)gstate.getBlendFuncB();
	const u8 eq = (u8)gstate.getBlendEq();

	for (int i = 0; i < s_current.blendBucketCount; i++) {
		BlendBucket &bucket = s_current.blendBuckets[i];
		if (bucket.funcA == funcA && bucket.funcB == funcB && bucket.eq == eq) {
			bucket.count++;
			if (gstate_c.vertexFullAlpha) {
				bucket.fullAlpha++;
			}
			return;
		}
	}

	// More than kMaxBlendBuckets distinct setups in one frame would mean the game is doing
	// something far more varied than expected, and the tail is not worth growing the struct for.
	if (s_current.blendBucketCount >= kMaxBlendBuckets) {
		return;
	}
	BlendBucket &bucket = s_current.blendBuckets[s_current.blendBucketCount++];
	bucket.funcA = funcA;
	bucket.funcB = funcB;
	bucket.eq = eq;
	bucket.count = 1;
	bucket.fullAlpha = gstate_c.vertexFullAlpha ? 1 : 0;
}

// The game drew another frame after this one's passes had already run, inside the same host frame.
//
// The passes run once, at the first 2D draw after the 3D - and when the game gets two frames into one
// host frame, the second frame's whole scene arrives after them. Measured in play on the frames that
// flickered: 1168 casters after the passes against 1159 before, every person and vehicle among them.
// The second frame paints over the first and nothing had captured it, so what reached the screen was a
// frame with no shadow under anything that moves - which on MEDIUM, with nothing remembered to fall
// back on, is every shadow there is. So a caster arriving after the passes starts a new capture, and the
// next seam runs the passes again for the frame that is actually shown.
//
// A second log settled what those second frames are: the same scene drawn again into the SAME
// framebuffer, with draw, caster and receiver counts within a few of the first - not one frame split
// in two by a 2D draw. So nothing captured before the re-capture is needed after it.
static int s_captureRestarts;

static void RestartCapture() {
	if (!s_recaptureThisFrame) {
		s_recaptureThisFrame = true;
		s_firstCasterIndices = s_capture.casterIndices;
	}
	memset(&s_capture, 0, sizeof(s_capture));
	s_positions.clear();
	s_casterIndices.clear();
	s_receiverIndices.clear();
	s_pedReceiverIndices.clear();
	s_batches.clear();
	for (CutoutGroup &group : s_cutGroups) {
		group.verts.clear();
		group.idx.clear();
	}
	s_cutPending = false;
	memset(&s_current, 0, sizeof(s_current));
	s_sunLuminance = 0.0f;
	NoteGameSun();
	s_haveViewMatrix = false;
	s_frameComposited = false;
	s_frameIndex++;
	s_objectBounds.clear();
	s_capture.blobTextures = s_blobTextureCount;
	s_captureRestarts++;
	if (s_captureRestarts <= 3 || s_captureRestarts % 1000 == 0) {
		NOTICE_LOG(Log::G3D, "LCS shadows: the game drew a second frame after the passes had run - capturing it again (%d so far)",
			s_captureRestarts);
	}
}

Reject ClassifyDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount) {
	s_current.draws++;
	NoteLights();
	// A cut-out baked by the previous flush and never committed must not pick up this one's texture.
	s_cutPending = false;

	const Reject reject = TestDraw(prim, vertexCount);
	if (reject == Reject::None && s_frameComposited) {
		RestartCapture();
		s_current.draws++;
		NoteLights();
	}
	if (reject != Reject::None) {
		s_current.rejected[(int)reject]++;
		if (reject == Reject::Blended) {
			NoteBlend(vertexCount);
		}
		return reject;
	}

	s_current.casters++;
	s_current.castersIfBlendIgnored++;

	// The first caster of the frame is the one to take the camera from: by the last draw the
	// game has usually moved on to 2D and left a matrix behind that means nothing here.
	if (!s_haveViewMatrix) {
		memcpy(s_frameViewMatrix, gstate.viewMatrix, sizeof(s_frameViewMatrix));
		memcpy(s_frameProjMatrix, gstate.projMatrix, sizeof(s_frameProjMatrix));
		// The same widening the shader upload applies, or this pass's screen-space
		// mask is built from a projection the frame was not drawn with and slides across
		// the picture. See the Widescreen section in Core/LCS/LCSGame.h.
		// LCS: no widescreen projection to match (VCS widened it here).
		s_frameViewInvValid = Invert3x3(s_frameViewMatrix, s_frameViewInvR);
		s_frameViewport.scale[0] = gstate.getViewportXScale();
		s_frameViewport.scale[1] = gstate.getViewportYScale();
		s_frameViewport.scale[2] = gstate.getViewportZScale();
		s_frameViewport.offset[0] = gstate.getViewportXCenter();
		s_frameViewport.offset[1] = gstate.getViewportYCenter();
		s_frameViewport.offset[2] = gstate.getViewportZCenter();
		s_frameViewport.rasterOffset[0] = gstate.getOffsetX();
		s_frameViewport.rasterOffset[1] = gstate.getOffsetY();
		s_frameViewport.rtWidth = (float)gstate_c.curRTWidth;
		s_frameViewport.rtHeight = (float)gstate_c.curRTHeight;
		s_frameViewport.rtOffsetX = gstate_c.curRTOffsetX;
		s_frameViewport.rtOffsetY = gstate_c.curRTOffsetY;
		{
			const float *m = s_frameViewMatrix;
			const float t[3] = { m[9], m[10], m[11] };
			s_frameCameraPos[0] = -(t[0] * m[0] + t[1] * m[1] + t[2] * m[2]);
			s_frameCameraPos[1] = -(t[0] * m[3] + t[1] * m[4] + t[2] * m[5]);
			s_frameCameraPos[2] = -(t[0] * m[6] + t[1] * m[7] + t[2] * m[8]);
			// Column 2 of the rotation is the world direction of the view's z axis; which end of
			// it points forward is the same question forwardIsNegativeZ already answers for the
			// cascade, so it is asked the same way here.
			const float sign = s_settings.forwardIsNegativeZ ? -1.0f : 1.0f;
			s_frameCameraFwd[0] = m[2] * sign;
			s_frameCameraFwd[1] = m[5] * sign;
			s_frameCameraFwd[2] = m[8] * sign;
		}
		s_haveViewMatrix = true;

		// The cache holds positions in the space the GE is fed, and the game rebases that space as
		// you travel. A rebase, a teleport and a load all show up here as the recovered camera
		// jumping further in one frame than any camera can move - and only the game's own camera,
		// read in world space on this same frame, tells them apart: across a rebase it stays put.
		const std::optional<float> worldX = LCS::CameraWorld(0);
		const std::optional<float> worldY = LCS::CameraWorld(1);
		const bool haveWorld = worldX.has_value() && worldY.has_value();
		if (s_haveLastCameraPos) {
			float moved = 0.0f;
			for (int c = 0; c < 3; c++) {
				const float d = s_frameCameraPos[c] - s_lastCameraPos[c];
				moved += d * d;
			}
			bool rebased = false;
			if (moved > 40.0f * 40.0f && haveWorld && s_haveLastCameraWorld) {
				const float worldDX = *worldX - s_lastCameraWorld[0];
				const float worldDY = *worldY - s_lastCameraWorld[1];
				const float geDZ = s_frameCameraPos[2] - s_lastCameraPos[2];
				if (worldDX * worldDX + worldDY * worldDY < 20.0f * 20.0f && fabsf(geDZ) < 20.0f) {
					const float shift[2] = {
						(s_frameCameraPos[0] - s_lastCameraPos[0]) - worldDX,
						(s_frameCameraPos[1] - s_lastCameraPos[1]) - worldDY,
					};
					RebaseCachedCasters(shift);
					rebased = true;
					if (++s_cacheRebases <= 20) {
						NOTICE_LOG(Log::G3D, "LCS shadows: the GE space rebased by %.1f %.1f - %d remembered cells moved with it",
							shift[0], shift[1], (int)s_buckets.size());
					}
				}
			}
			if (moved > 40.0f * 40.0f && !rebased) {
				if (++s_cacheClears <= 20) {
					NOTICE_LOG(Log::G3D, "LCS shadows: the camera jumped %.0f units - remembered shadows cleared",
						sqrtf(moved));
				}
				s_buckets.clear();
				// And forget which textures were blob shadows, which is the same event
				// seen from the other side. The learner keys on the texture's ADDRESS,
				// and a load puts the same artwork somewhere else - so every learned
				// entry becomes a stale address that matches nothing, while the table
				// stays full and refuses to learn the new ones. Sixteen slots of rubbish
				// and the vanilla blob under every ped and car, permanently, which is
				// exactly how it was reported: fine until you load a save.
				s_blobTextureCount = 0;
				s_blobCandidateCount = 0;
			}
		}
		memcpy(s_lastCameraPos, s_frameCameraPos, sizeof(s_lastCameraPos));
		s_haveLastCameraPos = true;
		if (haveWorld) {
			s_lastCameraWorld[0] = *worldX;
			s_lastCameraWorld[1] = *worldY;
		}
		s_haveLastCameraWorld = haveWorld;
	} else if (memcmp(s_frameViewMatrix, gstate.viewMatrix, sizeof(s_frameViewMatrix)) != 0) {
		s_current.viewMatrixChanges++;
	}

	// The world matrix translation is where this draw's geometry sits in whatever space the GE
	// is being fed. Bounding it over the frame says what that space actually is.
	const float origin[3] = { gstate.worldMatrix[9], gstate.worldMatrix[10], gstate.worldMatrix[11] };
	if (!s_current.casterBoundsValid) {
		s_current.casterBoundsValid = true;
		for (int i = 0; i < 3; i++) {
			s_current.casterMin[i] = origin[i];
			s_current.casterMax[i] = origin[i];
		}
	} else {
		for (int i = 0; i < 3; i++) {
			if (origin[i] < s_current.casterMin[i]) s_current.casterMin[i] = origin[i];
			if (origin[i] > s_current.casterMax[i]) s_current.casterMax[i] = origin[i];
		}
	}
	s_current.casterVerts += vertexCount;
	if ((vertTypeID & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE) {
		s_current.castersSkinned++;
	}
	if ((vertTypeID & GE_VTYPE_NRM_MASK) != GE_VTYPE_NRM_NONE) {
		s_current.castersWithNormals++;
	}
	return Reject::None;
}

const FrameStats &LastFrameStats() {
	return s_published;
}

const char *BlendSrcName(u8 funcA) {
	static const char *kNames[] = {
		"dstcolor", "invdstcolor", "srcalpha", "invsrcalpha", "dstalpha", "invdstalpha",
		"2*srcalpha", "2*invsrcalpha", "2*dstalpha", "2*invdstalpha", "fixA",
	};
	return funcA < (u8)ARRAY_SIZE(kNames) ? kNames[funcA] : "?";
}

const char *BlendDstName(u8 funcB) {
	static const char *kNames[] = {
		"srccolor", "invsrccolor", "srcalpha", "invsrcalpha", "dstalpha", "invdstalpha",
		"2*srcalpha", "2*invsrcalpha", "2*dstalpha", "2*invdstalpha", "fixB",
	};
	return funcB < (u8)ARRAY_SIZE(kNames) ? kNames[funcB] : "?";
}

const char *BlendEqName(u8 eq) {
	static const char *kNames[] = { "add", "sub", "revsub", "min", "max", "abs" };
	return eq < (u8)ARRAY_SIZE(kNames) ? kNames[eq] : "?";
}

const char *RejectName(Reject r) {
	switch (r) {
	case Reject::None: return "caster";
	case Reject::Through: return "2D / through";
	case Reject::OffscreenTarget: return "off-screen target";
	case Reject::Primitive: return "not a triangle";
	case Reject::NoDepthWrite: return "no depth write";
	case Reject::Blended: return "alpha blended";
	case Reject::Cutout: return "alpha-tested cutout";
	case Reject::TooFewVerts: return "too few verts";
	default: return "?";
	}
}

}  // namespace LCSShadow
