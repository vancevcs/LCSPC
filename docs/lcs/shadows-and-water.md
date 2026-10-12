# Shadows and water

`GPU/Common/LCSShadow.*` and `GPU/Common/LCSWater.*`, the VCS fork's `VCSShadow` and `VCSWater`
ported (its `docs/vcs/shadows.md` and `docs/vcs/water.md` have the design and every measurement
made on Vice City Stories). Sun shadows with ambient occlusion, and a shaded sea with roads that go
wet in the rain. Vulkan only, like SMAA. This file is what is different on Liberty City Stories and
how that was measured.

## Settings

Three rows on the menu's Display setup -> Advanced page, saved in lcs.ini:

| row | key | positions | default |
|---|---|---|---|
| Shadow quality | `Shadows` | LOW none, MEDIUM people and vehicles, HIGH props as well, ULTRA the whole city | HIGH |
| Ambient occlusion | `AmbientOcclusion` | on / off; live only while there are shadows | off |
| Water quality | `Water` | LOW the PSP's own, MEDIUM a shaded sea, HIGH wet roads in the rain as well | HIGH |

The VCS fork's ladders and defaults. Core/LCS has no renderer dependency, so nothing pushes these
into the renderer: each pass's `FollowSettings` reads `LCS::Settings()` at its `BeginFrame` and
applies a row when it moves (the debugger's own edits to the same fields stand until then).
`FollowSettings` also re-reads the gate every frame, because `LCS::IsActive` turns on when the EBOOT
loads, which is after the draw engine is built.

## Where they run

Each a few lines marked Fork-specific:

| what | where |
|---|---|
| `Init`, `Shutdown`, `DeviceLost` | `DrawEngineVulkan`'s constructor, destructor and `DestroyDeviceObjects`, beside LCSSmaa's |
| the passes | the top of `DrawEngineVulkan::Flush`: shadows, then water (shaded from a frame that has its shadows), then SMAA |
| the capture | both decode sites in `Flush`: `LCSWater::NoteDraw` and `LCSShadow::CaptureDraw` (one call where VCS had its block inline), and `NoteCutoutTexture` once the texture is applied |
| predecode | with either pass on, every draw takes the skinning path's decode into ordinary memory: the capture reads positions back, and the push buffer is write-combined |
| `BeginFrame` | `GPU_Vulkan::BeginHostFrame` |
| `DrawIndexedUP` | `Common/GPU/Vulkan/thin3d_vulkan.cpp`: copies `idata`, not `vdata`, into the index buffer |

The last is an upstream bug the VCS fork found and fixed: nothing in PPSSPP calls `DrawIndexedUP`, so
these passes are its first user, and vertex bytes read as indices still draw a mesh, of garbage
triangles that cover the screen many times. Here it was **30 fps to 8 with shadows on at any
quality**, water alone costing nothing; with the fix, 30 at every quality (below).

## What is different on LCS

### The sun

VCS hands the GE its time-of-day directional on light channel 0, and VCSShadow takes the sun from
there. **LCS hands it none.** Logged over a whole day (the clock written hour by hour): channel 0 is
(0.052, 0.999, 0.000) full white at every hour, on 2 to 4 draws a frame (Toni), channel 1
(0, 0, -1); the world is lit by vertex colours. So VCSShadow mirrored that fixed light to a "moon" at
noon, and its mask came out black across the ground and the tree trunks.

The sun comes from the clock instead (`LCS::ReadSun`): GTA III's `CTimeCycle` formula,
normalise(sin a, 1, 0.2 - cos a) with a the day's angle from midnight. Confirmed against the game's
own directional light frame, an RwFrame on the heap (`0x09aa20b0` that session) found by dumping RAM
at six hours and keeping what moved with the clock: its "at" row is exactly the negated formula at
all eleven hours read (12:00 (0.005, -0.640, -0.768) against (0, 0.640, 0.768); 06:00
(-0.700, -0.700, -0.140) against (0.700, 0.700, 0.140); midnight (-0.003, -0.781, 0.625) against
(0, 0.781, -0.625)). Below the horizon it is mirrored to a moon, as VCSShadow does with a GE light.
The water's sun glint uses the same sun.

The strength through the day follows the time cycle's light colour (`kLCSTimeCycleLight`, three
floats), where VCSShadow used the GE light's diffuse colour:

| hour | 0 | 3 | 6 | 8 | 10 | 12 | 14 | 16 | 18 | 20 | 22 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| red | 0.361 | 0.444 | 0.408 | 0.547 | 0.775 | 0.999 | 0.865 | 0.735 | 0.566 | 0.400 | 0.380 |

The clock (`kLCSClockHours`, `kLCSClockMinutes`) was found as a byte stepping once a real second,
with the hour beside it; written live, the HUD clock and the sky follow, which is how every time of
day here was tested.

### The sea

The probe (`LCS_WATER_PROBE`) from the park and from the north shore: the sea is 128-unit squares
(256 further out) at **z -0.5**, one texture (learned per session; `097eaa40` both runs), unlit,
**with normals and vertex colours**, and **blended** with fixed factors (GE blend 1,10,10). In front
of the squares, one 300-vertex mesh is the near sea. Every road and pavement LCS draws is lit. So:

- the learner drops VCS's two clauses against normals and colours (they kept VCS's near wave
  sectors out of learning, and would refuse every draw of LCS's sea); unlit, flat, wide, textured
  and three draws at one height is the sea here;
- draws are offered to the learner before the blend test, which files genuine transparency away
  and took LCS's sea with it.

Logged: "texture 097eaa40 is the sea, plane z = -0.50", within a frame of boot.

### The world in pieces

LCS draws the far sea, then a **2D sprite with the sea's own texture**, then the near sea and all the
land (anti-aliasing.md counts three 3D-to-2D turns a frame). The water pass runs at the first 2D
draw after 3D, so it shaded the far squares and nothing in front of them (the water-mask debug view
showed a strip at the horizon). A 2D draw with the learned sea texture is not the seam now; the mask
then covers the whole sea. The shadow pass needed nothing: its early-seam learning (VCS's, for a
draw it met in the rain) learns that sprite on its own ("passed over the early 2D draw (097eaa40)").

### Rain

`CWeather::Rain` is `0x08b5e180`: forced weather 2 and 5 give 0.5, and 0, 1 and 3 give 0, within a
second of the force, which is the VCS fork's measurement of its own Rain to the value. It drives the
wet roads, the droplets and the shadows' fade in rain. Forced rain on Hepburn Heights: on HIGH the
street goes dark and glossy with rain rings and the parked cars reflected; MEDIUM leaves it dry; the
shadows faded out.

### The GE space

LCS feeds the GE in more than one space within a frame: the sea in world space, the land offset by a
streaming origin ((1437.5, -213.9) at the park, (1375.0, -105.6) on the shore). Both passes' "moved
into the frame's space" correction (VCS's, for its chunk rebases) carries every such draw over; it
logs once per 5000. The camera recovered from the frame's matrix matched the game's own camera to
0.1 units.

## Measured

On the dev machine (AMD Radeon integrated graphics, PPSSPP at 4x internal resolution, 2048x1280), on
the park fixture and Hepburn Heights at noon: **30 fps at every shadow quality, with AO and with
water on HIGH**; the capture costs about 4 ms of CPU a frame (1200 draws). The park's trees lay
dappled canopy shadows on the ground; on the street the elevated railway stripes the road and Toni
casts his own; at 23:00 the moon's shadows are faint (moonStrength); in rain they fade out.

## Debugging

- `LCS_SHADOW_STATS=<frames>`: the shadow pass's state every that many host frames (capture,
  passes, the camera against the game's, the sun, the GE's lights).
- `LCS_SHADOW_MASK=1`: the mask over the frame instead of multiplied in.
- `LCS_AO_SHOW`, `LCS_AO_SPLIT`, and the water's `LCS_WATER_PROBE=<frame>`,
  `LCS_WATER_DEBUG=<view>` (4 the water mask), `LCS_WATER_RAIN=<0..1>`: the VCS levers, unchanged.
- The pipe's `set Shadows 0` / `set Water 1` / `set AmbientOcclusion 1` and `fps` (input.md
  "Testing"); the clock bytes to set the hour; forcing the weather by writing old, new and forced.

## Not done or not checked

- The water's screen-space mirror reflects whatever is above the horizon on screen, including
  things in front of the sea: Toni on the shore shows as a soft band in the water. VCS's design.
  Foliage occludes the sea as rectangles (alpha-tested depth), so the sea keeps its vanilla look
  right behind a tree: VCS's trade-off, chosen over painting the sea over the tree.
- No debugger tabs: VCS's Shadows and Water tabs (ImVCS) were not ported; every knob is still in
  `GetSettings()` for one.
- Interiors, tunnels and Shoreside Vale and Staunton not looked at; the passes' VCS tunings
  (cascade size, bias, wave size, wetness) untouched.
- Vulkan only.
