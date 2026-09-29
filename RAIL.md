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

**The train carries supply (2026-09-28, compiles and links, not yet seen in a
campaign).** Each time a train finishes an outbound run it delivers a trainload
at the forward terminus (`MaybeDeliver`, `railnet.cpp`): `RailTrainLoad` (120)
supply and the same in fuel, scaled by its surviving cars, **drawn from the
team's national pools** -- the money road supply spends in `SupplyUnits` -- and
handed to its own side's battalions within `RailRailheadKm` (25 km) of the
railhead, nearest first, each up to what `GetUnitSupplyNeed` /
`GetUnitFuelNeed` says it is short of, through the unit's own `SupplyUnit`.
Rail does not create supply; it delivers it with one hop's 2% loss instead of
a road journey through every node and bridge. A train killed on the way never
arrives. Arrivals are read off the timetable (`t0 + dwell + run + k * period`),
so it pays once per arrival however the campaign ticks fall. Every delivery is
a `rail:` log line with what was handed over and what the pools hold after.

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
- **The Train class.** A unit class of its own: land / unit / battalion /
  Supply / sptype 20 (`RAIL_TRAIN_SPTYPE`), a copy of the 16-KrAz Supply
  battalion appended to `FALCON4.ct` and `FALCON4.UCD` and listed in
  `teunits.lst`. `tools/campaign-editor/install_train_class.py` puts it into
  an install (idempotent, backs up first). The engine reads those tables only
  from the theater's `objectdir` (`terrdata\objects` for every Korea theater);
  the per-campaign `specialdbdir` copies are the editor's and are kept equal.
  Automatic trains spawn as this class; a theater without it falls back to
  the Supply battalion. Any Train-class unit becomes a train -- identified by
  class, so an ordinary Supply battalion is never taken over.
- **Tactical engagements:** the tick runs from `DoTacticalLoop` too. Place a
  train yourself: right-click the map, **Add Battalion**, Equipment
  **Arty/Rocket** (where `teunits.lst` files Supply), Unit Type **Train**.
  The editor normally snaps a new battalion to the nearest objective (a road
  junction or a town, often kilometres off the track); a Train stays exactly
  where it is dropped, and the log says how far that is from a line. Within
  3 km, it becomes the nearest line's train when the TE runs, for the side
  you gave it, even on a line not in `g_sRailTrainLines` -- but such a line
  gets no automatic train and no replacement. Further out, it stays put.
- **Campaign map:** right-click, **Rail lines** (a toggle beside the FLOT)
  draws the routes and marks your own trains; `g_bRailMapAllTrains 1` marks
  every train. The marks move only when the overlay is rebuilt (toggling
  any layer).

What it deliberately does not do yet: draw track in 3D, stop at bridges,
carry units.

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
| `g_nRailTrainLoad` | 120 | supply and fuel per railhead arrival, from the national pools (0 = carries nothing) |
| `g_nRailRailheadKm` | 25 | delivery radius around the forward terminus |
| `g_bCampRailLines` | 1 | campaign map starts with Rail lines on |
| `g_bRailMapAllTrains` | 0 | mark every train on the map, not just your own (test aid) |

### Turning it off again

Set `g_bRailTrains 0`. A train already written into a save stays in that
save as a Supply battalion that does not move (its `MoveUnit` still returns
early, because the unit is flagged; with the switch off nothing gives it a
position to go to).

---

## Drawing the track in 3D

Three ways were weighed (2026-09-28). Chosen: **option 2 for the pilot, with
option 3 planned** for bridges.

1. **Paint rail into the terrain tiles.** Rejected. The ground is a shared
   library of tiles reused across the map (`texture.bin`, texID = set << 4 |
   tile), so a line through a tile needs a unique copy of it: ~4,000 new
   tiles for ~4,000 km of route, >100 MB, near the library's limit, clashing
   with the BMS tile mods, and one or two texels wide at 256 px/km anyway.
2. **A strip that sits on the ground, drawn near the camera.** The pilot:
   `DrawableRail` (`sim/otwdrive/drawrail.cpp`, 2026-09-28, links, not yet
   seen): 100 ft pieces within `RailTrackRangeKm` (8) of the camera, ground
   height under both edges and the centre, a 16 ft ballast quad plus two
   rail quads lifted 1.5-2 ft, colours scaled by the light level. Off unless
   `g_bRailTrack 1`; `g_bRailTrackLog 1` logs `RAILTRACK:` lines (pieces, and
   exact vs approximate ground). Not yet: tunnels, bridges, fog, texture.
3. **Real 3D objects.** Planned, for bridges first (see the recon below).

### Recon for option 2 (the strip)

- `DrawableTrail` (`graphics/objects/drawsgmt.cpp`) is the model: world-space
  quads built as `ThreeDVertex`, `renderer->TransformPoint` /
  `TransformPointToView`, `renderer->DrawSquare(v0..v3, CULL_ALLOW_ALL)`,
  state set with `renderer->context.RestoreState(STATE_...)`. These are the
  CPU-transformed Render3D prims, not the DX-engine model path.
- Drawables join the frame with `OTWDriver.InsertObject` / `viewPoint->
  InsertObject`. `ObjectDisplayList::UpdateMetrics` (`objects/objlist.cpp`)
  sorts by `max(|dx|,|dy|) - Radius()` and `DrawBeyond` draws far to near,
  interleaved with the terrain rings -- so one drawable with a huge radius is
  distance 0, drawn every frame in the nearest ring, after the terrain.
- `OTWDriverClass::Enter` (`sim/otwdrive/otwdrive.cpp:2006`) creates the
  `RViewPoint` (line ~2104); cleanup at ~2896 and ~3108. That is where a
  long-lived drawable is added and removed.
- Risk: z-fighting where the strip's heights (from the ground-height lookup)
  differ from the terrain mesh actually rendered at that LOD. Handled by a
  small lift, a short range, and a knob-gated height log if it flickers.

### Recon for option 3 (objects: bridges first)

- **Bridges are already objects made of pieces.** `sim/otwdrive/addobj.cpp`
  ~300-360: when a bridge objective's features deaggregate, the lead feature
  flagged `FEAT_ELEV_CONTAINER` gets one `DrawableBridge` container
  (`graphics/include/drawbrdg.h`), and every span becomes a `DrawableRoadbed`
  (`drawrdbd.h`, a `DrawableBuilding`) with `visType` (base) and
  `visType + 1` (superstructure, when `FEAT_NEXT_IS_TOP` and not destroyed),
  heading from the feature's yaw, 10 ft height and a 20/280 ramp angle.
  Destroyed spans swap drawables in place (the `lastPointer` replacement).
  `DrawableRoadbed::OnRoadbed(pos, normal)` is how vehicles find the deck.
- `FEAT_FLAT_CONTAINER` / `DrawablePlatform` is the airbase equivalent.
- So a rail bridge = a bridge objective whose features use span models, laid
  where OSM says the line crosses water (`seg` = `b` in rail.json). Two routes:
  (a) bind each OSM rail-bridge run to the nearest existing bridge objective
  (Korea has 691) and treat its status as the line's -- no new objects, uses
  the ATM/damage/repair loop as is; (b) new bridge objectives (theater
  objective list + features + relink via `LinkCampaignObjectives`) where no
  road bridge is close. (a) first.
- Routes do not carry the bridge/tunnel flags yet (`build_route` concatenates
  piece points only); the game file needs them for both the strip (no strip
  in tunnels, a deck over bridges, trains hidden in tunnels) and option 3.
- No rail models exist in `KoreaObj`; sleepers/poles as objects would be
  thousands of instances -- not worth it. Train cars are KrAz trucks for now.

## Phases

1. **v0: a train that moves.** Done; seen riding the line in 3D.
2. **Draw the track.** Campaign map done (Rail lines toggle); 3D not yet.
3. **The Train class.** Done: its own unit class and TE entry; identified by
   class (`install_train_class.py` puts it into an install).
4. **Rail supply.** Done in code: a trainload per railhead arrival, from the
   national pools. That makes a train worth killing. Not yet seen in play.
5. **Bridges cut the line.** Bind OSM bridge segments to the nearest bridge
   objective; a dropped bridge becomes a terminus.
6. **Troop trains.** `LoadUnit`/`UnloadUnit` through the GTM.
7. **Own models.** A locomotive and wagons instead of KrAz trucks.
