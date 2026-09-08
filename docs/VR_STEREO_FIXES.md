# VR: plain-stereo convergence and clickable-cockpit aim fixes

Fixes a set of related VR bugs that only affect the **plain-stereo** path (2-view
`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`). The quad-views path was already correct, which is
why this went unnoticed: several constants and code paths were calibrated against quad-views
hardware, and plain stereo takes different branches.

Reproduced and fixed on a **Meta Quest 3 over Quest Link (Meta's native OpenXR runtime), D3D12
backend**. Any HMD whose per-eye frusta are canted outward will hit the primary bug.

---

## 1. Everything doubles / will not converge (the primary bug)

**Symptom.** In the 3D pit, the two eyes never fuse. Both the cockpit and the *outside world*
appear doubled, at every depth, in a way no IPD adjustment affects.

**Cause.** The runtime reports strongly asymmetric per-eye FOV. On a Quest 3:

| eye | angleLeft | angleRight | angleUp | angleDown |
|-----|-----------|------------|---------|-----------|
| 0 (L) | -54° | +40° | +44° | -55° |
| 1 (R) | -40° | +54° | +44° | -55° |

Each eye's cone is canted ~7° outward. The plain-stereo branch in `OTWDriverClass::RenderFrame`
rendered a **symmetric** frustum of total width `hf = angleRight - angleLeft` and submitted a
matching symmetric FOV via `SetSubmitFov()`:

```cpp
if (sessionQuad and haveFov)
    renderer->SetVRFrustum(fl, fr, fu, fd);   // quad: TRUE off-axis frustum
else {
    renderer->SetFOV(hf);                      // stereo: SYMMETRIC
    g_pOpenXRBackend->SetSubmitFov(hf, vf);    // ...and submitted symmetric
}
```

That is only self-consistent if the runtime honours an arbitrary app-chosen FOV. Where the
compositor instead maps the image onto the eye's real cone, each eye's image centre lands ~7°
off-axis **in opposite directions** — ~14° of divergence, present at every depth. Divergent
disparity cannot be fused by the human visual system at all, hence "nothing converges."

Distant geometry doubling is the diagnostic tell: true IPD parallax goes to zero at infinity, so
doubled clouds mean the error is *angular*, not translational. (Confirmed with a RenderDoc capture:
a distant cloud landed on **identical pixels** in both eye render targets, while a near dashboard
part differed by 152px — i.e. the rendered stereo pair was geometrically correct, and the error was
introduced at composition.)

**Fix.** Plain stereo now renders and submits the runtime's true off-axis per-eye frustum, exactly
as the quad path always has. New `OpenXRBackend::ClearSubmitFov()` drops the symmetric submit-FOV so
`EndEye()` submits `views[eye].fov`.

Files: `sim/otwdrive/otwloop.cpp`, `graphics/dxengine/openxrbackend.{h,cpp}`
Knob: `VrTrueEyeFov` (default **on**; `0` = previous behaviour)

---

## 2. Stereo drifts as you look away from centre

**Symptom.** With #1 fixed, straight-ahead converges — but looking **down-left / down-right** at the
aux panels goes misaligned again, worsening the further the head turns.

**Cause.** In `VCock_HeadCalc()` the per-eye IPD was added to the body-frame lateral axis and rotated
into the world by `ownshipRot` — the **airframe's** orientation, which contains no head rotation:

```cpp
posCam.y += GetEyeLateralOffsetFeet(xeye);            // body right
MatrixMult(&OTWDriver.ownshipRot, &posCam, &posW);    // aircraft orientation
```

The eyes are separated across the skull, so that separation rotates with the **head**. Body-right and
head-right coincide only while looking forward, which is exactly why the error was zero at centre and
grew with head rotation. `RenderWorldViewInstanced` already did this correctly (it rotates its eye
offset by `camRot`), so the two paths disagreed.

**Fix.** Head lean still goes through `ownshipRot` (it is a body-frame 6DOF translation); the per-eye
IPD is now rotated by `cameraRot` (`= ownshipRot * headMatrix`).

Also hardened: `eyeLatFeet[]` is now computed by projecting the eye-to-eye delta onto the **head's own
right axis** rather than taking the raw `appSpace` X component. The raw component is only "lateral"
while the head faces the reference space's forward direction; past ~90° of yaw it can invert, which made
the effective IPD sign depend on which way the headset happened to face at recenter.

Files: `sim/otwdrive/vcock.cpp`, `graphics/dxengine/openxrbackend.cpp`
Knob: `VrHeadRelIpd` (default **on**)

---

## 3. Clickable cockpit: cursor and hit-test disagreed

Three separate defects, all downstream of #1.

**3a. Hit-test projected with the wrong vertical.** The pick deliberately symmetrised the vertical
frustum while keeping the horizontal off-axis. That matched the old symmetric render, but after #1 the
cockpit is drawn with the true vertically-canted frustum (`U=+44°, D=-55°`), leaving the projected
buttons ~10% of screen height from where they are drawn. The hit-test now tracks whatever the render
actually used.

**3b. Cursor drawn with the wrong projection.** `VrSetEyeCam()` (which sets the projection the cursor
glyph is drawn through) still hard-coded the symmetric-stereo assumption, so the cursor was drawn
through a different projection than the cockpit it sits on — cursor over one panel, pick firing on
another. Now mirrors the render.

**3c. Cursor stereo offset along the wrong axis.** The cursor anchor lives in cockpit/body space, and
the per-eye offset was applied straight to `.y` (airframe right) — the same class of bug as #2. Measured:
the two eyes drew the cursor **634px apart** aiming at the lower-left panel, versus ~226px near centre.
The offset is now rotated by `headMatrix`.

Also fixed: the free-aim cursor depth. With no button hit, the anchor used `dPt`, the nearest button's
projection *along the ray* (`dist × cos θ`). Aimed away from the panel that cosine collapses, parking the
cursor far nearer than anything real and splitting it in two mid-screen — while `VrRayReach`, the knob
meant for exactly this, never applied because some button is always "nearest". `dPt` is now used only when
it is actually near the panel plane.

Files: `sim/otwdrive/vcock.cpp`
Knob: `VrHeadRelCursorIpd` (default **on**)

---

## Configuration

All three fixes default **on**; each can be reverted individually via `FFViper.cfg`:

```
set g_bVrTrueEyeFov 0        # revert #1
set g_bVrHeadRelIpd 0        # revert #2
set g_bVrHeadRelCursorIpd 0  # revert #3c
```

`VrRayReach` becomes load-bearing for the free-aim cursor: it should be set to the cockpit panel depth
in button units (~814 on an F-16 pit at default seat position).

Note that several existing hand-tuned constants were compensating for these bugs and should now sit at
their neutral values — in particular `VrDetectBiasXStereoDx12` (default `-180.0f`), which existed to
paper over the 3a/3b projection mismatch.

---

## Known remaining issues (not addressed here)

- **View instancing still needs the #1 fix.** `RenderWorldViewInstanced` retains the old symmetric
  `SetFOV`/`SetSubmitFov` for its world pass, so enabling `VrViewInstancing` on plain-stereo hardware
  reintroduces the divergence. It must stay off (`VrViewInstancing 0`, `VrD3D12ViCockpit 0`) until the
  same change is ported there. Worth doing — VI roughly halves world geometry submission.
- **External views have no head tracking.** Every HMD orientation/position consumer
  (`GetHeadYawPitchRoll`, `GetHeadBasis`, `GetHeadPosFeet`) is gated to cockpit display modes, so in the
  external/orbit views (9/0) the scene is rendered from a head-unaware camera while the real head pose is
  still submitted to the compositor. The result is that head movement appears to move the world the wrong
  way. Implementing this means composing the head basis into the external camera's orientation.
