# Object rendering: the fin flicker, dynamic lights and per-pixel lighting

Working notes from the 2026-09-19/20 session. Everything here was measured against the shipped
Korea data and against RenderDoc captures of the running game — re-run the tools rather than
re-deriving.

## State of play (read this first)

Five fixes are committed on this branch (2026-09-20) and deployed to
`C:\FreeFalcon6\FFViper.exe`:

| fix | status | knob |
|---|---|---|
| Missile fin flicker (§1) | **verified by the user in 2D**; VR look pending | `g_nObjCullMode`, `g_bObjCullPit` |
| F-16 strobe lighting its own jet (§2) | built; **not yet re-checked in the field** | `g_bObjLightMasks`, `g_bObjSpotCones` |
| Per-pixel object lighting (§3) | user: "looks better"; VR cost **not measured** | `g_bObjPixelLight` |
| Light strips rendered as flat red panels (§4) | built; **not yet re-checked in the field** | — (corrected semantics) |
| Canopy/glass surfaces not lit (constant gold at night, §5) | built; **not yet re-checked in the field** | — (state fix) |

To A/B any of them, set its knob to 0 in `C:\FreeFalcon6\ffviper.cfg` and restart the game (the
exe and the cfg are only read at launch). To rebuild after a shader edit, **touch
`src/falclib/include/falcon4.rc`** first (the shader is embedded as an RCDATA resource) and
check the deployed exe contains the new string — see §6.

## 1. The missile fin flicker — solved (verified in 2D)

**Symptom.** Missile fins (and other small, thin detail) flicker/speckle in motion — a moiré
that re-rolls every frame. Visible in the frame-552 thumbnail as a hatch pattern across the
fins.

**Root cause.** The fins are thin tapered plates: two nearly-coincident faces a few ULP apart
at range. With the object pass unculled, both faces rasterise at the same depth and the
per-pixel winner rides on depth noise, so the fin speckles between its two sides.

**Why the first fix did not work — the wing stores ride the PIT list.** The player's own
stores are drawn from the VB manager's `PitList`: `CockAttachWeapons()` then
`SetPitMode(true)` / `VCock_DrawThePit()` / `SetPitMode(false)` wrap them, so their surfaces
carry `m_SurfacePit = true`. The first version of the cull **excluded the pit** (to protect the
cockpit), so the missiles were never culled while every other object was. Proven in a capture:
other objects read `CullMode.Front`, the missile body read `CullMode.NoCull`.

**The cull direction is inverted in the port — load-bearing fact.** The projection built in
`Render3D::SetFOV` / `SetVRFrustum` carries a deliberate reflection (`Flip.m02 = 1`,
`Flip.m21 = -1`, `Flip.m10 = 1`, i.e. `det = −1`) for the RH→LH conversion. A reflection
reverses screen-space winding, so **D3D7's cull-back is the port's CULL_FRONT**. Field-tested
both ways: cull-back makes **airbase ground/pavement vanish**; cull-front leaves it intact.

**Fix.**
- `g_nObjCullMode` (default 1): 0 = none, 1 = D3D7 parity (cull front), 2 = the other side
  (the naive guess; kills the ground). `DrawSurface` sets it per surface through
  `IRenderer::SetObjectCull` (D3D12 implements it; Vulkan is a no-op default for now).
- `g_bObjCullPit` (default 1): also apply it to pit-list draws, so the attached stores are
  covered. 0 restores the old pit no-cull if the cockpit interior ever loses panels.

**Ruled out, do not re-chase:**
- `dwzBias`. Restored earlier; no effect (both fin faces share the surface).
- The double-wound-triangle theory: there are **no coincident triangle pairs** anywhere in
  these meshes. The antiparallel vertex pairs are where a tapered plate's two faces meet at
  the knife edge, not a second winding.
- Dynamic lights — the user confirmed the flicker is light-independent.
- The mesh data itself: no zero-length or non-unit normals, no triangles spanning antiparallel
  normals, no degenerate slivers, ordinary texels. The defect was never in the model.

## 2. The F-16 external strobe flash — fixed, awaiting a field check

**Symptom.** The external F-16's shading flashes white with every anti-collision/strobe pulse.

**Root cause.** The object light set dropped the D3D7 per-light flags and the spot cone:
- `OwnLight` — the light may only light the object that owns it (nav/formation lights).
- `NotSelfLight` — it must NOT light its own object (the strobe, particle/muzzle lights).
- The strobe is a `D3DLIGHT_SPOT` (≈5° inner cone, **range 1500 ft**), and every port light
  was uploaded as an omni point — so it lit the jet itself and everything within 1500 ft.

**Fix.** `g_bObjLightMasks` (default 1) honours OwnLight/NotSelfLight against the object's
light owner; `g_bObjSpotCones` (default 1) applies the cone in both object shaders
(`Params.y=2`, `z=cos(outer/2)`, `w=cos(inner/2)`, `Direction` = beam axis). The spot axis is
now transformed as a direction (the legacy `TransformCoord` added the object's translation to
it).

**F-16CG light table** (no re-dump needed): switch 8 = nav/formation (green/red/white,
`OwnLight`, ranges 1.9–3 ft), switch 9 = strobe (white **spot**, `NotSelfLight`, range 1500;
plus a `Static` white point range 4 used for the emissive precompute), switch 127 = the small
cyan/white panel lights (`OwnLight`, `Static`). `Static` lights are excluded from the dynamic
set by design (they only feed the emissive precompute).

## 3. Per-pixel object lighting — landed (user: "looks better")

**Symptom that forced it.** The F-16's intake-side red lens (a `range 3 ft` lamp) washed whole
multi-foot fuselage panels red. The range was uploaded correctly — the light model was simply
evaluated **per vertex** (legacy D3D7 Gouraud), so a vertex inside the lamp's 3 ft picks up the
colour and interpolation smears it across the entire triangle. On low-poly airframes that is
"the whole surface near the light turns red".

**Fix.** The light model now lives in ONE place per backend — `FFObjectLighting` in
`ffemu.hlsl` (D3D12) and `ffobject.hlsl` (Vulkan) — used by the legacy per-vertex path and by a
new per-pixel path gated on `FF_PIXELLIGHT` (bit 20). The VS then passes world normal / world
position / camera→point vector; the PS normalizes and evaluates the same sun+point+spot sum and
the Blinn-Phong specular per pixel. `FF_AFTERBURNER` keeps the VS's white→flame override;
`FF_EMISSIVE` surfaces are lit like everything else and their emissive is now ADDED (see §4);
terrain and 2D never set FF_LIGHTING and are untouched.

- Knob: `g_bObjPixelLight` (default 1). 0 = bit-identical legacy per-vertex path.
- Also kills the per-vertex specular popping on coarse geometry.
- Compile-checked against the game's own toolchain before shipping: FXC `vs/ps_5_0` (the base
  D3D12 blobs) and DXC `vs/ps_6_1` + `6_6` (VI/bindless) plus SPIR-V for the Vulkan twin.
- Watch when checking: cockpit look (now per-pixel too) and **VR frame cost** — the light loop
  runs per pixel in both eyes. If it costs frames, the cheap next step is an early-out for
  lamps whose range cannot reach the fragment.

## 4. The F-16 "intake red patches" — solved: the emissive rule (2026-09-20)

**Symptom.** With the light switch held on, hard-edged red patches cover whole polygons on the
left forward fuselage / intake area. Frame 632 (lights off) shows the same panels grey; frame
650 (lights on) shows them red. **`g_bObjPixelLight` did not change it** — because it was never
lighting.

**Root cause — the port REPLACES the material with the emissive colour; D3D7 ADDS it.** The
patches are the F-16's **formation-light strips**, thin surfaces along the fuselage sides
(`EID 28887`, idx 24, model x[8.9,13.6] y[-2.3,-1.4] z[2.0,3.2]; the matching right-side strip
is `EID 28893`). Their per-vertex **emissive** colours are red on the left (`0x004A0000`) and
green on the right (`0x00004300`) — a self-lit strip, exactly as authored. But:

- port (before this fix): `col.rgb = i.Emissive.rgb` (+ texture) — the whole surface renders as
  a flat saturated self-lit panel;
- D3D7 fixed function: `materialDiffuse x (ambient + sum lights) + materialEmissive`, i.e. the
  emissive is added on top of the *lit* material, so a glow strip reads as a thin glow.

Proven with pixel history + shader-output reads: at a red pixel the shader output was *pure red*
(R>0, G=B=0) with a light set of `sun + green lamp`, white vertex colours, and a texture — the
only red input was `i.Emissive`. Everything drawn over it later either failed the depth test or
was backface-culled.

**Fix.** For `FF_EMISSIVE` surfaces (not `FF_AFTERBURNER` — that mapping is deliberate, #49),
the lighting is computed as usual and the emissive is **added**:
`col.rgb = vertexColor * saturate(lit) + i.Emissive.rgb` (per-vertex path), or
`c.rgb = lit(...) * vertexColor * ... + i.Emis` in the per-pixel block. The emissive colour now
travels to the PS in its own interpolant (`Emis`, TEXCOORD9 / location 9). AB keeps its white
override. Both backends updated (`ffemu.hlsl`, `ffobject.hlsl`).

**Epilogue.** This was the last of the three "lighting" symptoms and it was never the light
model: the model data was right, the port's emissive shortcut was wrong. Also explains why the
patch tracked the *light switch* (the strips are switch-gated) and why `dwzBias`, culling and
per-pixel lighting all left it alone.

## 5. Sorted-alpha model surfaces were drawn unlit — canopy glass edition (2026-09-20)

**Symptom.** The F-16's cockpit glass never responds to outside brightness: at night the canopy
still carries its constant gold tint (and the same in reverse in daylight, presumably).

**Root cause.** Alpha surfaces of models are deferred and drawn through the **sorted-alpha
path**: `PushSurfaceIntoSort` (`DrawNode`) → `DX2D_AddObject` → `DX2D_Flush2DObjects` →
`DrawSortedAlpha` (`dxengine.cpp`). That path never restored the object pass's flag word, so the
draws ran on **whatever 2D state happened to run last** — in the capture `flags = 0x000005`
(`FF_TEXTURE0 | FF_VERTEXCOLOR`, **no `FF_LIGHTING`**). The shader therefore output
`material x vertexColour x texture` with no lighting at all. The canopy's vertex colours carry
the gold (`0x...9F9560`), so it rendered as a constant, unlit gold surface.

**Fix.** `DrawSortedAlpha` calls `g_pRenderer->BeginObjectPass()` before
`SetObjectAlphaBlend(true)`, per item (2D items can run between 3D ones). The object pass base
flags (`FF_LIGHTING | FF_PIXELLIGHT | NVG/IR`) are restored; `DrawSurface` still re-issues the
per-surface caches (texture, specular, `dwzBias`, emissive/afterburner). The alpha blend and
no-depth-write state are applied right after, as before. Works on both backends (BeginObjectPass
is on `IRenderer`).

**Proven from the capture** (`cockpit_glow_texture_fix.rdc`): canopy draws `EID 3666` (idx 228)
and `EID 3708` (idx 408), vertex colours gold with alpha, `flags 0x000005` — and their shader
output at night was ~(0.6, 0.58, 0.38) x alpha over a black sky. Note the *taxi-light beam cone*
(`EID 1933`, node 30) is a genuinely SwEmissive surface and is expected to glow.

**Files:** only `dxengine.cpp` (`DrawSortedAlpha`).

## 6. RenderDoc recipes (the tooling that worked)

- Installed: `C:\Program Files\RenderDoc` (v1.46). `renderdoccmd thumb -o out.png file.rdc`
  prints a frame thumbnail — the fastest way to see whether a capture is even useful.
- Headless-ish scripting: `qrenderdoc --python script.py` (no capture argument) runs a script
  where the global `pyrenderdoc` is a `CaptureContext`. Load a capture with
  `pyrenderdoc.LoadCapture(path, renderdoc.ReplayOptions(), "", False, True)` and poll
  `IsCaptureLoading()` / `IsCaptureLoaded()`.
- Data access: `bc = pyrenderdoc.GetBlockingController()` — `GetBufferData(rid, off, len)` →
  bytes, `GetTextureData(rid, sub)`, `PixelHistory(tex, x, y, sub, cast)`,
  `GetCBufferVariableContents(...)` (8 args: pipeline, shader, stage, entry, cbuf slot,
  buffer, offset, length). `bc.SetFrameEvent(eid, True)` moves the replay.
- Constant-buffer layout (see `d3d12renderer.cpp`): `b0` cbViewport, `b1` cbView (view/proj/
  camPos), `b2` cbObject (world), `b3` cbRender (flags, alphaRef, fog, chroma, material,
  specular, clouds...), `b4` cbLights = `float4 ambient; uint count;` **at byte 16 as a uint —
  reading it as float is wrong** — then 8 lights of 64 B: position, direction, colour,
  params (x=range, y=type 0/1/2, z/w = spot cosines).
- The port's projection is reflected (see §1), and the object pass draws the *player's own*
  wing stores from the pit list — so those draws are `CullMode.NoCull` in captures taken
  before `g_bObjCullPit`.
- **VR captures are not useful**: on D3D12 there is no desktop eye mirror any more (the D3D11
  one was purged), so RenderDoc only sees the desktop back buffer — the menu or a black frame.
  Capture **flat** for now.
- Shader edits: `ffemu.hlsl` is baked into the exe as an RCDATA resource
  (`falcon4.rc` → `FFEMU_HLSL`), and the renderer prefers an external `shaders\FFEmu.hlsl` if
  one exists (none does in the install). After editing the shader, **touch `falcon4.rc`** or
  MSBuild will not re-embed it; verify with a string search in the built exe
  (`FF_PIXELLIGHT` must be present). The base D3D12 blobs compile with **FXC**
  (`vs/ps_5_0`, `d3dcompiler_47`) and the VI/bindless ones with **DXC**
  (`vs/ps_6_1`, `6_6`); test both before shipping.
- Captures are 300–460 MB each; processing every draw with buffer reads takes minutes and
  loads the CPU. Kill stale `qrenderdoc` runs before starting another.

## 7. Knobs added this session (all default to the new behaviour)

| knob | default | meaning |
|---|---|---|
| `g_nObjCullMode` | 1 | 0 = no cull, 1 = D3D7 parity (cull front), 2 = the other side (kills the ground) |
| `g_bObjCullPit` | 1 | apply the cull to pit-list draws too (the wing stores ride that path) |
| `g_bObjLightMasks` | 1 | honour `OwnLight` / `NotSelfLight` |
| `g_bObjSpotCones` | 1 | apply spot-light cones instead of omni points |
| `g_bObjPixelLight` | 1 | per-pixel object lighting (0 = legacy per-vertex) |
| `g_bObjZBiasEnable` / `g_nObjZBiasStep` | 1 / 60 | (earlier) per-surface `dwzBias` in the object pass |

## 8. Files touched (uncommitted)

`src/graphics/dxengine/common/irenderer.h`, `.../common/ffstatemap.h`, `.../dxengine.cpp`
(cull call + `m_SurfacePit` + `DrawSortedAlpha`'s object-pass state), `.../dxengine.h`
(`SurfaceItemType::Pit`), `.../dx2dengine.cpp` (radar blit opts out), `.../dxlightengine.cpp`
(light flags, spot fields, spot-axis transform), `.../d3d12/d3d12renderer.{h,cpp}` (cull + light
flag), `.../common/shaders/ffemu.hlsl`, `src/graphics/shaders/ffobject.hlsl`,
`src/graphics/shaders/ffcommon.hlsli`, `src/graphics/vulkan/vulkanrenderer.cpp` (flag),
`src/ui/src/f4config.cpp` (knobs). Deployed exe:
`Falcon4___x64_Release\FFViper.exe` → copied to `C:\FreeFalcon6\FFViper.exe`
(the pre-session exe is backed up as `FFViper.exe.bak-objcull`).

## 9. Next actions for a following session

1. Field-check the two unverified fixes: the strobe flash (external view, strobe on) and the
   light strips (they should read as a *thin* red/green glow on a lit panel, not flat slabs).
2. VR: confirm the fin flicker is gone with `g_bObjCullPit 1` (fixed in 2D; the pit itself is
   culled now — if any cockpit panel disappears, set `g_bObjCullPit 0` and scope the exclusion
   to the pit model instead of everything attached to it).
3. Watch VR frame cost with per-pixel lighting (the light loop runs per pixel in both eyes).
   If it bites: early-out for lamps whose range cannot reach the fragment, and/or keep the pit
   on the legacy path.
4. The code and this doc are committed; when the field checks pass, add the user-facing summary
   to the `CHANGELOG.md` render section.
5. Open (not chased): the RenderDoc desktop mirror for VR is gone on D3D12 (the D3D11 one was
   purged). Reinstating it would let captures be taken in the headset — see §6.
