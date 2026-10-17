# Can deep strikes on the North's economy change a 10-day Korea war? (campsim, 2026-10-17)

Goal: with a revamped supply model and stronger rear air defence, do a player's strikes on factories, power plants and refineries change the outcome of a
10-day war by more than the noise, at a visible cost to Blue? Campsim only: the game exe and the install's `save0` were not touched. Every mechanism is a switch,
off by default. No win gates, no timers. 325 ten-day runs, 34 arms, results in `tools/campsim/strike/results.jsonl`, full tables in `tools/campsim/strike/report.txt`.

## The answer

**Strikes plus a mechanism that lets starvation bite move Blue's share of objectives by 5-7 points and remove the North's wins, but nothing produces a campaign that ends with a winner around day 10.**
The strike (factories, power / nuclear plants and refineries of the North held destroyed from hour 48) on its own moves the share by +1.8 +-1.1 points, which is noise.

| Mechanism on top of the strike | Strike minus control, Blue share at day 10 (paired, same seeds) | Games per arm |
|---|---|---|
| none (stock engine) | +1.8 +-1.1 | 12 |
| M1 starvation drains morale | +1.8 +-1.5 | 9-10 |
| M2 supply-aware fall-back | **+5.9 +-1.9** | 9 |
| M1 + M2 | **+5.7 +-0.9** | 10 |
| M3 SA-10 / SA-5 on every economic site, all Chinese AD to SA-10 | +5.0 +-3.1 | 12 |
| control for M3: the same number of extra weak (KS-19 gun) battalions | +4.0 +-1.1 | 12 |
| M1 + M2 + M3 | +4.5 +-1.1 | 10 |
| M4 stores (depots and army bases hold local stock) | +1.0 +-1.0 | 9 |
| M5 strike shock (morale falls near damaged objectives) | +4.9 +-0.8 | 7 |
| M5 + M1 + M2 | **+7.3 +-1.2** | 6 |
| M6 initiative counts supply-weighted ground vehicles | +3.6 +-1.1 | 8 |
| M6 + M1 + M2 | **+6.2 +-1.1** | 8 |
| tight supply (use x4, production 0.5) + harsh M1 + M2 | -3.4 +-3.3 | 9 |
| ... and the SAM world | +1.2 +-2.2 | 9 |

**Who wins.** Across the 13 strike / control pairs (stores left out), the North took Seoul or Pusan in 16 of 122 games without a strike and in 1 of 121 with one; Blue took Pyongyang and Wonsan in 13 and 11.
So strikes protect Blue against losing but do not help it win, and the war does not end: with a strike 9-12 of every 12 games are undecided at day 10 (stock: 12 of 12). The only setup that regularly produces an ending is the
SAM world, and its Blue wins come around hour 94 (day 4), the early ending the brief does not want (SAM world: 4 Blue and 3 North wins in 12 games without a strike, 6 and 0 with; the weak-battalion control gives 0 and 1, so it is the SAMs, but why is not explained).

**Cost to Blue.** The strikes themselves are free (the targets are simply held destroyed). The only price in these numbers is what the SAMs add to ordinary operations: **+311 aircraft lost per game (+18%), +429 to ground fire (+34%)**, almost all
near the front. M5 hurts Blue too: its no-strike control sits at 46.6% against 50.8%, because Blue's own bombed airbases and bridges shock Blue's battalions.

**Recommendation.** The part of this worth keeping is M2 and M1 + M2 (cheap, +5.7 +-0.9, the North's wins disappear, no cost to Blue). M5 and M6 add 1-2 points on top and are not yet worth the complexity. None of it will end the war by day 10,
because **Blue's ground offensive does not respond to the enemy being weak**: Blue is on full offensive in 18-20% of frames whether the North is fed or starved, even with the front at 24% supply the share is 54-56%, and the initiative score that starts an
offensive (see below) did not unlock more even when it counted supply. The next thing to look at is what makes Blue's battalions take objectives (about 10 are on capture orders at any time), not more on the North's side.

## Where the North's air defence sits, and what Blue loses to it, by depth

Static (`sam_depth.py chnb.cam`): 186 DPRK air-defence battalions plus 12 Chinese and 4 Russian. DPRK: 103 KS-19 (100 mm guns), 47 SA-2, 22 S-60, 9 SA-3, 3 SA-5, 2 SA-4; **no SA-10** (6 in China, 2 in Russia's team).
By depth north of the starting front (y 490, 50 km bands, DPRK + PRC): 13 behind the line, then 20 / 43 / 25 / 34 / 27 / 14 / 8 / 6 / 8 going north. 72 of the 74 factories, power and
nuclear plants, chemical plants and refineries have an AD battalion within 25 km (most of them KS-19 or SA-2).

Dynamic (new `AIRLOSSDEPTH` line, aircraft Blue lost to ground fire, by the shooter's position, mean per game, stock war, 12 games):

| Shooter's y | south of 500 | 500-549 | 550-599 | 600-649 | 650-699 | 700+ |
|---|---|---|---|---|---|---|
| Blue aircraft lost (any ground shooter / air-defence battalions) | 274 / 72 | 273 / 129 | 459 / 315 | 61 / 55 | 76 / 75 | 42 / 41 |

85% of what Blue loses to ground fire is lost within 110 km of the starting line (and half of that to something other than an AD battalion); the deep belt (y 600 and north) costs about 180 aircraft a game, nearly all AD.
**Blue's AI almost never flies deep**: the AI-strike arm (sliders for factories, power plants, depots and roads at 100) lost the same ~175 aircraft deep as the control. Adding an SA-10 and an SA-5 on every economic site
raised the near-front losses by about 400 a game and the deep ones by about 65: those sites sit between y 465 and 550, in the band Blue's close support flies. A player who flies deep would pay far more, but the campaign AI cannot show that and the harness cannot test a player.

## What the strikes do, and why that is not enough

Stock engine, 12 seeds each:

| | no strike | strike |
|---|---|---|
| Blue share of objectives, h240 | 50.8 +-1.0 | 52.6 +-0.9 |
| North front-line battalion supply, lowest / at h240 | 80% / 93% | 59% / 69% |
| North battalions under 25% supply at h240 | 1 | 18 |
| North battalions on CAPTURE orders (mean after h72) | 24.8 | 11.3 |
| North on full offensive (share of frames) | 72% | 53% |
| North ground vehicles at h240 | 1,869 | 2,417 |
| Blue on full offensive | 20% | 18% |

A starved battalion (under 50% supply) is tasked as reserve only, so the North's *attackers* disappear and the China counteroffensive that normally pushes Blue's share from 56% (hour 96) back to 50% (hour 240) does not
happen: with the strike the share holds at 52-54%. The defence does not weaken: garrisons keep their valid DEFEND orders. The North also loses *fewer* vehicles (an attacker loses more than a defender), which is why the strike leaves it with 550 more vehicles at day 10.

Why a starved North froze the front (item 1 of the brief): supply never fed morale; about 75% of the North's battalions are on RESERVE orders and never touch the front; the garrison check that sends a battalion under half supply to reserve comes *after* the rule that keeps a valid order, so a starved garrison keeps holding.

Why Blue does not exploit it (read from the code, `team.cpp`): a side starts an offensive only when its **initiative** is 50-55 or more, and a side being attacked is forced onto the defensive. Initiative is 50% ground-vehicle ratio, 40% aircraft ratio and 10% losses, each as an arctangent of
ours over theirs; it never looks at supply, production or objectives. M6 changes the vehicle term to supply-weighted vehicles; the strike effect with it is +3.6 (+6.2 with M1 + M2) and the games still do not end.

## What was built (all campsim-only knobs, off by default)

| Switch | What it does | Result |
|---|---|---|
| `starvemorale=G[:BREAK]` (M1) | `RallyUnit`: a battalion under half supply loses G morale points an hour at 0 supply (in proportion below half) instead of regaining it, and breaks under BREAK; broken = WITHDRAW tactic, double speed, reserve only. G=12 break 30 | with strikes mean North morale 88 -> 74; +1.8 alone |
| `supplyfall=1` (M2) | `AddToLists` (`gtm.cpp`) + `FindSupplyFallback` (`gndunit.cpp`): a battalion under half supply that is not attacking is sent to the nearest city, port, depot or army base behind the third line and stays until it is back over 80% | 200 moves a game in the control, ~800 with strikes; +5.9 alone |
| `blowat=HOUR` | the `blow=` rules (factory / power / refinery, `hold`) start at that hour | the delayed-strike harness |
| M3 `make_sam_mod.py` | `sites`: an SA-10 (class 799) and an SA-5 (733) battalion on each of the 74 economic sites; `china`: all Chinese AD becomes SA-10 plus one more with every 4th Chinese battalion; `dummy`: two KS-19 on each site as the control. Scenarios `chboth`, `chsite`, `chchn`, `chsa5`, `chdum` in the install's `Campaign\SAVE` (test files, not shipped) | the table above |
| `AIRLOSSDEPTH` | aircraft lost to ground fire by shooter band, every 6 hours | the depth table above |
| M4 | the existing store model (`stocks=1 storecap=25 storestart=50`); the search that refuses enemy-owned links was *not* changed | strike +1.0 +-1.0; the model on its own is bad for Blue: share 44.5% against 50.8%, the North wins in 4 of 9 control games |
| `strikeshock=G[:KM]` (M5) | `RallyUnit`: a battalion loses G morale points an hour (scaled by damage and nearness) while its own side's factories, power / nuclear plants, refineries, depots, army bases, ports, airbases or bridges within KM (default 60) are below 75% status. G=20, 60 km. Hourly list of damaged objectives, so it is cheap. Stock: morale only changes when the unit is shot at | with strikes: front supply 91% -> 32% at h240, 86 battalions under 25%; +4.9 alone, +7.3 with M1 + M2; both teams are shocked, so the no-strike baseline falls to 46% |
| `initsupply=1` (M6) | `NewInitiativePointSetting`: the ground term counts vehicles x battalion supply | +3.6 alone, +6.2 with M1 + M2 |

M1, M2, M5 and M6 apply to every team; Blue's starved or shocked battalions react too. Orchestration: `tools/campsim/strike_spread.py` (arms, pacing, report), `make_sam_mod.py`, `sam_depth.py`.

## Everything else that was tried

| Arm | Games | Share h240 | North front supply (low) | Blue aircraft lost | Blue wins, North wins, undecided |
|---|---|---|---|---|---|
| no strike | 12 | 50.8 | 80% | 1,710 | 2, 1, 9 |
| strike | 12 | 52.6 | 59% | 1,669 | 0, 0, 12 |
| AI strike (sliders, Blue flies it) | 10 | 48.5 | 80% | 1,759 | 0, 3, 7 |
| AI strike in the SAM world | 10 | 53.7 | 74% | 2,049 | 3, 0, 7 |
| SAM world, no strike / strike | 12 / 12 | 50.5 / 55.5 | 73% / 48% | 2,021 / 1,845 | 4,3,5 / 6,0,6 |
| weak-battalion control, no strike / strike | 12 / 12 | 49.0 / 53.0 | 82% / 54% | 1,882 / 1,872 | 0,1,11 / 0,0,12 |
| M1 + M2, no strike / strike | 10 / 10 | 49.2 / 54.9 | 82% / 64% | 1,687 / 1,722 | 1,1,8 / 1,0,9 |
| M1 + M2 + M3, no strike / strike | 10 / 10 | 49.6 / 54.0 | 78% / 57% | 1,952 / 1,854 | 0,1,9 / 1,1,8 |
| stores, no strike / strike | 9 / 9 | 44.5 / 45.5 | 64% / 58% | 1,699 / 1,660 | 0,4,5 / 0,4,5 |
| tight supply + M1 (harsh) + M2, no strike / strike | 9 / 9 | 56.0 / 52.5 | 24% / 25% | 1,662 / 1,595 | 3,0,6 / 0,0,9 |
| ... in the SAM world | 9 / 9 | 54.3 / 55.4 | 30% / 24% | 1,909 / 1,731 | 1,0,8 / 2,0,7 |
| strike shock, no strike / strike | 7 / 7 | 46.6 / 51.4 | 78% / 23% | 1,628 / 1,624 | 0,0,7 / 0,0,7 |
| strike shock + M1 + M2, no strike / strike | 6 / 6 | 45.6 / 52.9 | 75% / 21% | 1,649 / 1,700 | 0,0,6 / 0,0,6 |
| initiative with supply, no strike / strike | 8 / 8 | 50.9 / 54.5 | 80% / 60% | 1,714 / 1,695 | 2,1,5 / 1,0,7 |
| initiative with supply + M1 + M2, no strike / strike | 8 / 8 | 47.1 / 53.3 | 82% / 59% | 1,693 / 1,670 | 0,3,5 / 0,0,8 |

When and what to strike (paired against the no-strike control, base world, 11-12 games each): from hour 24 -0.2 +-1.4; from 48 +1.8 +-1.1; from 96 +1.2 +-0.7; factories only -0.2 +-1.5; power / nuclear plants only +3.1 +-1.4.
The power plants matter more than the factories; an earlier or later start does not help.

## Caveats

- The strike is free to Blue by construction. The only Blue cost measured is the one the SAMs add to ordinary operations. A human flying deep would pay the deep-belt price, which the campaign AI never does.
- **Seed counts are uneven, and below the brief's 12 per arm for most of the later arms**: 12 for the stock, SAM and weak-battalion pairs; 9-10 for M1, M2, M1 + M2, the combined SAM arms, stores and tight supply; 6-8 for the strike-shock and initiative arms (the queues were stopped when the question changed); 11 for the timing / target arms.
  The error bars above are standard errors of the paired differences; read the 6-8 game arms (shock, initiative) as indications.
- The extra battalions of M3 stand alone on their objectives (no brigade). The weak-battalion control exists because that alone changes the war; compare M3 with it, not with the stock arm.
- The work ran as several batches (the main pairs, second-round variants, a top-up, then M5 and M6 at the user's request), not as one round. 325 runs, four exes built from `5c670350` and the commits after it.
- `main` needed hand-resolved conflicts in `gtm.cpp` and `main.cpp` (it has no railnet) and was compile-checked after the first commit; the strike-shock and initiative commits have not been built on `main`.
- The Chinese counteroffensive, the trigger and the supply knobs are the best of the earlier spread (`noprod=1 prodsup=0.75 usemult=2`, `alliedcounter=1:8:80`, `chn_appr2`, China on its own side of the Yalu); nothing about them was changed.

## Reproduce

```
python tools/campsim/make_sam_mod.py sites,china chnb chboth      # scenarios (needs the install's Campaign\SAVE\chnb.cam, see make_ally_mod.py prc-border)
set CAMPSIM_EXE_DIR=<folder with the built campsim.exe and its dlls>
python tools/campsim/strike_spread.py plan                         # plan3..plan8 for the other arms (tight supply, stores, shock, initiative)
python tools/campsim/strike_spread.py reanalyse ; python tools/campsim/strike_spread.py report
```
