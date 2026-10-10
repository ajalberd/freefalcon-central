# Work in progress

Rolling handoff note. Read this first. Keep it short — when something is settled, cut it
down to the one fact a future session would otherwise re-derive, and let the code comments
and commit messages carry the rest.

Companion docs:

- `CAMPAIGN-SUPPLY-ENGINE.md` — how supply, production and power actually work.
- `OBJECT-RENDERING.md` — the missile fin flicker, the dynamic-light flags/spot fixes and
  per-pixel object lighting, plus the RenderDoc recipes. Read it before touching the object
  pass or the light CB.
- `tools/terrain/tilesurvey.py` — reads a theater's terrain and tile data offline, no build
  and no game needed. Re-run it instead of re-deriving any terrain number.
- `tools/models/objsurvey.py` — same for the 3D object database. Reads `KoreaObj.DXH`/`.DXL`:
  parent records, LOD names, and a model's whole node stream including every switch number,
  its branches and the geometry hanging off each one.

Planned work, one doc each. Reconnaissance, not plans. **Put findings in the topic doc, not
here.**

- `CAMPAIGN-EDITOR.md` — **an editor already exists** (`src/campaign/camptool/`, a Win32
  dialog app in the solution), but `CAMPTOOL` is defined only in the Debug configs, so it
  compiles to nothing in Release. One Debug|x64 build answers how much still works.
- `RENDER-LIGHTING.md` — **GT7 tone mapping landed on D3D12 (2026-09-27), staged**: the scene
  renders FP16 and GT7 runs at the 3D->2D boundary, but lighting is still LDR-authored, so today it
  only rolls off what already exceeded 1 (additive glows, specular). Vulkan is not done. Cockpit
  sun shadows exist (D3D12; the fit box was stale until 2026-09-27); no world shadows. **Cumulus shadows**
  on ground, objects and pit are written and compile but are **not flown** (2026-09-29) — see its section. The "missile shading issue" turned out to be the fin z-fight, not
  lighting — see `OBJECT-RENDERING.md`.
- `TERRAIN-AND-FILTERING.md` — **terrain seams and the flickering ring-edge lines are fixed (2026-10-10)**:
  root cause was stale posts at each LOD ring's outer edge (the strips entering the band were never
  uploaded). Also: linear-light mips and BC-encoded mips for big atlases, the Advanced-page aniso/mip
  sliders, `FarPlaneKm` (the world far plane was a hard 85 km; the haze and slider reach now follow it),
  `BubbleScale`, and the `[GPUPROF]`/`[FRAMEPROF]` profiler (Seoul: GPU ~15% busy, not GPU-bound). **Open:
  VR antialiasing landed (`VrMsaaSamples`, flown, helps); overlap
  of the two eyes is not done (command-allocator/ring audit first); a right-eye terrain dropout seen in
  debug flights was never separately explained.
- `COCKPIT-OVERHAUL.md` — the cockpit displays use a **three-size GIF+`.rct` bitmap font
  set**, separate from the `.bft` menu fonts, and `g_rttFontScale` magnifies glyph geometry
  without touching UVs — the source bitmap is a hard ceiling on sharpness.
- `CAMPAIGN-MAP-PALETTE.md` — **Israel ships Korea's colour table byte-identical**, so its
  desert quantises into a temperate palette and the map reads washed pale green.
  `TMap::ColorTable` cannot be assumed to describe the ground; the tiles can.
- `UI-OVERHAUL.md` — branch `ui-adaptive`. **The menus now fill any window** (`UiWidth/
  UiHeight/UiScale`, `-uisize desktop -uiscale 0`), adapted at load by `ui95/cadapt.cpp`: map
  screens (campaign, recon, TE) fill and pin, every other screen keeps its stock block centred
  with bars edge to edge. **`-uitest <script>` is a harness Claude runs itself**
  (`tools\uitest\run.ps1`, `sweep.ps1`): it clicks through the real mouse path and dumps shots
  and layout JSON. It deploys as `FFViper-ui.exe`, never over the rail build.
- `WEATHER-FRONTS.md` — **weather now varies across the theater** (moving fronts and
  random patches over the Setup condition, `g_nWeatherFronts`), built and deployed but
  **not flown**. Read its `FRONTS:` log lines first. The Python port in the editor must
  stay in step with `weatherfronts.cpp`; selftest pins it.
- **Fixed: saves bloated by duplicated units** (`UnitClass::SetInactive`,
  `campaign/camplib/unit.cpp`). Reactivating a unit before the campaign's list scan had
  moved it out re-inserted it into lists it was still in. The campaign UI replays a
  helicopter's past PICKUP then AIRDROP back to back, so airmobile infantry gained a copy
  on every replay. One save held 3 battalions about 43,000 times each and wrapped the 16-bit
  unit count (131,595 → 523), so it would not load. `EncodeUnitData` now also writes each
  unit once. `tools/campaign-editor/repair_units.py` fixes a save written before the fix.
- **Ships have no AI -- next candidate.** Written from the code, not flown:
  - *Movement* (`TaskForceClass::MoveUnit`, `campaign/camptask/navunit.cpp:476`): in port
    (the nearest objective within 1 km is a `Port`) with no waypoints, a ship sits still for
    ever. At sea with no waypoints, it makes itself three `WPF_REPEAT` waypoints, 20 km north
    and back (if that point is water), and patrols them for ever. With waypoints, it follows
    them, turning 45 degrees at most per step. **Nothing gives ships waypoints**:
    `NavalTaskingManagerClass::Task` (`ntm.cpp:111`) is an empty `return 0`, and the campaign
    UI has no route editing (clicking a task force opens its target list). So no ship ever
    enters or leaves port.
  - *Combat*: aggregated, the same dice combat as the ground war. `ChooseTarget` takes any
    enemy (ROE ground fire) within detection range, ship or ground unit, plus aircraft in the
    air. So ships fight each other only if a scenario starts them within range. Ship-vs-ship
    in the 3D world is unchecked.
  - *What the scenarios hold* (the editor's Units tab now has a Status column): Tiger Spirit
    (`save0`) has 15 in port (13 DPRK at their naval bases, 2 ROK) and 43 at sea. Rolling Fire
    (`save1`) has 1 in port (PRC, Namp'o) and 51 at sea. Iron Fortress is unchecked.
  - *A rudimentary AI, in `NavalTaskingManagerClass::Task`*, roughly in order of payoff:
    1. Return to the nearest friendly port when damaged or low on supply; sortie when repaired.
       First check whether being in port repairs or resupplies anything at all.
    2. Patrol boxes between friendly ports and the front's coastline instead of 20 km north.
    3. Engage: route toward a detected enemy task force within some range, then home again.
    4. Carrier groups hold a station off a friendly coast.
    5. Later: shore bombardment for a coastal ground offensive, and supply convoys between
       friendly ports.
    Precondition for all of it: a water-only route. MoveUnit heads straight for the next
    waypoint one grid square at a time. Each step is timed by `GetUnitMovementCost` (via
    `TimeToMove`, `campupd/update.cpp:678`), which should price land out for a ship, so a
    straight line that meets the coast stalls there rather than crossing it (unverified in
    game). Routes therefore need an A* over water squares. The cheap first experiment:
    let the editor write task force waypoints (it already decodes them) and watch how a ship
    follows a hand-made route.
  - *Added 2026-10-03.* **Supply never returns to a ship**: only battalions and squadrons are
    resupplied (`supply.cpp`); `CollectWeapons` (`unit.cpp` ~4934) only ever lowers a task
    force's `supply`, and at 0 it cannot fire. **Sea tankers and cargo ships are cosmetic**:
    `STYPE_UNIT_SEA_TANKER/SEA_TRANSPORT` is read only by `ntm.cpp` (an air-strike context
    label) and the map icons. Ports make national supply whether or not a ship is there.
    The in-game news line "naval ships fired on X forces" (format 1802) is one generic string
    for any ship volley, whatever it hit; campsim counts DPRK ground vehicles lost to naval fire
    at ~12 per 5 days (thousands to air). `NORD_*` orders exist and are saved, but nothing sets
    them, and TE order-setting is Battalion-only. A deaggregated ship near land halts for good
    (`gndai.cpp` ~1198). save0: 10 DPRK ships in port, no US/ROK ones.
  - *Built 2026-10-03: naval AI v0* (`navunit.cpp`, cfg `NavalAI` default 1, `NavalTankerFuel` default 50).
    A ship with no waypoints now plans a water route (A* over the Naval cost table, one waypoint per
    change of direction): warships sortie from port after a 15-120 min rest, patrol 15-45 km out, and
    patrol a triangle S -> P1 -> P2 -> S from where they are (S = the ship's position when it needs a plan; every route ends at S, so no stored station; points on a 10-25 km ring for carriers/cruisers/battleships/amphibious, 15-45 km for the rest, picked away from the nearest hostile objective; out-and-back if no second point; was random out-and-back first) (stay on station; the first version
    sent every ship home when 60+ km from a port, which was 21 of 25 Blue ships on day 0) and go to port
    only when supply < 30%, where docking resupplies them to 100; sea tankers, cargo and
    supply ships sail between friendly ports with a 60-150 min rest at each. A docked tanker adds
    `NavalTankerFuel` x (missing refinery output %) to its team's fuel pool, so it pays nothing while
    all refineries work. Why plain waypoints never sailed: `ResetCurrentWP` skips a flag-less waypoint
    whose departure has passed and `MoveUnit` waits on a future one, so only a `WPF_REPEAT` waypoint is
    chased by distance; the planner flags every waypoint. State is the waypoint list plus the saved
    `orders` byte (`NORD_TRANSPORT` = on a voyage), so no header change. Measured in campsim (save0,
    1 day, seed 1): 49 of 51 task forces moved >3 km (before: ships idle or 20 km north and back).
    **Not yet checked:** the fuel delivery (refineries were intact, so it adds 0), ships in 3D (a
    deaggregated ship near land halts for good, `gndai.cpp` ~1198), no port resupply (deliberately
    skipped until ships fight more).
  - *Player orders v0 (compiled, NOT yet clicked in the game).* Select one of your task forces (its route
    is drawn), right-click open water on the campaign map: "Send <ship> here" (`TaskForceOrderStation`,
    `navunit.cpp`) routes it over water and holds it (`orders = NORD_STATION`; `MoveUnit` returns early
    when the route is done); a ship on station also gets "Resume patrol". Items are added to MAP_POP at
    hookup (`NavalStationAttach`, `campmenu.cpp`), like Build package. Assumes the selected ship is
    `gMapMgr->GetCurWPID()`. Local only: nothing is sent for multiplayer.
  - *Map hover tooltip (compiled, NOT yet seen in the game).* The ui95 tooltip engine (250 ms,
    `C_Handler::CheckHelpText`) already asked `C_MapIcon::GetHelpText`, which only had the name label
    (needs Names on) and was not rebuilt when the mouse moved between icons of one control. Now
    `gMapIconTipHook` (`cicons.cpp`, set in `SetupCampaignMenus`) builds "name - kind xN - side" (+ supply
    and orders for ships, status for objectives) from the entity; an enemy unit the player's side has not
    identified says "Unidentified contact"; `C_Handler::ResetHelp` is called when the icon under the mouse
    changes.
    Checked in the `-uitest` harness (new commands `hovericon`, `hovership`, `rclickship`, `listships`,
    `mapmove`; scripts `hover.txt`, `hover2.txt`, `shiprecon.txt`): the tip shows for ships and
    objectives. Known defect: a tooltip box is not erased when the mouse moves on (stale fragments stay
    on the map until it redraws). A Recon on a Blue ship at campaign start opened fine in the harness;
    Andrew's report of a hang (WER 1002, no FFCrash entry) after right-click ship -> Recon is NOT reproduced.
  - *Route line + dragging (measured with the harness `curinfo`).* The map draws a non-flight unit's route from
    the waypoint BEFORE its current one (`C_Map::BuildCurrentWPList`, `cmap.cpp:1455`), not from the unit, so the
    line need not touch the ship. The waypoint control is already marked draggable for ships (`cmap.cpp:1507`), but
    the drag handler ignores them: `waypoint.cpp` MOUSEMOVE returns `if (not un->IsFlight())`. Next step for
    drag-to-edit: let it move a ship waypoint, treat the edited route as a player order (`NORD_STATION`-style
    hold so the planner does not overwrite it), and decide what happens when a dragged leg crosses land.
  - *Ship drag/drop (2026-10-03).* A dropped ship waypoint is an order (`waypoint.cpp` -> `TaskForceOrderStation`);
    a drop on a land cell within 3 km of a friendly working port docks the ship there, otherwise it snaps to the
    nearest water cell within 6 km (the map picture and the 1 km cover cells disagree at coasts). Not yet tried
    with the mouse (the harness cannot drag).
  - *Ships "disappear".* Measured, campsim save0 2 days seed 5: 9 of 58 task forces are gone with the naval AI on
    AND off (first losses at 3 h / 5 h on, 30 h off), so they are being sunk; patrolling reaches the war sooner.
  - *Hang after Recon on a ship (WER 1002 at 16:49 and 18:02; log ends with THREADSYNC lines): NOT reproduced.*
    The harness cannot see the live 3D view (two shots 8 s apart are byte-identical for a ship AND an objective
    Recon). `FFHang.log` (new watchdog in `winmain.cpp`, `FFHangWatchdog`): if the game window does not answer
    for 6 s it writes a symbolised stack of every thread. Read it after the next hang.
  - *Fixed: Recon could not be panned in the adaptive UI (2026-10-03).* `RECON_PANNER` (350x240 drag area, stock
    x 337 of 1024, so its centre is exactly 512) fell into "right half" in `PlaceEdges` (`cadapt.cpp`) and moved by
    the whole 908 px growth instead of half: x 1245 instead of 791, right of the pane centre (966). A loose
    control centred on its container (within 8 px) now stays centred. Checked in the harness: panner at 791,
    and `hold RECON_PANNER 90 1500` changes the real-window capture (`reconpan2.txt`).
  - *Fixed: voices stay silent after a campaign movie.* `PlayMovieMF` (`MovieMF.cpp`) called `F4SilenceVoices()`
    but never `F4HearVoices()`; the DDraw player pairs them. Music/effects after a movie not investigated.
- **TODO: build the JSOW the way BMS 4.32/4.38 has it.** The data rows exist in Korea's
  `FALCON4.WCD` -- 298 `AGM-154A JSOW` (BLU-97 cluster payload, kinetic, 1250 ft blast)
  and 313 `AGM-154C JSOW` (unitary penetrator, `SimDataIdx` 99) -- both with a 90 km
  campaign range. But neither is on the F-16CG's hardpoint lists, and there is no
  glide-weapon behaviour behind them (wings deploying, GPS/INS flyout to a steerpoint, the
  154A dispensing over the target). Port the model and the flyout from BMS. See
  [[freefalcon-bms-asset-porting]] (the memory note) for the known asset mismatches, then
  add it to the hardpoint lists and the squadrons' stores rows (the editor's Stores panel
  unlocks it).

---

## JHMCS helmet cueing — written, compiles, NOT flown yet

`src/sim/otwdrive/hmcs.cpp` + `src/sim/include/hmcs.h` (knobs listed in the header). What was
there before: the 2001 S.G. "HMS" — aircraft whose class `Flags` carry `0x20000000` draw a
screen-centre circle when an AIM-9 is uncaged, **and the uncaged seeker's own search is switched
off** (`mislfcc.cpp`), because the padlock view was meant to hand it the target. Nobody padlocks
in VR, so an uncaged AIM-9 on an HMS aircraft could never lock anything.

Now, while the player is in the 3D pit on an equipped aircraft (`g_nHmcs 1` = flagged ones,
`2` = all), the helmet line of sight — `OTWDriver.headMatrix` column 1, i.e. HMD / TrackIR /
mouse look alike — slaves the caged AIM-9 seeker when there is no radar target to slave to,
drives the uncaged search, and replaces the fixed ACM BORE line (`radar/modes.cpp`), so TMS-up
in a dogfight locks what you look at (the AIM-120 half). Symbology is drawn as directions at
optical infinity through each eye's own camera (`VrSetEyeCam`), so it needs no IPD term.
`g_nHmcs 0` restores the old behaviour exactly.

The HMCS knob is a hotspot on the blank plate below CMDS (where BMS 4.38 has it). The pit model
has no knob there, so `DrawHmcsKnob` (vcock.cpp) draws one at runtime through the VR-hands route —
an overlay after the pit flush, so nothing in the pit can occlude it; the plate normal is hard-coded
from pitray (`kHmcsKnobNormal`, root pit only). Lit as the pit shader lights a pit surface IN
SHADOW (`g_d3d11Amb` × sun level + cockpit fill); the sun term is left out because its shadow map
is GPU-only, so in direct sun on the plate the knob reads darker than the panel. Real pit geometry
(lodlens-style append, pointer as 4 switch-gated variants) would fix lighting and occlusion both. `SimHmcsKnobUp`/`Down` at button (−609, 664, −1182), left/right click, off → dim → mid →
bright (`g_nHmcsLevel`, cfg = position at launch). `SimHmcsToggle` is the key version.
`3dbuttons.dat` now ships in the installer (`res\art\ckptart\`, copied from the install).
`SimLogPitPoint` works on a flat screen too: one mouse click, `flat mouse … resid` line, feeds
`pitray.py` directly.

To verify first: symbols upright and not mirrored (`up` is derived as right x forward); HUD
blanking box position (`g_fHmcsHudTop/Bottom/HalfWidth`); text size in the eye
(`g_fHmcsTextScale`); `set g_bHmcsLog 1` prints LOS, seeker and radar-slave state once a second.

---

## THE LIVE TASK: recon crashes when map-detail logging is on

Opening **Recon** in a campaign crashes with `set g_bLogCampMapDetail 1` and does not with
it at `0`. One trial each way, in the field. The toggle *should* be inert: it gates one
`_snprintf` and one `FFDebugLog` in `CampMapDetailBuild` (`cmapdetail.cpp`) and nothing
else. It does **not** disable the detail layer — that is `CampMapDetail`, a separate knob.

**Ruled out, do not re-chase.** (a) A name collision with the `CampMapDetail` table entry —
`ConfigOption::CheckID` is an exact `stricmp` after the `g_b` prefix, not a substring match.
(b) A duplicated `FFDebugLog` handle across static libs — `RedViper.map` has exactly one
`s_f` and one `$TSS0` guard, correctly folded. (c) Opening the log file — `g_bLogCampProducers`
is already on and uses the same logger, so the file is open long before the map logs.

**Fixed, unproven as the cause.** The log buffer was `char ln[200]` written by an unbounded
`sprintf` whose worst case is **195 characters**. Five bytes of margin, and a smash there
would crash somewhere unrelated and only with logging on — exactly the observed shape. Now
`_snprintf` into `char[512]`. If recon stops crashing, this was it; **check, do not assume.**
The `[CENSUS]` logger in `cmap.cpp` uses the same pattern but tops out near 135 chars, so it
is not a candidate.

**Old crash reports are unreadable.** `GetRegisterString` used USER32 `wsprintf` with
`%016llX`, which has never supported `ll` — every register printed `lX` and the varargs then
slid. Fixed (two 32-bit halves, still no CRT). Every register in any report from before that
fix is meaningless. Only the fault line and code bytes were real, and they decoded to
`obj->shortAt0x10 = (short)(x * obj->floatAt0x14)` with `rdx` ≈ `0xFCA` — a small integer
used as a pointer. Nothing in `cmapdetail.cpp` has that shape. If a fresh dump points the
same way, grep for a class with a `short`/`WORD` at +0x10 and a `float` at +0x14.

**How recon reaches the code:** `C_3dViewer::StampRttIntoMenu` ends with
`gMainHandler->RefreshAll(&viewport)`, dirtying every visible window, so the campaign map
redraws during recon → `C_ScaleBitmap::Draw` → `O_Output::Blend4BitDetail`. Prior art:
commit `aa305b57` fixed a *different* recon crash (`C_3dViewer::Cleanup` freeing `m_pRTT`
between `rend3d_` and `rendOTW_`). Read it before assuming this one is new.

**Next:** get a real dump with the current build. If it still crashes, separate the two knobs
— `g_bCampMapDetail 0` *and* `g_bLogCampMapDetail 1` makes `CampMapDetailBuild` return before
the log call, so a crash there is not the map at all.

---

## Landed on this branch — the residue worth keeping

The commits and code comments carry the detail. This is only what a future session would
otherwise have to rediscover.

**Object rendering (2026-09-20).** Five object-pass
fixes landed in one session; all the knobs below default to the new behaviour and each has a
`ffviper.cfg` escape hatch. Full write-up, evidence and RenderDoc recipes: `OBJECT-RENDERING.md`.
- **Missile fin flicker — fixed** (verified in 2D; VR check pending). Thin fins z-fight; the
  first cull excluded the pit path, and the player's own stores ride it. `g_nObjCullMode`
  (1 = D3D7 parity = cull FRONT: the clip projection reflects, so cull-back kills the ground),
  `g_bObjCullPit` (1 = cull pit-list draws too).
- **F-16 external strobe flash — fixed** (awaiting a look). The port dropped the `D3DLIGHT7`
  `OwnLight`/`NotSelfLight` flags and the spot cone. `g_bObjLightMasks` / `g_bObjSpotCones`.
- **F-16 light-strip "red patches" — fixed** (awaiting a look). The port *replaced* the material
  with the emissive colour; D3D7 *adds* it. No knob — this is the corrected semantics.
- **Per-pixel object lighting — landed**, user says it looks better; VR cost unmeasured.
  `g_bObjPixelLight` (0 = legacy per-vertex). The light model is one shared function per
  backend (`FFObjectLighting`).
- **Canopy/glass surfaces were unlit — fixed** (awaiting a look). The sorted-alpha path
  (`DrawSortedAlpha`) never restored the object-pass flags, so alpha model surfaces drew with
  whatever 2D state ran last (no `FF_LIGHTING`) — the glass never responded to outside light.
  `DrawSortedAlpha` now calls `BeginObjectPass()` per item.

**Lamp sources — fixed (2026-09-21), accepted in flight.** Three port bugs, none of them art.
(a) D3D7 armed `EMISSIVEMATERIALSOURCE = D3DMCS_COLOR2` on **every** surface and disarmed it only
for a `SwEmissive` surface whose switch was off; the port had that inverted, so every lamp the
models author on a plain surface lost its colour. (b) Each light's `dcvAmbient` was dropped — the
term with no `N·L`, which is what lights a lamp's own housing and `POINTLIST` lamps (zero normal).
(c) `DrawSortedAlpha` calls `BeginObjectPass()` per item, which rebuilds the shader flag word and
drops `FF_TEXTURE0`; `DrawSurface` only re-binds a texture when it *changes*, so any sorted-alpha
item reusing the previous texture drew untextured — alpha-shaped sprites became opaque flat quads.
That was the wingtip "grey box", and it hit **every** textured surface on that path, not just
lamps. Two traps worth carrying: the emissive must be added to the lit material **before** the
texture stage (past it, a textured lamp saturates to white), and `m_SurfacePit` is the whole pit
**path**, which carries the player's wings and stores — gate on *untextured* pit surfaces, not on
"pit". Knobs `ObjEmissiveAll`, `PitEmissive`, `LightAmbient`, `LightFalloffReach`; `LightSprites`
now defaults 0. Measured model data, the RenderDoc recipe and the dead ends: `LIGHT-SOURCES.md`.

**Campaign planning.** The candidate filter must use `FalconLocalSession->GetTeam()`, never
`gSelectedTeam` — that is the TE editor's variable and it holds a *country*. `PACKAGE_WIN`
was always loaded by `CMN_SCF.LST`; do not "fix" it by loading it again. The campaign must
open with the **takeoff** lock closed, not the TOT lock, or `tactical_make_flight` pins every
flight to an exact second over the target and the planner rightly refuses.

**Campaign map detail.** Water posts carry colour index 0 and `ColorTable[0]` is pure white,
which is why the ocean used to be a flat white field; index-0 posts now take their tile's
average colour. Needs `CampMapFromTerrain 1`. The window recomposites whenever the source
rect moves — not pre-optimised; if panning stutters, the fixes are a coarser zoom tier or
deferring the rebuild to mouse-up.

**JFS and the ramp start.** The switch was never missing from the model, it was never being
*drawn*. Its two branches are **reversed from their numbering: branch 0 is thrown to START,
branch 1 is OFF** — confirmed in the cockpit, not from the geometry. The accumulator only
recharges above `jfsMinRechargeRpm`, so with the engine stopped you get **one JFS start per
engine run**; a second press refuses silently and looks exactly like a dead switch. The green
run light needs MAIN PWR — the whole indicator block in `VCock_Exec` is skipped when
`mainPower == MainPowerOff`. Nothing clears `EngineStopped` on its own: with
`g_bUseAnalogIdleCutoff 0` only `SimThrottleIdleDetent` does it, and only above throttle 0.1
with rpm ≥ 0.20; with it `1` the engine lights itself once rpm ≥ 0.20. `set g_bLogJfs 1`
prints the decision behind each press.

**Avionics levers.** Sixteen of them drew their position from `HasPower()` — *is this box's
bus live* — instead of `PowerSwitchOn`, the raw bit the callbacks write, so on a cold jet the
lever snapped straight back every frame. If one still does nothing, check the Avionics
setting: `SimSMSOn` and siblings open with `if (not g_bRealisticAvionics) return;` and the 3D
button dispatch plays the click either way.

**Controllers "Assign".** A `.scf` in `res/art/` plus a `<File>` line in the `.wxs` is **not
enough — a window is inert until some `*_scf.lst` names it.** `buttonassign.scf` shipped for
years and was never listed, so `FindWindow` returned NULL and the click silently did nothing.
Control ids do not read like their roles: OK = `BTNASSIGN_ASSIGN`, Cancel = `BTNASSIGN_OPEN`,
Redetect = `BTNASSIGN_DETECT`. `BTNASSIGN_DEVICE_LIST` is absent on purpose (the dialog stays
locked to the device of the cell that opened it). The device-cell reuse branch was made
idempotent in passing — **that was not the bug.**

**VR screenshots.** The desktop back buffer is useless in VR — the eyes go straight to the
compositor and the back buffer only holds the clear. `EndEyeFrame` now claims a pending
request and reads the eye directly. Two things that cost a round trip each: **the eye image
is TYPELESS** (`R8G8B8A8_TYPELESS` is not a legal placed-footprint format — copy with the
concrete UNORM member, and a capture that declines here writes *nothing*, indistinguishable
from a dead key); and **"pretty" must be queued from inside `DisplayFrontText`**, because its
EXECUTE frame is the clean one and the usual queue point is after both eyes are submitted.

**Campaign teardown heap corruption.** `O_Output::SetScaleImage` allocated `Rows_`/`Cols_`
only when still NULL, sized *from the image* — so a second, larger image kept the first one's
size while `SetScaleInfo` filled to the new extent. `cmap.cpp` does exactly that, and the
terrain-built map is the larger. Fixed by reallocating for whatever image it is handed. The
`F4IsBadWritePtr` guard it replaced never worked: it only refuses writes to an *unmapped*
page, and it asked about `sizeof(short)` on an array of `long`. **The lesson:** a "double
free" verdict names where damage surfaced, not where it was done — read the reported site as
a symptom and go looking for a writer. An earlier `delete` → `delete[]` fix is correct C++
and is still in, but it was **not** the fix.

**Nosewheel steering and the `IsDigital` trap.** `MakePlayerVehicle` **clears `IsDigital` on
the player's airframe**, so any condition testing it is false on the player jet. That caused
two separate bugs on this branch (the JFS switch and NWS). Treat every `IsSet(IsDigital)` as
suspect. `SimNWSToggle` is new — FF6 had no NWS control at all — and defaults ON.

**3D kneeboard.** Board on the right thigh, three pages, click to cycle, its own font
(`g_nKnee3DFont`, default 2 = 10x7). Placement and blend live in the `kneeboard` line of
`3Dckpit.dat` — data, no rebuild. It dragged up four bugs that were not kneeboard bugs; see
the RTT section below.

---

## Facts worth not re-deriving

### Terrain

Measured against the shipped Korea data with `tilesurvey.py`. Re-run it for any other theater.

- LOD 0 is 4096×4096 posts at **819.995 ft/post**; `Theater.map` flags `0x1`, so posts are
  9-byte `TNewdiskPost`. Blocks deduplicated (47,128 stored of 65,536).
- **A tile spans 4×4 posts**, not one — 16,384 of 16,384 sampled groups share a texID. One
  tile = 3,280 ft. DXT1, 256² (809) or 512² (278), H resolution only.
- **`v` is inverted**: `DiskblockToMemblock` has `u` increasing with the column but
  `v = stop - ...` decreasing with the row, so a cell's first post row is at the *bottom* of
  its tile. Measured against the column axis as control: column seams 2.10× interior detail,
  rows sampled like columns 2.81×, rows inverted **1.98×**.
- Quantising tile pixels into `TMap::ColorTable` costs mean RGB error **9.6 of 441** (p99 26).
  That is why the map can stay 8-bit and every overlay keeps working.
- **`MaxZoomLevel_` is 64** source pixels across the view for Korea, not the 32 floor in
  `SetupMap` — the floor never bites. Max zoom 8.6 nm.
- Tile reuse is extreme: 1,071 distinct tiles theater-wide, one of them 49.4% of the ground,
  top 200 = 95.2%. A small decoded-tile cache goes a long way.

**Other theaters are not Korea.** Both of these had already bitten by the time Israel was
installed, and both are in `CAMPAIGN-MAP-PALETTE.md` in full:

- **texID width varies.** Korea puts res at bits 12–15 and nothing above, so its texIDs fit
  in 16 bits. Israel widens the set field and moves res to bits 16–19, so every one of its
  texIDs is `0x20000` or more. Never treat a texID as a bounded index — key on the
  `(set, tile)` pair. `TextureDB::ExtractSet` is `(texID >> 4) & 0xFF`, eight bits; Israel
  declares 259 sets but references only up to 248, so it works by luck. A theater
  referencing set ≥ 256 would alias, in the stock engine as much as anywhere.
- **`ColorTable` need not belong to the theater.** Israel's is **byte-identical to Korea's**,
  all 256 entries — a 2002 `.map` against 2011 terrain.
- Theater-derived caches are keyed to `FalconTerrainDataDir` and dropped when it moves.
  Before that, loading a second theater in one session kept showing the first one's map.

### Terrain tile mods (JSGME) — BMS 4.32 Korea, built 2026-09-26

JSGME lives in `C:\FreeFalcon6` with its `MODS\` folder beside it. A mod is a folder that
mirrors the install root, and enabling it copies its files over the game's, backing the
originals up in `MODS\!BACKUP`. The Korea texture path is the same in both games, so the
mods are straight overlays.

| Mod | Contents | Size |
|---|---|---|
| `BMS 4.32 Korea Tiles` | `terrdata\korea\texture\texture.bin` + 2,306 tiles in `texture\` | 179 MB |
| `BMS 4.32 Korea FarTiles` | `terrdata\korea\texture\farTiles.dds` + `FArtILES.PAL` | 48 MB |

Rebuild (idempotent; overwrites the two mod folders — disable them in JSGME first):

    python tools/terrain/bms_tiles_mod.py --bms "L:/Falcon BMS 4.32/Data/Terrdata/korea/texture" --ff C:/FreeFalcon6 --theater korea --name "BMS 4.32 Korea"

**Why it is compatible**

- **Tile names.** 4.32 is a strict superset of FF6's tiles: 2,174 shared plus 132 new ones
  (Kunsan, Gimpo, training sets). All of the shared art differs. Every FF6 tile gets
  replaced, and none goes missing.
- **`texture.bin`.** 4.32's file is FF6's 113 sets in the same order, plus 6 appended sets,
  in the same Falcon 4 format. FF6's terrain map indexes by set and tile, so the existing
  map still points at the right tiles. The 4,360 trailing bytes are never reached by FF's
  reader. The paths/areas (roads, rivers) are richer.

**The conversion**

- **Why tiles need converting.** `graphics/texture/terrtex.cpp` tags **every** tile DXT1
  ("MUST BE DXT1"). Of 4.32's tiles, 1,985 are DXT1, 320 are DXT5 and 1 is DXT3. Copied
  as-is, those 321 would render as garbage.
- **The method.** A DXT3/5 block is 8 bytes of alpha followed by a DXT1-style colour
  block, and terrain has no alpha, so the script drops the alpha half.
- **The trap.** DXT1 reads a block with endpoints `c0 <= c1` as 3-colour + transparent,
  while DXT3/5 always read 4-colour. The script handles both cases:
  - `c0 < c1`: swap the endpoints and flip each index's low bit (0↔1, 2↔3).
  - `c0 == c1` (a flat block): set every index to 0.
- **Other details.** Every mip level is converted. The header's linear size becomes the
  DXT1 top level. Mipmaps and 128/1024 sizes load fine and are copied untouched.
- **Verification.** Decoded against the source, the result is pixel-identical: worst diff 0
  over 217k blocks, 26 of them endpoint-swapped.

**Limits and risks**

- **FarTiles is the risky mod, so it is separate.** 4.32 regenerated every far tile
  (96,749 records against FF6's 96,664, none identical), and its order against FF's terrain
  far-tile ids is unverified. Enable it on its own and look for patchy or mismatched
  distant terrain. If you see that, disable it; the near tiles do not depend on it.
- **No new airbases.** Kunsan and Gimpo will not appear: that needs 4.32's terrain map
  (`Theater.map`/`.o2`/`.l2` and friends) and the campaign objectives, not just tiles.
- **Korea only.** The Israel theater is untouched.
- **Campaign-map caches (not checked).** It is unverified whether the campaign-map and
  campaign-editor tile caches pick up swapped art without being deleted.
- **Not flown yet.** Neither mod has been enabled in game.

**BMS 4.37 Korea Tiles (built 2026-09-26, judged not worth it).** Compared against 4.32 on the offline renders, it looked clearly worse (patchwork farmland, lost roads). Kept for the record; use 4.32. Built from `C:\Falcon BMS 4.37`, the
"Polak" Korea. It is a different kind of mod from 4.32's:

- **Its set list is reorganised.** `Texture_Polak.bin` has 250 sets and 3,471 tiles. Only 9
  of FF6's 113 sets match by position, and only 50 exist anywhere in it. Using it would
  scramble FF's terrain map. Its terrain is new too (`TERRAIN_POLAK.*`, L2 only).
- **The mod keeps FF6's `texture.bin`** and swaps images only. Build it with
  `--keep-bin --tiles-dir Texture_Polak --no-far`; the full command is in the script's
  docstring.
- **The tiles still fit FF's map.** Every one of FF6's 2,174 tile files, day and night
  (`*N.dds` is the **night** tile, not a normal map), exists under the same name.
  Rendering FF's L0 map with the three tile sets (scratch `mosaic.py`), coastlines, towns
  and rivers line up in 4.37 as in 4.32.
  - Coast-tile shape correlation against FF6: 4.37 median 0.47, 4.32 0.72, random 0.01.
  - Seams are more visible: boundary/interior step 1.7, against 0.97 for 4.32.
  - The farmland reads as a checkerboard, because 4.37 art varies more tile to tile.
- **Roads painted into FF6's tiles are mostly gone in 4.37's.** BMS draws them elsewhere,
  so expect fewer visible roads.
- **One blank placeholder.** `HFARMD99` is white in 4.37. `is_blank` catches it (and nothing
  else of the 2,174), and the builder takes 4.32's version instead.
- **Conversion.** 302 DXT5/DXT3 tiles converted; the result is 2,174 files, all DXT1, 350 MB.
- **4.32 and 4.37 mods overlap file for file.** Enable only one; JSGME will warn about the
  conflict.

**Tile size limit: 512 px (found 2026-09-26).** `TextureHandle::Load` (`graphics/texture/tex.cpp`)
builds the CPU mip chain the terrain needs only when `w <= 512`. A 1024 tile is uploaded
**single-mip** and will shimmer at range. The loader also reads only the file's top level
(`dwLinearSize` bytes), so a mip chain in the file is ignored.

- **Existing casualty:** the 66 tiles 4.32 ships at 1024 already hit this.
- **To go past 512:** raise that gate, which costs RGBA8 video memory (about 5.3 MB per
  1024 tile), or upload the file's own BC1 mip chain.

### Terrain tile upscaling (experiment, 2026-09-26, not flown)

**Tool:** `tools/terrain/tile_upscale.py`, per tile:

1. Decode, converting DXT3/5 first.
2. Reflect-pad 16 px and run a 4× Spandrel model, then crop the pad off.
3. Resize to `--size` with an antialiased filter.
4. **Back-project:** iterate until the result's downsample equals the source tile. The model
   adds texture but cannot move a coastline or open a seam.
5. Encode DXT1 with `nvcompress -bc1 -production`, with a mip chain.

Night tiles are copied unchanged; the loader sizes them separately.

**Environment, outside the repo** in `C:\Users\Andrew\.ffupscale\` (not `AppData\Local`: this
app virtualises that into its private package cache):

- `venv\` holds torch 2.14 cu130 and Spandrel. Run the tool with `venv\Scripts\python.exe`.
- `models\` holds Phhofm's `4xNomos2_realplksr_dysample`, `4xNature_realplksr_dysample` and
  `4xBHI_dat2_real` (GitHub releases, CC-BY-4.0).
- `nvtt\` holds `nvcompress` from the NVTT 3.2.5 install. The installed copy lacks
  `nvtt30205.dll`, so `C:\FreeFalcon6`'s copy of that DLL was added.

**Trial:** 108 tiles (the coast and farmland regions), 4.32 art, run through all three models.
- Speed on the RTX 5080: RealPLKSR about 20 s per 108 tiles; DAT2 about 95 s.
- All three keep colour and shape. The gain is modest: sharper tree clumps, field edges and
  rock, but the 4.32 art's DXT block artifacts get sharpened too.
- DAT2 had the cleanest edges, with the fewest halos.
- Comparison script and images: scratch `trial_compare.py` and `*-upscale-compare.png`.

**Built:** `MODS\Upscaled Korea Tiles (4.32 + DAT2 512)`, a standalone replacement for
`BMS 4.32 Korea Tiles`:
- It carries 4.32's `texture.bin` and night tiles, plus every day tile through DAT2 at 512.
- 4.32's 1024 tiles come down to 512, which gains them their mips.
- Enable it **instead of** the 4.32 mod, never both.

**Also built:** `MODS\Upscaled FF6 Korea Tiles (DAT2 512)`. It holds FF6's own 1,087 day tiles
through DAT2 at 512 (183 MB, 31 min), over FF6's own `texture.bin`; the night tiles are
untouched. The comparison (scratch `ff6_compare.py`) shows **almost no visible gain**. FF6's
field tiles are smooth, low-detail photo patches with little for a model to sharpen, and their
hard tile-to-tile colour jumps stay: land seam ratio 12.5 → 9.8, against 2.5 → 1.9 for 4.32.
The 4.32 art is the better base; upscaling does not close the gap for FF6.

**Ideas if the look is not enough:**
- Deblock first with a 1× model (`1xDeJPG_realplksr_otf`) so the blocks are not sharpened.
- Try 1024 after the mip gate is changed.
- Add a terrain detail texture in the shader. That is the real fix for low-altitude sharpness
  and tile repetition.

### The 3D pit object

- **The live pit is parent 2402**, LOD `3DPIT_F16CJ_L1`, set by `cockpitmodel 2402` in
  `3Dckpit.dat` — not by `VIS_VRCOCKPIT`. It declares **255 switches**; the three older pits
  (1, 870, 2403) declare 129, so every switch id above 128 is silently dropped there.
- **A switch is a run of sibling DOF nodes** sharing a `SwitchNumber`. The engine draws the
  first branch where `SwitchValues[n] & (1 << branch)`, so **mask 0 draws nothing at all** —
  that is what "the part is missing from the pit" usually means.
- 150 switch numbers are modelled of the 255 declared. Check before assuming a control exists,
  and before modelling one that already does.
- **`3dbuttons.dat` coordinates are model units × −569** (its own header says so).

### The RTT display canvases (3Dckpit.dat)

- **Three coordinate frames.** Canvas points are in a **pilot-eye** frame: x forward, y right,
  **z down**, one unit = `1 / RTT_POSITION_SCALING` ft = **1.159 in**. `3dbuttons.dat` is the
  same frame **× −56.9**. The BSP model is **canvas / 10** exactly — calibrated by matching
  switch 138 (the CAT lever, model 1.37/−1.31/1.71) to its hotspot (canvas 14.01/−13.09/16.99),
  and cross-checked against the HUD combiner (5.5 units = 6.6 in wide, the real figure). Note
  the engine places panels at canvas/**10.35**, so every panel sits at 96.6% of its intended
  distance from the cockpit origin — a ~0.8 in static offset, not drift.
- **A quad can pitch but cannot roll or yaw.** `DrawRttQuad` builds the fourth corner as
  `(ll.x, ur.y, ll.z)`, so `ul`/`ur` must share x and z, and `ul`/`ll` must share y.
- **The blend char decides whether it reads as a display or an object.** `c` →
  `STATE_CHROMA_TEXTURE_GOURAUD2`, rewritten by `DrawRttQuad` to the additive emissive
  composite — right for symbology, and drawn *over everything*. `t` → `STATE_TEXTURE`: opaque
  and **depth-tested**, so the pit occludes it. The depth buffer still holds the cockpit at
  composite time — the `ClearZBuffer()` just before `VCock_Exec` is a genuine no-op.
- **A depth-tested composite needs real depth.** The 2D screen path emits `sz = 0.0`, which
  under reversed-Z is the **far plane**; `DrawRttQuad` used to request depth-from-q only for
  the HUD-occlusion case, so a `t` canvas failed the depth test against everything and drew
  nothing. Now derived from `FFMapState(...).depthTest`.
- **`NEAR_CLIP_DISTANCE` (clipflags.h) is 1.0 ft while `ContextMPR::ZNEAR` is 0.2.** Nothing
  in the pit had ever been within a foot of the eye (HUD/MFD/DED all ~2 ft). Worse,
  `polylibclip.cpp:39` does not reject — it **pushes the vertex out to** `NEAR_CLIP_DISTANCE`,
  so a near corner jumps and the quad folds along the diagonal `DrawSquare` splits it on.
  Suppressed for RTT quads in front of the real near plane. **Still open globally** — see
  Known open.
- **The atlas is the scarce resource.** 768×768 base, hardcoded (`rttTarget` in the dat is
  parsed then overwritten with `768 * g_rttSS`). Stock occupancy 55%; the kneeboard took the
  last usable portrait block, `432 452 662 766`. Zones *and* the font scale are both multiplied
  by `g_rttSS`, so a zone's **base** size sets apparent text density and SS only buys sharpness.
- **`Render2D::Render2DTri` does not clip, it REJECTS** — any vertex outside the viewport drops
  the whole triangle (`render2d.cpp:301`). A vertex at exactly ±1.0 lands exactly on
  `rightPixel`/`topPixel`, so a full-zone fill drawn at ±1.0 is a coin flip. Inset it.
  `Render2DLine` has no such test, which is why symbology never hits this and fills do.
- **Batched 2D primitives need an explicit `context.FlushPending()`** while the atlas is still
  bound. Lines, tris and text only batch into the context vertex buffer and flush lazily on the
  next `SelectTexture1`/`RestoreState`/`EndDraw`; under D3D12 nothing in a page draw does any of
  those, so they reach the eye only when something else happens to flush — seen as flicker.
  `Render2DBitmap` is immediate (`DrawTL`) and never flickers, which is the tell.
- **`Render2DBitmap` puts a CPU raster into the atlas** with no new machinery: during the RTT
  pass `AdjustRttViewport` sets the screen metric to the whole atlas, so its destination is in
  atlas pixels (`VirtualDisplay::GetRttRect`). It creates and destroys a temp texture per call
  — a per-frame upload. See Known open.
- **`CockpitManager::Exec` runs in `Mode2DCockpit` only**, so no `CPObject` ticks in the 3D pit;
  anything shared with a 2D cockpit object must be driven from `VCock_Exec`. And **`VCock_Init`
  runs before the `CockpitManager` exists**, so anything needing the manager is built on first use.
- **When a panel does not appear, change ONE variable.** Moving the kneeboard *and* switching
  its blend in the same test proved only that some combination worked, and pointed at placement
  when the cause was the blend.

### Kneeboard map page

- **`InvertRGBOrder` masks off the alpha byte** — it keeps bits 0..23 and swaps R with B. Fine
  for the 2D board, whose `Compose` is a straight blit; fatal for the 3D one, whose
  `Render2DBitmap` draws with `BLEND_ALPHA`, so the whole map composited fully transparent.
- **`pixelMag` was computed and never applied.** `UpdateMapDimensions` sizes the world window as
  `0.5 * width / pixelMag * ...` — it *assumes* the source is magnified on the way in — but the
  rasteriser copied 1:1, so the page showed `pixelMag` times more world than the route overlay
  was scaled for and the map could never line up with its own waypoints. Affected the 2D board
  identically, and is now fixed for both.
- The row/column offsets were dividing by the **swapped** axis scales. Invisible on Korea (square
  map, square theater, both 0.999737) and wrong anywhere else.
- `art/ckptart/KneeMap.gif` **does not exist** in the install; only `campaign/<theater>/Kneemap.gif`
  does, which is the first path tried. A theater without one falls through to a missing file and
  then divides by `image.height` = 0.

### The sky

- **`RenderOTW::DrawSky` returns immediately after `DrawSkyDome()` when `g_b3DSky`** (default
  true, #96). So `DrawSkyNoRoof`/`DrawSkyBelow`/`DrawSkyAbove` — and every knob they read,
  including `HorizonFillerExtend` — are **dead code in a stock build**. Check the dispatch
  before tuning anything in the 2D sky path.
- The dome replaced the 2D sky wholesale but the **horizon filler was not drawing sky** — it was
  covering the near/far terrain seam with ground haze "instead of a black contour stripe".
  Nothing inherited that job, so the stripe was exposed at altitude (visible ~40k ft, gone by
  30k, black, undulating with the terrain). `DrawHorizonFillerOverDome` now runs just the filler
  over the dome.

### ui95 / the Controllers table

- `ui95` draws a control at `control.x + VX_[client]` and clips to that client, and the scrollbar
  clamps `VX_` *upward* to `ClientArea.left` — so a control's x is measured from its own client's
  left edge. That is the whole trick behind the frozen columns: client 4 is the frozen strip
  (FUNCTION + KEYBOARD), client 2 the scrolling strip with the device columns re-based to x 0.
  `WIN_MAX_CLIENTS` is 8.
- Client 4 has no scrollbar — that is what stops it drifting — so its vertical position is
  mirrored from client 2 once a frame. **Do not hook the scroll call sites instead:** the wheel
  and the slider drag are handled inside ui95, which writes `Parent_->VY_` directly and never
  calls back.
- A header belongs to the client its own column lives in. One shared client is what made FUNCTION
  and KEYBOARD scroll out over the device headers.
- `g_keyListPreserveScroll` must be set by anything that rebuilds the list without changing the
  row set — binding, and all three clear paths. Only a search/filter should scroll to the top.

---

## Known open

| Item | State |
|---|---|
| **CPU near-clip lowered to 0.2 ft (2026-09-27, compiles, not flown)** | `CpuNearClip()` (`graphics/include/nearclip.h`) now drives both `NEAR_CLIP_DISTANCE` (BSPlib) and `NEAR_CLIP` (Render3D), default 0.2 = `ZNEAR`; `set g_fCpuNearClip 1.0` restores the old clip. **Correction to the old note:** DX-engine models (pit, aircraft, stores) never reach these clippers (`StateStackClass::DrawObject` hands them to the GPU), so it was never "all BSP geometry" -- only the CPU-transformed Render3D prims (RTT quads, cursors, padlock boxes, lens flare, trails, sky/weather bits). Watch for anything odd drawn very close to the eye in VR. |
| **Israel 3D pit: root geometry + Israel art (WIP)** | Israel's `3Dckpit.dat` loads parents **4061/4065** (LOD 5296/5300), not 2402. Same pit shell and exterior as the root pit (vertex extents identical; the parent bboxes are stale) and the **same atlas layout** — 3 of 11 textures byte-identical — but its art is far better (weathered 2048² DXT3 panel atlas vs the root's flat print) while its geometry is an older revision: no eyebrow warnings, AOA indexer or AR lights (switches 2, 101–106, 109–115) and no MRK BCN. Plan: append the patched root LOD 4105 into Israel's `.Dxl`, set its 11 texture ids to Israel's (slot order matches), repoint LODs 5296/5300, raise the parents' nSwitches/nDOFs 253/161 → 255/163; per-LOD revert log as in `lodlens.py`. Reverse (Israel art into root) needs a check that those texture ids are not shared and whether the DXH texture table stores sizes. |
| **Aircraft break-up on destruction (restored 2026-09-26, compiles, not flown)** | (1) `SfxClass::TryParticleEffect` (`sim/otwdrive/sfx.cpp`) leaves `SFX_SMOKING_PART`/`SFX_FLAMING_PART` alone when they carry a model (`baseObj`), so the part stays a falling, spinning, bouncing model. (2) `RunExplosion` (`sim/aircraft/damage.cpp`) sends its four parts again; slots with no piece model (`visType[2..5]` <= 0 or out of range, or no drawable) are skipped and the entity freed, so nothing leaks. (3) Parts trail RV smoke while they move — `TRAIL_BURNING_SMOKE2` for the burning piece, `TRAIL_BURNING_SMOKE` for the rest (the same trails a damaged jet uses); the trail is killed when the part comes to rest or dies (`TrailNew` now default-initialised in `sfx.h`). (4) `CreateDamageF16Effects` returns 0 unless `VIS_CF16A` really has the break switches (> 5), so F-16s fall through to the generic parts instead of skipping them. Kill puffs, sounds and the 1-in-3 chance that a smoking part lands and smokes for 90 s are Falcon 4.0's own. **To check in game:** shoot down a jet (any), watch from the chase/external view: four pieces should fly off with smoke and explode or settle. The pieces are mostly shared F-4E/F-15E part models, so on some types they will not match the airframe. **Build** (no member added; `sfx.h` only gained initialisers). |
| **BMS 4.32 assets worth taking — done 2026-09-26 (4.38 preferred)** | **Swapped to 4.38**, each levelled to the file it replaces (backups `.bak-pre438`): `AbInt` ← `F16SndAbInt1`, `TlScrape` ← `TailScrape` (peak-guarded, −1.3 dB under target), `touchDn` ← `Touchdown`. `NearTurbineExt` stays 4.32 (4.38 has none; external only). **4.38 airflow, ids 321–325**, driven by `CockpitSounds`: `AirflowCanopy` (q from 80 kt, full 500, pitch with it), `AoaLowSpeed` (6→12° × M0.45→0.65), `AoaHighSpeed` (4→9° × M0.72→0.84), `GearWind` (gearPos × 80→250 kt), `WindInCkpt` (canopy up, by groundspeed to 120 kt). FF6 plays `SFX_WIND` only in outside views, so the pit had no air noise at all. **4.32 calibre guns, ids 326–349** (4.38 has none): six groups × {ext loop, ext end, int loop, int end}, ext linked to int; each burst split at its −6 dB point into a crossfaded loop + the decay tail, levelled to −6 dB mean (`sounds\guns\`). `GunCalibreSfx` (guns.cpp) reads the calibre from the weapon name, skips rotary guns (M61, GAU-8/12, GSh-N-30, Phalanx, M134), caches per type. Aircraft use it only while their data keeps the stock Vulcan ids 25/26/27 (`doweapon.cpp`); ground tracer guns use it in place of `SFX_MCGUN` (`ground/weapon.cpp`). **Clickable (3 F-16 `3dbuttons.dat`, backups `.bak-bms432`)**: `SimDigitalBUP`, `SimFLCSReset`, `SimFLTBIT`, `SimOverHeat`, `SimGndJettOn/Off`, `SimRetUp/Dn`. Positions derived from FF6's own pit: the switch's spot in texture 912 traced through the model UVs (smallest-UV-area triangle wins — large wrapping surfaces contain every texel) to the 3D surface, ×−569; validated on ALT FLAPS (15), LE FLAPS (25), SYM wheel (15 units). HUD brightness was already clickable (`SimSymWheelUp/Dn`). Not done: `SimRadarGainUp` (control ambiguous), `SimPickle` (stick), `touchdn2/3` + `cnpyhndl` (4.38 preferred). |
| **BMS Korea terrain tiles (JSGME mods, built, not flown)** | Three mods in `C:\FreeFalcon6\MODS`: 4.32 `Tiles` (safe: superset by name, same `texture.bin` set order), 4.32 `FarTiles` (risky: regenerated, order vs FF's far-tile ids unverified — enable separately), and 4.37 `Tiles` (images only over FF's own `texture.bin`; fits the map but looks worse than 4.32 — rejected, don't use). Enable one tile mod at a time. Details: *Facts → Terrain tile mods (JSGME)*. |
| **Cockpit sound mix: BMS layers vs FF's engine (2026-09-26)** | FF6's in-pit F-16 engine sums to about **−27 dB** (`Int.wav` −46 dB mean; `EngRumbleInt` carries it), BMS 4.38's to about **−6 dB** — BMS's mix runs ~21 dB hotter (afterburner ~14). Everything imported at BMS's *table* volumes therefore played 14–21 dB too loud ("super loud engine noise" was mostly the airflow/AoA/gear-wind layers). `tools/audio/sndtbl_shift.py --offset -1800` shifts those 17 rows (ECS, breathing, buffet, wheel brake, canopy latch, seat motor, the five airflow layers) from their recorded BMS baseline (`f4sndtbl.txt.bms-baseline.json`, so re-runs never stack). Clicks, guns, touchdown, tail scrape and `AbInt` were matched to FF files and are not in it. Another pass had cut `airflowcanopy.wav` itself by 12 dB and nudged `Int.wav` +2 dB; the airflow file is restored to BMS level (`.bak-othertrim`), `Int.wav` left as is. Tune with one number. |
| 3D kneeboard map uploads every frame | `Render2DBitmap` → `DrawBitmap2D` creates and destroys a temp texture per call: ~2.7 MB/frame at the stock 3× supersample. The raster itself is throttled (the map window is fixed for a mission). Fix if it costs frames: a persistent `TextureHandle` + textured fan, as `RenderGMComposite::DrawComposite` does. |
| Half the farthest terrain ring has no texture | `L3 lod=4 posts=304130 tiled=154040 noSrv=150090` — 49% of the far ring has no SRV resident (L2 is 16%). `tex=0` on those rows means `lod > LastNearTexLOD()` (far texture set), not "untextured". The far ring is the horizon. |
| **Rail from OSM: converter + editor overlay (2026-09-27, branch `rail-tracks`)** | No theater ships any rail data, so `tools/campaign-editor/osm_rail.py` builds `rail.json` (campaign km, x east / y north) from OpenStreetMap into the theater terrain folder. Falcon's own lat/long (`ApproxLatLong`) is 45-195 km out in Korea (map drawn ~1.25x wide E-W), so the projection is fitted: affine to airbases, then refined against the terrain shoreline (median 1.06 km). **Nine named lines are in `C:\FreeFalcon6\terrdata\korea\rail.json`** (2026-09-28): Gyeongbu, Honam, Gyeongui, Pyongbu, Pyongui, Pyongra, Kangwon, Pyongdok, Manpo (about 4,000 km, double track counted twice). Known gap: about 5 km through the DMZ between Gyeongui (ends at Dorasan) and Pyongbu (ends short of Kaesong). **Trains v0 (2026-09-28)**: `railnet.cpp` reads `rail.txt` (stitched routes) and runs one Supply battalion flagged `U_TRAIN` per route in `g_sRailTrainLines` (default Pyongbu), hub to 10 km short of the front and back; position is a function of game time, used by both `BattalionClass::MoveUnit` and `GroundClass::Exec`. **Confirmed in game 2026-09-28** (Rolling Fire): 9 routes loaded, train 110 spawned for team 6 on the Pyongbu Line, 149 km hub-to-front run. **Seen riding the line in 3D in a TE (2026-09-28)**; a hand-placed Supply battalion on the Kangwon Line enlisted and ran (112 km run). `g_bRailTrains 1` and `g_bRailMapAllTrains 1` are in `C:\FreeFalcon6\ffviper.cfg` (old exe/pdb/cfg kept as `*.bak-pre-rail`). **Build from the worktree** (`..\freefalcon-rail\FreeFalcon.sln`): a `main` build copied into the install drops all of this. **Campaign map "Rail lines" toggle (2026-09-28, compiles, not seen)**: routes over the live layer, train markers (own side; all with `RailMapAllTrains`); the markers only move when the overlay is rebuilt (toggling any layer). The tick also runs in tactical engagements now. **Train class + rail supply (2026-09-28, links, not seen in play)**: a "Train" unit class (Supply sptype 20) in the Korea FALCON4.ct/UCD and teunits.lst via `tools/campaign-editor/install_train_class.py` (files backed up as `*.bak-pre-train`); trains identified by class; each railhead arrival hands `RailTrainLoad` supply+fuel from the national pools to battalions within `RailRailheadKm`, logged as `rail: ... reached the railhead`. Design, answers and phases: `RAIL.md`. Editor map: **Rail** / **Rail fit** buttons. See the editor README, "Where the rail comes from". |
| **Damage feeds route cost (2026-09-27, compiles, not flown)** | `GetObjectiveMovementCost` scales the ground cost of entering a damaged road/junction/rail node (up to x2) or bridge (up to x4) by objective status; `PathDamageCost` (100, 0 = stock). Watch whether columns detour around a half-dropped bridge. See `CAMPAIGN-SUPPLY-ENGINE.md`. |
| **Objective icons shaded by health (2026-09-27, compiles, not seen)** | Objective icon sets darken linearly with status down to `CampMapIconMin` (0.35); `CampMapIconHealth 0` = stock. Two stock bugs fixed on the way: `AddObjective` seeded every icon with status 0, and `C_MapIcon::UpdateInfo` never stored a status change for an icon that had not moved -- so objective icons never learned their status at all. Darkening uses UI95's own blend with back = 0 (`O_Output`: negative `bperc_`). |
| Pale circles on the campaign map | Pre-dates this work. Ruled out: Logistics overlays, threat rings (`ShowCircles` is inside `#if 0`), the terrain basemap, the waypoint list. Cheap test: zoom — map imagery scales, drawn marks do not. |
| Menu is fixed-resolution | Fixed on branch `ui-adaptive`; open items in `UI-OVERHAUL.md`. |

---

## Build and deploy

x64 only. MSBuild is at
`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` —
VS 2022's fails with "v145 toolset not found".

```
MSBuild.exe FreeFalcon.sln /p:Configuration=Release /p:Platform=x64 /m
```

`Falcon4.vcxproj` links with `/FORCE`, so **a link error will not fail the build**. Grep for
`LNK2001` / `LNK2019` / `unresolved` after anything that adds or moves a symbol. `LNK4088` is
normal — it comes from the pre-existing duplicate `IID_*` symbols (`LNK4006`).

Rebuild All whenever a header gains a member.

**Nothing copies the exe into the game directory.** The build lands in
`Falcon4___x64_Release/FFViper.exe`; the game runs `C:\FreeFalcon6\FFViper.exe`. Copy it by
hand and check timestamps before concluding a change "did nothing".

Game config is `C:\FreeFalcon6\ffviper.cfg`. Keys are the variable name *including* the
`g_b`/`g_n`/`g_f`/`g_s` prefix, e.g. `set g_bCampMapDetail 1`.

`FFCrash.log` cannot catch heap corruption — `STATUS_HEAP_CORRUPTION` (`0xc0000374`) fast-fails
past SEH, so the file stays stale, which reads as "no crash log" rather than "wrong tool". Use
the Windows Application event log (id 1000) and the WER dumps in `%LOCALAPPDATA%\CrashDumps`
with `cdb.exe` from the Windows SDK. The exe's symbols are **`RedViper.pdb`**, not FFViper.pdb:

```
cdb -z <dump> -y "<install>;<repo>\Falcon4___x64_Release" -c ".lines -e;.reload /f;.ecxr;kb 40;q"
```

Page heap (`gflags /p /enable FFViper.exe /full`) faults on the bad write itself rather than
the aftermath.

---

## campsim (headless campaign simulator)

`src/tools/campsim/` - a console exe that loads a `.cam`, ticks the real campaign AI and
reports the winner: no renderer, no UI, no input. It compiles `winmain.cpp`, `f4config.cpp`
and `RedProfiler.cpp` itself and links the same engine libs as FFViper, so the AI is the
shipped one. x64 only, in the solution.

```
MSBuild.exe src\tools\campsim\campsim.vcxproj /p:Configuration=Release /p:Platform=x64 /m
campsim.exe C:/FreeFalcon6 save0 [days] [seed]
```

Needs `OpenAL32.dll`, `nvtt30205.dll`, `dxcompiler.dll`, `dxil.dll` and `openxr_loader.dll`
next to it (copy from the FFViper output / the game install). `save0` + 10 days takes about
8 minutes on the dev box: one progress line per campaign hour, a full state dump per campaign
day, a `RESULT` line at the end. `#END_GAME` is caught from a hidden window, same as the UI.

Two things the headless path must do that the game gets from other startup paths: call
`DeviceIndependentGraphicsSetup(FalconTerrainDataDir, Falcon3DDataDir, FalconMiscTexDataDir)`
(mission planning reads `TheMap.GetMEA`) and `ReadAllRadarData()` (weapon checks read
`radarDatFileTable`), and put `<install>/Zips` on the resource path (`sim/...` lives there).

**`InactivateUnit` (`camplib/camplist.cpp`) is now idempotent** - it checks
`InactiveList->Find()` before inserting. Headless ticking re-scanned the same inactive unit
before its flag cleared and `InactiveList` accepts duplicates, so the same 131 reserve units
gained ~240 entries/hour (962 -> 6000+ units in a day) and `AddReinforcements` walked them.
Same class as the 2026 Artscout duplicate-list fix in `unit.cpp`.

---

## campsim session log (2026-09-29)

Aims, in the order asked:

1. Read the campaign save format; compare GOG Falcon 4.0 `save0.cam` (data v65)
   with FreeFalcon6 `save0.cam` (v99) - which fields more units, which is fairer
   for DPRK vs ROK.
2. Work out whether the 10-day "Rolling Fire" campaign can be simulated to see
   who wins, given the in-game 64x time-compression cap.
3. Do it as a code-only simulation (no GUI): a headless harness, built with the
   VS2026 solution, validated against the save parse (M1 parity), then 8 seeds
   for odds.
4. Weather does not matter.
5. Keep the `InactivateUnit` change, write it up, and make the exe show
   progress / in-game time while it runs.
6. Nothing is tested - do not push anything.
7. Finish the local work, run it, and add debug prints so the elapsed time is
   visible.

Prompts, in order (abridged where the same message carried tool answers):

- "First familiarize yourself with the campaign .mds. Then, I want you to compare
  2 campaign saves, one from freefalcon, the other from vanilla falcon 4.0 ...
  I want to understand which has more units, which is more 'fair' in terms of
  DPRK vs ROK troops. [D:\GOG Games\Falcon 4.0\campaign\SAVE\save0.cam]
  [C:\FreeFalcon6\campaign\SAVE\save0.cam] I think these are both 'Rolling Fire'
  aka at the DMZ."
- "Is it possible to plan a simulation for the FF6 campaign to see who will win
  it? Right now, the fastest timeskip ingame is 64x but the rolling fire campaign
  is 10 days."
- "go."
- "Wait, I thought we were doing a code-only simulation, no gui needed?"
- "let's try it. Don't care about weather."
- "Oh, it's not VS2022, its VS2026. Anywhere in the MD files tell you how to
  compile? or did you get around that"
- "whats happening rn? also, why are you modifying campaign code?"
- "ah i see. let's write that down, the change you made in WIP-NOTES. you can
  keep it, but let's have that EXE show progress/ingame time so I know it's
  running - or some debug print"
- "don't push anything - none of it is tested. take that push back."
- "yes, finish local work and run it. but again, let's add debug prints to the
  simulation so i can tell the time its taking"
- "don't do anything else. Just log what you HAVE done in the .md, including my
  aims and prompts."

Choices made through the question tool: temp one-off comparison script; all
domains for strength; `save0` pair only; harness only (no in-game runs); VS2026
sln project; M1 parity as the fidelity bar; 8 runs for the batch.

What was done:

- **Save comparison** (temp, not in the repo):
  `C:\Users\Andrew\AppData\Local\Temp\opencode\compare_cams.py` and
  `compare_report.md`. Uses the repo's `tools/campaign-editor/ffcamp` parser,
  each save with its own class table (GOG `terrdata\objects`, FF6
  `campaign\SAVE\CampaignDB`). Result: FF6 962 units vs GOG 912; start ratios
  DPRK:ROK ground 2.54x (vanilla) vs 1.38x (FF6), aircraft 3.19x vs 1.16x,
  air-defense 8.0x vs 1.13x, plus navies for the ROK/US in FF6. FF6 is the
  fairer start and favours the ROK/US side. Method: engine `GetTotalVehicles`
  roster sums + `NumElements`, validated against `.tea` auto-save stats (FF6
  DPRK air-defense 2232/2232, strike aircraft 708/708).
- **`src/tools/campsim/`** (new, untracked): `main.cpp`, `stubs.cpp`,
  `campsim.vcxproj`; added to `FreeFalcon.sln` (x64 only; Win32/Remote map to
  `Release|x64` without `Build.0`, so the solution's x64 configs build it).
- **Build**: `MSBuild.exe src\tools\campsim\campsim.vcxproj
  /p:Configuration=Release /p:Platform=x64 /m` with the VS2026 MSBuild (VS2022
  cannot do v145). Output
  `src\Falcon4___x64_Release\Tools\campsim\campsim.exe`.
- **Run**: `campsim.exe C:/FreeFalcon6 save0 [days] [seed]`. Needs
  `OpenAL32.dll`, `nvtt30205.dll`, `dxcompiler.dll`, `dxil.dll` and
  `openxr_loader.dll` next to the exe (copied from the FFViper output / the
  game install).
- **Harness shape**: compiles `winmain.cpp`, `f4config.cpp` and
  `RedProfiler.cpp` itself and links the same engine libs as FFViper, so the AI
  is the shipped one; a hidden window catches `FM_CAMPAIGN_OVER`; a VU pump
  thread and a sim-tickler thread stand in for the sim loop; one campaign minute
  per step; `DeviceIndependentGraphicsSetup` (mission planning reads
  `TheMap.GetMEA`) and `ReadAllRadarData` (weapon checks read
  `radarDatFileTable`) are required, and `<install>/Zips` must be on the
  resource path.
- **Progress output**: banner, per-tick timings for the first 5 ticks, then one
  line per campaign hour
  `[day D HH:MM] h/total (pct%) units=N endgame=E wall=Ns eta=Ns`, a full state
  dump per campaign day, and a final
  `RESULT seed=.. endgame=.. engine=.. campaign_h=.. wall_s=..`.
- **M1 parity** (campaign loaded): 962 units = 835 active + 127 reserve;
  coalition split ROK 380 / CIS 16 / PRC 73 / DPRK 493; 2629 objectives. Matches
  the Python parse exactly.
- **M2**: one-day run completes in about 2 minutes; no endgame; front moves and
  the ATM builds flights/packages.
- **M3 (10 days, seed 12345)**: last observed day 5 14:00 = 125/240 h (52%),
  ~3000 units (mostly flights/packages), wall 301 s, eta about 280 s, no
  endgame. The run was aborted by the user around day 5 and was not finished.
- **Engine edits**: `camplib/camplist.cpp` `InactivateUnit` idempotency guard
  (kept, described above); temporary `printf` instrumentation in
  `campaign.cpp` `DoCampaignLoop` was added during bring-up and reverted. No
  other engine changes.
- **Repo state**: nothing committed and nothing pushed; local `main` equals
  `origin/main` (`37bb31b9`), no unpushed commits. Working tree:
  `M WIP-NOTES.md`, `M src/campaign/camplib/camplist.cpp`,
  `?? src/tools/campsim/`, plus pre-existing `M tools/campaign-editor/*` and
  `?? .claude/` that this session did not touch.

Open / not done:

- M3 did not finish; M4 (8-seed odds) was not started.
- The harness diverges from the shipped exe in one known way (the
  `InactivateUnit` guard) and runs the campaign aggregate-only, with no sim or
  renderer, so deaggregated combat never happens.
- No JSON/CSV output and no batch runner yet; results are the text log.

---

## Housekeeping

`main` is pushed. **PR #53** (`ajalberd:main` → `FreeFalcon:develop`) carries the campaign
planning and map work. PRs #49/#50/#52 are fully subsumed by `main`.

Two things that cost time before:

- **Check subsumption with `git cherry -v main origin/<branch>`, not a diff.** These branches
  were cut from `develop`, so `git diff main...origin/<branch>` answers a different question
  and showed 746 phantom insertions where the real answer was zero.
- `gh` resolves this checkout to upstream `FreeFalcon/freefalcon-central` while the branches
  live in `ajalberd/freefalcon-central`, so a PR needs `--head ajalberd:<branch>`.

## campsim: results, tools and the two crashes (2026-09-29)

- **Result, Korea `save0` (Red Herring), FF6, 4 seeds x 10 days, no player: all four end
  in the script's own Stalemate (event 15, `#IF_BORDOM_HOURS 240`) at ~9.7 days.** Objectives
  moved by 5 (Blue 1207->1212, DPRK+PRC 1404->1399); ground vehicles Blue 82%, DPRK 80%
  left. Seoul/Pusan (event 14) and Pyongyang/Wonsan (event 17) never come near. The AI does
  not press an attack on either side, so "fair" here means "inert", not "balanced".
- **PRC (team 5) is folded into DPRK within the first campaign half hour** (objectives and
  units flip owner). Count DPRK+PRC as one side.
- **Order of battle, `tools/campsim/compare_report.md`** (`compare.py`, `saves.py`): vanilla is
  DPRK-heavy (AD 6.6x), BMS 4.37 is Blue-heavy (ground 0.55x), FF6 sits between with a
  Blue air/sea lead. BMS units come from a lite decoder (v108 unit records are longer
  than `ffcamp` knows, anchored on `type,VU_ID,type`); BMS objectives are not decoded.
- **Tools** (`tools/campsim/`): `run_batch.py` (seeds in parallel; exit codes printed),
  `serve.py` + `viewer.html` (plays `runs/*.jsonl` on the terrain map), `make_backdrop.py`.
  `campsim.exe <game> <save> <days> <seed> <timeline.jsonl> <frame_minutes>`.
  Wall time: ~5 min alone, ~21 min each with 4 in parallel.
- **Two engine crashes found and fixed (both are shipped-game code, Release only):**
  1. `PackageClass::BuildPackage` (`package.cpp`): `ShiAssert(bw and tw and aw and eaw)` is
     compiled out, then `bw->GetNextWP()` dereferenced NULL. Now guarded.
  2. `AirTaskingManagerClass::Task` (`atm.cpp`): `BuildPackage` queues support requests and
     `RequestMission` deletes an earlier request for the same mission/target -- possibly the
     one being built (use-after-free, then double free in `Remove`) or the neighbour `lp`.
     The request is now detached while built and the walk restarts from the end (`seen`).
  Without these, 1-day runs crashed 5 in 6. Whether a normal session hits them is unproven.
- The recorder must not use CRT `fopen` (resmgr remaps it; corrupted the heap): plain Win32
  `CreateFile`. The harness pumps the VU queue inline between ticks, not on a thread.

### campsim: ground war findings and what is still open (2026-09-29, end of session)

- **Ground units barely move.** Over 10 days the median battalion's net displacement is 0 km;
  ~2,200 unit-km moved on day 0 (initial deployment), then 50-250 unit-km/day across ~660
  battalions. Average morale stays 98-99% on both sides -- morale is not what stops them.
- **The AI does start offensives, they just do not take anything.** `SelectGroundAction`
  runs: ROK (Blue) goes OFFENSIVE at hour 1 (initiative 64) for ~12 h, DPRK is OFFENSIVE
  from day 1 on (initiative 67, action points 64-85, objective 1065 then 848), yet objectives
  changed hands by 5 in 10 days. So the break is downstream: `GroundTaskingManager::Task`
  assigning `GORD_CAPTURE/ASSAULT`, battalions choosing a move tactic, or capture itself.
  Next step: the timeline frame now carries per-battalion orders, tactic and supply
  (unit tuple fields 9-11) and per-team `a:[type,tempo,points,initiative,objective]`;
  histogram orders/tactic per team per hour and see whether any battalion holds
  `GORD_CAPTURE` with a move tactic. ROE ground-capture is allowed only ROK<->DPRK.
- **A third crash remains (~1 in 4 one-day runs, heap corruption or a bad pointer).** The
  request list (`ATM requestList`) goes bad *inside* `ProcessRequest`'s scan loop. It did not
  reproduce with per-`Remove` checks compiled in, so it is timing-sensitive; diagnostics were
  removed. Suspects: `Remove(pp)` on a node that is not in this list, and re-entrancy
  (`BuildPackage` -> message dispatch -> `ProcessRequest` while `Task` walks the list).
  Use `cdb` (see above) for the stack; gflags page heap was not permitted here.
- **Sensitivity run ("strip N% of DPRK front units, does the front move?") was not done.**

### campsim: why the ground war does not move (resumed 2026-09-29; traces removed)

Established (each from a timeline or a trace, not from reading code alone):
- **Offensives start but are tiny.** Team action cycles ~19 h OFFENSIVE + 1 h CONSOLIDATE
  (DPRK from h21; ROK only h1-8, then DEFENSIVE for good). Only ~15 battalions per side ever
  hold `GORD_CAPTURE` (about 5% of ~420); ~45% are `AIRDEFENSE` and ~40% `RESERVE`.
  Cause: capture objectives are enemy-owned, near-front (`IsNearfront`), ROE-allowed, and the
  assigner gives **one objective per unit** (`AssignUnits`: "only take one objective per
  available unit"), so the offensive is capped by the count of near-front enemy objectives.
- **Tactics come from `FALCON4.TT` (binary, 68-byte entries; decode with the struct in
  `tactics.h`).** `MOVE_WEDGE`(25, prio 8) needs `special=2` (team is in OFFENSIVE),
  supply > 30, minOdds 3; `WAIT_TIL_READY`(29, prio 2) accepts anything, so a capture unit
  falls to it whenever the team is not in OFFENSIVE (16 of 19 traced units) -- and also
  stays in `MOVE_AIRBORNE`(16) waiting for helicopters (airborne battalions, 2-4 per side).
  ROK units keep stale CAPTURE orders after ROK drops to DEFENSIVE and just wait.
- **Even in the 19 h OFFENSIVE windows, DPRK capture units hold WEDGE and move ~0 km**
  (net, 18 h), supply falling. Path failures are rare (5 units in 3 days: `NO PATH` /
  `BuildGroundWP` failed), so it is not routing. Per-cell cost is large:
  `TimeToMove` = 6-51 minutes per 1 km cell (speed 2-10, terrain multipliers).
- **Ruled out:** morale (98-99%), the reset of `last_move` by re-ordering (only ~83 capture
  re-orders in 3 days; one unit, 2081, flips objective every hour).
Not established -- next probe: in `UnitClass::ChangeUnitLocation` (`camplib/unit.cpp:2436`)
log, per WEDGE capture unit, whether `GetMoveTime() >= TimeToMove` ever holds and what
`DetectOnMove()` returns (`<0` sets `nomove` and puts the unit back = contact stops it).
Also check whether `SetUnitNextMove` / `UpdateTime()` (TRACKED_MOVE_CHECK_INTERVAL) starve
the accumulator at 1-minute ticks -- a harness-vs-game difference worth ruling out by
running the tick at the game's real step.

### campsim experiments and the research platform (end of 2026-09-29)

- **Knobs** (`speed=`, `cost=`, `strip=`, `init=`; see `tools/campsim/README.md`) and
  `run_batch.py --tag/--set` make single-parameter experiments a one-liner.
- **Result: the front does not respond to speed, terrain cost or force size.** 3-day runs, seed 41:
  baseline, `speed=3`, `cost=0.25`, both, `strip=6:25` all end within a few objectives of each
  other (Blue 1207->1212..1215, DPRK+PRC 1404->1396..1399). Day-0 movement is ~2,500 unit-km in
  every configuration and ~100-250 unit-km/day afterwards, so the limit is what units are *ordered*
  to do, not how fast they can go.
- **Measured (per-unit counters, `movement-diag.patch`), baseline:** DPRK capture battalions fail
  95% of step attempts on "not enough accumulated move time" (a 1 km cell costs 30-90 min at their
  modified speed), so they do move, but ~30 cells in 2 days. Contact stops (`DetectOnMove`) are a
  few percent; `CaptureObjective` triggers: 0-1. At 12x faster movement Blue units at 2% supply
  take 4,000-5,000 steps without net progress (thrashing), and captures stay at 0-1.
- **User's hypothesis, unverified:** deaggregated (3D) units move faster than the 2D campaign.
  Plausible -- campsim has no 3D layer. In-game test: watch one battalion's km/h aggregated vs
  inside the bubble.
- **Where to look next for the AI:** (1) `GroundTaskingManager::AssignUnits` gives one objective per
  unit and only near-front enemy objectives are capture targets, so an offensive is ~15 units;
  (2) `MOVE_WEDGE`/`ECHELON` need the team in OFFENSIVE (`special=2`), otherwise units fall to
  `WAIT_TIL_READY`; (3) whether defenders ever lose an objective (`DetectVs` capture condition).

### ROOT CAUSE of the stalled ground war: `SimToGrid` rounding (found + fixed 2026-09-30, sim-verified, NOT yet flown)

- **Bug:** `GridToSim` returns the *centre* of a cell (`x*G + G/2`, `G = FEET_PER_KM = 3279.98f`),
  `SimToGrid` was `FloatToInt32(sim / G)`, and `FloatToInt32` rounds to nearest (ties to even: `fistp`
  on x86, `cvtss2si` on x64). The quotient is `x + 0.5` to within float error, so **553 of 1024 grid
  coordinates read back one cell too high** (always +1; `tools/campsim/simtogrid_check.py`). A ground
  unit stepping onto such a cell in a *decreasing* x or y direction read back where it started, spent
  its move time and never moved again (retrying is identical); increasing directions worked, often
  jumping two cells. So ground units could only advance north/east. Seen in the sim as a battalion
  "moving" 584,514 -> 584,513 every 51 minutes and never leaving (`SETLOC` read-back trace).
- **Fix:** `SimToGrid` = `FloatToInt32(floorf(sim / G))` (`camplib/find.cpp`), behind
  `g_nSimToGridFix` (default 1; `set g_nSimToGridFix 0` in FFViper.cfg = old behaviour, for A/B).
  campsim knob `simtogrid=0` does the same.
- **Effect (Korea save0, no player):** old = stalemate at ~9.7 d, ~5 objectives change hands,
  ~2,500 unit-km moved on day 0. Fixed = ~30,000 unit-km on day 0, 500-650 ownership changes, and
  **Seoul (objective 228) falls in 1.8-4.7 days -> "Bad guys win" (event 14)**: seeds 61 (4.7 d),
  62 (3.0 d), 63 (1.8 d), 51 (2.4 d); seed 64 was heading there (Blue objectives 1207 -> 1140 at 5.7 d)
  when it hit the remaining crash. ROK goes on the offensive first (objectives +40 in ~8 h), then DPRK
  counter-attacks; ground strength freezes after ~28 h (Blue ~3,100, DPRK ~7,100) while objectives keep
  sliding to DPRK.
- **Caveats:** this is AI vs AI, no player, no 3D; it is the campaign as shipped *with* the bug fixed, so
  the stalemate people saw was partly this bug. Whether the fixed war is too fast / DPRK-favoured is now
  a real balance question (tempo, reinforcements, the Seoul trigger, ROK defence priorities) -- and exactly
  what campsim is for. **Test in game before relying on it** (build FFViper, fly a few campaign hours,
  compare `g_nSimToGridFix 0/1`). Deaggregated 3D units were never affected; aggregated ones were.
- Remaining crash (0xC0000005, ~1 in 3 long runs, timing-dependent) still unfound; the fixed war
  exercises it more because more units fight.

### Why the sim and the live game disagreed: `g_nNoPlayerPlay` (found 2026-09-30)

- **Mechanism** (`camplib/team.cpp`, hourly team update, ~line 2838): if the local player's team has not
  flown a mission for `g_nNoPlayerPlay` hours (default **2**), the engine calls
  `ApplyPlayerInput(playerTeam, ..., -10)`: that team loses **2 initiative points to the enemy and one
  reinforcement** each time (`TransferInitiative`, `AddReinforcement(-1)`), and `lastPlayerMission` is
  reset so it repeats every ~2 h. The player's team (ROK/Blue) therefore decays unless somebody keeps
  flying. campsim picks a ROK player squadron and never flies, so Blue was penalised all through the
  earlier "DPRK takes Seoul in 2-5 days" runs. Andrew's `FFViper.cfg` has `g_nNoPlayerPlay 400`, which
  disables it; his autosave shows initiative Blue 83 / DPRK 17.
- **Evidence (seeds 81, 82, 2 days):** NoPlayerPlay 2 -> DPRK "bad guys win" (event 14) at 1.4-1.6 d, Blue
  objectives 1202/1179; NoPlayerPlay 400 -> Blue 1281/1314 objectives, DPRK ground 57%/47% left, no endgame.
  Full `FFViper.cfg` (`--set usecfg=1`, loads the file as the game does) reproduces the live direction;
  five keys differ from defaults and the "other" group (FloatingBullseye, NoPlayerPlay, DeagTimer,
  ATCTaxiOrderFix, UseRC135) carries almost all of it; NoPlayerPlay is the load-bearing one.
- **So the earlier "Seoul falls in ~3 days" numbers are a harness artifact**, not the campaign's balance.
  The fair AI-vs-AI baseline is NoPlayerPlay 400 (or `usecfg=1`). Always run campsim with it from now on.
- **Design consequence worth knowing:** with the stock default a hands-off player's side loses ground
  steadily. That is intentional (play or lose), but 2 h is very harsh for a long fast-forward.
- campsim knobs added: `usecfg=1`, `cfgfile=<path>` (load only some keys), `simtogrid=0`.

### Fair AI-vs-AI baseline with the user's own FFViper.cfg (2026-09-30)

campsim `--set usecfg=1`, Korea save0, 4 seeds x 10 days (SimToGrid fixed, g_nNoPlayerPlay 400):
- seeds 91, 92: ran all 10 days, no endgame; Blue objectives 1207 -> 1297 / 1332, DPRK 1404 -> 1332 / 1297;
  ground vehicles left: Blue 66% / 71%, DPRK 39% / 39%.
- seed 93: **event 17 "we win" (Pyongyang/Wonsan taken) at 43.8 h (1.8 d)**: Blue 1342 objectives, DPRK ground 39%.
- seed 94: crashed (the remaining 0xC0000005) at 4.6 d with Blue 1316 / DPRK 1313.
So with this config the AI war favours Blue/ROK strongly and DPRK ground is ~40% of start by day 10 in every
run, which matches Andrew's live game (Blue ahead, DPRK ground down 60%+ in a day). The balance question is now
"is ROK/Blue too strong with RealisticAttrition + FireOntheMove + LargeStrike?", not "does it move".
Editor: new "hidden reinforcements" layer (127 units in save0), see tools/campaign-editor/README.md.

### DPRK air force: what limits it (2026-09-30, campsim, usecfg=1, 3 seeds x 3 days each)

Vanilla F4 save0 gave DPRK more aircraft than the allies (918 vs 796; strike 768 vs 540); FF6 flipped it
(1120 vs 1722), BMS is near even. In the sim and in the live game DPRK's air force is overwhelmed inside a day
(live: squadron rosters 1283 -> 621 in 19 h) and then flies ~130-170 flights against Blue's 250-340.
Timeline frames now carry `ad` per team = [pending requests, missionsToFill, missionsFilled, packages,
airbases, airbases<50%, mean airbase status].
Levers tried, each changes DPRK flights airborne (h6-72 mean ~137) by nothing:
1. `stripair=1/2/3:50` (Blue squadrons -50%, 576 aircraft): DPRK flights 171 vs 159 early, 132 vs 129 later; ground
   outcome within noise. Blue's flights rebuild to ~250 anyway.
2. `airtempo=6:100` (ATM may task 100% of pending requests): flights 137 vs 137. The budget
   (`missionsToFill = requested * actionTempo / 100`, team.cpp) is usually far above the pending count.
3. `abheal=6` (DPRK airbases repaired every 10 ticks, status 98-99% instead of ~65%, 13-16 of 50 < 50% without):
   flights 131-151, packages unchanged, pending requests pile up (60-65 vs 34-42).
So the cap is not the airframe count, the tasking budget, or runway damage. Requests exist and go unfilled, so the
next suspect is squadron-level availability in `FindBestAir`/`BuildPackage`: aircraft ready, pilots, takeoff slots
per airbase (`FindTakeoffSlot`, ATM_MAX_CYCLES), squadron stores (weapons), sortie rates. Next step: count
`BuildPackage` results (success / delayed / failed) per team and the reason `FindBestAir` returns no squadron.
campsim knobs added: `stripair=T:P`, `airtempo=T:P`, `abheal=T`.

---

## STATE AT END OF SESSION 2026-09-30 (read this first)

**Committed (local only, nothing pushed):** the SimToGrid fix + `g_nSimToGridFix` toggle (`camplib/find.cpp`,
`ui/src/f4config.cpp`) is on all three lines: `main` 03faf489, `ui-adaptive` 4aecffa4, `rail-tracks` 8f3f8d44
(cherry-picked, rail never merged into main). Rail tree compile-checked (camplib + F4Config). **Not built into
`FFViper.exe`/rail exe and not copied to `C:\FreeFalcon6` except `FFViper-uitest.exe` (ui-adaptive build).**

**Uncommitted engine fixes (in `main` working tree and `ui-adaptive` working tree):** `camptask/atm.cpp`
(request detached while built, restart walk, `seen[]`), `camptask/package.cpp` (NULL ingress/egress guard),
`camplib/camplist.cpp` (InactivateUnit idempotent), and on ui-adaptive `falcsnd/fsound.cpp` (F4StartStream skips an
unreadable stream and logs the file instead of dividing by zero). All compile and run in campsim; not flown.
`FFViper-uitest.exe` (deployed 2026-09-30 20:43, backup `.bak-pre-simtogrid`) contains all of them + the fix.
**Remaining crash** (0xC0000005, ~1 in 3 long campsim runs, timing-dependent) still unfound.

**Game files changed in `C:\FreeFalcon6` (all reversible):** `movies\` and `Theaters\Israel\movies\` replaced by
lossless IYUV re-encodes (no x64 Indeo/Cinepak decoder on this PC); originals in `movies_indeo5_orig` and
`Theaters\Israel\movies_orig_indeo_cinepak`; `movies_msvc` is a leftover test folder (delete it, deletion was blocked
for me). `tools/movies/vfwprobe.cpp` tests whether an AVI decodes through VFW like the game does.

**Findings that matter (details in the sections above):**
1. SimToGrid rounding = the stalled ground war. 2. `g_nNoPlayerPlay` (default 2 h) penalises the player's team, so
campsim must run with `--set usecfg=1`; the earlier "DPRK takes Seoul in 3 days" was a harness artifact. 3. With
Andrew's cfg the AI war favours Blue/ROK (DPRK ground ~40% of start by day 10; one seed ended "we win" at 1.8 d).
4. DPRK air force is overwhelmed inside a day; its sorties (~135 flights) are NOT limited by airframe count, tasking
budget (`airtempo`) or runway damage (`abheal`); requests pile up unfilled, so the cap is squadron availability.
5. Reinforcements = pre-placed inactive units released by an hourly counter; the editor now shows them ("hidden
reinforcements" layer, 127 in save0). 6. Initiative = zero-sum 0-100 meter from force ratios that gates offensives.

**Open / next:** (a) *the failure counters you asked for are NOT written yet*: add per-team counts of
`BuildPackage` results (PRET_SUCCESS/DELAYED/NO_ASSETS/TIMEOUT/NOTARGET/CANCELED) in `AirTaskingManagerClass::Task`
and of why `FindBestAir` (`atm.cpp` ~1557) rejects squadrons (specialty score `<= lowestScore`, caps/service, range,
speed, `to < scheduleTime`, no available aircraft); read the rest of FindBestAir first (it continues past the
availability check). (b) find the remaining crash. (c) balance question: is ROK too strong with RealisticAttrition +
FireOntheMove + LargeStrike? (d) decide whether to push main/ui-adaptive and whether to commit the crash fixes.
(e) the live watcher (`tools/campsim/watch_save.py`, log `live_log.csv`) runs as a detached python process; restart
with `python watch_save.py --every 5`.

**UPDATE (same evening): the crash and sound fixes are now committed too (local, not pushed).** Three commits per
line, in this order: SimToGrid fix, crash fixes (atm/package/camplist), sound fix (fsound).
- `main`: 03faf489, 06c2f2b3, 806681f7
- `ui-adaptive`: 4aecffa4, 9bf47c45, fc4571bd
- `rail-tracks` (cherry-picked): 8f3f8d44, f76350b1, f4cbaaf6
Compile-checked: rail (camplib, F4Config, CampTask, FalcSnd), main (FalcSnd; campsim builds the rest). The
"Uncommitted engine fixes" paragraph above is therefore obsolete. Still not built into a rail or main game exe.

## Air power: failure counters and what limits DPRK (2026-09-30, campsim)

**Counters** (`gAtmDiag[team][24]` in `atm.cpp`, emitted as `"af"` in the campsim timeline; 0-9 =
FindBestAir rejection reasons, 10+PRET = BuildPackage results). Team ids: ROK = 2, DPRK = 6.
They are cheap and left in the engine tree (uncommitted); `tools/campsim/air_report.py` summarises
airborne flights per day from a timeline.

**Findings (3 days, fair cfg, seeds 11-13)**
- Tasking is symmetric. BuildPackage successes are the same for both sides (~6300 in 3 days);
  DPRK has more DELAYED retries (~65k vs ~30k) and ~7% range rejections vs ~2% (its bases are
  further from targets). 72% of squadron evaluations fail on specialty score <= mission priority
  threshold for both sides; ~22% on "no available aircraft". The planner is not what caps DPRK.
- Airborne flights per day, baseline: ROK ~270/240/200/215, DPRK ~180/140/120/155. ROK leads from
  day 1 (not a day-2 crossover); the gap is ~1.5-2x.
- **g_bRealisticAttrition=0 is the lever**: DPRK ~310/315/245/225 vs ROK ~300/345/330/365 - near
  parity, and ground/objective outcome swings toward ROK taking territory (obj 1354 vs 1257).
  With it on, every shot-down aircraft is lost, so DPRK's smaller, older pool bleeds out.
- Halving ROK airframes (`stripair=2:50`) does not hurt ROK for long (recovers by day 3) and
  DPRK stays ~150: DPRK is capped by losses, not by ROK numbers.
- `g_nPercentage_available_aircraft 40` (user cfg) vs 80: 80 gave FEWER flights for both
  (noisy, probably more sorties lost sooner); not a lever.
- Files: `cfgs/F_noattr_full.cfg`, `G_pct80_full.cfg`; runs `save0-{fail,noattr,pct80,rokair50}-s11/12`.

**10-day follow-up (seeds 21, 22)**
- Attrition off (`F_noattr_full.cfg`): both ran the full 10 days, no early end. DPRK holds ~210-295
  airborne flights/day the whole time (ROK 160-350): sustained air war, not a day-1 collapse.
  Objective ownership changes 435 / 548; ground vehicles end ROK 80-84%, DPRK 31-40%.
- Your config (`usecfg=1`, attrition on): seed 21 ENDED at 2.0 days with endgame code 17 (a
  victory/"game over" trigger, minute 2855; not investigated - check whether it is legitimate);
  seed 22 crashed (0xC0000005) at 4.2 days - the known timing-dependent crash again (~1 in 3).
  DPRK flights in those runs stay ~115-185/day vs ROK 170-290.
- Open: investigate the endgame-17 trigger at day 2; find the 0xC0000005; add a DPRK aircraft/
  replacement boost knob to test an alternative to turning attrition off.

## PLAN (not started): "DPRK Air Boost" JSGME mod (2026-09-30)

Goal: a more dynamic Korea war - DPRK keeps a real air presence past day 1 and contests the
front - without changing the engine or the user's cfg. Planning only; nothing built.

**Why this lever** (see "Air power: failure counters"): tasking is symmetric; DPRK is capped by
a smaller pool bleeding out under `g_bRealisticAttrition=1` (and ~7% range rejections because its
bases sit far from targets). So give it more airframes and put them closer, rather than touching
the ATM.

**Form.** Mod folder `MODS\DPRK Air Boost\campaign\korea2012\` containing a modified copy of the
scenario save(s) the user plays (`save0.cam` "Red Herring" first; check which `.cam` the
Instant Action / TE menu loads, plus `Instant.cam`). It overlays the campaign data the same way
the tile mods overlay textures; JSGME backs the originals up. The mod only applies to NEW
campaigns (a running save keeps its own units).

**Method.**
1. Tooling: extend `tools/campaign-editor` (it already parses `.uni`/`.obj`/`.tea`, and writes
   edits) so it can (a) list squadrons per team with airbase, type, aircraft count, (b) add
   aircraft to an existing squadron, (c) clone a squadron onto another airbase, (d) re-home a
   squadron. If writing `.uni` back is not supported, that is the first task; otherwise do it with
   a small script that patches the unit stream (no length prefix; keep `U_INACTIVE` bit).
2. Survey: per DPRK airbase/airstrip, distance to the DMZ and to the main ROK targets (Seoul,
   Osan, Suwon...), runway/capacity (parked aircraft, hangars; shelters), what already flies
   from it. Pick the 4-6 closest forward bases (Haeju / Sunchon / Onchon / Hwangju style names
   come from the objective list, not memory - read them from the save).
3. Changes, in order of preference (each its own variant so they can be A/B tested):
   - A: top up existing DPRK squadrons to full strength (many sit below max).
   - B: re-home 2-4 existing squadrons to the forward bases (shorter range, fewer range rejects).
   - C: add new squadrons (clone MiG-21/23/29/Su-25 types the DPRK already has) at the forward bases,
     plus a few replacement/reserve aircraft; keep totals near vanilla F4.0 DPRK (918 vs ROK 796
     aircraft) as the target - "more aircraft for DPRK", per the user.
   - D: optional hidden reinforcement squadrons arriving day 2-3 (editor already shows arrival hours).
   Keep ROK untouched so the effect is attributable.
4. Capacity checks: an airbase only launches what its ATM schedule allows (`ATMAirbaseClass`
   schedule blocks, runway damage); piling squadrons on one strip just creates `noav`/`airbase`
   rejects (counter 9). Spread across bases.
5. Validate with campsim, fair cfg (`usecfg=1`, attrition ON, since that is the user's setting):
   metrics = airborne flights per day per side (`air_report.py`), `af` reject mix (range share
   should fall), objectives changed, ground outcome, time-to-endgame; 3 seeds x 5 days per variant,
   then 10 days for the best. Success = DPRK flights stay >= ~70% of ROK's past day 3 and the war is
   not decided before ~day 7. Also watch for the endgame-17 trigger seen at day 2 on one seed.
6. Package: `MODS\DPRK Air Boost` (+ a README listing the variant and the exact counts changed),
   add a zip under `dist/` like the other patches, note the mod in WIP-NOTES.

**Risks / open questions.**
- Campaign ownership of squadron data: squadrons may be defined by the theater DB (`Falcon4.UCD`/
  `CampaignDB`) AND placed in the `.cam`; confirm that adding to the `.cam` alone is enough.
- Old behaviour: a squadron's `GetUnitRange`/airbase `d + loiter` check limits tasking radius, so
  re-homing may matter more than adding aircraft; test B before C.
- Mod must not affect the rail branch or other campaigns (`korea1980s`, `eurowar`): file-scoped.
- Alternative with no data change: ship the cfg (`g_bRealisticAttrition 0`) - already proven to give
  near-parity; the mod is the "keep realistic attrition" route.
- Still open elsewhere: endgame-17 at day 2, the 0xC0000005 crash.

### DPRK Air Boost: survey + tooling (2026-10-01)

**Survey of `campaign\SAVE\save0.cam` (Korea, "Red Herring")** - `tools/campsim/survey_air.py`.
- Squadron records: 46 DPRK(6), 39 ROK(2), 40 US(1), 25 PRC(5), 8 Russia(4). All squadrons are
  already at class maximum (roster 2 bits per vehicle group, max 16/32/48), so "top up" (variant A) is
  impossible - more aircraft means more squadrons.
- **There are no helicopter battalions.** Helicopters are *squadrons* of rotary aircraft; ground
  battalions are all vehicles. DPRK: 2 Mi-24 Hind (32 each = 64) + 4 Mi-8 (16 each = 64); no Mi-17/
  Mi-28/Hughes types. ROK: 2 AH-1 (64) + 2 MD-500 TOW (64) + Lynx 16 + UH-60/UH-1/CH-47/BO-105 etc.
  US: 2 AH-64D (64) + UH-60, CH-47, CH-46/53. So ROK+US have roughly 4x DPRK's attack helis.
- **Bombers:** DPRK has 5 H-5 (Il-28 class) squadrons, 32 each = 160, parked at Panghyon, Uiju
  and Orang - all y > 675, i.e. 200+ km behind the DMZ. Strike: Su-25BM 3x32, A-5 2x16, Q-5D 3x16,
  Su-7 2x16. PRC adds H-6A 96 but team 5 is folded into DPRK. ROK/US bombers: B-1B, B-2A, B-52H, F-117A.
- DPRK squadron aircraft by type (1120 total incl. transports): H-5 160, MiG-23ML 128, Su-25BM 96,
  MiG-29A 96, MiG-21bis 96, MiG-21MF 80, An-2 80, Mi-24 64, Mi-8 64, Q-5D 48, An-24 48, MiG-21PF 48,
  Su-7 32, A-5 32, IL-76 16, J-7B/E 16 each.
- Forward DPRK bases (y 460-570, front of the DMZ): Ongjin (265,461) MiG-21MF, Taetan (249,479)
  Su-25BM, Kwail (216,512) MiG-23, Hwangju (307,543) MiG-21MF, Koksan (393,551) MiG-21MF,
  Onch'on (238,569) MiG-29, Mirim (315,581) MiG-29 + An-2; Ayang-Ni strip and Togu-ri Army Base host
  Mi-24 / Mi-8. Seoul-area ROK/US bases are at y ~ 350-440.

**Tooling.** `tools/campsim/make_air_mod.py <variant> <outdir>` clones donor squadron records
byte-for-byte (new VU id / camp id / name id, re-pointed `x,y,destX,destY,airbaseId,hotSpot`) and
re-wraps `.uni` with `entities.encode_units`; `walk_units` verifies the result decodes. Variants:
`none` (round-trip control), `heli` (+2 Mi-24, +1 Mi-8), `bomber` (+2 H-5, +2 Su-25BM, +1 A-5),
`fighter` (+MiG-29, +MiG-23, +2 MiG-21bis), `air` (all), `rehome`/`air+rehome` (move 2 H-5 forward).
For `ffcamp/entities.py` I added `_at` offsets (`destX`, `nameId`, `airbaseId`, `hotSpot`) - decode
is unchanged. A throw-away game dir `tools/campsim/gamework/` (junctions/hardlinks to C:\FreeFalcon6,
own `campaign\SAVE`) holds the variant saves as `a_<variant>.cam` so the real game is untouched;
run: `run_batch.py --game <gamework> --save a_air --tag mod --set usecfg=1`.

### DPRK Air Boost: results (3 days, fair cfg incl. attrition ON, seeds 11-13, 2026-10-01)

Avg airborne flights per day (day1/2/3), ROK vs DPRK; objectives ROK/DPRK at day 3:
| variant | ROK | DPRK | obj |
|---|---|---|---|
| none (control) | ~285/265/270 | ~170/130/130 | 1328-1348 / 1281-1319 |
| heli | ~285/275/262 | ~175/133/125 | ~1336 / 1293 (ground: DPRK loses a bit less) |
| bomber | ~285/280/270 | ~187/128/124 | ~1342 / 1287 |
| fighter | ~276/268/265 | ~220/144/142 | ~1343 / 1286 |
| **air (all 12 squadrons)** | ~225/150/158 | **~250/166/219** | **1261-1329 / 1282-1351** |
- Each group alone moves little (fighters best, +25% day 1). Combined, DPRK reaches parity-plus and
  ROK's flights drop 25-45% (more DPRK counter-air/strike sorties land on ROK bases): non-linear.
- Combined variant flipped objectives toward DPRK in 2 of 3 seeds (1350 vs 1261), i.e. it may now be
  too strong; a half-strength variant (drop the 2 extra H-5 + A-5, or the fighters) is the next test, as
  is a 10-day run. The war is "more dynamic", not decided.
- **Built (not enabled/flown): `C:\FreeFalcon6\MODS\DPRK Air Boost`** (`campaign\SAVE\save0.cam`,
  README.txt) and `dist\DPRK-Air-Boost-JSGME.zip`. Caveats: need to confirm in the UI that
  "Red Herring" loads `campaign\SAVE\save0.cam`; new campaigns only; untested in 3D (cloned squadrons
  have not been loaded by the real game, only by campsim, which is the same campaign engine).

### Half-strength mod, 10 days, + scripted events (2026-10-01; seeds 21, 22; fair cfg, attrition ON)

`half` = 6 squadrons (Mi-24 Koksan, H-5 Onch'on, Su-25BM Ongjin, MiG-29A Hwangju, MiG-21bis Koksan,
MiG-23ML Taetan). Average airborne flights/day, day 0..10:
| variant | ROK (s21) | DPRK (s21) | DPRK (s22) | obj ROK/DPRK day 10 |
|---|---|---|---|---|
| none | 272-313, mean ~283 | 161,125,143,97,90,104,110,123,127,156,221 (mean ~132) | mean ~145 | 1360/1269, 1321/1308 |
| **half** | mean ~268 | 213,158,167,149,143,155,166,148,141,147,227 (mean ~165) | mean ~185 | **1313/1316, 1289/1340** |
| air (12) | mean ~235 | mean ~185 | (crashed day 5.9) | 1286/1343 |
- Half gives DPRK ~65-75% of ROK's flights all 10 days (control ~45-50%), objectives roughly even by
  day 10 (control: ROK +90..150). No early endgame in either. DPRK ground still falls to ~35-40%.
  Recommended default for the JSGME mod = `half`; `air` is the aggressive option. Not yet packaged as
  `half` (the built mod in MODS\DPRK Air Boost is the full 12) - swap file from `variants/half\save0.cam`.
- 0xC0000005 again on `air` seed 22 at day 5.9 (known timing crash; 1 of 6 here).

**Scripted events** (`campaign\SAVE\save0.tri`; campsim now logs `EVENT n FIRED|reset at min..` with
DPRK supply, aircraft, ground vehicles and ROK aircraft/ground, and writes `ev` + `st` per timeline frame).
Triggers that matter:
- **#11 China joins** (`CHANGE_RELATIONS 5 6 1` -> PRC becomes part of DPRK): fires if DPRK `supplyLevel <= 40`
  OR aircraft ratio DPRK:ROK*10 <= 6. **save0 starts with DPRK supply = 40, so it fires at minute 1 in
  every control/half run.** In the `air` runs it fired at min 365-425 (supply drifts to 41-44, ratio rule
  then catches it). So China is effectively "in the war from the start".
- **#12 Russia joins** (team 4 -> DPRK): DPRK supply <= 20 OR aircraft ratio <= 3. Supply never gets
  there; the ratio rule fires on **day 1** (min 1265-1505; `air` s22 at 2465 = day 2) because the DPRK
  *aircraft count stat* collapses 1200 -> ~200 within ~a day while ROK keeps ~500+. Fired in all 6 runs.
- #7 "Combined forces advanced beyond DMZ" fires at min 95-195 in all runs (ROK holds any of 8 listed
  objectives early). Events 1-6, 8-10, 13-17 never fired in 10 days (no Seoul/Pyongyang falls, no end).
- Endgame: #14 DPRK owns Seoul(228) or Pusan(417); #15 no major event for 240 h (bored); #16 day >= 15;
  #17 ROK owns any of 680/260/404 (`O` = ANY one). The earlier endgame-17 at day 2 came from that.
- Ideas (not done): ship a modified `save0.tri` in the mod - China at supply <= 25 / ratio <= 4, Russia at
  <= 10 / <= 2 - so reinforcements arrive when DPRK is actually losing, not at minute 1. Also check what a
  folded-in PRC adds (team 5: 25 squadrons, 43 battalions).

## Videos, events, China (2026-10-01 evening)

**Videos never played in this build, whatever the codec.** `PlayMovie` (`ui/src/winmain.cpp`) returned
immediately when `g_bUseD3D12` is set (D3D12 is the sole backend), because the DDraw movie player has no
surface. Both the intro (`FM_PLAY_INTRO_MOVIE`) and every campaign/UI clip (`C_Movie::Play`) go through it.
My earlier IYUV re-encode was necessary-but-never-sufficient; I never verified playback in the game - that
was wrong. **Fix (ui-adaptive 80fd6600):** `src/ui/src/MovieMF.cpp` plays `<name>.mp4` through Media Foundation
(MFPlay) in a borderless topmost window over the game window; any key/click skips. Own file because mfplay.h
conflicts with the game's DirectDraw headers; `Falcon4.vcxproj` gets `mfplay/mfplat/mf/mfuuid.lib` (the link
uses `/NODEFAULTLIB`, so `#pragma comment(lib)` is ignored). All 36 clips converted to H.264/AAC `.mp4` beside
the `.avi` in `C:\FreeFalcon6\movies` and `Theaters\Israel\movies` (ffmpeg, crf 20). Standalone test
`tools/movies/mftest.exe` played E1A (42.7 s) to the end and a mid-play screenshot showed the picture.
**Not yet seen inside the game**: deployed as `C:\FreeFalcon6\FFViper-uitest.exe` (backup
`.bak-pre-movie`); that exe also contains another session's uncommitted music edits (`cmusic.cpp`, `ui_main.cpp`).
Windowed play is assumed; exclusive fullscreen may hide the movie window. NOT in main/rail yet.

**Scripted events / News Report.** The game already records event clips: `CampEventDataMessage::playMovie` ->
`UI_AddMovieToList` -> `PlayUIMovieQ` plays the clip and `AddToNewsWindow` adds a clickable "HH:MM text" line to the
News Report window (the NEWS_FLASH button blinks). It is session-only (rebuilt empty on load), shows time of day not
the day. (`AddToNewsWindow` runs after `PlayUIMovie`, so the list filled even while the clips were skipped.) The `.evt` member of a save stores only the fired *flags*, not when. Idea
(not done): persist `day+time+event` in the save (or a sidecar), rebuild the News list on load.

**Editor:** the Triggers tab now has an "Events fired" panel (green = fired in the open file, from the `.evt`
member) and highlights the matching `DO_EVENT/RESET_EVENT/IF_EVENT_PLAYED` rows in the script listing. Open
`Auto Save.cam` to see the live game. `triggers.fired_events/event_titles`, `api_triggers` fields `events`,
`eventsFromSave`; selftest still passes (9559 checks).

**China did NOT join in the live game, so my "China at minute 1" finding is probably a campsim artifact.**
Live `Auto Save.cam` (day 0, ~4 h in): fired events = {7, 9} only; team 5 (PRC) still separate with 180 aircraft,
678 vehicles, supplyLevel 100; DPRK supplyLevel **99**. campsim logs DPRK supplyLevel 37-44 from minute 1, which is
what trips event 11 (`IF_SUPPLY 6 L 40`). Event 11/12 also have an aircraft-ratio rule (DPRK:ROK*10 <= 6 / <= 3):
live ratio 278:380 = 7.3 so neither fires yet, and will once DPRK air drops below ~60% of ROK's (China) and ~30% (Russia).
So earlier sim runs had PRC folded into DPRK from the start - **all campsim results have that distortion**; the
cause of the low supply in campsim (supply stat = `shave*100/swant` over unit supply, `team.cpp` ~2640) is still
unexplained and should be fixed or the trigger neutralised before trusting balance numbers.

**Correction/confirmation (editor check, same evening):** the user's later save `Auto Save - day2_410.cam` shows event 11
(China joins) FIRED, team 5 stats = 255 (merged into DPRK), supplyLevel 99 for DPRK, aircraft DPRK 134 vs ROK 275
(ratio 4.9 <= 6). So in the live game China joins via the **aircraft-ratio rule** by day 2, not at minute 1 - the
campsim minute-1 firing (supply) is still an artifact, but the end state (China folded in within ~2 days) is real.
Russia (ratio <= 3, i.e. DPRK < ~92 aircraft vs 275) had not fired. The editor's Events panel shows exactly this.

### Event history in saves + News Report rebuild (2026-10-01, ui-adaptive)

Built, committed on ui-adaptive, deployed as `C:\FreeFalcon6\FFViper-uitest.exe` (previous: `.bak-pre-movie`). **Not yet seen in the game.**
- `.evt` member = [short count][count x (short event, short flags)] **+ new trailer** `"EVT2"`(int) `[short n]`
  `n x (short kind, short id, uint32 campaignMs)`; kind 0 = event fired, 1 = news clip played (movie id). Older
  readers stop after the flags, so old builds still load new saves, and old saves load with an empty history.
  Code: `campupd/cmpevent.cpp` (`EventLogAdd/Size/Get`, written in `SaveCampaignEvents`, read in `LoadCampaignEvents`,
  cleared in `NewCampaignEvents`; `EventClass::SetEvent` logs the first fire), `falclib/msgsrc/campeventdatamsg.cpp`
  (logs the clip when it plays), `ui/src/campaign/campaign.cpp` (`RestoreNewsList()` adds the logged clips back to the
  News Report when the campaign screen is set up; entries read `Day N HH:MM  text`).
- Tested with campsim (`savecam=NAME` knob saves the campaign at the end of a run; same engine patch applied in the
  main tree, uncommitted): saved -> history present -> loaded and re-saved -> identical history.
- Editor: Victory tab "Events fired" chips show `Day N HH:MM` and a timeline of events + news clips. Old saves
  show only the fired flag. selftest 9559 checks, 0 failures.
- Limits: 64 history entries; times are campaign ms; only saves made by this build carry history; `main` and the rail
  branch do not have these commits yet.

## Reinforcements: what actually happens (2026-10-01 night)

**Correction.** I told the user air/naval reinforcements "never arrive" because I counted units with `reinforcement > 0`.
That field is NOT cleared on release (squadrons/task forces keep their level); only the `U_INACTIVE` flag says pending.
campsim now logs `REINF h=N` every campaign hour (units really inactive, by kind, with the lowest release level, next to each
team's counter; `staleInInactiveList` = released units still in `InactiveList`, ~100 by h12, harmless but untidy). Result:
squadrons DO arrive as the counter passes their level (ROK-side squadrons 30 -> 29 -> 26 -> 22 by h19; counter +1/h).
The team counter is the *merged* team's: US units report `GetTeam()==2`, so they use team 2's counter (team 1's stays 16).

**Real bug found: ground reinforcement hours are ignored.** `BattalionClass::BattalionClass(stream)` (`camptask/battalio.cpp`)
runs `GroundUnitClass(stream)` (which reads the saved release level) and then `InitLocalData(NULL)`, whose `else` branch does
`SetReinforcement(0)`. So every battalion loaded from a save has level 0 and is released at the first hourly tick, whatever
hour the scenario gave it (DPRK 12..96, ROK 6..48, US 12..48; 74 battalions in save0). Confirmed in campsim: ROK-side
battalions in the inactive list 52 -> 1 by hour 8, DPRK 22 -> 0, while the counter is only 9; `REINF h=0` shows min level 0.
In live saves the 74 are all active within ~4 h. Aircraft/ships stagger correctly.
**Fix:** save the level before `InitLocalData(NULL)` and restore it (toggle `g_nBattalionReinforceFix`, default 1, `0` = old).
In the main tree only so far (uncommitted); results of fixed-vs-old campsim runs below when finished.

**Fixed vs old, campsim (4 days, fair cfg, seeds 11-13; 2026-10-01):** with the fix, DPRK's 22 reserve battalions stay
pending at h8 (min level 12) and 19 of them are still pending at h29 (min level 48; the ROK side holds 13-19 too) - i.e.
reinforcements now trickle in over days as scenario authors set them, instead of all landing in hour 1-2. Outcome at day 4
is within noise of the old behaviour: ROK/DPRK ground vehicles 76%/34% and 72%/34% (fix) vs 67%/35% and 75%/33% (old);
objectives ROK 1326/1343 vs 1353/1340; airborne flights unchanged. Seed 13: fix crashed 0xC0000005 at 0.8 d (known
timing crash), old ended "we win" (endgame 17) at 2.4 d. Default stays 1 (`g_nBattalionReinforceFix`). Not committed; not in
the ui-adaptive/rail trees or any game exe yet.

## 2026-10-01 evening: sim fidelity, Campaign Lab, crash + freeze fixes (branch main, no rail)

Install: `C:\FreeFalcon6\FFViper.exe` = main build (rail kept as `FFViper.exe.bak-rail` + `RedViper.pdb.bak-rail`).
Newest build staged as `FFViper.next.exe` / `RedViper.next.pdb` (game was running): rename over FFViper.exe / RedViper.pdb.

- **campsim skipped the new-campaign setup** (`AdjustCampaignOptions`): unit supply stayed ~37% so DPRK's supply stat fired
  China at minute 1. Knob `newgame` (default on when no event fired), `ratio=G:A:D:N`, `exp=A:G`. Now DPRK supply 99-100%
  like the live game; China fires at ~h4-6 via the air ratio (live: Day 1 16:26, ~7.5 h in).
- **Campaign Lab** GUI: `python tools/campsim/serve.py` -> http://localhost:8770/ (runs, progress, overlay charts incl. the
  China/Russia trigger stats, event table, crash stacks).
- **Balance finding**: Blue wins (event 17) at h30-70 in every completed run. Loss attribution (`gLossDiag`): ~92% of DPRK
  ground losses are air strikes (2,152 vehicles by h6, 4,890 by h36; ~355 from ground combat). Lever: Falcon4.AII
  `2DHitChanceGround` (1.5): 3 -> win h50-58, 5 -> h57-69, 8 -> h93. Symmetric, but Blue loses only ~41 vehicles to air by
  h24. Objective strikes don't use it. Undecided: Andrew dismissed the question; nothing changed in game data.
- **Fixed: day 2-5 crash** - VuEntity refcount was ushort; local session leaks ~4,000 refs/h, wrapped at ~h14, deleted
  the live session. 32-bit now (Rebuild All). Six 5-day runs incl. two crashing seeds pass.
- **Fixed: reinforcements re-announced hourly** - SetInactive(0) cleared the flag before InactiveList->Remove, which the
  filter refused; arrived units stayed listed (55 at h12 -> 0).
- **Fixed: permanent campaign freeze** - NEW_SYNC handshake waited INFINITE both ways; caught live (clock stopped,
  both threads waiting, flags not paused), resumed by setting the named events. Waits capped at 250 ms, `THREADSYNC` log.
- **WIP: player-held ground orders** (`g_bPlayerGroundHold`, default 0). Andrew chose "hold until arrived" + map marker +
  cfg toggle. With the GTM guard, campsim `holdtest=8` shows held units that stop moving (s6: 1/8 reached) and a
  playerhold=0 run that died after setup. Next: find what moves a brigade element when the GTM doesn't order it.
- Other: VuLinkedList::Remove(VU_ID) iterator-after-erase, PackageListCounter::DelObj erase(end()).
- Git: `main` synced with origin/main; ~12 commits on top, nothing pushed. `.git` holds a 4.66 GB cruft pack from an
  aborted `git stash -u` that walked the gamework junctions (gamework now in .git/info/exclude); `git gc --prune=now` removes it.

## 2026-10-03: China/Russia, GTM reserves, challenge sliders (sim only, nothing shipped)

- **Challenge sliders are the strongest built-in lever.** Ground Forces 1 notch toward Red (DPRK keeps 16 company
  slots, Blue 12): Blue wins h41 -> h57 (+Rookie skill) / h67 (+Veteran), Blue a2a losses 126 -> 242/292. Skill values:
  Recruit 0, Cadet 1, Rookie 2, Veteran 3, Ace 4. Air Forces slider: no effect on outcome. (Pre-clock-fix numbers.)
- **China/Russia ground never fight.** PRC's 43 bns (783 veh) start ~340 km behind the front (BMS ~220 km), fold into
  DPRK when China joins, and stay RESERVE/assigned=0. Ruled out: AII path limits (x4), staging 130 km behind front
  (make_ally_mod.py prc), supply (89-100%), reserve throughput (GtmReservesPerCycle 1 -> 20 moved 2 -> 72 DPRK units
  per 6 h, outcome unchanged), farthest-first reserves (GtmReserveFarthest: PRC moves ~25 km in 5 days). A 2-3 day war
  is simply over before they arrive. GTM diag: DPRK is "defensive" all war; DEFEND has 1-3 open objectives per call.
- **Fixed:** GTM reserve step kept the FIRST candidate, not the closest (removed the new best instead of the old).
- **China air** (FF6 25 sq/530 ac on paper; only 80 fighters active day 1 vs BMS 144 modern): make_ally_mod.py
  prcair (squadrons on DPRK airbases, reinforcements active) -> Blue a2a losses +52%, ground lost to air +68%, win
  time ~same. With g_bEnableABRelocation 1: 23 squadrons rebase (vs 6), day-1 sorties +20%, no further change.
  Relocation code only moves squadrons off CAPTURED bases (or too near/far from front), not bombed ones.
- **campsim clock fix:** SimLibElapsedTime now follows game time like the game's timer thread; clock-gated code
  (rebasing, scramble) runs. Baseline Ground1+Veteran win moved h67 -> h48: earlier sims understated Blue's speed.
- Engineers: DPRK 13 vs ROK 8; bridges are rarely the bottleneck (0-4 blown); engineer requests are dead code
  (gndtaskingmsg.cpp handler commented out); all units are division 0. Engineers die like everything else (13 -> 1-2).
- Andrew chose "Nothing yet" for shipping China-air / relocation; 2DHitChanceGround question dismissed twice.

## 2026-10-03: where the campaign stands, and what would make it fun (read this first next session)

### Measured state of Korea save0 (main build, no rail, campsim with game-accurate setup + clock)
- Blue wins (event 17: owns Pyongyang and/or Wonsan) in **every** run: ~h40-55 at stock challenge settings, live
  game h26. Sequence matches live: E5 at start, China ~h5 (air-ratio trigger, airbases bombed), Russia ~day 2.
- **Air decides everything**: ~92% of DPRK ground losses are air strikes (2,152 vehicles by h6, 4,890 by h36; ground
  combat ~355). DPRK air barely touches Blue ground (~40-200 vehicles over days). DPRK posture is "defensive" the
  whole war; it never runs an offensive.
- **China/Russia are cosmetic**: their armies fold into DPRK and never move (NOT fixed - see below); China's air can
  matter (+50% Blue aircraft lost when staged on DPRK bases) but doesn't change the outcome.
- Levers measured (see earlier sections): 2DHitChanceGround (1.5 -> 5: win ~h57-69), Ground Forces slider (1 notch to
  Red: +40-60% war length), enemy skill (Veteran: 2.3x Blue a2a losses), DPRK Air Boost mod (more sorties, but Blue
  wins *sooner* because China/Russia triggers fire later), reserves/relocation (no outcome change).

### Chinese troop movement: NOT fixed (root cause understood)
The GTM only assigns DPRK's idle units via the reserve step (1 per cycle; DEFEND has 1-3 open slots), and every
candidate is scored by distance, so units 300+ km away are never chosen, and even "farthest first" moves them ~25 km in
5 days. The war is over before they could arrive. Fixing the AI's choice is not enough; China needs to *arrive*:
1. **China entry as a reinforcement wave** (best bet): place PRC battalions inactive (U_INACTIVE + reinforcement level)
   in DPRK's rear / along the Yalu crossings, and release them when event 11 fires (trigger script or engine hook on
   CHANGE_RELATIONS), so they appear within ~100 km of the front as a counteroffensive.
2. **Flip DPRK to OFFENSIVE when China joins** (TeamClass::SelectGroundAction / .tri), so CAPTURE orders exist and the
   new units are tasked instead of parked in reserve.
3. Measure with campsim (`ally_track.py`, GTM log) before shipping.

### What would make the war fun and challenging to manage (proposals, none built)
1. **Pace**: a war that lasts days-weeks, not hours. Air must stop being an instant win button:
   - attrition for Blue air (SAM/AAA belts near the front, enemy skill), weapon stockpiles that run out (squadron
     stores / supply), weather and night limiting strike sorties, AII FlightCombatRate / tasking intervals,
     and/or 2DHitChanceGround.
2. **A real enemy plan**: DPRK opening with an offensive across the DMZ (it never attacks today), Scud/SOF strikes on
   Blue airbases, mobile SAMs, counterattacks when Blue overextends.
3. **Phases with stakes**: DMZ defence -> counteroffensive -> push north -> China intervenes as a real second front
   (above) -> negotiated end / ceasefire events, instead of one capture ending the war.
4. **Player agency on the ground**: finish the player-held ground orders (WIP, g_bPlayerGroundHold - some held
   units stop moving), visible priorities/PAKs, logistics that matter (power grid already shown on the Production
   layer; supply depots, bridges).
5. **Limited resources to manage**: fewer Blue aircraft (Air slider), reinforcements on a schedule, losses that hurt.
6. **Tools**: Campaign Lab (python tools/campsim/serve.py) to measure every change; make each proposal a JSGME mod
   or cfg toggle and A/B it with 4+ seeds.

## 2026-10-03 (later): "hold both cities" test, PAKs, the day-4 freeze, supply logger

**Win condition.** save0.tri event 17 is `#IF_CONTROLLED 2 O 680 260 404` - OR: any one of Pyongyang (PAK 260,
which contains 680) or Wonsan (404). Wonsan ended every run (12/12, h31-h55). Requiring all three (`A`, file
`both.tri`, campsim `tri=both`): Blue won 2 of 12 in 10 days (h68, h96). Pyongyang PAK slider at max (`pak=260:100`)
did not help (0/4, same seeds 1/4 without it).

**Wonsan garrison:** ~22 bn / 430 veh within 20 km at h36; 17 destroyed by h48 (theatre: 1,251 DPRK veh lost to
air vs 71 to ground in that window). Reinforcing it adds targets.

**The day-4 freeze (bigger than balance).** In all 8 noend runs nothing changes after ~h96 for 6 days: no
captures, zero losses either side, Blue parked 17-58 km from Pyongyang with ~270 flights airborne. Blue's GTM is
OFFENSIVE and issues ~30 CAPTURE assignments per 6 h, but only 2-3 of 227 battalions hold one; a capture unit
near Wonsan (obj 405) sat "moving" 0 km for 80 h. ~15 Blue spearhead battalions at 1-2% supply. Supply logger:
ROK resupplies where nothing arrived = 4,929 of 7,929 (62%), fuel delivered 64% - deliveries fail along the
long road to the front (suspect GetObjectivePath search limits). Same family as "China never moves". Next:
instrument why CAPTURE orders are dropped and why the supply path fails.

**PAKs** (team.cpp, gtm.cpp): per PAK, AI ground_priority = air_priority = front proximity (<=40) + assigned
strength (+-30) + objective priority bonus; the player's slider sets player_priority, used ONLY by air planning
(OCA target choice +50 for the ground-action PAK, -200 last target; every mission request's 0-100 PAK term; 0
cancels missions there). Ground uses the AI number. `#SET_PAK_PRIORITY` locks a PAK (event 3 pins Pyongyang 100);
`#CHANGE_PRIORITIES` is commented out in the engine - a no-op.

**Supply chain** (supply.cpp): factories/army bases/depots/ports -> supply + replacements, refineries -> fuel,
x nearest non-hostile power station (power plants AND type 17 "nuclear"; DPRK owns 14 nuclear, 0 power plants);
captured sites produce 0. Units draw <=50% of need per cycle, routed from FindNearestSupplySource with 2%/node
loss + damage. Trigger supply% = units have/(have+need) - stays 96-101% for DPRK all war (so China only enters on
the air ratio). DPRK supply capacity falls to 41% by day 10. Replacement pools pile up unused (ROK 15,593 by
day 10; battalions got ~212 total) - Korea runs the old replacement code (NoTypeBonusRepl absent = 0); verify
before acting. Logger: `"sp"` frame field + supply_report.py; engine counters gSupplyDiag/gSupplyRatio.

## Lost enemy ships stay on the map as dim ghosts (2026-10-04)

Stock hid every movable enemy unit the moment its spotted timer lapsed (`UI_Refresher::UpdateMapItem`,
urefresh.cpp), so ships popped out of existence at sea. Now an enemy task force the player has SEEN stays
at its last known position as a dimmed icon (`MAPICONLIST::Ghost`, drawn at `CampMapGhostBright`, default
0.30); the tooltip says "last known position" and shows no live detail. Position, strength and heading are
frozen. It goes away only when the unit is removed (destroyed / inactive). A ship never spotted stays hidden.
Last-known positions live in a static map in urefresh.cpp, so they survive icon-list rebuilds but not a
restart or a save/load. Config: `set g_bCampMapShipGhosts 0` = stock, `set g_fCampMapGhostBright 0.3`.
Header change (new MAPICONLIST member): Rebuild All. Harness: `tools/uitest/ghost.txt` with the new
`spotships 1|0` command; `listships` now prints the ghost flag. Verified hidden -> shown -> dim ghost.

### Wrecks and tooltip follow-ups (2026-10-04)

- A destroyed ship now leaves a dark wreck marker (`MAPICONLIST::Ghost == 2`, half the ghost brightness).
  `UI_Refresher::RemoveMapItem` orphans the icon when the entity is gone/dead and the player could see it;
  the marker never updates, is skipped by `CheckHotSpots`/`MouseOver` (no click, no tooltip), and
  `C_MapIcon::AddIcon` replaces it if a new unit reuses the id. It lasts until the map is rebuilt.
  `set g_bCampMapShipWrecks 0` = stock. Harness: `tools/uitest/wreck.txt` (`sinkship <team> [n]`).
  Note: `ce->IsDead()` through a CampEntity pointer returns 0 for a unit with U_DEAD set (UnitClass's
  `IsDead() const` does not override FalconEntity's non-const one), so dead ships keep their icon until the
  campaign deletes the unit; the wreck path keys on the entity being gone from vuDatabase.
- Tooltip: ship tips had no name ("- supply 100%"); now falls back to the unit class name. The output
  thread only called Update() (the only place a tip is drawn) when a window was dirty; it now also wakes
  once a tip's delay has passed. NOT reproduced as the cause: the harness screen is never idle, so the
  tooltip worked with or without that change. Real-mouse failure still unexplained.

### Recon hang root cause + hang watchdog v2 (2026-10-04)

FFHang.dmp from a live freeze (read with cdb + RedViper.pdb): the UI **output thread** was in
`C_Handler::Update -> C_Window::DrawWindow -> C_3dViewer::ViewGreyOTW -> RenderOTW::PreLoadScene ->
ObjectLOD::WaitUpdates`, spinning at ObjectLOD.cpp `while (not TheLoader.Paused());`, **holding the handler
critical section**; the window thread then blocked in `C_Handler::EnterCritical` (ProcessUserCallbacks). The
loader was stuck at `Loader::paused == PAUSING` with its thread asleep in `WaitForSingleObject(INFINITE)`:
the pause wake-up was lost. `TextureBankClass::WaitUpdates` already had a 2 s timeout for exactly this race
(an old "PHASE 5 hang-fix" comment); `ObjectLOD::WaitUpdates` and the xobjectlod copy did not.
Fix: ObjectLOD/xObjectLOD WaitUpdates re-send the pause request while waiting and bail after 2 s;
`Loader::MainLoop` waits 250 ms instead of INFINITE. The exact lost-wake interleaving is not proven.
Watchdog v2 (winmain.cpp): RtlVirtualUnwind stacks with module+offset, plus FFHang.dmp (MiniDumpWriteDump via
dbghelp, loaded on demand). Harness: `freeze <ms>` + `tools/uitest/hang.txt`. Analyse with:
`cdb -z FFHang.dmp -y <build dir> -c ".lines -e; ~*kc 15; q"`.

### Music trace (2026-10-04)

`[MUSIC]` lines in FFDebug.log (cmusic.cpp): Stop/FadeOut_Stop/Pause/FadeOut_Pause/Resume/PlayQ with flags and
stream state, and every stream callback (msg 2 = FADE_IN_DONE, 3 = FADE_OUT_DONE). In the harness the intro
movie handshake is healthy (FadeOut_Pause -> FADE_OUT_DONE -> Resume -> FADE_IN_DONE); the user's silence
after the intro video is not reproduced. Candidate to check in his log: a FADE_OUT_DONE arriving after
Resume with flags == 0 takes gMusicCallback's else-branch and StopStream()s the resumed stream.

## 2026-10-03/04: campaign AI fixes, Korea Escalation mod, initiative (read this first for the ground/air war)

Andrew's rule: fix AI mechanics (paths, supply, orders) before balance; then make China/Russia matter. Every
change is an f4config switch; `set g_x 0` in FFViper.cfg restores stock. campsim knob in brackets.

### Commits (main, local, not pushed)
- 58af75fa trigger log: the .evt member gets a CND1 trailer recording which #IF branch fired each action;
  the campaign editor lights only the branch that ran. (China in china_test fired at 14:05 day 1 on the
  air-ratio check, DPRK 258 vs ROK 391 aircraft.)
- f90bb897 **GtmCaptureFront** [capfront]: capture targets also include non-secondary front-line objectives
  (stock: only secondaries <= 3 links from the front, so a chain of bridges/junctions froze a front).
  Pyongyang reached in 8/8 runs vs 2/8.
- 1494ce22 **SupplyNeedFix** [needfix]: a squadron's need counts only its own stores-table weapons; stock
  summed off-table stores, the team's need went negative and ALL battalion resupply stopped from ~h12.
- 144223e4 / ea1717cf **GridPathPartial** [partialpath]: grid and objective routes over the 96-step cap
  follow the partial route; a stale `last_obj` (85-140 km behind) is replaced. Path failures 9,765 -> 12.
- 9b55d4dc SelectAirActions crash with no front left; a8142c2d AllUnitList duplicates (airmobile infantry
  re-inserted per helicopter trip, up to 28 copies -- earlier GTM candidate counts were inflated).
- 4e5959a6 **ReserveHold** [reshold]: reserves on our own objectives away from the front stop hopping
  back 3 links every check (halved order churn).
- 33fa3f11 **SupplySplitShares** [splitshares]: ground and air get separate shares of the supply pool
  (squadrons' never-used stores gap diluted battalions to ~4% a trip). ROK battalions under 50%: 34 -> 0.
- 27b36e59 all of the above ON by default; GtmReservesPerCycle 8, GtmCaptureUnits 3. Falcon4.AII
  ObjGroundPathMaxCost 500 -> 2000 (rear units were never picked for orders) -- now shipped in the mod.
- fb01730f intel bar charts: Airbases/Aircraft/Ground Vehicles drew fuel/airbases/aircraft.
- 146ab072 the naval/UI thread's work (ship ghosts/wrecks, naval AI legs, Recon hang fix, music trace),
  committed at Andrew's request.
- 44e938b6 **ReserveNoPullback** [nopullback] (ON since aaef1490): a healthy unit whose capture target was
  taken by a neighbour holds it instead of falling back 3 links as RESERVE (BattalionClass::MoveUnit, ~300
  a day for ROK, ~49 km back each -> 24-87); idle healthy units within 25 km of the front get no reserve
  order (reserve objectives are always 20-60 km back). **EnemyTownPathCost** [towncost] (stock 4): route
  cost through enemy towns; at 4, 3% of capture orders drive 30%+ further (Sep'o depot 110 km vs 50 km).
  Player drag-orders use the same route builder (tactical_set_orders -> BuildGroundWP).
- 40324378 campsim diagnostics: SCRAMBLE, ATMWHY, INIT (see below).
- aaef1490 **AlertScramble** [scramble], **InitTrueLosses** [truelosses], **CounterAttackInitiative**
  [counterinit]; d6fbd2cb **CaptureInitiative** [capinit] and the NOFLIGHT diagnostic.

### Root causes found 2026-10-04
- **Scrambles never happened:** alert flights always hold 2 planes; counter-air requests are set to 4
  (FindBestAir: `mis->aircraft = 4` when match_strength); FindBestAirFlight rejects any flight with fewer
  planes. DPRK: ~73,000 rejections / 24 h, 0 scrambles. AlertScramble: ~700-800 DPRK, ~150 ROK a run.
- **DPRK never attacks** (China/Russia folded in never got a capture order): a DEFENSIVE side stays
  defensive while any enemy is on the offensive (ROK is, all war); offensives need initiative >= 50/55;
  initiative is one 100-point pool with the player's team; ROK's loss term is pinned at 100 because
  reinforcements raise its start baseline (losses read 0-13); and every captured objective hands the
  captor 5 points (objectivemsg.cpp), ~600 captures in 2 days -> DPRK ~6. A MINOROFFENSIVE never captures.
- **China/Russia as separate factions** (campsim escalfac.tri: Friendly with DPRK, War with Blue): worse.
  Non-player teams' initiative drifts to ~40 (leak code); they sat idle ~130 km back. Keep the merge.
- **DPRK strikes**: packages fail on "no aircraft free" (NOFLIGHT, DPRK strike 18,177 / 24 h vs range 937),
  not on broken loadouts. DPRK builds about as many strike packages as ROK (255 vs 319 / 24 h).
- **Front units sent back as "Reserve <town>"**: stock GTM reserve step keeps the candidates CLOSEST to the
  action objective (the comment says farthest) and reserve objectives are 20-60 km back; the bigger source
  was MoveUnit's 3-link fall-back after a neighbour captured the target (fixed by ReserveNoPullback).

### Korea Escalation JSGME mod (C:\FreeFalcon6\MODS\Korea Escalation; tools/campsim/make_escalation_mod.py)
Byte-identical to campsim's gamework `escal.cam`/`escal.tri` (run with `--save escal --set tri=escal`).
- save0.tri = gamework bothwr.tri: Blue wins only with Pyongyang AND Wonsan; Russia joins when Wonsan falls.
- save0.cam: China and Russia staged 100-170 km behind the front (make_ally_mod "prc+cis"), every PRC
  battalion and active squadron cloned once (clones: no brigade, U_PARENT), and DPRK bombers laid out like
  vanilla's Tu-16s: H-6A (China's Tu-16) squadrons, 1 active at Sunan, reinforcements Sunan h48 / Toksan h72.
- Falcon4.AII with ObjGroundPathMaxCost 2000. The install's own Falcon4.AII is stock again
  (backup Falcon4.AII.bak-objpathcost500), so play with the mod enabled.

### Measured (campsim, mod, Andrew's sliders 2:2:2:2 + Ace, 6 seeds x 4 days, medians)
"War ends" = Blue holds both Pyongyang and Wonsan (the mod's rule); Pyongyang is always the later one.

| Setting | War ends | Wonsan falls | DPRK offensive | Red capture orders/h (China+Russia) | Scrambles DPRK/ROK |
|---|---|---|---|---|---|
| stock rule (either city), fixes on | h34-45 | h34-45 | 0% | 0 | 0 / 0 |
| mod, ReserveNoPullback on | h72 | | 0% | 0 | 0 / 0 |
| + AlertScramble | h65 | | 0% | 0 | 658 / 142 |
| + scramble, true losses, counter 20, capture 5 | h77 | | 2% | 6.9 (2.6) | 770 / 180 |
| + ... counter 15, capture 2 (**live cfg**) | h81 | h44-63 | 4% | 9.8 (4.2) | 829 / 178 |
| + ... counter 10, capture 2 | h74 | | 4% | 10.7 (4.9) | 773 / 228 |

China in the mod loses ~55% of its 1,566 vehicles (stock China ~70); Russia staged takes losses after joining
but never advances. DPRK bombers: ROK ground lost to air by h48 187 -> 207. Blue wins every run.

### Live setup (2026-10-04)
FFViper-ai.exe built from the clean worktree ff-ai-build at d6fbd2cb (backups .bak-27b36e59, .bak-fb01730f).
FFViper.cfg block appended (backup FFViper.cfg.bak-pre-escalation): AlertScramble 1, InitTrueLosses 1,
CounterAttackInitiative 15, CaptureInitiative 2. Enable "Korea Escalation" in JSGME, start a NEW campaign.

### Force counts on paper vs Falcon 4.0 and BMS 4.37
Save files as shipped; a new campaign rescales them by the challenge sliders (at 2:2:2:2 campsim's DPRK starts
with ~7,050 vehicles, not 10,320). Active units, then inactive reinforcements. BMS units are read with a
lighter decoder (its squadron records are longer); treat its active/reinforcement split as approximate.

| Save | Side | Battalions | Ground veh | + reinf bns / veh | Squadrons | Aircraft | + reinf sq / ac |
|---|---|---|---|---|---|---|---|
| FF6 save0 | US | 32 | 592 | 20 / 613 | 19 | 397 | 21 / 361 |
| | ROK | 183 | 6,616 | 32 / 1,380 | 30 | 756 | 9 / 208 |
| | Japan | 1 | 32 | - | - | - | - |
| | DPRK | 405 | 10,320 | 22 / 680 | 46 | 1,120 | - |
| | China | 43 | 1,068 | - | 13 | 267 | 12 / 263 |
| | Russia | 8 | 216 | - | 4 | 43 | 4 / 64 |
| | **Blue** | 216 | 7,240 | 52 / 1,993 | 49 | 1,153 | 30 / 569 |
| | **Red** | 456 | 11,604 | 22 / 680 | 63 | 1,430 | 16 / 327 |
| FF6 + Escalation | China | 86 | 2,136 | - | 26 | 534 | 12 / 263 |
| | DPRK air | | | | 47 | 1,152 | 2 / 64 |
| | **Red** | 499 | 12,672 | 22 / 680 | 77 | 1,729 | 18 / 391 |
| Falcon 4.0 save0 | US | 8 | 234 | 24 / 1,016 | 11 | 228 | 13 / 280 |
| | ROK | 137 | 4,691 | 32 / 1,315 | 12 | 288 | - |
| | Japan | 5 | 83 | - | - | - | - |
| | DPRK | 405 | 14,171 | 27 / 1,109 | 27 | 648 | 13 / 270 |
| | China | 43 | 1,829 | - | 10 | 212 | - |
| | Russia | 8 | 232 | - | 8 | 139 | - |
| | **Blue** | 150 | 5,008 | 56 / 2,331 | 23 | 516 | 13 / 280 |
| | **Red** | 456 | 16,232 | 27 / 1,109 | 45 | 999 | 13 / 270 |
| BMS 4.37 Save0 | US | 1 | 17 | - | 18 | 249 | - |
| | ROK | 228 | 6,182 | - | 4 | 94 | - |
| | DPRK | 161 | 3,959 | - | 15 | 260 | 3 / 50 |
| | China | 39 | 948 | - | 12 | 178 | - |
| | Russia | 0 | 0 | - | 4 | 42 | - |
| | **Blue** | 229 | 6,199 | - | 22 | 343 | - |
| | **Red** | 200 | 4,907 | - | 31 | 480 | 3 / 50 |

DPRK air by type. FF6: 1,120 active (fighters 480: MiG-21/23/29, J-7; attack 368: H-5 x160, Su-25BM, Q-5,
A-5, Su-7; helicopters 128; transport 144), no bomber-role squadrons. Vanilla: 648 + 270 (MiG-19/21/23/29,
Su-27 and MiG-25 in reserve; Il-28, Su-25, Mi-24; **Tu-16 bombers 24 + 48**). BMS: 260 + 50 (MiG-21/23/29,
J-5/6/7/8, Q-5N, Su-25, Il-28). Vanilla's Red ground force is the largest (16,232 vs FF6 11,604 vs BMS 4,907);
FF6's Blue air is twice vanilla's and over three times BMS's.

### Open
- Counterattacks only ~4% of hours: capped at one per action timeout (18 h). Next lever: that cooldown.
- Russia staged but never advances; China never reaches the front line in force.
- DPRK strike/CAS is aircraft-bound ("no aircraft free"); Blue air still decides the war (~5,200 DPRK
  vehicles lost to air by h48 vs ~200 ROK).
- Force counts above are on paper; compare post-slider counts in a new campaign if balance work starts.

### Tooltip: real-mouse failure fixed (2026-10-04)

The tooltip never showed in the real game although it did in the harness. The `[TIP]` trace (chandler.cpp
`TipLog`, deduped, 1500-line cap; plus `[TIP] map icon ... hook` in cicons.cpp) showed correct text being armed
(objectives, ships, battalions, even the stock "Time Acceleration" button) and **no `show` ever**: the game
re-delivers WM_MOUSEMOVE at the SAME pixel while the mouse is still (55-65 per rest), and each one ran
`HelpOff()` + re-arm, restarting the 250 ms delay. Fix in `C_Handler::ProcessMessage` WM_MOUSEMOVE: a move to
the pixel the pending/showing tip was armed at is ignored. Confirmed working by the user. Also: ship tooltips
had no name (task forces return an empty GetName) -> falls back to the unit class name; and the output thread
now wakes Update() when a tip's delay has passed (idle screen) -- not the cause, harmless. The `[TIP]` logging
can be stripped.
Observed, not fixed: a corvette task force tooltip read "supply 250%" -- task force supply can exceed 100, so the
"%" label is wrong and `NavalPlan`'s "go to port below 30% supply" rule should be checked against the real
range of `GetUnitSupply()`.

### Campaign map zoom to the mission (2026-10-04)

`C_Map::FitFlightPlan` (cmap.cpp) hard-coded 1640 ft/pixel (and `/ 1000` = 1.64 per stock pixel). The terrain-
derived map is finer (`s_mapFeetPerPixel`), so waypoint-area centre and zoom came out at half the true value:
the view sat north-west against the China border. Now uses `FEET_PER_PIXEL`; identical maths for the stock map.
Harness: `fitflight [team|-1]` + `tools/uitest/fit.txt` (needs ~90 s after START_CAMP for flights to exist).
User confirmed it works.

### Add Package: two Add Flight windows (2026-10-04) -- NOT reproduced

User screenshot: two ADD FLIGHT windows, one filled in, one with only the static defaults. Checked: Te_flght.scf
defines one TAC_FLIGHT_WIN; PACKAGE.scf comes from CMN_SCF.LST, Te_flght.scf only from cp_pkg_scf.lst (campaign)
and TE_SCF.LST (TE); FFDebug.log shows exactly one load per session; the harness (1932x768 and 1365x768) opens
one window via right-click -> Add Package -> NEW. `[UIDUP]` (cparser.cpp) logs any window id loaded twice. The
user later said it "seems alright now". If it returns: send FFDebug.log and note the exact clicks.

### Other 2026-10-04 facts

- Installed exe: `C:\FreeFalcon6\FFViper.exe` was NOT the stale Sep 26 build I first claimed; it is whatever the
  user's VS build last copied (check LastWriteTime, not memory). The user also runs `FFViper-ai.exe` (a build of
  the AI-fix worktree `ff-ai-build`); these UI changes are only in the `freefalcon-central` main checkout.
- `FFViper.cfg` `set g_nUiWidth/Height -1` overrides `-uisize` on the command line; to test another surface size
  edit the cfg (and restore it). `tools/uitest/run.ps1 -ExeName FFViper-test.exe` runs a scratch copy while the
  game is open.
- Harness commands added this session: hovericon/hovership, rclickship, listships (prints ghost flag), mapmove,
  mapzoom, curinfo, spotships, sinkship, compress, fitflight, freeze. Scripts: hover*.txt, ghost.txt,
  wreck.txt, music.txt, fit.txt, hang.txt, addpkg3.txt, recon*.txt, ship*.txt.
