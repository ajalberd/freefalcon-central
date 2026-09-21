# Lamp sources

**Status: solved.** The F-16's lamps — wingtip and intake nav lights, the tail strobe, the
landing-light beam, the afterburner plume — read as lit sources again, outside and from the
cockpit, and the cockpit stays dark with its own lamps off.

Nothing was wrong with the art. Three port bugs were discarding data the models already carried.
Read `RENDER-LIGHTING.md` for the shading work this grew out of, and `OBJECT-RENDERING.md` before
touching the object pass or the light CB.

## The three bugs

### 1. The emissive gate was inverted

D3D7's object pass armed the emissive material source for **every** surface and only disarmed it
for a `SwEmissive` surface whose switch was off (`dxengine.cpp` of the D3D7 tree, commit
`35b1e813`; the same `D3DMCS_COLOR2` default is in the pass state block and in `ModelInit`):

```cpp
SetRenderState(EMISSIVEMATERIALSOURCE, D3DMCS_COLOR2);           // always, every surface
if (SwitchValues && SwEmissive && !(SwitchValues[n] & mask))
    SetRenderState(EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);     // this one surface: no glow
```

The port (`CDXEngine::DrawSurface`, `#49`) had `bool emissive = false` and only `SwEmissive`
surfaces could turn it **on** — so every lamp the models author on a plain surface lost its colour
and drew as black geometry.

The full D3D7 vertex formula. The port carried only the third term, and the second was missing
entirely:

```
lit   = matEmissive(COLOR2)                              <- bug 1
      + matAmbient(COLOR1) * sum(lightAmbient * atten)   <- bug 2
      + matDiffuse(COLOR1) * sum(lightDiffuse * N.L * atten)
final = texture * lit
```

That last line matters on its own: the emissive is **added to the lit material and then modulated
by the texture**. Added past the texture it becomes a flat unmodulated term, which saturates any
textured lamp to white. Note the distinction — it is added to `matDiffuse*lit`, it is *not
multiplied by* the vertex colour, which is black on a lamp lens. Only the second of those
extinguishes lamps.

### 2. Each light's own ambient was dropped

Every shipped lamp is authored with an ambient as well as a diffuse, always the diffuse colour
scaled (F-16 nav: diffuse 0.51 / ambient 0.17; pit flood: 0.98 / 0.41). It is carried as that
scalar in `GpuLight.Color.w` and applied with **no N·L** — which is the point. It is the term that
lights a lamp's own housing, skin the lamp sits flush against (where the light is tangential and
the diffuse term leaves a dark core inside the glow), and `POINTLIST` lamps whose vertex normal is
`(0,0,0)`. The sun leaves `Color.w` at 0.

### 3. `FF_TEXTURE0` is lost in the sorted-alpha pass — the "grey box"

`FF_TEXTURE0` is never set from the pass flags; it is only ever a side effect of `SetTexture`.
`CDXEngine::DrawSortedAlpha` calls `BeginObjectPass()` **per item** (added when the canopy glass was
found to be unlit), and that rebuilds the flag word from scratch as
`FF_VERTEXCOLOR | FF_LIGHTING | FF_ALPHATEST`, dropping it. `DrawSurface` then re-issues
`SelectTexture` **only when `m_TexID != LastTexID`**, and `LastTexID` was not invalidated. So any
sorted-alpha item reusing the previous item's texture drew with the texture stage switched off in
the shader: `texA` stuck at its 1.0 default, the corona's alpha discarded, and an alpha-shaped
sprite became an **opaque flat quad** of `vertexColour × lit`.

Measured in `graybox.rdc`: a flat `0x0C0C0C` plateau with hard axis-aligned edges; pixel history
says two 12-index draws (`ev 3127`, `3134`) fully replace the wing skin underneath, so source alpha
is 1.0 despite `SrcAlpha/InvSrcAlpha` blending; and `gFlags` reads `0x10000D` on the wing skin
(FF_TEXTURE0 set) against `0x10000C` on the billboards drawn over it.

This was never lamp-specific. **Every** textured surface on that path was affected — canopy glass,
the afterburner shells, the pit's lamp lenses.

## What the models actually contain

Measured with `tools/models/objsurvey.py` plus a vertex-pool reader, over `F-16CJ_00_Nm_L1`
(LOD 2719, the player's jet) and `3DPIT_F16CJ_L1` (LOD 4105). **These numbers are the reference —
re-derive from the data, never from a claim in a doc.** This file used to assert the wingtips had
no lens at all. They have five. That one sentence cost three sessions.

### The lamp-lens signature

A lamp's colour lives in COLOR2 because there is nowhere else for it to live, so the diffuse is
**black**. That tell holds across the external model:

- **`node@55576`** — the intake nav lamps. One untextured `Alpha|VColor` surface, 24 vertices, two
  4-triangle fans. Each fan's centre is diffuse `0xFF000000` with emissive `0xFFFF0000` (port, red)
  / `0xFF00FF00` (starboard, green); the rim is `0x00000000` diffuse (alpha 0) with `0x00696969`.
  Colour 100% COLOR2, radial fade 100% COLOR1's alpha.
- **`node@74864`** — the landing-light beam cone, emissive `E5E5FF`.
- **`node@75776 … node@91152`** — the afterburner plume, ~1000 vertices, emissive `FF8000` /
  `FFFF80` / `0000FF` shock diamonds.

48 of the F-16's 208 plain surfaces carry a vertex emissive.

### The wingtip lamps: five billboards

Under the switch-8 DOF, five `Alpha|Texture|BillBoard` quads, each 1.5 ft square, authored at the
model ORIGIN and placed by its own `ROTATE` DOF:

| node | DOF translate | lamp |
| --- | --- | --- |
| `54524` | `-17.750, 0.010, -11.290` | tail strobe |
| `55036` | `-4.550, +15.030, -0.500` | starboard wingtip — **top** |
| `54812` | `-4.550, +15.030, +0.030` | starboard wingtip — **bottom** |
| `55484` | `-4.550, -15.010, -0.500` | port wingtip — **top** |
| `55260` | `-4.550, -15.010, -0.060` | port wingtip — **bottom** |

(−z is up here: the tail-tip strobe is at z −11.29 and the landing light at z +3.9 points down.)
That is exactly the real jet's layout — position/formation lights top **and** bottom at each tip.

They share texture slot 13, a 512×512 sheet whose **RGB is white and whose shape is entirely in the
alpha channel** (82% of the sheet is alpha 0). Each lamp gets its own cell: the port-wingtip cell,
uv `[0.529..0.978] × [0.022..0.471]`, is a red-tinted radial blob (texels `(198,60,57)` → white),
77% of it alpha 0. **The lamp's colour is in the texture**, which is why the emissive must be
modulated by it rather than added past it.

`node@54616` is a separate 4-vertex `POINTLIST` at the same positions, diffuse `0xFFFF0000` /
`0xFF00FF00`, zero emissive, `(0,0,0)` normal — one pixel per lamp, the pinpoint inside the corona.

### The pit's COLOR2 is two different things

All 396 pit surfaces carry a COLOR2, and it is **not** all self-illumination. `node@120` is the main
tub — 9720 indices, untextured — split by brightness:

| band | verts | extent | diffuse | what it is |
| --- | --- | --- | --- | --- |
| ≥160 (`C0C0C0`, `C70000`, `DBDBDB`) | 915 | one small cluster, x[-0.42 2.18] | **`0xFF000000`, all of them** | real lamps: caution panel + red warnings |
| 96–159 (`9A9E9E`, `7F7F7F`, `656565`) | 1345 | x[-6.48 11.87] — the **whole tub** | ordinary painted | baked shading |
| 32–95 (`313131`, `252525`) | 3778 | whole tub | ordinary | baked shading |
| <32 (`080808`) | 2885 | whole tub | ordinary | a 3% floor |

Adding that mid band unmodulated lights the cockpit up with every lamp switched off. But the pit LOD
also carries the player's **wings and stores** for the view out of the canopy, and their lamps must
keep glowing — so "is it a pit surface" is the wrong question. **Textured** is the right one:

| node | what | flags | diffuse | emissive |
| --- | --- | --- | --- | --- |
| `364640` | wingtip pair, y ±15.3 | `Alpha / Chroma / VColor / **Tex 10**` | `0xFF9E0F00` | `0x0FFFFFFF` |
| `364368` | tail strobe | `Alpha / Chroma / VColor / **Tex 10**` | `0xFFD62A00` | `0x0FFFFFFF` |
| `120` | the tub | `VColor`, **untextured** | painted | `9A9E9E` … `080808` |

D3D7 modulates the emissive by the texture, which bounds it by the art. It is only on an
*untextured* pit surface that COLOR2 is both unbounded and not a lamp. So the gate is "untextured
pit surface", not "pit surface".

An earlier note in `ffemu.hlsl` called pit COLOR2 "a subtle specular, not self-illumination". Half
right: it is both, and the diffuse is what tells them apart.

## What changed

| where | change |
|---|---|
| `CDXEngine::DrawSurface` | emissive defaults **on**, as D3D7 did; a `SwEmissive` surface whose switch is off turns it off. Knob `ObjEmissiveAll`. |
| same | the afterburner test now also requires `SwEmissive`. `SwitchNumber` is meaningless on other surfaces (it is 0), so without this the 44 plain Alpha surfaces on the F-16 alone would each be claimed as the AB cone and drawn additive with the flame gradient. |
| same | the blanket emissive is not applied to **untextured** pit-path surfaces. Knob `PitEmissive`. |
| `CDXEngine::DrawSortedAlpha` | invalidate `LastTexID` after `BeginObjectPass()`, so `FF_TEXTURE0` survives. No knob — the old behaviour is simply wrong. |
| `CDXLight::UpdateDynamicLights` | each light's `dcvAmbient` carried as a scale in `GpuLight.Color.w`. Knob `LightAmbient`. |
| same | the hard-coded `range * 8` shader cutoff for `a1 == 0` lamps is now `LightFalloffReach`. |
| `FFObjectLighting` (`ffemu.hlsl` + `ffobject.hlsl`) | `lit += Color.rgb * Color.w * atten`, **no N·L**. |
| `PS_Main` / `PS_Object` | the emissive is added to the lit material **before** the texture stage (D3D7 order), not past it. |
| `ObjectVSCore` (both) | the `FF_EMISSIVE` per-vertex branch carries `o.Spec`; it used to drop it, which was invisible only while `FF_EMISSIVE` meant a handful of surfaces. |
| `f4config.cpp` | `LightSprites` defaults **0**. |

## Knobs

| knob | default | what it does |
|---|---|---|
| `ObjEmissiveAll` | 1 | D3D7's emissive-from-COLOR2 on every surface. 0 = the port's inverted gate. |
| `PitEmissive` | 0 | extend it to **untextured** pit surfaces too. Off: their COLOR2 is baked shading, and adding it lights the cockpit with every lamp off. 1 shows the alternative, lit legends and all. |
| `LightAmbient` | 1.0 | scale on each light's own ambient term. 0 = the port's diffuse-only lighting. |
| `LightFalloffReach` | 8.0 | how far past its authored range a lamp with no falloff in its data (`a1 == 0`) reaches. 8 = wide soft wash (a 3 ft nav lamp still lights skin 24 ft away — this is why one wingtip reddens the forward fuselage); 1 = D3D7's literal flat-to-range; 2–3 is the middle. The per-object cull still uses the authored range. |
| `LightFalloffD3D7` | 1 | authored `1/(a0 + a1*d)` attenuation instead of the port's linear ramp. |
| `LightSprites` | 0 | synthetic additive glow at each dynamic light. Only for lamps whose model has no lens; with the above fixed it double-lights every lamp that does. |
| `LightSpriteSize` / `LightSpriteGain` | 1.0 / 1.0 | sprite size and brightness, when enabled. |
| `PitFillScale` / `PitFillReach` | 1.0 / 10 | cockpit flood/instrument fill. |
| `PitShadow` / `PitShadowStrength` | 1 / 1.0 | cockpit sun shadows. |

## Evidence: how to get any of this again

**Model data.** `python tools/models/objsurvey.py C:\FreeFalcon6\terrdata\objects\KoreaObj
--tree <lod>` prints the lights pool and the switch-emissive surfaces. Everything else above was a
short script over `objsurvey`'s `walk_nodes` + `VERTEX` struct — per-surface diffuse/emissive/normal,
vertices within N feet of a light, COLOR2 histograms, DOF-placed billboards. The lamp signature to
search for is **`crgb == 0` with a bright emissive**. `--tree <lod> --switch N` reports geometry
under one switch, but note it walks *DOF subtrees* only: a surface can also be gated by its own
`SwitchNumber` field without living under a switch DOF.

**Model surgery, as a diagnostic.** `tools/models/lodlens.py` writes lamp lens fans into a LOD of
`KoreaObj.DXL` — append-only, the original record untouched, a JSON log beside the `.DXH`, a
`--revert` that is one 8-byte write, and a `.DXH` backup. Use it to answer *"would geometry here fix
this?"* in minutes before committing to engine work; `--dry-run` builds and verifies the record
without writing. It is **not** a way to ship a fix: a patch needs both the repointed `.DXH` (2.1 MB,
shippable) and an appended record inside the 219 MB `.DXL`, nobody has built that append-and-repoint
step for the installer, and it would be Korea-specific and fragile against any modified object
database. Anything fixed this way lives on one machine. Revert with
`python tools/models/lodlens.py <basename> --lod <n> --revert`.

**RenderDoc** (1.46, no standalone Python module — `qrenderdoc.exe --python <script.py>` runs one
and the full replay API is available). Four traps, each of which cost a round here:

- **`--python` does not forward arguments.** `sys.argv[1]` raises `IndexError` and the script dies
  before writing anything, which looks exactly like a hang. Hard-code the paths.
- **stdout goes nowhere** — write to a log file.
- **The process stays alive** after the script ends. Kill it (`Get-Process qrenderdoc |
  Stop-Process -Force`) before starting another, or they pile up and thrash.
- **`GetReadOnlyResources` over many events is very slow** on D3D12. Query one event, not forty.

Recipes: `SaveTexture` on the swapchain resource dumps the frame; `PixelHistory(texture, x, y,
subresource, typeCast)` (first argument is the TARGET TEXTURE, from the `Present(ResourceId::N)`
action name) — `Passed()` plus `depthTestFailed`/`backfaceCulled`/`shaderDiscarded` says which draw
wrote a pixel, and `preMod`/`postMod` reveal the effective blend. For the shader flags,
`GetConstantBlocks(ShaderStage.Pixel, True)` then `GetBufferData(resource, byteOffset, 8)`, and
unpack the first `uint`: that is `gFlags` (cbRender, b3).

Captures under `C:\FreeFalcon6\rdoc_captures\`: `graybox.rdc` (the one that solved it),
`cockpit_glow_blacksprite.rdc`, `extlamppower.rdc`, `redlight_blacksprite.rdc`,
`inside_outside_lampissue-frame1282.rdc`, `-frame2452.rdc`.

## Dead ends — do not repeat

- **"The real fix is art."** It is not. The lens geometry is in the LODs; the engine was discarding
  it. Read a surface's vertex COLOR2 *and its diffuse* before concluding a model has no source.
- **Reaching for art before the engine is ruled out.** Lens geometry was authored into LOD 2719 to
  give the wingtips a source they already had; the bug was `FF_TEXTURE0`. The tool itself is fine
  (see above) — the mistake was using it as a *fix* while the symptom was still unexplained. Its
  `--panel` cover quads, added to hide the grey box, were a straight regression: opaque `0xFF383838`
  slabs on both wingtip faces, visible in daylight, and they did not hide it.
- **Tuning the light sprites.** Three rounds of profile/size/nudge work all chased a black *housing*
  that was really a black *lens*. A pixel history proved the sprite drew and passed at the black
  pixels, which should have been the clue that the black came from a later, opaque draw.
- **The falloff curve** was measured to help the *spill*, not the *lens*.
- **Gating on "is it the pit".** `m_SurfacePit` is the whole pit **path**, which carries the
  player's wings and stores. The same trap is already documented for `g_bObjCullPit`.

## Still open

- The **cockpit black dot** was never separately proven to be bug 3, but the pit carries the same
  class of geometry on the same path and it went away with the rest. If it returns, `gFlags` at that
  pixel is the first thing to read.
- The object pass is brighter at night than the terrain (models use day albedo × TOD ambient; the
  terrain has night tiles). See `RENDER-LIGHTING.md`.
- The pit's own caution-panel legends (the 915 black-diffuse verts in `node@120`) stay dark, because
  the pit gate is per-surface and they share an untextured surface with the baked shading. A
  per-vertex rule — glow only where the diffuse is black — would light them; it is a shader change
  and a rule inferred from the data rather than read out of D3D7, so it was not taken.
- The Vulkan backend's SPIR-V blobs are regenerated by `tools/build_shaders.bat`, which
  `dxengine.vcxproj` runs pre-build, so `ffobject.hlsl` stays in step automatically on Windows.
