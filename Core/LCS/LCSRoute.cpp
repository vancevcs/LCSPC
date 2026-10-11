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

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/LCS/LCSAddresses.h"
#include "Core/LCS/LCSFrontEnd.h"
#include "Core/LCS/LCSGame.h"
#include "Core/LCS/LCSMemory.h"
#include "Core/LCS/LCSRoute.h"
#include "Core/LCS/LCSSettings.h"
#include "Core/LCS/LCSState.h"

namespace LCS {

namespace {

// --- The graph (emu thread) ---

struct RoadNode {
	float x = 0.0f;
	float y = 0.0f;
	int firstLink = 0;
	int linkCount = 0;
};

std::vector<RoadNode> g_nodes;      // the car nodes, [0, carNodeCount)
std::vector<int> g_links;           // a car node, or -1
std::vector<u8> g_disabled;         // per node: switched off, as last read
// Which piece of the network a node is in, counting only nodes that are switched on (-1 for one
// that isn't). Two nodes are reachable from each other exactly when these agree: every link has
// its reverse (11510 of 11510 measured).
std::vector<int> g_component;
int g_componentCount = 0;
int g_disabledCount = 0;
u32 g_loadedFrom = 0;   // the CPathFind and the node array the copy came from
u32 g_nodeArray = 0;

float Dist(const RoadNode &a, const RoadNode &b) {
	return std::hypot(a.x - b.x, a.y - b.y);
}

void ClearGraph() {
	g_nodes.clear();
	g_links.clear();
	g_disabled.clear();
	g_component.clear();
	g_componentCount = 0;
	g_disabledCount = 0;
	g_loadedFrom = 0;
	g_nodeArray = 0;
}

void BuildComponents() {
	const int count = (int)g_nodes.size();
	g_component.assign((size_t)count, -1);
	g_componentCount = 0;
	std::vector<int> queue;
	for (int start = 0; start < count; start++) {
		if (g_disabled[start] || g_component[start] >= 0) {
			continue;
		}
		const int id = g_componentCount++;
		g_component[start] = id;
		queue.clear();
		queue.push_back(start);
		for (size_t at = 0; at < queue.size(); at++) {
			const RoadNode &here = g_nodes[queue[at]];
			for (int k = 0; k < here.linkCount; k++) {
				const int next = g_links[here.firstLink + k];
				if (next < 0 || g_disabled[next] || g_component[next] >= 0) {
					continue;
				}
				g_component[next] = id;
				queue.push_back(next);
			}
		}
	}
}

// The "switched off" flags, which the scripts change as the story opens roads. Re-read before each
// search; the pieces are rebuilt only when one changed.
bool RefreshDisabled() {
	bool changed = g_component.size() != g_nodes.size();
	int disabled = 0;
	for (size_t i = 0; i < g_nodes.size(); i++) {
		const std::optional<u8> flags = ReadU8(g_nodeArray + (u32)i * kLCSPathNodeStride + kLCSPathNodeFlags);
		if (!flags) {
			return false;
		}
		const u8 off = (*flags & kLCSPathNodeDisabled) ? 1 : 0;
		changed |= g_disabled[i] != off;
		g_disabled[i] = off;
		disabled += off;
	}
	g_disabledCount = disabled;
	if (changed) {
		BuildComponents();
	}
	return true;
}

// The car half of the node array and its links, refused whole if anything doesn't add up: a graph
// read mid-load is an incomplete one, not one to route through.
bool LoadGraph(u32 paths) {
	ClearGraph();
	const std::optional<u32> nodeArray = ReadU32(paths + kLCSPathNodeArray);
	const std::optional<u32> linkArray = ReadU32(paths + kLCSPathLinkArray);
	const std::optional<u32> afterLinks = ReadU32(paths + kLCSPathAfterLinks);
	const std::optional<u32> total = ReadU32(paths + kLCSPathNodeCount);
	const std::optional<u32> cars = ReadU32(paths + kLCSPathCarNodeCount);
	if (!nodeArray || !linkArray || !afterLinks || !total || !cars || !*nodeArray || !*linkArray) {
		return false;
	}
	// 5519 and 2196 in this game; an order of magnitude past that is not a path graph.
	if (*cars == 0 || *cars > *total || *total > 50000 || *afterLinks <= *linkArray ||
			(*afterLinks - *linkArray) / 2 > 200000) {
		return false;
	}
	// The links have no count of their own: they run to the next array (11510 of them, and the
	// nodes' own first + count reach exactly that).
	const int linkSpace = (int)((*afterLinks - *linkArray) / 2);
	const u32 count = *cars;
	g_nodes.resize(count);
	int maxLink = 0;
	for (u32 i = 0; i < count; i++) {
		const u32 rec = *nodeArray + i * kLCSPathNodeStride;
		const std::optional<u16> x = ReadU16(rec + kLCSPathNodePos);
		const std::optional<u16> y = ReadU16(rec + kLCSPathNodePos + 2);
		const std::optional<u16> first = ReadU16(rec + kLCSPathNodeFirstLink);
		const std::optional<u8> flags = ReadU8(rec + kLCSPathNodeFlags);
		if (!x || !y || !first || !flags) {
			ClearGraph();
			return false;
		}
		RoadNode &n = g_nodes[i];
		n.x = (float)(s16)*x / kLCSPathNodeScale;
		n.y = (float)(s16)*y / kLCSPathNodeScale;
		n.firstLink = (int)(s16)*first;
		n.linkCount = *flags & 0x0f;
		if (n.firstLink < 0 || n.firstLink + n.linkCount > linkSpace) {
			ClearGraph();
			return false;
		}
		maxLink = std::max(maxLink, n.firstLink + n.linkCount);
	}
	g_links.resize(maxLink);
	for (int i = 0; i < maxLink; i++) {
		const std::optional<u16> v = ReadU16(*linkArray + (u32)i * 2);
		if (!v) {
			ClearGraph();
			return false;
		}
		const int to = *v & kLCSPathLinkMask;
		// None measured, but a car node linked to a ped node is not a road.
		g_links[i] = to < (int)count ? to : -1;
	}
	g_disabled.assign(count, 0);
	g_component.clear();
	g_nodeArray = *nodeArray;
	g_loadedFrom = paths;
	if (!RefreshDisabled()) {
		ClearGraph();
		return false;
	}
	NOTICE_LOG(Log::System, "LCS route: %u road nodes, %d links, %d switched off, %d pieces", count, maxLink,
		g_disabledCount, g_componentCount);
	return true;
}

bool EnsureGraph() {
	const std::optional<u32> paths = ReadU32(kLCSThePathsPtr);
	const std::optional<u32> nodeArray = paths && *paths ? ReadU32(*paths + kLCSPathNodeArray) : std::nullopt;
	if (!paths || !*paths || !nodeArray) {
		ClearGraph();
		return false;
	}
	if (*paths == g_loadedFrom && *nodeArray == g_nodeArray && !g_nodes.empty()) {
		return RefreshDisabled();
	}
	return LoadGraph(*paths);
}

constexpr int kAnyPiece = -1;
constexpr int kSwitchedOffToo = -2;

// The switched-on node nearest a point, of one piece or any (kAnyPiece); or with kSwitchedOffToo,
// any node at all.
int NearestNode(float x, float y, float maxDistance, int component) {
	int best = -1;
	float bestSq = maxDistance * maxDistance;
	for (size_t i = 0; i < g_nodes.size(); i++) {
		if (component != kSwitchedOffToo && (g_component[i] < 0 || (component >= 0 && g_component[i] != component))) {
			continue;
		}
		const float dx = g_nodes[i].x - x;
		const float dy = g_nodes[i].y - y;
		const float d = dx * dx + dy * dy;
		if (d < bestSq) {
			bestSq = d;
			best = (int)i;
		}
	}
	return best;
}

// A* over the switched-on nodes, straight-line distance as the heuristic (a road is never shorter
// than the gap it spans, so the first time the goal is settled it is settled optimally). With
// `switchedOffCost`, switched-off nodes too, a step onto one costing that many times its length (the
// heuristic stays a lower bound).
std::vector<int> FindRoute(int from, int to, float switchedOffCost = 0.0f) {
	std::vector<int> path;
	const int count = (int)g_nodes.size();
	if (from < 0 || to < 0 || from >= count || to >= count) {
		return path;
	}
	std::vector<float> best((size_t)count, 1e30f);
	std::vector<int> came((size_t)count, -1);
	std::vector<u8> settled((size_t)count, 0);
	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
	best[from] = 0.0f;
	open.push({ Dist(g_nodes[from], g_nodes[to]), from });
	while (!open.empty()) {
		const int node = open.top().second;
		open.pop();
		if (settled[node]) {
			continue;
		}
		settled[node] = 1;
		if (node == to) {
			break;
		}
		const RoadNode &here = g_nodes[node];
		for (int k = 0; k < here.linkCount; k++) {
			const int next = g_links[here.firstLink + k];
			if (next < 0 || settled[next] || (g_disabled[next] && switchedOffCost <= 0.0f)) {
				continue;
			}
			const float step = Dist(here, g_nodes[next]);
			const float cost = best[node] + (g_disabled[next] ? step * switchedOffCost : step);
			if (cost < best[next]) {
				best[next] = cost;
				came[next] = node;
				open.push({ cost + Dist(g_nodes[next], g_nodes[to]), next });
			}
		}
	}
	if (!settled[to]) {
		return path;
	}
	for (int at = to; at != -1; at = came[at]) {
		path.push_back(at);
	}
	std::reverse(path.begin(), path.end());
	return path;
}

// --- The destination (emu thread) ---

// Where an entity a blip follows is now: its pool's slot, live only while the flag byte still holds
// the handle's low byte (the game's own GetAt). A person in a vehicle is the vehicle.
std::optional<std::pair<float, float>> EntityPosition(int type, u32 handle) {
	u32 poolPtr = 0;
	u32 size = 0;
	switch (type) {
	case kLCSBlipTypeCar: poolPtr = kLCSVehiclePoolPtr; size = kLCSVehicleSize; break;
	case kLCSBlipTypePed: poolPtr = kLCSPedPoolPtr; size = kLCSPedSize; break;
	case kLCSBlipTypeObject: poolPtr = kLCSObjectPoolPtr; size = kLCSObjectSize; break;
	default: return std::nullopt;
	}
	if ((s32)handle < 0) {
		return std::nullopt;
	}
	const std::optional<u32> pool = ReadPointer(poolPtr);
	const std::optional<u32> entries = pool ? ReadPointer(*pool) : std::nullopt;
	const std::optional<u32> flags = pool ? ReadPointer(*pool + 4) : std::nullopt;
	if (!entries || !flags) {
		return std::nullopt;
	}
	// The pool's capacity is at +8 (60 vehicles, 60 peds read live).
	const u32 slot = handle >> 8;
	if (slot >= ReadU32(*pool + 8).value_or(0) || ReadU8(*flags + slot).value_or(0xff) != (handle & 0xff)) {
		return std::nullopt;
	}
	u32 entity = *entries + slot * size;
	if (type == kLCSBlipTypePed && ReadU8(entity + 0x538).value_or(0) != 0) {
		const u32 vehicle = ReadU32(entity + 0x534).value_or(0);
		if (vehicle && Memory::IsValidAddress(vehicle)) {
			entity = vehicle;
		}
	}
	const std::optional<float> x = ReadFloat(entity + kLCSEntityPositionOffset);
	const std::optional<float> y = ReadFloat(entity + kLCSEntityPositionOffset + 4);
	if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
		return std::nullopt;
	}
	return std::make_pair(*x, *y);
}

struct Destination {
	bool have = false;
	bool mission = false;
	float x = 0.0f;
	float y = 0.0f;
};

// The map's marker, and the mission destination nearest the player: a plain blip (sprite 0) on a
// place or on something that moves. Blips with a sprite are the city's icons, never a destination.
Destination FindDestination(float px, float py) {
	Destination marker;
	Destination mission;
	float missionSq = 0.0f;
	for (int i = 0; i < kLCSBlipCount; i++) {
		const u32 e = kLCSRadarTrace + (u32)i * kLCSBlipStride;
		if (ReadU8(e + kLCSBlipInUse).value_or(0) == 0 || ReadU16(e + kLCSBlipDisplay).value_or(0) == 0) {
			continue;
		}
		const int type = (int)ReadU32(e + kLCSBlipType).value_or(0);
		const u16 sprite = ReadU16(e + kLCSBlipSprite).value_or(0xffff);
		std::optional<std::pair<float, float>> pos;
		if (type == kLCSBlipTypeCoord || type == kLCSBlipTypeContact) {
			const std::optional<float> x = ReadFloat(e + kLCSBlipPos);
			const std::optional<float> y = ReadFloat(e + kLCSBlipPos + 4);
			if (x && y && std::isfinite(*x) && std::isfinite(*y)) {
				pos = std::make_pair(*x, *y);
			}
		} else if (sprite == 0) {
			pos = EntityPosition(type, ReadU32(e + kLCSBlipHandle).value_or(0xffffffff));
		}
		// Somewhere on the map, which a slot mid-allocation may not be yet.
		if (!pos || std::fabs(pos->first) > 4000.0f || std::fabs(pos->second) > 4000.0f) {
			continue;
		}
		if (sprite == kLCSBlipSpriteMapMarker && type == kLCSBlipTypeCoord) {
			marker = { true, false, pos->first, pos->second };
		} else if (sprite == 0) {
			const float d = (pos->first - px) * (pos->first - px) + (pos->second - py) * (pos->second - py);
			if (!mission.have || d < missionSq) {
				mission = { true, true, pos->first, pos->second };
				missionSq = d;
			}
		}
	}
	// On a mission its destination wins; off one, the marker. VCS routed to the objective first
	// always, and its safe-house hint then beat a marker between jobs; $560 says which it is here.
	const u32 space = ReadU32(kLCSScriptSpacePtr).value_or(0);
	const bool onMission = space && ReadU32(space + kLCSGlobalOnMission * 4).value_or(0) != 0;
	if (onMission && mission.have) {
		return mission;
	}
	return marker.have ? marker : mission;
}

// --- The line (emu thread builds it, the UI thread draws it) ---

LCSRouteTuning g_tuning;

std::mutex g_lock;
RouteDraw g_draw;            // under g_lock
LCSRouteStats g_stats;       // under g_lock

// Between searches.
std::vector<std::pair<float, float>> g_route;
Destination g_destination;
int g_recompute = 0;
u64 g_searches = 0;
// Four searches a second: a wrong turn shows within a car length or two, and A* over 2196 nodes
// stays off most frames.
constexpr int kRecomputeTicks = 15;
// How far the nearest road still on must be before the route starts on the switched-off road the
// player is on, and what a closed road's length counts for then.
constexpr float kLeadInFrom = 20.0f;
constexpr float kSwitchedOffCost = 20.0f;

// One segment of the route clipped to the unit disc. A real clip: at a range of 96 the road ahead
// is often one segment with both ends off the radar and the middle across it.
bool ClipToDisc(float x0, float y0, float x1, float y1, float *ox0, float *oy0, float *ox1, float *oy1) {
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float a = dx * dx + dy * dy;
	if (a < 1e-9f) {
		return false;
	}
	const float b = 2.0f * (x0 * dx + y0 * dy);
	const float c = x0 * x0 + y0 * y0 - 1.0f;
	const float disc = b * b - 4.0f * a * c;
	if (disc < 0.0f) {
		return false;
	}
	const float sq = std::sqrt(disc);
	const float lo = std::max(0.0f, (-b - sq) / (2.0f * a));
	const float hi = std::min(1.0f, (-b + sq) / (2.0f * a));
	if (lo >= hi) {
		return false;
	}
	*ox0 = x0 + lo * dx;
	*oy0 = y0 + lo * dy;
	*ox1 = x0 + hi * dx;
	*oy1 = y0 + hi * dy;
	return true;
}

// The parts of a clipped segment outside a circle of radius r at the centre (the player's arrow),
// as up to two t ranges.
int ClipOutHole(float x0, float y0, float x1, float y1, float r, float t[4]) {
	t[0] = 0.0f;
	t[1] = 1.0f;
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float a = dx * dx + dy * dy;
	if (r <= 0.0f || a < 1e-9f) {
		return 1;
	}
	const float b = 2.0f * (x0 * dx + y0 * dy);
	const float c = x0 * x0 + y0 * y0 - r * r;
	const float disc = b * b - 4.0f * a * c;
	if (disc <= 0.0f) {
		return 1;
	}
	const float sq = std::sqrt(disc);
	const float tA = (-b - sq) / (2.0f * a);
	const float tB = (-b + sq) / (2.0f * a);
	if (tB <= 0.0f || tA >= 1.0f) {
		return 1;
	}
	int n = 0;
	if (tA > 0.0f) {
		t[n * 2] = 0.0f;
		t[n * 2 + 1] = tA;
		n++;
	}
	if (tB < 1.0f) {
		t[n * 2] = tB;
		t[n * 2 + 1] = 1.0f;
		n++;
	}
	return n;
}

// `shown`: the radar is on screen, so the calibration circle may be.
void Publish(const char *status, bool shown, std::vector<RouteSegment> &&segments) {
	std::lock_guard<std::mutex> guard(g_lock);
	g_draw.segments = std::move(segments);
	g_draw.toMission = g_destination.mission;
	g_draw.calibration = g_tuning.showCalibration && shown;
	g_draw.centreX = kLCSRadarLeft + kLCSRadarSize * 0.5f;
	g_draw.centreY = kLCSRadarTop + kLCSRadarSize * 0.5f;
	g_draw.radius = kLCSRadarSize * 0.5f;
	g_draw.thickness = g_tuning.thickness;
	g_stats.status = status;
	g_stats.nodes = (int)g_nodes.size();
	g_stats.links = (int)g_links.size();
	g_stats.disabled = g_disabledCount;
	g_stats.components = g_componentCount;
	g_stats.routeNodes = (int)g_route.size();
	float length = 0.0f;
	for (size_t i = 1; i < g_route.size(); i++) {
		length += std::hypot(g_route[i].first - g_route[i - 1].first, g_route[i].second - g_route[i - 1].second);
	}
	g_stats.routeLength = length;
	g_stats.toMission = g_destination.mission;
	g_stats.haveDestination = g_destination.have;
	g_stats.destination[0] = g_destination.x;
	g_stats.destination[1] = g_destination.y;
	g_stats.searches = g_searches;
}

// Whether the game is drawing its radar this frame (CHud::Draw's own tests), and the player is
// driving something that uses the roads: on foot, on water or in the air a road route is wrong about
// the way to go, which is the VCS fork's rule too. Null when it is; why not otherwise.
const char *Hidden() {
	if (ReadU8(kLCSMultiplayer).value_or(0) != 0) {
		return "multiplayer";
	}
	if (GameMenuUp()) {
		return "game menu";
	}
	if (ReadU8(kLCSHudWantsToDraw).value_or(1) != 1 || ReadU8(kLCSCamWideScreenOn).value_or(0) != 0 ||
			ReadS32(kLCSRadarMode).value_or(0) == 2 || ReadU8(kLCSRadarSuppressed).value_or(0) != 0) {
		return "radar hidden";
	}
	const LCSState &state = GetState();
	if (state.inVehicle.has_value()) {
		if (!*state.inVehicle) {
			return "on foot";
		}
		if (state.vehicleClass == VehicleClass::Boat || state.vehicleClass == VehicleClass::Heli ||
				state.vehicleClass == VehicleClass::Plane) {
			return "not a road vehicle";
		}
	}
	return nullptr;
}

}  // namespace

LCSRouteTuning &RouteTuning() {
	return g_tuning;
}

void RouteTick() {
	if (!IsActive() || !Settings().gpsRoute) {
		g_route.clear();
		Publish("off", false, {});
		return;
	}
	if (const char *why = Hidden()) {
		g_recompute = 0;
		Publish(why, false, {});
		return;
	}

	// The radar's frame: where it is centred, which way is up, how far it sees.
	const std::optional<float> ox = ReadFloat(kLCSRadarOrigin);
	const std::optional<float> oy = ReadFloat(kLCSRadarOrigin + 4);
	const std::optional<float> sn = ReadFloat(kLCSRadarSin);
	const std::optional<float> cs = ReadFloat(kLCSRadarCos);
	const std::optional<float> range = ReadFloat(kLCSRadarRange);
	if (!ox || !oy || !sn || !cs || !range || !(*range > 1.0f && *range < 10000.0f) ||
			std::fabs(std::hypot(*sn, *cs) - 1.0f) > 0.1f) {
		Publish("no radar frame", false, {});
		return;
	}

	if (--g_recompute <= 0) {
		g_recompute = kRecomputeTicks;
		g_route.clear();
		g_destination = Destination();
		if (EnsureGraph()) {
			g_destination = FindDestination(*ox, *oy);
			// The player's node within 150, the destination's within 600 of wherever it was dropped
			// (a rooftop, a park), and both in the same piece: the network comes in several, which
			// overlap where roads are switched off.
			const int from = g_destination.have ? NearestNode(*ox, *oy, 150.0f, kAnyPiece) : -1;
			const int to = from >= 0 ? NearestNode(g_destination.x, g_destination.y, 600.0f, g_component[from]) : -1;
			if (to >= 0) {
				g_searches++;
				// The road the player is on may be one the game has switched off (a dock, a mission's
				// area: the car fixture's park road, 37 units from the nearest road still on), and it
				// is still the way out. So off any road that is on, the search starts there, with
				// closed roads at twenty times their length: it leaves them at the first open road
				// going the right way. (Leading in to the nearest open node instead drew a spur back
				// from where the two met.) Beside an open road, a closed one alongside isn't the way.
				std::vector<int> nodes;
				const int here = NearestNode(*ox, *oy, 150.0f, kSwitchedOffToo);
				const float fromDistance = std::hypot(g_nodes[from].x - *ox, g_nodes[from].y - *oy);
				if (here >= 0 && here != from && g_disabled[here] && fromDistance > kLeadInFrom) {
					nodes = FindRoute(here, to, kSwitchedOffCost);
				}
				if (nodes.empty()) {
					nodes = FindRoute(from, to);
				}
				for (int node : nodes) {
					g_route.push_back({ g_nodes[node].x, g_nodes[node].y });
				}
			}
		}
	}
	if (g_route.size() < 2) {
		Publish(!g_nodes.empty() ? (g_destination.have ? "no route" : "no destination") : "no road graph", true, {});
		return;
	}

	// The game's own arithmetic (0x08967b20), then the disc's box on the PSP's screen.
	const float inv = 1.0f / *range;
	const float radius = kLCSRadarSize * 0.5f;
	const float centreX = kLCSRadarLeft + radius;
	const float centreY = kLCSRadarTop + radius;
	const float hole = g_tuning.playerHoleRadius / radius;
	// The line starts where the player is along it, every tick: the closest point of its first few
	// segments, so a node already passed (or one behind) isn't drawn as a stub back to it.
	size_t start = 0;
	std::pair<float, float> startPoint = g_route[0];
	float bestSq = 1e30f;
	for (size_t i = 0; i + 1 < g_route.size() && i < 6; i++) {
		const float ax = g_route[i].first, ay = g_route[i].second;
		const float bx = g_route[i + 1].first - ax, by = g_route[i + 1].second - ay;
		const float len = bx * bx + by * by;
		const float t = len > 1e-6f ? std::clamp(((*ox - ax) * bx + (*oy - ay) * by) / len, 0.0f, 1.0f) : 0.0f;
		const float cx = ax + t * bx, cy = ay + t * by;
		const float d = (cx - *ox) * (cx - *ox) + (cy - *oy) * (cy - *oy);
		if (d < bestSq) {
			bestSq = d;
			start = i;
			startPoint = { cx, cy };
		}
	}
	std::vector<RouteSegment> segments;
	segments.reserve(g_route.size());
	float px = 0.0f, py = 0.0f;
	for (size_t i = start; i < g_route.size(); i++) {
		const std::pair<float, float> &point = i == start ? startPoint : g_route[i];
		const float dx = (point.first - *ox) * inv;
		const float dy = (point.second - *oy) * inv;
		const float rx = *cs * dx + *sn * dy;
		const float ry = *cs * dy - *sn * dx;
		float a0, b0, a1, b1;
		if (i > start && ClipToDisc(px, py, rx, ry, &a0, &b0, &a1, &b1)) {
			float t[4];
			const int pieces = ClipOutHole(a0, b0, a1, b1, hole, t);
			for (int p = 0; p < pieces; p++) {
				const float lo = t[p * 2], hi = t[p * 2 + 1];
				// +y is forward on the radar and down on the screen.
				segments.push_back({
					centreX + (a0 + lo * (a1 - a0)) * radius, centreY - (b0 + lo * (b1 - b0)) * radius,
					centreX + (a0 + hi * (a1 - a0)) * radius, centreY - (b0 + hi * (b1 - b0)) * radius,
				});
			}
		}
		px = rx;
		py = ry;
	}
	const bool empty = segments.empty();
	Publish(empty ? "route off the radar" : "drawing", true, std::move(segments));
}

void RouteReset() {
	ClearGraph();
	g_route.clear();
	g_destination = Destination();
	g_recompute = 0;
	g_searches = 0;
	Publish("off", false, {});
}

bool GetRouteSegments(RouteDraw *out) {
	std::lock_guard<std::mutex> guard(g_lock);
	if (g_draw.segments.empty() && !g_draw.calibration) {
		return false;
	}
	*out = g_draw;
	return true;
}

bool RouteHasDrawing() {
	std::lock_guard<std::mutex> guard(g_lock);
	return !g_draw.segments.empty() || g_draw.calibration;
}

void RouteGetStats(LCSRouteStats *out) {
	std::lock_guard<std::mutex> guard(g_lock);
	*out = g_stats;
}

}  // namespace LCS
