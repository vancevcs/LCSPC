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

#include <vector>

#include "Common/CommonTypes.h"

// The GPS route: a line on the radar along the roads, from the player to the map's marker or to a
// mission's destination. docs/lcs/gps.md; the VCS fork's VCSRoute and VCSRadar are the same design
// (its docs/VCS_ADDRESSES.md "The road network" is how that graph was read).
//
// - The graph is the game's: ThePaths' car nodes, what the traffic drives on. Copied to the host
//   once per graph (it moves only with a new game or a load), and the "switched off" flag re-read
//   every recompute, because the story opens roads: those nodes are left out, so a route never
//   crosses the Callahan Bridge the game has under construction.
// - The destination is a radar blip: the map's marker, or a mission's (a place, or a car, a person
//   or an object, followed through the game's pools). On a mission its destination wins; off one,
//   the marker.
// - A* to the destination's nearest node the player can reach, four times a second, with a lead-in
//   along a switched-off road the player is on; projected through the radar's own transform every
//   tick, started where the player is along it, and clipped to the disc.
//
// Threads: RouteTick and everything that reads PSP memory run on the emu thread. The UI thread
// takes finished segments (in the PSP's 480x272 screen units) through GetRouteSegments.

namespace LCS {

struct LCSRouteTuning {
	// Leave the player's arrow a hole in the line, in 480x272 units, so the game's own arrow shows
	// over it (everything drawn here lands on the finished frame).
	float playerHoleRadius = 3.5f;
	// The line's width, in 480x272 units.
	float thickness = 1.25f;
	// Draw the circle the route is clipped to, to check it against the game's radar.
	bool showCalibration = false;
};
LCSRouteTuning &RouteTuning();

// Emu thread, from LCS::Tick.
void RouteTick();
// Emu thread: drop the graph and the route (shutdown).
void RouteReset();

struct RouteSegment {
	float x1, y1, x2, y2;   // PSP screen units
};
struct RouteDraw {
	std::vector<RouteSegment> segments;
	bool toMission = false;     // the line's colour: a mission's destination, or the map's marker
	bool calibration = false;   // draw the radar's circle too
	float centreX = 0.0f, centreY = 0.0f, radius = 0.0f;
	float thickness = 1.0f;
};
// UI thread. False when there is nothing to draw.
bool GetRouteSegments(RouteDraw *out);
// UI thread: whether GetRouteSegments has anything, without the copy.
bool RouteHasDrawing();

struct LCSRouteStats {
	const char *status = "";
	int nodes = 0;          // car nodes copied
	int links = 0;
	int disabled = 0;       // switched off now
	int components = 0;     // pieces the enabled network is in
	int routeNodes = 0;
	float routeLength = 0.0f;
	bool toMission = false;
	bool haveDestination = false;
	float destination[2] = {};
	u64 searches = 0;
};
void RouteGetStats(LCSRouteStats *out);

}  // namespace LCS
