# Supply and China: the overnight spread (2026-10-16)

302 simulated 10-day runs of Korea Rolling Fire (the installed Korea Escalation `save0.cam`), all in `campsim`; the game executable was not touched.
Everything below is reproducible with `tools/campsim/supply_spread.py` (queue, `spread/results.jsonl` holds one line per run, hourly data is in
`tools/campsim/runs/<save>-sp-<config>-s<seed>.jsonl`, viewable in the viewer by that name). `spread_levers.py` is the regression. Goal from Andrew:
make supply matter, and get a fair, dynamic war that is close but Blue-sided by day 10, with China staged at the border and not at the border.

## The short version

1. **China at the border is better than China forward** (6 seeds, same supply settings): Blue holds 2-4 points more of the objectives, the North keeps
   more of China alive (46-52 battalions at day 10 against 18-30), and Blue's vehicles are 1.7-2.1x the North's instead of 2.2-3.4x. Forward-staged China
   arrives early, fights piecemeal inside the North and is ground down. At the border it comes in about two days after the trigger and survives.
2. **No supply lever moves who holds what.** Every moderate lever (use, production, start level, resupply cadence, replacement scale, airlift, bombing bridges)
   changes how much supply is *felt*, and none of them moves Blue's share of the objectives by more than the noise (about 4 points). Starving the North
   hard (front-line supply 7-40%, 60-220 battalions under 25%) does not hand Blue the war: the war freezes (Blue's share stays 48-52%, the North keeps its
   vehicles) because a battalion under 50% supply is only tasked as reserve and one under 30% cannot fire.
3. **The dial that sets the balance is the China counteroffensive**, `AlliedCounterattack` with `AlliedCounterMin` (allied battalions at the front that
   launch it). At the border: none 82% Blue, 24 -> 71%, 16 -> 67%, 12 -> 63%, 10 -> 56%, 9 -> 55%, **8 -> 50%** (12 seeds, 42-58% by seed).
4. **Chosen: min 10 with Blue unable to win before day 11 (`chn_appr4`): Blue 54%, ahead in 8 of 12, no game ends early (see "Final choice").** The even alternative, **fair, close, Blue-ahead in force, supply felt, no Blue win in 10 days (11 of 12 seeds):** China at the border + `AlliedCounterMin 8` +
   depots and army bases not producing + production x0.75 + battalion supply use x2 (`noprod=1 prodsup=0.75 usemult=2`). Blue ends with 50% of the objectives
   (42-58 by seed), 1.7x the North's vehicles, the North's front-line supply bottoms at about 82% (mean) and ends at 94%. Min 9 or 10 makes Blue 55-56% but ends
   the war on day 3.5-6 in 40-60% of seeds (Blue takes Pyongyang and Wonsan).

## The setting I chose: close, Blue-sided, runs the ten days (`AlliedCounterMin 10` + Blue cannot win before day 9)

Target: a war that is close but leans to Blue by day 10 and is still a war on day 10. The counteroffensive dial alone cannot do both (min 8 is even and rarely ends, 50%; min 9-10 is Blue-ahead,
55-57%, but ends the war on day 3-6 in about half the games when Blue takes Pyongyang and Wonsan). Slowing Blue's ground war (`hcg` 2.5-4) moved the Blue wins only from day 3-4 to day 4-5. What works is a
small script change: event 17 (Blue holds Pyongyang, South Pyongyang and Wonsan) is gated with `#IF_CAMPAIGN_DAY G 9` (`chn_appr3.tri`), so Blue can only end the war from about day 8.5. Chosen:

**China staged at the border (`prc-border`), `chn_appr3` (China joins when ROK holds Sariwon, Hwangju, Anbyon or Wonsan; Blue cannot win before day 9), `AlliedCounterattack` min 10 within 80 km,
depots and army bases not producing, production x0.75, battalion supply use x2.**

12 seeds, border: Blue holds **57%** of the objectives at day 10 (45-69 by seed; Blue ahead in 9 of 12), Blue's vehicles are 1.85x the North's, **Blue ends the war in 2 of 12 games (h207, h221, i.e. on
day 9)** and the other 10 are unresolved at day 10 with the sides still fighting, China keeps 56 battalions, and the North's front-line supply bottoms at 76% and ends at 90% (2 battalions under 25% at the end).
With min 9 instead: 54% (47-66), ahead in 8 of 12, vehicles 2.3x, 2 wins (both h207). For comparison, the same settings with no gate: min 10 gives 56% and Blue wins 7 of 12 games on day 3-6; min 8
gives 50% and Blue wins 1 of 12 (h114); the finer grid (counteroffensive distance 60-120 km, hcg 2.5 / 4, rest hours) moved Blue's share by 3 points or less.

| Setting (border, supply: noprod, prod x0.75, use x2) | Blue share of objectives at day 10 | Blue / North vehicles | Blue wins before day 10 |
|---|---|---|---|
| min 8, stock win rule (12 seeds) | 50% (42-58) | 1.70 | 1 of 12 (h114) |
| min 10, stock win rule (12 seeds) | 56% (44-69) | 1.91 | 7 of 12 (h64-144) |
| **min 10, no Blue win before day 9 (12 seeds)** | **57% (45-69)** | **1.85** | **2 of 12 (h207, h221)** |
| min 9, no Blue win before day 9 (12 seeds) | 54% (47-66) | 2.30 | 2 of 12 (h207) |
| min 10 + hcg 2.5, stock win rule (6) | 57% (52-68) | 1.73 | 3 of 6 (h100-113) |

**Same chosen setting, China forward instead of at the border (12 seeds each):**

| | China at the border | China forward |
|---|---|---|
| Blue's share of objectives at day 10 | **57%** (45-69), Blue ahead in 9 of 12 | 47% (44-50), Blue ahead in 0 of 12 |
| Blue / North vehicles | 1.85 | 2.76 (Blue's army is intact but it holds fewer objectives) |
| Blue ends the war | 2 of 12 (day 9) | 0 of 12 |
| Chinese battalions left | 56 | 23 |
| China joins / reaches contact | h40-68 / about h90-105 | h48-66 / about h40-55 |
| North's front supply low point / end | 76% / 90% | 87% / 94% |

China forward is the North-leaning version: it arrives early and is ground down, but the North holds the ground, and Blue never leads on objectives. At the border the war is close and Blue-leaning.

The supply settings contribute the "felt" part (the front-line low point of 76-80% against 91% for the unchanged game); they do not move the result.

## Best answer without touching the win rule: China's size is the dial (`make_china_size.py`, `chnb48`)

The win gate below is my own device. A smoother, scenario-honest dial turned out to be how many Chinese battalions there are (the Escalation mod doubles China to 86; `make_china_size.py N` deletes cloned battalions down to N, minimum 43).
Everything else fixed: China at the border, `chn_appr2` (the scenario's own win rule: Blue wins when it holds Pyongyang, South Pyongyang and Wonsan), counteroffensive min 8, depots and army bases not producing,
production x0.75, supply use x2.

| Chinese battalions | Seeds | Blue share of objectives at day 10 (mean +- se; range) | Blue ahead | Blue / North vehicles | War still on at day 10 | North front supply low / end | Chinese left |
|---|---|---|---|---|---|---|---|
| 86 (the mod) | 24 | 49.0% +- 0.9 (41-58) | 9 of 24 | 1.80 | 23 of 24 | 81 / 94 | 52 |
| 72 | 8 | 50.1% +- 1.5 (45-56) | 3 of 8 | 2.03 | 7 of 8 | 82 / 92 | 42 |
| 60 | 8 | 52.0% +- 1.4 (49-59) | 5 of 8 | 2.28 | 7 of 8 | 81 / 94 | 40 |
| **48** | **24** | **54.7% +- 0.9 (49-69)** | **23 of 24** | **2.29** | **18 of 24 (Blue wins at h78-99 in the other 6)** | **83 / 92** | **28** |
| 43 (the 43 originals) | 12 | 51.4% +- 1.2 (43-58) | 8 of 12 | 2.30 | 12 of 12 | 85 / 94 | 24 |

**48 Chinese battalions, no gate:** Blue leads on objectives in 23 of 24 games (54.7%), three games in four are still being fought on day 10, and the rest end with a Blue win between day 3.5 and day 4. Adding the day-11 gate on top
of 48 would make it ten days in every game; it was not tested together. This is the answer that needs no change to the scenario's rules, only to China's order of battle.

### Closing the early-end gap: 43-46 battalions, still the scenario's own win rule

48 battalions ended 6 of 24 games early. The next sizes down, 16 seeds each (43 has 12): **46: Blue 51.9% +- 1.0, ahead in 11 of 16, the war still on at day 10 in 16 of 16; 45: Blue 55.3% +- 1.7, ahead in 14 of 16, on in 13 of 16;
43 (the 43 originals): Blue 51.4% +- 1.2, ahead in 8 of 12, on in 12 of 12.** Pooled over the 43-46 range (44 games): Blue 53.0% of the objectives, ahead in 33 of 44 games, and **the war is still being fought on day 10 in 41 of 44 (93%)**,
with the North's front-line supply bottoming at 81-85% and ending at 90-94%, Blue / North vehicles 2.3-2.65, 22-30 Chinese battalions left. The size dial is flat within the noise between 43 and 48 (the standard errors are 1-1.7 points),
so read it as "about 45": close, Blue-leaning, nearly always unresolved on day 10, no change to any rule.

**Correction after 24 more seeds at 45 (n = 40): Blue 55.0% +- 1.0 (sd 6.2; 46-72), ahead in 35 of 40, Blue's vehicles 2.46x, the North's front supply low point 82% and 91% at the end, 29 Chinese
battalions left, and the war still on at day 10 in 30 of 40 games (75%); the other 10 end on a Blue win between day 3 and day 4.7 (h70-112).** The 93% above came from the first 16 seeds and was optimistic. Pooling every game at 43, 45
and 46 battalions (68 games) the war is still on at day 10 in 58 (85%) and Blue leads on objectives in 54 (79%). Size 46 (16 of 16 unresolved, Blue 51.9%) and 43 (12 of 12, 51.4%) are the quieter choices; 45 is the Blue-leaning one.

**All sizes on equal footing (40 seeds each, 24 for 48; border, scenario's own win rule, min 8, noprod, prod x0.75, use x2):**

| Chinese battalions | Blue share of objectives at day 10 | Blue ahead | War still on at day 10 | Blue / North vehicles | Front supply low / end | Chinese left |
|---|---|---|---|---|---|---|
| 43 (the originals) | 52.1% +- 0.8 (43-70) | 27 of 40 | **39 of 40 (98%)** | 2.50 | 83 / 92 | 23 |
| 46 | 51.9% +- 0.8 (42-67) | 26 of 40 | 35 of 40 (88%) | 2.53 | 84 / 93 | 24 |
| **45** | **55.0% +- 1.0 (46-72)** | **35 of 40** | 30 of 40 (75%) | 2.46 | 82 / 91 | 29 |
| 48 | 54.7% +- 0.9 (49-69) | 23 of 24 | 18 of 24 (75%) | 2.29 | 83 / 92 | 28 |

There is a trade-off and no setting gets both for free: **43 is the "fair war through day 10" choice** (a 98% chance the war is still being fought, Blue ahead in 68% of games, 52% of the objectives), **45 is the "Blue-sided" choice** (Blue ahead in 88%,
55%, but 25% of games end with a Blue win on day 3-4.7). The size is the dial; the day-11 gate is the way to remove the early endings at 86 battalions.

The day-11 win gate does not combine well with a small China: 48 battalions + the gate gives Blue 66.8% (58-72, ahead in 16 of 16, vehicles 3.3x), because Blue keeps taking ground with no win to stop it. The gate belongs with the 86-battalion
mod (Blue 56%, 30 seeds, no game ends early), not with the small-China setting.

**Recommended (scenario rules unchanged): China at the border, about 45 battalions (`make_china_size.py 45`), `chn_appr2`, `AlliedCounterMin 8`, depots and army bases not producing, production x0.75, supply use x2.**
Alternative, gate-based: the 86-battalion mod with `chn_appr4` and min 10 (below).

## Final choice: the same setting with the war guaranteed to run the ten days (`chn_appr4`, no Blue win before campaign day 11)

The day-9 gate still let 2 of 12 games end on day 9. Gating the win at `#IF_CAMPAIGN_DAY G 11` (`chn_appr4.tri`, `d10x` in the spread) means Blue cannot end the war inside the ten days at all. 12 seeds, China at the border,
`AlliedCounterMin 10`, depots and army bases not producing, production x0.75, supply use x2: **Blue holds 54% of the objectives at day 10 (45-65 by seed), Blue is ahead in 8 of 12 games, Blue's vehicles are 2.1x the North's,
no game ends early (0 of 12), China keeps 47 battalions, the North's front-line supply bottoms at 78% and ends at 90%.** With China forward (6 seeds, same rules): Blue 47% (46-49), ahead in 0 of 6, China 24 left. The
day-9 version (57%, 2 of 12 end on day 9) and the stock win rule (56%, 7 of 12 end on day 3-6) are the alternatives if you prefer Blue to be able to finish it.

### The final setting on a wide sample (30 border seeds, 18 forward seeds, plus references)

| Setting | Seeds | Blue share of objectives at day 10 (mean +- standard error; sd; range) | Blue ahead | Blue / North vehicles | North front supply low / end | Chinese left | Blue wins before day 10 |
|---|---|---|---|---|---|---|---|
| **Final: border, min 10, Blue cannot win before day 11** | 30 | **56.0% +- 1.4** (sd 7.9; 43-74) | 22 of 30 | 2.06 | 77 / 91 | 52 | 0 |
| Same, China forward | 18 | 45.9% +- 0.9 (sd 3.8; 33-50) | 1 of 18 | 2.51 | 85 / 94 | 22 | 0 |
| Min 8, stock win rule, border | 24 | 49.0% +- 0.9 (sd 4.6; 41-58) | 9 of 24 | 1.80 | 81 / 94 | 52 | 1 |
| Unchanged supply (control), border | 12 | 49.0% +- 1.3 (sd 4.4; 43-56) | 5 of 12 | 1.68 | 91 / 95 | 49 | 0 |
| Unchanged supply (control), forward | 6 | 46.9% +- 0.3 | 0 of 6 | 3.38 | 92 / 95 | 18 | 0 |

The win gate is my own design choice, not something the scenario had: without it every Blue-ahead setting ends the war on day 3-6 in about half the games (min 10, stock rule: 7 of 12) and the only setting that runs the ten days
without a gate (min 8) is even, not Blue-sided. If you would rather not touch the win rule, min 8 is the honest even-war setting; the gate is what turns "Blue-leaning" into "Blue-leaning and still going on day 10".

## Supply code, second pass (what reading it again found)

- **Replacements are a stock, supply is a flow.** `ProduceSupplies` halves the supply and fuel pools each tick and caps them at `MAX_SUPPLIES`, but the replacement pool is `old + new` with no decay and no cap: that is why the
  North ends a 10-day war holding about 4,000 replacements it never used. Delivery is the limit: a battalion gets at most half of its shortfall times the ground share, only on its resupply tick (20 min on attack orders, 3 h otherwise),
  and the rounding-up bonus only below 85% strength on a 51% roll. Production of replacements is not what binds.
- **A latent underflow.** In the ground-replacement branch `SetReplacementsAvail(avail - repl_v)` has no guard (only the rounding-up branch checks `avail > 12`); the pool is a `ushort`, so an over-draw would wrap to about 65,000. The ratios
  keep the total under the pool in practice, so it has not shown; it is a one-line guard if it ever does.
- **A dead path.** The old squadron replacement code subtracts the replacements twice (`NoTypeBonusRepl = 0` only; it is 1 in the shipped AII).
- **The delivery path is a wall for anything not friendly.** `SendSupply` (and the store search) plan with `PATH_MARINE` only, so an enemy-held objective or a broken bridge ends the route; 22% of the North's resupply attempts find no
  source in the stock war. Losses are multiplicative per road, intersection, rail or bridge node (2% each, exact maths since `SupplyExactLoss`; `SupplyInterdiction` makes damaged nodes cost up to 15%, bridges 50%).
- **Production has no memory of damage beyond status.** Output is `DataRate x status x nearest friendly power plant`; the AI's strikes leave it at 87-99% in a normal war (capture is 70-80% of the loss), so a production-based choke
  needs a player or a future AI that bombs and keeps the factories down.
- **The action-type multiplier is off.** `actionBonus` (offensive x4, defensive x1, none x0) would multiply production, but `NoActionBonusProd = 1` in the AII turns it off; turning it on would make the offensive side four times
  as productive, and a team with no action none.
- **Unit-level levers that exist but do nothing in 10 days:** moving 2% per about 20 km, idle 2% per tick, firing a share; at the stock rates consumption is about a sixth of production, which is why use x2-x3 and a production
  scale of 0.75 are needed for supply to be felt at all.

## What the supply code does (re-read for this)

Production (`ProduceSupplies`): factory, port, army base, depot make supply and replacements (class DataRate x damage x nearest friendly power plant), refineries fuel;
`DataRateModSup` 1.5 and `DataRateModRepl` 1.0 (Falcon4.AII) scale them; 5 supply and 1 replacement are free each hour; a team's action type multiplies production unless
`NoActionBonusProd` is set (it is, = 1). The national pool is half of last tick's plus the new production (a flow, not a stockpile). `SupplyUnits` hands each battalion
or squadron up to `MAX_SUPPLY_RATIO` (0.5) of its need per resupply, battalions every 20 minutes on attack orders (x6 bonus) and every 3 hours otherwise
(`GND_OFFENSIVE_SUPPLY_TIME` / `GND_DEFENSIVE_SUPPLY_TIME`, `unit.cpp`), over the objective links with 2% loss a node. Vehicles also draw on the fuel pool (`SupplyUnit`: supply + fuel/2).
Airlift landings add supply, fuel and replacements from nothing (capped by `AirliftNeed` / `AirliftRunway`, both on by default now).

How supply reaches the fighting (this is why it can only matter through the percentage): a battalion's combat strength is `supply% x (100 - fatigue)% x morale% x experience`
(`AdjustForSupply`), its shots scale with supply %, below 50% the GTM tasks it only as reserve (`gtm.cpp`), at 30% or less it cannot fire (`haveWeaps`), a brigade needs more than 20%.
Consumption: moving costs 2% per about 20 km, waiting 2% per tick, firing a share of the vehicles that shoot; none of it was big enough to ever bind in a short war, which is the
original finding. The team supply level that the scripted triggers read (`IF_SUPPLY`) averages every unit including the rear and the air force and stays near 91%, so a front-line
choke never fires a supply-keyed event.

Search and delivery: `FindNearestSupplySource` walks the objective links and refuses any enemy-owned objective and any broken bridge, so a unit in a small pocket near the front
cannot be resupplied (22% of the North's resupply attempts in the stock baseline find no source). The regional-stores and convoy ideas are documented in `CAMPAIGN-SUPPLY-ENGINE.md`.

## Levers, measured (least squares over 208 moderate runs, `spread/levers_all.txt`)

Per step of the lever, others held (rough standard error in brackets; clear = more than 2 standard errors). North front-line supply low point, in percentage points:

| Lever | Supply low point | Blue share of objectives |
|---|---|---|
| battalion supply use, per +1x | -7.3 (0.4) clear | +0.3 (0.4) noise |
| start supply, per -25 points | -16.9 (0.6) clear | +0.2 (0.6) noise |
| production scale, per -0.5 (1.5 -> 1.0 -> 0.5) | -3.0 (0.5) clear | 0.0 (0.5) noise |
| ground resupply interval (non-attack orders), per +60 min | -2.3 (0.2) clear | -0.1 (0.2) noise |
| max share of need per resupply, per -0.1 | -2.1 (0.5) clear | -0.3 (0.5) noise |
| depots and army bases not producing | -0.4 (0.8) | -0.4 (0.8) |
| replacement scale, per -0.5 | -0.1 (0.6) | +0.2 (0.6) |
| no airlift credit | -1.2 (1.2) | +0.8 (1.2) |
| economic sliders up, bridges bombed (`objpri`) | -0.8 (1.0) | -1.6 (0.9) |
| **China staged at the border** | -3.4 (0.7) clear | **+2.4 (0.7) clear** |

Hard supply on purpose (6 seeds each, border; `h1`-`h6`): use x3-5, start 40-60%, production x0.4-0.75, resupply every 6-12 h, max share 0.25-0.3: the North's front-line
supply falls to 7-40% (end of war 22-61%), 58-220 battalions under 25%, and Blue's share stays 48-52% (the control is 49%), Blue/North vehicles drop from 1.7 to 1.0-1.35 (both
sides fight less), China survives better (77-85 left). Starvation freezes the front. The one thing supply does is change how the war *looks* and what a player's strikes on the economy
buy (earlier results: factories held down take the front to 14-38% supply, but the war ends at the same time).

## China at the border or not (same seeds 1-6, `c0` = no supply change)

| Config | Border: Blue share / vehicles B:N / China left | Forward: Blue share / vehicles B:N / China left |
|---|---|---|
| control | 51% / 1.70 / 51 | 47% / 3.38 / 18 |
| noprod, prod x0.75, use x2 | 52% / 1.83 / 52 | 47% / 2.25 / 26 |
| noprod, use x3, repl x0.5 | 51% / 2.00 / 47 | 48% / 3.03 / 19 |
| noprod, use x3, repl x0.5, max share 0.35 | 50% / 1.91 / 47 | 46% / 1.76 / 30 |
| noprod, use x3, resup 480 | 48% / 1.90 / 46 | 45% / 2.66 / 24 |
| use x2, sliders | 48% / 2.08 / 47 | 46% / 2.21 / 24 |
| use x3, start 50% | 50% / 1.71 / 48 | 48% / 2.18 / 24 |

China joins at h33-70 in both (the approach trigger, `chn_appr2`); at the border it reaches contact at h91-107, forward at h38-55.

## The China counteroffensive dial (border, `noprod prodsup=0.75 usemult=2`)

| `AlliedCounterMin` (allied battalions at the front) | Blue share (range) | Blue/North vehicles | Blue wins (event 17) |
|---|---|---|---|
| no counteroffensive | 82% (79-85) | 3.2 | 6 of 6, h74-108 |
| capture orders only (`AlliedCapture`) | 83% (80-89) | 4.1 | 6 of 6, h64-98 |
| 24 | 71% (68-80) | 2.5 | 6 of 6 |
| 16 | 67% (61-72) | 2.3 | 6 of 6, h64-97 |
| 12 | 63% (56-69) | 2.8 | 6 of 6, h64-107 |
| 12, within 120 km | 57% (47-62) | 1.9 | 5 of 6 |
| 10 (12 seeds) | 56% (44-69) | 1.9 | 7 of 12, h64-144 |
| 9 (10 seeds) | 55% (44-63) | 2.1 | 4 of 10, h86-99 |
| **8 (12 seeds)** | **50% (42-58)** | **1.7** | **1 of 12 (h114)** |

(`noend=1` keeps the war running past Blue's win; "wins" is the first moment Blue holds Pyongyang and Wonsan.)

## Look at them (viewer, `http://localhost:8770/viewer.html?run=<name>` after `python serve.py 8770` in `tools/campsim`)

- `chnb-sp-npp075u2-s1` - the recommended setup, a Blue-leaning seed (58%); `chnb-sp-npp075u2-s4` - the same, an even one (51%).
- `chnb-sp-c0-s1` - China at the border, no supply change; `save0-sp-c0-s1` - the same with China forward (compare how China fares).
- `chnb-sp-t10-s1` - counteroffensive at 10: Blue wins at h85; `chnb-sp-t9-s2` - at 9.
- `chnb-sp-h1-s1`, `chnb-sp-h5-s1` - the starved North: front supply 7-11% and the front frozen.
- `chnb-sp-npu3r50mr35-s1` - a harder supply setup (front low point 74%) that is still even.

## What is and is not shipped

All of the supply and China switches are simulator knobs or cfg switches that default to the old behaviour except where noted; nothing here was built into the game
executable or installed beyond test scripts in `C:\FreeFalcon6\Campaign\SAVE` (`chn_*.tri`, `chnb.cam`). To play the recommended setup the game needs the cfg switches
(`AlliedCounterattack 1`, `AlliedCounterMin 8`, `NoProd`-style depot rule and the use / production scales are not cfg switches yet: they exist as campsim knobs), the
approach trigger in `save0.tri`, and the border staging in the Escalation mod build (`make_ally_mod.py prc-border`). Russia is parked.

Caveats: two campsim quirks - the seeds differ by up to 16 points of Blue share inside one config, so quote the means over 6-12 seeds, not single runs; and every number is
for the Escalation scenario (Blue combat air cut to Falcon 4.0 levels, DPRK bombers, doubled PRC), not stock Rolling Fire.
