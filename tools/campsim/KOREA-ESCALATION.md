# Korea Escalation (JSGME mod)

A campaign mod for the Korea theater's `save0` ("Red Herring"): a longer war that ends only with Pyongyang
and Wonsan, in which China and Russia actually join the fighting. It applies to **new** campaigns only.

- Installed at `C:\FreeFalcon6\MODS\Korea Escalation` (enable/disable in JSGME); its `Korea Escalation README.txt` lands in the game folder when enabled.
- Built by `tools/campsim/make_escalation_mod.py` (needs the install's stock `save0.cam` and `Falcon4.AII`,
  i.e. run it with the mod **disabled**). `--aii-only` rewrites just the AII and works with the mod enabled
  (stock AII from JSGME's `MODS/!BACKUP`, written to the mod and the live install).
- campsim reads the install's `Falcon4.AII`, so with the mod enabled sims run at the mod's values; `hcg=1.5` restores stock.
- The builder writes the same files to campsim's `gamework/campaign/SAVE/escal.cam` + `escal.tri`, so the
  simulation tests exactly what you play: `python run_batch.py --save escal --set tri=escal ...`.

## What is in it

| File | Change | Stock |
|---|---|---|
| `campaign/SAVE/save0.tri` | Blue wins only when it holds Pyongyang (680 + 260) **and** Wonsan (404) | either one ends the war |
| | Russia joins when Blue takes Wonsan (event 13 -> 12) | DPRK supply <= 20% or DPRK aircraft <= 30% of ROK's |
| | China joins unchanged: DPRK supply <= 40% or DPRK aircraft <= 60% of ROK's | |
| `campaign/SAVE/save0.cam` | China's 43 and Russia's 8 battalions staged on DPRK objectives 100-170 km behind the front, their squadrons on DPRK airbases (`make_ally_mod.stage(..., "prc+cis")`) | China ~340 km, Russia ~500 km back |
| | Every PRC battalion and active squadron cloned once: 86 battalions (2,136 vehicles), 26 squadrons. Clones have no brigade (U_PARENT), new ids and names | 43 battalions, 13 squadrons |
| | DPRK bombers laid out like vanilla Falcon 4.0's Tu-16s: H-6A squadrons (China's Tu-16) cloned to DPRK, 1 active at Sunan, reinforcements at Sunan (h48) and Toksan (h72) | DPRK has no bomber-role squadrons |
| | Russia's Pacific Fleet, 44-137 km off Wonsan, built for air defence: Kuznetsov carrier with an Su-33 squadron based on it, the Kiev battle group (Kiev, 2 Admiral Nakhimov, 6 Najin), 3 more Admiral Nakhimov cruisers (SAMs to 64 km), a Kilo submarine, 4 Osa II groups (12 missile boats). Neutral until Russia joins. Blue aircraft shot down by ships: median 80 -> 156 a run vs an Osa-heavy fleet | Russia has no ships |
| `campaign/SAVE/Falcon4.AII` | `ObjGroundPathMaxCost = 2000` | 500: rear units are never picked for orders |
| | `2DHitChanceGround = 5`: aircraft hit chance vs ground units is divided by this (unit.cpp; runway/building strikes unaffected). campsim, seeds 21-24 x 5 days, live cfg: Blue wins h42-69, 66% of DPRK ground losses to air, DPRK ground kills 650 Blue vehicles | 1.5: h31-46, 76%, 400. (3: h34-50, 69%; 8: h48-59, 60%) |

China and Russia stay their own (neutral) countries until the trigger brings them in, then fold into DPRK as
in stock. Keeping them as separate factions was tested and was worse (their initiative drifts to ~40 and
they sit idle), see WIP-NOTES.md 2026-10-03/04.

## Recommended exe and cfg

Play it with `FFViper-ai.exe` (main at d6fbd2cb or later) and this block in `FFViper.cfg`:

```
set g_bAlertScramble 1
set g_bInitTrueLosses 1
set g_nCounterAttackInitiative 15
set g_nCaptureInitiative 2
```

The ground-AI fixes (GtmCaptureFront, SupplyNeedFix, GridPathPartial, ReserveHold, SupplySplitShares,
ReserveNoPullback, ...) are on by default in that exe.

## Measured (campsim, sliders 2:2:2:2 + Ace, 6 seeds x 4 days, medians)

| Setup | War ends | Wonsan | DPRK offensive | Red capture orders / h (China+Russia) | Scrambles DPRK / ROK |
|---|---|---|---|---|---|
| stock trigger, AI fixes on | h34-45 | h34-45 | 0% | 0 | 0 / 0 |
| this mod, exe defaults | h72 | | 0% | 0 | 0 / 0 |
| this mod + recommended cfg | h81 (h71-90) | h44-63 | 4% | 9.8 (4.2) | 829 / 178 |

Blue still wins every run. China loses ~55% of its vehicles (stock China ~70 vehicles); Russia takes losses
after joining but does not advance; DPRK bombers add ~10% to ROK ground losses to air.

## Rebuilding

1. Disable the mod in JSGME (the builder refuses to run on a modded `save0.cam`).
2. `cd tools/campsim && python make_escalation_mod.py` (`--clones N` for more PRC copies,
   `--no-install` to write only campsim's `escal.*`).
3. Re-enable the mod.
