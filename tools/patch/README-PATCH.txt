FreeFalcon / FFViper -- HDR, cockpit, destruction, weather fronts patch
======================================================================

Cumulative: everything from the earlier campaign-planning and night-lighting
patches is included (described further down).


NEW IN THIS RELEASE
-------------------
1. Weather that varies across the theater
   Cold and warm fronts, squall lines, storm cells and clearings move across
   the map with the wind, over drifting patches of better and worse weather.
   The condition picked in Setup -> Graphics is now the PREVAILING weather;
   fronts vary it. What you fly in is the weather where you are: the sky
   darkens as a front comes on, the overcast thins in at a front's edge, and
   from clear air you see a front ahead as a bank of dark cumulus. Satellites
   no longer spot through Poor or worse, briefings give the weather at the
   target, and fronts bring gusts and temperature changes the flight model
   and bombs feel. A campaign save now keeps its weather.
   Settings in FFViper.cfg (defaults shown):
      set g_nWeatherFronts 1          (0 = one condition everywhere, as before)
      set g_fWeatherFrontsPerDay 3.0
      set g_fWeatherNoise 0.8

2. Fixed: campaign saves that would not load
   Helicopter-carried infantry was added to the unit list again every time
   the campaign map replayed a helicopter's pickup and drop. One save held
   three battalions ~43,000 times each; the unit count wrapped and the save
   loaded as 523 units. Fixed at the cause, and a save now writes each unit
   once regardless. A save broken before this patch can be repaired with the
   campaign editor's repair_units.py.

3. The campaign map remembers its filters
   Installations, units, names, bullseye, threat circles, the logistics layer
   and the FLOT line are kept per pilot in config\profiles\<pilot>\campmap.ini
   and come back the next time you run the game.

4. HDR rendering with Gran Turismo 7 tone mapping (D3D12)
   The 3D scene now renders in 16-bit float and Polyphony's GT7 operator
   rolls highlights off instead of clipping them -- lamps, glows, specular
   and the afterburner -- while the HUD and instruments are drawn after it,
   untouched. The sun takes the time of day's colour (orange at sunset, it
   was white at every hour). Sun on an MFD washes it out unless the canopy
   bow shades it, and the cockpit shadow map now actually covers the panel.
   Knobs: ToneMapGT7 (0 = the old 8-bit look), ToneMapExposure, SunTodTint,
   MfdGlare, PitShadowFitReach.

5. Cockpit
   - JHMCS helmet cueing: in the 3D pit the helmet line of sight slaves the
     AIM-9 seeker and replaces the fixed ACM bore line, so you lock what you
     look at -- HMD, TrackIR and mouse look alike. An HMCS brightness knob
     sits below the CMDS panel. g_nHmcs 0 restores the old behaviour.
     New and not yet tested in the air.
   - More of the F-16 pit is clickable: FLCS reset, digital backup, FLT BIT,
     overheat test, ground jettison, and more.
   - Engine sound curves: the placeholder volume/pitch charts every aircraft
     shipped with are replaced by proper curves, and the second in-cockpit
     engine layer finally plays. Canopy muffling (CanopyAttenuation), RWR
     font and a default RWR colour, radio menu size (MenuScale).

6. Destruction
   A jet that is shot down breaks up again: four pieces fly off, spinning
   and trailing smoke, and burn or explode where they land, as in Falcon
   4.0. F-16s use the generic pieces. New and not yet seen in game.

7. Campaign
   Objective icons on the campaign map darken as they are damaged, and the
   ground war routes around damaged bridges and road junctions instead of
   planning through them as if intact (PathDamageCost).

8. Smaller fixes
   The rain sound never played. The flight model asked for the wind at the
   wrong place. FRONTS: lines in FFDebug.log say what the weather is doing.

SOUNDS THIS PATCH EXPECTS BUT CANNOT SHIP
   The JFS starter, cockpit airflow and gear wind, per-switch clicks,
   separate canopy open/close loops and calibre-specific gun sounds are
   wired up in the exe, but the recordings and their sound-table entries
   live in your install and are not redistributable. Without them those
   sounds stay silent -- nothing breaks. See CHANGELOG.md in the source for
   the sound ids and file names each one looks for.


WHAT THIS IS
------------
A patch for an EXISTING, WORKING FreeFalcon installation. It is not a game
download -- no Falcon 4.0 data is included, and none can be legally
redistributed. If you do not already have FreeFalcon installed and running,
this will not do anything for you.

Adds mission planning to the campaign map: right-click a target and build a
package against it, the way BMS and Allied Force let you. Also draws the FLOT
-- the forward line of own troops.


WHAT IT ADDS
------------
1. Build package (right-click any target -> "Build package")
   A submenu of the squadrons that could actually fly a mission against what
   you clicked, nearest first, with a two- or four-ship choice. Clicking one
   files the package there and then. Squadrons that can engage the target are
   listed above ones that can only bring a generic sortie, so an airbase does
   not come up offering you airlift helicopters.

2. Add Package (right-click any target -> "Add Package")
   The full planning dialog -- a flight list, per-flight aircraft, role,
   squadron, airbase, size and target, and takeoff / time-on-target with
   locks. This is the window to use when one package needs several flights
   with different jobs: a SEAD flight on the target, an escort keyed to the
   strike.

   The window itself is stock FreeFalcon and always has been. It had never
   worked on ANY screen, the Tactical Engagement editor included, because the
   artwork it draws with is named by no image list -- it was opening with no
   background at all. That is what art\cp_uiskin.lst and the two files beside
   it are for. See INSTALL: without them the menu item does nothing.

3. FLOT line (right-click the map -> "FLOT line")
   The front, drawn as a line rather than inferred from where the unit icons
   stop. Off by default. The campaign has always tracked this; nothing drew
   it.

Four things behind the planner were also wrong, each of which made it look
like the feature was missing rather than broken:

 - The player's team was read from the Tactical Engagement editor's variable,
   which a campaign sets once on entry and never updates. Every squadron in
   the theater failed the team test, so the squadron list came up empty.
 - Flights were scheduled against the wrong hour. The availability check
   walks the campaign's 32-block schedule, and nothing ever told it which
   block to look at -- every flight asked about block 0 no matter when you
   scheduled it. A squadron busy in block 0 reported "no aircraft free" for
   any takeoff time you picked. A request that cannot be crewed now moves to
   the next block that can.
 - New packages demanded the flight be over the target at exactly the
   displayed second. Right when authoring a scenario, wrong in a running war,
   and it is why hand-built packages were refused so often. The campaign now
   pins takeoff instead.
 - The default role ignored what you clicked, so right-clicking an armoured
   battalion opened a CAP and offered you a map location instead of the unit.


INSTALL
-------
1. Back up your existing FFViper.exe.
2. Copy FFViper.exe into your FreeFalcon folder, overwriting.
3. Copy the art\ folder in as well, keeping its structure:
      art\cp_uiskin.lst
      art\cp_pkg_scf.lst
      art\resource\uiskin_ff4.irc
   These are additive -- they add files, they do not replace any. Without
   them "Add Package" comes up and does nothing, which is exactly the bug
   this release fixes. "Build package" works without them.
4. Copy the DLLs alongside the exe ONLY IF you do not already have them:
      OpenAL32.dll, openxr_loader.dll, dxcompiler.dll, dxil.dll, nvtt30205.dll
   If your install already runs the current dev build, you already have these
   and only need the exe and the art\ files.

Do NOT delete ST48W.dll, ST80W.dll or dbghelp.dll from your install -- they
are stock FreeFalcon files, deliberately not included here.


CONFIGURATION
-------------
Everything here is on by default except the FLOT line. Set these in
FFViper.cfg; the "set g_b" prefix is required and a bare name is ignored.

    set g_bCampaignAddMission 0        # hide the "Build package" submenu
    set g_bCampaignPackageWindow 0     # do not load the Add Package window
    set g_bCampFlotLine 1              # draw the FLOT from startup
    set g_bCampaignPackageTakeoffLock 0
                                       # revert to pinning time-on-target
                                       # rather than takeoff

If a flight is refused, the message now says which of the two reasons it was:
no aircraft free in that time block (try another squadron) or the timing
could not be planned (move the clock). With time-on-target locked -- the
padlock beside it on the Add Package window -- you are asking to be over the
target at exactly that second, which is the tightest request the planner
takes; unlock it and it plans from takeoff instead.

Also included, from earlier work on the campaign economy:

    set g_nSupplyInterdiction 100      # how much a damaged bridge or road
                                       # costs the supply run crossing it.
                                       # 0 restores stock behaviour.

For diagnosing a problem with any of the above:

    set g_bLogCampMenu 1               # writes what the squadron picker and
                                       # the flight planner decided to
                                       # FFDebug.log, next to the exe


KNOWN REMAINING ISSUES
----------------------
- The FLOT is drawn from a list the campaign sorts along a single axis, so a
  front that doubles back on itself will show the line crossing itself. Korea
  runs broadly east-west and traces correctly. The line is also coarse -- the
  campaign thins its own points to 30 km apart -- so it cuts corners.
- On the Add Package window, setting Status to "Target" discards both clocks
  on that window and plans for one minute from now. Leave it on "Takeoff",
  which is the default, unless you know you want that.
- A hand-built package is flagged as a Tactical Engagement mission. How that
  sits alongside a running ATO over a long campaign has not been tested.
- Packages with several flights, and per-flight targets such as an escort
  keyed to a strike, are built and accepted but have not been flown through
  to completion.


NO AUDIO? (not caused by this patch, but commonly hit)
------------------------------------------------------
This build routes DirectSound through OpenAL Soft, which reads alsoft.ini from
%APPDATA% and from the exe's folder. If a global alsoft.ini contains a driver
exclusion WITHOUT a trailing comma, e.g.

    drivers=-dsound

then OpenAL Soft has zero usable backends and opens no device at all -- total
silence. Per its own documentation, unlisted backends are only considered if
the list ENDS WITH A COMMA. The correct form is:

    drivers=-dsound,

Also check volume-adjust / output-limiter: a large positive volume-adjust with
output-limiter=false clips hard and sounds like constant crackling.


REGISTRY -- IMPORTANT IF YOU ALREADY RUN FREEFALCON (registry/ folder)
----------------------------------------------------------------------
This build reads its data paths from the Falcon "4.1" registry key instead of
"4.0". The reason: a stock Falcon 4.0 / GOG install owns the "4.0" key, so
sharing it makes one of the two games read the other's directories. Giving
FreeFalcon its own "4.1" key lets both live on the same machine.

*** If FreeFalcon already worked for you, your paths are under 4.0 and this
    exe will not find them until you migrate. Symptoms: missing terrain, no
    theater, or a failure to start. ***

Fix it with either (both need administrator rights -- they write to HKLM):

  registry\install-registry.bat   (recommended)
      Put it next to FFViper.exe, right-click -> "Run as administrator".
      - If you already have a 4.0 key, it COPIES 4.0 -> 4.1 and leaves 4.0
        completely untouched, so your existing install keeps working and a
        stock Falcon 4.0 is unaffected.
      - If you have no existing key, it writes 4.1 using its own folder as the
        install path, so it works wherever you installed.
      - Re-run with /force to overwrite the 4.1 paths with this folder
        (use this if your 4.0 key belongs to stock Falcon 4.0 rather than
        FreeFalcon, or if you moved your install).

  registry\FreeFalcon6-registry.reg
      Manual alternative. Assumes C:\FreeFalcon6 -- EDIT THE PATHS FIRST if
      your install is elsewhere, then double-click to import.

Both target the 32-bit registry view (WOW6432Node). That is intentional: the
game opens the key with KEY_WOW64_32KEY even though the exe is 64-bit. Do not
"correct" it to the native path or it will not find its data.

Nothing here deletes or edits your 4.0 key.


LICENSING
---------
See THIRD-PARTY-NOTICES.txt and ./licenses/. FreeFalcon itself is BSD 2-Clause,
which permits binary redistribution with the copyright notice and disclaimer
reproduced (both are included).
