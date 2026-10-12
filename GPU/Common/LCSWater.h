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

#pragma once

#include "Common/CommonTypes.h"
#include "GPU/ge_constants.h"

namespace Draw {
class DrawContext;
class Framebuffer;
}

// Wet surfaces - the sea, and rain on the roads - written for GTA: Vice City Stories and ported to
// Liberty City Stories.
//
// LCS: gated on LCS::IsActive (the disc, its version and the EBOOT's hash) and the menu's Water
// quality row. With either off, IsActive() is false, every entry point returns on its first
// line, and no other game executes a single instruction of this.
//
// ---------------------------------------------------------------------------------------------
// WHAT THE GAME ACTUALLY DRAWS, MEASURED BEFORE ANY OF THIS WAS WRITTEN
//
// There is no "this is water" bit in a display list, so the first thing built here was a probe
// that writes one frame's draw table to the log (see LCS_WATER_PROBE below). Standing on the
// grass at the west end of the Downtown bridge, looking across the water, the frame is 451 3D
// draws and the last sixteen of them are the sea:
//
//   * five 4-vertex strips, span 64 units, every vertex at z = 5.50 exactly, no normals, no
//     vertex colours, hardware lighting OFF - the far water sectors, one quad each;
//   * eight 1626-vertex triangle lists, span 32, z = 5.86..6.12 WITH normals - the near sectors,
//     which carry the game's own wave mesh;
//   * one 42-vertex piece at z = 5.50 again, span 128;
//   * and one 6330-vertex list spanning 2048 units - the whole map - at z = 2.50..5.00.
//
// All sixteen share one texture. That is what the classifier keys on, and the shape above is
// what the LEARNER looks for: a texture carrying several perfectly flat, unlit, normal-free,
// colour-free quads that all sit at the same z. The address is a heap address, so it is learned
// every session rather than named, exactly as LCSShadow learns the blob-shadow textures.
//
// Two things fall out of that measurement for free and both are used below: the sea is a PLANE at
// a known height, and the game applies no lighting to it whatsoever - so there is nothing to
// preserve except the colour, and everything else is ours to compute.
//
// ---------------------------------------------------------------------------------------------
// FOUR PASSES, ALL INSIDE THE FRAME THEY BELONG TO
//
//   1. A copy of the frame the game has just finished drawing, so the shading passes can READ the
//      colour they are about to change. That is what makes reflections possible at all, and it is
//      also what lets the sea keep the game's own time-of-day tint instead of being repainted a
//      colour that is wrong at dusk.
//   2. A depth pre-pass over the captured geometry, water and solid alike, in the camera's own
//      transform. This is the occlusion: a pier, a boat or the player in front of the sea wins
//      those pixels, and nothing is painted over them.
//   3. One shading pass, drawn twice with the same shader - once over the solid geometry for road
//      wetness, once over the water for the sea - with the depth test set to EQUAL so only the
//      surface that won the pre-pass is shaded.
//   4. One triangle compositing that over the game's framebuffer, premultiplied.
//
// Everything runs at the moment the frame turns from 3D to 2D, so the HUD drawn afterwards is
// untouched and nothing is a frame stale. See OnFlush.
//
// Reconstructing world position from the GAME's depth buffer instead of drawing the geometry
// again was rejected for the same reason LCSShadow rejected it: PPSSPP rewrites gl_Position.z on
// its way out, differently depending on backend and render state, and replicating a moving target
// would break in ways that look like water bugs. Drawing the captured geometry a second time
// hands the fragment shader an exact world position as a varying.
//
// ---------------------------------------------------------------------------------------------
// THE PROBE
//
// Still here, and still the first thing to reach for when the sea stops being recognised. Arm it
// with an environment variable:
//
//     LCS_WATER_PROBE=<host frame number>      e.g. LCS_WATER_PROBE=900
//
// counted in host frames since the module started. It writes one line per 3D draw and then goes
// quiet. Unset, it costs one integer compare a frame. It is an environment variable rather than a
// setting because it has to be armed before the frame it looks at, from a command line, with
// nobody at the keyboard. While it is armed, the camera's position in both spaces is logged once
// a second as well - that is the pair the road mask's alignment depends on.
//
// Two more of the same kind, for the half of this that only appears in bad weather:
//
//     LCS_WATER_RAIN=<0..1>       force the rain level, as the rainOverride setting does.
//                                 NORMALISED - the game's own Rain peaks at 0.5, see
//                                 kRainFullScale, and everything here works in 0..1.
//     LCS_WATER_DEBUG=<0..4>      pick a debug view, as the debugView setting does
//
// While any of the three is set, each rebuild of the road mask is also written next to the
// executable as `vcs_roadmask.pgm` - a plain greyscale image of the road network it is about
// to sample. "Is the mask right" and "is it in the right place" are two questions, and a
// picture of the mask on its own answers the first without the second getting in the way.

namespace LCSWater {

// Why a draw is not part of the scene this module cares about. The tests run in this order and
// the first failure wins, so the counts partition the frame exactly.
enum class Reject : u8 {
	None = 0,        // captured, as water or as an occluder
	Through,         // 2D: HUD, radar, menus
	OffscreenTarget, // a render-to-texture pass - its own camera
	Primitive,       // lines, points, sprites
	NoDepthWrite,    // particles, coronas, the vanilla blob shadow: not part of the solid scene
	Blended,         // blending that genuinely composites - glass and smoke should not occlude
	NearCamera,      // the game's full-screen overlays, which it draws as world geometry
	TooFewVerts,
	Count,
};

struct FrameStats {
	int draws;
	int waterDraws;
	int solidDraws;
	int waterVerts;
	int rejected[(int)Reject::Count];

	// The SKINNED draws specifically - which in this game means people, including the player.
	//
	// They are counted apart from everything else because they are the one thing whose absence
	// from the depth pre-pass is immediately obvious: a ped that does not occlude gets the sea
	// painted over him. Reported from play, looking at the character's front with water behind.
	// If `skinnedCaptured` is short of `skinnedDraws`, the breakdown says which test ate them.
	int skinnedDraws;
	int skinnedCaptured;
	int skinnedRejected[(int)Reject::Count];

	// The learner. `textureAddr` is zero until the sea has been recognised this session, and
	// `planeZ` is the height it was recognised at - which is also the number that says whether
	// the answer is sane, because a sea is not at z = 900.
	u32 textureAddr;
	float planeZ;
	int learnCandidates;   // draws that fit the water SHAPE this frame, whatever their texture
	int framesSinceWater;

	// The sun, as this module found it. Kept separate from LCSShadow's answer on purpose: the
	// two modules are independent and a player may run either without the other. If they ever
	// disagree, LCSShadow's is the one that has been argued with.
	bool sunValid;
	float sunDir[3];        // pointing TOWARDS the light
	float sunLuminance;

	// The camera, recovered from the view matrix, in the space the GE is fed.
	bool cameraValid;
	float cameraPos[3];

	// The same camera's worth of the game's OWN world space, read out of PSP memory: the
	// player's position. These two are printed side by side because the road mask is built in
	// world space and sampled in GE space, and nothing else says whether those are the same
	// space. On foot the third-person camera sits a few units from the player, so agreement to
	// within about ten units means they are.
	bool playerValid;
	float playerPos[3];

	// The camera in the game's OWN world space, read out of CCam, and the translation between the
	// two spaces that falls out of it: world = GE + geOffset. Measured rather than assumed - the
	// first run of the probe put the recovered camera at (14.0, -31.4) while the player stood at
	// (236.3, -133.6), which is a rebase of a couple of hundred units in XY and none at all in Z.
	// This is what lets a road network stored in world space be sampled by geometry that arrives
	// in the other one.
	bool camWorldValid;
	float camWorld[3];
	float geOffset[2];

	// How far the freshly derived offset sat from the latched one this frame. It should be a
	// few centimetres of noise; anything that grows with speed means the two halves of the
	// derivation are being taken from different frames again, and the puddles will swim.
	float offsetResidual;

	// Chunk swaps, counted since boot - see NoteDraw and PrepareViewCorrection: rebases of the GE
	// space followed rather than treated as a jump, and draws moved into the frame's own camera space.
	int rebasesFollowed;
	int viewCorrectedDraws;

	// The weather, straight from the game.
	//
	// `rain` is the raw value; `rainNorm` is it divided by the largest the game produces, which
	// was measured rather than assumed - see kRainFullScale. `wetness` is the lagged one the
	// puddles actually follow, and the gap between the last two IS the feature.
	bool weatherValid;
	float rain;
	float rainNorm;
	float wetness;
	bool wetnessSnapped;    // ...set this frame by a step change in the weather
	int weatherOld;
	int weatherNew;

	// The road mask: how many links went into it, and where it is centred.
	int roadLinks;
	int roadNodes;
	bool roadValid;
	float roadCentre[2];

	// What the last rebuild cost, on the emu thread, and how many there have been. A rebuild
	// happens every span/8 of travel - every few seconds while driving, which is exactly when
	// streaming stutter is being complained about, so it needs a number rather than a hunch.
	float graphMs;
	float rasteriseMs;   // the whole build, summed over the frames it is spread across
	float stepMs;        // ...and the worst any single frame has paid, high-water mark
	float uploadMs;
	int roadRebuilds;

	// How reflective the sea actually came out, measured over the frame's own water geometry
	// rather than reasoned about. See MeasureReflection: `medianFresnel` is the middle of the
	// distribution and `mirrorFraction` is the share of it that is more than half mirror, which
	// is the number "too reflective" is a complaint about.
	bool reflectionMeasured;
	float medianFresnel;
	float mirrorFraction;
	int reflectionSamples;
};

// Player-facing knobs.
struct Settings {
	// --- the sea ---
	bool water;

	// Wave periods per world unit, and how steep the resulting normal is. The game's own near
	// sectors already carry a wave MESH; this is the fine detail on top of it, which is the part
	// a 32-unit sector's worth of vertices cannot express.
	float waveScale;
	float waveAmplitude;
	float waveSpeed;

	// How far out the fine detail fades to a flat mirror. Without this the far sea aliases into
	// noise as the normal changes faster than a pixel.
	float detailFade;

	// How much of the sea's colour is ours rather than the game's. The pass starts from the pixel
	// the game drew - which is why the water still goes orange at sunset and grey in the rain
	// without anything here knowing the time - and mixes towards deepColour by this much.
	float deepMix;
	float deepColour[3];

	// Reflectance at normal incidence. Water is about 0.02; the Fresnel term does the rest.
	float fresnelF0;

	// The reflection is the frame itself, mirrored about the horizon. That is exact for a
	// distant shoreline and approximate for anything close, which is the right way round: what a
	// sea actually reflects is the sky and the far bank.
	float reflectionStrength;
	float reflectionSpread;   // how far the wave normal drags the reflection sideways
	float mirrorScale;        // 1.0 is a straight mirror about the horizon line

	// Water at a grazing angle really is a near-mirror - the Fresnel term is not wrong - but a
	// PIXEL-SHARP one is, and that is what reads as glass rather than as sea. Real water at any
	// distance is roughened by capillary waves smaller than a pixel, and the reflection it gives
	// back is smeared by them. These two are that, and between them they are most of the
	// difference between "reflective" and "a mirror":
	//
	//   reflectionBlur - how far the taps spread, in screen widths, at the detail-fade distance.
	//   maxReflection  - the ceiling the Fresnel term is clamped to. Physically this stands in
	//                    for the same roughness: a rough surface cannot reach the flat-water
	//                    reflectance at grazing incidence.
	float reflectionBlur;
	float maxReflection;

	float specularPower;
	float specularStrength;

	// --- depth, the shore and the fine detail ---
	//
	// The sea's depth under each pixel comes from the floor pass - the height of whatever solid
	// geometry the view ray meets beneath the surface - and it is what lets the shallows keep the
	// game's own turquoise and the sand show through, while open water still goes deep.
	//
	// How deep the water has to be before its colour is all deep colour, in world units along the
	// view ray. Red goes first and blue last, as it does in real water: the channels use 0.35, 0.7
	// and 1.0 of this.
	float extinctionDepth;
	// The foam where the water runs out onto the sand, and how deep the water may be for it to
	// form - the band is wider on a gently shelving beach and thinner against a sea wall.
	float shoreFoam;
	float foamDepth;
	// The fine ripples: a generated, tiling normal map sampled twice at different scales and
	// directions, in place of the regular sines that read as a pattern at range. Repeats per world
	// unit, and how far it tilts the surface.
	float normalMapScale;
	float normalMapStrength;

	// --- rain on the roads ---
	bool wetRoads;

	// Metres either side of a path link that count as road surface.
	//
	// Smaller than a road looks, and deliberately: the graph is the AI traffic network, so a
	// two-way street is TWO parallel lines of nodes about eight metres apart, and each of them
	// gets this width. Seven metres - which is what a street half-width measures - produced a
	// mask covering 26.6% of a 512-metre square and bleeding twenty metres past the kerb into
	// the canal. Four is a street.
	float roadHalfWidth;
	float roadMapSpan;        // how much world the mask covers, in units

	// -1 uses the game's own rain level. Anything else forces it, which is the only way to see
	// this working without waiting for the weather.
	float rainOverride;

	// How long the roads take to soak, and how long the puddles take to go once it stops. Not the
	// same number, and not close: a road wets from rain that has been falling for a while and
	// dries from evaporation and drainage, which are much slower. Seconds to travel the whole
	// 0..1 range.
	float wetSeconds;
	float drySeconds;

	// Where the water goes as it dries. A road is cambered, so the last of it sits either in the
	// gutter or along the crown - which of the two is decided per stretch of road, so a street
	// does not drain the same way for its whole length. In world units.
	float drainPatchSize;

	// The size of the puddles themselves, as periods per world unit. Drying does two things at
	// once: the wet band retreats towards the drain, and what is left breaks into patches, and
	// this is the second.
	float puddleScale;

	float wetDarkening;       // how much a wet road darkens, 1.0 = not at all
	float rippleScale;        // droplet impacts per world unit
	float rippleRate;         // impacts per second per cell
	float rippleStrength;

	// How bright a droplet's ring is in its own right, from any angle. rippleStrength only tilts the
	// surface normal, and a tilted normal only shows through the reflection and the sun's highlight:
	// looking down at a road the reflection is about two per cent of the colour and the highlight
	// needs the sun lined up, so the rings were visible only along the road at a low angle or into
	// the glint. Reported as the droplets appearing only at a certain camera angle.
	float rippleLight;

	// A road reflects, but it is not a lake, and the difference is almost entirely in these two.
	//
	// The reflection is the frame mirrored about the horizon, which is exact for a surface at eye
	// height and increasingly wrong for one at your feet - so a road handed the sea's mirror comes
	// back with palm trees standing full height in it, and reads as a flood. Compressing the
	// mirror towards the horizon is what turns that back into a wet surface: the same
	// approximation, told the truth about how far below the eye this surface is.
	float wetReflection;
	float wetMirrorScale;

	// How much of the road map's outer edge fades back to dry, as a fraction of its half width.
	// Without it the wetness ends in a line ruled straight across the street where the map does.
	float wetEdgeFade;

	// --- shared ---
	float surfaceScale;       // the shading buffer's size relative to the frame
	int debugView;            // 0 off, 1 wetness, 2 normals, 3 reflection source, 4 water mask, 5 depth
};

const FrameStats &LastFrameStats();
Settings &GetSettings();
const char *RejectName(Reject r);

// The shading buffer, for the debugger to preview. Null until the pass has run once.
Draw::Framebuffer *SurfaceBuffer();

// Read on every draw, so these live in the header to stay inlineable rather than costing a call
// per draw call. Two flags, not one: g_available is the compat flag, g_active is that AND the
// runtime switch, and it is the one the hot path tests.
extern bool g_available;
extern bool g_active;

inline bool IsAvailable() { return g_available; }
inline bool IsActive() { return g_active; }

void SetEnabled(bool enabled);
bool IsEnabled();

// Put the roads straight to fully wet, or straight to dry, without waiting for wetSeconds.
//
// The renderer calls SoakNow itself when the rain level takes a step - a cheat or a mission
// script forcing the weather, which is not weather arriving but a decision being made, and the
// ramp models water accumulating from rain that has been FALLING. Both are also buttons on the
// debugger tab, because a thing with a thirty-second time constant is otherwise slow to look at.
void SoakNow();
void DryNow();

void Init();
void Shutdown();

// Releases the GPU objects and nothing else, for when the draw context is going away. Each one is
// created on first use, so a restored device simply builds them again.
void DeviceLost();

// Per-host-frame. Publishes last frame's counts, rebuilds the road mask if the camera has moved
// far enough to need it, and flushes the probe. Renders nothing.
void BeginFrame(Draw::DrawContext *draw);

// Called from the draw engine for every draw that reaches the GPU, after vertexFullAlpha has
// settled and the indices are decoded - the same seam the shadow capture uses, and for the same
// reason: the blend test reads the former and the counts need the latter.
void NoteDraw(GEPrimitiveType prim, u32 vertTypeID, int vertexCount,
	const u8 *decoded, int numDecodedVerts, const u16 *indices, int indexCount,
	int stride, int posOffset, const float world[12]);

// Called from the draw engine at the top of every vertex flush, before it establishes any state.
// The first 2D batch of a frame that has captured any 3D is the seam between the world and the
// HUD, and that is when all four passes run. Returns true if anything was drawn, which is the
// caller's cue to rebind its own render target.
bool OnFlush(Draw::DrawContext *draw, bool through, Draw::Framebuffer *target,
	int bufferWidth, int bufferHeight);

}  // namespace LCSWater
