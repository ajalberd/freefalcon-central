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

* **`[HITCH]`** (flown 2026-10-10): a watcher thread samples the sim thread's call stack
  every ~20 ms while a VR frame has been running 40 ms or more (but under 1.5 s, so a menu or exit is not a
  hitch). It copies raw return addresses while the thread is suspended (no heap, no CRT, no loader lock) and
  logs them as `module+0xoffset` after resuming. `tools/profiling/hitchsym.cpp` (build line in its header)
  resolves them with the PDB built WITH that exe (`RedViper.pdb`, not `FFViper.pdb`) and tallies where the sim
  thread sat. `[FRAMEPROF]` now also splits the OTW cycle: viewpoint update, external list, `xrBeginFrame`,
  `EndStereoFrame` submit, and the desktop-mirror `Present`.

**First `[HITCH]` capture (2026-10-10, ~190 s over Seoul, 71 samples)** says where the sim thread blocks:

| Where | Samples | What it is |
|---|---|---|
| `TextureBankClass::WaitUpdates` (from `OTWDriverClass::VCock_Exec`) | 23 | every frame the virtual cockpit calls `SetTextureSet` then drains the loader synchronously; when the cockpit texture set changes, every cockpit texture reloads on the sim thread (100-240 ms; seen at mission start, 37 s and 160 s) |
| `XrWaitGated` | 14 | the runtime's frame pacing after a long frame (Link), not game code |
| `D3D12Backend::CompositeBitmap565` (from `ImageBuffer::SwapBuffers`, every 3D frame) | 10 | the whole 2D screen surface (3840x2160 = 8.3 Mpixels) converted RGB565 -> RGBA per pixel on the CPU, uploaded, then 16.6 MB cleared, for the DESKTOP window, every VR frame |
| `TexBoxDown` / `TexDecodeBCImage` / `TexEncodeBCColor` (from `TextureDB::Activate`, `TextureHandle::Load`) | ~13 | the mip chains this document added, built on the sim thread while terrain tiles and cockpit atlases load |
| `LoadTextureFile` (WIC decode), `UploadAndBarrier` | 4 | sky textures at mission start |

Changes made from it (same output, less work): the overlay composite skips rows that are empty now and were
empty last frame, uses a 64K lookup instead of per-pixel shifts, and `SwapBuffers` skips clearing a surface
that had nothing on it (`g_bUiSurfaceEmpty`); the mip box filter, the BC decoder and the BC encoder
(`TexParallelFor`) run as up to 8 chunks on worker threads (bit-identical to the serial loop, checked with a
hash over a 2048x2048 chain; ~2.5x faster). `[FRAMEPROF]` now also reports `swap-buffers`. **Flown:** frame time fell from a locked 16.7 ms to 10.5-14.5 ms,
`CompositeBitmap565` no longer appears in the hitch list, and the remaining hitches were mission load (up to 460 ms in the first
2 s), the threaded mip work finishing, and the cockpit `WaitUpdates` stalls (fixed above, unflown).

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

* **VR antialiasing: flown 2026-10-10, confirmed to help (static buildings stop crawling).** The per-eye path was single-sample
  (`BeginEyeFrame`), so building edges and alpha-tested (chroma-key) windows and fences crawled with head
  micro-motion even when paused. Now cfg **`VrMsaaSamples`** (0 = off, the default; 2/4/8) renders the eye
  into the multisampled FP16 scene the flat path already used and resolves it in `OutputHdrScene`. The one
  missing piece was a **multisampled eye depth buffer** (file-static in `d3d12backend.cpp`, shared by both
  eyes; D32S8). The count is snapped down to what the GPU supports for FP16 colour *and* D32S8; any failure
  falls back to the single-sample eye. Needs the HDR scene (`g_bToneMapGT7`). 4x at 2720x2976 per eye is
  about 0.5 GB of extra VRAM. Log: `[VRMSAA] requested xN -> using xM`, `[VRMSAA] eye depth WxH xN up`.
  Every PSO builder already follows the bound target's sample count (checked), and a multisampled depth
  has no array view, so the (unused) cloud/shadow depth read is skipped via `m_sceneDepthMs`.
  **Alpha-to-coverage** (cfg `AlphaToCoverage`, default on, only meaningful when the target is multisampled,
  so flat MSAA gets it too): opaque draws get `AlphaToCoverageEnable`, and for an alpha-tested draw the PS
  outputs a sharpened alpha (one pixel of ramp around `gAlphaRef`, via `fwidth`) as coverage instead of
  hard-`discard`ing the edge; an opaque draw that is not alpha-tested outputs alpha 1. `FF_A2C` (cb bit 24) and
  the PSO state are one function of values in the PSO key (`A2cWanted`), and the render constants are
  re-filled whenever its answer flips. If an external `FFEmu.hlsl` lacks `FF_A2C` the renderer switches it
  off at start-up (an old shader on an A2C pipeline would dither holes into cut-outs).
  **Measured (Seoul, same flight style, `[GPUPROF]`):** GPU per eye 1.16-1.25 ms off -> 1.30-1.54 ms at 4x
  (+0.15-0.3 ms; terrain 0.07 -> 0.14 ms, objects roughly unchanged), GPU busy still 14-18%, frame rate
  unchanged. The GPU accepted 4x for FP16 + D32S8 at 2720x2976 (`[VRMSAA] requested x4 -> using x4`).
  Still to watch if you change content: holes or dithering in window/fence cut-outs would be the A2C
  bookkeeping (`AlphaToCoverage 0` isolates it from MSAA itself).
### Status of the remaining roadmap (updated 2026-10-10)

| Item | Status | Notes |
|---|---|---|
| Overlap the two eyes | **written, not flown** (`VrEyeOverlap`, default 0; see "Eye overlap" below) | CPU frame time is the limit, not the GPU. Needs a separate command allocator per eye and an audit of the per-frame constant/vertex rings (they probably assume the previous eye finished). Audit first. |
| Smooth the coarsest far terrain ring | **knob written, not flown** (`TerrainRingScale`) | Rings were L0 13 km @250 m, L1 26 km @500 m, L2 52 km @1 km, L3 104 km @2 km, L4 296 km @4 km (each LOD reaches half the next; every ring is 52 posts wide). `TerrainRingScale 2` makes every ring but the last twice as wide at the same spacing, so with a 150 km far plane the 4 km posts are never seen and 2 km posts reach ~100 km. Costs data streaming/tile textures and some CPU; GPU terrain is ~0.1 ms. Capped by the clipmap window (~2.3x). Try 1.5 then 2. |
| World sun shadows | **not started** | Only the cockpit has them. Biggest visual jump left and the biggest job. |
| View-instanced (stereo-instanced) rendering | **off on purpose** | Constants are quad-tuned and break plain stereo; see the VR rig memory. Do not enable until that is untangled. |
| Alpha-to-coverage for cut-outs | **done, flown** | `AlphaToCoverage` (default on); only acts when the target is multisampled. |
| Small positive mip bias | **available, not recommended yet** | The Advanced-page slider goes to +0.5 / +1.0 (less crawl on distant building textures, a little less sharpness). Untested on the user's rig; MSAA already removed most of the crawl. |
| Specular glints on roofs | **unconfirmed** | Per-pixel Blinn-Phong may twinkle with micro-motion; capping the power would calm it. Not seen on the user's buildings; do not change blind. |
| Gamma-space lighting -> linear light | **not started** | Fixes the washed-out look but changes the whole look; needs tuning. |
| Temporal AA | **not started** | Was blocked on MSAA; MSAA has landed, so it is unblocked. |

New this session, written but **not yet flown**:

* **Cockpit texture drain.** `VCock_Exec` called `TheTextureBank.WaitUpdates()` every frame; that drains the
  whole loader queue, so any pending scenery texture became a 100-250 ms sim-thread stall (the
  `WaitUpdates` hitch samples above). It now drains only when the cockpit's drawable or texture set changed.
  Check the `[HITCH]` list after the first seconds of a flight.
* **HMCS / head-locked symbology doubles between the eyes during hard turns** (text splits left vs right;
  screenshots look fine). The code draws both eyes from one head basis, so it is not a per-eye projection
  bug. Leading theory: the runtime re-warps the submitted eye images (timewarp/ASW) and treats head-locked
  content as world-locked. New log line **`[HEADPOSE]`** (every ~2 s, `openxrbackend.cpp`): largest angle
  between the head orientation the game draws with and each submitted eye pose (`cant L/R`), between the two
  eyes, and the peak head turn rate. Large cant or eye-vs-eye angle = app-side; ~0 = the runtime warp. If
  it is the warp, the fix is to draw the HMCS/HUD into a head-locked OpenXR quad layer (view space) instead
  of the world image; a quad has a fixed depth, so "at infinity" needs a large distant quad.

**Update (HEADPOSE flown):** the app's poses agree (head-vs-eye 0.07 deg, eye-vs-eye 0.05 deg at up to 167 deg/s), so the doubling is the runtime re-warping head-locked content. **Head-locked HMCS quad: flown 2026-10-10, the doubling is fixed** (cfg `VrHmcsQuad`, default 1; `VrHmcsQuadHalfFov` 35 deg, `VrHmcsQuadDist` 60 m; `VrHmcsQuad 0` = the old in-eye draw): the HMCS is drawn once per frame at xrEye 0 into a 2048x2048 RTT (`D3D12Backend::EnsureHmcsRtt/BindHmcsRtt/HmcsRttTex`, a header change -> Rebuild All) with a symmetric +/-35 deg frustum about the head's forward axis, and submitted as a VIEW-space quad layer (`OpenXRBackend::SubmitHmcsQuad`, copied in `EndStereoFrame`, same plumbing as the subtitle quad). `VCock_Exec` skips its per-eye `Hmcs_Draw` while `g_bVrHmcsQuadActive` (previous frame's result). Things to check in the headset: symbology size/position unchanged vs before, text sharpness (29 px/deg), nothing clipped beyond 35 deg, and the quad appears on top of the canopy. Dynamic cues that follow world targets are baked at the sim-time pose, so they lag a turn by one frame like a real helmet display; the cross and data block are exactly head-fixed. D3D12 only; the knob (`DrawHmcsKnob`) is part of the pit and still draws in the eyes.

### Eye overlap (cfg `VrEyeOverlap`, default 0; written 2026-10-10, NOT built or flown)

Audit result: both eyes shared one allocator and one set of per-frame rings (constant, vertex, index, SRV-descriptor, clipmap-strip) because the slot was the swap-chain index and the rings reset per eye; `EndEyeFrame` fence-waited, so that was safe. Overlap needs only a distinct slot per eye: with `VrEyeOverlap 1` each eye alternates between slot 0 and 1 (`D3D12Backend::FrameIndex()` returns the eye slot, which drives `BeginCommandList`'s allocator + `m_allocFence` and the renderer's ring slot), and every eye but the LAST of the frame skips the fence wait (`SetEyeNoWait`, from `OpenXRBackend::EndEye`). The last eye still waits, so the CPU-written single buffers (clipmap chunk bounds, the desktop overlay upload, the XR quad copies, the mirror) are never touched while the GPU reads them. Checked and left alone: shared FP16/MSAA targets and eye depth (GPU-ordered on one queue), texture frees (deferred 8 epochs), texture uploads (GPU-side queue sync). A pending screenshot forces the wait. Expect ~1-1.5 ms less CPU per frame (the eye-1 GPU time stays on the critical path because the last eye still waits; waiting lazily at the next frame start would save the rest but needs the bounds buffer double-buffered first). `[GPUPROF]` now reports only eyes it could collect (it skips a slot while the previous one is pending). Watch for: flicker or wrong constants in the second eye, corrupted terrain, a debug-layer error about allocators or resources in use. If anything looks off, `VrEyeOverlap 0` is the old path.

**Flown 2026-10-10 (HUD doubling, logged, not investigated):** with `VrEyeOverlap 1` the HUD looked doubled between the eyes; with 0 it can still happen but only very slightly. So there are two causes, not yet separated: (1) a small baseline doubling (the HUD is pit geometry drawn in each eye image at the glass depth, so it is subject to the same runtime re-warp the HMCS had, and its symbology is meant to read at infinity); (2) something extra under overlap -- suspect per-eye CPU state (the combiner/RTT HUD texture or its constants) that the eye-2 recording reads while eye 1's GPU work is still using it. `VrEyeOverlap` stays default 0 until (2) is understood. Next steps if picked up: compare `[HEADPOSE]` and a screenshot of each eye with overlap on; check whether the HUD RTT is redrawn per eye or once (a once-per-frame RTT sampled by an overlapped eye would show last frame's content in one eye); consider moving the HUD symbology into the head-locked quad like the HMCS.

### Whole-frame CPU profile (2026-10-10, `HitchMs 6`, `TerrainRingScale 2`, 6000 samples; fixed afterwards, not re-flown)

Inclusive share of the sim thread's samples: `OTWDriverClass::Cycle` 91%; `DrawScene` 44%; **`RenderOTW::TransformVertexSet`/`TransformRun`/`ComputeVertexColor` 32% (+ `cosf`/`sinf` 11%)**; `D3D12Backend::EndEyeFrame` fence wait 18% (the GPU, not work: the eye overlap target); `CDXEngine::FlushBuffers` (object draws) 15%; `TerrainClipmap_Update` 7% (`UploadRect`/`FillRect` 5%); `TerrainGpu_Render` 7%; texture mip building ~5%; particles ~3%. The big one was dead work: with GPU terrain on, `DrawGroundAndObjects` still ran `BuildVertexSet` + `TransformVertexSet` -- the legacy CPU screen-space terrain transform with per-vertex lighting -- although `DrawTerrainRing/ConnectorRing/GapFiller` are skipped (`cpuTerr`) and nothing else reads those vertices. It scaled with ring width, which is why `TerrainRingScale 2` cost so much CPU (draw-scene 3.3 -> 9-10 ms) while the GPU terrain pass stayed ~0.1 ms. Now skipped unless the sensor-RTT path needs it (cfg `CpuTerrainXform 1` restores it). The span list is still built (objects use it). Re-measure with `[GPUPROF]` draw-scene ms and `TerrainRingScale 2`. Tools: `HitchMs` (6 = whole frame), `tools/profiling/hitchsym.exe`; run it on the last N samples only: `grep HITCH FFDebug.log | tail -6000`.

**Flown 2026-10-10 after the fix (`TerrainRingScale 2`):** draw-scene 2.0-2.6 ms/frame (was 9-10; stock ring 3.3), 60 fps locked, 0 frames over 20 ms in every 5 s window, GPU busy 14-22%, ~2,000-2,300 terrain chunks per eye at 0.11 ms. `TerrainRingScale 2` is now affordable and a good default candidate.
