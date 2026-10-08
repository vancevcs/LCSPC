# LCS anti-aliasing (SMAA)

`GPU/Common/LCSSmaa.cpp`, a port of the VCS fork's `VCSSmaa` (its `docs/vcs/anti-aliasing.md` has
the design: why not a post shader, the three stages, how the middle one differs from real SMAA).
Setting `AntiAliasing` in `lcs.ini`, on by default; a box on the debugger's status line, with the
debug views beside it. Vulkan only.

## Where it runs

Hooked into `DrawEngineVulkan::Flush`: armed by a 3D flush into the game's own framebuffer (at
least 480x272), fired by the 2D flush after it, so it smooths the world and runs before the HUD.
`Init` / `Shutdown` / `DeviceLost` ride the draw engine's own, `BeginFrame` the host frame's
(`GPU_Vulkan::BeginHostFrame`).

**LCS draws its world in pieces.** The VCS rule - run at every 3D-to-2D turn - ran three times in
every host frame here (900 runs in 300 frames, steady): two half-drawn worlds smoothed for nothing,
and their pixels smoothed twice. The HUD comes after the last turn (in the weights view every HUD
element is drawn over the weights). So the turns are counted, and the pass runs from the turn the
previous frame ended on: once a frame (300 runs in 300 frames), at the HUD seam, and still at the
last turn of a frame that happens to have more.

## Looking at it

`LCS_SMAA_DEBUG=<n>` at boot, or the combo on the status line: 0 as it ships, 1 a split (left half
smoothed, right half as drawn, in one frame), 2 the blend weights (red horizontal edges, green
vertical). With the variable set, the runs per 300 host frames go to the log.

Measured on the car fixture at PPSSPP's 4x internal resolution (2048x1280): the weights view finds
the car's outline, the trees and the ground edges, and the HUD, radar and help text are untouched.
At 4x, downscaled to a 1080p window, the difference is small; SMAA earns more at 1x or 2x.

## Not checked

- The cost (not measured on any machine).
- What the two earlier 2D pieces between the world's parts are.
- Other backends: like the VCS passes, Vulkan only.
