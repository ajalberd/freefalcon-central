# Campaign editor

An external GUI for authoring FreeFalcon theaters and campaigns. It runs
outside `FFViper.exe` — a small local web server plus a browser page — and
edits the game's data files directly.

```bash
tools/campaign-editor/Campaign Editor.bat
```

or, with a game directory somewhere other than `C:\FreeFalcon6`:

```bash
python tools/campaign-editor/server.py --gamedir D:\Games\FreeFalcon6
```

Python 3.8+ and nothing else. The server binds to `127.0.0.1` only. **numpy is
optional** and buys one thing: the terrain base layer. Everything else — the
tables, the campaign files, the units, the objectives, the kneeboard map — is
pure standard library.

## How it is arranged

You open a campaign file first, the way Mission Commander does, and every tab
after that is a view of *that file*. The one exception is **Database**, which
edits the theater's shared `CampaignDB` — so a weapon you retune there changes
every campaign in the theater, and the UI says so on each row.

| Tab | What it edits |
| --- | --- |
| Theater | The `.tdf`, and cloning it into a new theater |
| Map | Units and objectives on the kneeboard map |
| Campaign | Scenario and UI names, clock, day, tempo, starting ratios |
| Teams | The eight teams' names, flags, colours and mottos |
| Units | The same units as the map, as a filterable list |
| Objectives | Airbases, cities, bridges, factories, SAM sites … |
| Squadrons | The selectable squadron list (read-only) |
| Victory | How the campaign ends: its trigger script, plus the TE clock |
| File contents | The `.cam`'s members, and duplicate/delete for the file |
| Database | The nineteen `CampaignDB` tables, grouped |

## The map

Two base layers, switched in the toolbar:

- **Terrain** — the theater's actual ground imagery, served as a tile pyramid
  and rendered to whatever zoom you are at. Roads, rivers, fields, coastline:
  the ground the sim flies over, which is what you want when placing units.
- **Kneeboard** — the campaign's own `Kneemap.gif`, the flat 1024x1024 picture.

Serving those tiles is the one place numpy is needed. Opening the map starts a
one-time parallel decode of the ~1500 ground textures the theater uses; after
that a tile is a cache lookup and a PNG encode, a millisecond or two, and the
browser caches what it fetched on top. The tile endpoints are served by a
threaded server, so a screenful of tiles renders on several cores at once
instead of queueing behind one another.

And on top of either, the airbase **TACAN** overlay: the station name and its
channel, drawn the way the game draws it.

The campaign's own `Kneemap.gif` with everything on it, drawn with **the
game's own icons** — the NATO symbols the in-game campaign map uses, in the
right team colour, read straight out of the UI art. All ~2600 objectives are
there too. Pan, zoom, toggle any team, unit kind or objective layer, click
anything to inspect it. **Game icons** in the toolbar switches back to plain
geometric markers if you prefer them.

The cursor is a gunsight reticle with its hotspot in the open centre, not a
hand: the point of this map is putting units on exact kilometre squares, and a
hand cursor covers the pixel you are aiming at. It turns blue with a centre pip
when you are armed to place a unit.

- **Shift-drag** a unit to move it.
- **Right-click** anywhere for a menu: place a unit here, or — over a unit —
  inspect, move, duplicate or delete it.
- **Place unit…** opens a picker filtered by kind, with a search that matches
  the vehicles a unit is made of. That matters because the class table has five
  different rows all called "AAA"; the picker tells them apart by showing
  `KS-19, ZU-23, S-60` against `K-30 BIHO, K-200 AD`.

Dense layers (terrain, infrastructure) fade out when zoomed out so the map
stays readable; they come back as you zoom in. The layer list folds away into
a corner chip.

### How a campaign actually ends

This surprised me, so it is worth stating plainly: **a `.cam` carries no
victory condition.** `EndgameResult` in the header only records which outcome
fired. What decides it is a separate plain-text script, `<scenario>.tri`,
sitting next to the campaign and evaluated on every campaign tick by
`ReadScriptedTriggerFile` (`src/campaign/campupd/cmpevent.cpp`). A
`#END_GAME <n>` in that script posts `FM_CAMPAIGN_OVER`, and outside a
tactical engagement that is the only thing in the engine that ever writes
`EndgameResult`. A campaign whose script has no `#END_GAME` cannot end.

So yes, it is functional, and every shipped campaign has four ways out: an
allied win, an OPFOR win, a stalemate timer and a day limit. The Victory tab
renders them in English with the objective ids resolved to real places, lists
every objective the script watches (click one to jump the map to it), and
shows the whole script.

Two details that are easy to invert:

- `#IF_CONTROLLED <team> A|O <campIds...>` — `A` means the team must hold
  **all** of the listed objectives, anything else means **any one** of them.
- `#IF_CAMPAIGN_DAY G <n>` — `G` is `>=`, not `>`, so `G 14` fires on day 14.

The ids are **campaign ids**, the same `campId` the objective stream carries.
A few of the shipped front-line events name ids that no objective in that
campaign has; the evaluator skips a missing id, so it reads as satisfied
inside an `A` list and can never fire inside an `O` list. `selftest.py`
reports those and fails only when one appears in an endgame condition, where
it would make that victory unreachable.

### Where the terrain comes from

Theater.map says LOD 0 is 256x256 blocks of 16x16 posts — 4096x4096 posts at
820 ft each — and one ground tile covers 4x4 of them. That is 3280 ft, one
kilometre. So **there is exactly one ground tile per campaign grid kilometre**,
and the theater is 1024x1024 km: the same grid the units and objectives use,
and the same 1024x1024 the kneemap is drawn at. A campaign coordinate is a
tile index; nothing needs rescaling.

At full resolution that is 262144 x 262144 pixels, so it is served as a
pyramid of 256x256 tiles, zoom 0 (the whole theater in one tile) to 10 (one
km per tile, native DXT1 resolution). Low zooms come from a 4 px/km overview
built once and cached under the system temp directory; high zooms are
assembled on demand from per-tile mipmaps. No tile takes more than about
0.2 s, and the browser caches them for a day.

Orientation was settled against data already known to be right, not assumed:
of the 913 ground battalions in korea2012's `save0.cam`, indexing the texID
grid as `[y][x]` puts 100% of them on land and 82.5% of the naval task forces
on water. Every other reflection of the grid scores far worse.

This is the one part of the editor that needs **numpy**. Without it the
terrain layer simply is not offered and the kneeboard map still works.

### Where the TACANs come from

`stations.dat` in the campaign directory, read by `TacanList::TacanList`
(`src/sim/navaids/tacan.cpp`):

```
stationId channel band callsign range tactype ilsfreq
764 042 X 18 25 1 111.7          # Chongju, channel 042X
```

`stationId` is the objective's **campaign id**, which is what ties a station to
a place on the map. In every shipped theater all 85 stations match an
objective and all 85 of those are airbases. Selecting an airbase shows its
channel, range and ILS frequency in the detail pane.

### Where the icons come from

Each unit and objective type carries an `IconIndex`, and following it takes
four files:

1. `art/main/imageids.id` — plain text, `NAME <tab> id`. `IconIndex` 10008 is
   `ICON_INFANTRY`.
2. `art/resource/imagerc.irc` — which file holds a colour's set, e.g.
   `RED_TEAM_ICONS "art\resource\reddark"`.
3. `<base>.idx` — an int32 size and version, then 60-byte `ImageHeader`
   records: type, `char ID[32]`, flags, centre x/y, w, h, and offsets.
4. `<base>.rsc` — its own 8-byte header, then 8-bit indexed pixels followed by
   256 little-endian RGB555 palette words. `0x7C1F`, magenta, is the colour key.

That 8-byte `.rsc` header is the whole trick: the offsets are relative to the
data, not the file, and reading from byte zero gives an all-black palette that
still decodes without complaint.

Ground, naval and objective symbols live in `<colour>dark`; aircraft
silhouettes live in the directional `<colour>air_*` sets. The editor merges the
two into one PNG atlas per team colour and blits from it. A team's colour is
the `team_colour` byte in the campaign header, mapped through
`TeamColorIconIDs` (`src/ui/src/common/teamdata.cpp`) onto white, green, blue,
brown, orange, yellow, red, grey in that order.

Placed units are built field by field from the engine's `Save()` chain rather
than copied from a neighbour, so they carry no inherited parent formation,
objective assignment or stale waypoints. They get a free VU id and campaign id,
a roster computed from the unit table the way `BuildElements()` computes it,
and whatever orders you pick. Battalions, brigades and naval task forces can be
placed; flights and packages cannot, because the campaign AI creates and
destroys those as it plans and a hand-made one would only confuse it.

## Theaters and new campaigns

**New theater** clones an existing one: copies the campaign directory
(including its own `CampaignDB`, wherever the source keeps it — the Israel
theaters hold theirs in `objectdir`), writes a `.tdf` that keeps the source
file's comment layout, and registers it in `theater.lst`. Terrain, objects and
misc textures keep pointing at the source theater's folders, so a Korea 1980s
variant costs about 9 MB rather than a whole new map. The three shipped
theaters cannot be deleted from the UI.

**New campaign** starts from an existing `.cam` and renames it. That is not a
shortcut — a campaign is only partly authored data. The objective graph, the
terrain links, the team records and the weather all have to agree with each
other, and the engine will not rebuild them from nothing. Starting from one
that already works and changing it is the only honest way to get a new
campaign, and everything in it is then editable here.

## Editing safely

Nothing is written until you press **Save to disk**, and only files you
actually changed are written — and within a file, only the members you
changed. Move one unit and the `.uni` member is rewritten while the objective
list, the weather and the pilot roster are copied through byte for byte. The
first write in a session copies the originals to
`<campaign dir>/_editor-backup/<timestamp>/`. **Discard** throws away every
pending change and re-reads from disk.

Rows and records you have not touched come back out byte for byte. That is not
incidental: these files carry compiler padding and leftover bytes after the NUL
in fixed-size name arrays, so every writer here patches in place rather than
rebuilding from parsed values.

## Worked example: Korea 1980s

1. Select **Korea 2012 Theater**, then **Clone**. Name it `Korea 1980s`.
2. Pick the clone in the left rail, then open `save0.cam`.
3. **Database → Weapons** — find `AIM-7E`, `AIM-9P`, and drop `AIM-120`'s
   `Rariety` to 0 so the planners stop reaching for it.
4. **Database → Vehicles** — find `F-16A`, and set the hardpoint `Weapon`
   slots. A slot whose `Weapons` count is 255 is a weapon *list* index
   (`FALCON4.WLD`); otherwise it is a weapon row (`FALCON4.WCD`). The detail
   pane names whichever it resolves to.
5. **Database → Units** — raise `NumElements` on the infantry rows for a denser
   1980s order of battle.
6. **Map** — right-click to drop extra infantry along the DMZ, shift-drag
   existing units, re-side anything you want.
7. **Save to disk**, then pick the theater in the game.

## Layout

```
ffcamp/
  lzss.py       the engine's blocked LZSS, both directions
  records.py    on-disk layouts for the CampaignDB tables
  campdb.py     reads and writes a CampaignDB directory
  campfile.py   the .cam/.tac container and the .cmp campaign header
  entities.py   the .uni unit stream: decode, patch, build, delete
  objectives.py the .obj objective stream
  names.py      the place-name table (DEFAULT.idx / .wch)
  tacan.py      airbase TACAN stations (stations.dat)
  triggers.py   the .tri campaign script: parse, and render in English
  terrain.py    the ground-imagery tile pyramid (needs numpy)
  uiart.py      the game's icon art: imageids.id, .idx/.rsc, PNG atlases
  theater.py    theater.lst, .tdf files, and cloning
  workspace.py  one editing session: dirty tracking, backups, saving
server.py       stdlib HTTP server and JSON API
web/            the page: index.html, style.css, app.js, map.js
selftest.py     checks every layout against the shipped data files
```

## selftest.py

```bash
python tools/campaign-editor/selftest.py
```

Reads every CampaignDB table and every `.cam`/`.tac` in the install, writes
each back in memory, and requires the bytes to be identical. It decodes every
unit and objective stream, re-encodes it, and checks that patching one record's
position changes two bytes and disturbs no other record. It also builds a
battalion from scratch, appends it, reads it back, and removes it again —
requiring the stream to come back byte-identical. It decodes icons out of the
UI art and checks they have both transparent and coloured pixels, that the
atlas packs every frame inside the sheet, and that 95%+ of the unit and
objective rows resolve to an icon that exists. It checks the terrain geometry,
renders a tile at five zoom levels and times them, and confirms every TACAN
station lands on an airbase. It parses every trigger script and checks each
campaign has a reachable way to end. A layout that is one byte off still
"parses"; it just shifts every field silently. On a stock install it is
1582 checks.

## Formats, and where they came from

Everything here is transcribed from the engine, not guessed.

| What | Where |
| --- | --- |
| Class table framing and the x64 `dataPtr` note | `src/falclib/classtbl.cpp` |
| Per-type table loaders and the `g_bFFDBC` framing | `src/falclib/entity.cpp` |
| Table record layouts | `src/falclib/include/entity.h` |
| `.cam` container | `StartReadCampFile`, `src/campaign/campupd/campaign.cpp` |
| Campaign header fields and version gates | `CampaignClass::Decode`, `src/campaign/campupd/cmpclass.cpp` |
| Unit stream dispatch | `NewUnit`, `src/campaign/camplib/unit.cpp` |
| Unit subclass fields | `src/campaign/camptask/{battalio,brigade,squadron,flight,package,navunit,gndunit}.cpp` |
| Objective stream | `LoadBaseObjectives` and `ObjectiveClass`, `src/campaign/camplib/objectiv.cpp` |
| Place names | `src/campaign/camplib/name.cpp` |
| Waypoints, and the `haves` bits | `src/campaign/camplib/campwp.cpp` |
| LZSS | `src/utils/lzss.cpp` |
| Theater definitions | `src/falclib/theaterdef.cpp` |
| Grid coordinates | `ConvertGridToSim`, `src/campaign/camplib/find.cpp` |
| Terrain posts, blocks and LODs | `tools/terrain/tilesurvey.py`, and Theater.map |
| TACAN stations | `TacanList::TacanList`, `src/sim/navaids/tacan.cpp` |
| Campaign triggers and `#END_GAME` | `src/campaign/campupd/cmpevent.cpp` |
| Icon resource format | `C_Resmgr::LoadIndex` / `LoadData`, `src/ui95/cresmgr.cpp` |
| ImageHeader layout | `src/ui95/imagersc.h` |
| Team colour to icon set | `TeamColorIconIDs`, `src/ui/src/common/teamdata.cpp` |
| Roster packing | `UnitClass::BuildElements`, `src/campaign/camplib/unit.cpp` |
| Id ranges for new units | `src/campaign/camplib/campbase.cpp` |

Three details that cost time and are easy to get wrong again:

- `WP_HAVE_DEPTIME` is `0x01` and `WP_HAVE_TARGET` is `0x02`, not the other way
  round. Swapping them still decodes squadrons and battalions correctly, and
  desyncs the moment a flight with a target waypoint appears.
- `DiskUIEventNode` is 20 bytes, not 18: it is `#pragma pack(push, 4)`, so there
  are two bytes of padding after `team`.
- An objective's feature-status block always advances the stream by the `size`
  byte that precedes it, even when the class table now says it should be a
  different length — the reader takes the smaller of the two and skips the rest.

## Not covered

- Objectives can be moved, re-sided and re-prioritised, but not created or
  deleted. A new objective needs entries in the feature tables and a place in
  the neighbour-link graph, which is a different and much larger problem.
- Campaign data versions below 52. Every shipped campaign is 73 or 99.
- Squadron rosters and pilot records are decoded but not exposed for editing.
- Adding a brand new entity to the class table. You can duplicate and retune
  existing rows, but a new row needs a visual model in the object database.
- A mid-campaign save (`Auto Save.cam`) carries objective *deltas* rather than a
  full objective list. The editor borrows the base scenario's objectives so the
  map is still populated, and marks them read-only.

## Editing the victory conditions

A campaign has no victory field. What ends it is the `.tri` script beside it,
and the only thing in the engine that writes `EndgameResult` outside a
tactical engagement is a `#END_GAME` in that script.

The Victory tab shows every `#END_GAME` with the guard chain that reaches it,
as controls rather than text: the team, whether they must hold **all** or
**any** of the listed places, the places themselves as removable chips, the
day limit, the stalemate hours and the result code. Each change rewrites one
line of the `.tri` and nothing else — the file keeps its comments, blank lines
and indentation, which are the only documentation these hand-written scripts
have.

Adding an objective id the campaign does not carry is refused. The engine
guards each lookup with `if (o and ...)`, so a missing id is skipped: harmless
inside an `A` list, where it reads as satisfied, and fatal inside an `O` list,
where the condition can never fire.

Underneath is the text the campaign-select page actually shows, read from
`<artdir>/art/Main/lcktxtrc.irc`. Nothing in the game checks it against the
script, and three of the twelve shipped campaigns disagree with their own
script, so the tab prints the sentence the script really implements and offers
to write it into the file. Be aware when a theater inherits another's `artdir`
— Korea 1980s uses `artKorea2012` — because then the two share one file and
one set of three blurbs; the tab says so when that is the case.

Both writers go through the same backup-then-write path as every other edit,
so the originals are kept alongside the rest of the session's changes.
