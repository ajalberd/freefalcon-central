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
   `TYPE_BRIDGE` it crosses, losing `losses + 2` percent per hop.
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

**Nothing, currently.** Loss per node is `GetObjectiveSupplyLosses() + 2`, and the
only thing that ever sets that value is the `objSetLosses` message — sent from
exactly one place in the tree, supply.cpp:934, which is **commented out**:

```cpp
// o->SendObjMessage(o->Id(),FalconObjectiveMessage::objSetLosses,0,0,0);
```

So losses are permanently 0 and every hop costs a flat 2%, bombed or pristine.
Link costs are static too — `LinkCampaignObjectives` (`campupd/campaign.cpp:261`)
computes them once at campaign build from terrain and nothing rewrites them.

**This looks like an unfinished feature, not a design choice.** The campaign
generates interdiction missions against roads and bridges specifically
(`AMIS_INT`, `enemySupplyInterdictionZone`) — the mechanism those strikes were
meant to drive is disabled. Wiring damage into `SetObjectiveSupplyLosses` would
make road interdiction mean something. **Open question, not yet done.**

### What does cut supply

- **Ownership.** `GetObjectivePath(..., s->GetTeam(), ...)` is team-aware. No
  friendly path, no delivery.
- **The front line.** A source that becomes frontline or secondline stops being a
  source, undamaged.

So: **bombing acts on production, the ground war acts on delivery.**

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

## Open questions

- Ground **movement** — whether destroyed bridges block or slow unit movement is a
  separate system from supply and has not been traced here.
- Army bases may have roles beyond supply (basing or generating ground units) not
  covered above.
- The disabled `objSetLosses` path — see Roads and bridges.
