#include <stdio.h>
#include <conio.h>
#include <stddef.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include "cmpglobl.h"
#include "listadt.h"
#include "campcell.h"
#include "campterr.h"
#include "asearch.h"
#include "path.h"
#include "find.h"
#include "vutypes.h"
#include "campaign.h"
#include "atm.h"
#include "f4vu.h"
#include "camplist.h"
#include "campwp.h"
#include "update.h"
#include "loadout.h"
#include "navunit.h"
#include "tactics.h"
#include "tacan.h"
#include "classtbl.h"
#include "graphics/include/tmap.h"
#include "ptdata.h"
#include "camp2sim.h"
#include "aircrft.h"
#include "radar.h"
#include "debuggr.h"

//sfr: added check
#include "invalidbufferexception.h"

// ============================================
// Prototypes
// ============================================

WayPoint ResetCurrentWP(TaskForce tf);
WayPoint DoWPAction(TaskForce tf, WayPoint w);

// ============================================
// Defines and other nifty stuff
// ============================================

// ============================================
// Externals
// ============================================

extern unsigned char SHOWSTATS;

extern CampaignHeading WithdrawUnit(Unit u);

//extern VU_ID_NUMBER vuAssignmentId;
//extern VU_ID_NUMBER vuLowWrapNumber;
//extern VU_ID_NUMBER vuHighWrapNumber;
extern VU_ID_NUMBER lastNonVolatileId;
extern VU_ID_NUMBER lastLowVolitileId;
extern VU_ID_NUMBER lastVolatileId;

extern FILE *save_log, *load_log;

extern int start_save_stream, start_load_stream;

#ifdef DEBUG
extern int gCheckConstructFunction;
#endif

// =================================
// Smart heap pool stuff
// =================================

#ifdef USE_SH_POOLS
MEM_POOL TaskForceClass::pool;
#endif

// ============================================
// TaskForce Class Functions
// ============================================

// KCK: ALL TASK FORCE CONSTRUCTION SHOULD USE THIS FUNCTION
TaskForceClass *NewTaskForce(int type)
{
    TaskForceClass *new_taskforce;
    /*VuEnterCriticalSection();
    lastVolatileId = vuAssignmentId;
    vuAssignmentId = lastNonVolatileId;
    vuLowWrapNumber = FIRST_NON_VOLATILE_VU_ID_NUMBER;
    vuHighWrapNumber = LAST_NON_VOLATILE_VU_ID_NUMBER;*/
    new_taskforce = new TaskForceClass(type);
    /*lastNonVolatileId = vuAssignmentId;
    vuAssignmentId = lastVolatileId;
    vuLowWrapNumber = FIRST_VOLATILE_VU_ID_NUMBER;
    vuHighWrapNumber = LAST_VOLATILE_VU_ID_NUMBER;
    VuExitCriticalSection();*/
    return new_taskforce;
}

TaskForceClass::TaskForceClass(ushort type)
    : UnitClass(type, GetIdFromNamespace(NonVolatileNS))
{
    orders = 0;
    supply = 100;
    air_target = FalconNullId;
    missiles_flying = 0;
    VU_TIME SEARCHtimer = 0;
    VU_TIME AQUIREtimer = 0;
    radar_mode = FEC_RADAR_OFF;
    search_mode = FEC_RADAR_OFF;
    last_combat = last_move = 0;
    last_direction = 0;
    SetParent(1);
}

TaskForceClass::TaskForceClass(VU_BYTE **stream, long *rem)
    : UnitClass(stream, rem)
{
    if (load_log)
    {
        fprintf(load_log, "%08x TaskForceClass ", *stream - start_load_stream);
        fflush(load_log);
    }

    memcpychk(&orders, stream, sizeof(uchar), rem);
    memcpychk(&supply, stream, sizeof(Percentage), rem);
    air_target = FalconNullId;
    missiles_flying = 0;
    radar_mode = FEC_RADAR_OFF;
    search_mode = FEC_RADAR_OFF;
    last_combat = last_move = 0;
    last_direction = 0;

    if (GetSType() == STYPE_UNIT_CARRIER)
    {
        SetTacan(1);
    }
}

TaskForceClass::~TaskForceClass(void)
{
    if (IsAwake())
        Sleep();
}

int TaskForceClass::SaveSize(void)
{
    return UnitClass::SaveSize() + sizeof(uchar) + sizeof(Percentage);
}

int TaskForceClass::Save(VU_BYTE **stream)
{
    UnitClass::Save(stream);

    if (save_log)
    {
        fprintf(save_log, "%08x TaskForceClass ", *stream - start_save_stream);
        fflush(save_log);
    }

    memcpy(*stream, &orders, sizeof(uchar));
    *stream += sizeof(uchar);
    memcpy(*stream, &supply, sizeof(Percentage));
    *stream += sizeof(Percentage);
    return TaskForceClass::SaveSize();
}

// event handlers
int TaskForceClass::Handle(VuFullUpdateEvent *event)
{
    // copy data from temp entity to current entity
    TaskForceClass *tmp_ent = (TaskForceClass *)(event->expandedData_.get());

    orders = tmp_ent->orders;
    supply = tmp_ent->supply;
    return (UnitClass::Handle(event));
}

// This is the speed we're trying to go
int TaskForceClass::GetUnitSpeed() const
{
    return GetMaxSpeed();
}

int TaskForceClass::GetVehicleDeagData(SimInitDataClass *simdata, int remote)
{
    static CampEntity ent;
    static int round;
    int dist, i, ptIndexAt;

    // Reinitialize static vars upon query of first vehicle
    if (simdata->vehicleInUnit < 0)
    {
        simdata->vehicleInUnit = 0;
        ent = NULL;

        if (not remote)
        {
            // Used only in port
            round = 0;
            simdata->ptIndex = GetDeaggregationPoint(0, &ent);

            if (simdata->ptIndex)
            {
                // Yuck  The first call returns only the list index, NOT a real point index.
                // To ensure we have at least one set of points we have to actually query for them
                // then reset again...
                simdata->ptIndex = GetDeaggregationPoint(0, &ent);

                if (simdata->ptIndex)
                    simdata->ptIndex = GetDeaggregationPoint(0, &ent);

                ent = NULL;
                GetDeaggregationPoint(0, &ent);
            }

            // Used only at sea
            WayPoint w;
            w = GetCurrentUnitWP();

            if (w)
            {
                // Find heading to next waypoint
                GridIndex ux, uy, wx, wy;
                GetLocation(&ux, &uy);
                w->GetWPLocation(&wx, &wy);
                simdata->heading = AngleTo(ux, uy, wx, wy);
            }
        }
    }
    else
    {
        simdata->vehicleInUnit++;
    }

    if (not remote)
    {
        if (simdata->ptIndex)
        {
            // In port
            float dx, dy;

            // Find the center point and direction point for this ship
            simdata->ptIndex = GetDeaggregationPoint(simdata->campSlot, &ent);
            ptIndexAt = GetDeaggregationPoint(simdata->campSlot, &ent);

            if (not ptIndexAt)
            {
                ShiAssert(
                    not simdata
                            ->ptIndex); // We should always have an even number of points

                // Reuse the old points, but with an offset
                ent = NULL;
                GetDeaggregationPoint(simdata->campSlot, &ent); // Reset
                simdata->ptIndex =
                    GetDeaggregationPoint(simdata->campSlot, &ent);
                ptIndexAt = GetDeaggregationPoint(simdata->campSlot, &ent);
                round++;
            }

            ShiAssert(
                ptIndexAt); // We must have at least two points (center and toward)
            TranslatePointData(ent, simdata->ptIndex, &simdata->x, &simdata->y);

            // Face toward the "at" point
            dx = PtDataTable[ptIndexAt].yOffset -
                 PtDataTable[simdata->ptIndex]
                     .yOffset; // KCK NOTE: axis' are reversed
            dy = PtDataTable[ptIndexAt].xOffset -
                 PtDataTable[simdata->ptIndex]
                     .xOffset; // KCK NOTE: axis' are reversed
            simdata->heading = (float)atan2(dx, dy);

            // If we reused a point, shift our center point along the at vector
            simdata->x += dx * round;
            simdata->y += dy * round;
        }
        else
        {
            // At sea
            dist = (simdata->vehicleInUnit - 1) >> 2;

            switch ((simdata->vehicleInUnit - 1) bitand 0x3)
            {
            case 0:
                simdata->x = XPos() - 1500.0f - 1500.0f * dist;
                simdata->y = YPos() - 1500.0f - 1500.0f * dist;
                break;

            case 1:
                simdata->x = XPos() + 1500.0f + 1500.0f * dist;
                simdata->y = YPos() - 1500.0f - 1500.0f * dist;
                break;

            case 2:
                simdata->x = XPos() + 1500.0f + 1500.0f * dist;
                simdata->y = YPos() + 1500.0f + 1500.0f * dist;
                break;

            case 3:
            default:
                simdata->x = XPos() - 1500.0f - 1500.0f * dist;
                simdata->y = YPos() + 1500.0f + 1500.0f * dist;
                break;
            }
        }
    }

    // We're always at sea level
    simdata->z = 0.0f;

    // Determine skill (Sim only uses it for anti-air stuff right now, so bow to expedience
    simdata->skill = ((TeamInfo[GetOwner()]->airDefenseExperience - 60) / 10) +
                     rand() % 3 - 1;
    // simdata->skill = ((TeamInfo[GetOwner()]->navalExperience - 60) / 10) + rand()%3 - 1;

    // Clamp it to legal sim side values
    if (simdata->skill > 4)
        simdata->skill = 4;

    if (simdata->skill < 0)
        simdata->skill = 0;

    // Weapon loadout
    for (i = 0; i < HARDPOINT_MAX; i++)
    {
        simdata->weapon[i] = GetUnitWeaponId(i, simdata->campSlot);

        if (simdata->weapon[i])
            simdata->weapons[i] = GetUnitWeaponCount(i, simdata->campSlot);
        else
            simdata->weapons[i] = 0;
    }

    simdata->playerSlot = NO_PILOT;
    simdata->waypointList = CloneWPToList(GetFirstUnitWP(), NULL);

    return MOTION_GND_AI;
}


int TaskForceClass::GetDeaggregationPoint(int slot, CampEntity *installation)
{
    int pt = 0, type;
    static int last_pt, index = 0;

    if (not *installation)
    {
        // We're looking for a new list, so clear statics
        last_pt = index = 0;

        // Check if we care about placement
        if (not Moving())
        {
            // Find the appropriate installation
            GridIndex x, y;
            Objective o;
            GetLocation(&x, &y);
            o = FindNearestObjective(x, y, NULL, 0);
            *installation = o;

            // Find the appropriate list
            if (o)
            {
                ObjClassDataType *oc = o->GetObjectiveClassData();
                index = oc->PtDataIndex;

                while (index)
                {
                    if (PtHeaderDataTable[index].type == DockListType)
                    {
                        // The first time we look, we just want to know if we have a list.
                        // Return now.
                        return index;
                    }

                    index = PtHeaderDataTable[index].nextHeader;
                }

#ifdef DEBUG
                FILE *fp = fopen("PtDatErr.log", "a");

                if (fp)
                {
                    char name[80];
                    o->GetName(name, 79, FALSE);
                    fprintf(fp, "Obj %s @ %d,%d: No header list of type %d.\n",
                            name, x, y, DockListType);
                    fclose(fp);
                }

#endif
            }
        }
    }

    if (index)
    {
        // We have a list, and want to find the correct point
        UnitClassDataType *uc = GetUnitClassData();
        VehicleClassDataType *vc = GetVehicleClassData(uc->VehicleType[slot]);

        // Check which type of point we're looking for
        // TODO: Check ship type here...
        // type = SmallDockPt;
        type = LargeDockPt;

        // Return the next point, if it's the base type
        // NOTE: Log error if we don't have enough points of this type
        if (last_pt)
        {
            last_pt = pt = GetNextPt(last_pt);
#ifdef DEBUG

            if (not pt or PtDataTable[pt].type not_eq type)
            {
                FILE *fp = fopen("PtDatErr.log", "a");

                if (fp)
                {
                    char name[80];
                    GridIndex x, y;
                    (*installation)->GetName(name, 79, FALSE);
                    (*installation)->GetLocation(&x, &y);
                    fprintf(fp,
                            "HeaderList %d (Obj %s @ %d,%d): Insufficient "
                            "points of type %d.\n",
                            index, name, x, y, type);
                    fclose(fp);
                }
            }

#endif
            return pt;
        }

        // Find one of the appropriate type
        pt = GetFirstPt(index);

        while (pt)
        {
            if (PtDataTable[pt].type == type)
            {
                last_pt = pt;
                return pt;
            }

            pt = GetNextPt(pt);
        }

#ifdef DEBUG
        FILE *fp = fopen("PtDatErr.log", "a");

        if (fp)
        {
            char name[80];
            GridIndex x, y;
            (*installation)->GetName(name, 79, FALSE);
            (*installation)->GetLocation(&x, &y);
            fprintf(fp,
                    "HeaderList %d (Obj %s @ %d,%d): No points of type %d.\n",
                    index, name, x, y, type);
            fclose(fp);
        }

#endif
    }

    return pt;
}


// ============================================
// Naval AI (Artscout - 2026)
//
// A ship with no waypoints used to sit in port for ever, or shuttle 20 km north and back. With
// g_nNavalAI set it now plans routes over water squares (A* with the Naval cost table):
//  - warships sortie from port after a rest and patrol out-and-back from where they are; they go
//    to port only when supply drops below 30%, and docking resupplies them;
//  - sea tankers, cargo and supply ships sail from port to port. A tanker that docks tops up the
//    team fuel pool, but only to cover missing refinery output, so it never competes with
//    working refineries (and it is one finite load per voyage plus a rest in port).
// Waypoints carry WPF_REPEAT because ResetCurrentWP skips a flag-less waypoint whose departure
// time has passed, so only a flagged one is sailed to by distance. The in-port rest is the first
// waypoint's station time. orders == NORD_TRANSPORT marks "on a voyage" (saved with the unit).
// ============================================

extern int g_nNavalAI;
// campsim NAVAL: [0] voyages/patrols planned over water, [1] plans that found no route, [2] fallbacks to the
// stock straight 20 km patrol (no terrain check on the way), [3] of those, started on a sea-mask land cell
int gNavalDiag[8][8]; // [4] moves onto a sea-mask land cell, [5] of those with the 45-degree turn limit
// bending the heading, [6] routes found only on the grid alone (sea mask relaxed)
extern int g_nNavalTankerFuel;

static int NavalIsSupportShip(TaskForce tf)
{
    int st = tf->GetSType();
    return st == STYPE_UNIT_SEA_TANKER or st == STYPE_UNIT_SEA_TRANSPORT or
           st == STYPE_UNIT_SEA_SUPPLY;
}

// A friendly, working port between mind and maxd grid squares away: the nearest one, or a random one.
static Objective NavalFindPort(TaskForce tf, GridIndex x, GridIndex y,
                               int mind, int maxd, int random)
{
    Objective best = NULL, o;
    float bestd = 1e9F;
    int seen = 0;
    VuListIterator myit(AllObjList);

    o = GetFirstObjective(&myit);

    while (o)
    {
        if (o->GetType() == TYPE_PORT and o->GetTeam() == tf->GetTeam() and
            o->GetObjectiveStatus() > 0)
        {
            GridIndex ox, oy;
            o->GetLocation(&ox, &oy);
            float d = Distance(x, y, ox, oy);

            if (d >= mind and d <= maxd)
            {
                if (random)
                {
                    seen++;

                    if (rand() % seen == 0)
                        best = o;
                }
                else if (d < bestd)
                {
                    bestd = d;
                    best = o;
                }
            }
        }

        o = GetNextObjective(&myit);
    }

    return best;
}

// Lay a water route from (x,y) to (tx,ty) as flagged waypoints, one per change of direction, so a
// straight line between two waypoints is exactly the path the search found. Returns 1 if laid.
// With append set the route is added after the existing waypoints (no rest, current waypoint kept).
static int NavalRoute(TaskForce tf, GridIndex x, GridIndex y, GridIndex tx,
                      GridIndex ty, CampaignTime rest, int append = 0)
{
    PathClass path;
    GridIndex cx = x, cy = y;
    int i, h, last = -1, laid = append ? 1 : 0;
    CampaignTime now = Camp_GetCurrentTime();
    WayPoint w;

    const int found = tf->GetUnitGridPath(&path, x, y, tx, ty);

    if (path.GetLength() < 3)
    {
        // campsim NAVAL: why a route failed (first 40)
        static int logged = 0;

        if (logged < 40)
        {
            logged++;
            printf("NAVALFAIL team %d (%d,%d)->(%d,%d) %.0f km: search %d, path %d, target cost %.0f, start "
                       "mask %d, target mask %d\n", tf->GetTeam(), x, y, tx, ty, Distance(x, y, tx, ty), found,
                       path.GetLength(), GetMovementCost(tx, ty, tf->GetMovementType(), 0, Here),
                       SeaMaskLand(x, y), SeaMaskLand(tx, ty));
        }

        return 0;
    }

    if (not append)
        tf->DisposeWayPoints();

    for (i = 0; i < path.GetLength(); i++)
    {
        h = path.GetDirection(i);

        if (h < 0 or h > 7)
            break;

        if (last >= 0 and h not_eq last)
        {
            w = tf->AddUnitWP(cx, cy, 0, 60, now, laid ? 0 : (int)rest, 0);
            w->SetWPFlags(WPF_REPEAT);
            laid++;
        }

        cx = (GridIndex)(cx + dx[h]);
        cy = (GridIndex)(cy + dy[h]);
        last = h;
    }

    w = tf->AddUnitWP(cx, cy, 0, 60, now, laid ? 0 : (int)rest, 0);
    w->SetWPFlags(WPF_REPEAT);

    if (not append)
        tf->SetCurrentWaypoint(1);

    return 1;
}

// Pick the next leg for a ship that has no waypoints. Returns 1 if it got a route.
static int NavalPlan(TaskForce tf, GridIndex x, GridIndex y, Objective inport)
{
    CampaignTime rest = 0;
    Objective dest = NULL;
    GridIndex tx = 0, ty = 0;
    int i, tries;

    if (NavalIsSupportShip(tf))
    {
        if (inport)
            rest = (60 + rand() % 90) * CampaignMinutes; // unload and reload

        dest = NavalFindPort(tf, x, y, 12, 90, TRUE);

        if (not dest)
            dest = NavalFindPort(tf, x, y, 12, 250, FALSE);

        if (not dest)
            return 0;

        dest->GetLocation(&tx, &ty);

        if (not NavalRoute(tf, x, y, tx, ty, rest))
            return 0;

        tf->SetUnitOrders(NORD_TRANSPORT);
        return 1;
    }

    if (inport)
        rest = (15 + rand() % 105) * CampaignMinutes;
    else if (tf->GetUnitSupply() < 30)
    {
        // Low on ammunition: the only reason a warship goes home
        Objective home = NavalFindPort(tf, x, y, 0, 400, FALSE);

        if (home)
        {
            home->GetLocation(&tx, &ty);

            if (NavalRoute(tf, x, y, tx, ty, 0))
                return 1;
        }
    }

    // Patrol: a loop S -> P1 -> P2 -> S over open water, S being where the ship is now. Every
    // route ends back at S, so the next plan starts from S again and the ship keeps its station
    // without storing one. The points sit on a ring around S (big ships close in, small ones
    // range wider) and are chosen away from the nearest hostile objective, so a patrol does not
    // start by steaming at the enemy coast. It docks only to resupply.
    {
        const int st = tf->GetSType();
        const int big = (st == STYPE_UNIT_CARRIER or st == STYPE_UNIT_CRUISER or
                         st == STYPE_UNIT_BATTLESHIP or
                         st == STYPE_UNIT_AMPHIBIOUS);
        const int rmin = big ? 10 : 15, rspan = big ? 15 : 30;
        float ex = 0.0F, ey = 0.0F, bestd = 150.0F;
        int haveEnemy = 0, k;
        GridIndex px[2] = {0, 0}, py[2] = {0, 0};
        float ang[2] = {0.0F, 0.0F};
        Objective o;
        VuListIterator myit(AllObjList);

        o = GetFirstObjective(&myit);

        while (o)
        {
            if (GetRoE(tf->GetTeam(), o->GetTeam(), ROE_GROUND_FIRE) ==
                ROE_ALLOWED)
            {
                GridIndex ox, oy;
                o->GetLocation(&ox, &oy);
                float d = Distance(x, y, ox, oy);

                if (d < bestd and d > 1.0F)
                {
                    bestd = d;
                    ex = (ox - x) / d;
                    ey = (oy - y) / d;
                    haveEnemy = 1;
                }
            }

            o = GetNextObjective(&myit);
        }

        for (k = 0; k < 2; k++)
        {
            int found = 0;

            for (tries = 0; tries < 16 and not found; tries++)
            {
                float a = (float)(rand() % 360) * 0.0174533F;
                float r = (float)(rmin + rand() % rspan);
                float cx = (float)cos(a), cy = (float)sin(a);
                GridIndex qx = (GridIndex)(x + cx * r),
                          qy = (GridIndex)(y + cy * r);

                if (qx < 0 or qy < 0 or qx >= Map_Max_X or qy >= Map_Max_Y or
                    not ShipWater(qx, qy))
                    continue;

                // Away from the enemy: not within 60 degrees of the way to them (relaxed on the
                // last few tries so a ship boxed in against the enemy coast still gets a patrol)
                if (haveEnemy and tries < 12 and cx * ex + cy * ey > 0.5F)
                    continue;

                // The second point must be well clear of the first
                if (k == 1 and Distance(px[0], py[0], qx, qy) < 10.0F)
                    continue;

                px[k] = qx;
                py[k] = qy;
                ang[k] = a;
                found = 1;
            }

            if (not found)
                break;
        }

        if (k == 2 and NavalRoute(tf, x, y, px[0], py[0], rest))
        {
            NavalRoute(tf, px[0], py[0], px[1], py[1], 0, 1);
            NavalRoute(tf, px[1], py[1], x, y, 0, 1);
            return 1;
        }

        // Could not find two points: out and back to one
        if (k >= 1 and NavalRoute(tf, x, y, px[0], py[0], rest))
        {
            NavalRoute(tf, px[0], py[0], x, y, 0, 1);
            return 1;
        }
    }

    return 0;
}

// A tanker that has docked: add one load to the team fuel pool, scaled by how much refinery
// output is missing (nothing at all while every refinery is working).
static void NavalDeliverFuel(TaskForce tf)
{
    Objective o;
    int n = 0, sum = 0, missing;
    VuListIterator myit(AllObjList);

    if (tf->GetSType() not_eq STYPE_UNIT_SEA_TANKER or g_nNavalTankerFuel <= 0)
        return;

    o = GetFirstObjective(&myit);

    while (o)
    {
        if (o->GetType() == TYPE_REFINERY and o->GetTeam() == tf->GetTeam())
        {
            n++;
            sum += o->GetObjectiveStatus();
        }

        o = GetNextObjective(&myit);
    }

    missing = n ? 100 - sum / n : 100;

    if (missing > 0)
    {
        Team t = tf->GetTeam();
        TeamInfo[t]->SetFuelAvail(TeamInfo[t]->GetFuelAvail() +
                                  g_nNavalTankerFuel * missing / 100);
    }
}

int TaskForceOrderStation(TaskForce tf, GridIndex tx, GridIndex ty)
{
    GridIndex x, y;

    if (not tf)
        return 0;

    // The map picture and the campaign's 1 km cover cells do not agree at a coast, so a drop that
    // looks like open water can be a land cell and the other way round. A drop within 3 km of a
    // friendly working port is an order to dock there; any other drop on land is moved to the
    // nearest water cell within 6 km (and refused if there is none).
    if (not ShipWater(tx, ty))
    {
        Objective port = NavalFindPort(tf, tx, ty, 0, 3, FALSE);

        if (port)
            port->GetLocation(&tx, &ty);
        else
        {
            int r, i, j, found = 0;

            for (r = 1; r <= 6 and not found; r++)
            {
                for (j = -r; j <= r and not found; j++)
                {
                    for (i = -r; i <= r and not found; i++)
                    {
                        if (abs(i) not_eq r and abs(j) not_eq r)
                            continue; // the ring only

                        GridIndex qx = (GridIndex)(tx + i), qy = (GridIndex)(ty + j);

                        if (qx >= 0 and qy >= 0 and qx < Map_Max_X and qy < Map_Max_Y and ShipWater(qx, qy))
                        {
                            tx = qx;
                            ty = qy;
                            found = 1;
                        }
                    }
                }
            }

            if (not found)
                return 0;
        }
    }

    tf->GetLocation(&x, &y);

    if (not NavalRoute(tf, x, y, tx, ty, 0))
    {
        // Too close for a route: just stay where we are
        if (Distance(x, y, tx, ty) > 3.0F)
            return 0;

        tf->DisposeWayPoints();
    }

    tf->SetUnitOrders(NORD_STATION);
    return 1;
}

void TaskForceReleaseStation(TaskForce tf)
{
    if (tf and tf->GetUnitOrders() == NORD_STATION)
        tf->SetUnitOrders(NORD_NONE);
}

int TaskForceClass::MoveUnit(CampaignTime time)
{
    GridIndex x = 0, y = 0;
    GridIndex nx = 0, ny = 0;
    GridIndex ox = 0, oy = 0;
    WayPoint w = NULL, ow = NULL;
    Objective o = NULL;
    int moving = 1;
    CampaignHeading h = 0;

    // RV - Biker
    // Naval units now have three modes:
    // (a) Sit still in harbor
    // (b) Do a 20 km track (repeating waypoints)
    // (c) Followy WPs

    GetLocation(&x, &y);

    w = ResetCurrentWP(this);

    FindNearestUnit(x, y, NULL);

    // Check for mode a
    o = FindNearestObjective(x, y, NULL, 1);

    // Artscout - 2026: a player-ordered ship holds its station once it has arrived
    if (not w and GetUnitOrders() == NORD_STATION)
        return TRUE;

    // Artscout - 2026 (NAVAL AI): plan the next leg instead of idling
    if (g_nNavalAI and not w)
    {
        Objective port = (o and o->GetType() == TYPE_PORT) ? o : NULL;

        if (port and port->GetTeam() == GetTeam())
        {
            // Docking resupplies a ship that is low (never lowers one above 100)
            if (GetUnitSupply() < 100)
                SetUnitSupply(100);

            // A tanker that has finished a voyage unloads here
            if (GetUnitOrders() == NORD_TRANSPORT)
            {
                NavalDeliverFuel(this);
                SetUnitOrders(NORD_NONE);
            }
        }

        extern int gNavalDiag[8][8];

        if (NavalPlan(this, x, y, port))
        {
            w = GetCurrentUnitWP();
            gNavalDiag[GetTeam() % 8][0]++;
        }
        else
            gNavalDiag[GetTeam() % 8][1]++;
    }

    // RV - Biker - If we are in port and have no WPs do nothing
    if (o and o->GetType() == TYPE_PORT and not w)
    {
        return TRUE;
    }

    // If not in port and no WPs... create a repeating path 20 km north and back
    if (not w)
    {
        extern int gNavalDiag[8][8];
        gNavalDiag[GetTeam() % 8][2]++;
        gNavalDiag[GetTeam() % 8][3] += SeaMaskLand(x, y) ? 1 : 0;
        DisposeWayPoints();

        w = AddUnitWP(x, y, 0, 60, TheCampaign.CurrentTime + (rand() % 15), 0,
                      0);
        w->SetWPFlags(WPF_REPEAT);

        // This should prevent naval units to run into ground
        if (ShipWater(x, y + 20))
        {
            w = AddUnitWP(x, y + 20, 0, 60,
                          TheCampaign.CurrentTime +
                              (15 + (rand() % 15)) * CampaignMinutes,
                          0, 0);
        }
        else
        {
            w = AddUnitWP(x, y, 0, 60,
                          TheCampaign.CurrentTime + 15 * CampaignMinutes, 0, 0);
        }

        w->SetWPFlags(WPF_REPEAT);

        w = AddUnitWP(x, y, 0, 60,
                      TheCampaign.CurrentTime +
                          (30 + (rand() % 15)) * CampaignMinutes,
                      0xffffffff, 0);
        w->SetWPFlags(WPF_REPEAT);

        SetCurrentWaypoint(1);
        w = GetCurrentUnitWP();
    }

    w->GetWPLocation(&nx, &ny);

    // RV - Biker - Wait for departure
    if (Camp_GetCurrentTime() < w->GetWPDepartureTime())
    {
        SetUnitLastMove(Camp_GetCurrentTime());
        return 0;
    }

    // Move, if we're not at destination
    if (x not_eq nx or y not_eq ny)
    {
        if (w)
            ow = w->GetPrevWP();

        if (ow)
            ow->GetWPLocation(&ox, &oy);
        else
            GetLocation(&ox, &oy);

        // Artscout - 2026 (g_bNavalMoveFix, 0 = stock): ChangeUnitLocation moves one cell a call, but this
        // loop steered every step of the tick from where the ship was when the tick began -- one compass
        // heading for the whole tick, and the arrival test (DirectionTo returns Here at the waypoint) never
        // saw the waypoint, so ships overshot their turns and cut across coasts.
        extern bool g_bNavalMoveFix;

        while (moving)
        {
            if (g_bNavalMoveFix)
                GetLocation(&x, &y);

            h = DirectionTo(ox, oy, nx, ny, x, y);

            if (h > 7)
            {
                moving = 0;
                h = Here;
            }

            // This is kinda hacky - basically, limit change in direction to 45 deg per move
            if (h > last_direction)
            {
                if (h - last_direction < 5)
                    h = (last_direction + 1) bitand 0x07;
                else
                    h = (last_direction + 7) bitand 0x07;
            }

            else if (h < last_direction)
            {
                if (last_direction - h < 5)
                    h = (last_direction + 7) bitand 0x07;
                else
                    h = (last_direction + 1) bitand 0x07;
            }

            //this moves the unit
            const int wanted = DirectionTo(ox, oy, nx, ny, x, y); // before the turn limit, for campsim NAVAL

            if (ChangeUnitLocation(h) > 0)
            {
                last_direction = h;

                GridIndex mx, my;
                GetLocation(&mx, &my);

                if (SeaMaskLand(mx, my))
                {
                    extern int gNavalDiag[8][8];
                    gNavalDiag[GetTeam() % 8][4]++;
                    gNavalDiag[GetTeam() % 8][5] += (h not_eq wanted) ? 1 : 0;
                }
            }
            else
            {
                moving = 0;
            }

            // Now do combat
            if (GetCombatTime() > CombatTime())
            {
                DoCombat();
            }
        }
    }

    return 0;
}

int TaskForceClass::DoCombat(void)
{
    int combat;
    SetCombatTime(TheCampaign.CurrentTime);

#if 0 // JPO mthis stuff now done in Choose Target - like Battalion
    // KCK: Super simple targetting (c)
    Team who = GetTeam();
    CampEntity e;
    FalconEntity *react_against = NULL, *air_react_against = NULL;
    int react, spot, best_reaction = 1, best_air_react = 1;
    int search_dist;
    float react_distance, air_react_distance, d;
    react_distance = air_react_distance = 9999.0F;

    SetEngaged(0);
    SetCombat(0);
    SetChecked();

    search_dist = GetDetectionRange(Air);
#ifdef VU_GRID_TREE_Y_MAJOR
    VuGridIterator detit(RealUnitProxList, YPos(), XPos(), (BIG_SCALAR)GridToSim(search_dist));
#else
    VuGridIterator detit(RealUnitProxList, XPos(), YPos(), (BIG_SCALAR)GridToSim(search_dist));
#endif
    e = (CampEntity)detit.GetFirst();

    while (e)
    {
        if (GetRoE(who, e->GetTeam(), ROE_GROUND_FIRE) == ROE_ALLOWED)
        {
            combat = 0;
            react = DetectVs(e, &d, &combat, &spot);

            if ( not e->IsFlight() and react >= best_reaction and d < react_distance)
            {
                // React vs a ground/Naval target
                best_reaction = react;
                react_distance = d;
                react_against = e;
                SetEngaged(1);
                SetCombat(combat);
            }
            else if (e->IsFlight() and react >= best_air_react and d < air_react_distance)
            {
                // React vs an air target -
                best_air_react = react;
                air_react_distance = d;
                air_react_against = e;

                if ( not e->IsAggregate())
                {
                    // Pick a specific aircraft in the flight if it's deaggregated
                    CampEnterCriticalSection();

                    if (e->GetComponents())
                    {
                        VuListIterator cit(e->GetComponents());
                        FalconEntity *fe;
                        float rsq, brsq = FLT_MAX;

                        fe = (FalconEntity *)cit.GetFirst();

                        while (fe)
                        {
                            rsq = DistSqu(XPos(), YPos(), fe->XPos(), fe->YPos());

                            if (rsq < brsq)
                            {
                                air_react_against = fe;
                                air_react_distance = (float)sqrt(rsq);
                                brsq = rsq;
                            }

                            fe = (FalconEntity *)cit.GetNext();
                        }
                    }

                    CampLeaveCriticalSection();
                }

                SetEngaged(1);
                SetCombat(combat);
            }
        }

        e = (CampEntity)detit.GetNext();
    }

    if (air_react_against)
        SetAirTarget(air_react_against);

    if (react_against)
        SetTarget(react_against);

#endif

    if (Engaged())
    {
        FalconEntity *e = GetTarget();
        FalconEntity *a = GetAirTarget();

        // Check vs our Ground Target
        if (not e)
            SetTarget(NULL);
        else
        {
            if (Combat() and IsAggregate())
            {
                combat = ::DoCombat(this, e);

                if (combat <= 0 or Targeted())
                    SetTargeted(0);
            }
        }

        // Check vs our Air Target
        if (not a)
            SetAirTarget(NULL);
        else if (Combat() and IsAggregate())
        {
            combat = ::DoCombat(this, a);

            if (combat < 0)
                SetAirTarget(
                    NULL); // Clear targeting data so we can look for another
        }
    }

    return 0;
}

int TaskForceClass::Reaction(CampEntity e, int knowledge, float range)
{
    int score = 0, enemy_threat_bonus = 1;
    CampEntity et = NULL;
    MoveType tmt, omt;

    if (not e)
        return 0;

    // Some basic info on us.
    omt = GetMovementType();
    tmt = e->GetMovementType();

    // Aircraft on ground are ignored (technically, we could shoot at them.. but..)
    if (e->IsFlight() and not((Flight)e)->Moving())
        return 0;

    // Score their threat to us
    if (knowledge bitand FRIENDLY_DETECTED)
        enemy_threat_bonus++;

    if (knowledge bitand FRIENDLY_IN_RANGE)
        enemy_threat_bonus += 2;

    et = ((Unit)e)->GetCampTarget();

    // All units score vs enemy engaged with this unit
    if (et == this)
        score += e->GetAproxHitChance(omt, 0) / 5 * enemy_threat_bonus;

    // Bonus if we can shoot them
    if (knowledge bitand ENEMY_IN_RANGE)
        score += GetAproxHitChance(tmt, FloatToInt32(range / 2.0F)) / 5;

    // Added bonus for them attacking
    if (et and (tmt == Air or tmt == LowAir))
        score += GetAproxHitChance(tmt, 0) / 5 * enemy_threat_bonus;

    return score;
}

/* 2002-02-11 COMMENTED OUT BY S.G. TOO MANY CHANGES TO TRACK THEM ALL (LIKE FOR OTHER UNIT TYPES)
   int TaskForceClass::DetectVs (AircraftClass *ac, float *d, int *combat, int *spot)
   {
   int react,det = Detected(this,ac,d);
   CampEntity e;

   if ( not (det bitand REACTION_MASK))
   return 0;

   e = ac->GetCampaignObject();
   react = Reaction(e,det,*d);
   if (det bitand ENEMY_IN_RANGE and react)
 *combat = 1;
 if (det bitand FRIENDLY_DETECTED)
 {
 SetSpotted(e->GetTeam(),TheCampaign.CurrentTime);
 *spot = 1;
 }
 return react;
 }
 */

extern int CheckValidType(CampEntity u, CampEntity e);
extern int CanItIdentify(CampEntity us, CampEntity them, float d, int mt);

int TaskForceClass::DetectVs(AircraftClass *ac, float *d, int *combat,
                             int *spot)
{
    int react, det = Detected(this, ac, d);
    CampEntity e;

    e = ac->GetCampaignObject();

    // 2001-03-22 ADDED BY S.G. DETECTION DOESN'T INCLUDED SPOTTED, ONLY THAT THIS ENTITY DETECTED THE OTHER BY ITSELF.
    int detTmp = det;

    // Check type of entity before GCI is used
    if (CheckValidType(this, e))
        detTmp or_eq e->GetSpotted(GetTeam()) ? ENEMY_DETECTED : 0;

    // Check type of entity before GCI is used
    if (CheckValidType(e, this))
        detTmp or_eq GetSpotted(e->GetTeam()) ? FRIENDLY_DETECTED : 0;

    if (not(detTmp bitand REACTION_MASK))
        return 0;

    react = Reaction(e, detTmp, *d);

    if (det bitand ENEMY_IN_RANGE and react)
        *combat = 1;

    // Spotting will be set only if our enemy is aggregated or if he's an AWAC. SensorFusion or GroundClass::Exec will hanlde deaggregated vehicles.
    // I can't let SensorFusion handle the spotting for AWAC because this will put a too big toll on the CPU
    // e has to be a flight since it is derived from an aircraft class so less checks needs to be done here then against flights below
    if (det bitand FRIENDLY_DETECTED)
    {
        // Spotting will be set only if our enemy is aggregated or if he's an AWAC. SensorFusion or GroundClass::Exec will hanlde deaggregated vehicles.
        if ((e->IsAggregate() and CheckValidType(e, this)) or
            (e->IsFlight() and e->GetSType() == STYPE_UNIT_AWACS))
        {
            SetSpotted(
                e->GetTeam(), TheCampaign.CurrentTime,
                CanItIdentify(
                    this, e, *d,
                    e->GetMovementType())); // 2002-02-11 MODIFIED BY S.G. Added 'CanItIdentify' which query if the target can be identified
            *spot = 1;
        }
    }

    return react;
}

/* 2002-02-11 COMMENTED OUT BY S.G. TOO MANY CHANGES TO TRACK THEM ALL (LIKE FOR OTHER UNIT TYPES)
   int TaskForceClass::DetectVs (CampEntity e, float *d, int *combat, int *spot)
   {
   int react,det;

   det = Detected(this,e,d);
   if ( not (det bitand REACTION_MASK))
   return 0;
   react = Reaction(e,det,*d);
   if (det bitand ENEMY_DETECTED)
   e->SetSpotted(GetTeam(),TheCampaign.CurrentTime);
   if (det bitand ENEMY_IN_RANGE and react)
 *combat = 1;
 if (det bitand FRIENDLY_DETECTED)
 {
 SetSpotted(e->GetTeam(),TheCampaign.CurrentTime);
 *spot = 1;
 }

 return react;
 }

 */
int TaskForceClass::DetectVs(CampEntity e, float *d, int *combat, int *spot)
{
    int react, det;

    det = Detected(this, e, d);

    int detTmp = det;

    // Check type of entity before GCI is used
    if (CheckValidType(this, e))
        detTmp or_eq e->GetSpotted(GetTeam()) ? ENEMY_DETECTED : 0;

    // Check type of entity before GCI is used
    if (CheckValidType(e, this))
        detTmp or_eq GetSpotted(e->GetTeam()) ? FRIENDLY_DETECTED : 0;

    if (not(detTmp bitand REACTION_MASK))
        return 0;

    react = Reaction(e, detTmp, *d);

    // We'll spot our enemy if we're not broken
    if (det bitand ENEMY_DETECTED)
    {
        if (IsAggregate() and CheckValidType(this, e))
            e->SetSpotted(
                GetTeam(), TheCampaign.CurrentTime,
                (CanItIdentify(
                    this, e, *d,
                    e->GetMovementType()))); // 2002-02-11 MODIFIED BY S.G. Say 'identified if it has the hability to identify
    }

    if (det bitand ENEMY_IN_RANGE and react)
        *combat = 1;

    if (det bitand FRIENDLY_DETECTED)
    {
        // Spotting will be set only if our enemy is aggregated or if he's an AWAC. SensorFusion or GroundClass::Exec will hanlde deaggregated vehicles.
        if ((e->IsAggregate() and CheckValidType(e, this)) or
            (e->IsFlight() and e->GetSType() == STYPE_UNIT_AWACS))
        {
            SetSpotted(
                e->GetTeam(), TheCampaign.CurrentTime,
                1); // 2002-02-11 Modified by S.G. Ground units are always identified (doesn't change a thing)
            *spot = 1;
        }
    }

    return react;
}


int TaskForceClass::ChooseTactic(void)
{
    return NULL;
}

int TaskForceClass::CheckTactic(int tid)
{
    return 0;
}

CampaignTime TaskForceClass::GetMoveTime(void)
{
    if (last_move and TheCampaign.CurrentTime > last_move)
        return TheCampaign.CurrentTime - last_move;

    last_move = TheCampaign.CurrentTime;

    return 0;
}

void TaskForceClass::GetRealPosition(float *x, float *y, float *z)
{
    // This will use the last move time to determine the real x,y bitand z of the unit
    float movetime =
        (float)(SimLibElapsedTime - last_move) / VU_TICS_PER_SECOND;
    float speed;
    float heading;
    float dist;
    int h = GetNextMoveDirection();
    mlTrig sincos;

    if (h < 0 or h > 7 or SimLibElapsedTime < last_move)
    {
        *x = XPos();
        *y = YPos();
        *z = TheMap.GetMEA(XPos(), YPos());
        return;
    }

    speed = (float)GetUnitSpeed() * KPH_TO_FPS;
    dist = speed * movetime;
    heading = h * 45.0F * DTR;
    mlSinCos(&sincos, heading);
    *x = XPos() + dist * sincos.cos;
    *y = YPos() + dist * sincos.sin;
    *z = TheMap.GetMEA(XPos(), YPos());
}

// ===========================================
// Support Functions
// ===========================================

WayPoint ResetCurrentWP(TaskForce tf)
{
    WayPoint w;
    GridIndex x, y, ux, uy;

    w = tf->GetCurrentUnitWP();

    while (w and w->GetWPDepartureTime() < Camp_GetCurrentTime())
    {
        if (w->GetWPFlags()) // Make sure we actually get here, it's important
        {
            w->GetWPLocation(&x, &y);
            tf->GetLocation(&ux, &uy);

            if (DistSqu(x, y, ux, uy) > 2.0F)
                return w;

            if (DoWPAction(tf, w))
                return tf->GetCurrentUnitWP();
        }

        w = w->GetNextWP();
        tf->SetCurrentUnitWP(w);
    }

    return w;
}

// Unit within operation area, needs to take it's Waypoint's action
// This currently is only called if this waypoint has a flag set
WayPoint DoWPAction(TaskForce tf, WayPoint w)
{
    WayPoint cw;

    if (not w or not tf)
        return NULL;

    // Check Actions
    /* int action = w->GetWPAction();
     switch (action)
     {
     case WP_NOTHING:
     default:
     break;
     }
     */

    // Check Flags
    if (w->GetWPFlags() bitand WPF_REPEAT)
    {
        // Check if we've been here long enough
        if (Camp_GetCurrentTime() > w->GetWPDepartureTime())
        {
            // If so, go on to the next wp (adjust their times from now)
            tf->AdjustWayPoints();
        }
        else
        {
            // If not, restore previous WP and readjust times
            cw = w->GetPrevWP();
            tf->SetCurrentUnitWP(cw);
            tf->AdjustWayPoints();
            return cw;
        }
    }

    return NULL;
}

//MI added function for movement
int TaskForceClass::DetectOnMove(void)
{
    if (not Engaged() and not(GetUnitMoved() % 5))
        return 0;

    return ChooseTarget();
}

// JPO addtions
int TaskForceClass::CanShootWeapon(int wid)
{
    if (WeaponDataTable[wid].GuidanceFlags bitand WEAP_RADAR and
        missiles_flying > 1)
        return FALSE;

    // Check for radar guidance, and make adjustments if necessary
    if (not(WeaponDataTable[wid].GuidanceFlags bitand WEAP_RADAR) or
        GetRadarMode() == FEC_RADAR_GUIDE or
        GetRadarMode() == FEC_RADAR_SEARCH_100)
        return TRUE;

    return FALSE;
}

void TaskForceClass::ReturnToSearch(void)
{
    if (missiles_flying < 1 and IsEmitting())
    {
        radar_mode = search_mode;

        if (radar_mode == FEC_RADAR_OFF)
            SetEmitting(0);
    }
    else if (not IsEmitting())
        radar_mode = FEC_RADAR_OFF;
}

int TaskForceClass::StepRadar(
    int t, int d,
    float range) //me123 modifyed to take tracking/detection parameter
{
    int radMode = GetRadarMode();

    if (IsAggregate())
    {
        // Check if we still have any radar vehicles
        if (class_data->RadarVehicle == 255 or
            not GetNumVehicles(class_data->RadarVehicle))
            return FEC_RADAR_OFF;

        // Check if we're already in our fire state
        if (radMode == FEC_RADAR_GUIDE or radMode == FEC_RADAR_SEARCH_100)
            return radMode;

        // Check for switch over to guide
        if (radMode == FEC_RADAR_AQUIRE)
        {
            SetRadarMode(FEC_RADAR_GUIDE);
            return FEC_RADAR_GUIDE;
        }
        else
        {
            SetRadarMode(FEC_RADAR_AQUIRE);

            // KCK: Good operators could shoot before going to guide mode. Check skill and return TRUE
            if (GetRadarMode() == FEC_RADAR_AQUIRE and
                rand() % 100 < TeamInfo[GetOwner()]->airDefenseExperience -
                                   MINIMUM_EXP_TO_FIRE_PREGUIDE)
                SetRadarMode(FEC_RADAR_GUIDE);

            return GetRadarMode();
        }
    }

    assert(range);
    /*
       FEC_RADAR_OFF 0x00     // Radar always off
       FEC_RADAR_SEARCH_100 0x01     // Search Radar - 100 % of the time (always on)
       FEC_RADAR_SEARCH_1 0x02     // Search Sequence #1
       FEC_RADAR_SEARCH_2 0x03     // Search Sequence #2
       FEC_RADAR_SEARCH_3 0x04     // Search Sequence #3
       FEC_RADAR_AQUIRE 0x05     // Aquire Mode (looking for a target)
       FEC_RADAR_GUIDE 0x06     // Missile in flight. Death is imminent*/


    // Check if we still have any radar vehicles
    if (class_data->RadarVehicle == 255 or
        not GetNumVehicles(class_data->RadarVehicle))
        return FEC_RADAR_OFF;

    assert(radarDatFileTable not_eq NULL);
    RadarDataSet *radarData =
        &radarDatFileTable
            [((VehicleClassDataType *)Falcon4ClassTable
                  [class_data->VehicleType[class_data->RadarVehicle]]
                      .dataPtr)
                 ->RadarType];


    // Check if we're already in our fire state
    if (radMode == FEC_RADAR_SEARCH_100)
        return radMode;

    // Check for switch over to guide
    float skill =
        TeamInfo[GetOwner()]->airDefenseExperience / 30.0f * 1000; // from 1 - 3
    skill *= (float)radarData->Timeskillfactor;
    skill /= 100.0f;
    float timetosearch;
    float timetoaquire;

    if (not d and not t)
        SetRadarMode(search_mode);

    if (GetRadarMode() == FEC_RADAR_CHANGEMODE and
        search_mode >= FEC_RADAR_SEARCH_1)
        SetRadarMode(search_mode); // we are changing mode.. realy not off

    switch (GetRadarMode())
    {
    case FEC_RADAR_OFF:
        timetosearch = radarData->Timetosearch1 - skill;

        if (range <= radarData->Rangetosearch1 and not SEARCHtimer)
            SEARCHtimer = SimLibElapsedTime;
        else if (range >= radarData->Rangetosearch1 or
                 SimLibElapsedTime - SEARCHtimer > timetosearch + 6000.0f)
            SEARCHtimer = 0;

        if (range <= radarData->Rangetosearch1 and SEARCHtimer and
            SimLibElapsedTime - SEARCHtimer > timetosearch)
        {
            SEARCHtimer = SimLibElapsedTime;
            search_mode = FEC_RADAR_SEARCH_1;
            SetRadarMode(FEC_RADAR_SEARCH_1);
        }

        break;

    case FEC_RADAR_SEARCH_1:
        AQUIREtimer = SimLibElapsedTime;

        if (not SEARCHtimer)
            SEARCHtimer = SimLibElapsedTime;

        timetosearch = radarData->Timetosearch1 - skill;

        if (d and range <= radarData->Rangetosearch2 and
            SimLibElapsedTime - SEARCHtimer >= timetosearch)
        {
            SetRadarMode(FEC_RADAR_CHANGEMODE);
            search_mode = FEC_RADAR_SEARCH_2;
            SEARCHtimer = SimLibElapsedTime;
        }

        break;

    case FEC_RADAR_SEARCH_2:
        AQUIREtimer = SimLibElapsedTime;
        timetosearch = radarData->Timetosearch2 - skill;

        if (not SEARCHtimer)
            SEARCHtimer = SimLibElapsedTime;

        if (d and range <= radarData->Rangetosearch3 and
            SimLibElapsedTime - SEARCHtimer >= timetosearch)
        {
            search_mode = FEC_RADAR_SEARCH_3;
            SetRadarMode(FEC_RADAR_CHANGEMODE);
            SEARCHtimer = SimLibElapsedTime;
        }
        else if (not d) // no detection step search down
        {
            SetRadarMode(FEC_RADAR_CHANGEMODE);
            search_mode = FEC_RADAR_SEARCH_1;
            SEARCHtimer = SimLibElapsedTime;
        }

        break;

    case FEC_RADAR_SEARCH_3:
        AQUIREtimer = SimLibElapsedTime;
        timetosearch = radarData->Timetosearch3 - skill;

        if (not SEARCHtimer)
            SEARCHtimer = SimLibElapsedTime;

        // goto aquire ?
        if (d and range <= radarData->Rangetoacuire and
            SimLibElapsedTime - SEARCHtimer >= timetosearch)
        {
            search_mode = FEC_RADAR_AQUIRE;
            SetRadarMode(FEC_RADAR_CHANGEMODE);
            AQUIREtimer = SimLibElapsedTime;
        }
        else if (not d) //  no detection step search down
        {
            SetRadarMode(FEC_RADAR_CHANGEMODE);
            search_mode = FEC_RADAR_SEARCH_2;
            SEARCHtimer = SimLibElapsedTime;
        }

        break;

    case FEC_RADAR_AQUIRE:
        SEARCHtimer = 0;
        timetoaquire = radarData->Timetoacuire - skill;

        // only allow to be in aquire for the coast amount of time
        if (not t and not d and
            SimLibElapsedTime - AQUIREtimer >= (unsigned)radarData->Timetocoast)
        {
            SetRadarMode(FEC_RADAR_CHANGEMODE);
            search_mode = FEC_RADAR_SEARCH_3;
        }
        else if (t and range <= radarData->Rangetoguide and
                 SimLibElapsedTime - AQUIREtimer >= timetoaquire)
        {
            search_mode = FEC_RADAR_GUIDE;
            SetRadarMode(FEC_RADAR_CHANGEMODE);
            return FEC_RADAR_GUIDE;
        }

        break;

    case FEC_RADAR_GUIDE:
        AQUIREtimer = SimLibElapsedTime;
        search_mode = FEC_RADAR_AQUIRE;

        if (not t)
            SetRadarMode(FEC_RADAR_CHANGEMODE);

        break;
    }


    /* else if (SimLibElapsedTime - AQUIREtimer > timetoaquire)
     {
    // KCK: Good operators could shoot before going to guide mode. Check skill and return TRUE
    if (GetRadarMode() == FEC_RADAR_AQUIRE and rand()%100 < TeamInfo[GetOwner()]->airDefenseExperience - MINIMUM_EXP_TO_FIRE_PREGUIDE)
    {
    search_mode = FEC_RADAR_AQUIRE ;
    SetRadarMode(FEC_RADAR_GUIDE);
    }
    }
     */
    int out = GetRadarMode();

    if (out == FEC_RADAR_OFF)
        out = search_mode;

    return out;
}

int TaskForceClass::ChooseTarget(void)
{
    FalconEntity *artTarget, *react_against = NULL, *air_react_against = NULL;
    CampEntity e;
    float d, react_distance, air_react_distance;
    int react, best_reaction = 1, best_air_react = 1, combat, retval = 0,
               pass = 0, spot = 0, estr = 0, capture = 0, nomove = 0;
    int search_dist;
    Team who;

    if (IsChecked())
        return Engaged();

    who = GetTeam();
    react_distance = air_react_distance = 9999.0F;

#ifdef DEBUG
    DWORD timec = GetTickCount();
#endif

    // Special case for fire support
    if (Targeted())
        artTarget = GetTarget(); // Save our target
    else
        artTarget = NULL;

    SetEngaged(0);
    SetCombat(0);
    SetChecked();

    search_dist = GetDetectionRange(Air);

    if (search_dist < MAX_GROUND_SEARCH)
        search_dist = MAX_GROUND_SEARCH;

#ifdef VU_GRID_TREE_Y_MAJOR
    VuGridIterator detit(RealUnitProxList, YPos(), XPos(),
                         (BIG_SCALAR)GridToSim(search_dist));
#else
    VuGridIterator detit(RealUnitProxList, XPos(), YPos(),
                         (BIG_SCALAR)GridToSim(search_dist));
#endif
    //  CalculateSOJ(detit); 2002-02-19 REMOVED BY S.G. eFalcon 1.10 SOJ code removed

    e = (CampEntity)detit.GetFirst();

    while (e)
    {
        if (GetRoE(who, e->GetTeam(), ROE_GROUND_FIRE) == ROE_ALLOWED)
        {
            combat = 0;
            react = DetectVs(e, &d, &combat, &spot);

            if (not e->IsFlight() and react >= best_reaction and
                d < react_distance)
            {
                // React vs a ground/Naval target
                best_reaction = react;
                react_distance = d;
                react_against = e;
                SetEngaged(1);
                SetCombat(combat);
            }
            else if (e->IsFlight() and react >= best_air_react and
                     d < air_react_distance)
            {
                // React vs an air target -
                best_air_react = react;
                air_react_distance = d;
                air_react_against = e;

                if (not e->IsAggregate())
                {
                    // Pick a specific aircraft in the flight if it's deaggregated
                    CampEnterCriticalSection();

                    if (e->GetComponents())
                    {
                        VuListIterator cit(e->GetComponents());
                        FalconEntity *fe;
                        float rsq, brsq = FLT_MAX;

                        fe = (FalconEntity *)cit.GetFirst();

                        while (fe)
                        {
                            rsq =
                                DistSqu(XPos(), YPos(), fe->XPos(), fe->YPos());

                            if (rsq < brsq)
                            {
                                air_react_against = fe;
                                air_react_distance = (float)sqrt(rsq);
                                brsq = rsq;
                            }

                            fe = (FalconEntity *)cit.GetNext();
                        }
                    }

                    CampLeaveCriticalSection();
                }

                // Make sure our radar is on (if we have one)
                if (not IsEmitting() and class_data->RadarVehicle < 255 and
                    GetNumVehicles(class_data->RadarVehicle))
                    SetEmitting(1);

                SetEngaged(1);
                SetCombat(combat);
            }
        }

        e = (CampEntity)detit.GetNext();
    }

    SetOdds((GetTotalVehicles() * 10) / (estr + 10));

    if (not Parent() and best_reaction > 1)
        EngageParent(this, react_against);

    if (air_react_against)
    {
        SetAirTarget(air_react_against);
        retval = 1;
    }

    if (react_against)
    {
        SetTarget(react_against);
        SetTargeted(0);
        retval = 1;
    }
    else if (artTarget and
             (not artTarget->IsUnit() or ((Unit)artTarget)->Engaged()) and
             orders == GORD_SUPPORT)
    {
        // Keep blowing away this target until the target gets out of range, disengages, or we get new orders
        // (Target will get reset after a null DoCombat result)
        SetTarget(artTarget);
        SetTargeted(1);
        SetEngaged(1);
        SetCombat(1);
        return -1; // We want to sit here and shoot until we can't any longer
    }

    if (nomove)
        return -1;

    return retval;
}
