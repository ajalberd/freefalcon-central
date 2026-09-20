# Lighting, shadows and tone mapping

**Status: item 3 (the "missile shading issue") is resolved — and it was never lighting. Per-pixel
object lighting has landed as the first piece of item 1. Item 2 (cockpit shadows) has landed for
D3D12 — see below. Item 4 (HDR + GT7 tone mapping) is still not started, and it is the last step of
the lighting redo, not a feature that can land first. Reconnaissance below; append findings here
rather than growing `WIP-NOTES.md`. The object-pass work has its own doc — read
`OBJECT-RENDERING.md` before touching the object light path, the light CB, or the shaders.

## The goal

Four things, and they are not one project:

1. Redo the lighting system. — **started**: per-pixel object lighting landed (see below).
2. Real shadows in the cockpit. — **landed on D3D12** (2026-09-20, below); Vulkan pending.
3. Fix the missile shading issue. — **done**, and it was the fin z-fight, not lighting (below).
4. Integrate Gran Turismo 7 tone mapping. — not started; needs the HDR project (below).

References for (4):
- <https://s3.amazonaws.com/gran-turismo.com/pdi_publications/s2025_PBS_Physically_Based_Tone_Mapping_GT7.pdf>
- <https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp>

## What was wrong in the object pass (2026-09-19/20) — see `OBJECT-RENDERING.md`

Three symptoms that had been read as lighting problems were chased down and fixed in the object
pass. Summary only, so this doc does not drift — evidence and knobs are in the other doc:

- **The "missile shading issue" was never lighting.** It is the missile-fin z-fight: the player's
  own wing stores ride the VB manager's pit list, which the first cull excluded. Fixed with
  `g_nObjCullMode` (D3D7 parity = cull FRONT, because the clip projection carries a deliberate
  reflection) and `g_bObjCullPit`.
- **The F-16 external strobe flash** — the port dropped the `D3DLIGHT7` `OwnLight` /
  `NotSelfLight` flags and uploaded the strobe's spot as a 1500 ft omni point, so it lit its own
  jet and everything nearby. Fixed with `g_bObjLightMasks` / `g_bObjSpotCones`. Semantics:
  `OwnLight` = may only light its own object; `NotSelfLight` = must not light its own object;
  `Static` = excluded from the dynamic set (feeds the emissive precompute only).
- **The F-16 light-strip "red patches"** — the port *replaced* the material with the emissive
  colour; D3D7 *adds* it (`materialDiffuse × (ambient + Σ lights) + materialEmissive`), so thin
  glow strips rendered as flat saturated panels. Fixed: emissive adds on top of the lit material;
  `FF_AFTERBURNER` keeps its deliberate white→flame override.
- **Canopy/glass surfaces were unlit** — the sorted-alpha path (`DrawSortedAlpha`) never restored
  the object-pass flags, so alpha model surfaces drew on whatever 2D state ran last (no
  `FF_LIGHTING`); the F-16's gold canopy stayed constant at night. Now it calls
  `BeginObjectPass()` per item.

**Per-pixel object lighting has landed** (`g_bObjPixelLight` / `FF_PIXELLIGHT`): the light model
lives once per backend in `FFObjectLighting` (`ffemu.hlsl` / `ffobject.hlsl`) and the pixel
shader can evaluate it per pixel instead of per vertex (legacy Gouraud) — the reason a 3 ft lamp
used to wash whole low-poly panels. Watch the cockpit (now per-pixel too) and VR frame cost;
`g_bObjPixelLight 0` restores the legacy path.

## The thing to settle first: this renderer is LDR

The swapchain is `DXGI_FORMAT_R8G8B8A8_UNORM` (`d3d12backend.cpp:287`, and
`d3d12backend.cpp:479` returns the same as the backbuffer format). There is **no HDR render
target and no global tone-mapping pass**. Depth/stencil is
`DXGI_FORMAT_D32_FLOAT_S8X24_UINT`.

A tone-mapping curve maps scene radiance onto display luminance. With no HDR buffer there is
no scene radiance to map from — lighting is computed and written straight to 8-bit. So GT7's
curve is **not a drop-in**: bolting it onto the existing LDR output would only re-curve
already-clipped values, which is a colour filter, not tone mapping. Getting the real benefit
means an HDR scene target plus a resolve pass, and *that* is the actual project. Worth being
clear about the size of it up front rather than discovering it halfway.

Two things make this less daunting than it sounds:

- **The cloud system is already HDR internally** and tone maps itself. `CloudTonemap` /
  `CloudTonemapInv` in `ffemu.hlsl` (~line 1090) are an exponential curve and its exact
  inverse, `1 - exp(-L*k)`. There is a long comment there warning that the two must stay
  adjacent and agree, because when they disagreed the failure was a per-channel *hue* shift,
  not a brightness error. Any global tone map has to reconcile with this: either the clouds
  stop self-tonemapping and feed radiance into the global pass (correct), or the two curves
  fight. Read that comment before touching either.
- The cloud work already established the discipline of tracking units (radiance vs. LDR)
  through this shader, and the comments record where that went wrong before.

## Cockpit lighting today

All in `ffemu.hlsl`. The cockpit is a special case threaded through the shader rather than a
separate path:

- ~line 85: for cockpit surfaces only, the ambient **floor** is dampened so the sun reads.
- ~line 355: the N·L gradient for cockpit surfaces is applied **only by brightening lit
  faces** — "a gradient by adding light, never removing it". Shadowed faces go darker only
  relatively.
- ~line 363: for cockpit surfaces the ambient is dimmed by the sun level, so the pit goes dark
  at night.
- ~line 399 (#72): most pit surfaces carry **no material specular** (`SpecularIndex = 0`), so
  they read matte/dead.

CPU side: `CockpitManager::ComputeLightFactors` and `CockpitManager::ApplyLighting` produce
the per-frame light factors the 2D/3D pit art is tinted by. `ApplyLightingToPalette` does the
same for palettised images (the kneeboard map uses it).

### The pit's own lamps, and why the flood knob did nothing (2026-09-20)

The 3D pit model (LOD 4105) carries **6 light nodes**: nav (switch 8, green/red), tail strobe
(7), landing (9, a spot), and — the two that matter — **interior/flood (127)** and
**instrument (128)**, both authored as points with a **2.2-unit range inside a 22-unit pit**
(`tools/models/objsurvey.py --tree 4105` dumps them). Two port decisions made them dead:

- **The shader's attenuation was a hard linear ramp, `saturate(1 - d/range)`.** D3D7 used the
  light's authored curve `1/(a0 + a1·d + a2·d²)`, cut at the range (D3D9's rule). With the ramp
  the flood lit a 2-unit ball in the middle of the pit — "the knob does nothing". Fixed:
  `g_bLightFalloffD3D7` (default 1) has the light engine hand `a0`/`a1` to the shader in
  `Params.z/w` (free for point lights; spots keep the cone cosines there) and the shader uses the
  curve. **Only when `a1 > 0`**: the F-16 tail strobe is authored `a0=1.01, a1=0`, and the curve
  degenerates for it to a flat ~0.99 blob out to its range — a hard-edged disc under the tail that
  reads as "the light is on the bottom of the plane". Lights without a falloff term keep the ramp.
- **The knob's real effect belongs to the 2D art.** `CockpitManager::ComputeLightFactors` is what
  the 2D pit is tinted by; the 3D pit never saw it. Now `CockpitManager::GetCockpitFill` publishes
  the flood+instrument contribution (without the environment, which the object pass already carries
  as `gAmbient`), `VCock_Exec` hands it to the renderer per frame, and `FFObjectLighting` adds it
  to the ambient for `FF_COCKPIT` surfaces. Plumbing: `IRenderer::SetCockpitFill` →
  `cbRender.gCockpitFill` (D3D12) / `ObjUbo.cockpitFill` (Vulkan, appended after the light array so
  no existing offset shifts). Knobs: `PitFillScale`, `PitFillReach`.
- **The fill is a lamp at the pilot's eye, not an ambient.** The pit is drawn camera-relative, so
  `|WPos|` is the distance from the eye and the fill falls off from there (`gCockpitFill.w` =
  reach, default 10 model units). A flat add lit the *whole* pit LOD — including the nose and wings
  it carries for the view out of the canopy — and read as "the cockpit lights light up the entire
  plane". `PitFillReach` tunes it: too far and the exterior lights up again, too near and the panel
  misses it.
- **The jet's own lights spill into the pit.** The pit is a separate object from the player's
  aircraft, so `OwnLight` (nav/formation) excluded every one of the jet's lights from the cockpit.
  `UpdateDynamicLights` now lets `OwnLight` through when the RECEIVER is the pit; `NotSelfLight`
  (the landing light, flashes) stays excluded, and each light's own range keeps the spill small.
- **The fill skips TRANSPARENT draws.** The HUD combiner and canopy glass sit at the fill's origin
  (the pilot's eye), so they took it at full strength and read as a lit gray box over the cockpit.
  `FillRenderCB` (D3D12) / `RecordObjectDraw` (Vulkan) now pass `gCockpitFill` only when the draw is
  opaque (`BLEND_OPAQUE`). Opaque pit surfaces keep it; glass gets none, which is also physically
  right — light passes through a transparent surface rather than scattering in it.
- **The EMISSIVE term is added AFTER the texture** on the per-pixel path (both backends). D3D7's
  order was `albedo * (lit + emissive)`, and the port matched it — but that multiplies a lamp's
  emissive by its own albedo, and the F-16's intake strip has a dark albedo and a dim red emissive
  (74,0,0, read out of LOD 2648 with `tools/models/objsurvey.py`), so the lamp's own surface stayed
  dark and only the light's spill on the skin showed. Unmodulated, a lamp reads as a source
  (`albedo * lit + emissive`). The legacy per-vertex path keeps the D3D7 order (there the VS folds
  the emissive into the vertex colour). If this ever reads as flat saturated strips again, this is
  the one block to revisit.

Note the CPU light set is culled by `dvRange + objectRadius` while the shader cut at `dvRange`
alone — the flood light was *in* the pit's set and the shader then zeroed it. That inconsistency
is why the symptom looked like a missing light rather than a falloff.

## Cockpit shadows (2026-09-20) — D3D12 landed

One model, one light, so this is a depth map, not a shadow system. The pit BSP is replayed
depth-only through the object path into a 1024² D32 target from the sun's direction; the cockpit
branch of `PS_Main` compares and darkens **only the sun term**. Knobs: `PitShadow` (default 1) and
`PitShadowStrength` (default 1.0) in `FFViper.cfg`.

The decisions that matter, in the order they bit:

- **The map is fitted in the PIT'S MODEL SPACE, not world space.** The pit is a rigid shell, so its
  self-shadow depends on the sun direction *in the pit's frame* — not on the camera, the eye or the
  aircraft's attitude. That makes the map view-independent (VR renders it once per frame and both
  eyes sample it), it keeps the fit tight (the pit's bbox, not a world region), and it removes the
  per-eye camera-relative translation problem entirely. The sun direction is rotated into the model
  frame with the pit's own `RotMatrix` (`CDXEngine::RenderPitShadowMap`).
- **The replay is the SAME node walk as the normal draw** (`DrawNode`/`DrawSurface` with
  `m_ShadowWalk` set): same DOF/switch handling, no second geometry path to drift. Alpha surfaces
  are skipped — the canopy/HUD glass must not cast an opaque shadow. Slot children (attached stores)
  are skipped: they are not queued yet at that point, and they sit outside the fitted extent.
- **Ordering**: `FlushBuffers` replays the pit list *before* `FlushObjects` dispenses it, so the
  list is still full. Once per frame only — `SetSunLight` re-arms the latch (`StartDraw` runs once
  per frame, before the eye loop), so the second eye / the quad group reuse the map.
- **The matrix layout is the trap.** The shader's `mul(p, M)` dots p with each **CPU matrix row**
  (HLSL packs cbuffers column-major by default; verified in the DXBC: `dp4 o0.x, v0, cb0[0]`), so
  `CDXEngine::RenderPitShadowMap` composes the light view and the ortho **directly into the rows**
  rather than multiplying two D3DX matrices. The fit was validated offline (all bbox corners inside
  uv/depth for six sun directions, depth monotone with "nearer the sun = deeper", reversed-Z).
- **Reversed-Z**: the map clears to 0 (far) and the PSO uses `GREATER_EQUAL`; a texel nearer the sun
  holds a larger depth. The PS is lit when `stored <= own + bias`; a normal offset (1.5 shadow
  texels, derived from the matrix's first row) carries the acne, and the constant bias only absorbs
  quantisation.
- **Depth-only PSO**: `NumRenderTargets = 0`, no pixel shader, stencil off, cull NONE (the object
  pass culls against a *reflected* camera projection — meaningless for a light-space pass). The
  shadow target is `R32G8X24_TYPELESS` viewed as `D32_FLOAT_S8X24` (the format every PSO already
  bakes, so no new DSV format) and as `R32_FLOAT_X8X24_TYPELESS` for the SRV (t6).
- **The format trap that cost a black screen**: an `R32G8X24_TYPELESS` resource **cannot** be viewed
  as plain `R32_FLOAT`. `CreateShaderResourceView` with that format does not return an error — it
  **removes the device** (`DXGI_ERROR_INVALID_CALL`), after which *every* later PSO creation fails
  with `DEVICE_REMOVED` and the screen goes black in all views. The log's tell is
  `[D3D12R] PSO create failed (key 0x201059)` followed by every other key. The scene depth uses
  `R32_FLOAT_X8X24_TYPELESS` for the same reason (`SceneDepthSrvCpu`); do the same. Reproduced
  offline with the debug layer before fixing — that is the fastest way to settle a PSO/format
  question, and it is worth doing for any new view.
- **Where it lives**: `IRenderer::SetPitShadowVP/BeginPitShadowPass/EndPitShadowPass` +
  `PitShadowSupported` (default no-op, so Vulkan compiles and skips); `D3D12Backend::Ensure/
  Bind/UnbindPitShadowTarget`; `cbShadow` is root CBV b6 and t6 joins the per-draw SRV table
  (bump `srvRange.NumDescriptors` AND `SRV_PER_DRAW` together — they are the same fact twice).
- **Testing gotcha**: the renderer prefers an EXTERNAL `<FalconDataDirectory>\shaders\FFEmu.hlsl`
  over the copy baked into the exe (runtime-tunable workflow, see `embeddedshader.h`). If a tuned
  shader from an earlier session is sitting in the install, the new one is ignored — update or
  delete it before judging the shadows.

Known limits, deliberately:

- **D3D12 only.** `VulkanRenderer` inherits the no-op default (`PitShadowSupported() == false`) and
  `ffobject.hlsl` has no lookup, so Vulkan renders exactly as before. The Vulkan port is mechanical
  (set-0 binding 4, a depth-only render pass, the same model-space fit) but it is not done.
- **The per-pixel path only** (`FF_PIXELLIGHT`). With `ObjPixelLight 0` the legacy Gouraud VS path
  passes `sunShadow = 1.0`, so there are no cockpit shadows. The VS cannot sample t6 anyway — the
  SRV table is PIXEL visibility.
- **Attached stores do not cast** and are outside the fitted extent (they still sample it and get
  "outside = lit").
- **Watch the deferred solid surfaces.** `DrawSolidSurfaces` runs at the pit→world transition,
  *after* `SetPitMode(false)` cleared `FF_COCKPIT`; if the pit's own surfaces ever move onto the
  VColor/solid stack they would silently lose both the cockpit shading and the shadow. Verify in
  flight if the pit's look changes after touching the surface flags.
- Per-frame cost: one depth-only replay of the pit's ~400 surfaces, shared by both VR eyes. If VR
  CPU time regresses, the merge candidate is one combined index buffer for the pit (one draw).

## Shadows: the rest of the world still has nothing

Grep for `shadowmap` / `ShadowPass` / `ShadowMatrix` across `src/graphics` still returns **zero
hits** — the cockpit map is named `PitShadow*` throughout, so those greps no longer mean "no
shadow code anywhere", only "no world shadow code". `DrawableShadowed` (`drawshdw.h`) is the 1997
class for drawing an aircraft's blob shadow on the ground — not a shadow map, and not reusable for
the cockpit.

The cockpit path above is deliberately pit-only. World shadows would need a cascaded/tiled scheme
and a much larger fit; nothing about the pit's model-space trick carries over to terrain.

- The depth buffer already has a stencil and it is already used (HUD aperture clip), so don't
  assume the stencil is free. The pit shadow target is a *separate* depth texture with an unused
  stencil plane, exactly so the scene stencil is never touched.
- In VR this runs once per frame (model space), but the pit's normal draws run per eye.

## Suggested order

These are listed in the order they were asked, which is not the order to do them.

1. **Missile fin flicker** — **done** (it was the cull, not lighting; see above).
2. **Cockpit shadows** — **done on D3D12** (2026-09-20, above). The next steps, in order of value:
   - **Vulkan parity** — the user's canonical VR path is Vulkan multiview, so the feature is
     invisible there. Mechanical: set-0 binding 4 + a depth-only render pass + the same fit.
   - **The pit's look** (the user's "it's kind of old looking", 2026-09-20) — now that the pit has
     real shadows, the remaining flatness is (a) no crevice occlusion (SSAO — the PS comment at the
     cockpit branch already calls it "a separate step"), (b) most pit surfaces carry no material
     specular (`SpecularIndex = 0`, a DATA problem — see `COCKPIT-OVERHAUL.md`), (c) the texture
     and font resolution ceiling. Shadows were the shading half; those are the art half.
3. **Lighting redo + HDR + GT7 tone mapping** — one project, not two. The tone curve is the
   last step of it, not a feature that can land first. Per-pixel object lighting is the first
   piece of the redo and is already in.

## Do not re-derive

- Swapchain `R8G8B8A8_UNORM`; depth `D32_FLOAT_S8X24_UINT`. LDR, no tone-map pass.
- `CloudTonemap`/`CloudTonemapInv` are the *only* tone mapping in the engine and they are
  cloud-local.
- No world shadow-map machinery exists. The cockpit shadow map (2026-09-20) is a *separate*,
  pit-only depth target and shares nothing with a world scheme: 1024² `R32G8X24_TYPELESS`
  (DSV `D32_FLOAT_S8X24`, SRV `R32_FLOAT`), t6 + cbShadow(b6), fitted to the pit's model bbox,
  replayed once per frame, gated on `FF_COCKPIT`.
- The shader's `mul(p, M)` dots p with each CPU matrix **row** (HLSL column-major cbuffer packing);
  the shadow VP is composed into the rows on purpose. Any new hand-built matrix must do the same,
  and the fit must be validated against that operation, not against `p * M` on paper.
- The legacy STATE_* enum is translated to state bundles in `ffstatemap.cpp`. `Make()` there
  gives the blend/filter/depth defaults for every state — that table is the fastest way to
  answer "is this thing depth-tested / what blend does it use".
- The object light model is ONE function per backend (`FFObjectLighting` in `ffemu.hlsl` /
  `ffobject.hlsl`); per-vertex and per-pixel paths both go through it. Do not fork it. Its
  `sunShadow` parameter (D3D12 only for now) scales the DIRECTIONAL term alone.
- Point lamps use their AUTHORED D3D7 attenuation (`1/(a0 + a1·d)`, cut at the range), handed over
  in `Params.z/w` — but only when `a1 > 0`; a1 == 0 data (the tail strobe) keeps the ramp, or the
  curve degenerates to a flat disc. The pit's flood/instrument knob effect comes from
  `CockpitManager::GetCockpitFill` → `gCockpitFill` → the FF_COCKPIT ambient, as a LAMP AT THE EYE
  (`|WPos|` falloff, `PitFillReach`), never a flat add; and `OwnLight` lights are let into the pit
  (the pit is the same aircraft) while `NotSelfLight` stays out.
- Emissive is ADDED to the lit material, never a replacement (`FF_AFTERBURNER` excepted).
- The clip projection reflects (`det = −1`), so D3D7's cull-back is this port's cull-FRONT —
  and the player's own stores are drawn from the pit list. Both bit us; see `OBJECT-RENDERING.md`.
- The light CB is `ambient[4]`, a **uint** count at byte 16 (reading it as float is wrong), then
  8 lights of 64 B: position, direction, colour, params (x=range, y=type 0/1/2, z/w=spot
  cosines). Recipes for reading it (and everything else) from a capture are in
  `OBJECT-RENDERING.md` §6.
