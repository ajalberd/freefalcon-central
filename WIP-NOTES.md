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

Still unrun: anything past filing a package from the submenu. Leave `LogCampMenu 1`
on — the trace prints the squadron roster by team, so a wrong team can't hide
behind a count again.

### 2b. Add Package — the real dialog

**`PACKAGE_WIN` had never been loaded by any screen.** `art\taceng\package.scf`
defines it in full and is named by `art\tenew_scf.lst` alone, which appears nowhere
in the source. So `FindWindow(PACKAGE_WIN)` returned NULL in Tactical Engagement
too — the earlier note here that the stock items "work on the TE screen" was wrong.
`tenew_scf.lst` reads as a newer TE window set FF6 shipped and never switched on.

Its art is stranded the same way: `WIN_PACKAGE` and the rest of "FF4 UI version
0.3" live only in `art\uiskin\ff4\win_all.idx/.rsc`, which no image list names.

Three new files in the install carry it in — `art\resource\uiskin_ff4.irc`,
`art\cp_uiskin.lst`, `art\cp_pkg_scf.lst` — and `campaign.cpp` loads them before
`cp_scf.lst`, because `C_Resmgr::LoadIndex` is what registers the image names
(`AddNewID` for anything not in the ID table) and the `.scf` parse resolves them.
Knob: `CampaignPackageWindow`, default on. Confirmed: `[PKGWIN] PACKAGE_WIN
RESOLVED` appears in `FFDebug.log`.

**Untested: everything after the window resolving.** Right-click a target → **Add
Package**. What to watch:

- Whether it *draws*. The FF4 skin is grey/white and the campaign screen is blue;
  they have never been composited.
- Window groups. The package window is group 3274, Add Flight 3275, and the
  campaign screen has its own. `tactical_add_flight` calls
  `EnableWindowGroup(win->GetGroup())` — whether that disturbs the map underneath
  is unknown.
- The flight tree (`ATO_PACKAGE_TREE`) and Add Flight → several flights in one
  package, each with its own target. That is the thing the submenu cannot do.
- The Takeoff / Time on Target spinners and their locks — the other thing the
  submenu cannot do, and the answer to "no aircraft free in this time block".
- `REQF_TE_MISSION` alongside a running ATO, which matters more once a package has
  an escort keyed to a strike.


Replaces the stock Add Flight / Add Package items, which were hidden again because
they never worked in the campaign (they look up `PACKAGE_WIN` / `TAC_FLIGHT_WIN`
from `te_scf.lst`; the campaign screen loads `cp_scf.lst`, so the items came up and
silently did nothing).

Right-click a target → **Build package ▸** → two/four ship, then the squadrons that
can fly this mission against this target, nearest first:

```
336 TFS  F-16C x18  84nm  OCA Strike
```

Available on the map, objective, unit, air-unit and naval popups. Knob:
`CampaignAddMission`, now defaulting **on**.

Watch for:

- **A hand-built package carries `REQF_TE_MISSION`.** How that sits alongside a
  running ATO is genuinely unsettled — this is the most likely source of trouble.
- Whether the submenu lays out correctly with twelve rows. The row labels are
  rewritten on every open and unused rows hidden; `OpenWindow` re-measures each
  time, so it should, but it has not been seen.
- The refusal dialog. "No aircraft free in this time block" and "could not be
  planned" are reported separately, so the message should match the situation.

### 3. Producer / network census — `LogCampProducers 1`

Open the Production overlay once, then read `FFDebug.log`. Now covers the supply
network types as well as the producers, and reports `avgFeatures`.

**This answers an open question:** whether `TYPE_ROAD` objectives can be damaged at
all. Damage only registers through `CalcStatus` walking `class_data->Features`, so
a type with none can never drop below status 100 and the interdiction work above is
bridges-only. Roads are never *drawn* on the map (`filters.cpp:50` files them under
`_OBTV_OTHER`, off by default), which is why only the bridge icon is ever visible —
but that is a display filter, not an answer.

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
| **Add Squadron** is dead in the campaign | Stock, not new. `ObjMenuOpenCB` (campmenu.cpp) clears `C_BIT_ENABLED` on `MID_ADD_SQUADRON` whenever `GameType == 1`, so on an airbase it greys out and everywhere else it is off — basing a squadron is a TE-editor operation. `SetupCampaignMenus` hides `MID_SQUADRONS` on the objective popup but leaves this one visible. Hiding it too is a one-liner if the dead row is worth removing. |

---

## Housekeeping

`main` is **50 commits ahead of `origin/develop`** and **31 ahead of `origin/main`**
— nothing pushed. Three stale PRs, of which #50 and #52 are almost certainly
subsumed by what is on `main`:

| PR | Branch |
|---|---|
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
