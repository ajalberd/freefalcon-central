# The three Taiwan campaigns (FF6 Taiwan 3)

What each of the theater's campaigns is, the forces both sides start with, the
victory conditions as shipped and as now, and what the campaign AI does with
them — measured in `campsim` (see `tools/campsim/README.md`), 10 simulated days,
seeds 1-6, `--set usecfg=1` (the install's `FFViper.cfg`, so `NoPlayerPlay=400`:
without it the player's team is docked initiative every hour and the enemy wins
in 2-5 days, see `CAMPAIGN-AI-FINDINGS.md` "harness traps").

The theater itself was installed 2026-10-25 (`THEATER-INSTALLS.md`), including
the maneuver-table repair without which its AI has no dogfight decisions at all.

## What the three saves are

The package is a conversion of the Korea campaign slots: every save's header
still says `UIName "Red Herring"`, and the `.tri` comments still talk about
Seoul and Pyongyang. The theater's `art/main/lcktxtrc.irc` renames the three
Korean campaigns to its own (`TXT_ROLLING_FIRE`, `TXT_IRON_FORTRESS`,
`TXT_TIGER_SPIRIT`), and `theater.lst`-driven tools show:

| save | campaign (in-game) | TDF blurb order | posture |
|---|---|---|---|
| `save0` | **Kittie's last stand** | 1st ("Kitties Last Stand") | Blue on the offensive (`#SET_EVENT 9`), tempo 150 |
| `save1` | **EYE OF THE SERPERNT** (sic) | 3rd ("Eye of the Serpent") | Blue on the offensive |
| `save2` | **Raging Dragon** | 2nd | Blue on the defensive (`#SET_EVENT 10`), bullseye (600, 400) |

Sides: **Blue = Taiwan (2) + Japan (3)**, **Red = China (6)**. The U.S. (1),
CIS (4) and PRC (5) teams exist in every save and hold nothing at all; Japan has
no forces in `save2`. Map 1024 × 1024 km, 1227-1228 objectives. Campaign clock
starts 09:00 in `save0`/`save1` and ~05:00 in `save2`, day 0.

## Forces at the start

| campaign | side | objectives | ground veh | aircraft | ships | bn / bde / tf | supply |
|---|---|---|---|---|---|---|---|
| **save0** Kittie's last stand | Taiwan | 406 | 6,034 | 729 | 51 | 176 / 15 / 16 | 100% |
| | Japan | 30 | 112 | 53 | 36 | 13 / 0 / 10 | 100% |
| | China | 791 | 6,294 | 1,401 | 238 | 202 / 16 / 70 | **80%** |
| **save1** Eye of the Serpent | Taiwan | 408 | 6,062 | 590 | 19 | 167 / 14 / 9 | 100% |
| | Japan | 28 | 112 | 47 | 17 | 12 / 0 / 5 | 100% |
| | China | 792 | 4,701 | 951 | 84 | 219 / 4 / 27 | 100% |
| **save2** Raging Dragon | Taiwan | 435 | 6,298 | 884 | 53 | 220 / 15 / 14 | **80%** |
| | China | 792 | 7,244 | 1,485 | 240 | 358 / 15 / 70 | 100% |

(Taiwan's westernmost objectives are Chin-Men / Chin-Men Tao / Yang-Chai — the
offshore islands off Xiamen; Japan's are the Ryukyus, Yonaguni to Hirara; China
holds the mainland.)

So: `save0` sends a Blue that is outnumbered 1:1.8 in the air into a China at
80% supply; `save1` gives Blue the ground advantage (1.3:1) but only 0.67:1 in
the air; `save2` is China's war — 358 battalions and 1.7:1 in the air against a
defending Blue at 80% supply.

## Victory conditions

### As shipped

`save0` and `save1` shipped the **same** script (byte-identical, Korea's):

| code | condition as shipped | reachable? |
|---|---|---|
| 14 | China holds Tai-Pei (1246) and Sungshan Intl (1212) | yes |
| 16 | `#IF_CAMPAIGN_DAY G 10` | yes — fires at 240 h |
| 17 | **team 1 (U.S.)** holds San-Chih (1239), Ching-Shui (1274), Shui-Ti-Liao (1503) | **no** — the U.S. owns no units and no objectives, and the three named towns are Taiwan's own |

`save2`:

| code | condition as shipped | reachable? |
|---|---|---|
| 14 | China holds Tai-Pei, Tai-Pei HQ (61), Taichung (1281), Kao-Hsiung (1478), Shanwei (111) | yes (Shanwei starts Chinese) |
| 15 | `#IF_BORDOM_HOURS 120` — nothing happens for 5 days | yes |
| 16 | `#IF_CAMPAIGN_DAY G 7` | yes |
| 17 | Taiwan holds the four nodes above **and Shanwei** | **no** — Shanwei is a mainland objective, Chinese from the start, and Taiwan's ground units cannot get there |

Net effect: `save0`/`save1` had no Blue win at all, so every run ended as a
nameless timeout — which is exactly what 12 runs did (6 seeds each, all
`endgame=16` at 231 campaign hours). `save2` could only be lost or drawn.

### Now (2026-10-26)

Each campaign has a win, a loss and a draw, all reachable. Listed in **file
order**, which matters: the script posts a campaign-over for every matching
block and the last one wins, so the draw is written before the wins:

| campaign | order | code | condition |
|---|---|---|---|
| save0 | 1 | 14 | China holds Tai-Pei + Sungshan Intl (any time) |
| | 2 | 16 | at day 10, otherwise (draw) |
| | 3 | **17** | **at day 10: Taiwan still holds Tai-Pei + Sungshan Intl** |
| save1 | 1 | 14 | China holds Tai-Pei + Sungshan Intl |
| | 2 | 16 | at day 10, otherwise (one island each: draw) |
| | 3 | **17** | **at day 10: Taiwan holds the capital and its airport AND has retaken Lianhe (868) and Shijing (865)** — the counteroffensive the campaign is named for |
| | 4 | 14 | **at day 10: China still holds Lianhe + Shijing — the day-one grab stands** |
| save2 | 1 | 14 | China holds Tai-Pei, HQ, Taichung, Kao-Hsiung |
| | 2 | 15 | `#IF_BORDOM_HOURS 120` |
| | 3 | 16 | at day 7, otherwise (draw) |
| | 4 | **17** | **at day 7: Taiwan still holds those four** |

Originals kept as `save?.tri.bak-orig` in `C:\FreeFalcon6\Theaters\Taiwan\campaign`.
The scripts are re-read on every trigger check (never edit one during a run).
Validated with `tools/campaign-editor/ffcamp/triggers.py`.

## The engine fix this needed

`save2` crashed every run at 23.9 campaign hours:

```
CRASH at campaign min 1432 (day 0): code 0xC0000094 at 000000014014550D
  AirTaskingManagerClass::FindBestAirFlight  atm.cpp:2199
  AirTaskingManagerClass::BuildDivert       atm.cpp:1161
  AirTaskingManagerClass::ProcessRequest    atm.cpp:1363
```

`score -= max(5, mis->match_strength / aa)` divides by
`aa = GetUnitScore(cf, Air)`, which is 0 for a flight with no air score left
(shot out, or a zero-strength element still in the list). Such a flight is not a
divert target at all, so it is skipped:

```c
aa = GetUnitScore(cf, Air);
if (aa <= 0) { ALERT_WHY(7); continue; }
```

(Scoring it as "maximally outmatched" instead looked reasonable and let
`BuildDivert` hand a mission to a flight with no aircraft left — the ATM then
livelocked. Skip is the correct shape.) Rebuilt campsim; a 2-day `save2` run now
passes the old crash point.

## The two freezes this needed

With the divide-by-zero fixed, `save2` then stopped dead, 100 % CPU, campaign
clock frozen, at some hour between 4 and 33 — sometimes in a 6-way batch,
sometimes alone:

**1. `BrigadeClass::DisposeChildren` (campaign/camptask/brigade.cpp:914).** It
loops `while (element[0])` killing children, and relied on each `KillUnit()` to
take itself off the list. That only happens when the child's parent pointer
resolves; a child that cannot detach left `element[0]` untouched and the loop
never ended (stack: `DisposeChildren → KillUnit → VuHashTable::Find`). Taiwan
save2 carries 45 units whose parent id is never seen in the campaign; Korea's
saves carry none (save0/save1 too). Fixed by forcing progress:

```c
if (element[0] == id) RemoveChild(id);   // KillUnit() did not detach it
```

**2. The brigade-column deadlock (`BattalionClass::MoveUnit`,
campaign/camptask/battalio.cpp:862).** A battalion in `GTACTIC_MOVE_BRIGADE_COLUMN`
parks when it is closer to the *previous element's next waypoint* than that
element is — it waits for the unit ahead to pass. With the column broken up, the
"unit ahead" can be tens of km behind with different orders, and the wait never
ends: 796th Armored (bn 4762) sat 33 km short of Chiayi from day 2 to the end of
the war, 116 km travelled and 0 km net. Now the wait only applies while the
previous element is within 8 km:

```c
const float columnAhead = 64.0F;    // (8 km)^2, grid cells
if (DistSqu(x, y, px, py) < columnAhead && ( ...old wait test... ))
```

Korea never enters that branch — its `column wait` counter is 0 for both teams at
every 6-hour snapshot of a whole Rolling Fire war, in both builds — so the change
is inert there by construction. (Do not validate this with A/B outcomes: the
same seed and exe give 6.1 and 7.9 days.)

## save0 rebuilt around its bridgehead (2026-10-26, later)

save0 shipped the same script as save1 and, more to the point, it ships 60 Chinese ground
units already ashore 2 km from Kuan-Yin (1245) and 9 km from Sinfong (400) -- 40 km west of
Tai-Pei -- while China owned no island objective at all. The landing could never hold
anything and no ending mentioned it, so the campaign's own content was not in its victory
conditions. Fixed by giving the bridgehead to China (objective `owner` 2 -> 6 on 1245 and
400, backup `save0.cam.bak-lodgement`) and writing the endings around it:

| order | code | condition |
|---|---|---|
| 1 | 14 | China holds Tai-Pei + Sungshan Intl (any time) |
| 2 | 16 | at day 10, otherwise (draw) |
| 3 | **17** | **at day 10: Taiwan holds Tai-Pei + Sungshan + Kuan-Yin + Sinfong** |
| 4 | 14 | **at day 10: China still holds Kuan-Yin + Sinfong** (the landing stands) |

The airport now changes hands repeatedly (seed 1: Sungshan China h3 -> Taiwan h4 -> China h10
-> Taiwan h35; Kuan-Yin retaken h26, Sinfong h27), Taiwan's ground strength falls to
3,030-4,027 by day 10, and the war is a fight rather than a churn. Campaign titles were still
Korea's `UIName "Red Herring"` in all three saves and are now **Kittie's Last Stand**,
**Eye of the Serpent** and **Raging Dragon**.

## Results with the new endings

6 seeds each, 10 days, `usecfg=1`, 6 jobs:

| campaign | result | ended | objectives Taiwan | objectives China | ground Taiwan | ground China |
|---|---|---|---|---|---|---|
| **save0** Kittie's Last Stand | **17 Taiwan, 6/6** | 9.6 d | 405→369-406 | 792→791-828 | 6,034→3,030-4,027 | 6,294→2,257-2,787 |
| **save1** Eye of the Serpent | **14 China, 6/6** | 9.6 d | 408→406 | 792→794 | 6,062→5,242-5,449 | 4,701→4,116-4,214 |
| **save2** Raging Dragon | 4 × 16 draw, 2 × 15 stalemate | 6.8 d / 5.6-6.1 d | 435→286-355 | 792→872-941 | 6,298→3,825-4,596 | 7,244→4,801-5,859 |

- **save0** is Blue's, but no longer for free: China's landing force has to be reduced every
  time, and the capital's airport changes hands on the way. Taiwan's army loses 33-50 % of its
  vehicles doing it.
- **save1** is China's by default: it takes Lianhe and Shijing on day one and keeps them
  (408→406, 792→794 -- the only movements in the war), and none of its 219 battalions ever
  crosses to the island. Its new red block awards it the campaign for keeping the grab.
- **save2** never decides: China grinds Taiwan down to 286-355 objectives but never holds one
  of the four nodes at the deadline (draw) or the quiet-hours clock fires first (stalemate).
  It lands 82 battalions and holds no island objective at the end.

## What the AI did (baseline, with the shipped win conditions)

6 seeds each, 10 days, `usecfg=1`, 4 jobs, shipped `.tri`:

| campaign | endings | front |
|---|---|---|
| save0 | 6 × 16 (timeout) at 231 h | 170 distinct objectives changed hands, 259-380 capture events a run, and Taiwan ended holding every one of them: Taoyuan/Taipei/Taichung churn, net 406→385-406 |
| save1 | 6 × 16 (timeout) at 231 h | **2** ownership changes in every seed (Lianhe, Shijing — China takes both on day one and nothing else moves), net 408→406 / 792→794 |
| save2 | crash at 23.9 h in every run | n/a |

Attrition over the ten days: `save0` Taiwan 6,034→2,998-4,213 vehicles, China
6,294→2,293-2,559, aircraft up on both sides (729→1,159-1,447 and
1,401→2,357-2,690). `save1` is much quieter: Taiwan 6,062→5,108-5,466, China
4,701→4,116-4,220. `save0` also costs 24-36 minutes of wall time a run (China's
1,401 aircraft and 70 task forces), `save1` 5-6 minutes.

## save1 rebuilt: a war that moves (2026-10-26, night)

save1 was inert for a mechanical reason. The GTM's `GTMCOST` diag showed Taiwan scoring
35,115 capture candidates and **refusing every one** at the 2000-node ground-path cap
(`lowest cost 2000`): with no land path across the strait, both armies got no capture
orders, so `MoveUnit` had nowhere to send them and the war moved 2 objectives in ten days.
The campaign's own brief (`lcktxtrc.irc`, `TXT_SC_2`) says the opposite -- after the failed
PRC invasion, the ROC and its allies counter-invade the mainland.

Fixed on three fronts, all backed up in `save1.cam.bak-lodgement`, `.bak-lodgement2`,
`.bak-beachhead`:

| change | what |
|---|---|
| Blue lodgement | Kuan-Yin-style foothold at Xiamen: objectives 27 Ta Shan Ting, 872 Aotou, 866 Xindian, 365 Hsin-Chin are Taiwan's, with 18 Blue battalions (the 11 Air Assault ones plus 7 others) landed on them |
| Chinese beachhead | the failed invasion the brief describes: objectives 401 Hukou and 367 Hsin-Chu City are China's, with 30 Chinese battalions landed around them |
| engine | `g_bKeepCaptureOrder` (battalio.cpp): a battalion executing a capture order keeps it while the objective is still the enemy's, instead of losing it the moment the front-line hop count moves -- with 4,242 capture orders assigned for 49 objectives taken in one ten-day run, 3,985 lasting under an hour |

Result, 3 seeds x 10 days:

| seed | result | flips | battalions moving | track | Taiwan obj | China obj |
|---|---|---|---|---|---|---|
| 1 | 17 Taiwan | **65** | 113 | 13,369 km | 410 -> 417 | 790 -> 783 |
| 2 | 17 Taiwan | **67** | 114 | 15,479 km | 410 -> 409 | 790 -> 791 |
| 3 | 17 Taiwan | **107** | 120 | 14,064 km | 410 -> 415 | 790 -> 785 |

Before: 2 ownership changes, 82 locally shuttling battalions, 8,341 km, 6 x 14 China.
`GTMCOST` for team 2 now reports `lowest cost 1` (targets reachable), and Blue takes
objectives on the mainland, including Xiamen Army Base.

Korea Rolling Fire gate after the engine change: **PASS, 3 of 3** seeds a Blue win by day 10
(141.6 h, 168.3 h, 196.2 h).

Follow-up if save1 should not be a Blue sweep: it now wins 3 of 3 (holds the capital and both
islands while containing the beachhead). Tightening `#17` (e.g. also requiring the Hsin-Chu
objectives back) or loosening the red block would give China a way to win.

## The objective link graph was the reason nothing moved (2026-10-27)

Units do not path cell by cell first: `BuildUnitWaypoints` routes them objective to objective over the
campaign's link graph (`GetUnitObjectivePath(&path, o, t)`) and only then grid-paths the last leg. Measured
in the saves, that graph is broken in Taiwan and fine in Korea:

| campaign | objectives | links | links/objective | isolated (no links) |
|---|---|---|---|---|
| Korea Rolling Fire | 2,700 | 6,246 | 2.31 | 86 (3%) |
| Taiwan save0/1/2 | ~1,227 | ~1,325 | 1.08 | 716 (58%) |

The engine's own `MOVE` diagnostic agreed, in the last six hours of a ten-day run: Taiwan's Blue team
failing **1,355 grid paths and 245 waypoint builds, 24 battalions**; Korea **1 and 6, 2 battalions**. The
failure lines are short coastal moves -- `(257,371) -> (258,373), 2 km: water cells 1/3` -- the exact moves
a link graph would route around.

Fix: `BuildMissingObjectiveLinks()` in `campupd/cmpclass.cpp`, called once at the end of
`CampaignClass::LoadCampaign`, behind `g_bSynthObjectiveLinks`. Every objective with fewer than two
neighbours is linked to its nearest ones within 40 km, both ways, cost from distance (`AddObjectiveNeighbor`).
It is a guard as much as a fix: the graph is only touched when a theatre has **fewer than 1.5 links per
objective**, so Korea's healthy graph is left alone (with the synthesis applied there too, its own gate
slipped from 3 of 3 to 2 of 3).

Result, save1, ten days (build 15:30):

| seed | result | Taiwan objectives | China objectives |
|---|---|---|---|
| 1 | 17 Taiwan | 410 -> **444** | 790 -> 756 |
| 2 | 17 Taiwan | 410 -> **442** | 790 -> 758 |
| 3 | 17 Taiwan | 410 -> **431** | 790 -> 769 |

Against 410 -> 413 / 413 / 402 before the change, and two ownership changes in ten days when this started.
Korea Rolling Fire gate: **3 of 3** (174.8 h, 150.8 h, 144.0 h).

Refinement (build 18:04): the first version linked nearest neighbours up to 40 km and ignored the water
between them, so it also joined objectives across straits -- China's failed grid paths went from 295 to
1,168 in six hours and rear battalions marched on the spot. The builder now walks the straight line between
two cells with `GetMovementCost(Tracked)` (a sample per km) and refuses the pair if any sample is
impassable, and the radius is 20 km. Same seed, three builds, looking at five rear Chinese battalions around
Putian plus one USMC battalion:

| build | their total track | net movement | direction reversals | Blue failed grid paths / no-waypoints |
|---|---|---|---|---|
| 40 km, nearest neighbour | 4,157 km | 286 km | 386 | 768 / 363 |
| 40 km, no-draft rule added | 3,452 km | 140 km | 416 | 1,906 / 313 |
| **20 km, land only** | **606 km** | **276 km** | **28** | **525 / 222** |

Seven times less walking for the same ground covered, and the ping-pong is gone (28 reversals against 386).
save1 over three seeds: 410 -> 441 / 416 / 422, all ending 17; Korea gate **3 of 3** (137.2 h, 150.8 h,
187.2 h). The tighter graph does give up some of the raw objective churn of the 40 km version
(444 / 442 / 431), which is the trade: less territory changing hands, almost none of it wasted marching.

### Engine switches added for this theatre (all in the 15:30 build)

| switch | what it does | kept? |
|---|---|---|
| `g_bSynthObjectiveLinks` | builds missing objective links in theatres whose graph is broken | **on** |
| `g_bSelfTask` | a battalion with no objective takes the nearest enemy one, else a friendly one to hold | on |
| `g_bGroundAssault` | an ASSAULT/AIRBORNE order on a non-cargoed unit becomes a ground CAPTURE | on |
| `g_bDefaultTactic` | a battalion with tactic 0 gets the tactic matching its order (CAPTURE -> move column) | on |
| reachability filter | self-task skips objectives whose cell the unit cannot enter; a unit whose objective is across water is redirected | on |
| `g_bUnstickWater` | a unit standing on a cell it cannot enter is nudged to the nearest cell it can stand on | on |
| `g_bKeepCaptureOrder` | a unit keeps its capture order while the objective is still the enemy's | on |

## The campaign map itself was wrong (2026-10-28)

The grid the AI paths on -- `Theaters\Taiwan\campaign\Taiwan.thr`, one byte per kilometre -- painted
**656,218 km2 of open sea as land** (agreement with the 3D terrain: 30.9%) and carried **no roads at all**,
where Korea has 9.6% road cells and the AI uses them. That is why units drove across the strait, why
objectives kept resolving to cells no ground unit could enter, and why the link graph had to be synthesised
before anything moved.

Repainted from the theatre's own 3D terrain (`tools/terrain/paint_sea_from_3d.py`, backup
`Taiwan.thr.bak-pre-sea`): water agreement 30.9% -> **93.5%**, enterable cells 93.3% -> **30.7%** (Korea
39.1%). save1 then went 410 -> 431 / 441 / 451 objectives across three seeds, all ending in a Blue win, and
Korea's gate stayed 3 of 3. Roads are still missing.

The method, the measurements and the checklist for another theatre are in
[THEATER-TERRAIN-AUDIT.md](THEATER-TERRAIN-AUDIT.md).

## Rule for any further Taiwan work

Every change made for this theatre -- engine, campaign data or trigger script -- gets a
Korea check before it is called done:

```
cd tools/campsim
python regress_taiwan.py          # Korea Rolling Fire (save0), seeds 1-3, ten days
```

A **majority** of the three seeds must be a Blue win by day 10 (`endgame=17`,
`campaign_h <= 240`); 2 of 3 passes, 1 or 0 fails. The gate exists because Taiwan work has
twice changed engine behaviour Korea shares (the brigade-column wait; the reinforcement
placement rule), and Rolling Fire is the fastest way to notice.

## Reproducing

```bash
cd tools/campsim
python run_batch.py --theater taiwan --save save0 --days 10 --seeds 1 2 3 4 5 6 --jobs 6 --set usecfg=1
```

Run files are `runs/save<N>-Taiwan-s<seed>.jsonl` (+ `.log`, `.ffdebug.log`),
labelled "Taiwan - Kittie's last stand" etc. in the Campaign Lab
(`python serve.py`, viewer backdrop `backdrop-Taiwan.png`, made with
`python make_backdrop.py "C:\FreeFalcon6\Theaters\Taiwan\terrdata" backdrop-Taiwan.png`).
