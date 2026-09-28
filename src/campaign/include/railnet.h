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
// Off unless g_bRailTrains is set. See WIP-NOTES.md, "Rail from OSM".

class UnitClass;

// Campaign thread, at startup and every campaign stage (5 min): load the
// routes, adopt or spawn one train per configured route, and move its termini
// as the front moves.
void RailCampaignTick(int startup);

// BattalionClass::MoveUnit for a train. Returns what MoveUnit returns.
int RailMoveTrain(UnitClass *u);

// Where car `car` of this train sits right now (sim feet, yaw in radians,
// speed in ft/s). False if the unit is not a train this module is running.
bool RailTrainPose(UnitClass *u, int car, float *x, float *y, float *yaw,
                   float *speed);

// Campaign map (UI thread). Both copy out under the module's lock and load
// rail.txt on first use, so the map can draw the routes even with trains off.
//
// Calls fn once per route point, in order; returns how many routes there are.
typedef void (*RailPointFn)(void *ctx, int route, int index, float simX, float simY);
int RailVisitRoutes(RailPointFn fn, void *ctx);

struct RailTrainInfo
{
    float simX, simY; // lead car
    int team;
    int moving;
};

// Running trains, up to `max`; returns how many were written.
int RailGetTrains(RailTrainInfo *out, int max);

#endif
