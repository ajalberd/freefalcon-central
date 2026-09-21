# Campaign / scenario editor

**Status: built and working.** `tools/campaign-editor/` is an external GUI for
authoring theaters and campaigns — a stdlib Python HTTP server plus a browser
page, no dependencies. See `tools/campaign-editor/README.md` for how to run it
and what it covers. This file keeps the format findings, because they are the
expensive part and they are worth having outside the tool's own source.

## The goal

An external GUI for authoring campaigns and scenarios: campaign length and win
conditions, unit placement, squadrons, using the shipped campaigns as base
templates. Worked example: **Korea, 1980s** — F-16s carrying Sparrows, an older
inventory, more infantry. That example exercises three different kinds of edit
at once — loadout and weapon availability, the class table, and unit density —
so a tool that can only move existing units around would not reach it.

All three are reachable now, plus unit placement on a projected map.

## What was decided, and why

**CampTool was not used.** `src/campaign/camptool/` is a real Win32 dialog app
wrapped in `#ifdef CAMPTOOL`, enabled only in Debug configurations. It was not
built or evaluated: the formats turned out to be cheap to read directly, and an
offline Python tool does not tie the editor's build to the engine's or inherit
a 1998 dialog layout. `camptool/` remains the best cross-check if a format
question ever comes up that the loaders do not answer.

**The tool reads the files, it does not link the engine.** Every layout is
transcribed from the headers and the (de)serializers, and
`tools/campaign-editor/selftest.py` checks those transcriptions against the
shipped data: read every table and every campaign file, write it back in
memory, require identical bytes. 1582 checks on a stock install. This is the
whole safety story — a layout that is one byte off still parses, it just
shifts every field silently.

## The data

Three campaign directories under `C:\FreeFalcon6\campaign\`: `SAVE`, `eurowar`,
`korea2012`. Each carries its **own `CampaignDB`**, which is what makes "Korea
'80s with an older inventory" possible at all: the class table is per-campaign,
not global.

### Open questions, now answered

**Which files are generated vs. authored?** The `.cam` container holds ten
members. `.cmp` (campaign header), `.uni` (units) and `.obj` (objectives) are
the authored ones and all three are editable here. `.obd` holds objective
deltas against the base scenario, `.tea` team data, `.plt` pilots, `.wth`
weather, `.evt`/`.pst` event and post state. The editor writes back only the
members it changed and leaves the rest byte-identical.

**What are `save0/1/2.cam`?** They are the three campaigns the theater
advertises — for korea2012, the "Korea 2012 / Korean Freedom / Korea 1983"
listed in the `.tdf` description. `Auto Save.cam` is a user autosave.
`Instant.cam` carries no units. `te_new.tac` is the tactical-engagement
template.

**What defines a win condition today?** For tactical engagements, explicitly:
`TE_TimeLimit`, `TE_VictoryPoints` and `TE_team_pts` in the campaign header.

For a campaign it is **not in the `.cam` at all**. `EndgameResult` only records
which outcome fired. The conditions live in `<scenario>.tri`, a plain-text
script beside the campaign, evaluated every tick by `ReadScriptedTriggerFile`
(`src/campaign/campupd/cmpevent.cpp`). `#END_GAME <n>` posts
`FM_CAMPAIGN_OVER`, and that is the *only* writer of `EndgameResult` outside a
tactical engagement — a script without one means a campaign that cannot end.
See "The trigger script" below. The starting ratios and `Tempo` remain the
knobs that shape the war in between, per `CAMPAIGN-SUPPLY-ENGINE.md`.

**Is the class table editable independently of the object database?** Yes for
retuning, no for adding. Every `FALCON4.*CD` row can be edited and duplicated
freely, and the class table's `visType[]` points at models in `KoreaObj`, which
is shared art. So Korea '80s is reachable by retuning and re-pointing existing
rows; a genuinely new vehicle type needs a model first.

**External or in-game?** External, and not in the solution. A separate Python
tool does not need the engine's structs at runtime, only their layouts, and
those are pinned by the self-test.

## Formats

### CampaignDB tables

Flat arrays of fixed-size C structs behind a 16-bit count, in one of two
framings (`LoadUnitData` and friends, `src/falclib/entity.cpp`):

```
classic   <count:i16> <record * count>
FFDBC     <0:i16>     <record * count> <count:i16>
```

FreeFalcon's `g_bFFDBC` picks the second when the leading count reads zero —
that is how the shipped tables get past the old 16-bit entry ceiling. Every
table in korea2012 uses FFDBC except `.PD` and `.PHD`.

Verified record sizes, all at natural alignment with `char` (not `wchar_t`) for
`_TCHAR`:

| File | Record | Bytes | Rows (korea2012) |
| --- | --- | ---: | ---: |
| `FALCON4.ct` | `Falcon4EntityClassType` | 81 | 3678 |
| `FALCON4.UCD` | `UnitClassDataType` | 336 | 750 |
| `FALCON4.VCD` | `VehicleClassDataType` | 160 | 691 |
| `FALCON4.WCD` | `WeaponClassDataType` | 60 | 797 |
| `FALCON4.OCD` | `ObjClassDataType` | 54 | 689 |
| `FALCON4.FCD` | `FeatureClassDataType` | 60 | 648 |
| `FALCON4.FED` | `FeatureEntry` | 32 | 11073 |
| `FALCON4.WLD` | `WeaponListDataType` | 208 | 654 |
| `FALCON4.SSD` | `SquadronStoresDataType` | 603 | 275 |
| `FALCON4.RCD` | `RadarDataType` | 58 | 169 |
| `FALCON4.RWD` | `RwrDataType` | 22 | 63 |
| `FALCON4.ICD` | `IRSTDataType` | 20 | 63 |
| `FALCON4.VSD` | `VisualDataType` | 20 | 17 |
| `FALCON4.SWD` | `SimWeaponDataType` | 52 | 664 |
| `FALCON4.ACD` | `SimACDefType` | 52 | 266 |
| `FALCON4.PHD` | `PtHeaderDataType` | 28 | 410 |
| `FALCON4.PD` | `PtDataType` | 12 | 9500 |
| `falcon4.rkt` | `RocketClassDataType` | 6 | 17 |
| `falcon4.ddp` | `DirtyDataClassType` | 4 | 184 |

`Falcon4EntityClassType` is `#pragma pack(1)`, but the `VuEntityType` it embeds
was compiled at natural alignment, so it keeps its own three bytes of tail
padding — 60, not 57. `dataPtr` is the 32-bit on-disk slot and holds a **row
index** into the table named by `dataType`, not a pointer.

`VehicleClassDataType::Weapon[hp]` is likewise a row index: into `FALCON4.WLD`
when `Weapons[hp] == 255` (the "this is a weapon list" marker `LoadoutWeapons`
checks), into `FALCON4.WCD` otherwise. It is *not* a class-table id, which is
an easy and silent mistake — the wrong lookup returns plausible names.

### The `.cam` container

`StartReadCampFile`, `src/campaign/campupd/campaign.cpp`:

```
int32   offset of the directory
...     member payloads, back to back
at offset:
int32   member count
per member: uint8 name length, name, int32 offset, int32 size
```

`.ver` holds the campaign data version as **ASCII digits**. Shipped campaigns
are v73 or v99; the engine's `gCurrentDataVersion` is 73.

`.cmp` is `int32 outer, int32 raw_size, LZSS(raw)`, and the decoded stream is a
flat field sequence gated on that version — `CampaignClass::Decode` is the
reference.

### The `.obj` objective stream

`short num, int32 raw_size, int32 compressed_size, LZSS(raw)`, then `num`
records of `short type` plus `ObjectiveClass`'s own fields. There is no
polymorphic dispatch -- every objective is an `ObjectiveClass`. All 2600-odd
objectives in each shipped campaign decode with the stream consumed exactly.

The one awkward part is the per-feature status block: the writer stored a
`size` byte and that many bytes, and the reader recomputes what the length
*should* be from the current class table. When they disagree it reads the
smaller of the two and skips the rest, so the stream always advances by exactly
`size` either way.

`classInfo_[VU_TYPE]` gives the objective type, and the objective range of
`Classtable_Types` maps cleanly onto it -- verified against korea2012, where
every objective falls in the set airbase, airstrip, army base, border, bridge,
chemical, city, C2, depot, factory, fortification, intersection, nuclear, pass,
port, power plant, radar, radio tower, refinery, road, town, village, HARTS,
SAM site.

`static_data.nameid` indexes the place-name table: `DEFAULT.idx` is a short
count plus that many short offsets, and `DEFAULT.wch` is the char stream they
slice (`src/campaign/camplib/name.cpp`). The offsets are signed shorts used as
file positions, so a stream past 32767 bytes wraps negative and must be read
unsigned. `"Nowhere"` is the null place name.

### The `.uni` unit stream

`int32 outer, int16 count, int32 raw_size, LZSS(raw)`, then `count` records,
each `int16 type` followed by the entity's own `Save()` output. There are no
per-record lengths, so record N+1 is only findable by decoding record N
exactly. Dispatch is `NewUnit`: `classInfo_[VU_DOMAIN]` picks air/land/sea and
`classInfo_[VU_TYPE]` picks the kind, then

```
CampBaseClass -> UnitClass (+ waypoints) -> GroundUnit/AirUnit -> the subclass
```

All 31 campaign and TE files across the three theaters decode with the stream
consumed exactly: battalions, brigades, squadrons, flights, packages and task
forces.

Two details worth not re-deriving:

- `WP_HAVE_DEPTIME` is `0x01` and `WP_HAVE_TARGET` is `0x02`
  (`campaign/camplib/campwp.cpp`), which is the opposite of what the read order
  in the constructor suggests. Swapping them still decodes squadrons and
  battalions correctly and desyncs the moment a flight with a target waypoint
  appears — so the shipped campaigns look fine and every TE file breaks.
- `DiskUIEventNode` is 20 bytes, not 18. It is `#pragma pack(push, 4)`, so two
  bytes of padding follow `team`.

### LZSS

`src/utils/lzss.cpp`, blocked I/O, 12-bit index and 4-bit length, `BREAK_EVEN`
1, so a match carries 2..17 bytes. The window starts zeroed with
`current_position = 1`, so history byte *h* lives at `(h + 1) & 4095`, and
matches may overlap the bytes they are still producing. The editor's encoder is
a greedy matcher rather than the engine's binary tree; it lands within 10% of
the engine's own output, which is all that matters since any stream
`LZSS_Expand` accepts is valid.

### Coordinates

Grid units are kilometres. `ConvertGridToSim` (`campaign/camplib/find.cpp`)
maps grid **y** onto sim north and grid **x** onto sim east, so x runs west to
east and y runs south to north. `Kneemap.gif` is 1024×1024 and `TheaterSizeX/Y`
is 1024×1024, i.e. one pixel per grid kilometre with north at the top row —
grid coordinates land directly on image pixels after flipping y.

Checked, not assumed: 910 of 913 ground battalions in korea2012's `save0.cam`
land on land pixels and 49 of 57 task forces on water, with team means putting
CIS north-east and PRC north-west of the DPRK.

### The trigger script

`<scenario>.tri`, next to the campaign. A flat token stream with
`#IF_...`/`#ELSE`/`#ENDIF` nesting; conditions set a flag on a small stack and
actions run when the enclosing conditions hold. `#TOTAL_EVENTS` declares the
event count, everything before `#ENDINIT` is start-up state, `#ENDSCRIPT`
terminates.

All twelve shipped campaigns have exactly four endgames: an allied win, an
OPFOR win, a stalemate timer (`#IF_BORDOM_HOURS`) and a day limit
(`#IF_CAMPAIGN_DAY`). korea2012's `save0.tri`, for example:

```
#IF_CONTROLLED 2 O 680 260 404      ROK holds P'Yongyang South, P'Yongyang or Wonsan
#END_GAME 17
```

Two details worth not re-deriving:

- `#IF_CONTROLLED <team> A|O <campIds...>`: `A` is AND (hold **all**),
  anything else is OR (hold **any**).
- `#IF_CAMPAIGN_DAY G <n>` uses `>=`, despite `G`, so `G 14` fires on day 14.

The ids are campaign ids. The evaluator guards each lookup with
`if (o and ...)`, so an id no entity carries is *skipped*: harmless inside an
`A` list (reads as satisfied), fatal inside an `O` list (can never fire). Four
of the shipped scripts name five such ids each, all in the front-line events
rather than the endgames.

### The briefing text, and where it drifts

The text on the campaign-select page is not generated from the script. It is
three pairs of hand-written strings in `<artdir>/art/Main/lcktxtrc.irc`:

```
[ADDTEXT] TXT_SCENARIO_1 "Rolling Fire by FF5.5"
[ADDTEXT] TXT_SC_1 "Victory Conditions: Allies; Control P'Yongyang or Wonson ..."
```

`SelectScenarioCB` (`src/ui/src/campaign/cpselect.cpp`) maps `save0` to
`TXT_SCENARIO_1`, `save1` to `_2`, `save2` to `_3`; `art/campaign/select/
cs_pua.scf` binds `TXT_SC_<n>` to the SitRep box beside it. Note that the
stock Korea theater's `artdir` **is** the art tree (`art/main/...`) while an
override theater's `artdir` merely contains one (`artKorea2012/art/Main/...`),
and that Korea 1980s inherits `artKorea2012`, so the two share one file and
one set of three blurbs.

Nothing checks the text against the script, and all twelve shipped campaigns
were compared: nine agree, three do not, and the failures are the interesting
ones.

| Campaign | The text says | The script does |
| --- | --- | --- |
| EuroWar `save0` | allies hold P'Yongyang **or** Wonson | `#IF_CONTROLLED 2 A` -- **all three** of South P'Yongyang, P'Yongyang and Wonsan |
| Korea `save1` | allies recapture Seoul; 14 days | no allied objective at all: result 17 fires on *not having lost* past day 10, and the timeout is day 15 |
| EuroWar `save1` | Korea's stock blurb, unedited | allied win is a NOT-guard over four EuroWar objectives past day 10; timeout day 18 |

EuroWar `save0` is the one to remember: the difference between an easy
campaign and a nearly impossible one is the single letter between `A` and `O`
in `#IF_CONTROLLED`, and the briefing renders both as "or". "Wonson" is a typo
for Wonsan in every shipped blurb, and several theaters ship Korea's text
verbatim.

The editor reads both sides and writes both sides: the Victory tab shows each
`#END_GAME` with its guard chain as editable controls, prints the sentence the
script actually implements underneath the blurb, and will write that sentence
into `lcktxtrc.irc` on request. Both writers are line-surgical -- one directive
or one `[ADDTEXT]` rewritten, every comment, blank line and indent preserved --
because these files are hand-written and their layout is the only
documentation they have.

### Ground imagery, and the campaign grid

Theater.map (`tools/terrain/tilesurvey.py` reads it) puts LOD 0 at 256x256
blocks of 16x16 posts -- 4096x4096 posts, 820 ft each -- and one ground tile
covers 4x4 posts. 4 x 820 = 3280 ft = one kilometre, so **there is exactly one
DXT1 ground tile per campaign grid kilometre**, and the theater is 1024x1024
km. That is the same grid units and objectives are positioned on, and the same
1024x1024 the kneemap is drawn at. A campaign coordinate indexes a tile
directly; nothing needs rescaling, and the three coordinate systems were never
actually three.

The texID for a kilometre is the post at `(x*4, y*4)` in the LOD 0 grid
(100% of 4x4 post groups share one texID, so any of the four will do). Tiles
are DXT1 DDS under `texture/texture/`, 256x256 or 512x512, 1087 of them in
Korea with about 1071 actually referenced.

Orientation was measured, not assumed: indexing the texID grid as `[y][x]`
puts 100% of korea2012's 913 ground battalions on land and 82.5% of its naval
task forces on water. Every other reflection scores far worse. So the array
row is campaign y (north) and the column is campaign x (east).

### TACAN stations

`stations.dat` in the campaign directory, whitespace-separated, `#` and `;`
comment a line (`TacanList::TacanList`, `src/sim/navaids/tacan.cpp`):

```
stationId channel band callsign range tactype ilsfreq
```

`stationId` is the objective's **campaign id**. Only the first four fields are
required; the loader defaults range to 150, tactype to 1 and ILS to 111.1. In
every shipped theater all 85 stations match an objective, and all 85 of those
are airbases.

### Campaign-map icons

The map symbology is real art, not drawn by the engine. A unit or objective
type's `IconIndex` is a numeric image id; `art/main/imageids.id` maps it to a
name (`10008` is `ICON_INFANTRY`), and `art/resource/imagerc.irc` says which
file holds a given team colour's set (`RED_TEAM_ICONS` -> `art\resource\reddark`).

Each set is a `.idx`/`.rsc` pair (`src/ui95/cresmgr.cpp`). Both carry an 8-byte
`int32 size, int32 version` header. The `.idx` body is 60-byte `ImageHeader`
records -- type, `char ID[32]`, flags, centre x/y, w, h, image offset, palette
size, palette offset (`src/ui95/imagersc.h`). The `.rsc` body holds 8-bit
indexed pixels followed immediately by 256 little-endian RGB555 palette words,
with `0x7C1F` (magenta) as the colour key.

**The offsets are relative to the data, not the file.** `C_Resmgr::LoadData`
reads the size and version before the buffer, so everything is shifted by
eight bytes -- and reading from byte zero yields an all-black palette that
decodes perfectly happily and is silently wrong.

Ground, naval and objective symbols are in `<colour>dark` (75 icons, plus a
`<colour>lite` variant for bright backgrounds); aircraft silhouettes are in
`<colour>air_<dir>` and `<colour>war_<dir>`, 120 each, one set per eight
compass directions. A team's colour index is the `team_colour` byte in the
campaign header, and `TeamColorIconIDs` (`src/ui/src/common/teamdata.cpp`)
maps it onto white, green, blue, brown, orange, yellow, red, grey in that
order.

In korea2012, 682 of 682 objective rows and 740 of 745 unit rows with a
non-zero `IconIndex` resolve to an icon that exists; the five misses are
aircraft types named in `imageids.id` that the shipped 120-icon air sets never
got art for.

### Theaters

A `.tdf` of `key value` lines listed in `theater.lst` at the game root, parsed
by `ParseSimlibFile` against `theaterdesc[]` in `src/falclib/theaterdef.cpp`.
Keys outside that table — `specialdbdir` is the one every shipped file carries
— are ignored by the parser but preserved on rewrite. A new theater is a copied
campaign directory, a `.tdf` pointing at it, and a line in `theater.lst`;
terrain, objects and art can stay shared with the parent.

The `CampaignDB` a theater uses is resolved as: `specialdbdir` if it exists,
else the campaign directory's own `CampaignDB`, else `objectdir`. That last one
is where the engine itself reads the tables from — `OpenCampFile` maps `.ct`,
`.ucd`, `.ocd` and the rest onto `FalconObjectDataDir`, which `theaterdef.cpp`
sets from `objectdir` — so a theater that ships no DB of its own, like the
Israel theaters, which keep the only copy in `Theaters\Israel\objects`, still
resolves every class index. Cloning such a theater copies those tables into the
new campaign directory, so the clone does not edit the parent's.

### Creating units

A new battalion, brigade or task force is built field by field from the
`Save()` chain rather than cloned, so it inherits no parent formation,
objective assignment or waypoint list. A battalion record is 115 bytes with no
waypoints; a task force is 76.

- VU ids come from the non-volatile band, `LAST_OBJECTIVE_VU_ID_NUMBER + 1`
  upward (`campaign/camplib/campbase.cpp`), and camp ids from the first free
  slot across both units and objectives.
- `roster` is two bits per vehicle group, 16 groups.
  `UnitClass::BuildElements` builds it with XOR, so a group count above 3 would
  bleed into its neighbour; the editor masks instead.
- Defaults matching what the shipped campaigns actually contain: `unitFlags`
  `U_PARENT` (0x20), `orders` `GORD_DEFEND` (6), supply and morale 100,
  everything else zero.

Flights and packages are deliberately not creatable: the campaign AI creates
and destroys them while planning, and a hand-authored one has no package to
belong to.

## Do not re-derive

- `tools/terrain/tilesurvey.py` and `tools/models/objsurvey.py` read terrain and
  the object database offline, no game needed.
- `tools/campaign-editor/ffcamp/` is now the third of these: it reads the
  campaign database, campaign files, unit streams, objective streams, the UI
  icon art, the terrain imagery and the trigger scripts offline, and writes
  the editable ones back.
  `ffcamp/terrain.py` is the one module that needs numpy; it renders the same
  ground tiles `tools/terrain/tilesurvey.py` surveys. Use it rather than writing a fourth parser,
  and run its `selftest.py` after touching any layout -- 1582 checks on a stock
  install, all of them byte-equality against the shipped data.
