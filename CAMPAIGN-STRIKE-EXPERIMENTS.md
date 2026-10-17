# Can deep strikes on the North's economy change a 10-day Korea war? (campsim, 2026-10-17)

Goal: with a revamped supply model and stronger rear air defence, do a player's strikes on factories, power plants and refineries change the outcome of a
10-day war by more than the noise, at a visible cost to Blue? Campsim only: the game exe and the install's `save0` were not touched. Every mechanism is a switch,
off by default. No win gates, no timers. 366 ten-day runs, 34 arms, results in `tools/campsim/strike/results.jsonl`, full tables in `tools/campsim/strike/report.txt`.

## The answer

**Strikes plus a mechanism that lets starvation bite move Blue's share of objectives by 5-7 points and remove the North's wins, but nothing produces a campaign that ends with a winner around day 10.**
The strike (factories, power / nuclear plants and refineries of the North held destroyed from hour 48) on its own moves the share by +1.8 +-1.1 points, which is noise.

| Mechanism on top of the strike | Strike minus control, Blue share at day 10 (paired, same seeds) | Games per arm |
|---|---|---|
| none (stock engine) | +1.8 +-1.1 | 12 |
| M1 starvation drains morale | +2.3 +-1.3 | 11 |
| M2 supply-aware fall-back | **+5.7 +-1.5** | 11 |
| M1 + M2 | **+5.7 +-0.8** | 11 |
| M3 SA-10 / SA-5 on every economic site, all Chinese AD to SA-10 | +5.0 +-3.1 | 12 |
| control for M3: the same number of extra weak (KS-19 gun) battalions | +4.0 +-1.1 | 12 |
| M1 + M2 + M3 | +4.5 +-1.1 | 10 |
| M4 stores (depots and army bases hold local stock) | +0.6 +-0.9 | 11 |
| M5 strike shock (morale falls near damaged objectives) | +5.4 +-0.7 | 10 |
| M5 + M1 + M2 | **+7.0 +-1.0** | 10 |
| M6 initiative counts supply-weighted ground vehicles | +2.8 +-1.3 | 10 |
| M6 + M1 + M2 | **+6.1 +-0.9** | 10 |
| tight supply (use x4, production 0.5) + harsh M1 + M2 | -3.1 +-3.0 | 10 |
| ... and the SAM world | +1.1 +-2.0 | 10 |

**Who wins.** Across the 13 strike / control pairs (stores left out), the North took Seoul or Pusan in 16 of 140 games without a strike and in 1 of 139 with one; Blue took Pyongyang and Wonsan in 15 and 11.
So strikes protect Blue against losing but do not help it win, and the war does not end: with a strike most games are undecided at day 10 (stock: 12 of 12; 9-10 of 10 in most other arms). The only setup that regularly produces an ending is the
SAM world, and its Blue wins come around hour 94 (day 4), the early ending the brief does not want (SAM world: 4 Blue and 3 North wins in 12 games without a strike, 6 and 0 with; the weak-battalion control gives 0 and 1, so it is the SAMs, but why is not explained).

**Cost to Blue.** The strikes themselves are free (the targets are simply held destroyed). The only price in these numbers is what the SAMs add to ordinary operations: **+311 aircraft lost per game (+18%), +429 to ground fire (+34%)**, almost all
near the front. M5 hurts Blue too: its no-strike control sits at 46.6% against 50.8%, because Blue's own bombed airbases and bridges shock Blue's battalions.

**Recommendation.** The part of this worth keeping is M2 and M1 + M2 (cheap, +5.7 +-0.8, the North's wins disappear, no cost to Blue). M5 and M6 add about 0-1 points on top and are not worth the complexity. None of it will end the war by day 10,
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
ours over theirs; it never looks at supply, production or objectives. M6 changes the vehicle term to supply-weighted vehicles; the strike effect with it is +2.8 (+6.1 with M1 + M2) and the games still do not end.

## Why Blue stops capturing at day 3-4 (read from the existing runs, no new runs)

Blue is not slow early: in every one of the 24 stock and strike seeds it is on offensive 80-96% of hours 6-72 and gains about 4 objectives an hour (1,239 to ~1,550, a 62-65% share at the peak, hour 65-90).
After hour 84 it is on offensive 0-19% of the time and only 2-3 of its ~230 battalions hold capture orders, so Pyongyang is never reached. The cause is the initiative score (INIT lines in the logs, seed 2):

| Hour | Blue vs North ground vehicles | aircraft | ground term | air term | blend | Blue initiative |
|---|---|---|---|---|---|---|
| 42 | 4,041 vs 2,640 | 216 vs 232 | 64 | 48 | 58 | 60 |
| 60 | 4,175 vs 1,961 | 228 vs 247 | 73 | 48 | 62 | 76 |
| 78 | 3,816 vs 2,944 | 151 vs 689 | 59 | 14 | 41 | 50 |
| 114 | 3,805 vs 2,708 | 163 vs 727 | 61 | 15 | 43 | 42 |
| 240 | 3,682 vs 1,916 | 189 vs 920 | 70 | 13 | 46 | 45 |

When China's air force joins (about hour 70), the aircraft term (40% of the blend) falls from 48 to 13 and holds Blue under the 50 it needs to start an offensive, although Blue keeps a 1.4-1.9x ground lead.
A side being attacked is forced defensive, so the North keeps the offensive for the rest of the war. This is also why starving the North's ground units, or weighting initiative by supply (M6), did so little: the ground term is not the limiter.
`initw=G:A:L` (blend weights, stock 5:4:1) was built to test it; a first test (8:1:1, 8 seeds each, strike and no strike) was queued and stopped at the user's request before any result, so the hypothesis is untested.

## A player pushing north in the frozen window, with and without the strike (4-5 seeds per arm)

The frozen phase (Blue defensive from hour ~84) is a refusal of the AI to task Blue, not a lack of strength, so a player who orders Blue's battalions forward should get around it. campsim's `holdtest` was extended
(`holdat=H`, `holdchain=1`): at hour 84, 12 Blue battalions are given capture orders at the nearest North objective exactly as a waypoint drag does, and a battalion whose target has fallen is sent at the next nearest
one within 60 km. `playerhold=1` makes the AI leave them alone (the WIP `g_bPlayerGroundHold`); `playerhold=0` lets the AI take them back, as stock does. (An engine crash this exposed, a null objective in
`BattalionClass::MoveUnit`, is guarded.) Same seeds as the arms above, so the comparison is paired where the seed exists; the no-hold baselines were run on the older exe.

| Arm | Games | Blue share h240 | Change vs same-seed no-hold | Objectives the ordered battalions took | Blue wins (hours) | North wins |
|---|---|---|---|---|---|---|
| player push, no strike | 4 | 52.2 | +2.0 | 268 | 0 | 0 |
| player push, AI may take units back, no strike | 5 | 48.7 | -2.3 | 277 | 0 | 1 |
| **player push + strike** | 4 | **63.4** | **+11.4** | 408 | **4 of 4** (132, 152, 188, 199) | 0 |
| **player push + strike + starvation morale + fall-back** | 5 | **67.7** | **+14.2** | 362 | **4 of 5** (98, 128, 200, 206) | 0 |

Neither the strike alone (no wins, share +1.8) nor the push alone (+2.0) does it; together they end the war with a Blue win in 8 of 9 games, most of them between day 5 and day 9, which is the "campaign ending around day 10 with a winner" the brief wanted.
The order of causes the logs give: the strike starves the North's garrisons (front supply 44% at the lowest) and takes away its offence; the AI will not use that, because Blue's initiative sits at 45-50, so the player's orders
are what carry the front forward. With the AI free to retask the battalions (`playerhold=0`) the push does nothing. Caveats: 4-5 games per arm; the "player" is an idealised one (12 battalions, always the nearest objective, instant re-orders); the
objectives-taken count includes objectives that change hands more than once; the no-hold baselines are from the older exe.

## What was built (all campsim-only knobs, off by default)

| Switch | What it does | Result |
|---|---|---|
| `starvemorale=G[:BREAK]` (M1) | `RallyUnit`: a battalion under half supply loses G morale points an hour at 0 supply (in proportion below half) instead of regaining it, and breaks under BREAK; broken = WITHDRAW tactic, double speed, reserve only. G=12 break 30 | with strikes mean North morale 88 -> 74; +2.3 alone |
| `supplyfall=1` (M2) | `AddToLists` (`gtm.cpp`) + `FindSupplyFallback` (`gndunit.cpp`): a battalion under half supply that is not attacking is sent to the nearest city, port, depot or army base behind the third line and stays until it is back over 80% | 200 moves a game in the control, ~800 with strikes; +5.7 alone |
| `blowat=HOUR` | the `blow=` rules (factory / power / refinery, `hold`) start at that hour | the delayed-strike harness |
| M3 `make_sam_mod.py` | `sites`: an SA-10 (class 799) and an SA-5 (733) battalion on each of the 74 economic sites; `china`: all Chinese AD becomes SA-10 plus one more with every 4th Chinese battalion; `dummy`: two KS-19 on each site as the control. Scenarios `chboth`, `chsite`, `chchn`, `chsa5`, `chdum` in the install's `Campaign\SAVE` (test files, not shipped) | the table above |
| `AIRLOSSDEPTH` | aircraft lost to ground fire by shooter band, every 6 hours | the depth table above |
| M4 | the existing store model (`stocks=1 storecap=25 storestart=50`); the search that refuses enemy-owned links was *not* changed | strike +0.6 +-0.9; the model on its own is bad for Blue: share 44.8% against 50.8%, the North wins in 4 of 11 control games |
| `strikeshock=G[:KM]` (M5) | `RallyUnit`: a battalion loses G morale points an hour (scaled by damage and nearness) while its own side's factories, power / nuclear plants, refineries, depots, army bases, ports, airbases or bridges within KM (default 60) are below 75% status. G=20, 60 km. Hourly list of damaged objectives, so it is cheap. Stock: morale only changes when the unit is shot at | with strikes: front supply 91% -> 32% at h240, 86 battalions under 25%; +5.4 alone, +7.0 with M1 + M2; both teams are shocked, so the no-strike baseline falls to 46% |
| `initsupply=1` (M6) | `NewInitiativePointSetting`: the ground term counts vehicles x battalion supply | +2.8 alone, +6.1 with M1 + M2 |

M1, M2, M5 and M6 apply to every team; Blue's starved or shocked battalions react too. Orchestration: `tools/campsim/strike_spread.py` (arms, pacing, report), `make_sam_mod.py`, `sam_depth.py`.

## Everything else that was tried

| Arm | Games | Share h240 | North front supply (low) | Blue aircraft lost | Blue wins, North wins, undecided |
|---|---|---|---|---|---|
| no strike / strike | 12 / 12 | 50.8 / 52.6 | 80% / 59% | 1,710 / 1,669 | 2,1,9 / 0,0,12 |
| AI strike (sliders, Blue flies it) / in the SAM world | 11 / 10 | 48.9 / 53.7 | 80% / 74% | 1,764 / 2,049 | 0,3,8 / 3,0,7 |
| SAM world, no strike / strike | 12 / 12 | 50.5 / 55.5 | 73% / 48% | 2,021 / 1,845 | 4,3,5 / 6,0,6 |
| weak-battalion control, no strike / strike | 12 / 12 | 49.0 / 53.0 | 82% / 54% | 1,882 / 1,872 | 0,1,11 / 0,0,12 |
| M1, no strike / strike | 11 / 11 | 49.0 / 51.4 | 84% / 66% | 1,702 / 1,640 | 0,2,9 / 0,0,11 |
| M2, no strike / strike | 11 / 11 | 48.0 / 53.7 | 82% / 65% | 1,692 / 1,669 | 0,3,8 / 0,0,11 |
| M1 + M2, no strike / strike | 11 / 11 | 49.6 / 55.3 | 82% / 63% | 1,705 / 1,737 | 2,1,8 / 1,0,10 |
| M1 + M2 + M3, no strike / strike | 10 / 10 | 49.6 / 54.0 | 78% / 57% | 1,952 / 1,854 | 0,1,9 / 1,1,8 |
| stores, no strike / strike | 11 / 11 | 44.8 / 45.5 | 64% / 58% | 1,705 / 1,663 | 0,4,7 / 0,5,6 |
| tight supply + M1 (harsh) + M2, no strike / strike | 11 / 10 | 55.1 / 52.4 | 24% / 27% | 1,641 / 1,591 | 3,0,8 / 0,0,10 |
| ... in the SAM world | 10 / 10 | 54.1 / 55.2 | 30% / 25% | 1,882 / 1,731 | 1,0,9 / 2,0,8 |
| strike shock, no strike / strike | 10 / 10 | 46.6 / 52.0 | 76% / 21% | 1,684 / 1,629 | 0,0,10 / 0,0,10 |
| strike shock + M1 + M2, no strike / strike | 10 / 10 | 46.8 / 53.8 | 75% / 23% | 1,688 / 1,748 | 1,0,9 / 0,0,10 |
| initiative with supply, no strike / strike | 10 / 10 | 51.5 / 54.3 | 81% / 60% | 1,711 / 1,698 | 2,1,7 / 1,0,9 |
| initiative with supply + M1 + M2, no strike / strike | 10 / 10 | 48.0 / 54.1 | 82% / 59% | 1,723 / 1,686 | 0,3,7 / 0,0,10 |

(Blue wins, North wins, undecided.) When and what to strike (paired against the no-strike control, base world, 11-12 games each): from hour 24 -0.2 +-1.4; from 48 +1.8 +-1.1; from 96 +1.2 +-0.7; factories only -0.2 +-1.5; power / nuclear plants only +3.1 +-1.4.
The power plants matter more than the factories; an earlier or later start does not help.

## Caveats

- The strike is free to Blue by construction. The only Blue cost measured is the one the SAMs add to ordinary operations. A human flying deep would pay the deep-belt price, which the campaign AI never does.
- **Seed counts: every arm has 10-12 games, not all 12.** 12 for the stock, SAM and weak-battalion pairs; 11 for M1, M2, M1 + M2, stores and the timing / target arms; 10 for the SAM + M1 + M2, shock, initiative, tight-supply and AI arms. The last queue (a top-up to 12) was stopped at the user's request, so the brief's 12 per arm is not met everywhere. The error bars are standard errors of the paired differences.
- The extra battalions of M3 stand alone on their objectives (no brigade). The weak-battalion control exists because that alone changes the war; compare M3 with it, not with the stock arm.
- The work ran as several batches (the main pairs, second-round variants, a top-up, then M5 and M6 at the user's request), not as one round. 366 runs, four exes built from `5c670350` and the commits after it.
- `main` needed hand-resolved conflicts in `gtm.cpp` and `main.cpp` (it has no railnet) and was compile-checked after the first commit; the strike-shock and initiative commits have not been built on `main`.
- The Chinese counteroffensive, the trigger and the supply knobs are the best of the earlier spread (`noprod=1 prodsup=0.75 usemult=2`, `alliedcounter=1:8:80`, `chn_appr2`, China on its own side of the Yalu); nothing about them was changed.

## Reproduce

```
python tools/campsim/make_sam_mod.py sites,china chnb chboth      # scenarios (needs the install's Campaign\SAVE\chnb.cam, see make_ally_mod.py prc-border)
set CAMPSIM_EXE_DIR=<folder with the built campsim.exe and its dlls>
python tools/campsim/strike_spread.py plan                         # plan3..plan8 for the other arms (tight supply, stores, shock, initiative)
python tools/campsim/strike_spread.py reanalyse ; python tools/campsim/strike_spread.py report
```
