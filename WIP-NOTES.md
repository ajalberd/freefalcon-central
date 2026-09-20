# Work in progress

Rolling handoff note. Read this first. Keep it short — when something is settled, cut it
down to the one fact a future session would otherwise re-derive, and let the code comments
and commit messages carry the rest.

Companion docs:

- `CAMPAIGN-SUPPLY-ENGINE.md` — how supply, production and power actually work.
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
- `RENDER-LIGHTING.md` — **the renderer is LDR end to end**, so GT7's tone curve is not a
  drop-in; the HDR pipeline is the project and the curve is its last step. No shadow
  machinery exists at all. The "missile shading issue" is the fin flicker.
- `COCKPIT-OVERHAUL.md` — the cockpit displays use a **three-size GIF+`.rct` bitmap font
  set**, separate from the `.bft` menu fonts, and `g_rttFontScale` magnifies glyph geometry
  without touching UVs — the source bitmap is a hard ceiling on sharpness.

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
| **CPU near-clip is 1.0 ft globally** | Fixed for RTT quads only. The same 1.0 ft clip still applies to **all BSP geometry**, so any cockpit surface within a foot of your eye in VR is being clipped *and displaced* (`polylibclip` relocates rather than rejects). Lowering the constant to match `ZNEAR` is the principled fix and needs its own testing — it touches terrain and objects too. |
| 3D kneeboard map uploads every frame | `Render2DBitmap` → `DrawBitmap2D` creates and destroys a temp texture per call: ~2.7 MB/frame at the stock 3× supersample. The raster itself is throttled (the map window is fixed for a mission). Fix if it costs frames: a persistent `TextureHandle` + textured fan, as `RenderGMComposite::DrawComposite` does. |
| `SimNWSToggle` not in `controls.xml` | Registered in `findfunc.cpp` but absent from the function catalogue, so it cannot be bound from the Controllers page. That file also stores user bindings and is rewritten by the game — edit with care. |
| Half the farthest terrain ring has no texture | `L3 lod=4 posts=304130 tiled=154040 noSrv=150090` — 49% of the far ring has no SRV resident (L2 is 16%). `tex=0` on those rows means `lod > LastNearTexLOD()` (far texture set), not "untextured". The far ring is the horizon. |
| Damage does not feed **link cost** | A dropped bridge is expensive to cross but pathfinding still routes over it. Touches everything walking the objective graph. |
| Missile fin flicker | See `RENDER-LIGHTING.md`. `dwzBias` was restored and did **not** fix it. Next move is a two-frame capture, not more reading. |
| Dead 2D/3D cloud path, two latent bugs | Only reachable with Z-buffering off, so harmless today. (a) `Drawable2DCloud::Update()` is never called, so `position`/`cloudTexture` are never assigned — and `DrawableObject`'s ctor does not initialise `position`, so `Draw()` would build an 80,000 ft quad from uninitialised memory. (b) `RealWeather::Setup`'s loop calls `real2DClouds[i].Setup()` outside the guard that allocates the array. |
| Objective icons shaded by health | Asked for, not built. The damage overlay exists but must be switched on; the request was to darken the red icons themselves. |
| Pale circles on the campaign map | Pre-dates this work. Ruled out: Logistics overlays, threat rings (`ShowCircles` is inside `#if 0`), the terrain basemap, the waypoint list. Cheap test: zoom — map imagery scales, drawn marks do not. |
| **Add Squadron** dead in campaign | Stock. `ObjMenuOpenCB` clears `C_BIT_ENABLED` when `GameType == 1`; basing a squadron is a TE-editor operation. |
| Menu is fixed-resolution | UI95 is not a layout engine — absolute pixel rects from `.scf` with art authored to match. Routes: scale the composited surface, or re-author every resource. |

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
