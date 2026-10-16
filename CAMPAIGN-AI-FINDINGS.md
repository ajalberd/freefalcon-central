# Campaign AI findings (FreeFalcon 6 source) - a re-scan checklist for other Falcon code bases

What we found wrong with the **ground war, supply, air tasking and campaign scripts** in the FF6 source, where in the
code each problem lives, how it showed up, what fixed it, and how to check whether the same thing is true somewhere
else (for example Falcon BMS, if you ever get to read or test its campaign code). Everything below was measured with
`campsim` (headless campaign, `tools/campsim`), not read off the code alone; "Not verified" marks what was not.

Every fix is an `FFViper.cfg` switch (`set g_x 0` restores stock), listed in `src/ui/src/f4config.cpp`.
Stock behaviour = what the original game does. Detail and dates: `WIP-NOTES.md`, sections "ROOT CAUSE of the stalled
ground war", "2026-10-03/04: campaign AI fixes" and "campsim". Supply chain internals: `CAMPAIGN-SUPPLY-ENGINE.md`.

## How to use this on another code base

For each item: (1) find the function named in "where"; (2) check the **stock behaviour** line against what that code
does; (3) if it matches, run the **check**. Names differ in BMS; the shapes below (a rounding conversion, a "sum of
needs", a "one order per unit" loop, a path length cap, a string compare in a script reader) are what to look for.
BMS's campaign source is not public, so for BMS most checks have to be done by behaviour (timelines of unit
positions, supply numbers, which scripted events fire), which is exactly what `campsim` records.

## The test harness traps (read before trusting any number)

| Trap | Effect | Rule |
|---|---|---|
| `g_nNoPlayerPlay` (`team.cpp` hourly update) | a team whose player has not flown for N hours (default 2) loses 2 initiative points and a reinforcement, repeatedly | headless runs need `usecfg=1` or `g_nNoPlayerPlay 400`; the default made the enemy "win in 2-5 days" |
| `AdjustCampaignOptions` (new-game setup) | resupplies units to the start level, applies force ratios; a raw save skips it | campsim `newgame=` is on when no event has fired yet |
| the player's team shares one 100-point initiative pool | it affects who may attack | read initiative, not just positions |
| config hardlinks | a test dir linked to the install follows your in-game settings | pin `ratio=` / `exp=` in tests |
| trigger scripts (`<scenario>.tri`) are re-read on **every** check | editing one changes runs in progress | never edit during a run |

## Defects, by area

### 1. Movement: the grid <-> sim conversion

- **Symptom:** ground units "do not move / do not capture"; speed x3 or terrain cost x0.25 changed nothing.
- **Where:** `SimToGrid` in `src/campaign/camplib/find.cpp`.
- **Stock behaviour:** converts a sim position to a campaign cell with round-to-nearest, but unit positions sit
  exactly half a cell from the boundary, so 553 of 1024 coordinates read back one cell too high. An aggregated unit
  can never step onto such a cell when moving in the decreasing direction.
- **Fix:** `floorf` (`g_nSimToGridFix`, default 1). `FloatToInt32` rounds (it is `fistp`/`cvtss2si`), it does not truncate.
- **Check elsewhere:** any grid<->world conversion done with a rounding cast. Test: pick a unit, order it west and
  south, log its cell each tick; it must be able to step in all four directions.

### 2. Orders: which objectives can be captured

- **Symptom:** the front froze for days with units idle (Pyongyang reached in 2 of 8 runs).
- **Where:** `IsValidObjective` / `GetAddBits` in `src/campaign/camptask/gtm.cpp`; link marking in `camplist.cpp MarkObjectives`.
- **Stock behaviour:** capture orders only go to **secondary** objectives flagged front / second / third line, a hop
  count over the objective link network (not kilometres). Bridges, junctions and SAM sites are never targets, so a
  chain of them behind the front leaves zero valid targets.
- **Fix:** `g_bGtmCaptureFront` lets front-line non-secondaries be targets (Pyongyang 2/8 -> 8/8). A non-secondary can
  have no primary: guard `GetObjectivePrimary()`.
- **Also in `gtm.cpp`:** `AssignUnits` gives one objective per unit; `g_nGtmCaptureUnits` (stock 1, FF6 default 3)
  and `g_bGtmKeepCapture` (stock re-plans every cycle) change how many units attack and how stable orders are.
  `g_bGtmCaptureBestScore` / `g_bGtmCaptureNearest`: a unit sent 100+ km to capture something drops from 15% of trips
  to 2% when it takes the best-scoring target within 60 km.
- **Check elsewhere:** count valid capture targets each tasking cycle; if it is 0 while enemy objectives exist, this is it.

### 3. Orders: reserves pulled back from the front

- **Symptom:** healthy front-line battalions kept falling back as "Reserve <town>" (~300 a day for the player's allies).
- **Where:** `BattalionClass::MoveUnit` in `battalio.cpp` (a capture target taken by a *neighbour* made the order
  invalid, so the unit dropped back 3 links as RESERVE); the GTM reserve step in `gtm.cpp` (keeps the units
  *closest* to the action objective, though the code comment says farthest; reserve objectives are always 20-60 km back).
- **Fix:** `g_bReserveNoPullback` (hold instead of falling back; idle healthy units within 25 km of the front get no
  reserve order), `g_bReserveHold`, `g_bGtmReserveFix` (stock: no reserves move up while on the offensive),
  `g_nGtmReservesPerCycle` (stock 1), `g_bGtmReserveFarthest`.
- **Check:** log orders per unit per hour; count "reserve" orders given to units within 25 km of the front.

### 4. Pathing

- **Symptom:** units retry a far waypoint forever; the last objective is 85-140 km behind them.
- **Where:** grid and objective path search in `camplib/path.cpp` (cap `MAX_DISTANCE` = 96 steps), callers in
  `battalio.cpp` and `gndunit.cpp`.
- **Stock behaviour:** a search that hits the 96-step cap returns a partial route which is **discarded**; the unit
  retries the same waypoint every cycle. Raising the ground path limit does not help (the cap is separate).
- **Fix:** `g_bGridPathPartial` follows the partial route and replaces a stale `last_obj`. Path failures 9,765 -> 12.
- **Related data setting:** `ObjGroundPathMaxCost` in `Falcon4.AII` (500) rejects every unit whose cost-to-arrive
  is above it, so rear/allied units only ever get reserve slots at home. 2000 moved China's mean travel 31 -> 113 km.
  It is a data value, not code: check the shipped AI file.
- **Enemy territory surcharge:** `GetObjectiveMovementCost` multiplies cost x2 for an enemy objective and x4 for an
  enemy town (`g_fEnemyTownPathCost`); 3% of capture orders then drive 30%+ further. Player drag-orders use the same
  route builder (`tactical_set_orders` -> `BuildMission` -> `BuildGroundWP`).
- **Water:** `g_bWaterObjectiveFix`: objectives on water cells (ports) were unreachable, units retried forever.

### 5. Supply

- **Symptom:** all battalion resupply stopped about 12 hours in; later, battalions at ~4% supply.
- **Where:** `src/campaign/campupd/supply.cpp` (`SupplyUnits`), `camptask/squadron.cpp` (`GetUnitSupplyNeed`),
  `camplib/unit.cpp`, `camptask/battalio.cpp`.
- **Stock behaviour (three separate bugs):**
  1. `SupplyUnits` only shares out when the team's *summed* need is above 0. Squadrons whose stores are off the
     weapon table (old weapon numbering in saves; reinforcements are not cleaned by new-game setup) report about
     -1,600 each, the sum goes negative, and **no battalion is resupplied**. Fix `g_bSupplyNeedFix`.
  2. One ratio over everyone's need: squadrons' never-used stores (about 10x a battalion's need, about 98% of sortie
     weapons come back unused) dilute battalions to ~4% a trip. Fix `g_bSupplySplitShares` (ROK battalions under 50%
     supply: 34 -> 0).
  3. Road losses are truncated per node, so small shipments vanish. Fix `g_bSupplyExactLoss`.
- **Warning:** `SquadronClass::GetUnitSupplyNeed` has side effects (relocates, scrambles). Never call it from a diagnostic.
- **Check elsewhere:** log team need and the amount actually delivered per hour; negative or zero need with units at
  low supply is the signature. Production-limited supply (pool near 0 by day 3) is a separate issue, not investigated.

### 6. Initiative and the offensive

- **Symptom:** the enemy never attacked; China/Russia (folded into the enemy team) never got a capture order.
- **Where:** `camplib/team.cpp` (initiative, `g_nNoPlayerPlay`), `falclib/msgsrc/objectivemsg.cpp` (capture award).
- **Stock behaviour:** a defensive side stays defensive while *any* enemy is on the offensive; offensives need
  initiative >= 50/55; initiative is one 100-point pool shared with the player's team; the loss term is pinned (the
  start baseline rises with reinforcements, so losses read 0-13); every captured objective hands the captor 5 points.
  A minor offensive never captures.
- **Switches:** `g_bInitTrueLosses`, `g_nCounterAttackInitiative` (0 = stock), `g_nCaptureInitiative` (stock 5).
- **Not a fix:** splitting allies into separate teams. Non-player teams' initiative drifts to about 40 and they idle.

### 7. Air tasking

- **Alert flights never scrambled.** `FindBestAir` sets counter-air requests to 4 aircraft, an alert flight holds 2,
  `FindBestAirFlight` rejects any flight with fewer planes ("too few aircraft"): about 73,000 rejections a day, zero
  scrambles. Fix `g_bAlertScramble` (`atm.cpp`).
- **Strike shortage** on the weaker side is "no aircraft free", not range or loadouts (diagnostic NOFLIGHT in campsim).
- **Crashes fixed on the way:** `SelectAirActions` with no front left; `atm.cpp`, `package.cpp`, `camplist.cpp`
  (2026-09-30); `AllUnitList` filled with duplicates (airmobile infantry re-inserted per helicopter trip, up to 28
  copies), which had inflated every earlier candidate count.

### 8. Campaign scripts (`.tri`) and the engine's script reader

These cost a day each and apply to any theater with scripted endings (found on the Israel / ITO data, 2026-10-12).

- **Exact token compare:** `ReadScriptLine` (`src/campaign/campupd/cmpevent.cpp`) strips only `\n` and `\r`, so a line
  such as `#ENDIF ` with a trailing space is **not** an ENDIF. The IF stack is never popped and every later block
  silently nests inside it, so end conditions never fire. Check: trailing whitespace in every script
  (`grep -n " $" *.tri`). Making the reader trim is a safe engine change (not made, it affects every theater).
- **Structure bugs in the shipped Israel scripts:** end blocks without `#ELSE` never run; "falls / regained" blocks
  never reset their own event, so their +-50 initiative shift repeats on every check; the offensive/defensive toggles
  tested their own event and could never fire (Korea's stock pattern cross-references the other event); a block
  placed after `#ENDSCRIPT` is never read; objective IDs from one theater pointed at other places in another (the
  later saves were renumbered but kept the old scripts).
- **Team merges:** `#CHANGE_RELATIONS a b 1` *merges* team a into team b (`SetTeam`). After Syria joins Egypt, write
  every end condition against team 6, not Syria.
- **Crash:** `#SET_PAK_PRIORITY` on an objective with no PO data wrote through NULL in release builds (the null check
  was DEBUG-only). Fixed in `cmpevent.cpp`; **not yet in the main-branch game exe**.
- **Scenario names:** the engine picks the script by the scenario name stored *inside* the `.cam`, not the file name.
  Israel Classic's `save0.cam` names `save2` and the reverse; a renamed save keeps reading the old script.
- **`IF_CAMPAIGN_DAY G N`** means `day >= N`; `CheckTriggers` returns early if the local session's team is 255.

### 9. Naval (smaller, partly open)

- Only battalions and squadrons are resupplied; a task force's supply only goes down, and at 0 it cannot shoot.
- Sea tankers and cargo ships are cosmetic (nothing reads them but labels and icons). No waypoint UI for ships.
- Plain waypoints never sailed because `ResetCurrentWP` skips flag-less ones; the planner uses `WPF_REPEAT`.
- `g_bNavalMoveFix`: a ship steered from its start-of-tick position and overshot turns across coasts.
- The campaign cover grid (`<theater>.THR`, a hand-drawn 1 km raster) calls 45,538 cells water that 3D shows as land;
  `g_bNavalSeaMask` is built but off (ships then mostly park on 3D land at anchorages). Open.

### 10. Supply: airlift credit from nothing, and consumption that is too small to bind

- Every landed airlift flight adds `3 x elements / 3` supply, fuel and replacements to the national pools (`update.cpp`), regardless of
  aircraft, need, runway state or any stock at the departure base; flights are base-to-base shuttles at least 40 km apart (`atm.cpp`).
  About 600 landings in five days: a quarter of DPRK supply and ~80% of its replacements. Destroying production does nothing at the
  front while it runs.
- Battalion supply is a percentage; moving costs 2% per ~20 km, an idle timer costs 2%, firing costs `supply x shots / ammunition`.
  Measured per 6 hours across ~400 DPRK battalions: moving ~250 points, firing at aircraft ~110, **firing at ground ~5**: ground
  combat costs almost no supply. Resupply gives `need x min(0.5, pool/need) x typeBonus` (attack role x6) every 20 min / 3 h, so any
  non-trivial pool refills a unit a few percent down. Supply does not constrain the ground war.
- Switches `g_bAirliftNeed` / `g_bAirliftRunway` (on by default) cap the airlift credit by the need around the destination and refuse
  landings at airbases with no working runway: DPRK supply credit -65%, replacements -9% (always plenty of nearby shortfall).
- `RelSquadBonus` replacement split: see section 7 / `g_bReplSplitFix`.

## Open items (not solved)

- China/Russia ground troops barely move even with the fixes (the war ends before 300 km of travel).
- DPRK (weaker side) supply is production-limited: pool near 0 by day 3.
- About 30 battalions of the stronger side are still under 50% supply by day 6-8 with everything on.
- 3 units with no land route; a reserve-order no-op (`SetUnitOrders(int)` hits an empty base overload).
- Not verified in the live game: the ITO script fixes, the naval AI, anything described as "sim-verified only".

## Where the tools are

| Need | Tool |
|---|---|
| run the real campaign AI headless, any theater | `tools/campsim` (`README.md`) |
| compare settings across seeds, watch a war | Campaign Lab (`python tools/campsim/serve.py`) and its map viewer |
| see which `#IF` branch fired | the `.evt` CND1 trailer, shown in the campaign editor |
| read / edit `.cam`, `.tri`, class tables | `tools/campaign-editor` (`ffcamp` library) |
| decode `Falcon4.AII`, `FALCON4.TT` | struct layouts in `tactics.h`; `campsim` log lines |

## China does move now; the allied counteroffensive (`AlliedCounterattack`, 2026-10-15)

Korea Rolling Fire, current code (troop trains and rail mobilisation on): China's 86 battalions start 140-200 km from Blue, join the war at h11,
and ride to the front: median 80-90 km from the nearest Blue battalion at h24, 19-34 km at h36-48. They then sit in RESERVE (69 of 86 at h24) behind a
North that is DEFENSIVE all war (its planner hands out DEFEND to the thin band of near-front towns and RESERVE to the rest: at h24 the North's Korean
battalions are 132 RESERVE, 170 air defence, 6 DEFEND, 2 CAPTURE), and are killed piecemeal: 28-37 battalions left at h120 (1,566 -> 554-666
vehicles), supply 94% and morale 60-77 throughout. `china_report.py <run>` prints this. China's problem is orders, not supply or morale.

`AlliedCounterattack` (cfg, off by default; `AlliedCounterMin` 8 battalions, `AlliedCounterKm` 80 km of the front, `AlliedCounterRestHours` 0; campsim
`alliedcounter=1[:min:km:rest]`): when that many allied battalions folded into a team (team DPRK, `GetCountry()` China or Russia) are at the front, the team
launches a full counteroffensive whatever its initiative. Two things were needed: the launch (`SelectGroundAction`, bypassing the initiative test) and a
hold, because Blue's own counterattack cycle (`CounterAttackInitiative`, 18 h) flipped the North back to DEFENSIVE at its next hourly evaluation, so every
allied offensive lasted one hour; the enemy's launches are now held back for the action timeout. Result (5 days, 2 seeds): DPRK is OFFENSIVE 39% of the
time (h39-57, h75-93, h111-) against 5%, 32-38 of China's battalions have CAPTURE orders at h48 against 9-11, 37-49 are alive at h120 against 28-37, and the
North ends with 1,147-1,246 objectives against 1,069-1,099. Blue still wins the war in neither or one seed of two (h119 at baseline). The North's offensive does
not restart right after Blue's offensive ends (an 18 h cycle, rest hours make no difference); why has not been traced.
`AlliedCapture` (cfg, off by default; campsim `alliedcapture=1[:min:km]`, same Min / Km as above) is the alternative: the North stays DEFENSIVE and only the folded-in
allied battalions get CAPTURE orders (the planner collects capture candidates, keeps the allied ones, assigns them before the defensive pass; `gtm.cpp` `Task`). Blue
is not forced onto the defensive. Result, 5 days, 2 seeds: China has CAPTURE orders earlier (33-39 of 86 at h24, 27-34 at h48 against 2-9) but dies faster: 21-24
battalions alive at h120 against 37-49 with the team counteroffensive (28-37 at baseline), and the North's objectives end at 1,071-1,243 against 1,194-1,246
(counteroffensive) and 1,069-1,099 (baseline). The team counteroffensive is better for China because it also forces Blue onto the defensive for the action timeout; attacking
into Blue's running offensive costs the allies their strength.
**Correction and what China's losses are made of (2026-10-15, later).** The one-hour / 18-hour cycles above were partly my own doing: Blue's US battalions are folded into the ROK
team with a different country, so the rule also gave Blue a counteroffensive, which held the North's back (`GADBG`: `held 1` for 18 h). `AlliedCounterTeams` (default `6`, DPRK) now lists the
teams that may use `AlliedCounterattack` / `AlliedCapture`. With Blue excluded the North's counteroffensive runs 86-87% of the time (5 days, 2 seeds): it ends with 1,469-1,474 objectives
(it started with 1,440; baseline 1,091-1,113), Blue with 1,205-1,210 (started 1,239; baseline 1,587-1,609), China has 42-45 battalions left (28-41 baseline), and Blue does not win inside the 5 days.
That is a swing from a Blue romp to a back-and-forth, and probably too strong; `AlliedCounterMin` / `Km` set when it starts. `AlliedCounterRestHours` does not yet produce pauses (the action
re-arms at once; not traced). China's losses (`ALLYLOSS` lines, vehicles lost to air / artillery / ground, 5 days, 2 seeds): baseline 656 / 0 / 983 and 350 / 0 / 847, so 60-71% ground combat and 29-40%
air; team counteroffensive 512 / 255 / 349 and 413 / 219 / 490 (air 31-44%, Blue artillery now 19-24%); allied-only capture 686 / 12 / 792 and 730 / 13 / 780 (air 45%).
**When should China join? (event #11, 8-day runs, 2 seeds, counteroffensive on)** Stock: `IF_SUPPLY 6 L 40` (never fires, the stat stays ~91%) or `IF_FORCE_RATIO A 6 2 L 6` (DPRK aircraft x10 / ROK
<= 6: fires at h11). New trigger `#IF_GROUND_PCT team L|G pct` (the team's ground vehicles as a percent of its start, `startStats`); variants in `tools/campsim/tri/chn_*.tri` (copy next to
`save0.tri` and run `tri=chn_<name>`). Results (China joins / objectives DPRK vs Blue at h192; Blue holds 1,239 and DPRK 1,440 at the start):
`chn_g70` (`IF_GROUND_PCT 6 L 70`) joined at h28 / h10 (the team stat falls faster than the vehicle count I used to predict day 3): 1,570 vs 1,109 and 1,512 vs 1,167; `chn_ratio`
(`IF_FORCE_RATIO G 6 2 L 10`) h23 / h11: 1,578 vs 1,101 and 1,523 vs 1,156; `chn_appr` (`IF_CONTROLLED 2 O 253 256 732`: ROK holds Sariwon, Hwangju or Anbyon, the approaches to Pyongyang
and Wonsan) h80 / h60: 1,421 vs 1,258 and 1,306 vs 1,373, with 37-56 Chinese battalions left, and no Blue win in 8 days. Baseline without the rule: Blue wins at h114-117 with 1,590-1,610 objectives.
Blue first takes (baseline, 2 seeds): Hoeyang h40-46, Sariwon h71-82, Hwangju h79-99, Anbyon h83-101, Wonsan h75-101, South Pyongyang h100-112, Pyongyang h107-117.
South of Pyongyang: Sariwon (253) 62 km, Hwangju (256) 42 km, Songnim (257) 35 km, Chunghwa (259) 21 km, Nampo (258) 33 km; south of Wonsan: Anbyon (732) 18 km, Hoeyang (411) 43 km, Kojo (407) 28 km.
**The approach trigger on 4 seeds, and with the production changes (8 days).** Seed 3 exposed a gap in `chn_appr`: Blue took Wonsan itself at h64 without ever taking Sariwon, Hwangju or Anbyon, so China never
joined (DPRK 940 vs Blue 1,458 objectives, 86 Chinese battalions idle). `chn_appr2` adds Wonsan (404): China joins in all four seeds (h38-80) and the end state is level: DPRK 1,342-1,431 vs Blue 1,248-1,358 objectives,
24-56 Chinese battalions left, no Blue win. With the production stack on top (fixes, depots and army bases not producing, use x3; `chn_appr`, 4 seeds): factories intact -> China joins h47-67, DPRK supply 94-96%, at most 1
battalion under 25%, objectives DPRK 1,438-1,619 vs Blue 1,081-1,241; factories held down -> DPRK battalion supply 70-75% at h96 (95-92% again at the end), objectives DPRK 1,439-1,577 vs Blue 1,123-1,261. The
supply choke shows on day 4 and fades by day 8 because the North's ground force has shrunk to about a third (less to feed) and it has taken ground back; it does not change who holds what.
