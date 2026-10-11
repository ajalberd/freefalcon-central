# Avionics port: BMS 4.38 manuals vs FF6

Audit of `TO 1F-16CMAM-1 BMS.pdf` and `TO 1F-16CMAM-34-1-1 BMS.pdf` (BMS 4.38, 1 July 2025) against the FF6 source,
and the features ported from it. Branch `rail-tracks`, commits tagged `avionics:` (and `tools:` for the data tools).

**Nothing here has been flown.** Every item compiles in the per-project sandbox (all sim, campaign, UI and
`Falcon4.vcxproj` projects were compiled after the last code change) but none has been seen in the game. The
"To check in game" column is the checklist. **Rebuild All**: `mfd.h`, `aircrft.h`, `icp.h`, `hud.h`, `lantirn.h`,
`playerrwr.h`, `fcc.h`, `commands.h`, `vehicle.h` changed.

## How the audit was done, and what it got wrong

1. First pass was text only (`pdftotext`), code greps and a callback diff (FF6 757 names vs BMS 1,509, 608 shared;
   `Docs/01 Input Devices/02 Key File Editor/Complete Callback List.txt`). **The figures were not looked at.** From
   then on each feature was built from the rendered page image (Ghostscript; see the `freefalcon-pdf-figures` memory).
   The first TCN page was written from prose and had to be redone.
2. **Over-calls, found by reading the manual / headers / data properly:**
   - RESET menu, RCCE, FIX, ACAL, CORR, OFP, INSM are non-functional in BMS too (the manual says so).
   - ECM in FF6 already had consent, standby, auto/semi, ICP REQJAM and jammer power; the TFR page already had
     NORM/LP1/STBY/WX/ECCM, ride and clearance; the HSD already had CNTL, CPL/DCPL, DEP/CEN, EXP1/2, FRZ and the
     flight-member datalink (ADLNK/GDLNK); the FCR matches the manual (control page, NO RAD, NOT SOI).
   - The UHF/IFF backup panels are block-50 pit hardware FF6's pit lacks; the backup TACAN control BMS moved to an
     MFD page is the TCN page.
   - **CBU-103 / CBU-105 are in the data** as "CBU-87/103 CEM" and "CBU-97/105 SFW"; the first search looked for the
     wrong strings. Only GBU-39 and AGM-158 were really missing.
   - The vehicle table also already had F-16C+/CJ/DJ, KF-16C/D, F-15K etc.; Link 16 is gated by a new flag, below.

## Done

| Commit | Feature | Where | To check in game |
|---|---|---|---|
| `03d3b759` | **TCN MFD format**: backup TACAN mode, INC/DEC triangles, CH + channel keypad, band. On the dead RCCE slot, menu OSB 4. | `displays/TcnMfd.cpp`, `mfd.h` (`TCNMode`, own `TcnDrawable`) | MFD menu OSB 4. Keypad digits: left column 1-5 (OSB 20 down to 16), right column 6 7 8 9 0, ENTR/RTN/RCL top row. |
| `69f7dd05` `e74d84a9` | **DWAT** "ALTITUDE - ALTITUDE", once per takeoff; disarmed until the jet has stood on a runway. | `AircraftClass::CheckDWAT` | Take off, climb 500 ft, push over hard. Honours Betty. |
| `df078726` | **RWR SYS TEST** (`SimRWRSysTest`): "RWR SYSTEM GO", then the part-number screen. | `PlayerRwrClass` | Bind the callback; no pit hotspot. |
| `408e7eb0` | **TGP Sniper readouts**: SPI lat/long/elev, zoom, north pointer, laser mode/status/code. | `TgpMfdDrawable::DrawSniperReadouts` | Check overlap with RALT, station row, OSB labels. |
| `76326ea7` | **TGP control page + extras**: CNTL (OSB 5) opens GRAY (OSB 19, moved from OSB 20) and north-pointer/meterstick select (OSB 18); PTR (OSB 2) IR-pointer status toggle (no beam in the world); LST (OSB 20) moves the pod's SPI onto a target another aircraft is lasing (`IS_LASED`); meterstick. | `tgpmfd.cpp` | LST only matters with a second laser (multiplayer / human wingman); the FCC may overwrite the point each frame, unknown until flown. |
| `091aad84` `ecc8cd8d` | **TFR**: HUD `LO TF`; `LIMIT` (bank > 60 or dive > 20, my interpretation); `TERRAIN ->` / `<- TERRAIN` (ground within 1.5 turn radii at +/-20 deg, every 4th frame); MFD flashing `TFR LIMITS`; **NOT TIMED OUT** 180 s warm-up when LEFT HDPT power comes on during the flight (a spawn already powered counts as warm; TFR stays STBY, only ride/clearance/power work); **TF FAIL** (WARN, light, PULL UP, autopilot dropped) when the pod loses power under TF. | `lantirn.h/.cpp`, `lantmfd.cpp`, `hud.cpp`, `cautions.cpp` | Needs `g_bTFRFixes`. NO TER deliberately not done (FF6's ground-ray model would show it constantly). |
| `f01af9a5` | **HSD cursor zero (CZ)** on OSB 9. | `navfcc.cpp`, `fccmain.cpp` | OSB 9 vs 10 is a guess from the figure; the text says OSB 10. |
| `5225bc58` | **EWS FDBK / REQCTR / BINGO** are real toggles: FDBK gates "CHAFF FLARE", REQCTR gates "COUNTER", BINGO gates "LOW" and "OUT". | `icp.h`, `icplist.cpp`, `icpcniswitch.cpp`, `icp.cpp`, `chafflare.cpp`, `playerrwr.cpp` | LIST 7, dobber down through CH, FL, REQJAM, FDBK, REQCTR, BINGO; any digit toggles. |
| `f5732b64` | **HUD Attitude Awareness Arc** in Dogfight mode (99.5 mR, shrinks nose-up, closes nose-down, roll-stabilised, gone with the gear down). | `HudClass::DrawAttitudeAwarenessArc` | Centre assumed at HUD (0,0). |
| `f8746094` | **HUD landing declutter** on the uncage key (gear down, NAV/ILS): roll indicator, ILS bars, flight director, DED data off; clears on leaving NAV/landing, gear up, WOW or the next uncage. | `hud.cpp`, `navhud.cpp`, `SimToggleMissileCage` | On final in NAV, press the uncage key. |
| `c71ac09e` | **TIME page DELTA TOS**: type HHMMSS + ENTR; the dobber flips the sign. Applied to CRUS TOS, STPT page time and the HUD time to go. Avionics-side only: the campaign flight plan is untouched. | `icp*.cpp`, `scratchpad.cpp`, `navfcc.cpp` | LIST not needed: ICP 6 (TIME). Digits go to DELTA TOS, so use CLR/another page key to leave. |
| `c6afa2f9` | **ECM XMIT** switch (`SimEcmXmitStep`, `SimEcmXmit1/2/3`): 1 = aft antenna only (jam cover only for radars behind you); 2 and 3 = front and rear cover but your own radar loses `EcmXmitDegrade` (default 20 %) of its detection range while the jammer transmits. Default XMIT 2 keeps the old cones. The T lamp is the existing ECM lamp. No warm-up (you chose not to). | `radar.cpp`, `aircrft.h`, `commands.cpp`; knob in `f4config.cpp` | Keyfile only, no pit hotspot. |
| `7ce8a5b8` `f2757d15` | **Link 16 lite on the HSD**: other friendly flights (airborne, on screen) as ring + altitude + heading; **hostile tracks** the team has spotted while one of our AWACS is up above 5000 ft (cfg `Link16Hostile`, default 1). Both gated by `VEH_HAS_LINK16` (0x10000000) in the vehicle class, on the viewer and on the flights shown; both ride the ADLNK toggle. | `FireControlComputer::DrawFriendlyFlights/DrawLink16Hostiles`, `vehicle.h`, `tools/avionics/set_link16.py` | Flag applied 2026-10-11 to `terrdata/objects` and the four `campaign/*/CampaignDB` `FALCON4.VCD` (backups `*.bak-pre-link16`): F-16CJ, DJ, C+, KF-16C/D, MLU AM-B/AM-D, F-15K, F/A-18E/F, E-3, E-2C. Not flagged: F-16A/B/C/CG/DG/XL, HAF, AGSR. |
| `8f999bbd` | **DTE LOAD**: highlight sequence over MPD, COMM, INV, PROF, MSMD, GPS, DLNK, FCR, then the UFC goes back to its mission defaults (`ICPClass::InitStuff`); DTC id from the flight. | `dtemfd.cpp` | It wipes pilot-entered ICP data, like a real load. |
| (this batch) | **FLIR / LANTIRN, built out as in the manual (TO 1F-16CMAM-34-1-1 2.6.2.1)**, experimental render path. The **FLIR MFD page** now shows the infrared picture (grey) and has the BMS layout: top OSB 1 mode (OPER/STBY/OFF), left OSB 20 OPER, OSB 18 STBY, OSB 16 GRAY (manual gain/level on), right OSB 10 BSGT, gain and level readout ("50G" "100L") top left, **NOT TIMED OUT** with a countdown. The picture is **repeated on the HUD** (green, in the HUD zone behind the symbology) while the FLIR is OPER and the HUD brightness is up. **Controls:** ICP BRT wheel = `SimBrtWheelUp/Dn` (HUD raster brightness, default 60 %, all the way down = no HUD image); ICP FLIR arrows = `SimFlirLevelUp/Down`; on the page OSB 2-3 `HUD -/+`, OSB 6-9 `GAIN +/-` and `LVL +/-`; BSGT lit turns OSB 6-9 into UP/DN/LT/RT slews of the HUD image (the throttle cursor is not wired). **Power and cool-down:** needs the nav pod (LEFT HDPT) powered and cooled down: 600 s after power-on (BMS says 8-15 min; cfg `FlirCooldown`, 0 = no wait; a jet that spawns powered is warm); losing pod power drops OPER. `SimFLIRToggle` follows the same rules. No cfg line is needed to use it: `HudFlir` is only a kill switch (default 1; set 0 if the HUD render path misbehaves). | `LantirnClass` (lantirn.h/.cpp: state, `DrawHudRaster`), `FlirMfdDrawable` (flirmfd.cpp, now a `LantirnDrawable`), `vcock.cpp`, commands | **Not seen.** Check first: does the FLIR page show the world in OPER; does the HUD show it (else the atlas rebind order is wrong); conformal (`HudFlirTrim`/`HudFlirScale`/BSGT); symbology survives on top; frame cost; any GPU hang in the log. Gain/level scale the picture's brightness only (the IR path has no separate contrast control); WHOT/BHOT polarity, look-into-turn and snaplook are not done. |
| `78393258` | **Avionics New Weapons JSGME mod** (built, **not enabled**): GBU-39 SDB (clone of GBU-38, lighter) and AGM-158 JASSM (clone of Storm Shadow, JASSM range/weight) in the F-16 hardpoint lists that carry their source. | `tools/avionics/add_weapons.py`; mod in `C:\FreeFalcon6\MODS\Avionics New Weapons` | Enable AFTER Korea Rail War (both replace `FALCON4.ct`), disable BEFORE it. SDB falls like a JDAM; models are the sources'. Untested in the game. |

## Not done, and why

- **First night flight (2026-10-11), FLIR ON blanked the HUD, DED and MFDs.** The HUD raster's `RenderIR` was created (`Setup`) inside the open atlas pass; the MFD code closes the pass around its own `DisplayInit` for that reason. It is now built by `LantirnClass::EnsureHudRenderer` from vcock.cpp BEFORE `renderer->StartRtt`. FFDebug.log shows `HUD FLIR: raster renderer created` when it happens. If the displays still blank, set `HudFlir 0` and send the log.
- **Same flight, HUD dim at night / SYM wheel dead / DAY-AUTO-NIGHT switch looked stuck.** Causes found: the SYM-wheel gain was blended out by the daylight level (so the wheel did nothing at night, and it starts at full anyway) - new cfg `HudGainNight` (default 2.0); the NIGHT/AUTO alpha curves cap at 0.4/0.3; AUTO BRT only recomputed on a switch or wheel touch (now every frame); and the pit switch mesh mask used three different formulas (cycle commands, `SimHUDBrt*`, and the per-frame pit update that overwrote them) - now one: `HudClass::BrightnessSwitchMask()` (DAY 2, AUTO 1, NIGHT 4). `HUD light: switch= wheel= alpha=` lines in FFDebug.log record every change. Untested; if the mesh shows the wrong position the three numbers in that function are the thing to swap.
- **Second night flight (2026-10-11):** displays no longer blanked, but the HUD showed no picture (the IR terrain *was* being rendered: sensor dispatches 0 -> 450 / 5 s), the HUD text went white, frame time 8.4 -> 16.3 ms, the FLIR MFD page came out shrunk into its top-right corner, and the HUD brightness switch showed DAY with the HUD in NIGHT. Changes: the HUD FLIR now renders into a **render target of its own** (`VirtualDisplay::CreateExtraRtt` / `SwapRttTexture`, `LantirnClass::RenderHudFlir`), once per frame on the first eye (the atlas pass runs per eye, so the old way drew the scene twice), and is composited on the HUD glass under the symbology (`DrawHudFlirQuad`, green via `g_rttHudTint`, additive). cfg `HudFlirRes` (default 512). The TFR/FLIR MFD page now re-binds the atlas after its scene like the TGP page. The switch masks are DAY 4 / AUTO 1 / NIGHT 2. Log lines: `HUD FLIR: own render target 512x512 created`, `HUD FLIR: first frame rendered into the own target`.
- **Third flight (2026-10-11):** the separate StartRtt/FinishRtt pass on eye 0 broke that eye (its HUD/DED/MFDs blank, HUD text loose on the screen); the right eye was normal. Now the FLIR target is prepared before the atlas pass (`PrepareHudFlir`) and drawn INSIDE it at the HUD as a sub-render with the target swapped in (`RenderHudFlir`, the MFD sensor-page sequence), then made sampleable (`VirtualDisplay::ReleaseExtraRtt`) and the atlas re-bound. cfg `HudFlirDebug 1` shows the raw target in a screen corner. **HUD brightness root cause:** `DrawRttQuad` packs the vertex colour with `D3DRGBA`, which neither clamps nor masks, so any HUD gain above 1 (the SYM wheel reached 2.0) spilled into the neighbouring channels -- "brighter" was never brighter and the switch looked reversed. The gain is clamped to 0..1; wheel = HudGainMin..1, DAY x1, AUTO x0.5..1 by daylight, NIGHT x0.5; `HudGainMax`/`HudGainNight` are now inert. Switch masks back to the original DAY 2 / AUTO 1 / NIGHT 4.
- **Fourth flight (2026-10-11): the HUD FLIR shows** -- 3D objects appear in green where they are, in level flight. It did not roll with the jet (camera rebuilt from Yaw/Pitch/Roll); now `OTWDriver.ownshipRot` x a body-frame boresight/trim offset, the same matrix as the world view. Terrain is not in the picture yet (objects only). The HUD brightness switch still did nothing at night although the log shows the switch state changing: `[HUDGAIN] pit:` (inputs) and `[HUDGAIN] composite:` (the gain DrawRttQuad really used) lines added to find where it is lost.
- **FLIR:** WHOT/BHOT polarity, look-into-turn / snaplook, the throttle cursor as the BSGT slew, and pit hotspots for the BRT wheel / FLIR arrows (key-bind only).
- **Pit hotspots** for SYS TEST and ECM XMIT: the pit's RWR/ECM panels would have to be matched to a texture legend
  first (the technique is in the 2026-09-26 WIP note: texture trace through the UVs, x -569). Key-only for now.
- **IR pointer** has no in-world beam; **LST** and the **TGP gain/level** have no image effect (the MFD intensity is a
  packed colour, so scaling it is not a simple multiply).
- **ECM warm-up (5-6 min)** was declined; **NO TER**, **HUD FLIR brightness / BSGT / look-into-turn** not done.
- **Full Link 16** (net entry, STN, donors, MIDS DED pages, FCR/HUD/HMCS symbology, PDLT, flight-member numbers on the
  HUD) is not in the code.
- **IFF/UHF backup panels** are pit hardware FF6's pit lacks.

## Notes for whoever continues

- Compile one project without touching the install: `cc.ps1`-style sandbox, see the `freefalcon-build-workflow`
  memory. `/p:` switches must come from PowerShell, not Git Bash.
- The working tree is CRLF (autocrlf): a Python edit that inserts LF text leaves mixed endings.
- `src/ui/src/f4config.cpp` is edited by another session; my two knobs (`EcmXmitDegrade`, `Link16Hostile`) were
  committed by staging a HEAD-plus-my-hunk blob (`git update-index --cacheinfo`) so their edits were not swept in.
- All of this sits on `rail-tracks`, which also carries the rail work: do not merge it into main wholesale.
