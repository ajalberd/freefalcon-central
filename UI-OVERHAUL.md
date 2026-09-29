# UI overhaul: a menu that fits the window

Branch `ui-adaptive` (off `main`, worktree `..\freefalcon-ui`). Reconnaissance, not a plan.
Nothing here has been run yet: every "today" statement is from reading the code, and the
first job is to confirm the ones marked **check** in game.

Goal: the ui95 front end (main menu, campaign, planner, TE, dogfight, logbook, setup, tacref,
ACMI) stops being a fixed 1024x768 picture. It fills the window at any size and aspect, and in
VR it can use more of the field of view than a 4:3 panel does.

Out of scope: the in-sim 2D (radio/comms/AWACS popups, exit dialog, HUD overlays). Those are drawn
at the sim's own resolution already and have their own size knobs (`MenuScale`, `VrMenuScale`).

---

## How the menu reaches the screen today

1. **One CPU surface, fixed size.** `C_Handler::Setup` (`ui95/chandler.cpp:119`) makes `Front_`,
   an RGB565 `ImageBuffer`, 800x600, or 1024x768 with `g_bHiResUI` (default true,
   `f4config.cpp:1070`). Every ui95 window paints straight into it. There are no per-window
   surfaces in the GPU path; `C_Window::GetSurface` + `Compose` is the dead DirectDraw path.
2. **The window is resized to match.** `UI_Startup` enters `FalconDisplayConfiguration::UILarge`
   (`ui_main.cpp:1580`), which is 1024x768 (`falclib/dispcfg.cpp:43`); `EnterMode` sets the
   window's client rect to it. The D3D12 init path forces `WS_OVERLAPPEDWINDOW`
   (`ddstuff/devmgr.cpp:1068`). The sim later enters `Sim` at `DispWidth x DispHeight`, so the
   window changes size on every menu/sim transition.
3. **Present = stretch.** `C_Handler::CopyToPrimary` → `ImageBuffer::PresentGpu`
   (`ddstuff/imagebuf.cpp:797`) → `D3D12Backend::BlitBitmap565` (`d3d12backend.cpp:2533`):
   convert 565 → RGBA8 on the CPU, upload, draw **one full-screen triangle** into whatever the
   back buffer is. No aspect handling, no letterbox. Vulkan's `BlitBitmap565` is the peer.
4. **Mouse = raw client pixels.** `C_Handler::EventHandler` (`chandler.cpp:2195`) uses
   `lParam` client coordinates as surface coordinates, 1:1, with no scale.
   So if the window is ever not exactly 1024x768 (maximised, dragged, DPI-scaled), the picture
   stretches but the hit-testing does not. **Check** what maximising the menu window does today.
5. **No DPI awareness** anywhere (no manifest entry, no `SetProcessDpiAwareness`). On a 150 %
   desktop Windows bitmap-scales the whole window. **Check.**
6. **VR.** Both D3D12 branches of `PresentGpu` also call `OpenXR_CacheMenuSurface`;
   `OpenXRBackend::RunMenuFrame` (`openxrbackend.cpp:2849`) converts to RGBA, draws the cursor
   (client → panel mapping already *scales*, `:2903`) and submits a **flat
   `XrCompositionLayerQuad`** at `g_fVrMenuDist` 1.8 m, `g_fVrMenuHeight` 1.82 m tall, width =
   height x aspect. `EnsureUiSwapchain(w, h)` already follows the surface size. So a bigger
   surface flows into VR unchanged. At 4:3 that quad is about 68 x 54 degrees; at 16:9 it would
   be about 84 degrees wide on a flat plane, which is where a cylinder layer starts to pay.
   No cylinder or equirect layer is used anywhere yet.

## Where the layout lives: data, not code

- `.scf` window scripts in `C:\FreeFalcon6\art\**` (not in the repo, like all of `art\`).
  **141 windows**. `[SETUP] <id> <type> w h`, `[XY] x y`, `[RANGES] minx miny maxx maxy w h`,
  then controls with absolute `[XY]`/`[XYWH]` in their client's coordinates.
  - 125 of the 141 `[RANGES]` are exactly `0 0 1024 768` (the drag clamp).
  - **31 windows are full-screen 1024x768**: the screen shells (`main/main_win.scf`,
    `campaign/cp_main.scf`, `planner/briefing.scf`, `setup/setup.scf`, `tacref/tacref.scf`,
    `taceng/tac_play/tacpmain.scf`, ...). They share one shape: a 1024x768 background bitmap
    (`UI_MAIN_BG`), a top bar tile `BAR_O` at 0,0 1024x32 and a bottom bar tile `BAR_M` at
    0,728 1024x40.
  - The big content windows sit in that frame. The campaign map is `CP_PUA_MAP`, 1024x696 at
    0,32 (`campaign/cp_mspua.scf`), exactly the gap between the bars. Side panels are pinned by
    absolute x (`CP_SUA` 249x310 at 774,32).
- `NIGHTFALCON_UI 1` (`ui95/cparser.cpp:809`) means the `art1024` vs `art` split is dead: the
  1024 art *is* `art\`. There is no 800x600 art set to keep working, and the `g_bHiResUI false`
  branches are dead weight. (Worth deleting in this branch.)
- Tag counts across all `.scf`: ~1,500 `[XYWH]`, ~1,200 `[XY]`, 720 buttons, 160 listboxes, 154
  dropdowns, 108 `[TILE]`s. Re-authoring by hand is out; any change has to be a rule applied at
  load time, or a script over the files.

## What ui95 already supports (the good news)

- **Surface size is one number.** Nothing else in ui95 allocates against 1024x768.
  `FrontRect_` is read back from `Front_` (`chandler.cpp:272`).
- **Windows can already be resized.** `C_Window` has `x_ y_ w_ h_`, `Min/MaxX_Y_`, `Min/MaxW_H_`,
  `SetW/SetH` clamped to them, `ResizeSurface` (`ui95/cwindow.cpp:211`), `SetRanges` (`:347`).
  Controls are positioned relative to their client area (see *Facts → ui95* in WIP-NOTES), so a
  client that moves carries its controls.
- **The campaign map sizes itself from its window.** `C_Map::CalculateDrawingParams`
  (`ui/src/campaign/cmap.cpp:345`) takes its aspect and extent from `DrawRect_`, the client
  rect. A taller or wider map window should just show more map. **Check.**
- **Only a handful of code sites know the resolution** (the rest is data):
  | Site | What |
  |---|---|
  | `chandler.cpp:119` | `Front_` size |
  | `ui_main.cpp:1580`, `dispcfg.cpp:43` | UI display mode = window size |
  | `ui_main.cpp:1615`, `chandler.cpp:1244`, `campaign/general.cpp:2803` + TGA headers `:2762` | Screenshot buffer and TGA writer, sized 1024x768. **Overflows the moment the surface grows.** Fix first. |
  | `campaign/cpselect.cpp:323-539` | Picks the big occupation map for the 1024 layout |
  | `acmi/src/acmiui.cpp:723` | ACMI options popup at a fixed x,y (`ACMIOptionsPopupHiResX/Y`) |
  | `cwindow.cpp:168` | Default `MaxX_ = 800` before `[RANGES]` overrides it |
- `C_3dViewer` (loadout, tacref, recon) renders a "screen-sized" RTT and stamps it into the 565
  surface. It will need the new surface size, not 1024x768. Popups (`gPopupMgr`) clamp to
  something; **check** what.

## The hard limits

- **Pixels are 1:1.** ui95 blits 16-bit bitmaps and `.bft` bitmap fonts at their native size.
  It cannot draw *scaled*, so "bigger" comes in two kinds:
  - more logical pixels (a larger surface, more space, same size text), or
  - the same logical pixels magnified at present time (bigger, but only as sharp as the art).
  Real sharpness at 1440p/4K needs higher-resolution art and fonts, which is an art project, not
  a code one. Same ceiling as the cockpit fonts (`COCKPIT-OVERHAUL.md`).
- **RGB565 end to end** (surface, `.rsc` art, the XR cache). Banding is part of the look; going
  to 32-bit touches every ui95 blitter (`O_Output`) and is its own project.
- **Replacing ui95** (ImGui, RmlUi, an HTML layer) means rewriting about 100k lines under
  `src/ui/` that call `C_Button`/`C_ListBox`/... directly, on top of ui95's 35k. That's the
  "complete overhaul" in the literal sense. It's recorded here so nobody re-scopes it: it is
  years of work, not a branch.

---

## Routes, cheapest first

Each step is useful on its own, and each one is a prerequisite for the next.

### 1. Fit: the 1024x768 UI fills the window, undistorted

- The menu window stops snapping to 1024x768. It keeps the user's window/fullscreen size across
  menu and sim, and can be resized and maximised. (It's `EnterMode(UILarge)` that snaps it.)
- `BlitBitmap565` draws an aspect-correct rect instead of the full-screen triangle
  (pillarbox on 16:9, letterbox on tall windows), with a filter choice: bilinear, or sharp
  (integer-ish nearest plus a bilinear edge).
- One inverse mapping, window client → surface, at the top of `EventHandler` (every mouse
  message). The XR cursor already has the forward mapping.
- Per-monitor DPI awareness, so Windows stops blurring it a second time.
- Fix the screenshot buffer and TGA sizes to read `Front_`'s size.

This is small: about five files and no data changes. It does not give more *space*, only no more
postage stamp.

### 2. Stage: a bigger logical surface, stock layout centred

- `Front_` = window size / `UiScale` (new knob; auto picks the scale that makes the height 768).
  At 16:9 that's about 1365x768, at 21:9 about 1792x768.
- On load, every `.scf` window is offset by the stage origin, `((W-1024)/2, (H-768)/2)`, and
  `[RANGES]` that say `0 0 1024 768` are widened to the surface. Popups can then be dragged into
  the side space.
- The full-screen shells' background and bars need something in the side strips: stretch
  `BAR_O`/`BAR_M` (they are `[TILE]`s, so tiling across is free), and extend or mirror-pad
  `UI_MAIN_BG`.

### 3. Adapt: anchors, so the big content windows grow

- New `.scf` tags (the parser is ours, `ui95/cparser.cpp`), e.g. `[ANCHOR] L T R B` on a window
  or client area: edges pinned to the surface edges or to the stage. Plus a load-time default rule
  for the common shape: *a window whose rect is 0,32 → 1024,728 fills between the bars*. That one
  rule covers the campaign map, and probably the TE and planner maps too.
- Side panels pin to the right edge (`CP_SUA` at x 774 becomes "right - 250").
- The anchors can be written by a script over `art\` (like the other `tools/` rewriters), with
  backups, and shipped through the installer manifest like any other `art\` change.
- The screens where more space pays most: **campaign map**, **mission planner / briefing**,
  **TE editor map**, **logbook/ACMI lists**, **tacref**. The rest can stay centred.

### 4. VR: more room than a flat 4:3 quad

- The UI surface in VR doesn't have to match the desktop window. Give VR its own canvas size
  (e.g. 2048x1024 logical), since `EnsureUiSwapchain` already follows the surface.
- Submit it as an `XrCompositionLayerCylinderKHR` (extension `XR_KHR_composition_layer_cylinder`;
  Quest's runtime supports it) wrapped around the head at about 1.8 m, roughly 110-130 degrees.
  Keep the quad as a fallback when the extension is missing.
- The cursor mapping moves from "client → panel" to "ray → cylinder u,v", which also removes the
  mouse's dependence on the desktop window size.
- Pixel density: a 1024x768 panel over ~68 degrees is about 15 px/deg, below Quest 3's ~25. So
  the VR canvas wants a magnify-at-present factor as well as more logical pixels. That is where
  route 1's filter matters.

### 5. Art

Higher-resolution `UI_MAIN_BG`, bars and fonts; a 32-bit surface. Only after 1-4 show what's
actually worth redrawing.

---

## First things to check in game (before writing any of it)

1. Maximise the menu window: does the picture stretch, and does clicking miss? (Confirms
   points 3-4 above.)
2. A 150 % DPI desktop: is the menu blurry? (Point 5.)
3. Set the surface to 1365x768 with nothing else changed: which screens break, and how? That's
   a one-line experiment in `chandler.cpp:119`, after the screenshot buffer is fixed.
4. Give `CP_PUA_MAP` a larger `[SETUP]` w/h and matching `[RANGES]` by hand: does the map simply
   show more?
