# The campaign supply and production engine

A running reference on how FreeFalcon's campaign economy actually works, written
from the code rather than from lore. Every claim here cites where it lives so it
can be re-checked when the code moves.

Started while building the Logistics map overlays (`C_Map::ShowCampaignOverlay`,
`src/ui/src/campaign/cmap.cpp`). Extended as questions come up — append, don't
rewrite, and correct in place when something turns out to be wrong.

Primary source: `src/campaign/campupd/supply.cpp`.

---

## The one-paragraph version

Power plants scale what producers make. Producers fill three national pools —
supply, fuel, replacements. Each pool is rationed across every unit that needs it,
capped at half of what is asked for. What a unit is owed is then pathfound to it
across the road network, losing a little at every hop. Supply becomes ammunition,
fuel becomes fuel, replacements become vehicles and airframes.

Bombing acts on **production**. The ground war acts on **delivery**. Neither can
reduce the other side to zero.

---

## The chain, as it actually exists

Six stages. Note that power is not a stage that "comes first" in sequence — it is
a multiplier applied inside stage 2.

### 1. Power — a multiplier, not a supply

`ProduceSupplies`, supply.cpp:79

```cpp
po = FindNearestFriendlyPowerStation(AllObjList, o->GetTeam(), x, y);
power = po ? po->GetObjectiveStatus() : 0;
s = o->GetObjectiveDataRate() * power / 100;
```

Each producer is scaled by the status of **its nearest non-hostile power plant**.
`FindNearestFriendlyPowerStation` (`camplib/find.cpp:1251`) is plain
nearest-neighbour — no transmission network, no real range limit — so the set of
producers a plant feeds is exactly its Voronoi cell.

`TYPE_NUCLEAR` and `TYPE_POWERPLANT` are treated **identically**. A nuclear plant
carries no special weight; its importance is entirely positional, i.e. how many
producers happen to be nearest to it.

Only active when `g_bPowerGrid` (default on). With it off, `power = 100` always.

### 2. Production — type decides which pool

`ProduceSupplies`, supply.cpp:73-165. Output is always
`class_data->DataRate * GetObjectiveStatus() / 100`, so damage scales output
linearly, then power scales it again.

| Objective type | Feeds |
| --- | --- |
| `TYPE_FACTORY`, `TYPE_ARMYBASE`, `TYPE_DEPOT`, `TYPE_PORT` | supply **and** replacements |
| `TYPE_REFINERY` | fuel **only** |
| `TYPE_CITY` | nothing |

The pools do not convert. A refinery cannot make ammunition; a factory cannot make
fuel. This is decided purely by the objective's type.

Korea census (`g_bLogCampProducers 1`, day 1, everything at 100%):

| type | count | total DataRate | each |
| --- | --- | --- | --- |
| Factory | 113 | 23950 | 212 |
| Army base | 59 | 7500 | 127 |
| Depot | 26 | 3800 | 146 |
| Port | 41 | 4400 | 107 |
| Refinery | 8 | 5400 | **675** |

### 3. The pools — national, and leaky

supply.cpp:210-250. One bucket per team per currency. There is no "this factory
supplies that division" — everything pools.

```cpp
supply[who]       = AUTOMATIC_SUPPLY + (supply[who] / 5)  * rate * actionBonus;
fuel[who]         = AUTOMATIC_SUPPLY +  fuel[who]         * rate * actionBonus;
replacements[who] = AUTOMATIC_REPLACEMENTS + (repl / 40)  * rate * actionBonus;

supply[who] = (TeamInfo[who]->GetSupplyAvail() / 2) + supply[who];
```

Two things matter here:

- **Fuel enters undivided** while supply is divided by 5 and replacements by 40.
  A refinery's 675 carries full weight; a factory's 212 becomes ~42. This is why
  the eight refineries are the highest-value fixed targets in the theater.
- **The pool halves every tick** before new production is added. There is no
  stockpile. Losses show up within campaign hours, and so does recovery.

`AUTOMATIC_SUPPLY` (5) and `AUTOMATIC_REPLACEMENTS` (1) arrive regardless — a
floor no amount of bombing gets past.

### 4. Rationing — capped at half

`SupplyUnits`, supply.cpp:455-500

```cpp
sratio = TeamInfo[who]->GetSupplyAvail() / sneeded;
if (sratio > MAX_SUPPLY_RATIO) sratio = MAX_SUPPLY_RATIO;   // 0.5
```

Separate ratios for supply, fuel and replacements, each pool against its own total
need. **Even a pristine economy only ever fills half of what the army asks for.**

### 5. Delivery — pathfound to the unit

For each eligible unit (supply.cpp:640-670):

1. `FindNearestFriendlyObjective(who, &x, &y, 0)` — nearest friendly objective to
   the unit.
2. `FindNearestSupplySource(o)` — walks objective **links** outward from there
   until it hits an `IsSupplySource()`: `TYPE_CITY`, `TYPE_PORT`, `TYPE_DEPOT`,
   `TYPE_ARMYBASE`, provided it is not frontline or secondline.
3. The full amount is deducted from the pool.
4. `SendSupply(s, o, &gots, &gotf)` pathfinds source → destination and deposits a
   running tally at each `TYPE_ROAD` / `TYPE_INTERSECT` / `TYPE_RAILROAD` /
   `TYPE_BRIDGE` it crosses, losing `NodeSupplyLoss()` percent per hop — 2% if
   the node is intact, more if it has been bombed (see
   [Roads and bridges](#roads-and-bridges)).
5. The unit receives `gots` / `gotf` — what survived the journey.

Deduction happens **before** the journey, so transit losses are real waste, not
returned to the pool. No friendly path means `SendSupply` returns 0 and the unit
gets nothing that tick.

### 6. Consumption — what the currencies become

**Battalions** (`camptask/battalio.cpp:1499`): supply is a 0-100 **percentage**,
and fuel is folded into it at half weight for wheeled and tracked units.

```cpp
if (GetMovementType() == Wheeled or Tracked) supply = supply + fuel / 2;
got = (supply * 100) / GetTotalVehicles();
SetUnitSupply(GetUnitSupply() + got);
```

That percentage scales firepower directly (`camplib/unit.cpp:5453`):

```cpp
if (wc > 2)  wc = (wc * GetUnitSupply()) / 100;    // supply % of shots
else if (rand() % 100 > GetUnitSupply())  wc = 0;  // supply % of vehicles can fire
```

**Supply is ammunition.** A battalion at 40% fires 40% as much.

**Squadrons** (`camptask/squadron.cpp:911`): fuel is real jet fuel (`UseFuel`);
supply refills `SetUnitStores` — the squadron's **munitions magazine**, per weapon
type, against `SquadronStoresDataTable`. Starve a squadron and specific weapons
stop being available to load.

**Replacements** are `ChangeVehicles()` — new vehicles and airframes to rebuild
losses. Every supply producer makes these from the same number in the same step,
so hitting production hits reinforcement too.

---

## Resupply cadence

A unit is only eligible when
`CurrentTime - LastResupplyTime > GetUnitSupplyTime()` (supply.cpp:525,
`camplib/unit.cpp:4596`):

| Unit | Interval |
| --- | --- |
| Battalion, `GRO_ATTACK` | **20 minutes** |
| Battalion, otherwise | **3 hours** |
| Squadron, team on ground offensive | `ActionTimeOut / 2` hours |
| Squadron, otherwise | `ActionTimeOut` hours |

Attacking battalions resupply nine times more often than defending ones — an
offensive is far more sensitive to a cut supply line than a dug-in defence.

---

## What destroying things actually does

### Producers (factory, refinery, depot, port, army base)

**Cuts production, linearly with damage.** Also cuts replacements, since both come
from the same number. No buffer absorbs it.

**Does not affect delivery.** `IsSupplySource()` (`camplib/objectiv.cpp:2222`)
checks type and front-line status and **never checks damage**. A depot bombed flat
is still a perfectly valid supply origin.

### Cities / political objectives

**Nothing.** Cities produce nothing at all, and as sources they are immune to
damage for the reason above. They appear on supply routes because they are valid
origins, not because they contribute.

### Roads and bridges

**Stock: nothing.** **FreeFalcon: throughput, via `SupplyInterdiction`.**

#### Why the stock field could not simply be switched on

Loss per node was `GetObjectiveSupplyLosses() + 2` — a flat 2%, bombed or
pristine, because `obj_data.losses` is dead in two independent ways:

1. **Nothing writes it non-zero.** The only `objSetLosses` send in the tree is at
   supply.cpp:934 — which is inside the **block comment spanning lines 806–980**,
   the legacy `SupplyUnits` that the live one at line 364 replaced. It is not a
   disabled line inside working code; the whole function around it is gone.
2. **The live path zeroes it.** `objSetSupply` — sent every tick by the live
   `SupplyUnits` (supply.cpp:724) — calls `SetObjectiveSupplyLosses(0)` in its
   handler (`objectivemsg.cpp:111`). On exactly the nodes a supply run just
   recorded traffic through. So anything written to the field would be cleared the
   same tick that mattered.

Reviving the field was therefore not an option. The value is derived where it is
**read** instead.

#### What it does now

`NodeSupplyLoss()` (supply.cpp, above `SendSupply`) derives the per-hop loss from
the node's **own `GetObjectiveStatus()`** — the same 0–100 the production maths
and the map's Damage layer use, rolled up from per-feature damage by
`ObjectiveClass::CalcStatus`:

```
loss% = 2  +  worst x (100 - status)/100  x  SupplyInterdiction/100
worst = 50 for TYPE_BRIDGE, 15 for road / intersect / railroad
clamped at 95
```

| Node | Status | Loss for that hop |
|---|---|---|
| Bridge | 100 (intact) | 2% |
| Bridge | 50 | 27% |
| Bridge | 0 (dropped) | 52% |
| Road | 0 | 17% |

A bridge bites harder than a road because that is what a bridge is — there is no
going round one, where a cratered road is a detour. Two dropped bridges on a route
put roughly **a quarter** of what was sent through it.

Taking the number from `status` rather than storing a second one means **repair
unwinds it for free**: status climbs as features are rebuilt, the route re-opens on
its own, and there is no parallel mechanism to keep in step and no save-game field
to migrate.

`SupplyInterdiction` (cfg, default 100) scales it. **0 restores stock exactly.**
Above 100 for a harsher campaign — the 95% clamp means a closed route starves a
front rather than erasing a convoy outright.

#### The half that is still missing

Link **costs** remain static. `LinkCampaignObjectives`
(`campupd/campaign.cpp:261`) computes them once at campaign build from terrain and
nothing rewrites them. Rather than rewrite them, damage is applied at SEARCH time
(2026-09-27, `PathDamageCost`, default 100): `GetObjectiveMovementCost` (`camplib/path.cpp`)
scales the ground cost of ENTERING a damaged road/junction/rail node by up to x2 and a
damaged bridge by up to x4, linear in objective status, capped at 254 (dear, never closed —
a 0% bridge is still blocked by the older engineer check). It applies to everything that
walks the objective graph (supply, ground units), ground move types only. Not flown yet: watch
whether columns now detour around a half-dropped bridge, and whether any front starves.

#### Why you never see a road on the map

They are not drawn. `filters.cpp:50` files `TYPE_ROAD` under `_OBTV_OTHER`, a
category the campaign map leaves off by default, so the only supply-network icon
that shows is the bridge. That is a display filter, not evidence the objectives are
absent — see below.

#### Unverified: can a ROAD be damaged at all?

Damage only registers through `CalcStatus`, which walks
`class_data->Features`. A type with **no features** can never drop below status 100
however much ordnance lands on it — which would make the above a bridges-only
mechanic regardless of the knob. The campaign requests `AMIS_INT` against roads
with `targetID = FalconNullId` (an area mission, no specific target), which hints
that way. The class table is theater data, not code, so `LogCampProducers` now
reports `avgFeatures` for the network types as well — one run of the Production
overlay answers it. **Pending a log.**

Two things argue the objectives themselves do exist even though nothing draws
them: the live supply loop deposits traffic at `TYPE_ROAD` nodes
(`supply.cpp:352`), and the campaign requests `AMIS_INT` against them
(`supply.cpp:777`). If the theater held none, both branches would be dead code.

Read the census like this:

| `avgFeatures` | `count` | Meaning |
|---|---|---|
| 0 | > 0 | Roads exist but cannot be damaged — interdiction is bridges-only |
| > 0 | > 0 | Roads are bombable; the damage curve applies to them too |
| — | 0 | No road objectives in this theater; only bridges carry the network |

### What does cut supply

- **Ownership.** `GetObjectivePath(..., s->GetTeam(), ...)` is team-aware. No
  friendly path, no delivery.
- **The front line.** A source that becomes frontline or secondline stops being a
  source, undamaged.

So: **bombing acts on production and on throughput, the ground war acts on
delivery outright.**

---

## Target value, derived

1. **Refineries** — 8 in the theater, ~675 each, the only fuel source, and fuel
   enters the pool undivided. One refinery is 12.5% of all fuel.
2. **Power plants feeding clusters** — a second multiplication on the same output.
   Value is positional; the Power overlay's fan of links shows which ones matter.
3. **Factories** — 60% of supply production, but 113 of them. Attrition, not
   decapitation.

---

## Map overlays that show this

`C_Map::ShowCampaignOverlay`, right-click the campaign map → **Logistics**.

| Layer | Shows |
| --- | --- |
| Power coverage | A line from each producer to the plant that feeds it; bright where that plant is down |
| Supply flow | The road network carrying traffic, drawn as edges between linked objectives; sources largest, bridges next |
| Production | Each producer sized against the theater's biggest |
| Target damage | Every objective tinted by damage taken |

Relevant config: `SupplyMapThreshold` (how much traffic before a node is recorded
at all — stock 5 hides almost everything, default here 0), `LogCampProducers`
(the census table above), `PowerGrid`.

---

## Airlift: the hidden income (found 2026-10-13, campsim)

Every airlift flight that lands credits its team's national pools with `3 x CountUnitElements() / 3` supply, fuel **and**
replacements (`update.cpp`, `WP_LAND` case, `AIRLIFT_SUPPLIES/FUEL/REPLACEMENTS` = 3). Nothing is taken from anywhere: the
departure base's stock is not touched, there is no cargo, and the credit does not depend on the aircraft type or count (a pair of
transports is credited 12 points for DPRK, 8 or 12 for ROK, a number from the squadron class), on the destination's need or on
its runway state (36 of 392 DPRK landings were at damaged airbases and were credited anyway).

How the flights come about: the ATM cycles through each team's airbases and requests one airlift into one friendly airbase per
cycle (`atm.cpp` ~913, time on target 90 minutes out); the squadron has to be at another airbase at least 40 km away (`atm.cpp`
~1773); the flight size is randomised downward (`package.cpp` ~846). campsim `airliftlog=1` prints one `AIRLIFTLAND` line per
credited landing. Korea, 3 days, 898 landings: DPRK 392 (5.6 an hour, three squadrons, 20 distinct home-landing pairs, median 117 km
apart, all between its own airbases), ROK 506 (7.2 an hour, three squadrons, 12 pairs, median 208 km, about a quarter of them to or
from Tsuiki in Japan).

What it is worth (5 days, DPRK, fix on): about 600 landings = 7,000 each of supply, fuel and replacements. Production at baseline
makes about 20,800 supply and 1,800 replacements, so airlift is ~25% of supply and ~80% of replacements; with the power plants
held down (production 630) it is nearly the whole economy. That is why destroying production never showed at the front. Switching
only the airlift credit off (`noairlift=1`) cut baseline ground replacements from 3,516 to 1,468 and changed little else; with
power plants held down **and** no airlift, battalion supply slid from 96% to 74% (front 66%), 23 battalions fell under 25% supply,
ground replacements fell to ~30 and DPRK had ~25% fewer ground vehicles by hour 72-119. The landings happen either way (608 vs 598).

Consequences: shooting down airlift flights or cratering their destination runways is the only way to hit this income today; the
flights themselves are base-to-base shuttles. Candidate changes (none made): cap the credit per team per day, scale it by aircraft
type and count, require a supply deficit at the destination, require a usable runway, or remove it.

### Switches: AirliftRunway and AirliftNeed (both on by default, 0 = stock)

- `g_bAirliftRunway` (cfg `AirliftRunway`): a landing is credited only if the airbase has a runway that is not destroyed (the runway-point
  test `ObjectiveClass::CalcStatus` already uses to cap an airbase's status).
- `g_bAirliftNeed` (cfg `AirliftNeed`): each currency is credited only up to what is short around the destination: supply and fuel =
  the shortfall of the team's battalions within 40 km of the airbase; replacements = missing vehicles in those battalions plus missing
  aircraft in the squadrons based there. Squadron supply need is not counted (side effects, and it is mostly the gap to a never-drawn
  weapon-stores table).

campsim (`airliftneed=`, `airliftrunway=`, `AIRLIFT` lines with offered / credited / refused / cut): Korea, 5 days, 2 seeds, DPRK. Baseline:
the flights bring ~7,080 supply and replacements; with both rules supply credited is 2,444 (35%), replacements 6,467 (91%); 2% of landings
(7) were at cratered runways. Power plants held down: supply credited 4,769 of 7,044 (68%), battalion supply 86% mean / 83% at the front
(unrestricted airlift 93% / 92%; no airlift at all 74% / 66%). The need cap bites supply and barely touches replacements, because every
base always has far more than 12 points of missing aircraft and vehicles nearby, so replacements remain a free stream well above
production (1,800). With every DPRK airbase destroyed the flights mostly cannot take off (offered fell from ~7,000 to 84), DPRK ground
replacements fell from ~3,500 to ~1,400. Not yet done: a tighter replacement cap (ground shortfall only, or scaled by flight size).

## Hubs, bombing and the network: measurements (2026-10-13)

**What each producer type is worth** (Korea save0, campaign start, `python tools/campsim/supply_hubs.py`; "rate" = class DataRate, output is
rate x damage% x nearest plant%; sources are what `IsSupplySource` accepts):

| | FreeFalcon 6 north / south | Falcon 4.0 north / south | BMS 4.37 north / south |
|---|---|---|---|
| Factory | 55 (10,650) / 58 (13,300) | 56 (9,000) / 61 (11,500) | 12 (1,200) / 28 (2,800) |
| Army base | 15 (1,900) / 44 (5,600) | 15 (30) / 45 (90) | none |
| Depot | 13 (2,100) / 13 (1,700) | 13 (1,300) / 13 (1,300) | 1 (rate 1) / 1 (rate 1) |
| Port | 20 (2,000) / 21 (2,400) | 20 (6,000) / 21 (6,300) | 0 / 18 (900) |
| Refinery | 3 (2,000) / 5 (3,400) | 3 (1,800) / 5 (3,000) | 0 / 1 (100) |
| Power / nuclear plants | 14 / 22 | 14 / 22 | 0 / 5 |
| Cities (sources) | 167 / 194 | 167 / 194 | 45 / 34 (rate 1) |
| All objectives | 2,700 | 2,659 | 825 |

FF6 raised the army-base rate from 2 / 6 to 127 and cut ports from 300 to 100 relative to vanilla. **BMS 4.37 has fewer hubs, not more**:
its Korea campaign file holds 825 objectives (no army bases, 40 factories at rate 100, 18 ports at 50, one refinery), so a BMS
economy is far leaner; BMS's own supply code is not public, so whether it even runs `ProduceSupplies` is unknown.

**What one object of each type is worth** (FF6 Korea save0, both sides; DataRate is the class value the editor shows as "rate"; pool points per
day at 100% damage and power, measured against the simulator: supply = rate x 0.3 (DataRateModSup 1.5 / 5), replacements = rate / 40, fuel = rate x 1.5):

| Object type | Feeds | Count | Rate each (mean, range) | Supply / day each | Replacements / day each | Fuel / day each |
|---|---|---|---|---|---|---|
| Factory | supply + replacements | 113 | 212 (100-400; 58 at 100, 36 at 300, 18 at 400) | ~64 | ~5.3 | - |
| Army base | supply + replacements | 59 | 127 (100 or 200) | ~38 | ~3.2 | - |
| Depot | supply + replacements | 26 | 146 (100 or 200) | ~44 | ~3.7 | - |
| Port | supply + replacements | 41 | 107 (100, three at 200) | ~32 | ~2.7 | - |
| Refinery | fuel only | 8 | 675 (600 or 800) | - | - | ~1,010 |
| Nuclear plant | scales producers | 27 | 396 (class rate unused) | - | - | - |
| Power plant | scales producers | 9 | 300 (class rate unused) | - | - | - |
| City | delivery source only | 361 | 100 (unused) | - | - | - |
| Airbase / airstrip | nothing | 90 | 1-2 | - | - | - |
| Bridge | delivery node (2% loss, more if damaged) | 762 | 100 (unused) | - | - | - |

On top of that every team gets 5 supply and 5 fuel and 1 replacement per hourly production call regardless, and airlift landings
credit 12 of each per landing (see "Airlift"). DPRK: 15,350 supply capacity a day is about 4,600 supply and 380 replacements a day.

**Bombing a hub type** (DPRK, supply+replacement capacity at hour 2, baseline 15,350 a day): all 15 army bases -12%, all 20 ports -12%,
all 13 depots -12%, all 55 factories **-50%** (1,200 fuel from the 2 refineries is untouched by all four). Production is linear in damage
and each hub contributes only its own rate; a bombed army base, port or depot still counts as a delivery source (`IsSupplySource` ignores damage).

**The delivery network** (5 days, DPRK, 2 seeds): destroying all 464 DPRK bridges (5,906 features; there are only 9 road objectives and no
railroad objectives) cut delivered supply by about 60% and raised transit loss from 0.2% to 3.4%, but battalion supply stayed at 96%
(front 92%): the bridges also stop units from moving, which is where most supply is spent, so need fell with delivery. Roads: nothing,
they barely exist as objectives. Transit loss at baseline is 0.2%: nearly every unit is within one or two links of a source.

## Experiment: regional stocks (`SupplyStocks`, off by default)

Depots and army bases become **stores**: each holds a local stock of supply and fuel (class DataRate x `StoreCapPct`, default 100, the same number of
each, at least 20), and they stop producing unless `StoresProduce`. Factories, ports and refineries still fill the national pool. Each hourly
production call the pool tops the stores up, none taking more than `StoreFillPct` (default 10) of its capacity, shared out by how short they are.
A battalion asks the nearest store behind it (own depot or army base, not front line; `FindNearestSupplyStore`) for up to half of what it is short
of and draws that from the store's stock instead of the pool; the delivery path and losses are unchanged. A store that is captured loses its stock, one
that is damaged loses stock above capacity x status. Squadrons keep using the pool. Stocks are not saved (stores start full on load).
campsim: `stocks=1 storesproduce= storecap=PCT storefill=PCT`, `STORES` lines every 6 hours.

Korea, 5 days, 2 seeds (unit supply %, mean battalion at hour 119, front line in brackets; control = the national pool):

| Scenario | DPRK | ROK | What the stores did |
|---|---|---|---|
| Control (stock model off) | 94% (94%) | 96% (95%) | |
| Stores, default capacity | 94% (94%) | 96% (94%) | nothing: stocks 98% full at the end |
| Stores that also produce | 93% (92%) | 96% (94%) | nothing |
| Tiny stores (cap 10% = 20 points each) | 85% (78%) | 80% (67%) | drained: ~20,000 (DPRK) / ~139,000 (ROK) supply points asked for and not in stock |
| Stores, power plants held down | 90% (88%) | 96% (95%) | DPRK stores down to 71% of capacity, refilled by the floor and airlift |
| Cap 25% + power plants held down | 88% (82%) | 88% (81%) | |
| All depots and army bases held down | 86% (83%) | 93% (89%) | DPRK stock 7% of capacity: stock burned with the bombed stores |

No store within reach: 648 DPRK / 484 ROK resupplies in five days (depots and army bases are far scarcer than the cities and ports the stock model's
"source" accepted) -- those units get nothing, yet their supply barely moved: consumption is small. Ground vehicles and objectives did not differ
from control. Conclusion: with realistic capacities a five-day war never leans on the stores; they bite only when they are small (buffer of a few
days) or when the stores themselves are destroyed or captured, which is the behaviour a regional-stock model is for. A per-store capacity in
days of local consumption, rather than a multiple of the DataRate, would be the next thing to tune.

## Open questions

- Ground **movement** — whether destroyed bridges block or slow unit movement is a
  separate system from supply and has not been traced here.
- Army bases may have roles beyond supply (basing or generating ground units) not
  covered above.
- Whether `TYPE_ROAD` objectives carry any bombable features — see Roads and
  bridges. Decides whether damage-scaled interdiction is a bridges-only mechanic.
- Damage now feeds **route cost** at search time (`PathDamageCost`, 2026-09-27) — compiles,
  not flown. See Roads and bridges.
