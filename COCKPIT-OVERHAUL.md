# Cockpit overhaul — textures, fonts, shading

**Status: not started.** Reconnaissance only. Append findings here rather than growing
`WIP-NOTES.md`. Shading overlaps heavily with `RENDER-LIGHTING.md`; read both.

## The goal

Better cockpit textures, better and more realistic fonts, better shading.

## Fonts: there are two unrelated systems, and only one of them matters here

This is the single most useful thing to know before starting, because they look like one
system and are not.

**1. UI / menu fonts.** `.bft` bitmap fonts in `art/fonts/`, indexed by
`art/fonts/FONTIDS.ID`: Arial (12/14/16), Arial Narrow, Bank Gothic (14/16/20/24/26),
Haettenschweiler (16/18/24), OCR-A/OCR-B extended (10/12/14/16/24/56), Zurich (12–24). These
drive the menus and the 2D UI. **Not** what the cockpit displays use.

**2. Cockpit / RTT display fonts.** Texture-mapped GIF atlases with a companion `.rct` metrics
file, loaded in `Render2D::Load2DFontSet` (`render2d.cpp:747`):

| index | texture | metrics |
|---|---|---|
| 0 | `art/ckptart/6x4font.gif` | `6x4font.rct` |
| 1 | `art/ckptart/8x6font.gif` | `8x6font.rct` |
| 2 | `art/ckptart/10x7font.gif` | `10x7font.rct` |
| 3 | `art/ckptart/warn_font.gif` | `warn_font.rct` |

Everything drawn on a cockpit display — HUD, DED, PFL, MFDs, RWR, the 3D kneeboard — uses
these. Selected by `VirtualDisplay::SetFont(n)`; the active set is `pFontSet` (`Font2D` /
`Font3D`). With `g_bAutoScaleFonts` the loader picks resolution-specific variants from an
`autofont` directory instead, including widescreen-specific sets.

**There are only three sizes.** Index 3 is `warn_font`, a *different typeface*, not a fourth
size. Any plan that assumes a continuous size range is wrong from the start.

### Why the display text looks soft, precisely

`g_rttFontScale` is set to `g_rttSS` (3 by default) during the RTT pass. `ScreenText` scales
the **glyph quad geometry** by that factor and leaves the **UVs untouched** — see the `#7`
comment at `render2d.cpp:430`. So a 10×7 glyph is magnified 3× in the atlas. Apparent size
goes up; sharpness cannot, because the source bitmap is the ceiling.

That is the whole reason "make the font nicer" is not a tuning job. Options, roughly in order
of cost:

- **Higher-resolution replacement atlases.** Keep the pipeline, redraw the GIFs at 3–4× and
  rewrite the `.rct` metrics. Cheapest real improvement. Risk: every display's layout is tuned
  to the current glyph metrics, so changing advance widths moves text on the HUD, DED and MFDs
  at once.
- **A fourth, larger size.** `Font2D.totalFont` already grows from 3 to 4 when `warn_font`
  loads, so the loader tolerates a variable count — but index 3 is taken. Adding index 4 means
  touching the loader and the `FontSet` array bounds.
- **SDF / MSDF glyphs.** Resolution-independent, correct answer long-term, and the right
  match for VR where you lean in and the panel fills your view. Needs a new shader path and a
  generator; the existing `ScreenText` emits textured quads, so the geometry side is already
  most of the way there.

Realism note: the F-16's DED and MFDs use a specific line-drawn character set, and the HUD
another. "More realistic" probably means redrawing to match those, which is an art task with
a metrics dependency, not a code task.

### Related, already done

The 3D kneeboard has its own font selector, `g_nKnee3DFont` (`f4config.cpp`), defaulting to 2
(10×7) instead of the 2D board's `kneefont` (0, 6×4). That is a per-display override of the
same three-size set — a pattern worth reusing, not a fix for the underlying limit, and it is
the concrete evidence that the limit bites: 10×7 is the largest matched size there is, and on
a 5.6 in page seen at an angle in a headset it is still only just adequate. That page renders
708×942 actual pixels, so the shortfall is not resolution — it is the source bitmap.

## Textures

The 3D pit is **parent 2402, LOD `3DPIT_F16CJ_L1` (4105)**, selected by `cockpitmodel 2402`
in `art/ckptart/3Dckpit.dat`. The LOD carries **11 textures** and 63,258 vertices across 396
surfaces. `tools/models/objsurvey.py` lists them — use it rather than guessing which art file
is which.

Not yet established, and worth finding out before any art work:

- Where those 11 textures live on disk and at what resolution.
- Whether the texture bank imposes a size cap (`TheTextureBank`, `texbank.h`).
- Whether the pit textures are palettised. If they are, higher-fidelity art means changing
  format, not just resolution — and `ApplyLightingToPalette` exists precisely because some
  cockpit art is 8-bit.

## Shading

Covered in `RENDER-LIGHTING.md`; the cockpit-specific facts:

- Cockpit lighting is a set of special cases inside `ffemu.hlsl`, not a separate path:
  ambient-floor damping (~line 85), an N·L gradient applied **only by brightening lit faces**
  (~line 355), ambient dimmed by sun level at night (~line 363).
- **Most pit surfaces carry no material specular** (`SpecularIndex = 0`, `#72` note at ~line
  399), which is why the pit reads matte and dead. This is a **data** problem as much as a
  shader one — a specular response needs per-surface material data the BSP does not currently
  carry.
- There are no shadows of any kind in the cockpit. See `RENDER-LIGHTING.md`.

## Suggested first step

Pick one display and do the whole loop on it — new atlas, new `.rct`, look at it in the
headset. The DED is a good candidate: small, fixed-width, a well-known real typeface, and its
zone in the RTT atlas is only 199×80 base so mistakes are cheap and visible. That answers "can
we improve these fonts at all without disturbing every other display" before committing to a
full redraw.

## Do not re-derive

- Two font systems; the cockpit one is the GIF+`.rct` set in `art/ckptart/`, three sizes plus
  a different-typeface warn font.
- `g_rttFontScale` scales glyph geometry, not UVs. Source bitmap = sharpness ceiling.
- The pit is one BSP model, parent 2402 / LOD 4105, 11 textures.
- The RTT atlas facts (768 base, zone occupancy, blend chars) are in `WIP-NOTES.md`.
