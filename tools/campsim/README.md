# campsim tools

Headless run of the **real campaign AI** (`src/tools/campsim`, builds with the solution, x64 only),
plus the tools to record, replay and compare runs. Everything here is for campaign-AI research.

```
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

## Timeline format (`runs/*.jsonl`, one JSON object per line)

`{"meta":1,..., "roe":[8x8 ground-capture ROE], "po":<primary objectives>, "objs":[[campId,x,y,type,team]...]}`
then frames `{"t":minutes,"day","hour","o":[[campId,team,status] changed owners],
"u":[[vuId,kind,team,x,y,veh,full,moving,morale,orders,tactic,supply]...],
"a":[[actionType,tempo,points,initiative,objective] per team],"s":[[gnd,ad,air,ships,objs] per team]}`
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
