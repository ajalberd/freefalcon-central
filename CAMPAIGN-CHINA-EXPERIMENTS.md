# China as its own team, shared offensives and per-team offensive bars (campsim, 2026-10-17)

Goal: get China's troops into the Korea "Rolling Fire" war quickly once it joins, see what China as a team of its own does to initiative, and fix the
turn-taking that locked one side out of the war. Campsim only (LTCG build, `tools/campsim/build_ltcg.ps1`); the game exe and the install's `save0` were not
touched. Every mechanism is a campsim switch, off by default. 3 seeds per arm, campaigns end at a win condition (no `noend`), no player waypoints or strikes.
Harness: `tools/campsim/china_trials.py run|report [round]`, results in `tools/campsim/china/results.jsonl` (rounds 1-6, 34 arms).

## The decision: `h_bar`

```
usecfg=1 tri=chn_appr2 alliedcounter=1:8:80 noprod=1 prodsup=0.75 usemult=2      (trial base)
chinaown=1 neutralnouse=1 neutralhold=1 initindep=1 turnmode=4 teambar=1:50,2:50,3:50,5:30,6:15
save chdd (China's ground battalions staged at Dandong / the Pyongbu line's Yalu crossing), original join trigger (ROK holds Sariwon / Hwangju / Anbyon / Wonsan)
```

| Piece | What it does | Why |
|---|---|---|
| `chinaown=1` | China stays a team of its own when it joins (allied to the North, at war with the North's enemies) instead of being folded into the DPRK; its GTM may use the ally's objectives (`OwnerOrAlly` in gtm.cpp) and it draws on the North's supply pool (`SupplyMember` in supply.cpp) | Folded, China fought only after the North's pool had refilled it (~h100). Own team without these: no capture orders (FASTEST scoring rejects units >100-150 km from a target; reserve objectives only Chinese) and no supply pool |
| `neutralnouse=1` | A team at war with nobody burns no battalion supply | Neutral China went 100% -> 26% supply by h60 and joined reserve-only |
| `neutralhold=1` | Battalions of a team at war with nobody stay where they are | **Bug in every earlier staged run:** neutral China's own tasking walked 79 of 86 battalions staged on the Yalu ~100 km inland by hour 24, so rounds 1-4 never really started China at the border. With it: 0 km drift, first fire on a Blue battalion 23-28 h after the join (was 32-40), 1.8-3.7k volleys at Blue (was 0.5-2.4k) |
| `initindep=1` | Each team's initiative is computed on its own, not 100 minus the enemy's | Zero-sum cannot describe three teams at war. Not tested in isolation with China as its own team |
| `turnmode=4` | Starting an offensive no longer forces the enemy defensive | The stock rule plus the timer reset locked the losing side out for the whole war (`d_base`: Blue DEFENSIVE 167-195 h of 240, stalemates). Shared: wars end |
| `teambar=1:50,2:50,3:50,5:30,6:15` | Per-team initiative bar to start an offensive (overrides the cfg's `CounterAttackInitiative 15` for those teams) | With 15 for everyone and shared offensives, Blue attacks 94-95% of the time after China joins and China's entry barely matters. Blue 50 makes Blue pause (consolidate) when China costs it initiative; China at 50 never attacks (0-4% of the time), 30 attacks 76-93% |

## Results (original trigger, 3 seeds)

| Arm | Blue on the offensive after China joins | First Chinese fire on a Blue battalion | Outcome (event@hour, Blue share of objectives) |
|---|---|---|---|
| `d_base` stock turn-taking, China folded | 25% (defensive from the join to the end) | h100-123 | no winner x3, 46-49% |
| `o_own_is` own team + shared, bar 15, no hold | 73-96% | h85-106 | Blue 17@108 / 160 / 159 |
| `h_is` + neutralhold, bar 15 | 94-95% | h73-104 | Blue 17@143 / 164 / 195 |
| **`h_bar`** + neutralhold, Blue 50 / China 30 / DPRK 15 | 95% / 48% / 21% (rest consolidate, 0% defensive) | h83-94 | Blue 17@136; no winner @240 (68%, 57%) |
| `h_bar50` everyone 50 | 85-92% | h70-79 | Blue 17@156 / 189 / 149 |

In `h_bar` seed 3 Blue's initiative sat at 20-47 from h108 on, so it consolidated for ~130 h while China attacked, and slowly lost ~140 objectives (1508 -> 1367)
without any scripted help. In seed 1 Blue's air war kept its initiative above 50 and it never paused.

## Also tried

- **`#FORCE_DEFENSIVE <team> <hours>`** (new trigger command, cmpevent.cpp / team.cpp; variants `tri/chn_appr2_fd36|48|72.tri`): Blue held defensive for N h
  when China joins, on top of `h_bar`. It produces a clear setback and comeback (Blue loses 50-125 objectives in the 2-3 days after the join, then retakes them;
  `p48`/`p72` seed 1 end with Blue taking Pyongyang ~h220), but 5 of 9 runs end without a winner at h240 and one (`p36` s2) collapses. Not adopted: a scripted
  timer rather than something the war earns.
- **Join immediately / day 1** (`tri/chn_join0.tri`, `chn_join1.tri`): debug triggers to study China's tasking without waiting for the approach trigger.
- **Turn-mode variants**: `turnmode=1` (a spent offensive releases the enemy) changes little; `turnmode=8` (allied strike needs fit units) makes Blue win
  by h80-90 with China never firing. `initw=8:1:1` (ground-weighted initiative) alone does not help.
- **Staging nearer the front** (`chfr`, 70-150 km inside the North): China fights sooner but has no effect on the outcome.

## Open

- China's own objective count barely moves (303 -> 303-312): China does the fighting, the North takes the ground back. Look at what China's capture orders target.
- Events 9 / 10 ("Combined forces go on offensive / defensive", `chn_appr2.tri`) flip ~30 times a week under shared offensives because both sides are on
  the offensive at once; event 10 should require Blue not to be attacking (`#IF_ON_OFFENSIVE 2 / #ELSE / #IF_ON_OFFENSIVE 6`). Not patched yet.
- A non-forced China shock: `#SHIFT_INITIATIVE` from Blue to China at the join with the `h_bar` bars (decays by half per recalculation, may be too brief).
- Chinese logistics of their own, Chinese air as its own team, Russia (team 4) as its own team; more seeds before any of this moves into the game exe.
- Timeline objective rows (`o`) are change-only: each frame lists the objectives that changed hands since the last one ([id, team, status]); ownership at any
  time is the meta line's start owner plus those changes (see `tools/campsim/supply_reach.py`). China's capture count can be tracked that way.
