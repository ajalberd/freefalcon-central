# Supply research: making supply shape the Korea war (campsim, 2026-10-18)

Question: what would make supply change the war -- give it ebb and flow, and give the player decisions (strikes, interdiction, protecting
their own lines) -- instead of being a number that stays near 90%? Campsim only, LTCG build; every mechanism is a new switch, off by default;
the baseline is the `h_bar` configuration (CAMPAIGN-CHINA-EXPERIMENTS.md). 3 seeds per arm (6 in the paired round 12), 10-day runs, campaigns end at a win condition.
Harness: `tools/campsim/supply_trials.py run|report|daily [round]`, results in `tools/campsim/supply/results.jsonl` (rounds 1-12, 291 runs); exact same-seed pairs: `tools/campsim/supply_paired.py`.
Nothing here touches the game exe, the install's save0 / cfg, h_bar's settings, existing switches or the trigger scripts.

## The answer

**Supply never matters today because it has no geography: every city, depot, army base and port behind the front is a source (126
logistics hubs plus 361 cities), captured ones work at once, and resupply outruns use -- Blue battalions spend 2.1% of their hours under 50%
supply. Give supply a reach and a travel time and it starts to shape the war: Blue's offensive culminates on days 3-5 deep in the North,
the North's front starves as its lines lengthen, and the war ends. In exact same-seed pairs (section 4) model S won 6 of 6 wars on days 6-10 where the h_bar baseline won 0 of 6. The
player's choices then matter mainly through *lines*: blowing the bridges your army must cross, or letting the enemy cut your bridges or
convoys, delays the win in 5 of 6 paired wars and loses one; which enemy target you hit (depots, factories, convoys) makes no consistent
difference.**

### Ranked recommendations

| # | Mechanism (switches) | What it does to the war | What it gives the player | Evidence |
|---|---|---|---|---|
| 1 | **Logistics reach**: captured depots work after 72 h, sources at most 12 links away, 10% loss per road node (`sxrear=72 sxhops=12 sxloss=10`) | Blue's spearhead supply falls to 44-79% on days 3-5 (up to 76 battalions under 50%); starved battalions stop taking capture orders (1.8% of their hours vs 20.8%) and the offensive culminates | When to push, when to wait for the depots to move up | strong: every reach arm, 30 runs |
| 2 | **Blue's offensive bar 40 instead of 50 with (1)** (`teambar=1:40,2:40,3:40,5:30,6:15`) | Supply now brakes Blue instead of the bar, so Blue recovers after the setback: `R_bar40` all 3 seeds advance, stall on days 3-5 and come back (seed 2: 1449 -> 1401 -> 1587 objectives), 2 Blue wins | -- | moderate: 3 runs, plus 9 of model S |
| 3 | **Convoy time** (`sxdelay=10`, 1 h per link) | Long lines refill slowly: the North's front starves to 27-46% by day 7-8 in most seeds as Blue pushes it back from its depots | A convoy on the road can be hit (`sxconvoyhit`, below) | moderate |
| **S** | **(1)+(2)+(3) together** (`sxrear=72 sxhops=12 sxloss=10 sxdelay=10` + bar 40) | **9 of 9 Blue wins in rounds 7-10 (h138-224), 6 of 6 in the paired round 12 (h138-234; h_bar 0 of 6)**; stall on days 3-5, the North's front starves in seeds 1-2 (28-43% by day 7; seed 3 stays near 80%), Blue's share 60-62% | All of the above | strong: 15 runs, 6 exact pairs against h_bar |
| 4 | **Logistics hubs only** (`sxnocity=1`) with S | Sharpens it for both sides (Blue spearhead 43-61% on day 5, North front 34-57%); 3/3 Blue wins, mean h177 | Depots and ports become the only targets that matter | 3 runs |

**The player's choices on model S, exact same-seed pairs** (round 12, 6 seeds, `det=1`; each war is identical to its unstruck twin until the
strike starts -- h49 for strikes from h48, h73 for strikes from h72, hour 3-5 for convoy interdiction -- so every difference is caused by the
strike; "10" = the 10 objectives of the type nearest the front, `blowlim=TYPE:TEAM:10`). Model S alone: 6/6 Blue wins, h138-234.

| Choice | Blue wins | Win hour vs the same war unstruck | Blue share | Reading |
|---|---|---|---|---|
| **Blue blows the North's 10 front bridges** | 5/6 | **later in 5 of 6**, +23 h on average | -1.9 | **backfires**: Blue has to cross them |
| **The North blows 10 of Blue's bridges** | 5/6 | **later in 5 of 6**, +14 h | -2.0 | **protect your bridges** |
| **The North interdicts Blue's convoys** (10% per hour on the road) | 5/6 | **later in 5 of 6**, +43 h | -1.0 | **protect your convoys** -- the biggest effect |
| Blue interdicts the North's convoys (15%/h) | 6/6 | earlier in 2, later in 4, -4 h | +0.1 | no consistent effect: the North's front already starves |
| Blue hits the North's 10 depots + army bases | 6/6 | earlier in 4, later in 2, +0 h | -0.5 | no consistent effect |
| Blue hits the North's 10 factories | 6/6 | earlier in 3, later in 3, -0 h | -0.4 | no effect |
| (h_bar baseline instead of model S) | 0/6 | -- | -7.1 | model S is what ends the war |

So the decision the model gives the player is about *lines*: which bridges to leave standing for one's own advance, and spending effort
to defend one's bridges and convoys. Strikes on the enemy's rear (depots, factories, convoys) change the war as much as chance does.
Earlier 3-seed rounds (7-10, before exact replays) pointed the same way: every bridge strike and the North on Blue's bridges lost wars
(1/3 and 0/3 with all bridges held down), the rest sat inside the run-to-run spread.

**Runs worth viewing** (Campaign Lab, `tools/campsim/runs/`; checked present and complete):
- `chdd-sx-R_bar40-s2.jsonl` -- the arc: 1449 objectives on day 3, spearhead supply down to 52%, back to 1401 by day 6, resupply, 1587 by
  day 10 (no winner at 240 h).
- `chdd-sx-P_S-s3.jsonl` and `chdd-sx-P_bridgeB10-s3.jsonl` -- the same war, frame for frame, until hour 73, when the North blows 10 of
  Blue's bridges: unstruck, Blue wins at h234; struck, no winner at 240 h (Blue 55%).
- `chdd-sx-P_S-s5.jsonl` and `chdd-sx-P_convB10-s5.jsonl` -- the North interdicting Blue's convoys from hour 4: the win moves from h138 to h240.
  `chdd-sx-P_h-s5.jsonl` is the same seed under plain h_bar: no winner, Blue 51%.

**Not recommended:** `usemult=4` (both sides starve, the war freezes at 46-52%); `sxfront` (engaged units get less: symmetric attrition,
the North's front starves as much as Blue's); `initsupply` (supply-weighted initiative made Blue's comeback rarer: 0 wins in 9 runs with the reach rules);
`sxown`, `sxrear` alone, `sxrail`, `sxwheeled` (no effect: supply barely travels while every city is a source).

**Engine findings along the way:** supply convoys are routed on Foot (`SendSupply`), so they walk round a blown bridge; the source search
(`FindNearestSupplySource`) never checks who owns the source; rail objectives are almost never on a supply route; the campaign thread runs
with truncating float rounding (`_controlfp(_RC_CHOP)`) that campsim's main-thread loop does not set; and model S starves China's battalions
still far back in China (10% supply at 150-250 km from a source), because they draw on the North's sources -- the China phase needs a look
under any reach rule.

**Caveats:** after a strike the two wars drift apart chaotically, so a single pair says little -- read the direction across the 6 seeds.
Rounds 1-10 were run before replays were exact and are 3-seed samples. Strikes are objectives held destroyed, not flown missions.
Nothing here has been tried in the game.

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

## 4. Determinism (campsim `det=1`) -- verified

`det=1` (campsim only, off by default) makes a seed replay the same war:
1. the ATM works to a build budget (20 per call) instead of 200 ms of wall clock;
2. the campaign thread does no campaign work and there is no background VU pump -- message handlers drew on their own per-thread `rand()`,
   so which thread got to a message (a matter of scheduling) changed the war; the main thread drains the queue itself;
3. `rand()` is re-seeded when the tick loop starts; main-thread `GetTickCount` / `timeGetTime` become a per-call counter;
4. **C++ allocations come from a private heap without Windows' randomised low-fragmentation heap, zeroed** -- the last two leaks were reads
   of uninitialised memory (2 of 8 solo runs parted at minute 21 by one extra North Korean flight) and address-dependent behaviour (the
   ATM's "no aircraft free" count drifted by h60).
A `dethash=M` switch prints per-minute state hashes (units, flights, objectives, ATM lists) to find where two runs part.

Verified: 16 runs of one seed (10 one after another, 6 at once) identical over a day; 8 runs (4 normal exe, 4 with ASLR off) identical over
6 days; and in the 9-way harness, **each of 6 twin pairs (model S and h_bar, seeds 1-3) identical frame for frame for the whole war
(152-242 hourly frames)**. In round 12 every strike arm is identical to its unstruck twin up to the strike hour. Stock campsim parts at hour 1.

## 5. Notes

- `blow=` keeps its original syntax; the player-sized strike is the new `blowlim=TYPE:TEAM:N` (rounds 9-10 were run with an interim syntax,
  `blow=TYPE:TEAM:hold:N`, before it was moved to the new key; `S_depot10_rep` with `blowlim` reproduces it within the noise).
- During round 9 every running campsim process on the machine was force-stopped to kill runs started on a stale exe.
