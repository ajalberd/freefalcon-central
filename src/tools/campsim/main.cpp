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
#include <vector>
#include <string>
#include <algorithm>
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
#include "battalion.h"
#include "squadron.h"
#include <dbghelp.h>
#include "package.h"
#include "entity.h"
#include "falcent.h"
#include "classtbl.h"
#include "supply.h"
#include "gtmobj.h"

extern "C" {
#include "codelib/resources/reslib/src/resmgr.h"
extern "C++" int gAtmDiag[NUM_TEAMS][24];
extern "C++" int gLossDiag[NUM_TEAMS][8];
#include "cmpevent.h"
extern "C++" EventClass **CampEvents;
extern "C++" short CE_Events;
extern "C++" int CondLogSize(void);
extern "C++" int CondLogGet(int i, int *line, int *branch, int *depth, unsigned long *time, int *a, int *b);

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
                if (staleInList++ < 4 && (hour == 1 || hour == 12))
                    printf("STALE h=%d id=%u team=%d %s camp=%d reinf=%d parent=%d inAll=%d cargo=%d dead=%d veh=%d\n", hour,
                           (unsigned)u->Id().num_, (int)u->GetTeam(),
                           u->IsBattalion() ? "bn" : u->IsSquadron() ? "sq" : u->IsBrigade() ? "brig" : u->IsTaskForce() ? "tf" : "other",
                           (int)u->GetCampID(), (int)u->GetUnitReinforcementLevel(), (int)u->Parent(),
                           AllUnitList->Find(u) ? 1 : 0, (int)u->Cargo(), (int)u->IsDead(), (int)u->GetTotalVehicles());

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

    // Which #IF branches led to the actions (cmpevent.cpp's condition history).
    static int condSeen = 0;

    for (; condSeen < CondLogSize(); condSeen++)
    {
        int line, branch, depth, a, b;
        unsigned long t;

        if (CondLogGet(condSeen, &line, &branch, &depth, &t, &a, &b))
            printf("COND tri line %d %s depth %d at min %d | a=%d b=%d\n", line, branch ? "ELSE" : "IF", depth,
                   (int)((t - startTime) / CampaignMinutes), a, b);
    }

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

// Ground tasking diagnostics (gtm.cpp gGtmDiag/gGtmAction): per team, over the last period,
// the ground posture and, per order type, objectives wanting units and candidates offered per
// call, units newly assigned and units confirmed in orders they already had.
extern "C++" int gGtmDiag[NUM_TEAMS][GORD_LAST][5];
extern "C++" int gGtmAction[NUM_TEAMS][8];
extern "C++" int gGtmWhy[NUM_TEAMS][10];
extern "C++" int gSupplyUse[8][4];

static void GtmLog(int hour)
{
    static int prev[NUM_TEAMS][GORD_LAST][5], prevAct[NUM_TEAMS][8];
    static const char *ord[] = {"RES", "CAP", "SEC", "ASL", "ABN", "CMD", "DEF", "SUP", "REP", "AD", "RCN", "RAD"};
    static const char *act[] = {"none", "defensive", "consolidate", "minor-off", "OFFENSIVE", "?5", "?6", "?7"};

    for (int t : {2, 6})
    {
        char buf[1024];
        int n = sprintf(buf, "GTM h=%d team %d posture:", hour, t);

        for (int a = 0; a < 8; a++)
            if (gGtmAction[t][a] - prevAct[t][a])
                n += sprintf(buf + n, " %s=%d", act[a], gGtmAction[t][a] - prevAct[t][a]);

        n += sprintf(buf + n, " |");

        for (int o = 0; o < GORD_LAST && o < 12; o++)
        {
            int d[5];

            for (int k = 0; k < 5; k++)
                d[k] = gGtmDiag[t][o][k] - prev[t][o][k];

            if (d[3])
                n += sprintf(buf + n, " %s obj/call %d cand/call %d new %d kept %d;", ord[o], d[0] / d[3], d[1] / d[3],
                             d[2], d[4]);
        }

        printf("%s\n", buf);

        // Why units were or were not offered for capture (gtm.cpp gGtmWhy), per GTM cycle.
        {
            static int prevWhy[NUM_TEAMS][10];
            static const char *why[] = {"keptCAP", "keptOther", "immobile", "broken", "supply<50",
                                        "singleRole", "CAPcand", "notCapable", "(kept SEC", "kept DEF)"};
            int calls = 0;

            for (int a = 0; a < 8; a++)
                calls += gGtmAction[t][a] - prevAct[t][a];

            n = sprintf(buf, "GTMWHY h=%d team %d per cycle:", hour, t);

            for (int k = 0; k < 10; k++)
                n += sprintf(buf + n, " %s %d", why[k], calls ? (gGtmWhy[t][k] - prevWhy[t][k]) / calls : 0);

            printf("%s\n", buf);
            memcpy(prevWhy[t], gGtmWhy[t], sizeof(prevWhy[t]));
        }

        memcpy(prev[t], gGtmDiag[t], sizeof(prev[t]));
        memcpy(prevAct[t], gGtmAction[t], sizeof(prevAct[t]));
    }

    // Supply need, split: SupplyUnits (supply.cpp) gives each unit need * pool / (sum of needs), and only
    // when that sum is positive. Units holding more than they want report a NEGATIVE need, so a surplus
    // in some units can zero the sum and cut everyone off. Per team: positive and negative needs of
    // battalions and squadrons, and the worst surplus holders.
    for (int t : {2, 6})
    {
        long bp = 0, bn = 0, sp = 0, sn = 0;
        int nbn = 0, nsn = 0, worst[3] = {0, 0, 0};
        unsigned worstId[3] = {0, 0, 0};
        VuListIterator uit(AllUnitList);

        for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
        {
            if (u->GetTeam() != t || !(u->IsBattalion() || u->IsSquadron()))
                continue;

            int sq = u->IsSquadron();
            int need = 0;

            if (!sq)
                need = u->GetUnitSupplyNeed(FALSE);
            else
            {
                // SquadronClass::GetUnitSupplyNeed also relocates and scrambles the squadron (squadron.cpp),
                // so the logger must not call it; same formula, read only.
                UnitClassDataType *uc = u->GetUnitClassData();
                int want = 0, got = 0;

                for (int i = 0; uc && i < MAXIMUM_WEAPTYPES; i++)
                {
                    want += SquadronStoresDataTable[uc->SpecialIndex].Stores[i];
                    got += ((SquadronClass *)u)->GetUnitStores(i);
                }

                need = (want - got) / SQUADRON_PT_SUPPLY;
            }

            if (need >= 0)
                (sq ? sp : bp) += need;
            else
            {
                (sq ? sn : bn) += need, (sq ? nsn : nbn)++;

                for (int k = 0; k < 3; k++)
                    if (need < worst[k])
                    {
                        for (int m = 2; m > k; m--)
                            worst[m] = worst[m - 1], worstId[m] = worstId[m - 1];

                        worst[k] = need, worstId[k] = (unsigned)u->Id().num_ | (sq ? 0x80000000u : 0);
                        break;
                    }
            }
        }

        printf("SUPNEED h=%d team %d: battalions +%ld %ld (%d in surplus), squadrons +%ld %ld (%d in surplus), "
               "sum %ld | worst:", hour, t, bp, bn, nbn, sp, sn, nsn, bp + bn + sp + sn);

        for (int k = 0; k < 3 && worst[k]; k++)
            printf(" %s %u %d", (worstId[k] & 0x80000000u) ? "sq" : "bn", worstId[k] & 0x7fffffffu, worst[k]);

        printf("\n");

        // The worst squadron's stores against its table: weapon types it holds that the table does not
        // stock (never drawn down by SupplyUnit), and types above their table amount.
        if (worstId[0] & 0x80000000u)
        {
            Unit w = NULL;
            VuListIterator wit(AllUnitList);

            for (Unit u = GetFirstUnit(&wit); u; u = GetNextUnit(&wit))
                if (u->IsSquadron() && (unsigned)u->Id().num_ == (worstId[0] & 0x7fffffffu))
                    w = u;

            UnitClassDataType *uc = w ? w->GetUnitClassData() : NULL;

            if (uc)
            {
                int offTable = 0, offSum = 0, over = 0, overSum = 0, at255 = 0;

                for (int i = 0; i < MAXIMUM_WEAPTYPES; i++)
                {
                    int want = SquadronStoresDataTable[uc->SpecialIndex].Stores[i];
                    int got = ((SquadronClass *)w)->GetUnitStores(i);
                    at255 += got == 255;

                    if (!want && got)
                        offTable++, offSum += got;
                    else if (got > want)
                        over++, overSum += got - want;
                }

                printf("SUPNEED   sq %u (stores table %d): %d weapon types held but not in its table (%d shots), "
                       "%d above table (+%d), %d types at the 255 cap\n",
                       worstId[0] & 0x7fffffffu, (int)uc->SpecialIndex, offTable, offSum, over, overSum, at255);
            }
        }
    }

    // Who gets the supply and do squadrons use it, once a day: supply received by battalions vs squadrons;
    // squadron stores loaded onto sorties and returned unused (in supply points, 20 stores units each);
    // how full squadron stores are against their own tables.
    if (hour % 24 == 0)
    {
        static int prevSplit[NUM_TEAMS][2], prevFlow[NUM_TEAMS][2];

        for (int t : {2, 6})
        {
            int got[2], flow[2];

            for (int k = 0; k < 2; k++)
            {
                got[k] = gSupplySplit[t][k] - prevSplit[t][k], prevSplit[t][k] = gSupplySplit[t][k];
                flow[k] = gStoresFlow[t][k] - prevFlow[t][k], prevFlow[t][k] = gStoresFlow[t][k];
            }

            long want = 0, have = 0;
            int sq = 0, full90 = 0, under50 = 0;
            VuListIterator sit(AllUnitList);
            std::set<unsigned> seenSq;

            for (Unit u = GetFirstUnit(&sit); u; u = GetNextUnit(&sit))
            {
                if (u->GetTeam() != t || !u->IsSquadron() || !seenSq.insert((unsigned)u->Id().num_).second)
                    continue;

                UnitClassDataType *uc = u->GetUnitClassData();

                if (!uc)
                    continue;

                long w = 0, h = 0;

                for (int i = 0; i < MAXIMUM_WEAPTYPES; i++)
                {
                    int tw = SquadronStoresDataTable[uc->SpecialIndex].Stores[i];

                    if (tw)
                        w += tw, h += min((int)((SquadronClass *)u)->GetUnitStores(i), tw);
                }

                if (!w)
                    continue;

                sq++, want += w, have += h;
                full90 += h * 10 >= w * 9;
                under50 += h * 2 < w;
            }

            printf("SUPSPLIT h=%d team %d (24 h): supply received -- battalions %d, squadrons %d (ground share %d%%) | "
                   "squadron stores loaded onto sorties %d pts, returned unused %d pts | %d squadrons, stores %.0f%% "
                   "full, %d at 90%%+, %d under 50%%\n", hour, t, got[0], got[1], gSupplyShareG[t],
                   flow[0] / SQUADRON_PT_SUPPLY, flow[1] / SQUADRON_PT_SUPPLY, sq, want ? 100.0 * have / want : 0.0,
                   full90, under50);
        }
    }

    // Where battalion supply goes (battalio.cpp / unit.cpp gSupplyUse), summed supply-% points per 6 h
    {
        static int prevUse[8][4];

        for (int t : {2, 6})
        {
            int d[4];

            for (int k = 0; k < 4; k++)
                d[k] = gSupplyUse[t][k] - prevUse[t][k], prevUse[t][k] = gSupplyUse[t][k];

            printf("SUPUSE h=%d team %d: battalion supply %% points used -- moving %d, firing at aircraft %d, "
                   "firing at ground %d, waiting to move %d (last 6 h)\n", hour, t, d[0], d[1], d[2], d[3]);
        }
    }

    // Scrambles (atm.cpp gScramble): intercept requests, flights diverted to them, how many of those were ALERT
    // flights waiting on the ground, and requests that found no flight -- per team, last 6 h
    {
        extern int gScramble[8][4];
        static int prevScr[8][4];

        for (int t : {1, 2, 6})
        {
            int d[4];

            for (int k = 0; k < 4; k++)
                d[k] = gScramble[t][k] - prevScr[t][k], prevScr[t][k] = gScramble[t][k];

            printf("SCRAMBLE h=%d team %d: intercept requests %d, diverted %d (from ALERT %d), no flight %d (last 6 h)\n",
                   hour, t, d[0], d[1], d[2], d[3]);
        }

        // ALERT flights rejected for too few aircraft: how many they had vs how many the request wanted
        extern int gAlertVeh[8][2][8];

        for (int t : {2, 6})
        {
            int *a = gAlertVeh[t][0], *b = gAlertVeh[t][1];
            printf("SCRAMBLE h=%d team %d too-few: alert flight planes 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7+:%d | "
                   "request wants 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7+:%d (cumulative)\n", hour, t, a[0], a[1], a[2],
                   a[3], a[4], a[5], a[6], a[7], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        }

        // why packages fail (package.cpp gPkgWhy), cumulative
        extern int gPkgWhy[8][4][5];
        static const char *cls[] = {"strike", "cas/bai", "counter-air", "anti-ship"};

        for (int t : {2, 6})
            for (int c = 0; c < 4; c++)
            {
                int *w = gPkgWhy[t][c];
                printf("ATMWHY h=%d team %d %-11s too dangerous %d, no flight %d, could not reach %d, cancelled %d, "
                       "built %d (cumulative)\n", hour, t, cls[c], w[0], w[1], w[2], w[3], w[4]);
            }

        // requests no squadron could fill, by the furthest check a squadron of the right role passed (atm.cpp)
        extern int gNoFlightWhy[8][4][11];

        for (int t : {2, 6})
            for (int c = 0; c < 2; c++)
            {
                int *w = gNoFlightWhy[t][c];
                printf("NOFLIGHT h=%d team %d %-7s no squadron with the role %d, relocating %d, capabilities %d, "
                       "range %d, speed %d, schedule %d, no aircraft free %d, airbase full %d, passed all %d "
                       "(cumulative)\n", hour, t, cls[c], w[0], w[1], w[2], w[5], w[6], w[7], w[8], w[9], w[10]);
            }

        // initiative inputs (team.cpp gInitDiag), last calculation
        extern int gInitDiag[8][12];

        for (int t = 0; t < 8; t++)
        {
            int *g = gInitDiag[t];

            if (g[0] || g[1])
                printf("INIT h=%d team %d vs %d: ground %d vs %d, aircraft %d vs %d, ground losses %d vs %d | terms "
                       "ground %d air %d losses %d -> long run %d, initiative %d\n", hour, t, g[11], g[0], g[1], g[2],
                       g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10]);
        }

        // why ALERT flights were passed over (atm.cpp gAlertWhy), cumulative
        extern int gAlertWhy[8][9];

        for (int t : {2, 6})
        {
            int *w = gAlertWhy[t];
            printf("SCRAMBLE h=%d team %d ALERT flights passed over: priority %d, aborted/diverted %d, caps %d, "
                   "aircraft/priority %d (too few aircraft %d), >250 km %d, busy %d, outscored %d | chosen %d "
                   "(cumulative)\n",
                   hour, t, w[1], w[2], w[3], w[4], w[0], w[5], w[6], w[7], w[8]);
        }
    }

    // Who is short of supply, once a day: ROK battalions under 50%, by distance to the front, whether in
    // combat, moving or airmobile, and how long since SupplyUnits last served them against their own
    // resupply interval (a unit is only resupplied once that interval has passed).
    if (hour % 24 == 0)
    {
        int n = 0, low = 0, near10 = 0, near30 = 0, beyond30 = 0, engaged = 0, moving = 0, airmob = 0, overdue = 0;
        long sinceSum = 0, intervalSum = 0;
        VuListIterator lit(AllUnitList);
        std::set<unsigned> seenLow;

        for (Unit u = GetFirstUnit(&lit); u; u = GetNextUnit(&lit))
        {
            if (u->GetTeam() != 2 || !u->IsBattalion() || !seenLow.insert((unsigned)u->Id().num_).second)
                continue;

            n++;

            if (u->GetUnitSupply() >= 50)
                continue;

            low++;
            GridIndex x, y;
            u->GetLocation(&x, &y);
            float df = DistanceToFront(x, y);
            (df <= 10 ? near10 : df <= 30 ? near30 : beyond30)++;
            engaged += u->Engaged() ? 1 : 0;
            moving += u->Moving() ? 1 : 0;
            airmob += u->GetSType() == STYPE_UNIT_AIRMOBILE || u->GetSType() == STYPE_UNIT_INFANTRY;
            long since = (long)((TheCampaign.CurrentTime - u->GetLastResupplyTime()) / CampaignMinutes);
            long interval = (long)(u->GetUnitSupplyTime() / CampaignMinutes);
            sinceSum += since, intervalSum += interval;
            overdue += since > 2 * interval;
        }

        printf("LOWSUP h=%d ROK: %d of %d battalions under 50%% | front distance <=10 km %d, 10-30 km %d, >30 km %d | "
               "engaged %d, moving %d, infantry/airmobile %d | last resupplied %ld min ago on average, interval %ld min, "
               "more than 2 intervals overdue %d\n",
               hour, low, n, near10, near30, beyond30, engaged, moving, airmob, low ? sinceSum / low : 0,
               low ? intervalSum / low : 0, overdue);
    }

    // AllUnitList should hold each unit once. Every system that walks it (supply needs, statistics, the
    // GTM) double-counts duplicates. Once a day: entries vs unique units per team, and what the duplicated
    // ones are (infantry / carried as cargo / flagged inactive / how many copies).
    if (hour % 24 == 0)
    {
        std::map<unsigned, int> copies;
        std::map<unsigned, Unit> byId;
        VuListIterator dit(AllUnitList);

        for (Unit u = GetFirstUnit(&dit); u; u = GetNextUnit(&dit))
            copies[(unsigned)u->Id().num_]++, byId[(unsigned)u->Id().num_] = u;

        for (int t : {2, 6})
        {
            int entries = 0, unique = 0, dupUnits = 0, inf = 0, cargo = 0, inactive = 0, maxc = 0;
            char ex[256];
            int en = 0;
            ex[0] = 0;

            for (std::map<unsigned, int>::iterator it = copies.begin(); it != copies.end(); ++it)
            {
                Unit u = byId[it->first];

                if (u->GetTeam() != t)
                    continue;

                entries += it->second, unique++;

                if (it->second > 1)
                {
                    dupUnits++;
                    inf += u->GetSType() == STYPE_UNIT_INFANTRY;
                    cargo += u->Cargo() ? 1 : 0;
                    inactive += u->Inactive() ? 1 : 0;
                    maxc = it->second > maxc ? it->second : maxc;

                    if (en < 200)
                        en += sprintf(ex + en, " %u%s x%d", it->first, u->IsBattalion() ? "bn" : u->IsBrigade() ? "bde" : "",
                                      it->second);
                }
            }

            printf("UNITLIST h=%d team %d: %d entries, %d unique units, %d duplicated (infantry %d, cargo %d, "
                   "inactive %d, most copies %d) |%s\n", hour, t, entries, unique, dupUnits, inf, cargo, inactive, maxc, ex);
        }
    }

    // Every DPRK-held objective within 40 km of Pyongyang, once a day: can ROK be ordered to take it
    // (IsValidObjective(CAPTURE) = secondary, near the front, capture allowed) and who is attacking it.
    if (hour % 24 == 0)
    {
        std::map<int, int> attackers;
        VuListIterator uit(AllUnitList);

        for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
            if (u->GetTeam() == 2 && u->IsBattalion() && u->GetUnitOrders() == GORD_CAPTURE && u->GetUnitObjective())
                attackers[u->GetUnitObjective()->GetCampID()]++;

        VuListIterator oit(AllObjList);
        int nNear = 0, valid = 0, sec = 0, front = 0, attacked = 0;
        char list[2048];
        int ln = 0;

        list[0] = 0;

        for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
        {
            GridIndex x, y;
            o->GetLocation(&x, &y);
            float dd = sqrtf((float)((x - 301) * (x - 301) + (y - 580) * (y - 580)));

            if (o->GetTeam() != 6 || dd > 40.0F)
                continue;

            nNear++;
            int s = o->IsSecondary() ? 1 : 0, f = o->IsNearfront() ? 1 : 0;
            int a = attackers.count(o->GetCampID()) ? attackers[o->GetCampID()] : 0;
            sec += s;
            front += f;
            valid += s && f && GetRoE(2, 6, ROE_GROUND_CAPTURE) == ROE_ALLOWED;
            attacked += a > 0;

            if (s && ln < 1900)
                ln += sprintf(list + ln, " #%d@%.0fkm%s%s", o->GetCampID(), dd, f ? "/front" : "", a ? "/ATTACKED" : "");
        }

        printf("PYOBJ h=%d DPRK objectives within 40 km of Pyongyang: %d, secondary %d, near front %d, valid capture "
               "targets %d, under ROK attack %d | secondary:%s\n",
               hour, nNear, sec, front, valid, attacked, list);

        // From each Pyongyang city objective, the shortest route over the objective link network to an
        // ROK-held objective: link count and every objective on it (id, type, owner, S = secondary,
        // F/2/3 = front/second/third line). Capture orders only reach secondaries within three links of
        // the front; the objectives in between are taken only by units passing through.
        for (int target : {260, 680})
        {
            Objective start = (Objective)GetEntityByCampID(target);

            if (!start || start->GetTeam() == 2)
                continue;

            std::map<Objective, Objective> prevOf;
            std::vector<Objective> q;
            Objective found = NULL;
            prevOf[start] = NULL;
            q.push_back(start);

            for (size_t qi = 0; qi < q.size() && !found; qi++)
            {
                Objective c = q[qi];

                for (int l = 0; l < c->NumLinks(); l++)
                {
                    Objective nb = c->GetNeighbor(l);

                    if (!nb || prevOf.count(nb))
                        continue;

                    prevOf[nb] = c;

                    if (nb->GetTeam() == 2)
                    {
                        found = nb;
                        break;
                    }

                    q.push_back(nb);
                }
            }

            char route[1024];
            int rn = 0, hops = 0;
            route[0] = 0;

            for (Objective c = found; c && rn < 960; c = prevOf[c], hops++)
            {
                GridIndex x, y;
                c->GetLocation(&x, &y);
                rn += sprintf(route + rn, " #%d(t%d %s%s%s %d,%d)", c->GetCampID(), c->GetType(),
                              c->GetTeam() == 2 ? "ROK" : "DPRK", c->IsSecondary() ? " S" : "",
                              c->IsFrontline() ? " F" : c->IsSecondline() ? " 2" : c->IsThirdline() ? " 3" : "", x, y);
            }

            printf("PYPATH h=%d #%d -> nearest ROK objective: %d links:%s\n", hour, target, found ? hops - 1 : -1,
                   route);
        }
    }

    fflush(stdout);
}

// CAPTURE order life cycle (battalio.cpp gOrderChangeHook), ROK and DPRK battalions: who ends a capture
// order (caller file:line and the new order), whether the objective had been taken by then, how long the
// order lasted; plus a snapshot of the units holding one now (moving / stuck / low supply / broken).
extern "C++" void (*gOrderChangeHook)(BattalionClass *u, int oldOrders, int newOrders, VU_ID oid, void *caller);

struct CapEnd
{
    int team, newOrders, taken;
    void *caller;
    bool operator<(const CapEnd &o) const
    {
        if (team != o.team) return team < o.team;
        if (newOrders != o.newOrders) return newOrders < o.newOrders;
        if (taken != o.taken) return taken < o.taken;
        return caller < o.caller;
    }
};
static std::map<CapEnd, int> gCapEnds;
static std::map<unsigned, CampaignTime> gCapStart;
static int gCapAssigned[NUM_TEAMS], gCapLife[NUM_TEAMS][4]; // ended after <1 h, 1-6 h, 6-24 h, >24 h
static std::map<unsigned, std::pair<int, int>> gCapPos;    // last snapshot position of capture holders

static const char *CallerName(void *addr);

// Order churn: every order or objective change of an ROK/DPRK battalion, by (team, old order, new order,
// caller), and how far the new objective is from the old one. Printed daily as CHURN.
struct ChurnKey
{
    int team, oldOrders, newOrders;
    void *caller;
    bool operator<(const ChurnKey &o) const
    {
        if (team != o.team) return team < o.team;
        if (oldOrders != o.oldOrders) return oldOrders < o.oldOrders;
        if (newOrders != o.newOrders) return newOrders < o.newOrders;
        return caller < o.caller;
    }
};
struct ChurnVal
{
    int n;
    double km;
};
static std::map<ChurnKey, ChurnVal> gChurn;
static int gChurnSame[NUM_TEAMS]; // calls that changed nothing (same order, same objective)

// RESERVE -> RESERVE re-assignments from BattalionClass::MoveUnit (its objective failed IsValidObjective, so it
// fell back to FindRetreatPath): why the old objective failed, and whether the new one passes the same test.
// [0] old none, [1] old not secondary, [2] old not ours, [3] old near the front, [4] old valid(?),
// [5] new one valid for RESERVE, [6] new one invalid
static int gResWhy[NUM_TEAMS][7];

static int ReserveFailReason(int t, Objective o)
{
    if (!o)
        return 0;

    if (!o->IsSecondary())
        return 1;

    if (o->GetTeam() != t)
        return 2;

    if (o->IsNearfront())
        return 3;

    return 4;
}

static void CapOrderHook(BattalionClass *u, int oldOrders, int newOrders, VU_ID oid, void *caller)
{
    int t = u->GetTeam();

    if (t != 2 && t != 6)
        return;

    unsigned id = u->Id().num_;

    {
        Objective oldObj = u->GetUnitObjective(), newObj = (Objective)vuDatabase->Find(oid);

        if (oldOrders == GORD_RESERVE && newOrders == GORD_RESERVE && oldObj != newObj &&
            strstr(CallerName(caller), "MoveUnit"))
        {
            gResWhy[t][ReserveFailReason(t, oldObj)]++;
            gResWhy[t][TeamInfo[t]->gtm->IsValidObjective(GORD_RESERVE, newObj) ? 5 : 6]++;
        }

        if (oldOrders == newOrders && oldObj == newObj)
            gChurnSame[t]++;
        else
        {
            ChurnKey k = {t, oldOrders, newOrders, caller};
            ChurnVal &v = gChurn[k];
            v.n++;

            if (oldObj && newObj)
            {
                GridIndex ax, ay, bx, by;
                oldObj->GetLocation(&ax, &ay);
                newObj->GetLocation(&bx, &by);
                v.km += Distance(ax, ay, bx, by);
            }
        }
    }

    if (oldOrders == GORD_CAPTURE)
    {
        // taken: 1 objective is ours now, 2 still a valid capture target (dropped anyway), 0 no longer valid
        Objective o = u->GetUnitObjective();
        int taken = !o ? 0 : o->GetTeam() == t ? 1
                    : TeamInfo[t]->gtm->IsValidObjective(GORD_CAPTURE, o) ? 2 : 0;
        CapEnd k = {t, newOrders, taken, caller};
        gCapEnds[k]++;
        std::map<unsigned, CampaignTime>::iterator it = gCapStart.find(id);

        if (it != gCapStart.end())
        {
            CampaignTime h = (TheCampaign.CurrentTime - it->second) / CampaignHours;
            gCapLife[t][h < 1 ? 0 : h < 6 ? 1 : h < 24 ? 2 : 3]++;
            gCapStart.erase(it);
        }
    }

    if (newOrders == GORD_CAPTURE)
    {
        gCapAssigned[t]++;
        gCapStart[id] = TheCampaign.CurrentTime;
    }
}

static const char *CallerName(void *addr)
{
    static std::map<void *, std::string> cache;
    std::map<void *, std::string>::iterator it = cache.find(addr);

    if (it != cache.end())
        return it->second.c_str();

    HANDLE proc = GetCurrentProcess();
    char symBuf[sizeof(IMAGEHLP_SYMBOL64) + 256];
    IMAGEHLP_SYMBOL64 *sym = (IMAGEHLP_SYMBOL64 *)symBuf;
    sym->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
    sym->MaxNameLength = 255;
    DWORD64 d64 = 0;
    DWORD d32 = 0;
    IMAGEHLP_LINE64 ln = {sizeof(IMAGEHLP_LINE64)};
    char out[512];
    const char *name = SymGetSymFromAddr64(proc, (DWORD64)addr - 1, &d64, sym) ? sym->Name : "?";

    if (SymGetLineFromAddr64(proc, (DWORD64)addr - 1, &d32, &ln))
    {
        const char *f = strrchr(ln.FileName, '\\');
        sprintf(out, "%s %s:%lu", name, f ? f + 1 : ln.FileName, ln.LineNumber);
    }
    else
        sprintf(out, "%s", name);

    return (cache[addr] = out).c_str();
}

static void ChurnLog(int hour)
{
    static const char *ord[] = {"RES", "CAP", "SEC", "ASL", "ABN", "CMD", "DEF", "SUP", "REP", "AD", "RCN", "RAD"};

    for (int t : {2, 6})
    {
        int total = 0, bns = 0;
        std::vector<std::pair<int, ChurnKey>> v;

        for (std::map<ChurnKey, ChurnVal>::iterator it = gChurn.begin(); it != gChurn.end(); ++it)
            if (it->first.team == t)
                total += it->second.n, v.push_back(std::make_pair(it->second.n, it->first));

        VuListIterator uit(AllUnitList);
        std::set<unsigned> seen;

        for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
            if (u->GetTeam() == t && u->IsBattalion() && seen.insert((unsigned)u->Id().num_).second)
                bns++;

        std::sort(v.begin(), v.end(), [](const std::pair<int, ChurnKey> &a, const std::pair<int, ChurnKey> &b) {
            return a.first > b.first;
        });
        printf("CHURN h=%d team %d: %d order/objective changes in 24 h (%.1f per battalion), %d calls that changed "
               "nothing\n", hour, t, total, bns ? (double)total / bns : 0.0, gChurnSame[t]);

        for (size_t i = 0; i < v.size() && i < 8; i++)
        {
            const ChurnVal &c = gChurn[v[i].second];
            printf("CHURN   %5d  %s -> %s by %s, new objective %.0f km from the old on average\n", c.n,
                   v[i].second.oldOrders >= 0 && v[i].second.oldOrders < 12 ? ord[v[i].second.oldOrders] : "?",
                   v[i].second.newOrders >= 0 && v[i].second.newOrders < 12 ? ord[v[i].second.newOrders] : "?",
                   CallerName(v[i].second.caller), c.n ? c.km / c.n : 0.0);
        }

        int *r = gResWhy[t];
        printf("CHURN   reserve fall-backs in MoveUnit: old objective none %d, not secondary %d, not ours %d, near the "
               "front %d, valid %d | the retreat objective is valid for RESERVE %d, invalid %d\n",
               r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
        memset(gResWhy[t], 0, sizeof(gResWhy[t]));
        gChurnSame[t] = 0;
    }

    for (std::map<ChurnKey, ChurnVal>::iterator it = gChurn.begin(); it != gChurn.end(); ++it)
        it->second.n = 0, it->second.km = 0;
}

static void CapLog(int hour)
{
    static const char *ord[] = {"RES", "CAP", "SEC", "ASL", "ABN", "CMD", "DEF", "SUP", "REP", "AD", "RCN", "RAD"};

    if (hour % 24 == 0)
        ChurnLog(hour);
    int hold[NUM_TEAMS] = {0}, moving[NUM_TEAMS] = {0}, stuck[NUM_TEAMS] = {0}, lowsup[NUM_TEAMS] = {0},
        broken[NUM_TEAMS] = {0}, bns[NUM_TEAMS] = {0}, bnLow[NUM_TEAMS] = {0}, bnBroken[NUM_TEAMS] = {0};
    std::map<unsigned, std::pair<int, int>> pos;
    VuListIterator uit(AllUnitList);

    for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
    {
        int t = u->GetTeam();

        if ((t != 2 && t != 6) || !u->IsBattalion())
            continue;

        bns[t]++;
        bnLow[t] += u->GetUnitSupply() < 50;
        bnBroken[t] += u->Broken() ? 1 : 0;

        if (u->GetUnitOrders() != GORD_CAPTURE)
            continue;

        GridIndex x, y;
        u->GetLocation(&x, &y);
        unsigned id = u->Id().num_;
        pos[id] = std::make_pair((int)x, (int)y);
        hold[t]++;
        lowsup[t] += u->GetUnitSupply() < 50;
        broken[t] += u->Broken() ? 1 : 0;

        if (u->Moving())
        {
            moving[t]++;
            std::map<unsigned, std::pair<int, int>>::iterator p = gCapPos.find(id);

            if (p != gCapPos.end() && abs(p->second.first - x) + abs(p->second.second - y) <= 1)
                stuck[t]++;
        }
    }

    gCapPos.swap(pos);

    for (int t : {2, 6})
    {
        int ended = 0, taken = 0;

        for (std::map<CapEnd, int>::iterator it = gCapEnds.begin(); it != gCapEnds.end(); ++it)
            if (it->first.team == t)
                ended += it->second, taken += it->first.taken == 1 ? it->second : 0;

        printf("CAPORD h=%d team %d: battalions %d (supply<50 %d, broken %d) | CAPTURE held now %d: moving %d, "
               "stuck since last log %d, supply<50 %d, broken %d | since start assigned %d, ended %d (objective "
               "taken %d), lasted <1h %d 1-6h %d 6-24h %d >24h %d\n",
               hour, t, bns[t], bnLow[t], bnBroken[t], hold[t], moving[t], stuck[t], lowsup[t], broken[t],
               gCapAssigned[t], ended, taken, gCapLife[t][0], gCapLife[t][1], gCapLife[t][2], gCapLife[t][3]);

        // the eight biggest ways a capture order ends
        std::vector<std::pair<int, CapEnd>> v;

        for (std::map<CapEnd, int>::iterator it = gCapEnds.begin(); it != gCapEnds.end(); ++it)
            if (it->first.team == t)
                v.push_back(std::make_pair(it->second, it->first));

        std::sort(v.begin(), v.end(), [](const std::pair<int, CapEnd> &a, const std::pair<int, CapEnd> &b)
                  { return a.first > b.first; });

        for (size_t i = 0; i < v.size() && i < 8; i++)
            printf("CAPORD   %5d -> %s %s by %s\n", v[i].first,
                   v[i].second.newOrders >= 0 && v[i].second.newOrders < 12 ? ord[v[i].second.newOrders] : "?",
                   v[i].second.taken == 1 ? "(objective taken)"
                   : v[i].second.taken == 2 ? "(objective STILL VALID)" : "(objective no longer valid)",
                   CallerName(v[i].second.caller));
    }
}

// Why battalions that want to move do not (battalio.cpp gMoveDiag / gMoveFailHook). Every 6 h: per team
// the count of path failures, failed waypoint builds, brigade-column waits and holds; then the units that
// failed most often, with what lies on the straight line to where they wanted to go: water cells
// (rivers) and the bridges within 3 km of it (status, owner).
extern "C++" int gMoveDiag[NUM_TEAMS][4];
extern "C++" int gMovePartial[8];
extern "C++" int gMoveObjPartial[8];
extern "C++" int gWPFail[8][2];
extern "C++" int gWPFailLeg[3];
extern "C++" void (*gMoveFailHook)(BattalionClass *u, int why, GridIndex x, GridIndex y, GridIndex nx, GridIndex ny);

struct MoveFail
{
    int count, why, team;
    GridIndex x, y, nx, ny;
    GridIndex lx, ly; // "no waypoints" from the first leg: where that first waypoint was
    int lobj;
};
static std::map<unsigned, MoveFail> gMoveFails;

static void MoveFailHook(BattalionClass *u, int why, GridIndex x, GridIndex y, GridIndex nx, GridIndex ny)
{
    MoveFail &f = gMoveFails[u->Id().num_];
    f.count++, f.why = why, f.team = u->GetTeam();
    f.x = x, f.y = y, f.nx = nx, f.ny = ny;
    if (why == 1)
        f.lx = gWPFailLeg[0], f.ly = gWPFailLeg[1], f.lobj = gWPFailLeg[2];
}

static void MoveLog(int hour)
{
    static int prev[NUM_TEAMS][4];

    for (int t : {2, 6})
    {
        int d[4];

        for (int k = 0; k < 4; k++)
            d[k] = gMoveDiag[t][k] - prev[t][k], prev[t][k] = gMoveDiag[t][k];

        int units = 0;

        for (std::map<unsigned, MoveFail>::iterator it = gMoveFails.begin(); it != gMoveFails.end(); ++it)
            units += it->second.team == t;

        static int prevPartial[NUM_TEAMS], prevObjPartial[NUM_TEAMS];
        int partial = gMovePartial[t % 8] - prevPartial[t];
        int objPartial = gMoveObjPartial[t % 8] - prevObjPartial[t];
        prevPartial[t] = gMovePartial[t % 8];
        prevObjPartial[t] = gMoveObjPartial[t % 8];

        static int prevWP[NUM_TEAMS][2];
        int wp0 = gWPFail[t % 8][0] - prevWP[t][0], wp1 = gWPFail[t % 8][1] - prevWP[t][1];
        prevWP[t][0] = gWPFail[t % 8][0];
        prevWP[t][1] = gWPFail[t % 8][1];

        printf("MOVE h=%d team %d: no grid path %d, no waypoints %d (objective route %d, first leg %d), column wait %d, "
               "hold %d, partial path taken %d, partial objective route taken %d (last 6 h); %d battalions with path "
               "failures\n", hour, t, d[0], d[1], wp0, wp1, d[2], d[3], partial, objPartial, units);

        std::vector<std::pair<int, unsigned>> v;

        for (std::map<unsigned, MoveFail>::iterator it = gMoveFails.begin(); it != gMoveFails.end(); ++it)
            if (it->second.team == t)
                v.push_back(std::make_pair(it->second.count, it->first));

        std::sort(v.rbegin(), v.rend());

        for (size_t i = 0; i < v.size() && i < 6; i++)
        {
            const MoveFail &f = gMoveFails[v[i].second];
            int water = 0, cells = 0;
            int len = (int)Distance(f.x, f.y, f.nx, f.ny);

            for (int s = 0; s <= len; s++)
            {
                GridIndex cx = (GridIndex)(f.x + (f.nx - f.x) * (len ? (float)s / len : 0) + 0.5f);
                GridIndex cy = (GridIndex)(f.y + (f.ny - f.y) * (len ? (float)s / len : 0) + 0.5f);
                cells++;
                water += GetCover(cx, cy) == Water;
            }

            char bridges[256] = "";
            int nb = 0, n = 0;
            VuListIterator oit(AllObjList);

            for (Objective o = GetFirstObjective(&oit); o && nb < 4; o = GetNextObjective(&oit))
            {
                if (o->GetType() != TYPE_BRIDGE)
                    continue;

                GridIndex bx, by;
                o->GetLocation(&bx, &by);
                // distance from the bridge to the segment
                float vx = (float)(f.nx - f.x), vy = (float)(f.ny - f.y), wx = (float)(bx - f.x), wy = (float)(by - f.y);
                float l2 = vx * vx + vy * vy, tt = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
                tt = tt < 0 ? 0 : tt > 1 ? 1 : tt;
                float ddx = wx - tt * vx, ddy = wy - tt * vy;

                if (ddx * ddx + ddy * ddy <= 9.0f)
                    n += sprintf(bridges + n, " #%d %d%% team%d", (int)o->GetCampID(), (int)o->GetObjectiveStatus(),
                                 (int)o->GetTeam()),
                        nb++;
            }

            printf("MOVE   bn %u: %d failures (%s) at (%d,%d) -> (%d,%d) %d km: water cells %d/%d, bridges:%s\n",
                   v[i].second, f.count, f.why ? "no waypoints" : "no grid path", f.x, f.y, f.nx, f.ny, len, water,
                   cells, nb ? bridges : " none within 3 km");

            if (f.why == 1 && f.lobj)
            {
                Objective lo = (Objective)GetEntityByCampID(f.lobj);
                printf("MOVE     first leg to #%d (type %d, team %d) at (%d,%d), %.0f km from the unit; cell cost here "
                       "%.1f there %.1f (road ok %.1f)\n",
                       f.lobj, lo ? (int)lo->GetType() : -1, lo ? (int)lo->GetTeam() : -1, f.lx, f.ly,
                       Distance(f.x, f.y, f.lx, f.ly), GetMovementCost(f.x, f.y, Tracked, 0, Here),
                       GetMovementCost(f.lx, f.ly, Tracked, 0, Here),
                       GetMovementCost(f.lx, f.ly, Tracked, PATH_ROADOK, Here));
            }
        }
    }

    gMoveFails.clear();
}

// ---------------------------------------------------------------------------
// China reinforcement wave (prototype). prcwave=MIN:MAX -- when event 11 (China joins) fires, China's
// ground units arrive as a wave: each brigade (with its battalions) and each independent battalion is
// moved to a DPRK-held secondary objective MIN-MAX km behind the front, spread west to east, instead
// of starting its war 300+ km away on the Yalu. prcoff=H -- then put DPRK on the OFFENSIVE for H hours
// (re-asserted every hour, since Blue's own offensives force DPRK back onto the defensive), aimed at the
// Blue-held primary objective nearest the wave.
static int gPrcWaveMin, gPrcWaveMax, gPrcOffHours, gPrcWaveDone;
static CampaignTime gPrcOffUntil;
static VU_ID gPrcOffObjective;
static std::vector<VU_ID> gPrcGroups; // brigades and independent battalions of team 5 at load

static void PrcRecord()
{
    VuListIterator uit(AllUnitList);

    for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
        if (u->GetTeam() == 5 && u->GetDomain() == DOMAIN_LAND &&
            (u->IsBrigade() || (u->IsBattalion() && !u->GetUnitParent())))
            gPrcGroups.push_back(u->Id());

    printf("PRCWAVE recorded %d Chinese brigades / independent battalions\n", (int)gPrcGroups.size());
}

static void PrcMove(Unit u, Objective o, int jitter)
{
    GridIndex ox, oy;
    o->GetLocation(&ox, &oy);
    u->DisposeWayPoints();
    u->SetLocation(ox + (jitter % 3) - 1, oy + ((jitter / 3) % 3) - 1);
    u->SetUnitDestination(ox, oy);
}

static void PrcWave(int hour)
{
    std::vector<Objective> cand;
    VuListIterator oit(AllObjList);

    for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
    {
        if (o->GetTeam() != 6 || !o->IsSecondary())
            continue;

        GridIndex x, y;
        o->GetLocation(&x, &y);
        float d = DistanceToFront(x, y);

        if (d >= gPrcWaveMin && d <= gPrcWaveMax)
            cand.push_back(o);
    }

    if (cand.empty())
    {
        printf("PRCWAVE h=%d: no DPRK objective %d-%d km behind the front\n", hour, gPrcWaveMin, gPrcWaveMax);
        return;
    }

    std::sort(cand.begin(), cand.end(), [](Objective a, Objective b) {
        GridIndex ax, ay, bx, by;
        a->GetLocation(&ax, &ay);
        b->GetLocation(&bx, &by);
        return ax < bx;
    });

    int groups = 0, bns = 0, veh = 0;
    long sx = 0, sy = 0;
    float dsum = 0;
    const int n = (int)gPrcGroups.size();

    for (int i = 0; i < n; i++)
    {
        Unit g = (Unit)vuDatabase->Find(gPrcGroups[i]);

        if (!g || g->IsDead())
            continue;

        Objective o = cand[(size_t)i * cand.size() / n];
        GroundTaskingManagerClass *gtm = TeamInfo[g->GetTeam()]->gtm;
        int orders = gtm->IsValidObjective(GORD_DEFEND, o) ? GORD_DEFEND : GORD_RESERVE;
        PrcMove(g, o, 0);
        int j = 1;

        for (Unit e = g->IsBrigade() ? g->GetFirstUnitElement() : NULL; e; e = g->GetNextUnitElement())
        {
            PrcMove(e, o, j++);
            bns++;
            veh += e->GetTotalVehicles();
        }

        if (g->IsBattalion())
            bns++, veh += g->GetTotalVehicles();

        g->SetUnitOrders(orders, o->Id());
        GridIndex x, y;
        o->GetLocation(&x, &y);
        sx += x, sy += y, dsum += DistanceToFront(x, y);
        groups++;
    }

    printf("PRCWAVE h=%d: moved %d groups (%d battalions, %d vehicles) to %d objectives %d-%d km behind the front, "
           "mean %.0f km, centre (%ld,%ld)\n",
           hour, groups, bns, veh, (int)cand.size(), gPrcWaveMin, gPrcWaveMax, groups ? dsum / groups : 0.0f,
           groups ? sx / groups : 0, groups ? sy / groups : 0);

    if (gPrcOffHours > 0 && groups)
    {
        // aim at the Blue-held primary objective nearest the wave's centre
        GridIndex cx = (GridIndex)(sx / groups), cy = (GridIndex)(sy / groups);
        Objective best = NULL;
        float bd = 1e9f;
        VuListIterator pit(POList);

        for (Objective p = GetFirstObjective(&pit); p; p = GetNextObjective(&pit))
        {
            if (GetRoE(6, p->GetTeam(), ROE_GROUND_CAPTURE) != ROE_ALLOWED)
                continue;

            GridIndex x, y;
            p->GetLocation(&x, &y);
            float d = Distance(cx, cy, x, y);

            if (d < bd)
                bd = d, best = p;
        }

        if (best)
        {
            gPrcOffObjective = best->Id();
            gPrcOffUntil = TheCampaign.CurrentTime + gPrcOffHours * CampaignHours;
            printf("PRCOFF h=%d: DPRK on the offensive for %d h against PO %d (%.0f km from the wave)\n", hour,
                   gPrcOffHours, (int)best->GetCampID(), bd);
        }
    }
}

static void PrcTick(int iter)
{
    if (gPrcWaveMax > 0 && !gPrcWaveDone && CampEvents && 11 < CE_Events && CampEvents[11] &&
        CampEvents[11]->HasFired())
    {
        gPrcWaveDone = 1;
        PrcWave(iter / 60);
    }

    // every tick: Blue's own offensives push DPRK back to DEFENSIVE, and a GTM cycle that cannot staff
    // half its capture objectives zeroes the action's points (consolidate)
    if (gPrcOffUntil && TheCampaign.CurrentTime < gPrcOffUntil)
    {
        TeamClass *t = TeamInfo[6];
        Objective o = (Objective)vuDatabase->Find(gPrcOffObjective);

        if (t && o && GetRoE(6, o->GetTeam(), ROE_GROUND_CAPTURE) == ROE_ALLOWED &&
            t->GetGroundActionType() != GACTION_OFFENSIVE)
        {
            TeamGndActionType a = *t->GetGroundAction();
            a.actionType = GACTION_OFFENSIVE;
            a.actionObjective = o->Id();
            a.actionTime = TheCampaign.CurrentTime;
            a.actionTimeout = gPrcOffUntil;
            a.actionTempo = 50;
            a.actionPoints = 80;
            t->SetGroundAction(&a);

            if (t->GetInitiative() < 60)
                t->SetInitiative(60);
        }
    }
}

// Squadron rebasing: per team, how many squadrons have left the base they started the run on.
static std::map<VU_ID, VU_ID> gSqStartBase;

static void RebaseLog(int hour)
{
    int moved[NUM_TEAMS] = {0}, total[NUM_TEAMS] = {0};
    VuListIterator uit(AllUnitList);

    for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
    {
        if (!u->IsSquadron() || u->GetTeam() >= NUM_TEAMS)
            continue;

        VU_ID ab = u->GetUnitAirbaseID();
        auto it = gSqStartBase.find(u->Id());

        if (it == gSqStartBase.end())
            gSqStartBase[u->Id()] = ab;
        else if (it->second != ab)
            moved[u->GetTeam()]++;

        total[u->GetTeam()]++;
    }

    printf("REBASE h=%d ROK %d/%d US %d/%d DPRK %d/%d PRC %d/%d squadrons on a different base than at start\n", hour,
           moved[2], total[2], moved[1], total[1], moved[6], total[6], moved[5], total[5]);
}

// allylog=1 -- hourly, what the AI has told China's and Russia's starting battalions to do.
static int gAllyLog;
static std::vector<VU_ID> gAllyIds;

static void AllyLog(int hour)
{
    if (gAllyIds.empty())
    {
        VuListIterator uit(AllUnitList);

        for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
            if (u->IsBattalion() && (u->GetTeam() == 5 || u->GetTeam() == 4))
                gAllyIds.push_back(u->Id());
    }

    std::map<std::string, int> hist;

    for (size_t i = 0; i < gAllyIds.size(); i++)
    {
        Unit u = FindUnit(gAllyIds[i]);

        if (!u)
            continue;

        GridIndex x, y, dx, dy, ox = -1, oy = -1;
        u->GetLocation(&x, &y);
        u->GetUnitDestination(&dx, &dy);
        Objective o = u->GetUnitObjective();

        if (o)
            o->GetLocation(&ox, &oy);

        char key[128];
        sprintf(key, "team%d orders=%d assigned=%d obj=%s tactic=%d %s", (int)u->GetTeam(), u->GetUnitOrders(),
                u->Assigned() ? 1 : 0, o ? "yes" : "none", u->GetUnitTactic(),
                (o && Distance(x, y, ox, oy) < 3.0f) ? "AT-obj" : (o ? "away-from-obj" : ""));
        hist[key]++;

        if (i < 2)
            printf("ALLY h=%d bn=%d team=%d at %d,%d dest %d,%d orders=%d obj %d,%d assigned=%d tactic=%d moving=%d\n",
                   hour, (int)u->GetCampID(), (int)u->GetTeam(), x, y, dx, dy, u->GetUnitOrders(), ox, oy,
                   u->Assigned() ? 1 : 0, u->GetUnitTactic(), u->Moving() ? 1 : 0);
    }

    for (auto &h : hist)
        printf("ALLYSUM h=%d %3d x %s\n", hour, h.second, h.first.c_str());
}

// Engineers answer only their own division's requests (gtm.cpp RequestEngineer), so list, per
// team and division: battalions, engineer battalions (active + reinforcements).
static void DumpEngineers(void)
{
    std::map<std::pair<int, int>, int> bn, eng, engInact;
    VuListIterator uit(AllUnitList);

    for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
        if (u->IsBattalion())
        {
            std::pair<int, int> k(u->GetTeam(), u->GetUnitDivision());
            bn[k]++;

            if (u->GetUnitNormalRole() == GRO_ENGINEER)
                eng[k]++;
        }

    VuListIterator iit(InactiveList);

    for (Unit u = GetFirstUnit(&iit); u; u = GetNextUnit(&iit))
        if (u->IsBattalion() && u->GetUnitNormalRole() == GRO_ENGINEER)
            engInact[std::pair<int, int>(u->GetTeam(), u->GetUnitDivision())]++;

    for (auto &e : bn)
        printf("ENGDIV team=%d div=%d battalions=%d engineers=%d reinfEngineers=%d\n", e.first.first, e.first.second,
               e.second, eng[e.first], engInact[e.first]);
}

// Hourly: per team, bridges blown (<30%: impassable unless an engineer is within 1 km, path.cpp),
// damaged (<50%: what engineers are sent to, gndunit.cpp), and how many have an engineer on them.
static void BridgeLog(int hour)
{
    int blown[NUM_TEAMS] = {0}, damaged[NUM_TEAMS] = {0}, covered[NUM_TEAMS] = {0}, engBusy[NUM_TEAMS] = {0},
        engTotal[NUM_TEAMS] = {0};
    std::vector<Unit> engs;
    VuListIterator uit(AllUnitList);

    for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
        if (u->IsBattalion() && u->GetUnitNormalRole() == GRO_ENGINEER && !u->IsDead())
            engs.push_back(u), engTotal[u->GetTeam()]++;

    VuListIterator oit(AllObjList);

    for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
    {
        if (o->GetType() != TYPE_BRIDGE || o->GetObjectiveStatus() >= 50)
            continue;

        int t = o->GetTeam();
        GridIndex ox, oy;
        o->GetLocation(&ox, &oy);
        (o->GetObjectiveStatus() < 30 ? blown : damaged)[t]++;

        for (Unit e : engs)
        {
            GridIndex ex, ey;
            e->GetLocation(&ex, &ey);

            if (e->GetTeam() == t && Distance(ex, ey, ox, oy) <= 1.0f)
            {
                covered[t]++;
                break;
            }
        }
    }

    for (Unit e : engs)
    {
        GridIndex ex, ey;
        e->GetLocation(&ex, &ey);
        Objective o = GetObjectiveByXY(ex, ey);

        if (o && o->GetType() == TYPE_BRIDGE)
            engBusy[e->GetTeam()]++;
    }

    for (int t : {2, 6})
        printf("BRIDGES h=%d team %d: blown(<30%%)=%d damaged(30-49%%)=%d withEngineer=%d | engineers %d, %d on a bridge\n",
               hour, t, blown[t], damaged[t], covered[t], engTotal[t], engBusy[t]);
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

// One "sp" row per team, the supply chain end to end (indices are what tools read):
//  0-2   pools: supplyAvail fuelAvail replacementsAvail
//  3-4   supplyLevel fuelLevel (% the .tri IF_SUPPLY triggers read: units' have / (have + need))
//  5-7   last distribution ratio x1000 for supply, fuel, replacements (500 = the cap)
//  8-19  gSupplyDiag cumulative (SUPDIAG_* order: prod supply/fuel/repl, sent/got supply, sent/got fuel,
//        resupplies, lost-all, no-source, repl ground/air)
//  20-26 producers: supply sites, supply capacity/day, refineries, fuel capacity/day (what ProduceSupplies
//        sums: data rate x power), sites captured (produce 0), sites under 50% power, sites under 50% status
//  27-28 power stations owned (power + nuclear plants), their mean status
//  29-33 battalions, mean supply, within 30 km of the front, their mean supply, battalions under 25% supply
static void SupplyFrame()
{
    extern bool g_bPowerGrid;
    long v[NUM_TEAMS][34];
    memset(v, 0, sizeof(v));

    {
        VuListIterator oit(AllObjList);

        for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
        {
            int tm = o->GetTeam();

            if (tm < 0 || tm >= NUM_TEAMS)
                continue;

            int type = o->GetType();
            bool sup = type == TYPE_FACTORY || type == TYPE_ARMYBASE || type == TYPE_DEPOT || type == TYPE_PORT;
            bool ref = type == TYPE_REFINERY;

            // FindNearestFriendlyPowerStation takes nuclear plants too.
            if (type == TYPE_POWERPLANT || type == TYPE_NUCLEAR)
            {
                v[tm][27]++;
                v[tm][28] += o->GetObjectiveStatus();
                continue;
            }

            if (!sup && !ref)
                continue;

            v[tm][sup ? 20 : 22]++;

            if (o->GetObjectiveStatus() < 50)
                v[tm][26]++;

            if (o->GetObjectiveOldown() != o->GetOwner())
            {
                v[tm][24]++;
                continue;
            }

            long power = 100;

            if (g_bPowerGrid)
            {
                GridIndex x, y;
                o->GetLocation(&x, &y);
                Objective po = FindNearestFriendlyPowerStation(AllObjList, tm, x, y);
                power = po ? po->GetObjectiveStatus() : 0;
            }

            if (power < 50)
                v[tm][25]++;

            v[tm][sup ? 21 : 23] += o->GetObjectiveDataRate() * power / 100;
        }
    }

    {
        VuListIterator uit(AllUnitList);

        for (Unit u = GetFirstUnit(&uit); u; u = GetNextUnit(&uit))
        {
            int tm = u->GetTeam();

            if (tm < 0 || tm >= NUM_TEAMS || !u->IsBattalion())
                continue;

            GridIndex x, y;
            u->GetLocation(&x, &y);
            int s = u->GetUnitSupply();
            v[tm][29]++;
            v[tm][30] += s;

            if (DistanceToFront(x, y) <= 30.0F)
                v[tm][31]++, v[tm][32] += s;

            if (s < 25)
                v[tm][33]++;
        }
    }

    for (int tm = 0; tm < NUM_TEAMS; tm++)
    {
        TeamClass *t = TeamInfo[tm];

        if (t)
        {
            TeamStatusType *cs = t->GetCurrentStats();
            v[tm][0] = t->GetSupplyAvail();
            v[tm][1] = t->GetFuelAvail();
            v[tm][2] = t->GetReplacementsAvail();
            v[tm][3] = cs->supplyLevel;
            v[tm][4] = cs->fuelLevel;
        }

        for (int k = 0; k < 3; k++)
            v[tm][5 + k] = gSupplyRatio[tm][k];

        for (int k = 0; k < SUPDIAG_LAST; k++)
            v[tm][8 + k] = gSupplyDiag[tm][k];

        if (v[tm][27])
            v[tm][28] /= v[tm][27];

        if (v[tm][29])
            v[tm][30] /= v[tm][29];

        if (v[tm][31])
            v[tm][32] /= v[tm][31];

        TlPrintf("%s[", tm ? "," : "");

        for (int k = 0; k < 34; k++)
            TlPrintf("%s%ld", k ? "," : "", v[tm][k]);

        TlPrintf("]");
    }
}

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

    // cumulative vehicles lost by the team, by shooter (unit.cpp gLossDiag)
    TlPrintf("],\"lo\":[");

    for (int tm = 0; tm < NUM_TEAMS; tm++)
    {
        TlPrintf("%s[", tm ? "," : "");

        for (int k = 0; k < 6; k++)
            TlPrintf("%s%d", k ? "," : "", gLossDiag[tm][k]);

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

    // "sp": the supply chain per team (see SupplyFrame).
    TlPrintf("],\"sp\":[");
    SupplyFrame();

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
// crashtest=N -- fault on purpose at tick N (checks the crash logger)
static int gCrashTest;
// noend=1 -- keep simulating after the engine declares the campaign over (crash hunting)
static int gNoEnd;
// holdtest=N -- give N ROK battalions a "player" order (capture the nearest DPRK objective),
// exactly as the campaign map's waypoint drag does, and log hourly whether the AI keeps it.
static int gHoldTest;
static VU_ID gHoldIds[16];
static VU_ID gHoldObj[16];
extern int gPlayerOrdering;
extern bool g_bPlayerGroundHold;

static void HoldTestStart(void)
{
    VuListIterator uit(AllUnitList);
    int n = 0;

    for (Unit u = GetFirstUnit(&uit); u && n < gHoldTest && n < 16; u = GetNextUnit(&uit))
    {
        if (!u->IsBattalion() || u->GetTeam() != 2 || !u->Parent() || u->GetMovementType() == NoMove ||
            u->GetUnitNormalRole() == GRO_AIRDEFENSE || u->GetUnitNormalRole() == GRO_FIRESUPPORT)
            continue;

        GridIndex x, y, ox, oy;
        u->GetLocation(&x, &y);
        Objective best = NULL;
        float bd = 1e9f;
        VuListIterator oit(AllObjList);

        for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
        {
            if (o->GetTeam() != 6)
                continue;

            o->GetLocation(&ox, &oy);
            float d = Distance(x, y, ox, oy);

            if (d < bd)
                bd = d, best = o;
        }

        if (!best || bd > 40.0f)
            continue;

        gPlayerOrdering = 1;
        ((Battalion)u)->SetUnitOrders(GORD_CAPTURE, best->Id());
        gPlayerOrdering = 0;

        if (g_bPlayerGroundHold)
            u->SetPlayerHeld(1);

        gHoldIds[n] = u->Id();
        gHoldObj[n] = best->Id();
        printf("HOLDTEST start bn camp=%d -> capture obj %d (%.0f km) held=%d\n", (int)u->GetCampID(),
               (int)best->GetCampID(), bd, u->PlayerHeld() ? 1 : 0);
        n++;
    }
}

static void HoldTestLog(int hour)
{
    for (int i = 0; i < 16 && gHoldIds[i] != FalconNullId; i++)
    {
        Unit u = FindUnit(gHoldIds[i]);

        if (!u)
            continue;

        GridIndex x, y, ox, oy;
        u->GetLocation(&x, &y);
        Objective o = (Objective)vuDatabase->Find(gHoldObj[i]);
        o ? o->GetLocation(&ox, &oy) : (void)(ox = x, oy = y);
        printf("HOLD h=%d bn=%d orders=%d keepsPlayerObj=%d held=%d dist=%.1f km objTeam=%d broken=%d\n", hour,
               (int)u->GetCampID(), (int)u->GetUnitOrders(), u->GetUnitObjectiveID() == gHoldObj[i] ? 1 : 0,
               u->PlayerHeld() ? 1 : 0, Distance(x, y, ox, oy), o ? (int)o->GetTeam() : -1,
               u->Broken() ? 1 : 0);
    }
}

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
extern VU_TIME SimLibElapsedTime;
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

// Squadron stores against their stores table, at load: how many squadrons hold weapon types their table
// does not stock (SupplyUnit never draws those down, so they count as a permanent surplus), active
// versus waiting in the reinforcement (inactive) list.
static void StoresAudit(const char *when)
{
    for (int pass = 0; pass < 2; pass++)
    {
        int n = 0, bad = 0;
        long off = 0;
        VuListIterator it(pass ? (VuLinkedList *)InactiveList : (VuLinkedList *)AllUnitList);

        for (Unit u = GetFirstUnit(&it); u; u = GetNextUnit(&it))
        {
            UnitClassDataType *uc = u->IsSquadron() ? u->GetUnitClassData() : NULL;

            if (!uc)
                continue;

            int types = 0;

            for (int i = 0; i < MAXIMUM_WEAPTYPES; i++)
                if (!SquadronStoresDataTable[uc->SpecialIndex].Stores[i] && ((SquadronClass *)u)->GetUnitStores(i))
                    types++, off += ((SquadronClass *)u)->GetUnitStores(i);

            n++;
            bad += types > 0;
        }

        printf("STORES %s %s: %d squadrons, %d hold weapon types not in their table (%ld shots in all)\n", when,
               pass ? "inactive (reinforcements)" : "active", n, bad, off);
    }
}

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
        else if (!strcmp(key, "crashtest"))
            gCrashTest = atoi(val);
        else if (!strcmp(key, "reserves"))
        {
            extern int g_nGtmReservesPerCycle;
            g_nGtmReservesPerCycle = atoi(val);
            printf("KNOB g_nGtmReservesPerCycle = %d\n", g_nGtmReservesPerCycle);
        }
        else if (!strcmp(key, "abreloc"))
        {
            extern bool g_bEnableABRelocation;
            g_bEnableABRelocation = atoi(val) != 0;
            printf("KNOB g_bEnableABRelocation = %d\n", (int)g_bEnableABRelocation);
        }
        else if (!strcmp(key, "farthest"))
        {
            extern bool g_bGtmReserveFarthest;
            g_bGtmReserveFarthest = atoi(val) != 0;
            printf("KNOB g_bGtmReserveFarthest = %d\n", (int)g_bGtmReserveFarthest);
        }
        else if (!strcmp(key, "allylog"))
            gAllyLog = atoi(val);
        else if (!strcmp(key, "objpathcost"))
            OBJ_GROUND_PATH_MAX_COST = (short)atoi(val),
            printf("KNOB ObjGroundPathMaxCost = %d\n", (int)OBJ_GROUND_PATH_MAX_COST);
        else if (!strcmp(key, "objpathsearch"))
            OBJ_GROUND_PATH_MAX_SEARCH = (short)atoi(val),
            printf("KNOB ObjGroundPathMaxSearch = %d\n", (int)OBJ_GROUND_PATH_MAX_SEARCH);
        else if (!strcmp(key, "splitshares"))
        {
            extern bool g_bSupplySplitShares;
            g_bSupplySplitShares = atoi(val) != 0;
            printf("KNOB g_bSupplySplitShares = %d\n", (int)g_bSupplySplitShares);
        }
        else if (!strcmp(key, "nopullback"))
        {
            extern bool g_bReserveNoPullback;
            g_bReserveNoPullback = atoi(val) != 0;
            printf("KNOB g_bReserveNoPullback = %d\n", (int)g_bReserveNoPullback);
        }
        else if (!strcmp(key, "scramble"))
        {
            extern bool g_bAlertScramble;
            g_bAlertScramble = atoi(val) != 0;
            printf("KNOB g_bAlertScramble = %d\n", (int)g_bAlertScramble);
        }
        else if (!strcmp(key, "truelosses"))
        {
            extern bool g_bInitTrueLosses;
            g_bInitTrueLosses = atoi(val) != 0;
            printf("KNOB g_bInitTrueLosses = %d\n", (int)g_bInitTrueLosses);
        }
        else if (!strcmp(key, "capinit"))
        {
            extern int g_nCaptureInitiative;
            g_nCaptureInitiative = atoi(val);
            printf("KNOB g_nCaptureInitiative = %d\n", g_nCaptureInitiative);
        }
        else if (!strcmp(key, "counterinit"))
        {
            extern int g_nCounterAttackInitiative;
            g_nCounterAttackInitiative = atoi(val);
            printf("KNOB g_nCounterAttackInitiative = %d\n", g_nCounterAttackInitiative);
        }
        else if (!strcmp(key, "towncost"))
        {
            extern float g_fEnemyTownPathCost;
            g_fEnemyTownPathCost = (float)atof(val);
            printf("KNOB g_fEnemyTownPathCost = %.1f\n", g_fEnemyTownPathCost);
        }
        else if (!strcmp(key, "reshold"))
        {
            extern bool g_bReserveHold;
            g_bReserveHold = atoi(val) != 0;
            printf("KNOB g_bReserveHold = %d\n", (int)g_bReserveHold);
        }
        else if (!strcmp(key, "partialpath"))
        {
            extern bool g_bGridPathPartial;
            g_bGridPathPartial = atoi(val) != 0;
            printf("KNOB g_bGridPathPartial = %d\n", (int)g_bGridPathPartial);
        }
        else if (!strcmp(key, "gpathmax"))
            GROUND_PATH_MAX = (short)atoi(val), printf("KNOB GroundPathMax = %d\n", (int)GROUND_PATH_MAX);
        else if (!strcmp(key, "holdtest"))
            gHoldTest = atoi(val);
        else if (!strcmp(key, "playerhold"))
            g_bPlayerGroundHold = atoi(val) != 0, printf("KNOB g_bPlayerGroundHold = %d\n", (int)g_bPlayerGroundHold);
        else if (!strcmp(key, "noend"))
            gNoEnd = atoi(val), printf("KNOB noend: run continues past the endgame\n");
        else if (!strcmp(key, "prcwave"))
        {
            sscanf(val, "%d:%d", &gPrcWaveMin, &gPrcWaveMax);
            printf("KNOB prcwave: China arrives %d-%d km behind the front\n", gPrcWaveMin, gPrcWaveMax);
            PrcRecord();
        }
        else if (!strcmp(key, "prcoff"))
            gPrcOffHours = atoi(val), printf("KNOB prcoff: DPRK offensive for %d h after the wave\n", gPrcOffHours);
        else if (!strcmp(key, "resfix"))
        {
            extern bool g_bGtmReserveFix;
            g_bGtmReserveFix = atoi(val) != 0;
            printf("KNOB g_bGtmReserveFix = %d\n", (int)g_bGtmReserveFix);
        }
        else if (!strcmp(key, "needfix"))
        {
            extern bool g_bSupplyNeedFix;
            g_bSupplyNeedFix = atoi(val) != 0;
            printf("KNOB g_bSupplyNeedFix = %d\n", (int)g_bSupplyNeedFix);
        }
        else if (!strcmp(key, "capfront"))
        {
            extern bool g_bGtmCaptureFront;
            g_bGtmCaptureFront = atoi(val) != 0;
            printf("KNOB g_bGtmCaptureFront = %d\n", (int)g_bGtmCaptureFront);
        }
        else if (!strcmp(key, "keepcap"))
        {
            extern bool g_bGtmKeepCapture;
            g_bGtmKeepCapture = atoi(val) != 0;
            printf("KNOB g_bGtmKeepCapture = %d\n", (int)g_bGtmKeepCapture);
        }
        else if (!strcmp(key, "covermap"))
        {
            // covermap=X0:Y0:X1:Y1 -- the campaign grid's ground cover as text, north at the top:
            // ~ water, , bog, . barren/plain, b brush, f forest, F heavy forest, U urban, = road,
            // and objectives on top: N nuclear/power plant, P port, C city/town, B bridge, o other
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            sscanf(val, "%d:%d:%d:%d", &x0, &y0, &x1, &y1);
            static const char cov[] = "~,..bfFU";
            std::map<int, char> objAt;
            VuListIterator oit(AllObjList);

            for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
            {
                GridIndex ox, oy;
                o->GetLocation(&ox, &oy);
                int t = o->GetType();
                char c = (t == TYPE_NUCLEAR || t == TYPE_POWERPLANT) ? 'N' : t == TYPE_PORT ? 'P'
                         : (t == TYPE_CITY || t == TYPE_TOWN) ? 'C' : t == TYPE_BRIDGE ? 'B' : 'o';
                objAt[ox * 4096 + oy] = c;
            }

            printf("COVERMAP x %d..%d (left to right), y %d..%d (top = north)\n", x0, x1, y1, y0);

            for (int y = y1; y >= y0; y--)
            {
                std::string row;

                for (int x = x0; x <= x1; x++)
                {
                    std::map<int, char>::iterator it = objAt.find(x * 4096 + y);
                    int c = (int)GetCover(x, y);
                    row += it != objAt.end() ? it->second : GetRoad(x, y) && c ? '=' : (c >= 0 && c < 8 ? cov[c] : '?');
                }

                printf("COVERMAP %4d %s\n", y, row.c_str());
            }
        }
        else if (!strcmp(key, "objinfo"))
        {
            // objinfo=ID[,ID...] -- name, type, owner, cell and how a tracked/wheeled unit sees each cell
            // around it (movement cost, roads allowed or not; > MAX_COST = impassable)
            for (const char *p = val; p && *p; p = strchr(p, ','), p = p ? p + 1 : NULL)
            {
                Objective o = (Objective)GetEntityByCampID(atoi(p));

                if (!o)
                {
                    printf("OBJINFO %d: not found\n", atoi(p));
                    continue;
                }

                _TCHAR name[80];
                o->GetName(name, 79, FALSE);
                GridIndex x, y;
                o->GetLocation(&x, &y);
                printf("OBJINFO #%d \"%s\" type %d team %d at (%d,%d) secondary %d primary %d parent #%d\n",
                       (int)o->GetCampID(), name, (int)o->GetType(), (int)o->GetTeam(), x, y, o->IsSecondary(),
                       o->IsPrimary(), o->GetObjectiveParent() ? (int)o->GetObjectiveParent()->GetCampID() : 0);

                for (int dy = 2; dy >= -2; dy--)
                {
                    char row[512];
                    int n = sprintf(row, "OBJINFO   y=%d:", y + dy);

                    for (int dx = -2; dx <= 2; dx++)
                    {
                        float c0 = GetMovementCost(x + dx, y + dy, Tracked, 0, Here);
                        float cr = GetMovementCost(x + dx, y + dy, Tracked, PATH_ROADOK, Here);
                        n += sprintf(row + n, " [%c%c cov%d %3.0f/%3.0f]", dx == 0 && dy == 0 ? '*' : ' ',
                                     GetRoad(x + dx, y + dy) ? 'R' : ' ', (int)GetCover(x + dx, y + dy),
                                     c0 > 999 ? 999.0f : c0, cr > 999 ? 999.0f : cr);
                    }

                    printf("%s\n", row);
                }
            }
        }
        else if (!strcmp(key, "waterfix"))
        {
            extern bool g_bWaterObjectiveFix;
            g_bWaterObjectiveFix = atoi(val) != 0;
            printf("KNOB g_bWaterObjectiveFix = %d\n", (int)g_bWaterObjectiveFix);
        }
        else if (!strcmp(key, "keepstall"))
        {
            extern int g_nGtmKeepCaptureStall;
            g_nGtmKeepCaptureStall = atoi(val);
            printf("KNOB g_nGtmKeepCaptureStall = %d h\n", g_nGtmKeepCaptureStall);
        }
        else if (!strcmp(key, "capunits"))
        {
            extern int g_nGtmCaptureUnits;
            g_nGtmCaptureUnits = atoi(val);
            printf("KNOB g_nGtmCaptureUnits = %d\n", g_nGtmCaptureUnits);
        }
        else if (!strcmp(key, "exactloss"))
        {
            extern bool g_bSupplyExactLoss;
            g_bSupplyExactLoss = atoi(val) != 0;
            printf("KNOB g_bSupplyExactLoss = %d\n", (int)g_bSupplyExactLoss);
        }
        else if (!strcmp(key, "pak"))
        {
            // pak=ID:VALUE[:TEAM] -- the player's PAK slider (Priorities screen) for objective ID's PAK:
            // sets player_priority, which overrides the AI's air_priority for that team's air planning
            // (OCA target choice and every mission request's PAK term; 0 cancels missions there).
            // TEAM defaults to 2 (ROK, the player's side). Repeat the knob for several PAKs.
            int id = 0, value = 100, team = 2;
            sscanf(val, "%d:%d:%d", &id, &value, &team);
            Objective o = (Objective)GetEntityByCampID(id);
            Objective po = o ? (o->IsPrimary() ? o : o->GetObjectivePrimary()) : NULL;
            POData pd = po ? GetPOData(po) : NULL;

            if (pd && team >= 0 && team < NUM_TEAMS)
            {
                pd->player_priority[team] = (short)value;
                printf("KNOB pak: team %d PAK %d (from objective %d) player priority %d (AI air %d ground %d)\n",
                       team, (int)po->GetCampID(), id, value, (int)pd->air_priority[team],
                       (int)pd->ground_priority[team]);
            }
            else
                printf("KNOB pak: objective %d has no PAK\n", id);
        }
        else if (!strcmp(key, "tri"))
        {
            // The trigger script is picked by the scenario name stored inside the .cam, not by the
            // save's file name, so a variant save keeps reading save0.tri unless we point it here.
            // CheckTriggers re-reads the file every pass, so switching after load takes effect.
            TheCampaign.SetScenario((char *)val);
            printf("KNOB tri: triggers from %s.tri\n", val);
            {
                char p[_MAX_PATH];
                sprintf(p, "%s/%s.tri", FalconCampUserSaveDirectory, val);

                if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES)
                    printf("KNOB tri: WARNING %s does not exist -- no trigger will fire\n", p);
            }
        }
        else if (!strcmp(key, "hcg"))
            HitChanceGround = (float)atof(val), printf("KNOB 2DHitChanceGround = %.2f\n", HitChanceGround);
        else if (!strcmp(key, "hca"))
            HitChanceAir = (float)atof(val), printf("KNOB 2DHitChanceAir = %.2f\n", HitChanceAir);
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

    char line[600];
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    wsprintfA(line, "\nCRASH at campaign min %d (day %d): code 0x%08X at %p",
              (int)(TheCampaign.CurrentTime / CampaignMinutes), (int)TheCampaign.GetCampaignDay(),
              (unsigned)er->ExceptionCode, er->ExceptionAddress);
    CrashOut(line);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
    {
        wsprintfA(line, " (%s %p)", er->ExceptionInformation[0] ? "write" : "read",
                  (void *)er->ExceptionInformation[1]);
        CrashOut(line);
    }

    CrashOut("\nstack:\n");
    HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;
    static char symBuf[sizeof(IMAGEHLP_SYMBOL64) + 256];

    for (int guard = 0; guard < 48; guard++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &sf, &ctx, NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL) || !sf.AddrPC.Offset)
            break;

        IMAGEHLP_SYMBOL64 *sym = (IMAGEHLP_SYMBOL64 *)symBuf;
        sym->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
        sym->MaxNameLength = 255;
        DWORD64 d64 = 0;
        DWORD d32 = 0;
        IMAGEHLP_LINE64 ln = {sizeof(IMAGEHLP_LINE64)};
        const char *name = SymGetSymFromAddr64(proc, sf.AddrPC.Offset, &d64, sym) ? sym->Name : "?";

        if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &d32, &ln))
            wsprintfA(line, "  %p %s  %s:%lu\n", (void *)sf.AddrPC.Offset, name, ln.FileName, ln.LineNumber);
        else
            wsprintfA(line, "  %p %s\n", (void *)sf.AddrPC.Offset, name);

        CrashOut(line);
    }

    CrashOut("CRASH END\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    InitializeCriticalSection(&gVuLock);
    setvbuf(stdout, NULL, _IONBF, 0);

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    SetUnhandledExceptionFilter(CampsimCrashFilter);
    gOrderChangeHook = CapOrderHook;
    gMoveFailHook = MoveFailHook;
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

    StoresAudit("loaded");
    ApplyNewGame(argc, argv, savefile);
    StoresAudit("after new-game setup");
    ApplyKnobs(argc, argv);

    // The campaign thread is running; keep it parked while we tick by hand.
    TheCampaign.Suspend();
    InterlockedExchange(&gTicklerRun, 0);
    Sleep(50);

    // ...and then stop it for good. It used to stay parked in campaign_wait_for_sim(INFINITE),
    // since campsim has no sim thread to wake it; with the handshake waits capped at 250 ms
    // (threadmgr.cpp) it woke four times a second and dispatched VU messages concurrently with
    // this thread -- two threads in ATM::ProcessRequest corrupted the request list
    // (ListClass::Insert crash, a_none-mod-s61 day 2).
    ThreadManager::stop_campaign_thread();

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
    DumpEngineers();

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
        while (TheCampaign.CurrentTime < target && (!gEndGame || gNoEnd))
        {
            DWORD t0 = GetTickCount();
            if (gMainThread)
                gMainThread->Update(0);

            TheCampaign.vuThread->Update(0);
            DWORD t1 = GetTickCount();
            TheCampaign.CurrentTime += CampaignMinutes;
            TheCampaign.TimeOfDay = TheCampaign.CurrentTime % CampaignDay;
            // The game's timer thread keeps the sim clock equal to game time (timerthread.cpp
            // SetTime); campaign code reads it too, e.g. squadron rebasing waits for it to pass
            // 09:00:50 day 1 ("don't relocate before the campaign has begun").
            SimLibElapsedTime = TheCampaign.CurrentTime;
            ApplyAirTempo();

            if ((iter % 10) == 0)
                ApplyAbHeal();

            DoCampaignLoop(first);
            CheckEventLog(startTime);

            if (gCrashTest && iter == gCrashTest)
                *(volatile int *)(INT_PTR)gCrashTest = 0;

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
            PrcTick(iter);

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

            if ((iter % 180) == 0)
                BridgeLog(iter / 60);

            if ((iter % 360) == 0)
                GtmLog(iter / 60), CapLog(iter / 60), MoveLog(iter / 60);

            if ((iter % 1440) == 0)
                for (int t : {2, 6})
                {
                    const int *p = gSupplyPath[t];
                    printf("SUPPATH h=%d team %d: trips %d (mean path %.1f links), no path %d, emptied on the road %d "
                           "(they started with %d supply+fuel)\n",
                           iter / 60, t, p[SUPPATH_TRIPS],
                           p[SUPPATH_TRIPS] ? (float)p[SUPPATH_HOPS] / p[SUPPATH_TRIPS] : 0.0f, p[SUPPATH_NO_PATH],
                           p[SUPPATH_EMPTIED], p[SUPPATH_EMPTIED_SENT]);
                }

            if (iter == 1 || (iter % 720) == 0)
                RebaseLog(iter / 60);

            if (gAllyLog && (iter == 1 || (iter % 180) == 0))
                AllyLog(iter / 60);

            if (gHoldTest && iter == 1)
                HoldTestStart();

            if (gHoldTest && (iter % 60) == 0 && iter <= 60 * 24)
                HoldTestLog(iter / 60);

            if ((iter % 60) == 0 && vuLocalSessionEntity)
                printf("SESSREF h=%d ref=%d\n", iter / 60, vuLocalSessionEntity->RefCount());

            if ((iter % 360) == 0)
                for (int tm : {1, 2, 6})
                    printf("LOSS h=%d team %d: ground<-air %d  <-artillery %d  <-ground %d  <-naval %d | air<-air %d  "
                           "<-ground %d | ships<-air %d  <-surface %d\n",
                           iter / 60, tm, gLossDiag[tm][0], gLossDiag[tm][1], gLossDiag[tm][2], gLossDiag[tm][3],
                           gLossDiag[tm][4], gLossDiag[tm][5], gLossDiag[tm][6], gLossDiag[tm][7]);
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
