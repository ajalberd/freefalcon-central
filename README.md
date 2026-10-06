# FreeFalcon — FFViper

A campaign-based, multiplayer, open-source flight simulator in the Falcon 4.0 line — rebuilt on a
modern engine, flown in VR, and given a campaign that actually fights a war.

![The campaign editor on Korea's save0, showing the railway network: the North and South Korean main
lines and the Chinese and Russian lines that cross into them](docs/images/campaign-editor-rail.jpg)

*The campaign editor (`tools/campaign-editor`) on Korea's opening campaign, with the railway layer:
21 routes from OpenStreetMap, from Shenyang and the Russian border down to Busan.*

FreeFalcon was once a project to mod the original MicroProse Falcon 4.0. FFViper is the fork that took
it from there. Everything below is new since the fork; [CHANGELOG.md](CHANGELOG.md) has the full list,
and the topic write-ups linked from each section have the details and the reasoning.

## Highlights

### A modern renderer — Direct3D 12 and Vulkan, 64-bit
The fixed-function Direct3D 7 engine is gone. FFViper renders through **native Direct3D 12 and
Vulkan** behind one backend-neutral interface, from **one set of HLSL shaders** compiled to DXIL and
SPIR-V and baked into the executable, as a **64-bit** binary.
- **GPU terrain** with mesh shaders: a clipmap streamed and culled on the GPU, one dispatch for the
  ground, seamless LOD connectors.
- **Reversed-Z depth**, bindless textures, MSAA and anisotropic filtering on both backends.
- **HDR scene with Gran Turismo 7 tone mapping**, a sun that follows the time of day, MFD glare,
  cockpit shadows fitted to the pilot's eye.
- **Sensor video**: TGP, Maverick and LANTIRN render a real 3D scene into the MFDs; **NVG** greens
  the whole world with tube gain, grain and vignette.

See [RENDER-LIGHTING.md](RENDER-LIGHTING.md), [OBJECT-RENDERING.md](OBJECT-RENDERING.md),
[WEATHER-FRONTS.md](WEATHER-FRONTS.md).

### VR through OpenXR
The goal the render work was for. Full **6DoF stereo** on D3D12 and Vulkan, **single-pass** (view
instancing / multiview), **quad views and foveated rendering** where the headset offers them.
- A cockpit made for the headset: a collimated HUD at infinity, render-to-texture displays, a
  **3D kneeboard** on the thigh with the route map and briefing pages.
- Menus, comms and AWACS panels on XR layers with a pointer; touch controllers with hand tracking;
  screenshots taken from the eye.

### Cockpit, systems and controls
- **A clickable 3D cockpit** that is data-driven, with a working **ramp start** (an animated JFS with
  its own start and loop audio), **nosewheel steering**, differential brakes and indicator lamps that
  read their own systems. Engine, canopy and switch sounds per aircraft. See
  [COCKPIT-OVERHAUL.md](COCKPIT-OVERHAUL.md).
- **A new controls system**: a DCS-style function x device table, XML bindings keyed to device
  GUIDs so they survive replugging, and **per-pilot profiles** — controls and a readable logbook.
- Menus that fill any window size at any DPI ([UI-OVERHAUL.md](UI-OVERHAUL.md)).

### A campaign that fights a war
The dynamic campaign looked alive but its ground war was stuck. Measured with a headless simulator
(below), the root causes were bugs, not balance — and they are fixed:
- **Ground units could not move south or west.** `SimToGrid` rounded where it should floor. One-line
  fix; the front moves now.
- **Supply stopped on day one.** Squadrons' out-of-table stores made the team's summed need negative,
  which skipped all battalion resupply from hour 12; and ground and air now draw separate shares.
- **Routes gave up.** Paths cap at 96 steps and a longer one's partial route was thrown away; units now
  follow it.
- **Fronts froze short of Pyongyang.** Capture orders only targeted secondary objectives within three
  links of the front; with `GtmCaptureFront`, Blue reaches Pyongyang in 8 of 8 runs instead of 2.
- **Reserves drove backwards**, DPRK never counter-attacked, alert flights never scrambled — each
  traced and switchable.

Every fix is a config switch, so stock behaviour is one line away; the ground-AI fixes are on by default. Around them: package planning from a
right-click, logistics overlays (power, supply, production, damage), a FLOT line, supply interdiction
and damage-aware routing, naval stations and wrecks on the map, campaign music that follows the war.
See [CAMPAIGN-SUPPLY-ENGINE.md](CAMPAIGN-SUPPLY-ENGINE.md).

### campsim and the Campaign Lab
`campsim` runs the **real campaign AI headless** — days of war in seconds, many seeds in parallel —
with knobs for every switch and a timeline of every unit, objective and supply flow. The **Campaign
Lab** (`tools/campsim/serve.py`) starts runs, overlays their charts and replays them on a map. Every
campaign change above was measured in it before it shipped. See [tools/campsim/README.md](tools/campsim/README.md).

### Korea Escalation
A JSGME campaign mod in which **China and Russia actually join the war**: staged forward, cloned to
full strength, a Russian Pacific Fleet off Wonsan, DPRK bombers, Blue air cut to Falcon 4.0 numbers,
and a war that ends only with Pyongyang *and* Wonsan. See
[tools/campsim/KOREA-ESCALATION.md](tools/campsim/KOREA-ESCALATION.md).

### Railways — Korea Rail War
*On the `rail-tracks` branch, shipped as the JSGME mod "Korea Rail War".*
No Falcon theater ever had a railway. Korea now has one: **21 routes from OpenStreetMap**, fitted to the
terrain — both Koreas' main lines, China's lines into Sinuiju and Manpo, Russia's into Tumangang —
drawn on the campaign map and in 3D.
- **Supply trains** are real units: they run a timetable from a supply hub to the railhead, deliver
  from the national pools, and can be spotted, bombed and killed.
- **Troop trains**: any battalion with a long march takes the train when that beats the road
  (a median 164 km trip: 8 h by rail against 24 h on foot). Riders stay on the map and in the sky's
  sights; ground contact makes them get off.
- **Rail mobilisation**: when China and Russia join, their armies ride from Manchuria to railheads
  behind the front — over the Yalu bridge at Sinuiju, or Ji'an–Manpo.
- **71 rail-bridge objectives** on the long river crossings: drop one and the line is cut, trains
  stop short and troops get off and march. Repair reopens it.

See [RAIL.md](RAIL.md).

### The campaign editor
`tools/campaign-editor` is an **external, browser-based editor** for theaters and campaigns (a
dependency-free Python server): units, objectives, squadrons, victory conditions, weather, the class
tables, cloning a theater into an era variant — every file format transcribed from the engine and
checked byte-for-byte against the shipped data. On `rail-tracks` it also builds the OSM railways,
flattens uneven airstrips in the terrain and moves objectives. See [CAMPAIGN-EDITOR.md](CAMPAIGN-EDITOR.md).

### Also
- **Native Linux build** — CMake + clang, an SDL3 and OpenAL platform layer, Vulkan and OpenXR; no Wine.
- **Stability**: the long-standing campaign heap corruption, 3D enter/exit leaks and hangs, a
  render-vs-sim use-after-free, a hang watchdog that writes a dump.
- **A WiX patch installer** for an existing FreeFalcon 6 install.

## Build Instructions

This is only a summary of the requirements and assumes a knowledge of your
way around installing libraries on your system. For more detail, see
[our documentation](https://github.com/FreeFalcon/docs).

FreeFalcon currently requires Visual Studio 2026 community edition. You will 
need Windows SDK and DirectX installed with Visual Studio and Kronis OpenXR 
Headers and Loader packages installed via nuget package manager.

To load the Installer project, you'll need the latest version of the WiX
Toolset, which can be found [here](http://wixtoolset.org/). Without this, you
will get an error when loading the solution, but you will still be able to
build the FreeFalcon source code- you just won't be able to package it into
an installer.

To set up the source code to run in Debug mode, you'll need an install of
FreeFalcon on your computer. To tell Visual Studio where this installation is,
right click on the FFViper project and select Properties from the dropdown.
Under Configuration Options in the window that pops up, select Debugging and
edit the Working Directory to the root of your FreeFalcon install.

To compile the code, do a build on the solution or the FFViper project. To run
the code, run the Debug target of the FFViper project. You should set FFViper
as the startup project in the solution's settings so that this is done
automatically. Once you've built the code once, you usually won't have to do
full rebuilds; just run the Debug target again on FFViper and it should build
any changes. If you pull down changes from GitHub, you may have to rebuild the
projects that were changed.

Also, if you are planning on sending patches, be sure that you set Visual
Studio to use spaces instead of tabs! This setting is located at
Tools -> Options... -> Text Editor -> C/C++ -> Tabs, select "Insert spaces".

## Adding new resources to the game directory

Inside installer folder there's res subfolder that contains new textures, setup pages
and other files needed to run the game correctly. copy contents of that folder into
the game folder.

## Building the Installer

For patch installer build run build_patch.cmd from installer directory. It will 
take exe from game dir and other files from res subfolder inside installer dir.

## Contributing

You can contribute to the project in various ways.

 * If you're a programmer, go ahead, fork us and hack away! If you feel that
   you have something good going, send a pull request and we'll talk about
   getting your code into the main source tree.
 * If you're not a programmer, not to worry! Just let us know if you find any
   bugs or have any suggestions in the issue tracker.

## Legal

FreeFalcon doesn't have the cleanest history regarding its terms of use, but
that all ends here. The code is now licensed under the rather liberal BSD
2-clause license; for the first time in its history, FreeFalcon is truly free.
See the LICENSE.md file for the full text of the license.
