# Korea Escalation (JSGME mod)

A campaign mod for the Korea theater's `save0` ("Red Herring"): a longer war that ends only with Pyongyang
and Wonsan, in which China and Russia actually join the fighting. It applies to **new** campaigns only.

- Installed at `C:\FreeFalcon6\MODS\Korea Escalation` (enable/disable in JSGME); its `Korea Escalation README.txt` lands in the game folder when enabled.
- Built by `tools/campsim/make_escalation_mod.py` (needs the install's stock `save0.cam` and `Falcon4.AII`,
  i.e. run it with the mod **disabled**).
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
| | Russia's Pacific Fleet, 44-127 km off Wonsan: Kuznetsov carrier with an Su-33 squadron based on it, Admiral Nakhimov missile cruiser, a Kilo submarine, 8 Osa II groups (24 missile boats). Neutral until Russia joins | Russia has no ships |
| `campaign/SAVE/Falcon4.AII` | `ObjGroundPathMaxCost = 2000` | 500: rear units are never picked for orders |

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
