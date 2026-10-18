#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>
#include "cmpglobl.h"
#include "listadt.h"
#include "f4vu.h"
#include "vutypes.h"
#include "objectiv.h"
#include "strategy.h"
#include "unit.h"
#include "find.h"
#include "path.h"
#include "asearch.h"
#include "campaign.h"
#include "update.h"
#include "f4vu.h"
#include "camplist.h"
#include "gtm.h"
#include "team.h"
#include "gndunit.h"
#include "gtmobj.h"
#include "aiinput.h"
#include "classtbl.h"
#include "debuggr.h"
#include "supply.h"

#define MAX_SUPPLIES 60000
// campsim experiment knob (maxratio=): the most of its need a unit may be sent per resupply cycle (stock 0.5)
float g_fMaxSupplyRatio = 0.5F;
#define MAX_SUPPLY_RATIO g_fMaxSupplyRatio

#define AUTOMATIC_SUPPLY 5
#define AUTOMATIC_REPLACEMENTS 1

#ifdef DEBUG
int gSupplyFromProduction[NUM_TEAMS];
int gSupplyFromAirlift[NUM_TEAMS];
int gSupplyFromOffensive[NUM_TEAMS];
int gFuelFromProduction[NUM_TEAMS];
int gFuelFromAirlift[NUM_TEAMS];
int gFuelFromOffensive[NUM_TEAMS];
int gReplacmentsFromProduction[NUM_TEAMS];
int gReplacmentsFromAirlift[NUM_TEAMS];
int gReplacmentsFromOffensive[NUM_TEAMS];
#endif

extern bool g_bPowerGrid;

// CAMPSIM DIAGNOSTIC (read only by tools/campsim): cumulative per team, see SUPDIAG_*.
int gSupplyDiag[NUM_TEAMS][SUPDIAG_LAST] = {{0}};
int gSupplySplit[NUM_TEAMS][2] = {{0}};
int gStoresFlow[NUM_TEAMS][2] = {{0}};
// g_bSupplySplitShares: decaying averages of supply points actually used by ground units (moving, firing) and
// squadrons (stores loaded minus returned), and the ground share of the pool last computed (%)
float gSupplyUseGround[NUM_TEAMS] = {0}, gSupplyUseAir[NUM_TEAMS] = {0};
int gSupplyShareG[NUM_TEAMS] = {0};
// Last distribution ratios (x1000): share of each unit's need the pool could cover, capped at 500.
int gSupplyRatio[NUM_TEAMS][3] = {{0}};
int gSupplyPath[NUM_TEAMS][SUPPATH_LAST] = {{0}};


// ====================
// Regional stocks (g_bSupplyStocks, an experiment, off by default)
// ====================
// Depots and army bases hold a local stock of supply and fuel (class DataRate x StoreCapPct, the same number of each). Production
// into the national pool is unchanged except that stores stop producing unless StoresProduce. Each production call the pool tops
// the stores up (at most StoreFillPct of capacity each, shared out by how short they are); a battalion draws from the nearest store
// behind it instead of from the pool, so the stock really goes down where the fighting is and a bombed or captured store loses what
// it held. Squadrons keep using the pool. The stock is not saved: stores start full on load.
extern bool g_bSupplyStocks, g_bStoresProduce;
extern int g_nStoreCapPct, g_nStoreFillPct;
static float gStoreSup[MAX_CAMP_ENTITIES], gStoreFuel[MAX_CAMP_ENTITIES];
static uchar gStoreOwner[MAX_CAMP_ENTITIES], gStoreInit[MAX_CAMP_ENTITIES];
// campsim diagnostic, per team: [0] supply drawn, [1] fuel drawn, [2] supply asked for but not in stock, [3] resupplies with no store
// behind the unit, [4] supply refilled from the pool, [5] stock lost to capture or damage, [6] fuel refilled
int gStoreDiag[NUM_TEAMS][8] = {{0}};
// campsim experiment knobs (not in the cfg): battalion supply consumption multiplier, and the level stores start at, percent of capacity
int g_nSupplyUseMult = 1, g_nStoreStartPct = 100;
// campsim experiment knob (not in the cfg): depots and army bases are storage only, they do not produce (BMS 4.37 has no army bases and one depot at rate 1)
bool g_bStoresNoProd = false;
// campsim diagnostic: the first resupply of each battalion that found no store: grid x, y, team, reason (1 no friendly objective near it, 2 no
// store reachable at all, 3 only front or second line stores reachable), the hour is left to the caller
int gStoreMiss[8000][10] = {{0}}, gStoreMissN = 0;
extern void StoreSearchDiag(Objective start, int *visited, int *rejected, int *minRej, int *hops, int *worstCost);
extern int gCaseBuf[12][300][6], gCaseCount[12], gCaseSlot;
int gCaseUnit[12][4] = {{0}}, gCaseN = 0; // x, y, team, anchor id of the kept cases
static uchar gStoreMissSeen[MAX_CAMP_ENTITIES];

static int IsStoreObj(Objective o)
{
    return o->GetType() == TYPE_DEPOT or o->GetType() == TYPE_ARMYBASE;
}

static int StoreCap(Objective o)
{
    int rate = o->static_data.class_data ? o->static_data.class_data->DataRate : 0;
    int c = rate * g_nStoreCapPct / 100;
    return c < 20 ? 20 : c;
}

static void StoreSync(Objective o)
{
    int id = o->GetCampID(), team = o->GetTeam();

    if (id < 0 or id >= MAX_CAMP_ENTITIES)
        return;

    int cap = StoreCap(o);

    if (not gStoreInit[id])
    {
        gStoreInit[id] = 1;
        gStoreOwner[id] = (uchar)team;
        gStoreSup[id] = gStoreFuel[id] = (float)cap * g_nStoreStartPct / 100.0F;
        return;
    }

    if (gStoreOwner[id] != team)
    {
        gStoreDiag[gStoreOwner[id] % NUM_TEAMS][5] += (int)gStoreSup[id];
        gStoreSup[id] = gStoreFuel[id] = 0.0F;
        gStoreOwner[id] = (uchar)team;
    }

    float lim = (float)(cap * o->GetObjectiveStatus() / 100); // bombed: the stock burns with it

    if (gStoreSup[id] > lim)
    {
        gStoreDiag[team % NUM_TEAMS][5] += (int)(gStoreSup[id] - lim);
        gStoreSup[id] = lim;
    }

    if (gStoreFuel[id] > lim)
        gStoreFuel[id] = lim;
}

// Top the team's stores up from its pools: shared out by how short each is, none taking more than StoreFillPct of its capacity.
static void FillStores(int who)
{
    static Objective st[512];
    static float wantS[512], wantF[512];
    int n = 0;
    float totS = 0.0F, totF = 0.0F;
    VuListIterator oit(AllObjList);

    for (Objective o = GetFirstObjective(&oit); o and n < 512; o = GetNextObjective(&oit))
    {
        if (not IsStoreObj(o) or o->GetTeam() != who)
            continue;

        StoreSync(o);
        int id = o->GetCampID(), cap = StoreCap(o);
        float room = (float)cap * (float)o->GetObjectiveStatus() / 100.0F, fill = (float)cap * g_nStoreFillPct / 100.0F;
        st[n] = o;
        wantS[n] = max(0.0F, min(room - gStoreSup[id], fill));
        wantF[n] = max(0.0F, min(room - gStoreFuel[id], fill));
        totS += wantS[n];
        totF += wantF[n];
        n++;
    }

    float poolS = (float)TeamInfo[who]->GetSupplyAvail(), poolF = (float)TeamInfo[who]->GetFuelAvail();
    float kS = totS > 0.0F ? min(1.0F, poolS / totS) : 0.0F, kF = totF > 0.0F ? min(1.0F, poolF / totF) : 0.0F;
    float usedS = 0.0F, usedF = 0.0F;

    for (int i = 0; i < n; i++)
    {
        int id = st[i]->GetCampID();
        gStoreSup[id] += wantS[i] * kS;
        gStoreFuel[id] += wantF[i] * kF;
        usedS += wantS[i] * kS;
        usedF += wantF[i] * kF;
    }

    TeamInfo[who]->SetSupplyAvail((int)(poolS - usedS));
    TeamInfo[who]->SetFuelAvail((int)(poolF - usedF));
    gStoreDiag[who][4] += (int)usedS;
    gStoreDiag[who][6] += (int)usedF;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// sxmap=1 (campsim supply research, off by default; implies the regional stores above): a supply MAP. A producer (factory, port, and
// depots / army bases where they produce) puts its output into the nearest store of its team -- not into one national pool -- keeping
// SxMapAirPct% back for the squadrons' pool; whatever a full store cannot take goes to the pool too. Every production call each store
// under half full is refilled from the nearest store with stock to spare, found by walking its own team's objective links: the shipment
// loses each road / junction / rail / bridge node's supply loss on the way (damage included -- SupplyInterdiction), and with sxconvoyhit
// a further share per link. Battalions draw from their nearest store as with stocks=1. So a factory feeds its region first, the front is
// fed only through the network, and a bombed factory, depot or bridge empties the stores behind it.
int g_nSxMap = 0, g_nSxMapAirPct = 30;
int g_nSxMapReach = 25, g_nSxMapShipPct = 25; // sxmapreach= links a short store looks for a donor; sxmapship= most a shipment moves, % of capacity
int gSxMapDiag[NUM_TEAMS][6] = {{0}}; // deposited locally, to the pool, shipments, shipped, arrived, short stores with no donor in reach
struct SxProd
{
    Objective o;
    float s;
};
static std::vector<SxProd> gSxProd;
static int NodeSupplyLoss(Objective c, int type);
extern costtype GetObjectiveMovementCost(Objective o, Objective t, int neighbor, MoveType type, Team team, int flags);
extern int g_nSxConvoyTeam, g_nSxConvoyHit;

static Objective SxNearestStore(Objective p, bool withRoom = true)
{
    GridIndex px, py;
    p->GetLocation(&px, &py);
    Objective best = NULL;
    float bd = 1e30F;
    VuListIterator oit(AllObjList);

    for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
    {
        if (not IsStoreObj(o) or o->GetTeam() not_eq p->GetTeam())
            continue;

        if (withRoom)
        {
            StoreSync(o);

            if (gStoreSup[o->GetCampID()] >= (float)StoreCap(o) * o->GetObjectiveStatus() / 100.0F - 1.0F)
                continue; // full: the next nearest
        }

        GridIndex x, y;
        o->GetLocation(&x, &y);
        const float d = (float)((x - px) * (x - px) + (y - py) * (y - py));

        if (d < bd)
            bd = d, best = o;
    }

    return best;
}

// production of this call -> stores (factor turns a producer's data rate into what the pool formula would have produced)
static void SxMapDeposit(int who, float factor)
{
    int toPool = 0;

    for (const SxProd &p : gSxProd)
    {
        if (p.o->GetTeam() not_eq who)
            continue;

        float amt = p.s * factor;
        float air = amt * g_nSxMapAirPct / 100.0F;
        amt -= air;
        toPool += FloatToInt32(air);
        Objective st = SxNearestStore(p.o);

        if (st)
        {
            StoreSync(st);
            const int id = st->GetCampID();
            const float room = (float)StoreCap(st) * st->GetObjectiveStatus() / 100.0F - gStoreSup[id];
            const float put = amt < room ? (room > 0 ? amt : 0) : room;
            gStoreSup[id] += put;
            gStoreFuel[id] += put;
            gSxMapDiag[who][0] += FloatToInt32(put);
            amt -= put;
        }

        toPool += FloatToInt32(amt);
    }

    TeamInfo[who]->SetSupplyAvail(TeamInfo[who]->GetSupplyAvail() + toPool);
    gSxMapDiag[who][1] += toPool;
}

// short stores are refilled from the nearest store with stock to spare, over the team's own objective links
static void SxMapShip(int who)
{
    static int parent[MAX_CAMP_ENTITIES];
    static Objective objOf[MAX_CAMP_ENTITIES];
    static uchar seen[MAX_CAMP_ENTITIES];
    VuListIterator oit(AllObjList);

    for (Objective b = GetFirstObjective(&oit); b; b = GetNextObjective(&oit))
    {
        if (not IsStoreObj(b) or b->GetTeam() not_eq who)
            continue;

        StoreSync(b);
        const int bid = b->GetCampID(), bcap = StoreCap(b);

        if (gStoreSup[bid] >= bcap * 0.5F)
            continue;

        // breadth-first from the short store to the nearest store holding more than 60% of its capacity
        memset(seen, 0, sizeof(seen));
        std::vector<Objective> q;
        std::vector<int> depth;
        q.push_back(b), depth.push_back(0);
        seen[bid] = 1;
        parent[bid] = -1;
        Objective donor = NULL;

        for (size_t h = 0; h < q.size() and not donor; h++)
        {
            Objective cur = q[h];

            if (depth[h] >= g_nSxMapReach)
                continue;

            for (int n = 0; n < cur->static_data.links; n++)
            {
                Objective c = cur->GetNeighbor(n);

                if (not c or seen[c->GetCampID()] or c->GetTeam() not_eq who)
                    continue;

                if (GetObjectiveMovementCost(cur, NULL, n, Wheeled, (Team)who, PATH_MARINE) >= 250)
                    continue;

                seen[c->GetCampID()] = 1;
                parent[c->GetCampID()] = cur->GetCampID();
                objOf[c->GetCampID()] = c;

                if (IsStoreObj(c))
                {
                    StoreSync(c);

                    if (gStoreSup[c->GetCampID()] > StoreCap(c) * 0.6F)
                    {
                        donor = c;
                        break;
                    }
                }

                q.push_back(c), depth.push_back(depth[h] + 1);
            }
        }

        if (not donor)
        {
            gSxMapDiag[who][5]++;
            continue;
        }

        const int did = donor->GetCampID();
        float amt = gStoreSup[did] - StoreCap(donor) * 0.5F;
        const float want = bcap * 0.8F - gStoreSup[bid];
        amt = amt < want ? amt : want;
        amt = amt < bcap * g_nSxMapShipPct / 100.0F ? amt : bcap * g_nSxMapShipPct / 100.0F;

        if (amt <= 0)
            continue;

        gStoreSup[did] -= amt;
        gStoreFuel[did] = gStoreFuel[did] > amt ? gStoreFuel[did] - amt : 0;

        // the route back from the donor to the short store
        float keep = 1.0F;
        int links = 0;

        for (int id = parent[did]; id >= 0 and id not_eq bid; id = parent[id], links++)
        {
            Objective c = objOf[id];
            int type = c ? c->GetType() : 0;

            if (c and (type == TYPE_ROAD or type == TYPE_INTERSECT or type == TYPE_RAILROAD or type == TYPE_BRIDGE))
                keep *= (100 - NodeSupplyLoss(c, type)) / 100.0F;
        }

        if (g_nSxConvoyHit > 0 and who == g_nSxConvoyTeam)
            for (int k = 0; k < links; k++)
                keep *= (100 - g_nSxConvoyHit) / 100.0F;

        gStoreSup[bid] += amt * keep;
        gStoreFuel[bid] += amt * keep;
        gSxMapDiag[who][2]++;
        gSxMapDiag[who][3] += FloatToInt32(amt);
        gSxMapDiag[who][4] += FloatToInt32(amt * keep);
    }
}

// campsim: a team's stores now
void StoreStats(int team, int *count, int *stockS, int *capS, int *stockF, int *low)
{
    *count = *stockS = *capS = *stockF = *low = 0;
    VuListIterator oit(AllObjList);

    for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
    {
        if (not IsStoreObj(o) or o->GetTeam() != team)
            continue;

        StoreSync(o);
        int id = o->GetCampID(), cap = StoreCap(o);
        (*count)++;
        *stockS += (int)gStoreSup[id];
        *stockF += (int)gStoreFuel[id];
        *capS += cap;

        if (gStoreSup[id] < cap * 0.25F)
            (*low)++;
    }
}

// ====================
// Production functions
// ====================

// This function collects supply from all producers and adds it to the team's totals.
// This should be called by the Campaign Master only
int ProduceSupplies(CampaignTime deltaTime)
{
    Objective o;
    Objective po = NULL;
    Team who;
    float rate;
    int type;
    ulong supply[NUM_TEAMS] = {0}, fuel[NUM_TEAMS] = {0}, s, f, power;
    ulong replacements[NUM_TEAMS] = {0};
    MissionRequestClass mis;
    GridIndex x, y;

    // Produce supplies, fuel and reinforcements (factories and refineries)
    {
        VuListIterator myit(AllObjList);
        o = GetFirstObjective(&myit);

        while (o)
        {
            type = o->GetType();

            //Cobra Added Army, depot, and port per JimG
            if ((type == TYPE_FACTORY or type == TYPE_PORT or
                 ((type == TYPE_ARMYBASE or type == TYPE_DEPOT) and not g_bStoresNoProd and (not g_bSupplyStocks or g_bStoresProduce))) and
                o->GetObjectiveOldown() == o->GetOwner()) // Supply
            {
                if (g_bPowerGrid)
                {
                    o->GetLocation(&x, &y);
                    po = FindNearestFriendlyPowerStation(AllObjList,
                                                         o->GetTeam(), x, y);

                    if (po)
                        power = po->GetObjectiveStatus();
                    else
                        power = 0;
                }
                else
                    power = 100;

                s = o->GetObjectiveDataRate() * power / 100;

                if (s)
                {
                    who = o->GetTeam();

                    if (g_nSxMap) // sxmap: this producer's output goes to its nearest store (SxMapDeposit), not the national pool
                    {
                        SxProd sp = {o, s * DataRateModSup};
                        gSxProd.push_back(sp);
                    }
                    else
                        supply[who] += FloatToInt32((
                            s *
                            DataRateModSup)); // A.S.  2001-12-09 DataRateModification for supply and fuel only
                    replacements[who] += FloatToInt32((
                        s *
                        DataRateModRepl)); // A.S.  2001-12-09 DataRateModification for replacements only

                    // *** old code ***
                    // supply[who] += s;
                    //     replacements[who] += s;


                    // Request an interdiction stike mission
                    mis.requesterID = o->Id();
                    o->GetLocation(&mis.tx, &mis.ty);
                    mis.vs = who;
                    mis.who = GetEnemyTeam(mis.vs);
                    mis.tot = Camp_GetCurrentTime() + rand() % deltaTime +
                              30 * CampaignMinutes;
                    mis.tot_type = TYPE_NE;
                    mis.targetID = o->Id();
                    mis.mission = AMIS_INTSTRIKE;
                    mis.roe_check = ROE_AIR_ATTACK;
                    mis.context = enemyProductionSource;
                    mis.priority = 0;
                    mis.RequestEnemyMission();

                    if (g_bPowerGrid)
                    {
                        // Request an interdiction stike mission against the power station
                        mis.requesterID = po->Id();
                        po->GetLocation(&mis.tx, &mis.ty);
                        mis.vs = who;
                        mis.who = GetEnemyTeam(mis.vs);
                        mis.tot = Camp_GetCurrentTime() + rand() % deltaTime +
                                  30 * CampaignMinutes;
                        mis.tot_type = TYPE_NE;
                        mis.targetID = po->Id();
                        mis.mission = AMIS_INTSTRIKE;
                        mis.roe_check = ROE_AIR_ATTACK;
                        mis.context = enemyFuelSource;
                        mis.priority = 0;
                        mis.RequestMission();
                    }
                }
            }
            else if (type == TYPE_REFINERY and
                     o->GetObjectiveOldown() == o->GetOwner()) // Fuel
            {
                if (g_bPowerGrid)
                {
                    o->GetLocation(&x, &y);
                    po = FindNearestFriendlyPowerStation(AllObjList,
                                                         o->GetTeam(), x, y);

                    if (po)
                        power = po->GetObjectiveStatus();
                    else
                        power = 0;
                }
                else
                    power = 100;

                f = o->GetObjectiveDataRate() * power / 100;

                if (f)
                {
                    who = o->GetTeam();
                    fuel[who] += f;
                    // Request an interdiction stike mission
                    mis.requesterID = o->Id();
                    o->GetLocation(&mis.tx, &mis.ty);
                    mis.vs = who;
                    mis.who = GetEnemyTeam(mis.vs);
                    mis.tot = Camp_GetCurrentTime() + rand() % deltaTime +
                              30 * CampaignMinutes;
                    mis.tot_type = TYPE_NE;
                    mis.targetID = o->Id();
                    mis.mission = AMIS_INTSTRIKE;
                    mis.roe_check = ROE_AIR_ATTACK;
                    mis.context = enemyFuelSource;
                    mis.priority = 0;
                    mis.RequestMission();

                    if (g_bPowerGrid)
                    {
                        // Request an interdiction stike mission against the power station
                        mis.requesterID = po->Id();
                        po->GetLocation(&mis.tx, &mis.ty);
                        mis.vs = who;
                        mis.who = GetEnemyTeam(mis.vs);
                        mis.tot = Camp_GetCurrentTime() + rand() % deltaTime +
                                  30 * CampaignMinutes;
                        mis.tot_type = TYPE_NE;
                        mis.targetID = po->Id();
                        mis.mission = AMIS_INTSTRIKE;
                        mis.roe_check = ROE_AIR_ATTACK;
                        mis.context = enemyFuelSource;
                        mis.priority = 0;
                        mis.RequestMission();
                    }
                }
            }

            // NOTE: IF WE WANT SPECIAL SUPPLY SOURCES (OFF-MAP SUPPLY SOURCES), ADD THE
            // BONUS HERE.
            // if (o->IsBonusSupplySource())
            // supply += 1000;

            o = GetNextObjective(&myit);
        }
    }
    // rates are per day - convert to this interval
    rate = (float)deltaTime / (float)CampaignDay;

    for (who = 0; who < NUM_TEAMS; who++)
    {
        int actionBonus = TeamInfo[who]->GetGroundAction()->actionType;

        // A.S. 2001-12-09  No Action Type Bonus for production of supply, fuel and replacements
        if (NoActionBonusProd)
            actionBonus = 1;

        // end added section

        supply[who] = AUTOMATIC_SUPPLY +
                      FloatToInt32((supply[who] / 5) * rate * actionBonus);
        fuel[who] =
            AUTOMATIC_SUPPLY + FloatToInt32(fuel[who] * rate * actionBonus);
        replacements[who] =
            AUTOMATIC_REPLACEMENTS +
            FloatToInt32((replacements[who] / 40) * rate * actionBonus);

#ifdef DEBUG
        gSupplyFromProduction[who] += supply[who];
        gFuelFromProduction[who] += fuel[who];
        gReplacmentsFromProduction[who] += replacements[who];
#endif
        gSupplyDiag[who][SUPDIAG_PROD_SUPPLY] += supply[who];
        gSupplyDiag[who][SUPDIAG_PROD_FUEL] += fuel[who];
        gSupplyDiag[who][SUPDIAG_PROD_REPL] += replacements[who];

        // Deplete unused extra supplies and move supplies to team supply pools
        supply[who] = (TeamInfo[who]->GetSupplyAvail() / 2) + supply[who];
        fuel[who] = (TeamInfo[who]->GetFuelAvail() / 2) + fuel[who];
        replacements[who] =
            TeamInfo[who]->GetReplacementsAvail() + replacements[who];

        if (supply[who] > MAX_SUPPLIES)
            supply[who] = MAX_SUPPLIES;

        TeamInfo[who]->SetSupplyAvail(supply[who]);

        if (fuel[who] > MAX_SUPPLIES)
            fuel[who] = MAX_SUPPLIES;

        TeamInfo[who]->SetFuelAvail(fuel[who]);
        TeamInfo[who]->SetReplacementsAvail(replacements[who]);

        if (g_nSxMap)
        {
            SxMapDeposit(who, rate * actionBonus / 5.0F);
            SxMapShip(who);
        }
        else if (g_bSupplyStocks)
            FillStores(who);
    }

    gSxProd.clear();
    return 1;
}

// ==================
// Supply functions
// ==================

void AddSupply(Objective o, int supply, int fuel)
{
    int s, f;
    WORD sup = o->static_data.local_data;

    s = LOBYTE(sup) + supply;

    if (s > 255)
        s = 255;

    f = HIBYTE(sup) + fuel;

    if (f > 255)
        f = 255;

    o->static_data.local_data = MAKEWORD(s, f);
}

// Artscout - 2026: what a damaged node costs the supply run crossing it.
//
// Stock is a flat 2% per hop whether the bridge is standing or lying in the river. The field that
// was meant to carry the difference -- obj_data.losses, read just below as
// GetObjectiveSupplyLosses -- is dead twice over. Nothing in the tree ever writes it non-zero: the
// only objSetLosses send sits inside the block comment at the foot of this file, in the legacy
// SupplyUnits that was replaced. And the one live message that touches it, objSetSupply, resets it
// to 0 (objectivemsg.cpp) -- on exactly the nodes a supply run just recorded traffic through. So the
// field cannot be revived by writing to it; anything written would be cleared the same tick. The
// value has to be derived where it is read, which is here.
//
// Derived from the objective's own status: the same 0-100 the production maths and the map's Damage
// layer use, rolled up from per-feature damage by ObjectiveClass::CalcStatus. Taking it from there
// rather than keeping a second number means repair unwinds this for free -- status climbs as
// features are rebuilt and the route re-opens on its own, with no parallel mechanism to keep in
// step, and no save-game field to migrate.
//
// A bridge bites harder than a road because that is what a bridge is: there is no going round one,
// where a cratered road is a detour. The link costs themselves are still static -- LinkCampaignObjectives
// computes them once at campaign build from the terrain and nothing rewrites them. The PLANNING
// half is applied at search time instead (2026-09-27): GetObjectiveMovementCost (path.cpp) scales
// the ground cost of entering a damaged node with the same bridge-vs-road weighting, so the planner
// now routes around what this function makes expensive to use. Knob: PathDamageCost.
// ---------------------------------------------------------------------------------------------------------------------------------
// campsim supply research (2026-10-18), every switch off by default (main.cpp knobs sx*):
//   sxown=1     a unit draws only on supply sources its own team holds (stock: the search never looks at the owner)
//   sxrear=H    a source taken in the last H hours is no source yet (depots follow an advance after a delay; 9999 = home sources only)
//   sxhops=N    a source further than N objective links is out of reach -- the unit gets nothing (cut off / over-extended)
//   sxloss=P    base loss per road / junction / bridge node, % (stock 2)
//   sxrail=P    base loss per railroad node, % (stock 2): rail as the cheap artery
// campsim measured (supply_reach.py, h_bar seed 1): 126 sources, ~70% of battalions within 25 km of one, supply 84-100% at any
// distance -- captured sources count at once, so an advance is never far from a source.
int g_nSxOwn = 0, g_nSxRearH = 0, g_nSxHops = 0, g_nSxLoss = -1, g_nSxRail = -1;
int g_nSxAttackUse = 0;           // sxattackuse=M (unit.cpp, battalio.cpp): battalions on capture orders use M times the supply
// sxpocket=H: a battalion that has found no reachable supply source for H hours (surrounded, or its lines cut) loses 15 morale a supply
// pass, and surrenders (is removed) when its morale falls under 10. Stock: a cut-off unit just keeps what it had.
int g_nSxPocketH = 0;
int gSxPocketDiag[NUM_TEAMS][3] = {{0}}; // cut-off passes, morale hits, surrenders (cumulative)
static CampaignTime gSxCutSince[MAX_CAMP_ENTITIES];
static std::vector<VU_ID> gSxSurrender;

static void SxPocket(Unit u, bool found)
{
    if (g_nSxPocketH <= 0 or not u->IsBattalion())
        return;

    const int id = u->GetCampID();

    if (id < 0 or id >= MAX_CAMP_ENTITIES)
        return;

    if (found)
    {
        gSxCutSince[id] = 0;
        return;
    }

    const int t = u->GetTeam() % NUM_TEAMS;
    gSxPocketDiag[t][0]++;

    if (not gSxCutSince[id])
        gSxCutSince[id] = TheCampaign.CurrentTime;

    if (TheCampaign.CurrentTime - gSxCutSince[id] < (CampaignTime)g_nSxPocketH * CampaignHours)
        return;

    const int m = u->GetUnitMorale() - 15;
    u->SetUnitMorale(m > 0 ? m : 0);
    gSxPocketDiag[t][1]++;

    if (m < 10)
        gSxSurrender.push_back(u->Id());
}

static void SxSurrenders(void)
{
    for (const VU_ID &id : gSxSurrender)
    {
        Unit u = (Unit)vuDatabase->Find(id);

        if (u and not u->IsDead())
        {
            gSxPocketDiag[u->GetTeam() % NUM_TEAMS][2]++;
            u->KillUnit();
        }
    }

    gSxSurrender.clear();
}

int g_nSxFront = -1;              // sxfront=P (see SupplyUnits): engaged battalions get P% of a delivery; -1 = off
int g_nSxWheeled = 0;             // sxwheeled=1 (see SendSupply): supply routes as Wheeled, not Foot
int gSxFrontCut[NUM_TEAMS] = {0}; // deliveries cut that way

// sxdelay=H: a battalion's delivery arrives H hours per 10 objective links of its route after it is sent (a convoy is on the road),
// so the far end of a long line refills slowly. 0 = stock (instant).
int g_nSxDelay = 0;
static int gSxLastHops = 0; // set by SendSupply
struct SxPending
{
    VU_ID id;
    int sneed, s, fneed, f;
    CampaignTime at;
};
static std::vector<SxPending> gSxPending;
int gSxDelayed[NUM_TEAMS] = {0}; // deliveries queued, cumulative

void SupplyUnit(Unit u, int sneed, int supply, int fneed, int fuel);

// sxconvoyhit=T:P (with sxdelay): team T's convoys on the road lose P% per hour in transit -- the enemy's interdiction sorties along its
// lines (the player's choice of where to send them). 0 = off.
int g_nSxConvoyTeam = 0, g_nSxConvoyHit = 0;
int gSxConvoyLost[NUM_TEAMS] = {0}; // supply points lost that way, cumulative

static void SxQueueDelivery(Unit u, int sneed, int s, int fneed, int f)
{
    SxPending p;
    p.id = u->Id();

    if (g_nSxConvoyHit > 0 and u->GetTeam() == g_nSxConvoyTeam)
    {
        const int hours = gSxLastHops * g_nSxDelay / 10;
        float keep = 1.0F;

        for (int h = 0; h < hours; h++)
            keep *= (100 - g_nSxConvoyHit) / 100.0F;

        gSxConvoyLost[u->GetTeam() % NUM_TEAMS] += s - FloatToInt32(s * keep);
        s = FloatToInt32(s * keep);
        f = FloatToInt32(f * keep);
    }

    p.sneed = sneed, p.s = s, p.fneed = fneed, p.f = f;
    p.at = TheCampaign.CurrentTime + (CampaignTime)gSxLastHops * g_nSxDelay * CampaignHours / 10;
    gSxPending.push_back(p);
    gSxDelayed[u->GetTeam() % NUM_TEAMS]++;
}

static void SxDeliverArrived(int who)
{
    for (size_t i = 0; i < gSxPending.size();)
    {
        if (gSxPending[i].at > TheCampaign.CurrentTime)
        {
            i++;
            continue;
        }

        Unit u = (Unit)vuDatabase->Find(gSxPending[i].id);

        // delivered at the first supply pass after it arrives, whoever's pass it is. (It used to wait for the pass of the battalion's own
        // team -- but with chinaown China is supplied in the North's pass and has none of its own, so Chinese deliveries never arrived:
        // campsim model S, China's median supply 16% by day 8.)
        (void)who;

        // (a battalion wiped out while its convoy was on the road is still in the database with no vehicles)
        if (u and u->IsBattalion() and not u->IsDead() and u->GetTotalVehicles() > 0)
            SupplyUnit(u, gSxPending[i].sneed, gSxPending[i].s, gSxPending[i].fneed, gSxPending[i].f);

        gSxPending[i] = gSxPending.back();
        gSxPending.pop_back();
    }
}
static uchar gSxOwner[MAX_CAMP_ENTITIES];
static CampaignTime gSxSince[MAX_CAMP_ENTITIES];
static bool gSxInit = false;
int gSxDiag[NUM_TEAMS][4] = {{0}}; // searches, no source in reach, refused (owner / too recent), total hops of found sources

// sxnocity=1: cities are no supply source -- only depots, army bases and ports. Stock counts every city behind the front, so a source
// is always a few links away and no bridge or road strike ever sits between a unit and its supply (campsim: all 250 of the North's
// bridges held down, its front still 93-98% supplied, by truck or on foot).
int g_nSxNoCity = 0;

static bool SxActive(void)
{
    return g_nSxOwn or g_nSxRearH > 0 or g_nSxHops > 0 or g_nSxNoCity;
}

// ownership history: when did the current owner take each objective (0 = held since the start)
static void SxTrackOwners(void)
{
    VuListIterator oit(AllObjList);

    for (Objective o = GetFirstObjective(&oit); o; o = GetNextObjective(&oit))
    {
        const int id = o->GetCampID();

        if (id < 0 or id >= MAX_CAMP_ENTITIES)
            continue;

        if (not gSxInit)
            gSxOwner[id] = o->GetTeam(), gSxSince[id] = 0;
        else if (gSxOwner[id] not_eq o->GetTeam())
            gSxOwner[id] = o->GetTeam(), gSxSince[id] = TheCampaign.CurrentTime;
    }

    gSxInit = true;
}

static bool SxSourceOk(Objective c, int who)
{
    if (not c->IsSupplySource())
        return false;

    if (g_nSxNoCity and c->GetType() == TYPE_CITY)
        return false;

    if (g_nSxOwn and c->GetTeam() not_eq who)
        return false;

    if (g_nSxRearH > 0)
    {
        const int id = c->GetCampID();

        if (id >= 0 and id < MAX_CAMP_ENTITIES and gSxSince[id] and
            TheCampaign.CurrentTime - gSxSince[id] < (CampaignTime)g_nSxRearH * CampaignHours)
            return false;
    }

    return true;
}

extern costtype GetObjectiveMovementCost(Objective o, Objective t, int neighbor, MoveType type, Team team, int flags);

// FindNearestSupplySource (find.cpp) with the sx rules: breadth-first over objective links, depth counted
static Objective SxFindSource(Objective o, int who)
{
    static uchar seen[MAX_CAMP_ENTITIES];
    std::vector<std::pair<Objective, int>> q;
    size_t head = 0;
    bool refused = false;

    memset(seen, 0, sizeof(seen));
    q.push_back(std::make_pair(o, 0));
    seen[o->GetCampID()] = 1;
    gSxDiag[who % NUM_TEAMS][0]++;

    while (head < q.size())
    {
        Objective cur = q[head].first;
        const int depth = q[head].second;
        head++;

        if (g_nSxHops > 0 and depth >= g_nSxHops)
            continue;

        for (int n = 0; n < cur->static_data.links; n++)
        {
            Objective c = cur->GetNeighbor(n);

            if (not c or seen[c->GetCampID()])
                continue;

            seen[c->GetCampID()] = 1;

            if (GetObjectiveMovementCost(cur, NULL, n, Wheeled, (uchar)who, PATH_MARINE) >= 250)
                continue;

            if (SxSourceOk(c, who))
            {
                gSxDiag[who % NUM_TEAMS][3] += depth + 1;
                return c;
            }

            if (c->IsSupplySource())
                refused = true;

            q.push_back(std::make_pair(c, depth + 1));
        }
    }

    gSxDiag[who % NUM_TEAMS][refused ? 2 : 1]++;
    return NULL;
}

static int NodeSupplyLoss(Objective c, int type)
{
    extern int g_nSupplyInterdiction;

    // Stock behaviour, and still the floor: the field is read in case anything ever does write it.
    int base = 2; // Automatic loss rate of 2% per objective

    if (type == TYPE_RAILROAD and g_nSxRail >= 0)
        base = g_nSxRail;
    else if (type not_eq TYPE_RAILROAD and g_nSxLoss >= 0)
        base = g_nSxLoss;

    int l = c->GetObjectiveSupplyLosses() + base;

    if (g_nSupplyInterdiction <= 0)
        return l;

    const int status = c->GetObjectiveStatus();

    if (status >= 100)
        return l; // intact: costs what it always did

    const int worst = (type == TYPE_BRIDGE) ? 50 : 15;
    l += (worst * (100 - status) / 100) * g_nSupplyInterdiction / 100;

    if (l > 95)
        l = 95;

    return l;
}

// This sends supply from s to d, subtracting from losses and adding to it's supply traffic field
// Updates values to amount which actually made it. returns 0 if none made it.
int SendSupply(Objective s, Objective d, int *supply, int *fuel)
{
    Objective c;
    PathClass path;
    int i, l, n, loss, type;
    extern bool g_bSupplyExactLoss;
    const int team = s->GetTeam();
    const int sent = *supply > 0 ? *supply : 0, sentf = *fuel > 0 ? *fuel : 0;

    if (not *supply and not *fuel)
        return 0;

    // sxwheeled=1: supply moves by truck. Stock routes it on Foot, which walks round a blown bridge -- campsim: with all 250 of the
    // North's bridges held down from hour 48 its front stayed 85-98% supplied.
    extern int g_nSxWheeled;

    if (GetObjectivePath(&path, s, d, g_nSxWheeled ? Wheeled : Foot, s->GetTeam(), PATH_MARINE) < 1)
    {
        gSupplyPath[team][SUPPATH_NO_PATH]++;
        return 0;
    }

    gSupplyPath[team][SUPPATH_TRIPS]++;
    gSupplyPath[team][SUPPATH_HOPS] += path.GetLength();
    gSxLastHops = path.GetLength();

    c = s;
    loss = 0;
    AddSupply(s, *supply / 10, *fuel / 10);

    // Artscout - 2026 (g_bSupplyExactLoss, off = stock): stock applies each node's loss in integer
    // maths, x * (100 - l) / 100, which takes at least 1 point per hop from any shipment under 50 --
    // a unit's share is often 2-10 points, so it is gone after a handful of road nodes no matter how
    // light the real losses are (campsim: 62% of ROK resupplies arrived empty). Exact mode keeps the
    // surviving fraction as a float and rounds once at the end of the trip.
    float keep = 1.0F;
    const int s0 = *supply, f0 = *fuel;

    for (i = 0; i < path.GetLength(); i++)
    {
        n = path.GetDirection(i);
        c = c->GetNeighbor(n);
        type = c->GetType();

        if (type == TYPE_ROAD or type == TYPE_INTERSECT or
            type == TYPE_RAILROAD or type == TYPE_BRIDGE)
        {
            l = NodeSupplyLoss(c, type);

            if (g_bSupplyExactLoss)
            {
                keep *= (100 - l) / 100.0F;
                *supply = FloatToInt32(s0 * keep + 0.5F);
                *fuel = FloatToInt32(f0 * keep + 0.5F);
                AddSupply(c, *supply / 10, *fuel / 10);
            }
            else
            {
                AddSupply(c, *supply / 10, *fuel / 10);
                *supply = *supply * (100 - l) / 100;
                *fuel = *fuel * (100 - l) / 100;
            }
        }

        if (not *supply and not *fuel)
        {
            gSupplyPath[team][SUPPATH_EMPTIED]++;
            gSupplyPath[team][SUPPATH_EMPTIED_SENT] += sent + sentf;
            return 0;
        }
    }

    return 1;
}

void SupplyUnit(Unit u, int sneed, int supply, int fneed, int fuel)
{
    Unit e;
    float sratio, fratio;

    if (u->IsBattalion() or u->IsSquadron())
    {
        if (not supply and not fuel)
            return;

        // KCK: We can add supply and fuel directly now, since we're asserting all
        // entities are local to the host. Values will be updated via dirty data
        u->SupplyUnit(supply, fuel);
        // // Send the supply message
        // u->SendUnitMessage(u->Id(),FalconUnitMessage::unitSupply,supply,fuel,0);
    }
    else if (u->IsBrigade())
    {
        if (sneed > 0)
            sratio = (float)supply / (float)sneed;
        else
            sratio = 1.0F;

        if (fneed > 0)
            fratio = (float)fuel / (float)fneed;
        else
            fratio = 1.0F;

        e = u->GetFirstUnitElement();

        while (e)
        {
            sneed = e->GetUnitSupplyNeed(FALSE);
            fneed = e->GetUnitFuelNeed(FALSE);
            SupplyUnit(e, sneed, FloatToInt32(sneed * sratio), fneed,
                       FloatToInt32(fneed * fratio));
            e = u->GetNextUnitElement();
        }
    }
}

// Supplies units - must be called by Campaign Master
// campsim experiment (chinaown=1): China, a team of its own once it joins, draws on the North's pool: its battalions and squadrons count in the
// North's needs and are supplied from it (a team of its own has no pool worth the name -- campsim: 70 of 86 Chinese battalions under 50% by hour 48)
extern bool g_bChinaOwn;

static bool SupplyMember(Unit u, Team who)
{
    if (u->GetTeam() == who)
        return true;

    return g_bChinaOwn and who == 6 and u->GetTeam() == 5 and TeamInfo[5] and GetTTRelations(5, 6) == Allied;
}

int SupplyUnits(Team who, CampaignTime deltaTime)
{
    if (g_bChinaOwn and who == 5 and TeamInfo[5] and GetTTRelations(5, 6) == Allied)
        return 0; // supplied with the North

    Objective o, s;
    Unit unit;
    int supply, fuel, replacements, gots, gotf, type;
    int sneeded = 0, fneeded = 0, rneeded = 0;
    int sneededGround = 0, sneededAir = 0; // g_bSupplySplitShares
    float sratio, fratio, rratio;
    GridIndex x, y;
    MissionRequestClass mis;
    // A.S. begin additional variables 2001-12-09
    // A.S. We now distinguish between aircrafts and ground vehicles
    int rneeded_a = 0, rneeded_v = 0, repl_a = 0, repl_v = 0;
    float rratio_a, rratio_v, a_v_nratio, repl; // A.S.
    float sqnbonus, lambda; // A.S.
    int repl_a_s = 0, repl_v_s = 0, repl_s = 0, repl_sa = 0,
        prob = 0; // A.S. debug variables
    // end added section

    if (not TeamInfo[who] or not(TeamInfo[who]->flags bitand TEAM_ACTIVE))
        return 0;

    if (SxActive())
        SxTrackOwners();

    SxDeliverArrived(who);

    sratio = fratio = rratio = 0.0F;

    // A.S. begin, 2001-12-09.
    rratio_a = rratio_v = a_v_nratio = repl = lambda = 0.0F; // A.S.
    sqnbonus =
        RelSquadBonus; // A.S. gives Sqn relative ( not ) more repl. than Bde.
    // end added section

    // zero supply values
    {
        VuListIterator objit(AllObjList);
        o = GetFirstObjective(&objit);

        while (o)
        {
            if (o->GetTeam() == who)
                o->static_data.local_data = 0;

            o = GetNextObjective(&objit);
        }
    }

    // Calculate needs
    {
        VuListIterator myit(AllUnitList);
        unit = GetFirstUnit(&myit);

        while (unit)
        {
            if (SupplyMember(unit, who) and
                (unit->IsBattalion() or unit->IsSquadron()))
            {
                // Artscout - 2026 (g_bSupplyNeedFix, off = stock): a unit holding more than it wants reports
                // a negative need. Stock summed those in, so a surplus anywhere could take the team total to
                // zero or below -- and then sratio stays 0 and no unit at all is resupplied. Count only what
                // is actually needed; the surplus is still drawn back below, unit by unit.
                extern bool g_bSupplyNeedFix;
                int sn = unit->GetUnitSupplyNeed(FALSE), fn = unit->GetUnitFuelNeed(FALSE);

                sneeded += (g_bSupplyNeedFix and sn < 0) ? 0 : sn;
                fneeded += (g_bSupplyNeedFix and fn < 0) ? 0 : fn;

                if (sn > 0)
                    (unit->IsSquadron() ? sneededAir : sneededGround) += sn;
                rneeded +=
                    unit->GetFullstrengthVehicles() - unit->GetTotalVehicles();

                // A.S. begin
                if (unit->IsSquadron() and
                    NoTypeBonusRepl) // A.S. extra calculation for squadrons
                {
                    rneeded_a += unit->GetFullstrengthVehicles() -
                                 unit->GetTotalVehicles();
                }

                // end added section
            }

            unit = GetNextUnit(&myit);
        }
    }


    // A.S. begin, 2001-12-09.
    if (NoTypeBonusRepl)
    {
        rneeded_v = rneeded - rneeded_a; //A.S. calculation for groud vehicles

        if (rneeded > 0)
            a_v_nratio = ((float)rneeded_a) / rneeded;
        else
            a_v_nratio = 1;

        repl = (float)TeamInfo[who]
                   ->GetReplacementsAvail(); // aggregate replacements available
        lambda = a_v_nratio * sqnbonus;

        // Artscout - 2026 (g_bReplSplitFix, off = stock): lambda is the squadrons' share of the replacement pool and ground
        // units get 1 - lambda. Capped at 1, any team whose aircraft are 1/RelSquadBonus (a quarter at the stock 4.0) of its
        // total shortfall gave its ground units exactly nothing: DPRK, whose airframes Blue shoots down by the thousand,
        // got 3 ground replacements in five days (ROK 4,446). Weighting the need instead keeps the squadron priority
        // (RelSquadBonus times more per vehicle) and never takes the ground share to zero; with little air need it is the
        // same number as before.
        {
            extern bool g_bReplSplitFix;

            if (g_bReplSplitFix)
            {
                float wa = sqnbonus * (float)rneeded_a;
                lambda = (wa + (float)rneeded_v) > 0.0F ? wa / (wa + (float)rneeded_v) : 1.0F;
            }
        }

        if (lambda > 1)
            lambda = 1;
    }

    // end added section


    // Calculate the maximum ratio of supply we can give out
    if (sneeded > 0)
        sratio = (float)TeamInfo[who]->GetSupplyAvail() / sneeded;

    if (sratio > MAX_SUPPLY_RATIO)
        sratio = MAX_SUPPLY_RATIO;

    // Artscout - 2026 (g_bSupplySplitShares, off = stock): share the pool between ground and air by what each
    // actually uses, then give each side a ratio from its own need. Stock used one ratio, pool / (everyone's
    // need), and squadron need is the gap to a full stores table -- which squadrons barely draw down (campsim
    // SUPSPLIT, ROK: ~98% of loaded weapons come back unused, ~50-100 points/day used, 8000-11000 points of
    // "need"). That diluted every battalion's share to ~4% per trip; the rest of the pool decayed unused.
    float sratioGround = sratio, sratioAir = sratio;
    {
        extern bool g_bSupplySplitShares;

        if (g_bSupplySplitShares)
        {
            float useG = gSupplyUseGround[who], useA = gSupplyUseAir[who];

            if (useA < 0.0F) // weapons returned from sorties loaded before this window
                useA = 0.0F;
            float shareG = (useG + useA > 1.0F) ? useG / (useG + useA) : 0.5F;
            float pool = (float)TeamInfo[who]->GetSupplyAvail();

            sratioGround = sneededGround > 0 ? pool * shareG / sneededGround : 0.0F;
            sratioAir = sneededAir > 0 ? pool * (1.0F - shareG) / sneededAir : 0.0F;

            if (sratioGround > MAX_SUPPLY_RATIO)
                sratioGround = MAX_SUPPLY_RATIO;

            if (sratioAir > MAX_SUPPLY_RATIO)
                sratioAir = MAX_SUPPLY_RATIO;

            gSupplyShareG[who] = FloatToInt32(shareG * 100.0F);
        }

        // decay the use averages: each supply pass keeps 80% of the history
        gSupplyUseGround[who] *= 0.8F;
        gSupplyUseAir[who] *= 0.8F;
    }

    if (fneeded > 0)
        fratio = (float)TeamInfo[who]->GetFuelAvail() / fneeded;

    if (fratio > MAX_SUPPLY_RATIO)
        fratio = MAX_SUPPLY_RATIO;

    if (rneeded > 0)
        rratio = (float)TeamInfo[who]->GetReplacementsAvail() / rneeded;

    if (not NoTypeBonusRepl) // A.S. added if-condiion 2001-12-09
    {
        if (rratio > 0.25F)
            rratio = 0.25F;
    }

    // A.S. begin, 2001-12-09
    if (NoTypeBonusRepl)
    {
        if (rratio > MAX_SUPPLY_RATIO)
        {
            rratio = MAX_SUPPLY_RATIO;
        }

        if (rneeded_a > 0)
            rratio_a = repl / rneeded_a;
        else
            rratio_a = MAX_SUPPLY_RATIO;

        if (rneeded_v > 0)
            rratio_v = repl / rneeded_v;
        else
            rratio_v = MAX_SUPPLY_RATIO;

        if (rratio_a > MAX_SUPPLY_RATIO)
            rratio_a = MAX_SUPPLY_RATIO;

        if (rratio_v > MAX_SUPPLY_RATIO)
            rratio_v = MAX_SUPPLY_RATIO;

        if (repl == 0) // to handle situations like 0/0
        {
            rratio_a = 0;
            rratio_v = 0;
        }
    }

    // end added section

    gSupplyRatio[who][0] = FloatToInt32(sratio * 1000.0F);
    gSupplyRatio[who][1] = FloatToInt32(fratio * 1000.0F);
    gSupplyRatio[who][2] = FloatToInt32(rratio * 1000.0F);

    // Supply units
    {
        VuListIterator myit(AllUnitList);
        unit = GetFirstUnit(&myit);

        while (unit)
        {
            // We only supply/repair Battalions and Squadrons
            if (SupplyMember(unit, who) and
                (unit->IsBattalion() or unit->IsSquadron()) and
                TheCampaign.CurrentTime - unit->GetLastResupplyTime() >
                    unit->GetUnitSupplyTime())
            {
                float typeBonus = 1.0F;

                if (unit->GetUnitCurrentRole() == GRO_ATTACK)
                    typeBonus = 6.0F;

                if (unit->IsSquadron())
                {
                    ((SquadronClass *)unit)->ReinforcePilots(2);
                    typeBonus = 2.0F;
                }

                // Add some randomness
                typeBonus += ((rand() % 50) - 25.0F) / 100.0F;

                if (typeBonus < 0.0F)
                    typeBonus = 0.0F;

                supply = FloatToInt32(unit->GetUnitSupplyNeed(FALSE) *
                                      (unit->IsSquadron() ? sratioAir : sratioGround) * typeBonus);
                fuel = FloatToInt32(unit->GetUnitFuelNeed(FALSE) * fratio *
                                    typeBonus);

                // sxfront=P: a battalion engaged with the enemy gets only P% of its delivery (convoys do not reach a unit under fire).
                // campsim: resupply outruns use (Blue battalions under 50% supply 2.2% of unit-hours, median low point 80%).
                if (g_nSxFront >= 0 and g_nSxFront < 100 and unit->IsBattalion() and (unit->Engaged() or unit->Combat()))
                {
                    supply = supply * g_nSxFront / 100;
                    fuel = fuel * g_nSxFront / 100;
                    gSxFrontCut[who % NUM_TEAMS]++;
                }

                // g_bSupplyStocks: a battalion asks its regional store for up to MAX_SUPPLY_RATIO of what it is short of (x typeBonus)
                const bool stockUnit = g_bSupplyStocks and unit->IsBattalion();

                if (stockUnit)
                {
                    supply = FloatToInt32(unit->GetUnitSupplyNeed(FALSE) * MAX_SUPPLY_RATIO * typeBonus);
                    fuel = FloatToInt32(unit->GetUnitFuelNeed(FALSE) * MAX_SUPPLY_RATIO * typeBonus);
                }

                // A.S.  2001-12-09. No Type Bonus for replacements. This helps fixing the bug that units can get more replacements than available.
                if (NoTypeBonusRepl)
                    typeBonus = 1.0F;

                // end added section

                replacements = FloatToInt32((unit->GetFullstrengthVehicles() -
                                             unit->GetTotalVehicles()) *
                                            rratio * typeBonus);

                // A.S. begin, 2001-12-09
                repl_s += replacements; // debug

                if (NoTypeBonusRepl) // New code for distinguishing between aircrafts and ground vehicles
                {
                    if (unit->IsSquadron()) // this algorithm guarantees that no team can get more repl than available
                    {
                        prob = rand() % 100;
                        repl_a = FloatToInt32((unit->GetFullstrengthVehicles() -
                                               unit->GetTotalVehicles()) *
                                              (lambda)*rratio_a);

                        if (TeamInfo[who]->GetReplacementsAvail() >= 1 and
                            prob < 51 and
                            (((float)unit->GetTotalVehicles()) /
                                 unit->GetFullstrengthVehicles() <=
                             0.8F)) // rounding up with probability 0.5
                            repl_a = FloatToInt32(
                                (float)ceil((unit->GetFullstrengthVehicles() -
                                             unit->GetTotalVehicles()) *
                                            (lambda)*rratio_a));

                        if (repl_a > 0)
                        {
                            TeamInfo[who]->SetReplacementsAvail(
                                TeamInfo[who]->GetReplacementsAvail() - repl_a);
                            unit->ChangeVehicles(min(
                                repl_a, TeamInfo[who]->GetReplacementsAvail()));
                            repl_a_s += min(
                                repl_a,
                                TeamInfo[who]->GetReplacementsAvail()); // debug
                        }
                    }
                    else
                    {
                        prob = rand() % 100;
                        repl_v = FloatToInt32((unit->GetFullstrengthVehicles() -
                                               unit->GetTotalVehicles()) *
                                              (1 - lambda) * rratio_v);

                        if (TeamInfo[who]->GetReplacementsAvail() > 12 and
                            prob < 51 and
                            (((float)unit->GetTotalVehicles()) /
                                 unit->GetFullstrengthVehicles() <=
                             0.85F)) // rounding up
                            repl_v = FloatToInt32(
                                (float)ceil((unit->GetFullstrengthVehicles() -
                                             unit->GetTotalVehicles()) *
                                            (1 - lambda) * rratio_v));

                        if (repl_v > 0)
                        {
                            TeamInfo[who]->SetReplacementsAvail(
                                TeamInfo[who]->GetReplacementsAvail() - repl_v);
                            unit->ChangeVehicles(min(
                                repl_v, TeamInfo[who]->GetReplacementsAvail()));
                            repl_v_s += min(
                                repl_v,
                                TeamInfo[who]->GetReplacementsAvail()); // debug
                        }
                    }

                    // prob = rand() % 100;
                }
                else
                {
                    if (replacements) // ++++++ old code begin ++++++
                    {
                        TeamInfo[who]->SetReplacementsAvail(
                            TeamInfo[who]->GetReplacementsAvail() -
                            replacements);

                        if (unit->IsSquadron())
                        {
                            repl_sa += replacements; // A.S. debug
                            TeamInfo[who]->SetReplacementsAvail(
                                TeamInfo[who]->GetReplacementsAvail() -
                                replacements);
                            unit->SetLastResupply(replacements);
                        }

                        unit->ChangeVehicles(replacements);
                    }
                } // end added section  (important: this section replaces( not ) the section marked with ++++++ old code ++++++ )

                if (fuel or supply)
                {
                    gSupplyDiag[who][SUPDIAG_RESUPPLIES]++;
                    unit->GetLocation(&x, &y);
                    o = FindNearestFriendlyObjective(who, &x, &y, 0);

                    if (stockUnit)
                    {
                        Objective st = o ? FindNearestSupplyStore(o) : NULL;
                        SxPocket(unit, st != NULL or (o and FindNearestSupplySource(o) != NULL));

                        if (not st)
                        {
                            gSupplyDiag[who][SUPDIAG_NO_SOURCE]++;
                            gStoreDiag[who][3]++;

                            if (gStoreMissN < 8000 and unit->GetCampID() < MAX_CAMP_ENTITIES and not gStoreMissSeen[unit->GetCampID()])
                            {
                                gStoreMissSeen[unit->GetCampID()] = 1;
                                gStoreMiss[gStoreMissN][0] = x;
                                gStoreMiss[gStoreMissN][1] = y;
                                gStoreMiss[gStoreMissN][2] = who;
                                gStoreMiss[gStoreMissN][3] = not o ? 1 : (FindNearestSupplyStore(o, 1) ? 3 : 2);
                                gStoreMiss[gStoreMissN][4] = (int)(TheCampaign.CurrentTime / CampaignHours);

                                gCaseSlot = -1;

                                if (o and gCaseN < 12 and gStoreMissN % 4 == 0)
                                {
                                    gCaseSlot = gCaseN++;
                                    gCaseUnit[gCaseSlot][0] = x;
                                    gCaseUnit[gCaseSlot][1] = y;
                                    gCaseUnit[gCaseSlot][2] = who;
                                    gCaseUnit[gCaseSlot][3] = o->GetCampID();
                                }

                                if (o)
                                    StoreSearchDiag(o, &gStoreMiss[gStoreMissN][5], &gStoreMiss[gStoreMissN][6], &gStoreMiss[gStoreMissN][7],
                                                    &gStoreMiss[gStoreMissN][8], &gStoreMiss[gStoreMissN][9]);

                                gStoreMissN++;
                            }
                        }
                        else
                        {
                            StoreSync(st);
                            const int sid = st->GetCampID();
                            int ds = min(supply, (int)gStoreSup[sid]), df = min(fuel, (int)gStoreFuel[sid]);
                            gStoreDiag[who][2] += supply - ds;
                            gStoreSup[sid] -= (float)ds;
                            gStoreFuel[sid] -= (float)df;
                            gStoreDiag[who][0] += ds;
                            gStoreDiag[who][1] += df;
                            gots = ds;
                            gotf = df;
                            gSupplyDiag[who][SUPDIAG_SENT_SUPPLY] += ds;
                            gSupplyDiag[who][SUPDIAG_SENT_FUEL] += df;

                            if (ds or df)
                            {
                                if (SendSupply(st, o, &gots, &gotf))
                                {
                                    SupplyUnit(unit, ds, gots, df, gotf);
                                    gSupplyDiag[who][SUPDIAG_GOT_SUPPLY] += gots;
                                    gSupplyDiag[who][SUPDIAG_GOT_FUEL] += gotf;
                                    gSupplySplit[who][0] += gots;
                                }
                                else
                                    gSupplyDiag[who][SUPDIAG_LOST_ALL]++;
                            }
                        }
                    }
                    else
                    {
                    s = o ? (SxActive() ? SxFindSource(o, who) : FindNearestSupplySource(o)) : NULL;
                    SxPocket(unit, s != NULL or (o and FindNearestSupplySource(o) != NULL));

                    if (o and s)
                    {
                        TeamInfo[who]->SetSupplyAvail(
                            TeamInfo[who]->GetSupplyAvail() - supply);
                        TeamInfo[who]->SetFuelAvail(
                            TeamInfo[who]->GetFuelAvail() - fuel);
                        gots = supply;
                        gotf = fuel;
                        gSupplyDiag[who][SUPDIAG_SENT_SUPPLY] += supply;
                        gSupplyDiag[who][SUPDIAG_SENT_FUEL] += fuel;

                        if (SendSupply(s, o, &gots, &gotf))
                        {
                            if (g_nSxDelay > 0 and unit->IsBattalion())
                                SxQueueDelivery(unit, supply, gots, fuel, gotf);
                            else
                                SupplyUnit(unit, supply, gots, fuel, gotf);
                            gSupplyDiag[who][SUPDIAG_GOT_SUPPLY] += gots;
                            gSupplyDiag[who][SUPDIAG_GOT_FUEL] += gotf;
                            // campsim: supply received by battalions [0] vs squadrons [1]
                            gSupplySplit[who][unit->IsSquadron() ? 1 : 0] += gots;
                        }
                        else
                            gSupplyDiag[who][SUPDIAG_LOST_ALL]++;
                    }
                    else
                        gSupplyDiag[who][SUPDIAG_NO_SOURCE]++;
                    }
                }

                unit->SetLastResupplyTime(TheCampaign.CurrentTime);
            }

            unit = GetNextUnit(&myit);
        }
    }

    SxSurrenders();

    if (NoTypeBonusRepl)
    {
        gSupplyDiag[who][SUPDIAG_REPL_GROUND] += repl_v_s;
        gSupplyDiag[who][SUPDIAG_REPL_AIR] += repl_a_s;
    }
    else
    {
        gSupplyDiag[who][SUPDIAG_REPL_GROUND] += repl_s - repl_sa;
        gSupplyDiag[who][SUPDIAG_REPL_AIR] += repl_sa;
    }

    // A.S. debug begin
    //if (NoTypeBonusRepl) {
    // if (who == 2 or who==6) {
    // FILE *deb;
    // deb = fopen("c:/temp/deb1.txt", "a");
    // fprintf(deb, "Team %2d  ReplaAvail = %3d  A_Needed = %3d  V_Needed %4d  Aircraft = %2d  Vehicle = %3d  TIME = %d\n", who, (int)repl, rneeded_a, rneeded_v, repl_a_s, repl_v_s, TheCampaign.CurrentTime );
    // fclose(deb);
    // }
    //}
    //else {
    // if (who == 2 or who==6) { // A.S. debug
    // FILE *deb;
    // deb = fopen("c:/temp/deb1.txt", "a");
    // fprintf(deb, "Team %2d  ReplaAvail = %3d  Needed = %3d bitor Repl_a = %2d repl_v = %3d bitor TIME = %d\n", who, TeamInfo[who]->GetReplacementsAvail(), rneeded, repl_sa, (repl_s-repl_sa) , TheCampaign.CurrentTime % CampaignHours );
    // fclose(deb);
    // }
    //}
    // A.S. debug end

    // Reset loss values, set supply values, and request missions
    {
        VuListIterator objit(AllObjList);
        o = GetFirstObjective(&objit);

        while (o)
        {
            if (o->GetTeam() == who)
            {
                supply = LOBYTE(o->static_data.local_data);
                fuel = HIBYTE(o->static_data.local_data);

                // Artscout - 2026: split what gets RECORDED from what the AI acts on.
                //
                // The single threshold below used to gate both, and at 5 it hides most of the
                // network: SendSupply deposits a TENTH of what passes at each node, so a segment
                // needs ~50 units of supply crossing it in one tick to clear it. Only the heaviest
                // junctions do, which is why a map of this data reads as a few isolated points
                // rather than the chain it actually is.
                //
                // obj_data.supply is display-only -- nothing in the sim reads it back -- so
                // recording more of it changes nothing but the dirty-data traffic. The interdiction
                // requests further down are gameplay and keep the original 5 regardless.
                extern int g_nSupplyMapThreshold;
                const int recordAt =
                    (g_nSupplyMapThreshold < 0) ? 5 : g_nSupplyMapThreshold;

                if (supply > recordAt or fuel > recordAt)
                    o->SendObjMessage(o->Id(),
                                      FalconObjectiveMessage::objSetSupply,
                                      (short)(supply), (short)(fuel), 0);

                if (supply > 5 or fuel > 5)
                {
                    type = o->GetType();

                    if (type == TYPE_ROAD or type == TYPE_INTERSECT)
                    {
                        // Request an interdiction mission
                        mis.requesterID = o->Id();
                        o->GetLocation(&mis.tx, &mis.ty);
                        mis.vs = o->GetTeam();
                        mis.who = GetEnemyTeam(mis.vs);
                        mis.tot = Camp_GetCurrentTime() +
                                  (30 + rand() % 480) * CampaignMinutes;
                        mis.tot_type = TYPE_NE;
                        mis.targetID = FalconNullId;
                        mis.mission = AMIS_INT;
                        mis.roe_check = ROE_AIR_ATTACK;
                        mis.context = enemySupplyInterdictionZone;
                        mis.RequestMission();
                    }

                    // RV - Biker - Do something special for bridges
                    //if (type == TYPE_BRIDGE or type == TYPE_DEPOT or type == TYPE_PORT)
                    if (type == TYPE_DEPOT or type == TYPE_PORT)
                    {
                        // Request an interdiction strike mission
                        mis.requesterID = o->Id();
                        o->GetLocation(&mis.tx, &mis.ty);
                        mis.vs = o->GetTeam();
                        mis.who = GetEnemyTeam(mis.vs);
                        mis.tot = Camp_GetCurrentTime() +
                                  (30 + rand() % 480) * CampaignMinutes;
                        mis.tot_type = TYPE_NE;
                        mis.targetID = o->Id();
                        mis.mission = AMIS_INTSTRIKE;
                        mis.roe_check = ROE_AIR_ATTACK;

                        if (type == TYPE_DEPOT)
                            mis.context = enemySupplyInterdictionDepot;

                        if (type == TYPE_PORT)
                            mis.context = enemySupplyInterdictionPort;

                        mis.RequestMission();
                    }

                    if (type == TYPE_BRIDGE)
                    {
                        // Check distance to FLOT
                        GridIndex ox = 0, oy = 0;
                        o->GetLocation(&ox, &oy);
                        float dist = DistanceToFront(ox, oy);

                        if (dist >= 50.0f)
                        {
                            mis.requesterID = o->Id();
                            mis.tx = ox;
                            mis.ty = oy;
                            mis.vs = o->GetTeam();
                            mis.who = GetEnemyTeam(mis.vs);
                            mis.tot = Camp_GetCurrentTime() +
                                      (30 + rand() % 480) * CampaignMinutes;
                            mis.tot_type = TYPE_NE;
                            mis.targetID = o->Id();
                            mis.mission = AMIS_INTSTRIKE;
                            mis.roe_check = ROE_AIR_ATTACK;
                            mis.context = enemySupplyInterdictionBridge;
                            mis.RequestMission();
                        }
                    }
                }
            }

            o = GetNextObjective(&objit);
        }
    }
    return 1;
}

/*
// Supplies units - must be called by Campaign Master
int SupplyUnits (Team team)
 {
 Objective o,s;
 Unit p;
 int supply,fuel,gots,gotf,type,i,who;
 int sneeded[NUM_TEAMS],fneeded[NUM_TEAMS];
 float sratio[NUM_TEAMS],fratio[NUM_TEAMS];
 GridIndex x,y;
 MissionRequestClass mis;
 VuListIterator myit(AllParentList);
 VuListIterator objit(AllObjList);

 // zero supply values
 o = GetFirstObjective(&objit);
 while (o)
 {
 o->static_data.local_data = 0;
 o = GetNextObjective(&objit);
 }

 // zero record values
 for (i=0; i<NUM_TEAMS; i++)
 sneeded[i] = fneeded[i] = 0;

 // Calculate needs
 p = GetFirstUnit(&myit);
 while (p)
 {
 who = p->GetTeam();
 if (p->GetDomain() not_eq DOMAIN_AIR or p->GetType() == TYPE_SQUADRON)
 {
 sneeded[who] += p->GetUnitSupplyNeed(FALSE);
 fneeded[who] += p->GetUnitFuelNeed(FALSE);
 }
 p = GetNextUnit(&myit);
 }

 // Calculate what ratio of needed supply we're going to give out
 for (i=0; i<NUM_TEAMS; i++)
 {
 if (sneeded[i] > 0)
 sratio[i] = (float)TeamInfo[i]->supplyAvail / sneeded[i];
 else
 sratio[i] = 0.0F;
 if (sratio[i] > 1.0F)
 sratio[i] = 1.0F;
 if (fneeded[i] > 0)
 fratio[i] = (float)TeamInfo[i]->fuelAvail / fneeded[i];
 else
 fratio[i] = 0.0F;
 if (fratio[i] > 1.0F)
 fratio[i] = 1.0F;
 }

 // Supply units
 p = GetFirstUnit(&myit);
 while (p)
 {
 who = p->GetTeam();
 if (p->GetDomain() not_eq DOMAIN_AIR or p->GetType() == TYPE_SQUADRON)
 {
 supply = p->GetUnitSupplyNeed(FALSE) * sratio[who];
 fuel = p->GetUnitFuelNeed(FALSE) * fratio[who];
 if (supply > TeamInfo[who]->supplyAvail)
 supply = TeamInfo[who]->supplyAvail;
 if (fuel > TeamInfo[who]->fuelAvail)
 fuel = TeamInfo[who]->fuelAvail;
 if (fuel or supply)
 {
 p->GetLocation(&x,&y);
 o = FindNearestFriendlyObjective(p->GetTeam(),&x,&y,0);
 if (o)
 {
 s = FindNearestSupplySource(o);
 if (s)
 {
 TeamInfo[who]->supplyAvail -= supply;
 TeamInfo[who]->fuelAvail -= fuel;
 gots = supply;
 gotf = fuel;
 if (SendSupply(s,o,&gots,&gotf));
 SupplyUnit(p,supply,gots,fuel,gotf);
 }
 }
 }
 RepairUnit(p);
 }
 p = GetNextUnit(&myit);
 }

 // Calculate current supply percentages (for UI)
 int shave[NUM_TEAMS]={0},swant[NUM_TEAMS]={0},fhave[NUM_TEAMS]={0},fwant[NUM_TEAMS]={0};
 p = GetFirstUnit(&myit);
 while (p)
 {
 who = p->GetTeam();
 if (p->GetDomain() == DOMAIN_LAND or p->IsSquadron())
 {
 int ths = p->GetUnitSupplyNeed(TRUE);
 int thf = p->GetUnitFuelNeed(TRUE);
 shave[who] += ths;
 fhave[who] += thf;
 swant[who] += ths + p->GetUnitSupplyNeed(FALSE);
 fwant[who] += thf + p->GetUnitFuelNeed(FALSE);
 }
 p = GetNextUnit(&myit);
 }
 for (i=0; i<NUM_TEAMS; i++)
 {
 if (swant[i])
 TeamInfo[i]->currentStats.supplyLevel = shave[i]*100/swant[i];
 if (fwant[i])
 TeamInfo[i]->currentStats.fuelLevel = fhave[i]*100/fwant[i];
 }


 // Reset loss values, set supply values, and request missions
// CampEnterCriticalSection();
 o = GetFirstObjective(&objit);
 while (o)
 {
 supply = LOBYTE(o->static_data.local_data);
 fuel = HIBYTE(o->static_data.local_data);
 if (supply > 5 or fuel > 5)
 {
 o->SendObjMessage(o->Id(),FalconObjectiveMessage::objSetSupply,(short)(supply),(short)(fuel),0);
// o->SendObjMessage(o->Id(),FalconObjectiveMessage::objSetLosses,0,0,0);
 type = o->GetType();
 if (type == TYPE_ROAD or type == TYPE_INTERSECT)
 {
 // Request an interdiction mission
 mis.requesterID = o->Id();
 o->GetLocation(&mis.tx,&mis.ty);
 mis.vs = o->GetTeam();
 mis.who = GetEnemyTeam(mis.vs);
 mis.tot = Camp_GetCurrentTime() + (30+rand()%480)*CampaignMinutes;
 mis.tot_type = TYPE_NE;
 mis.targetID = FalconNullId;
 mis.mission = AMIS_INT;
 mis.roe_check = ROE_AIR_ATTACK;
 mis.context = enemySupplyInterdictionZone;
// mis.priority = (supply+fuel)/20;
 mis.RequestMission();
 }
 if (type == TYPE_BRIDGE or type == TYPE_DEPOT or type == TYPE_PORT)
 {
 // Request an interdiction strike mission
 mis.requesterID = o->Id();
 o->GetLocation(&mis.tx,&mis.ty);
 mis.vs = o->GetTeam();
 mis.who = GetEnemyTeam(mis.vs);
 mis.tot = Camp_GetCurrentTime() + (30+rand()%480)*CampaignMinutes;
 mis.tot_type = TYPE_NE;
 mis.targetID = o->Id();
 mis.mission = AMIS_INTSTRIKE;
 mis.roe_check = ROE_AIR_ATTACK;
 if (type == TYPE_BRIDGE)
 mis.context = enemySupplyInterdictionBridge;
 if (type == TYPE_DEPOT)
 mis.context = enemySupplyInterdictionDepot;
 if (type == TYPE_PORT)
 mis.context = enemySupplyInterdictionPort;
// mis.priority = (supply+fuel)/20;
 mis.RequestMission();
 }
 }
 o = GetNextObjective(&objit);
 }
// CampLeaveCriticalSection();
 return 1;
 }

*/
