# Lighting, shadows and tone mapping

**Status: item 3 (the "missile shading issue") is resolved — and it was never lighting. Per-pixel
object lighting has landed as the first piece of item 1.** Items 2 (cockpit shadows) and 4
(HDR + GT7 tone mapping) are still not started. Reconnaissance below; append findings here
rather than growing `WIP-NOTES.md`. The object-pass work has its own doc — read
`OBJECT-RENDERING.md` before touching the object light path, the light CB, or the shaders.

## The goal

Four things, and they are not one project:

1. Redo the lighting system. — **started**: per-pixel object lighting landed (see below).
2. Real shadows in the cockpit. — not started.
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

## Shadows: there is nothing to build on

Grep for `shadowmap` / `ShadowPass` / `ShadowMatrix` across `src/graphics` returns **zero
hits**. `DrawableShadowed` (`drawshdw.h`) is the 1997 class for drawing an aircraft's blob
shadow on the ground — not a shadow map, and not reusable for the cockpit.

So "real shadows in the cockpit" is from-scratch: a depth-only pass from the sun, a shadow
map, and a lookup in the cockpit branch of `ffemu.hlsl`. Notes that matter here:

- The cockpit is one BSP model (parent 2402, LOD `3DPIT_F16CJ_L1`) drawn at near-Z in its own
  pass with `TheDXEngine.SetPitMode(true)` around it (`otwloop.cpp:3500`). A cockpit-only
  shadow map is therefore a well-bounded thing: one model, one light, a small world extent.
  That is a much easier first target than world shadows.
- The depth buffer already has a stencil and it is already used (HUD aperture clip), so don't
  assume the stencil is free.
- In VR this runs twice per frame. Anything added to the pit pass costs double.

## Suggested order

These are listed in the order they were asked, which is not the order to do them.

1. **Missile fin flicker** — **done** (it was the cull, not lighting; see above).
2. **Cockpit shadows** — self-contained, one model, visible payoff, and it forces the cockpit
   lighting path to be understood properly, which is prerequisite for (3) anyway.
3. **Lighting redo + HDR + GT7 tone mapping** — one project, not two. The tone curve is the
   last step of it, not a feature that can land first. Per-pixel object lighting is the first
   piece of the redo and is already in.

## Do not re-derive

- Swapchain `R8G8B8A8_UNORM`; depth `D32_FLOAT_S8X24_UINT`. LDR, no tone-map pass.
- `CloudTonemap`/`CloudTonemapInv` are the *only* tone mapping in the engine and they are
  cloud-local.
- No shadow-map machinery exists.
- The legacy STATE_* enum is translated to state bundles in `ffstatemap.cpp`. `Make()` there
  gives the blend/filter/depth defaults for every state — that table is the fastest way to
  answer "is this thing depth-tested / what blend does it use".
- The object light model is ONE function per backend (`FFObjectLighting` in `ffemu.hlsl` /
  `ffobject.hlsl`); per-vertex and per-pixel paths both go through it. Do not fork it.
- Emissive is ADDED to the lit material, never a replacement (`FF_AFTERBURNER` excepted).
- The clip projection reflects (`det = −1`), so D3D7's cull-back is this port's cull-FRONT —
  and the player's own stores are drawn from the pit list. Both bit us; see `OBJECT-RENDERING.md`.
- The light CB is `ambient[4]`, a **uint** count at byte 16 (reading it as float is wrong), then
  8 lights of 64 B: position, direction, colour, params (x=range, y=type 0/1/2, z/w=spot
  cosines). Recipes for reading it (and everything else) from a capture are in
  `OBJECT-RENDERING.md` §6.
