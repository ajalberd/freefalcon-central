# Theater terrain audit — is the campaign map good enough to path on?

How to tell whether a theatre's **campaign cover grid** (`<theater>.thr`) matches the ground the 3D world
draws, why it matters for ground movement, and what the Taiwan theatre had wrong. Korea is used as the
benchmark: its units move freely, so its grid is the reference.

This is the data layer under `GetMovementCost`. The AI plans every ground move, every objective link and
every path search on this grid, not on the 3D terrain — so a grid that disagrees with the world produces
units that drive across straits, objectives nobody can reach, and path failures that look like engine bugs.

---

## 1. What the grid is

`<theater>.thr` (e.g. `Theaters\Taiwan\campaign\Taiwan.thr`, `campaign\SAVE\KOREA.THR`) is one byte per
kilometre cell, 1024 × 1024, with a four-byte `short x, short y` header and cells stored in `x * Y + y`
order:

| bits | meaning |
|---|---|
| low nibble | cover type — **0 = water**, also 1 road?, 3, 5, 6, 7 … (the engine's cell-type table maps it) |
| `0x40` | **road** (Korea's road cells are e.g. `0x56` = type 22 + road) |
| rest | flags the engine reads (rail, bridge); untouched by the tools here |

The 3D world is a different file set (`terrdata\terrain\theater.l0`…), one ground tile per campaign km.
`tools\terrain\cover_compare.py` compares the two: per cell it takes the share of the tile's pixels that
are sea-coloured (not post heights, not texture *types* — those were tried and both lie).

## 2. How to measure it

Two independent tools, both read-only:

**In the engine** — a campsim audit that walks the real `GetMovementCost` the AI uses:

```
CAMPSIM_MAPTERRAIN=1 campsim.exe C:\FreeFalcon6 save0 0 1 run.jsonl 30
# MAPTERRAIN cells 1048576, enterable 410384 (39.1%), road cells 39579 (9.6% of enterable) |
#   objectives 2700: on road 1536 (57%), on water 70 (3%)
# plus MAPROW 00..63: a 64x64 ASCII map, '~' impassable, '.' enterable, '#' road
```

**Against the 3D terrain** — `tools\terrain\cover_compare.py`:

```
python cover_compare.py --theater C:\FreeFalcon6\Theaters\Taiwan\terrdata `
                        --thr C:\FreeFalcon6\Theaters\Taiwan\campaign\Taiwan.thr --out DIR
# === water agreement over 1048576 cells ===
#   both water 658313, both land 322222, agree 93.51%
#   campaign land but 3D water : 0 cells
# plus cover_diff.png: blue = both water, green = both land,
#   RED = campaign water / 3D land, YELLOW = campaign land / 3D water
```

## 3. Korea, the benchmark

```
MAPTERRAIN cells 1048576, enterable 410384 (39.1%), road cells 39579 (9.6% of enterable) |
  objectives 2700: on road 1536 (57%), on water 70 (3%)
```

A peninsula of land in a sea of water, with a road net the AI uses: 9.6 % of the enterable cells carry a
road and 57 % of objectives sit on one. That is what a hand-built theatre looks like.

## 4. What Taiwan had wrong

Before the fix (`Taiwan.thr`, 2011 vintage, converted rather than authored):

```
MAPTERRAIN cells 1048576, enterable 978440 (93.3%), road cells 0 (0.0% of enterable) |
  objectives 1228: on road 0 (0%), on water 453 (37%)
```

- **The sea was painted as land.** Agreement with the 3D world was 30.9 %; 656,218 km² the 3D world draws
  as water were land to the AI. A global offset does not explain it (the best shift recovered 2.3 %), so it
  is a painting error, not a mis-registration.
- **No roads at all** — not one byte with the `0x40` flag in the whole grid.
- 37 % of objectives sat on the few cells that *are* water, which is why so many ground tasks ended at
  "water cells 1/3, no grid path" and units retried the same impossible move for hours.

Consequences we measured before the fix: Taiwan's Blue team failing **1,355** grid-path searches and 245
waypoint builds per six hours against Korea's **1** and 6; a synthetic objective-link graph being the only
thing that got units moving at all; and 2/3 of campaigns stalled in the first days.

## 5. The fix that worked

`tools\terrain\paint_sea_from_3d.py` repaints the grid from the theatre's own 3D terrain — cells whose tile
is at least `--threshold` (default 0.5) sea get the water cover type (Korea's `0x30`), everything else is
left alone. It writes a `.bak-pre-sea` backup, refuses to run twice on the same file, and prints the cell
counts before writing.

```
python paint_sea_from_3d.py --theater C:\FreeFalcon6\Theaters\Taiwan\terrdata `
                            --thr C:\FreeFalcon6\Theaters\Taiwan\campaign\Taiwan.thr [--dry-run]
# water cells before 70136 (6.69%), 3D sea cells 658313 (62.78%), repainted 657386
```

Result on Taiwan:

| | before | after |
|---|---|---|
| water agreement with the 3D world | 30.93 % | **93.51 %** |
| "campaign land but 3D water" | 656,218 cells | **0 cells** |
| enterable (tracked) | 978,440 (93.3 %) | **322,222 (30.7 %)** — Korea 39.1 % |

Campaign effect, save1 "Eye of the Serpent", three seeds, ten days: Taiwan's objectives went
410 → 431 / 441 / 451, all three ending in a Blue win (418 / 437 / 437 before the repaint), and the Korea
gate stayed 3 of 3.

**Objectives on water are usually fine.** Of the 460 that sit on a water cell, 445 are within 1 km of land —
coastal cities and riverbanks, exactly what the engine's water-objective capture rule exists for. Only three
are genuinely stranded.

## 6. What is still missing

- **Roads: done from the scenario, not from OSM (2026-10-31).** See section 6a. OSM is still the way to
  add real arteries on top (`paint_roads_from_osm.py` without `--no-osm`), but the public Overpass server was
  returning 502/429 for hours, and accuracy was not the goal: a road net the campaign can *use* was.
- **Three stranded objectives** in Taiwan save1 (two "Nowhere" markers and "Yanpu"), to be nudged to the
  nearest enterable cell.
- The repainted grid changes the *campaign*, not just the map: link graphs, tasking and balancing all shift,
  so any theatre repainted this way needs its own three-seed run plus the Korea gate.

## 6a. Road placement (`tools	errain\paint_roads_from_osm.py --no-osm`)

The road bit only matters where units want to go, so the roads follow the campaign's own objective **links**
(the graph the AI already uses for objective-to-objective moves), taken as the union over all of the
theatre's saves:

1. **Bridge and port objectives** get the road bit on their own cell even where it is water — the engine reads
   road + water cover as a bridge/port (`GetMovementCost`, cost above `MAX_COST`), so these are the only road
   cells allowed on water. Every other road cell is on land.
2. **Every link** (<= 80 km, so sea and air hops are skipped) gets the cheapest 8-connected land path between its
   ends, Dijkstra on relief cost with road cells at 0.15 so routes share each other's cells and any OSM roads.
   A path more than 3x the straight distance is dropped as not a road.
3. **Spurs**: ground objectives no link reaches (villages, mostly) join the nearest road if it is within 15 km.
   Hill tops, passes, beaches, radars, SAM sites and radio towers are left off.

Optional first stage (no `--no-osm`): motorway/trunk/primary from OSM, fitted by the airfield affine
(29 airfields, 3.4 km RMS) and optionally refined on the OSM coastline against the 3D shore (`--no-refine`
skips the coastline download). Roads on water samples are dropped.

Writes a `.bak-pre-roads` backup and refuses to run twice; `--force` repaints from the backup.

Result (campsim, `usecfg=1`, seeds 1-3; same build, grid swapped between batches):

* 3,913 road cells (1.2 % of enterable land; Korea 9.6 %), 757 of 1,228 objectives (62 %) on one — all 359
  bridges, 100 % of towns, 83 % of cities (Korea: 57 % of objectives).
* **save1** Eye of the Serpent, ten days: Blue win 3 of 3, Taiwan objectives 453 / 453 / 454 (sea repaint only:
  431 / 441 / 451). Small, not proven: no same-build control.
* **save2** Raging Dragon, China's invasion — same-build control:

| | no roads | roads |
|---|---|---|
| seed 1 | ends day 5.6 (`endgame=15`), Blue 435 -> 315 objectives | day 7, Blue win (17), 435 -> 401 |
| seed 2 | **China wins day 2.2** (14), 435 -> 322 | day 7, draw (16), 435 -> 399 |
| seed 3 | ends day 5.5 (15), 435 -> 358 | day 7, draw (16), 435 -> 383 |

  Without roads Blue's reserves could not get to the front and China took 78-120 objectives before the game
  ended early; with roads China takes 34-52 and no seed is a Chinese win. The engine's `CAMPSIM_MAPTERRAIN`
  audit crashes in `resmgr.c` `_say_error` (stack-buffer overrun) on this build even with the pre-road grid;
  the figures above are from the painter's own report.

## 6b. Two faults the road work exposed (2026-10-31)

Scanning the grid against Korea's (cover, relief, 8-connected land pieces, objectives and units on water,
link ends) found two more faults, both fixed in the data and kept as tools.

**Inland water speckle** -- `paint_sea_from_3d.py` also painted grey roofs, shaded fields and river mud as
water: 59,299 inland water cells in 15,298 pieces (Korea 9,329 in 66; 9,397 single-cell), a lattice across
the Taipei basin, and 147 villages/towns/cities/airbases (Korea: only bridges and ports) on water cells.
In the ten-day save1/save2 logs 12,680 move failures were between two land cells the grid had cut apart; a 2 km move
across Taipei failed ~150 times per battalion. `fill_inland_water.py` turns inland water pieces of <= 400
cells back into land (nearest land byte; a piece holding a bridge or port objective is kept) and then every
other objective's own cell: land 30.7 % -> 34.8 % of the map, objectives on water 149 -> 0. "No grid path
between cut land" failures fell from 9,072 to 1,693 per three seeds. Backup `Taiwan.thr.bak-pre-fill`.

**Relief inverted** -- relief (bits 4-5, `ReliefCost` 1.0/1.41/1.73/2.0) in Korea follows the 3D terrain (mean
elevation by class 671 / 2,439 / 2,398 / 3,975 ft, correlation +0.64). Taiwan's was 4,543 / 2,131 / 1,408 /
261 ft, correlation -0.35: the coastal plain was the slowest class and the Central Range the fastest.
`relief_from_3d.py` ranks land cells by 3D mean elevation and cuts them into Korea's class shares (32/44/12/11 %):
correlation +0.87, class means 239 / 1,151 / 2,202 / 3,881 ft. Backup `Taiwan.thr.bak-pre-relief`.

Judged on map function, not on who wins (the shipped balance is not a reference): after fill + relief the
grid-path failures are mostly gone. What remains is `no waypoints`, which is the objective **link graph**,
not the grid -- see below.

**Cover type** -- the Central Range is typed Urban (cover 7: 45 % of land, Korea 1.6 %). It has the same
defence value as forest and the mean tracked cost per cell equals Korea's (4.76 vs 4.84), so left alone.

**Link graph (not fixed in data yet)** -- units path objective to objective over the link graph before the
grid. Taiwan: 717 of 1,227 objectives (58 %) have no link (321 villages, 223 bridges, 31 cities, 28 airbases),
largest connected piece 389 objectives. Korea: 86 of 2,700 (3 %), largest piece 2,528. The engine patch
`BuildMissingObjectiveLinks` (`cmpclass.cpp`, uncommitted, in no deployed game exe) papers over it at load in
campsim only; the data should carry proper links.

## 7. Checklist for another theatre

1. `CAMPSIM_MAPTERRAIN=1` on one of its campaigns: read the enterable %, road %, and objectives on water.
2. `cover_compare.py` against its 3D terrain: agreement %, and the direction of the mismatch in
   `cover_diff.png` (yellow = sea painted as land, red = land painted as water).
3. If the sea is wrong, `paint_sea_from_3d.py --dry-run` first, then for real; keep the `.bak-pre-sea`.
4. Re-run the audit: expect agreement in the 90s and an enterable fraction in the 30-45 % band.
5. Only then look at roads, and only then at engine behaviour — the engine cannot out-think a map with no
   coast and no roads.

## 8. Limitations

- The water test reads the *tile imagery* (sea-coloured pixels), so a photos-like coastal tile is judged by
  its colours: tiles with heavy surf or dark harbour water can read wetter than they are.
- The grid byte's upper bits (rail, bridge) are preserved but not interpreted by these tools.
- `cover_compare.py` needs `tilesurvey.py` beside it and a DDS-capable numpy install; the ASCII audit needs
  nothing but campsim.
- Korea's grid is hand-authored: it is a *reference for what good looks like*, not a generator to copy.
