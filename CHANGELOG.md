# FFViper — what this fork adds over base FreeFalcon

FFViper is a fork of **FreeFalcon** (Falcon 4.0 lineage). The old fixed‑function Direct3D 7
engine has been replaced by modern renderers — first D3D11, now **native Direct3D 12 and
Vulkan** — and the VR goal that drove the render work has landed: the sim flies in a headset
through **OpenXR**. Around that sit a modernized toolchain and an x64 build, a rebuilt
controls/input system, per‑pilot profiles, a native Linux build, and a long list of gameplay,
stability and audio fixes.

Everything below is relative to upstream FreeFalcon.

---

## 1. Render engine (D3D7 → D3D11 → D3D12 / Vulkan)

The entire fixed‑function D3D7 pipeline was replaced with a D3D11 backend.

* **D3D11 backend & device bring‑up** — new `D3D11Backend` (swapchain, back buffer, depth,
  feature level 11.0), `D3D11Renderer`, `D3D11GeometryBuffer`, `D3D11TextureManager`.
* **Fixed‑function emulation in HLSL** — `FFEmu.hlsl` reproduces the legacy render states
  (texture stages, chroma‑key, fog, alpha test, gouraud, material/emissive/specular) via a
  state map; the screen path (`ContextMPR`) and the object path both funnel into it.
* **Render‑to‑texture cockpit displays** — HUD, DED, RWR, MFDs render to a texture atlas and
  are composited onto the 3D cockpit panels; text/line symbology and colors fixed.
* **MSAA** for the 3D scene (resolve to back buffer, with fallback).
* **Textures** — DXT/NVTT pipeline, palette textures re‑baked on palette/TOD change, cockpit
  BSP textures, world‑object SRV binding.
* **Terrain** — fixed black ground (day/night multitexture, perspective), far‑tiles fog
  transition, pragmatic water shimmer, near/far draw‑distance tuning.
* **Lighting & effects** — object material/lighting (no longer flat‑white), dynamic lights
  (muzzle/explosion flashes), explosions/smoke/tracers/particles, depth‑test fixes so effects
  aren’t culled, objects no longer draw through terrain (depth/occlusion).
* **Afterburner** — reworked as additive emissive surfaces with a warm day/night gradient (closer
  to a real F‑16 plume) instead of the dim, washed‑out original.
* **Resolution & window** — settings‑driven resolution incl. widescreen modes (720p/1080p/2K/4K),
  windowed‑mode toggle, rendered mouse cursor.
* **3D cockpit** — camera orientation fixes (mirror/back‑facing), cockpit texture architecture
  on the 3D path (the dead 2D cockpit path is retired).
* **Legacy D3D7 removed** — the dead Direct3D 7 / DirectDraw code paths were stripped out across the
  engine (state machine, light/VB managers, contexts) and the live ones ported to D3D11; D3D7 is no
  longer used or executed at runtime.

### Native D3D12 and Vulkan

D3D11 was in turn retired: it served as the reference while the two modern backends were brought
up, and its code is now gone. Both live behind one backend‑neutral `IRenderer` / `IRenderBackend`
interface, and the renderer is picked in the graphics options.

* **Direct3D 12 backend** — own device/swapchain bring‑up, PSO cache, descriptor ring, per‑frame
  constant/vertex/index ring allocators, texture uploads on the copy queue, and DRED‑assisted
  device‑removal diagnostics. It is the default renderer.
* **Vulkan backend** — device/swapchain bring‑up on VMA, render passes for the scene, off‑screen
  RTT and menu targets, deferred resource destruction (freeing an asset under a live frame used to
  hang the GPU), and optional validation/sync‑validation layers behind config knobs.
* **Bindless textures** in both backends — D3D12 through `ResourceDescriptorHeap` (SM 6.6), Vulkan
  through descriptor indexing, with slots recycled when a texture is destroyed.
* **Reversed‑Z depth** and a D32 depth buffer, which is what finally stopped distant terrain from
  z‑fighting and "swimming".
* **MSAA** and anisotropic filtering on both backends; **off‑screen RTT** cockpit displays,
  sensor video and the 3D model viewer all render through the same neutral path.
* **x64** — the engine builds and runs as a 64‑bit binary (registry, file and asm paths ported).

### One shader language, compiled into the binary

* Both backends now build from the **same HLSL sources**. DXC compiles each entry point twice —
  DXIL for D3D12, SPIR‑V for Vulkan — from one manifest, so the two backends can no longer drift
  apart the way separate HLSL and GLSL sets did.
* The bytecode is **baked into the executable**; no `.hlsl` or `.spv` files ship next to the game,
  and a shader edit cannot silently ship stale bytecode.

### Terrain

* **GPU terrain** replacing the CPU‑built grid, with LOD "connector" seams (one shared surface
  between LODs instead of overlapping layers, so no z‑fight and no edge shimmer).
* **Mesh‑shader terrain** (D3D12 and Vulkan): a toroidal clipmap streams elevation posts and tile
  slots on the GPU, amplification shaders do the LOD pick and frustum cull, and one dispatch draws
  the ground. Falls back to the classic path when the hardware or the knob says no.
* Fixed black ground, far‑tile fog transition, and the day/night ground level — including the
  sensor pass, which used to read the TV/IR lamp as a black sun and drive the ground to night.
* **Ring-edge stale posts fixed** — the clipmap only uploaded the window's far edge and an 8-row rolling
  re-scan, so when a detail ring stepped forward its new outermost column was never uploaded and its quads
  dropped for ~50-100 ms: a line at every ring border that flickered and moved with you. The strips that
  enter the band are now uploaded at once. Tile seams are gone too (clamped terrain sampler and
  wrap-corrected gradients). Diagnostics: `[TERRAIN-HOLE]`, `TerrainCrackDebug`, `TerrainMeshDebugTint`.
  See [TERRAIN-AND-FILTERING.md](TERRAIN-AND-FILTERING.md).
* **View distance that matches what renders** — the world far plane (85 km) is now `FarPlaneKm`; the haze
  is solid before it, terrain rings are capped to it, and the Setup sliders reach it without `-G`. Turning
  your head no longer makes far ground pop in and out when the draw distance is stretched.

### Texture filtering

* **Mips built in linear light**, colour weighted by alpha (no chroma-key fringes), and **large DXT atlases
  (cockpit/object, > 512 px) now get mip chains** — mip 0 stays compressed as shipped, lower mips are
  re-encoded by a small built-in BC1/BC2/BC3 encoder; 16- and 24-bit textures get mips too.
* The mesh terrain's own sampler now honours the anisotropy option and `MipLodBias` (it was fixed 8x WRAP).
* **Setup > Graphics > Advanced**: an Anisotropic Level slider (2x-16x) and a Mip Bias slider (0 to +2).

### Sensor video and night vision

* **TGP / Maverick / LANTIRN** render a real 3D scene into the MFD texture atlas on every backend,
  not just symbology, with the sensor zone scissored so the picture cannot splatter over the
  HUD/DED, and a reduced terrain radius so the zoomed sensor does not flood the command list.
* **NVG** greens the world through the object, terrain and screen shaders alike, with tube gain,
  scanlines, grain and vignette, and a night ground level lifted so the goggles show a lit scene.

### Lighting, HDR and tone mapping (D3D12)

* **FP16 scene + Gran Turismo 7 tone mapping** — the 3D scene renders into an
  R16G16B16A16_FLOAT target and Polyphony's GT7 operator (MIT reference, ICtCp) runs once at
  the 3D→2D boundary, before the HUD and cockpit instruments, so glows, lamps, specular and the
  afterburner roll off instead of clipping while the overlays stay untouched. Pipelines follow
  the bound target's format. Knobs: `ToneMapGT7`, `ToneMapExposure`. Staged: lighting is still
  LDR‑authored, Vulkan and view instancing keep the 8‑bit path.
* **Sun colour from the time of day** — the object/terrain sun takes the TOD table's hue (orange
  at sunset) and fades as the sun sets; it used to be white at every hour. Knobs: `SunTodTint`,
  `SunTodDimRef`.
* **MFD sun glare** — sunlight on the MFD glass washes the display out in direct sun and not
  inside the cockpit shadow, per pixel via the pit shadow map. Knob: `MfdGlare`.
* **Cockpit shadow fit** — the shadow map was fitted to a stale model bounding box that left the
  glare shield, panel and MFDs outside it; it now fits a box around the pilot's eye (sharper, too).
  Knob: `PitShadowFitReach`.
* **CPU near clip 1.0 → 0.2 ft** — matches the GPU near plane, so CPU‑drawn 3D primitives
  (display quads, cursors, trails) within a foot of the eye are no longer pushed out and folded.
  Knob: `CpuNearClip`.

## 2. VR (OpenXR)

The long‑term goal of the render work, now shipped.

* **OpenXR session** with stereo rendering and full 6DoF head tracking, on D3D12 and Vulkan.
* **Single‑pass stereo** — view instancing on D3D12, multiview on Vulkan — so the world is drawn
  once for both eyes; **quad views** and foveated rendering are supported where the runtime offers
  them (Varjo/Pimax), with the focus pair driving anything resolution‑dependent.
* **Cockpit in VR** — RTT displays, HUD through a proper collimator (the symbology sits at
  infinity instead of painted on the glass), canopy, and a head‑pan limit.
* **3D kneeboard** — the pilot's kneeboard on the right thigh, on its own render‑to‑texture
  canvas: map with the route drawn on it, briefing and steerpoint pages, clickable to cycle,
  opaque and correctly occluded by the cockpit. Placement, size and blend are data
  (`3Dckpit.dat`), and it has its own font size knob for headset legibility.
* **Screenshots in VR** — captures read the rendered **eye** instead of the desktop back
  buffer, which in a VR session only ever holds the clear. Both the normal and the "pretty"
  key work in the headset.
* **VR interface** — the 2D menus, comms and AWACS panels are composited onto XR quad layers with
  a pointer ray and a 3D mouse cursor; the in‑3D menu and the exit dialog work in the headset.
* **Touch controllers** — models, click/pointer input, a ring menu, and skeletal hand tracking
  with orientation‑based skinning.
* **Correct headset gamma** — the eye images are handed to the runtime with the exact bits the
  monitor gets. The Vulkan path used to blit into the sRGB swapchain image, which encoded gamma a
  second time and washed the headset out; it now decodes in the copy shader to cancel it.

* **D3D12 frame profiler** — `[GPUPROF]` (GPU ms per pass from timestamp queries, GPU busy %, CPU wait on the
  GPU) and `[FRAMEPROF]` (the sim thread's CPU frame split, slow-frame counts, the worst frame) in
  `FFDebug.log`; the Seoul measurement showed the headset path is not GPU-bound.
* **VR desktop mirror** drawn at the desktop's scale instead of 1:1 real pixels.
* **MSAA in the headset** (`VrMsaaSamples`, off by default; 4x costs about +0.2 ms/eye and ~0.5 GB VRAM on a
  5080) and **alpha-to-coverage** for alpha-tested cut-outs (windows, fences) when the target is multisampled
  (`AlphaToCoverage`). The per-eye path was single-sample before, so building edges crawled with head
  micro-motion even when paused.

## 3. Modernized toolchain

* Migrated math from legacy **D3DX → DirectXMath**.
* Builds against a **modern Windows SDK** (packing/macro/intrinsic fixes); the x64 port that
  groundwork was for is done.
* **NVTT** texture tooling.
* All in‑repo source comments translated to **English** (the fork is pushed to a public repo).

## 4. New controls / input system

* **DCS‑style controls window** — a function × device table replacing the old per‑category list,
  with categories and BMS‑style labels.
* **XML‑based control config** — `config\controls.xml` catalog (offline‑generated) and runtime
  `keyboard.xml` / `<GUID>.xml` / `axismapping.xml`, replacing `keystrokes.key` /
  `axismapping.dat` at runtime.
* **GUID‑stable bindings** — axes and buttons are keyed to the device GUID, so they survive
  device re‑enumeration / re‑plugging.
* **Button‑assignment window** with Cyrillic UI text rendered through GDI in `ui95`, auto‑detect
  of pressed buttons, a stuck‑button guard, a “clear” column, in‑window function search/filter,
  and a restored list scrollbar.
* Modern **DirectInput** devices, scroll‑wheel functions, pitch/roll split out into the axes
  window.

## 5. Per‑pilot profiles (controls **and** logbook)

* Each pilot owns `config\profiles\<dir>\` holding `keyboard.xml`, `<GUID>.xml`,
  `axismapping.xml`, `logbook.xml`, `stats.plc`, `options.pop`, `rules.rul`.
* `config\profiles.xml` maps callsign → numbered dir (default pilot = dir 0 / folder `default`)
  and records the active pilot; the last selection persists across launches.
* The **logbook moved out of the encrypted `.lbk`** into readable `logbook.xml` (legacy `.lbk`
  is still read once and migrated). Player options and rules likewise live in the profile.
* Pilot list, rename and “new pilot” all operate through `profiles.xml`; nothing per‑pilot is
  written to `config\` the old way anymore.

## 6. Gameplay & QoL

* **Instant Action / Dogfight start-time clock** is visible again on a wide menu surface (its clip no longer
  stops at the stock width); the **intro movie is removed**; `BubbleScale` stretches the sim bubble past the
  Setup slider's 2x, and `[BUBBLE]` logs the real ranges per unit type.
* **Ramp start actually works** — the JFS switch is drawn and animated in the 3D pit (it was
  modelled all along, just never given a draw mask), its green run light reads the JFS bit
  instead of the engine overheat lamp, and sixteen avionics levers follow the switch the pilot
  moved rather than whether the bus behind it is live.
* **Nosewheel steering** — FF6 had no NWS control of any kind; there is one now, and the
  AR/NWS caution lamp doubles as an indication that steering is engaged.
* **Cockpit indicator lamps** — the EPU, ECM and ELEC panel lamps read their own status bits;
  several were wired to the wrong word and lit on unrelated conditions.
* **Instant Action**: unlimited ammo / chaff / flares (independent of options); fixed a weapon
  count leak that blocked missile launches.
* **MRM / AIM‑120**: launches in maddog (MRM) as it should; HUD MRM redesign (wider circle,
  speed/altitude inside, BMS‑style).
* **Differential braking** (per‑wheel brakes + steering).
* **Audio**: restored radio chatter / ST80 voice codec.
* **Engine sound curves.** Every shipped aircraft carries a one‑point volume chart
  (`sndIntChart 1 0 1`) — full volume at every rpm — and a pitch chart that runs the loop at about a
  third of its speed while the starter cranks the engine over. That is why a cold start grinds and
  why the in‑cockpit engine sits so loud. Wherever a chart is still that placeholder, the reference
  curves BMS uses for its own F‑16 are substituted (`readin.cpp`), and the second internal layer FF6
  already ships for the job (`EngRumbleInt.wav`, slot 278) is finally handed to the aircraft — it
  was named by nothing and set by no aircraft's data, so it had never played. Knob:
  `EngineRumbleLevel`.
* **JFS starter audio.** The jet fuel starter has start and loop recordings now — an external and an
  in‑cockpit pair per phase, selected by the sound system's own flags rather than by view code, and
  driven from the airframe's JFS state machine (set, cranking, and the four ways it stops). Voiced
  for the player's jet only: the AI run the same preflight, and a ramp of them lighting their
  starters together was a chorus of identical motors, not ambience.
* **Canopy muffling.** The extra attenuation a closed canopy applies — previously fixed at the
  aircraft's own `sndExternalVol`, which every aircraft sets to the same value — can be dialled.
  Knob: `CanopyAttenuation`.
* **RWR.** The scope's font is selectable (`RwrFont`); BMS draws its RWR with the 10x7 bitmap, which
  is now one knob away. The scope colour has a default at last, so a pit whose data carries no
  `rwrcolor` line no longer draws black symbology on a black scope.
* **Radio/comms menu size.** The popup menu scales as a whole — box, glyphs and line spacing
  together, because the box and the spacing both fall out of the viewport while a glyph is drawn at
  its font's pixel size. Knob: `MenuScale`; VR keeps its own `VrMenuScale`.
* **Cockpit switch sounds.** The clickable 3D pit is data-driven — every hotspot names its own sound
  in `art\ckptart\3dbuttons.dat` — and the F-16's file spread all 282 hotspots over just **five**
  recordings, so every console switch clicked with the same file. Controls that shared a click now
  have their own: trim wheel, RWR buttons, seat arming, the guarded switches, the rotary knobs, the
  battery, alternate gear, IFF, the jettison switch, the push buttons and the ECS. The ids are
  referenced by number from the `.dat` (289 and up), so no enum name is needed — see the table below.
* **Canopy loop per direction.** FF6 had a single canopy loop for both opening and closing; BMS ships
  separate ones. `sndCanopyOpenLoop` / `sndCanopyCloseLoop` default to the two new table entries, so
  no aircraft file has to change, and `AircraftClass::MoveDof` picks by `canopyState`. The old
  `sndCanopyLoop` field is kept for compatibility.

### Sounds these features expect

The source tree cannot carry the audio: the sound data lives in the install. A stock data set still
runs — a sound id past the end of the table is ignored rather than fatal — but these slots stay
silent until files are added:

| id | file, relative to the sound directory | used for |
|---|---|---|
| 283 / 284 | `jfsstart.wav` / `jfsstartint.wav` | JFS crank, external / in‑cockpit |
| 285 / 286 | `jfsloop.wav` / `jfsloopint.wav` | JFS running, external / in‑cockpit |
| 287 / 288 | `jfsend.wav` / `jfsendint.wav` | JFS spool‑down (ids reserved; nothing plays them yet) |
| 278 | `engines\fighter\f-16\EngRumbleInt.wav` | second internal engine layer (ships with the data) |
| 289–302 | `cockpit\*.wav` | cockpit switches, one per control: `altgeardown`, `bat`, `iffmode4monitor`, `jettisonswitch`, `pushbutton`, `rtryknob`, `rwrbutton`, `seatarm`, `toggsafe1`, `toggsafe2`, `trimwheel`, `ecsstart`, `ecsloop`, `ecsend` |
| 303 / 304 | `canopyopenloop.wav` / `canopycloseloop.wav` | canopy loop while opening / closing |

Table entries use the same twelve tab‑separated columns as every other line of
`<sound dir>\f4sndtbl.txt`, with the flag letters documented at the top of that file (`E` external,
`I` internal, `L` looped, `H` high priority):

    <file> <offset> <length> <maxDist> <minDist> <maxVol> <minVol> <flags> <pitchScale> <soundGroup> <linkedSound> <unused>

The id **is** the table index, so the JFS lines append at 283..288. The default table's 280..282 were
unused; they are padded so that the default table and the Israel theater's — which is three entries
longer — agree, because the same enum indexes both. The wav files themselves are not in this
repository and are not produced by the installer; the ones used here were decoded from a BMS install
and level‑matched to the files they replaced.

## 6b. Campaign — planning and the map

* **Build a package by right‑clicking a target.** A submenu of the squadrons that could
  actually fly a mission against what you clicked, nearest first, with a two‑ or four‑ship
  choice. Which squadrons are offered is the engine's own judgement (`GetMissionFromTarget`),
  and squadrons that can engage the target sort above ones that can only bring a generic
  sortie. Knob: `CampaignAddMission`.
* **Add Package works in the campaign.** The full planning dialog — flight tree, per‑flight
  targets and roles, takeoff / time‑on‑target with locks — which previously did nothing on
  any screen, the Tactical Engagement editor included, because nothing loaded `PACKAGE_WIN`'s
  window art. Knob: `CampaignPackageWindow`.
* **Hand‑built flights are scheduled against the right hour.** `FindAvailableAircraft` walks
  the ATM's 32‑block schedule, and nothing on the hand‑built path had ever set which blocks
  to ask about — every flight asked about block 0 regardless of its takeoff time. Fixed, and
  a request that cannot be crewed now slides to the next block that can rather than being
  refused.
* **Campaign packages are pinned to takeoff, not to an exact time on target.** The window
  opened demanding the flight be over the target at precisely the displayed second, which is
  right when authoring a scenario and wrong in a running war. Knob:
  `CampaignPackageTakeoffLock`.
* **The player's team comes from the session**, not from the Tactical Engagement editor's
  `gSelectedTeam`, which the campaign never kept in step.
* **Logistics map overlays** — power coverage, supply flow, production and target damage.
* **FLOT line** — the forward line of own troops, drawn from the campaign's own `FLOTList`.
  Knob: `CampFlotLine`.
* **Campaign map built from terrain**, with zoom detail.
* **Supply interdiction** — a damaged bridge or road costs the supply run crossing it,
  scaled by objective status so repair re‑opens the route. Knob: `SupplyInterdiction`.
* **Damage feeds route cost** — pathfinding charges more to enter a damaged road, junction, rail
  node (up to ×2) or bridge (up to ×4), scaled by objective status, so columns and supply route
  around wrecked crossings instead of only paying for them. Knob: `PathDamageCost`.
* **Objective icons darken with damage** — a flattened target reads at a glance without the
  damage overlay (and objective icons now actually track their status). Knobs:
  `CampMapIconHealth`, `CampMapIconMin`.
* **Add Squadron hidden in campaigns** — it is Tactical Engagement / campaign-editor work.

## 6c. Campaign AI — the ground war, measured

All found and checked with **campsim** (`src/tools/campsim`, `tools/campsim`): the real campaign code
run headless, days of war in seconds, with a JSONL timeline per run and the **Campaign Lab** web UI
(`serve.py`) to launch, chart and replay runs. Each fix is an `FFViper.cfg` switch; the ground-AI ones
are on by default, and `0` restores stock.

* **SimToGrid floors** (`SimToGridFix`) — it rounded, and ground units could not move south or west.
* **SupplyNeedFix** — squadrons' out-of-table stores made the team's summed supply need negative and
  stopped all battalion resupply from about hour 12. **SupplySplitShares** gives ground and air
  separate shares of the pool, so never-used squadron stores no longer dilute battalion deliveries.
* **GridPathPartial** — grid and objective paths cap at 96 steps; a failure threw the partial route
  away and the unit retried forever. It now follows the partial route.
* **GtmCaptureFront** — capture orders only went to *secondary* objectives within three links of the
  front, which froze the war short of Pyongyang (2 of 8 runs reached it; 8 of 8 with the fix).
  **GtmCaptureBestScore / CaptureMaxKm** keep far-sent units from driving across the whole front.
* **ReserveHold / ReserveNoPullback** — reserves stop hopping back three links every check, and a unit
  whose target a neighbour took holds it instead of driving ~49 km back.
* **GtmReserveFix** — the reserve step kept the first candidate instead of the closest.
* **AlertScramble, InitTrueLosses, CounterAttackInitiative, CaptureInitiative** — alert flights
  actually scramble, and DPRK gets the initiative to counter-attack instead of staying defensive
  all war.
* **NavalMoveFix** and naval AI legs, player ship stations, ship ghosts and wrecks on the map.
* Fixed: duplicate `AllUnitList` entries, arrived reinforcements left in the inactive list,
  battalions losing their reinforcement hour on save/load, a request-list use-after-free, an
  `SelectAirActions` crash with no front left; the event history is kept in the save.
* **Korea Escalation** (JSGME mod, `tools/campsim/make_escalation_mod.py`, documented in
  `KOREA-ESCALATION.md`) — China and Russia staged forward and cloned, a Russian Pacific Fleet, DPRK
  bombers, Blue air cut to Falcon 4.0 amounts, Pyongyang *and* Wonsan to win.
* Campaign screen **music follows the war**; the intel bar charts plot what their labels say.

## 7. Stability (corruption / hangs / crashes)

* **Campaign teardown heap corruption** — `O_Output` sized its scale buffers from the first
  image it was ever given and then kept them for every later, larger one, writing off the end.
  Long‑standing; it surfaced as a "double free" far from the damage.
* **Black stripe at the horizon at altitude** — the 3D skydome replaced the 2D sky wholesale,
  including the band that covered the near/far terrain seam. The dome now inherits that job.
* **Object‑list lifetime overhaul** — recursive lock serializing render vs sim/campaign access,
  idempotent insert, out‑of‑line `DrawableObject` destructor that unlinks on delete, SEH‑guarded
  draw/update callbacks; fixes the use‑after‑free (0xDD), self‑cycle OOM, and enter/exit‑3D hangs.
* **Release‑only 2D‑vanish bug** — root‑caused to an ODR struct‑packing mismatch
  (`#pragma pack` leak across translation units) and fixed by pinning the layout of the
  render‑context headers.
* Fixed the **memory leak on 3D enter/exit** (`std::bad_alloc` on repeated entry), the
  null model‑node load hang, the padlock (F3) crash, and dynamic‑campaign enter/exit hangs.
* **Menu 3D‑viewer black‑out** — the UI model viewer (Munitions, etc.) drew straight to the D3D11
  back buffer and flipped Present into the in‑sim compositing path, blacking out the menu; it now
  renders to an off‑screen target and is read back into the 2D menu surface.

## 8. Installer

* **WiX patch installer** that overlays the updated `FFViper.exe` + changed data assets
  (shaders, config, default profile, edited art) onto an existing FreeFalcon 6 install, with
  registry auto‑detection of the game folder.

## 9. Linux

The fork runs natively on Linux — no Wine, no translation layer. Vulkan is the renderer there,
and the same OpenXR path gives VR.

* **CMake + clang build** of the whole tree, reading the source lists straight out of the Windows
  `.vcxproj` files so the two builds cannot drift apart.
* **win32 foundation shim** — `<windows.h>` and friends reimplemented for the engine (handles,
  sync, files, strings, time, COM stubs), with an **SDL3** window/event/input layer and **OpenAL**
  in place of DirectSound.
* Path separators and file lookups made case‑ and separator‑tolerant, since the game data was
  authored on a case‑insensitive filesystem.

## 10. Railways (`rail-tracks` branch, JSGME mod "Korea Rail War")

No Falcon theater ever had rail: no rail cells, no rail objectives, no train class, and the engine's
rail pathing was dead code. All of this is in [RAIL.md](RAIL.md).

* **The network from OpenStreetMap** (`osm_rail.py`, `add_rail_lines.py`) — fitted to the terrain, not
  to Falcon's lat/long (which is 45–195 km out in Korea): 21 routes, the Korean main lines plus China
  (Shenyang–Dandong over the Yalu into Sinuiju, Meihekou–Ji'an into Manpo) and Russia (Khasan into
  Tumangang), with bridge and tunnel flags, kept off open sea and routed round airfields.
* **Supply trains** (`RailTrains`) — Supply battalions flagged as trains, running a timetable that is a
  pure function of game time between a supply hub and a railhead short of the front; they deliver
  from the national pools and can be spotted, struck and killed.
* **Troop trains** (`RailTroops`) — a battalion about to march takes the train when walk + entrain +
  ride + line changes + detrain beats the road by 25%; it stays a normal unit, rides in 3D as a string
  of cars, and gets off for a ground foe within 5 km. campsim: a median 164 km trip, 8.3 h by rail
  vs 23.6 h on foot.
* **Rail mobilisation** (`RailWave`) — when China or Russia joins, its armies far from the front are
  ordered by rail to railheads 20–60 km behind it, in their own pool of trains. On stock save0 China
  otherwise never moves (the GTM never orders it); with it, Chinese battalions ride from Manchuria to
  the front in 15–28 h.
* **Bridges cut the line** (`RailBridgeCuts`) — each long rail bridge binds to a bridge objective, and
  **71 new rail-bridge objectives** (`add_rail_bridges.py`) cover the long crossings that had none,
  the Yalu at Sinuiju included. A dropped span stops trains short and makes troops detrain.
* **Drawn** on the campaign map (Rail lines layer, bridge markers) and in 3D (a ballast strip with
  rails, bridge decks, tunnels; cars ride the rail top).
* **Shipped as a JSGME mod** built by `tools/rail-mod/make_rail_mod.py`, with the engine reading
  `config\mods\*.cfg` after `FFViper.cfg` so a mod can carry its own settings.
* Along the way: ten Korean airstrips whose 2011 terrain was uneven flattened (`flatten_airstrips.py`,
  with rollback), Changyon Highway Strip moved off a cliff, cumulus that writes depth so aircraft no
  longer show through clouds, GPU particles drawn after the cloud quads, and campsim's game clock
  (`vuxGameTime`) advancing at last.

---

*Targeted but not yet shipped: water/surf shaders, canopy reflections/rain, ejection‑seat texture,
radar‑lock reliability, throttle/RPM sync at mission start, and a thermal model for the IR sensor
image.*
