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

// LCS: ported from the VCS fork's GPU/Common/VCSWater (its docs/vcs/water.md has the design and every
// measurement). The notes below are VCS's, made on Vice City Stories, unless they say LCS. What the
// passes read of the game goes through Core/LCS: the weather, the camera and player positions, and
// the road network (LCSRoute, the GPS route's copy of ThePaths). docs/lcs/shadows-and-water.md has
// what was measured on Liberty City Stories.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/ShaderWriter.h"
#include "Core/System.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSRoute.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/LCS/LCSState.h"
#include "GPU/GPUState.h"
#include "GPU/Common/LCSWater.h"
#include "GPU/Common/LCSShadow.h"

namespace LCSWater {

bool g_available;
bool g_active;

static bool s_enabled = true;

// VCS world space is Z-up. Not inferred from anything here - CWorld::ProcessVerticalLine answers
// "what is the height at this point" by writing z, verified against the running game long before
// this file existed. See the conventions note in LCSShadow.h.
static const int kUpAxis = 2;

static Settings s_settings = {
	true,      // water
	0.55f,     // waveScale - periods per world unit
	0.13f,     // waveAmplitude
	1.30f,     // waveSpeed
	260.0f,    // detailFade
	0.30f,     // deepMix
	{ 0.016f, 0.075f, 0.105f },   // deepColour
	0.02f,     // fresnelF0
	// Measured over 391 samples of the frame's own water, the flat-water Fresnel has a median of
	// 0.68 and 74.4% of the visible sea sits above 0.5 - correct physics for a low camera over
	// water, and also why it read as a mirror. The fix for THAT is the blur below, not the
	// strength: a single tap is a sheet of glass whatever you scale it by. Cutting the strength to
	// 0.45 as well turned out to be an over-correction - it took the median reflection to 0.22,
	// and over open sea the reflection is of the SKY, which is nearly the water's own colour, so
	// at that weight the whole term is a no-op and the sea came back looking like the PSP's.
	0.70f,     // reflectionStrength
	0.035f,    // reflectionSpread
	1.00f,     // mirrorScale
	0.030f,    // reflectionBlur
	0.62f,     // maxReflection

	// Wider and stronger than a mirror's highlight, because the glitter path is the single
	// strongest cue that a distant surface is water - and at 120 the lobe was narrow enough that
	// it only existed when the sun happened to be dead ahead.
	55.0f,     // specularPower
	1.10f,     // specularStrength

	6.0f,      // extinctionDepth - units along the view ray to fully deep colour
	0.85f,     // shoreFoam
	0.55f,     // foamDepth - units of water under the foam band
	0.085f,    // normalMapScale - repeats per world unit, so one tile is about twelve units
	0.30f,     // normalMapStrength

	true,      // wetRoads
	4.0f,      // roadHalfWidth
	512.0f,    // roadMapSpan
	-1.0f,     // rainOverride - the game's own value
	30.0f,     // wetSeconds
	90.0f,     // drySeconds
	24.0f,     // drainPatchSize
	// 0.22, from Tools-side rendering of this exact formula at six wetness levels rather than by
	// eye. At 0.09 the wet broke into runs with a median length of 25 metres, which is a stripe;
	// at 0.22 the median is 6.7 metres half-dry and 1.9 metres nearly dry, which is a puddle.
	0.22f,     // puddleScale
	0.70f,     // wetDarkening
	// Impacts per world unit, so the reciprocal is the cell a droplet lands in and the ring grows
	// to a fraction of that. At 0.55 the cell was 1.8 metres and a ring reached 1.6 metres of
	// RADIUS - reported, accurately, as "like pebbles were raining". At 2.9 the cell is 34cm and a
	// ring reaches about 15cm, which is a raindrop.
	2.90f,     // rippleScale - impacts per world unit
	0.9f,      // rippleRate
	0.45f,     // rippleStrength
	0.60f,     // rippleLight - a ring crest's own brightness, as a share of the surface it sits on
	0.45f,     // wetReflection
	0.35f,     // wetMirrorScale
	0.25f,     // wetEdgeFade

	1.0f,      // surfaceScale
	0,         // debugView
};

Settings &GetSettings() {
	return s_settings;
}

static FrameStats s_current;
static FrameStats s_published;

const FrameStats &LastFrameStats() {
	return s_published;
}

const char *RejectName(Reject r) {
	switch (r) {
	case Reject::None: return "captured";
	case Reject::Through: return "2D (through mode)";
	case Reject::OffscreenTarget: return "off-screen target";
	case Reject::Primitive: return "not triangles";
	case Reject::NoDepthWrite: return "writes no depth";
	case Reject::Blended: return "genuinely blended";
	case Reject::NearCamera: return "sits on the camera";
	case Reject::TooFewVerts: return "degenerate";
	default: return "?";
	}
}

// ---------------------------------------------------------------------------------------------
// The frame's camera
//
// All of this is deliberately a second copy of what LCSShadow does rather than a shared helper.
// The two features are independent by design - the menu switches them separately and a player
// may want either without the other - and the alternative was to refactor a module that had only
// just finished a long debugging campaign. If one of these is ever changed, check the other.
// ---------------------------------------------------------------------------------------------

struct ViewportCapture {
	float scale[3];
	float offset[3];
	float rasterOffset[2];
	float rtWidth, rtHeight;
	int rtOffsetX, rtOffsetY;
};

static float s_frameViewMatrix[12];
static float s_frameProjMatrix[16];
static ViewportCapture s_frameViewport;
static bool s_haveViewMatrix;
static float s_frameCameraPos[3];
static float s_frameCameraForward[3];
static float s_lastCameraPos[3];
static bool s_haveLastCameraPos;
// The game's own camera in world space, as of the last frame's camera. It is what tells a rebase of
// the GE space from a real jump - see NoteDraw.
static float s_lastCameraWorld[2];
static bool s_haveLastCameraWorld;

// world = GE + s_geOffset. Zero until the camera has been read in both spaces once.
static float s_geOffset[2];
static bool s_geOffsetValid;

// The largest CWeather::Rain the game ever produces.
//
// MEASURED, not assumed, and it is not 1.0: forcing each weather id in turn over the WebSocket
// debugger and reading Rain back gives 0.500 for ids 2 and 5 and 0.000 for every other id from 0
// to 9. Everything here works in a normalised 0..1, so the raw value is divided by this - without
// which the whole feature ran at half strength in real weather and nobody could see why.
//
// The same run answered a second question: Rain follows a forced weather WITHIN HALF A SECOND,
// with no ramp at all. That is what makes the step detection below both possible and necessary.
static const float kRainFullScale = 0.5f;

// The lagged wetness the puddles follow, against the live rain that drives the droplets. The gap
// between these two is the whole feature: roads stay wet after the rain stops, and the rings stop
// the moment it does.
static float s_wetness;
static float s_rainNorm;
static float s_rippleRain;
static float s_lastRainNorm;
static bool s_haveLastRain;
static bool s_snapPending;
static double s_lastWetnessTime;

void SoakNow() {
	s_wetness = 1.0f;
}

void DryNow() {
	s_wetness = 0.0f;
}

// World to the pixel the game drew, and where the horizon lands in that picture.
static float s_cameraViewProj[16];
static bool s_viewValid;
static float s_horizonUV;

static void Mul4x4(const float a[16], const float b[16], float out[16]) {
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

static void Transpose4x4(const float in[16], float out[16]) {
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			out[col * 4 + row] = in[row * 4 + col];
		}
	}
}

// The rest of PPSSPP's vertex path as one matrix. Every step between the projection and
// gl_Position is linear in (x, y, z, w), because the perspective divide the viewport transform
// needs is undone again by the multiply back into clip space at the end of it.
//
// Depth is NOT copied faithfully, and is inverted here when the game asks for a reversed buffer:
// VCS runs one (measured z scale -32755.5 against offset +32763.5). This pass owns its own depth
// buffer and tests LESS against it, so copying the mapping verbatim would make the farthest
// surface win every pixel - and then the sea would be shaded on top of the boat in front of it.
static void BuildPostProjMatrix(const ViewportCapture &vp, float out[16]) {
	memset(out, 0, sizeof(float) * 16);
	out[15] = 1.0f;
	if (vp.rtWidth <= 0.0f || vp.rtHeight <= 0.0f) {
		out[0] = out[5] = out[10] = 1.0f;
		return;
	}
	const float invW = 2.0f / vp.rtWidth;
	const float invH = 2.0f / vp.rtHeight;
	const float invZ = 1.0f / 65536.0f;

	float scaleZ = vp.scale[2] * invZ;
	float offsetZ = vp.offset[2] * invZ;
	if (scaleZ < 0.0f) {
		scaleZ = -scaleZ;
		offsetZ = 1.0f - offsetZ;
	}

	out[0] = vp.scale[0] * invW;
	out[5] = vp.scale[1] * invH;
	out[10] = scaleZ;
	out[12] = (vp.offset[0] - vp.rasterOffset[0] + (float)vp.rtOffsetX) * invW - 1.0f;
	out[13] = (vp.offset[1] - vp.rasterOffset[1] + (float)vp.rtOffsetY) * invH - 1.0f;
	out[14] = offsetZ;
}

// Where the water plane's horizon falls on the screen, in UV.
//
// A direction has w = 0, so pushing the camera's own forward vector flattened into the water
// plane through the same matrix gives the vanishing point of every horizontal line running away
// from the camera - which is a point ON the horizon, and its height is all the reflection needs
// while the camera is not rolled.
static void ComputeHorizon() {
	s_horizonUV = 0.5f;
	float dir[3] = { s_frameCameraForward[0], s_frameCameraForward[1], 0.0f };
	const float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
	if (len < 1e-4f) {
		// Straight down. There is no horizon in the picture, so put it off the top and let the
		// reflection clamp rather than pick a number that is confidently wrong.
		s_horizonUV = 1.5f;
		return;
	}
	dir[0] /= len;
	dir[1] /= len;

	const float *m = s_cameraViewProj;
	const float y = dir[0] * m[1] + dir[1] * m[5] + dir[2] * m[9];
	const float w = dir[0] * m[3] + dir[1] * m[7] + dir[2] * m[11];
	if (fabsf(w) < 1e-6f) {
		s_horizonUV = 1.5f;
		return;
	}
	s_horizonUV = (y / w) * 0.5f + 0.5f;
}

// The translation between the space the GE is fed and the game's own world space.
//
// Derived HERE, at the seam, and not in BeginFrame - which is the whole of a reported bug. The
// puddles are a noise field sampled at (GE position + this offset), so any error in it moves the
// entire field. BeginFrame paired the camera it had just read out of PSP memory with the camera
// recovered from the PREVIOUS frame's view matrix, and at driving speed those are a frame of
// travel apart: the offset wobbled by however far the car had moved, every frame, and the puddles
// swam with it. Reported as "they flicker when driving or moving the camera".
//
// Latched on top of that. A rebase is a step, not a drift - measured, it holds to a tenth of a
// unit for many seconds and then jumps - so anything smaller than kOffsetLatch is noise and moving
// the field for it can only make puddles shimmer.
static const float kOffsetLatch = 0.35f;

static void UpdateWorldOffset() {
	if (!s_haveViewMatrix) {
		return;
	}
	const std::optional<float> cx = LCS::CameraWorld(0);
	const std::optional<float> cy = LCS::CameraWorld(1);
	if (!cx || !cy) {
		return;
	}
	const float candidate[2] = {
		*cx - s_frameCameraPos[0],
		*cy - s_frameCameraPos[1],
	};
	if (!s_geOffsetValid) {
		s_geOffset[0] = candidate[0];
		s_geOffset[1] = candidate[1];
		s_geOffsetValid = true;
		return;
	}
	const float dx = candidate[0] - s_geOffset[0];
	const float dy = candidate[1] - s_geOffset[1];
	s_current.offsetResidual = sqrtf(dx * dx + dy * dy);
	if (s_current.offsetResidual > kOffsetLatch) {
		s_geOffset[0] = candidate[0];
		s_geOffset[1] = candidate[1];
	}
}

static void ComputeView() {
	s_viewValid = false;
	if (!s_haveViewMatrix) {
		return;
	}
	const float *v = s_frameViewMatrix;
	const float view4[16] = {
		v[0], v[1], v[2], 0.0f,
		v[3], v[4], v[5], 0.0f,
		v[6], v[7], v[8], 0.0f,
		v[9], v[10], v[11], 1.0f,
	};
	float viewProj[16];
	Mul4x4(view4, s_frameProjMatrix, viewProj);
	float post[16];
	BuildPostProjMatrix(s_frameViewport, post);
	Mul4x4(viewProj, post, s_cameraViewProj);
	s_viewValid = true;
	ComputeHorizon();
}

// ---------------------------------------------------------------------------------------------
// The sun
//
// On a directional channel the GE treats lpos as a world-space vector pointing TOWARDS the light,
// which is what the specular term wants. VCS enables more than one channel and they are not all
// the sun, so the pick is the brightest one that is above the horizon - a light below it casts no
// glint on a sea surface anyway, which makes the horizon test do double duty as a filter and as
// the right physical answer.
// ---------------------------------------------------------------------------------------------

// LCS: the game's sun from its clock (LCS::ReadSun), set as the frame starts; the GE's lights carry
// none in LCS (one fixed direction at every hour - LCSShadow's NoteGameSun). Below the horizon
// there is no glint, as NoteLights would have it.
static bool s_sunFromGame = false;

static void NoteGameSun() {
	const LCS::SunState sun = LCS::ReadSun();
	s_sunFromGame = sun.valid;
	if (sun.valid && sun.dir[kUpAxis] > 0.05f) {
		s_current.sunValid = true;
		s_current.sunLuminance = sun.colour[0] * 0.299f + sun.colour[1] * 0.587f + sun.colour[2] * 0.114f;
		memcpy(s_current.sunDir, sun.dir, sizeof(sun.dir));
	}
}

static void NoteLights() {
	if (!gstate.isLightingEnabled() || s_sunFromGame) {
		return;
	}
	for (int i = 0; i < 4; i++) {
		if (!gstate.isLightChanEnabled(i)) {
			continue;
		}
		if (!gstate.isDirectionalLight(i)) {
			continue;
		}
		float dir[3] = { getFloat24(gstate.lpos[i * 3 + 0]),
			getFloat24(gstate.lpos[i * 3 + 1]),
			getFloat24(gstate.lpos[i * 3 + 2]) };
		const float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (len < 1e-4f) {
			continue;
		}
		dir[0] /= len; dir[1] /= len; dir[2] /= len;
		if (dir[kUpAxis] <= 0.05f) {
			continue;
		}
		const u32 diffuse = gstate.getDiffuseColor(i);
		const float r = (float)(diffuse & 0xFF) / 255.0f;
		const float g = (float)((diffuse >> 8) & 0xFF) / 255.0f;
		const float b = (float)((diffuse >> 16) & 0xFF) / 255.0f;
		const float lum = r * 0.299f + g * 0.587f + b * 0.114f;
		if (lum <= s_current.sunLuminance) {
			continue;
		}
		s_current.sunValid = true;
		s_current.sunLuminance = lum;
		memcpy(s_current.sunDir, dir, sizeof(dir));
	}
}

// ---------------------------------------------------------------------------------------------
// Capture
//
// Everything accepted is baked to the space the GE is fed and flattened to one triangle list, so
// the passes below are one draw call per 64k vertices rather than one per object. Two index
// streams over one vertex buffer: the sea, and everything solid.
// ---------------------------------------------------------------------------------------------

struct Batch {
	int firstVertex;
	int vertexCount;
	int firstWaterIndex;
	int waterIndexCount;
	int firstSolidIndex;
	int solidIndexCount;
};

static std::vector<float> s_positions;
static std::vector<u16> s_waterIndices;
static std::vector<u16> s_solidIndices;
static std::vector<Batch> s_batches;

// Scratch for one draw, kept between calls so the vectors stop reallocating after the first
// frame.
static std::vector<float> s_drawPos;
static std::vector<u16> s_drawIdx;

static const int kMaxBatchVerts = 60000;
static const size_t kMaxPositions = 1200000;   // 400k vertices; a frame here is under 40k

static void AppendGeometry(const float *pos, int vertCount, const u16 *idx, int idxCount,
	bool isWater) {
	if (vertCount <= 0 || idxCount < 3) {
		return;
	}
	if (s_positions.size() / 3 + (size_t)vertCount > kMaxPositions) {
		return;
	}
	// 16-bit indices cannot address past the batch, so a draw bigger than one is dropped
	// rather than silently wrapped. The PSP cannot submit one this large in practice.
	if (vertCount > kMaxBatchVerts) {
		return;
	}
	if (s_batches.empty() ||
		s_batches.back().vertexCount + vertCount > kMaxBatchVerts) {
		Batch b{};
		b.firstVertex = (int)(s_positions.size() / 3);
		b.vertexCount = 0;
		b.firstWaterIndex = (int)s_waterIndices.size();
		b.firstSolidIndex = (int)s_solidIndices.size();
		s_batches.push_back(b);
	}
	Batch &batch = s_batches.back();
	const int base = batch.vertexCount;
	s_positions.insert(s_positions.end(), pos, pos + (size_t)vertCount * 3);
	batch.vertexCount += vertCount;

	// Indices are relative to the batch, because DrawIndexedUP is handed the batch's own vertex
	// pointer and thin3d indices are 16-bit.
	std::vector<u16> &out = isWater ? s_waterIndices : s_solidIndices;
	for (int i = 0; i < idxCount; i++) {
		out.push_back((u16)(base + idx[i]));
	}
	if (isWater) {
		batch.waterIndexCount += idxCount;
	} else {
		batch.solidIndexCount += idxCount;
	}
}

// Every draw is captured in the space of the frame's FIRST 3D draw, whose view matrix is what the
// passes project with. Around a chunk swap the game draws part of a frame with the camera of the NEW
// space - LCSShadow found it, as the cause of every shadow flicker there was. Each draw is right on
// screen, because the GE puts it through its own view matrix; baked with its world matrix alone it
// lands a whole streaming cell, 125 units, away, and the wet road is shaded where it is not.
//
// Same fix as LCSShadow::PrepareViewCorrection, copied for the reason at the top of this file: a draw
// whose view matrix differs goes through its own view and back through the inverse of the frame's.
static bool s_frameViewInvValid;
static float s_frameViewInvR[9];
static float s_viewCorrA[9];
static float s_viewCorrB[3];
static int s_viewCorrectedDraws;
static int s_viewCorrectedLogs;

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
// frame's. Row vectors: frame = world * A + B.
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
			"LCS water: a draw used a different camera matrix than the frame's (%.1f %.1f %.1f units apart) - moved into the frame's space (%d draws so far)",
			s_viewCorrB[0], s_viewCorrB[1], s_viewCorrB[2], s_viewCorrectedDraws);
	}
	return true;
}

// Transforms a draw's decoded vertices into s_drawPos and expands its topology into s_drawIdx as
// a plain triangle list. Row-vector, matching the vertex shader: world = vec4(pos, 1.0) * m, with
// the 4x3 matrix holding its translation in the last row. For a skinned mesh the bones are
// already folded in by the decoder, which is what gets people in pose for free.
static void BakeDraw(const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, GEPrimitiveType prim, const float world[12],
	float drawMin[3], float drawMax[3]) {
	s_drawPos.resize((size_t)numDecodedVerts * 3);
	s_drawIdx.clear();

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

	const auto index = [&](int i) -> u16 {
		return (u16)(indices ? indices[i] : i);
	};
	switch (prim) {
	case GE_PRIM_TRIANGLES:
		for (int i = 0; i + 2 < indexCount; i += 3) {
			s_drawIdx.push_back(index(i));
			s_drawIdx.push_back(index(i + 1));
			s_drawIdx.push_back(index(i + 2));
		}
		break;
	case GE_PRIM_TRIANGLE_STRIP:
		for (int i = 0; i + 2 < indexCount; i++) {
			s_drawIdx.push_back(index(i));
			s_drawIdx.push_back(index(i + 1));
			s_drawIdx.push_back(index(i + 2));
		}
		break;
	case GE_PRIM_TRIANGLE_FAN:
		for (int i = 1; i + 1 < indexCount; i++) {
			s_drawIdx.push_back(index(0));
			s_drawIdx.push_back(index(i));
			s_drawIdx.push_back(index(i + 1));
		}
		break;
	default:
		break;
	}
	// Winding is irrelevant here: nothing in these passes culls a face. Depth decides everything,
	// and the sea has no back to speak of.
}

// ---------------------------------------------------------------------------------------------
// Recognising the sea
//
// The signature is the one the probe measured, and every clause earns its place:
//
//   flat        - every vertex within a couple of centimetres of one z. This is the whole idea.
//   wide        - at least 24 units across, which no decal or manhole cover is.
//   unlit       - the game applies no hardware lighting to water. A road IS lit.
//   no normals  - the far sectors carry none, and neither does any prelit scenery, so this is
//                 not on its own decisive - it is here to keep the near WAVE sectors, which do
//                 carry normals, out of the LEARNING set while still being accepted by texture.
//   no colours  - same role.
//   textured    - the address is the whole point.
//
// A texture that collects three such draws at one height in a frame is the sea. The address is a
// heap address, so this is learned per session and thrown away when the camera jumps, exactly as
// LCSShadow learns the blob-shadow textures.
// ---------------------------------------------------------------------------------------------

static u32 s_waterTexture;
static float s_waterPlaneZ;
static int s_framesSinceWater;

struct LearnEntry {
	int count;
	float z;
};
static std::unordered_map<u32, LearnEntry> s_learn;

static const int kLearnDrawsNeeded = 3;
static const int kForgetWaterFrames = 600;   // ~10 seconds without any sea in view

static void ForgetWater(const char *why) {
	if (s_waterTexture) {
		NOTICE_LOG(Log::G3D, "LCS water: forgetting texture %08x (%s)", s_waterTexture, why);
	}
	s_waterTexture = 0;
	s_waterPlaneZ = 0.0f;
	s_learn.clear();
}

static void OfferToLearner(int vertCount, u32 vertTypeID, const float drawMin[3],
	const float drawMax[3]) {
	if (s_waterTexture || vertCount < 4) {
		return;
	}
	// LCS: unlit is what separates the sea here. Its 128-unit squares at z -0.5 carry normals and
	// vertex colours on every draw, so VCS's two clauses against them (kept for its near wave
	// sectors) would refuse all of it; and every road and pavement LCS draws is lit (measured with
	// the probe over a park and the harbour: 132 sea draws unlit, every flat lit draw land).
	if (!gstate.isTextureMapEnabled() || gstate.isLightingEnabled()) {
		return;
	}
	const float dz = drawMax[kUpAxis] - drawMin[kUpAxis];
	if (dz > 0.02f) {
		return;
	}
	const float spanX = drawMax[0] - drawMin[0];
	const float spanY = drawMax[1] - drawMin[1];
	if (spanX < 24.0f || spanY < 24.0f) {
		return;
	}
	s_current.learnCandidates++;

	const u32 addr = gstate.getTextureAddress(0);
	LearnEntry &e = s_learn[addr];
	if (e.count == 0) {
		e.z = drawMin[kUpAxis];
		e.count = 1;
		return;
	}
	// One texture drawn at two different heights is not a sea, it is a texture that happens to
	// tile flat things - a car park roof and a pavement, say. Only agreement counts.
	if (fabsf(e.z - drawMin[kUpAxis]) > 0.10f) {
		e.z = drawMin[kUpAxis];
		e.count = 1;
		return;
	}
	e.count++;
	if (e.count >= kLearnDrawsNeeded) {
		s_waterTexture = addr;
		s_waterPlaneZ = e.z;
		s_learn.clear();
		NOTICE_LOG(Log::G3D, "LCS water: texture %08x is the sea, plane z = %.2f", addr, e.z);
	}
}

// ---------------------------------------------------------------------------------------------
// The road mask
//
// A top-down R8 image of the AI traffic network around the player, sampled in the fragment shader
// by world XY. Rasterising it once when the player has moved far enough is the difference between
// one texture fetch a pixel and a loop over thousands of line segments.
//
// The graph is the game's own - `ThePaths`, the network the traffic drives on, which is the
// definition of "where a car can go" and is authored rather than inferred. See VCSRoute.h.
// ---------------------------------------------------------------------------------------------

// Set from the probe environment variable, and read by the rasteriser - which runs long
// before the probe section of this file is reached, hence the forward declaration.
static bool s_probeArmed;

static const int kRoadMapSize = 512;

// Two bytes a texel: coverage, and the road's own height. The height is what stops a fence rail,
// a balcony or a rooftop from being rained on because a road happens to run underneath it - the
// mask is a top-down image and knows nothing about the third dimension unless it carries it.
//
// Stored relative to the map's centre height over +/-kRoadHeightRange, which is 0.5 units a step -
// far finer than the window the shader compares against.
static const float kRoadHeightRange = 64.0f;

// The shoulder, as a fraction of the half width. Fixed rather than a setting because the mask
// stores a ramp across the whole of `reach` and the shader reconstructs the kerb from it, so the
// two have to agree - and one number in one place is how they stay agreed.
static const float kRoadFeatherRatio = 0.6f;
static Draw::Texture *s_roadTexture;
static std::vector<u8> s_roadPixels;
static float s_roadCentre[2];
static float s_roadCentreZ;
static float s_roadSpan;
static bool s_roadValid;
static bool s_roadDirty;

// What a rebuild costs. It happens every span/8 of travel, which while DRIVING is every few
// seconds - on the emu thread, in the middle of the thing the player is complaining is
// stuttery. A number is the only way to know whether it is a contributor or a red herring.
// Budgeted in MILLISECONDS, checked against the clock, because two attempts to budget it by
// proxy both missed. Nodes were wrong: cost per node runs from a bounding-box rejection to
// hundreds of rasterised texels, and slices came out between 0.19 and 1.32 ms. Texels were wrong
// too, and more interestingly - most of a build is the ~10000 segment REJECTIONS, not the ~25000
// texels that survive them, so a texel budget let thousands of nodes through for free and put
// most of the build back into one frame.
//
// The clock is read every kRoadBuildCheckEvery nodes, which is often enough to land near the
// budget and rare enough that the reads themselves do not show up.
static const float kRoadBuildMsPerFrame = 0.35f;
static const int kRoadBuildCheckEvery = 64;

static float s_lastGraphMs;
static float s_lastRasteriseMs;   // the whole build, summed over the frames it took
static float s_worstStepMs;       // ...and the worst any single frame has paid, ever
static double s_buildStartTime;
static float s_buildMs;
static float s_lastUploadMs;
static int s_rebuilds;

static void ReleaseRoadTexture() {
	if (s_roadTexture) {
		s_roadTexture->Release();
		s_roadTexture = nullptr;
	}
}

// Rasterising the mask, SPREAD OVER FRAMES.
//
// It used to run in one go, and measured, that was 2.2-2.8 ms on the emu thread every time the
// camera left the middle eighth of the map - which while DRIVING is every couple of seconds, on a
// 33 ms frame budget, in the middle of the thing being reported as stuttery. Nothing about the job
// needs to be atomic: it writes a private buffer and only swaps it in when it is finished, so the
// live mask stays correct and merely a little stale in the meantime. The map reaches 256 units and
// the rebuild triggers at 64, so there is four times as much slack as a build needs.
//
// The graph load is NOT sliced. `EnsureGraph` is a pointer compare when warm and a full re-read of
// ~3000 nodes and ~17000 links when the level changes - measured at 47 ms, once per level, which
// is a load screen's worth of work happening during a load. Slicing it would mean making
// VCSRoute's loader resumable for the benefit of one caller.

static std::vector<u8> s_roadBuild;
static float s_buildCentre[2];
static float s_buildCentreZ;
static float s_buildSpan;
static int s_buildNode;
static int s_buildLinks;
static bool s_building;
static bool s_maskDumped;

static void BeginRoadBuild(float centreX, float centreY, float centreZ, float span) {
	{
		const double t0 = time_now_d();
		const bool ok = LCS::EnsureRoadGraph();
		s_lastGraphMs = (float)((time_now_d() - t0) * 1000.0);
		if (!ok) {
			return;
		}
	}
	if (LCS::RoadNodeCount() <= 0) {
		return;
	}
	s_roadBuild.assign((size_t)kRoadMapSize * kRoadMapSize * 2, 0);
	s_buildCentre[0] = centreX;
	s_buildCentre[1] = centreY;
	s_buildCentreZ = centreZ;
	s_buildSpan = span;
	s_buildNode = 0;
	s_buildLinks = 0;
	s_building = true;
}

// Returns true when the build finished this call.
static bool StepRoadBuild(float msBudget) {
	if (!s_building) {
		return false;
	}
	const int nodeCount = LCS::RoadNodeCount();
	s_current.roadNodes = nodeCount;
	if (nodeCount <= 0) {
		s_building = false;
		return false;
	}

	const float span = s_buildSpan;
	const float centreZ = s_buildCentreZ;
	const float texelsPerUnit = (float)kRoadMapSize / span;
	const float minX = s_buildCentre[0] - span * 0.5f;
	const float minY = s_buildCentre[1] - span * 0.5f;
	const float halfWidth = s_settings.roadHalfWidth;
	// The soft shoulder. A hard edge on a puddle reads as a decal; this is where the wet fades
	// out into the pavement, and it is why the mask is greyscale rather than a stencil.
	const float feather = halfWidth * kRoadFeatherRatio;
	const float reach = halfWidth + feather;

	const double deadline = time_now_d() + msBudget * 0.001;
	int n = s_buildNode;
	for (; n < nodeCount; n++) {
		// Checked BETWEEN nodes rather than inside the inner loop, so a node is always finished
		// once started and the buffer is never left half-written for one segment.
		if ((n & (kRoadBuildCheckEvery - 1)) == 0 && time_now_d() >= deadline) {
			break;
		}
		LCS::RoutePoint a;
		if (!LCS::RoadNodeAt(n, &a)) {
			continue;
		}
		for (int slot = 0; slot < 8; slot++) {
			int other = -1;
			if (!LCS::RoadLinkAt(n, slot, &other)) {
				break;
			}
			// Each edge is in the graph twice. Drawing it once is half the work and identical
			// output.
			if (other <= n) {
				continue;
			}
			LCS::RoutePoint b;
			if (!LCS::RoadNodeAt(other, &b)) {
				continue;
			}
			const float x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
			// Cheap reject: the segment's own bounding box against the map.
			if (std::fmax(x0, x1) < minX - reach || std::fmin(x0, x1) > minX + span + reach ||
				std::fmax(y0, y1) < minY - reach || std::fmin(y0, y1) > minY + span + reach) {
				continue;
			}
			s_buildLinks++;

			const int tx0 = (int)std::floor((std::fmin(x0, x1) - reach - minX) * texelsPerUnit);
			const int tx1 = (int)std::ceil((std::fmax(x0, x1) + reach - minX) * texelsPerUnit);
			const int ty0 = (int)std::floor((std::fmin(y0, y1) - reach - minY) * texelsPerUnit);
			const int ty1 = (int)std::ceil((std::fmax(y0, y1) + reach - minY) * texelsPerUnit);

			const float ex = x1 - x0;
			const float ey = y1 - y0;
			const float lenSq = ex * ex + ey * ey;
			const float invLenSq = lenSq > 1e-6f ? 1.0f / lenSq : 0.0f;
			const float reachSq = reach * reach;
			const float z0 = a.z, z1 = b.z;

			for (int ty = ty0 < 0 ? 0 : ty0; ty <= ty1 && ty < kRoadMapSize; ty++) {
				const float wy = minY + ((float)ty + 0.5f) / texelsPerUnit;
				u8 *row = s_roadBuild.data() + (size_t)ty * kRoadMapSize * 2;
				for (int tx = tx0 < 0 ? 0 : tx0; tx <= tx1 && tx < kRoadMapSize; tx++) {
					const float wx = minX + ((float)tx + 0.5f) / texelsPerUnit;
					float t = ((wx - x0) * ex + (wy - y0) * ey) * invLenSq;
					if (t < 0.0f) t = 0.0f;
					if (t > 1.0f) t = 1.0f;
					const float dx = wx - (x0 + ex * t);
					const float dy = wy - (y0 + ey * t);
					// Squared until the last moment: most texels in a segment's bounding box are
					// outside its reach, and those never need the root.
					const float dSq = dx * dx + dy * dy;
					if (dSq >= reachSq) {
						continue;
					}
					// A LINEAR ramp across the road, 1 at the centre line and 0 at the outer edge
					// of `reach`.
					//
					// The shader needs to know WHERE ACROSS THE ROAD a pixel is, not just whether
					// it is on one, because that is what a puddle retreats along as it dries. The
					// kerb's own softness is reconstructed from the same ramp - see
					// kRoadFeatherRatio - so nothing was lost by giving the byte the more
					// informative job.
					const float v = 1.0f - sqrtf(dSq) / reach;
					const u8 value = (u8)(v * 255.0f + 0.5f);
					if (value <= row[tx * 2]) {
						continue;
					}
					row[tx * 2] = value;
					// Where two roads cross at different heights - and this city has flyovers -
					// the texel keeps the height of whichever covers it more strongly. One byte
					// cannot hold both, and a bridge deck is what the player is standing on.
					float z = z0 + (z1 - z0) * t;
					z = (z - centreZ) / (2.0f * kRoadHeightRange) + 0.5f;
					if (z < 0.0f) z = 0.0f;
					if (z > 1.0f) z = 1.0f;
					row[tx * 2 + 1] = (u8)(z * 255.0f + 0.5f);
				}
			}
		}
	}
	s_buildNode = n;
	if (s_buildNode < nodeCount) {
		return false;
	}

	// Finished: swap it in. Nothing before this point has touched what is on screen.
	s_roadPixels.swap(s_roadBuild);
	s_roadCentre[0] = s_buildCentre[0];
	s_roadCentre[1] = s_buildCentre[1];
	s_roadCentreZ = s_buildCentreZ;
	s_roadSpan = s_buildSpan;
	s_current.roadLinks = s_buildLinks;
	s_roadValid = true;
	s_roadDirty = true;
	s_building = false;

	// While the probe is armed, write the mask out as a PGM. "Is the road mask right" splits into
	// two questions - is the rasterisation right, and is it in the right place - and a picture of
	// the mask on its own answers the first without the second getting in the way.
	// Once per session, not once per rebuild. It answers "is the rasterisation right", which
	// does not change between rebuilds - and a 256KB file write inside the build was landing in
	// the very frame whose cost was being measured, which is how it came to be reported as the
	// build being expensive.
	if (s_probeArmed && !s_maskDumped) {
		s_maskDumped = true;
		FILE *f = fopen("lcs_roadmask.pgm", "wb");
		if (f) {
			fprintf(f, "P5\n# centre %.1f %.1f span %.1f links %d nodes %d\n%d %d\n255\n",
				s_roadCentre[0], s_roadCentre[1], s_roadSpan, s_buildLinks, nodeCount,
				kRoadMapSize, kRoadMapSize);
			// Coverage only - a PGM cannot carry the height channel, and the question this
			// picture answers is about where the roads are.
			std::vector<u8> grey((size_t)kRoadMapSize * kRoadMapSize);
			for (size_t i = 0; i < grey.size(); i++) {
				grey[i] = s_roadPixels[i * 2];
			}
			fwrite(grey.data(), 1, grey.size(), f);
			fclose(f);
		}
	}
	return true;
}

static void EnsureRoadTexture(Draw::DrawContext *draw) {
	if (s_roadTexture && !s_roadDirty) {
		return;
	}
	if (s_roadPixels.empty()) {
		s_roadPixels.assign((size_t)kRoadMapSize * kRoadMapSize * 2, 0);
	}
	ReleaseRoadTexture();

	using namespace Draw;
	TextureDesc desc{};
	desc.type = TextureType::LINEAR2D;
	desc.format = DataFormat::R8G8_UNORM;
	desc.width = kRoadMapSize;
	desc.height = kRoadMapSize;
	desc.depth = 1;
	desc.mipLevels = 1;
	desc.generateMips = false;
	desc.swizzle = TextureSwizzle::DEFAULT;
	desc.tag = "lcs_road_mask";
	desc.initData.push_back(s_roadPixels.data());
	s_roadTexture = draw->CreateTexture(desc);
	s_roadDirty = false;
}

// ---------------------------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------------------------

// Whether the current blend setup can actually change what is already in the framebuffer.
//
// The same question, and the same answer, as LCSShadow: VCS switches blending on for its OPAQUE
// pass and specifies a blend that copies, so "is blending enabled" throws away the entire city
// and "can this blend alter the destination" keeps it.
static bool BlendAltersDestination() {
	if (gstate.getBlendEq() != GE_BLENDMODE_MUL_AND_ADD) {
		return true;
	}
	const GEBlendSrcFactor funcA = gstate.getBlendFuncA();
	const GEBlendDstFactor funcB = gstate.getBlendFuncB();
	if (funcA == GE_SRCBLEND_FIXA && funcB == GE_DSTBLEND_FIXB) {
		return !(gstate.getFixA() == 0xFFFFFF && gstate.getFixB() == 0x000000);
	}
	if (funcA == GE_SRCBLEND_SRCALPHA && funcB == GE_DSTBLEND_INVSRCALPHA) {
		return !gstate_c.vertexFullAlpha;
	}
	return true;
}

static void ProbeNoteDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount,
	const u8 *decoded, int numDecodedVerts, int stride, int posOffset, const float world[12]);

// Bookkeeping for one rejected draw. Written once so the skinned breakdown cannot drift out of
// step with the main counts as tests are added.
static inline void NoteReject(Reject why, bool skinned) {
	s_current.rejected[(int)why]++;
	if (skinned) {
		s_current.skinnedRejected[(int)why]++;
		// Once per reason per boot. A person missing from the pre-pass is a person the sea gets
		// painted over, and the counters above only ever reached a panel nobody has open while it
		// happens. Through and offscreen draws are not the main frame, so they say nothing.
		static bool s_said[(int)Reject::Count];
		if (why != Reject::Through && why != Reject::OffscreenTarget && !s_said[(int)why]) {
			s_said[(int)why] = true;
			NOTICE_LOG(Log::G3D, "LCS water: a person was left out of the occlusion pass (%s) - the sea can paint over them",
				RejectName(why));
		}
	}
}

// How many rebases of the GE space NoteDraw has followed rather than treated as a jump.
//
// There is deliberately NO re-capture of draws that arrive after the passes, though LCSShadow has one.
// It was copied across and taken out: in rain the frame has a 2D draw in the middle of it, the restart
// threw the road away and ran the passes again on what was left, and the sea won every road pixel.
static int s_rebasesFollowed;

void NoteDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount,
	const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, const float world[12]) {
	if (!g_active) {
		return;
	}
	s_current.draws++;
	const bool skinned = (vertTypeID & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE;
	if (skinned && !gstate.isModeThrough()) {
		s_current.skinnedDraws++;
	}
	NoteLights();
	ProbeNoteDraw(prim, vertTypeID, vertexCount, decoded, numDecodedVerts, stride, posOffset,
		world);

	if (gstate.isModeThrough()) {
		NoteReject(Reject::Through, skinned);
		return;
	}
	if (gstate_c.curRTWidth < 480 || gstate_c.curRTHeight < 272) {
		NoteReject(Reject::OffscreenTarget, skinned);
		return;
	}
	switch (prim) {
	case GE_PRIM_TRIANGLES:
	case GE_PRIM_TRIANGLE_STRIP:
	case GE_PRIM_TRIANGLE_FAN:
		break;
	default:
		NoteReject(Reject::Primitive, skinned);
		return;
	}
	if (vertexCount < 3 || numDecodedVerts <= 0) {
		NoteReject(Reject::TooFewVerts, skinned);
		return;
	}

	// The first 3D draw of the frame is the one to take the camera from: by the last draw the
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

		// The view matrix is row-vector and world-to-view, so R is at m[0..8] and t at m[9..11],
		// R is a rotation, and the camera position is -t * R^T. The world-space direction of view
		// axis c is column c of R.
		{
			const float *m = s_frameViewMatrix;
			const float t[3] = { m[9], m[10], m[11] };
			s_frameCameraPos[0] = -(t[0] * m[0] + t[1] * m[1] + t[2] * m[2]);
			s_frameCameraPos[1] = -(t[0] * m[3] + t[1] * m[4] + t[2] * m[5]);
			s_frameCameraPos[2] = -(t[0] * m[6] + t[1] * m[7] + t[2] * m[8]);
			// The PSP's view space looks down -Z, same convention as GL.
			s_frameCameraForward[0] = -m[2];
			s_frameCameraForward[1] = -m[5];
			s_frameCameraForward[2] = -m[8];
		}
		s_haveViewMatrix = true;
		s_current.cameraValid = true;
		memcpy(s_current.cameraPos, s_frameCameraPos, sizeof(s_frameCameraPos));

		// A rebase, a teleport or a load shows up as the camera moving further in one frame than
		// any camera can. A teleport or a load throws the heap away, and the learned texture address
		// with it. A REBASE does not: the game shifts the space it feeds the GE by a streaming cell
		// as you travel and nothing else changes - the log showed the same sea address relearned
		// within one frame of every "forgetting". Treating the one as the other forgot the sea and
		// dropped the road map at every chunk swap, and the wet pass stays off for the eight-odd
		// frames a rebuild takes. Reported as wet roads flickering while driving. The road map is in
		// world space and the offset is re-derived at every seam, so a rebase needs nothing done.
		//
		// Told apart the way LCSShadow tells them apart: the game's own camera, read in world space
		// on this same frame, stays put across a rebase.
		const std::optional<float> worldX = LCS::CameraWorld(0);
		const std::optional<float> worldY = LCS::CameraWorld(1);
		const bool haveWorld = worldX.has_value() && worldY.has_value();
		if (s_haveLastCameraPos) {
			float moved = 0.0f;
			for (int c = 0; c < 3; c++) {
				const float d = s_frameCameraPos[c] - s_lastCameraPos[c];
				moved += d * d;
			}
			if (moved > 40.0f * 40.0f) {
				bool rebased = false;
				float worldDX = 0.0f, worldDY = 0.0f;
				if (haveWorld && s_haveLastCameraWorld) {
					worldDX = *worldX - s_lastCameraWorld[0];
					worldDY = *worldY - s_lastCameraWorld[1];
					const float geDZ = s_frameCameraPos[2] - s_lastCameraPos[2];
					rebased = worldDX * worldDX + worldDY * worldDY < 20.0f * 20.0f && fabsf(geDZ) < 20.0f;
				}
				if (rebased) {
					s_rebasesFollowed++;
					if (s_rebasesFollowed <= 20) {
						NOTICE_LOG(Log::G3D, "LCS water: the GE space rebased by %.1f %.1f - kept the sea and the road map (%d so far)",
							(s_frameCameraPos[0] - s_lastCameraPos[0]) - worldDX,
							(s_frameCameraPos[1] - s_lastCameraPos[1]) - worldDY, s_rebasesFollowed);
					}
				} else {
					ForgetWater("the camera jumped");
					s_roadValid = false;
				}
			}
		}
		memcpy(s_lastCameraPos, s_frameCameraPos, sizeof(s_frameCameraPos));
		s_haveLastCameraPos = true;
		if (haveWorld) {
			s_lastCameraWorld[0] = *worldX;
			s_lastCameraWorld[1] = *worldY;
		}
		s_haveLastCameraWorld = haveWorld;
	}

	float drawMin[3], drawMax[3];
	BakeDraw(decoded, numDecodedVerts, indices, indexCount, stride, posOffset, prim, world,
		drawMin, drawMax);
	if (s_drawIdx.size() < 3) {
		NoteReject(Reject::TooFewVerts, skinned);
		return;
	}

	const bool isWater = s_waterTexture != 0 && gstate.isTextureMapEnabled() &&
		gstate.getTextureAddress(0) == s_waterTexture;

	if (!isWater) {
		// The game's full-screen overlays - the colour filter, the fades - are drawn as 3D
		// geometry sitting ON the camera rather than in through mode, so no render-state test can
		// see them for what they are. They would win every pixel of the depth pre-pass and the
		// whole frame would be "solid, two units away".
		//
		// Never a person, though. The overlays are flat quads, and a close camera puts a separate
		// piece of the player - a head, a hand - entirely inside this box, which filed it here and
		// painted the sea over it. Measured: the first stutter run logged a person rejected for
		// exactly this, with the camera pulled in behind him.
		bool wholeDrawAtCamera = !skinned;
		for (int c = 0; c < 3 && wholeDrawAtCamera; c++) {
			if (drawMin[c] < s_frameCameraPos[c] - 2.0f ||
				drawMax[c] > s_frameCameraPos[c] + 2.0f) {
				wholeDrawAtCamera = false;
			}
		}
		if (wholeDrawAtCamera) {
			NoteReject(Reject::NearCamera, skinned);
			return;
		}
		// Neither of the next two tests applies to a PERSON. A skinned draw is never a particle or
		// a pane of glass, and the game fades people out with their material alpha - which turns a
		// copying blend into one that alters the destination, so the player failed the blend test
		// on exactly those frames and the sea was composited straight over him. Reported from play
		// as the water overlapping the character "sometimes". A faded person occluding costs the
		// vanilla sea showing through them for as long as the fade lasts, which is the same good
		// way round as the alpha-tested case below.
		//
		// Not part of the solid scene: particles, coronas, the vanilla blob shadow.
		if (!skinned && !gstate.isDepthWriteEnabled()) {
			NoteReject(Reject::NoDepthWrite, skinned);
			return;
		}
		// Genuine transparency should not occlude what is behind it. Alpha-TESTED draws are kept
		// deliberately: a palm frond's depth is a rectangle, which is wrong, but the failure is
		// "the sea keeps its vanilla look behind this tree" rather than "the sea is painted over
		// the tree", and that is much the better way round.
		// Offer it to the learner before filing it. Until the sea has been recognised its own
		// draws land here, which is harmless - they write the same depth either way. LCS: before
		// the blend test, because LCS draws its sea blended (fixed factors, 1,10,10 in the probe).
		OfferToLearner(vertexCount, vertTypeID, drawMin, drawMax);
		if (!skinned && gstate.isAlphaBlendEnabled() && BlendAltersDestination()) {
			NoteReject(Reject::Blended, skinned);
			return;
		}
		s_current.solidDraws++;
		if (skinned) {
			s_current.skinnedCaptured++;
		}
	} else {
		s_current.waterDraws++;
		s_current.waterVerts += numDecodedVerts;
		s_framesSinceWater = 0;
	}

	AppendGeometry(s_drawPos.data(), numDecodedVerts, s_drawIdx.data(), (int)s_drawIdx.size(),
		isWater);
}

// ---------------------------------------------------------------------------------------------
// The passes
// ---------------------------------------------------------------------------------------------

struct WaterUB {
	float cameraViewProj[16];   //   0
	float camera[4];            //  64  xyz camera position, w time in seconds
	float sun[4];               //  80  xyz towards the sun, w luminance
	float waveA[4];             //  96  waveScale, waveAmplitude, waveSpeed, water opacity
	float waveB[4];             // 112  fresnelF0, specularPower, specularStrength, reflection
	float deep[4];              // 128  rgb deep colour, a deepMix
	float screen[4];            // 144  horizon UV, reflection spread, mirror scale, 1/detailFade
	float road[4];              // 160  map origin xy, 1/span, rain
	float wet[4];               // 176  darkening, ripple scale, ripple rate, ripple strength
	float wet2[4];              // 192  road reflection, road mirror scale, map edge fade, road z
	float wet3[4];              // 208  kerb feather, wetness, ripple rain, puddle scale
	float wet4[4];              // 224  drain patch size, reflection blur, max reflection, ripple light
	float misc[4];              // 240  debugView, isWaterPass, GE-to-world offset xy
	float sea[4];               // 256  floor uv scale xy, foam depth, shore foam
	float sea2[4];              // 272  normal map repeats per unit, its strength, extinction depth, -
	float shadowUndo[4];        // 288  the shadow composite's tint rgb and strength - see the road pass
};

static const UniformBufferDesc s_ubDesc{ sizeof(WaterUB), {
	{ "u_cameraViewProj", 0, -1, UniformType::MATRIX4X4, 0 },
	{ "u_camera", 1, 0, UniformType::FLOAT4, 64 },
	{ "u_sun", 2, 1, UniformType::FLOAT4, 80 },
	{ "u_waveA", 3, 2, UniformType::FLOAT4, 96 },
	{ "u_waveB", 4, 3, UniformType::FLOAT4, 112 },
	{ "u_deep", 5, 4, UniformType::FLOAT4, 128 },
	{ "u_screen", 6, 5, UniformType::FLOAT4, 144 },
	{ "u_road", 7, 6, UniformType::FLOAT4, 160 },
	{ "u_wet", 8, 7, UniformType::FLOAT4, 176 },
	{ "u_wet2", 9, 8, UniformType::FLOAT4, 192 },
	{ "u_wet3", 10, 9, UniformType::FLOAT4, 208 },
	{ "u_wet4", 11, 10, UniformType::FLOAT4, 224 },
	{ "u_misc", 12, 11, UniformType::FLOAT4, 240 },
	{ "u_sea", 13, 12, UniformType::FLOAT4, 256 },
	{ "u_sea2", 14, 13, UniformType::FLOAT4, 272 },
	{ "u_shadowUndo", 15, 14, UniformType::FLOAT4, 288 },
} };

struct CompositeUB {
	float params[4];
};
static const UniformBufferDesc s_compositeUBDesc{ sizeof(CompositeUB), {
	{ "u_params", 0, 0, UniformType::FLOAT4, 0 },
} };

static Draw::Framebuffer *s_sceneFbo;      // a copy of the frame, so the shading can read it
static Draw::Framebuffer *s_surfaceFbo;    // what this module has decided to paint on top
// The solid scene's world height at each pixel, which is the floor under the water there - see the
// floor pass in RenderSurface. Its depth buffer is copied into the surface's, so the solids are
// drawn once for both.
static Draw::Framebuffer *s_floorFbo;
static Draw::Pipeline *s_floorPipeline;
// The fine ripples and the foam's noise, generated once - see BuildNormalMap.
static Draw::Texture *s_normalTex;
static Draw::SamplerState *s_pointSampler;   // the floor heights are encoded, and must not blend
static Draw::SamplerState *s_wrapSampler;    // the normal map tiles
static Draw::Pipeline *s_depthPipeline;
static Draw::Pipeline *s_shadePipeline;
static Draw::Pipeline *s_compositePipeline;
static Draw::SamplerState *s_sampler;
static int s_surfaceWidth;
static int s_surfaceHeight;
// The viewport the floor and the surface are drawn into, which FillUniforms needs for the uv scale.
static int s_floorViewW;
static int s_floorViewH;
static bool s_setupFailed;
static bool s_frameShaded;
static bool s_shadeRendered;

static void ReleaseSizedResources() {
	if (s_sceneFbo) {
		s_sceneFbo->Release();
		s_sceneFbo = nullptr;
	}
	if (s_surfaceFbo) {
		s_surfaceFbo->Release();
		s_surfaceFbo = nullptr;
	}
	if (s_floorFbo) {
		s_floorFbo->Release();
		s_floorFbo = nullptr;
	}
	s_surfaceWidth = 0;
	s_surfaceHeight = 0;
}

static void ReleasePipelines() {
	if (s_depthPipeline) {
		s_depthPipeline->Release();
		s_depthPipeline = nullptr;
	}
	if (s_shadePipeline) {
		s_shadePipeline->Release();
		s_shadePipeline = nullptr;
	}
	if (s_compositePipeline) {
		s_compositePipeline->Release();
		s_compositePipeline = nullptr;
	}
	if (s_sampler) {
		s_sampler->Release();
		s_sampler = nullptr;
	}
	if (s_floorPipeline) {
		s_floorPipeline->Release();
		s_floorPipeline = nullptr;
	}
	if (s_pointSampler) {
		s_pointSampler->Release();
		s_pointSampler = nullptr;
	}
	if (s_wrapSampler) {
		s_wrapSampler->Release();
		s_wrapSampler = nullptr;
	}
	if (s_normalTex) {
		s_normalTex->Release();
		s_normalTex = nullptr;
	}
}

// The ripple texture: a tiling height field of small wind waves, stored as its normal, with a tiling
// noise beside it for the foam. Generated rather than shipped, because a texture file is one more
// asset that has to be deployed on four platforms, and this is forty lines of arithmetic.
//
// TILING BY CONSTRUCTION: every component is a sine with a whole number of periods across the tile,
// so the left edge meets the right exactly and the texture can repeat across a sea 2048 units wide
// without a seam. The amplitudes fall off as |k|^-1.6 - most of the energy in the long waves, as in
// a real sea - and the directions are spread about one wind so the ripples read as running
// somewhere rather than as a carpet. Sampled twice in the shader, at different scales and angles,
// which is what keeps the repeat from showing.
//
// The mips are built here, by averaging, and that is the anti-aliasing: averaged slopes flatten with
// distance, which is exactly what a sea does when its ripples get smaller than a pixel.
static Draw::Texture *BuildNormalMap(Draw::DrawContext *draw) {
	const int kSize = 256;
	const int kLevels = 9;
	struct Wave { float kx, ky, amp, phase; };
	std::vector<Wave> waves;
	uint32_t seed = 0x9e3779b9u;
	auto rnd = [&seed]() {
		seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
		return (float)(seed & 0xffffff) / (float)0x1000000;
	};
	while (waves.size() < 56) {
		const float angle = 0.6f + (rnd() - 0.5f) * 2.4f;   // about one wind, spread both ways
		const float k = 2.0f + rnd() * rnd() * 30.0f;        // cycles per tile, long waves favoured
		const int kx = (int)std::round(std::cos(angle) * k);
		const int ky = (int)std::round(std::sin(angle) * k);
		if (kx == 0 && ky == 0) {
			continue;
		}
		const float len = std::sqrt((float)(kx * kx + ky * ky));
		waves.push_back({ (float)kx, (float)ky, std::pow(len, -1.6f) * (0.5f + rnd()), rnd() * 6.2831853f });
	}

	// Tiling value noise for the foam, on a lattice that divides the tile.
	auto lattice = [](int x, int y, int cells, uint32_t salt) {
		x = ((x % cells) + cells) % cells;
		y = ((y % cells) + cells) % cells;
		uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + salt;
		h = (h ^ (h >> 13)) * 1274126177u;
		return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
	};
	auto valueNoise = [&lattice](float u, float v, int cells, uint32_t salt) {
		const float x = u * cells, y = v * cells;
		const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
		float fx = x - x0, fy = y - y0;
		fx = fx * fx * (3.0f - 2.0f * fx);
		fy = fy * fy * (3.0f - 2.0f * fy);
		const float a = lattice(x0, y0, cells, salt), b = lattice(x0 + 1, y0, cells, salt);
		const float c = lattice(x0, y0 + 1, cells, salt), d = lattice(x0 + 1, y0 + 1, cells, salt);
		return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
	};

	std::vector<float> slope((size_t)kSize * kSize * 2);
	std::vector<float> foam((size_t)kSize * kSize);
	float maxSlope = 0.0001f;
	for (int y = 0; y < kSize; y++) {
		for (int x = 0; x < kSize; x++) {
			float dx = 0.0f, dy = 0.0f;
			for (const Wave &w : waves) {
				const float arg = 6.2831853f * (w.kx * x + w.ky * y) / kSize + w.phase;
				const float c = std::cos(arg) * w.amp * 6.2831853f / kSize;
				dx += c * w.kx;
				dy += c * w.ky;
			}
			slope[((size_t)y * kSize + x) * 2 + 0] = dx;
			slope[((size_t)y * kSize + x) * 2 + 1] = dy;
			maxSlope = std::max(maxSlope, std::max(std::fabs(dx), std::fabs(dy)));
			const float u = (float)x / kSize, v = (float)y / kSize;
			foam[(size_t)y * kSize + x] = valueNoise(u, v, 16, 11u) * 0.55f +
				valueNoise(u, v, 32, 23u) * 0.30f + valueNoise(u, v, 64, 37u) * 0.15f;
		}
	}

	std::vector<std::vector<uint8_t>> levels(kLevels);
	std::vector<float> curSlope = slope, curFoam = foam;
	int size = kSize;
	for (int level = 0; level < kLevels; level++) {
		std::vector<uint8_t> &px = levels[level];
		px.resize((size_t)size * size * 4);
		for (int i = 0; i < size * size; i++) {
			// Normalised by the largest slope, so the map spans its full range and the strength
			// setting means the same thing whatever the waves happen to sum to.
			const float sx = curSlope[i * 2 + 0] / maxSlope, sy = curSlope[i * 2 + 1] / maxSlope;
			px[i * 4 + 0] = (uint8_t)std::clamp((int)std::lround((sx * 0.5f + 0.5f) * 255.0f), 0, 255);
			px[i * 4 + 1] = (uint8_t)std::clamp((int)std::lround((sy * 0.5f + 0.5f) * 255.0f), 0, 255);
			px[i * 4 + 2] = (uint8_t)std::clamp((int)std::lround(curFoam[i] * 255.0f), 0, 255);
			px[i * 4 + 3] = 255;
		}
		if (size == 1) {
			levels.resize(level + 1);
			break;
		}
		const int half = size / 2;
		std::vector<float> nextSlope((size_t)half * half * 2), nextFoam((size_t)half * half);
		for (int y = 0; y < half; y++) {
			for (int x = 0; x < half; x++) {
				for (int c = 0; c < 2; c++) {
					nextSlope[((size_t)y * half + x) * 2 + c] = 0.25f * (
						curSlope[((size_t)(2 * y) * size + 2 * x) * 2 + c] + curSlope[((size_t)(2 * y) * size + 2 * x + 1) * 2 + c] +
						curSlope[((size_t)(2 * y + 1) * size + 2 * x) * 2 + c] + curSlope[((size_t)(2 * y + 1) * size + 2 * x + 1) * 2 + c]);
				}
				nextFoam[(size_t)y * half + x] = 0.25f * (
					curFoam[(size_t)(2 * y) * size + 2 * x] + curFoam[(size_t)(2 * y) * size + 2 * x + 1] +
					curFoam[(size_t)(2 * y + 1) * size + 2 * x] + curFoam[(size_t)(2 * y + 1) * size + 2 * x + 1]);
			}
		}
		curSlope.swap(nextSlope);
		curFoam.swap(nextFoam);
		size = half;
	}

	Draw::TextureDesc desc{};
	desc.type = Draw::TextureType::LINEAR2D;
	desc.format = Draw::DataFormat::R8G8B8A8_UNORM;
	desc.width = kSize;
	desc.height = kSize;
	desc.depth = 1;
	desc.mipLevels = (int)levels.size();
	desc.generateMips = false;
	desc.tag = "lcs_water_ripples";
	for (const std::vector<uint8_t> &level : levels) {
		desc.initData.push_back(level.data());
	}
	return draw->CreateTexture(desc);
}

// The two samplers every shading fragment shader declares, whether or not it reads both. On
// Vulkan the descriptor layout is part of the pipeline, so a shader that declares a subset is a
// different layout, and there is nothing to gain from two of them.
//
// THREE, because thin3d has three texture slots (MAX_TEXTURE_SLOTS) and raising it would change every
// descriptor layout upstream has. The middle one is shared: the road mask for the wet-road pass, the
// floor heights for the sea's - each pass reads only its own, and binds it before it draws.
static const SamplerDef kShadeSamplers[] = {
	{ 0, "sceneTex", SamplerFlags(0) },
	{ 1, "auxTex", SamplerFlags(0) },
	{ 2, "normalTex", SamplerFlags(0) },
};

static const UniformDef kUniforms[] = {
	{ "mat4", "u_cameraViewProj", 0 },
	{ "vec4", "u_camera", 1 },
	{ "vec4", "u_sun", 2 },
	{ "vec4", "u_waveA", 3 },
	{ "vec4", "u_waveB", 4 },
	{ "vec4", "u_deep", 5 },
	{ "vec4", "u_screen", 6 },
	{ "vec4", "u_road", 7 },
	{ "vec4", "u_wet", 8 },
	{ "vec4", "u_wet2", 9 },
	{ "vec4", "u_wet3", 10 },
	{ "vec4", "u_wet4", 11 },
	{ "vec4", "u_misc", 12 },
	{ "vec4", "u_sea", 13 },
	{ "vec4", "u_sea2", 14 },
	{ "vec4", "u_shadowUndo", 15 },
};

static const VaryingDef kVaryings[] = {
	{ "vec3", "v_world", Draw::SEM_TEXCOORD0, 0, "highp" },
	{ "vec4", "v_clip", Draw::SEM_TEXCOORD1, 1, "highp" },
};

// The geometry vertex shader, shared by the depth pre-pass and the shading pass. Shared on
// purpose: the shading pass tests depth EQUAL against what the pre-pass wrote, and two shaders
// that merely ought to agree is not the same thing as one shader used twice.
static Draw::ShaderModule *CreateGeometryVS(Draw::DrawContext *draw) {
	using namespace Draw;
	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();
	const size_t kSize = 4096;
	char *code = new char[kSize];
	{
		ShaderWriter writer(code, lang, ShaderStage::Vertex);
		static const InputDef inputs[] = { { "vec3", "a_position", Draw::SEM_POSITION } };
		writer.BeginVSMain(inputs, kUniforms, kVaryings);
		writer.C("  vec4 clip = mul(vec4(a_position, 1.0), u_cameraViewProj);\n");
		writer.C("  v_world = a_position;\n");
		writer.C("  v_clip = clip;\n");
		writer.C("  gl_Position = clip;\n");
		writer.EndVSMain(kVaryings);
	}
	ShaderModule *vs = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
		(const uint8_t *)code, strlen(code), "lcs_water_vs");
	if (!vs) {
		ERROR_LOG(Log::G3D, "LCS water VS failed:\n%s", code);
	}
	delete[] code;
	return vs;
}

// Value noise, at global scope because GLSL cannot declare a function inside main().
//
// Used for two different things and worth having once: where a stretch of road drains to, and the
// shape of the puddles left on it. Both want something stable in WORLD space - a puddle that swims
// when the camera moves is not a puddle - which is what keying the hash off floor(worldXY) gives
// and what a screen-space noise could not.
// Value noise, emitted at global scope because GLSL cannot declare a function inside main().
//
// Used for two different things and so worth having once: which way a stretch of road drains, and
// the shape of the puddles left on it. Both want something stable in WORLD space - a puddle that
// swims when the camera moves is not a puddle - which is what keying the hash off floor(worldXY)
// gives and what a screen-space noise could not.
static void WriteShadeHelpers(ShaderWriter &writer) {
	writer.C("float vcsHash21(vec2 p) {\n");
	writer.C("  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);\n");
	writer.C("}\n");
	writer.C("float vnoise(vec2 p) {\n");
	writer.C("  vec2 i = floor(p);\n");
	writer.C("  vec2 f = p - i;\n");
	writer.C("  vec2 w = f * f * (3.0 - 2.0 * f);\n");
	writer.C("  return mix(mix(vcsHash21(i), vcsHash21(i + vec2(1.0, 0.0)), w.x),\n");
	writer.C("             mix(vcsHash21(i + vec2(0.0, 1.0)),\n");
	writer.C("                 vcsHash21(i + vec2(1.0, 1.0)), w.x), w.y);\n");
	writer.C("}\n");
	// Two octaves for the puddles specifically. One is too smooth to break a drying road into
	// anything a player would call a puddle: measured over this same formula, a single octave
	// leaves wet runs with a median length of 25 metres, which reads as a stripe painted down the
	// road. The second octave takes that to under seven.
	writer.C("float puddleNoise(vec2 p) {\n");
	writer.C("  return vnoise(p) * 0.65 + vnoise(p * 2.7 + vec2(11.3, 5.1)) * 0.35;\n");
	writer.C("}\n");
}

static void WriteShadeFS(ShaderWriter &writer) {
	writer.C("  vec3 P = v_world;\n");
	writer.C("  vec2 uv = v_clip.xy / max(abs(v_clip.w), 0.0001) * 0.5 + 0.5;\n");
	writer.C("  vec3 toCam = u_camera.xyz - P;\n");
	writer.C("  float dist = length(toCam);\n");
	writer.C("  vec3 V = toCam / max(dist, 0.0001);\n");
	writer.C("  float t = u_camera.w;\n");
	writer.C("  float isWater = u_misc.y;\n");
	writer.C("  vec3 scene = ").SampleTexture2D("sceneTex", "clamp(uv, 0.0, 1.0)").C(".rgb;\n");

	// --- the sea's normal -------------------------------------------------------------------
	//
	// Three layers of sine gradient rather than a normal map, because a texture would need a
	// projection, a scale and a wrap mode chosen for a surface that is 2048 units across. The
	// gradient of a sum of sines IS the normal, up to the amplitude, and it costs six cosines.
	//
	// The detail fade is not an optimisation: past a few hundred units the normal changes faster
	// than a pixel and the sea turns to sparkling noise, which is worse than a flat mirror.
	writer.C("  vec2 wp = P.xy * u_waveA.x;\n");
	// The SWELL, at about a sixth of the chop's frequency, and it does NOT fade with distance.
	//
	// This is what was wrong with the sea at range. `g` is a SLOPE, not a height, so a long
	// wavelength costs nothing in steepness - but fading the whole sum with distance took the
	// normal from 8 degrees of tilt at 30 metres to 0.9 degrees at 300, which is a mirror-flat
	// plane, and most of the time you look at this sea it is hundreds of metres away. The fade
	// exists to stop the FINE octaves aliasing into sparkle once they are smaller than a pixel;
	// the swell is never smaller than a pixel and had no business fading with them.
	writer.C("  vec2 ws = wp * 0.18;\n");
	writer.C("  vec2 swell = vec2(cos(ws.x + t * 0.35), cos(ws.y * 1.07 - t * 0.31));\n");
	writer.C("  vec2 chop = vec2(cos(wp.x + t * 1.10), cos(wp.y * 1.13 - t * 0.90));\n");
	writer.C("  chop += 0.55 * vec2(cos(wp.y * 2.30 - t * 1.70), cos(wp.x * 2.10 + t * 1.30));\n");
	writer.C("  chop += 0.28 * vec2(cos((wp.x + wp.y) * 4.10 + t * 2.30),\n");
	writer.C("                      cos((wp.x - wp.y) * 3.90 - t * 2.10));\n");
	writer.C("  float detail = clamp(1.0 - dist * u_screen.w, 0.10, 1.0);\n");
	// The sines are now only the swell and a little chop; the fine surface is the ripple map, two
	// samples of it at different scales, angles and speeds - the tuxalin water shader's approach,
	// and the reason it does not read as a pattern the way three regular sines did. Anchored in
	// the game's WORLD space, so a rebase of the space the GE is fed does not jump the ripples.
	// No distance fade of its own: the map's mips average the slopes, which flattens it at range
	// exactly as far as a pixel can no longer resolve it.
	writer.C("  vec2 wpos = P.xy + u_misc.zw;\n");
	writer.C("  vec2 ruv = wpos * u_sea2.x;\n");
	writer.C("  vec2 r1 = ").SampleTexture2D("normalTex", "ruv + vec2(t * 0.031, t * 0.019)").C(".rg * 2.0 - 1.0;\n");
	writer.C("  vec2 r2 = ").SampleTexture2D("normalTex", "vec2(ruv.y, -ruv.x) * 1.73 + vec2(-t * 0.023, t * 0.029)").C(".rg * 2.0 - 1.0;\n");
	// Rain roughens the sea: up to twice the ripple while it is falling.
	writer.C("  vec2 ripple = (r1 + vec2(-r2.y, r2.x)) * u_sea2.y * (1.0 + u_wet3.z);\n");
	writer.C("  vec2 g = (swell * 0.9 + chop * detail * 0.45) * u_waveA.y + ripple;\n");
	writer.C("  vec3 nWater = normalize(vec3(-g.x, -g.y, 1.0));\n");
	// --- how deep the water is here ----------------------------------------------------------
	//
	// Read off the floor pass: the world height of the nearest solid behind this pixel. Blue says
	// there was one; a floor above the water is something the far side of the sea, not under it,
	// and counts as none.
	writer.C("  vec4 fl = ").SampleTexture2D("auxTex", "clamp(uv * u_sea.xy, 0.0, 1.0)").C(";\n");
	writer.C("  float floorZ = (fl.r + fl.g / 255.0) * 256.0 - 64.0;\n");
	writer.C("  float hasFloor = step(0.5, fl.b) * step(floorZ, P.z + 0.05);\n");
	writer.C("  float depthV = mix(400.0, max(P.z - floorZ, 0.0), hasFloor);\n");
	// Along the view ray rather than straight down, because that is the water the light crosses:
	// looking down into the shallows you see the bottom, looking along them you do not.
	writer.C("  float depthPath = depthV / max(V.z, 0.12);\n");

	// --- the road's normal, and how wet it is ------------------------------------------------
	//
	// The geometric normal comes from the derivatives of the world position rather than from
	// vertex normals, and that is not a shortcut: VCS ships its scenery PRELIT and carries no
	// normals for any of it, so there are none to read. Derivatives are exact for a flat road.
	writer.C("  vec3 nGeom = normalize(cross(dFdx(P), dFdy(P)));\n");
	writer.C("  if (nGeom.z < 0.0) { nGeom = -nGeom; }\n");
	// P is in the space the GE is fed; the road map is in the game's own world space. One
	// addition crosses between them - see FrameStats::geOffset for how it is measured.
	writer.C("  vec2 rUV = (P.xy + u_misc.zw - u_road.xy) * u_road.z;\n");
	writer.C("  float inMap = step(0.0, rUV.x) * step(rUV.x, 1.0) *\n");
	writer.C("                step(0.0, rUV.y) * step(rUV.y, 1.0);\n");
	writer.C("  vec2 roadSample =").SampleTexture2D("auxTex", "clamp(rUV, 0.0, 1.0)").C(".rg;\n");
	// The mask stores a LINEAR ramp across the road: 1 on the centre line, 0 at the outer edge of
	// the shoulder, and exactly 0 where there is no road at all. So `across` is where this pixel
	// sits between the crown and the kerb, which is the axis a drying puddle retreats along, and
	// the kerb's own softness is the outer feather of that same ramp.
	writer.C("  float onRoadArea = step(0.004, roadSample.x);\n");
	writer.C("  float kerb = smoothstep(0.0, max(u_wet3.x, 0.001), roadSample.x);\n");
	// Normalised over the DRIVABLE half width, not over the whole of `reach`.
	//
	// This is a bug worth remembering the shape of. Measured over the ramp: the shoulder occupies
	// the outer u_wet3.x of it, so the tarmac ends at ramp = u_wet3.x and not at ramp = 0. Taking
	// `across` over the whole ramp put across = 1 - the gutter, where the drying water is supposed
	// to end up - out in the shoulder, where `kerb` had already faded it to nothing. Every puddle
	// that drained to the kerb was erased by the kerb.
	writer.C("  float across = clamp((1.0 - roadSample.x) / max(1.0 - u_wet3.x, 0.001),\n");
	writer.C("                       0.0, 1.0);\n");
	writer.C("  float road = kerb * onRoadArea * inMap;\n");
	// The mask is a top-down image, so without this a fence rail, a balcony or a rooftop gets
	// rained on because a road runs underneath it. Reported as pink fence rails over a canal.
	// The window is generous on purpose: a car roof is a metre and a half up and SHOULD be wet.
	writer.F("  float roadZ = u_wet2.w + (roadSample.y * 2.0 - 1.0) * %f;\n", kRoadHeightRange);
	writer.C("  float onRoad = 1.0 - smoothstep(1.6, 3.4, abs(P.z - roadZ));\n");
	writer.C("  road *= onRoad;\n");
	writer.C("  float up = smoothstep(0.80, 0.96, nGeom.z);\n");
	// The map has an edge, and a wet road that stops dead at it is a line ruled across the street.
	writer.C("  vec2 mapEdge = abs(rUV * 2.0 - 1.0);\n");
	writer.C("  float mapFade = clamp((1.0 - max(mapEdge.x, mapEdge.y)) / max(u_wet2.z, 0.001),\n");
	writer.C("                        0.0, 1.0);\n");
	// --- the puddles, and where they go as they dry ------------------------------------------
	//
	// Two things happen at once while a road dries, and neither of them on its own looks like
	// drying. The wet BAND retreats towards wherever that stretch of road drains, and what is
	// left BREAKS UP into patches. Together they read as puddles shrinking and running to the
	// side; either alone reads as a fade.
	//
	// Which way a stretch drains is hashed from its world position on a coarse grid, so one
	// street does not drain the same way for its whole length - a real road's camber changes with
	// the junctions. `drain` is 0 at the crown and 1 at the kerb, and is smooth so there is no
	// seam where the grid cells meet.
	writer.C("  float wetness = u_road.w;\n");
	writer.C("  float drain = smoothstep(0.35, 0.65, vnoise(P.xy * u_wet4.x));\n");
	writer.C("  float fromDrain = abs(across - drain);\n");
	writer.C("  float band = 1.0 - smoothstep(wetness - 0.18, wetness + 0.06, fromDrain);\n");
	// The break-up. At full wetness the threshold is below every noise value and this is 1
	// everywhere; as the wetness falls only the wettest hollows survive.
	//
	// `patchMask` and not `patch`: **patch is a RESERVED KEYWORD in GLSL 4.50** - it is a
	// tessellation qualifier - so `float patch` is a syntax error, the whole fragment shader fails
	// to compile, s_setupFailed latches, and the entire feature silently draws nothing. Water and
	// roads both. It cost a round trip to find because glslang's message goes out at WARN, which
	// this build's log level drops; only the bare "Failed to compile" line at ERROR gets through.
	// Check for that line after every build that touches this shader.
	writer.C("  float n = puddleNoise(P.xy * u_wet3.w);\n");
	// THREE STATES OF A WET ROAD, not one, and that is the whole of what this pass gets right now.
	//
	// It used to be one mask with a wetness-cubed floor under it, so in real rain the floor reached
	// 1 and the entire road became a single mirror - reported as wanting better puddles, and the
	// honest reading is that there were none: a puddle is a pool on a road that is NOT one.
	//
	//   damp  the whole surface, as soon as it is wet at all: darker, a faint smeared sheen, no
	//         mirror. Real wet asphalt is mostly this. It dries first.
	//   pool  where the water collects: world-anchored shapes from the puddle noise with a finer
	//         octave for a ragged edge, growing with the wetness and retreating to the drain side
	//         as it falls - the same `band` the old mask used. Sharp, strong reflection; the
	//         droplets land here.
	//   rim   a darker ring round each pool, where the water has soaked the surface and gone.
	//
	// The thresholds were set off the CPU, over the same formula: pools cover 38% of the road in
	// full rain, 12% half dry and 3% nearly dry - before the drain band thins them further.
	writer.C("  float n3 = n * 0.85 + vnoise(P.xy * u_wet3.w * 4.1 + vec2(3.7, 9.2)) * 0.15;\n");
	writer.C("  float poolLevel = mix(0.76, 0.52, wetness);\n");
	writer.C("  float surf = road * up * mapFade * (1.0 - isWater);\n");
	writer.C("  float pool = clamp(smoothstep(poolLevel, poolLevel + 0.05, n3) * band * surf, 0.0, 1.0);\n");
	writer.C("  float rim = clamp(smoothstep(poolLevel - 0.07, poolLevel, n3) * band * surf, 0.0, 1.0);\n");
	writer.C("  float damp = clamp(smoothstep(0.05, 0.35, wetness) * surf, 0.0, 1.0);\n");
	writer.C("  float wet = max(max(damp, rim), pool);\n");

	// --- droplet impacts ---------------------------------------------------------------------
	//
	// One expanding ring per grid cell, the cell's phase hashed from its coordinates so the
	// impacts are scattered rather than in step, and the whole thing keyed off WORLD position so
	// a puddle stays where it is while the camera moves. The ring is a wavelet - a sine damped by
	// distance from the ring's radius - and its gradient is the normal perturbation.
	//
	// Nine cells, and every one of them is behind the wetness test: a dry frame pays for a
	// compare, not for nine hashes.
	writer.C("  vec2 ripG = vec2(0.0);\n");
	// The ring's CREST as well as its slope: see rippleLight. The slope feeds the normal as before.
	writer.C("  float crest = 0.0;\n");
	// Gated on the ripple rain, not on the wetness. "It has stopped raining and the road is still
	// wet" is precisely the state in which rings would be wrong - nothing is landing on it.
	// ...and not past the distance at which a 15cm ring is smaller than a pixel, or the road turns
	// to sparkle. Same reasoning as the sea's detail fade, at a far shorter range because these
	// features are two orders of magnitude smaller than a swell.
	writer.C("  float ripFade = clamp(1.0 - dist * 0.035, 0.0, 1.0);\n");
	// The sea gets them too while it rains - rain landing on open water is the other place a
	// player sees it - and needs no wetness to qualify.
	writer.C("  if ((pool > 0.02 || isWater > 0.5) && u_wet3.z > 0.01 && ripFade > 0.01) {\n");
	writer.C("    vec2 rp = P.xy * u_wet.y;\n");
	writer.C("    vec2 cellBase = floor(rp);\n");
	writer.C("    for (int oy = -1; oy <= 1; oy++) {\n");
	writer.C("      for (int ox = -1; ox <= 1; ox++) {\n");
	writer.C("        vec2 c = cellBase + vec2(float(ox), float(oy));\n");
	writer.C("        float h = fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);\n");
	writer.C("        vec2 centre = c + vec2(fract(h * 7.13), fract(h * 13.77));\n");
	writer.C("        vec2 d = rp - centre;\n");
	writer.C("        float r = length(d) + 0.0001;\n");
	writer.C("        float phase = fract(t * u_wet.z + h);\n");
	// All three constants are in CELL units, so they scale with rippleScale and a ring stays the
	// same fraction of its cell however that setting is moved. At the default 34cm cell that is a
	// ring reaching about 15cm, a ripple wavelength near 5cm, and a decay over about 2cm.
	writer.C("        float ring = phase * 0.42;\n");
	writer.C("        float w = sin((r - ring) * 45.0) * exp(-abs(r - ring) * 16.0) *\n");
	writer.C("                  (1.0 - phase);\n");
	writer.C("        ripG += (d / r) * w;\n");
	writer.C("        crest += max(w, 0.0);\n");
	writer.C("      }\n");
	writer.C("    }\n");
	// On a road, only inside a pool - a raindrop on damp tarmac makes no ring anyone can see, and
	// rings scattered over the whole road read as printed circles rather than as rain.
	writer.C("    float ringMask = mix(pool, 1.0, isWater);\n");
	writer.C("    ripG *= u_wet.w * u_wet3.z * ripFade * ringMask;\n");
	writer.C("    crest *= u_wet3.z * ripFade * ringMask;\n");
	writer.C("  }\n");
	// A puddle is not glass. Wind keeps a film of water moving whether or not anything is landing
	// in it, and that wobble - small, slow, anchored in the world - is most of what tells a puddle
	// from a mirror lying on the road. The sea's ripple map at about five times the sea's scale.
	// Value noise rather than the ripple map, which on the roads is not bound: slot 2 carries the
	// shadow mask there - see the road pass.
	writer.C("  if (pool > 0.02) {\n");
	writer.C("    vec2 wq = wpos * 2.2 + vec2(t * 0.35, -t * 0.27);\n");
	writer.C("    vec2 pr = vec2(vnoise(wq + vec2(0.37, 0.0)) - vnoise(wq - vec2(0.37, 0.0)),\n");
	writer.C("                   vnoise(wq + vec2(0.0, 0.37)) - vnoise(wq - vec2(0.0, 0.37))) * 1.4;\n");
	writer.C("    ripG += pr * 0.07 * pool;\n");
	writer.C("  }\n");
	writer.C("  vec3 nWet = normalize(vec3(-ripG.x, -ripG.y, 1.0));\n");
	writer.C("  nWater = normalize(nWater + vec3(-ripG.x, -ripG.y, 0.0) * isWater);\n");

	// --- shading, common to both -------------------------------------------------------------
	writer.C("  vec3 N = mix(nWet, nWater, isWater);\n");
	writer.C("  float ndv = clamp(dot(N, V), 0.0, 1.0);\n");
	writer.C("  float fres = u_waveB.x + (1.0 - u_waveB.x) * pow(1.0 - ndv, 5.0);\n");
	// Capped. Flat water really does approach a perfect mirror at grazing incidence and the
	// Schlick term above is right about that - but flat water is what this sea is not, and a
	// rough surface cannot reach the flat-water reflectance. The ceiling stands in for the
	// roughness the wave model is too coarse to carry. The Water tab reports the median Fresnel
	// and the share of the sea above half, before and after.
	writer.C("  fres = min(fres, u_wet4.z);\n");

	// The reflection is the frame itself, mirrored about the horizon and dragged sideways by the
	// wave normal. Exact for a distant shoreline, approximate for anything close - which is the
	// right way round, because what a sea reflects is the sky and the far bank. Everything needed
	// for a screen-space ray march is here except a depth texture, and that is the upgrade path.
	writer.C("  float hy = u_screen.x;\n");
	// The sea gets a straight mirror; a road at your feet gets a compressed one, because the
	// approximation is exact only for a surface at eye height. See wetMirrorScale.
	writer.C("  float mirror = mix(u_wet2.y, u_screen.z, isWater);\n");
	writer.C("  vec2 mUV = vec2(uv.x + N.x * u_screen.y,\n");
	writer.C("                  hy - (uv.y - hy) * mirror + N.y * u_screen.y);\n");
	// Five taps, not one, spread wider the further away the surface is.
	//
	// This is most of the difference between "reflective" and "a mirror", and it is the honest fix
	// rather than a fudge: water at any distance is roughened by capillary waves far smaller than
	// a pixel, and what it hands back is the average over all of them. A single tap models a sheet
	// of glass. The spread grows with distance because that is where a pixel covers more of those
	// waves - which is also exactly where the Fresnel term is highest and the reflection is most
	// of what you are looking at.
	writer.C("  float blur = u_wet4.y * clamp(dist * u_screen.w, 0.05, 1.0);\n");
	writer.C("  vec3 refl0 = ").SampleTexture2D("sceneTex", "clamp(mUV, 0.003, 0.997)").C(".rgb;\n");
	writer.C("  vec3 refl = refl0;\n");
	writer.C("  refl += ").SampleTexture2D("sceneTex", "clamp(mUV + vec2(blur, 0.0), 0.003, 0.997)").C(".rgb;\n");
	writer.C("  refl += ").SampleTexture2D("sceneTex", "clamp(mUV - vec2(blur, 0.0), 0.003, 0.997)").C(".rgb;\n");
	writer.C("  refl += ").SampleTexture2D("sceneTex", "clamp(mUV + vec2(0.0, blur * 0.6), 0.003, 0.997)").C(".rgb;\n");
	writer.C("  refl += ").SampleTexture2D("sceneTex", "clamp(mUV - vec2(0.0, blur * 0.6), 0.003, 0.997)").C(".rgb;\n");
	writer.C("  refl *= 0.2;\n");
	// Take the shadows back out of a ROAD's reflection: the frame was multiplied by
	// mix(1, mix(tint, 1, lit), strength) where the sun does not reach, so dividing by the same
	// factor at the reflected point gives the colour before. Not the sea - its slot 2 is the
	// ripple map, and strength is 0 there.
	writer.C("  if (u_shadowUndo.a > 0.001 && isWater < 0.5) {\n");
	writer.C("    float litR = ").SampleTexture2D("normalTex", "clamp(mUV, 0.003, 0.997)").C(".r;\n");
	writer.C("    vec3 fR = mix(vec3(1.0), mix(u_shadowUndo.rgb, vec3(1.0), litR), u_shadowUndo.a);\n");
	writer.C("    refl /= max(fR, vec3(0.2));\n");
	writer.C("    refl0 /= max(fR, vec3(0.2));\n");
	writer.C("  }\n");

	writer.C("  vec3 H = normalize(u_sun.xyz + V);\n");
	writer.C("  float spec = pow(max(dot(N, H), 0.0), u_waveB.y) * u_waveB.z * u_sun.w;\n");

	// The sea starts from the pixel the game drew and moves towards the deep colour, which is
	// what keeps sunset, fog and rain looking like the game's own without anything here knowing
	// the time of day.
	// Scaled by (1 - fres), which is the other half of the Fresnel and the strongest "this is
	// water" cue there is over open sea: looking down you see INTO it and it goes deep and dark,
	// looking along it you see the sky off the surface and it goes pale. Mixing a fixed amount of
	// deep colour everywhere - which is what this did - tints the whole sea slightly darker and
	// reads as a colour filter rather than as depth.
	//
	// And now by DEPTH as well, per channel, which is what was missing: the deep colour went on
	// regardless of how much water there was, so looking down into the shallows - where the
	// Fresnel is lowest and the deep mix highest - painted deep-sea colour over the game's own
	// turquoise and the sand under it, and the beach read as murky. Red is absorbed first and blue
	// last, as in real water, which is what turns a shelving beach turquoise before it goes blue.
	writer.C("  vec3 extinct = clamp(vec3(depthPath) / (u_sea2.z * vec3(0.35, 0.70, 1.0)), 0.0, 1.0);\n");
	writer.C("  vec3 baseW = mix(scene, u_deep.rgb, u_deep.a * (1.0 - fres) * extinct);\n");
	// The reflection fades in the very shallows too, where there is hardly a surface to reflect.
	writer.C("  float shallow = clamp(depthV * 4.0, 0.0, 1.0);\n");
	writer.C("  vec3 colW = mix(baseW, refl, clamp(fres * u_waveB.w * shallow, 0.0, 1.0));\n");
	writer.C("  colW += vec3(spec * shallow);\n");
	writer.C("  colW += (scene * 0.5 + refl * 0.5) * (crest * u_wet4.w * 0.6 * (1.0 - fres));\n");
	// --- the shore ---------------------------------------------------------------------------
	//
	// Foam where the water runs out: strongest at the waterline and breaking up with depth, through
	// the ripple map's noise channel at two scales so the edge is ragged rather than ruled. It
	// breathes - the band swells and draws back - which is the waves arriving, and it sits over the
	// hard line where the game's water polygons meet its sand, which is exactly what needed hiding.
	// Lit by the game's own pixel, so it is white at noon and grey at night rather than glowing.
	writer.C("  if (isWater > 0.5 && hasFloor > 0.5 && u_sea.w > 0.001) {\n");
	writer.C("    float fn = ").SampleTexture2D("normalTex", "wpos * 0.21 + vec2(t * 0.013, -t * 0.009)").C(".b * 0.6 +\n");
	writer.C("               ").SampleTexture2D("normalTex", "wpos * 0.53 - vec2(t * 0.021, t * 0.017)").C(".b * 0.4;\n");
	writer.C("    float lap = 0.5 + 0.5 * sin(t * 1.4 + dot(wpos, vec2(0.031, 0.047)));\n");
	writer.C("    float band = 1.0 - clamp(depthV / (u_sea.z * (0.75 + 0.5 * lap)), 0.0, 1.0);\n");
	writer.C("    float foam = smoothstep(0.42, 0.62, fn * 0.75 + band * 0.85 - 0.30) * band * u_sea.w;\n");
	writer.C("    float lum = dot(scene, vec3(0.299, 0.587, 0.114));\n");
	writer.C("    vec3 foamCol = vec3(0.94, 0.97, 1.0) * clamp(lum * 1.9 + 0.08, 0.15, 1.0);\n");
	writer.C("    colW = mix(colW, foamCol, clamp(foam, 0.0, 1.0));\n");
	writer.C("  }\n");

	// Damp darkens; the rim darkens a little more; a pool shows what it reflects over darker ground.
	writer.C("  vec3 colR = scene * mix(1.0, u_wet.x, max(damp, rim)) * mix(1.0, 0.84, rim * (1.0 - pool));\n");
	// A pool reflects more than damp asphalt, because what makes a puddle visible is the sky in
	// it - but NOT as a mirror, which is what it was: the sharp centre tap at up to 92%, so a pool
	// at any distance was a clean copy of the buildings, reported as "puddles look like a mirror".
	// Now the same smeared average the damp road gets, a little of the sharp tap, at most half the
	// pixel, and dimmed: road water is dirty and shallow, and a reflection in it is darker than
	// the thing reflected. The wobble above does the rest.
	writer.C("  vec3 reflR = mix(refl, refl0, 0.25 * pool) * mix(1.0, 0.78, pool);\n");
	writer.C("  float dampRefl = fres * u_wet2.x * 0.4 * damp;\n");
	writer.C("  float poolRefl = clamp((fres * 1.1 + 0.06) * u_wet2.x * 2.0, 0.0, 0.5) * pool;\n");
	writer.C("  colR = mix(colR, reflR, clamp(max(dampRefl, poolRefl), 0.0, 1.0));\n");
	// The sun in a puddle is the sun's own small disc, not the sea's glitter path: that lobe is wide
	// on purpose (see specularPower), and on a flat pool it painted a white patch the size of the
	// pool. Eight times the exponent and about half the strength, so it is a point the wobble breaks
	// into glints.
	writer.C("  float specPool = pow(max(dot(N, H), 0.0), u_waveB.y * 8.0) * u_waveB.z * 0.55 * u_sun.w;\n");
	writer.C("  colR += vec3(spec * 0.2 * damp + specPool * pool);\n");
	// Each ring crest catches light from any angle, lit by what is actually there - the game's own
	// pixel and the reflection - so a ring is bright at noon and dim at night rather than glowing.
	// Scaled by (1 - fres) because at a low angle the reflection is already showing the ripple
	// through the normal, and that is where they would otherwise be counted twice.
	writer.C("  colR += (scene * 0.6 + refl * 0.4) * (crest * u_wet4.w * (1.0 - fres));\n");

	writer.C("  vec3 col = mix(colR, colW, isWater);\n");
	writer.C("  float a = mix(wet, u_waveA.w, isWater);\n");

	// The debug views, for the same reason LCSShadow has them: an effect that does not appear has
	// several causes that look identical from outside, and these separate them in one run.
	writer.C("  if (u_misc.x > 0.5) {\n");
	writer.C("    if (u_misc.x < 1.5) { col = vec3(damp, pool, rim); a = 1.0; }\n");
	writer.C("    else if (u_misc.x < 2.5) { col = N * 0.5 + 0.5; a = 1.0; }\n");
	writer.C("    else if (u_misc.x < 3.5) { col = vec3(clamp(mUV, 0.0, 1.0), 0.0); a = 1.0; }\n");
	writer.C("    else if (u_misc.x < 4.5) { col = mix(vec3(0.0), vec3(0.0, 1.0, 1.0), isWater); a = 1.0; }\n");
	writer.C("    else { col = vec3(hasFloor, clamp(depthV / 4.0, 0.0, 1.0), clamp(depthPath / 20.0, 0.0, 1.0)) * isWater; a = 1.0; }\n");
	writer.C("  }\n");

	// Premultiplied, so the composite is one ONE/INV_SRC_ALPHA blend and a pixel this pass did
	// not touch is left bit-for-bit as the game drew it.
	writer.C("  vec4 outColor = vec4(col * a, a);\n");
}

static bool EnsurePipelines(Draw::DrawContext *draw) {
	if (s_depthPipeline && s_shadePipeline && s_compositePipeline && s_floorPipeline) {
		return true;
	}
	if (s_setupFailed) {
		return false;
	}
	ReleasePipelines();

	using namespace Draw;
	const ShaderLanguageDesc &lang = draw->GetShaderLanguageDesc();

	ShaderModule *geomVS = CreateGeometryVS(draw);

	// The depth pre-pass fragment shader writes nothing; the pass exists for the depth it leaves.
	ShaderModule *depthFS = nullptr;
	{
		const size_t kSize = 2048;
		char *code = new char[kSize];
		{
			ShaderWriter writer(code, lang, ShaderStage::Fragment);
			writer.BeginFSMain(kUniforms, kVaryings);
			writer.C("  vec4 outColor = vec4(1.0);\n");
			writer.EndFSMain("outColor");
		}
		depthFS = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
			(const uint8_t *)code, strlen(code), "lcs_water_depth_fs");
		if (!depthFS) {
			ERROR_LOG(Log::G3D, "LCS water depth FS failed:\n%s", code);
		}
		delete[] code;
	}

	// The floor pass writes the world height of the nearest solid surface, which under a water pixel
	// is the sea bed. Sixteen bits across two channels of an ordinary RGBA8 target, over -64..192:
	// four thousandths of a unit, far finer than any shore. Blue marks that something was drawn at
	// all, so open water with nothing under it reads as deep rather than as a floor at -64.
	ShaderModule *floorFS = nullptr;
	{
		const size_t kSize = 2048;
		char *code = new char[kSize];
		{
			ShaderWriter writer(code, lang, ShaderStage::Fragment);
			writer.HighPrecisionFloat();
			writer.BeginFSMain(kUniforms, kVaryings);
			writer.C("  float h = clamp((v_world.z + 64.0) / 256.0, 0.0, 1.0) * 255.0;\n");
			writer.C("  vec4 outColor = vec4(floor(h) / 255.0, fract(h), 1.0, 1.0);\n");
			writer.EndFSMain("outColor");
		}
		floorFS = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
			(const uint8_t *)code, strlen(code), "lcs_water_floor_fs");
		if (!floorFS) {
			ERROR_LOG(Log::G3D, "LCS water floor FS failed:\n%s", code);
		}
		delete[] code;
	}

	ShaderModule *shadeFS = nullptr;
	{
		const size_t kSize = 24576;
		char *code = new char[kSize];
		{
			ShaderWriter writer(code, lang, ShaderStage::Fragment);
			writer.HighPrecisionFloat();
			writer.DeclareSamplers(kShadeSamplers);
			WriteShadeHelpers(writer);
			writer.BeginFSMain(kUniforms, kVaryings);
			WriteShadeFS(writer);
			writer.EndFSMain("outColor");
		}
		shadeFS = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
			(const uint8_t *)code, strlen(code), "lcs_water_shade_fs");
		if (!shadeFS) {
			ERROR_LOG(Log::G3D, "LCS water shade FS failed:\n%s", code);
		}
		delete[] code;
	}

	static const VaryingDef compositeVaryings[] = {
		{ "vec2", "v_uv", Draw::SEM_TEXCOORD0, 0, "highp" },
	};
	static const UniformDef compositeUniforms[] = {
		{ "vec4", "u_params", 0 },
	};
	static const SamplerDef compositeSamplers[] = {
		{ 0, "surfaceTex", SamplerFlags(0) },
	};

	ShaderModule *compVS = nullptr;
	ShaderModule *compFS = nullptr;
	{
		const size_t kSize = 2048;
		char *code = new char[kSize];
		{
			ShaderWriter writer(code, lang, ShaderStage::Vertex);
			static const InputDef inputs[] = { { "vec2", "a_position", Draw::SEM_POSITION } };
			writer.BeginVSMain(inputs, compositeUniforms, compositeVaryings);
			writer.C("  v_uv = a_position * 0.5 + 0.5;\n");
			writer.C("  gl_Position = vec4(a_position, 0.0, 1.0);\n");
			writer.EndVSMain(compositeVaryings);
		}
		compVS = draw->CreateShaderModule(ShaderStage::Vertex, lang.shaderLanguage,
			(const uint8_t *)code, strlen(code), "lcs_water_comp_vs");
		delete[] code;

		code = new char[kSize];
		{
			ShaderWriter writer(code, lang, ShaderStage::Fragment);
			writer.HighPrecisionFloat();
			writer.DeclareSamplers(compositeSamplers);
			writer.BeginFSMain(compositeUniforms, compositeVaryings);
			writer.C("  vec4 s = ").SampleTexture2D("surfaceTex", "v_uv").C(";\n");
			writer.C("  vec4 outColor = s * u_params.x;\n");
			writer.EndFSMain("outColor");
		}
		compFS = draw->CreateShaderModule(ShaderStage::Fragment, lang.shaderLanguage,
			(const uint8_t *)code, strlen(code), "lcs_water_comp_fs");
		delete[] code;
	}

	if (!geomVS || !depthFS || !floorFS || !shadeFS || !compVS || !compFS) {
		if (geomVS) geomVS->Release();
		if (depthFS) depthFS->Release();
		if (floorFS) floorFS->Release();
		if (shadeFS) shadeFS->Release();
		if (compVS) compVS->Release();
		if (compFS) compFS->Release();
		s_setupFailed = true;
		return false;
	}

	static const InputLayoutDesc geomLayout = {
		12,
		{ { SEM_POSITION, DataFormat::R32G32B32_FLOAT, 0 } },
	};
	static const InputLayoutDesc quadLayout = {
		8,
		{ { SEM_POSITION, DataFormat::R32G32_FLOAT, 0 } },
	};
	InputLayout *geomIL = draw->CreateInputLayout(geomLayout);
	InputLayout *quadIL = draw->CreateInputLayout(quadLayout);

	DepthStencilStateDesc writeDepth{};
	writeDepth.depthTestEnabled = true;
	writeDepth.depthWriteEnabled = true;
	writeDepth.depthCompare = Comparison::LESS;
	DepthStencilState *dsWrite = draw->CreateDepthStencilState(writeDepth);

	// EQUAL against what the pre-pass wrote, so exactly the surface the player can see is shaded
	// and nothing behind it is. Safe because both passes run the same vertex shader with the same
	// uniforms - which is why there is one of it.
	DepthStencilStateDesc testEqual{};
	testEqual.depthTestEnabled = true;
	testEqual.depthWriteEnabled = false;
	testEqual.depthCompare = Comparison::EQUAL;
	DepthStencilState *dsEqual = draw->CreateDepthStencilState(testEqual);

	DepthStencilStateDesc noDepth{};
	noDepth.depthTestEnabled = false;
	noDepth.depthWriteEnabled = false;
	noDepth.depthCompare = Comparison::ALWAYS;
	DepthStencilState *dsNone = draw->CreateDepthStencilState(noDepth);

	BlendState *noColour = draw->CreateBlendState({ false, 0x0 });
	BlendState *opaque = draw->CreateBlendState({ false, 0xF });

	// Premultiplied alpha: the shading pass has already multiplied its colour by its own coverage,
	// so a pixel it left at zero contributes nothing at all.
	BlendStateDesc overDesc{};
	overDesc.enabled = true;
	overDesc.colorMask = 0x7;   // leave alpha alone; the game keeps things in it
	overDesc.srcCol = BlendFactor::ONE;
	overDesc.dstCol = BlendFactor::ONE_MINUS_SRC_ALPHA;
	overDesc.eqCol = BlendOp::ADD;
	overDesc.srcAlpha = BlendFactor::ZERO;
	overDesc.dstAlpha = BlendFactor::ONE;
	overDesc.eqAlpha = BlendOp::ADD;
	BlendState *over = draw->CreateBlendState(overDesc);

	// No culling anywhere here. The city is single-sided walls and alpha-tested fences; depth is
	// what decides, not winding.
	RasterState *raster = draw->CreateRasterState({});

	SamplerStateDesc sampDesc{};
	sampDesc.magFilter = TextureFilter::LINEAR;
	sampDesc.minFilter = TextureFilter::LINEAR;
	sampDesc.mipFilter = TextureFilter::NEAREST;
	sampDesc.wrapU = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapV = TextureAddressMode::CLAMP_TO_EDGE;
	sampDesc.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
	s_sampler = draw->CreateSamplerState(sampDesc);

	SamplerStateDesc pointDesc = sampDesc;
	pointDesc.magFilter = TextureFilter::NEAREST;
	pointDesc.minFilter = TextureFilter::NEAREST;
	s_pointSampler = draw->CreateSamplerState(pointDesc);

	SamplerStateDesc wrapDesc = sampDesc;
	wrapDesc.mipFilter = TextureFilter::LINEAR;
	wrapDesc.maxAniso = 4.0f;
	wrapDesc.wrapU = TextureAddressMode::REPEAT;
	wrapDesc.wrapV = TextureAddressMode::REPEAT;
	wrapDesc.wrapW = TextureAddressMode::REPEAT;
	s_wrapSampler = draw->CreateSamplerState(wrapDesc);

	if (!s_normalTex) {
		s_normalTex = BuildNormalMap(draw);
	}

	PipelineDesc depthDesc{
		Primitive::TRIANGLE_LIST,
		{ geomVS, depthFS },
		geomIL, dsWrite, noColour, raster,
		&s_ubDesc,
	};
	s_depthPipeline = draw->CreateGraphicsPipeline(depthDesc, "lcs_water_depth");

	PipelineDesc floorDesc{
		Primitive::TRIANGLE_LIST,
		{ geomVS, floorFS },
		geomIL, dsWrite, opaque, raster,
		&s_ubDesc,
	};
	s_floorPipeline = draw->CreateGraphicsPipeline(floorDesc, "lcs_water_floor");

	PipelineDesc shadeDesc{
		Primitive::TRIANGLE_LIST,
		{ geomVS, shadeFS },
		geomIL, dsEqual, opaque, raster,
		&s_ubDesc,
		kShadeSamplers,
	};
	s_shadePipeline = draw->CreateGraphicsPipeline(shadeDesc, "lcs_water_shade");

	PipelineDesc compDesc{
		Primitive::TRIANGLE_LIST,
		{ compVS, compFS },
		quadIL, dsNone, over, raster,
		&s_compositeUBDesc,
		compositeSamplers,
	};
	s_compositePipeline = draw->CreateGraphicsPipeline(compDesc, "lcs_water_composite");

	geomVS->Release();
	depthFS->Release();
	floorFS->Release();
	shadeFS->Release();
	compVS->Release();
	compFS->Release();
	geomIL->Release();
	quadIL->Release();
	dsWrite->Release();
	dsEqual->Release();
	dsNone->Release();
	noColour->Release();
	opaque->Release();
	over->Release();
	raster->Release();

	if (!s_depthPipeline || !s_shadePipeline || !s_compositePipeline || !s_floorPipeline ||
			!s_normalTex || !s_pointSampler || !s_wrapSampler) {
		ERROR_LOG(Log::G3D, "LCS water pipelines failed to create");
		ReleasePipelines();
		s_setupFailed = true;
		return false;
	}
	return true;
}

static bool EnsureSizedResources(Draw::DrawContext *draw, int width, int height) {
	if (s_sceneFbo && s_surfaceFbo && s_floorFbo && s_surfaceWidth == width && s_surfaceHeight == height) {
		return true;
	}
	ReleaseSizedResources();
	s_sceneFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, false, "lcs_water_scene" });
	s_surfaceFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, true, "lcs_water_surface" });
	s_floorFbo = draw->CreateFramebuffer({ width, height, 1, 1, 0, true, "lcs_water_floor" });
	if (!s_sceneFbo || !s_surfaceFbo || !s_floorFbo) {
		ReleaseSizedResources();
		return false;
	}
	s_surfaceWidth = width;
	s_surfaceHeight = height;
	return true;
}

Draw::Framebuffer *SurfaceBuffer() {
	return s_surfaceFbo;
}

static void FillUniforms(WaterUB *ub, bool isWaterPass) {
	Transpose4x4(s_cameraViewProj, ub->cameraViewProj);
	memcpy(ub->camera, s_frameCameraPos, sizeof(float) * 3);
	ub->camera[3] = (float)(time_now_d() * 0.5);

	if (s_current.sunValid) {
		memcpy(ub->sun, s_current.sunDir, sizeof(float) * 3);
		ub->sun[3] = s_current.sunLuminance;
	} else {
		// No sun found this frame: no glint. Straight up rather than a zero vector, so the
		// normalize in the shader still has something to work with.
		ub->sun[0] = 0.0f; ub->sun[1] = 0.0f; ub->sun[2] = 1.0f;
		ub->sun[3] = 0.0f;
	}

	ub->waveA[0] = s_settings.waveScale;
	ub->waveA[1] = s_settings.waveAmplitude;
	ub->waveA[2] = s_settings.waveSpeed;
	ub->waveA[3] = 1.0f;   // the sea is opaque: the base colour already includes the game's own

	ub->waveB[0] = s_settings.fresnelF0;
	ub->waveB[1] = s_settings.specularPower;
	ub->waveB[2] = s_settings.specularStrength;
	ub->waveB[3] = s_settings.reflectionStrength;

	ub->deep[0] = s_settings.deepColour[0];
	ub->deep[1] = s_settings.deepColour[1];
	ub->deep[2] = s_settings.deepColour[2];
	ub->deep[3] = s_settings.deepMix;

	ub->screen[0] = s_horizonUV;
	ub->screen[1] = s_settings.reflectionSpread;
	ub->screen[2] = s_settings.mirrorScale;
	ub->screen[3] = s_settings.detailFade > 1.0f ? 1.0f / s_settings.detailFade : 0.0f;

	ub->road[0] = s_roadCentre[0] - s_roadSpan * 0.5f;
	ub->road[1] = s_roadCentre[1] - s_roadSpan * 0.5f;
	ub->road[2] = s_roadSpan > 0.0f ? 1.0f / s_roadSpan : 0.0f;
	// The LAGGED wetness, not the rain. See UpdateWetness.
	ub->road[3] = (s_settings.wetRoads && s_roadValid) ? s_wetness : 0.0f;

	ub->wet[0] = s_settings.wetDarkening;
	ub->wet[1] = s_settings.rippleScale;
	ub->wet[2] = s_settings.rippleRate;
	ub->wet[3] = s_settings.rippleStrength;

	ub->wet2[0] = s_settings.wetReflection;
	ub->wet2[1] = s_settings.wetMirrorScale;
	ub->wet2[2] = s_settings.wetEdgeFade;
	ub->wet2[3] = s_roadCentreZ;

	// The kerb's softness, reconstructed from the across-road ramp the mask now stores: the outer
	// kRoadFeatherRatio of `reach` is the shoulder.
	ub->wet3[0] = kRoadFeatherRatio / (1.0f + kRoadFeatherRatio);
	ub->wet3[1] = 0.0f;   // free; the wetness travels in u_road.w, gated with the rest
	ub->wet3[2] = s_rippleRain;
	ub->wet3[3] = s_settings.puddleScale;

	ub->wet4[0] = s_settings.drainPatchSize > 0.5f ? 1.0f / s_settings.drainPatchSize : 0.0f;
	ub->wet4[1] = s_settings.reflectionBlur;
	ub->wet4[2] = s_settings.maxReflection;
	ub->wet4[3] = s_settings.rippleLight;

	ub->misc[0] = (float)s_settings.debugView;
	ub->misc[1] = isWaterPass ? 1.0f : 0.0f;
	ub->misc[2] = s_geOffset[0];
	ub->misc[3] = s_geOffset[1];

	// The floor target is drawn into the same viewport as the surface, which can be smaller than the
	// target itself; this takes the shader's 0..1 across the viewport to the texels it lands on.
	ub->sea[0] = s_surfaceWidth > 0 ? (float)s_floorViewW / (float)s_surfaceWidth : 1.0f;
	ub->sea[1] = s_surfaceHeight > 0 ? (float)s_floorViewH / (float)s_surfaceHeight : 1.0f;
	ub->sea[2] = std::max(s_settings.foamDepth, 0.01f);
	ub->sea[3] = s_settings.shoreFoam;

	ub->sea2[0] = s_settings.normalMapScale;
	ub->sea2[1] = s_settings.normalMapStrength;
	ub->sea2[2] = std::max(s_settings.extinctionDepth, 0.1f);
	ub->sea2[3] = 0.0f;

	ub->shadowUndo[0] = 1.0f;
	ub->shadowUndo[1] = 1.0f;
	ub->shadowUndo[2] = 1.0f;
	ub->shadowUndo[3] = 0.0f;
}

static void RenderSurface(Draw::DrawContext *draw, Draw::Framebuffer *target,
	int width, int height, int viewW, int viewH, int drawnW, int drawnH) {
	s_shadeRendered = false;
	if (!EnsurePipelines(draw) || !EnsureSizedResources(draw, width, height)) {
		return;
	}

	using namespace Draw;

	// 1. The frame as it stands, so the shading can read the colour it is about to change. This
	// is what the reflection samples and what keeps the sea's own time-of-day tint.
	//
	// Only the part the game draws into, stretched over the whole copy: the shader samples it at the
	// pixel's position across the game's viewport, and 0..1 there has to mean 0..1 here.
	draw->BlitFramebuffer(target, 0, 0, drawnW, drawnH, s_sceneFbo, 0, 0, width, height,
		Aspect::COLOR_BIT, FB_BLIT_LINEAR, "lcs_water_scene_copy");

	// 2. The FLOOR: every solid surface captured, writing its world height as it goes. Under a
	// water pixel the nearest solid along the view ray is the sea bed, so this is what the sea's
	// depth is read from - and it is also the solids' half of the depth pre-pass, which is why its
	// depth buffer is copied across rather than the solids being drawn a second time.
	Viewport viewport{ 0.0f, 0.0f, (float)viewW, (float)viewH, 0.0f, 1.0f };
	s_floorViewW = viewW;
	s_floorViewH = viewH;
	WaterUB ub;
	FillUniforms(&ub, false);

	draw->BindFramebufferAsRenderTarget(s_floorFbo,
		{ RPAction::CLEAR, RPAction::CLEAR, RPAction::DONT_CARE, 0, 1.0f, 0, "lcs_water_floor" },
		"lcs_water_floor");
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, viewW, viewH);
	draw->BindPipeline(s_floorPipeline);
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	for (const Batch &batch : s_batches) {
		if (batch.solidIndexCount >= 3) {
			draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3,
				batch.vertexCount,
				s_solidIndices.data() + batch.firstSolidIndex, batch.solidIndexCount);
		}
	}
	draw->CopyFramebufferImage(s_floorFbo, 0, 0, 0, 0, s_surfaceFbo, 0, 0, 0, 0,
		width, height, 1, Aspect::DEPTH_BIT, "lcs_water_floor_depth");

	// 3. The water's half of the depth pre-pass, over the solids' depth just copied in. Occlusion
	// comes entirely from this: a pier in front of the sea wins its pixels and the shading pass
	// never sees them.
	draw->BindFramebufferAsRenderTarget(s_surfaceFbo,
		{ RPAction::CLEAR, RPAction::KEEP, RPAction::DONT_CARE, 0, 1.0f, 0, "lcs_water" },
		"lcs_water");
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, viewW, viewH);

	draw->BindPipeline(s_depthPipeline);
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
	for (const Batch &batch : s_batches) {
		if (batch.waterIndexCount >= 3) {
			draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3,
				batch.vertexCount,
				s_waterIndices.data() + batch.firstWaterIndex, batch.waterIndexCount);
		}
	}

	// 4. Shading, twice with one shader: the roads, then the sea. Slot 1 is the road mask for the
	// first and the floor for the second - see kShadeSamplers.
	Texture *roadTex = s_roadTexture;
	draw->BindFramebufferAsTexture(s_sceneFbo, 0, Aspect::COLOR_BIT, 0);
	draw->BindTextures(2, 1, &s_normalTex);
	draw->BindPipeline(s_shadePipeline);

	if (s_settings.wetRoads && s_roadValid && s_wetness > 0.002f && roadTex) {
		draw->BindTextures(1, 1, &roadTex);
		// Slot 2 is the SHADOW MASK on the roads, not the ripple map: the reflection is the frame
		// mirrored, the frame already has the shadows in it, and so every puddle showed the shadows
		// upside down - reported as "shadows project strangely onto the puddle reflections", and
		// answered for a while by fading the shadows out whenever it rained. The shader divides the
		// composite back out of what it reflects. The roads' only use of the ripple map was the
		// puddles' wobble, which is noise of its own now.
		float shadowUndo[4];
		Framebuffer *mask = LCSShadow::CompositedMask(shadowUndo);
		if (mask) {
			draw->BindFramebufferAsTexture(mask, 2, Aspect::COLOR_BIT, 0);
		}
		SamplerState *samplers[3] = { s_sampler, s_sampler, mask ? s_sampler : s_wrapSampler };
		draw->BindSamplerStates(0, 3, samplers);
		FillUniforms(&ub, false);
		if (mask) {
			memcpy(ub.shadowUndo, shadowUndo, sizeof(ub.shadowUndo));
		}
		draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
		for (const Batch &batch : s_batches) {
			if (batch.solidIndexCount < 3) {
				continue;
			}
			draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3,
				batch.vertexCount,
				s_solidIndices.data() + batch.firstSolidIndex, batch.solidIndexCount);
		}
	}

	if (s_settings.water) {
		// The floor heights are encoded across two channels and must not be filtered. And the ripple
		// map back in slot 2, where the roads may have left the shadow mask.
		draw->BindFramebufferAsTexture(s_floorFbo, 1, Aspect::COLOR_BIT, 0);
		draw->BindTextures(2, 1, &s_normalTex);
		SamplerState *samplers[3] = { s_sampler, s_pointSampler, s_wrapSampler };
		draw->BindSamplerStates(0, 3, samplers);
		FillUniforms(&ub, true);
		draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));
		for (const Batch &batch : s_batches) {
			if (batch.waterIndexCount < 3) {
				continue;
			}
			draw->DrawIndexedUP(s_positions.data() + (size_t)batch.firstVertex * 3,
				batch.vertexCount,
				s_waterIndices.data() + batch.firstWaterIndex, batch.waterIndexCount);
		}
	}
	s_shadeRendered = true;
}

static void RenderComposite(Draw::DrawContext *draw, Draw::Framebuffer *target,
	int width, int height) {
	if (!s_shadeRendered || !s_surfaceFbo || !target) {
		return;
	}
	using namespace Draw;
	draw->BindFramebufferAsRenderTarget(target,
		{ RPAction::KEEP, RPAction::KEEP, RPAction::KEEP, 0, 0.0f, 0, "lcs_water_composite" },
		"lcs_water_composite");
	draw->BindFramebufferAsTexture(s_surfaceFbo, 0, Aspect::COLOR_BIT, 0);
	draw->BindSamplerStates(0, 1, &s_sampler);

	Viewport viewport{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	draw->SetViewport(viewport);
	draw->SetScissorRect(0, 0, width, height);
	draw->BindPipeline(s_compositePipeline);

	CompositeUB ub;
	ub.params[0] = 1.0f;
	ub.params[1] = 0.0f;
	ub.params[2] = 0.0f;
	ub.params[3] = 0.0f;
	draw->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

	static const float kFullscreenTriangle[6] = {
		-1.0f, -1.0f,
		 3.0f, -1.0f,
		-1.0f,  3.0f,
	};
	draw->DrawUP(kFullscreenTriangle, 3);
}

static void MeasureReflection();

// How much of the render target the game actually draws into, in the target's own pixels. The same
// question as LCSShadow's DrawnArea, answered the same way - see there for why it is not simply the
// whole target.
static void DrawnArea(const ViewportCapture &vp, int bufferWidth, int bufferHeight,
	int fbWidth, int fbHeight, int *drawnW, int *drawnH) {
	*drawnW = fbWidth;
	*drawnH = fbHeight;
	if (bufferWidth > 0 && bufferHeight > 0 && vp.rtWidth > 0.0f && vp.rtHeight > 0.0f) {
		*drawnW = std::min(fbWidth, (int)(vp.rtWidth * (float)fbWidth / (float)bufferWidth + 0.5f));
		*drawnH = std::min(fbHeight, (int)(vp.rtHeight * (float)fbHeight / (float)bufferHeight + 0.5f));
	}
	if (*drawnW <= 0) *drawnW = fbWidth;
	if (*drawnH <= 0) *drawnH = fbHeight;
}

bool OnFlush(Draw::DrawContext *draw, bool through, Draw::Framebuffer *target,
	int bufferWidth, int bufferHeight) {
	if (!g_active || s_frameShaded || !through || !draw || !target) {
		return false;
	}
	if (s_current.draws == 0 || s_batches.empty()) {
		return false;
	}
	if (gstate_c.curRTWidth < 480 || gstate_c.curRTHeight < 272) {
		return false;
	}
	// LCS: not the seam. LCS draws its world in pieces (docs/lcs/anti-aliasing.md), and the first
	// 2D draw after the far sea is a sprite with the sea's own texture: after it come the near sea
	// (one 300-vertex mesh in front of the far 128-unit squares) and all the land. Measured with the
	// probe from the shore: run there, the passes shaded the far sea and nothing in front of it.
	// LCSShadow learns the same draw as an early seam (BeginFrame).
	if (s_waterTexture != 0 && gstate.isTextureMapEnabled() && gstate.getTextureAddress(0) == s_waterTexture) {
		return false;
	}
	s_frameShaded = true;

	// BEFORE the early-out below, and that is not tidiness. The road map cannot be built until the
	// offset is known, the wet pass will not run until the road map exists, and the early-out
	// fires when neither half has anything to draw - so deriving the offset after it is a
	// deadlock: dry inland roads never get a map, so they never get wet, so the offset is never
	// derived. It costs two bounds-checked reads and needs nothing but the view matrix.
	UpdateWorldOffset();

	// Nothing to paint. Both halves are off, or there is no sea in view and the roads are dry -
	// which is most of this game, so it is worth getting out before allocating a framebuffer.
	//
	// The road half is gated on the LAGGED wetness rather than on the rain, which is the whole
	// point of it: the pass has to keep running for a minute and a half after the rain stops or
	// there would be nothing to watch drying.
	const bool wantWater = s_settings.water && !s_waterIndices.empty();
	const bool wantWet = s_settings.wetRoads && s_roadValid && s_wetness > 0.002f &&
		!s_solidIndices.empty();
	if (!wantWater && !wantWet) {
		return false;
	}

	ComputeView();
	if (!s_viewValid) {
		return false;
	}
	MeasureReflection();

	int fbWidth = 0, fbHeight = 0;
	draw->GetFramebufferDimensions(target, &fbWidth, &fbHeight);
	if (fbWidth <= 0 || fbHeight <= 0) {
		return false;
	}

	float scale = s_settings.surfaceScale;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 1.0f) scale = 1.0f;
	int width = (int)((float)fbWidth * scale + 0.5f);
	int height = (int)((float)fbHeight * scale + 0.5f);
	if (width <= 0) width = 1;
	if (height <= 0) height = 1;

	// The surface buffer's viewport has to cover the same fraction of it that the game's viewport
	// covers of its render target, or the composite - which maps the whole buffer over the whole
	// target - lands offset.
	int drawnW = fbWidth, drawnH = fbHeight;
	DrawnArea(s_frameViewport, bufferWidth, bufferHeight, fbWidth, fbHeight, &drawnW, &drawnH);
	int viewW = (int)((float)drawnW * scale + 0.5f);
	int viewH = (int)((float)drawnH * scale + 0.5f);
	if (viewW <= 0 || viewW > width) viewW = width;
	if (viewH <= 0 || viewH > height) viewH = height;

	RenderSurface(draw, target, width, height, viewW, viewH, drawnW, drawnH);
	RenderComposite(draw, target, fbWidth, fbHeight);
	return true;
}

// ---------------------------------------------------------------------------------------------
// The probe - see the header
// ---------------------------------------------------------------------------------------------

struct DrawRow {
	u16 prim;
	u16 verts;
	u8 blendEnabled, funcA, funcB, blendEq, alphaTestFn;
	u8 depthWrite, depthTest, fullAlpha;
	u8 hasNormals, hasColour, hasUV, lighting, texEnabled, cullMode;
	u32 texAddr;
	u16 texW, texH;
	u8 texFmt;
	int rtW, rtH;
	float min[3];
	float max[3];
	float flatFraction;
};

static std::vector<DrawRow> s_rows;
static int s_frame;
static int s_probeFrame = -1;
static bool s_probeDone;

static void ArmProbeFromEnvironment() {
	s_probeFrame = -1;
	s_probeDone = false;
	const char *value = getenv("LCS_WATER_PROBE");
	if (value) {
		s_probeFrame = atoi(value);
	}
	s_probeArmed = s_probeFrame >= 0;
	if (s_probeArmed) {
		NOTICE_LOG(Log::G3D, "LCS water: probe armed for host frame %d", s_probeFrame);
	}

	// The other half of the probe. Road wetness only appears when it is raining, and waiting for
	// the weather is not a way to test a renderer - so this forces the rain level from the same
	// place the probe is armed from, with nobody at the keyboard. It sets the ordinary
	// rainOverride setting, so the menu row and this are the same lever.
	if (const char *rain = getenv("LCS_WATER_RAIN")) {
		s_settings.rainOverride = (float)atof(rain);
		NOTICE_LOG(Log::G3D, "LCS water: rain forced to %.2f", s_settings.rainOverride);
	}
	if (const char *view = getenv("LCS_WATER_DEBUG")) {
		s_settings.debugView = atoi(view);
		NOTICE_LOG(Log::G3D, "LCS water: debug view %d", s_settings.debugView);
	}
}

static void ProbeNoteDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount,
	const u8 *decoded, int numDecodedVerts, int stride, int posOffset, const float world[12]) {
	if (s_probeDone || s_frame != s_probeFrame || gstate.isModeThrough() || numDecodedVerts <= 0) {
		return;
	}
	DrawRow r;
	memset(&r, 0, sizeof(r));
	for (int c = 0; c < 3; c++) {
		r.min[c] = 1e30f;
		r.max[c] = -1e30f;
	}
	int onPlane = 0;
	for (int i = 0; i < numDecodedVerts; i++) {
		const float *p = (const float *)(decoded + (size_t)i * stride + posOffset);
		const float v[3] = {
			p[0] * world[0] + p[1] * world[3] + p[2] * world[6] + world[9],
			p[0] * world[1] + p[1] * world[4] + p[2] * world[7] + world[10],
			p[0] * world[2] + p[1] * world[5] + p[2] * world[8] + world[11],
		};
		for (int c = 0; c < 3; c++) {
			if (v[c] < r.min[c]) r.min[c] = v[c];
			if (v[c] > r.max[c]) r.max[c] = v[c];
		}
	}
	for (int i = 0; i < numDecodedVerts; i++) {
		const float *p = (const float *)(decoded + (size_t)i * stride + posOffset);
		const float z = p[0] * world[2] + p[1] * world[5] + p[2] * world[8] + world[11];
		if (z - r.min[kUpAxis] < 0.05f) {
			onPlane++;
		}
	}
	r.flatFraction = (float)onPlane / (float)numDecodedVerts;

	r.prim = (u16)prim;
	r.verts = (u16)(vertexCount > 65535 ? 65535 : vertexCount);
	r.blendEnabled = gstate.isAlphaBlendEnabled() ? 1 : 0;
	r.funcA = (u8)gstate.getBlendFuncA();
	r.funcB = (u8)gstate.getBlendFuncB();
	r.blendEq = (u8)gstate.getBlendEq();
	r.alphaTestFn = gstate.isAlphaTestEnabled() ? (u8)gstate.getAlphaTestFunction() : 0xff;
	r.depthWrite = gstate.isDepthWriteEnabled() ? 1 : 0;
	r.depthTest = gstate.isDepthTestEnabled() ? 1 : 0;
	r.fullAlpha = gstate_c.vertexFullAlpha ? 1 : 0;
	r.hasNormals = (vertTypeID & GE_VTYPE_NRM_MASK) != GE_VTYPE_NRM_NONE ? 1 : 0;
	r.hasColour = (vertTypeID & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE ? 1 : 0;
	r.hasUV = (vertTypeID & GE_VTYPE_TC_MASK) != GE_VTYPE_TC_NONE ? 1 : 0;
	r.lighting = gstate.isLightingEnabled() ? 1 : 0;
	r.texEnabled = gstate.isTextureMapEnabled() ? 1 : 0;
	r.cullMode = (u8)gstate.getCullMode();
	r.texAddr = r.texEnabled ? gstate.getTextureAddress(0) : 0;
	r.texW = r.texEnabled ? (u16)gstate.getTextureWidth(0) : 0;
	r.texH = r.texEnabled ? (u16)gstate.getTextureHeight(0) : 0;
	r.texFmt = r.texEnabled ? (u8)gstate.getTextureFormat() : 0xff;
	r.rtW = gstate_c.curRTWidth;
	r.rtH = gstate_c.curRTHeight;
	s_rows.push_back(r);
}

static void FlushProbe() {
	NOTICE_LOG(Log::G3D, "LCS water probe: frame %d, %d 3D draws", s_frame, (int)s_rows.size());
	// The two positions that say whether the road mask can be built at all. The capture arrives
	// in the space the GE is fed; the road graph lives in the game's own world space; on foot the
	// camera sits a few units from the player, so agreement to within about ten units means the
	// two spaces are the same one.
	NOTICE_LOG(Log::G3D, "LCS water probe: camera(GE) %.1f %.1f %.1f  player(world) %.1f %.1f %.1f%s",
		s_current.cameraPos[0], s_current.cameraPos[1], s_current.cameraPos[2],
		s_current.playerPos[0], s_current.playerPos[1], s_current.playerPos[2],
		s_current.playerValid ? "" : "  (player unreadable)");
	NOTICE_LOG(Log::G3D, "LCS water probe: water texture %08x plane z %.2f, rain %.3f, roads %d links",
		s_current.textureAddr, s_current.planeZ, s_current.rain, s_current.roadLinks);
	for (size_t i = 0; i < s_rows.size(); i++) {
		const DrawRow &r = s_rows[i];
		const float spanX = r.max[0] - r.min[0];
		const float spanY = r.max[1] - r.min[1];
		const float spanZ = r.max[kUpAxis] - r.min[kUpAxis];
		NOTICE_LOG(Log::G3D,
			"W %4d p%d v%-5d b%d,%d,%d,%d a%d zw%d zt%d fa%d n%d c%d t%d l%d "
			"tex %08x f%d %dx%d cull%d rt %dx%d "
			"min %.1f %.1f %.2f max %.1f %.1f %.2f span %.1f dz %.3f flat %.2f",
			(int)i, r.prim, r.verts,
			r.blendEnabled, r.funcA, r.funcB, r.blendEq, r.alphaTestFn,
			r.depthWrite, r.depthTest, r.fullAlpha,
			r.hasNormals, r.hasColour, r.hasUV, r.lighting,
			r.texAddr, r.texFmt, r.texW, r.texH, r.cullMode, r.rtW, r.rtH,
			r.min[0], r.min[1], r.min[2], r.max[0], r.max[1], r.max[2],
			spanX > spanY ? spanX : spanY, spanZ, r.flatFraction);
	}
	NOTICE_LOG(Log::G3D, "LCS water probe: end of frame %d", s_frame);
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

static void RefreshAvailability() {
	g_available = LCS::IsActive();
}

// LCS: the menu's Water quality row, followed from here at every BeginFrame rather than pushed by
// the menu (Core/LCS has no renderer dependency), as LCSShadow follows its rows: applied when the
// row moves, and the gate read again every frame, because LCS::IsActive turns on when the EBOOT
// loads, after the draw engine is built.
static int s_followedWater = -1;

static void FollowSettings() {
	// Not LCS (or no longer): nothing to follow. Off if the game has just stopped.
	if (!LCS::IsActive()) {
		if (g_active) {
			SetEnabled(s_enabled);
		}
		return;
	}
	const int mode = LCS::Settings().water;
	if (mode != s_followedWater) {
		s_followedWater = mode;
		s_settings.water = mode != LCS::kLCSWaterOff;
		s_settings.wetRoads = mode == LCS::kLCSWaterSeaAndRoads;
		// Off switches the module off entirely, so the capture stops too.
		SetEnabled(mode != LCS::kLCSWaterOff);
		NOTICE_LOG(Log::G3D, "LCS water: quality %d, %s", mode, g_active ? "on" : "off");
	} else if (!g_available) {
		SetEnabled(s_enabled);
		NOTICE_LOG(Log::G3D, "LCS water: %s", g_active ? "on" : "off");
	}
}

void Init() {
	s_followedWater = -1;
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
	s_positions.clear();
	s_waterIndices.clear();
	s_solidIndices.clear();
	s_batches.clear();
	s_rows.clear();
	s_learn.clear();
	s_waterTexture = 0;
	s_waterPlaneZ = 0.0f;
	s_framesSinceWater = 0;
	s_wetness = 0.0f;
	s_rainNorm = 0.0f;
	s_rippleRain = 0.0f;
	s_lastRainNorm = 0.0f;
	s_haveLastRain = false;
	s_lastWetnessTime = 0.0;
	s_haveViewMatrix = false;
	s_haveLastCameraPos = false;
	s_haveLastCameraWorld = false;
	s_frameViewInvValid = false;
	s_geOffsetValid = false;
	s_geOffset[0] = 0.0f;
	s_geOffset[1] = 0.0f;
	s_viewValid = false;
	s_frameShaded = false;
	s_shadeRendered = false;
	s_roadValid = false;
	s_roadDirty = true;
	s_roadSpan = s_settings.roadMapSpan;
	s_roadCentre[0] = 0.0f;
	s_roadCentre[1] = 0.0f;
	s_frame = 0;
	ArmProbeFromEnvironment();

	// LCS::IsActive is the disc, version and EBOOT check as well.
	RefreshAvailability();
	g_active = g_available && s_enabled;
}

void SetEnabled(bool enabled) {
	s_enabled = enabled;
	// Re-read rather than latched at Init: the draw engine is constructed before the settings
	// file has necessarily been read for this disc, and latching there is how a feature comes
	// back from the ini switched on and inert.
	RefreshAvailability();
	g_active = g_available && s_enabled;
	if (enabled) {
		s_setupFailed = false;
	}
	if (!g_active) {
		s_positions.clear();
		s_waterIndices.clear();
		s_solidIndices.clear();
		s_batches.clear();
		memset(&s_current, 0, sizeof(s_current));
		memset(&s_published, 0, sizeof(s_published));
	}
}

bool IsEnabled() {
	return s_enabled;
}

void Shutdown() {
	ReleasePipelines();
	ReleaseSizedResources();
	ReleaseRoadTexture();
	s_setupFailed = false;
	g_available = false;
	g_active = false;
	s_positions.clear();
	s_waterIndices.clear();
	s_solidIndices.clear();
	s_batches.clear();
	s_rows.clear();
	s_learn.clear();
	s_wetness = 0.0f;
	s_rainNorm = 0.0f;
	s_rippleRain = 0.0f;
	s_lastRainNorm = 0.0f;
	s_haveLastRain = false;
	s_lastWetnessTime = 0.0;
	s_roadPixels.clear();
	s_roadValid = false;
	memset(&s_current, 0, sizeof(s_current));
	memset(&s_published, 0, sizeof(s_published));
}

void DeviceLost() {
	ReleasePipelines();
	ReleaseSizedResources();
	ReleaseRoadTexture();
}

// The lag between rain and wet roads, and the step that skips it.
//
// Linear rather than exponential on purpose: "thirty seconds to soak" is a sentence a player can
// check with a stopwatch, and an exponential's time constant is not. The two directions are
// separate rates because they are separate physical processes - a road wets from rain landing on
// it and dries by evaporating and draining, and the second is much the slower.
static void UpdateWetness() {
	const double now = time_now_d();
	float dt = s_lastWetnessTime > 0.0 ? (float)(now - s_lastWetnessTime) : 0.0f;
	s_lastWetnessTime = now;
	// A loading screen, a savestate or a breakpoint can leave an arbitrary gap here, and letting
	// one through would soak or dry the whole city in a single frame.
	if (dt < 0.0f) dt = 0.0f;
	if (dt > 0.25f) dt = 0.25f;

	float rain = s_settings.rainOverride >= 0.0f ? s_settings.rainOverride
		: (s_current.weatherValid ? s_current.rain / kRainFullScale : 0.0f);
	if (!(rain > 0.0f)) {
		rain = 0.0f;   // also catches NaN
	} else if (rain > 1.0f) {
		rain = 1.0f;
	}
	s_rainNorm = rain;

	// NO step detection. An earlier version snapped the wetness whenever the rain jumped, on the
	// grounds that a forced weather is a decision rather than weather arriving - and it was wrong
	// about what that should mean on screen. Reported from play: "when the rain starts, the
	// puddles are covering 100% of the road; it should appear gradually just like the disappearance
	// does". Puddles FILLING is as much of the effect as puddles draining, and skipping it threw
	// half the feature away in exchange for not waiting.
	//
	// So the cheat's job is to start the RAIN, not to skip the soak. SoakNow is still there for
	// the debugger button, which is where impatience belongs.
	s_current.wetnessSnapped = false;
	s_lastRainNorm = rain;
	s_haveLastRain = true;

	if (s_wetness < rain) {
		const float rate = s_settings.wetSeconds > 0.01f ? dt / s_settings.wetSeconds : 1.0f;
		s_wetness += rate;
		if (s_wetness > rain) s_wetness = rain;
	} else if (s_wetness > rain) {
		const float rate = s_settings.drySeconds > 0.01f ? dt / s_settings.drySeconds : 1.0f;
		s_wetness -= rate;
		if (s_wetness < rain) s_wetness = rain;
	}
	if (s_wetness < 0.0f) s_wetness = 0.0f;
	if (s_wetness > 1.0f) s_wetness = 1.0f;

	// The droplet rings follow the LIVE rain, not the wetness - "it has stopped raining and the
	// road is still wet" is exactly the state where rings would be wrong. Given a small lag of
	// their own so a forced weather change fades them rather than cutting them off mid-ring.
	const float rippleRate = dt / 1.5f;
	if (s_rippleRain < rain) {
		s_rippleRain += rippleRate;
		if (s_rippleRain > rain) s_rippleRain = rain;
	} else {
		s_rippleRain -= rippleRate;
		if (s_rippleRain < rain) s_rippleRain = rain;
	}
}

// How reflective the sea actually came out.
//
// "Too reflective" is a complaint about a distribution, and the distribution is knowable: the
// Fresnel term the shader computes is dominated by the angle between the view and the surface,
// and the surface is a plane. So this walks the frame's own captured water vertices, computes the
// flat-water Fresnel at each, and reports the middle of the distribution and the share of it that
// is more than half mirror. The wave normal moves each sample by up to about ten degrees either
// way, which widens the distribution without moving its centre - so this is the centre, measured,
// rather than the exact per-pixel value.
static void MeasureReflection() {
	s_current.reflectionMeasured = false;
	s_current.medianFresnel = 0.0f;
	s_current.mirrorFraction = 0.0f;
	s_current.reflectionSamples = 0;
	if (!s_haveViewMatrix || s_waterIndices.empty() || s_batches.empty()) {
		return;
	}

	static std::vector<float> fresnels;
	static std::vector<float> raws;
	fresnels.clear();
	raws.clear();
	const float f0 = s_settings.fresnelF0;
	// Every vertex would be tens of thousands of pow() calls for one diagnostic number; a stride
	// keeps it to a few hundred and the median of a few hundred is the median.
	for (const Batch &batch : s_batches) {
		for (int i = 0; i + 2 < batch.waterIndexCount; i += 33) {
			const int idx = s_waterIndices[(size_t)batch.firstWaterIndex + i];
			const float *p = s_positions.data() + ((size_t)batch.firstVertex + idx) * 3;
			const float to[3] = {
				s_frameCameraPos[0] - p[0],
				s_frameCameraPos[1] - p[1],
				s_frameCameraPos[2] - p[2],
			};
			const float len = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
			if (len < 0.001f) {
				continue;
			}
			// dot(N, V) with N straight up is just the view vector's own z.
			float ndv = to[kUpAxis] / len;
			if (ndv < 0.0f) ndv = -ndv;
			if (ndv > 1.0f) ndv = 1.0f;
			const float one = 1.0f - ndv;
			const float raw = f0 + (1.0f - f0) * one * one * one * one * one;
			raws.push_back(raw);
			float f = raw * s_settings.reflectionStrength;
			if (f > s_settings.maxReflection) {
				f = s_settings.maxReflection;
			}
			fresnels.push_back(f);
		}
	}
	if (fresnels.empty()) {
		return;
	}
	std::sort(fresnels.begin(), fresnels.end());
	s_current.reflectionMeasured = true;
	s_current.reflectionSamples = (int)fresnels.size();
	s_current.medianFresnel = fresnels[fresnels.size() / 2];
	size_t mirror = 0;
	for (float f : fresnels) {
		if (f > 0.5f) {
			mirror++;
		}
	}
	s_current.mirrorFraction = (float)mirror / (float)fresnels.size();

	// The RAW distribution as well, once a second while the probe is armed. The effective numbers
	// above answer "how reflective is it now"; the raw ones answer "how reflective would any other
	// setting make it", which is the question a before-and-after needs and which a run under the
	// new defaults could not otherwise answer about the old ones.
	if (s_probeArmed && (s_frame % 60) == 0) {
		std::sort(raws.begin(), raws.end());
		size_t rawMirror = 0;
		for (float f : raws) {
			if (f > 0.5f) {
				rawMirror++;
			}
		}
		NOTICE_LOG(Log::G3D,
			"LCS water: sea Fresnel over %d samples - raw p10 %.3f p50 %.3f p90 %.3f, "
			"raw above half %.1f%% | effective p50 %.3f, above half %.1f%%",
			(int)raws.size(), raws[raws.size() / 10], raws[raws.size() / 2],
			raws[raws.size() * 9 / 10], 100.0f * (float)rawMirror / (float)raws.size(),
			s_current.medianFresnel, s_current.mirrorFraction * 100.0f);
	}
}

void BeginFrame(Draw::DrawContext *draw) {
	FollowSettings();
	if (!g_active) {
		return;
	}
	// A learned texture that has not drawn anything for a long time is stale - the level changed
	// under us, or the sea is simply not in view any more and the heap has moved on. Forgetting
	// costs one frame of relearning the next time water is on screen.
	if (s_waterTexture) {
		if (s_current.waterDraws == 0) {
			s_framesSinceWater++;
			if (s_framesSinceWater > kForgetWaterFrames) {
				ForgetWater("no water drawn for a long time");
			}
		} else {
			s_framesSinceWater = 0;
		}
	}

	s_current.textureAddr = s_waterTexture;
	s_current.planeZ = s_waterPlaneZ;
	s_current.framesSinceWater = s_framesSinceWater;
	s_current.roadValid = s_roadValid;
	s_current.roadCentre[0] = s_roadCentre[0];
	s_current.roadCentre[1] = s_roadCentre[1];

	// The probe writes the frame that has just ended, and it reads the summary fields above - so
	// it goes here, after they are filled in and before the counters are reset.
	if (!s_probeDone && s_frame == s_probeFrame && !s_rows.empty()) {
		FlushProbe();
		s_probeDone = true;
	}
	s_rows.clear();

	s_published = s_current;

	// The weather, and the player's position in the game's own world space. Both are one
	// bounds-checked read and both are read HERE rather than mid-frame, because this is the point
	// where nothing is half-drawn and the emu thread owns the memory either way.
	memset(&s_current, 0, sizeof(s_current));
	NoteGameSun();
	{
		const LCS::WeatherState w = LCS::ReadWeather();
		s_current.weatherValid = w.valid;
		s_current.rain = w.rain;
		s_current.weatherOld = w.oldType;
		s_current.weatherNew = w.newType;
	}
	UpdateWetness();
	s_current.rainNorm = s_rainNorm;
	s_current.wetness = s_wetness;
	{
		const std::optional<float> px = LCS::PlayerWorld(0);
		const std::optional<float> py = LCS::PlayerWorld(1);
		const std::optional<float> pz = LCS::PlayerWorld(2);
		if (px && py && pz) {
			s_current.playerValid = true;
			s_current.playerPos[0] = *px;
			s_current.playerPos[1] = *py;
			s_current.playerPos[2] = *pz;
		}
	}
	// The camera in the game's own world space. Only for centring the road map - the OFFSET that
	// crosses between the two spaces is derived at the seam instead, where both halves of it can
	// come from the same frame. See UpdateWorldOffset.
	{
		const std::optional<float> cx = LCS::CameraWorld(0);
		const std::optional<float> cy = LCS::CameraWorld(1);
		const std::optional<float> cz = LCS::CameraWorld(2);
		if (cx && cy && cz) {
			s_current.camWorldValid = true;
			s_current.camWorld[0] = *cx;
			s_current.camWorld[1] = *cy;
			s_current.camWorld[2] = *cz;
		}
	}
	s_current.geOffset[0] = s_geOffset[0];
	s_current.geOffset[1] = s_geOffset[1];

	// One line a second while the probe is armed, because "is the road mask in the right place" is
	// a question about a number that moves, and a single frame of it says nothing.
	//
	// The second half evaluates the mask on the CPU exactly where the player is standing, and
	// prints what the fragment shader would read there. That is the whole wetness calculation
	// short of the surface normal, so a dry road with the player standing on it is answered here
	// rather than by staring at a screenshot.
	if (s_probeFrame >= 0 && (s_frame % 60) == 0) {
		float cover = -1.0f, roadZ = 0.0f, best = 0.0f, bestZ = 0.0f;
		if (s_roadValid && !s_roadPixels.empty() && s_current.playerValid) {
			const float minX = s_roadCentre[0] - s_roadSpan * 0.5f;
			const float minY = s_roadCentre[1] - s_roadSpan * 0.5f;
			const float perUnit = (float)kRoadMapSize / s_roadSpan;
			const int px = (int)((s_current.playerPos[0] - minX) * perUnit);
			const int py = (int)((s_current.playerPos[1] - minY) * perUnit);
			const auto read = [&](int tx, int ty, float *z) -> float {
				if (tx < 0 || tx >= kRoadMapSize || ty < 0 || ty >= kRoadMapSize) {
					return -1.0f;
				}
				const u8 *texel = s_roadPixels.data() + ((size_t)ty * kRoadMapSize + tx) * 2;
				*z = s_roadCentreZ +
					((float)texel[1] / 255.0f * 2.0f - 1.0f) * kRoadHeightRange;
				return (float)texel[0] / 255.0f;
			};
			cover = read(px, py, &roadZ);
			// ...and the strongest road within ten metres, which is what separates "the mask has
			// no road here" from "he is standing on the pavement beside one".
			const int reachTexels = (int)(10.0f * perUnit);
			for (int dy = -reachTexels; dy <= reachTexels; dy++) {
				for (int dx = -reachTexels; dx <= reachTexels; dx++) {
					float z = 0.0f;
					const float v = read(px + dx, py + dy, &z);
					if (v > best) {
						best = v;
						bestZ = z;
					}
				}
			}
		}
		NOTICE_LOG(Log::G3D,
			"LCS water: camera GE %.1f %.1f %.1f  world %.1f %.1f %.1f  offset %.1f %.1f | "
			"player %.1f %.1f %.1f  cover %.2f roadZ %.2f  rain %.2f roadValid %d",
			s_lastCameraPos[0], s_lastCameraPos[1], s_lastCameraPos[2],
			s_current.camWorld[0], s_current.camWorld[1], s_current.camWorld[2],
			s_geOffset[0], s_geOffset[1],
			s_current.playerPos[0], s_current.playerPos[1], s_current.playerPos[2],
			cover, roadZ, s_rainNorm, s_roadValid ? 1 : 0);
		NOTICE_LOG(Log::G3D,
			"LCS water:   PASSES %s | last frame: %d water idx, %d solid idx, %d batches",
			s_setupFailed ? "FAILED TO SET UP - nothing is being drawn"
				: (s_shadeRendered ? "ran" : "did not run this frame"),
			(int)s_waterIndices.size(), (int)s_solidIndices.size(), (int)s_batches.size());
		{
			// People are the one thing whose absence from the depth pre-pass shows immediately -
			// the sea gets painted over them. This says whether they reach it, and if not, which
			// test ate them.
			char why[256];
			int at = 0;
			why[0] = 0;
			for (int i = 1; i < (int)Reject::Count && at < 200; i++) {
				if (s_published.skinnedRejected[i]) {
					at += snprintf(why + at, sizeof(why) - at, " %s=%d",
						RejectName((Reject)i), s_published.skinnedRejected[i]);
				}
			}
			NOTICE_LOG(Log::G3D, "LCS water:   skinned (people): %d seen, %d captured;%s",
				s_published.skinnedDraws, s_published.skinnedCaptured,
				why[0] ? why : " none rejected");
		}
		NOTICE_LOG(Log::G3D,
			"LCS water:   road map: %d rebuilds, last %.2f ms graph + %.2f ms rasterise "
			"(%.2f ms in the worst single frame) + %.2f ms upload | offset residual %.3f",
			s_published.roadRebuilds, s_published.graphMs, s_published.rasteriseMs,
			s_published.stepMs, s_published.uploadMs, s_published.offsetResidual);
		NOTICE_LOG(Log::G3D, "LCS water:   sun %s %.2f %.2f %.2f luminance %.2f",
			s_published.sunValid ? "found" : "NOT FOUND - no glint",
			s_published.sunDir[0], s_published.sunDir[1], s_published.sunDir[2],
			s_published.sunLuminance);
		NOTICE_LOG(Log::G3D,
			"LCS water:   wetness %.3f (rain %.2f, ripples %.2f)%s | best road within 10m %.2f "
			"at z %.2f (player z %.2f, centreZ %.2f), map centre %.1f %.1f",
			s_wetness, s_rainNorm, s_rippleRain,
			s_current.wetnessSnapped ? " SNAPPED" : "",
			best, bestZ, s_current.playerPos[2], s_roadCentreZ,
			s_roadCentre[0], s_roadCentre[1]);
	}

	// Rebuild the road mask when the player has left the middle of it. An eighth of the span is
	// far enough that this happens every few seconds at driving speed rather than every frame,
	// and near enough that the edge of the map is never in view.
	if (s_settings.wetRoads && s_current.camWorldValid && s_geOffsetValid) {
		const float span = s_settings.roadMapSpan;
		const float dx = s_current.camWorld[0] - s_roadCentre[0];
		const float dy = s_current.camWorld[1] - s_roadCentre[1];
		if (!s_building && (!s_roadValid || span != s_roadSpan ||
			fabsf(dx) > span * 0.125f || fabsf(dy) > span * 0.125f)) {
			BeginRoadBuild(s_current.camWorld[0], s_current.camWorld[1],
				s_current.camWorld[2], span);
			s_buildStartTime = time_now_d();
			s_buildMs = 0.0f;
		}
		if (s_building) {
			// A whole build is about 110k texels, so this is roughly eight frames at a few
			// tenths of a millisecond each. The car covers four metres in that time, against a
			// map that reaches 256 - and the old map stays live and correct throughout.
			const double t0 = time_now_d();
			const bool done = StepRoadBuild(kRoadBuildMsPerFrame);
			s_buildMs += (float)((time_now_d() - t0) * 1000.0);
			const float stepMs = (float)((time_now_d() - t0) * 1000.0);
			if (stepMs > s_worstStepMs) {
				s_worstStepMs = stepMs;
			}
			if (done) {
				s_lastRasteriseMs = s_buildMs;
				s_rebuilds++;
			}
		}
	}
	// Uploading it here rather than mid-frame: this is the one point in the frame with no render
	// pass open, and the rasterisation that dirties it has just run two lines above.
	if (draw && (s_settings.wetRoads || !s_roadTexture)) {
		const bool wasDirty = s_roadDirty || !s_roadTexture;
		const double t0 = time_now_d();
		EnsureRoadTexture(draw);
		if (wasDirty) {
			s_lastUploadMs = (float)((time_now_d() - t0) * 1000.0);
		}
	}
	s_current.graphMs = s_lastGraphMs;
	s_current.rasteriseMs = s_lastRasteriseMs;
	s_current.stepMs = s_worstStepMs;
	s_current.uploadMs = s_lastUploadMs;
	s_current.roadRebuilds = s_rebuilds;
	s_current.rebasesFollowed = s_rebasesFollowed;
	s_current.viewCorrectedDraws = s_viewCorrectedDraws;

	s_positions.clear();
	s_waterIndices.clear();
	s_solidIndices.clear();
	s_batches.clear();
	s_haveViewMatrix = false;
	s_viewValid = false;
	s_frameShaded = false;
	s_shadeRendered = false;
	s_frame++;
}

}  // namespace LCSWater
