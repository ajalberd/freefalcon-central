# Work in progress

Rolling handoff note. Read this first. Keep it short — when something is settled, cut it
down to the one fact a future session would otherwise re-derive, and let the code
comments and commit messages carry the rest.

Companion docs:

- `CAMPAIGN-SUPPLY-ENGINE.md` — how supply, production and power actually work.
- `tools/terrain/tilesurvey.py` — reads a theater's terrain and tile data offline, no
  build and no game needed. Re-run it instead of re-deriving any terrain number.

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
