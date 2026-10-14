# campsim tools

Headless run of the **real campaign AI** (`src/tools/campsim`, builds with the solution, x64 only),
plus the tools to record, replay and compare runs. Everything here is for campaign-AI research.

**Campaign Lab (GUI):** `python serve.py`, then open http://localhost:8770/. Pick a save, days,
seeds and settings, press *Start runs*, watch the progress bars, tick runs to overlay their
charts (objectives, DPRK supply against the China/Russia thresholds, aircraft, ground vehicles)
and see which scripted events fired when. *replay* opens the map viewer on that run. Runs from
the command line show up there too. Runs use `gamework/` as the game dir when it exists.

```
python serve.py                                                 # Campaign Lab
python run_batch.py --days 10 --seeds 1 2 3 4 --jobs 4          # baseline, ~20 min with 4 in parallel
python run_batch.py --days 3 --seeds 41 --tag spd3 --set speed=3  # one experiment
python serve.py                                                 # http://localhost:8770/  -> viewer.html
python compare.py > compare_report.md                           # order of battle: FF6 vs vanilla vs BMS 4.37
python make_backdrop.py                                         # terrain image for the viewer
```

`campsim.exe <game> <save> <days> <seed> [timeline.jsonl] [frame_minutes] [knob=value ...]`

## Knobs (extra `key=value` args; `--set` in run_batch)

| knob | effect |
|---|---|
| `speed=F` | scale every ground unit class's `MovementSpeed` |
| `cost=F` | scale the foot/wheeled/tracked terrain cost table (`path.cpp`) |
| `strip=T:P` | remove P% of the vehicles of every battalion of team T (DPRK = 6) |
| `init=T:V` | set team T's initiative at the start |
| `stripair=T:P` | remove P% of the aircraft of every squadron of team T |
| `airtempo=T:P` | each tick, the ATM of team T may task at least P% of its pending mission requests |
| `abheal=T` | team T's airbases and airstrips are fully repaired every 10 ticks |
| `usecfg=1` / `cfgfile=<path>` | load `FFViper.cfg` (or only some keys) as the game does; **use this for fair runs** |
| `simtogrid=0` | restore the old (buggy) sim->grid rounding |
| `newgame=0/1` | run the game's new-campaign setup (`AdjustCampaignOptions`: resupply every unit to the start level, force-ratio company chop, enemy experience, initial events). **Default on when no event has fired yet** (save0 and every variant); off reproduces the old harness, whose DPRK supply sat at ~37% and fired China at minute 1 |
| `ratio=G:A:D:N` | force ratios from the campaign setup screen, 0-4 (default 2:2:2:2) |
| `exp=A:G` | enemy air / ground experience, 0 green - 4 ace (default 0:0, the player-options default) |
| `crashtest=N` | fault on purpose at tick N, to check the crash logger |
| `tri=NAME` | read triggers from `campaign/SAVE/NAME.tri`. **Needed for trigger variants**: the engine picks the .tri by the scenario name stored *inside* the .cam, so a renamed save (`both.cam`) still reads `save0.tri`. Example: `both.tri` = save0 with the win needing Pyongyang AND Wonsan (`#IF_CONTROLLED 2 A 680 260 404`) |
| `theater=<tdf>` / `simroot=<dir>` | run another theater; see "Other theaters" below |
| `replfix=0/1` | `g_bReplSplitFix` (default 1, see "Replacements" below); `0` = stock split |
| `relsquad=F` | override `RelSquadBonus` (Falcon4.AII, stock 4.0) for the run; `REPLSPLIT` log lines every 6 hours show each team's air and ground vehicle shortfall and the lambda used |
| `noairlift=1` | airlift landings add no supply, fuel or replacements (the flights still happen); `AIRLIFT` lines every 6 hours count landings and what they added |
| `airliftneed=0/1`, `airliftrunway=0/1` | `g_bAirliftNeed` / `g_bAirliftRunway` (default 1: airlift credit capped by the need around the destination, none at an airbase with no working runway; 0 = stock) |
| `stocks=0/1`, `storesproduce=0/1`, `storecap=PCT`, `storefill=PCT` | the regional-stocks experiment (`g_bSupplyStocks`, off by default): depots and army bases hold supply and fuel stock that battalions draw down; see `CAMPAIGN-SUPPLY-ENGINE.md` "Experiment: regional stocks"; `STORES` lines every 6 hours |
| `airliftlog=1` | one `AIRLIFTLAND` line per credited landing: squadron, home airbase, landing airbase, distance, flight size, runway status |
| `blow=TYPE:TEAM[:hold]` | destroy every feature of the team's objectives of that type at the start (`hold`: re-destroyed every tick, a bombing campaign that is never repaired). TYPE: `factory refinery power depot port armybase bridge road railroad airbase producers all`; TEAM: a number or `red` (teams 4, 5, 6: China and Russia fold into DPRK). See "Supply what-ifs" |
| `pak=ID:VALUE[:TEAM]` | the player's PAK slider for objective ID's PAK (`player_priority`; team defaults to 2). Air planning only. Pyongyang is one PAK, 260 (680 belongs to it) |

A crash now writes `CRASH at campaign min ...` with the fault address and a symbolised stack
(file:line) into the run's log; the Lab shows it as *crashed ⓘ* with the stack on hover.

## Other theaters (Israel, Korea 2012, ...)

Without a theater, campsim runs Korea from `<game>/campaign/SAVE`. Any theater in the game's `theater.lst`
runs with `--theater` (run_batch) or the `theater=` knob (campsim.exe):

```
python run_batch.py --theater list                                      # what theater.lst offers
python run_batch.py --theater israel_classic --save save1 --days 19 --set usecfg=1
python run_batch.py --theater israel2007 --save save2 --days 10 --seeds 1 2 3 4 --jobs 4
campsim.exe <game> save1 19 1 out.jsonl 60 theater=theaters/Israel_Classic/Israel_Classic.tdf simroot=<dir>
```

`--theater` takes the .tdf's file stem (`israel_classic`), its display name (`"Israel 2012"`) or a .tdf
path, any case. Run files are named `<save>-<theater stem>[-tag]-s<seed>.*`, and the summary prints every
team (Korea's Blue/DPRK totals do not apply elsewhere).

What `theater=<tdf>` does (`ApplyTheater` in `main.cpp`, the directory half of `TheaterList::SetNewTheater`):
the .tdf's `campaigndir` (saves, .tri, Falcon4.AII), `terraindir`, `objectdir` (class table: FALCON4.ct and
friends), `3ddatadir` and `misctexdir` replace the Korea paths. `simroot=<dir>` is a folder holding a
`sim/` tree (radar, aircraft data). ITO keeps its sim data only in `ZipsITO/Simdata.zip`, which campsim's
relative `sim/radar/...` opens do not find; run_batch unpacks it once into `runs/_simroot/ZipsITO` and
passes `simroot=` itself. Without it the log shows `radarDatFileTable ... entries=0` and
`[SIMLIB-OPEN-FAIL] req='sim/radar/radtypes.lst'`, so check that line on a new theater. The Korea-family
theaters (korea_2012, korea1980s, eurowar) share Korea's sim data and need only `theater=`.

**Campaign Lab (`serve.py`).** It still *starts* Korea runs only (any theater's runs from the command line show up in it).
The run table has one column group per team, whatever the theater (objectives held, ground and air left, start to end) and
sorts by run, started, state, days or end (click a header; the choice is remembered). Runs carry the campaign's own name as
the game's campaign screen shows it (`Korea - Tiger Spirit`, `Israel 2012 - 2007 Fictional War`: the theater `.tdf` name plus
`TXT_SCENARIO_1..3` from its `art/main/lcktxtrc.irc`; `run_batch.campaign_labels`), and the end column says what ended the war:
the `END_GAME` code with the `//` comment above that block in the `.tri` (`END 14  Arabs own Hermon, Bir Jif Jaffar & Baluza`).

**Replay viewer (`viewer.html`)** opens any run: team names and colours come from the run's own `meta.teams`, the backdrop from
the theater in the run's file name (`python make_backdrop.py <terrdata dir> backdrop-<stem>.png`; `backdrop-Israel.png` serves all
three Israel theaters; the PNGs are generated, not committed). Main teams come first (ROK, DPRK / Israel, Egypt).
- map: wheel zoom, drag pan, double-click fits; territory glow per owner, objectives by type, ground units as framed boxes with a
  strength bar, flights, movement trails and rings on fresh captures; each layer can be switched off; hover for details
- sidebar: a clock and slider (space plays, arrows step), the scripted events with the frame each first fired (click to jump), a
  per-team table at the slider position with the change since frame 1 (forces; posture; battalion morale and supply; supply and
  fuel levels, pools, producers, power; flights, packages, airbases; cumulative losses by cause), and **three charts**, each with its
  own metric picker, that jump the slider when clicked
- the sidebar is resizable (drag the bar next to the map, double-click resets) and your width and chart choices are remembered
- the banner and the event list show the end code with its label

**Saving a replay as an animation:** the viewer's "Save replay as animation" panel renders the map as framed on screen
(zoom, pan, layers) plus a title / clock / scores overlay, scaled to 1024-2560 px wide, and saves an animated **WebP**
(full colour, best quality per MB), a **GIF**, or a **WebM** video. WebP and GIF are encoded by `serve.py` with Pillow
(`pip install pillow`; the file lands in `runs/_export/` and downloads); WebM is recorded in the page and GIF falls back to
an in-page encoder if Pillow is missing. The frame step defaults to about 120 frames; use the from/to boxes to cut a clip.

The `tri=` knob and the .tri / .cam lookups use the theater's campaign dir. `gamework/` links `Theaters`,
`Zips` and `ZipsITO` to the install, so the Israel saves are the install's own files either way.

### The Israel theaters (FF6 ITO 2)

Three theaters with three campaigns each, nine in all. Two are modern fictional wars (Israel 2012's **2004** and **2012**, hidden
in the shipped game and rebuilt 2026-10-13, see below) plus the **2007** one.

| theater (stem) | save0 | save1 | save2 |
|---|---|---|---|
| Israeli (`Israeli`; folder `Israel`) | 1967 Six Days (RAM22 update) | 1973 Yom Kippur | 1982 Lebanon |
| Israel 2012 (`Israel2007`) | **2004 Fictional War** (rebuilt) | **2012 Fictional War** (rebuilt) | **2007 Fictional War** (F-16I, AH-64D, US, Saudi) |
| Israel Classic (`Israel_Classic`) | 1982 | 1973 | 1967 |

Classic's `save0.cam` names `save2` as its scenario and `save2.cam` names `save0`, and the engine reads `<Scenario>.tri`, so on
disk the two script files are swapped to match. The saves' contents do not always match their button names (Classic's "1967"
starts with Israel already holding the old city of Jerusalem); read a run's first frame, not the label.

Teams: 2 Israel, 6 Egypt; 3 Iraq, 4 Jordan, 5 Syria, 7 Saudi, 1 US (2007). `#CHANGE_RELATIONS a b 1`
(allied) **merges** team a into team b (`SetTeam`), so once Syria or Jordan joins, their ground counts as
team 6: write end conditions against team 6, not the joining country. Day limits after the retune: 1967 Israeli day 9, Classic
1967 day 8, 1973 day 10, 1982 day 9, 2004 / 2007 / 2012 day 9-10. `#IF_CAMPAIGN_DAY G N` fires roughly N-1 days into the war, so
run `--days 10`.

**The objective numbering differs per theater.** Israel_Classic uses the original IDs that the ITO
scripts and the win-conditions document (`Theaters/Israel/FF Israeli Docs/`) were written for. The
Israeli and Israel 2012 saves use a later, larger objective set but shipped the same scripts, so their
IDs pointed at other places: "Beirut" 617 was an Egyptian power station. Translate by map position (the
three share `Theaters/Israel/terrdata`).

**The shipped scripts were patched 2026-10-12** in the install (`Theaters/*/campaign/save?.tri`, each
original kept as `save?.tri.bak-orig`):

- end blocks missing `#ELSE` never ran;
- missing `#ENDIF`s nested every 1973 end under four "not yet taken" checks;
- offensive/defensive toggles tested their own event, so they never fired;
- "falls/regained" blocks never reset themselves, so their ±50 initiative shift repeated on every check;
- 1973/1982 IDs remapped in the newer theaters;
- the 2007 Syria win sat below `#ENDSCRIPT` (now: Syria takes Haifa);
- a trailing space after `#ENDIF` (the engine strips only CR / LF, so `#ENDIF ` is not an ENDIF and every later block nested
  under it) was removed from every script;
- Classic's save0 / save2 scripts were swapped on disk to match the Scenario names inside the `.cam` files;
- Israeli 1967's conquest win, commented out by its author, was restored;
- Israel 2012's old save0 / save1 (1967 and 1973 data under 2004 / 2012 titles, buttons commented out) no longer exist as such:
  see the rebuild below.

The scripts that did all of this are in `israel/` (its README lists them: `fix_tri.py`, `build_modern.py`, `retune.py`, `report.py`).

**Retuned 2026-10-13 toward a 5-10 day war** (installed scripts and saves keep `*.bak-pre-retune`; the untouched originals `*.bak-orig`):
Israel 2012's hidden `save0` / `save1` were rebuilt as the modern **2004** and **2012 Fictional War** campaigns (copies of the 2007 save with
era aircraft, own scripts, Arab ground cut about 28-30%; the menu buttons in `art/campaign/select/cs_load.scf` are uncommented).
The other seven campaigns got shorter day limits (1973 day 10, 1982 / 2007 day 9-10), win conditions limited to what the armies
actually reach, a minimum campaign day before an ending can fire (`#IF_CAMPAIGN_DAY G n` wrapped around the end block) and
some ground-vehicle cuts. Seeds 1-6 give: 2004 / 2012 two Arab wins to three Israeli holds; 2007 two holds, one Arab win;
1967 Israeli: Israel wins on day 6.8 every run (historical); 1973 Israeli: Arabs win a third of the runs (day 4.4), Israel holds the rest;
1982 Israeli: Israel wins about two in three (day 4); Classic 1967: Israel wins three of four (day 4.6-6.9) and the Arabs one;
Classic 1973: one result each way (day 4.6, 7.0 and a day-10 hold); Classic 1982: one Israeli win (day 4.6), two day-9 holds.
Two traps found on the way: `#IF_CONTROLLED ... A` counts an objective ID that does not exist in that save as held (a win list
can be true at day 0 if its IDs belong to another theater's numbering), and campaign saves may borrow their objective list from
another save, so read ownership from a run's own first frame, not the editor.

The engine also crashed on `#SET_PAK_PRIORITY` for an objective without PO data (`cmpevent.cpp`, a
DEBUG-only null check); that check now always runs.

The engine **re-reads the .tri on every trigger check**, so editing a script changes runs already in
progress. Stop them first.

## Supply what-ifs (Korea, Rolling Fire)

`blow=` plus `supply_whatif.py` measure how much the supply chain matters (the chain itself is described in
`CAMPAIGN-SUPPLY-ENGINE.md`). Result of eight scenarios x 2 seeds x 5 days (usecfg=1, noend=1, all AI fixes on):

- DPRK production capacity per day: 15,350 baseline; 7,710 with the factories gone; 2,170 with every producer gone; 0 with the
  power plants gone (every producer is multiplied by its nearest plant's status). The supply pool falls with it within an hour
  (about 250 to 30 without power); refineries held down cut the fuel pool by about 60% in 12 hours and about 95% in 24.
- The front does not notice inside five days: mean battalion supply about 93% at baseline and 89-92% in the worst case, 0-2
  battalions under 25%, no pattern in the Blue win time or the final objective counts. Units start near 100% and burn about a
  point a day; production (21,000 supply over the five days) runs about six times above what they draw (3,300).
- Repair is slow: power plants blown once were back to about 68% of baseline production after five days.
- A finding on the side: DPRK ground units received 3 replacement vehicles in five days (ROK ground: 4,446) in every scenario,
  although DPRK's replacement pool grew to about 4,800 unused.

### Airlift is a hidden income

About 600 airlift landings in five days add 3 x elements / 3 (= 12) of supply, fuel and replacements each to the team pools, from
nothing: the flights are base-to-base shuttles between two friendly airbases at least 40 km apart and the credit ignores aircraft,
need and runway state. It is about a quarter of DPRK supply and four fifths of its replacements, and the reason destroying
production never reached the front. Details and numbers: `CAMPAIGN-SUPPLY-ENGINE.md` ("Airlift: the hidden income"). With power
plants held down and `noairlift=1` DPRK battalion supply slid from 96% to 74% in five days and ground replacements fell to about 30.

### Supply hubs: FF6 vs Falcon 4.0 vs BMS 4.37

`python supply_hubs.py` counts the objectives that make or distribute supply per side, with their DataRate, for the three games (BMS objectives are decoded by anchoring, its records are longer than ffcamp knows). BMS 4.37's Korea has 825 objectives against 2,700 and no army bases: fewer hubs, not more. Bombing army bases, ports or depots each takes about 12% of DPRK production, the factories 50%; details in `CAMPAIGN-SUPPLY-ENGINE.md` ("Hubs, bombing and the network").

### Replacements: why DPRK ground units got none (fixed, `g_bReplSplitFix`)

`SupplyUnits` (`supply.cpp`) splits the replacement pool between squadrons and ground units: squadrons get `lambda`, ground units
`1 - lambda`, and stock `lambda = min(1, airNeedShare * RelSquadBonus)` with `RelSquadBonus = 4.0`. So as soon as aircraft are 25% of
a team's total vehicle shortfall, ground units get exactly zero. Blue air shoots down thousands of DPRK airframes, so DPRK sat at
lambda 1.00 from hour 6 and its ground units received 3 replacement vehicles in five days (ROK, whose air share hovers around
25%, got 4,446) although the pool grew to about 4,800 unused. The fix weights the need instead, `lambda = w*air / (w*air + ground)`:
squadrons still get four times more per missing vehicle, ground never drops to zero, and with little air need it equals the old
number. Five days, Korea Rolling Fire, 2 seeds: DPRK ground replacements 0-3 to 3,400-3,600, DPRK ground vehicles at hour 119 about
1,000 to about 2,500, aircraft unchanged, and the stock Blue win no longer fires within five days. `relsquad=1.0` or `0.5` shows
the same effect from the data side (DPRK ground replacements about 4,200-4,950).

## Timeline format (`runs/*.jsonl`, one JSON object per line)

`{"meta":1,..., "roe":[8x8 ground-capture ROE], "po":<primary objectives>, "objs":[[campId,x,y,type,team]...]}`
then frames `{"t":minutes,"day","hour","ev":[fired event ids],"st":[[supply%,aircraft,groundVehs] per team (the
trigger statistics)],"af":[[ATM rejection counters] per team],"o":[[campId,team,status] changed owners],
"u":[[vuId,kind,team,x,y,veh,full,moving,morale,orders,tactic,supply]...],
"a":[[actionType,tempo,points,initiative,objective] per team],"s":[[gnd,ad,air,ships,objs] per team]}`
plus `"sp"`: the supply chain per team (pools, supply/fuel %, distribution ratios, cumulative production /
delivery / replacement counters, producer capacity, power, battalion supply) - index list above `SupplyFrame`
in `main.cpp`; `python supply_report.py runs/<run>.jsonl [--team 6] [--every 6]` prints it as a table,
and a last `{"final":1,"endgame":N}`. Kinds: 0 battalion, 1 brigade, 3 task force, 4 flight.
Orders = `GORD_*` (1 = CAPTURE), tactic = `GTACTIC_*` (`tactics.h`), action 4 = OFFENSIVE.

## Optional movement counters

`movement-diag.patch` adds per-unit counters (step attempts, "not enough move time", stops by
contact, captures triggered) to `unit.cpp` / `gndunit.cpp` / `battalio.cpp` and prints them at the
end of a run. It is a patch, not committed engine code: `git apply tools/campsim/movement-diag.patch`,
rebuild campsim, grep the log for `MVDIAG`; `git apply -R` to remove it.

## What the experiments say (Korea save0, FF6)

Baseline: stalemate at ~9.7 days, ~5 objectives change hands. Tripling ground speed, quartering
terrain cost, both, or stripping 25% of DPRK's ground forces all leave the front unchanged. See
the "campsim" sections of `WIP-NOTES.md` for the ground-tasking findings.

## Important: the SimToGrid bug

The ground war in the stock game does not advance because of a grid<->sim rounding bug (see
`WIP-NOTES.md`, "ROOT CAUSE of the stalled ground war"). `python simtogrid_check.py` reproduces it in
two seconds. Run with `--set simtogrid=0` for the old behaviour and without for the fixed one.
Baseline numbers in this README's earlier sections (stalemate, speed/cost insensitivity) are for the
**old** behaviour.
