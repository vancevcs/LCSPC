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

#pragma once

namespace Draw {
class DrawContext;
class Framebuffer;
}

// SMAA - subpixel morphological anti-aliasing - over the 3D scene of LCS, at the seam between the
// world and the HUD: the first 2D draw after the world, when the world is finished and the HUD has
// not started. So the edges of the city are smoothed and the HUD's text is not.
//
// A port of the VCS fork's VCSSmaa, unchanged but for the gate; its docs/vcs/anti-aliasing.md and
// this fork's docs/lcs/anti-aliasing.md say what was measured.
//
// WHY NOT A POST SHADER. PPSSPP's post-shader chain runs on the finished frame - HUD and all - and
// a pass in it sees only the pass before it, where SMAA's last stage needs both the original image
// and the weights the stage before it computed. A pass of our own has neither problem.
//
// THE THREE STAGES ARE SMAA's, and so is the arithmetic of the first and the last: luma edge
// detection with local contrast adaptation, and neighbourhood blending along whichever direction
// carries the larger weight. The middle stage differs in one way worth stating. Real SMAA reads the
// area a reconstructed silhouette covers out of a precomputed lookup texture (AreaTex); here it is
// computed in the shader for the orthogonal patterns - Z, L and U shapes, the MLAA cases, with the
// U split at its middle - from the same edge-end search. That is what SMAA's LOW and MEDIUM presets
// use the texture for; diagonal patterns and corner rounding (HIGH and ULTRA) are not done.
//
// Vulkan only: it is called from DrawEngineVulkan::Flush. LCS only: gated on LCS::IsActive and the
// AntiAliasing setting, so every other game never reaches it.

namespace LCSSmaa {

void Init();
void Shutdown();
void DeviceLost();

// Once per host frame, before any drawing. Disarms the pass and counts the frame's runs.
void BeginFrame();

// Whether it will run at all - the AntiAliasing setting, and this being LCS.
bool IsActive();

// For judging it, in the debugger's Status line or from LCS_SMAA_DEBUG at boot: 0 the pass as it
// ships, 1 a split screen - the left half anti-aliased, the right half as the game drew it, in one
// frame, which two separate screenshots never manage - and 2 the blend weights themselves (red the
// horizontal edges', green the vertical edges').
int &DebugView();

// At every flush; returns true when it bound framebuffers of its own, so the caller must rebind the
// game's. A 3D flush into the game's own target arms it; the next 2D flush into it runs the pass.
bool OnFlush(Draw::DrawContext *draw, bool through, Draw::Framebuffer *target,
	int bufferWidth, int bufferHeight);

}  // namespace LCSSmaa
