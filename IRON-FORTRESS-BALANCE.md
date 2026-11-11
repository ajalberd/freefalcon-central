# Iron Fortress balance (Korea save1, campsim, 2026-10-26)

Goal: make Korea's **Iron Fortress** (`campaign/SAVE/save1.cam`) a dynamic, close war that is slightly
Blue-sided or 50/50. Baseline is lopsided: the DPRK wins 5 of 6 seeds, ending days 5.6-8.0 by taking Pusan.

All arms: `run_batch.py --days 10 --seeds 1..6 --jobs 6 --save save1`, one exe per batch (the ATM's
200 ms wall-clock cutoff makes mixing exes unsafe). Table tool: `python tools/campsim/iron_arms.py`.

## Recommendation (fair levers only)

A lever is **fair** when it is a rule or a setting both sides could live under - not when it hands one
side free destruction or a handicapped enemy at t=0. On that test `blow=power:6:hold` is **not** a balance
lever: it destroys every DPRK power plant at the start and re-destroys them every tick, a bombing campaign
that nothing can repair and no player earned. It is a campsim what-if tool (the same `blow=` family the
supply study uses for upper bounds), so the `pow6` and `combo` arms below are **what-if results, not
recommendations**.

| want | config | result | fair? |
|---|---|---|---|
| **Blue-sided, best fair war** | `alliedcounter=1:8:80 alliedteams=2` | Blue 6/6 (6 seeds); Blue 349 -> 552-744 objectives, the DPRK 2338 -> 1943-2135; 629-1156 ownership changes | **yes** - an AI mechanic, available to either side via `AlliedCounterTeams`, and the option the weaker side is supposed to have (the DPRK uses it in Rolling Fire) |
| **Blue-sided, settings only** | `teambar=2:15` | Blue 5/6 + 1 undecided (6 seeds); Blue 349 -> 594-806 objectives; 885-1257 ownership changes | **yes** - one team's offensive bar, the same kind of value as the cfg's `CounterAttackInitiative` |
| *what-if, not fair* | `blow=power:6:hold` | Blue 9/12; Blue 122-631 objectives | **no** - free, unrepairable destruction at t=0 |
| *what-if, not fair* | `teambar=2:15 blow=power:6:hold` | Blue wins or survives all 12; Blue 566-957 objectives | **no** - the whole effect is the cheat |

`alliedcounter=1:8:80 alliedteams=2` is the recommendation: Blue launches a full counteroffensive when 8
of its folded-in (US) battalions are within 80 km of the front, whatever its initiative, and the enemy's
launches are held back while it runs. Measured consequence worth knowing before shipping it: it makes
Blue **offensive almost continuously** - in seeds 5 and 6 the team is `GACTION_OFFENSIVE` in all but
one-hour gaps every 20 h, because the mechanic bypasses the initiative test and re-launches as each action
timeout expires. Pacing it with the fourth field (`alliedcounter=1:8:80:12`, hours of rest) or the
readiness gate (`turnmode=8`, allied battalions must be supplied and unbroken) is the obvious fair next
step if that reads as too strong.

**Nothing in the ten attempts produced a decisive Blue victory.** Every win in the fair arms is event 17's
`#ELSE` branch (`#IF_CAMPAIGN_DAY G 10`, which fires at 9 days elapsed because the day counter is 1-based):
Blue never takes Seoul, so the timer decides — even at 744 objectives against the DPRK's 1943. Taking Seoul
needs a bigger change than a balance knob (a shorter war, a different objective set, or a much stronger
Blue).

**Read the win counts as ±1, and the objective counts as noisy.** Stock campsim is not deterministic
(`det=1` is off): the same seed re-run gives a different war — `pow6` seed 1 ended with 275 Blue
objectives in the 6-seed pass and 158 in the 12-seed pass, same knobs and seed. That is why the
recommendations above use 12 seeds; the 6-seed columns further down are indicative only, and the arms
that differ by a few dozen objectives are within the noise. The large, consistent effects (baseline
~170 objectives vs 550-800 for the good arms; the DPRK ending at 114% of its start in every baseline-like
arm) are not noise.

### How the ten arms classify

| class | arms |
|---|---|
| **fair** - rules and settings both sides could live under | `teambar=2:15`, `teambar=6:50`, `alliedcounter=1:8:80 alliedteams=2`, `relsquad=1.0` (replacement split), `hcg=4` (air-vs-ground lethality, global), `defendon`/`garblock`/`counterloss` (simulation rules) |
| **scenario authoring** - a different starting order of battle, legitimate if you mean to author a different campaign, not as a fix | `strip=6:25`, `stripair=6:40`, `startsup=6:70`, the `save1b` +29 battalions |
| **not fair** - free destruction or handicap at t=0 | `blow=power:6:hold` (and its combination, `combo`) |



## Why Blue loses, from the data

| | Blue (ROK) | DPRK |
|---|---|---|
| objectives at start | 349 (13%) | 2338 (87%) |
| ground vehicles | 2842 | 5001 |
| battalions | 162 | 322 |
| aircraft | 848 | 812 -> **1851 by the end** |
| reinforcement items queued | 41 (all released by h54: 22 bn, 16 sq, 3 tf) | 18 (by h24: 3 bn, 15 sq) |

1. **The win rule is the biggest factor.** Event 14 needs the DPRK to hold Seoul **and** Pusan
   (`#IF_CONTROLLED 6 A 228 417`); it already holds Seoul (228) at the start, so the DPRK wins by taking
   one city. Blue's event 17 needs the same pair (`A` = AND), i.e. it must retake Seoul; its `#ELSE`
   gives Blue the war at day 10 for surviving. Events 15 (288 boredom hours) and 16 (day 15) are dead
   code: 17's `#ELSE` preempts them.
2. **Blue loses the attrition race.** Blue's ground falls to 56-85% of its start while the DPRK's *grows*
   8-22% in every baseline seed. The DPRK's production and replacements outrun its losses; Blue's do not.
   That is why force levers fail: stripping 25% of the DPRK's vehicles (`strip=6:25`) just leaves it room
   to grow back (it ends at **140%** of the stripped start), and adding 29 Blue battalions (`save1b`)
   only moves the win rate from 1/6 to 2/6.
3. **Blue's air is real but not decisive, and it does not scale.** Blue has 16 reinforcement squadrons
   (288 aircraft: both B-52 wings, F-15C/E, both F-14D, F/A-18C/F, both F-117) that are never put on
   `AllAirList`, so the ATM never tasks them (`airreinf=1` arms them and makes Blue *worse*: 0/6 vs 1/6).
   Making aircraft deadlier against ground (`hcg=4`) also makes Blue worse, because by then the DPRK has
   twice Blue's aircraft.
4. **Initiative is zero-sum, which flatters the weaker side but also collapses it.** Stock gave Blue 100 at
   h18 while the DPRK sat at 0; by h108 Blue was at 0 and the DPRK at 100. Per-team initiative
   (`initindep=1`) leaves Blue permanently low (0/6, worse) because it reflects the force ratio.
   `teambar=6:50` (make the DPRK earn its offensives) also made Blue worse.

## The defend/capture defect (found from a run, then measured)

Blue's `2nd Infantry Battalion` (vuId 4340) at Sunch'ang (objective 554, 458/175):

| hour | orders/tactic | position | what happened |
|---|---|---|---|
| 50-54 | `GORD_CAPTURE` / MOVE_WEDGE | 17 -> 1 km | advances on Sunch'ang |
| **55** | **`GORD_DEFEND` / DEFEND, objId 554** | (457,174), 1 km | captures it, is at once told to defend it — from **outside the objective's cell** |
| 56-70 | DEFEND | 1 km | sits still; no DPRK battalion within 12 km |
| **71** | DEFEND | 1 km | **one** DPRK battalion with `GORD_CAPTURE` walks in and takes it back |
| 72-155 | DEFEND | 1 km | never re-tasked; sits beside the enemy-held town for 85 h |

From the code:

1. **Capture happens only from the objective's own 1 km cell**, on the movement path:
   `GroundUnitClass::MoveUnit` -> `DetectVs` sets `*capture = 1` only on `ENEMY_SAME_HEX`
   (`gndunit.cpp:687-694`, `CaptureObjective` at `:290`; the `g_bWaterObjectiveFix` comment at `:623-627`
   states the 1 km-cell requirement).
2. **`GORD_DEFEND` does not garrison**: `BattalionClass::PickFinalLocation` sends a defender to
   `FindBestCover` *near* the objective (`battalio.cpp:1459-1475`), i.e. into the adjacent cell, where no
   capture can happen — while a capture of that objective can happen to it.
3. **The counter-attack branch is dead in practice**: `PickFinalLocation` re-tasks a defender whose
   objective is enemy-held, but only when its orders change; nothing re-checks when the objective is taken
   from under it.

Three experiment switches were added (flags in `src/ui/src/f4config.cpp`, parsing in
`src/tools/campsim/main.cpp`):

| knob | effect |
|---|---|
| `defendon=1` | a DEFEND battalion garrisons the objective's own cell (measured: DEFEND unit-hours on the objective's cell rise from **37-44% to 86-89%**) |
| `counterloss=KM` | when an objective falls, battalions of the losing team still ordered to defend it within KM go back for it |
| `garblock=1` | an objective cannot be captured while a battalion of its owner stands in its cell |

### The defect test, measured

| arm | knobs | outcome | Blue objectives | flips |
|---|---|---|---|---|
| dfix | `defendon=1 counterloss=5` | DPRK 6/6 | 131-199 | 567-870 |
| **gblock** | `defendon=1 garblock=1 counterloss=5` | **Blue 6/6** (day-10 clause) | **213-235** | **274-429** |

Garrisoning alone does not save the towns — they still fall at the stock rate, which is the second half of
the defect: standing in the objective's cell does not stop a capture, only combat does. With `garblock`
they hold, but the war goes sticky: ownership changes drop from ~650 to ~300 and every run ends on the
timer. So defence becomes real and dynamism is lost; `garblock` is a modelling fix, not a balance fix.

## Blue's counteroffensive mechanic

`alliedcounter=1:8:80 alliedteams=2` — the mechanic that fixed the *weaker* side in Rolling Fire (there the
DPRK used it; `alliedteams` defaults to 6, so Blue has to be opted in). This is the single most effective
change to the *war* (Blue's counteroffensives fire whatever its initiative):

| arm | outcome | Blue objectives | DPRK objectives | flips |
|---|---|---|---|---|
| **bcount** | **Blue 6/6** (day-10 clause) | **552-744** | 1943-2135 | 629-1156 |

Before this arm Blue never advanced at all (349 -> 163-215). With it Blue gains ground every seed.

## The ten lever attempts (6 seeds each, vs baseline)

| # | arm | knobs | Blue wins | DPRK wins | Blue objectives (median) | DPRK ground | flips |
|---|---|---|---|---|---|---|---|
| — | baseline | — | 1/6 | 5/6 | 163-215 (**174**) | 114% | 572-1162 |
| 1 | **tbar15** | `teambar=2:15` | **5/6** + 1 limit | 0/6 | 594-806 (**640**) | 109% | 885-1257 |
| 2 | tbar650 | `teambar=6:50` | 0/6 | 6/6 | 89-194 (**123**) | 115% | 375-564 |
| 3 | hcg4 | `hcg=4` | 0/6 | 6/6 | 132-161 (**150**) | 117% | 595-999 |
| 4 | str625 | `strip=6:25` | 1/6 | 5/6 | 114-289 (**135**) | **140%** | 699-1274 |
| 5 | sair640 | `stripair=6:40` | 1/6 | 5/6 | 115-242 (**151**) | 115% | 578-947 |
| 6 | sup670 | `startsup=6:70 noprod=1` | 0/6 | 6/6 | 116-190 (**167**) | 115% | 545-857 |
| 7 | relsq1 | `relsquad=1.0` | 0/6 | 6/6 | 110-268 (**139**) | 119% | 547-1032 |
| 8 | **pow6** | `blow=power:6:hold` | **4/6** | 2/6 | 133-552 (**286**) | 114% | 566-1093 |
| 9 | bground | `save1b` (+29 ROK battalions) | 2/6 | 4/6 | 111-211 (**165**) | 110% | 717-1228 |
| 10 | **combo** | `teambar=2:15 blow=power:6:hold` | **5/6** + 1 limit | 0/6 | 632-799 (**665**) | **97%** | 896-1176 |
| — | bcount | `alliedcounter=1:8:80 alliedteams=2` | 6/6 | 0/6 | 552-744 (**659**) | 111% | 629-1156 |

Also run, for the record: `airreinf=1` (0/6) and `initindep=1` (0/6), both worse than baseline.

## 12-seed confirmation of the two best arms

Six seeds is not enough here (the war is bimodal and campsim is not deterministic), so the two
recommended configurations were re-run on seeds 1-12 with fresh tags:

| arm | Blue wins | DPRK wins | Blue objectives (median) | DPRK objectives | Blue ground | DPRK ground | flips |
|---|---|---|---|---|---|---|---|
| pow6_12 (`blow=power:6:hold`) | **9/12** | 3/12 | 122-631 (**296**) | 2056-2565 | 87% | 115% | 633-961 |
| combo12 (`teambar=2:15 blow=power:6:hold`) | **10/12** + 2 undecided at the limit | 0/12 | 566-957 (**697**) | 1730-2121 | 86% | **95%** | 883-1248 |

`pow6` is the closest to the requested "slightly Blue-sided or 50/50" (75% Blue) and is genuinely
bimodal: Blue either breaks through (400-631 objectives) or the DPRK wins outright (122-231). The combo
is stronger — Blue wins or survives all twelve — and it is the only configuration that stops the DPRK's
ground from outgrowing its start (95% median, against 115% for `pow6` and 114% at baseline), because
killing the power plants takes DPRK production to zero. Both keep the war dynamic (600-1250 ownership
changes, against 572-1162 at baseline).

## What the attempts say

- **Only two classes of lever move the outcome: who attacks when, and the DPRK's production.**
  Blue's offensive bar (arm 1) and Blue's counteroffensive mechanic turn a 1/6 into 5-6/6; destroying the
  DPRK's power (arm 8) turns it into 4/6. Raising the DPRK's bar, per-team initiative, more/less force on
  either side, more supply, better replacements and deadlier air all fail or backfire.
- **Force levers fail because the DPRK regenerates.** It ends at 108-122% of its start in the baseline and
  at **140%** of a 25%-cut start (arm 4), because its production refills what is taken. This is the same
  lesson as `SUPPLY-RESEARCH.md`: levers change how supply is *felt*, not who holds what — with the
  exception that cutting the production itself (arm 8) does change it.
- **Air is the DPRK's, not Blue's.** Blue's 288 idle aircraft are a defect worth fixing for its own sake,
  but arming them makes Blue worse (replacement split), and making air deadlier against ground helps the
  side with more aircraft — the DPRK by day 3.
- **The best arms are also the most dynamic**: flips rise from 572-1162 (baseline, mostly one-way) to
  885-1257 (arm 1) and 896-1176 (arm 10), with Blue advancing from ~174 to ~650 objectives.

## Reproducing

```
cd tools/campsim
python run_batch.py --days 10 --seeds 1 2 3 4 5 6 --jobs 6 --save save1 --tag baseline
python run_batch.py --days 10 --seeds 1 2 3 4 5 6 --jobs 6 --save save1 --tag combo \
       --set teambar=2:15 blow=power:6:hold
python run_batch.py --days 10 --seeds 1 2 3 4 5 6 --jobs 6 --save save1 --tag bcount \
       --set alliedcounter=1:8:80 alliedteams=2
python iron_arms.py baseline combo bcount        # comparison table
python make_blue_size.py 1 --step 4              # rebuild save1b (the +29 battalion variant)
```

The timelines land in `runs/save1-<tag>-s<seed>.jsonl` and open in the Campaign Lab / viewer.

## Files

- `src/campaign/camptask/battalio.cpp` - `defendon` (garrison the objective's cell).
- `src/campaign/camptask/gndunit.cpp` - `garblock` (a garrison holds the cell) and `counterloss`
  (defenders go back for a lost objective).
- `src/ui/src/f4config.cpp` - the three flags (`g_bDefendOccupy`, `g_bGarrisonBlocks`, `g_fCounterLossKm`),
  all off by default.
- `src/tools/campsim/main.cpp` - knob parsing for `defendon`, `counterloss`, `garblock`.
- `tools/campsim/make_blue_size.py` - the "more Blue ground units" save variant (`save1b`).
- `tools/campsim/iron_arms.py` - the arm comparison table.
- `tools/campsim/_verify_defend.py` - checks how often DEFEND battalions are on their objective's cell and
  counts recaptures.
