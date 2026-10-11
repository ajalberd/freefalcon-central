# Avionics port: BMS 4.38 manuals vs FF6

Audit of `TO 1F-16CMAM-1 BMS.pdf` and `TO 1F-16CMAM-34-1-1 BMS.pdf` (BMS 4.38, 1 July 2025) against the
FF6 source, and the features ported from it. Branch `rail-tracks`, commits tagged `avionics:`.

**Nothing here has been flown.** Every item compiles in the per-project sandbox (all sim, campaign, UI and
`Falcon4.vcxproj` projects were compiled after the last change) but none has been seen in the game. The
"To check in game" lines are the checklist.

## How the audit was done, and what it got wrong

1. Text only (`pdftotext`), plus code greps, plus a callback diff (FF6 757 names vs BMS 1,509, 608 shared;
   `Docs/01 Input Devices/02 Key File Editor/Complete Callback List.txt`). **The figures were not looked at.**
   The user pointed that out; from then on each feature was built from the rendered page image
   (Ghostscript, see the `freefalcon-pdf-figures` memory). The first TCN page was written from prose and had to
   be redone: the figure put the keypad digits on the left column and ENTR/RTN/RCL on the top row.
2. **Over-calls, found by reading the manual / headers properly:**
   - RESET menu, RCCE, FIX, ACAL, CORR, OFP, INSM are non-functional in BMS too (the manual says so). Not gaps.
   - The ECM system in FF6 is further along than assumed (consent, standby, auto/semi, ICP REQJAM, jammer power).
   - The TFR page already has NORM/LP1/STBY/WX/ECCM, HARD/MED/SOFT ride, 1000/500/300/200/VLC clearance. The first
     audit only saw part of the header. What was missing were HUD advisories.
   - HSD already has CNTL, CPL/DCPL, DEP/CEN, EXP1/2, FRZ, and the flight-member air/ground datalink (ADLNK/GDLNK).
     So "Link 16 lite" for flight members is mostly there; what Link 16 would add is other flights and AWACS tracks.
   - The UHF/IFF backup panels are cockpit hardware (the F-16 block 50 pit), FF6's pit has the older AUX COMM panel.
     There is nothing to click; the backup TACAN control BMS moved to an MFD page is now the TCN page.
   - The BMS manual, FCR chapter: FF6 parity is high (all modes, control page, NO RAD, NOT SOI).

## Done (this branch)

| Commit | Feature | Where | To check in game |
|---|---|---|---|
| `03d3b759` | **TCN MFD format**: backup TACAN mode (AA T/R / T/R), INC/DEC triangles, CH + channel keypad, band X/Y. Replaces the dead RCCE slot, OSB 4 of the MFD menu. | `displays/TcnMfd.cpp`, `mfd.h` (`TCNMode` after `HADMode`, own `TcnDrawable`), `menumfd.cpp` | MFD menu, OSB 4. INC/DEC/band change the channel on the ILS/TCN DED page. Keypad: digits left column 1-5 (OSB 20 down to 16), right column 6 7 8 9 0, ENTR/RTN/RCL top row. Acts on the live source (UFC, or AUX COMM when its master switch has it). |
| `69f7dd05` | **DWAT**: "ALTITUDE - ALTITUDE" once per takeoff when within 3 min of liftoff, gear up, climbed >= 300 ft (never > 10,000), and the descent rate puts you at runway height in <= 30 s. | `AircraftClass::CheckDWAT` (aircraft.cpp), members in `aircrft.h` | Take off, climb 500 ft, push over hard: one call. Honours the Betty switch. |
| `df078726` | **RWR SYS TEST**: callback `SimRWRSysTest`; screen 1 "RWR / SYSTEM GO", screen 2 the ALR-56M part numbers, 2.5 s each, then the normal scope. | `PlayerRwrClass::StartSysTest/DrawSysTest`, `commands.cpp`, `findfunc.cpp` | Bind `SimRWRSysTest` in the keyfile. No pit hotspot yet. |
| `408e7eb0` | **TGP page readouts** (Sniper base page): SPI lat/long/elevation top left, zoom as multiple of the wide FOV, north pointer, laser mode / status / code along the bottom. Display only. | `TgpMfdDrawable::DrawSniperReadouts` | Positions were read off the figure, so check for overlap with RALT, the station row and the OSB labels. The north arrow assumes screen-up is the pod heading. |
| `091aad84` | **HUD "LO TF"** (TFR vertical-clearance warning, flashes below 75 % of the set clearance while the TF autopilot flies). | `hud.cpp`; needs `g_bTFRFixes` | LANTIRN F-16, TF engaged, pick 1000 ft, be at 600. |
| `f01af9a5` | **HSD cursor zero (CZ)** on OSB 9; zeroes the radar's slews, works whoever is SOI. | `navfcc.cpp`, `fccmain.cpp` | Slew in FCR A-G, switch to HSD, press CZ. OSB 9 vs 10 is a guess from the figure; the text says OSB 10. |
| `5225bc58` | **EWS page FDBK / REQCTR / BINGO** are real toggles: dobber to the line, any digit toggles. FDBK gates "CHAFF FLARE", REQCTR gates "COUNTER", BINGO gates "LOW" *and* "OUT" (was LOW only). Defaults ON = old behaviour. | `icp.h` (`EDIT_FDBK/EDIT_REQCTR`, `EWS_FDBK_ON/EWS_REQCTR_ON`), `icplist.cpp`, `icpcniswitch.cpp`, `icp.cpp`, `chafflare.cpp`, `playerrwr.cpp` | LIST 7, dobber down through CH, FL, REQJAM, FDBK, REQCTR, BINGO. |
| `f5732b64` | **HUD Attitude Awareness Arc** in Dogfight mode: 99.5 mR arc, shrinks nose-up (smallest at +87), closes nose-down, roll-stabilised, 5.2 mR end ticks, gone with the gear down. | `HudClass::DrawAttitudeAwarenessArc` (mislhud.cpp) | Dogfight override. Level = half circle under the centre, ground-side always down. Centre assumed at HUD (0,0). |

Build: `mfd.h`, `aircrft.h`, `icp.h`, `hud.h`, `playerrwr.h`, `commands.h` changed, so **Rebuild All**.

## Not done, and why

- **ECM pod (BMS 2.7.4):** warm-up (5-6 min, "S" lamps), XMIT 1/2/3 (avionics-priority / ECM-priority / active jam,
  the last two degrade own FCR/TFR/HARM), 5 programs, T lamp. The consent/standby layer already exists. Needs a
  decision on how harsh warm-up and own-sensor degradation should be (it changes the ramp-start routine).
- **TIME page DELTA TOS** (static "00:00:00" today): needs the numeric-entry path and a pass over every steerpoint's TOS.
- **TGP:** CNTL page (gain/level, GRAY, N/M, MGC), IR pointer, LST, INR/AREA/POINT as BMS names them (FF6 has
  POINT/AREA already), variable zoom beyond the three FOV steps. The pod's own 3D render path is fragile (see
  `laserpod.cpp` notes), so these were left alone unattended.
- **HUD landing declutter** (uncage with gear down in NAV: heading scale up, roll indicator / ILS bars / FD / DED data removed).
- **TFR:** "NOT TIMED OUT" 3-minute cooldown, NO TER, LIMIT, TERRAIN arrows, WARN / TF-FAIL, LANTIRN AAF.
- **Link 16 / MIDS / IDM:** none in the code. Flight members are already on the HSD (ADLNK). A "lite" version
  (other flights + AWACS tracks as PPLI/SA symbols on HSD, FCR, HUD) needs the campaign's air picture exposed to the
  sim; not started.
- **Weapons:** SDB GBU-39, JASSM AGM-158, WCMD CBU-103/105 are not in `FALCON4.WCD`; each needs a model, WCD rows,
  loadouts and SMS pages.
- **Pit hotspots** for the new callbacks (SYS TEST etc.) and the IFF/UHF backup panels: pit art, not code.
- **DTE page** still prints a fixed DTC id; with no DTC from the planner there is nothing real to load.

## Notes for whoever continues

- Compile one project without touching the install: `cc.ps1`-style, see `freefalcon-build-workflow` memory.
  `/p:` switches must come from PowerShell, not Git Bash.
- Line endings: the working tree is CRLF (autocrlf), a Python edit that inserts LF text leaves mixed endings.
- Work was done on `rail-tracks` in the shared worktree; only `avionics:` files were staged. Another session's
  uncommitted graphics edits (`src/graphics/**`, `f4config.cpp`, notes) were left alone.
