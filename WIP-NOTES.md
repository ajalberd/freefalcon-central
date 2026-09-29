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
  sun shadows exist (D3D12; the fit box was stale until 2026-09-27); no world shadows. The "missile shading issue" turned out to be the fin z-fight, not
  lighting — see `OBJECT-RENDERING.md`.
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

## Housekeeping

`main` is pushed. **PR #53** (`ajalberd:main` → `FreeFalcon:develop`) carries the campaign
planning and map work. PRs #49/#50/#52 are fully subsumed by `main`.

Two things that cost time before:

- **Check subsumption with `git cherry -v main origin/<branch>`, not a diff.** These branches
  were cut from `develop`, so `git diff main...origin/<branch>` answers a different question
  and showed 746 phantom insertions where the real answer was zero.
- `gh` resolves this checkout to upstream `FreeFalcon/freefalcon-central` while the branches
  live in `ajalberd/freefalcon-central`, so a PR needs `--head ajalberd:<branch>`.
