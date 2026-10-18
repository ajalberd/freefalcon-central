# Supply research: making supply shape the Korea war (campsim, 2026-10-18)

Question: what would make supply change the war -- give it ebb and flow, and give the player decisions (strikes, interdiction, protecting
their own lines) -- instead of being a number that stays near 90%? Campsim only, LTCG build; every mechanism is a new switch, off by default;
the baseline is the `h_bar` configuration (CAMPAIGN-CHINA-EXPERIMENTS.md). 3 seeds per arm, 10-day runs, campaigns end at a win condition.
Harness: `tools/campsim/supply_trials.py run|report|daily [round]`, results in `tools/campsim/supply/results.jsonl` (rounds 1-9, 228 runs).
Nothing here touches the game exe, the install's save0 / cfg, h_bar's settings or the trigger scripts.

## The answer

**Supply never matters today because it has no geography: 126 supply sources (every city behind the front counts), captured ones work at
once, and resupply outruns use -- Blue battalions spend 2.2% of their hours under 50% supply. Give supply a reach and a travel time, and it
starts to shape the war: Blue's offensive culminates on days 3-5 deep in the North, the North's front starves as its lines lengthen, and the
player's strikes acquire real, sometimes opposite, values.**

### Ranked recommendations

| # | Mechanism (switches) | What it does to the war | What it gives the player |
|---|---|---|---|
| 1 | **Logistics reach**: captured depots work after 72 h, sources at most 12 links away, 10% loss per road node (`sxrear=72 sxhops=12 sxloss=10`) | Blue's spearhead supply falls to 44-79% on days 3-5 (up to 76 battalions under 50%) and its offensive culminates; the ground war stops being a one-way slide | Deciding when to push and when to wait for the depots to move up |
| 2 | **Blue's offensive bar 40 instead of 50 when (1) is on** (`teambar=1:40,2:40,3:40,5:30,6:15`) | Supply now does the braking the bar did; Blue recovers after the setback. `R_bar40`: all 3 seeds advance, stall on days 3-5, come back (seed 2: 1449 -> 1401 -> 1587 objectives), 2 Blue wins (h184, h238) | -- |
| 3 | **Convoy time** (`sxdelay=10`, 1 h per link) | Long lines refill slowly: the North's front starves to 27-46% by day 7-8 in most seeds (27-32% with convoy time alone, `w_d10`) as Blue pushes it back from its depots; with (1)+(2) Blue wins 3/3 on days 8-9 (h188-220) | A target in transit: interdiction of convoys (not built -- next step) |
| 4 | **Logistics hubs only** (`sxnocity=1`: depots, army bases, ports are sources; cities are not) | Sharpens (1)-(3) for both sides (Blue spearhead 43-61% on day 5, North front 34-57% by days 5-7); Blue wins 3/3 earlier (h148-208) | Depots and ports become the targets that matter |
| 5 | Strikes priced by the model (`blow=TYPE:TEAM:hold:N`, the N nearest the front) | see below | A real choice of target, and a reason to defend one's own |

Model **S** = (1)+(2)+(3) (`sxrear=72 sxhops=12 sxloss=10 sxdelay=10 teambar=1:40,2:40,3:40,5:30,6:15`; `sxwheeled=1` makes no difference):
Blue wins 3/3 in each of two batches (h201-220; h138-224), stalling on days 3-5 while the North's front starves.

**Strikes on model S, player-sized (10 objectives nearest the front, held down), Blue win hour per seed (no strike: 138 / 187 / 224):**

| Strike | Seeds 1 / 2 / 3 | Reading |
|---|---|---|
| North's depots + army bases (from h48) | 127 / 169 / 184 | **faster in every seed** (-11, -18, -40 h) -- the best target |
| North's factories | 115 / 174 / 220 | faster in every seed, less (-23, -13, -4 h) |
| North's ports | 153 / 198 / 160 | mixed |
| **North's bridges** | none / 192 / 205 | **counter-productive**: a blown bridge is one Blue must cross to advance |
| The North hits Blue's bridges (from h72) | 184 / 198 / none | **costs Blue** -- one war not won, the others later: protect your lines |
| The North hits Blue's depots + army bases | none / 198 / 222 | costs Blue one war |

With every objective of a type held down (an upper bound): depots + army bases from h48 -> 3/3 wins at h112-205; the same from h96 ->
h143-191 (late still pays); bridges -> 1 win in 3; the North on Blue's bridges -> 0 wins in 3 (Blue 52-56%).

**Runs worth viewing** (Campaign Lab, `tools/campsim/runs/`):
- `chdd-sx-R_bar40-s2.jsonl` -- the arc: advance to 1449 objectives on day 3, spearhead supply 52%, fall back to 1401 by day 6, resupply,
  1587 by day 10.
- `chdd-sx-S9-s2.jsonl` -- model S: Blue stalls on days 3-5 (spearhead 62%), the North's front starves (56% -> 43%), Blue wins at h187.
- `chdd-sx-S_bridgeB10-s3.jsonl` next to `chdd-sx-S9-s3.jsonl` -- the North blows 10 of Blue's bridges from h72: Blue never wins (55%)
  where the same war without the strike ends in a Blue win at h224.

**Not recommended:** `usemult=4` (both sides starve, the war freezes at 46-52%); `sxfront` (engaged units get less: symmetric attrition,
the North's front starves as much as Blue's); `initsupply` (supply-weighted initiative made Blue's comeback rarer); `sxown`, `sxrear`
alone, `sxrail`, `sxwheeled` (no effect while every city is a source -- supply barely travels).

**Engine findings along the way:** supply convoys are routed on Foot (`SendSupply`), so they walk round a blown bridge; the source search
(`FindNearestSupplySource`) never checks who owns the source; rail objectives are almost never on a supply route (rail as an artery needs
the rail network, not these nodes); the GTM already takes capture orders from units under 50% supply and shots already scale with supply --
the effects of low supply exist, low supply just never happens.

**Caveats:** 3 seeds per arm and the war varies a lot between seeds (Blue's win hour in model S: 138-224); differences under about a day
are inside the noise, which is why the strike table reads per seed. Strikes are modelled as objectives held destroyed (`blow`), not as
flown missions. Determinism is partial (section 4). Nothing here has been tried in the game.

**Next steps if this goes forward:** (a) convoys as entities the ATM can find and strike (the `sxdelay` queue is where they would live);
(b) the rail network as the cheap artery from the Chinese border and from Pusan; (c) a player-facing supply view (spearhead supply, depot
reach) so the culmination is visible; (d) repeat the S + strike rows with 6+ seeds before porting anything.

## 1. Why supply does nothing today (measured)

| Measure (h_bar baseline) | Value |
|---|---|
| Supply sources (cities, ports, depots, army bases not on the front / second line) | 126 objectives; ~70% of battalions within 25 km of one |
| Blue battalion supply vs distance to the nearest source (`supply_reach.py`) | 84-100% at every distance band, days 1-5 |
| Blue battalion-hours under 50% supply | **2.2%** |
| Lowest supply a Blue battalion ever reaches (median battalion) | 80% |
| Hourly change when supply falls / rises | -4.9 / +6.9 points |

Three engine facts make this so:
1. **A captured source counts at once.** `IsSupplySource` excludes only front-line and second-line objectives, so an advancing army is
   resupplied from the depot it took yesterday. The source search does not even check who owns the source.
2. **Resupply outruns use.** A battalion is resupplied every few hours with up to half its shortfall from a team pool that never runs dry;
   it uses ~5 points an hour in a fight.
3. **Low supply already hurts -- it just never happens.** Shots fired scale with supply (unit.cpp), and a battalion under 50% gets no
   capture orders (gtm.cpp). The levers exist; supply never falls far enough to pull them.

So the problem is not the effects of low supply but that nothing makes supply low *in some places and not others*. Earlier work changed
the pool (production, use multipliers, strikes on factories) -- that moves supply everywhere at once and freezes the war rather than
shaping it (CAMPAIGN-SUPPLY-SPREAD.md, CAMPAIGN-STRIKE-EXPERIMENTS.md). This round makes supply depend on *where* a unit is.

## 2. Mechanisms tried (new campsim switches, all off by default; `supply.cpp`, knobs in `main.cpp`)

| Switch | What it does |
|---|---|
| `sxown=1` | a unit draws only on sources its own team holds |
| `sxrear=H` | a source taken in the last H hours is no source yet -- depots follow an advance after a delay (9999 = only sources held since the start) |
| `sxhops=N` | a source more than N objective links away is out of reach: the unit gets nothing |
| `sxloss=P` / `sxrail=P` | base supply loss per road / rail node on the way, % (stock 2) |
| `sxfront=P` | a battalion engaged with the enemy gets P% of a delivery (convoys do not reach a unit under fire) |
| `sxdelay=H` | a delivery arrives H hours per 10 links after it is sent (the convoy is on the road) |
| `sxwheeled=1` | supply is routed as trucks (stock: on Foot, round blown bridges) |
| `sxnocity=1` | cities are no supply source; only depots, army bases and ports |
| `blow=TYPE:TEAM:hold:N` | (campsim strike tool) hold down only the N objectives of the type nearest the enemy |
| (existing) `usemult`, `initsupply`, `blow=TYPE:TEAM:hold` + `blowat` | consumption multiplier; initiative counts supply-weighted ground strength; a strike campaign held down from an hour |

## 3. Results, rounds 1-3 (3 seeds each; Blue share of objectives at the end; baseline 56-61%)

| Arm | Blue spearhead supply d3/d5/d7 | Blue bns <50% on d5 | Blue peak, then loss | Blue share at end | Ends |
|---|---|---|---|---|---|
| baseline | 88-95 / 84-97 / 83-93 | 3-18 | none or small | 56-61% | Blue win 1-2 of 3 |
| `sxown` | 93-96 / 91-97 / 93-95 | 0-10 | mixed | 51-61% | as baseline |
| `sxrear=24/72` alone | 84-94 / 85-90 / 89-96 | 4-14 | mixed | 50-62% | as baseline |
| `sxloss=10` alone | 82-94 / 85-94 / 90-97 | 3-17 | -88..-131 | 48-56% | no winner |
| `sxrail=1` (rail free) | identical to sxloss=10 in 2 of 3 seeds | | | | supply routes hardly cross rail objectives |
| `usemult=4` (double use) | 77-89 / 44-71 / 37-85 | 44-98 | -94..-168 | 46-52% | both sides starve, war freezes (North front 48-67%) |
| **home sources + 8-link reach + 10%/node** (`r_home`) | **53-59 / 24-48 / 38-64** | **58-110** | **peak h66-89, then -138..-196** | 47-49% | no winner |
| reach + forward depots after 48 / 72 / 120 h (`r48`, `r72`, `r120`) | 45-68 / 36-87 / 47-85 | 26-99 | peak h61-89, then -115..-225 (one recovers) | 46-60% | 1 Blue win in 9 |
| **reach 12 links, depots after 72 h** (`r72h12`) | **52-79 / 49-78 / 82-90** | 40-61 | **1 culminates; 2 stall on days 3-5 and come back** | 48-62% | Blue win at h210 in 1 |
| `sxfront=0` (engaged units get nothing) | 72-83 / 82-90 / 88-92 | 6-19 | -72..-200 | 46-51% | no winner; the North's front starves too (57-64%) |
| `sxfront=25` | 81-87 / 67-84 / 76-88 | 6-29 | mixed | 49-61% | 2 Blue wins |

**What the reach rules do.** With supply drawn only from depots the team has held for a while, over at most 8-12 objective links, at 10% loss per
road node, Blue's spearhead runs dry around day 3-4 deep in the North (spearhead supply 24-56%, up to 110 Blue battalions under 50%), its
offensive culminates, and the North -- fighting near its own depots -- takes ground back. That is the "ebb" the war never had: earned by
Blue's own advance, not scripted. With forward depots after 72 h and a 12-link reach the spearhead's supply recovers within 2-3 days.

Daily Blue objectives, `r72h12` seed 2: 1239 -> 1307 -> 1369 -> **1449** -> 1419 -> **1368** -> 1412 -> 1434 -> 1474 -> 1518 -> 1599
(spearhead supply 100 -> 94 -> 71 -> **56** -> 64 -> 89 ...): advance, culminate on day 3, lose 80 objectives, resupply, come back.

**Why Blue usually does not come back in the longer-delay arms:** its spearhead is resupplied by day 7-8 (85-92%), but its initiative stays
under its h_bar bar of 50 (38-40 after the setback), so it keeps consolidating while the North (bar 15) attacks. Round 4 tests tying
initiative to supply (`initsupply`), so the comeback follows the resupply.

## 4. Determinism (for paired comparisons)

New `det=1` switch for campsim only: the ATM works to a build budget instead of 200 ms of wall clock, the campaign thread does no campaign
work (its message handlers drew on their own `rand()`), there is no background VU pump, `rand()` is re-seeded when the tick loop starts,
and main-thread `GetTickCount`/`timeGetTime` become a per-call counter. Two runs of one seed now match for 5+ days in most cases (4 of 6
simultaneous launches identical over 5 days; stock: every pair parts at hour 1), but not always -- the first process launched in a burst
still sometimes parts at hour 1, cause not found. The comparisons below therefore read as 3-seed samples, not exact pairs.
