# Rail in the campaign

How railways fit FreeFalcon's campaign engine: what the engine already has,
what the first train does, and the order to build the rest in. Companion to
`CAMPAIGN-SUPPLY-ENGINE.md`, which this leans on. Code first: every claim
cites where it lives.

---

## What the engine already has (and what it does not)

The original designers planned rail and never shipped it. The hooks are real
code; the data behind them is empty in every installed theater.

| Hook | Where | State |
|---|---|---|
| `Rail` movement type | `falclib/include/falcent.h` | Defined. No unit class uses it. |
| Rail pathfinding (`PATH_RAILOK`) | `camplib/path.cpp:400, 704, 790` | Written; **nothing passes the flag**. Dead code. |
| Rail bit per grid cell | `campterr.h:21` (`RailMask 0x80`) | Every `.thr` has **0** rail cells. |
| `TYPE_RAILROAD`, `TYPE_RAIL_TERMINAL` objectives | `classtbl.h:86` | Class rows exist; Korea places **none**. |
| Rail nodes carry supply | `supply.cpp:353` (`SendSupply`) | Railroad nodes are hops like roads (15% worst loss when bombed). |
| Damage raises route cost | `path.cpp` (`PathDamageCost`) | Railroad nodes included. |
| Weapons have a hit chance vs rail | `flight.cpp:3405` (`GetWeaponHitChance(..., Rail)`) | Per-weapon column exists. |
| `COVERAGE_RAIL` tile paths | `terrtex.h:30` | No terrain tile carries one. |
| Unit transport | `unit.cpp:4532` `LoadUnit` / `UnloadUnit`, `battalio.cpp` `GTACTIC_MOVE_AIRBORNE` | Used for helicopter lift of battalions. |

So the track data had to come from outside (OpenStreetMap, `osm_rail.py`),
but the campaign already knows what a rail node, a rail bridge and a carried
unit are.

---

## The questions, answered

### Does the DMZ break the line?

Yes, twice over. The routes are built per named line, and OSM names the two
halves differently (Gyeongui south of the DMZ, Pyongbu north of it), so they
are separate routes with a ~5 km gap. And a train only runs on the longest
stretch of its route that its own side holds (`FindTermini`, `railnet.cpp`):
the route is sampled every 2 km, each sample takes the owner of the nearest
objective, and the train turns round `RailFrontStandoff` km short of hostile
ground. If the front moves, the turn-round point moves with it at the next
campaign stage.

### Can it intersect with supply hubs?

Yes, and that is how the termini are chosen. The rear end is the friendly
supply source furthest back along the line within `RailRunKm`, using the
engine's own test, `ObjectiveClass::IsSupplySource()` (city, port, depot or
army base, not front-line or second-line). These are exactly the objectives
the supply engine starts its deliveries from (`FindNearestSupplySource`).

**Not done yet: the train does not carry supply.** v0 is the moving part only.
The natural next step (Phase 3 below) is to make a railhead a supply source
while the train that serves it is alive: units near the front terminus draw
from it with rail's low per-hop loss, and a dead train or a dropped bridge
cuts that off. It uses the pools and rationing that exist; it does not invent
a second economy.

### Will rails interact with bridges? Can we make our own?

Two routes, in order of cost:

1. **Borrow the existing bridges (recommended first).** Korea has 691 bridge
   objectives. They are road bridges, but a rail line usually crosses a river
   within a kilometre or two of one. OSM marks every rail bridge segment
   (`bridge=yes`; `rail.json` keeps them as `seg` flags). Bind each rail river
   crossing to the nearest bridge objective; when its status is 0 the train
   cannot pass and the line is cut there. That puts rail inside the loop the
   game already runs: the ATM plans strikes against bridges, bombing drops them,
   `RepairObjectives` rebuilds them, and `PathDamageCost` already treats a
   damaged bridge as a choke. No new data files.
2. **Our own rail bridges.** New objectives in the theater's objective list,
   with features using the existing bridge span models, then a relink of the
   objective graph. That is theater editing (the editor cannot add objectives
   yet), and the objective links are built once at campaign creation
   (`LinkCampaignObjectives`). Worth doing for signature targets only, later.

### Can rails be bombed and repaired?

- **The train: yes, now.** It is an ordinary Supply battalion (class 77,
  sixteen KrAz trucks as boxcars). It takes damage in the sim car by car and dies
  when its vehicles do. Whether the air tasking manager picks it as a target
  on its own, like other spotted ground units, is **not checked yet**; a
  player-planned strike on it should work like one on any battalion. A new train spawns at the hub after
  `RailRespawnHours` (12 h).
- **The track: not yet.** Track only becomes bombable once something on it is
  an objective. Phase 2 (bridges, above) is the high-value version. Railroad
  or Rail-terminal objectives at junctions and yards would make the rest
  bombable. Their damage already feeds supply loss and route cost.
- **Repair** comes free with both: objective repair is status-driven, so a
  repaired bridge reopens the line with no extra state to keep in step.

### Can trains transport ground units?

Yes, and the engine has the pattern. Helicopter lift works like this: a
battalion is given `GTACTIC_MOVE_AIRBORNE` and waits, a flight arrives and
calls `LoadUnit` (the battalion goes inactive as cargo), flies, and
`UnloadUnit` puts it down at the destination (`unit.cpp:4532`). A train can
do the same with the ground tasking manager deciding when rail is worth it:
same side, both ends near the same friendly route, distance over roughly
100 km. The unit rides inactive, so killing the train kills the cargo too
(`UnloadUnit` has an unfinished "apply damage" TODO to fill in). Phase 4:
it touches the GTM, which is the riskiest code in the campaign to change.

---

## v0: what is in the game now (branch `rail-tracks`)

Off unless `set g_bRailTrains 1` is in `FFViper.cfg`.

- `rail.txt` in the theater's terrain folder: one continuous route per named
  line, stitched offline (`rail.build_routes`: piece ends within 0.5 km join;
  the route is the shortest track between the two nodes furthest apart).
- `campaign/camplib/railnet.cpp` loads it (`FalconTerrainDataDir\rail.txt`).
- One train per route listed in `g_sRailTrainLines` (default: the Pyongbu
  Line, Pyongyang to Kaesong). It is a Supply battalion flagged `U_TRAIN`
  (the spare `U_B1` bit, saved with the unit), with `U_NO_PLANNING` and
  `U_SCRIPTED` so the ground tasking manager leaves it alone.
- **Position is a function of game time.** A cycle is: dwell at the hub
  (`RailTrainDwell` minutes), run to the front at `RailTrainSpeed` km/h,
  dwell, run back. The campaign thread (aggregated, via
  `BattalionClass::MoveUnit`) and the sim thread (every car, every frame, via
  `GroundClass::Exec`) both ask `RailTrainPose` and get the same answer, so
  there is no state to hand between them. When the front moves the cycle is
  re-based around the train's current spot, so it never jumps.
- Cars stay in one order behind the lead, 70 ft apart, following the curve.
  At a terminus they turn round in place.
- After a load, a train in the save is picked up again by projecting it onto
  the nearest route.
- Every decision goes to `FFDebug.log` as `rail: ...` lines.
- **Tactical engagements:** the tick runs from `DoTacticalLoop` too. Place a
  train yourself: right-click the map, **Add Battalion**, Equipment
  **Arty/Rocket** (that is where `teunits.lst` files Supply), Unit Type
  **Supply**, and drop it within 1.5 km of a rail line. When the TE runs it
  becomes that line's train (`EnlistPlacedTrains`), for the side you gave it,
  even on a line not in `g_sRailTrainLines` -- but such a line gets no
  automatic train and no replacement. In a campaign nothing is enlisted:
  Supply battalions there are the campaign's own.
- **Campaign map:** right-click, **Rail lines** (a toggle beside the FLOT)
  draws the routes and marks your own trains; `g_bRailMapAllTrains 1` marks
  every train. The marks move only when the overlay is rebuilt (toggling
  any layer).

What it deliberately does not do yet: draw track in 3D, carry supply, stop
at bridges, carry units.

### Config

| Key | Default | Meaning |
|---|---|---|
| `g_bRailTrains` | 0 | Master switch. Off because trains are real units and are written into saves. |
| `g_sRailTrainLines` | `""` = `Pyongbu` | Comma-separated route-name prefixes, quoted: `set g_sRailTrainLines "Pyongbu,Gyeongbu"` |
| `g_nRailTrainSpeed` | 50 | km/h |
| `g_nRailTrainDwell` | 20 | minutes at each end |
| `g_nRailFrontStandoff` | 10 | km short of hostile ground |
| `g_nRailRunKm` | 160 | longest hub-to-front run |
| `g_nRailRespawnHours` | 12 | campaign hours before a destroyed train is replaced |

### Turning it off again

Set `g_bRailTrains 0`. A train already written into a save stays in that
save as a Supply battalion that does not move (its `MoveUnit` still returns
early, because the unit is flagged; with the switch off nothing gives it a
position to go to).

---

## Phases

1. **v0: a train that moves** (this branch). Check it in game first.
2. **Draw the track.** On the campaign map first (cheap, and it is where you
   plan the strike), then in 3D.
3. **Bridges cut the line.** Bind OSM bridge segments to the nearest bridge
   objective; a dropped bridge becomes a terminus.
4. **Rail supply.** A served railhead becomes a supply source. That makes a
   train worth killing.
5. **Troop trains.** `LoadUnit`/`UnloadUnit` through the GTM.
6. **Own models.** A locomotive and wagons instead of KrAz trucks.
