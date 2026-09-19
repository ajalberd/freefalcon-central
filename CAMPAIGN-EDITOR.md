# Campaign / scenario editor

**Status: not started.** This is reconnaissance, not a plan — what is already in the tree,
what the data looks like, and where the first hour should go. Append findings here as they
come up rather than growing `WIP-NOTES.md`.

## The goal

An external GUI tool for authoring campaigns and scenarios: set the campaign length and win
conditions, place units, edit squadrons, using the three shipped campaigns as base templates.

Worked example to design against: **Korea, 1980s** — F-16s carrying Sparrows, an older
inventory across the board, and a lot more infantry. That example is doing real work, because
it exercises three different kinds of edit at once: loadout/weapon availability, the class
table, and unit density. A tool that can only move existing units around would not reach it.

## There is already an editor in the tree

`src/campaign/camptool/` — `campdriv.cpp`, `dialog.cpp`, `unitdlg.cpp`, `display.cpp`,
`wingraph.cpp`, plus its own `camptask/` copy of the campaign task code. `CampTool` is a real
project in `FreeFalcon.sln`, and `unitdlg.cpp` alone has unit size/type combos, strength-point
adjustment and per-domain stat dialogs.

**It is invisible in Release.** The whole thing is wrapped in `#ifdef CAMPTOOL`, and
`camptool.vcxproj` defines `CAMPTOOL` in **Debug|Win32 and Debug|x64 only** (lines 198 and
224). Release, Remote and Protected omit it, so every source compiles to an empty translation
unit. Nobody deleted this; it was just never turned on.

So the first question is not "how do we build an editor" but **"how much of one already
works?"** That is a cheap question to answer and it changes the whole shape of the project.

## First step

Build `CampTool` as **Debug|x64** and see what comes up. Note the memory rule about never
building Win32 in this tree. Expect it to be stale — it is legacy code that has not been
compiled in this configuration for a long time, and the x64 serialization fixes that were
applied to the object database may not have reached it.

Three outcomes, each pointing somewhere different:

- It builds and runs → the project is "modernise and extend an existing editor", which is far
  cheaper than a new tool and keeps the file-format knowledge that is already encoded in it.
- It builds but is unusable → harvest it as documentation of the formats, write the GUI fresh.
- It does not build → find out whether that is a handful of x64 issues or rot.

## The data

Three campaign directories under `C:\FreeFalcon6\campaign\`: `SAVE`, `eurowar`, `korea2012`.
Those are the "three campaigns" to template from.

Per campaign:

- `CampaignDB/` — the class and object tables. `FALCON4.{ACD,FCD,FED,ICD,OCD,PD,PHD,RCD,RWD,
  SSD,SWD,UCD,VCD,VSD,WCD,WLD}`, plus `FALCON4.ct`, `falcon4.ddp` (?), `falcon4.rkt`,
  `f4sndtbl.sfx` and its own `KoreaObj.Dxh`. **Each campaign carries its own copy**, which is
  what makes "Korea '80s with an older inventory" possible at all — the class table is
  per-campaign, not global.
- `*.cam` — campaign state/saves, including `Instant.cam`.
- `*.tac` — tactical engagements. `te_new.tac` in each, plus a set of naval templates in
  `SAVE`.
- `*.trn` — training missions.
- Theater and priority data: `Falcon4.AII`, `Defense.pri`, `Intdict.pri`, `KOREA.NAM`,
  `KOREA.THR`, `KOREA.tc`, `Korea.tm`, `FLIST.TXT`, and a pile of `.db`/`.b` files
  (`Element.db`, `Flight.db`, `FLEVENT.DB`, `FORD*.DB`, `LOADOUTH.B`, …).
- `Kneemap.gif` — the kneeboard map, per campaign. (The 3D kneeboard reads this.)

Engine code: `src/campaign/{camplib,camptask,camptool,campui,campupd}`.

`CAMPAIGN-SUPPLY-ENGINE.md` already documents how supply, production and power work, and is
the companion to read before touching win conditions — "campaign length" and "who is winning"
are downstream of that model, not independent knobs.

## Open questions

- **Which of these files are generated vs. authored?** An editor must not write the ones the
  engine derives, or saves will disagree with the initial state.
- **What actually defines a win condition today?** Worth finding before designing UI for it.
- **Is the class table (`FALCON4.*CD`) editable independently of the object database?** The
  Korea '80s example needs weapon and aircraft availability changes, and those tables index
  into `KoreaObj`, which is shared art. Adding a unit type that has no model is a dead end.
- **External or in-game?** "External GUI" was the ask. CampTool is a Win32 dialog app inside
  this solution, which is external in the sense that matters (a separate exe) but ties the
  tool's build to the engine's. That is probably a feature — it gets the real structs for free
  instead of re-declaring the formats and drifting.

## Do not re-derive

- `tools/terrain/tilesurvey.py` and `tools/models/objsurvey.py` read terrain and the object
  database offline, no game needed. Any unit-placement UI will want the objective graph and
  the tile data; read them with these rather than writing a third parser.
