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
| Map | Units and objectives on the kneeboard map — the main editing surface |
| Campaign | Scenario and UI names, clock, day, tempo, starting ratios |
| Teams | The eight teams' names, flags, colours and mottos |
| Units | The same units as a filterable list, with squadron names, aircraft, patches, home bases and composition |
| Objectives | Airbases, cities, bridges, factories, SAM sites …, with supply and per-feature damage |
| Squadrons | The selectable squadron list, with names and aircraft (read-only) |
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

- **Ctrl-F** searches the map by name, type, team, aircraft, home base or
  `#camp id`. All the words have to match. Every match gets a yellow ring and
  the rest fade. Matches show even on layers that are switched off. **Enter**
  and **Shift-Enter** step through them, centring the map and opening each
  one, and **Esc** closes the search.
- **hidden reinforcements** (Layers panel, under Units; off by default) shows the units the game
  keeps off the map: those flagged inactive in the save (`U_INACTIVE`), waiting for their team's
  reinforcement counter. They are drawn dimmed with a dashed ring and `+Nh`, the campaign hour they
  arrive (the counter rises one point per hour). The hover tip repeats it. Korea `save0` has 127.
- **Shift-drag** a unit to move it.
- **Right-click** anywhere for a menu. Over a unit: inspect, move, duplicate,
  delete, plus the commands the campaign screen puts there — orders, supply,
  morale and losses, resupply or empty a squadron's racks, and a team change.
  Over an objective: inspect, repair or destroy every feature, resupply it, or
  re-side it.
- **Click** anything to edit it in the sidebar. A unit gets its condition
  (supply, morale, fatigue, losses, fuel), orders, composition, and — for a
  squadron — the pilot roster, the stores table and the aircraft's hardpoints.
  An objective gets supply, fuel, losses, priority, owner and a damage control
  per feature. Rings on the map mark the places the trigger script watches;
  the sidebar's idle state lists them (see below).
- **Place unit…** opens a picker filtered by kind, with a search that matches
  the vehicles a unit is made of. That matters because the class table has five
  different rows all called "AAA"; the picker tells them apart by showing
  `KS-19, ZU-23, S-60` against `K-30 BIHO, K-200 AD`.

Dense layers (terrain, infrastructure) fade out when zoomed out so the map
stays readable; they come back as you zoom in. The layer list folds away into
a corner chip. Its **Teams** section counts what each side holds — units,
objectives and airbases, biggest holder first — which is the quickest way to
see who the enemy in a scenario actually is; Saudi Arabia owning objectives
with no units on the map, or the U.S. owning none at all, both show up there.
The yellow cross with a circle is the campaign's **bullseye**, the reference
point for relative bearings; it is `BullseyeX`/`BullseyeY` in the campaign
header, which the Campaign tab edits.

### Objectives the script watches

A campaign's victory conditions are the trigger script, and the only thing in
it that names a place is `#IF_CONTROLLED` — "team 2 controls any of 680, 260,
404". The map draws a ring around every place such a condition names:

- **solid ring** — the team must hold it;
- **dashed ring** — the team must *not* hold it, from a condition in an `#ELSE`
  branch, which is how half of the shipped conditions are written;
- **double ring** — the condition is on the path to an `#END_GAME`, so it ends
  the campaign rather than firing a front-line event.

The ring's colour is the team the condition names. Conditions that only feed
events are off by default — `include event conditions` in the legend or the
sidebar turns them on — and in a campaign whose endgames name no place at all
(Israeli `save0` and Israel Classic `save0` end on events and a timer) every
control condition is shown instead, because otherwise the layer would be empty.

The sidebar's idle state lists the conditions themselves, grouped by team and
polarity, with the places as buttons that centre the map. `A` means the team
must hold **all** of the list and `O` means **any one** of it; that letter is
the difference between an easy campaign and a nearly impossible one, and it is
in the list rather than the ring because it is a property of the set, not of
the place. Ids the script names that no objective carries are reported at the
bottom of the list — harmless inside an `A` list, fatal inside an `O` one.

### Squadrons, and what the lists show

A squadron's unit table row is named for its *role* — "Fighter", "Bomber" —
which is the same for all of them, so the editor composes the real name the
way `UnitClass::GetName` does: `<ordinal> <class name> Squadron`, from the
unit's `nameId` (a number: 120 is the 120th) and its class-table row. The
Units tab shows that name, the aircraft, the patch, the home airbase (the
objective its `airbase_id` points at, or the header record's airbase label),
and what the unit is made of, so a squadron can be told from its neighbours
without opening it.

The **Squadrons** tab is the campaign header's roster of flyable squadrons —
a different list, joined to the units by VU_ID. Its rows carry the same name
and aircraft, and clicking one opens the matching unit.

### Sorting and filtering

Every list — Units, Objectives, Squadrons, and each Database table — sorts by
clicking a column header, and has a filter box under each header. A text
filter is a substring; a numeric one also takes a comparison, so `> 40`,
`<= 10` and `= 3` work in the Range or Strength columns. The Database tables
sort and filter on the server rather than in the browser, because they page:
a client-side sort would only reorder the 400 rows that happen to be loaded.
Sorting and filtering do not disturb edits — an edited row keeps its marker.

### Repair and destroy

An objective's damage is two bits per feature in the `.obj` record, four
features to a byte (`ObjectiveClass::GetFeatureStatus`): 0 normal, 1 repaired,
2 damaged, 3 destroyed. The engine recomputes the objective's condition from
those bits when it loads the campaign, so they are the real repair/destroy
switch, and the sidebar edits them directly. Destroying or repairing a feature
also walks its `FEAT_PREV_CRIT` / `FEAT_NEXT_CRIT` neighbours, the way
`SetFeatureStatus` does, so a runway and the taxiway it depends on never
disagree. Supply, fuel and losses are plain bytes in the same record and are
editable next to them.

The block on disk is not always the size the class table predicts — the reader
takes the smaller of the two, and Israel Classic ships both kinds — so the
editor only offers the features that both agree exist.

### Aircraft loadouts

A hardpoint's `Weapon` is a **row** in `FALCON4.WCD`, or in `FALCON4.WLD` when
`Weapons[hp]` is 255 — the marker `LoadoutWeapons` checks. The two arrays are
therefore one edit, and the editor's hardpoint control writes them together: a
searchable picker of every weapon and weapon list, and the count. Picking a
list sets the 255 for you, because writing the row without it would make the
engine look the same number up in the other table. It is available in
**Database → Vehicles** and on a squadron's page, where it says how many unit
types share the aeroplane before you change them.

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

A save has no script of its own, and the engine never looks for one named
after the file it loaded: `CheckTriggers(TheCampaign.Scenario)` reads
`<Scenario>.tri`, and the `Scenario` field in a save's header is the scenario
it was started from. So an autosave or a user save plays the ending of its
scenario, and editing that script changes it for every save started from it.
The Victory tab names the file it opened and says when it was borrowed. The
same field is where the save's base objective list comes from — the save
itself carries only deltas — so it is not a free choice.

The header wins even when the file ships its own script: Israel Classic's
`save0.cam` names `save2`, and its `save2.cam` names `save0`, so each plays the
other's ending and the `save0.tri`/`save2.tri` beside them are read by the
*other* file. The Victory tab says which file it actually opened, and the
Campaign tab validates a `Scenario` change against the objective list it has to
come from.

To change a save's ending **without** touching its scenario, the Victory tab
offers **Give this save its own ending**: it copies the scenario (objective
list included), writes a script for the copy, and points the save's `Scenario`
field at it. Units, teams, pilots and the objective deltas still come from the
save; only the base list and the script move, and the deltas still apply
because the copy's objectives are the ones they were made against. The field
is validated on the Campaign tab too: a save cannot be pointed at a scenario
that carries no objective list.

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
campaign, and everything in it is then editable here. Two things outside the
`.cam` come across as well: the trigger script is copied to `<name>.tri`, so
the copy has an ending you can then edit on the Victory tab, and starting from
a save also copies the scenario's objective list into the copy — the engine
reads a save's base objectives from its header's `Scenario`, which the copy no
longer points at.

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
position changes two bytes and disturbs no other record. It patches every
editable condition field — a unit's supply, morale, fatigue, losses, orders and
fuel, an objective's supply, fuel, losses and priority, and the two-bit feature
damage states — and requires each to move only the bytes it owns and survive a
re-encode. It checks that every flyable squadron record joins to a unit and a
class row. It also builds a battalion from scratch, appends it, reads it back,
and removes it again — requiring the stream to come back byte-identical. It
decodes icons out of the UI art and checks they have both transparent and
coloured pixels, that the atlas packs every frame inside the sheet, and that
95%+ of the unit and objective rows resolve to an icon that exists. It resolves
every theater's movie table — all seventeen ids, and that the Israel theaters
really do point at their own avi files — and checks the script annotations:
which actions this build ignores, and which events are written but never
tested. It checks the terrain geometry, renders a tile at five zoom levels and
times them, and confirms every TACAN station lands on an airbase. It parses
every trigger script and checks each campaign has a reachable way to end. A
layout that is one byte off still "parses"; it just shifts every field
silently. On a stock install it is well over eight thousand checks.

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
- `#SET_TEMPO` and `#CHANGE_PRIORITIES` are **dead in this build**: their
  handlers in `ReadScriptedTriggerFile` are commented out, so the shipped
  scripts' `SET_TEMPO 255` and four `CHANGE_PRIORITIES` lines do nothing. The
  listing says so on the line.

## Not covered

- Objectives can be moved, re-sided, re-prioritised, resupplied and have their
  features damaged or repaired, but not created or deleted. A new objective
  needs entries in the feature tables and a place in the neighbour-link graph,
  which is a different and much larger problem.
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

The full listing under the endgame cards says what the tokens do not:

- **Movies by name.** `#PLAY_MOVIE 104` is a number until `MOVIES.ID` (name →
  id) and `movies.irc` (name → file and title) name it, so the line reads
  *"DPRK PUSHED BACK TO DMZ · movies/E3.avi"*. Both files sit under the
  theater's art directory, and every Israel theater overrides them with its own
  avi files, so the path shown is the one that theater would play.
- **Actions this build ignores.** `#SET_TEMPO` and `#CHANGE_PRIORITIES` have
  their handlers commented out in `cmpevent.cpp`; the shipped Korea scripts use
  both, and the line says "no effect in this build".
- **Events nothing tests.** `#IF_EVENT_PLAYED` is the only reader of an event
  flag, so an event that is set and cleared but never tested is a latch with no
  reader — its only effect is the movie beside it. Korea's `save0` events 6 and
  8 are exactly that, and the listing flags them. Events fired immediately
  before an `#END_GAME` are exempt, since nothing is meant to test those.

Underneath is the text the campaign-select page actually shows, read from
`<artdir>/art/Main/lcktxtrc.irc`. Nothing in the game checks it against the
script, and three of the twelve shipped campaigns disagree with their own
script, so the tab prints the sentence the script really implements and offers
to write it into the file. Be aware when a theater inherits another's `artdir`
— Korea 1980s uses `artKorea2012` — because then the two share one file and
one set of three blurbs; the tab says so when that is the case.

Both writers go through the same backup-then-write path as every other edit,
so the originals are kept alongside the rest of the session's changes.

## Weather

The Weather tab edits the `.wth` member: the prevailing condition, wind, temperature
and cloud bases, and the **fronts** the game moves across the theater. Since 2026 the
game has had cold and warm fronts, squall lines, storm cells, clearings, and drifting
random patches. The tab draws them over the map and previews up to 48 h ahead with the
same model the game runs (`ffcamp/weather.py`, a port of
`src/graphics/weather/weatherfronts.cpp`; selftest pins the two together). Pick a front
and click the map to move it; at a previewed hour, the click says where it should be
*then*.

A campaign (`.cam`) reads only the fronts block from this member, and with it the
prevailing condition, wind and temperature. Its cloud bases come from the condition. A
file with no fronts block leaves the game to seed its own. See `WEATHER-FRONTS.md` at
the repo root for the model and the format.

## Squadron stores

**Why the loadout screen says OUT.** The loadout screen lists every weapon on the
aircraft's hardpoints, then colours each one by the squadron's stock over the stores
maximum in `FALCON4.SSD`. A weapon with a maximum of 0 is never issued (resupply skips
it), so it reads OUT for the whole war. The Stores panel now lists those weapons,
tagged **OUT · locked**, and **Unlock N locked weapons** gives each one a maximum and
fills this squadron. The maximum is class data: it changes every squadron class that
shares the stores row, in every campaign of the theater.

Select a squadron on the map and its detail panel gets a **Stores** table: the
weapons it can arm with, how many supply points it holds, and the ceiling.

Two different tables meet here, and conflating them is the easy mistake:

| | Lives in | Scope |
| --- | --- | --- |
| **Count** | the squadron entity in the `.cam` | this squadron only |
| **Max** | one row of `FALCON4.SSD`, picked by the unit class's `SpecialIndex` | every squadron class sharing that row |

`SquadronClass::GetAvailableStores` divides one by the other:

```c
if (i == infiniteAA || i == infiniteAG || i == infiniteGun) return 4;
return (GetUnitStores(i) * 4) / SquadronStoresDataTable[SpecialIndex].Stores[i];
```

so availability is 0..4 and that is exactly what the bar in the table shows —
the same number the loadout screen colours by.

**A max of 0 is the real enable/disable.** The weapon cannot be armed whatever
the count says, which is why the checkbox writes the maximum rather than the
count. The panel names how many squadron classes share the row before you touch
it. Emptying the rack instead (count 0) leaves the weapon selectable but
unavailable until resupply.

Rows whose maximum is 0 are hidden unless **Show all** is ticked. There are a
lot of them: a squadron's 600-slot array is not cleared when it is created, so
most slots hold leftover stock for weapons the class could never carry. That
stock is inert — the engine only ever reads slots that a loadout names.

Two details worth not re-deriving:

- The stores index is a **row in `FALCON4.WCD`**, not a class-table index.
  `SetUnitStores` is called with the weapon id straight out of a loadout and
  `WeaponDataTable` is loaded row-for-row from that file.
- The array is 600 entries (`MAXIMUM_WEAPTYPES`) while Korea's `.WCD` has 797
  rows, and neither `GetUnitStores` nor `SetUnitStores` bounds-checks. The
  editor refuses an index past the end of the array rather than writing a file
  that would corrupt the squadron's neighbours in memory. `camplib.h` has a
  commented-out `#define MAXIMUM_WEAPTYPES 1200` right below it, so this was
  known about.

Edits are patched a byte at a time into the decompressed unit stream, the same
discipline as every other unit edit; the self-test asserts that changing one
weapon moves exactly one byte and that it survives a re-encode.

## Squadrons: two lists, not one

There are two squadron lists in a campaign and they answer different questions.

**The Squadrons tab** is the campaign header's roster of *flyable* squadrons --
one 68-byte record each, carrying the patch, the airbase label and the strength
the campaign-select screen shows. It decides which squadrons the player may
fly. In Korea's `save0` there are 112 of them.

**The squadron units** are the entities in the unit stream: 158 of them in the
same campaign, with the stores, pilots, kills and airbase assignment. Every
flyable record matches a unit; not every unit is flyable.

The only key the two share is the VU_ID, and it matches cleanly -- all 112 of
Korea's flyable records resolve to a unit -- so the Squadrons tab links each row
through to its unit.

### The squadron page

Selecting a squadron unit (Units tab, or the map) gives the page Mission
Commander shows: patch, name, aircraft, airbase, specialty, kill tallies,
missions, losses, the pilot roster and the stores table.

Three of those are joins rather than fields:

- **Name.** `UnitClass::GetName` composes `"<ordinal> <class name> <size>"`, so
  a unit's `nameId` is a **number** -- 120 is "120th" -- and not an index into
  the place names. Resolving it the way an objective's `nameId` resolves gives a
  town in Korea instead of a squadron, which is exactly the wrong turn to take.
- **Aircraft** is the unit class's first `VehicleType`, the same join the
  composition list does.
- **Patch** is an index into `art/resource/patches`. Its file order is the order
  of that directory's `imageids.id` *and* of the engine's `SquadronMatchIDs`
  table -- all three agree, so the index resolves directly. The 35th Fighter
  Squadron carrying patch 10, `_35TH_FS_`, is the check that it is right.

Pilot **names** are not here: `PilotInfoClass` holds only usage, voice and
photo ids, and the names come from the callsign table, which the editor does not
read yet. The roster shows the ids the game looks them up by, with skill,
rating, status, kills and missions.

### The .idx files have two pixel formats

Worth knowing before reading any other UI95 resource. The low two bits of an
entry's `flags` select the format:

| `flags & 3` | Layout |
| --- | --- |
| 1 | 8-bit indices into a per-entry RGB555 palette at `pal`, `palN` entries |
| 2 | RGB555 direct, two bytes a pixel; `palN` is 0 and `pal` is just the end of the image |

Reading a format-2 entry as paletted gives an empty palette and a **fully
transparent** image rather than an error. About a third of the squadron patches
are direct colour, so a third of them silently came out blank. The unit icons
are all paletted, which is why this never showed up until the patches were read.
