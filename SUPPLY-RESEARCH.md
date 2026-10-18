# Supply research: making supply shape the Korea war (campsim, 2026-10-18)

Question: what would make supply change the war -- give it ebb and flow, and give the player decisions (strikes, interdiction, protecting
their own lines) -- instead of being a number that stays near 90%? Campsim only, LTCG build; every mechanism is a new switch, off by default;
the baseline is the `h_bar` configuration (CAMPAIGN-CHINA-EXPERIMENTS.md). 3 seeds per arm, 10-day runs, campaigns end at a win condition.
Harness: `tools/campsim/supply_trials.py run|report|daily [round]`, results in `tools/campsim/supply/results.jsonl` (rounds 1-10, 243 runs).
Nothing here touches the game exe, the install's save0 / cfg, h_bar's settings, existing switches or the trigger scripts.

## The answer

**Supply never matters today because it has no geography: every city, depot, army base and port behind the front is a source (126
logistics hubs plus 361 cities), captured ones work at once, and resupply outruns use -- Blue battalions spend 2.1% of their hours under 50%
supply. Give supply a reach and a travel time and it starts to shape the war: Blue's offensive culminates on days 3-5 deep in the North,
the North's front starves as its lines lengthen, and the war ends -- every one of 9 runs of the combined model was a Blue win on days 6-9,
against 5 of 18 for the h_bar baseline. The player's strikes then matter, but only a few clearly: blowing bridges the army must cross
loses wars, and letting the enemy wreck one's own bridges does too; hitting depots is about a day faster, within the noise.**

### Ranked recommendations

| # | Mechanism (switches) | What it does to the war | What it gives the player | Evidence |
|---|---|---|---|---|
| 1 | **Logistics reach**: captured depots work after 72 h, sources at most 12 links away, 10% loss per road node (`sxrear=72 sxhops=12 sxloss=10`) | Blue's spearhead supply falls to 44-79% on days 3-5 (up to 76 battalions under 50%); starved battalions stop taking capture orders (1.8% of their hours vs 20.8%) and the offensive culminates | When to push, when to wait for the depots to move up | strong: every reach arm, 30 runs |
| 2 | **Blue's offensive bar 40 instead of 50 with (1)** (`teambar=1:40,2:40,3:40,5:30,6:15`) | Supply now brakes Blue instead of the bar, so Blue recovers after the setback: `R_bar40` all 3 seeds advance, stall on days 3-5 and come back (seed 2: 1449 -> 1401 -> 1587 objectives), 2 Blue wins | -- | moderate: 3 runs, plus 9 of model S |
| 3 | **Convoy time** (`sxdelay=10`, 1 h per link) | Long lines refill slowly: the North's front starves to 27-46% by day 7-8 in most seeds as Blue pushes it back from its depots | A convoy on the road can be hit (`sxconvoyhit`, below) | moderate |
| **S** | **(1)+(2)+(3) together** (`sxrear=72 sxhops=12 sxloss=10 sxdelay=10` + bar 40) | **9 of 9 Blue wins, h138-224 (mean h188)**; stall on days 3-5, the North's front starves in seeds 1-2 (28-43% by day 7; seed 3 stays near 80%), Blue's share 60-62% | All of the above | strong for "the war ends", weak for anything finer |
| 4 | **Logistics hubs only** (`sxnocity=1`) with S | Sharpens it for both sides (Blue spearhead 43-61% on day 5, North front 34-57%); 3/3 Blue wins, mean h177 | Depots and ports become the only targets that matter | 3 runs |

**Strikes on model S** (held destroyed from h48 -- from h72 for strikes on Blue; "10" = the 10 of the type nearest the front,
`blowlim=TYPE:TEAM:10`; "all" = every one, `blow=TYPE:TEAM:hold`). No strike: 9/9 wins, mean h188 (138-224).

| Strike | Blue wins | Mean win hour (range) | Reading |
|---|---|---|---|
| North's depots + army bases, 10 | 6/6 | 164 (126-193) | about a day faster -- inside the run-to-run spread |
| ... all, from h48 / from h96 | 3/3 / 3/3 | 165 / 159 | the same; a late campaign still pays |
| North's factories, 10 / all | 3/3 / 2/3 | 170 / 152 | no clear gain |
| North's ports, 5 / all | 3/3 / 3/3 | 170 / 154 | no clear gain |
| **North's bridges, 10 / all** | **2/3 / 1/3** | 199 / 195 | **loses wars: Blue has to cross them** (its share falls to 53-55% with all bridges down) |
| **The North hits Blue's bridges, 10 / all** | **2/3 / 0/3** | 191 / -- | **loses wars: protect your bridges** (Blue 52-56% with all down) |
| The North hits Blue's depots + army bases, 10 / all | 2/3 / 3/3 | 210 / 196 | slower, one war lost with 10 |
| Convoy interdiction of the North, 5% / 15% per hour on the road (`sxconvoyhit=6:5`, `6:15`) | 3/3 / 2/3 | 185 / 146 | no clear gain -- the North's front already starves |
| The North interdicts Blue's convoys, 10% per hour (`sxconvoyhit=2:10`) | 2/3 | 201 | costs Blue: spearhead supply 49-59% on day 5, one war lost |

So the decision the model gives the player is mostly about *lines*: which bridges to leave standing for one's own advance, and defending
one's bridges and convoys. Which enemy target to hit (depots vs factories vs ports vs convoys) is a day at most -- not resolvable with 3-9
runs per arm, and probably not a big lever on its own.

**Runs worth viewing** (Campaign Lab, `tools/campsim/runs/`; all checked present and complete):
- `chdd-sx-R_bar40-s2.jsonl` -- the arc: 1449 objectives on day 3, spearhead supply down to 52%, back to 1401 by day 6, resupply, 1587 by
  day 10 (no winner at 240 h).
- `chdd-sx-S9-s2.jsonl` -- model S: Blue stalls on days 3-5 (spearhead 62%), the North's front starves (56% -> 43%), Blue wins at h187.
- `chdd-sx-S_bridgeB10-s3.jsonl` next to `chdd-sx-S9-s3.jsonl` -- the North blows 10 of Blue's bridges from h72: no winner at 240 h
  (Blue 55%), where the same seed without the strike was a Blue win at h224 (a different run, not an exact replay -- see section 5).

**Not recommended:** `usemult=4` (both sides starve, the war freezes at 46-52%); `sxfront` (engaged units get less: symmetric attrition,
the North's front starves as much as Blue's); `initsupply` (supply-weighted initiative made Blue's comeback rarer: 0 wins in 9 runs with the reach rules);
`sxown`, `sxrear` alone, `sxrail`, `sxwheeled` (no effect: supply barely travels while every city is a source).

**Engine findings along the way:** supply convoys are routed on Foot (`SendSupply`), so they walk round a blown bridge; the source search
(`FindNearestSupplySource`) never checks who owns the source; rail objectives are almost never on a supply route; the campaign thread runs
with truncating float rounding (`_controlfp(_RC_CHOP)`) that campsim's main-thread loop does not set; and model S starves China's battalions
still far back in China (10% supply at 150-250 km from a source), because they draw on the North's sources -- the China phase needs a look
under any reach rule.

**Caveats:** 3 seeds per arm; the same seed run twice in the harness ends a day or more apart (model S seeds 1-3: h138 / h144, h187 / h154,
h224 / h210), so differences under about a day are noise. Strikes are objectives held destroyed, not flown missions. Nothing here has been
tried in the game.

**Next steps if this goes forward:** (a) convoys as entities the ATM can find and strike (the `sxdelay` queue is where they would live);
(b) the rail network as the cheap artery from the Chinese border and from Pusan; (c) a player-facing supply view (spearhead supply, depot
reach) so the culmination is visible; (d) 6-10 seeds for the strike rows before deciding anything on target values.

## 1. Why supply does nothing today (measured)

Supply against distance to the nearest friendly source, battalions, days 1-5 pooled over 3 seeds (`supply_reach.py --pool`; ownership
followed through the war; sources as the engine counts them, minus the front-line exclusion):

| h_bar baseline | 0-25 km | 25-50 km | 50-100 km | 100-150 km | 150-250 km | bn-hours under 50% |
|---|---|---|---|---|---|---|
| Blue, hubs + cities | 86% of bns, supply 93 | 13%, 93 | 1%, 95 | -- | -- | 2.1% |
| North, hubs + cities | 87%, 93 | 9%, 92 | 4%, 95 | 0%, 91 | 1%, 100 | 2.2% |
| Blue, hubs only | 66%, 93 | 23%, 91 | 10%, 96 | 1%, 100 | -- | 2.1% |
| North, hubs only | 70%, 93 | 17%, 93 | 10%, 94 | 1%, 93 | 2%, 99 | 2.2% |
| **model S** | | | | | | |
| Blue, hubs + cities | 87%, 82 | 12%, 83 | 1%, 84 | -- | -- | **15.6%** |
| North, hubs + cities | 89%, 85 | 8%, 82 | 3%, 92 | -- | 0%, 100 | **10.5%** |
| China, hubs + cities | 56%, 90 | 15%, 74 | 4%, 61 | 5%, 36 | 16%, 10 | 32.0% |

Supply does not fall with straight-line distance in either model -- almost every battalion is within 50 km of something that counts. Model S
works through the link network and travel time instead: 7 times as many starved battalion-hours for Blue, 5 times for the North.

Other baseline measures: the median Blue battalion's lowest supply all war is 80%; supply falls ~4.9 points an hour when it falls and rises
~6.9 when it rises.

Three engine facts make this so:
1. **A captured source counts at once.** `IsSupplySource` excludes only front-line and second-line objectives, so an advancing army is
   resupplied from the depot it took yesterday. The source search does not even check who owns the source.
2. **Resupply outruns use.** A battalion is resupplied every few hours with up to half its shortfall from a team pool that never runs dry.
3. **Low supply already hurts -- it just never happens.** Shots fired scale with supply (unit.cpp, the weapon-count check near line 5630), and a battalion under
   50% gets no capture orders (gtm.cpp). Measured in model S (6 runs, days 1-7, Blue battalions): under 50% supply they held capture orders
   1.8% of their hours against 20.8% above 50%, while moving (31% / 29%) and engaged (27% / 27%) as often -- a starved spearhead stays in
   contact but stops attacking. That is the culmination mechanism; no new effect was needed.

So the problem is not the effects of low supply but that nothing makes supply low *in some places and not others*. Earlier work changed
the pool (production, use multipliers, strikes on factories) -- that moves supply everywhere at once and freezes the war rather than
shaping it (CAMPAIGN-SUPPLY-SPREAD.md, CAMPAIGN-STRIKE-EXPERIMENTS.md).

## 2. Mechanisms tried (new campsim switches, all off by default; `supply.cpp`, knobs in `main.cpp`)

| Switch | What it does | Goal item it covers |
|---|---|---|
| `sxown=1` | a unit draws only on sources its own team holds | captured sources |
| `sxrear=H` | a source taken in the last H hours is no source yet (9999 = only sources held since the start) | depots moving forward after a delay; lines traced from rear depots (9999) |
| `sxhops=N` | a source more than N objective links away is out of reach: the unit gets nothing | lines traced from rear depots; cut-off units |
| `sxloss=P` / `sxrail=P` | base supply loss per road / rail node on the way, % (stock 2) | Blue culminating deep in the North; rail |
| `sxfront=P` | a battalion engaged with the enemy gets P% of a delivery | effects of low supply in combat |
| `sxdelay=H` | a delivery arrives H hours per 10 links after it is sent (the convoy is on the road) | convoys |
| `sxconvoyhit=T:P` | with `sxdelay`, team T's convoys lose P% per hour on the road | convoy interdiction as a player choice |
| `sxwheeled=1` | supply is routed as trucks (stock: on Foot, round blown bridges) | cut bridges starving units beyond them |
| `sxnocity=1` | cities are no supply source; only depots, army bases and ports | strikes on depots / bridges |
| `blowlim=TYPE:TEAM:N` | (campsim strike tool) hold down only the N objectives of the type nearest the enemy | player-sized strike packages |
| (existing, unchanged) `usemult`, `initsupply`, `blow=TYPE:TEAM:hold` + `blowat` | consumption multiplier; supply-weighted initiative; a whole target type held down from an hour | ammo use; initiative; strikes (upper bound) |

Train interdiction was not tested: the rail trains (railnet.cpp) are off in this branch's campaign and rail objectives are not on supply routes.

## 3. Results, rounds 1-3 (3 seeds each; Blue share of objectives at the end; baseline 56-61%)

| Arm | Blue spearhead supply d3/d5/d7 | Blue bns <50% on d5 | Blue peak, then loss | Blue share at end | Ends |
|---|---|---|---|---|---|
| baseline | 88-95 / 84-97 / 83-93 | 3-18 | none or small | 56-61% | Blue win 1-2 of 3 |
| `sxown` | 93-96 / 91-97 / 93-95 | 0-10 | mixed | 51-61% | as baseline |
| `sxrear=24/72` alone | 84-94 / 85-90 / 89-96 | 4-14 | mixed | 50-62% | as baseline |
| `sxloss=10` alone | 82-94 / 85-94 / 90-97 | 3-17 | -88..-131 | 48-56% | no winner |
| `sxrail=1` (rail free) | identical to sxloss=10 in 2 of 3 seeds | | | | supply routes hardly cross rail objectives |
| `usemult=4` (double use) | 77-89 / 44-71 / 37-85 | 44-98 | -94..-168 | 46-52% | both sides starve, war freezes (North front 48-67%) |
| **home sources only + 8-link reach + 10%/node** (`r_home`, "lines traced from rear depots") | **53-59 / 24-48 / 38-64** | **58-110** | **peak h66-89, then -138..-196** | 47-49% | no winner |
| reach + forward depots after 48 / 72 / 120 h (`r48`, `r72`, `r120`) | 45-68 / 36-87 / 47-85 | 26-99 | peak h61-89, then -115..-225 (one recovers) | 46-60% | 1 Blue win in 9 |
| **reach 12 links, depots after 72 h** (`r72h12`) | **52-79 / 49-78 / 82-90** | 40-61 | **1 culminates; 2 stall on days 3-5 and come back** | 48-62% | Blue win at h210 in 1 |
| `sxfront=0` (engaged units get nothing) | 72-83 / 82-90 / 88-92 | 6-19 | -72..-200 | 46-51% | no winner; the North's front starves too (57-64%) |
| `sxfront=25` | 81-87 / 67-84 / 76-88 | 6-29 | mixed | 49-61% | 2 Blue wins |

Rounds 4-10 (the comeback, convoy time, trucks, hubs only, strikes, convoy interdiction) are in the tables above and in
`python supply_trials.py report <round>` / `daily <round>`.

**What the reach rules do.** With supply drawn only from depots the team has held for a while, over at most 8-12 objective links, at 10% loss
per road node, Blue's spearhead runs dry around day 3-4 deep in the North (spearhead supply 24-56%, up to 110 Blue battalions under 50%), its
offensive culminates, and the North -- fighting near its own depots -- takes ground back. That is the "ebb" the war never had: earned by
Blue's own advance, not scripted. Daily Blue objectives, `r72h12` seed 2: 1239 -> 1307 -> 1369 -> **1449** -> 1419 -> **1368** -> 1412 ->
1434 -> 1474 -> 1518 -> 1599 (spearhead supply 100 -> 94 -> 71 -> **56** -> 64 -> 89 ...).

**Why Blue does not come back with the h_bar bar of 50:** its spearhead is resupplied by day 7-8 (85-92%), but its initiative stays at 38-40
after the setback, under the bar, so it keeps consolidating while the North (bar 15) attacks. Tying initiative to supply (`initsupply`) did
not help; lowering Blue's bar to 40 did (recommendation 2).

## 4. Determinism (campsim `det=1`)

`det=1` (campsim only): the ATM works to a build budget instead of 200 ms of wall clock; the campaign thread does no campaign work (its
message handlers drew on their own per-thread `rand()`); no background VU pump; `rand()` re-seeded when the tick loop starts; main-thread
`GetTickCount` / `timeGetTime` become a per-call counter (import-table patch).

Verified: the same seed run twice, one after the other on an idle machine, gave identical unit states for all 50 hours compared in 5 of 7
pairs (baseline; `sxdelay=10`; reach; `sxwheeled`; full model S once); 4 of 6 simultaneous launches of one seed were identical over 5 days.
Not achieved: the other two solo pairs (Blue bar 40; model S on a 6-day run) parted at hour 1, and runs inside the 9-way harness do not
replay (model S seed 2: h187 once, h154 the next time). Stock campsim parts at hour 1 every time. The remaining source was not found; a
startup race is left (it shows in the first campaign hour, e.g. event 7 firing at minute 35 vs 45). **So the comparisons in this report are
3-seed samples, not exact same-seed pairs**, and only effects larger than the ~1-1.5 day run-to-run spread are claimed.

## 5. Notes

- `blow=` keeps its original syntax; the player-sized strike is the new `blowlim=TYPE:TEAM:N` (round 9 was run with an interim syntax,
  `blow=TYPE:TEAM:hold:N`, before it was moved to the new key; `S_depot10_rep` with `blowlim` reproduces it within the noise).
- During round 9 every running campsim process on the machine was force-stopped to kill runs started on a stale exe.
