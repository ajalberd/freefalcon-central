// Artscout - 2026: railway routes and trains. See railnet.h.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <mutex>
#include <queue>
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
extern int g_nRailTrainLoad;
extern int g_nRailRailheadKm;
extern bool g_bRailBridgeCuts;
extern int g_nRailBridgeBindKm;
extern int g_nRailBridgeMinM;
extern char FalconTerrainDataDir[];

namespace
{
const float KPH_FPS = 0.911344F;           // 1 km/h in ft/s
const float CAR_SPACING_FT = 70.0F;        // one KrAz and a gap
const float SAMPLE_FT = 2.0F * GRID_SIZE_FT; // ownership sampled every 2 km
const float HUB_REACH_FT = 4.0F * GRID_SIZE_FT; // a hub this close counts as on the line
const float MIN_RUN_FT = 20.0F * GRID_SIZE_FT;  // less friendly track than this: no train
const float ENLIST_REACH_FT = 3.0F * GRID_SIZE_FT; // a TE Supply battalion this close to a line is its train
const float CUT_STANDOFF_FT = 1.0F * GRID_SIZE_FT; // a railhead at a dropped bridge stops this far short

// A rail bridge long enough to be a river crossing, and the bridge objective whose
// status is its status (Korea has no rail bridges of its own: the nearest road bridge
// stands in, so the ATM's strikes, bomb damage and repair all reach the line).
struct Span
{
    float s0, s1;  // along the route
    VU_ID obj;     // FalconNullId = nothing close enough
    float bindFt;  // how far that objective is from the middle of the span
    bool down;     // last seen at 0% or with a destroyed span
};

// A route in sim feet: x north, y east (the sim's own axes), s = distance
// along it from the first point. seg[i] is the flag of the segment from point i
// to i + 1 ('-', 'b' bridge, 't' tunnel); the last entry is '-'.
struct Route
{
    std::string name;
    std::vector<float> x, y, s;
    std::vector<char> seg;
    std::vector<Span> bridges;
    float len;
};

bool g_bridgesBound = false;

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
    double lastDelivery;  // game seconds of the last railhead arrival it delivered for
    bool stranded;        // a dropped bridge between it and every hub: no deliveries
    bool halted;          // no run long enough left (logged once); parked where it stood
    float adoptS;         // >= 0: just picked up at this point on the line, not yet running
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
int g_routeGen = 0; // bumped on every route load (troop network rebuild)

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
    g_bridgesBound = false;
    g_routeGen++;

    char path[_MAX_PATH];
    sprintf_s(path, "%s\\rail.txt", FalconTerrainDataDir);
    FILE *f = nullptr;

    if (fopen_s(&f, path, "r") or not f)
    {
        Log("rail: no %s -- run tools/campaign-editor/osm_rail.py", path);
        return false;
    }

    char line[512];

    // Version 2 adds a bridge/tunnel flag after each point; version 1 files still load, all plain.
    if (not fgets(line, sizeof line, f) or
        (strncmp(line, "ffrail 1", 8) and strncmp(line, "ffrail 2", 8)))
    {
        Log("rail: %s is not an ffrail 1 or 2 file", path);
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
            char flag = '-';

            if (sscanf_s(line, "%f %f %c", &kx, &ky, &flag, 1) < 2)
                break;

            if (flag not_eq 'b' and flag not_eq 't')
                flag = '-';

            // Campaign km, x east / y north -> sim feet, x north / y east.
            float sx = ky * GRID_SIZE_FT, sy = kx * GRID_SIZE_FT;

            if (not r.x.empty())
                r.len += hypotf(sx - r.x.back(), sy - r.y.back());

            r.x.push_back(sx);
            r.y.push_back(sy);
            r.s.push_back(r.len);
            r.seg.push_back(flag);
        }

        if (r.x.size() >= 2)
        {
            r.seg.back() = '-';

            // Bridge runs long enough to be river crossings; the rest are culverts and
            // overpasses, which nothing in the campaign stands for.
            const float minFt = (g_nRailBridgeMinM > 0 ? g_nRailBridgeMinM : 0) * 3.2808F;

            for (size_t i = 0; i + 1 < r.x.size();)
            {
                if (r.seg[i] not_eq 'b')
                {
                    i++;
                    continue;
                }

                size_t j = i;

                while (j + 1 < r.x.size() and r.seg[j] == 'b')
                    j++;

                if (r.s[j] - r.s[i] >= minFt)
                {
                    Span sp = {r.s[i], r.s[j], FalconNullId, 0.0F, false};
                    r.bridges.push_back(sp);
                }

                i = j;
            }

            g_routes.push_back(r);
        }
    }

    fclose(f);
    Log("rail: %d routes from %s", (int)g_routes.size(), path);

    for (const Route &r : g_routes)
    {
        float tunnel = 0.0F;

        for (size_t i = 0; i + 1 < r.x.size(); i++)
            if (r.seg[i] == 't')
                tunnel += r.s[i + 1] - r.s[i];

        Log("rail:   %-28s %6.1f km, %d bridges of %d m or more, %.1f km in tunnels",
            r.name.c_str(), r.len / GRID_SIZE_FT, (int)r.bridges.size(), g_nRailBridgeMinM,
            tunnel / GRID_SIZE_FT);
    }

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

// What the track is at distance s: the flag, and on a bridge or in a tunnel the
// whole run of that flag (points i0..i1) around it.
char TrackAt(const Route &r, float s, size_t *i0, size_t *i1)
{
    s = s < 0.0F ? 0.0F : (s > r.len ? r.len : s);
    size_t lo = 0, hi = r.s.size() - 1;

    while (hi - lo > 1)
    {
        size_t mid = (lo + hi) / 2;

        if (r.s[mid] <= s)
            lo = mid;
        else
            hi = mid;
    }

    const char k = r.seg[lo];
    size_t a = lo, b = lo + 1;

    while (a > 0 and r.seg[a - 1] == k)
        a--;

    while (b + 1 < r.x.size() and r.seg[b] == k)
        b++;

    if (i0)
        *i0 = a;

    if (i1)
        *i1 = b;

    return k;
}

// ---------------------------------------------------------------- bridges

// Bind every route's bridge spans to the nearest bridge objective within
// RailBridgeBindKm of the span's middle. Once per load, on the campaign thread;
// the objectives are scanned before the lock is taken.
void BindBridges()
{
    struct Near
    {
        VU_ID id;
        float x, y;
    };
    std::vector<Near> objs;
    VuListIterator it(AllObjList);

    for (Objective o = GetFirstObjective(&it); o; o = GetNextObjective(&it))
    {
        if (o->GetType() == TYPE_BRIDGE)
        {
            Near n = {o->Id(), o->XPos(), o->YPos()};
            objs.push_back(n);
        }
    }

    const float reach = (g_nRailBridgeBindKm > 0 ? g_nRailBridgeBindKm : 0) * GRID_SIZE_FT;
    std::lock_guard<std::mutex> hold(g_lock);

    if (g_bridgesBound)
        return;

    g_bridgesBound = true;

    for (Route &r : g_routes)
    {
        int bound = 0;

        for (Span &sp : r.bridges)
        {
            float mx, my, yaw, best = reach;
            PointAt(r, 0.5F * (sp.s0 + sp.s1), &mx, &my, &yaw);
            sp.obj = FalconNullId;

            for (const Near &n : objs)
            {
                const float d = hypotf(n.x - mx, n.y - my);

                if (d <= best)
                {
                    best = d;
                    sp.obj = n.id;
                    sp.bindFt = d;
                }
            }

            if (sp.obj not_eq FalconNullId)
                bound++;
        }

        Log("rail: %s -- %d of its %d bridges stand on a bridge objective within %d km (%d "
            "bridge objectives in the theater)",
            r.name.c_str(), bound, (int)r.bridges.size(), g_nRailBridgeBindKm, (int)objs.size());
    }
}

// A bridge objective is down for the railway at 0%, or as soon as any of its
// features is destroyed: one span in the river cuts the track even while the
// status average says the bridge is still mostly standing.
bool BridgeDown(Objective o)
{
    if (not o)
        return false;

    if (o->GetObjectiveStatus() == 0)
        return true;

    for (int f = 0; f < o->GetTotalFeatures(); f++)
        if (o->GetFeatureStatus(f) == VIS_DESTROYED)
            return true;

    return false;
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
    bool frontAtCut; // the forward terminus is a dropped bridge, not the front line
    bool stranded;   // cut off from any supply hub by a dropped bridge: runs, carries nothing
};

// Where a train on this route should shuttle, for `team` (-1 = whichever team
// holds most of the route). The route is sampled every 2 km and each sample
// takes the owner of the nearest objective; the longest friendly run is the
// train's world. Its end next to hostile ground is the front, pulled back by
// RailFrontStandoff; the rear is the friendly supply source (IsSupplySource:
// city, port, depot, army base, not on the front) furthest back along the run
// within RailRunKm.
//
// A dropped bridge (`down`, one flag per r.bridges entry) is a wall: the run
// stops short of it. With `sNow` >= 0 (where the train is) the run holding the
// train is chosen over a longer one beyond a broken bridge, so a train never
// jumps across the river; with no enemy past either end, the broken bridge is
// the forward terminus, CUT_STANDOFF short of it.
Termini FindTermini(const Route &r, int team, float sNow, const std::vector<char> &down)
{
    Termini out = {false, team, 0.0F, 0.0F, nullptr, false, false};
    int n = (int)(r.len / SAMPLE_FT) + 1;
    std::vector<int> owner(n);
    std::vector<char> cut(n, 0);
    int count[NUM_TEAMS] = {0};

    // Samples on or next to a dropped bridge, at least the one nearest its middle.
    for (size_t k = 0; k < r.bridges.size() and k < down.size(); k++)
    {
        if (not down[k])
            continue;

        const Span &sp = r.bridges[k];
        const int mid = (int)(0.5F * (sp.s0 + sp.s1) / SAMPLE_FT + 0.5F);

        for (int i = 0; i < n; i++)
            if (i * SAMPLE_FT >= sp.s0 - CUT_STANDOFF_FT and i * SAMPLE_FT <= sp.s1 + CUT_STANDOFF_FT)
                cut[i] = 1;

        if (mid >= 0 and mid < n)
            cut[mid] = 1;
    }

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

    // Runs of samples neither hostile to the team nor on a dropped bridge: the one
    // holding the train (or nearest it), else the longest.
    int bestA = -1, bestB = -1;
    float bestGap = 1e30F;
    const int nowI = sNow >= 0.0F ? (int)(sNow / SAMPLE_FT + 0.5F) : -1;

    for (int i = 0; i < n;)
    {
        if (IsHostile(team, owner[i]) or cut[i])
        {
            i++;
            continue;
        }

        int j = i;

        while (j + 1 < n and not IsHostile(team, owner[j + 1]) and not cut[j + 1])
            j++;

        bool take;

        if (nowI >= 0)
        {
            const float gap = (float)(nowI < i ? i - nowI : (nowI > j ? nowI - j : 0));
            take = bestA < 0 or gap < bestGap or (gap == bestGap and j - i > bestB - bestA);

            if (take)
                bestGap = gap;
        }
        else
            take = bestA < 0 or j - i > bestB - bestA;

        if (take)
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
    // if both or neither, the one nearer any hostile objective. A dropped
    // bridge past an end is not the enemy, but with the enemy past neither end
    // it is where the supplies go.
    bool cutA = bestA > 0 and cut[bestA - 1], cutB = bestB < n - 1 and cut[bestB + 1];
    bool hostA = bestA > 0 and not cutA, hostB = bestB < n - 1 and not cutB;
    bool frontIsB;

    if (hostA not_eq hostB)
        frontIsB = hostB;
    else if (not hostA and cutA not_eq cutB)
        frontIsB = cutB;
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

    // The cut samples already stop CUT_STANDOFF short of a dropped bridge.
    const bool frontAtCut = frontIsB ? cutB : cutA;
    float standoff =
        frontAtCut ? 0.0F : (g_nRailFrontStandoff > 0 ? g_nRailFrontStandoff : 0) * GRID_SIZE_FT;
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
    out.frontAtCut = frontAtCut;
    // Behind it a dropped bridge and no supply hub on this side: nothing to carry.
    out.stranded = not hub and (frontIsB ? cutA : cutB);
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

Unit SpawnTrain(const Route &r, const Termini &tm)
{
    float x, y, yaw;
    PointAt(r, tm.sRear, &x, &y, &yaw);
    GridIndex gx = GridOf(y), gy = GridOf(x);
    Objective o = tm.hub ? tm.hub : FindNearestObjective(gx, gy, NULL);

    if (not o or IsHostile(tm.team, o->GetTeam()))
        return nullptr;

    // The Train class (RAIL_TRAIN_SPTYPE): sixteen KrAz T-255B trucks, our boxcars until the
    // train has models of its own. A theater without it gets the Supply battalion it was
    // copied from.
    Unit u = NewUnit(DOMAIN_LAND, TYPE_BATTALION, STYPE_UNIT_SUPPLY, RAIL_TRAIN_SPTYPE, NULL);

    if (not u)
    {
        static bool warned = false;

        if (not warned)
            Log("rail: no Train class (Supply sptype %d) in this theater's FALCON4.ct -- "
                "trains are plain Supply battalions",
                RAIL_TRAIN_SPTYPE);

        warned = true;
        u = NewUnit(DOMAIN_LAND, TYPE_BATTALION, STYPE_UNIT_SUPPLY, 1, NULL);
    }

    if (not u)
    {
        Log("rail: no Supply battalion class in this theater either -- no train on %s",
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

// ---------------------------------------------------------------- supply

// A trainload for the railhead: when a train finishes an outbound run it hands supply and fuel
// to its own side's battalions near the forward terminus, nearest first, each up to what it is
// short of. It is drawn from the team's national pools -- the same money road supply spends in
// SupplyUnits (supply.cpp) -- so rail does not create supply, it delivers it: one hop at the
// intact-node loss (2%, as NodeSupplyLoss) instead of a road journey through every node and
// bridge on the way. A train killed on the way never arrives, and a damaged one carries what its
// surviving cars can (the load scales with vehicles left).
void MaybeDeliver(const Route &r, Train &t, Unit train, double now)
{
    if (g_nRailTrainLoad <= 0)
        return;

    // The timetable: arrivals at the front are t0 + dwell + run + k * period.
    const float v = SpeedFps();
    const double dwell = DwellSec();
    const double run = fabsf(t.sFront - t.sRear) / v;
    const double period = 2.0 * (run + dwell);

    if (period <= 0.0)
        return;

    const double first = t.t0 + dwell + run;

    if (now < first)
        return;

    const double arrived = first + floor((now - first) / period) * period;

    // Half a period of slack: re-basing the cycle when the ends move can nudge the last arrival
    // later, and one arrival must not pay out twice.
    if (arrived <= t.lastDelivery + period * 0.5)
        return;

    t.lastDelivery = arrived;

    const int team = train->GetTeam();
    const int full = train->GetFullstrengthVehicles();
    const int left = train->GetTotalVehicles();
    const float strength = full > 0 ? (float)left / (float)full : 1.0F;
    const int load = (int)(g_nRailTrainLoad * strength);
    int supply = load < (int)TeamInfo[team]->GetSupplyAvail() ? load : (int)TeamInfo[team]->GetSupplyAvail();
    int fuel = load < (int)TeamInfo[team]->GetFuelAvail() ? load : (int)TeamInfo[team]->GetFuelAvail();

    // Who is near the railhead and short of something, nearest first.
    float hx, hy, yaw;
    PointAt(r, t.sFront, &hx, &hy, &yaw);
    const float reach = (g_nRailRailheadKm > 0 ? g_nRailRailheadKm : 1) * GRID_SIZE_FT;

    struct Need
    {
        Unit u;
        float d;
        int s, f;
    };

    std::vector<Need> needs;
    VuListIterator it(AllUnitList);

    for (Unit u = GetFirstUnit(&it); u; u = GetNextUnit(&it))
    {
        if (u->GetTeam() not_eq team or not u->IsBattalion() or u->IsDead() or u->IsTrain())
            continue;

        const float d = hypotf(u->XPos() - hx, u->YPos() - hy);

        if (d > reach)
            continue;

        Need n = {u, d, u->GetUnitSupplyNeed(FALSE), u->GetUnitFuelNeed(FALSE)};

        if (n.s > 0 or n.f > 0)
            needs.push_back(n);
    }

    for (size_t i = 1; i < needs.size(); i++) // nearest first (a handful of units)
        for (size_t j = i; j > 0 and needs[j].d < needs[j - 1].d; j--)
            std::swap(needs[j], needs[j - 1]);

    // Take from the pools only what is handed over, less the one hop's 2% in transit.
    int gaveS = 0, gaveF = 0, units = 0;

    for (const Need &n : needs)
    {
        const int s = n.s < supply - gaveS ? n.s : supply - gaveS;
        const int f = n.f < fuel - gaveF ? n.f : fuel - gaveF;

        if (s <= 0 and f <= 0)
            break;

        n.u->SupplyUnit(s > 0 ? s * 98 / 100 : 0, f > 0 ? f * 98 / 100 : 0);
        gaveS += s > 0 ? s : 0;
        gaveF += f > 0 ? f : 0;
        units++;
    }

    TeamInfo[team]->SetSupplyAvail(TeamInfo[team]->GetSupplyAvail() - gaveS);
    TeamInfo[team]->SetFuelAvail(TeamInfo[team]->GetFuelAvail() - gaveF);

    Log("rail: %s -- train %d reached the railhead (%d of %d cars): %d supply, %d fuel to %d "
        "unit(s) within %d km; %d short of something there; pools now %d / %d",
        r.name.c_str(), train->GetCampID(), left, full, gaveS, gaveF, units, g_nRailRailheadKm,
        (int)needs.size(), (int)TeamInfo[team]->GetSupplyAvail(),
        (int)TeamInfo[team]->GetFuelAvail());
}

bool IsTrainClass(Unit u)
{
    return u->GetSType() == STYPE_UNIT_SUPPLY and u->GetSPType() == RAIL_TRAIN_SPTYPE;
}

// A unit of the Train class that is not running yet -- placed in the TE editor (Add Battalion,
// Equipment "Arty/Rocket", Unit Type "Train"), or written into a scenario -- becomes the nearest
// line's train. Identified by class, so an ordinary Supply battalion is never taken over.
void EnlistPlacedTrains()
{
    VuListIterator it(AllUnitList);

    for (Unit u = GetFirstUnit(&it); u; u = GetNextUnit(&it))
    {
        if (not u->IsBattalion() or u->IsTrain() or u->IsDead() or not IsTrainClass(u))
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
            Log("rail: Train %d placed %.1f km from the %s -- it runs on that line",
                u->GetCampID(), bestOff / GRID_SIZE_FT, best->name.c_str());
        }
        else
        {
            // Flag it anyway: a train with no line gets no position from here, so it stays
            // where it is instead of being driven down the roads by the ground planner.
            u->SetTrain(1);
            u->SetDontPlan(1);
            u->SetScripted(1);
            Log("rail: Train %d is more than %.0f km from every rail line -- it stays put",
                u->GetCampID(), ENLIST_REACH_FT / GRID_SIZE_FT);
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
void RailTroopTick(const std::vector<std::vector<char>> &downs);

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

    if (not g_bridgesBound)
        BindBridges();

    // Which bridges are down right now, per route; say so when that changes.
    std::vector<std::vector<char>> downs(g_routes.size());

    for (int ri = 0; ri < (int)g_routes.size(); ri++)
    {
        const Route &r = g_routes[ri];
        std::vector<char> &down = downs[ri];
        down.assign(r.bridges.size(), 0);

        for (size_t k = 0; k < r.bridges.size(); k++)
        {
            const Span &sp = r.bridges[k];

            if (sp.obj == FalconNullId)
                continue;

            Objective o = (Objective)vuDatabase->Find(sp.obj);
            down[k] = (g_bRailBridgeCuts and BridgeDown(o)) ? 1 : 0;

            if ((down[k] not_eq 0) not_eq sp.down)
            {
                _TCHAR name[80] = {0};

                if (o)
                    o->GetName(name, 79, FALSE);

                Log("rail: %s -- bridge at km %.1f (%s, objective %d, %.1f km off, %d%%) is %s",
                    r.name.c_str(), 0.5F * (sp.s0 + sp.s1) / GRID_SIZE_FT, name,
                    o ? o->GetCampID() : -1, sp.bindFt / GRID_SIZE_FT,
                    o ? o->GetObjectiveStatus() : -1,
                    down[k] ? "DOWN: the line is cut there" : "back up: the line is open");
                std::lock_guard<std::mutex> hold(g_lock);
                g_routes[ri].bridges[k].down = down[k] not_eq 0;
            }
        }
    }

    RailTroopTick(downs); // Artscout - 2026: troop trains (below)

    // Every live train unit gets a record of its own on its nearest line -- placed in a TE,
    // from a save, or spawned here -- so several trains can share a line, each at its own
    // point in the cycle. One out of reach of every line has none and holds still.
    {
        VuListIterator it(AllUnitList);

        for (Unit u = GetFirstUnit(&it); u; u = GetNextUnit(&it))
        {
            if (not u->IsBattalion() or not u->IsTrain() or u->IsDead())
                continue;

            {
                std::lock_guard<std::mutex> hold(g_lock);

                if (FindTrainById(u->Id()))
                    continue;
            }

            int best = -1;
            float bestOff = ENLIST_REACH_FT, bestS = 0.0F;

            for (int ri = 0; ri < (int)g_routes.size(); ri++)
            {
                float off;
                float s = Project(g_routes[ri], u->XPos(), u->YPos(), &off);

                if (off < bestOff)
                {
                    bestOff = off;
                    bestS = s;
                    best = ri;
                }
            }

            if (best < 0)
                continue;

            std::lock_guard<std::mutex> hold(g_lock);
            Train *slot = nullptr;

            // A line's automatic slot that has no train yet (a fresh start, or a load) takes it.
            for (Train &t : g_trains)
                if (t.route == best and t.id == FalconNullId)
                    slot = &t;

            if (not slot)
            {
                Train fresh = {best, FalconNullId, 0, 0.0F, 0.0F, 0.0, false, -1.0, false, 0.0,
                               false, false, -1.0F};
                g_trains.push_back(fresh);
                slot = &g_trains.back();
            }

            slot->id = u->Id();
            slot->running = false;
            slot->deadSince = -1.0;
            slot->adoptS = bestS;
            Log("rail: %s -- train %d is on the line at km %.1f (placed, or from a save)",
                g_routes[best].name.c_str(), u->GetCampID(), bestS / GRID_SIZE_FT);
        }
    }

    // A line in RailTrainLines with no record at all gets an empty one, for SpawnTrain.
    {
        std::lock_guard<std::mutex> hold(g_lock);

        for (int ri = 0; ri < (int)g_routes.size(); ri++)
        {
            if (RouteWanted(g_routes[ri]) and not FindTrainByRoute(ri))
            {
                Train fresh = {ri, FalconNullId, 0, 0.0F, 0.0F, 0.0, false, -1.0, false, 0.0,
                               false, false, -1.0F};
                g_trains.push_back(fresh);
            }
        }
    }

    // Records are added above and removed below, both on this thread only; the sim thread
    // only reads them under the lock. So indices hold for the loop.
    size_t count;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        count = g_trains.size();
    }
    std::vector<size_t> drop;

    for (size_t ti = 0; ti < count; ti++)
    {
        Train t;
        bool otherOnLine = false;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            t = g_trains[ti];

            for (size_t k = 0; k < g_trains.size(); k++)
                if (k not_eq ti and g_trains[k].route == t.route and g_trains[k].id not_eq FalconNullId)
                    otherOnLine = true;
        }

        const int ri = t.route;
        const Route &r = g_routes[ri];
        const std::vector<char> &down = downs[ri];

        // A route not in RailTrainLines still runs a train someone placed on it (TE), but
        // gets no automatic one and no replacement when it dies.
        const bool wanted = RouteWanted(r);

        Unit u = t.id == FalconNullId ? nullptr : (Unit)vuDatabase->Find(t.id);

        if (u and (u->IsDead() or not u->IsTrain()))
            u = nullptr;

        // Just picked up (placed, or from a save): where it stands on the line.
        float adoptS = t.adoptS;
        t.adoptS = -1.0F;

        bool skip = false;

        if (not u and t.id not_eq FalconNullId)
        {
            // It was running and now it is gone: destroyed. The line's automatic train is
            // replaced after RailRespawnHours; any other train is simply gone.
            if (not wanted or otherOnLine)
            {
                Log("rail: %s -- train destroyed", r.name.c_str());
                drop.push_back(ti);
                continue;
            }

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

        // An empty slot on a line that no longer wants a train (the config changed).
        if (not u and t.id == FalconNullId and not wanted)
        {
            drop.push_back(ti);
            continue;
        }

        Termini tm = {false, -1, 0.0F, 0.0F, nullptr, false, false};

        // Where the train is now, so the termini keep it on its own side of a dropped bridge.
        float sNow = -1.0F;

        if (u and adoptS >= 0.0F)
            sNow = adoptS;
        else if (u and t.running)
        {
            float dir, speed;
            sNow = TrainS(t, now, &dir, &speed);
        }

        if (not skip)
        {
            tm = FindTermini(r, u ? u->GetTeam() : -1, sNow, down);

            if (not tm.ok)
            {
                if (not t.noTermini)
                    Log("rail: %s -- no friendly run of %d km or more for team %d; no train",
                        r.name.c_str(), (int)(MIN_RUN_FT / GRID_SIZE_FT), tm.team);

                t.noTermini = true;
                skip = true;

                // A running train whose stretch has become too short (a bridge dropped
                // either side of it) stops where it is instead of running on across the gap.
                if (u and t.running and sNow >= 0.0F and not t.halted)
                {
                    t.sRear = t.sFront = sNow;
                    SetPhase(t, sNow, true, now);
                    t.halted = true;
                    t.lastDelivery = now;
                    Log("rail: %s -- train %d halted at km %.1f", r.name.c_str(), u->GetCampID(),
                        sNow / GRID_SIZE_FT);
                }
            }
            else
            {
                t.noTermini = false;
                t.halted = false;
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

            // Did it reach the railhead since the last look? Checked on the timetable it has been
            // running, before the ends move and the cycle is re-based. A stranded train (a
            // dropped bridge between it and every hub) carries nothing.
            if (t.running and not t.stranded)
                MaybeDeliver(r, t, u, now);
            else
                t.lastDelivery = now; // starts running now: nothing owed for arrivals before this

            bool moved = not t.running or fabsf(tm.sFront - t.sFront) > GRID_SIZE_FT or
                         fabsf(tm.sRear - t.sRear) > GRID_SIZE_FT or tm.stranded not_eq t.stranded;
            t.sRear = tm.sRear;
            t.sFront = tm.sFront;
            t.stranded = tm.stranded;
            SetPhase(t, s, outbound, now);

            if (moved or startup)
            {
                char where[64];

                if (tm.frontAtCut)
                    sprintf_s(where, "to %d km short of a dropped bridge",
                              (int)(CUT_STANDOFF_FT / GRID_SIZE_FT));
                else
                    sprintf_s(where, "to %d km short of the front", g_nRailFrontStandoff);

                Log("rail: %s -- train %d runs %.0f km %s %s%s", r.name.c_str(), u->GetCampID(),
                    fabsf(tm.sFront - tm.sRear) / GRID_SIZE_FT,
                    tm.hub ? "from a supply hub" : "from the end of friendly track", where,
                    tm.stranded ? " -- STRANDED behind a dropped bridge, carrying nothing" : "");
            }
        }
        else if (not skip and not u and wanted)
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
                t.lastDelivery = now;
                t.stranded = tm.stranded;
                t.halted = false;
                Log("rail: %s -- spawned train %d for team %d: %.0f km %s to %d km short of %s",
                    r.name.c_str(), u->GetCampID(), tm.team,
                    fabsf(tm.sFront - tm.sRear) / GRID_SIZE_FT,
                    tm.hub ? "from a supply hub" : "from the end of friendly track",
                    tm.frontAtCut ? (int)(CUT_STANDOFF_FT / GRID_SIZE_FT) : g_nRailFrontStandoff,
                    tm.frontAtCut ? "a dropped bridge" : "the front");
            }
        }

        std::lock_guard<std::mutex> hold(g_lock);
        g_trains[ti] = t;
    }

    if (not drop.empty())
    {
        std::lock_guard<std::mutex> hold(g_lock);

        for (size_t k = drop.size(); k-- > 0;)
            g_trains.erase(g_trains.begin() + drop[k]);
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

bool RailTrainPose(UnitClass *u, int car, float *x, float *y, float *yaw, float *speed,
                   RailTrackAt *at)
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
    const float sCar = s + towardRear * car * CAR_SPACING_FT;
    PointAt(r, sCar, x, y, yaw);

    if (dir < 0.0F)
        *yaw += 3.14159265F;

    if (at)
    {
        size_t i0, i1;
        at->kind = TrackAt(r, sCar, &i0, &i1);
        at->ax = r.x[i0];
        at->ay = r.y[i0];
        at->bx = r.x[i1];
        at->by = r.y[i1];
        const float runLen = r.s[i1] - r.s[i0];
        at->t = runLen > 0.0F ? (sCar - r.s[i0]) / runLen : 0.0F;
        at->t = at->t < 0.0F ? 0.0F : (at->t > 1.0F ? 1.0F : at->t);
    }

    return true;
}

int RailVisitTrack(RailTrackPointFn fn, void *ctx)
{
    std::lock_guard<std::mutex> hold(g_lock);

    if (not LoadRoutes())
        return 0;

    for (int ri = 0; ri < (int)g_routes.size(); ri++)
    {
        const Route &r = g_routes[ri];

        for (int i = 0; i < (int)r.x.size(); i++)
            fn(ctx, ri, i, r.x[i], r.y[i], r.seg[i]);
    }

    return (int)g_routes.size();
}

int RailGetBridges(RailBridgeInfo *out, int max)
{
    std::lock_guard<std::mutex> hold(g_lock);
    int n = 0;

    for (const Route &r : g_routes)
    {
        for (const Span &sp : r.bridges)
        {
            if (n >= max)
                return n;

            if (sp.obj == FalconNullId)
                continue;

            float yaw;
            PointAt(r, 0.5F * (sp.s0 + sp.s1), &out[n].simX, &out[n].simY, &yaw);
            out[n].down = sp.down ? 1 : 0;
            n++;
        }
    }

    return n;
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

// ---------------------------------------------------------------- troop trains
//
// A battalion with a new destination, about to plan its road march (BattalionClass::MoveUnit,
// just before BuildGroundWP), asks RailTryBoard whether the railway is faster. The network is
// every route sampled every 2 km (the same samples FindTermini judges ownership on), joined where
// one route's end lies within JUNCTION_FT of another route. Each campaign stage the samples are
// refreshed: owner (nearest objective), cut (on or next to a dropped bridge) and safe (friendly
// with no hostile sample within RailTroopStandoffKm along the line -- where troops may detrain:
// at 10 km most riders ran into enemy ground units at the stop, campsim 2026-10-05).
//
// The rule, all in hours, for a trip from P to D:
//   road = 1.3 x |PD| / RailTroopRoadKph
//   rail = walk to the line + RailTroopLoadMin + ride / RailTrainSpeed
//          + RailTroopTransferMin per change of line + RailTroopLoadMin + walk to D
//   walks are 1.3 x straight line / RailTroopRoadKph, each at most RailTroopWalkKm;
//   the ride crosses only samples not hostile to the battalion and not cut.
// It rides if |PD| >= RailTroopMinKm, rail <= road x (100 - RailTroopSavePct)%, and fewer than
// RailTroopTrains of its side are riding. Otherwise it marches as before.
//
// A rider's position is a pure function of game time along its planned journey, like a train's.
// It leaves the ground planner (and the GTM) until it detrains; then it marches the last leg to
// its destination with the orders it had. If the line ahead turns hostile or a bridge on it
// drops, it stops where it is and detrains there; if it is engaged, it gets off at once.
// Riders are not saved: a battalion riding when the game is saved marches on from where it was.

extern bool g_bRailTroops;
extern int g_nRailTroopMinKm;
extern int g_nRailTroopRoadKph;
extern int g_nRailTroopWalkKm;
extern int g_nRailTroopLoadMin;
extern int g_nRailTroopTransferMin;
extern int g_nRailTroopSavePct;
extern int g_nRailTroopTrains;
extern int g_nRailTroopStandoffKm;
extern int g_nRailTroopContactKm;

namespace
{
const float JUNCTION_FT = 5.0F * GRID_SIZE_FT; // a route end this close to another route joins it
const float ROAD_DETOUR = 1.3F;                 // road length over straight line
const double REBOARD_SEC = 2.0 * 3600.0;        // after detraining, march for at least this long

struct Sample
{
    float x, y;
    short owner;
    char cut, safe;
};

struct Junction
{
    int a, b;    // global sample indices
    float hopFt; // straight line between them
};

std::vector<std::vector<Sample>> g_samples; // per route
std::vector<int> g_sampleBase;              // global index of each route's sample 0
std::vector<Junction> g_junctions;
int g_sampleCount = 0;
int g_netBuilt = -1; // g_routeGen the net was built for

struct RideStep
{
    int node;  // global sample index
    double t;  // game seconds when the rider is there
};

struct Rider
{
    VU_ID id;
    int team;
    float px, py;            // where it started walking
    double tBoard, tDepart;  // reaches the line; train leaves
    double tArrive, tRelease;
    std::vector<RideStep> steps;
    bool stopped;            // cut short: detrains at (sx, sy)
    float sx, sy;
    float destX, destY;
};

std::vector<Rider> g_riders;
std::vector<std::pair<VU_ID, double>> g_reboard; // detrained at; no new ride before + REBOARD_SEC

struct TroopStats
{
    int asked, rode, tooShort, noLine, noReach, notFaster, full, stopped, arrived, interrupted;
} g_tstats = {0};
double g_tstatsLogged = -1.0;

int SampleRoute(int node)
{
    int r = 0;

    while (r + 1 < (int)g_sampleBase.size() and g_sampleBase[r + 1] <= node)
        r++;

    return r;
}

const Sample &NodeSample(int node)
{
    const int r = SampleRoute(node);
    return g_samples[r][node - g_sampleBase[r]];
}

// Samples and junctions, once per route load (under g_lock).
void BuildNet()
{
    if (g_netBuilt == g_routeGen)
        return;

    g_netBuilt = g_routeGen;
    g_riders.clear();
    g_samples.assign(g_routes.size(), {});
    g_sampleBase.assign(g_routes.size(), 0);
    g_junctions.clear();
    g_sampleCount = 0;

    for (size_t ri = 0; ri < g_routes.size(); ri++)
    {
        const Route &r = g_routes[ri];
        const int n = (int)(r.len / SAMPLE_FT) + 1;
        g_sampleBase[ri] = g_sampleCount;
        g_samples[ri].resize(n);

        for (int i = 0; i < n; i++)
        {
            float yaw;
            Sample &sm = g_samples[ri][i];
            PointAt(r, i * SAMPLE_FT, &sm.x, &sm.y, &yaw);
            sm.owner = 0;
            sm.cut = 0;
            sm.safe = 0;
        }

        g_sampleCount += n;
    }

    for (size_t ra = 0; ra < g_routes.size(); ra++)
    {
        const int na = (int)g_samples[ra].size();

        for (int end = 0; end < 2; end++)
        {
            const int ia = end ? na - 1 : 0;
            const Sample &ea = g_samples[ra][ia];

            for (size_t rb = 0; rb < g_routes.size(); rb++)
            {
                if (rb == ra)
                    continue;

                float off;
                const float s = Project(g_routes[rb], ea.x, ea.y, &off);

                if (off > JUNCTION_FT)
                    continue;

                int ib = (int)(s / SAMPLE_FT + 0.5F);
                ib = ib < 0 ? 0 : (ib >= (int)g_samples[rb].size() ? (int)g_samples[rb].size() - 1 : ib);
                const Sample &eb = g_samples[rb][ib];
                Junction j = {g_sampleBase[ra] + ia, g_sampleBase[rb] + ib, hypotf(eb.x - ea.x, eb.y - ea.y)};
                bool dup = false;

                for (const Junction &k : g_junctions)
                    dup = dup or (k.a == j.b and k.b == j.a) or (k.a == j.a and k.b == j.b);

                if (not dup)
                {
                    g_junctions.push_back(j);
                    Log("rail: troops -- junction %s km %.0f <-> %s km %.0f (%.1f km apart)",
                        g_routes[ra].name.c_str(), ia * SAMPLE_FT / GRID_SIZE_FT,
                        g_routes[rb].name.c_str(), ib * SAMPLE_FT / GRID_SIZE_FT,
                        j.hopFt / GRID_SIZE_FT);
                }
            }
        }
    }
}

// Owner / cut / safe for every sample; campaign thread, each stage.
void TroopRefresh(const std::vector<std::vector<char>> &downs)
{
    {
        std::lock_guard<std::mutex> hold(g_lock);
        BuildNet();
    }

    std::vector<std::vector<Sample>> fresh;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        fresh = g_samples;
    }

    const int standoff = (int)((g_nRailTroopStandoffKm > 0 ? g_nRailTroopStandoffKm : 0) * GRID_SIZE_FT / SAMPLE_FT + 0.5F);

    for (size_t ri = 0; ri < fresh.size(); ri++)
    {
        std::vector<Sample> &v = fresh[ri];
        const Route &r = g_routes[ri];
        const int n = (int)v.size();

        for (int i = 0; i < n; i++)
        {
            Objective o = FindNearestObjective(GridOf(v[i].y), GridOf(v[i].x), NULL);
            v[i].owner = (short)(o ? o->GetTeam() : 0);
            v[i].cut = 0;
        }

        for (size_t k = 0; k < r.bridges.size() and ri < downs.size() and k < downs[ri].size(); k++)
        {
            if (not downs[ri][k])
                continue;

            const Span &sp = r.bridges[k];
            const int mid = (int)(0.5F * (sp.s0 + sp.s1) / SAMPLE_FT + 0.5F);

            for (int i = 0; i < n; i++)
                if (i * SAMPLE_FT >= sp.s0 - CUT_STANDOFF_FT and i * SAMPLE_FT <= sp.s1 + CUT_STANDOFF_FT)
                    v[i].cut = 1;

            if (mid >= 0 and mid < n)
                v[mid].cut = 1;
        }

        // safe = no sample of another, hostile-to-the-owner team within the standoff
        for (int i = 0; i < n; i++)
        {
            bool safe = v[i].owner > 0 and not v[i].cut;

            for (int j = i - standoff; safe and j <= i + standoff; j++)
                if (j >= 0 and j < n and v[j].owner > 0 and IsHostile(v[i].owner, v[j].owner))
                    safe = false;

            v[i].safe = safe ? 1 : 0;
        }
    }

    std::lock_guard<std::mutex> hold(g_lock);
    g_samples.swap(fresh);
}

// Can a battalion of `team` be at this sample (ride through it)?
bool Passable(const Sample &sm, int team)
{
    return not sm.cut and not(sm.owner > 0 and IsHostile(team, sm.owner));
}

// Detrain here? The sample must be safe for the owner, and the owner a friend of the team.
bool Alightable(const Sample &sm, int team)
{
    return sm.safe and sm.owner > 0 and not IsHostile(team, sm.owner) and Passable(sm, team);
}

float RoadFps()
{
    return (g_nRailTroopRoadKph > 1 ? g_nRailTroopRoadKph : 1) * KPH_FPS;
}

double WalkSec(float ft)
{
    return ROAD_DETOUR * ft / RoadFps();
}

double LoadSec()
{
    return (g_nRailTroopLoadMin > 0 ? g_nRailTroopLoadMin : 0) * 60.0;
}

double TransferSec()
{
    return (g_nRailTroopTransferMin > 0 ? g_nRailTroopTransferMin : 0) * 60.0;
}

Rider *FindRider(VU_ID id)
{
    for (Rider &r : g_riders)
        if (r.id == id)
            return &r;

    return nullptr;
}

// Position of a rider at game time t (sim feet); *moving = on the move.
void RiderPos(const Rider &r, double t, float *x, float *y, int *moving)
{
    *moving = 0;

    if (r.stopped)
    {
        *x = r.sx;
        *y = r.sy;
        return;
    }

    const Sample &b = NodeSample(r.steps.front().node);

    if (t < r.tBoard)
    {
        // walking from (px, py) to the line, started at tBoard - WalkSec
        const double walk = WalkSec(hypotf(b.x - r.px, b.y - r.py));
        const double f = walk > 0.0 ? 1.0 - (r.tBoard - t) / walk : 1.0;
        const float k = (float)(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
        *x = r.px + (b.x - r.px) * k;
        *y = r.py + (b.y - r.py) * k;
        *moving = 1;
        return;
    }

    if (t < r.tDepart)
    {
        *x = b.x;
        *y = b.y;
        return;
    }

    for (size_t i = 0; i + 1 < r.steps.size(); i++)
    {
        const RideStep &s0 = r.steps[i], &s1 = r.steps[i + 1];

        if (t >= s1.t)
            continue;

        const int r0 = SampleRoute(s0.node), r1 = SampleRoute(s1.node);
        const double f = s1.t > s0.t ? (t - s0.t) / (s1.t - s0.t) : 1.0;
        const float k = (float)(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));

        if (r0 == r1)
        {
            // along the track itself, not the chord between samples
            const float sA = (s0.node - g_sampleBase[r0]) * SAMPLE_FT;
            const float sB = (s1.node - g_sampleBase[r0]) * SAMPLE_FT;
            float yaw;
            PointAt(g_routes[r0], sA + (sB - sA) * k, x, y, &yaw);
        }
        else
        {
            // a change of lines: wait at the junction, then the short hop
            const Sample &a = NodeSample(s0.node), &c = NodeSample(s1.node);
            const double hold = TransferSec();
            const double ft = (t - s0.t) < hold ? 0.0 : ((t - s0.t - hold) / ((s1.t - s0.t - hold) > 0 ? (s1.t - s0.t - hold) : 1.0));
            const float kk = (float)(ft < 0.0 ? 0.0 : (ft > 1.0 ? 1.0 : ft));
            *x = a.x + (c.x - a.x) * kk;
            *y = a.y + (c.y - a.y) * kk;
        }

        *moving = 1;
        return;
    }

    const Sample &e = NodeSample(r.steps.back().node);
    *x = e.x;
    *y = e.y;
}

void LogTroopStats(double now, bool force)
{
    if (not force and g_tstatsLogged >= 0.0 and now - g_tstatsLogged < 6.0 * 3600.0)
        return;

    g_tstatsLogged = now;
    int riding[NUM_TEAMS] = {0};

    for (const Rider &r : g_riders)
        if (r.team >= 0 and r.team < NUM_TEAMS)
            riding[r.team]++;

    Log("rail: troops -- so far %d trips weighed: %d by rail, %d marched (%d short, %d no line within reach, "
        "%d line does not reach the destination, %d road faster, %d trains all in use); %d arrived, %d "
        "stopped short, %d got off in ground contact; riding now by team: %d %d %d %d %d %d %d %d",
        g_tstats.asked, g_tstats.rode, g_tstats.asked - g_tstats.rode, g_tstats.tooShort, g_tstats.noLine,
        g_tstats.noReach,
        g_tstats.notFaster, g_tstats.full, g_tstats.arrived, g_tstats.stopped, g_tstats.interrupted,
        riding[0], riding[1], riding[2], riding[3], riding[4], riding[5], riding[6], riding[7]);
}
} // namespace

// Called from RailCampaignTick each stage, after the bridge pass.
void RailTroopTick(const std::vector<std::vector<char>> &downs)
{
    if (not g_bRailTroops)
        return;

    TroopRefresh(downs);
    const double now = GameSeconds();
    std::lock_guard<std::mutex> hold(g_lock);

    // dead riders off the books
    for (size_t i = g_riders.size(); i-- > 0;)
    {
        Unit u = (Unit)vuDatabase->Find(g_riders[i].id);

        if (not u or u->IsDead())
        {
            Log("rail: troops -- battalion %d destroyed while riding (team %d)", u ? u->GetCampID() : -1,
                g_riders[i].team);
            g_riders.erase(g_riders.begin() + i);
        }
    }

    for (size_t i = g_reboard.size(); i-- > 0;)
        if (now - g_reboard[i].second > REBOARD_SEC)
            g_reboard.erase(g_reboard.begin() + i);

    LogTroopStats(now, false);
}

bool RailIsRiding(UnitClass *u)
{
    if (not u or g_riders.empty())
        return false;

    std::lock_guard<std::mutex> hold(g_lock);
    return FindRider(u->Id()) not_eq nullptr;
}

bool RailTryBoard(UnitClass *u, GridIndex dx, GridIndex dy)
{
    if (not g_bRailTrains or not g_bRailTroops or not u or not u->IsBattalion() or u->IsTrain() or
        u->GetDomain() not_eq DOMAIN_LAND or not u->IsAggregate() or u->Engaged() or u->Retreating() or
        u->Cargo() or not FalconLocalGame or not FalconLocalGame->IsLocal())
        return false;

    const double now = GameSeconds();
    std::lock_guard<std::mutex> hold(g_lock);

    if (g_netBuilt not_eq g_routeGen or g_samples.empty())
        return false;

    for (const auto &rb : g_reboard)
        if (rb.first == u->Id())
            return false;

    if (FindRider(u->Id()))
        return false;

    const int team = u->GetTeam();

    // A side at peace with everyone does not mobilise the railway. Korea Escalation stages China and
    // Russia inside North Korea while they are still neutral, and their own planner sends some of them
    // home: by rail they reached Shenyang or Khasan before their country joined (campsim, 6 runs: 43
    // Chinese battalions inside China at h24 with trains, 12 without).
    {
        bool atWar = false;

        for (int t = 1; t < NUM_TEAMS and not atWar; t++)
            atWar = t not_eq team and TeamInfo[t] and IsHostile(team, t);

        if (not atWar)
            return false;
    }

    const float px = u->XPos(), py = u->YPos();
    const float qx = GridToSim(dy), qy = GridToSim(dx); // sim x north = grid y
    const float trip = hypotf(qx - px, qy - py);
    g_tstats.asked++;

    if (trip < (g_nRailTroopMinKm > 0 ? g_nRailTroopMinKm : 0) * GRID_SIZE_FT)
    {
        g_tstats.tooShort++;
        return false;
    }

    const float walkMax = (g_nRailTroopWalkKm > 0 ? g_nRailTroopWalkKm : 0) * GRID_SIZE_FT;
    const double v = SpeedFps();
    const int N = g_sampleCount;
    std::vector<double> best(N, 1e30);
    std::vector<int> prev(N, -1);
    std::vector<char> done(N, 0);
    int seeds = 0;

    // Boarding: the nearest passable sample of each route within the walk.
    for (size_t ri = 0; ri < g_routes.size(); ri++)
    {
        float off;
        const float s = Project(g_routes[ri], px, py, &off);

        if (off > walkMax)
            continue;

        int i = (int)(s / SAMPLE_FT + 0.5F);
        i = i < 0 ? 0 : (i >= (int)g_samples[ri].size() ? (int)g_samples[ri].size() - 1 : i);
        const Sample &sm = g_samples[ri][i];

        if (not Passable(sm, team))
            continue;

        const int node = g_sampleBase[ri] + i;
        const double t = WalkSec(hypotf(sm.x - px, sm.y - py)) + LoadSec();

        if (t < best[node])
            best[node] = t, prev[node] = -1, seeds++;
    }

    if (not seeds)
    {
        g_tstats.noLine++;
        return false;
    }

    // Dijkstra over the samples (a few thousand nodes; once per new march order).
    typedef std::pair<double, int> QItem;
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> open;

    for (int i = 0; i < N; i++)
        if (best[i] < 1e29)
            open.push(QItem(best[i], i));

    while (not open.empty())
    {
        const double bt = open.top().first;
        const int at = open.top().second;
        open.pop();

        if (done[at] or bt > best[at])
            continue;

        done[at] = 1;
        const int r = SampleRoute(at);
        const int i = at - g_sampleBase[r];
        const int n = (int)g_samples[r].size();

        for (int d = -1; d <= 1; d += 2)
        {
            const int j = i + d;

            if (j < 0 or j >= n or not Passable(g_samples[r][j], team))
                continue;

            const double t = bt + SAMPLE_FT / v;

            if (t < best[at + d])
                best[at + d] = t, prev[at + d] = at, open.push(QItem(t, at + d));
        }

        for (const Junction &jn : g_junctions)
        {
            const int other = jn.a == at ? jn.b : (jn.b == at ? jn.a : -1);

            if (other < 0 or not Passable(NodeSample(other), team))
                continue;

            const double t = bt + TransferSec() + jn.hopFt / v;

            if (t < best[other])
                best[other] = t, prev[other] = at, open.push(QItem(t, other));
        }
    }

    // Detraining: the safe, friendly sample within the walk of D with the best total.
    int alight = -1;
    double railSec = 1e30;

    for (int node = 0; node < N; node++)
    {
        if (best[node] >= 1e29)
            continue;

        const Sample &sm = NodeSample(node);
        const float off = hypotf(sm.x - qx, sm.y - qy);

        if (off > walkMax or not Alightable(sm, team))
            continue;

        const double t = best[node] + LoadSec() + WalkSec(off);

        if (t < railSec)
            railSec = t, alight = node;
    }

    const double roadSec = WalkSec(trip);

    if (alight < 0)
    {
        g_tstats.noReach++;
        return false;
    }

    std::vector<int> path;

    for (int k = alight; k >= 0; k = prev[k])
        path.push_back(k);

    std::reverse(path.begin(), path.end());

    const float rideFt = (float)((best[alight] - best[path.front()]) * v);
    const int save = g_nRailTroopSavePct < 0 ? 0 : (g_nRailTroopSavePct > 95 ? 95 : g_nRailTroopSavePct);

    if (railSec > roadSec * (100 - save) / 100.0 or rideFt < 0.5F * g_nRailTroopMinKm * GRID_SIZE_FT)
    {
        g_tstats.notFaster++;
        return false;
    }

    int riding = 0;

    for (const Rider &r : g_riders)
        riding += r.team == team ? 1 : 0;

    if (riding >= g_nRailTroopTrains)
    {
        g_tstats.full++;
        return false;
    }

    Rider rd;
    rd.id = u->Id();
    rd.team = team;
    rd.px = px;
    rd.py = py;
    const Sample &b = NodeSample(path.front());
    rd.tBoard = now + WalkSec(hypotf(b.x - px, b.y - py));
    rd.tDepart = rd.tBoard + LoadSec();
    const double base = rd.tDepart - best[path.front()];

    for (int node : path)
    {
        RideStep st = {node, base + best[node]};
        rd.steps.push_back(st);
    }

    rd.tArrive = rd.steps.back().t;
    rd.tRelease = rd.tArrive + LoadSec();
    rd.stopped = false;
    rd.sx = rd.sy = 0.0F;
    rd.destX = qx;
    rd.destY = qy;
    g_riders.push_back(rd);
    g_tstats.rode++;

    int changes = 0;

    for (size_t k = 0; k + 1 < path.size(); k++)
        changes += SampleRoute(path[k]) not_eq SampleRoute(path[k + 1]) ? 1 : 0;

    const int r0 = SampleRoute(path.front()), r1 = SampleRoute(alight);
    Log("rail: troops -- battalion %d (team %d, country %d) rides %.0f km, %s km %.0f to %s km %.0f, %d change(s): "
        "%.1f h by rail vs %.1f h on the road (%.0f km away); %d of %d trains in use",
        u->GetCampID(), team, (int)u->GetCountry(), rideFt / GRID_SIZE_FT, g_routes[r0].name.c_str(),
        (path.front() - g_sampleBase[r0]) * SAMPLE_FT / GRID_SIZE_FT, g_routes[r1].name.c_str(),
        (alight - g_sampleBase[r1]) * SAMPLE_FT / GRID_SIZE_FT, changes, (railSec) / 3600.0,
        roadSec / 3600.0, trip / GRID_SIZE_FT, riding + 1, g_nRailTroopTrains);

    u->DisposeWayPoints();
    u->ClearUnitPath();
    return true;
}

bool RailMoveRider(UnitClass *u, int *ret)
{
    *ret = 0;

    if (not u or g_riders.empty())
        return false;

    const double now = GameSeconds();
    std::lock_guard<std::mutex> hold(g_lock);
    Rider *rd = FindRider(u->Id());

    if (not rd)
        return false;

    if (u->IsDead())
    {
        g_riders.erase(g_riders.begin() + (rd - &g_riders[0]));
        return false;
    }

    // Deaggregated (a player nearby): the sim has the vehicles; hold the timetable's place.
    if (not u->IsAggregate())
        return true;

    float x, y;
    int moving;
    RiderPos(*rd, now, &x, &y, &moving);

    // In contact with enemy ground forces on the way: off the train at once, and fight from here.
    // Air attack does not stop it -- the strike's losses fall on the battalion as on any column,
    // and the train runs on (Engaged() is set by any shot, so the target is what tells them apart).
    // Only a foe within RailTroopContactKm counts: a battalion targets ground units at detection
    // range, and in campsim the median "contact" was 40 km away.
    CampEntity foe = u->Engaged() ? u->GetCampTarget() : nullptr;
    const float contactFt = (g_nRailTroopContactKm > 0 ? g_nRailTroopContactKm : 0) * GRID_SIZE_FT;

    if (foe and not foe->IsFlight() and not rd->stopped and
        hypotf(foe->XPos() - x, foe->YPos() - y) <= contactFt)
    {
        u->SimSetLocation(x, y, 0.0F);
        const char *phase = now < rd->tBoard ? "walking to the line"
                            : now < rd->tDepart ? "entraining"
                            : now < rd->tArrive ? "riding" : "detraining";
        const Sample &stop = NodeSample(rd->steps.back().node);
        Log("rail: troops -- battalion %d in contact with enemy ground unit %d while %s: got off at "
            "(%.0f, %.0f), %.0f km from where it set out, %.0f km short of its stop, foe %.0f km away",
            u->GetCampID(), foe->GetCampID(), phase, y / GRID_SIZE_FT, x / GRID_SIZE_FT,
            hypotf(x - rd->px, y - rd->py) / GRID_SIZE_FT, hypotf(stop.x - x, stop.y - y) / GRID_SIZE_FT,
            hypotf(foe->XPos() - x, foe->YPos() - y) / GRID_SIZE_FT);
        g_tstats.interrupted++;
        g_reboard.push_back(std::make_pair(u->Id(), now));
        g_riders.erase(g_riders.begin() + (rd - &g_riders[0]));
        u->DisposeWayPoints();
        u->ClearUnitPath();
        return false;
    }

    // The line ahead turned hostile, a bridge on it dropped, or the stop is no longer safe:
    // stop here and detrain.
    if (not rd->stopped and now >= rd->tDepart and now < rd->tArrive)
    {
        bool blocked = not Alightable(NodeSample(rd->steps.back().node), rd->team);

        for (size_t i = 0; not blocked and i < rd->steps.size(); i++)
            if (rd->steps[i].t >= now and not Passable(NodeSample(rd->steps[i].node), rd->team))
                blocked = true;

        if (blocked)
        {
            rd->stopped = true;
            rd->sx = x;
            rd->sy = y;
            rd->tArrive = now;
            rd->tRelease = now + LoadSec();
            g_tstats.stopped++;
            Log("rail: troops -- battalion %d: the line ahead is cut or hostile; detrains at (%.0f, %.0f), "
                "%.0f km short of its stop",
                u->GetCampID(), y / GRID_SIZE_FT, x / GRID_SIZE_FT,
                hypotf(NodeSample(rd->steps.back().node).x - x, NodeSample(rd->steps.back().node).y - y) /
                    GRID_SIZE_FT);
        }
    }

    u->SimSetLocation(x, y, 0.0F);
    u->SetMoving(moving);

    if (now >= rd->tRelease)
    {
        if (not rd->stopped)
            g_tstats.arrived++;

        Log("rail: troops -- battalion %d detrained at (%.0f, %.0f), %.0f km from its destination",
            u->GetCampID(), y / GRID_SIZE_FT, x / GRID_SIZE_FT,
            hypotf(rd->destX - x, rd->destY - y) / GRID_SIZE_FT);
        g_reboard.push_back(std::make_pair(u->Id(), now));
        g_riders.erase(g_riders.begin() + (rd - &g_riders[0]));
        u->DisposeWayPoints();
        u->ClearUnitPath();
    }

    return true;
}

int RailGetRiders(RailTrainInfo *out, int max)
{
    std::lock_guard<std::mutex> hold(g_lock);
    const double now = GameSeconds();
    int n = 0;

    for (const Rider &r : g_riders)
    {
        if (n >= max)
            break;

        float x, y;
        int moving;
        RiderPos(r, now, &x, &y, &moving);
        out[n].simX = x;
        out[n].simY = y;
        out[n].team = r.team;
        out[n].moving = moving;
        n++;
    }

    return n;
}
