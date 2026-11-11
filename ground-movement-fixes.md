# Falcon 4 campaign ground-movement fixes (FreeFalcon, for porting to BMS)

These six come from the original Falcon 4 campaign code, which BMS shares, so the function names should carry over. BMS may already
have fixed some of them, so each one says how to check first.

---

## 1. SimToGrid: a rounding bug that freezes ground movement

**Where:** `SimToGrid()` (camplib `find.cpp`), and any other sim-to-grid conversion.

**The bug:** `GridToSim()` returns the **centre** of a 1 km cell, so a unit's sim position sits exactly half a cell from each edge.
`SimToGrid()` divides and then **rounds to nearest** (`FloatToInt32` rounds; it does not truncate). Float error on that x.5 decides
which way it goes, and 553 of the 1,024 grid coordinates read back one cell too high. A battalion stepping into such a cell going
**south or west** reads back the cell it started from, so it never moves. The ground war just stalls. Speeding units up or cutting path
costs changed nothing while this was present.

**Check:** loop over `GridToSim(i)` → `SimToGrid()` for i = 0…1023 and see whether you get i back. Watch whether aggregated battalions
ever finish moves heading south or west.

**Fix:** use `floor(x / GRID_SIZE_FT)`. Audit every grid↔sim conversion so it floors rather than rounds.

---

## 2. Capture targets stop 3 links from the front (front freezes behind bridges and junctions)

**Where:** `GroundTaskingManagerClass::IsValidObjective()` (capture branch) and `GetAddBits()` in `gtm.cpp`.

**The bug:** capture orders only target **secondary** objectives (towns, depots and so on) flagged front, second or third line. That flag is
a hop count over the objective link network, not a distance. Bridges, junctions, SAM sites and rail stops are never capture targets;
they change hands only when a unit drives through them. If the next secondary sits four or more links behind a chain of those, there are
zero valid capture targets and the front stops for good. In our runs Pyongyang sat four links behind a SAM site → town → junction chain
from h48 to the end. With the fix, Blue reached it in 8 of 8 runs instead of 2 of 8.

**Check:** count valid capture targets per team per tasking cycle. Look for a front that sits idle while a team is on the offensive with
healthy units.

**Fix:** also accept non-secondary objectives that are themselves **front line** as capture targets: `IsSecondary() || IsFrontline()`.
Make sure `GetAddBits` doesn't strip the capture bit for them.

**Trap:** a non-secondary can have no primary objective, so null-check `GetObjectivePrimary()` wherever the capture path uses it. We
crashed there.

---

## 3. Reserve units bounce back and forth, burning supply

**Where:** `BattalionClass::MoveUnit()`, the "is my objective still valid?" check at the top.

**The bug:** each move, the battalion re-checks its objective with `IsValidObjective(orders, obj)`. For reserve orders that's only true on
a **secondary** objective. If it isn't valid, the unit falls back 3 links (`FindRetreatPath`) and takes a reserve order on wherever it
lands, which is usually a road, junction or bridge. That's invalid again, so the unit falls back again, and the tasking manager sends it
forward again. That was about 1,843 hops on day 1 for one side, roughly 7 per battalion and half of all order changes. Driving costs
supply, so this is what drained rear reserves.

**Check:** log order changes per battalion per day. Reserve units cycling between the same few places means you have it.

**Fix, two parts:**
- A reserve battalion sitting on one of its own team's objectives away from the front counts as valid and stays put.
- When the fall-back lands on a non-secondary, step up to its secondary parent, if that's a valid reserve objective.

---

## 4. The winners drive backwards (two places)

**(a) In `MoveUnit()`:** a capture order becomes invalid the moment its target is friendly, usually because a neighbouring battalion took it
first. Stock code then falls back 3 links as reserve, so the battalion that just won drives about 49 km **backwards**. That happened around
300 times a day for one side, the biggest source of pull-backs.

**Fix:** if the objective is now ours and the unit is healthy (not broken, supply at least 50%), hold it until the tasking manager gives it a
new order.

**(b) In the tasking manager's reserve assignment** (the `GORD_RESERVE` candidate list in `AssignUnits`): reserve objectives only score
20–60 km behind the front, but stock code picks the idle candidates **closest** to the action, which means front-line units. Healthy front
battalions get sent 10+ km back about 115 times a run.

**Fix:** drop idle units within about 25 km of the front from the reserve candidates, unless they're broken or under 50% supply (those
should still pull back to refit).

---

## 5. Rear armies never get a real job (path-cost ceiling)

**Where:** `ScoreUnit()` in `gtm.cpp`, and `CostToArrive()` / the objective path search in `path.cpp`. The limit comes from `Falcon4.AII`
`[GTM] ObjGroundPathMaxCost` (stock 500).

**The bug:** `CostToArrive()` runs an objective-path search capped at `ObjGroundPathMaxCost`, and if the search fails it returns the cap.
`ScoreUnit()` then rejects any unit whose cost is at least the cap. With 500, anything more than about one or two days' travel can't be
assigned to a front objective. Second-echelon and allied armies (China, Russia) only ever get reserve slots near home and never come
forward.

**Check:** count units rejected for path cost in `ScoreUnit`, and look at how far rear armies actually move.

**Fix:** this is a **data** value, not code. Raise `ObjGroundPathMaxCost` (we used 2000; China's mean travel went from 31 km to 113 km).
It also bounds the path search, so watch CPU time.

---

## 6. Long moves fail forever (partial paths thrown away)

**Where:** `BattalionClass::MoveUnit()` (the grid-path step), `BuildGroundWP()` (route from the unit's last objective), and
`GetUnitObjectivePath()` in `gndunit.cpp`.

**The bug, three ways:**
- **Grid paths** are capped at `MAX_DISTANCE` (96) steps, and the search also stops after `GroundPathMax` nodes. Past either limit it
  returns failure **with the partial route so far**, which stock code throws away. The unit clears its waypoints, rebuilds the same far
  waypoint, and fails again every tick. The same battalions failed hundreds of times on 70–230 km legs, and China's army never left the
  Yalu.
- **Objective routes** have the same 96-hop cap. A 300+ km move returns failure, `MoveUnit` clears the objective, and the unit sits until
  it's re-tasked.
- **`last_obj` goes stale:** `BuildGroundWP()` starts the route from the battalion's `last_obj`, which only updates when the unit passes
  through an objective. After a cross-country move it can be 85–140 km behind the unit, so the path back to it fails the 96-step cap.

**Fix:**
- If a search fails but the partial route ends nearer the target than the unit is now, accept it. The unit only keeps about 8 steps
  anyway and re-plans from there.
- Lay waypoints along a partial objective route on the same condition.
- If `last_obj` is more than about 10 km away, start the route from the nearest objective to the unit instead.

Raising the cap is **not** the fix; the 96-step limit is structural.

---

## How to land these safely

1. **Instrument first.** Each of these was found from a counter, not by reading code. Log per team per hour: failed grid paths, failed
   objective paths, units rejected for path cost, valid capture targets, order changes per battalion, and fall-backs from `MoveUnit`.
2. **Put each fix behind its own switch** that defaults to the fixed behaviour but can restore stock, so later balance changes can be
   blamed on the right fix.
3. **Use a headless campaign runner if you can.** Ours runs a 10-day war in a few minutes, and a deterministic mode replays a seed
   exactly, so each fix can be compared against its own baseline.
4. **Order of impact:** SimToGrid first, because nothing else matters if units can't move south or west. Then the path fixes (partial
   paths, path-cost cap), then the order fixes (capture front, reserve hold and no-pullback).
5. **Expect wars to get shorter.** Fixed armies move far more; ours went from stuck to Blue winning in 1–3 days. Tune war length with
   other levers, not by putting these bugs back.
