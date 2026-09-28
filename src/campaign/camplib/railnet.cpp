// Artscout - 2026: railway routes and trains. See railnet.h.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <mutex>
#include <string>
#include <vector>
#include "cmpglobl.h"
#include "campaign.h"
#include "objectiv.h"
#include "unit.h"
#include "battalion.h"
#include "find.h"
#include "team.h"
#include "classtbl.h"
#include "cmpclass.h"
#include "falcsess.h"
#include "falcgame.h"
#include "fflog.h"
#include "railnet.h"

extern bool g_bRailTrains;
extern char g_strRailTrainLines[];
extern int g_nRailTrainSpeed;
extern int g_nRailTrainDwell;
extern int g_nRailFrontStandoff;
extern int g_nRailRunKm;
extern int g_nRailRespawnHours;
extern char FalconTerrainDataDir[];

namespace
{
const float KPH_FPS = 0.911344F;           // 1 km/h in ft/s
const float CAR_SPACING_FT = 70.0F;        // one KrAz and a gap
const float SAMPLE_FT = 2.0F * GRID_SIZE_FT; // ownership sampled every 2 km
const float HUB_REACH_FT = 4.0F * GRID_SIZE_FT; // a hub this close counts as on the line
const float MIN_RUN_FT = 20.0F * GRID_SIZE_FT;  // less friendly track than this: no train
const float ENLIST_REACH_FT = 3.0F * GRID_SIZE_FT; // a TE Supply battalion this close to a line is its train

// A route in sim feet: x north, y east (the sim's own axes), s = distance
// along it from the first point.
struct Route
{
    std::string name;
    std::vector<float> x, y, s;
    float len;
};

// One per configured route. `s` runs from sRear to sFront; the pair can be
// either way round along the route.
struct Train
{
    int route;
    VU_ID id;
    int team;
    float sRear, sFront;
    double t0;            // game time the current cycle started, seconds
    bool running;         // t0 and the termini are set
    double deadSince;     // game seconds when the train was found dead (< 0 = not dead)
    bool noTermini;       // last termini search failed (logged once)
};

// vuxGameTime in seconds, as a double: the cycle maths subtract times, and an
// unsigned millisecond clock would wrap on "now - how far into the cycle".
double GameSeconds()
{
    return (double)vuxGameTime / 1000.0;
}

std::mutex g_lock;
std::vector<Route> g_routes;
std::vector<Train> g_trains;
std::string g_loadedDir;
bool g_loadTried = false;

void Log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    strcat_s(buf, sizeof buf, "\n");
    FFDebugLog(buf);
}

// ---------------------------------------------------------------- routes

bool LoadRoutes()
{
    if (g_loadTried and g_loadedDir == FalconTerrainDataDir)
        return not g_routes.empty();

    g_loadTried = true;
    g_loadedDir = FalconTerrainDataDir;
    g_routes.clear();
    g_trains.clear();

    char path[_MAX_PATH];
    sprintf_s(path, "%s\\rail.txt", FalconTerrainDataDir);
    FILE *f = nullptr;

    if (fopen_s(&f, path, "r") or not f)
    {
        Log("rail: no %s -- run tools/campaign-editor/osm_rail.py", path);
        return false;
    }

    char line[512];

    if (not fgets(line, sizeof line, f) or strncmp(line, "ffrail 1", 8))
    {
        Log("rail: %s is not an ffrail 1 file", path);
        fclose(f);
        return false;
    }

    while (fgets(line, sizeof line, f))
    {
        int n = 0, used = 0;

        if (strncmp(line, "route ", 6) or sscanf_s(line + 6, "%d %n", &n, &used) < 1 or
            n < 2)
            continue;

        Route r;
        r.name = line + 6 + used;

        while (not r.name.empty() and (r.name.back() == '\n' or r.name.back() == '\r'))
            r.name.pop_back();

        r.len = 0.0F;

        for (int i = 0; i < n and fgets(line, sizeof line, f); i++)
        {
            float kx, ky;

            if (sscanf_s(line, "%f %f", &kx, &ky) not_eq 2)
                break;

            // Campaign km, x east / y north -> sim feet, x north / y east.
            float sx = ky * GRID_SIZE_FT, sy = kx * GRID_SIZE_FT;

            if (not r.x.empty())
                r.len += hypotf(sx - r.x.back(), sy - r.y.back());

            r.x.push_back(sx);
            r.y.push_back(sy);
            r.s.push_back(r.len);
        }

        if (r.x.size() >= 2)
            g_routes.push_back(r);
    }

    fclose(f);
    Log("rail: %d routes from %s", (int)g_routes.size(), path);

    for (const Route &r : g_routes)
        Log("rail:   %-28s %6.1f km", r.name.c_str(), r.len / GRID_SIZE_FT);

    return not g_routes.empty();
}

// Position and direction of travel (increasing s) at distance s.
void PointAt(const Route &r, float s, float *x, float *y, float *yaw)
{
    if (s < 0.0F)
        s = 0.0F;

    if (s > r.len)
        s = r.len;

    size_t lo = 0, hi = r.s.size() - 1;

    while (hi - lo > 1)
    {
        size_t mid = (lo + hi) / 2;

        if (r.s[mid] <= s)
            lo = mid;
        else
            hi = mid;
    }

    float seg = r.s[hi] - r.s[lo];
    float t = seg > 0.0F ? (s - r.s[lo]) / seg : 0.0F;
    *x = r.x[lo] + (r.x[hi] - r.x[lo]) * t;
    *y = r.y[lo] + (r.y[hi] - r.y[lo]) * t;
    *yaw = atan2f(r.y[hi] - r.y[lo], r.x[hi] - r.x[lo]);
}

// Distance along the route of the point nearest (x, y); *off = how far away.
float Project(const Route &r, float x, float y, float *off)
{
    float best = 1e30F, bestS = 0.0F;

    for (size_t i = 0; i + 1 < r.x.size(); i++)
    {
        float ax = r.x[i], ay = r.y[i];
        float dx = r.x[i + 1] - ax, dy = r.y[i + 1] - ay;
        float l2 = dx * dx + dy * dy;
        float t = l2 > 0.0F ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0.0F;
        t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
        float px = ax + dx * t - x, py = ay + dy * t - y;
        float d2 = px * px + py * py;

        if (d2 < best)
        {
            best = d2;
            bestS = r.s[i] + (r.s[i + 1] - r.s[i]) * t;
        }
    }

    if (off)
        *off = sqrtf(best);

    return bestS;
}

GridIndex GridOf(float simCoord)
{
    return (GridIndex)(simCoord / GRID_SIZE_FT);
}

// ---------------------------------------------------------------- motion

float SpeedFps()
{
    int kph = g_nRailTrainSpeed > 5 ? g_nRailTrainSpeed : 5;
    return kph * KPH_FPS;
}

float DwellSec()
{
    return (g_nRailTrainDwell > 0 ? g_nRailTrainDwell : 0) * 60.0F;
}

// Where along the route the train is at game time `now`, which way it is
// heading (+1 = increasing s) and its speed. One cycle: dwell at the hub,
// run to the front, dwell there, run back.
float TrainS(const Train &t, double now, float *dir, float *speed)
{
    float v = SpeedFps();
    float sign = t.sFront >= t.sRear ? 1.0F : -1.0F;
    float run = fabsf(t.sFront - t.sRear) / v;
    float dwell = DwellSec();
    float period = 2.0F * (run + dwell);

    if (period <= 0.0F)
    {
        *dir = sign;
        *speed = 0.0F;
        return t.sRear;
    }

    float tau = (float)fmod(now - t.t0, (double)period);

    if (tau < 0.0F)
        tau += period;

    if (tau < dwell) // loading at the hub, facing the front
    {
        *dir = sign;
        *speed = 0.0F;
        return t.sRear;
    }

    tau -= dwell;

    if (tau < run) // outbound
    {
        *dir = sign;
        *speed = v;
        return t.sRear + sign * v * tau;
    }

    tau -= run;

    if (tau < dwell) // unloading at the front, turned for home
    {
        *dir = -sign;
        *speed = 0.0F;
        return t.sFront;
    }

    tau -= dwell;
    *dir = -sign;
    *speed = v;
    return t.sFront - sign * v * tau;
}

// Re-base the cycle so the train is at `s` right now, heading toward the
// front (outbound) or back to the hub. Used when a train is adopted after a
// load and whenever the termini move, so the train never jumps.
void SetPhase(Train &t, float s, bool outbound, double now)
{
    float v = SpeedFps();
    float dwell = DwellSec();
    float run = fabsf(t.sFront - t.sRear) / v;
    float lo = t.sRear < t.sFront ? t.sRear : t.sFront;
    float hi = t.sRear < t.sFront ? t.sFront : t.sRear;
    s = s < lo ? lo : (s > hi ? hi : s);
    float tau = outbound ? dwell + fabsf(s - t.sRear) / v
                         : 2.0F * dwell + run + fabsf(t.sFront - s) / v;
    t.t0 = now - tau;
    t.running = true;
}

// ---------------------------------------------------------------- the front

bool IsHostile(int a, int b)
{
    return a not_eq b and GetTTRelations((Team)a, (Team)b) >= Hostile;
}

struct Termini
{
    bool ok;
    int team;
    float sRear, sFront;
    Objective hub;
};

// Where a train on this route should shuttle, for `team` (-1 = whichever team
// holds most of the route). The route is sampled every 2 km and each sample
// takes the owner of the nearest objective; the longest friendly run is the
// train's world. Its end next to hostile ground is the front, pulled back by
// RailFrontStandoff; the rear is the friendly supply source (IsSupplySource:
// city, port, depot, army base, not on the front) furthest back along the run
// within RailRunKm.
Termini FindTermini(const Route &r, int team)
{
    Termini out = {false, team, 0.0F, 0.0F, nullptr};
    int n = (int)(r.len / SAMPLE_FT) + 1;
    std::vector<int> owner(n);
    int count[NUM_TEAMS] = {0};

    for (int i = 0; i < n; i++)
    {
        float x, y, yaw;
        PointAt(r, i * SAMPLE_FT, &x, &y, &yaw);
        Objective o = FindNearestObjective(GridOf(y), GridOf(x), NULL);
        owner[i] = o ? o->GetTeam() : 0;

        if (owner[i] > 0 and owner[i] < NUM_TEAMS)
            count[owner[i]]++;
    }

    if (team < 0)
    {
        team = 0;

        for (int t = 1; t < NUM_TEAMS; t++)
            if (count[t] > count[team])
                team = t;
    }

    out.team = team;

    if (team <= 0)
        return out;

    // Longest run of samples not hostile to the team.
    int bestA = -1, bestB = -1;

    for (int i = 0; i < n;)
    {
        if (IsHostile(team, owner[i]))
        {
            i++;
            continue;
        }

        int j = i;

        while (j + 1 < n and not IsHostile(team, owner[j + 1]))
            j++;

        if (bestA < 0 or j - i > bestB - bestA)
        {
            bestA = i;
            bestB = j;
        }

        i = j + 1;
    }

    if (bestA < 0)
        return out;

    float sA = bestA * SAMPLE_FT, sB = bestB * SAMPLE_FT;

    if (sB > r.len)
        sB = r.len;

    // Which end faces the enemy? The one with hostile ground just past it;
    // if both or neither, the one nearer any hostile objective.
    bool hostA = bestA > 0, hostB = bestB < n - 1;
    bool frontIsB;

    if (hostA not_eq hostB)
        frontIsB = hostB;
    else
    {
        float ax, ay, bx, by, yaw, dA = 1e30F, dB = 1e30F;
        PointAt(r, sA, &ax, &ay, &yaw);
        PointAt(r, sB, &bx, &by, &yaw);
        VuListIterator it(AllObjList);

        for (Objective o = GetFirstObjective(&it); o; o = GetNextObjective(&it))
        {
            if (not IsHostile(team, o->GetTeam()))
                continue;

            float ox = o->XPos(), oy = o->YPos();
            float da = hypotf(ox - ax, oy - ay), db = hypotf(ox - bx, oy - by);
            dA = da < dA ? da : dA;
            dB = db < dB ? db : dB;
        }

        frontIsB = dB < dA;
    }

    float standoff = (g_nRailFrontStandoff > 0 ? g_nRailFrontStandoff : 0) * GRID_SIZE_FT;
    float runMax = (g_nRailRunKm > 20 ? g_nRailRunKm : 20) * GRID_SIZE_FT;
    float sign = frontIsB ? 1.0F : -1.0F; // direction of the front along s
    float front = frontIsB ? sB - standoff : sA + standoff;
    float farEnd = frontIsB ? sA : sB;

    // The hub: the friendly supply source near the line furthest back from
    // the front but inside runMax. Failing that, the far end of the run.
    float rear = farEnd;

    if (fabsf(front - rear) > runMax)
        rear = front - sign * runMax;

    Objective hub = nullptr;
    float hubBack = 0.0F;
    VuListIterator it(AllObjList);

    for (Objective o = GetFirstObjective(&it); o; o = GetNextObjective(&it))
    {
        if (IsHostile(team, o->GetTeam()) or not o->IsSupplySource())
            continue;

        float off;
        float s = Project(r, o->XPos(), o->YPos(), &off);
        float back = (front - s) * sign; // how far behind the front

        if (off > HUB_REACH_FT or back < MIN_RUN_FT or back > runMax)
            continue;

        if ((s - sA) * (s - sB) > 0.0F) // outside the friendly run
            continue;

        if (back > hubBack)
        {
            hubBack = back;
            hub = o;
            rear = s;
        }
    }

    if (fabsf(front - rear) < MIN_RUN_FT)
        return out;

    out.ok = true;
    out.sFront = front;
    out.sRear = rear;
    out.hub = hub;
    return out;
}

// ---------------------------------------------------------------- units

// Callers hold g_lock.
Train *FindTrainById(VU_ID id)
{
    for (Train &t : g_trains)
        if (t.id == id)
            return &t;

    return nullptr;
}

Train *FindTrainByRoute(int route)
{
    for (Train &t : g_trains)
        if (t.route == route)
            return &t;

    return nullptr;
}

bool RouteWanted(const Route &r)
{
    const char *list = g_strRailTrainLines[0] ? g_strRailTrainLines : "Pyongbu";
    char buf[0x100];
    strncpy_s(buf, list, _TRUNCATE);
    char *ctx = nullptr;

    for (char *tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx))
    {
        while (*tok == ' ')
            tok++;

        if (*tok and _strnicmp(r.name.c_str(), tok, strlen(tok)) == 0)
            return true;
    }

    return false;
}

// An existing train unit (after a load) that sits on this route and is not
// already running on another one.
Unit AdoptTrain(const Route &r, const std::vector<VU_ID> &claimed, float *sOut)
{
    VuListIterator it(AllUnitList);

    for (Unit u = GetFirstUnit(&it); u; u = GetNextUnit(&it))
    {
        if (not u->IsBattalion() or not u->IsTrain() or u->IsDead())
            continue;

        bool taken = false;

        for (const VU_ID &id : claimed)
            taken = taken or id == u->Id();

        if (taken)
            continue;

        float off;
        float s = Project(r, u->XPos(), u->YPos(), &off);

        if (off < 3.0F * GRID_SIZE_FT)
        {
            *sOut = s;
            return u;
        }
    }

    return nullptr;
}

Unit SpawnTrain(const Route &r, const Termini &tm)
{
    float x, y, yaw;
    PointAt(r, tm.sRear, &x, &y, &yaw);
    GridIndex gx = GridOf(y), gy = GridOf(x);
    Objective o = tm.hub ? tm.hub : FindNearestObjective(gx, gy, NULL);

    if (not o or IsHostile(tm.team, o->GetTeam()))
        return nullptr;

    // Supply battalion, sptype 1: sixteen KrAz T-255B trucks in the Korea
    // class table, our boxcars until the train has models of its own.
    Unit u = NewUnit(DOMAIN_LAND, TYPE_BATTALION, STYPE_UNIT_SUPPLY, 1, NULL);

    if (not u)
    {
        Log("rail: no Supply battalion class in this theater -- no train on %s",
            r.name.c_str());
        return nullptr;
    }

    u->SetOwner(o->GetOwner());
    // NewUnit built the roster before there was an owner, sized by team 0's
    // vehicle limits -- rebuild it for the real team, or UpdateUnit kills an
    // empty battalion on its first pass.
    u->BuildElements();
    u->ResetLocations(gx, gy);
    u->ResetDestinations(gx, gy);
    u->SetUnitOrders(GORD_RESERVE, o->Id());
    u->SetTrain(1);
    u->SetDontPlan(1);
    u->SetScripted(1); // keeps the ground tasking manager's hands off it
    return u;
}

// Tactical engagement: a Supply battalion placed on a rail line (TE editor, right-click the map,
// Add Battalion, Equipment "Arty/Rocket", Unit Type "Supply") becomes that line's train. TE only --
// in a campaign the Supply battalions are the campaign's own and must not be taken over.
void EnlistPlacedTrains()
{
    if (FalconLocalGame->GetGameType() not_eq game_TacticalEngagement)
        return;

    VuListIterator it(AllUnitList);

    for (Unit u = GetFirstUnit(&it); u; u = GetNextUnit(&it))
    {
        if (not u->IsBattalion() or u->IsTrain() or u->IsDead() or
            u->GetSType() not_eq STYPE_UNIT_SUPPLY)
            continue;

        // The nearest line, not the first within reach: lines meet at junctions.
        const Route *best = nullptr;
        float bestOff = ENLIST_REACH_FT;

        for (const Route &r : g_routes)
        {
            float off;
            Project(r, u->XPos(), u->YPos(), &off);

            if (off < bestOff)
            {
                bestOff = off;
                best = &r;
            }
        }

        if (best)
        {
            u->SetTrain(1);
            u->SetDontPlan(1);
            u->SetScripted(1);
            Log("rail: Supply battalion %d placed %.1f km from the %s -- it is that line's train",
                u->GetCampID(), bestOff / GRID_SIZE_FT, best->name.c_str());
        }
    }
}
} // namespace

// ---------------------------------------------------------------- UnitClass

void UnitClass::SetTrain(int p)
{
    if (p and not(unit_flags bitand U_TRAIN))
    {
        unit_flags or_eq U_TRAIN;
        MakeUnitDirty(DIRTY_UNIT_FLAGS, SEND_SOON);
    }
    else if (not p and (unit_flags bitand U_TRAIN))
    {
        unit_flags and_eq compl U_TRAIN;
        MakeUnitDirty(DIRTY_UNIT_FLAGS, SEND_SOON);
    }
}

// ---------------------------------------------------------------- entry points

// Locking: g_routes only changes inside LoadRoutes when the theater changes
// (the campaign map may also call it, but by then the routes are loaded and it
// returns at once), so the tick reads routes without the lock. Train records
// are copied out under the lock, worked on, and written back, so the objective
// scans in FindTermini never hold up the sim thread's per-car RailTrainPose.
void RailCampaignTick(int startup)
{
    if (not g_bRailTrains or not FalconLocalGame or not FalconLocalGame->IsLocal())
        return;

    {
        std::lock_guard<std::mutex> hold(g_lock);

        if (not LoadRoutes())
            return;
    }

    double now = GameSeconds();
    EnlistPlacedTrains();

    for (int ri = 0; ri < (int)g_routes.size(); ri++)
    {
        const Route &r = g_routes[ri];

        // A route not in RailTrainLines still runs a train someone placed on it (TE), but
        // gets no automatic one and no replacement when it dies.
        const bool wanted = RouteWanted(r);

        if (not wanted)
        {
            bool have;
            {
                std::lock_guard<std::mutex> hold(g_lock);
                Train *rec = FindTrainByRoute(ri);
                have = rec and rec->id not_eq FalconNullId;
            }
            float ignored;
            std::vector<VU_ID> none;

            if (not have and not AdoptTrain(r, none, &ignored))
                continue;
        }

        Train t;
        std::vector<VU_ID> claimed;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            Train *rec = FindTrainByRoute(ri);

            if (not rec)
            {
                Train fresh = {ri, FalconNullId, 0, 0.0F, 0.0F, 0.0, false, -1.0, false};
                g_trains.push_back(fresh);
                rec = &g_trains.back();
            }

            t = *rec;

            for (const Train &o : g_trains)
                if (o.route not_eq ri and o.id not_eq FalconNullId)
                    claimed.push_back(o.id);
        }

        Unit u = t.id == FalconNullId ? nullptr : (Unit)vuDatabase->Find(t.id);

        if (u and (u->IsDead() or not u->IsTrain()))
            u = nullptr;

        // After a load the record is new but the train is in the save.
        float adoptS = -1.0F;

        if (not u and t.id == FalconNullId)
        {
            u = AdoptTrain(r, claimed, &adoptS);

            if (u)
            {
                t.id = u->Id();
                t.running = false;
                Log("rail: %s -- picked up train %d from the save", r.name.c_str(),
                    u->GetCampID());
            }
        }

        bool skip = false;

        if (not u and t.id not_eq FalconNullId)
        {
            // It was running and now it is gone: destroyed.
            if (t.deadSince < 0.0)
            {
                t.deadSince = now;
                t.running = false;
                Log("rail: %s -- train destroyed; next one in %d h", r.name.c_str(),
                    g_nRailRespawnHours);
            }

            if (now - t.deadSince < g_nRailRespawnHours * 3600.0)
                skip = true;
            else
            {
                t.id = FalconNullId;
                t.deadSince = -1.0;
            }
        }

        Termini tm = {false, -1, 0.0F, 0.0F, nullptr};

        if (not skip)
        {
            tm = FindTermini(r, u ? u->GetTeam() : -1);

            if (not tm.ok)
            {
                if (not t.noTermini)
                    Log("rail: %s -- no friendly run of %d km or more for team %d; no train",
                        r.name.c_str(), (int)(MIN_RUN_FT / GRID_SIZE_FT), tm.team);

                t.noTermini = true;
                skip = true;
            }
            else
            {
                t.noTermini = false;
                t.team = tm.team;
            }
        }

        if (not skip and u)
        {
            // Keep it where it is and which way it is going; move the ends.
            float s;
            bool outbound;

            if (adoptS >= 0.0F)
            {
                s = adoptS;
                outbound = true;
            }
            else if (t.running)
            {
                float dir, speed;
                s = TrainS(t, now, &dir, &speed);
                outbound = (dir > 0.0F) == (t.sFront >= t.sRear);
            }
            else
            {
                s = tm.sRear;
                outbound = true;
            }

            bool moved = not t.running or fabsf(tm.sFront - t.sFront) > GRID_SIZE_FT or
                         fabsf(tm.sRear - t.sRear) > GRID_SIZE_FT;
            t.sRear = tm.sRear;
            t.sFront = tm.sFront;
            SetPhase(t, s, outbound, now);

            if (moved or startup)
                Log("rail: %s -- train %d runs %.0f km %s to %d km short of the front",
                    r.name.c_str(), u->GetCampID(), fabsf(tm.sFront - tm.sRear) / GRID_SIZE_FT,
                    tm.hub ? "from a supply hub" : "from the end of friendly track",
                    g_nRailFrontStandoff);
        }
        else if (not skip and wanted)
        {
            u = SpawnTrain(r, tm);

            if (u)
            {
                t.id = u->Id();
                t.deadSince = -1.0;
                t.sRear = tm.sRear;
                t.sFront = tm.sFront;
                t.t0 = now; // start of a cycle: loading at the hub
                t.running = true;
                Log("rail: %s -- spawned train %d for team %d: %.0f km %s to %d km short of the front",
                    r.name.c_str(), u->GetCampID(), tm.team,
                    fabsf(tm.sFront - tm.sRear) / GRID_SIZE_FT,
                    tm.hub ? "from a supply hub" : "from the end of friendly track",
                    g_nRailFrontStandoff);
            }
        }

        std::lock_guard<std::mutex> hold(g_lock);
        Train *rec = FindTrainByRoute(ri);

        if (rec)
            *rec = t;
    }
}

int RailMoveTrain(UnitClass *u)
{
    // Deaggregated: the lead car reports where it is (GNDAIClass::Order_Battalion).
    if (not u->IsAggregate())
        return 0;

    float x, y, yaw, speed;

    if (not RailTrainPose(u, 0, &x, &y, &yaw, &speed))
        return 0; // not running (yet): stay put, do not wander off with the planner

    ((BattalionClass *)u)->SimSetLocation(x, y, 0.0F);
    u->SetMoving(speed > 0.0F ? 1 : 0);
    return 0;
}

bool RailTrainPose(UnitClass *u, int car, float *x, float *y, float *yaw, float *speed)
{
    if (not u or not u->IsTrain())
        return false;

    std::lock_guard<std::mutex> hold(g_lock);
    Train *t = FindTrainById(u->Id());

    if (not t or not t->running or t->route < 0 or t->route >= (int)g_routes.size())
        return false;

    const Route &r = g_routes[t->route];
    float dir;
    float s = TrainS(*t, GameSeconds(), &dir, speed);

    // Cars stay in one order along the track (hub side of the lead), so the
    // consist never jumps at a terminus; it only turns round.
    float towardRear = t->sFront >= t->sRear ? -1.0F : 1.0F;
    PointAt(r, s + towardRear * car * CAR_SPACING_FT, x, y, yaw);

    if (dir < 0.0F)
        *yaw += 3.14159265F;

    return true;
}

int RailVisitRoutes(RailPointFn fn, void *ctx)
{
    std::lock_guard<std::mutex> hold(g_lock);

    if (not LoadRoutes())
        return 0;

    for (int ri = 0; ri < (int)g_routes.size(); ri++)
    {
        const Route &r = g_routes[ri];

        for (int i = 0; i < (int)r.x.size(); i++)
            fn(ctx, ri, i, r.x[i], r.y[i]);
    }

    return (int)g_routes.size();
}

int RailGetTrains(RailTrainInfo *out, int max)
{
    std::lock_guard<std::mutex> hold(g_lock);
    int n = 0;
    double now = GameSeconds();

    for (const Train &t : g_trains)
    {
        if (n >= max or not t.running or t.id == FalconNullId or t.route < 0 or
            t.route >= (int)g_routes.size())
            continue;

        float dir, speed, yaw;
        float s = TrainS(t, now, &dir, &speed);
        PointAt(g_routes[t.route], s, &out[n].simX, &out[n].simY, &yaw);
        out[n].team = t.team;
        out[n].moving = speed > 0.0F;
        n++;
    }

    return n;
}

float RailDistanceKm(float simX, float simY)
{
    std::lock_guard<std::mutex> hold(g_lock);

    if (not LoadRoutes())
        return -1.0F;

    float best = -1.0F;

    for (const Route &r : g_routes)
    {
        float off;
        Project(r, simX, simY, &off);

        if (best < 0.0F or off < best)
            best = off;
    }

    return best < 0.0F ? best : best / GRID_SIZE_FT;
}
