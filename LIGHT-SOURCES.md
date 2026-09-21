# Lamp sources: what was tried (2026-09-20)

**Status: the lamp-source problem is NOT solved.** The light *spill* works; the lamp *source*
(the visible lens/strip at the light's position) still reads black at the F-16's wingtip lights and
the intake strips. The last attempt (additive sprites at each dynamic light) is in the tree and can
be turned off with `LightSprites 0`. This doc records what was tried, what the models actually
contain, and the evidence, so the next attempt does not repeat any of it.

Read `RENDER-LIGHTING.md` first for the shading work this grew out of.

## The symptom

- External view: the wingtip nav lights and the intake red/green lights have **no visible source** —
  a grey/black box where the lamp should be, with the light's spill on the skin around it.
- Cockpit: the same lamps are a **black dot**.
- The user's expectation: a lamp that is on should read as a lit lens/strip, not as an unlit box.

## What the models actually contain (the root of it)

Read with `tools/models/objsurvey.py` (dump a LOD's lights and its switch-emissive surfaces) plus a
small script that decodes each surface's vertex `dwSpecular` (the shader's `Emis`):

- The player's F-16 is **`F-16CJ_00_Nm_L1` (LOD 2719)**. Its light nodes are real and work:
  wingtip nav lights (switch 8, green/red, range 3, **a1 = 0**), intake lights (switch 8, green/red,
  range 1.9, but flagged **Static** = excluded from the dynamic set), landing (spot), interior
  (switch 127, flagged Static).
- Its **switch-emissive surfaces** are only: the intake strips (emissive **83,0,0** red and
  **65,0,0** green — i.e. 0.33/0.25, dim), the tail strip (255,255,255), a few grey/green strips,
  and the amber interior panel lights (255,~100,16). **There is no emissive surface anywhere near
  the wingtip lamps.**
- The 3D pit (`3DPIT_F16CJ_L1`, LOD 4105) has **6 light nodes** (nav green/red, tail strobe, landing
  spot, interior 127, instrument 128) and **zero switch-emissive surfaces** — so from inside, every
  pit lamp is pure `albedo x lit` and goes black at night.
- (An earlier dump of the wrong model, `F-16 AGSR_00_Nm_L1` LOD 2648, had amber/teal emissives at
  the intake — the CJ's are the dim red/green above.)

So the models simply have nothing that can glow at the wingtip lamps, and only a very dim strip at
the intake. No shader change can make an unlit grey box into a lamp; that part is data.

## What was tried, in order

1. **Authored D3D7 point-light falloff** (`LightFalloffD3D7`). The shader attenuated every point
   lamp with a hard ramp `1 - d/range`; D3D7 used `1/(a0 + a1*d)` cut at range. This made the pit's
   own lamps (range 4-12.5) much stronger and fixed the "flood does nothing" feel. It took three
   passes to get the `a1 == 0` data right:
   - all lights on the curve: the tail strobe (a0=1.01, a1=0) became a **flat 0.99 disc** out to its
     range — a hard-edged blob under the tail ("the light is under the plane");
   - a1 == 0 lights on the old ramp: the flat disc was gone, but so was the spill — a 3 ft nav light
     lit nothing beyond 3 ft ("**it doesn't light the rest of the plane anymore**");
   - current: a1 == 0 lights get a **synthesized falloff** (a1 = a0/range) and a shader cutoff at
     8x the authored range, so the lamp is bright near, ~half at its range, and still spills well
     past it — the per-object cull (authored range + receiver radius) is what actually bounds them.
     That is the state to judge: if the spill is back but the strobe reads too broad, the multiplier
     is the knob to tune (currently 8x).
2. **Cockpit fill for the flood/instrument knobs** (`PitFillScale`, `PitFillReach`). The sim
   publishes the 2D art's flood+instrument colour; `FFObjectLighting` adds it for `FF_COCKPIT`
   surfaces. Iterated three times:
   - flat ambient add -> lit the whole pit model including the nose/wings ("the cockpit lights light
     the whole plane"); fixed by making it a lamp at the pilot's eye (`|WPos|` falloff, reach knob);
   - it lit the HUD combiner (drawn **opaque**, stencilled) -> "a grey box over the cockpit"; fixed
     by passing the fill only for opaque draws **with the aperture stencil disarmed**.
   This part works and the user accepted it ("internal cockpit lights up and doesn't affect outer").
3. **Emissive order** (`FF_PIXELLIGHT` path): the emissive was added *before* the texture, so
   `albedo * (lit + emissive)`. The F-16 intake strip's albedo is dark and its authored emissive is
   dim, so the lamp's own surface stayed dark while its spill showed. Moved the emissive to *after*
   the texture (`albedo*lit + emissive`). It brightened lamps generally ("lamps look good in terms
   of brightness") but the intake strips still read as shading rather than as sources.
4. **Visible light sprites** (`LightSprites`, `LightSpriteSize`, `LightSpriteGain`).
   `CDXEngine::DrawLightSprites` draws one camera-facing additive billboard per active dynamic light
   (position from the light, colour from its diffuse, size = 0.35 x range) through the existing
   particle path — one `DrawIndexedInstanced` for all of them. Iterations:
   - first version: a small glow appeared (user: "looks great"), but the lamp's own **black housing**
     stayed black. Guessed occlusion by the housing -> nudged the sprite toward the eye (0.4 ft +
     half the glow size). "not much change".
   - Then a capture (`redlight_blacksprite.rdc`) with a **pixel history** proved the sprite draw
     (`ev=19352`, 6 indices x 2 instances) **passes at the black pixels** — so the sprite was drawn
     and in front. The profile was `(1-r)^2`: a pin-point with a faint halo, so the housing stayed
     black around it. Broadened the core (plateau to r=0.35 + smoothstep rim).
   - User verdict on the result: **"not good"**. The sprites are still in the build; `LightSprites 0`
   turns them off.

## Evidence and how to get it again

- **Model data**: `python tools/models/objsurvey.py C:\FreeFalcon6\terrdata\objects\KoreaObj
  --tree <lod>` prints the lights pool and switch-emissive surfaces; `--parent <id>` prints a
  parent's bbox/LODs. A small script reading the vertex pool gives each surface's `dwSpecular`
  (emissive) per vertex — that is how the dim 83,0,0 / 65,0,0 values above were found.
- **RenderDoc captures**: this RenderDoc (1.46) ships no standalone Python module, but
  `qrenderdoc.exe --python <script.py>` runs one, and the full replay API is available:
  `renderdoc.OpenCaptureFile/OpenCapture`, `controller.GetRootActions()`,
  `controller.PixelHistory(texture, x, y, subresource, typeCast)` (the first argument is the
  TARGET TEXTURE, not x/y — get it from the `Present(ResourceId::N)` action name),
  `controller.SaveTexture(...)` to dump the frame. Pixel history's `Passed()` plus
  `depthTestFailed`/`backfaceCulled`/`shaderDiscarded` says which draw actually wrote a pixel, and
  that is how the wingtip "no lamp draw at all" and the "sprite passes but is a pin-point" findings
  were made.
- Captures referenced: `extlamppower.rdc`, `inside_outside_lampissue-frame1282.rdc`,
  `inside_outside_lampissue-frame2452.rdc`, `redlight_blacksprite.rdc` (all under
  `C:\FreeFalcon6\rdoc_captures\`).

## What is still open

1. **The real fix is art**: author emissive surfaces (or brighten the existing strips) at the lamp
   positions in the LODs — wingtip nav lights, the intake strips, the pit's lamp lenses. The engine
   cannot invent a source where the model has none.
2. **Or accept a synthetic source**: the sprite path works mechanically; it needs a look the user
   accepts. Options: tie the sprite to the lamp's *geometry* (a quad on the lamp surface rather than
   a billboard), render it with depth-test off inside a small radius, or give it a proper lens
   texture. This is what "not good" rejected, so a different approach is needed before more tuning.
3. **Do not** expect the emissive-order change or the falloff change to fix the source: both were
   measured to help the *spill*, not the *lens*.
4. Unrelated, still open: the object pass is brighter at night than the terrain (models use day
   albedo x TOD ambient; the terrain has night tiles). See `RENDER-LIGHTING.md`.

## Knobs added in this session

| knob | default | what it does |
|---|---|---|
| `PitShadow` / `PitShadowStrength` | 1 / 1.0 | cockpit sun shadows (accepted) |
| `LightFalloffD3D7` | 1 | authored point-light falloff instead of the linear ramp |
| `PitFillScale` / `PitFillReach` | 1.0 / 10 | flood/instrument fill weight and reach (accepted) |
| `LightSprites` | 1 | additive sprite at each dynamic light (**the rejected one**; 0 = off) |
| `LightSpriteSize` / `LightSpriteGain` | 1.0 / 1.0 | sprite size and brightness |
