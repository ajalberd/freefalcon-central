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

#endif
