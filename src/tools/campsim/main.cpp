// campsim - headless FreeFalcon campaign simulator.
//
// Loads a campaign (.cam + CampaignDB + theater files), ticks the real
// campaign AI, and reports the outcome. No 3D, no UI, no window:
// FM_CAMPAIGN_OVER is caught on a hidden message-only window.
//
// Usage: campsim.exe [gamedir] [savefile] [days] [seed]

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <set>
#include <map>
#include <string>
#include <stdarg.h>

#include "f4find.h"
#include "campaign.h"
#include "camplib.h"
#include "cmpclass.h"
#include "asearch.h"
#include "campcell.h"
#include "campterr.h"
#include "camplist.h"
#include "objectiv.h"
#include "unit.h"
#include "team.h"
#include "find.h"
#include "f4vu.h"
#include "falcsess.h"
#include "classtbl.h"
#include "entity.h"
#include "aiinput.h"
#include "tactics.h"
#include "debuggr.h"
#include "threadmgr.h"
#include "weather.h"
#include "simloop.h"
#include "falcuser.h"
#include "setup.h"
#include "radardata.h"
#include "gndunit.h"
#include "path.h"
#include "atm.h"
#include "playerop.h"
#include "../../crashhandler/bugslayerutil.h"
#include "../../crashhandler/crashhandler.h"
#include "package.h"
#include "entity.h"
#include "falcent.h"
#include "classtbl.h"

extern "C" {
#include "codelib/resources/reslib/src/resmgr.h"
extern "C++" int gAtmDiag[NUM_TEAMS][24];
#include "cmpevent.h"
extern "C++" EventClass **CampEvents;
extern "C++" short CE_Events;

// Scripted trigger events (the .tri file): log every change of the fired flag, with the
// numbers the China/Russia triggers test (supply %, aircraft and ground-vehicle counts).
static char gEvPrev[64];
static char gSaveCam[64] = "";
// Reinforcement diagnostics: every campaign hour, how many units are still waiting in the inactive
// list, per kind, with their release levels, next to each team's reinforcement counter.
extern "C++" VuFilteredList *InactiveList;
static void ReinfLog(int hour)
{
    int n[NUM_TEAMS][3];
    int lowest[NUM_TEAMS][3];
    int staleInList = 0;
    memset(n, 0, sizeof(n));

    for (int t = 0; t < NUM_TEAMS; t++)
        for (int k = 0; k < 3; k++)
            lowest[t][k] = 999;

    if (InactiveList)
    {
        VuListIterator it(InactiveList);

        for (Unit u = GetFirstUnit(&it); u; u = GetNextUnit(&it))
        {
            if (!u->Inactive())
            {
                staleInList++;
                continue;
            }
            int k = u->IsBattalion() ? 0 : (u->IsSquadron() ? 1 : 2);
            int t = u->GetTeam();

            if (t >= 0 && t < NUM_TEAMS)
            {
                n[t][k]++;

                if (u->GetUnitReinforcementLevel() < lowest[t][k])
                    lowest[t][k] = u->GetUnitReinforcementLevel();
            }
        }
    }

    printf("REINF h=%d staleInInactiveList=%d", hour, staleInList);

    for (int t = 1; t <= 6; t++)
        if (TeamInfo[t])
            printf(" | T%d ctr=%d bn=%d(min %d) sq=%d(min %d) tf=%d", t, (int)TeamInfo[t]->GetReinforcement(), n[t][0],
                   lowest[t][0], n[t][1], lowest[t][1], n[t][2]);

    printf("\n");
    fflush(stdout);
}

static void CheckEventLog(CampaignTime startTime)
{
    if (!CampEvents)
        return;

    for (int i = 1; i < CE_Events && i < 64; i++)
    {
        char now = (CampEvents[i] && CampEvents[i]->HasFired()) ? 1 : 0;

        if (now != gEvPrev[i])
        {
            gEvPrev[i] = now;
            TeamStatusType *r = TeamInfo[6] ? TeamInfo[6]->GetCurrentStats() : NULL;
            TeamStatusType *k = TeamInfo[2] ? TeamInfo[2]->GetCurrentStats() : NULL;
            printf("EVENT %d %s at min %d (day %d) | DPRK supply=%d ac=%d gnd=%d | ROK ac=%d gnd=%d\n", i,
                   now ? "FIRED" : "reset", (int)((TheCampaign.CurrentTime - startTime) / CampaignMinutes),
                   TheCampaign.GetCampaignDay(), r ? (int)r->supplyLevel : -1, r ? (int)r->aircraft : -1,
                   r ? (int)r->groundVehs : -1, k ? (int)k->aircraft : -1, k ? (int)k->groundVehs : -1);
            fflush(stdout);
        }
    }
}

}

// Defined in stubs.cpp (winmain.cpp normally provides these).
void CampsimSetupGlobals(void);
void CampsimSetSeed(unsigned int seed);
extern void CampsimDumpMoveDiag(void);
// The movement counters live in an optional engine patch (tools/campsim/movement-diag.patch).
// Without it this resolves to the empty stub in stubs.cpp.
#pragma comment(linker, "/alternatename:?CampsimDumpMoveDiag@@YAXXZ=?CampsimDumpMoveDiagStub@@YAXXZ")
void CampsimSetAppWindow(HWND win);

extern void InitVU(void);
extern VU_ID gPlayerSquadronId;
extern AS_DataClass *ASD;
extern char FalconPictureDirectory[_MAX_PATH];

// campaign.cpp's internal tick entry points.
void DoCampaignLoop(int startup);
void UpdateParentUnits(CampaignTime deltatime);
void UpdateRealUnits(CampaignTime deltatime);

static bool gEndGame = false;
static int gEndGameResult = 0;

// The game's sim loop pumps the main VU message queue. campsim has no sim
// loop, so this tiny thread does that job for JoinGame/insert broadcasts.
// Held by the pump while it drains the VU queue and by the timeline recorder
// while it walks the unit/objective lists, so a frame never sees a list that
// is being edited.
static CRITICAL_SECTION gVuLock;

static volatile LONG gPumpRun = 1;

static DWORD WINAPI CampsimVuPump(LPVOID)
{
    while (gPumpRun)
    {
        EnterCriticalSection(&gVuLock);

        if (gMainThread)
            gMainThread->Update(0);

        LeaveCriticalSection(&gVuLock);

        Sleep(1);
    }

    return 0;
}

// The campaign thread lock-steps with the sim loop (NEW_SYNC): it blocks in
// ThreadManager::campaign_wait_for_sim until the sim thread signals. The real
// sim loop does that; campsim's tickler stands in for it so Suspend/Resume
// handshakes complete.
// CAMPSIM_HANGDUMP=1: break into an attached debugger after 90s so a hang can
// be captured with a stack (used during bring-up).
static DWORD WINAPI CampsimHangWatchdog(LPVOID)
{
    Sleep(90000);
    printf("WATCHDOG: breaking into debugger\n");
    fflush(stdout);
    DebugBreak();
    return 0;
}

static volatile LONG gTicklerRun = 1;

static DWORD WINAPI CampsimSimTickler(LPVOID)
{
    while (gTicklerRun)
    {
        ThreadManager::sim_signal_campaign();
        Sleep(1);
    }

    return 0;
}

static LRESULT CALLBACK CampsimWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == FM_CAMPAIGN_OVER)
    {
        gEndGame = true;
        gEndGameResult = (int)w;
        TheCampaign.EndgameResult = (uchar)w;
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static HWND CampsimCreateWindow(void)
{
    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = CampsimWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "campsim-hidden";
    RegisterClass(&wc);
    return CreateWindow("campsim-hidden", "campsim", 0, 0, 0, 0, 0, NULL, NULL,
                        wc.hInstance, NULL);
}

// CheckTriggers ends a campaign by posting FM_CAMPAIGN_OVER to the app window
// (cmpevent.cpp:1065). Drain the queue so CampsimWndProc sees it.
static void CampsimPumpMessages(void)
{
    MSG msg;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static const char *KindName(int k)
{
    switch (k)
    {
    case 0:
        return "battalion";
    case 1:
        return "brigade";
    case 2:
        return "squadron";
    case 3:
        return "taskforce";
    }
    return "other";
}

// 0..3 are the campaign's real units, 4..5 are transient air units the ATM
// creates (flights/packages).
static int KindOf(Unit u)
{
    if (u->IsBattalion())
        return 0;
    if (u->IsBrigade())
        return 1;
    if (u->IsSquadron())
        return 2;
    if (u->IsTaskForce())
        return 3;
    if (u->IsFlight())
        return 4;
    if (u->IsPackage())
        return 5;
    return -1;
}

template <typename L>
static void CountUnitList(L *list, int counts[NUM_TEAMS][6], int *total)
{
    VuListIterator uit(list);
    Unit u = GetFirstUnit(&uit);

    while (u)
    {
        int k = KindOf(u);
        int t = u->GetTeam();

        if (k >= 0 && t >= 0 && t < NUM_TEAMS)
            counts[t][k]++;

        (*total)++;
        u = GetNextUnit(&uit);
    }
}

static void DumpCampaign(const char *tag)
{
    printf("== %s\n", tag);
    printf("scenario=%s theater=%s save=%s day=%d endgame=%d time=%u seedrand=%d\n",
           TheCampaign.Scenario, TheCampaign.TheaterName, TheCampaign.SaveFile,
           TheCampaign.GetCampaignDay(), TheCampaign.EndgameResult,
           (unsigned int)TheCampaign.CurrentTime, rand());
    fflush(stdout);

    for (int t = 0; t < NUM_TEAMS; t++)
    {
        if (!TeamInfo[t])
            continue;
        TeamStatusType *cs = TeamInfo[t]->GetCurrentStats();
        printf("team %d %-12s gnd=%d air=%d ad=%d sea=%d ab=%d init=%d\n", t,
               TeamInfo[t]->name, cs->groundVehs, cs->aircraft,
               cs->airDefenseVehs, cs->ships, cs->airbases,
               TeamInfo[t]->GetInitiative());
    }

    int active = 0, inactive = 0;
    int counts[NUM_TEAMS][6];
    int inactiveCounts[NUM_TEAMS][6];
    memset(counts, 0, sizeof(counts));
    memset(inactiveCounts, 0, sizeof(inactiveCounts));

    CountUnitList(AllUnitList, counts, &active);
    CountUnitList(InactiveList, inactiveCounts, &inactive);

    printf("units=%d (active=%d inactive=%d)\n", active + inactive, active,
           inactive);

    {
        std::set<unsigned int> ids;
        int n = 0, dup = 0;
        VuListIterator iit(InactiveList);
        Unit u = GetFirstUnit(&iit);

        while (u)
        {
            n++;

            if (!ids.insert(u->Id().num_).second)
                dup++;

            u = GetNextUnit(&iit);
        }

        printf("  inactive total=%d unique=%d dup=%d\n", n, (int)ids.size(),
               dup);
    }

    for (int t = 0; t < NUM_TEAMS; t++)
    {
        int sum = 0;

        for (int k = 0; k < 6; k++)
            sum += counts[t][k] + inactiveCounts[t][k];

        if (!sum)
            continue;

        printf("  team %d %-12s bat=%d+%d bde=%d+%d sqn=%d+%d tf=%d+%d "
               "flt=%d+%d pkg=%d+%d total=%d\n",
               t, TeamInfo[t]->name, counts[t][0], inactiveCounts[t][0],
               counts[t][1], inactiveCounts[t][1], counts[t][2],
               inactiveCounts[t][2], counts[t][3], inactiveCounts[t][3],
               counts[t][4], inactiveCounts[t][4], counts[t][5],
               inactiveCounts[t][5], sum);
    }

    int objs = 0;
    int airbases[NUM_TEAMS];
    int airstrips[NUM_TEAMS];
    memset(airbases, 0, sizeof(airbases));
    memset(airstrips, 0, sizeof(airstrips));

    {
        VuListIterator oit(AllObjList);
        Objective o = GetFirstObjective(&oit);

        while (o)
        {
            objs++;
            int t = o->GetTeam();

            if (t >= 0 && t < NUM_TEAMS)
            {
                if (o->GetType() == TYPE_AIRBASE)
                    airbases[t]++;
                else if (o->GetType() == TYPE_AIRSTRIP)
                    airstrips[t]++;
            }

            o = GetNextObjective(&oit);
        }
    }

    printf("objectives=%d\n", objs);

    for (int t = 0; t < NUM_TEAMS; t++)
    {
        if (!airbases[t] && !airstrips[t])
            continue;

        printf("  team %d %-12s airbases=%d airstrips=%d\n", t,
               TeamInfo[t]->name, airbases[t], airstrips[t]);
    }

    fflush(stdout);
}


// ---------------------------------------------------------------------------
// Timeline recorder (tools/campsim/viewer.html plays it back).
//
// One JSON object per line. Line 1 is "meta": every objective once, with its
// grid position. Each later line is a frame: the objectives whose owner
// changed since the last frame, every mobile unit's position and strength,
// and per-team totals. Grid units are the campaign's own (1 km).
// ---------------------------------------------------------------------------
// The engine's headers swap in ResFOpen for fopen, and the CRT FILE machinery
// corrupted the (SmartHeap) heap here, so the timeline goes through plain
// Win32 file I/O instead.
static HANDLE gTl = INVALID_HANDLE_VALUE;
static std::string gTlBuf;

static void TlFlush(void)
{
    DWORD w;

    if (gTl != INVALID_HANDLE_VALUE && !gTlBuf.empty())
        WriteFile(gTl, gTlBuf.data(), (DWORD)gTlBuf.size(), &w, NULL);

    gTlBuf.clear();
}

static void TlPrintf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(tmp, sizeof(tmp) - 1, fmt, ap);
    va_end(ap);

    if (n < 0)
        n = (int)sizeof(tmp) - 1;

    gTlBuf.append(tmp, n);

    if (gTlBuf.size() > 65536)
        TlFlush();
}
static std::map<int, int> gTlObjTeam;

static void TimelineMeta(const char *save, unsigned int seed, int days)
{
    if (gTl == INVALID_HANDLE_VALUE)
        return;

    TlPrintf("{\"meta\":1,\"scenario\":\"%s\",\"seed\":%u,\"days\":%d,"
                 "\"day0\":%d,\"teams\":[",
            save, seed, days, TheCampaign.GetCampaignDay());

    for (int t = 0; t < NUM_TEAMS; t++)
        TlPrintf("%s\"%s\"", t ? "," : "",
                TeamInfo[t] ? TeamInfo[t]->name : "");

    TlPrintf("],\"roe\":[");

    for (int a = 0; a < NUM_TEAMS; a++)
    {
        TlPrintf("%s[", a ? "," : "");

        for (int b = 0; b < NUM_TEAMS; b++)
            TlPrintf("%s%d", b ? "," : "", GetRoE(a, b, ROE_GROUND_CAPTURE));

        TlPrintf("]");
    }

    {
        int po = 0;
        VuListIterator pit(POList);
        Objective p = GetFirstObjective(&pit);

        while (p)
        {
            po++;
            p = GetNextObjective(&pit);
        }

        TlPrintf("],\"po\":%d,\"objs\":[", po);
    }

    VuListIterator oit(AllObjList);
    Objective o = GetFirstObjective(&oit);
    int first = 1;

    while (o)
    {
        GridIndex x, y;
        o->GetLocation(&x, &y);
        TlPrintf("%s[%d,%d,%d,%d,%d]", first ? "" : ",", (int)o->GetCampID(),
                (int)x, (int)y, (int)o->GetType(), (int)o->GetTeam());
        gTlObjTeam[o->GetCampID()] = o->GetTeam();
        first = 0;
        o = GetNextObjective(&oit);
    }

    TlPrintf("]}\n");
    TlFlush();
}

static int CountList(ListClass *l);

// kind: 0 battalion 1 brigade 2 squadron 3 taskforce 4 flight
static void TimelineFrame(CampaignTime startTime)
{
    if (gTl == INVALID_HANDLE_VALUE)
        return;

    EnterCriticalSection(&gVuLock);
    CampaignTime t = TheCampaign.CurrentTime;
    long sum[NUM_TEAMS][4]; // ground, air defense, aircraft, ships
    int objsOwned[NUM_TEAMS];
    memset(sum, 0, sizeof(sum));
    memset(objsOwned, 0, sizeof(objsOwned));

    TlPrintf("{\"t\":%d,\"day\":%d,\"hour\":%d,\"o\":[",
            (int)((t - startTime) / CampaignMinutes),
            TheCampaign.GetCampaignDay(),
            (int)((t % CampaignDay) / CampaignHours));

    {
        VuListIterator oit(AllObjList);
        Objective o = GetFirstObjective(&oit);
        int first = 1;

        while (o)
        {
            int team = o->GetTeam();
            int id = o->GetCampID();

            if (team >= 0 && team < NUM_TEAMS)
                objsOwned[team]++;

            std::map<int, int>::iterator it = gTlObjTeam.find(id);

            if (it == gTlObjTeam.end() || it->second != team)
            {
                TlPrintf("%s[%d,%d,%d]", first ? "" : ",", id, team,
                        (int)o->GetObjectiveStatus());
                gTlObjTeam[id] = team;
                first = 0;
            }

            o = GetNextObjective(&oit);
        }
    }

    TlPrintf("],\"u\":[");
    {
        VuListIterator uit(AllUnitList);
        Unit u = GetFirstUnit(&uit);
        std::set<unsigned int> seen;
        int first = 1;

        while (u)
        {
            int k = KindOf(u);
            int team = u->GetTeam();

            if (k >= 0 && k != 5 && team >= 0 && team < NUM_TEAMS &&
                seen.insert(u->Id().num_).second)
            {
                GridIndex x, y;
                u->GetLocation(&x, &y);
                int veh = u->GetTotalVehicles();
                int full = u->GetFullstrengthVehicles();

                if (k <= 1)
                    sum[team][u->GetUnitNormalRole() == GRO_AIRDEFENSE ? 1 : 0] += veh;
                else if (k == 2 || k == 4)
                    sum[team][2] += veh;
                else
                    sum[team][3] += veh;

                // Squadrons sit on their airbase and never move; the summary
                // counts them but the frame does not need them.
                if (k != 2)
                {
                    GridIndex ox = 0, oy = 0, dx = 0, dy = 0;
                    int oid = 0;

                    if (k <= 1)
                    {
                        Objective uo = u->GetUnitObjective();

                        if (uo)
                        {
                            uo->GetLocation(&ox, &oy);
                            oid = uo->GetCampID();
                        }

                        u->GetUnitDestination(&dx, &dy);
                    }

                    TlPrintf("%s[%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]", first ? "" : ",",
                             (unsigned)u->Id().num_, k, team, (int)x, (int)y, veh,
                             full, u->Moving() ? 1 : 0,
                             k <= 1 ? u->GetUnitMorale() : 0,
                             k <= 1 ? (int)u->GetUnitOrders() : 0,
                             k <= 1 ? (int)u->GetUnitTactic() : 0,
                             k <= 1 ? (int)u->GetUnitSupply() : 0,
                             oid, (int)ox, (int)oy, (int)dx, (int)dy);
                    first = 0;
                }
            }

            u = GetNextUnit(&uit);
        }
    }

    TlPrintf("],\"a\":[");

    for (int tm = 0; tm < NUM_TEAMS; tm++)
    {
        TeamGndActionType *ga = TeamInfo[tm] ? TeamInfo[tm]->GetGroundAction() : NULL;

        TlPrintf("%s[%d,%d,%d,%d,%d]", tm ? "," : "", ga ? (int)ga->actionType : 0,
                 ga ? (int)ga->actionTempo : 0, ga ? (int)ga->actionPoints : 0,
                 TeamInfo[tm] ? (int)TeamInfo[tm]->GetInitiative() : 0,
                 ga ? (int)(ga->actionObjective.num_ & 0xFFFF) : 0);
    }

    TlPrintf("],\"ad\":[");

    for (int tm = 0; tm < NUM_TEAMS; tm++)
    {
        AirTaskingManagerClass *atm = TeamInfo[tm] ? TeamInfo[tm]->atm : NULL;
        int pkgs = 0, ab = 0, abLow = 0, abSum = 0;

        if (atm && atm->packageList)
        {
            VuListIterator pit(atm->packageList);

            for (Unit pk = GetFirstUnit(&pit); pk; pk = GetNextUnit(&pit))
                pkgs++;
        }

        {
            VuListIterator oit(AllObjList);
            Objective o = GetFirstObjective(&oit);

            while (o)
            {
                if (o->GetTeam() == tm && (o->GetType() == TYPE_AIRBASE || o->GetType() == TYPE_AIRSTRIP))
                {
                    ab++;
                    abSum += o->GetObjectiveStatus();

                    if (o->GetObjectiveStatus() < 50)
                        abLow++;
                }

                o = GetNextObjective(&oit);
            }
        }

        TlPrintf("%s[%d,%d,%d,%d,%d,%d,%d]", tm ? "," : "", atm ? CountList(atm->requestList) : 0,
                 atm ? atm->missionsToFill : 0, atm ? atm->missionsFilled : 0, pkgs, ab, abLow,
                 ab ? abSum / ab : 0);
    }

    // cumulative FindBestAir rejects [0..9] and BuildPackage results [10..22]
    TlPrintf("],\"af\":[");

    for (int tm = 0; tm < NUM_TEAMS; tm++)
    {
        TlPrintf("%s[", tm ? "," : "");

        for (int k = 0; k < 24; k++)
            TlPrintf("%s%d", k ? "," : "", gAtmDiag[tm][k]);

        TlPrintf("]");
    }

    TlPrintf("],\"ev\":[");

    for (int i = 1, f = 0; i < CE_Events && i < 64; i++)
        if (CampEvents && CampEvents[i] && CampEvents[i]->HasFired())
            TlPrintf("%s%d", f++ ? "," : "", i);

    TlPrintf("],\"st\":[");

    for (int tm = 0; tm < NUM_TEAMS; tm++)
    {
        TeamStatusType *cs = TeamInfo[tm] ? TeamInfo[tm]->GetCurrentStats() : NULL;
        TlPrintf("%s[%d,%d,%d]", tm ? "," : "", cs ? (int)cs->supplyLevel : 0, cs ? (int)cs->aircraft : 0,
                 cs ? (int)cs->groundVehs : 0);
    }

    TlPrintf("],\"s\":[");

    for (int tm = 0; tm < NUM_TEAMS; tm++)
        TlPrintf("%s[%ld,%ld,%ld,%ld,%d]", tm ? "," : "", sum[tm][0],
                sum[tm][1], sum[tm][2], sum[tm][3], objsOwned[tm]);

    TlPrintf("]}\n");
    TlFlush();
    LeaveCriticalSection(&gVuLock);
}


// ---------------------------------------------------------------------------
// Experiment knobs: extra args of the form key=value after the positionals.
//   speed=F     scale every ground unit class's MovementSpeed
//   cost=F      scale the ground terrain cost table (foot/wheeled/tracked)
//   strip=T:P   remove P% of the vehicles of every battalion of team T
//   abheal=T     team T's airbases/airstrips are fully repaired every 10 ticks
//   airtempo=T:P ATM of team T may task at least P% of its pending mission requests each tick
//   stripair=T:P remove P% of the aircraft of every squadron of team T
//   init=T:V    set team T's initiative
//   simtogrid=0 restore the old (buggy) sim->grid rounding, for A/B runs
//   usecfg=1    load FFViper.cfg from the game dir (as the real game does) before the campaign loads
// ---------------------------------------------------------------------------
// airtempo=T:P -- every tick, make sure team T's ATM may task at least P% of the mission requests it
// has pending (the engine sets missionsToFill = requested * groundAction.actionTempo / 100).
static int gAirTempo[NUM_TEAMS];
// abheal=T -- every few ticks, repair every damaged feature of team T's airbases and airstrips.
static int gAbHeal[NUM_TEAMS];

static void ApplyAbHeal(void)
{
    VuListIterator oit(AllObjList);
    Objective o = GetFirstObjective(&oit);

    while (o)
    {
        int t = o->GetTeam();

        if (t >= 0 && t < NUM_TEAMS && gAbHeal[t] &&
            (o->GetType() == TYPE_AIRBASE || o->GetType() == TYPE_AIRSTRIP) && o->GetObjectiveStatus() < 100)
            for (int f = 0; f < o->GetTotalFeatures(); f++)
                o->RepairFeature(f);

        o = GetNextObjective(&oit);
    }
}

static int CountList(ListClass *l)
{
    int n = 0;

    for (ListNode e = l ? l->GetFirstElement() : NULL; e; e = e->GetNext())
        n++;

    return n;
}

static void ApplyAirTempo(void)
{
    for (int t = 0; t < NUM_TEAMS; t++)
    {
        if (!gAirTempo[t] || !TeamInfo[t] || !TeamInfo[t]->atm)
            continue;

        AirTaskingManagerClass *atm = TeamInfo[t]->atm;
        int need = (CountList(atm->requestList) * gAirTempo[t]) / 100;

        if (atm->missionsToFill < need)
            atm->missionsToFill = need;
    }
}

extern short NumUnitEntries;
extern int g_nSimToGridFix;
extern void ReadFalcon4Config();
extern void ParseFalcon4Config(FILE *file);
extern bool g_bRealisticAttrition, g_bFireOntheMove, g_bLargeStrike;
extern int g_nNoPlayerPlay;

// newgame=1 (default when no event has fired yet) -- run the setup the UI runs when the player starts a NEW
// campaign (campjoin.cpp -> AdjustCampaignOptions): enemy experience, force-ratio company chop,
// every unit resupplied to the team's start supply level, squadrons fuelled and armed, initial
// events. Without it the save's raw unit supply (~40%) drives the China/Russia supply triggers.
//   ratio=G:A:D:N   force ratios as on the campaign setup screen (0-4, default 2:2:2:2)
//   exp=A:G         enemy air / ground experience (0 green .. 4 ace, default from player options)
extern void AdjustCampaignOptions(void);

static void ApplyNewGame(int argc, char **argv, const char *savefile)
{
    // A save no campaign event has fired in yet is a campaign start (save0 and every variant built
    // from it); a mid-campaign save keeps the supply it was saved with.
    int on = 1;

    for (int i = 1; CampEvents && i < CE_Events; i++)
        if (CampEvents[i] && CampEvents[i]->HasFired())
            on = 0;

    int r[4] = {2, 2, 2, 2};

    for (int a = 7; a < argc; a++)
    {
        if (!strncmp(argv[a], "newgame=", 8))
            on = atoi(argv[a] + 8);
        else if (!strncmp(argv[a], "ratio=", 6))
            sscanf(argv[a] + 6, "%d:%d:%d:%d", &r[0], &r[1], &r[2], &r[3]);
        else if (!strncmp(argv[a], "exp=", 4))
            sscanf(argv[a] + 4, "%d:%d", &PlayerOptions.CampEnemyAirExperience,
                   &PlayerOptions.CampEnemyGroundExperience);
    }

    if (!on)
    {
        printf("KNOB newgame=0: units keep the supply stored in the save (%s)\n", savefile);
        return;
    }

    TheCampaign.GroundRatio = (short)r[0];
    TheCampaign.AirRatio = (short)r[1];
    TheCampaign.AirDefenseRatio = (short)r[2];
    TheCampaign.NavalRatio = (short)r[3];
    TheCampaign.EnemyAirExp = (uchar)PlayerOptions.CampEnemyAirExperience;
    TheCampaign.EnemyADExp = (uchar)PlayerOptions.CampEnemyGroundExperience;
    AdjustCampaignOptions();
    printf("KNOB newgame: ratios %d:%d:%d:%d, enemy exp %d:%d, units resupplied to start level\n",
           r[0], r[1], r[2], r[3], PlayerOptions.CampEnemyAirExperience,
           PlayerOptions.CampEnemyGroundExperience);
}

static void ApplyKnobs(int argc, char **argv)
{
    for (int a = 7; a < argc; a++)
    {
        char *eq = strchr(argv[a], '=');

        if (!eq)
            continue;

        *eq = 0;
        const char *key = argv[a];
        const char *val = eq + 1;

        if (!strcmp(key, "speed"))
        {
            float f = (float)atof(val);
            int n = 0;

            for (int i = 0; i < NumUnitEntries; i++)
            {
                UnitClassDataType *uc = &UnitDataTable[i];

                if (uc->MovementType == Foot or uc->MovementType == Wheeled or
                    uc->MovementType == Tracked)
                {
                    int s = (int)(uc->MovementSpeed * f);
                    uc->MovementSpeed = (short)(s > 32000 ? 32000 : s);
                    n++;
                }
            }

            printf("KNOB speed x%.2f on %d ground unit classes\n", f, n);
        }
        else if (!strcmp(key, "cost"))
        {
            float f = (float)atof(val);

            for (int c = 0; c < COVER_TYPES; c++)
                for (int m = Foot; m <= Tracked; m++)
                    if (CostTable[c][m] < 50.0F)
                    {
                        CostTable[c][m] *= f;

                        if (CostTable[c][m] < 0.5F)
                            CostTable[c][m] = 0.5F;
                    }

            printf("KNOB terrain cost x%.2f\n", f);
        }
        else if (!strcmp(key, "strip"))
        {
            int team = atoi(val);
            const char *colon = strchr(val, ':');
            int pct = colon ? atoi(colon + 1) : 0;
            int units = 0, removed = 0;
            VuListIterator uit(AllUnitList);
            Unit u = GetFirstUnit(&uit);

            while (u)
            {
                if (u->IsBattalion() and u->GetTeam() == team)
                {
                    int n = (u->GetTotalVehicles() * pct) / 100;

                    if (n > 0)
                    {
                        u->ChangeVehicles(-n);
                        removed += n;
                    }

                    units++;
                }

                u = GetNextUnit(&uit);
            }

            printf("KNOB strip team %d by %d%%: %d battalions, %d vehicles removed\n", team, pct, units,
                   removed);
        }
        else if (!strcmp(key, "simtogrid"))
        {
            g_nSimToGridFix = atoi(val);
            printf("KNOB g_nSimToGridFix = %d\n", g_nSimToGridFix);
        }
        else if (!strcmp(key, "abheal"))
        {
            int team = atoi(val);

            if (team >= 0 && team < NUM_TEAMS)
                gAbHeal[team] = 1;

            printf("KNOB abheal team %d: airbases and airstrips repaired every 10 ticks\n", team);
        }
        else if (!strcmp(key, "airtempo"))
        {
            int team = atoi(val);
            const char *colon = strchr(val, ':');
            int pct = colon ? atoi(colon + 1) : 0;

            if (team >= 0 && team < NUM_TEAMS)
                gAirTempo[team] = pct;

            printf("KNOB airtempo team %d floor %d%% of pending requests\n", team, pct);
        }
        else if (!strcmp(key, "savecam"))
        {
            strncpy(gSaveCam, val, sizeof(gSaveCam) - 1);
            printf("KNOB savecam %s (campaign saved at the end of the run)\n", gSaveCam);
        }
        else if (!strcmp(key, "stripair"))
        {
            int team = atoi(val);
            const char *colon = strchr(val, ':');
            int pct = colon ? atoi(colon + 1) : 0;
            int units = 0, removed = 0;
            VuListIterator uit(AllUnitList);
            Unit u = GetFirstUnit(&uit);

            while (u)
            {
                if (u->IsSquadron() and u->GetTeam() == team)
                {
                    int n = (u->GetTotalVehicles() * pct) / 100;

                    if (n > 0)
                    {
                        u->ChangeVehicles(-n);
                        removed += n;
                    }

                    units++;
                }

                u = GetNextUnit(&uit);
            }

            printf("KNOB stripair team %d by %d%%: %d squadrons, %d aircraft removed\n", team, pct, units,
                   removed);
        }
        else if (!strcmp(key, "init"))
        {
            int team = atoi(val);
            const char *colon = strchr(val, ':');
            int v = colon ? atoi(colon + 1) : 0;

            if (TeamInfo[team])
                TeamInfo[team]->SetInitiative((short)v);

            printf("KNOB initiative team %d = %d\n", team, v);
        }
        else if (strcmp(key, "usecfg") and strcmp(key, "cfgfile") and strcmp(key, "newgame") and
                 strcmp(key, "ratio") and strcmp(key, "exp"))
            printf("KNOB unknown '%s'", key), printf("\n");

        *eq = '=';
    }

    fflush(stdout);
}

static int PickPlayerSquadron(void)
{
    int pick = -1;

    for (int i = 0; i < TheCampaign.NumAvailSquadrons; i++)
    {
        if (TheCampaign.CampaignSquadronData[i].country == COUN_SOUTH_KOREA)
        {
            pick = i;
            break;
        }
    }

    if (pick < 0)
    {
        for (int i = 0; i < TheCampaign.NumAvailSquadrons; i++)
        {
            if (TheCampaign.CampaignSquadronData[i].country == COUN_US)
            {
                pick = i;
                break;
            }
        }
    }

    if (pick < 0 && TheCampaign.NumAvailSquadrons > 0)
        pick = 0;

    if (pick < 0)
    {
        printf("ERR: no selectable squadron\n");
        return 0;
    }

    FalconLocalSession->SetCountry(
        TheCampaign.CampaignSquadronData[pick].country);
    gPlayerSquadronId = TheCampaign.CampaignSquadronData[pick].id;
    printf("player squadron idx=%d country=%d id=%d/%d\n", pick,
           TheCampaign.CampaignSquadronData[pick].country,
           (int)gPlayerSquadronId.num_, (int)gPlayerSquadronId.creator_);
    return 1;
}

#define STEP(msg)                                                              \
    do                                                                         \
    {                                                                          \
        printf("step: %s\n", msg);                                             \
        fflush(stdout);                                                        \
    } while (0)

// A crash used to end a run with nothing but a truncated log (the ~1-in-5 0xC0000005 in long
// runs). Write the fault and a symbolised stack into the log itself (stdout). Raw Win32 only:
// the process is already damaged.
static void CrashOut(const char *s)
{
    DWORD n = 0, w = 0;

    while (s && s[n])
        n++;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, NULL);
}

static LONG __stdcall CampsimCrashFilter(EXCEPTION_POINTERS *ep)
{
    static LONG inCrash = 0;

    if (InterlockedExchange(&inCrash, 1))
        return EXCEPTION_CONTINUE_SEARCH;

    char line[160];
    wsprintfA(line, "\nCRASH at campaign min %d (day %d)\nreason : ", (int)(TheCampaign.CurrentTime / CampaignMinutes),
              (int)TheCampaign.GetCampaignDay());
    CrashOut(line);
    CrashOut(GetFaultReason(ep));
    CrashOut("\nstack:\n");
    const DWORD opts = GSTSO_MODULE | GSTSO_SYMBOL | GSTSO_SRCLINE;
    LPCTSTR frame = GetFirstStackTraceString(opts, ep);

    for (int guard = 0; frame && guard < 64; guard++)
    {
        CrashOut("  ");
        CrashOut(frame);
        CrashOut("\n");
        frame = GetNextStackTraceString(opts, ep);
    }

    CrashOut("CRASH END\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    InitializeCriticalSection(&gVuLock);
    setvbuf(stdout, NULL, _IONBF, 0);

    SetCrashHandlerFilter(CampsimCrashFilter);
    const char *gamedir = (argc > 1) ? argv[1] : "C:/FreeFalcon6";
    const char *savefile = (argc > 2) ? argv[2] : "save0";
    int days = (argc > 3) ? atoi(argv[3]) : 0;
    unsigned int seed = (argc > 4) ? (unsigned int)strtoul(argv[4], NULL, 0)
                                   : (unsigned int)time(NULL);

    const char *timelinePath = (argc > 5) ? argv[5] : NULL;
    int timelineEvery = (argc > 6) ? atoi(argv[6]) : 60; // campaign minutes

    if (timelineEvery < 1)
        timelineEvery = 60;

    char slashdir[_MAX_PATH];
    strncpy(slashdir, gamedir, _MAX_PATH - 1);
    slashdir[_MAX_PATH - 1] = 0;

    for (char *p = slashdir; *p; p++)
        if (*p == '\\')
            *p = '/';

    strcpy(FalconDataDirectory, slashdir);
    sprintf(FalconTerrainDataDir, "%s/terrdata/korea", slashdir);
    sprintf(FalconObjectDataDir, "%s/terrdata/objects", slashdir);
    strcpy(Falcon3DDataDir, FalconObjectDataDir);
    sprintf(FalconMiscTexDataDir, "%s/terrdata/misctex", slashdir);
    sprintf(FalconPictureDirectory, "%s/Pictures", slashdir);
    sprintf(FalconCampUserSaveDirectory, "%s/campaign/SAVE", slashdir);
    strcpy(FalconCampaignSaveDirectory, FalconCampUserSaveDirectory);

    SetCurrentDirectory(slashdir);
    STEP("dirs");

    CampsimSetSeed(seed);

    InitDebug(DEBUGGER_TEXT_MODE);
    STEP("debug");

    ResInit(NULL);
    ResCreatePath(FalconDataDirectory, FALSE);
    ResAddPath(FalconCampUserSaveDirectory, FALSE);
    {
        char tmp[_MAX_PATH];
        sprintf(tmp, "%s/sim", FalconDataDirectory);
        ResAddPath(tmp, TRUE);
        sprintf(tmp, "%s/Zips", FalconDataDirectory);
        ResAddPath(tmp, FALSE);
    }
    STEP("res paths");

    // usecfg=1: load FFViper.cfg exactly as the game does (campaign-relevant keys such as
    // g_bRealisticAttrition, g_bFireOntheMove, g_bLargeStrike change the war).
    for (int a = 7; a < argc; a++)
    {
        if (!strncmp(argv[a], "cfgfile=", 8))
        {
            FILE *cf = fopen(argv[a] + 8, "r");

            if (cf)
            {
                ParseFalcon4Config(cf);
                fclose(cf);
                printf("KNOB cfgfile %s: RealisticAttrition=%d FireOntheMove=%d LargeStrike=%d\n", argv[a] + 8,
                       (int)g_bRealisticAttrition, (int)g_bFireOntheMove, (int)g_bLargeStrike);
            }
            else
                printf("KNOB cfgfile: cannot open %s\n", argv[a] + 8);
        }

        if (!strcmp(argv[a], "usecfg=1"))
        {
            ReadFalcon4Config();
            printf("KNOB usecfg: RealisticAttrition=%d FireOntheMove=%d LargeStrike=%d NoPlayerPlay=%d SimToGridFix=%d\n",
                   (int)g_bRealisticAttrition, (int)g_bFireOntheMove, (int)g_bLargeStrike, g_nNoPlayerPlay,
                   g_nSimToGridFix);
        }
    }


    CampsimSetupGlobals();
    STEP("globals (comms/weather)");

    // Campaign mission planning reads terrain heights through TheMap.GetMEA.
    // The game loads the terrain DB from FalconDisplay.Setup; campsim calls
    // the device-independent half directly.
    DeviceIndependentGraphicsSetup(FalconTerrainDataDir, Falcon3DDataDir,
                                   FalconMiscTexDataDir);
    STEP("terrain setup");

    ASD = new AS_DataClass();
    STEP("ASD");

    ReadCampAIInputs("Falcon4");
    STEP("AII");

    if (!LoadClassTable("Falcon4"))
    {
        printf("ERR: LoadClassTable failed\n");
        return 1;
    }

    STEP("class table");

    // Campaign weapon checks read the sim's radar dataset (radar.cpp).
    ReadAllRadarData();
    printf("radarDatFileTable=%p entries=%d\n", (void *)radarDatFileTable,
           NumRadarDatFileTable);
    fflush(stdout);
    STEP("radar data");

    InitVU();
    CreateThread(NULL, 0, CampsimVuPump, NULL, 0, NULL);
    STEP("VU");

    if (!LoadTactics("Falcon4"))
    {
        printf("ERR: LoadTactics failed\n");
        return 1;
    }

    STEP("tactics");

    {
        HWND win = CampsimCreateWindow();
        CampsimSetAppWindow(win);
        printf("campsim window=%p seed=%u\n", win, seed);
    }

    STEP("window");

    if (getenv("CAMPSIM_HANGDUMP"))
        CreateThread(NULL, 0, CampsimHangWatchdog, NULL, 0, NULL);

    ThreadManager::setup();
    CreateThread(NULL, 0, CampsimSimTickler, NULL, 0, NULL);
    STEP("thread manager");

    Camp_Init(1);
    STEP("camp init");

    if (!TheCampaign.LoadScenarioStats(game_Campaign, (char *)savefile))
    {
        printf("ERR: LoadScenarioStats failed\n");
        return 1;
    }

    STEP("scenario preload");

    if (!PickPlayerSquadron())
        return 1;

    STEP("player squadron");

    if (!TheCampaign.LoadCampaign(game_Campaign, (char *)savefile))
    {
        printf("ERR: LoadCampaign failed\n");
        return 1;
    }

    STEP("campaign loaded");

    ApplyNewGame(argc, argv, savefile);
    ApplyKnobs(argc, argv);

    // The campaign thread is running; keep it parked while we tick by hand.
    TheCampaign.Suspend();
    InterlockedExchange(&gTicklerRun, 0);
    Sleep(50);

    // From here the main thread drains the VU queue itself, between ticks. A
    // background pump ran message handlers that edit the ATM request list
    // while Task() was walking it (use-after-free in ListClass::Remove).
    InterlockedExchange(&gPumpRun, 0);
    EnterCriticalSection(&gVuLock);
    LeaveCriticalSection(&gVuLock);
    Sleep(50);

    {
        CampEntity ps = (CampEntity)FindUnit(gPlayerSquadronId);

        if (ps && ps->IsSquadron())
            FalconLocalSession->SetPlayerSquadron((Squadron)ps);
        else
            printf("WARN: player squadron not found in unit list\n");
    }

    DumpCampaign("loaded");

    if (days > 0)
    {
        CampaignTime startTime = TheCampaign.CurrentTime;
        CampaignTime target = startTime + (CampaignTime)days * CampaignDay;
        int first = 1;
        int iter = 0;
        DWORD startWall = GetTickCount();

        printf("campsim: save=%s seed=%u days=%d, target %.1f h from %.1f h\n",
               savefile, seed, days,
               (target - startTime) / (float)CampaignHours,
               startTime / (float)CampaignHours);
        fflush(stdout);

        if (timelinePath)
        {
            gTl = CreateFileA(timelinePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

            if (gTl == INVALID_HANDLE_VALUE)
                printf("WARN: cannot open timeline %s\n", timelinePath);
            else
            {
                TimelineMeta(savefile, seed, days);
                TimelineFrame(startTime);
            }
        }

        // Mirrors HandleCampaignThread's per-iteration body (campaign.cpp:
        // 2606-2741) with a fixed 1-campaign-minute step.
        while (TheCampaign.CurrentTime < target && !gEndGame)
        {
            DWORD t0 = GetTickCount();
            if (gMainThread)
                gMainThread->Update(0);

            TheCampaign.vuThread->Update(0);
            DWORD t1 = GetTickCount();
            TheCampaign.CurrentTime += CampaignMinutes;
            TheCampaign.TimeOfDay = TheCampaign.CurrentTime % CampaignDay;
            ApplyAirTempo();

            if ((iter % 10) == 0)
                ApplyAbHeal();

            DoCampaignLoop(first);
            CheckEventLog(startTime);
            if ((iter % 60) == 0 && iter <= 60 * 30)
                ReinfLog(iter / 60);
            DWORD t2 = GetTickCount();
            first = 0;
            TheCampaign.vuThread->Update(0);
            DWORD t3 = GetTickCount();
            UpdateRealUnits(CampaignMinutes);
            DWORD t4 = GetTickCount();
            CampsimPumpMessages();
            iter++;

            // First few ticks show where the time goes; after that one line
            // per campaign hour is enough to watch it run.
            if (iter <= 5)
            {
                printf("tick %d: vu=%u loop=%u vu2=%u real=%u total=%u\n", iter,
                       t1 - t0, t2 - t1, t3 - t2, t4 - t3, t4 - t0);
                fflush(stdout);
            }

            if ((iter % 60) == 0 || gEndGame)
            {
                CampaignTime t = TheCampaign.CurrentTime;
                int live = 0;
                int counts[NUM_TEAMS][6];
                double frac = (double)(t - startTime) / (double)(target - startTime);
                double elapsed = (GetTickCount() - startWall) / 1000.0;
                double eta = (frac > 0.001) ? elapsed * (1.0 - frac) / frac : 0.0;
                memset(counts, 0, sizeof(counts));
                CountUnitList(AllUnitList, counts, &live);
                printf("[day %d %02d:%02d] %.1f/%.1f h (%3.0f%%) units=%d "
                       "endgame=%d wall=%.0fs eta=%.0fs\n",
                       TheCampaign.GetCampaignDay(),
                       (int)((t % CampaignDay) / CampaignHours),
                       (int)((t % CampaignHours) / CampaignMinutes),
                       (t - startTime) / (float)CampaignHours,
                       (target - startTime) / (float)CampaignHours,
                       100.0f * (float)frac, live, TheCampaign.EndgameResult,
                       elapsed, eta);
                fflush(stdout);
            }

            if ((iter % timelineEvery) == 0)
                TimelineFrame(startTime);

            if ((TheCampaign.CurrentTime % CampaignDay) < CampaignMinutes)
                DumpCampaign("checkpoint");
        }

        CampsimPumpMessages();
        TimelineFrame(startTime);

        if (gTl != INVALID_HANDLE_VALUE)
        {
            TlPrintf("{\"final\":1,\"endgame\":%d,\"engine\":%d,\"minutes\":%d}\n",
                    (int)TheCampaign.EndgameResult, gEndGameResult,
                    (int)((TheCampaign.CurrentTime - startTime) / CampaignMinutes));
            TlFlush();
            CloseHandle(gTl);
            gTl = INVALID_HANDLE_VALUE;
        }

        CampsimDumpMoveDiag();
        DumpCampaign("final");

        if (gSaveCam[0])
        {
            int ok = TheCampaign.SaveCampaign(game_Campaign, gSaveCam, 0);
            printf("SAVECAM %s -> %d\n", gSaveCam, ok);
            fflush(stdout);
        }

        printf("RESULT seed=%u endgame=%d engine=%d campaign_h=%.1f "
               "wall_s=%.0f\n",
               seed, TheCampaign.EndgameResult, gEndGameResult,
               (TheCampaign.CurrentTime - startTime) / (float)CampaignHours,
               (GetTickCount() - startWall) / 1000.0f);
        fflush(stdout);
    }

    printf("endgame=%d engine-result=%d\n", TheCampaign.EndgameResult,
           gEndGameResult);
    return 0;
}
