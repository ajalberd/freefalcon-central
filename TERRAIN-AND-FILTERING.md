# Terrain, texture filtering, view distance and the frame profiler

Written 2026-10-10 from a long session on branch `rail-tracks`. Everything here was checked in the
code or measured from `FFDebug.log`; where something was only reasoned, it says so. The goal was
"is FF6 as modern as it gets?" for filtering, mipmapping and view distance, then chasing the
terrain seams and lines a headset makes obvious.

## Texture filtering and mips (D3D12)

What was there: anisotropic filtering (up to 16x, `DisplayOptions`), mips built on the CPU for terrain
tiles, palette textures and 32-bit textures up to 2048.

What changed (`src/graphics/texture/tex.cpp`):

* **Mips are averaged in linear light** (`TexBoxDown`, sRGB LUTs) and **colour is weighted by alpha**.
  The old box filter averaged raw 8-bit values (gamma space, so bright-on-dark detail faded too fast)
  and let chroma-keyed texels (alpha 0, key colour kept in RGB) bleed a blue/black fringe into their
  opaque neighbours at every level. Textures stay plain UNORM; only the filter changed.
* **Large compressed textures get mips.** DXT 513..4096 px (cockpit and object atlases) used to upload
  single-mip. Mip 0 now stays exactly as shipped (still compressed); the lower mips are decoded,
  filtered and re-encoded by a small BC1/BC2/BC3 encoder (`TexEncodeBCImage`). VRAM cost is +1/3, not
  the 4-8x of decoding to RGBA8. The encoder was round-tripped through the decoder offline (RGB error
  about 5/255). Each atlas logs `[FF] BC mip chain WxH fmt mips ms`.
* **16-bit and 24-bit RGB textures get mips** (expanded to B8G8R8A8 to carry the chain).
* One helper, `EngineTexCreateMippedRGBA`, now builds the chain for the palette, 32-bit, 24-bit and
  16-bit paths.

Samplers: the **mesh terrain has its own sampler** (`d3d12renderer.cpp`, the mesh root signature). It was
hard-wired 8x aniso + WRAP and ignored both the anisotropy option and `MipLodBias`. It now follows the
options. The object pass uses the s0/s1 pair.

### Setup > Graphics > Advanced

Two new sliders under Windowed Mode (`art\setup\advanced.scf`, `USERIDS.ID`, `lcktxtrc.irc`,
`graphicstab.cpp`, `ui_setup.cpp`):

* **Anisotropic Level**: 2x 4x 8x 16x (the checkbox beside it still switches the feature).
* **Mip Bias (x0.1)**: 0, 5, 10, 15, 20 tenths = 0.0 .. +2.0 mips. No negatives: the integer readout box
  cannot show them, and a sharper-than-native bias only brings shimmer back.

Saved in `display.xml` (`anisoLevel`, `mipBiasTenths`). A slider only overrides the cfg `MipLodBias` if it is
moved: the page remembers what it showed on open and writes back only a change
(`DisplayOptions::nMipBiasTenths`, `MIP_BIAS_UNSET` = leave the cfg alone). Adding that member means
**Rebuild All**.

## Terrain: the seams and the flickering lines

Three separate things, in the order they were found.

1. **Tile seams** (every terrain tile is its own texture). A quad on a tile's edge extrapolates its uv a
   little past 0..1; WRAP turned that into a sample from the tile's opposite edge, blended in by the
   filter, wider at every mip. Fix: CLAMP addressing on the terrain sampler, plus **wrap-corrected pixel
   gradients** in `PS_Terrain` (where one tile ends and the next begins, the uv jumps by ~1 between two
   neighbouring pixels, so the hardware picked the blurriest mip along the seam; subtracting the nearest
   whole number from `ddx/ddy` leaves the true gradient). cfg `TerrainSeamFix`: bit 0 clamp, bit 1
   gradients, bit 2 underlap (below). Default 7.
2. **Ring-boundary underlap** (bit 2). The coarser ring keeps a one-quad rim under the finer ring's edge,
   sunk by `40 + 0.03 * step` ft, so a hairline gap between rings would show ground instead of sky. It
   did **not** cure the lines (they were not gaps between rings) but is harmless and stays on.
3. **The lines that moved with you** (black near, white far; magenta under `TerrainCrackDebug`; flashing
   through the LOD tint colours). **Root cause: stale posts at each ring's outer edge.** The clipmap only
   uploaded (a) the strip at the far side of the 256-post window and (b) an 8-row rolling re-scan of the
   active band. The band had no margin past the ring (the ring edge sits at the data-availability limit),
   so when a ring stepped forward its new outermost column or row was never uploaded; the re-scan took
   ~13 updates to reach it. Quads touching a stale post are dropped, so an edge column of the ring
   vanished for ~50-100 ms every step, at every ring. Found with `[TERRAIN-HOLE]` (it stamps which
   absolute post last wrote each window texel and checks every post inside each ring: stale counts fell by 8
   per update). **Fix:** when a level's band changes, upload the strips that just entered it immediately
   (`terrainclipmap.cpp`, "the strips that just ENTERED the band"). Flown 2026-10-10: seams gone.

Dead ends, so nobody repeats them: ring sizes were steady (the `[TERRAIN-RING]` log fires only on change and
printed twice in a flight); ring boxes nest correctly (`[TERRAIN-NEST]` never fired); the chunk frustum cull
is not the cause (`TerrainNoCull` made no difference); the rail strip (`g_bRailTrack`) was suspected and is
**not** the cause; no command list is being discarded (`[LIST-DISCARD]` never fired); `ReadPost` marks
posts invalid only past `availSafe`.

**Not re-checked separately:** an intermittent whole-screen terrain dropout in the **right eye** was seen
during the debug flights (it showed as a magenta flash under `TerrainCrackDebug`). It was never explained by
the CPU side (no `[TERRAIN-SKIP]`, no frustum jumps, no discards). It may have been the same stale-edge bug
at a worse moment, or not. If it comes back, reproduce with `TerrainCrackDebug 1` and read `[TERRAIN-SKIP]`,
`[TERRAIN-FRUSTUM]`, `[TERRAIN-HOLE]`.

### Far terrain is coarse

Post spacing is 820 ft (250 m) at the finest level and doubles per LOD, so the coarsest ring (L4) has posts
~4 km apart. The stair-stepped, blocky horizon is that data, not the headset's resolution. Smoothing the
coarsest ring is open work.

## View distance: what actually limits it

* **The world far plane is `ContextMPR::ZFAR`, 280,000 ft (85 km).** Terrain, sky and objects share one
  projection (`render3d.cpp`), so anything beyond it is clipped no matter what the Setup slider asks for.
  The terrain slider reaches 40 + (6N-2)*10 km, where `N` was `GraphicSettingMult` (`-G<n>`): `-G5` asked
  for 320 km and drew 85.
* **The haze is sized from the slider**, not from the far plane. At stock settings it is solid at 72 km, just
  before the 85 km cut, which hid it. Stretched out by `-G5` (solid at ~290 km) the planar cut showed, and
  because the plane is flat while the haze is radial, ground at the edge of a wide FOV is drawn well past
  85 km straight-line while straight ahead it stops: turning your head made far ground appear and vanish.

What changed:

* cfg **`FarPlaneKm`** (default 85.34 = the old value). Set in `RenderOTW::Setup` (not `ContextMPR`'s
  constructor, which runs before the cfg is read), via `ContextMPR::SetFarPlane`. Floored at 1.25 x the sky
  dome radius, since the dome is clipped by the plane too.
* The **haze is solid by 0.9 x the far plane** (`SetTerrainTextureLevel`); stock settings already satisfy it.
* **Terrain rings are capped to the far plane** (`RingRange`): a ring asked for more used to be streamed and
  uploaded though nothing past the plane is drawn.
* The **Setup slider reach follows the far plane** (`ReadFalcon4Config`), so `-G` is no longer needed.
  `FarPlaneKm 150` gives N = 2 and a 140 km terrain slider. `-G<n>` still overrides.
* Downstream audit for a larger plane: the OpenXR `farZ` of 80000 is in a legacy D3D11 branch nothing calls;
  the screen-path depth constants are refreshed by `SetFarPlane`; the volumetric cloud pass is declared but
  never defined in this tree; reversed-Z float depth handles the larger far/near ratio.

Flown at `FarPlaneKm 150` with the slider maxed: the log's fog ran 21-126 km (it was 21-72) and FPS held.

### The bubble

Per entity type `bubbleRange_` x the session's `bubbleRatio` (Setup slider 0.5-2, number keys up to 3x
offline) x a camera multiplier decides what exists as a sim object; objectives are not scaled. cfg
**`BubbleScale`** (default 1, 0.25-8) is a local-only extra multiplier (it is not sent to other players).
The first query of each type logs `[BUBBLE] type=... base=... => N km`. Bubble cost is CPU, not GPU; a bigger
bubble means more AI simulated.

## The frame profiler (D3D12)

Only Vulkan had one. `FFDebug.log` now gets, every 5 s (cfg `GpuProf`, default on):

* **`[GPUPROF]`**: GPU time per pass per slot (`sky terrain objects 2d-screen pit-shadow tonemap hdr-out
  setup`) from timestamp queries, GPU busy % of wall, how long the CPU sat waiting on the GPU, draws,
  triangles and terrain chunks per slot, CPU `DrawScene` time and `xrWaitFrame`. A slot is one command list
  (one eye in the per-eye VR path).
* **`[FRAMEPROF]`**: the sim thread's CPU frame split (campaign wait, real-time function, sim cycle, OTW
  cycle), counts of frames over 12/20/33 ms, and the worst frame with its own breakdown.

Measured over Seoul (RTX 5080, Quest 3 over Link at 1.9x, 120 Hz): **not GPU-bound**. GPU busy was 13-22%
(~1.1-1.7 ms per eye; terrain 0.08 ms, objects 0.2-0.8 ms, ~3,400 draws and ~200k triangles per eye).
Frames ran 60-115 per second. The visible cost is that `EndEyeFrame` waits on a fence after every eye, so
the CPU records eye 1, idles while the GPU draws it, then records eye 2 and idles again (~3 ms/frame
standing still). **Overlapping the eyes was NOT done**: the next eye reuses the same command allocator, and
the renderer's per-frame constant/vertex rings probably assume the previous eye finished, so it needs
separate allocators per eye and an audit of those rings first.

## Menus

* **Instant Action / Dogfight start-time clock was clipped away.** Its bar items pin to the right of a wider
  surface, the adapter grew the window to hold them but left its whole-window clip at the old width. A
  whole-window client now grows with the window (`cadapt.cpp`).
* **The desktop window mirroring the VR menu** used to be the headset layout at 1:1 real pixels (tiny on a 4K
  monitor). It is now the same layout drawn bigger (`UI95_GetWindowSize`): the desktop's scale, or as large
  as fits with `UiWidth/UiHeight -1`. The headset panel is unchanged: `VrUiWidth 1920`, `VrUiHeight 768`,
  `VrUiHeightDeg 45`, `VrUiRadius 2.0`. (1366x768 at 60 deg was tried and filled the whole field of view.)
* **The intro movie is gone** (`FM_START_GAME` no longer sends `FM_PLAY_INTRO_MOVIE`). Switching the display
  into its movie mode fought the window/resolution setup. `-nomovie` is still accepted and ignored (the UI
  test harness passes it). Briefing and campaign movies are separate and untouched.

## Debug switches (all default OFF; restart to apply)

| cfg | what it does |
|---|---|
| `TerrainCrackDebug 1` | VR eye path: no sky dome or horizon filler, magenta eye clear, so a hole through the terrain shows magenta |
| `TerrainMeshDebugTint 1` | tints each LOD ring (red, green, blue, yellow); a quad whose morph fell back to the unblended height is painted white |
| `TerrainNoCull 1` | zeroes the frustum planes so no chunk is ever culled (costs GPU time) |
| `TerrainSeamFix <bits>` | 1 clamp, 2 wrap-corrected gradients, 4 ring underlap; default 7 |

Log lines worth grepping: `[TERRAIN-HOLE]` (stale or invalid posts inside a ring; `<== A LINE` marks a bad
row or column; silent for the first 10 s after a refill), `[TERRAIN-BOX]` (ring box moves), `[TERRAIN-NEST]`,
`[TERRAIN-RING]`, `[TERRAIN-RANGE]`, `[TERRAIN-SKIP]`, `[TERRAIN-FRUSTUM]`, `[LIST-DISCARD]`, `[BUBBLE]`.

## Open

* **VR has no antialiasing.** `BeginEyeFrame` passes one sample (`d3d12backend.cpp`, "VR eye is
  single-sample for now"); the Setup MSAA checkbox only affects the flat monitor. Building edges and
  alpha-tested (chroma-key) windows and fences, which are a hard `discard`, crawl with head micro-motion
  even when paused. The pieces for MSAA in the eye path exist (the HDR scene begin takes a sample count;
  PSOs are cached by sample count); it needs a multisampled eye depth buffer and a resolve. Alpha-to-coverage
  follows from it.
* Overlap the two eyes (above).
* Smooth the coarsest terrain ring.
* World sun shadows (only the cockpit has them), linear-light lighting, temporal AA.
