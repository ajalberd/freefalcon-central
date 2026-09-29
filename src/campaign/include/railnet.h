#ifndef RAILNET_H
#define RAILNET_H

// Artscout - 2026: railway routes and trains.
//
// The routes come from rail.txt in the theater's terrain folder, written by
// tools/campaign-editor/osm_rail.py from OpenStreetMap (no shipped theater has
// any rail of its own). A train is an ordinary Supply battalion flagged
// U_TRAIN: the campaign and the sim both see a normal ground unit that can be
// spotted, targeted, damaged and killed, but its position comes from here
// instead of from the ground planner. It shuttles between a friendly supply
// hub and a point short of the front, and its place on the route is a pure
// function of game time, so the campaign thread and the sim thread agree on
// where it is without handing state to each other.
//
// Off unless g_bRailTrains is set. See RAIL.md.

class UnitClass;

// The "Train" unit class: land / unit / battalion / STYPE_UNIT_SUPPLY / this sptype. A copy of
// the 16-KrAz Supply battalion (class 77) appended to the theater's FALCON4.ct and FALCON4.UCD,
// and listed in teunits.lst so the TE editor offers it. Any unit of this class is a train.
#define RAIL_TRAIN_SPTYPE 20

// Campaign thread, at startup and every campaign stage (5 min): load the
// routes, adopt or spawn one train per configured route, and move its termini
// as the front moves.
void RailCampaignTick(int startup);

// BattalionClass::MoveUnit for a train. Returns what MoveUnit returns.
int RailMoveTrain(UnitClass *u);

// What the track is at a point: kind '-' plain, 'b' bridge, 't' tunnel (the
// flags rail.txt carries from OSM). On a bridge, the whole bridge run goes from
// (ax, ay) to (bx, by) and the point is t (0..1) of the way along it -- the deck
// is level between the ground at the two ends, not the river bed under it.
struct RailTrackAt
{
    char kind;
    float ax, ay, bx, by, t;
};

// Where car `car` of this train sits right now (sim feet, yaw in radians,
// speed in ft/s), and optionally what the track is there. False if the unit is
// not a train this module is running.
bool RailTrainPose(UnitClass *u, int car, float *x, float *y, float *yaw,
                   float *speed, RailTrackAt *at = nullptr);

// Campaign map (UI thread). Both copy out under the module's lock and load
// rail.txt on first use, so the map can draw the routes even with trains off.
//
// Calls fn once per route point, in order; returns how many routes there are.
typedef void (*RailPointFn)(void *ctx, int route, int index, float simX, float simY);
int RailVisitRoutes(RailPointFn fn, void *ctx);

// The same walk with each point's track flag (of the segment that starts there:
// '-', 'b' or 't'; the last point's is '-'). For the 3D track strip.
typedef void (*RailTrackPointFn)(void *ctx, int route, int index, float simX, float simY,
                                 char flag);
int RailVisitTrack(RailTrackPointFn fn, void *ctx);

// Rail bridges bound to a bridge objective (a dropped one cuts the line), for
// the campaign map. `down` = the objective is at 0% or has a destroyed span.
struct RailBridgeInfo
{
    float simX, simY; // middle of the rail bridge
    int down;
};

int RailGetBridges(RailBridgeInfo *out, int max);

struct RailTrainInfo
{
    float simX, simY; // lead car
    int team;
    int moving;
};

// Running trains, up to `max`; returns how many were written.
int RailGetTrains(RailTrainInfo *out, int max);

// Distance in km from a sim position to the nearest rail line; -1 if the theater has none.
// The TE editor uses it to leave a Supply battalion where it was dropped on a line.
float RailDistanceKm(float simX, float simY);

// Height in feet of the rail top above the ground when the 3D track strip is drawn (drawrail.cpp),
// 0 when it is off: train cars ride on it rather than sink into it.
float DrawRailTopFt();

#endif
