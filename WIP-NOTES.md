# Work in progress

Rolling handoff note. What is in flight, what is waiting on a test, what is known
broken. Start a new session by reading this file.

Companion docs:

- `CAMPAIGN-SUPPLY-ENGINE.md` — how supply, production and power actually work, and
  what has been changed about them.

---

## Test these first

Everything below is committed on `main` and the full solution builds
(`FFViper.exe` links clean, no unresolved externals), but none of it has been run.

### 1. Supply interdiction — `SupplyInterdiction`

A damaged bridge or road now costs the supply run crossing it. Default 100, `0`
restores stock exactly.

Test: bomb a bridge on a supply route, watch the Logistics → Supply flow overlay
downstream of it thin out. Full detail in `CAMPAIGN-SUPPLY-ENGINE.md`, "Roads and
bridges".

### 2. Build package — right-click a target

**First run found it missing from every popup but the map.** `HookupCampaignMenus`
called `CampaignPackageMenuAttach` five times on `MAP_POP` and never on the other
four; the rebuild side was wired to all of them correctly, but it opens with
`GetSubMenu(MID_CAMP_PACKAGE)` and returns when there is no submenu, so an
objective popup came up as stock Recon / Status / Add Squadron. Fixed — one attach
per menu. The map popup was never wrong (`AddItem` refuses a duplicate ID, so
calls 2-5 returned at the first line), which is why the mistake was invisible
anywhere it could have been noticed.

**Second run found the submenu greyed on every target.** `LogCampMenu 1` said why:

```
[PKGMENU] team=1 ... | squadrons=112 otherTeam=112 noVehicles=0 noRole=0 | offered=0
```

The candidate filter compared against `gSelectedTeam`, which is the *Tactical
Engagement* editor's variable — TE drives it from its team list box and hardcodes
it to 1 for training, the campaign assigns it once on entry and nothing keeps it in
step. Same shape of mistake as reaching for `te_scf.lst`'s windows, one layer in.
Now `FalconLocalSession->GetTeam()`, which is what every other campaign screen and
`ato.cpp` use. `SetOwner` takes a *country* and now gets one.

Team fix confirmed in the field: `myTeam=2 (session country=1, gSelectedTeam=1)`,
roster `[2:49 4:4 5:13 6:46]` — country 1 maps to team 2, and `gSelectedTeam` was
holding the *country*.

**The submenu is a real wall**: `candidates=49 ... shown=12 of 12 slots`. Twelve
rows cannot show the roster, which is what the Add Package dialog is for.

### 2b. Add Package — the real dialog

Right-click a target → **Add Package** opens it. Confirmed opening, and Add Flight
opens from its New button.

**Correction to what this section first said.** It claimed `PACKAGE_WIN` was loaded
by nothing. Wrong — `CMN_SCF.LST` names `art\taceng\package.scf`, and
`LoadCommonWindows()` runs on every screen including the campaign, so it was always
there. The search that produced the claim globbed `art/*.lst`, which does not match
an uppercase `.LST`; `CMN_SCF.LST` was never read. Loading it a second time gave
two windows, the second inert. Fixed — `cp_pkg_scf.lst` now carries only
`Te_flght.scf`, which *is* genuinely absent (only `TE_SCF.LST` names it).

**The missing art holds up**, re-checked against the `.idx` resources rather than
the `.irc` files that point at them: `WIN_PACKAGE` / `WIN_ADD_FLIGHT` are in none
of campaign, common, campmap, mission, intel, records, tactical, tacengbg — only
`art\uiskin\ff4\win_all`. So the package window was opening with **no background**,
which is the likeliest reason an earlier session decided Add Package "did nothing".
That is what `art\cp_uiskin.lst` + `art\resource\uiskin_ff4.irc` fix.

**Resolved: flights were being pinned to an exact time on target.** The trace:

```
[PKGFLT] BuildMission failed err=1 (NO_ASSETS) | start_at=1 |
         tot=34209000 totType=3 now=32438610 takeoffLock=0 totLock=34209000
```

`totLock` non-zero = the TOT padlock is closed, and `tactical_make_flight` checks
`gPackageTOT` **before** `start_at`, pinning `tot_type` to `TYPE_EQ` — be over the
target at exactly that second. The planner declining that is the planner working.
Same fact explains why the Status dropdown seemed inert: `start_at` is only read
once `gPackageTOT` is zero, so it was never reached.

The window always opened that way — both locks start at 0 in `package.scf` and
`SetupPackageControls` calls `LockTakeoffTimeCB`, whose else branch closes the TOT
lock. Right for the TE editor, wrong for a campaign, so the campaign now opens with
**takeoff** locked instead (`CampaignPackageTakeoffLock`, default on; TE untouched).
The refusal message now names which case it was.

Allied Force was checked as a reference (`D:\Program Files (x86)\Lead
Pursuit\Battlefield Operations`): same control IDs, same `CMN_SCF.LST` placement,
just re-laid-out (426x430 vs 450x768). It **keeps** `PILOT_SKILL` and
`START_AT_LIST` at y=132 and y=154 of a 223-tall window, so the screenshot without
those rows is BMS, not AF — dropping Status would not be "what AF does".

Still untested: the flight tree with several flights, per-flight targets, the
Takeoff/TOT locks, window-group interaction with the map, and `REQF_TE_MISSION`
alongside a running ATO.

---

## Confirmed working

- Supply overlay draws the network as routes, not a scatter of discs.
- Menu 3D viewers (recon, loadout, tactical reference) — the RTT had no depth
  buffer, which is why models rendered see-through.
- HUD no longer draws over the canopy bars.
- Terrain-derived campaign map with zoom detail.
- Throttle invert.

---

## Known open

| Item | State |
|---|---|
| Damage does not feed **link cost** | A dropped bridge is expensive to cross but pathfinding still routes over it. Touches everything walking the objective graph, not just supply. |
| Missile fin flicker | Long-standing. Per-surface `dwzBias` was restored and did **not** fix it — do not re-chase that. |
| Objective icons shaded by health | Asked for, not built. What exists is the *Damage overlay layer*, which you have to switch on. The narrower request was to darken the red icons themselves. |
| Pale circles on the campaign map | Unidentified, and **pre-dates this work** (visible in screenshots from before the FLOT overlay existed). Ruled out by reading: the Logistics overlays (trace says layer 0), threat rings (`Circles_`/`ShowCircles` is inside `#if 0`, and `ShowCampaignOverlay` clears the overlay anyway), the terrain-derived basemap (`CampMapFromTerrain 0` and they remain), and the waypoint list (no circle drawing in it). They sit near steerpoints and airfields. Next cheap test: zoom in and out -- map imagery scales with the map, drawn marks do not. |
| ~~FLOT line~~ **done, confirmed drawing** | Map right-click -> **FLOT line**; knob `CampFlotLine`, default **off** (the row is always present; the knob is only the state it starts in). A toggle rather than a layer, so it composites over whichever Logistics layer is live and survives all of them being off. Two things had to be found: the list is only ever built as the player enters a vehicle (`gamemgr.cpp`), so the overlay rebuilds it before reading, and the pale discs it was first blamed for are steerpoint markers. Confirmed in the field: 6 points, 82-136 px apart on a 2 px/grid overlay. That coarseness is `RebuildFLOTList`'s own 30 km dedupe, so the line cuts corners on a detailed front -- drawing from `FrontList` directly would be finer, and is a different job. Inherited: the single-axis sort will cross itself where a front doubles back. |
| **Add Squadron** is dead in the campaign | Stock, not new. `ObjMenuOpenCB` (campmenu.cpp) clears `C_BIT_ENABLED` on `MID_ADD_SQUADRON` whenever `GameType == 1`, so on an airbase it greys out and everywhere else it is off — basing a squadron is a TE-editor operation. `SetupCampaignMenus` hides `MID_SQUADRONS` on the objective popup but leaves this one visible. Hiding it too is a one-liner if the dead row is worth removing. |

---

## Housekeeping

`main` is pushed. **PR #53** (`ajalberd:main` -> `FreeFalcon:develop`) carries the
campaign planning and map work.

PRs #49, #50 and #52 were all fully subsumed by `main` and #50/#52 are now closed;
#49 has zero commits `main` does not already contain.

**Check subsumption with `git cherry`, not a diff.** `git cherry -v main origin/<branch>`
marks a commit `-` when an equivalent patch is already on `main`, which is exactly the
question. `git diff main...origin/<branch>` answers a different one: these branches were
cut from `develop`, so the three-dot base is `develop` and the diff shows what the branch
changed relative to *that*, not what it would add to `main`. It looked like 746
uncommitted insertions when the real answer was zero.

`gh` resolves this checkout to the upstream **`FreeFalcon/freefalcon-central`** while the
branches live in **`ajalberd/freefalcon-central`**, so a PR needs
`--head ajalberd:<branch>`; a plain `--head main` fails with "No commits between
develop and main".

---|---|
| #52 | `vr-frame-pacing` |
| #50 | `build-x64-objdir` |
| #49 | `vr-stereo-convergence-fixes` |

Suggested: close #50 and #52, open one PR from `main`.

---

## Build

x64 only. MSBuild lives at
`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
— VS 2022's will fail with "v145 toolset not found".

```
MSBuild.exe FreeFalcon.sln /p:Configuration=Release /p:Platform=x64 /m
```

`Falcon4.vcxproj` links with `/FORCE`, so **a link error will not fail the build**.
Grep the output for `LNK2001` / `LNK2019` / `unresolved` after any change that
adds or moves a symbol.

Rebuild All whenever a header gains a member.

---

## Next up: campaign map detail on zoom

**The question.** Zooming into the campaign map stops getting sharper past a point
and starts magnifying pixels. Where does the ceiling actually sit, and what would
it take to have the map keep resolving into real ground the way the 3D world does?

### Where the ceiling is now

Three separate limits, and it matters which one you are hitting:

| Limit | Value (Korea) | Status |
|---|---|---|
| Source image resolution | LOD 0, ~820 ft/post → **1 map pixel per post** | **This is the wall.** |
| Zoom floor | `MaxZoomLevel_` floored at **32 source pixels across the view** (`cmap.cpp`, in `SetupMap`) | Reachable, deliberate |
| Map window size | client rect of the map window | Not the constraint — see below |

`BuildTerrainMapImage` (`cmap.cpp:452`) reads `Theater.o<lod>` + `Theater.l<lod>`
once at first open and turns the **whole theater** into one 8-bit `IMAGE_RSC`,
`BlocksWide × 16` by `BlocksHigh × 16` pixels. One post → one pixel, coloured by
`post.color` through `TMap::ColorTable`. Zooming does not re-read anything; it
changes how many of those source pixels are stretched across the window.

So at full zoom you are looking at 32 posts across the window — about 4.3 nm of
ground at ~820 ft/post, magnified ~25× on an 800 px window. The blockiness is real
pixels being enlarged. **At LOD 0 there is no finer post data to go and fetch.**

### What would actually give more detail

Each terrain post carries more than a colour byte:

```c
typedef struct TNewdiskPost {
    UInt32 texID;   // <-- the texture tile the 3D engine draws on this post
    Int16  z;
    UInt8  color;   // <-- all the map currently uses
    UInt8  theta, phi;
}
```
`src/graphics/include/tdskpost.h`

`texID` is the tile the sim renders on that post — a DDS loaded through
`TextureBankClass` (`src/graphics/bsplib/texbank.cpp`). That is the same ground
imagery you fly over. A 64×64 tile per post is **64× the linear resolution** of the
one colour byte; at ~820 ft/post that is ~13 ft/pixel.

**It cannot be one bitmap.** 64× linear is 4096× the pixels — Korea would be tens
of gigabytes. The current build-it-all-once design is exactly what has to change.

### The work, in the order it should be done

1. **Establish what the tiles actually are.** Read the DDS header for a handful of
   `texID`s: dimensions, format, whether they are DXT-compressed (needs decoding to
   get CPU pixels — the texture bank hands out GPU handles, which the 2D map cannot
   blit). Instrument rather than assume; the tile size is in the header, not in the
   code. This step alone decides whether the rest is cheap or expensive.

2. **Decide the cheap win first.** Before any of the below, check whether raising
   the `MaxZoomLevel_ < 32` floor is what you actually want. If the complaint is
   "it stops zooming" rather than "it goes blurry", that is a one-line change.

3. **Replace the single image with a view-window renderer.** Instead of
   `BuildTerrainMapImage` producing the theater, have it produce **only the posts
   currently visible, at a resolution chosen from the zoom level**. Zoomed out:
   today's one-pixel-per-post path, unchanged. Zoomed past some threshold: sample
   the tile referenced by each visible post.

4. **Cache by (block, zoom tier).** Rebuilding on every pan would be unusable.
   Tier the zoom into a few steps so the cache key is coarse, and evict on distance
   from the view.

5. **Keep the overlay coordinate system honest.** `s_mapFeetPerPixel` /
   `CampGridToOverlay` / `StampOverlayDisc` all assume the overlay buffer matches
   the map image 1:1. A view-window renderer changes what "map pixel" means per
   frame, so the overlays (Logistics layers, FLOT) have to be re-projected rather
   than stamped once. **This is the part most likely to be underestimated.**

6. **Fall back cleanly.** Any theater whose tiles cannot be read must drop to the
   current post-colour map, the way the post map already falls back to the painted
   one.

### Is the fixed-resolution menu a prerequisite? No.

Worth separating two things that sound related and are not:

- **Menu resolution** caps how many screen pixels the map window occupies — i.e.
  how much ground you see at once, and how crisp it is at 1:1.
- **Source detail** caps how far you can zoom before magnifying.

Zooming in shows *less* ground in the *same* window, so a bigger window does
nothing for the zoom ceiling. Fixing the menu first would not move this problem
one inch, and the map work does not depend on it.

It is also the harder of the two jobs, for a reason worth knowing before starting
it: UI95 is not a layout engine. Windows are lists of **absolute pixel rectangles**
loaded from `.scf` resources, with bitmap art authored to match. There is no
relative sizing to turn on. The two routes are (a) render the menu at its native
size and scale the composited surface to the desktop — cheap, uniformly soft, and
the 3D viewers already go through an RTT so they could be done at native res; or
(b) re-author every resource, which is a data project, not a code one.

Both are real options, but neither is on the critical path for map detail. If the
motivation for the menu rework is *"the map is too small to be useful"*, that is a
genuine and separate complaint — just do not expect it to sharpen a zoomed-in map.

### Read before starting

- `BuildTerrainMapImage`, `s_mapFeetPerPixel`, `MapPixelsPerKm`, `SetupMap` —
  `src/ui/src/campaign/cmap.cpp`
- `TNewdiskPost` — `src/graphics/include/tdskpost.h`
- `TextureBankClass` — `src/graphics/include/texbank.h`,
  `src/graphics/bsplib/texbank.cpp`
- Knobs: `CampMapFromTerrain`, `CampMapTerrainLod` (0 = finest, already the
  default), `CampMapFlipNS` / `CampMapFlipEW` — `src/ui/src/f4config.cpp`
