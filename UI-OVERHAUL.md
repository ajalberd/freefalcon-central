# UI overhaul: a menu that fits the window

Branch `ui-adaptive` (off `main`, worktree `..\freefalcon-ui`).

Goal: the ui95 front end (main menu, campaign, planner, TE, dogfight, logbook, setup, tacref,
ACMI) stops being a fixed 1024x768 picture. It fills the window at any size and aspect, gives
the map screens more room, and in VR can use more of the field of view than a 4:3 panel does.
Enlarged pixels are fine (Andrew, 2026-09-29): the aim is *room*, not sharpness.

Out of scope: the in-sim 2D (radio/comms/AWACS popups, exit dialog, HUD overlays). Those are drawn
at the sim's own resolution already and have their own size knobs (`MenuScale`, `VrMenuScale`).

**Verdict: ui95 adapts; no rewrite needed.** Everything below was checked with the test harness,
not inferred, unless marked **unverified**.

---

## Knobs (FFViper.cfg, or the command line for one run)

| cfg | command line | what |
|---|---|---|
| `set g_nUiWidth 2560` / `set g_nUiHeight 1440` | `-uisize 2560x1440` | menu window client size; 0 = 1024x768 (stock); -1 (or `-uisize desktop`) = fill the desktop work area |
| `set g_fUiScale 1.5` | `-uiscale 1.5` | magnification. The layout surface is window / scale, never below 1024x768; 0 = auto (layout exactly 768 tall) |
| `set g_bUiAdapt 0` | | turn the layout adaptation off (stock rects on a bigger surface) |
| `set g_sUiAdaptEdges "foo;bar"` | | extra `.scf` files laid out by EDGES, on top of the built-in map screens (`kEdgesFiles` in `cadapt.cpp`: campaign, recon, TE play and editor) |
| `set g_fReconZoomRate 6` | | recon zoom, % per unit of panner offset per tick; 0 = stock linear |

Stock behaviour is unchanged when none are set.

## The test harness: see it without asking anyone to look

`FFViper.exe -nomovie -window -uitest <script>` runs a script on its own thread once ui95 is up
(`src/ui/src/uitest.cpp`). It waits for windows, clicks controls **through the real WM_ mouse
path** (so the client->surface mapping is exercised), and writes to `<install>\uitest\out\`:

- `result.log`: one `OK`/`FAIL`/`INFO` line per command; the last line is `DONE`, `TIMEOUT` or
  `SCRIPT-ERROR`. A missing last line means the game died (see `FFCrash.log`).
- `shot <name>`: the ui95 surface as drawn (BMP). `wshot <name>`: the window as on screen.
- `layout <name>`: JSON of every shown window and control: rect, client, type, and for pictures
  the image size and cover mode; lint for off-canvas windows and controls outside their window.

`tools\uitest\run.ps1 -Script campaign.txt -UiSize 1600x900 [-UiScale 1.25] -Deploy -Png <dir>`
deploys the build as `<install>\FFViper-ui.exe` (**never touches `FFViper.exe`**, the rail build),
runs one script, prints the log, keeps PNGs/JSON/logs in `<dir>`, and slices this run's
`FFDebug.log` (the `[UIADAPT]` lines) into `<script>.ffdebug.txt`. `tools\uitest\montage.ps1`
tiles a folder of PNGs into one contact sheet. Scripts live in `tools\uitest\*.txt`
(`campaign.txt` goes main -> select -> commit -> priorities -> campaign map; `screen_*.txt` open each
main-menu destination). Names come from `userids.h` (run.ps1 writes them to `uitest\ids.txt`) and
the art's `.id` files; `C_Parser::FindIDStr` is a stub, and IDs are reused across screens, so a
value can print as `A|B|C`.

A campaign run takes ~12 s; a screen run ~6 s. A campaign commit writes no save files.

## How it works now

1. **Sizes** (`ui95/chandler.cpp`): `UI95_GetWindowSize` = UiWidth x UiHeight;
   `UI95_GetSurfaceSize` = that / UiScale, floored at 1024x768. `UI_Startup` sizes the menu window
   to the first and `Front_` to the second. The present already stretched `Front_` over the whole
   back buffer (`D3D12Backend::BlitBitmap565`, one full-screen triangle), so magnification is free.
2. **Mouse**: `ClientToSurface` at the top of `C_Handler::EventHandler` remaps every client-coordinate
   mouse message by the client's *real* size (and the wheel after its `ScreenToClient`). The harness
   uses the inverse. The XR cursor already scaled client -> panel.
3. **Layout** (`ui95/cadapt.cpp`, `UI95_AdaptWindow`, called by `C_Parser::LoadWindowList` straight
   after each `.scf` window is parsed). Two modes:
   - **STAGE** (default). Every piece is placed by where it sits on the 1024x768 *stage*: pieces in
     the 32-px title bar or 40-px bottom bar go to the bars, the left half staying left and the
     right half pinning right; bar strips anchored at one edge stretch halfway in so split bars
     still meet; pictures spanning a whole shell are drawn 1:1 in the middle with black margins
     (`O_Output::COVER_CENTER`); stage-wide bar pictures stretch (`COVER_STRETCH`); everything else
     rides with the stage to the middle. A window becomes the rect holding its pieces, and never
     less than its stock body.
   - **EDGES** (files in `UiAdaptEdges`, default the campaign map screen). Rules per container: a
     window touching both stage edges of an axis fills it, one edge pins to it, neither centres;
     "touching the top/bottom" counts the bars, so `CP_PUA_MAP` (0,32 1024x696) fills between them.
     Backdrops scale to cover (`COVER_SCALE`).
   Each adapted window logs `[UIADAPT]` to FFDebug.log; a control whose setter did not take is
   logged as `wanted ... has ...`.
4. **Cover drawing**: `O_Output` has a `cover_` mode (`ooutput.h`), used by `C_Bitmap::SetCover` and
   `C_Tile::SetCover`; `IMAGE_RSC::BlitCover` (in `cadapt.cpp`) is a nearest-neighbour 8/16-bit blit
   honouring the colour key. `O_Output`'s own scaling (`SetScaleInfo`) is 8-bit only and capped at
   2x (the map uses it), so it could not do backgrounds.

## Facts the harness established (don't re-derive)

- **Some art is aligned to its background.** The campaign-select panel frames are painted into the
  background picture; the windows sit on them. That is why STAGE keeps the stock block intact
  instead of spreading or cover-scaling it.
- **Code adds controls after parse** (the campaign-select squadron list, at positions measured from
  the stock window). A window must never shrink below its stock body, and when it grows to reach a
  bar, its whole-window client's `VX_/VY_` carry the body offset so later controls land right.
  **Absolute ones exist too** (the munitions loadout grid): `C_Window::AddControl`/`AddControlTop`
  call `UI95_AdaptLateControl`, which places them by the same STAGE rules from the window's recorded
  stock and new origins; `C_Window::Cleanup` forgets the window.
- **A client that spans the window on one axis is the window** (munitions' main client is
  0,0 1024x728, the window less its bottom bar). Its controls are placed one by one, and its clip
  reaches every edge it touched; moving it as a unit left its controls without the stage shift.
- **A title-bar item starts at the top** (y <= 6 and ends by 33); text that merely ends above the
  bar's edge is body (munitions' FLIGHT: value was pulled into the bar away from its label).
- **Recon's pivot** is found by `recon_pivot.txt` + `pivot.py`: a zoom pair of the overhead view is
  one image magnified about the pivot. It caught `SceneW()` reporting the back buffer while the
  viewer's RTT was bound.
- **Some controls have no size at parse** (`UI_THEATER_IMAGE` is an empty button until a theater
  is chosen, then 1024x768). A 0x0 box is body, not bar.
- **Some "tiles" are pictures.** The logbook background is a `[TILE]` of one 1024-wide photo;
  stretching it repeated the cockpit. A tile whose image is at least half the stage is a picture.
- **The campaign map just shows more map** in a bigger window (`C_Map` sizes from `DrawRect_`).
- A bigger surface with no adaptation does not crash anything; windows sit top-left.

## Screens checked at 1600x900 (sweep, all DONE, no FAIL, no mismatches)

main, campaign select, priorities (`STRAT_WIN`), campaign map, logbook, tacref, ACMI load, setup,
comms, theater, TE, instant action, dogfight. Campaign map: map fills 1600x828, side panels pinned,
toolbars split. The rest: stock layout centred, bars edge to edge, black margins.

## Recon (checked scaled, 3200x1300 x1.35)

`rec_eye` and `rec_list` are EDGES screens: the 3D pane client (0,32 1024x696) fills between the
bars, and the target list stays a 500-wide panel pinned top-left (EDGES windows narrower than half
the stage pin instead of stretching). Recon draws its 3D straight to the back buffer with the pane
rect in surface pixels, so `ConfineGpuViewportToPane` (`c3dview.cpp`) scales it by
`SceneW / surface width` -- 1 for the off-screen RTT viewers, UiScale for recon. `recon.txt` gets
there unattended: `rclickicon CP_PUA_MAP 20` opens `OBJECTIVE_POP`, and `clickin OBJECTIVE_POP 30 41`
picks its Recon row (popup rows are not controls; the offset is the stock menu layout).

## Done since (2026-09-29, all checked at 3200x1300 x1.35 with the harness)

- **UiScale end to end**: the mouse maps through `ClientToSurface`; every scripted click lands.
- **Desktop fill**: `-uisize desktop` / UiWidth,UiHeight -1 gave a 3424x1361 client on Andrew's
  3440x1440 (work area 3440x1400); `UiScale 0` made the layout 1932x768 (x1.77). Recommended setting
  for an ultrawide: stock screens fill the height, map screens gain ~1.9x the width.
- **Recon zoom** is proportional (`ReconZoomRate`, default 6): 4000 -> 30000 ft in <2 s held; it was
  10 ft per unit per tick (panner offset / 4 = +-5, ~8 ticks/s), over a minute end to end.
- **Recon pivot** at pane centre when scaled (was centre x UiScale; see the facts above).
- **TE**: play map (`te.txt`), editor via New (`te_new.txt`), flight plan, briefing, munitions
  (`te_plan/te_brief/te_munitions.txt`).

## VR menu panel (2026-09-29, built, NOT seen in the headset yet)

- The pre-3D menu panel (`OpenXRBackend::RunMenuFrame`, D3D12) was a flat quad fixed at 1.3 m tall,
  2.1 m away, in `appSpace`: ~35 deg tall, ~45 deg wide at 4:3.
- Now sized by angle: `VrUiHeightDeg` (45) tall at `VrUiRadius` (2.0 m), width from the layout's
  aspect. On `XR_KHR_composition_layer_cylinder` (Quest 3 offers it; enabled with the core set,
  `p->cylinderEnabled`) it is a cylinder layer centred on the start pose, else the same-sized quad.
  `VrUiCylinder 0` forces the quad.
- In VR the layout is `VrUiWidth x VrUiHeight` (1920x768) at scale 1, whatever the desktop knobs:
  ~120 deg round at 45 deg tall. Map screens get the extra width; the rest stay centred.
- First frame logs `OpenXR: menu panel cylinder|quad WxH, H m at R m (arc x height deg)` to
  `openxr_diag.txt`. Unverified: that the cylinder faces the viewer (spec: centred on -Z from its
  pose) and the cursor crosshair still tracks. The Vulkan menu path is unchanged (quad).

## Open / next

- **Sharp scaling: done** (`UiFilter`, default 1; `-uifilter`). Was: nearest at a non-integer scale
  (1.77) doubles some pixels and not others. Plan: the "sharp bilinear" pixel-art filter in the
  menu blit (crisp texels, one blended pixel at each seam), as a knob.
- **DPI awareness**: still none. On a scaled desktop Windows bitmap-stretches the window again.
- **Menu <-> sim window size**: the sim still enters its own DispWidth x DispHeight.
- **Screens not reached yet**: intel, ATO, OOB, TE mission builder and victory conditions, the
  campaign planner map (flight plan from the campaign), popups (`gPopupMgr`), ACMI options popup
  (fixed x,y knobs).
- **Small leftovers**: the TE editor's team panel keeps its stock height (black below it);
  munitions' "FLIGHT:" pair sits in the title bar rather than just under it; a few stray pixels in
  the tacref/TE margins.
- **Main-menu photo** could cover (`COVER_SCALE`) rather than letterbox; it has no aligned art
  apart from the logo.
- **VR**: the menu quad already follows the surface aspect (width = height x aspect, 1.82 m tall
  at 1.8 m): 4:3 is ~68 deg wide, 16:9 ~84 deg. Next: a VR-only surface size and an
  `XrCompositionLayerCylinderKHR` (Quest supports `XR_KHR_composition_layer_cylinder`) at
  ~110-130 deg, the cursor mapped by ray -> u,v; keep the quad as fallback.
- Dead weight to delete: the `g_bHiResUI false` / `art1024` branches (`NIGHTFALCON_UI 1`).
- Fixed on the way: the cfg parser's string values were not terminated (`f4config.cpp`), so a value
  shorter than the default kept the default's tail.

## The hard limits (unchanged)

- ui95 draws bitmaps and `.bft` fonts at native size; magnification is only as sharp as the art.
- RGB565 end to end.
- Replacing ui95 would mean rewriting ~100k lines in `src/ui/` on top of ui95's 35k. Not needed.
