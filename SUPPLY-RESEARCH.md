# Supply research: making supply shape the Korea war (campsim, 2026-10-18)

Question: what would make supply change the war -- give it ebb and flow, and give the player decisions (strikes, interdiction, protecting
their own lines) -- instead of being a number that stays near 90%? Campsim only, LTCG build; every mechanism is a new switch, off by default;
the baseline is the `h_bar` configuration (CAMPAIGN-CHINA-EXPERIMENTS.md). 3 seeds per arm (6 in the paired round 12), 10-day runs, campaigns end at a win condition.
Harness: `tools/campsim/supply_trials.py run|report|daily [round]`, results in `tools/campsim/supply/results.jsonl` (rounds 1-15, 520+ runs); exact same-seed pairs: `tools/campsim/supply_paired.py`; maps: `supply_view.py`.
Nothing here touches the game exe, the install's save0 / cfg, h_bar's settings, existing switches or the trigger scripts.

## The answer

**Supply never matters today because it has no geography: every city, depot, army base and port behind the front is a source (126 hubs
plus 361 cities), captured ones work at once, production goes into one national pool, and resupply outruns use -- Blue battalions spend
2.1% of their hours under 50% supply.** Three findings came out of 15 rounds (520+ runs, the last five as exact same-seed pairs):

1. **Reach and travel time make supply shape the ground war.** Model S (captured depots after 72 h, sources within 12 links, 10% loss per
   road node, convoy time, Blue's bar 40) makes Blue's offensive culminate on days 3-5 and starves the North's front as its lines lengthen.
   Blue wins 3 of 6 paired wars (h149-211) where h_bar wins 0 of 6, and Blue's share is higher in 5 of 6 (+3.9).
2. **Supply can set the rhythm instead of initiative bars.** With model S, Blue's bar at 15 (the cfg's value, no per-team bars) plays out
   like bar 40 (3/6 wins, +0.8); h_bar's bar 50 on top over-brakes Blue (1/6, -5.7).
3. **Only a supply *map* -- producers feeding regional depots, shipments between depots that can be cut -- makes the player's strike choices
   matter consistently.** In the map model, in 6 of 6 paired wars each: interdicting the North's convoys raises Blue's share (+3.3); the
   North hitting Blue's depots (-2.4) or interdicting Blue's convoys (-2.7) lowers it. Without the map, which target the player hits makes
   no consistent difference -- only *lines* do (bridges Blue must cross, its own bridges and convoys). **But as built the map model breaks
   the balance: Blue falls 10-12 points** (its producers are far south, captured depots start empty, the North fights beside its own).

**Correction (2026-10-19).** Every arm with convoy time (`sxdelay`) before round 12's rerun had a bug: deliveries to Chinese battalions
(supplied from the North's pool under `chinaown`) were queued but never handed over, so China starved after joining (median supply 16%
by day 8). That inflated model S's results: "9 of 9 / 6 of 6 Blue wins" in the first version of this report became **3 of 6** with China
supplied. Rounds 4-10 and the first round 12 are affected wherever `sxdelay` was on (`d5`, `d10`, `R_d10*`, `w_d10*`, `S*`, `N*`); the
reach-only arms (`R*`, `r*`) are not. All tables below use the fixed build. The base settings `noprod=1 prodsup=0.75 usemult=2` (tighter
supply than stock; depots and army bases do not produce) are deliberate and in every run.

### What each idea did (rounds 12-15, exact same-seed pairs, 6 seeds; "share" = Blue's share of objectives at the end vs the paired war)

| # | Idea (switches) | vs | Blue wins | Share | Reading |
|---|---|---|---|---|---|
| S | **Model S** (`sxrear=72 sxhops=12 sxloss=10 sxdelay=10 sxwheeled=1` + bar 40) | h_bar | 3/6 vs 0/6 | **+3.9, higher in 5 of 6** | the war ends; culmination on days 3-5 |
| 1 | **Rail**: trains on all 22 lines (China's from Shenyang / Dandong too), twice the load (`raillines=... railload=240`; the cfg already runs one on the Pyongbu Line) | S | 4/6 | **+2.3, higher in 4 of 6** | helps Blue, whose lines are the long ones |
| 2 | **Few convoys**: depot-to-depot shipments of the map model, interdictable (`sxconvoyhit`) | map | -- | **North's hit +3.3 (6 of 6); Blue's hit -2.7 (6 of 6)** | the clearest player choice found |
| 3 | **Attacking uses more** (`sxattackuse=2` / `3`) | S | 1/6 / 0/6 | -4.7 / **-7.6 (6 of 6)** | punishes whoever attacks -- here Blue; too blunt |
| 4 | **Pockets** surrender (`sxpocket=12`, no route to any source) | S | 2/6 | -1.2, mixed | rare (0-3 surrenders a war); small |
| 5 | **Bridges**: supply-line bridges behind the front (`blowsel=bridge:6:10:10:1`) / front bridges struck once, then repaired (`blowsel=bridge:6:0:10:0`) | S | 1/6 / 2/6 | -4.3 / -3.9 | still backfire; repair only softens it |
| 6 | **Visibility**: `supply_view.py` draws spearhead / front / depot supply by day (images below) | -- | -- | -- | the culmination is plain to see on a map |
| 7 | **Rhythm**: bar 15 for all / h_bar's bars kept (`teambar=`) | S | 3/6 / 1/6 | +0.8 / -5.7 | supply alone can brake Blue; the bars become unnecessary |
| M | **Map model** (`sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10`, 2% node loss) | S | 0/6 | **-10.6 (6 of 6)** | strikes matter (below), balance does not |

**Strikes and interdiction, by model** (exact pairs, 6 seeds; strikes held from h48 -- h72 for the North's on Blue -- on the 10 objectives
of the type nearest the front):

| Strike | On model S | On the map model |
|---|---|---|
| Blue interdicts the North's convoys / shipments (15% per hour or link) | -0.4, mixed | **+3.3, higher in 6 of 6** |
| Blue hits the North's 10 depots + army bases | -1.2, mixed | +0.6 / +1.3 / +2.9 in the three map variants (higher in 5-6 of 6 in two) |
| Blue hits the North's 10 factories | -1.9, lower in 4 of 6 | +0.3 to +1.1, mostly higher |
| Blue blows the North's 10 front bridges | **-3.4, lower in 4 of 6** | -- |
| Blue blows supply-line bridges behind the front | -4.3, lower in 5 of 6 | +0.2 to +0.8, noise |
| The North blows 10 of Blue's bridges | **-4.3, lower in 5 of 6** | -- |
| The North hits Blue's 10 depots + army bases | -- | **-2.4, lower in 6 of 6** |
| The North interdicts Blue's convoys / shipments (10%) | **-5.0, 1/6 wins** | **-2.7, lower in 6 of 6** |

### Recommendation

1. **Adopt the reach rules (model S) as the ground-war supply model**, and let supply rather than per-team bars brake offensives (bar 15
   for all works as well as 40). It ends the war and gives it a shape. Add rail on all lines -- it is already built, and it helps the side
   with long lines.
2. **Build the map model properly, but balance it first.** It is the only version in which the player's choice of target -- and defending
   one's own depots and convoys -- consistently matters. To balance it, test: Blue's ports (Inchon, Pusan) and the rail artery as
   producers feeding the front; captured depots that keep part of their stock; air resupply for Blue's forward stores. Then make shipments
   real units the ATM and the player can find (the `sxmap` shipment step is where they would be spawned).
3. **Drop** offensive consumption as built (too blunt), and treat pockets as a later, small addition.
4. **Bridges stay counter-productive for the attacker** in every form tried (front, rear, held, repairable). A design answer is needed:
   e.g. bridges only the defender may blow, or the AI preferring targets behind the enemy's front that its own advance does not use.

**Runs and images worth viewing** (`tools/campsim/runs/`, Campaign Lab; images in `tools/campsim/supply/`):
- `chdd-sx-R_bar40-s2.jsonl` -- the arc: 1449 objectives on day 3, spearhead supply 52%, back to 1401 by day 6, then 1587 by day 10.
- `chdd-sx-P_S-s5.jsonl` against `chdd-sx-P_h-s5.jsonl` (same seed): model S wins at h211; h_bar does not win (Blue 51%).
  Images: `view-chdd-sx-P_S-s5.png`, `view-chdd-sx-P_h-s5.png`.
- `chdd-sx-P_M-s1.jsonl` and `chdd-sx-P_M_convN15-s1.jsonl` -- the map model, the same war until hour 18, then Blue interdicts the
  North's shipments: Blue 45% -> 51% at the end.
- `view-chdd-sx-P_map50-s5.png` -- the map model starving Blue's front from day 4 (red circles), Blue pushed back from 1383 to 1250.

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
| `blowsel=TYPE:TEAM:SKIP:N:HOLD` | (campsim strike tool) skip the SKIP nearest the enemy, strike the next N; HOLD 0 = once, then repairable | rear vs front bridges; repair timing |
| `sxattackuse=M` | battalions on capture orders use M times the supply (firing and moving) | offensives consume more |
| `sxpocket=H` | a battalion with no route to any source for H hours loses morale each supply pass, surrenders under 10 | pockets |
| `sxmap=1` (+ `sxmapreach`, `sxmapship`, `sxmapair`) | producers fill their nearest depot / army base; short stores are refilled by shipments from the nearest store with stock over the team's own links, losing each node's loss (damage included) and `sxconvoyhit`; battalions draw from their nearest store | the map model; few convoys |
| (existing) `rail=1 raillines=... railload=N` | the rail branch's supply trains (real units) on the listed lines, delivering N at the railhead | rail artery |
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
