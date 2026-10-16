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
4. **Fair, close, Blue-ahead in force, supply felt, no Blue win in 10 days (11 of 12 seeds):** China at the border + `AlliedCounterMin 8` +
   depots and army bases not producing + production x0.75 + battalion supply use x2 (`noprod=1 prodsup=0.75 usemult=2`). Blue ends with 50% of the objectives
   (42-58 by seed), 1.7x the North's vehicles, the North's front-line supply bottoms at about 82% (mean) and ends at 94%. Min 9 or 10 makes Blue 55-56% but ends
   the war on day 3.5-6 in 40-60% of seeds (Blue takes Pyongyang and Wonsan).

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
