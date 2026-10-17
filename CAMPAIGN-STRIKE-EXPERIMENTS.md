# Can deep strikes on the North's economy change a 10-day Korea war? (campsim, 2026-10-17)

Goal: with a revamped supply model and stronger rear air defence, do a player's strikes on factories, power plants and refineries change the outcome of a
10-day war by more than the noise, at a visible cost to Blue? Campsim only: the game exe and the install's `save0` were not touched. Every mechanism is a switch,
off by default. No win gates, no timers. 196 ten-day runs, 26 arms, results in `tools/campsim/strike/results.jsonl`, full tables in `tools/campsim/strike/report.txt`.

## The answer

**No mechanism makes the strikes decisive.** What they do is real but small: they take the North's offence apart and leave the front where it is.

| What was added | Effect of the strike on Blue's share of objectives at day 10 (paired, same seeds) | Seeds |
|---|---|---|
| Nothing (stock engine) | +1.8 +-1.1 points | 12 |
| M1 starvation drains morale | +2.7 +-2.0 | 4 |
| M2 supply-aware fall-back | +4.1 +-3.9 | 4 |
| M1 + M2 | **+5.2 +-0.8** | 8 |
| M3 SA-10 / SA-5 on every economic site, all Chinese AD to SA-10 | +5.0 +-3.1 | 12 |
| Control for M3: the same number of extra battalions but with 100 mm guns (KS-19) | +4.0 +-1.1 | 12 |
| M1 + M2 + M3 | +4.9 +-1.3 | 8 |
| M4 stores (depots and army bases hold local stock) | +1.4 +-1.4 | 6 |
| Tight supply (use x4, production 0.5) + harsh M1 + M2 | -1.2 +-3.5 | 6 |
| ... and the SAM world | +3.9 +-1.8 | 6 |

Only M1 + M2 reaches the 5-point mark with a small error bar, and it did not move the winner or the win time: the strikes never made Blue win earlier (stock: Blue took
Pyongyang and Wonsan in 2 of 12 no-strike games, around hour 76, and in 0 of 12 strike games). The share gain is the same size with M3's SAMs (+5.0) as with the same number of
weak battalions (+4.0), so it is not the SAMs that are working on the share.

**Who wins** (games of 10 days; Blue = holds Pyongyang and Wonsan, North = Seoul or Pusan falls; the rest are undecided at day 10, which is what the brief wants):

| Arm | Games | Blue wins (mean hour) | North wins | Undecided |
|---|---|---|---|---|
| stock, no strike / strike | 12 / 12 | 2 (h76) / 0 | 1 / 0 | 9 / 12 |
| SAM world (M3), no strike / strike | 12 / 12 | 4 (h112) / 6 (h94) | 3 / 0 | 5 / 6 |
| weak-battalion control, no strike / strike | 12 / 12 | 0 / 0 | 1 / 0 | 11 / 12 |
| M1 + M2, no strike / strike | 8 / 8 | 1 (h83) / 0 | 1 / 0 | 6 / 8 |
| M1 + M2 + M3, no strike / strike | 8 / 8 | 0 / 1 (h92) | 1 / 1 | 7 / 6 |
| stores (M4), no strike / strike | 6 / 6 | 0 / 0 | 4 / 3 | 2 / 3 |
| tight supply in the SAM world, no strike / strike | 6 / 6 | 0 / 2 (h105) | 0 / 0 | 6 / 4 |

Over the nine pairs of arms (stores left out), the strikes remove the North's wins (9 of 72 games without a strike, 1 of 72 with; Blue's wins are 8 and 9) because they take its offence away. In the SAM world they also come with more Blue wins (6 of 12 against 0 of 12 in both the
weak-battalion and the stock strike games), but those wins come at day 4 (hour 94), which is the early ending the brief does not want, and the SAM world is more decisive in general (4 Blue and 3 North wins in 12 games
without a strike, against 2 and 1 in the stock war and 0 and 1 with the weak battalions). Why the SAMs make the war more decisive is not explained.

The strikes themselves are free to Blue here (the targets are simply held destroyed), so the only price Blue pays in these numbers comes from the SAMs existing: **+311 aircraft lost per game (+18%), +429 to ground
fire (+34%)**, almost all of it near the front (see below).

Recommendation: do not rely on supply or air defence tuning to make strikes decisive. The measured limit is on the other side of the war: **Blue's ground offensive does
not respond to the enemy being weak** (Blue is on full offensive in 18-20% of frames whether the North is fed or starved; even with the front at 26% supply the share is 53.9%).
Look at how Blue's ground AI picks its action (posture, action points, capture orders; about 10 battalions on capture at any time) before spending more on the North's side.
If a package is wanted anyway, take M1 + M2 (cheap, costs nobody anything, +5 points and the North's wins gone) and treat M3 as an option for a bloodier air war: it costs Blue 300 aircraft a game and, with strikes, gives
early Blue wins that the 10-day goal does not want.

## Where the North's air defence sits, and what Blue loses to it, by depth

Static (`sam_depth.py chnb.cam`): 186 DPRK air-defence battalions plus 12 Chinese and 4 Russian. DPRK: 103 KS-19 (100 mm guns), 47 SA-2, 22 S-60, 9 SA-3, 3 SA-5, 2 SA-4; **no SA-10** (6 in China, 2 in Russia's team).
By depth north of the starting front (y 490, 50 km bands, DPRK + PRC): 13 behind the line, 20 / 43 / 25 / 34 / 27 / 14 / 8 / 6 / 8 in the bands going north. 72 of the 74 factories, power and
nuclear plants, chemical plants and refineries have an AD battalion within 25 km (most of them KS-19 or SA-2).

Dynamic (new `AIRLOSSDEPTH` line, aircraft Blue lost to ground fire, by the shooter's position, mean per game, stock war, 12 games):

| Shooter's y | south of 500 | 500-549 | 550-599 | 600-649 | 650-699 | 700+ |
|---|---|---|---|---|---|---|
| Blue aircraft lost (any ground shooter / air-defence battalions) | 274 / 72 | 273 / 129 | 459 / 315 | 61 / 55 | 76 / 75 | 42 / 41 |

85% of what Blue loses to ground fire is lost within 110 km of the starting line (and half of that to something other than an AD battalion); the deep belt (y 600 and north) costs about 180 aircraft a game, all of it AD.
**Blue's AI almost never flies deep**: the AI-strike arm (sliders for factories, power plants, depots and roads at 100) lost the same ~175 aircraft deep as the control. Adding an SA-10 and an SA-5 on every economic site
raised the near-front losses by about 400 a game and the deep ones by about 65: those sites sit between y 465 and 550, in the band Blue's close support flies. So deep SAMs would cost a player who flies deep a lot, but the
campaign AI cannot show that, and the harness cannot test a player.

## What the strikes do, and why that is not enough

Strike = factories, power / nuclear plants and refineries of the North held destroyed from hour 48 (`blow=...:6:hold blowat=48`). Stock engine, 12 seeds each:

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
happen: with the strike the share holds at 52-53%. The defence does not weaken: the garrisons keep their valid DEFEND orders, and the attacker stays at its 10 capture battalions. The North also loses *fewer* vehicles
(an attacker loses more than a defender), which is why the strike leaves it with 550 more vehicles at day 10.

Why a starved North froze the front (item 1 of the brief), found earlier and confirmed here: supply never feeds morale; about 75% of the North's battalions are on RESERVE orders and never touch the front; the garrison check
that sends a battalion under half supply to reserve comes *after* the rule that keeps a valid order, so a starved garrison keeps holding.

## What was built (all campsim-only knobs, off by default)

| Switch | What it does | Result |
|---|---|---|
| `starvemorale=G[:BREAK]` (M1) | `RallyUnit`: a battalion under half supply loses G morale points an hour at 0 supply (in proportion below half) instead of regaining it, and breaks under BREAK; broken = WITHDRAW tactic, double speed, reserve only. G=12 break 30 (harsh: 30 / 45) | Mean North morale falls from 88 to 75 (72 with M3), 60-80 battalions under 40; Blue +2.7 alone |
| `supplyfall=1` (M2) | `AddToLists` (`gtm.cpp`) + `FindSupplyFallback` (`gndunit.cpp`): a battalion under half supply that is not attacking is sent to the nearest city, port, depot or army base behind the third line and stays until it is back over 80% | 200 moves a game in the control, 780 with strikes; Blue +4.1 alone |
| `blowat=HOUR` | the `blow=` rules (factory / power / refinery, `hold`) start at that hour | the delayed-strike harness |
| M3 `make_sam_mod.py` | `sites`: an SA-10 (class 799) and an SA-5 (733) battalion on each of the 74 economic sites; `china`: all Chinese AD becomes SA-10 plus one more with every 4th Chinese battalion; `dummy`: two KS-19 on each site as the control. Scenarios `chboth`, `chsite`, `chchn`, `chsa5`, `chdum` in the install's `Campaign\SAVE` (test files, not shipped) | see the table at the top; a control of the same number of weak battalions gives the same share gain |
| `AIRLOSSDEPTH` | aircraft lost to ground fire by shooter band, every 6 hours | the depth table above |
| M4 | the existing store model (`stocks=1 storecap=25 storestart=50`); the search that refuses enemy-owned links was *not* changed | the strike gains +1.4 +-1.4, and the model on its own is bad for Blue: share 43.7% against 50.8%, the North wins (Seoul or Pusan falls) in 4 of 6 games against 1 of 12 |

M1 and M2 apply to every team; Blue's starved battalions fall back too (they rarely starve, so this changes little). The strike harness and orchestration: `tools/campsim/strike_spread.py` (arms, pacing, report),
`make_sam_mod.py`, `sam_depth.py`.

## Everything else that was tried (stock M1/M2/M3 unless noted)

| Arm | Share h240 | North front supply (low) | Blue aircraft lost | Seeds |
|---|---|---|---|---|
| no strike | 50.8 +-1.0 | 80% | 1,710 | 12 |
| strike | 52.6 +-0.9 | 59% | 1,669 | 12 |
| AI strike (sliders, Blue flies it) | 48.2 +-1.5 | 80% | 1,738 | 8 |
| AI strike in the SAM world | 52.1 +-2.4 | 75% | 2,026 | 8 |
| SAM world, no strike / strike | 50.5 +-1.9 / 55.5 +-2.4 | 73% / 48% | 2,021 / 1,845 | 12 |
| weak-battalion control, no strike / strike | 49.0 +-1.2 / 53.0 +-0.9 | 82% / 54% | 1,882 / 1,872 | 12 |
| M1 + M2, no strike / strike | 49.0 +-0.9 / 54.2 +-0.7 | 82% / 66% | 1,673 / 1,711 | 8 |
| M1 + M2 + M3, no strike / strike | 49.5 +-1.1 / 54.4 +-1.5 | 78% / 58% | 1,951 / 1,854 | 8 |
| stores, no strike / strike | 43.7 +-1.6 / 45.2 +-0.9 | 65% / 59% | 1,663 / 1,625 | 6 |
| tight supply + M1 (harsh) + M2, no strike / strike | 53.9 +-3.4 / 52.7 +-0.2 | 26% / 27% | 1,617 / 1,597 | 6 |
| ... in the SAM world | 53.3 +-1.1 / 57.1 +-1.9 | 33% / 21% | 1,904 / 1,750 | 6 |

When and what to strike (paired against the no-strike control, base world, 6 seeds unless noted): from hour 24 -1.6 +-1.7; from 48 +1.8 +-1.1 (12); from 96 +1.3 +-0.9; factories only -1.2 +-2.6;
power / nuclear plants only +2.3 +-2.0. Nothing separates them: the power plants matter a little more than the factories, and an earlier or later start does not help.

## Caveats

- The strike is free to Blue by construction. The only Blue cost measured is the one the SAMs add to ordinary operations. A human flying deep would pay the deep-belt price, which the campaign AI never does.
- The extra battalions of M3 stand alone on their objectives (no brigade). The weak-battalion control exists because that alone changes the war; compare M3 with it, not with the stock arm.
- The error bars are standard errors of the paired differences; 4 seeds (M1, M2 alone) are weak evidence, 12 are good for about +-1 point.
- 196 runs, one exe (`scratchpad/csbM`, built from `5c670350`). Both branches carry the change (`rail-tracks` 5c670350, `main` 7985bb4f); `main` needed hand-resolved conflicts in `gtm.cpp` and `main.cpp` (it has no railnet) and was compile-checked afterwards: campsim builds; it was not run.
- The Chinese counteroffensive, the trigger and the supply knobs are the best of the earlier spread (`noprod=1 prodsup=0.75 usemult=2`, `alliedcounter=1:8:80`, `chn_appr2`, China on its own side of the Yalu); nothing about them was changed.

## Reproduce

```
python tools/campsim/make_sam_mod.py sites,china chnb chboth      # scenarios (needs the install's Campaign\SAVE\chnb.cam, see make_ally_mod.py prc-border)
set CAMPSIM_EXE_DIR=<folder with the built campsim.exe and its dlls>
python tools/campsim/strike_spread.py plan                         # plan3 / plan4 for the tight-supply and the stores, timing, target arms
python tools/campsim/strike_spread.py reanalyse ; python tools/campsim/strike_spread.py report
```
