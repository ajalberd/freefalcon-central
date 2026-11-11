# Supply inventory: every producer, every consumer, every knob

Written 2026-11-01 from the code (branch `campsim-gpu`), with measured numbers from the existing Korea Rolling Fire runs
(`tools/campsim/runs/save0-knew-s1..s3.log`: ROK/Blue = team 2, DPRK/Red = team 6, fixes on, 6-8 days each).
Companion to `CAMPAIGN-SUPPLY-ENGINE.md` (how the engine works) and `SUPPLY-RESEARCH.md` (what the experiments found). This file is the
**ledger**: one place that lists every number that adds to or takes from a team's supply, and what it is worth per day, so the three
what-if questions at the end can be answered as arithmetic first and simulation second.

**Which "Korea" is which (found 2026-11-01, corrects the first draft).** The installed `C:\FreeFalcon6\campaign\SAVE` is the *Korea China
War mod* (its `save0.cam` and `Falcon4.AII` are byte-identical to `MODS\Korea China War`): `DataRateModSup 0.75`, 86 Chinese battalions.
Plain **Korea Rolling Fire** is the pinned test copy `freefalcon-central\tools\campsim\gamework`: `DataRateModSup 1.5`, 43 Chinese
battalions, no start bonus. **Decision (Andrew, 2026-11-01): the China War mod is the baseline.** The measured tables below
(`save0-knew-s1..s3`, 26 Oct) ran against the install, i.e. the mod; the section 9 answers are run with `--game C:/FreeFalcon6` (tags
`mod-s0..s5`). A first, partial set on plain Rolling Fire was stopped unfinished (shelved in `tools/campsim/runs/plain_partial`).
Where this file says "installed" it means the China War mod.

Units used throughout: a **pool point** is what `GetUnitSupplyNeed` counts: one vehicle's full load. A 30-vehicle battalion at 40 %
supply is short 18 pool points. `1 pool point = 100 / vehicles` percentage points for that battalion.

---

## 1. The shape of the economy

```
producers (factory, army base, depot, port | refinery)  x  nearest friendly power plant %  x  own damage %
        |                                                   (ProduceSupplies, once per campaign hour)
        v
3 national pools per team:  SUPPLY   FUEL   REPLACEMENTS
   supply  = 5/h + (sum(rate x mod) / 5)  / 24 + pool/2   <- the old pool is HALVED every hour (no stockpile)
   fuel    = 5/h + (sum(refinery rate))   / 24 + pool/2   <- same halving
   repl    = 1/h + (sum(rate x mod) / 40) / 24 + pool     <- replacements ACCUMULATE (no halving)
   (cap 60,000 on supply and fuel)
        |
        v   SupplyUnits, twice per hour (stage 4 and stage 10)
each eligible unit asks ratio x need x typeBonus; ratio = min(0.5, pool / everyone's need)
        |   path from nearest source (city/port/depot/army base), 2 % lost per road/junction/rail/bridge node
        v
battalion supply % (also fuel at half weight)  |  squadron stores + fuel  |  replacement vehicles/aircraft
```

Bombing a producer lowers the hourly inflow within the hour. Because the pool is halved hourly, the only buffer in the whole system is
**inside the units** (battalions start at 100 %, squadrons at full stores).

---

## 2. Producers (things that add to a pool)

### 2a. Fixed producers (`ProduceSupplies`, `supply.cpp:495`)

Per producer per day (power 100 %, undamaged): supply = `rate x DataRateModSup / 5`, replacements = `rate x DataRateModRepl / 40`,
fuel (refineries only) = `rate`. A producer counts only while `GetObjectiveOldown() == GetOwner()`.

| Type | Feeds | Korea count (DPRK / ROK) | Total rate (DPRK / ROK) | Supply / day each, mod 0.75 (mod 1.5) | Repl / day each |
|---|---|---|---|---|---|
| Factory | supply + repl | 49 / 52 | 9,550 / 11,600 | mean rate 195 -> ~29 (~58) | ~4.9 |
| Army base | supply + repl | 15 / 21 | 1,900 / 2,700 | rate 127 -> ~19 (~38) | ~3.2 |
| Depot | supply + repl | 13 / 6 | 2,100 / 900 | rate 162 -> ~24 (~49) | ~4.0 |
| Port | supply + repl | 18 / 7 | 1,800 / 800 | rate 100 -> ~15 (~30) | ~2.5 |
| Refinery | **fuel only** | 2 / 4 | 1,200 / 2,800 | rate 600-800 -> 600-800 fuel | 0 |
| Power / nuclear plant | multiplier | 14 / 22 | (class rate unused) | scales every producer by its **nearest** plant's status | |
| City | nothing, but a delivery source | 167 / 194 | | | |

(PRODMIX h0 in the logs; stock FF6 Korea has 113 factories, 59 army bases, 26 depots, 41 ports, 8 refineries over both sides.)

**Team daily income, from those totals** (power 100 %, nothing damaged, `NoActionBonusProd=1`, so no action multiplier):

| | Supply/day at mod 0.75 (installed `campaign/SAVE`) | at mod 1.5 (stock `korea1980s/2012`) | Replacements/day | Fuel/day |
|---|---|---|---|---|
| DPRK (15,350 supply rate, 1,200 fuel rate) | 120 + 15,350x0.75/5 = **2,422** | **4,725** | 24 + 15,350/40 = **408** | 120 + 1,200 = **1,320** |
| ROK (16,000, 2,800) | 120 + 2,400 = **2,520** | **4,920** | 24 + 400 = **424** | 120 + 2,800 = **2,920** |
| PRC (team 5), CIS (team 4) | **120** (the floor) | 120 | 24 | 120 |

China and CIS have **no producers in Korea theater**: unless the campsim-only `chinaown` knob is on, their battalions live on the
5-per-hour floor plus airlift. (campsim: 70 of 86 Chinese battalions under 50 % supply by h48 without it.) **The China War mod sets
`g_bChinaOwnTeam 1`, so there China draws on the North's pool** (its 86 battalions are part of Red's demand); the plain game does not.

### 2b. Direct credits to a pool (not from producers)

| Source | Rate | Where | Notes |
|---|---|---|---|
| Hourly floor | **+5 supply, +5 fuel, +1 replacement** per hour per team (120 / 120 / 24 a day) | `supply.cpp:37-38` | Even with every producer dead. 5 % of DPRK supply income at mod 0.75. It is added after the `rate` multiply, so it is a flat amount per hourly call |
| Airlift landing | `3 x elements/3` of **each** of supply, fuel, replacements per landing | `update.cpp:48,605` | From nothing. Korea: 5-6 landings/h DPRK, 7/h ROK before the fixes; with `AirliftRunway` + `AirliftNeed` (on): DPRK 5 days -> supply credited 35 % of offered, replacements 91 % |
| Naval tanker docking | `g_nNavalTankerFuel` = **50** fuel x (missing refinery output %) | `navunit.cpp:760` | Zero while every refinery works |
| `StartOffensive` floor | tops pools up to `StartOffBonusSup/Fuel/Repl` | `team.cpp:3950` | Stock 5000 / 5000 / 1000. **Installed AII sets all three to 0 (off).** |
| Scripted `#SET_..` events | tops pools up to the script's numbers | `cmpevent.cpp:1440` | e.g. the China scripts; floors, not additions |
| Campaign setup | initial pools are in the save | | not read yet |

### 2c. Things that look like producers but are not

* Reinforcements arriving (`AddReinforcements`, `iaction.cpp:2245`) are free: no pool is charged. Battalions raised start at 100 % (or 0 for some).
* `RepairObjectives` is free: no pool access found.
* `ReinforcePilots(2)` on each squadron resupply is free.
* Rail supply trains (`railnet.cpp:1273`) are **conduits**, not producers: `RailTrainLoad` = 120 supply+fuel per arrival, taken from the pool.

---

## 3. Conduits (how pool points reach a unit) and their caps

| Stage | Rule | Value |
|---|---|---|
| Passes | `SupplyUnits` at stage 4 and 10 of 12 five-minute stages | **2 per hour** |
| Eligibility | `now - lastResupply > GetUnitSupplyTime()` | battalion `GRO_ATTACK` role **20 min** (so every pass), other battalions **180 min**, squadron `ActionTimeOut` = 18 h (9 h on offensive) |
| Ratio | `min(0.5, pool / sum of all needs)` for supply, fuel, replacements (`MAX_SUPPLY_RATIO`) | at most half a unit's shortfall x typeBonus per cycle |
| typeBonus | attack role **x6**, squadron **x2**, otherwise x1, plus random -0.25..+0.25 | an attacker asks up to 3x its shortfall: it refills fully in one pass if the pool has it |
| Split (on) | `SupplySplitShares`: pool shared ground/air by recent real use | fixes the dilution by squadrons' unused stores |
| Source | nearest friendly objective -> walk links to the first city/port/depot/army base not on the front | damage never matters; captured sources count at once |
| Transit | path source -> unit, **2 % per road/junction/rail/bridge node** (`NodeSupplyLoss`), +`SupplyInterdiction` x damage (bridge up to +50, road +15, clamp 95) | exact mode on: once per trip. Measured: 4.1 links (DPRK) and 7.9 links (ROK) mean path, 0 emptied on the road |
| Fuel to battalions | `got = (supply + fuel/2) x 100 / vehicles` for wheeled/tracked | fuel is worth half a supply point to a truck |
| Replacements | wheel: `shortfall x (1-lambda) x rratio_v`, air: `shortfall x lambda x rratio_a` | `lambda = w*air/(w*air+ground)`, w = `RelSquadBonus` = 4; capped 0.5 |

---

## 4. Consumers (things that take away)

### 4a. Battalions: supply % is spent on moving, firing and (nominally) idling

| Consumer | Rule | Rate |
|---|---|---|
| **Moving** | `battalio.cpp:1406`: every time `GetUnitMoved() > 20` steps (one grid cell per step) it burns **2 percentage points** | ~**0.095 % per km** (2 % per ~21-30 km). 100 km of driving = ~9.5 %. Ownership of the road does not matter |
| **Firing** | `unit.cpp:5150`: `supply -= supply x shots fired / total ammunition`, shots scaled by supply %; each fire step | tiny at ground targets; heavier at aircraft (air defence battalions) |
| **Idle** | `battalio.cpp:1457`: 2 points when a unit has waited > 3 x DEG_TO_SEC, `g_bNeutralNoUse` exempts neutral teams | in practice ~0 (rear units use nothing) |
| Multipliers (campsim only) | `usemult=`, `useteam=T:M`, `sxattackuse=` | not in the game |

**Measured, Korea Rolling Fire, per day, 3 seeds** (percentage points summed over all battalions, from `SUPUSE`; pool points delivered from `SUPSPLIT`):

| | battalions | moving | firing at aircraft | firing at ground | idle | pool points delivered to battalions | to squadrons |
|---|---|---|---|---|---|---|---|
| ROK (Blue) | 211 | 4,800-6,200 (~25 per battalion) | 230-320 | 48-56 | ~0 | **2,120-2,320** | 29-40 |
| DPRK (Red) | 374 | 3,060-3,150 (~8 per battalion) | 950-1,420 | 27-33 | ~0 | **990-1,120** | 60-76 |

Two things follow. (1) Battalion supply is a **fuel gauge**: movement is 85-95 % of use, ground combat is under 1 %. A bloody stalemate
costs nothing; a long advance costs everything. (2) Deliveries (~100 % of consumption since units sit at 90-97 %) match
5,000 pct-points x ~43 vehicles / 100 for ROK and 3,100 x ~34 / 100 for DPRK, a consistency check on the vehicle-weighted maths.

### 4b. Squadrons: munitions and fuel

| Consumer | Rule | Rate |
|---|---|---|
| Munitions loaded onto a sortie | `UpdateSquadronStores`: stores table down by `ceil(weapons x planes / 4)` per weapon type (20 shots = 1 supply point in the need maths) | measured **1,290-1,700 pool points/day loaded** per side |
| Returned unused | `ResupplySquadronStores` on abort/return | **92-95 % comes back** (1,190-1,600 a day) |
| **Net squadron use** | loaded - returned | ROK ~100-120 a day, DPRK ~150-200 a day: **5-15 % of battalion use** |
| Fuel | sortie takes `fuelAvail x planes / 100` squadron fuel units; refills `fuel x 10,000` units per pool fuel point (`SUPPLY_PT_FUEL`) | **not measured.** Quick arithmetic says it cannot bind: a squadron starts with `uc->Fuel x 384` units and a four-ship uses ~hundreds. To verify |
| Squadron fuel *need* | `uc->Fuel x aircraft x SQUADRON_MISSIONS_PER_HOUR(4) x 2 x ResupplyInterval(120)/60` | enormous (assumes 4 sorties per plane per hour for 4 hours); it only dilutes the fuel ratio |
| Aircraft lost | replaced from the REPLACEMENTS pool | see 4c |

### 4c. Replacements: vehicles and aircraft

| Consumer | Rule | Rate |
|---|---|---|
| Ground vehicle replacement | up to `shortfall x (1 - lambda) x rratio_v`, only when pool > 12 | pool is spent as units are short; **no cap per day other than the pool** |
| Aircraft replacement | `shortfall x lambda x rratio_a` | squadron priority x4 |
| Observed | DPRK ground got ~3,500 in 5 days after the split fix (about 700 a day, of which airlift ~80 %); the unspent pool reached **4,100** by h239 | production is 408/day: replacements are **not** what limits DPRK; delivery (no source / no route) is |

### 4d. Waste (consumers that produce nothing)

| Sink | Size |
|---|---|
| **Hourly pool halving** (supply and fuel) | Anything not delivered within ~2 hours is half-lost each hour. DPRK: ~1,100 delivered out of ~2,420 produced a day, so about half of its production is **wasted** because nothing asks for it. ROK delivers ~93 % of production |
| Transit loss | 2 % per node: 0.2 % of volume at baseline (units are 1-3 links from a source) |
| Clamp | `SetUnitSupply` caps at 100 %: attackers asking 3x shortfall waste the excess (not measured) |
| Pool cap | 60,000 (never reached) |
| Over-delivery to squadrons | stores gap to a table they never draw; the SplitShares fix limits it |

---

## 5. What supply actually does to a unit (does it "matter"?)

| Effect | Where | Strength |
|---|---|---|
| Firepower | `unit.cpp:5743`: shots = shots x supply % (3+ weapons) or a supply % chance for each (1-2) | linear |
| Combat strength | `AdjustForSupply() = supply/100 x (100-fatigue)/100 x morale/100 x experience` (`gndunit.cpp:2816`, `update.cpp:399`) | linear in supply: a 50 % battalion fights as half |
| Can fight at all | `haveWeaps` needs supply > 30 | hard cut |
| Orders | reserve can go to the front only at >= 50 %, a won objective is held at >= 50 %, a capture order is kept at >= 20 % | soft |
| Morale | **none** in stock (campsim `starvemorale=` adds it) | supply never touches morale, so a starved unit keeps its orders |
| Move | none: a unit at 0 % still moves (the burn stops at `supply > 2`) | the army never stops for fuel |

So a starved battalion is weak, not stopped. That, plus movement being the only meaningful burn, is why supply has never bound in a
5-day war.

---

## 6. Knob registry

### 6a. `Falcon4.AII` (data, per campaign)

| Key | Stock Korea | Installed `campaign/SAVE` | Effect |
|---|---|---|---|
| `DataRateModSup` | 1.5 | **0.75** | supply-rate multiplier (the single biggest production dial) |
| `DataRateModRepl` | 1.0 | 1.0 | replacement-rate multiplier |
| `RelSquadBonus` | 4.0 | 4.0 | squadron weight in the replacement split |
| `NoActionBonusProd` | 1 | 1 | 1 = no x1/x2 action bonus on production |
| `NoTypeBonusRepl` | 1 | 1 | uses the new split |
| `ResupplyInterval` (MIN_RESUPPLY) | 120 | 120 | squadron fuel need horizon, minutes |
| `ActionTimeOut` | 18 | 18 | squadron resupply interval hours (9 on offensive) |
| `StartOffBonusSup/Fuel/Repl` | 5000/5000/1000 | **0/0/0** | pool floor when a team starts an offensive |
| `ObjGroundPathMaxCost` | 500 | 2000 | reach of reserve/ally units (indirect) |

### 6b. `FFViper.cfg` / `f4config.cpp` (game, default in brackets)

`PowerGrid` (on), `SupplyInterdiction` (100), `SupplyMapThreshold` (cfg 0), `SupplyNeedFix` (on), `SupplySplitShares` (on), `ReplSplitFix` (on),
`SupplyExactLoss` (on), `AirliftRunway` (on), `AirliftNeed` (on), `NavalTankerFuel` (50), `StartSupplyTeam/Pct` (0/100), `ProdCutTeam/Pct` (0/100),
`PortCargoHours` (0), `SupplyStocks` / `StoresProduce` / `StoreCapPct` (100) / `StoreFillPct` (10) (experiment, off), `ReserveHold`,
`RailTrainLoad` (120), `RailRunKm` (160), `RailRespawnHours` (12), `NoPlayerPlay` (400; the stock 2 h penalises the player's team),
`LoadoutSquadStoreResupply` (on).

### 6c. Hard-coded in source (no knob)

`AUTOMATIC_SUPPLY` 5, `AUTOMATIC_REPLACEMENTS` 1, `MAX_SUPPLIES` 60,000, pool halving of supply and fuel, replacements divisor 40 and supply
divisor 5, `MAX_SUPPLY_RATIO` 0.5 (campsim `maxratio=`), typeBonus 6 / 2 / 1, move burn 2 per 21 steps, idle burn 2, `SQUADRON_PT_SUPPLY` 20,
`SUPPLY_PT_FUEL` 10,000, `SQUADRON_MISSIONS_PER_HOUR` 4, `GND_OFFENSIVE_SUPPLY_TIME` 20 min / `GND_DEFENSIVE_SUPPLY_TIME` 180 min,
airlift 3 per element-third, `NodeSupplyLoss` base 2 and worst 50 (bridge) / 15 (road), supply 2 passes per hour, production 1 per hour.

### 6c2. What the Korea China War mod's cfg changes (`config/mods/Korea China War.cfg`, read after `FFViper.cfg`)

Supply-relevant lines: `g_bStoresNoProd 1` (**depots and army bases do not produce**: only factories and ports do), `g_nSupplyUseMult 2`
(**battalion supply use x2** for every team, moving and firing), `g_nStartSupplyTeam 6` + `g_nStartSupplyPct 70` (the North's battalions
start at **70 % supply**; this is the "North at 70 %", not a production cut), `g_bChinaOwnTeam 1` (China on the North's pool),
`g_bNeutralNoUse 1` (a neutral team burns nothing). Others: `g_bInitIndependent`, `g_nTurnMode 4`, `g_bNeutralHold`, the allied
counteroffensive, offense bars Blue 40 / China 30 / DPRK 15, air reinforcement fixes. So **the mod's own Red income is factories +
ports only** (~10.6k rate, ~1,700 supply a day) and its burn is **x2 for both sides**. campsim reads this file with `usecfg=1`; a
campsim knob given later overrides the cfg value (my `usemult=` runs replace the mod's x2).

### 6d. campsim research knobs (several are now cfg keys, see 6c2)

`usemult` `useteam` `sxattackuse` (consumption), `startsup` `initsupply` (start level), `noprod` `prodsup` `prodcut` `blow=TYPE:TEAM[:hold]`
`blowat` (production), `maxratio` `gndsupoff` `gndsupdef` (conduit), `sxrear` `sxhops` `sxloss` `sxdelay` `sxown` `sxnocity` `sxscorch` `sxmap`
`sxconvoys` `sxpocket` `sxfront` `sxwheeled` (geography), `stocks` `storecap` `storefill` `storestart` (stores), `noairlift` `airliftneed`
`airliftrunway` (airlift), `chinaown` `neutralnouse` `starvemorale` (teams/morale). Details: `SUPPLY-RESEARCH.md` knob table.

---

## 7. Is the amount reasonable?

| Item | Verdict | Reason |
|---|---|---|
| Production scale vs battalion use | **Plausible order of magnitude, tight for Blue** | ROK income ~2,520 vs ~2,350 delivered (93 %); DPRK ~2,420 vs ~1,150 (47 %). At mod 1.5 both have 2x headroom. Neither side is starved at either setting; nothing needs to be rescaled to *run*, only to *bite* |
| Pool halving (no stockpile) | **Unrealistic, and it is why bombing "shows" instantly but never accumulates** | Real armies hold weeks of stock; here a day of dead factories is invisible because units hold the buffer, then every dead hour is gone for good |
| Battalion burn is movement-only | **Unreasonable** | 85-95 % of use is driving at 0.095 %/km; ground combat is under 1 %. A tank brigade in a week-long battle uses no more than one parked. Real campaigns are ammunition-limited |
| Squadron stores | **Reasonable but irrelevant** | 92-95 % of loaded weapons return; 5-15 % of battalion use; load factor is 4 weapons per point and 20 shots per need point (two different conversions in the code) |
| Fuel | **Squadron fuel decorative; battalion fuel is not** | every battalion resupply asks fuel as well as supply and counts it at half weight, so the fuel pool drains almost as fast as supply (timeline, ROK h77: 6,984 fuel sent vs 7,396 supply; DPRK 4,747 vs 4,378, pool ~50). Squadron fuel cannot bind (derived, not measured) |
| Replacements | **Reasonable income (408-424 a day), badly gated** | the pool piles up unspent (4,100 by day 10) while ground vehicles fall; limit is delivery, not production |
| Airlift credit | **Unreasonable (patched)** | stock credits 12 points of each per landing from nothing = ~25 % of DPRK supply and ~80 % of its replacements; the `AirliftNeed` + `AirliftRunway` fixes cut this to 35 % / 91 % of the offer |
| Hourly floor (+5/+5/+1) | **Fine for Blue/Red, load-bearing for China/CIS** | they have no producers: 120 a day for 86 battalions (in the mod they share the North's pool instead) |
| Ratio cap 0.5 and attacker x6 | **Arbitrary, harmless** | attackers refill in one pass if the pool has it; defenders every 3 h at half shortfall |
| Transit loss 2 % per node | **Irrelevant at 1-8 links** | measured 0.2 % of volume |
| Supply -> combat power | **Linear and strong; nothing else reads it** | `AdjustForSupply` is linear; morale ignores it; a starved unit keeps moving and keeps its orders |

---

## 8. Ledger arithmetic for the three what-ifs (preview, not yet simulated)

Daily DPRK income at mod 0.75, power 100 %, no damage; consumption as measured in section 4 (about 1,150 supply/day, ~150-200 of it squadrons):

| Scenario | Supply/day | vs measured use (1,150) | Replacements/day | Fuel/day |
|---|---|---|---|---|
| All DPRK producers intact | **2,422** | 2.1x | 408 | 1,320 |
| **All factories gone** | 120 + (1,900+2,100+1,800) x 0.75/5 = **990** | 0.86x: short by ~15 % | 24 + 5,800/40 = 169 | 1,320 (factories do not make fuel) |
| All army bases/depots/ports gone, factories intact | 120 + 9,550 x 0.75/5 = 1,552 | 1.35x | 24 + 239 = 263 | 1,320 |
| Everything fixed gone (factory+base+depot+port) | 120 (floor) | 0.10x | 24 | 1,320 |
| Power plants all down | floor only (120 / 24 / 120) | 0.10x | | |
| Realistic loss in a normal war (earlier `PRODMIX`, use x1: rate left 14,300 at h24, 12,145 at h72, 10,947 at h120, 7,627 at h234 of 15,350) | ~2,265 (h24), ~1,940 (h72), ~1,760 (h120), ~1,260 (h234) | 2.0x, 1.7x, 1.5x, 1.1x | | |

At mod 1.5 double the supply rows. The point of the preview: **factories alone are 62 % of the rate, so losing all of them lands DPRK
around its measured use; it is not a collapse.** Everything below that needs the other three classes or the plants.

---

## 9. Planned questions (to answer next, in this order)

The three the user asked, plus the ones the inventory shows are needed to answer them honestly. Each says what to run and what the
answer looks like. Baseline for all: Korea Rolling Fire (`save0`), installed AII (`DataRateModSup 0.75`, no start bonus), fixes on,
`usecfg=1`, `det=1`, 6+ seeds, 10 days, both factions.

**Q0. What is the maximum a unit can consume?** (prerequisite for the three scenarios)
* Battalion: continuous movement at the unit's top speed (km/day x 0.095 %), plus continuous firing at its full rate; per battalion class
  (wheeled, tracked, foot, air defence, artillery) -> pool points/day, summed over each faction's order of battle. Needs the speed table.
* Squadron: aircraft x sorties per plane (the code assumes 4 an hour; the real ceiling is crew rest, loadout, ATM requests) x loadout weight
  x 100 % unreturned -> pool points and fuel per day.
* Replacements: every vehicle and aircraft lost refilled -> points/day.
* Output: a **max-demand ledger** per faction, with and without realistic limits.

**Q0 answer (battalions, movement; China War mod `save0`, class data via `tools/campsim/saves.py`).** Max burn = class `MovementSpeed`
(km/h) x 24 h x 0.095 %/km, never stopping, flat terrain, daylight. Summed over every battalion, in pool points a day (vehicles x
percent / 100), with a day/night mix (x0.87) for a more realistic ceiling:

| Side | Battalions / vehicles | Max move demand, flat out | Day/night mix | Measured delivered (control, day 1) | Multiple |
|---|---|---|---|---|---|
| Blue (ROK 215 + US 52 + Japan 1) | 268 / 9,233 | **4,623** | 4,022 | ROK 1,161 at x1 (1,998 at the mod's x2) | 4.0x (3.5x) |
| Red (DPRK 427 + PRC 86 + CIS 8) | 521 / 13,352 | **~8,608** | ~7,490 | DPRK 1,235 at x1 (1,533 at x2; pool-limited, ratio ~0.3) | 7.0x (6.1x) |

Daily income (pool points), China War mod (`DataRateModSup 0.75`): Blue ~2,520, Red ~2,422 (+ airlift). So **at flat-out movement Blue
needs 1.8x and Red 3.6x its income**; at the stock modifier 1.5 Blue would be at break-even and Red at 1.8x. Firing, squadrons and
replacements add to this but are small (sections 4a-4c). Squadron and replacement maxima are not computed yet (they need the sortie
rate; see the Q0 bullets above).

Caveat: `usemult` multiplies what the AI's battalions actually do, it does not make them drive. It is the closest existing lever to
"every unit burns at the maximum rate". **The mod ships `SupplyUseMult 2`**, so the 26 Oct logs behind section 4a (and the 1,998 /
1,533 above) are already at x2; at x1 the day-1 deliveries are 1,161 / 1,235. Blue's flat-out maximum is therefore ~x4 and Red's ~x7:
scenarios s9/s10 use Blue x4, Red x8; s1-s3 used Blue x2 (the mod's own setting) and Red x6. A true forced-burn knob would need a campsim rebuild (the uncommitted `main.cpp` and campaign edits in
this worktree make that a separate decision).

**Q1. Would max-rate use matter with all factories intact?** Add a campsim knob `burn=MAX` that debits every battalion and squadron at
the Q0 rate regardless of behaviour (pure accounting, so the answer is independent of how the AI moves). Measure: pool vs demand each
pass, the ratio cap, battalion supply % over time (front and rear), days until the front falls under 50 %, effect on objectives held.

**Q2. Same, with every factory gone** (`blow=factory:TEAM:hold` from hour 0, plus the version with all four hub types and the version with
the power plants). Expected from section 8: ~15 % short of *measured* use, far short of *max* use. Measure how long the unit buffers last.

**Q3. Same, with the destruction a real campaign does by day 3 / 5 / 10** (`PRODMIX`: mostly capture, 4-13 % bombs, 8-28 % grid).
Run the three time points, both factions, and the two realistic stories (Blue advance; Blue air campaign).

**Q4. Where does the limit sit when it binds: production, the pool halving, the 0.5 ratio cap, the 2 passes per hour, or the 3-hour
defender interval?** Vary each independently at max burn. This says which knob would have to change to make supply matter.

**Q5. How much buffer is inside the units?** Zero production, start 100 %: days until front < 50 % at (a) measured use, (b) 2x, (c) max.

**Q6. Does the answer change with `DataRateModSup 1.5` (stock) and `StartOffBonus 5000`?** Rerun Q1-Q3.

**Q7. Is fuel ever binding?** Measure squadron fuel and battalion fuel draws; check the 100-vs-10,000 unit mismatch in section 4b.

**Q8. Is ground-combat ammunition worth modelling?** What would a unit burn if firing cost what it should (a firefight sized to
shots)? Today it is 0.4 % of use. Candidate lever for making supply matter without a bigger economy.

**Q9. Do China and CIS matter?** They run on the 120/day floor. Rerun with `chinaown=1` and quantify what the game does today.

**Q10. Which strikes are worth it, per the ledger?** Per-point value of each producer class (section 2a), per day and per sortie, and the
airlift credit as a target.

**Q11. Should depots and army bases produce at all?** Four variants against the baseline, each through Q1-Q3:
(a) stock, they produce; (b) `noprod=1`, storage only, no production; (c) stores (`SupplyStocks`): they hold a local stock the pool
refills, no production; (d) stores that also produce (`StoresProduce`). Section 2a ledger: dropping their production removes 26 % of
DPRK's and 22.5 % of ROK's supply rate, taking ROK (1,980) below its measured use (~2,350). Measure: ROK/DPRK battalion supply at the
front, who the strike value shifts to (factories vs depots), what capturing a depot is worth to the captor, store coverage (about half
of battalions have no reachable store; `store_map.py`), and the war length and winner. Context: the production role is a FF6 add-on
("Cobra Added Army, depot, and port per JimG"), the army-base rate went from 2/6 to 127 in FF6, and BMS 4.37 has no army bases and one
depot at rate 1. The case for production is that it ties ground captured to supply captured (capture is 67-79 % of what a normal war
removes from production).

Also noted but not scheduled: ownership delay for captured producers (`Oldown == owner`), whether `AddReinforcements` should draw
replacements, and the 4 vs 20 shots-per-point mismatch.

---

## 10. Not verified / assumptions

* Fuel numbers in 4b are derived from constants, not measured.
* The "~43 vehicles per battalion" and "~34" are back-solved from deliveries, not counted.
* Delivered points are an upper bound on use (the 100 % clamp wastes excess; not measured).
* `AddReinforcements` and `RepairObjectives` were not traced end to end; only found to touch no pool.
* The three seeds are 6-8 days and end early on a Blue win, so day-by-day rates after day 6 are thinner.
* PRODMIX counts team 2 as ROK; US and Japan squadrons draw on whichever team they belong to in this save (not checked).

---

## 11. Results (2026-11-01): China War mod baseline, 6 seeds x 10 days (12 seeds for s0, s8, s9, s10), `noend=1`

Game dir `C:/FreeFalcon6`, `usecfg=1 replfix=1 airliftneed=1 airliftrunway=1`, frozen 26 Oct exe. Tags `mod-s0..s10`;
`python tools/campsim/supply_inventory.py mod-s0 mod-s1 ...` prints every table.

**What the mod ships (section 6c2) and what that means for these runs.** `config/mods/Korea China War.cfg` is read by `usecfg=1`: depots
and army bases do not produce, **battalion burn is x2 for every team**, the North's battalions start at 70 % supply, China draws on
the North's pool. A `usemult=` / `noprod=` knob on the command line overrides it. So: **s8 is the mod exactly as shipped**; s0 and s7
(`usemult=1`) burn half as much as the mod; "burn" below is relative to s0's x1.

Burn levels: x1 = s0; **x2 both = the mod (s8)**; Blue x2 / Red x6 = s1, s2, s3, s6; **Blue x4 / Red x8 = the measured maxima (Q0: Blue ~x4,
Red ~x7)** = s9, s10. Mean battalion supply %, mean of 6 seeds:

| Scenario | Red supply % h24/72/120/168 | Blue supply % h24/72/120/168 | Red bns <25 % (h72) | Blue bns <25 % (h168) | Supply made /day Red / Blue | Blue wins at hour |
|---|---|---|---|---|---|---|
| s0 burn x1, mod production (12 seeds) | 80 / 91 / 92 / 91 | 97 / 97 / 97 / 96 | 0 | 0 | 1,674 / 2,361 | **177** (12/12; range 150-201) |
| **s8 the mod as shipped (x2)** (12 seeds) | 76 / 83 / 82 / 80 | 95 / 95 / 94 / 92 | 3 | 1 | 1,686 / 2,398 | 190 (12/12; range 140-238) |
| s5 x1, **Red factories gone** | 74 / 75 / 72 / 71 | 97 / 96 / 97 / 97 | 4 | 0 | 612 / 2,358 | 166 (6/6) |
| s1 Blue x2, Red x6 | 53 / 50 / 51 / 54 | 96 / 94 / 93 / 91 | 73 | 3 | 1,701 / 2,366 | 132 (6/6) |
| s2 s1 + **Red factories gone** | 49 / 45 / 44 / 44 | 95 / 94 / 94 / 90 | 98 | 4 | 626 / 2,298 | 127 (6/6) |
| s3 s1 + **Blue factories gone** | 53 / 49 / 48 / 46 | 91 / 88 / 82 / 82 | 85 | 11 | 1,680 / 610 | 143 (6/6) |
| s9 **max burn** (Blue x4, Red x8) (12 seeds) | 51 / 47 / 45 / 45 | 85 / 80 / 78 / 76 | 85 | 26 | 1,676 / 2,402 | 129 (12/12; range 100-165) |
| s10 s9 + **Blue factories gone** (12 seeds) | 51 / 46 / 44 / 49 | 79 / 69 / 61 / 64 | 102 | 36 | 1,681 / 609 | **203 (6/12 by day 10; 12/12 by day 20, mean 271)**; wins at 169-224, six runs never win in 10 days |
| s7 x1, depots + army bases produce | 85 / 93 / 92 / 92 | 97 / 97 / 97 / 96 | 1 | 0 | **2,165 / 2,856** | 182 (6/6) |
| s6 s1 + depots + army bases produce | 58 / 54 / 48 / 48 | 96 / 94 / 94 / 94 | 68 | 1 | **2,079 / 2,854** | 140 (6/6) |

(s4 and s4b were void: the mod already switches depot/army-base production off, so `noprod=1` and zeroing them changed nothing.)

**Q1. If every unit used supply at the maximum rate, with all factories standing, would it matter?** Yes, differently per side.
* **Red** (income ~1,700 a day, only 15-40 % above what it uses at x1): at x2 (the mod) it slips from ~92 to ~80 %; at x6-x8 it is
  at ~50 %, 70-100 of its battalions are under 25 %, and Blue wins 45-55 hours sooner (129-136 vs 177-183). It is **production-limited at
  any burn above ~x1.4**: it sends what it makes (~1,500 of 1,700), the pool never builds, and extra burn only lowers the level it
  settles at. It never reaches zero (floor, airlift, and units stop burning under 3 %).
* **Blue** (income ~2,360): fine at x2 (93-96 %); at its true maximum (x4) it falls to 86 -> 77 % with 28 battalions under 25 % by day 7,
  and still wins in 12/12 runs (mean hour 129, vs 177 at x1). Its economy is big enough to carry even flat-out movement for ten days.

**Q2. ...and with the factories gone?**
* **Red's factories** at x1 take it from 92 % to ~72 % (-18 points) and shorten the war 17 hours (the ledger predicted "short, not a
  collapse"). At max burn they add only 4-9 points: Red is already production-limited.
* **Blue's factories** at x2 cost Blue ~9 points and 11 hours (143 vs 132). **At Blue's true maximum (s10) this is the one scenario
  that slows the war most: Blue falls to 61-79 %, 36 battalions under 25 %, and only 6 of 12 runs win within ten days (at hour 169-224). A 20-day
  rerun (12.4) shows all 12 do win, at a mean of hour 271: about twice as long, not stopped.** Blue's factories are 72 % of its income; this is where supply starts to matter, and only when burn and a factory
  campaign coincide.

**Q3. Destruction a real campaign does by day 3 / 5 / 10** (Red supply capacity, rate/day, from the frames):

| | h24 | h72 (day 3) | h120 (day 5) | h168 (day 7) | h239 (day 10) |
|---|---|---|---|---|---|
| s0 control | 10,583 | 10,230 (-3 %) | 10,177 (-4 %) | 7,698 (-27 %) | 2,898 (-73 %) |
| s1 (Blue wins sooner) | 10,473 | 10,542 | 8,682 (-17 %) | 4,473 (-57 %) | 790 (-92 %) |

A normal war has taken 3-4 % of Red's production by day 5; it falls after day 6 by capture, not bombing. The "few days" case is the
intact case.

**Q11. Should depots and army bases produce?** They add **~500 supply a day** to each side (+29 % Red, +21 % Blue; s7 vs s0, s6 vs s1)
and change the outcome by little: Red supply +2 to +5 points at x1, +4 to +5 early at x6 (later mixed, 48 vs 51), Blue wins within
noise (182 vs 183; 140 vs 132 hours, six seeds). So with the mod's own choice (off) the economy is ~20-30 % smaller and the war is not
different in a way six seeds can see. The reasons for off (single role, no double-counting a depot as store and producer, BMS parity)
therefore cost nothing here; the reasons for on (captured depots feed the captor) are worth ~500 a day, small next to the burn
multiplier.

**What this says about the ledger.**
* Burn / income is the dial. Red's income is 1.4x its x1 use; Blue's is ~2x its x1 use, ~1x its x2, ~0.5x its x4 maximum.
* The pool is always empty on Red's side and empty on Blue's only above x2, so the pool halving rule and the 0.5 ratio cap are never the
  limit: **production is**.
* Strikes: Red's factories are worth ~18 points of supply at x1 and ~nothing at max burn; Blue's factories are worth ~9 points at x2 and a
  stalled war at x4. A Red strike campaign on Blue's factories is more consequential than the mirror, because Red's economy is already the
  binding one.

**Caveats.** `usemult` multiplies what battalions do; it is not forced burn (units that do not move still burn nothing), so x4 on Blue
is the AI's own movement at four times the cost. Six seeds, bimodal wars (the earlier research found 12 seeds needed for a clean
split); s10 is now 12 seeds (6/12 wins); the other scenarios in the table are still 6. Squadron and replacement maxima (Q0), Q4-Q10 are not run.

**Found while running.** (1) The mod's cfg is read by campsim (`usecfg=1`), so the mod is not "plain Rolling Fire plus a cfg": run it
only with `--game C:/FreeFalcon6` or a copy that carries `config/mods`. (2) `noprod=1` appeared to do nothing for the reason above;
the 2026-10-15 "storage only" results were run before the mod cfg existed and are unaffected. (3) The installed `FFViper.cfg` does not
set `StoresNoProd`; only the mod does.

---

## 12. The remaining questions (2026-11-01)

Same setup as section 11 (China War mod, `C:/FreeFalcon6`, 6 seeds x 10 days unless stated). Run tags `q4-*`, `q5-*`, `q6-*`, `q9-*`,
`q10-*`; `python tools/campsim/supply_inventory.py --table <tags>` prints the one-line comparison used below.

### 12.1 Q0, rest: squadron and replacement maxima

**Squadron munitions.** Gross stores loaded onto sorties per day (the ceiling if every loaded weapon were expended), from `SUPSPLIT`, 12
seeds, days 1-6:

| | loaded / day | returned unused | net use | battalions received |
|---|---|---|---|---|
| Blue, mod as shipped | 1,297 | 1,193 (92 %) | **104** | 2,003 |
| Red, mod as shipped | 1,375 | 1,192 (87 %) | **184** | 1,211 |
| Blue, max burn | 1,736 | 1,620 (93 %) | 116 | 2,414 |
| Red, max burn | 1,491 | 1,315 (88 %) | 175 | 1,182 |

So **the squadron ceiling at today's sortie tempo is ~1,300-1,700 pool points a day per side: the same size as Red's whole supply
income and 60-80 % of Blue's.** It is held down only because 87-93 % of what is loaded comes back (air-to-air missiles rarely fire; strikes
often abort). A real expenditure model (every sortie drops its weapons) would roughly double the demand on each side, and tripling the sortie tempo
would triple it. Today air munitions are 5-15 % of battalion use only because of the return rate, not because loadouts are small.

**Replacements.** Ground vehicles lost per day (net fall of the side's vehicle count plus replacements given, mean 12 seeds, days 2-6;
day 1 is inflated by Chinese units folding into the North's count): Red ~700-1,400 a day, Blue ~570-1,000, against replacement production of
~290 (Red) / ~390 (Blue) plus airlift. Replacements actually given to ground units: Red 330-890 a day, Blue 600-680. So **Red's
replacement income covers about half of its losses, Blue's roughly all of its own**; Blue's pool still builds to 5,000-7,000 unspent by day
10 because late losses fall away. (The aircraft replacement counter in the same frames reads 400-1,000 a day, implausibly high for
airframes; I did not trust it.)

### 12.2 Q7: fuel

Fuel made and sent to units per day (12 seeds):

| | Red made / sent | Blue made / sent |
|---|---|---|
| mod as shipped, h24-48 | 1,320 / 1,158 | 3,471 / 1,764 |
| mod as shipped, h96-120 | 1,285 / 566 | 2,972 / 1,531 |
| max burn, h24-48 | 1,225 / 1,006 | 3,479 / 3,059 |
| max burn, h96-120 | 880 / 328 | 3,366 / 1,518 |

**Red's fuel is production-limited for the first two days (sends 82-88 % of what its two refineries make, ratio 0.03-0.06), then
demand-limited as its battalions die. Blue's fuel is plentiful except on day 2 at max burn (88 % of production used).** Fuel counts as
half a supply point to a truck, so Red's ~1,100 fuel a day is worth ~550 supply points, a third of what its battalions receive. Section
12.5 tests that directly (refineries held down).

### 12.3 Why Red's supply plateaus near 50 % instead of zero

At h72, max burn, Red battalions (6 seeds pooled, 2,064 samples) are **bimodal, not near 50 %**: 31 % under 20 %, 35 % at 60-79 %,
15 % at 80 %+. The split follows the orders: **RESERVE units (the ones driving to the front) average 29 %, CAPTURE units 83 %, SUPPORT
79 %, SECURE 85 %, AIR DEFENSE 61 %, DEFEND 55 %.** The supply pass gives a battalion with attack-role orders 6x its shortfall every 20 minutes and
everyone else 1x every 3 hours, so the limited pool is spent on the attackers first and the reserves that burn the most are left
starved. The mean hides it. At burn x1 (s0) the split is invisible (RESERVE 96 %, CAPTURE 98 %).

### 12.4 Why Blue fails to win in half the s10 runs

The six non-winning seeds are **not collapsing**: Blue keeps ~4,000-4,300 ground vehicles and 30-60 battalions under 25 % at h168, the
same as the winners. They are **slower**: Blue holds 1,330-1,500 objectives at h168 against 1,750-1,820 in s9, and Red still holds 800-1,050
against ~580 in s9. The war is stretched past the 240-hour cap, not stopped; the six wins land at hour 169-224 and the rest are
cut off by the run length. **The 20-day rerun (12 seeds, `s10-20d`, same knobs) confirms it: Blue wins 12 of 12, at hour 112, 149, 170, 222, 224, 244, 289,
295, 326, 342, 417 and 463 (mean 271, median ~267, i.e. day 11), against a mean of 129 hours (day 5) with its factories intact (s9) and 177 at
burn x1.** So Blue's factories gone at its maximum burn roughly **doubles the war** instead of stopping it; the earlier "6/12" was the 240-hour cutoff.
The two other Blue cases cut off at ten days were rerun at 20 days (6 seeds): **zero supply production wins 4/6, mean hour 339; power plants gone wins 2/6, at hours
310 and 432.** The three ways to hurt Blue's economy rank: factories (all win, mean day 11) < zero production (4/6, day 14) < power plants (2/6, rest unfinished at day 20).

### 12.5 Q4: what binds at max burn (Blue x4, Red x8)

Baseline s9 (12 seeds) against one limiter relaxed at a time (6 seeds each). Red / Blue mean battalion supply % at h24 / 72 / 120 / 168:

| Variant | Red | Blue | Blue wins at hour |
|---|---|---|---|
| s9 baseline | 51 / 47 / 45 / 45 | 85 / 80 / 78 / 76 | 129 (12/12) |
| `maxratio=1.0` (no 0.5 cap on a unit's share) | 52 / 50 / 56 / 52 | 84 / 82 / 75 / 71 | 99 (6/6) |
| `gndsupdef=60` (defenders every hour, not 3 h) | 50 / 47 / 45 / 42 | 92 / 87 / 83 / 82 | 123 (6/6) |
| `prodsup=2.25` (production x3) | 64 / 68 / 74 / 77 | 93 / 92 / 93 / 92 | 152 (6/6) |

**Production is the limit.** Removing the ratio cap does nothing for Red and costs Blue a few points; the shorter interval helps Blue
~6 points and Red none. Tripling production lifts Red from ~45 to ~75 % and Blue from ~76 to ~92 %. The hourly pool halving and the
twice-an-hour passes are not separate knobs and were not varied, but with production tripled the supply they allow is enough, so neither
is the limiter. (Blue winning at hour 99 without the cap is a 30-hour shift on six seeds; I treat it as unconfirmed.)

### 12.6 Q5: how long does the buffer inside the units last (zero supply production)

`prodcut=TEAM:0` removes that team's whole supply production including the +5/h floor (fuel and replacements untouched; airlift stays).
6 seeds each. Red / Blue supply % at h24 / 72 / 120 / 168:

| | Red zero production | Blue zero production |
|---|---|---|
| mod as shipped (x2) | **65 / 58 / 51 / 49** (normal 76 / 83 / 82 / 80); Blue wins at hour 152 (normal 190) | **90 / 84 / 71 / 68** (normal 95 / 95 / 94 / 92); **Blue wins in 0 of 6 within ten days; 4 of 6 by day 20, mean hour 339** (normal 12/12 by day 8) |
| max burn | 43 / 40 / 40 / 40 (normal 51 / 47 / 45 / 45); wins at hour 108 | 72 / 62 / 53 / 52 (normal 85 / 80 / 78 / 76); **0 of 6**, 83 battalions under 25 % |

* **Red is not starved to zero by losing supply production.** It loses ~10 points per two days at the shipped burn and settles near 40-50 %.
  What keeps it up: **airlift** (252 landings and 2,772 supply credited by hour 48 in one run, capped by need near its airbases) and **fuel
  counted at half weight** (~1,100 a day from two refineries).
* **Blue depends on its production far more**: its supply sent falls to ~200 a day and it cannot win a war it normally wins by day 8.
  Measured (`AIRLIFT` lines, hour 48 and 144, zero-production runs): Blue's 339 landings were offered 2,944 supply and credited **836 (28 %)**, with
  259 landings cut by the need rule (the `AirliftNeed` cap counts only battalions within 40 km of the landing airbase); Red's 272 landings
  were credited **3,158 of 3,264 (97 %)**. Red's airbases sit among its battalions, Blue's do not.
* The buffer is therefore ~2 days at the shipped burn (Red falls 10 points per 48 h) before the secondary sources take over.

### 12.7 Q6: the stock supply modifier (1.5) instead of the mod's 0.75

6 seeds, `prodsup=1.5` (Red makes 3,226 a day, Blue 4,630):

| | Red | Blue | Blue wins |
|---|---|---|---|
| shipped burn, modifier 0.75 (12 seeds) | 76 / 83 / 82 / 80 | 95 / 95 / 94 / 92 | 12/12, hour 190 |
| shipped burn, modifier 1.5 | 85 / 91 / 90 / 90 | 95 / 96 / 96 / 95 | 5/6, hour 181 |
| max burn, modifier 0.75 (12 seeds) | 51 / 47 / 45 / 45 | 85 / 80 / 78 / 76 | 12/12, hour 129 |
| max burn, modifier 1.5 | 56 / 54 / 54 / 56 | 90 / 90 / 89 / 88 | 6/6, hour 142 |

At the shipped burn doubling production adds ~10 points to Red and nothing to Blue; at max burn it adds 5-11 points to Red and ~12 to Blue.
Red stays near 55 % even at 1.5, so no plausible production modifier closes the gap at x8; that takes tripling (12.5). The mod's 0.75 is
not what holds the war together at the shipped burn. (The stock start bonus is an AII key and was not varied.)

### 12.8 Q9: China on the North's pool

`chinaown=0` (China not a team of its own) vs the mod's `g_bChinaOwnTeam 1`: Red supply 75 / 82 / 84 / 86 vs 76 / 83 / 82 / 80, Blue wins at
hour 156 (6/6) vs 190. Most Chinese units fold into the North's count either way (328 vs 274 battalions); the 14 left on their own pool sit
at 17 % supply. **China's supply is not a problem in the mod; the choice moves the war length by ~34 hours through initiative and shared
offensives, not through supply.**

### 12.9 Q10: what a strike is worth (mod as shipped, x2 burn; target held down from hour 0, 6 seeds)

Baseline s8 (12 seeds): Red supply 76 / 83 / 82 / 80, Blue 95 / 95 / 94 / 92, Red makes 1,690 a day, Blue 2,392, Blue wins at hour 190.

| Strike on | Red's supply made / day | Red supply % h24 / 72 / 120 / 168 | Blue wins at hour |
|---|---|---|---|
| Red **power plants** | 124 (-93 %) | 63 / 58 / 51 / 51 | 151 (6/6) |
| Red **factories** | 611 (-64 %) | 66 / 63 / 58 / 55 | 157 (6/6) |
| all Red producers | 410 (-76 %) | 64 / 60 / 54 / 52 | 156 (6/6) |
| Red ports | 1,473 (-13 %) | 72 / 82 / 77 / 75 | 142 (5/6) |
| Red refineries | 1,680 (0 %) | 73 / 80 / 79 / 76 | 176 (6/6) |

| Strike on | Blue's supply made / day | Blue supply % h24 / 72 / 120 / 168 | Blue wins |
|---|---|---|---|
| Blue **power plants** | 158 (-93 %) | 82 / 70 / 66 / 61, 52 battalions under 25 % | **0 of 6** |
| Blue **factories** | 634 (-73 %) | 92 / 89 / 85 / 84 | 173 (6/6) |
| Blue ports | 2,058 (-14 %) | 95 / 94 / 93 / 92 | 190 (6/6) |
| Blue refineries | 2,433 (+2 %) | 94 / 94 / 93 / 93 | 195 (6/6) |

* **Power plants are the best target on both sides, by far**: holding down the plants cuts 93 % of production because every producer is scaled by
  its nearest plant, which beats factories (the biggest class) and ports and refineries are worth little.
* **Blue's power grid is the most damaging single target in this study: 0/6 wins within ten days at the shipped burn** (20-day rerun: **2 of 6 win, at hours 310 and 432; four never win by hour 480**, so unlike the factory case it can be a lost war). Red's factories and plants
  slow Blue's win by 33-39 hours.
* **Refineries do nothing to supply** (fuel is half-weight and the pool is not the limit); ports a few points; Red's win-time numbers for
  ports and refineries are within six-seed noise.
* The depots and army bases are not a target in the mod: they do not produce.

### 12.10 Forced burn (the real "every unit at max rate"), new knob `burnmax=PCT`

`burnmax=PCT` (scratch build from `HEAD` + this knob and `fireuse`, `ff-supply-build`, not committed): every battalion of a team debits
`0.0476 x MovementSpeed` percentage points per supply pass (twice an hour) x PCT/100, **whether or not it moves**, i.e. the flat-out burn of
section 11/Q0. The natural movement and firing burn still apply on top (`usemult=1`), so 100 % is slightly above the Q0 ceiling. The new exe was
checked against the old one at the shipped burn and at max burn: supply levels agree to 1-2 points (Red 76 / 84 / 82 / 80 vs 76 / 83 / 82 / 80;
max burn 50 / 47 / 46 / 46 vs 51 / 47 / 45 / 45); win hours differ by 16-28 (six seeds, seed-to-seed spread 100-240).

6 seeds each, mean battalion supply %, h24 / 72 / 120 / 168:

| Scenario | Red | Blue | Bns < 25 % (Red h72 / Blue h168) | Blue wins within 10 days |
|---|---|---|---|---|
| mod as shipped (x2), new exe | 76 / 84 / 82 / 80 | 95 / 95 / 93 / 92 | 4 / 3 | 6/6, hour 162 |
| `usemult` max (Blue x4, Red x8), new exe | 50 / 47 / 46 / 46 | 84 / 81 / 77 / 78 | 96 / 24 | 6/6, hour 146 |
| **forced burn 50 %** | 54 / 33 / 11 / 8 | 93 / 92 / 84 / 82 | 132 / 9 | 6/6, hour 151 |
| **forced burn 100 %** | **28 / 15 / 6 / 2** | **86 / 76 / 64 / 60** | **214 / 87** | 6/6, hour 147 |
| forced 100 % + Blue factories gone | 28 / 14 / 5 / 3 | 78 / 62 / 52 / 48 | 216 / 115 | 4/6, hour 140 |
| forced 100 % + Blue power plants gone | 27 / 18 / 10 / 6 | 63 / 48 / 47 / 41 | 228 / 136 | 0/6 |

* **This is the true answer to "all units at the maximum rate": Red collapses (supply ~2 % by day 7, 214 battalions under 25 % by day 3);
  Blue declines to ~60 % and holds.** Half the flat-out burn already takes Red to 8 %. Red's ~1,700 a day cannot feed 500 battalions
  burning 8,600 a day, which is exactly the ledger's 1.8x (Blue) / 3.6x (Red) shortfall.
* Blue still wins early (hour 147) because Red starves much sooner than Blue does: more burn shortens the war until Blue's own economy
  is also hit. With Blue's factories gone four of six still win within ten days; with its power plants gone none do (**within ten days; a 20-day
  rerun of the power-plant case at the shipped burn is running to see if "never" is really "slowly"**).
* Forced burn, unlike `usemult`, is independent of how much the AI moves: Red at `usemult` x8 sat at ~46 % because units that do not drive do
  not burn; forced burn removes that escape.

### 12.11 Q8: would a real ammunition model make supply matter? (`fireuse=N`)

`fireuse=N` multiplies the supply a battalion spends **firing** (the stock rule spends `supply x shots / total ammunition`, `unit.cpp`
CollectWeapons) on top of the mod's x2. 6 seeds, new exe:

| | Red supply % h24 / 72 / 120 / 168 | Blue supply % | Blue bns < 25 % (h168) | Blue wins | ground-firing use / day, Blue / Red (pct-points) | moving use / day, Blue / Red |
|---|---|---|---|---|---|---|
| x1 (shipped) | 76 / 84 / 82 / 80 | 95 / 95 / 93 / 92 | 3 | hour 162 | 55 / 47 | 5,791 / 3,536 |
| x10 | 70 / 78 / 74 / 76 | 94 / 93 / 91 / 92 | 2 | hour 158 | 247 / 137 | 6,028 / 3,313 |
| x30 | 68 / 76 / 72 / 72 | 90 / 90 / 89 / 88 | 5 | hour 168 | 551 / 299 | 5,693 / 3,545 |
| x100 | 64 / 73 / 70 / 70 | 89 / 88 / 85 / 83 | 12 | hour 203 | 993 / 575 | 5,092 / 3,482 |

**A hundred-fold ammunition price takes ~10 points off both sides and ~40 hours onto the war; it does not make supply bind.** Ground-firing
use grows only 18x at x100 (it is capped by what the unit has and by how rarely battalions fight: use stays one fifth of movement even at
x100). A cheap multiplier is therefore the wrong way to make fighting cost supply; a real model would have to spend ammunition per
engagement, not per unit-hour of existing firing logic. Moving remains 80-95 % of the burn.

---

## 13. Overnight goal (set 2026-11-01): a supply tuning that makes supply a choice

**Goal.** Find settings of the existing supply knobs (battalion burn `SupplyUseMult`, production modifier `DataRateModSup` via `prodsup=`,
power-grid redundancy `gridk=` / `gridmean=`) under which supply shapes the war without deciding it. No timers, no gates, no scenario scripting:
every effect must come from a mechanism. Judged on China War mod `save0`, `usecfg=1`, the scratch exe with `burnmax`/`fireuse`/`gridk`
(`ff-supply-build`, from `HEAD`), strikes begin at hour 48 (`blowat=48`, a campaign that starts on day 3), held down (a player who keeps hitting them).

A setting **passes** when, over 12 seeds x 20 days:

| # | Criterion | Number |
|---|---|---|
| 1 | Baseline (no strike): Blue still wins, on a day that leaves room for play | wins in >= 10/12; mean win hour 190-260 (day 8-11) |
| 2 | Blue striking Red's power plants matters | mean win hour at least 15 % earlier than baseline |
| 3 | Red striking Blue's power plants matters but is survivable | win hour at least 15 % later than baseline and >= 8/12 still win by day 20 |
| 4 | Supply is visible but does not zero a side | Red battalion supply 30-70 % and Blue >= 60 % averaged over days 5-7 of the baseline |

**Stage 1, screen (6 seeds x 10 days):** burn x2 / x3 (x2 is the mod) x production modifier 0.75 / 1.5 x grid redundancy off / best-of-2 x
{no strike, Blue strikes Red's plants, Red strikes Blue's plants} = 24 scenarios, 144 runs, tags `t1-*`. A setting goes forward if it is
within reach of criteria 1-4 on the screen (win rate and supply levels; win hours are noisy at six seeds).
**Stage 2, finalists (12 seeds x 20 days):** the best two or three settings x the three strike cases, tags `t2-*`.
**Output:** the ranked table below, with the criterion each near-miss fails, and what to change in the game cfg for the winner.

**Not part of the goal:** code changes to the game (airlift need rule, the grid rule in the game itself, a new ammunition model) and
anything that touches the user's working tree: all runs use the frozen copy in the scratchpad (`exe3`), and every new tag is unique.

### 13.1 Stage 1 result (6 seeds x 10 days, strikes from hour 48, new exe)

Setting id = burn multiplier `u`, production modifier `p`, grid rule `g` (g1 = stock nearest plant, g2 = best of the two nearest). Columns follow
the four criteria; "wins" are within ten days, so criterion 3 (Red hits Blue, which delays Blue past day 10) shows 0-1 wins and is only decided at stage 2.

| Setting | 1 baseline wins / mean hour | 2 Blue hits Red: mean hour (change) | 3 Red hits Blue: wins by day 10 | 4 Red / Blue supply days 5-7 |
|---|---|---|---|---|
| u2 p0.75 g1 (the mod today) | 6/6, 181 | 156 (-13 %) | 0/6 | 80 / 93 |
| u2 p0.75 g2 | 6/6, 170 | 148 (-13 %) | 1/6 | 83 / 94 |
| u2 p1.5 g1 | 6/6, 198 | 151 (-24 %) | 0/6 | 91 / 96 |
| u2 p1.5 g2 | 6/6, 187 | 134 (-29 %; 5/6 win) | 1/6 | 90 / 96 |
| u3 p0.75 g1 | 6/6, 175 | 158 (-10 %) | 0/6 | **66** / 85 |
| **u3 p0.75 g2** | **6/6, 203** | **171 (-16 %)** | 0/6 | **69 / 89** |
| u3 p1.5 g1 | 6/6, 166 | 150 (-10 %) | 0/6 | 87 / 93 |
| u3 p1.5 g2 | 6/6, 179 | 137 (-24 %) | 0/6 | 87 / 94 |

* **Criterion 4 (supply visible: Red 30-70 %) is only met at burn x3 with the mod's low production (0.75).** At x2, or with production 1.5, Red sits at 80-91 %
  and supply is not a story.
* **u3 p0.75 g2 is the only setting that meets 1 (203 h), 2 (-16 %) and 4 (69 / 89) together**; 3 is decided at 20 days. u3 p0.75 g1 meets 4 but
  its baseline is too quick (175) and the strike barely moves it (-10 %): **the grid redundancy is what makes the Blue strike matter at the
  same burn**, not the other way round.
* Every setting has Blue winning 6/6 at baseline; Red striking Blue's power plants takes the win past day 10 in 5 of 6 seeds or more in every one.
* Stage 2 settings: **A = u3 p0.75 g2** (the candidate), **C = u2 p1.5 g2** (the high-supply alternative with the strongest strike effect), **D = u3 p1.0 g2**
  (between A and the fail at 1.5, not in the screen), each 12 seeds x 20 days x {no strike, Blue hits Red, Red hits Blue}. Tags `t2-*`.

### 13.2 Stage 2 result (12 seeds x 20 days, strikes from hour 48 and held) and verdict

| Setting | 1 baseline wins / mean hour (need >=10/12, 190-260) | 2 Blue hits Red: hour (change; need <= -15 %) | 3 Red hits Blue: wins / hour (need >=8/12, >= +15 %) | 4 Red / Blue supply d5-7 (need Red 30-70, Blue >=60) |
|---|---|---|---|---|
| A  u3 p0.75 g2 | 12/12, 162 **fail (too fast)** | 155 (-4 %) **fail** | 3/12, 418 (+157 %) **fail (too deadly)** | 67 / 86 **pass** |
| C  u2 p1.5 g2  | 12/12, 174 **fail (too fast)** | 162 (-7 %) **fail** | 6/12, 381 (+119 %) **fail (nearer)** | 90 / 96 **fail (no story)** |
| D  u3 p1.0 g2  | 12/12, 171 **fail (too fast)** | 157 (-8 %) **fail** | 1/12, 329 (+92 %) **fail** | 78 / 91 **fail (Red too high)** |

**No setting passes all four. Verdict: the three existing knobs cannot make supply "shape without deciding" in this baseline.**

What the 12-seed, 20-day numbers say, and how they differ from the six-seed screen:

* **Criterion 1 fails everywhere, and the screen over-stated it.** A scored 203 h at 6 seeds x 10 days and 162 h at 12 seeds x 20 days. Seed spread of a
  win hour is about +-40 h, so the stage-1 win hours were noise-level; only the win *counts* and supply levels carried over. All three baselines are 160-175 h
  (day 7), the same as the mod today. No knob combination here moved the unstruck war to day 8-11: burn and production change supply, not the pace of the ground push.
* **Criterion 2 fails: Blue striking Red's plants is worth only 4-8 % at 20 days** (the screen said 16-29 %, again seed noise on 6). Red's supply does fall
  (A: 67 -> 54 % by day 7, C: 89 -> 69 %, D: 78 -> 59 %), and Red battalions under 25 % go from 14 to 40 in A, but Red's army was never what stopped Blue.
  Supply on the Red side is already low in A before any strike, so a strike on top costs little extra.
* **Criterion 3 fails the other way: Red striking Blue's plants is not survivable at A and D** (3/12 and 1/12 Blue wins by day 20; Blue supply 56-58 % on day 7 and
  70-77 battalions under 25 %), and half-survivable at C (6/12, supply 68 %). The effect is asymmetric: Blue's offence runs on a long supply line and burns more,
  so a hit on its plants stops it, while a hit on Red's plants barely slows a defender. That asymmetry is the mechanism, not a tuning error.
* **Criterion 4 is met by A only** (Red 67 %, Blue 86 %): the only combination with burn x3 and the mod's 0.75 production. C (production 1.5) hides supply entirely;
  D (production 1.0) is in between and misses.

Ranking by closeness: **A** (one pass, two hard fails, carries visible supply), **C** (no pass, but the least lopsided strike result, 6/12), **D** (dominated by both).

**Cfg consequence.** `gridk` (best-of-N power) exists only in the campsim scratch build; the game has the stock nearest-plant rule. So A cannot be applied to the game
by cfg alone. The only cfg-reachable part is `SupplyUseMult 3` with the mod's `DataRateModSup 0.75` (g1), which in the screen met criterion 4 (66 / 85) but not 1 or 2.
**Recommendation: leave the mod as shipped.** Supply is a lever for the player, mostly *against* Blue; if the aim is also a lever *against* Red, that needs a mechanism
the knobs do not provide (Red's economy matters little because Red does not need to advance), i.e. a code or scenario change, which this goal excluded.

**Grid-knob check (recorded):** `blowlim=power:red:4` (four Red power plants destroyed): stock rule 1,248 supply made, mean-of-3 1,326, best-of-3 1,674,
against 1,705 with every plant up. The best-of-N rule keeps Red's output at ~98 % of the undamaged figure; the stock rule loses ~27 %.

**Left open:** fuel knob not varied; `StartOffBonus` (AII) not varied; Q4 ratio-cap hour-99 result unconfirmed; Blue zero-production / power cases at 6 seeds only; nothing committed.

## 14. Q12: moving units consume fuel (tank per battalion), 2026-11-02

**What was built (scratch campsim only, `ff-supply-build`, not in the game).** `fueltank=1` gives every wheeled or tracked battalion a fuel tank beside its supply:
the fuel pool refills the tank (stock: fuel is added to supply at half value), and a unit's fuel need is its empty tank, not its supply shortfall.
`movefuel=N` empties the tank by N points at every move burn event (the same 21-step event that costs supply); an empty tank refuses all movement until
the next resupply. `fueltank=1` alone is the control: the tank never drains, fuel is simply no longer half a supply point. Diagnostic: `FUELTANK` lines in
the log (burned points, refused moves, mean tank, empty tanks per 6 h); `supply_inventory.py --fuel <tags>` summarises them. Trains and foot units have no tank.

**Runs.** Mod as shipped, `save0`, `usecfg=1`, 6 seeds x 20 days, tags `f1-*`. Each setting x {no strike, Blue holds down Red's refineries from hour 48,
Red holds down Blue's refineries from hour 48}. N = 8, 16, 32 tank points per burn event (a full tank is 100; N=2 in the smoke test did nothing).

| Setting | Baseline Blue wins / mean hour | Blue hits Red's refineries | Red hits Blue's refineries | Blue tank % / empty tanks (baseline -> Red strike) | Red battalion supply d5-7 |
|---|---|---|---|---|---|
| no tank (mod today) | 6/6, 168 | - | - | - | 83 |
| tank, no burn (control) | 5/6, 176 | 6/6, 160 | 6/6, 160 | 100 % | 76 |
| N=8  | 6/6, 166 | 6/6, 172 | 6/6, 174 | 66 % / 12 -> 39 % / 35 | 84 |
| N=16 | 6/6, 165 | 6/6, 163 | 6/6, 185 | 44 % / 24 -> 26 % / 42 | 90 |
| N=32 | 6/6, 148 | 6/6, 143 | 6/6, 163 | 30 % / 31 -> 14 % / 59 | 90 |

**Result: fuel burn makes the tanks bind but does not change who wins or when.**

* All 54 runs with a burning tank (9 scenarios x 6 seeds) end with a Blue win; the only miss in the 72 tank runs is the control baseline, seed 5. Win hours move 143-185 against 168 for no tank;
  with six seeds a win hour is good to about +-40 h, so none of the differences (-15 % .. +10 %) is resolved.
* The tanks are really used. Blue, the side that has to travel, runs at 66 / 44 / 30 % mean tank at N = 8 / 16 / 32 with 12-31 battalions empty at any time; Red,
  mostly defending, stays at 74-80 %. Red striking Blue's refineries takes Blue's tank from 66 to 39 % (N=8) and from 30 to 14 % (N=32) and Blue's fuel pool from
  109 to 21 (N=8): a strike on refineries is plainly visible in Blue's tanks.
* It does not slow Blue's war. Even at N=32 with the refineries down (14 % tanks, 59 empty), Blue wins 6/6 at hour 163. A refill reaches a unit on its next resupply
  pass (20 min for attack orders, with the 6x attack-role share), so an empty tank is a pause, not a stop.
* Blue striking Red's refineries (bred) only trims Red's fuel (pool 83 -> 30 at N=8, tank 80 -> 75 %): Red does not need to move.
* Battalion supply % goes *up* with a tank (Red 83 -> 84-90, Blue 93 -> 97): fuel no longer takes half a point of the shortfall, so the same supply pool covers
  ammunition alone. Supply is therefore less of a story with a tank, not more.
* "Moves refused" per day in the `--fuel` table counts move attempts of empty-tank units, not lost kilometres; read it as an indicator, not a distance.

**Verdict.** The same shape as power plants: a side can be made to look starved, and Blue's strikes hurt Red little because Red barely advances, but the win date does not
shift beyond noise. A fuel tank that makes the *attacker's pace* depend on the pool would need a refill that is rationed (a refill per pass capped well below a
full tank) or a speed that falls with the tank, neither of which is a knob today. Not recommended for the mod as is; the extra code (a per-unit field, hence a
save change, in the real game) buys visible tank levels but no change to the war.

**Left open.** Larger N (64+), a rationed refill, a speed penalty instead of a hard stop, 12 seeds to resolve +-15 %, 6-seed `f1` not repeated on the t2-A (burn x3, best-of-2) setting.
Runner and exe for this screen: `ff-supply-build\_fuel_run` (the session scratchpad was cleared mid-run and the queue restarted from there).

## 15. Aircraft fuel (2026-11-03): tracked per squadron, never read, and about 100 times out of scale

Asked while looking for what a refinery strike could do: is aircraft fuel counted anywhere? Yes, but nothing acts on it.

**What the engine does (`squadron.cpp`).** A squadron holds one fuel stock. A flight takes `fuelAvail x planes / 100` from it when it is created (`UpdateSquadronStores`,
`flight.cpp:3437`) and gives back what it did not burn on landing (`ResupplySquadronStores`, `LoadoutSquadStoreResupply`, on). The national fuel pool refills it at
`SUPPLY_PT_FUEL` = 10,000 stock units per pool point (`SquadronClass::SupplyUnit`). The stock is clamped at zero and **nothing tests it**: a flight launches from a squadron
whose stock is empty (`HasFuel` and `CheckFuel` look at the flight's own tanks). The stated need is 4 sorties per plane per hour for 4 hours.

**Instrument (scratch campsim only).** `AIRFUEL` lines per 6 h and team: units taken, returned, refilled, launches that found less than they took, squadrons, aircraft, mean
stock (also in four-ship sorties), squadrons under 4 sorties. `supply_inventory.py --air <tags>` summarises. Tag `a1-base`, mod as shipped, 6 seeds x 10 days.

| Team, days | taken / day | returned | **net / day** | refilled / day | stock left (four-ship sorties per squadron) | launches short | net as pool points if a stock unit were a pound | what its refineries make a day |
|---|---|---|---|---|---|---|---|---|
| Blue 2-3 | 347k | 241k | 106k | 1.64M | 6,470 | 4 | 1,060 | 3,199 |
| Blue 4-7 | 651k | 468k | 183k | 7.67M | 4,680 | 386 | 1,832 | 3,094 |
| Blue 8-10 | 715k | 549k | 166k | 6.95M | 4,990 | 364 | 1,659 | 3,491 |
| Red 2-3 | 515k | 323k | 192k | 2.25M | 6,379 | 5 | 1,915 | 1,320 |
| Red 4-7 | 840k | 591k | 249k | 1.37M | 6,166 | 1 | 2,486 | 1,089 |
| Red 8-10 | 1,109k | 1,004k | 105k | 1.15M | 6,177 | 0 | 1,047 | 735 |

* **The stock cannot bind.** An original squadron starts with about 6,700 four-ship sorties of fuel (Blue 29 squadrons, mean 4.85 M units; Red 47, 2.68 M) and the whole
  air force uses 0.1-0.2 % of it a day. The pool refills 10-40 times what is used.
* **The exceptions are reinforcement squadrons.** Blue goes from 29 to 49 squadrons between hours 72 and 96; 6 of them sit under 4 sorties for the rest of the war, and
  10-48 launches per 6 h find too little stock. They fly anyway.
* **Scale.** The sortie draw divides pounds by 100; the pool pays 10,000 stock units per point, so one pool point is about 36 four-ship sorties instead of the 0.36 that
  10,000 lb would be. Read as pounds (not checked: whether `class_data->Fuel` is pounds), the air force would use **1,000-1,800 pool points a day for Blue (30-60 % of what
  its refineries make) and 1,000-2,500 for Red (140-230 % of what its two refineries make)**. So at honest scale Red could not fly its day on its refineries, and a
  refinery strike would ground an air force. Today it does nothing to air operations.
* **For the choice question:** refineries currently touch only ground fuel (half-weight in battalion supply, section 4a). Making the squadron stock real (a sortie needs
  stock; the conversion matched to pounds; reinforcement squadrons arriving with a tank) would give refineries a second, distinct job that a player can see and act on:
  aircraft out of fuel. Not built. It is a game-balance change (Red would be fuel-limited from day 1) that needs its own runs.

## 16. Legibility and the aircraft-fuel prototype, results (2026-11-03)

Goal changed this session: not "supply as a dial for a longer war" but "targets that each do something different, that the player can see, with counterplay".
Two things were built (main tree: news; scratch tree `ff-supply-build`: aircraft fuel). Nothing is committed.

### 16a. Supply news (`g_bSupplyNews`, default on; `SupplyNews()` in supply.cpp)

Six-hour check per team, 3 h minimum gap per kind, hysteresis (down 65/30, up 75/40; front 50/30, up 60/40). Kinds: industry (power/damage), fuel output, front supply.
Strings 1870-1880 added to the four campaigns' `Strings.txt` (backups `.bak-pre-supplynews`).

campsim, 6 seeds x 10 days, strike at hour 48 (`blowat=48`), announcements per run (team 2 = Blue, team 6 = Red):

| run | what it announced |
| --- | --- |
| a2-base (no strike) | 2-4 per seed, all fuel hurt/recovered or late industry hurt; none before h48 except one |
| a2-pred (Blue hits Red power) | 6/6 seeds: Red "industry crippled" **and** "fuel crippled" at h54; then "front supply hurt/crippled" at h96-138 |
| a2-pblue (Red hits Blue power) | 6/6 seeds: Blue industry + fuel crippled at h54; Blue front hurt h90-102 in 4/6 |
| a2-bred (Blue hits Red refineries) | 6/6 seeds: Red **fuel crippled only** at h54, no industry line (right: refineries are not factories) |

So a strike produces its own line within one check (6 h), the three target classes read differently (power = industry + fuel + later front; refinery = fuel only), and
there is no spam (up to 10 lines in a power run, three or four typical). Flapping seen once: pred-s2 "front crippled" at h162, 174, 198 (no recovery line between;
2->1 is silent by design) - fine, but could be given a longer gap.
**Not verified:** the rendered text. campsim does not build event text; the `#LE`/`#LT` tokens and the blip need an in-game or UI-harness check.

### 16b. Aircraft fuel prototype (scratch only; knobs `airfuel=N`, `airburn=PCT`)

Tank = N loads per aircraft (load = class Fuel/100), pool point = 100 stock units (pound scale), flight not built if squadron stock < 2 aircraft loads.
6 seeds x 10 days, flights/day (Blue / Red), days 1-3, 3-7, 7-10:

| run | Blue | Red | Blue wins by d10 (mean hour) |
| --- | --- | --- | --- |
| a3-base (no gate) | 801 / 1234 / 1448 | 1524 / 1956 / 2548 | 6/6 (180) |
| airfuel=8 | 642 / 595 / 551 | 1232 / 1282 / 1175 | 3/6 (202) |
| airfuel=8 + Blue hits Red refineries | 625 / 612 / 551 | 1170 / 1156 / 1051 | 5/6 (181) |
| airfuel=8 + Red hits Blue refineries | 578 / 404 / 337 | 1283 / 1352 / 1330 | 2/6 (214) |
| airfuel=8, burn 50 % | 755 / 815 / 664 | 1415 / 1604 / 1856 | 6/6 (182) |
| airfuel=8, burn 25 % | 674 / 972 / 1076 | 1377 / 1671 / 1799 | 5/6 (178) |

* The gate works and is the dominant effect: at 8 loads, both air forces fly about half of baseline from day 3 on, and the late-war surge disappears.
* A refinery strike now moves aircraft: Red hitting Blue's refineries (3,174 -> 124 points/day) cuts Blue flights 32 % and 39 % in the later windows. The reverse (Blue hitting
  Red, 1,320 -> 120/day) cuts Red flights only 5-10 %. Not understood: Red's squadrons are already near empty at 8 loads and sit at about 6 sorties each either way; needs a
  per-squadron time series before trusting either number.
* Win hours are within noise (6 seeds, +-40 h) except that a full-burn tank (8 loads) delays Blue (3/6, 2/6 vs 6/6): Blue's air force is the main killer (section 11), so grounding it
  slows the war. 50 % and 25 % burn put it back.
* "refused" counts every retry by air tasking, not lost sorties; use flights/day.

### 16c. Player-sized strikes against aircraft tanks (2026-11-04, tags `b1-*`, `b2-*`, 6 seeds x 10 days)

Strikes are `blowsel=refinery:TEAM:0:N:0` (the N refineries nearest the enemy, struck once, repairable): N=1 at hour 24, N=2 at hour 48 (Red has only two refineries, so N=2 is all of
Red's output). Tank = `airfuel` loads per aircraft; no-strike controls `b1-t4`, `b1-t6`, `a3-n8`. Flights per day over days 1-3 / 3-7 / 7-10; "vs control" is the same tank, no strike.

| tank | strike | Red flights vs control (Blue hits Red) | Blue flights vs control (Red hits Blue) |
| --- | --- | --- | --- |
| none | h24 / h48 | no change (within +-7 %) | no change |
| 4 | h24, 1 refinery | -4 / -12 / -24 % | -1 / -14 / -13 % |
| 4 | h48, 2 refineries | -7 / -18 / -32 % | +3 / -15 / -14 % |
| 6 | h24 | -8 / -11 / -6 % | -1 / -7 / -7 % |
| 6 | h48 | -8 / -19 / -25 % | -16 / -6 / -13 % |
| 8 | h24 | 0 / -1 / +2 % | -7 / +1 / -7 % |
| 8 | h48 | 0 / -2 / -11 % | -11 / -3 / -10 % |

* **No tank size gives a 20 % drop within a day.** The effect arrives over days, because squadron stock is a buffer (Red's 58 squadrons hold about a week of its net burn): the biggest
  drops are Red on days 7-10 with tank 4 (-24 % to -32 %). A repairable one-refinery strike on Blue costs Blue about 13 % of its flights from day 3.
* Recovery is slow: Red's refinery output after the h24 strike stays 720-960 points/day against 1,320; after the h48 strike it is 160-380 on day 7-10.
* Tank sweep with no strike: tank 2 already throttles hard (Blue 411-519 flights/day against 555-633 at tank 4-6, Red 1,093 -> 818 against 1,250-1,290); 50 % burn gives most of it back
  (tank 4: Blue 641-745, Red 1,378-1,591). Blue wins by day 10: tank 2 4/6, 4 5/6, 6 4/6; none 6/6. Win hours are not resolved at 6 seeds.
* Verdict: the stock tank makes refineries matter on a scale of days, and more for Red than Blue. To get a felt-within-hours effect the buffer has to shrink (tank 1-2) or the
  pool itself has to gate launches. Not tested.

### 16d. Tank 1 and 2 with player-sized strikes (2026-11-05, tags `c1-*`, 12 seeds x 10 days)

Flights per day, days 1-3 / 3-7 / 7-10, against the same tank with no strike (`c1-t2`: Blue 455/512/473, Red 1,085/1,104/908; `c1-t1`: Blue 345/467/430, Red 679/726/620).

| tank | strike | Red flights (Blue hits Red) | Blue flights (Red hits Blue) |
| --- | --- | --- | --- |
| 2 | h24, 1 refinery | -6 / -14 / -23 % | -10 / 0 / +3 % |
| 2 | h48, 2 refineries | -8 / -34 / -43 % | -1 / -10 / -5 % |
| 1 | h24 | -9 / -5 / -14 % | -1 / -3 / +2 % |
| 1 | h48 | -2 / -23 / -25 % | -3 / -19 / -9 % |

* Still no 20 % drop in the first day or two (-1 to -10 %). The best case is losing both of Red's refineries at hour 48 with tank 2: Red flights -34 % on days 3-7, -43 % on days 7-10.
* Tank 1 is not better than 2: a tank 1 control already flies half of what the unrestricted air force does (Red 679 against about 1,520), so a strike has less left to take.
* A one-refinery strike at hour 24 costs the struck side 5-23 % over the war; Blue's air force loses at most 19 % (tank 1, hour 48).
* Blue wins by day 10 (12 seeds): control tank 2 7/12, tank 1 7/12; strike runs 2/12-9/12 with no pattern except tank 2 + Red striking at hour 48 (2/12). Not resolved.
* Conclusion: a squadron stock tank, whatever its size, makes refineries matter on a scale of days, not hours. Not yet measured: the national fuel pool level (it may be a second buffer).

### 16e. The national fuel pool is not a buffer (2026-11-05, tags `d1-*`, 6 seeds x 10 days, exe8 logs `fuel pool` in the AIRFUEL line)

Code: `ProduceSupplies` (supply.cpp:975) sets the pool to this cycle's refinery output plus **half of last cycle's leftover**, so it is a flow that decays by half per cycle, not a stockpile.
Measured (mean over 6 seeds, pool points at hours 6 / 24 / 48 / 54 / 72 / 120 / 240):

| run | Red pool | Blue pool |
| --- | --- | --- |
| no gate, no strike | 83 / 136 / 131 / 110 / 133 / 100 / 59 | 207 / 178 / 187 / 173 / 176 / 202 / 213 |
| tank 2, no strike | 96 / 162 / 150 / 125 / 123 / 113 / 85 | 220 / 244 / 229 / 216 / 241 / 192 / 238 |
| tank 2, both of the side's refineries struck at hour 48 | Red 98 / 166 / 144 / **43** / **40** / 19 / 38 | Blue 211 / 199 / 238 / **162** / 172 / 182 / 204 |

* A refinery strike reaches the pool within one 6 h reading: Red -70 % at h54 and -87 % by day 5; Blue -25 to -40 % (Blue has more refineries than the two nearest the enemy) and partly recovers.
* So the delay between a strike and the flights falling (section 16c/16d: days, not hours) is not the pool. It is the squadron stock, and even at tank 1 flights fall less than the pool does.
  Not explained: why Red's flights fall only 25-43 % when its pool falls 70-87 %. Candidates: sorties return 85 % of the fuel they load, so net need is a fraction of the draw; flights are limited by tasking.
* A launch gate on the pool would react within hours, but the pool is a noisy flow reading (60-250 points), so it is not a usable gate as it stands.

### 16f. Why the starting stock is huge, and in every campaign (2026-11-05)

Origin (original game code, not ours): `SquadronClass` constructor, squadron.cpp:173: `fuel = Fuel * 24 * SQUADRON_MISSIONS_PER_HOUR(4) * MIN_RESUPPLY * 2 / 60` (= Fuel x 384 with the data's resupply interval).
`SQUADRON_PT_FUEL` = 100 ("lbs each point of fuel is worth") and `SUPPLY_PT_FUEL` = 10,000 ("lbs of fuel each point of supply fuel is worth"): the stock is a count of 100 lb points, the formula
gives it in pounds (class Fuel is pounds) times a day-of-flying multiplier, and the pool refill pays 10,000 *stock points* (1,000,000 lb) for a 10,000 lb pool point. Inference: the intent was a few days of
full-tempo flying, and the missing /100 (and the 10,000 refill) make it about 100x that. The 4.85 M mean stock for Blue's F-16 squadrons is 384 x about 12,630 (class Fuel).

One day of campsim per theater (seed 1, mean squadron stock at hour 6, four-ship sorties per squadron; days at the opening tempo = sorties / flights per squadron-day):

| theater | team 2 sorties / days | team 6 sorties / days |
| --- | --- | --- |
| Korea 2012 | 6,732 / ~126 | 7,124 / ~118 |
| Korea 1980s | 5,525 / ~78 | 7,631 / ~85 |
| EuroWar | 6,200 / ~67 | 5,803 / ~64 |
| Israeli | 9,569 / ~46 | 9,571 / (no flights in the first 6 h) |
| Israel 2012 | 9,492 / ~76 | 10,266 / ~118 |
| Israel Classic | 9,531 / ~72 | 9,561 / (small force) |
| Taiwan | 10,152 / ~104 | 18,226 / ~257 |

Every campaign starts with 46 to 250 days of full-tempo flying; none can run out of aircraft fuel in a war. The national fuel pool at hour 6 is 54-374 points in Korea/EuroWar/Taiwan and 1,200-1,600 for the Israel
Red sides.

## 17. Goal (set 2026-11-05): a realistic aircraft fuel stockpile for Korea Rolling Fire, tested as player choices

**What we control:** stockpile (squadron stock), burn per unit, production (refinery output). Settled in discussion: burn stays physical, production stays campaign data, **stock is the lever to own**, and it is the one with a clearly wrong value
(about 120 days of full-tempo flying, a 100x scale slip in the original code, section 16f).

**Definition.** `airdays=D` (scratch campsim, exe9): a squadron holds at most D days of the Rolling Fire opening tempo, measured from `a3-base` days 1-3 as units drawn at launch per aircraft per day
(Blue 927, the North 1,667), independent of class fuel; it starts full; the national pool refills it as it is used (a pool point = 100 stock units); a flight is not built if the squadron cannot cover two aircraft.
12-day runs, 6 seeds, `noend=1`, mod as shipped, `usecfg=1`.

**Experiment.**
* **A. Stock sweep (12 runs):** D = 1, 2, 3, 5, 7, 14, each with no strike (control) and with both sides' refineries destroyed at hour 0 (struck once, repairable, not held down).
* **B. Player-choice matrix (28 runs) at D = 3 and 7:** destroy all of Red's refineries, all of Blue's, or both, at hour 0, 12, 24, 48 or 96 (hour-0 both is in A); no strike is the control.
* Tags `f2-d<D>-ctl`, `f2-d<D>-<red|blue|both>-h<H>`; queues `_fuel_run/queue_f2a.txt`, `queue_f2b.txt` (they start after the section 16 follow-up runs `e1` and `g1` finish). Read with
  `tools/campsim/fuel_response.py` (hours until the struck side's flights are 20 % down, change at +24/+72/+120 h, stock left).

**Success criteria for the chosen D:**
1. **No baseline distortion:** with no strike, flights per day within 10 % of the unrestricted war (`a3-base`) for both sides, and Blue still wins at about the same rate.
2. **A real clock:** destroying a side's refineries at hour 0 or 12 grounds a clear part of its air force (flights down at least 20 %) within the first week; destroying them at hour 96 does much less (stock buys time).
3. **Choice with consequences:** the six cases (Red/Blue/both x early/late) give visibly different outcomes in time to effect and size, and the attacker's side is not hurt by its own strike more than by the enemy's.
4. **Counterplay:** the response curve leaves time to repair or resupply (time to effect at least about a day).
Not part of this round: ground fuel tank, navy, refinery repair speed. Follow-up from 16d: a 12-seed series gives T20 = 30 h for Red with tank 2 and both refineries struck at hour 48 (flights -20 % at +24 h, -38 % at +72 h):
the 6-seed windowed table in 16c/16d hid that by averaging days.

### 16g. Triple burn (`airfuel=6 airburn=300`, tags `e1-*`, 6 seeds x 10 days, 2026-11-05)

The burn lever did not make refineries matter. Control: Red net burn 1,050-1,480 points/day against 1,080-1,320 produced (so Red is now at the limit), but the control itself is throttled to Blue 363-490 / Red 1,042-1,208 flights/day by the small cap
(4 sorties of stock per squadron throughout). Strikes against that: Red (h24, 1 refinery) +7 / +1 / 0 % at +24 / +72 / +120 h; Red (h48, both) -2 / -8 / -9 %; Blue (h24) -5 / +3 / -10 %. No T20 reached.
A cap smaller than the surge makes the cap, not production, the limit; scaling draw and return together (airburn) only makes the cap smaller in flights. Burn is not the lever to tune; the stock sweep (section 17) is.

### 16h. Both sides lose every refinery at hour 0 (tags `g1-*`, 6 seeds x 10 days, held down, 2026-11-05)

Flights per day, days 1-3 / 3-7 / 7-10, against the same setting with refineries intact; wins by day 10 in brackets.

| model | Blue | Red |
| --- | --- | --- |
| shipped (stock about 120 days) | 769 / 1,198 / 1,441 against 801 / 1,234 / 1,448 (6/6 at h174, was 6/6 at h180) | 1,462 / 2,062 / 2,704 against 1,524 / 1,956 / 2,548 |
| air tank 8 (`a3-n8`) | 495 / 332 / 287 against 642 / 595 / 551: **-23 / -44 / -48 %** (0/6 against 3/6) | 1,170 / 1,220 / 992 against 1,232 / 1,282 / 1,175: -5 / -5 / -16 % |
| air tank 8 + ground tank 8 | 440 / 326 / 307 against 543 / 542 / 490: -19 / -40 / -37 %; ground tank 49 -> 24 %, empty 17 -> 43 (0/6 against 4/6) | 1,106 / 1,108 / 901 against 1,271 / 1,358 / 1,369: -13 / -18 / -34 %; ground tank 72 -> 69 %, empty 35 -> 44 |

Shipped: losing all fuel production at hour 0 changes nothing in ten days (battalion supply, flights, win hour all within noise). With a tank (0.4-0.8 days of tempo), Blue's air force is down 23 % in the first three days and about 45 %
afterwards; Red's holds for a week and is 16-34 % down by day 7-10. The side that must fly and drive most (Blue) is hurt first and most, and Blue wins 0/6 with air + ground tanks; the North's low burn is its protection.

## 18. Results of the stock sweep and player-choice matrix (tags `f2-*`, exe9, 12 days, 6 seeds, 2026-11-06)

**Verdict: no D from 1 to 14 days passes criterion 1. The stock size is not what limits Blue's flying in this prototype; something in the refill path is, so `airdays` alone cannot be the lever yet.**

### 18a. Controls (no strike) against the unrestricted war `a3-base`; flights per day, days 1-3 / 3-7 / 7-10

| D (days) | Blue flights/day | against base 801 / 1,234 / 1,448 | Red flights/day | against base 1,524 / 1,956 / 2,548 | Blue refused for fuel per day | Blue wins (mean hour) |
|---|---|---|---|---|---|---|
| 1 | 626 / 776 / 802 | -22 / -37 / -45 % | 1,205 / 1,637 / 2,016 | -21 / -16 / -21 % | 1,259 / 2,949 / 2,926 | 6/6, 169 |
| 2 | 660 / 932 / 869 | -18 / -24 / -40 % | 1,359 / 1,580 / 2,040 | -11 / -19 / -20 % | 828 / 2,748 / 2,950 | 6/6, 184 |
| 3 | 671 / 811 / 780 | -16 / -34 / -46 % | 1,292 / 1,521 / 1,925 | -15 / -22 / -24 % | 1,015 / 2,577 / 3,456 | 6/6, 195 |
| 5 | 705 / 885 / 960 | -12 / -28 / -34 % | 1,276 / 1,454 / 1,826 | -16 / -26 / -28 % | 1,060 / 1,893 / 2,939 | 6/6, 212 |
| 7 | 676 / 940 / 940 | -16 / -24 / -35 % | 1,390 / 1,617 / 1,790 | -9 / -17 / -30 % | 851 / 2,409 / 3,010 | 6/6, 184 |
| 14 | 663 / 887 / 911 | -17 / -28 / -37 % | 1,329 / 1,584 / 1,977 | -13 / -19 / -22 % | 782 / 2,365 / 3,046 | 6/6, 190 |
| base | 801 / 1,234 / 1,448 | | 1,524 / 1,956 / 2,548 | | 0 | 6/6, 180 |

* Fourteen days of stock fly no more than one day of stock (Blue 663 against 626 in days 1-3, 911 against 802 in days 7-10). The mean stock per squadron is large (D=14: 1,804 sorties at day 3, 1,100 at day 10)
  yet launches are still refused at the same rate. So the binding thing is not the average stock. My reading (not yet measured per squadron): the busy squadrons run dry while idle ones keep a full tank,
  and the pool refills a squadron only as it is used, at about 80-100 k units a day against 280-350 k taken at launch. The next step is to log the per-squadron stock distribution before touching D again.
* Win rate and hour are noise at this sample size: the six controls span 169-212 h, base 180 h, all 6/6.
* Battalion supply (ground is as shipped in these runs) is the same as base in every run (Red 74-86 %, Blue 91-95 %): an air-only stock does not touch the ground war.

### 18b. Player-choice matrix (flights per 6 h against the same-D control, struck side, mean over 6 seeds; T20 = hours after the strike until 20 % down)

| D | case | Blue T20 / +24 h / +72 h / +120 h | Red T20 / +24 h / +72 h / +120 h |
|---|---|---|---|
| 3 | Blue hit at h0 | 60 h / +11 / -26 / -29 % | |
| 3 | Blue hit at h12 | 48 h / -3 / -2 / -30 % | |
| 3 | Blue hit at h24 | 30 h / -12 / -5 / -25 % | |
| 3 | Blue hit at h48 | 12 h / -32 / -39 / -33 % | |
| 3 | Blue hit at h96 | 18 h / -26 / -22 / -29 % | |
| 3 | Red hit at h0 / 12 / 24 / 48 / 96 | | none / none / none / none / 138 h (-8 to -20 % at +72/+120 h at most) |
| 3 | both at h0 | 72 h / +10 / -26 / -16 % | none; -9 % at +72 h |
| 3 | both at h24 | 30 h / -13 / +1 / -26 % | 210 h; -6 / -2 / -5 % |
| 3 | both at h48 | 24 h / -24 / -24 / -26 % | none; +3 / +3 / -6 % |
| 7 | Blue hit at h0 / 12 / 24 / 48 / 96 | 72 / 54 / 48 / 24 / 24 h; at +120 h -17 / -21 / -12 / -30 / -31 % | |
| 7 | Red hit at h0 / 12 / 24 / 48 / 96 | | none for all five; at most -16 % |
| 7 | both at h48 | 24 h / -24 / -29 / -30 % | 48 h; -13 / -15 / -2 % |

* **Blue is the side that feels it.** A Blue strike at hour 48 or later bites in 12-24 hours (-24 to -33 % in the first day) and ends near -30 % whatever the day or D; a strike at hour 0 or 12 takes 48-72 hours, and the
  first day is even +11 % (the stock is full, the launch rate is capped by something else).
* **Red is almost untouched** at D = 3 and 7: one cell in ten reaches -20 % (D=3, hour 12, 120 h; hour 96, 138 h), most are -5 to -15 %. Its refineries make 1,300 a day against a need of about 400 (section 15), so
  one hit cannot matter and all of them in all of its plants did not either.
* **Later strikes work faster, not slower** (Blue hour 48: 12-24 h, hour 0: 60-72 h). The opening tempo is lower than the day-3 tempo, so the stock only starts to drain once the war is hot. That is the opposite of
  criterion 2 (early strikes should bite harder). Cause is the tempo ramp, not the stock.
* **Both sides struck at hour 0 changes the war a little, whatever D** (wins and mean win hour, Blue): D=1 4/6, 240; 2: 4/6, 213; 3: 5/6, 206; 5: 6/6, 227; 7: 6/6, 196; 14: 5/6, 230, against 6/6 and 169 / 184 / 195 / 212 / 184 / 190
  for the matching controls. The sign is the same in all six pairs (30/36 wins against 36/36, about 29 hours later on average), and it does not depend on D, which fits the refill-path reading above.
* Wins elsewhere in the matrix are 3/6 to 6/6 and win hours 152-227 with no pattern by strike side or hour: the air fuel stock does not decide the war on its own.

### 18c. Against the criteria

1. No baseline distortion: **fails at every D** (Blue flights -12 to -46 %).
2. A real clock: Blue yes (about -30 % from day 4 on); Red no. Early strikes are not stronger than late ones.
3. Choice with consequences: Blue's three cases (early, late, both) differ in time-to-effect (12-72 h) but end the same; Red's cases are all about nothing; so the six cases do not give six outcomes.
4. Counterplay: time to effect is 12-72 h for Blue (at least a day except for strikes at hour 48+, which are 12-24 h), so only just.

### 18d. What this changes

* Do not ship `airdays`/`airfuel` as it is. The stock number is not the problem to solve; the squadron refill path (how fast a launched flight's draw is replaced from the pool, and who gets it first) is.
* Next experiment, before any more D values: (1) log per-squadron stock (min / median / max, count empty) in the AIRFUEL line; (2) try a refill that tops a squadron up from the pool at the start of its tasking cycle,
  not only as it is used; (3) rerun D = 3 and 7 controls and check they land within 10 % of `a3-base`. Only then does the strike matrix mean anything.
* Red's insensitivity is a data fact (surplus refining), not a stock one. If the player is to have a Red target class that matters, it is the stock *or* the surplus, and the surplus is campaign data.

## 19. Per-squadron stock and a top-up before launch (tags `g2-*`, `g3-*`, exe10, 12 days, 6 seeds, 2026-11-06)

New in the scratch campsim (exe10): an `AIRSPREAD` line per team every 6 h (sorties of fuel per squadron: min / p10 / median / p90 / max, squadrons under 2 sorties, fuel the squadrons ask the pool for), read with
`tools/campsim/air_spread.py`; and `airtopup=1`: a squadron that cannot cover a flight is topped up to its cap from the national pool at that moment, and refuses only if the pool is empty too.

### 19a. The mean stock in 18a was misleading
Blue, D = 3, no strike (`g2-d3-ctl`): median 30 sorties per squadron at hour 24 (18 at hour 72, 4 at hour 168) against a mean of 418; the maximum is 6,890 (a squadron of a small-fuel class, which inflates the sorties
count). Red: median 7, 6, 5; max 750. Red's squadrons ask the pool for 12,300 / 25,300 / 31,900 points; the pool is a few hundred. So: **most squadrons are nearly empty from day 1; the refill passes cannot cover what they ask for;
a handful of outlier squadrons make the average look full.** That is the reason the stock size did not matter in 18a (D = 14 changed nothing).

### 19b. With the top-up, the baseline comes back for Blue (flights per day, days 1-3 / 3-7 / 7-10, against `a3-base`)
| | Blue | Red |
|---|---|---|
| `a3-base` | 801 / 1,234 / 1,448 | 1,524 / 1,956 / 2,548 |
| D=3, top-up | 676 / 1,199 / 1,379: -16 / -3 / -5 % | 1,396 / 1,627 / 1,720: -8 / -17 / -33 % |
| D=7, top-up | 690 / 1,227 / 1,450: -14 / -1 / 0 % | 1,470 / 1,746 / 1,671: -4 / -11 / -34 % |
| D=14, top-up | 680 / 1,202 / 1,384: -15 / -3 / -4 % | 1,463 / 1,625 / 1,956: -4 / -17 / -23 % |
Without the top-up (18a) Blue was -12 to -46 %. Launches refused for fuel fall from about 1,000-3,000 a day to 70-340. Blue still wins 6/6 (hour 158-200 in all controls).
* Blue days 1-3 stay about 15 % below base (cause not looked at: probably the opening pool).
* Red days 7-10 are 23-34 % below base: by then Blue has cut its refineries to about 630-830 a day (from 1,320), and with a realistic stock the pool binds. The 120-day stock hid this.
* Stock size (D = 3 / 7 / 14) hardly changes anything with the top-up, because the median squadron holds only a few sorties; the outliers hold the rest.

### 19c. Strike matrix with the top-up (D = 7, control `g2-d7-top`; flights per 6 h of the struck side against it)
| strike | Blue T20 / +24 / +72 / +120 h | Red T20 / +24 / +72 / +120 h |
|---|---|---|
| Red at h0 | | 72 h / +6 / -22 / -18 % |
| Red at h24 | | 54 h / +11 / -3 / -14 % |
| Red at h96 | | 12 h / -22 / +6 / -9 % |
| Blue at h0 | 90 h / +9 / -5 / -8 % | |
| Blue at h24 | 72 h / -5 / -24 / -12 % | |
| Blue at h96 | 12 h / -9 / -8 / -13 % | |
| both at h0 | 90 h / +12 / +13 / -6 % | 78 h / +3 / -12 / -6 % |
| both at h24 | 60 h / +4 / -45 / +8 % | 48 h / -2 / -23 / -2 % |
| both at h96 | none / -19 / -12 / -7 % | none / -5 / +8 / 0 % |
* Blue wins 6/6 in every run (hour 172-204 against 168 in the control): no effect on the war.
* The effect of a strike is small and noisy (typically -5 to -25 % at +72 h, back to -5 to -15 % at +120 h; the 6-seed noise is about 10 points, e.g. both at h24 +72 h -45 %, +120 h +8 %). A strike at hour 0 takes 70-90 h
  to reach -20 % (if it ever does), at hour 24 50-70 h: slow enough for counterplay, too weak to be a choice. Late strikes (h96) show a quick dip (12 h) that disappears again.
* Against the section 17 criteria: (1) baseline: Blue yes from day 3, Red no late; (2) clock: weak; (3) six cases distinct: no; (4) counterplay: yes, but there is little to counter.

### 19d. What this says
* The stock is not the lever; the **refill path** was, and the top-up fixes it. With it the national pool (a flow: production plus half the last leftover) is the only thing standing between a strike and the squadrons.
* A strike now removes a surplus, not a stock: Blue's refineries make about 3,300 a day against a net air use of about 1,700 (a margin of about 2x), Red's 1,300 against 400-1,300; a struck plant that is repaired within days
  (hold 0) costs the pool little. That fits the small effects above. It is my reading; not tested.
* To make a strike a real choice the next dials are **production margin** (how much the refineries make over what the air force burns) and **repair time**, both campaign data, with the top-up on. Untested: Blue refineries at
  50-70 % of their output, and the same strikes held down (`:hold`) so a hit stays a hit.
* Not shipped: `airdays`, `airfuel`, `airtopup` are scratch-tree knobs only.

## 20. Output cuts, held-down strikes and the offensive fuel cheat (tags `g4-*`, `g5-*`, exe11/exe12, D = 7 + top-up, 12 days, 6 seeds, 2026-11-07)

New scratch knobs: `fuelcut=TEAM:PCT` (that team's refineries make PCT % of their fuel; team 2 = Blue, 6 = the North) and `offfuel=0`: a ground offensive (`TeamClass` offensive start, `team.cpp`) normally tops the attacker's fuel pool up to a floor
(`fuelAvail < initiative-scaled sup`, `StartOffBonusFuel`) and caps the enemy's pool to the same scale, whatever the refineries make; `offfuel=0` removes both. `g4` runs have it on (stock), `g5` off. Held-down strike = `blowsel=refinery:TEAM:0:0:1`
(every refinery of that side, destroyed at the hour and kept destroyed). Controls: `g2-d7-top` (cheat on), `g5-off0` (cheat off).

### 20a. Output cuts, no strike (flights per day, days 1-3 / 3-7 / 7-10)
| setting | Blue | Red |
|---|---|---|
| control `g2-d7-top` | 690 / 1,227 / 1,450 | 1,470 / 1,746 / 1,671 |
| Blue at 60 % (`g4-b60`) | 658 / 1,154 / 1,279: -5 / -6 / -12 % | |
| Blue at 40 % (`g4-b40`) | 749 / 1,202 / 1,507: +9 / -2 / +4 % | |
| Red at 50 % (`g4-r50`) | | 1,458 / 1,634 / 1,853: -1 / -6 / +11 % |
| control, cheat off (`g5-off0`) | 741 / 1,173 / 1,424 | 1,463 / 1,592 / 2,047 |
| Blue at 40 %, cheat off (`g5-off0-b40`) | 750 / 1,179 / 1,257: +1 / +1 / -12 % | |
* **Cutting output to 40 % does not bite**, with the cheat on or off. Blue's net use is about 1,460 points a day (net units / 100) and 40 % of its output is about 1,230-1,370, so about 85 % of the use is covered, and flights follow (-0 to -12 %).
  Production is about 2x the burn, so an output cut needs to go below about 30 % to matter. The offensive cheat is not what shields Blue (off0 gives the same).
* Of what a flight draws at launch about 70 % comes back on landing; only the net burn must be produced. That is why zero production still leaves about 60 % of the flights flying for five days (20b).

### 20b. Held-down strikes (struck side's flights against the same-cheat control; wins are Blue's, mean win hour)
| strike | cheat on: Blue/Red T20, +24 / +72 / +120 h | wins | cheat off: T20, +24 / +72 / +120 h | wins |
|---|---|---|---|---|
| control | | 6/6, 168 | | 6/6, 177 |
| Blue h0 | 84 h, +12 / +1 / -32 % | 4/6, 195 | | |
| Blue h24 | 66 h, -13 / -27 / -20 % | 6/6, 202 | 42 h, +5 / -10 / -38 % | **5/6, 241** |
| Blue h96 | 36 h, -12 / -25 / -23 % | 5/6, 160 | | |
| Red h0 | 84 h, -1 / +2 / -20 % | 6/6, 158 | | |
| Red h24 | 54 h, +7 / -13 / -10 % | 6/6, 165 | 120 h, +8 / +1 / -23 % | 6/6, 170 |
| Red h96 | 12 h, -29 / -8 / -21 % | 6/6, 166 | | |
| both h24 | Blue 42 h, -4 / -34 / -23 %; Red 60 h, -2 / -17 / +2 % | 6/6, 206 | Blue 84 h, -13 / -9 / -23 %; Red 204 h, -1 / 0 / 0 % | **2/6, 231** |
| both h96 | Blue 36 h, -10 / -24 / -38 %; Red none | 6/6, 188 | Blue 30 h, -14 / -36 / -43 %; Red none | 6/6, 183 |
| Blue 60 % + held Blue h24 | 84 h, +11 / -6 / -33 % | 4/6, 217 | | |
| Red 50 % + held Red h24 | 72 h, +3 / -24 / -8 % | 6/6, 173 | | |
* A held strike on **Blue** takes 30-85 h to reach -20 % and ends at -20 to -43 % by day 5-6 whenever it is struck; a strike on **Red** is -10 to -24 % at +72/+120 h at best. Red's production already falls on its own
  (1,320 to 630-830 a day by Blue's attacks), so the strike removes little more.
* With the offensive cheat off, a held Blue strike at hour 24 shifts Blue's win from hour 177 to 241 (5/6), and both sides struck at hour 24 gives Blue 2/6 (231 h); the same strikes with the cheat on stay 6/6 (202 / 206 h). Six seeds,
  win-hour spread about +/-20 h: this is the first war-level effect in the series, but one cell of six runs each; it needs a second set of seeds before it is believed.
* Late strikes (hour 96) bite fast on Blue (30-36 h) and end at -23 to -43 %.

### 20c. Reading
* What makes a strike matter, in order: the **refill path** (top-up, 19), **keeping the plant down** (held, 20b: a one-off hit that is repaired within days costs the pool nothing), and the **offensive cheat** (20b: it softens the war-level effect).
* **Output cuts alone are not a lever** at any reasonable setting (20a): production runs at twice the burn.
* **Red is the weak target class** in every variant (-10 to -24 % at best); Blue is the one that feels strikes. Either Red's surplus is data to change, or the player should be told strikes on the North's fuel do little.
* Not done: the offensive cheat's effect on supply (not just fuel) pools; a second seed set for the 5/6 and 2/6 cells; the news wording in the real game; nothing here is shipped (scratch knobs `airdays`, `airfuel`, `airtopup`, `fuelcut`, `offfuel`).

## 21. Stock locked at 3 days, every team sized from its own consumption (tags `h1-base`, `h2-*`; exe13/exe14; 12 seeds, 10 days; 2026-11-07)

`AIRFUEL` / `AIRSPREAD` now log every team that owns squadrons (`tools/campsim/team_air_rates.py` reads them). Teams with aircraft in Korea Rolling Fire: 2 (Blue), 4 (five squadrons, 51-57 aircraft, about 255 flights a day, about 4,400-4,900 units drawn per aircraft per day,
almost all of it returned: net 18-61 points a day, refineries 120), 5 (China), 6 (the North). New knob `airrates=T:R,...` (units drawn per aircraft per day) replaces the old Blue 927 / everyone-else 1,667 pair.

**Rate rule (data-driven):** each team's peak sustained tempo in the 12-seed shipped baseline `h1-base`: the highest 72 h average of units drawn at launch per aircraft per day. Result: team 2 = 1,372 (hours 90-162), team 4 = 4,583,
team 5 (China) = 2,733 (hours 168-240; it ramps late: 228 per aircraft per day on day 1, 2,719 by day 8), team 6 = 1,917. Cap = 3 x that per aircraft. The opening-tempo rates (927 / 1,667) would have starved China once it joined.

| flights per day, days 0-1 / 1-3 / 3-7 / 7-10 | `h1-base` (shipped) | `h2-d3-top` (3 days + refill, offensive fuel top-up on) | change |
|---|---|---|---|
| Blue | 1,009 / 712 / 1,238 / 1,440 | 986 / 666 / 1,176 / 1,403 | -2 / -6 / -5 / -3 % |
| China (5) | 237 / 316 / 743 / 1,234 | 237 / 325 / 575 / 723 | 0 / +3 / -23 / -41 % |
| North (6) | 1,694 / 1,354 / 1,985 / 2,801 | 1,598 / 1,390 / 1,660 / 2,054 | -6 / +3 / -16 / -27 % |
| team 4 | 257 / 256 / 254 | the same | 0 |
Blue wins: `h1-base` 12/12 at hour 184; `h2-d3-top` 11/12 at 180; with the offensive fuel top-up off (`h2-d3-top-off0`) 10/12 at 179. Blue's flights are now within 2-6 % of the baseline in every window; the day 1-3 gap of section 19 (-14 %) is gone
(the per-team rate is higher than the old 927). The side that falls behind is now the Red side late: China -23 / -41 % and the North -16 / -27 % in days 3-10, where their net use passes their refinery output (China 832-1,285 against 912-814; North 1,337-2,654 against 1,195-1,320 points a day).

## 22. Red refinery strikes at the locked 3-day per-team cap (12 seeds, 10 days, tags i1-*)

Settings: `airdays=3 airtopup=1 airrates=2:1372,4:4583,5:2733,6:1917`, control `h2-d3-top` (12 seeds, no strike). Strikes are held down
(the refinery stays destroyed) unless noted. Red = teams 4, 5, 6 (3 refineries: China 1, North 2, team 4 none, so i1-t4-h0 strikes nothing and is a second control: 10/12, 174 h, the noise floor).

### 22a. What the strike does to Red's fuel

Refinery output drops from 912 to 120 points a day for China and from 1,320 to 120 for the North (a strike at hour 0). Red flights per 6 h against the control, mean over seeds
(cheat = the offensive fuel top-up in `StartOffensive`):

| Strike | cheat | 24 h after | 72 h after | 120 h after | Red stock in sorties at +24/+72/+120 (control, from h0: 144/125/112) |
|---|---|---|---|---|---|
| all 3, held, h0 | on | +9% | -5% | -12% | 145/119/104 |
| all 3, held, h12 | on | 0% | -4% | -11% | 141/116/100 |
| all 3, held, h24 | on | +12% | +1% | -5% | 132/112/101 |
| all 3, held, h48 | on | +4% | -9% | -6% | 122/113/98 |
| all 3, held, h96 | on | -10% | -19% | -16% | 109/92/88 |
| all 3, struck once, h24 | on | +8% | -12% | -3% | 133/115/100 |
| nearest 1, held, h0 | on | +4% | +2% | 0% | 142/120/103 |
| China only, h0 | on | +9% | -3% | +4% | 148/124/113 |
| North only, h0 | on | +3% | -8% | -4% | 141/120/109 |
| all 3, held, h0 | off | -1% | +3% | -10% | 142/118/107 |
| all 3, held, h24 | off | +5% | -13% | -16% | 126/112/97 |

Flights by team, days 3-7 and 7-10 (h0 strike, cheat on): China 575 -> 517 and 723 -> 844 per day (the latter is the late ramp, not the strike), North 1,660 -> 1,419
and 2,054 -> 1,733 (-15%, -16%). With the cheat off the North falls 1,701 -> 1,444 and 1,884 -> 1,783.
Launch refills per 6 h, North, seed 3: 58,700 / 60,100 / 47,200 / 24,400 units at h24/72/120/168 in the control against 41,000 / 32,500 / 30,800 / 10,700 under the strike.
Refusals for fuel at h168: 284 in the control against 528 under the strike, so the strike does bite, but only from about day 5.

### 22b. What it does to the war

| Run | Blue wins | mean hour |
|---|---|---|
| control h2-d3-top | 11/12 | 180 |
| red h0 / h12 / h24 / h48 / h96 (held) | 12 / 12 / 10 / 10 / 11 of 12 | 174 / 172 / 180 / 178 / 168 |
| nearest 1, h0 / h24 | 12 / 11 | 172 / 185 |
| struck once h24 | 10/12 | 172 |
| China only / North only / team 4 only (h0) | 11 / 11 / 10 | 189 / 178 / 174 |
| control, cheat off | 10/12 | 179 |
| red h0 / h24, cheat off | 11 / 12 | 177 / 171 |

All within the 12-seed noise (a one-seed swing is 8 points of win rate and the win hour moves 10 h from seed to seed). Red ground supply % (24/72/120/168 h) is unchanged:
74/79/77/74 in the control against 72-74/79-82/75-78/75-79 under every strike. The ground tank was off in these runs, so a ground fuel effect is not measured here.

### 22c. Reading

* A refinery strike on Red is felt in the air only: stock ends 5-12% under the control after five days (104 against 112 sorties after an h0 strike) and flights fall 5-19% from day 3-5, because a 3-day tank outlasts
  a war that ends around day 7.5.
* Early strikes (h0-h24) do less than a strike at h96 in the first days because the refill path keeps stock topped from the pool in the opening days; hitting late catches the stock already drawn down.
* The nearest single refinery is nearly invisible (0 to +4%); the choice "which refinery" matters less than "all of them and keep them down".
* Turning the offensive fuel cheat off did not change the picture at this resolution.
* No result changes who wins or when. Refinery strikes alone are a visible but weak lever; ground fuel (tank on) is the open test.

## 23. Smaller tanks: 2 days and 1 day (12 seeds, 10 days, tags j1-*)

Same per-team rates and launch-time refill as section 22, `airdays=2` and `airdays=1`. Controls j1-d2-ctl and j1-d1-ctl; strikes are held on all 3 Red refineries from hour 0, 24 or 96.

Flights per day by window (0-1 / 1-3 / 3-7 / 7-10 days):

| Team | 3 days | 2 days | 1 day |
|---|---|---|---|
| Blue | 986 / 666 / 1,176 / 1,403 | 987 / 755 / 1,215 / 1,455 | 978 / 747 / 1,252 / 1,459 |
| China | 237 / 325 / 575 / 723 | 237 / 385 / 557 / 587 | 237 / 445 / 665 / 637 |
| North | 1,598 / 1,390 / 1,660 / 2,054 | 1,667 / 1,507 / 1,579 / 1,842 | 1,635 / 1,529 / 1,632 / 1,789 |

Blue wins: 3 days 11/12 at 180 h; 2 days 11/12 at 165 h; 1 day 12/12 at 165 h.

Strike effect on Red flights against the same-size control (at +24 / +72 / +120 h) and Red stock in sorties at +24/+72/+120:

| Run | flights | stock (sorties) | Blue wins, hour |
|---|---|---|---|
| 3 d, h0 (section 22) | +9 / -5 / -12% | 145/119/104 | 12/12, 174 |
| 2 d, h0 | -13 / -10 / +3% | 94/81/70 | 12/12, 174 |
| 2 d, h24 | -11 / -11 / -10% | 87/77/64 | 12/12, 170 |
| 2 d, h96 | +4 / -9 / -4% | 69/61/59 | 12/12, 171 |
| 1 d, h0 | -2 / -21 / -18% | 49/39/35 | 12/12, 168 |
| 1 d, h24 | -5 / -11 / -12% | 46/39/36 | 10/12, 184 |
| 1 d, h96 | -15 / -6 / -7% | 35/32/31 | 11/12, 172 |

Reading: a smaller tank does not slow the war (the refill tops it up from the pool at launch), and Blue wins as often as with 3 days. The strike bites sooner and a little harder
(-10 to -13% by +24 to +72 h at 2 days, -21% at +72 h at 1 day, against +9 to -5% at 3 days) but still moves no win rate beyond the 12-seed noise.

## 24. Why sorties are refused for fuel (12 seeds, 10 days, tags k1-*, scratch campsim exe15, AIRREFUSE lines and a pool ledger)

At every refused launch (flight.cpp) the scratch campsim now records the pool at the ask, the squadron's own cap and what the top-up took. 3-day cap, per-team rates, launch-time refill.

North (team 6), 3-day control, refusals per window and why:

| Days | refused | pool empty | pool short | cap below two aircraft | mean pool at the ask (points) | squadrons refused per 6 h | most refusals one squadron in 6 h |
|---|---|---|---|---|---|---|---|
| 0-1 | 255 | 243 | 0 | 12 | 8 | 10 | 26 |
| 1-3 | 1,187 | 1,172 | 5 | 10 | 4 | 15 | 56 |
| 3-7 | 3,351 | 3,220 | 22 | 110 | 3 | 16 | 81 |
| 7-10 | 2,015 | 1,883 | 10 | 121 | 3 | 12 | 62 |

China days 3-7: 2,931 refused, 2,889 with the pool empty, 39 with the cap below need. Blue days 3-7: 810 refused, 775 empty. 1-day control: the same shape (North 3-7: 3,138 refused, 2,935 empty, 186 cap).
Only 3-8% of top-ups took the whole pool, because the pool at the ask is a few points (a squadron wants about 100).

Who draws the pool (fuel points per day, 3-day control, ground = sp row 13, air = launch top-ups):

| Team, days | made by refineries | ground units | squadron top-ups |
|---|---|---|---|
| North 0-1 | 1,310 | 2,348 | 1,693 |
| North 1-3 | 1,258 | 216 | 2,306 |
| North 3-7 | 1,071 | 116 | 1,796 |
| China 3-7 | 904 | 12 | 874 |
| Blue 3-7 | 2,890 | 415 | 2,739 |

With the strike (h0) the refineries make 120 a day and the North's top-ups still take 1,344 / 942 / 325 a day in days 1-3 / 3-7 / 7-10: the pool is filled mainly by something other than refineries
(the offensive fuel top-up and the half of last cycle's leftover; not separated here).

Reading: refusals are not "pool had fuel but the flight did not get it". 96-99% of them are the pool at 0-9 points. After day 1 the squadrons are the pool's only user (91-99% of draws, ground < 10%).
About 3% (North late) are squadrons whose own cap is below two aircraft's worth and can never fly. A few squadrons (10-16 per 6 h, one up to 81 times) account for the refusals, so the count
is mostly the same squadrons retrying each cycle. Not yet measured: whether a refused flight is re-tasked to a squadron that has fuel.

## 25. A typical seed, and NK + China refineries dead at hour 0

Seed choice (8 surviving runs: k1-d3-ctl, k1-d3-red-h0, k1-d1-ctl, j1-d2-ctl, j1-d1-ctl, i1-red-h24/h48/h96): seed 4 is the most typical, 13 h mean and 48 h worst deviation from each run's median win hour and a win in all 8;
seeds 1, 3, 5, 12 follow (17-20 h); seed 8 stalls most (no win in 4 of 8). Viewer: http://localhost:8770/viewer.html?run=save0-k1-d3-ctl-s4

Team 4 has no refineries, so "all Red refineries held down from hour 0" (k1-d3-red-h0) is exactly NK plus China. Same seed, control against strike (3-day cap, 10 days):

| Seed | Blue win hour control -> strike | North flights/d days 1-3 | days 3-7 | China flights/d days 3-7 | days 7-10 |
|---|---|---|---|---|---|
| 4 | 190 -> 161 | 1,636 -> 1,362 | 1,834 -> 1,558 | 494 -> 724 | 559 -> 905 |
| 1 | 206 -> 177 | 1,266 -> 1,250 | 1,629 -> 1,509 | 697 -> 766 | 685 -> 908 |
| 3 | 152 -> 160 | 1,232 -> 1,730 | 1,826 -> 1,564 | 651 -> 626 | 481 -> 709 |
| 5 | 194 -> 152 | 1,418 -> 1,298 | 1,678 -> 1,681 | 632 -> 480 | 925 -> 863 |
| 12 | 218 -> 179 | 1,204 -> 1,444 | 1,498 -> 1,642 | 464 -> 521 | 1,415 -> 916 |

Seed 4: North refused launches per day 514 -> 818 (days 1-3) and 1,017 -> 1,626 (days 3-7); stock stays at about 97-111 sorties against 100-119; Red battalion supply stays at 98%.
Blue won earlier with the strike in 4 of 5 seeds (mean 192 -> 166 h) but the same-config control spread is 10 h and the win hour moves 30 h between seeds, so five seeds do not establish an effect.

## 26. Stock defined as 3 days of net burn: Red refinery strikes (12 seeds, 10 days, tags m1-base, n1-*; exe16; 2026-11-07)

Definition used from here on: stock cap = 3 x the peak sustained 72 h net burn (drawn at launch minus returned on landing) per aircraft per day, measured on the uncapped run `m1-base` (12/12 Blue wins, mean hour 179). Net rates: Blue 382, China 909, North 668 units per aircraft per day; team 4 (net 58) is too small to fly a sortie, so it keeps a 1-day gross cap (1,528). Knobs: `airdays=3 airtopup=1 airrates=2:382,4:1528,5:909,6:668`.

Outcome, 12 seeds each (strike = every Red refinery held down from the stated hour). Seed links: [control seed 4](http://localhost:8770/viewer.html?run=save0-n1-ctl-s4), [strike at h0 seed 4](http://localhost:8770/viewer.html?run=save0-n1-red-h0-s4), [strike at h24 seed 8, the seed Blue failed to finish](http://localhost:8770/viewer.html?run=save0-n1-red-h24-s8), [uncapped baseline seed 4](http://localhost:8770/viewer.html?run=save0-m1-base-s4).

| Tag | Blue wins | Mean win hour | Red battalion supply % d1/d3/d5/d7 | Red battalions under 25 % |
|---|---|---|---|---|
| m1-base (uncapped) | 12/12 | 179 | 75 / 83 / 81 / 81 | 3 |
| n1-ctl (cap, no strike) | 12/12 | 179 | 73 / 81 / 76 / 77 | 3 |
| n1-red-h0 | 12/12 | 181 | 74 / 80 / 74 / 74 | 3 |
| n1-red-h24 | 11/12 | 176 | 73 / 78 / 75 / 73 | 5 |
| n1-red-h48 | 11/12 | 166 | 74 / 81 / 78 / 80 | 3 |
| n1-red-h96 | 11/12 | 166 | 74 / 78 / 74 / 74 | 6 |

The one miss in each of h24, h48 and h96 is seed 8, which is also the slowest seed in the control (win at 222 h, 18 h from the 240 h cutoff); the h0 strike did not miss it. Win hours move by 30+ h between seeds, so this is not a strike effect I can separate from noise.

Flights per day (mean over seeds, control -> strike at h0), days 3-7 and 7-10:

| Team | days 3-7 | days 7-10 |
|---|---|---|
| North (6) | 1,598 -> 1,496 (-6 %) | 2,004 -> 1,695 (-15 %) |
| China (5) | 572 -> 470 (-18 %) | 730 -> 781 (+7 %) |
| Blue (2) | 1,240 -> 1,251 | 1,465 -> 1,455 |

Per-aircraft draw falls under the strike (North days 7-10: 1,429 -> 801 units per aircraft per day), which means the cap binds the struck side harder and aircraft fly less, but the struck side's fleet is the larger one by day 7 (North 622 aircraft vs 493 in the control) so total flights barely move. A later strike (h24 to h96) hits a fleet that has already built up to its cap and costs the North 0-6 % of flights.

Refusals (AIRREFUSE; `m1-base` has none because it is uncapped): with the 3-day-net cap North launches refused per 6 h window go from 1,160 (control, days 1-3) to 1,494 (strike) and from 3,579 to 5,153 (days 3-7); 94-98 % of refusals find the pool empty. Blue's cap (382 net/aircraft/day) refuses far fewer (655 in days 3-7) and ~36 % of those are "cap below need".

Conclusion: with a realistic net-burn stockpile a strike on Red's refineries is visible in the air war (North -6 % to -15 % flights, 30-45 % more refused launches) but does not decide the campaign: Blue still wins 11-12 of 12 in 166-181 h on average.

## 27. Ground fuel burn at tank = 100 points, `movefuel=1` (tag p1-burn, 12 seeds, 10 days; FUELBURN / FUELTANK lines)

`movefuel=N` burns N points of the 100-point battalion tank per move event. Points burned per battalion that moved in the last 24 h (mean over seeds):

| Team | Moving / wheeled+tracked | Median | p90 | Max |
|---|---|---|---|---|
| Blue (2) days 1-10 | 57-77 of 83-98 | 14-20 | 26-33 | 38-45 |
| North (6) day 1 | 100 of 242 | 8 | 16 | 25 |
| North (6) day 5 | 44 of 150 | 9 | 20 | 31 |
| North (6) day 9 | 7 of 44 | 6 | 9 | 15 |

So at `movefuel=1` a tank lasts 100 / 30 = 3.3 days for a p90-pace Blue battalion and 5 days for a p90 North battalion: `movefuel=1` already is the "3 days of net burn" ground tank (3 days at the heaviest pace, more for most). Result without any strike: mean tank 97 % (day 1) -> 83 % (Blue, day 10) and 99 -> 96 % (North), 0 empty tanks until Blue day 9-10 (12 refused moves per 6 h at most). Blue win: 11/12 (seed 12 missed; hours 144-215 otherwise). Seed link: [p1-burn seed 4](http://localhost:8770/viewer.html?run=save0-p1-burn-s4).

Reading: the tank at this size is a buffer that refill keeps topped off; it only binds if the pool that refills it is starved, which is what the strike matrix in section 28 (tags q1-*) tests.

## 28. Ground strike matrix at the 3-day tank (12 seeds, 10 days, tags q1-*; exe16; `fueltank=1 movefuel=1` + the section 26 air caps; strikes held down from hour 0; 2026-11-08)

Control: [q1-ctl seed 4](http://localhost:8770/viewer.html?run=save0-q1-ctl-s4). Strike seeds: [refineries](http://localhost:8770/viewer.html?run=save0-q1-ref-h0-s4), [depots + army bases](http://localhost:8770/viewer.html?run=save0-q1-depot-h0-s4), [factories + ports](http://localhost:8770/viewer.html?run=save0-q1-facport-h0-s4), [bridges](http://localhost:8770/viewer.html?run=save0-q1-bridge-h0-s4), [all producers](http://localhost:8770/viewer.html?run=save0-q1-all-h0-s4), [Blue's refineries struck](http://localhost:8770/viewer.html?run=save0-q1-bluref-h0-s4).

| Run (struck from h0) | Blue wins | Mean win hour | Red supply % d1/d3/d5/d7 | Red bn < 25 % | Supply made per day Red / Blue |
|---|---|---|---|---|---|
| q1-ctl | 11/12 | 161 | 70 / 77 / 73 / 74 | 7 | 1691 / 2397 |
| Red refineries | 12/12 | 168 | 69 / 72 / 71 / 71 | 9 | 1674 / 2372 |
| Red depots + army bases | 12/12 | 174 | 70 / 77 / 75 / 73 | 6 | 1670 / 2353 |
| Red factories + ports | 12/12 | 150 | 60 / 54 / 50 / 50 | 55 | 397 / 2410 |
| Red bridges | 12/12 | 151 | 69 / 74 / 81 / 83 | 11 | 1638 / 2285 |
| Red all producers | 12/12 | 152 | 60 / 54 / 49 / 49 | 58 | 399 / 2419 |
| **Blue refineries** | **4/12** | **204** | 69 / 76 / 72 / 70 | 5 | 1686 / 2329 |

Ground tank, mean over seeds (moves refused per 6 h summed over the window, mean tank %, most empty tanks in a 6 h sample):

| Run | Blue days 3-7 | Blue days 7-10 | North days 7-10 |
|---|---|---|---|
| q1-ctl | 0 refused, 88 %, 0 empty | 285, 82 %, 0 | 178, 94 % |
| Red refineries | 23, 88 %, 0 | 110, 82 %, 0 | 168, 95 % |
| Red bridges | 0, 90 %, 0 | 100, 86 %, 0 (burn 3,594 -> 908: Blue is stuck too) | 0, 99 % (burn 339 -> 87: the North cannot move) |
| Red all producers | 0, 89 %, 0 | 6, 84 %, 0 | 112, 92 % |
| **Blue refineries** | **11,737, 59 %, 17** | **47,348, 37 %, 35** | 1,685, 84 %, 2 |

Air, same runs (flights per day, days 3-7 / days 7-10): Blue control 1,242 / 1,461; Blue refineries struck 855 / 799 (-31 % / -45 %); North with all Red producers struck 1,209 / 1,093 against 1,556 / 1,721 (-22 % / -36 %).

What this says:
- A strike on Red's refineries, depots or army bases does nothing visible to the ground war at the 3-day tank: no empty tank, no refused move to speak of, Blue wins 12/12.
- Only factories + ports (supply production 1,691 -> 397 a day, 55 battalions under 25 %) and bridges (the North stops moving, burn -70 to -75 %) change the Red picture; both make Blue win about 10 h sooner, which is the wrong direction for a longer war.
- Blue is the side whose fuel shows up. Blue's refineries make only about 120 pool points a day when struck, so Blue's tanks fall to 59 % (days 3-7) and 37 % (days 7-10), up to 35 tanks sit empty at once, tens of thousands of moves are refused, Blue air falls 31-45 %, and Blue wins only 4 of 12 (mean 204 h). That is the pass criterion we wanted (a strike visibly changes the ground picture and the outcome, and no side is zeroed: the North's battalion supply and supply production are untouched, Blue still has its battalions and air force).
- Why the asymmetry: Blue keeps 57-77 of its 83-98 wheeled/tracked battalions moving for all 10 days, while the North has 44 of 150 still moving on day 5 and 7 of 44 on day 9 (section 27) because it is being destroyed. Fuel only matters to the side that still has to move.

Open (not done): front progress per day, units with 0 fuel but full ammo, rationed refill knob, strike at h24/h48/h96 on Blue's refineries, and whether Blue wins 4/12 because of the ground tank or the air cut (separate `fueltank=0` run with the same Blue strike).

## 29. How the North keeps flying with its refineries dead, and how a starved ground unit still fights (tags r1-*, r2-*, q1-*; exe16; 12 seeds, 10 days; 2026-11-08)

> **Correction (section 31, 2026-11-08):** the r2 row (`noairlift=1`) removes airlift's *replacements and supplies* as well as its fuel. Its North aircraft count is already 182 against 391 on days 1-3, before any fuel shortage, so most of its -68 % flights is aircraft not replaced, not fuel. Taking only the fuel out of airlift (section 31) cuts the North's fuel burned by 60-70 % but leaves its flights at 1,340-1,460 a day. Read the 'airlift carries the fuel' lines below with that in mind.

**Air.** All runs: `airdays=3 airtopup=1 airrates=2:382,4:1528,5:909,6:668` (section 26), all Red refineries held down from hour 0 unless marked control. North = team 6, China = team 5. "Refill" = fuel units a day that squadrons took from the pool; flights are per day.

| Run | North refill d1 / d1-3 / d3-7 / d7-10 | North flights d1-3 / d3-7 / d7-10 | North aircraft d1-3 / d3-7 | China flights d3-7 |
|---|---|---|---|---|
| r1-ctl (no strike, `offfuel=0`) | 175k / 236k / 171k / 85k | 1,434 / 1,676 / 1,856 | 364 / 472 | 601 |
| n1-red-h0 (strike, offensive top-up ON, airlift ON) | 130k / 132k / 106k / 27k | 1,364 / 1,496 / 1,695 | 391 / 535 | 470 |
| r1-red-h0 (strike, `offfuel=0`) | 126k / 116k / 57k / 13k | 1,328 / 1,420 / 1,742 | 392 / 567 | 578 |
| **r2-red-h0 (strike, `offfuel=0`, `noairlift=1`)** | **50k / 15k / 15k / 12k** | **454 / 538 / 806** | 182 / 274 | 289 |

Seeds: [r1-ctl s4](http://localhost:8770/viewer.html?run=save0-r1-ctl-s4), [r1-red-h0 s4](http://localhost:8770/viewer.html?run=save0-r1-red-h0-s4), [r2-red-h0 s4](http://localhost:8770/viewer.html?run=save0-r2-red-h0-s4).

With the refineries dead the North's own production is 120 points (12k units) a day, yet it still refilled 126k on day 1 and 116k on days 1-3. Switching off the offensive fuel floor (`offfuel=0`) halves the refill only from day 3 (106k -> 57k). Switching airlift off as well (`noairlift=1`) takes the refill to 15k a day and the North's flights fall 68 % (days 1-3), 68 % (days 3-7) and 57 % (days 7-10) against the control; China's fall 52 % and its aircraft count falls from 204 to 55 by days 3-7. So **in the stock game the North's fuel after its refineries die is airlift (plus the offensive floor on days 3+)**, not the refineries. The ground picture changes too: Red battalion supply falls to 67 / 68 / 60 / 62 % (d1/d3/d5/d7) with 20 battalions under 25 %, against 75 / 82 / 78 / 76 and 3 in the control. Blue wins 12/12 at hour 174 (not a useful measure; section 28).

Lever for the player: airlift landings are what feeds a struck North, which means airbases, runways and transports are strikable counters to a refinery campaign.

**Ground.** A starved battalion is weak, not stopped (section 5): firepower and strength scale with supply %, it needs more than 30 % to fight at all, takes no capture order below 50 %, never loses morale (stock), and still moves at full speed at 0 % (burn stops at 2 %). In the runs the North's ground force is destroyed rather than starved: battalions 320 (days 1-2) -> 248 -> 175 -> 108 -> 59 (days 9-10); capture orders held 41 -> 22 -> 4 -> 1 -> 0 in the control. With factories + ports struck ([q1-facport-h0 s4](http://localhost:8770/viewer.html?run=save0-q1-facport-h0-s4), control [q1-ctl s4](http://localhost:8770/viewer.html?run=save0-q1-ctl-s4)) 81-90 of ~320 battalions sit under 50 % supply, capture orders fall on the same curve (35 -> 16 -> 3 -> 0), and none of the units holding capture orders is under 50 %: starved units are simply left in reserve. Supply use is 85-95 % movement and under 1 % ground combat, so a defending army burns almost nothing.

Ground-supply knobs (campsim; several are cfg keys, section 6): consumption `usemult` `useteam` `sxattackuse` `neutralnouse`; start level `startsup` `initsupply`; production `prodsup` `noprod` `prodcut` `blow` `blowsel` `blowat`; delivery `maxratio` `gndsupoff` `gndsupdef` `sxconvoys` `sxhops` `sxrear` `sxloss` `sxdelay` `sxrail`; local stores `stocks` `storestart` `storecap` `storefill` `storesproduce`; consequences `starvemorale` `supplyfall` `strikeshock`; fuel tank `fueltank` `movefuel`; airlift and replacements `noairlift` `airliftneed` `replfix` `relsquad`.

## 30. Striking the North's airbases (tags s1-*, s2-*; exe16; 12 seeds, 10 days; 2026-11-08)

All runs use the section 26 air caps (`airdays=3 airtopup=1 airrates=2:382,4:1528,5:909,6:668`) and `blowat=H` (hour of the strike). `blowsel=airbase:6:SKIP:N:HOLD` strikes the North's own airbases only (team 6; China's stay intact): N = 0 means all 25, N = 8 the eight nearest the enemy; HOLD 1 keeps them destroyed all 10 days, HOLD 0 strikes once and lets the campaign repair them. Control r1-ctl (no strike). "Holds three towns" = the first hour Blue owns objectives 260 / 680 / 404 together (gate-free, replayed from the `o` deltas; `scratchpad/holds3.py`, uses the record's `t` in minutes, not the `hour` field, which is hour-of-day).

| Run | Strike | Blue holds three towns, mean (median) | Seeds | North airbase status d3 / d5 | North aircraft lost d3 / d5 | North airlift landings by h72 (seed 4) |
|---|---|---|---|---|---|---|
| r1-ctl | none | 186 (183) | 11/12 | 95 / 86 % | 2,642 / 4,254 | 351 |
| **s1-ab6-h0** | all 25, held, h0 | **108 (109)** | 12/12 | 32 / 28 % | 320 / 366 | **3** |
| s1-ab6ref-h0 | all 25 + Red refineries, held, h0 | 109 (109) | 12/12 | 33 / 29 % | 303 / 337 | - |
| s1-ab6ref-h48 | all 25 + refineries, held, h48 | 154 (152) | 12/12 | 40 / 28 % | 1,963 / 2,034 | - |
| s1-ab6n8-h0 | 8 nearest, held, h0 | 165 (159) | 12/12 | 80 / 74 % | 2,010 / 3,240 | - |
| s1-ab6n8ref-h0 | 8 nearest + refineries, held, h0 | 145 (142) | 12/12 | 81 / 70 % | 1,848 / 2,669 | - |
| s2-ab6once-h0 | all 25, struck once, repairable | 152 (144) | 12/12 | 74 / 63 % | 2,178 / 3,597 | 312 |
| s2-ab6n8once-h0 | 8 nearest, struck once, repairable | 163 (161) | 12/12 | 92 / 79 % | 2,497 / 3,975 | 365 |

(The air-loss and airbase-status columns are mean of the 12 seeds from `lo` / `ad`; the "-" cells in the last column were not looked up (seed 4 airlift lines exist only for the runs shown).) Seed 4 runs: [r1-ctl](http://localhost:8770/viewer.html?run=save0-r1-ctl-s4), [s1-ab6-h0](http://localhost:8770/viewer.html?run=save0-s1-ab6-h0-s4), [s1-ab6n8-h0](http://localhost:8770/viewer.html?run=save0-s1-ab6n8-h0-s4), [s1-ab6ref-h0](http://localhost:8770/viewer.html?run=save0-s1-ab6ref-h0-s4), [s2-ab6once-h0](http://localhost:8770/viewer.html?run=save0-s2-ab6once-h0-s4), [s2-ab6n8once-h0](http://localhost:8770/viewer.html?run=save0-s2-ab6n8once-h0-s4).

What it shows:
- Keeping all North airbases down is the strongest strike found so far: the towns fall ~78 h sooner (every seed 88-127 h against 138-240 for the control) and the one control seed that never got there does. Adding the refineries changes nothing (109 vs 108).
- It works through the air war, not through fuel. With the airbases held down the North loses 320 aircraft in 3 days instead of 2,642 (it is not meeting Blue in the air), Blue loses fewer ground vehicles (2,110 vs 2,775 by h72) and the North loses more to Blue air (3,548 vs 2,965): Blue's strike aircraft work almost unopposed. Airlift landings fall 351 -> 3, so the airlift of section 29 is cut as a side effect.
- North flight counts do not fall (they rise: about 2,450 flights on day 1 against 1,640, 2,910 against 1,680 on days 3-7), and "airbase full" refusals go from ~1,400 to ~11,000 by h240. I did not establish what these flights are (probably launches that cannot use the damaged fields); the loss numbers above are the better measure.
- Half measures give half the effect: eight bases held = -21 h, eight held plus refineries = -41 h, all 25 held from h48 = -32 h. A single strike with repair is worth about -34 h (all 25) / -23 h (eight): the campaign repairs the fields only to 63-74 % status by days 3-5, and airlift still lands (312 vs 351 landings), but the damage lasts long enough to cost the North its air arm early.
- Caveat: HOLD 1 is a ceiling, not a realistic repair rate. Open: a repeated strike (re-strike every N hours), a strike that also takes China's forward fields, and a player-side limit (how many sorties it costs Blue to keep 25 fields down).

## 31. Airlift fuel: none, or only from an ally's pool (tags u1-*; exe17; 12 seeds, 10 days; 2026-11-08)

New campsim switch `airliftfuel=TEAM:MODE[,TEAM:MODE...]` (update.cpp `gAirliftFuelMode`, scratch build only): per landing team, 1 = stock (fuel credited from nothing), 0 = a landing adds no fuel (supply and replacements unchanged), 2 = the fuel is taken from the pools of the team's allies (Blue 1-3, Red 4-6), richest first, and only as much as they hold, so a team whose allies are empty gets nothing. Runs: the section 26 air caps, every Red refinery held down from hour 0, `airliftfuel=5:...,6:...` (China and the North); Blue's airlift stays stock. Comparison runs from earlier sections: r1-ctl (no strike, `offfuel=0`), n1-red-h0 (strike, nothing switched off), r1-red-h0 (strike, `offfuel=0`), r2-red-h0 (strike, `offfuel=0 noairlift=1`).

North (team 6). "Fuel burned" = net fuel units a day (drawn at launch minus returned on landing); windows are days 1-3 / 3-7 / 7-10.

| Run | Fuel burned (k units/day) | Flights/day d3-7 | Aircraft d3-7 | North airlift fuel by h72 (seed 4) | Blue holds the three towns, mean hour |
|---|---|---|---|---|---|
| r1-ctl (no strike) | 139 / 123 / 52 | 1,676 | 472 | - | 180 (n1-ctl) |
| n1-red-h0 | 106 / 92 / 22 | 1,496 | 535 | 3,546 | 182 |
| r1-red-h0 (`offfuel=0`) | 100 / 59 / 9 | 1,420 | 567 | - | 163 |
| **u1-red-nofuel** (`airliftfuel=5:0,6:0`) | 38 / 42 / 26 | 1,462 | 710 | **0** | 177 (10/12) |
| **u1-red-ally** (`5:2,6:2`) | 57 / 47 / 18 | 1,430 | 646 | 828 | 172 |
| **u1-red-ally-nofloor** (`5:2,6:2 offfuel=0`) | 51 / 37 / 17 | 1,342 | 691 | 798 | 181 |
| r2-red-h0 (`offfuel=0 noairlift=1`) | 23 / 20 / 10 | 538 | 274 | 0 (all airlift off) | 176 |

Seed 4: [u1-red-nofuel](http://localhost:8770/viewer.html?run=save0-u1-red-nofuel-s4), [u1-red-ally](http://localhost:8770/viewer.html?run=save0-u1-red-ally-s4), [u1-red-ally-nofloor](http://localhost:8770/viewer.html?run=save0-u1-red-ally-nofloor-s4), against [n1-red-h0](http://localhost:8770/viewer.html?run=save0-n1-red-h0-s4) and [r2-red-h0](http://localhost:8770/viewer.html?run=save0-r2-red-h0-s4).

- The fuel switches do what they should to the fuel: with the airlift fuel gone, or taken from allies only, the North burns 62-70 % less fuel than the control on days 3-7 (42 / 47 / 37k against 123k), and the ally mode cuts the North's airlift fuel from 3,546 to 828 by hour 72. Taking the offensive top-up away as well saves a little more (37k).
- Flights do not follow the fuel. The same runs fly 1,340-1,460 flights a day on days 3-7 (control 1,676), and the North's aircraft count is higher than in the strike run with everything on (646-710 against 535). Flights are not a good measure of air activity when fuel is short; fuel burned is. What a flight does with less fuel (shorter sortie, partial load) I did not establish.
- The r2 row (flights -68 %) is mostly not a fuel effect: `noairlift=1` also removes airlift's aircraft replacements, and the North already has 182 aircraft against 391 on days 1-3. This corrects the conclusion in section 29.
- Squadron stocks never run out. North stock per aircraft (four-ship sorties) goes from 53 at hour 24 to 35 at hour 240 in the control, 51 to 32 with the strike, 55 to 34 with no airlift fuel, 44 to 29 in r2. Under the cap the squadrons hold far more than the burn they actually do.
- Blue's win hour does not move: 163-182 for every variant against 180 for the control, inside the 30-hour spread between seeds. u1-red-nofuel misses two seeds (10/12) and r1-red-h0 is quickest (163); neither is a clear effect.


## 32. What limits North flights: airlift aircraft replacements (runs v1-*, 12 seeds, 10 days)

New scratch switch `airliftrepl=TEAM:MODE` (0 = an airlift landing adds no aircraft replacements; supplies and fuel unchanged), exe18. Same caps as before (`airdays=3 airtopup=1`, `airrates`), North = team 6, China airlift (5) also cut.

| run | what is cut | North aircraft d1-3 / d3-7 | flights/day d1-3 / d3-7 | net fuel burn (pool pts/day) d1-3 / d3-7 | Blue holds 3 towns (mean h) |
|---|---|---|---|---|---|
| n1-ctl | nothing | 318 / 355 | 1,668 / 1,398 | 1,833 / 1,321 | 180 |
| n1-red-h0 | refineries held down | 315 / 391 | 1,613 / 1,364 | 1,805 / 1,057 | 182 |
| u1-red-nofuel | refineries + airlift fuel | 358 / 592 | 1,545 / 1,148 | 1,555 / 380 | 177 (10/12) |
| v1-ctl-norepl | airlift replacements only | 206 / 154 | 1,431 / 588 | 1,544 / 514 | 127 |
| v1-red-norepl | refineries + replacements | 201 / 161 | 1,390 / 587 | 1,475 / 448 | 141 |
| v1-red-norepl-nofuel | refineries + replacements + airlift fuel | 205 / 182 | 1,359 / 515 | 1,437 / 298 | 126 |
| r2-red-h0 | refineries + all of airlift (noairlift=1) | 205 / 182 | 1,348 / 454 | 1,442 / 234 | 176 |

Reading:
- **Aircraft count is what limits North flights.** Cutting only the replacements, with no strike at all, takes North flights from 1,398 to 588 a day on days 3-7 (-58%). That is almost all of r2's drop (454). Cutting airlift fuel alone (u1) only moves flights 1,364 to 1,148 (-16%) and leaves more aircraft alive.
- Fuel is the second lever: adding "no airlift fuel" on top of no replacements takes d3-7 flights 587 to 515 and net burn 448 to 298.
- The refinery strike does not move the Blue hold once replacements are cut (141 vs 127 hours; seed spread is ~20 h, so call it no effect), and it does not lower North flights. It does raise the share of sorties refused for fuel (see section 33: flights are set by the ATM tempo quota, not by fuel).
- **Unexplained:** r2 (all of airlift cut, supplies too) does NOT bring the Blue hold forward (176 h), while replacements alone does (127 h). Same aircraft numbers in both. So the airlift supplies being removed seems to help the North ground side, or hurt Blue's advance; not yet investigated.

Seed links (seed 4 typical): http://localhost:8770/viewer.html?run=save0-v1-ctl-norepl-s4, http://localhost:8770/viewer.html?run=save0-v1-red-norepl-s4, http://localhost:8770/viewer.html?run=save0-v1-red-norepl-nofuel-s4


## 33. Clean test: no refinery output, no airlift fuel, no offensive floor; and what Red burns when fuel is no issue (tags w1-*, exe18, 12 seeds, 10 days, 2026-11-08)

Two runs, both with the section 26 caps (`airtopup=1`, `airrates=2:382,4:1528,5:909,6:668`):
- `w1-red-clean`: `airdays=3 blowsel=refinery:red:0:0:1 airliftfuel=5:0,6:0 offfuel=0` (refineries held down from hour 0, airlift brings no fuel, the offensive fuel floor is off).
- `w1-ctl-unlim`: `airdays=100`, no strike. Squadron stock never runs short, so nothing is refused for fuel; this is what Red consumes when fuel is no issue.

### 33a. Consumption (net burn = drawn at launch - returned on landing; pool points = 100 units; mean over 12 seeds)

| | unlimited fuel | capped control (n1-ctl) | clean test |
|---|---|---|---|
| Red (teams 4+5+6) days 1-2 | 1,706 | 1,581 | 1,070 |
| Red days 3-7 | 1,818 | 2,070 | 611 |
| Red days 8-10 | 587 | 874 | 264 |
| **Red mean per day, days 1-10** | **1,426** | 1,614 | **599 (42%)** |
| DPRK (team 6) days 1-2 / 3-7 / 8-10 | 1,513 / 1,142 / 357 | 1,452 / 1,251 / 505 | 925 / 260 / 143 |
| China (team 5) days 1-2 / 3-7 / 8-10 | 152 / 673 / 230 | 86 / 814 / 369 | 103 / 345 / 121 |

* Without a fuel limit Red wants about 1,700 points a day for the first week, then about 590 as the fighting slows. The 3-day cap alone does not change that (1,614 vs 1,426 a day is within the seed spread).
* The clean test cuts Red's burn by 58 % over the war and the DPRK's days 3-7 burn to 23 % of what it wants.

### 33b. Fuel does not run out; the flow into empty squadrons does

DPRK squadron stock in the clean test: 3,475 pool points at h72, 3,036 at h168, 2,373 at h240 (control 3,476 / 3,000 / 3,010). Squadrons under 4 sorties: 33 of 54 at h72, 39 of 57 at h168 (control 24 of 54, 30 of 58). Refused sorties per day at h72: 1,676 against 1,169 flown (control 584 against 1,548). The force is not out of fuel; the squadrons that are empty cannot be refilled from a pool that is at zero, and the rest keep flying.

### 33c. The "120 a day" is not refinery output

`supply.cpp:955` adds `AUTOMATIC_SUPPLY` (5 points) to each team's fuel every supply cycle, which is 120 a day at one cycle an hour. A struck refinery makes nothing; the 120 left in every strike run is this constant. In the clean test the DPRK's income is that 120 against a burn of 260 (days 3-7), which matches the stock sliding about 110 a day.

### 33d. Outcome

| | unlimited | capped control | clean test |
|---|---|---|---|
| Blue vehicles lost to aircraft by h168 | 889 | 980 | **218 (-75 %)** |
| Blue holds the three towns, mean hour (12/12 seeds each) | 163 | 180 | **166** |
| North flights/day, days 1-10 | 1,884-2,768 | 1,755-3,098 | 1,582-3,289 |

* Cutting every free fuel source takes 75 % off what the North air force does to Blue's ground units, and it still flies as many sorties.
* Blue's win hour does not move (166 vs 163 unlimited, 180 control; seed spread 30 h).

### 33e. Code behind the flat flight count (read, not yet measured)

* `FindBestAir` (atm.cpp:1643) ranks squadrons on rating, specialty, range, aircraft available and airbase schedule. It never reads fuel.
* Our scratch refusal (flight.cpp:3424-3464) returns `PRET_CANCELED`; `package.cpp:1068` cancels the package and the `default:` case at `atm.cpp:686` deletes the request. Nothing re-tasks it to another squadron. Stock FreeFalcon has no such refusal at all.
* The ATM walks its request list until `missionsFilled >= missionsToFill` (atm.cpp:596). A refusal is not a fill, so it moves to the next request. `missionsToFill = pending requests x actionTempo / 100` (team.cpp:1685); the "flies always" types (CAP and similar) skip the quota. So the flight count is set by tempo, not by fuel; scarcity changes which requests are filled.
* Burn is per minute flown (flight.cpp:1111); a dead flight returns nothing (flight.cpp:5142).
* Not measured: mission type of launches and refusals. The idea that the surviving flights skew toward non-damaging types fits the -75 % damage with flat flights, but is an inference until mission type is logged.

Seed 4: http://localhost:8770/viewer.html?run=save0-w1-red-clean-s4, http://localhost:8770/viewer.html?run=save0-w1-ctl-unlim-s4


## 34. Limiting airlift fuel to a share (`airliftfuelpct`, tags x1-*, exe19, 12 seeds, 10 days, 2026-11-08)

New scratch switch `airliftfuelpct=TEAM:PCT`: a landing keeps PCT percent of its fuel credit (after the runway and need caps; the fraction is carried to the next landing). All runs: refineries held down from hour 0, `offfuel=0`, `airdays=3 airtopup=1 airrates=...`, applied to teams 5 and 6. x1-pct100 is the stock airlift with the offensive floor off, the baseline; w1-red-clean (section 33) is 0 %.

| run | airlift fuel d3-7 (pts/day, DPRK) | Red net burn d1-2 / d3-7 / d8-10 | flights/day d3-7 | sorties refused for fuel d3-7 | Blue vehicles lost to aircraft by h168 | Blue holds 3 towns (mean h) |
|---|---|---|---|---|---|---|
| pct100 | 610 | 1,424 / 1,232 / 317 | 1,408 | 50 % | 796 | 187 |
| pct40 | 231 | 1,275 / 877 / 282 | 1,354 | 56 % | 536 | 177 |
| pct20 | 75 | 1,130 / 763 / 203 | 1,369 | 58 % | 384 | 160 |
| pct10 | 44 | 1,235 / 668 / 239 | 1,365 | 59 % | 380 | 155 (11/12) |
| 0 (w1-red-clean) | 0 | 1,070 / 611 / 264 | 1,363 | 58 % | 218 | 166 |

* The damage curve is steepest from 100 % to 20 % (796 to 384 vehicles, -52 %); below 20 % it flattens (380 at 10 %) until fuel is removed altogether (218).
* The delivered airlift fuel is below the nominal share (20 % delivered 12 % of the baseline), because the need cap and landing counts also fall.
* Flights stay at 1,350-1,410 a day in every run (the ATM tempo quota, section 33e). The win hour is 155-187 with a 30 h seed spread; 10-20 % look 25 h earlier than the baseline but that is inside the spread.
* With the offensive floor on (n1-red-h0) the same stock airlift gave 981 vehicles lost; the floor alone is worth about 185.

Seed 4: http://localhost:8770/viewer.html?run=save0-x1-pct20-s4, http://localhost:8770/viewer.html?run=save0-x1-pct100-s4


## 35. Fuel-aware air tasking (`airaware=1`, tags y1-*, exe20, 12 seeds, 10 days, 2026-11-08)

New scratch switch `airaware=1`: `FindBestAir` (atm.cpp) passes over a squadron whose fuel stock cannot cover two aircraft and that the national pool could not top up (the same test the flight builder applies at the end), so the request goes to the next squadron instead of being refused with the starved one. All runs: refineries held down from hour 0, `offfuel=0`, `airdays=3 airtopup=1 airrates=...`, `airliftfuelpct` on teams 5 and 6. Compared with the same airlift share without `airaware` (x1-*, w1-red-clean) and with the unlimited-fuel control.

| run | Red net burn d1-2 / d3-7 / d8-10 | DPRK flights/day d1-3 / d3-7 / d7-10 | sorties refused d3-7 | Blue vehicles lost to aircraft by h168 | Blue aircraft lost to aircraft | Blue holds 3 towns (mean h) |
|---|---|---|---|---|---|---|
| unlimited fuel (w1-ctl-unlim) | 1,706 / 1,818 / 587 | 1,551 / 1,497 / 1,974 | 39 % | 889 | 257 | 163 |
| airlift 100 %, plain tasking (x1-pct100) | 1,424 / 1,232 / 317 | 1,420 / 1,408 / 1,588 | 50 % | 796 | 174 | 187 |
| airlift 100 %, aware (y1-aware100) | 1,818 / 1,412 / 304 | 1,444 / 1,568 / 2,235 | 28 % | 934 | 184 | 167 |
| airlift 20 %, plain (x1-pct20) | 1,130 / 763 / 203 | 1,304 / 1,369 / 1,965 | 58 % | 384 | 121 | 160 |
| airlift 20 %, aware (y1-aware20) | 1,254 / 920 / 279 | 1,108 / 1,123 / 2,122 | 53 % | 593 | 124 | 169 |
| airlift 0, plain (w1-red-clean) | 1,070 / 611 / 264 | 1,258 / 1,363 / 1,979 | 58 % | 218 | 95 | 166 |
| airlift 0, aware (y1-aware0) | 1,093 / 695 / 251 | 965 / 924 / 2,074 | 58 % | 322 | 83 | 172 |

* **Flights do respond to fuel once tasking is aware**: at 20 % and 0 % airlift, days 3-7 flights fall to 1,123 and 924 (plain tasking: 1,369 and 1,363). With stock airlift they rise (1,568).
* **The damage goes up, not down**, at every airlift level (593 vs 384 at 20 %; 322 vs 218 at 0): aware tasking stops spending requests on starved squadrons, so the fuel that is left goes to sorties that fly. Fuel matters less to the damage, more to the sortie count.
* **The Blue win hour does not move in any run**: 160-187 with a 30 h seed spread, and the unlimited-fuel control (163) is inside it. North air damage is not what holds Blue back in this model, so no fuel setting shortens the war. What did move it earlier: cutting airlift aircraft replacements (127 h, section 32) and holding the North's airbases down (108 h, section 30).

Seed 4: http://localhost:8770/viewer.html?run=save0-y1-aware20-s4, http://localhost:8770/viewer.html?run=save0-y1-aware0-s4


## 36. The air fuel model in the source tree (2026-11-09, tags z1-*, repo campsim build, 12 seeds, 10 days)

The scratch model (sections 15-35) is now in the repo working tree, off by default, behind cfg keys: `AirFuelDays`, `AirFuelRates`, `AirFuelAware`, `AirliftFuelPct`, `OffFuelCheat` (see WIP-NOTES.md "Air fuel model"). campsim run knobs `airdays airrates airaware airliftfuelpct offfuel` set the same variables. Check: the repo's own campsim, refineries held down from hour 0, versus the scratch result of the same settings (x1-pct20) and the same war with the features off.

| run | Red net burn d3-7 | airlift fuel d3-7 (DPRK) | flights/day d3-7 | refused for fuel | Blue vehicles lost to aircraft by h168 | Blue holds 3 towns (mean h) |
|---|---|---|---|---|---|---|
| z1-fuel20 (repo build: days 3, rates, airlift 20 %, cheat off) | 660 | 73 | 1,333 | 59.5 % | 381 | 172 (12/12) |
| x1-pct20 (scratch build, same settings) | 763 | 75 | 1,369 | 58.1 % | 384 | 160 (12/12) |
| z1-stock (repo build, features off) | n/a (stock burn is in different units) | 1,030 | 1,865 | 0 % | 1,499 | 201 (9/12) |

* The port reproduces the scratch run: airlift fuel 73 vs 75, flights 1,333 vs 1,369, Blue vehicle losses 381 vs 384; the win hour (172 vs 160) is inside the seed spread. The repo working tree also carries other uncommitted ground-war work, so exact equality was not expected.
* Against the stock game (features off) the North's aircraft destroy a quarter of what they do at stock: 381 vs 1,499 Blue vehicles by h168, and refinery strikes now matter (stock: refused 0 %).
* Not done: the game exe (`FFViper-campaign.exe`) has not been rebuilt or deployed, nothing is committed, and the Korea China War mod lines (make_china_war_mod.py) are not in the installed mod yet. All runs in sections 29-36 used the installed mod stack (Korea Rail War + Korea China War: save0/.tri/.AII byte-identical to the China War mod's, both `config/mods` cfgs read by `usecfg=1`), so China's entry, the h_bar settings and the 70% start were in force; whether the rail troop trains ran was not checked.

Seed 4: http://localhost:8770/viewer.html?run=save0-z1-fuel20-s4, http://localhost:8770/viewer.html?run=save0-z1-stock-s4


## 37. Default Korea vs the mod stack, three configurations each (tags e1-*, exe20, 12 seeds, 10 days, 2026-11-09)

Two game folders: the **default** (`ff-default-game`: JSGME mods undone from `MODS\!BACKUP`, so the original save0/.tri/.AII, the original object database, no `rail.txt`, no `config\mods`, rail lines in FFViper.cfg commented out) and the **mod stack** (`C:\FreeFalcon6`: Korea Rail War + Korea China War). Three configurations on each: **as-pushed** (HEAD behaviour: no air fuel model, offensive fuel cheat on, stock airlift), **new** (`airdays=3` + the section 26 rates, airlift fuel 20 % for teams 5 and 6, `offfuel=0`) and **new + refineries** (the same, Red refineries held down from hour 0).

| run | Red net burn d1-2 / d3-7 / d8-10 | North flights/day d3-7 | sorties refused for fuel | Blue vehicles lost to aircraft by h168 | Red aircraft lost by h168 | Blue holds 3 towns (mean h) |
|---|---|---|---|---|---|---|
| default, as-pushed | 2,755 / 1,008 / 350 | 3,823 | 0 % | 218 | 3,992 | 67 |
| default, new | 1,368 / 473 / 112 | 2,638 | 40 % | 100 | 2,467 | 67 |
| default, new + refineries | 1,156 / 171 / 128 | 2,713 | 61 % | 71 | 1,767 | 67 |
| mod stack, as-pushed | 1,946 / 3,715 / 1,803 | 2,079 | 0 % | 1,402 | 8,883 | 175 |
| mod stack, new | 1,391 / 1,688 / 637 | 1,558 | 45 % | 816 | 5,754 | 166 |
| mod stack, new + refineries | 1,250 / 712 / 228 | 1,372 | 57 % | 434 | 3,715 | 169 |

* In both environments the refinery strike now costs the North: Blue's vehicle losses to aircraft fall 816 -> 434 (mod stack) and 100 -> 71 (default), where as-pushed a strike did nothing (sections 22-25).
* The default war is over in 67 hours whatever the fuel setting; the mod stack's war lasts 166-175 hours with the win hour inside the seed spread for all three configurations. The North's air force does 6 times the damage in the mod stack (1,402 vs 218 vehicles) simply because the war lasts longer.
* "Red aircraft lost" counts aircraft lost to aircraft and to ground fire, all Red teams.

Seed 4 (the typical one): default [as-pushed](http://localhost:8770/viewer.html?run=save0-e1-def-asis-s4), [new](http://localhost:8770/viewer.html?run=save0-e1-def-new-s4), [new + refineries](http://localhost:8770/viewer.html?run=save0-e1-def-newred-s4); mod stack [as-pushed](http://localhost:8770/viewer.html?run=save0-e1-mod-asis-s4), [new](http://localhost:8770/viewer.html?run=save0-e1-mod-new-s4), [new + refineries](http://localhost:8770/viewer.html?run=save0-e1-mod-newred-s4).



## 38. The air fuel model in the other theaters: Israel 2012 and Taiwan, with and without a refinery cut (2026-11-10, tags f1-/f2-*, exe21, 6 seeds, 10 days)

**Setup.** Default game folder (`ff-default-game`, no Korea mods), campsim exe21 (the rail-tree build with the Taiwan `FindBestAirFlight` guard), `usecfg=1 noend=1 replfix=1 airliftneed=1 airliftrunway=1`.
Four configurations per theater, 6 seeds each:
`as-pushed` (stock air fuel: `f1-*`), `new` (`airdays=3 airtopup=1 airrates=... airliftfuelpct=5:20,6:20 offfuel=0`), `newred` (new + every Red refinery held down from hour 0:
`blowat=0 blowsel=refinery:red:0:0:1`), `newblue` (new + the Blue side's refineries, `refinery:2:0:0:1`). Rates are the baseline net burn per aircraft per day (days 1-7):
Israel 2:930, 6:680; Taiwan 2:1020, 6:935. Airlift keeps 20 % of its fuel credit for teams 5 and 6 only, so the Blue side's airlift stays at 100 %, which softens the Blue cut.

### 38a. Israel 2012 (`save2`, Blue = Israel 2, Red = Egypt 6)

| | Israel burn | Egypt burn | Israel flights/day | Egypt flights/day | Israel vehicles lost to aircraft | Egypt vehicles lost to aircraft | Israel aircraft lost | Egypt aircraft lost |
|---|---|---|---|---|---|---|---|---|
| as-pushed | 731 | 957 | 889 | 937 | 423 | 1,034 | 1,459 | 1,975 |
| new | 712 | 942 | 930 | 902 | 391 | 1,005 | 1,528 | 1,906 |
| new + Egypt refineries cut | 625 | 234 | 908 | 308 | 192 | 916 | 1,196 | 1,008 |
| new + Israel refineries cut | 277 | 1,011 | 519 | 1,041 | 464 | 635 | 982 | 1,579 |

(burn = net pool points a day, days 3-7; flights per day days 3-7; losses to aircraft by hour 168.)

| Final ground / objectives | Israel | Egypt | End codes (6 = timeout or Israel holds, 7 = Arab win) |
|---|---|---|---|
| as-pushed | 2904 -> 832 / 500 -> 242 | 6541 -> 4461 / 1115 -> 1361 | 6 x 6 |
| new | 2904 -> 925 / 500 -> 279 | 6541 -> 4461 / 1115 -> 1324 | 5 x 6, 1 x 7 |
| new + Egypt cut | 2904 -> 810 / 500 -> 224 | 6541 -> 4503 / 1115 -> 1379 | 3 x 6, 3 x 7 |
| new + Israel cut | 2904 -> 899 / 500 -> 232 | 6541 -> 4562 / 1115 -> 1372 | 6 x 6 |

* The plain model changes almost nothing: Egypt makes about 16,000 pool points a day and uses about 950, so 0 % of sorties are refused. Israel makes 2,000-4,700 and uses about 700.
* Egypt cut: Egypt's flights fall 67 % (902 -> 308), 10 % of its sorties are refused, Israeli vehicles lost to aircraft fall 51 % (391 -> 192).
* Israel cut: Israeli flights fall 42 % (930 -> 519), 6 % of sorties refused; Israeli vehicles lost to aircraft do not fall (391 -> 464) and Egyptian ones fall 37 %.
* **The ground war is the same in all four**: Israel loses 2,000 of 2,900 ground strength and half its objectives in every configuration. The Arab-win counts (0, 1, 3, 0 of 6) are too small to read.

### 38b. Taiwan (`save2` Raging Dragon, Blue = Taiwan 2, Red = China 6)

| | Taiwan burn | China burn | Taiwan flights/day | China flights/day | Taiwan veh. lost to aircraft | China veh. lost to aircraft | Taiwan aircraft lost | China aircraft lost | End codes |
|---|---|---|---|---|---|---|---|---|---|
| as-pushed | 1,778 | 2,439 | 2,719 | 3,724 | 610 | 681 | 2,605 | 3,928 | 6 x 16 |
| new | 1,834 | 1,945 | 2,776 | 3,987 | 471 | 796 | 2,638 | 3,346 | 4 x 16, 2 x 17 |
| new + China cut | 1,845 | **539** | 2,753 | 3,514 | 363 | 611 | 2,446 | 1,980 | 3 x 16, 3 x 17 |
| new + Taiwan cut | **1,274** | 2,019 | 2,200 | 4,147 | 577 | 527 | 2,160 | 3,157 | 3 x 16, 3 x 17 |

| Final ground / objectives | Taiwan | China |
|---|---|---|
| as-pushed | 6298 -> 2941 / 435 -> 357 | 7244 -> 4816 / 792 -> 870 |
| new | 6298 -> 3058 / 435 -> 385 | 7244 -> 4754 / 792 -> 842 |
| new + China cut | 6298 -> 3299 / 435 -> 390 | 7244 -> 4548 / 792 -> 837 |
| new + Taiwan cut | 6298 -> 3098 / 435 -> 372 | 7244 -> 4661 / 792 -> 855 |

* China's burn falls 72 % with its refineries cut, but its flights fall only 12 % (3,987 -> 3,514) and 4 % of sorties are refused. Squadrons keep flying on part-fills; only the pool is empty.
* Taiwan's own cut takes its burn down 31 % and its flights 21 %, 4 % refused.
* Code 17 (a Taiwan-side win condition) appears 0, 2, 3, 3 times of 6; end codes are too few to read. Ground and objectives are within 4 % across the four configurations.

### 38c. Why a refinery cut cannot touch the ground war (read from the code and the `sp` counters, 2026-11-10)

A battalion has one number, `UnitSupply`. `BattalionClass::SupplyUnit` adds `fuel / 2` to it (wheeled and tracked only); nothing in movement, combat or morale reads a fuel level.
The team `fuelLevel` figure mixes in squadron fuel need and nothing reads it for decisions. With Egypt's refineries cut (Israel, 6 seeds): Egypt's fuel pool 0 from day 1,
fuel distribution ratio 3-10 of 500, cumulative fuel sent to units 524 against 16,549 uncut, yet battalion mean supply 96.0 % (95.8 % uncut) and 0 battalions under 25 %.
The supply pool does not depend on refineries and does not bind in ten days (section 5). Counters: `sp` rows in the jsonl (index 1 pool fuel, 4 fuelLevel, 6 fuel ratio x1000,
13/14 sent/got fuel, 30 battalion mean supply, 33 battalions under 25 %); script `gfuel.py` in the scratchpad.


## 39. A ground fuel tank that slows battalions (branch `ground-fuel`, exe23, Israel 2012 `save2`, 6 seeds, 10 days, 2026-11-11, tags g1-*, g2-*)

**Built.** `fueltank=1 movefuel=N fuelslow=AT:MIN fuelfight=PCT` (battalio.cpp, main.cpp): a tank per wheeled or tracked battalion (global array, not saved), N points burned per move event, refilled from the fuel pool;
under AT percent tank the unit's speed falls linearly to MIN percent at empty (no hard stop), and `fuelfight` cuts combat strength by up to PCT at empty. `FUELTANK` log lines give mean tank and battalions slowed.
Air knobs as the `f2-is-*` runs; Egypt refineries held down from hour 0 where marked.

| Config | Egypt tank d10 / slowed | Israel ground / objectives at end | Egypt ground / objectives at end | Arab wins (code 7) |
|---|---|---|---|---|
| no tank (f2-is-new) | - | 925 / 279 | 4461 / 1324 | 1/6 |
| tank, no cut (movefuel=2, slow 50:20) | 95 % / 1 of 267 | 833 / 230 | 4652 / 1373 | 0/6 |
| no tank, Egypt cut (f2-is-newred) | - | 810 / 224 | 4503 / 1379 | 3/6 |
| tank + Egypt cut | 60 % / 92 of 256 | 910 / 248 | 4563 / 1356 | 3/6 |
| tank + Egypt cut + fuelfight 50 | 62 % / 88 of 263 | 924 / 303 | 4608 / 1299 | 2/6 |
| hard tank (movefuel=6, slow 100:5, fuelfight 60), no cut | 91 % / 171 of 263 | 830 / 235 | 4410 / 1368 | 1/6 |
| **hard tank + Egypt cut** | **45 % / 202 of 251** | **1053 / 314** | 4449 / 1290 | **0/6** |

* The tank is real: with the pool empty, Egypt's battalions drain and slow (60 % mean tank, a third slowed at the mild setting; 45 %, four fifths slowed at the hard one).
* At the mild setting nothing about the war moves beyond seed noise (about +-100 ground, +-50 objectives).
* Even at the hard setting, with four fifths of Egypt's battalions crawling and strength cut, Egypt still ends with 4,449 ground and 1,290 objectives (from 1,115): it takes about 175 objectives. What moves is Israel's side: +30 % ground (810 -> 1053), +40 % objectives (224 -> 314), no Arab win in 6 seeds (3/6 without the tank). Six seeds; read as a direction.
* Reading: speed is a weak lever. Travel time is not what limits a 2.2 : 1 force in ten days. Next candidates: defenders burning ammo while engaged, a depot-stock chain, and orders that need fuel (GTM capture assignment).
