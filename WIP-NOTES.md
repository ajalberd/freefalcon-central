# Work in progress

Rolling handoff note. Read this first. Keep it short — when something is settled, cut it
down to the one fact a future session would otherwise re-derive, and let the code
comments and commit messages carry the rest.

Companion docs:

- `CAMPAIGN-SUPPLY-ENGINE.md` — how supply, production and power actually work.
- `tools/terrain/tilesurvey.py` — reads a theater's terrain and tile data offline, no
  build and no game needed. Re-run it instead of re-deriving any terrain number.
- `tools/models/objsurvey.py` — same idea for the 3D object database. Reads `KoreaObj.DXH`
  and `.DXL`: parent records, LOD names, and a model's whole node stream including every
  switch number, its branches and the geometry hanging off each one.

---

## THE LIVE TASK: recon crashes when map-detail logging is on

**Symptom.** In a campaign, opening **Recon** crashes to the exception handler. It
happens with `set g_bLogCampMapDetail 1` and does *not* happen with it at `0`. Confirmed
by the user in the field, one trial each way.

**This matters because the toggle should be inert.** It gates one `_snprintf` and one
`FFDebugLog` call in `CampMapDetailBuild` (`cmapdetail.cpp`) and nothing else. It does
**not** disable the detail layer — that is `CampMapDetail`, a separate knob.

### Ruled out — do not re-chase these

| Theory | Why it is dead |
|---|---|
| `g_bLogCampMapDetail` collides with the `CampMapDetail` table entry | `ConfigOption::CheckID` is `stricmp(&str[3], Name) == 0` — an exact match after the `g_b` prefix, not a substring match. The knob sets only the log flag. |
| `FFDebugLog`'s handle is duplicated across static libraries | `cmapdetail.cpp` is a fourth TU including `fflog.h`, and it lives in a different lib from the other three. But `Falcon4___x64_Release/RedViper.map` has exactly one `?s_f@?1??FFDebugLogFile@...` and one `$TSS0` guard. Correctly folded. |
| Opening the log file is itself the problem | `g_bLogCampProducers` is already `1` in the user's config and goes through the same `FFDebugLog`, so `FFDebug.log` is open before the map ever logs. Whatever the toggle does, it is not opening the file. |

### Fixed, but unproven as the cause

The log buffer was `char ln[200]` written by an unbounded `sprintf`, and the format's
worst case is **195 characters**. Five bytes of margin. A smash there would not fault
there — it would corrupt the frame and crash somewhere unrelated, only ever with logging
on, which is exactly the observed shape. Now `_snprintf` into `char[512]` with explicit
termination. **If recon no longer crashes with logging on, this was it.** Do not assume
that; check.

For contrast, the `[CENSUS]` logger a few hundred lines up in `cmap.cpp` uses the same
`char[200]` + `sprintf` pattern but tops out around 135 characters, so it is not a
candidate — worth knowing before "fixing" it too.

### The old crash report was unreadable, and now is not

`GetRegisterString` (`crashhandler.cpp`) used `wsprintf` with `%016llX`. That is the
USER32 `wsprintf`, which has never supported the `ll` modifier — every register printed a
literal `lX`, and the argument list then slid several slots, so `FLG` and the segment
registers were showing fragments of other registers. Fixed: two 32-bit halves, still
`wsprintf` (no CRT, no extra stack, which is what a handler on a possibly-blown stack
wants).

So **every register in the report below is meaningless**. Only these two lines were real:

```
Read from location 00000fde caused an access violation.
Code: f3 0f 59 42 14 f3 0f 2c c8 66 89 4a 10 f3 0f 10
```

which decodes to

```
mulss      xmm0, dword ptr [rdx+0x14]
cvttss2si  ecx, xmm0
mov        word ptr [rdx+0x10], cx
```

i.e. `obj->shortAt0x10 = (short)(x * obj->floatAt0x14)` with `rdx` ≈ `0xFCA` — a small
integer used as a pointer. Nothing in `cmapdetail.cpp` has that shape; it contains no
float-to-short stores. Grep for a class with a `short`/`WORD` at +0x10 and a `float` at
+0x14 if a fresh dump points the same way.

### How recon can reach the new code at all

`C_3dViewer::StampRttIntoMenu` (`src/ui/src/general/c3dview.cpp`) ends with
`gMainHandler->RefreshAll(&viewport)`, which marks the rect dirty on **every visible
window**. So the campaign map control redraws during recon, and that goes through
`C_ScaleBitmap::Draw` → `O_Output::Blend4BitDetail`.

Prior art: commit `aa305b57` ("QC round: recon crash") fixed a different recon crash —
`C_3dViewer::Cleanup` freed `m_pRTT` between `rend3d_` and `rendOTW_` once recon moved
onto the RTT. Read it before assuming this one is new.

### Next steps, in order

1. **Get a real dump.** Deploy the current build (see *Build*), set
   `g_bLogCampMapDetail 1`, open recon. If it crashes, the report now names a real RIP
   and real registers.
2. **If it no longer crashes**, the 200-byte buffer was the cause. Say so and close it.
3. **If it still crashes**, separate logging from the detail layer: set
   `g_bCampMapDetail 0` *and* `g_bLogCampMapDetail 1`. Detail off means
   `CampMapDetailBuild` returns before the log call, so a crash there is not the map at
   all.
4. Only then start reading. `FFDebug.log` next to the exe holds the `[MAPDETAIL]` lines;
   the last one before the crash says which rebuild was in flight.

---

## Test these first

Everything here builds (`FFViper.exe` links clean, no unresolved externals). Items 1–2b
are committed on `main`; **item 3 and everything above is uncommitted**, in the working
tree, pending the user's verification.

### 1. Supply interdiction — `SupplyInterdiction`

A damaged bridge or road costs the supply run crossing it. Default 100, `0` restores
stock exactly. Bomb a bridge on a supply route and watch the Logistics → Supply overlay
thin out downstream. Detail in `CAMPAIGN-SUPPLY-ENGINE.md`, "Roads and bridges".

### 2. Build package / Add Package — right-click a target

Both were made to work and confirmed opening. Untested: the flight tree with several
flights, per-flight targets, the Takeoff/TOT locks, window-group interaction with the
map, and `REQF_TE_MISSION` alongside a running ATO.

The three findings worth not re-deriving (the rest is in the commit messages): the
candidate filter must use `FalconLocalSession->GetTeam()`, never `gSelectedTeam` — that
is the TE editor's variable and it holds a *country*; `PACKAGE_WIN` was always loaded by
`CMN_SCF.LST`, so do not "fix" it by loading it again; and the campaign must open with
the **takeoff** lock closed, not the TOT lock, or `tactical_make_flight` pins every
flight to an exact second over the target and the planner rightly refuses.

### 3. Campaign map detail on zoom — `CampMapDetail`

Zoomed in, the visible patch is drawn from the **ground tiles the sim flies over** —
12.8 ft/pixel against the base map's 820. Default on; needs `CampMapFromTerrain`, which
must be `1`. Confirmed drawing in the field.

1. **It draws.** Zoom right in: fields, forest edges, roads and rivers should resolve.
   `LogCampMapDetail 1` prints one `[MAPDETAIL]` line per rebuild — but see the live task
   above before switching it on.
2. **Registration.** Icons, bullseye and FLOT should sit where they did. Nothing in the
   coordinate system was touched, so a shift means the cell grid is misaligned.
3. **Orientation.** Roads should run continuously across tile-cell boundaries. The
   row-axis inversion for this was measured, not guessed — see *Facts* below.
4. **Overlays still tint it.** Switch on a Logistics layer and the FLOT while zoomed in.
5. **Cost while panning.** The window recomposites whenever the source rect moves. Drag
   at full zoom and watch for stutter. Not pre-optimised on purpose; if it is too slow,
   the fixes are a coarser zoom tier or deferring the rebuild to mouse-up.
6. **Sea colour.** Water posts carry colour index 0 and `ColorTable[0]` is pure white, so
   the base map used to paint the ocean a flat white field. Index-0 posts now take their
   tile's average colour instead. Base map and detail layer should now agree across the
   zoom threshold.

### 4. JFS switch and the indicator lights — 3D pit

The JFS switch was never missing from the model; it was never being drawn. Climb into the
3D pit and look at the left console, forward of the throttle.

1. **The lever is there with the jet cold.** Before this it only appeared once the JFS was
   already running, which is precisely when you are no longer looking for it.
2. **It throws.** Click it (or the `SimJfsStart` key). The lever should snap from the aft
   OFF cant to upright START, and back when the JFS spins down.
3. **The green RUN light beside it lights with it**, and only with it. It used to be wired
   to the engine Overheat lamp, so it lit on an overheat and never on a JFS run.
4. **The other lamps on that panel.** EPU RUN, EPU HYDRAZINE/AIR, ECM PWR/FAIL and the
   eight ELEC panel lamps had the same defect and now read their own bits. Worth a glance
   during a ramp start — that is where all of them are visible at once.
5. Main power off still freezes these lamps rather than clearing them, because the whole
   block is skipped when `mainPower == MainPowerOff`. Pre-existing; not touched.

### 5. Avionics power levers — the lever now follows the lever

Sixteen levers (SMS, FCC, MFD, UFC, GPS, DL, MAP, L/R HPT, HUD, FCR, IFF, and the four EWS
switches) drew their position from `HasPower()`, which answers *is this box's bus live*,
not *where did the pilot put the switch*. `SMSPower` and friends only appear in
`systemStates[PowerNonEssentialBus]`, so on a cold jet `HasPower` is 0 whatever the lever
is doing: the callback set the mask to 2 and `VCock_Exec` pulled it back to 1 the next
frame. The switch clicked and snapped straight back. They now read `PowerSwitchOn`, the
raw bit the callbacks write. The box staying dark until its bus comes up is unchanged.

**If these still do nothing, check the Avionics setting.** `SimSMSOn`/`SimSMSOff` and their
siblings open with `if (not g_bRealisticAvionics) return;` — on "Enhanced" they are inert
before they touch anything, and the 3D button dispatch plays the click either way.

### 6a. JFS: one start per engine run, and the light needs main power

Measured in the field, not inferred:

```
[JFS] start  throttle=0.000 accum=0.0 fuel=7162.0 jfsFlag=1 engineStopped=1 mainPower=0
```

`jfsFlag=1` means the start **succeeded**. `accum=0.0` is the state *after* the start, not a
refusal: `JfsEngineStart` passes the `>= 90` gate and then sets `jfsaccumulator = 0.0f`
("all used up").

**The accumulator only recharges above `auxaeroData->jfsMinRechargeRpm`** (`engine.cpp`), so
with the engine stopped it stays at 0 forever — you get **one JFS start per engine run**.
A second press then refuses silently and looks exactly like a dead switch. `jfsRechargeTime`
defaults to 60 s.

**`mainPower=0` is why the green run light stayed dark.** The whole indicator-light block in
`VCock_Exec` is skipped when `mainPower == MainPowerOff`, so the lamp is never written. Put
MAIN PWR to BATT first, as a real ramp start does.

The switch has only two branches in `3DPIT_F16CJ_L1` — there is no START1/START2 to be stuck
in, so "the first press logs `start`" is correct from OFF. **The branches are reversed from
what their numbering suggests: branch 0 is the lever thrown to START, branch 1 is OFF.**
Confirmed in the cockpit, not from the geometry — the tip cluster that reads as "upright" in
the vertex dump is the OFF pose. So the mask is 1 while the JFS runs and 2 while it does not
(`2 - IsSet(JfsStart)`), and `SimJfsStart`'s immediate writes match.

### 6b. Starting the engine after the JFS — the detent is not optional

`JfsStart` sets `rpmCmd = 0.25f` (`engine.cpp`), so the JFS spins the engine to **25%**, and
`JFSSpinTime` gives it 240 s before it gives up. The bar for lighting is **rpm >= 0.20**.

Nothing clears `EngineStopped` on its own. Two mutually exclusive paths:

- `g_bUseAnalogIdleCutoff 0` (the default, and what is set here): only
  `SimThrottleIdleDetent` clears it, and only when the throttle is already above 0.1 **and**
  rpm >= 0.20. Press it below 0.1 and it *sets* `EngineStopped` instead. Advancing the
  throttle alone does nothing at all — there is no automatic light.
- `g_bUseAnalogIdleCutoff 1`: `SimThrottleIdleDetent` early-returns and becomes inert; the
  engine lights by itself once rpm >= 0.20 and `IO.IsAxisCutOff(AXIS_THROTTLE)` is false.
  The better setting for a HOTAS throttle with a real idle detent.

Ramp start: MAIN PWR → BATT, JFS, wait for ~25% rpm, throttle up, then the detent.
`SimThrottleIdleDetent` has a 3D hotspot in `3dbuttons.dat` at the throttle quadrant.

### 6. The JFS refuses silently — `LogJfs`

`SimJfsStart` needs the throttle below 0.1, and `JfsEngineStart` needs the accumulator at
90%+ and fuel aboard; above 400 KIAS or 20,000 ft it also refuses at random. None of that
is reported, and the click sound is played by the button dispatch before the callback is
even reached — so a refused start and a dead switch look and sound identical.

`set g_bLogJfs 1` in `FFViper.cfg` prints one `[JFS]` line per press to `FFDebug.log`
naming the decision (`start`, `stop`, `throttle-not-idle`) and the state behind it. The
line after a press that did nothing says which guard bit.

### 7. VR screenshots are black — the mirror they read was never written (FIXED)

Fixed by capturing the EYE instead of building the mirror. See "VR screenshots" below for
what shipped; the diagnosis that follows is kept because it explains the shape of the fix.

Screenshots land in `<install>/pictures` as `YYYY-MM-DD_HHMMSS.bmp`. Both keys reach
`OTWDriverClass::TakeScreenShot`; "pretty" only suppresses 2D text and labels for one frame
first. Menu captures work. **Every in-VR capture is pure black**, and the reason is that
`g_bXrMirror` is declared in `f4config.cpp`, defaults to true, is registered in the config
table — and **has no reader anywhere in the source**. There is no eye-to-swapchain blit.

`D3D12_RequestScreenCapture` defers to Present and reads the swap-chain back buffer on the
stated assumption that "with XrMirror on it holds the eye the compositor was handed"
(`d3d12backend.cpp`). It does not: in a VR session the eyes go straight to the compositor's
own swapchains and the desktop back buffer only ever holds `BeginFrame(0xFF000000)`.
Toggling `XrMirror` changes nothing either way.

On **Vulkan** it is worse: there is no capture path at all, so `TakeScreenShot` falls
through to `OTWImage->BackBufferToRAW`, the CPU-side RGB565 surface the 3D scene never
touches. Fixing VR capture properly means writing the mirror blit (one eye, or both
side-by-side as the `f4config` comment describes) in the OpenXR backend.

### 8. Controllers "Assign" — the dialog was never loaded (FIXED, verified)

Verified in the field: `open:` → `autodetect: staged button 36 on device 5 (held 6 polls)`
→ `wrote buttonId=420; readback MATCHES`, four bindings in a row.

**Root cause, from the trace.** `DeviceCellCB` fired correctly on every click
(`deviceCell: click id=513000 func=set dev=5`) and no `open:` line ever followed, which puts
the failure inside `OpenButtonAssignWindow` before its first statement completes:

```cpp
C_Window *win = gMainHandler->FindWindow(SETUP_BTNASSIGN_WIN);
if (not win) return;                    // silent
```

`art/setup/buttonassign.scf` defines `SETUP_BTNASSIGN_WIN` and already shipped in
`ffviperpatch.wxs`, but the only thing that loads a setup window is
`LoadWindowList("st_scf.lst")` in `ui_setup.cpp`, and **`st_scf.lst` named every setup .scf
except that one**. The window never existed, so the click ran the handler and did nothing.

Fixed by adding `art\setup\buttonassign.scf` to `st_scf.lst`, and by shipping that list from
`src/installer/res/art/st_scf.lst` (declared in the .wxs) — without the second half it is
fixed only on one machine. Exactly the shape of the earlier `cp_pkg_scf.lst` fix.

**The lesson worth keeping:** a .scf in `res/art/` and a `<File>` line in the .wxs are not
enough. A window is inert until some `*_scf.lst` names it. `FindWindow` returning NULL was
silent at every call site; `OpenButtonAssignWindow` now logs that abort by name.

**Control-id map** (the .scf names do not read like their roles): OK = `BTNASSIGN_ASSIGN`,
Cancel = `BTNASSIGN_OPEN`, Redetect = `BTNASSIGN_DETECT`. The dialog wires its own callbacks
in `SetupButtonAssignWindow`, so it does not depend on `HookupSetupControls` — `ui_setup.cpp`
mentions it nowhere, and that is fine.

**Two controls the .scf does not define**, both null-guarded so they cost a feature rather
than crash: `BTNASSIGN_CLEAR` (the staged per-device Clear is unreachable from the dialog)
and `BTNASSIGN_DEVICE_LIST` (no device dropdown, so the dialog stays locked to the device of
the cell that opened it — which matches the intended #53 device-locked behaviour anyway).
`BTNASSIGN_FUNC_LIST`/`BTNASSIGN_SEARCH` are absent on purpose; the .scf header says the
function list and search live in the main window. The `open:` line now reports which of
these were found.

### What the trace also ruled out

Assign works for keyboard cells and does nothing for device cells; no `[ASSIGN]` line was
produced at all, so `OpenButtonAssignWindow` is never reached. Ruled out by reading:
`DEVCELL_BASE` (510000) is nowhere near the keyboard-cell range the `g_ctxIsKeyboard` test
uses (`KEYCODES`=100000..110000), so device cells are not being misclassified;
`gDIDevButtons` and `gDIDevNames` are written and read with the same full-device-index
convention; the device dropdown's `GetTextID() - 1` matches how `BuildControllerList`
numbers it.

The device-cell reuse branch in `UpdateDeviceCells` was also made idempotent (it set only
colour, text and user data, while the keyboard equivalent `UpdateKeyMapButton` re-applies
`SetMenu`, `SetCallback`, `C_BIT_ENABLED` and the hotspot on every refresh). **That was not
the bug** — the trace showed reused cells still delivering clicks — but it removes a real
asymmetry, so it stays.

`set g_bLogAssign 1` now traces the whole chain, not just the dialog: how many device
columns were built and with what button counts, whether each row-0 cell was created or
reused, every context-menu open with its cell classification, every device-cell click, and
the OK commit with a read-back.

### 9. VR screenshots — capture the eye, not the desktop

Both keys (normal and "pretty") reach `OTWDriverClass::TakeScreenShot` and land in
`<install>/pictures` as `YYYY-MM-DD_HHMMSS.bmp`. The desktop back buffer is useless in VR,
so `EndEyeFrame` now claims a pending request and reads the left eye image directly, at full
eye resolution, after the eye's list has executed and fenced and before `ReleaseEyes` hands
it back. The desktop path stands aside for 8 Presents and takes over if no eye claims it,
which keeps the key working in the head-locked menu. Nothing happens unless a shot is pending.

Two things this cost a round trip each, worth not rediscovering:

- **The eye image is TYPELESS.** The runtime hands out typeless colour images so the app can
  pick an sRGB or UNORM view — `openxrbackend.cpp` demotes `_SRGB` to `_UNORM` when building
  the eye RTV for exactly that reason. `GetDesc()` then reports `R8G8B8A8_TYPELESS`, which is
  not a legal placed-footprint format. Copy with the concrete UNORM member of the same family.
  A capture that declines here writes **nothing at all**, which looks identical to a dead key.
- **"Pretty" needs the request queued from inside `DisplayFrontText`.** Its EXECUTE frame is
  the one with the text and labels suppressed, but the usual queue point is the end of the
  whole draw — after both eyes are submitted. The request would then be claimed by the NEXT
  frame's eye, which is the CLEANUP frame with the text back on. The flat path never had this
  problem because it reads the back buffer at Present, still within the clean frame.

Every screenshot now writes one `[SHOT]` line to `FFDebug.log`, unconditionally (it is a
keypress). `g_bXrMirror` gates the eye path and finally means something; 0 restores the old
desktop-back-buffer behaviour. The QUAD copy path (`EndViCopyGroup`) is not covered.

### 10. Campaign teardown heap corruption — `STATUS_HEAP_CORRUPTION`

**The crash handler cannot catch this.** Heap corruption raises a fast-fail that bypasses SEH,
so `FFCrash.log` stays empty and stale. Use the Windows event log and the WER dumps instead:

```
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=(Get-Date).AddDays(-14)} |
  Where-Object { $_.Message -match 'FFViper' -and $_.Id -eq 1000 }
```

Dumps land in `%LOCALAPPDATA%\CrashDumps`, one per crash, and `cdb.exe` ships with the
Windows SDK at `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64`. The exe's symbols are
`RedViper.pdb` (not FFViper.pdb), now deployed next to the exe so dumps symbolise unaided:

```
cdb -z <dump> -y "<install>;<repo>\Falcon4___x64_Release" -c ".lines -e;.reload /f;.ecxr;kb 40;q"
```

That turned "crashes 4x on a campaign ramp start" into a named stack in one pass:

```
_free_base / O_Output::Cleanup (ooutput.cpp delete Cols_) / C_ScaleBitmap::Cleanup /
C_Map::Cleanup / CleanupCampaignUI / HandleCampaignThread
```

`!analyze` verdict: `HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE`.

**It is not new.** The identical signature goes back to 2026-09-14, 22 occurrences across many
builds — it predates the 3D-pit and controller work entirely. What changed was hitting a path
that trips it reliably.

**The cause is a write overrun, and the free side was a red herring.** `O_Output::SetScaleImage`
assigns the new image and then allocates only when the buffers are still NULL:

```c
Image_ = newimage;
if (Rows_ == NULL) Rows_ = new long[Image_->Header->h * 2];
if (Cols_ == NULL) Cols_ = new long[Image_->Header->w * 2];
```

They are sized *from the image*, so "allocate once" is right exactly once. Give the same
`O_Output` a second, larger image and they keep the first one's size while `SetScaleInfo`
fills them to the new image's extent, writing off the end of both blocks.

The campaign map does exactly that: `cmap.cpp` picks between `Map_->SetImage(s_terrainMap)`
and `Map_->SetImage(MapID)` at runtime, and the terrain-built map is the larger. That is why
this arrived with the terrain map work and why it is campaign-specific. The smashed neighbour
dies later and elsewhere, which is why every dump blamed a "double free" in
`CleanupCampaignUI` nowhere near the write.

Fixed by reallocating both buffers for whatever image `SetScaleImage` is handed, and bounding
both fill loops by the real capacity. That replaces an `F4IsBadWritePtr` guard tagged
"JB 010304 CTD" which never worked: it only refuses a write to an **unmapped** page, so
running a few hundred longs into the adjacent allocation passed it silently — and it asked
about `sizeof(short)` on an array of `long`, so it under-checked by half.

**A correction worth keeping.** The first attempt changed the four scalar `delete`s in
`O_Output::Cleanup` to `delete[]`. That is correct C++ and is still in, but it was **not** the
fix — the crash survived it, and one clean load afterwards was luck, not evidence. A heap
"double free" verdict names where the damage surfaced, not where it was done; with corruption,
read the reported site as a symptom and go looking for a writer.

If it ever returns, page heap (`gflags /p /enable FFViper.exe /full`) faults on the bad write
itself rather than the aftermath.

### 11. Nosewheel steering, and the `IsDigital` trap

`NoseSteerOn` gates the block in `eom.cpp` that gives the rudder pedals authority over the
nosewheel. It has two setters and **a realistic-avionics ramp start met neither**: the
preflight one in `AircraftClass` is skipped for `START_RAMP` by design, and the automatic one
was gated on `IsSet(IsDigital) or not g_bRealisticAvionics` — and `MakePlayerVehicle` *clears*
`IsDigital` on the player's airframe. The pedals moved the rudder surface and the jet would
not turn while taxiing.

**This is the second bug on this branch from that one assumption** (the JFS switch was the
first). Treat any condition testing `IsDigital` as suspect: on the player jet it is false.

`SimNWSToggle` is new — FF6 had no NWS control of any kind. It defaults ON so leaving it
unbound just means steering works. It is registered in `findfunc.cpp` but **not yet listed in
`controls.xml`'s function catalogue**, so it will not appear in the Controllers list until
that entry is added; that file also stores the user's bindings and is rewritten by the game,
so edit it with care.

The AR/NWS panel lamp needed nothing: `cautions.cpp` already lights it from `NoseSteerOn` or
boom contact, so it was dead on the ground for the same reason. It now doubles as a free
indication that steering is engaged.

### 12. The Controllers table is two client areas

`ui95` draws a control at `control.x + VX_[client]` and clips to that client, and the
scrollbar clamps `VX_` *upward* to `ClientArea.left` (`cscroll.cpp`) — so a control's x is
measured from its own client's left edge. That is the whole trick behind the frozen columns:

```
client 4 = frozen strip,    window x 163..503   FUNCTION + KEYBOARD
client 2 = scrolling strip, window x 503..845   device columns, re-based to x 0
```

The frozen controls keep the coordinates they always had; the device columns drop the
`DEVCOL_X0` origin because their client now *starts* where that origin was. `WIN_MAX_CLIENTS`
is 8 and this window uses 0..4.

Client 4 has no scrollbar — that is what stops it drifting sideways — so its vertical position
is mirrored from client 2 once a frame in `RefreshJoystickCB`. **Do not try to hook the scroll
call sites instead:** the wheel and the slider drag are handled inside ui95, which writes
`Parent_->VY_` directly and never calls back into `controltab.cpp`.

A header belongs to the client its own column lives in. Giving them all one client is what made
FUNCTION and KEYBOARD scroll out over the device headers.

`g_keyListPreserveScroll` must be set by anything that rebuilds the list without changing the
row set — binding, and all three clear paths. Only a search/filter should scroll back to the top.

---

## Facts worth not re-deriving

All measured against the shipped Korea data with `tools/terrain/tilesurvey.py`. Re-run it
for any other theater rather than assuming these.

- LOD 0 is 4096×4096 posts at **819.995 ft/post**; `Theater.map` flags = `0x1`, so posts
  are 9-byte `TNewdiskPost`. Blocks are deduplicated (47,128 stored of 65,536).
- **A tile spans 4×4 posts**, not one — 16,384 of 16,384 sampled groups share a texID.
  One tile = 3,280 ft of ground. Tiles are DXT1, 256² (809) or 512² (278), all present,
  H resolution only.
- **`v` is inverted**: `DiskblockToMemblock` computes `u` increasing with the column but
  `v = stop - ...` decreasing with the row, so the first post row of a cell is at the
  *bottom* of its tile. Sampling rows like columns mirrors every cell against its
  neighbours. Measured, with the column axis as the control (adjacent cells usually carry
  different tiles, so a perfect seam is not available): column seams 2.10× interior
  detail, rows sampled like columns 2.81×, rows inverted **1.98×**.
- Quantising tile pixels into the theater's own `TMap::ColorTable` costs a mean RGB error
  of **9.6 of 441** (p99 26). That is why the map can stay 8-bit and every overlay keeps
  working.
- **`MaxZoomLevel_` is 64** source pixels across the view for Korea, not the 32 floor in
  `SetupMap` — the floor never bites. Max zoom is 8.6 nm.
- Tile reuse is extreme: 1,071 distinct tiles theater-wide, one of them 49.4% of the
  ground, top 200 = 95.2%. A small decoded-tile cache goes a long way.

### The 3D pit object

Measured with `tools/models/objsurvey.py` against the shipped `KoreaObj`.

- **The live pit is parent 2402**, LOD `3DPIT_F16CJ_L1` — set by `cockpitmodel 2402` in
  `art/ckptart/3Dckpit.dat`, not by `VIS_VRCOCKPIT`. It declares **255 switches**; the
  three older pits (parents 1, 870, 2403) declare only 129, so every switch id above 128
  is silently dropped on those — `DrawableBSP::SetSwitchMask` returns early.
- **A switch is a run of sibling DOF nodes** sharing a `SwitchNumber`, each with its own
  `SwitchBranch` and its whole subtree sized by `dwDOFTotalSize`. The engine draws the
  first branch where `SwitchValues[n] & (1 << branch)`. **Mask 0 therefore draws nothing
  at all** — that is what "the part is missing from the pit" usually means.
- 150 switch numbers are present in the tree. Not all of the 255 declared are modelled, so
  check before assuming a control exists — and check before modelling one that already does.
- **`3dbuttons.dat` coordinates are model units × −569** (its own header says so). That is
  how to confirm a click hotspot lands on the geometry it is supposed to drive.

**The detail layer is a stand-in, not a different map.** `MapRect_`, `CenterX_`, `scale_`,
`FEET_PER_PIXEL`, the zoom clamps, `CampGridToOverlay` and every icon position still speak
in whole-theater base-map pixels and were not edited. `C_ScaleBitmap::SetDetail` swaps only
what gets blitted. Nothing re-projects because nothing moved.

Not done on purpose: zoom tiering (subdiv is recomputed every time the rect moves); LOD 2
and below (past `lastNearTexLOD` the posts index the *far* texture set); lowering the zoom
floor now that there is real detail under it; 512² tiles at full resolution (all tiles are
normalised to 256² on load).

---

## Known open

| Item | State |
|---|---|
| Damage does not feed **link cost** | A dropped bridge is expensive to cross but pathfinding still routes over it. Touches everything walking the objective graph. |
| Missile fin flicker | Long-standing. Per-surface `dwzBias` was restored and did **not** fix it — do not re-chase that. |
| Objective icons shaded by health | Asked for, not built. The *Damage overlay layer* exists but must be switched on; the request was to darken the red icons themselves. |
| Pale circles on the campaign map | Pre-dates this work. Ruled out: Logistics overlays, threat rings (`ShowCircles` is inside `#if 0`), the terrain basemap, the waypoint list. They sit near steerpoints and airfields. Cheap test: zoom in and out — map imagery scales, drawn marks do not. |
| **Add Squadron** dead in campaign | Stock, not new. `ObjMenuOpenCB` clears `C_BIT_ENABLED` when `GameType == 1`; basing a squadron is a TE-editor operation. Hiding the row is a one-liner if it is worth removing. |
| Menu is fixed-resolution | UI95 is not a layout engine — windows are absolute pixel rects from `.scf` with art authored to match. Routes: render at native size and scale the composited surface, or re-author every resource (a data project). **Not on the critical path for map detail**: zooming shows less ground in the same window, so a bigger window does nothing for the zoom ceiling. |

---

## Build and deploy

x64 only. MSBuild lives at
`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
— VS 2022's will fail with "v145 toolset not found".

```
MSBuild.exe FreeFalcon.sln /p:Configuration=Release /p:Platform=x64 /m
```

`Falcon4.vcxproj` links with `/FORCE`, so **a link error will not fail the build**. Grep
for `LNK2001` / `LNK2019` / `unresolved` after anything that adds or moves a symbol.
`LNK4088` is normal here — it comes from the pre-existing duplicate `IID_*` symbols
(`LNK4006`), not from unresolved externals.

Rebuild All whenever a header gains a member.

**Nothing copies the exe into the game directory.** The build lands in
`Falcon4___x64_Release/FFViper.exe`; the game runs `C:\FreeFalcon6\FFViper.exe`. Copy it
by hand, and check the timestamps before concluding a change "did nothing" — that has
already cost one round trip.

Game config is `C:\FreeFalcon6\ffviper.cfg`. Keys are the variable name including the
`g_b`/`g_n`/`g_f`/`g_s` prefix, e.g. `set g_bCampMapDetail 1`.

---

## Housekeeping

`main` is pushed. **PR #53** (`ajalberd:main` → `FreeFalcon:develop`) carries the campaign
planning and map work. PRs #49/#50/#52 are fully subsumed by `main`.

Two things that cost time before:

- **Check subsumption with `git cherry -v main origin/<branch>`, not a diff.** These
  branches were cut from `develop`, so `git diff main...origin/<branch>` answers a
  different question and showed 746 phantom insertions where the real answer was zero.
- `gh` resolves this checkout to upstream `FreeFalcon/freefalcon-central` while the
  branches live in `ajalberd/freefalcon-central`, so a PR needs `--head ajalberd:<branch>`.
