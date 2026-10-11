# LCS GPS route

`Core/LCS/LCSRoute.cpp`, drawn by `DrawLCSOverlay` in `UI/LCSMenuScreen.cpp`. While the player
drives, a line on the radar along the roads to the map's marker or to where a mission sends them.
`GPSRoute` in lcs.ini (the Gameplay page; on by default); Debug -> LCS -> Route has the state, a
calibration circle and the line's width. The VCS fork's `VCSRoute` / `VCSRadar` are the same
design; this is them on LCS's data.

## The road graph is the game's

What the traffic drives on, so it is authored, and it is where a car can go. `get_closest_car_node`
(02C6, `0x08a220b0`) hands `[0x08b3980c]` (ThePaths, a `CPathFind`) to FindNodeClosestToCoors
(`0x0897572c`) and to the node-to-coordinates call (`0x089777a8`), which shows the layout:

| | |
|---|---|
| `+0x00` | the nodes, 20 bytes each: VC's `CPathNode` (VCS's are 10) |
| `+0x08` | the links, u16 node indices in the low 14 bits |
| `+0x0c` | the next array, so where the links end (11510 of them) |
| `+0x14`, `+0x18`, `+0x1c` | 5519 nodes = 2196 car + 3323 ped, the car ones first |
| `+0x24` | 4604, the car nodes' links |
| node `+4` `+6` `+8` | x, y, z, s16 times 8 (the VFPU loads them as shorts and scales by 2^-3) |
| node `+0xc` | the first link, s16 |
| node `+0x10` | the link count (low nibble); 0x20 switched off, 0x40 between levels, 0x10 dead end |

Read live from the car fixture, and the structure checks out against itself: each node's first link
plus its count is the next node's first link (5518 of 5518), every link resolves after the mask and
has its reverse (11510 of 11510), none joins a car node to a ped node, the median link is 19.2 units
long (95% under 50), and the extents (x -1585..1588, y -1683..634) are the city. The links' top
two bits are flags (432 with 0x8000, 144 with 0xc000).

**Switched-off roads are left out.** 442 of the car nodes carried 0x20 at the start of the story,
and plotted they are the Callahan Bridge (under construction for the whole game), the Porter Tunnel,
Shoreside's crossings and a few docks and ramps: `switch_roads_off`'s work. Routing over them would
send the player across a bridge that isn't there. Without them the network is in 9 pieces (657,
609 and 470 nodes, the three islands, and six scraps), so a marker on a locked island has no route,
which is right. The flags are re-read before every search (2196 byte reads) and the pieces rebuilt
when one changes, because the story opens roads.

The car half is copied to the host once (the CPathFind and the node array pointers are the key; a
new game or a load moves them) and the search runs on the copy.

## The destination is a blip

`CRadar::ms_RadarTrace` at `0x08e4aa20`, 75 blips of 0x50 bytes (SetCoordBlip `0x08967e40`): type
`+0x04` (1 car, 2 ped, 3 object, 4 a place, 5 a contact point), pool handle `+0x08`, position
`+0x0c`, in use `+0x33`, display `+0x3e` (0 nothing ... 3 marker and blip), sprite `+0x40`.

- **The map's marker** (Square on the map page, "Player Target") is a place blip with sprite 49,
  colour 0, display 2. Measured by placing one: a new blip at the cursor.
- **A mission's destination** is a plain blip, sprite 0 (`add_blip_for_coord` makes it scale 3,
  display 3). The car fixture's mission: a place blip at (1156.8, -256.2). One that follows a car, a
  person or an object is followed through the game's pools, as its blip draw does (`0x08969f8c`):
  `[0x08b4c540]` vehicles (0x6e0 each), `[0x08b4c53c]` peds (0xcb0), `[0x08b4c54c]` objects
  (0x220); a pool is {entries, flags, capacity}, and slot `handle >> 8` is live while its flag byte
  equals `handle & 0xff`. A person in a vehicle is the vehicle. The arithmetic was checked on the
  player's own car and ped (handles 0x1001 and 0x101 came back to the same entities) and on a
  mission's: on foot, the first mission's blip is Vincenzo's car (type 1, handle 0x1202), whose
  stored position is a stale (1156.8, -256.2) and whose pool slot is the car, 5 units from Toni.
  VCS refused entity blips for want of the pools.
- Blips with a sprite are the city's icons and never a destination.

**Which wins:** on a mission (script global `$560`, the one the auto-save waits on) its nearest
destination, else the marker; off one the marker, else the nearest plain blip. VCS routed to the
objective first always (asked for there), and between jobs its safe-house hint then beat a dropped
marker; `$560` tells the two apart here.

## The search

Four times a second: the destination's nearest switched-on node within 600 units in the player's
piece, A* to it (straight-line heuristic), the line as the nodes it passes.

**Where it starts.** The nearest switched-on node within 150 of the player, unless that is more
than 20 units off and the nearest node of all is a switched-off one: then the player is on a closed
road (the car fixture's park road, 27 to 37 units from an open one), and the search starts there,
with closed roads at twenty times their length, so the line follows the road the player is on and
leaves it at the first open road going the right way. The first version led in to the nearest open
node instead and drew a spur back from where the two met.

**Every tick** the line is projected again, starting at the player's place along it: the closest
point of its first six segments, so a node already passed isn't drawn as a stub behind the car.

## On the radar

CRadar::TransformRealWorldPointToRadarSpace (`0x08967b20`): `d = (p - origin) / range`, then
`(cos * d.x + sin * d.y, cos * d.y - sin * d.x)`, +y forward, with the origin at `0x08b6e218` (the
player's position), sin / cos at `0x08b6e220` / `0x08b6e224` (a unit vector, read 1.0000) and the
range at `0x08b5e7c0` (96.0). The disc to the screen (`0x08967860`): `(left + w/2 + x w/2, top +
h/2 - y h/2)` with the box from four getters: left 12, top 196, 65.8 square on the PSP's 480x272
(top 170 and 92 square in multiplayer, `0x08b56c7c`). The line is clipped to the disc, with a hole
of 3.5 units at the centre so the game's arrow shows over it, and drawn by EmuScreen on the
rectangle the picture is displayed in, as the page hints are: 1.25 units wide over a dark edge 2 px
wider, in the radar's own colours, the marker's red `#C8393C` and the destination's yellow
`#D2D201`.

**It is drawn only when the game draws the radar.** CHud::Draw (`0x08988940`) does the HUD while
`0x08b5e214` is 1 and the camera's cutscene bars (`0x08b83415`, TheCamera+0x75) are off, and the
radar then unless RADAR MODE is OFF or `0x08b5e56d` is set. Also not with the game's menu up, in
multiplayer, on foot, or in a boat, a helicopter or a plane (the VCS fork's rule: a road route is
wrong about the way to go anywhere but on the roads).

## Measured

| | result |
|---|---|
| the graph on load | 2196 nodes, 4604 links, 442 switched off, 9 pieces (logged) |
| the mission destination, from a Portland pavement | a yellow line on the radar exactly over a Python A* of the same graph, projected with the frame read at the same paused moment |
| a marker 475 units away, `$560` set to 0 | a red line up the road, exactly over the Python route (17 nodes) |
| the radar box | the game's ring centred within a pixel of (44.9, 228.9) on a capture; the calibration circle sits on the map disc |
| the park road (switched off) | the line from the arrow along the park road to the street, then to the destination, no spur |
| on foot (the park fixture, the car blip up) | no line |
| the Gameplay page | GPS ROUTE is a real row, ON |

## Not checked

- A line drawn to an entity blip: the pool walk is checked on a mission's blip, but that one is
  replaced by the destination the moment Toni is back in the car.
- In the game's menu and in a cutscene the line is withheld by the code above, not captured.
- Multiplayer: off by the flag, never run.
- A route across an island the story has opened (the flags are re-read; no save that far is on hand).

## Testing

The car fixture (`lcs_car_vincenzo.ppst`) is on a mission with a place blip. Hold S first: it rolls
downhill, and held too long S reverses it down the hill to the street. To test the marker off a
mission, place one on the map and write `$560` (`[0x08b38d7c] + 560 * 4`) to 0 over the debugger,
or move the marker blip's position (`+0x0c`, `+0x10`, and the copy at `+0x20`, `+0x24`). Pause the
CPU with the debugger before a capture: the line holds its last tick, and the radar's frame can be
read at the same moment for an offline projection.
