#include "stdhdr.h"
#include <float.h>
#include "lantirn.h"
#include "graphics/include/renderir.h"
#include "graphics/include/fflog.h"
#include "graphics/include/imagebuf.h" // Artscout - 2026: off-screen RTT for FLIR/TGP scene
#include "graphics/include/rviewpnt.h"
#include "otwdrive.h"
#include "simdrive.h"
#include "aircrft.h"
#include "airframe.h"
#include "sms.h"

#include "campbase.h"
#include "camplist.h"
#include "../../graphics/include/drawbsp.h"
#include "falclib/include/dispopts.h"

extern bool g_bTFRFixes;
extern bool g_bHudFlir;
extern float g_fHudFlirTrim, g_fHudFlirScale, g_fFlirCooldown;

LantirnClass *theLantirn;

LantirnClass::LantirnClass() : DrawableClass()
{
    display = NULL;
    m_pRTT =
        NULL; // Artscout - 2026: off-screen RTT for the FLIR/TGP scene (D3D11)
    m_pMfdImage =
        NULL; // Artscout - 2026: MFD 2D surface we read the scene back into
    m_flags = AVAILABLE;
    m_tfr_alt = 1000;
    m_tfr_ride = TFR_SOFT;
    m_tfrmode = TFR_STBY;
    scanpos = 0.0f;
    scandir = 1.0f;
    scanrate = 60.0F * DTR;
    gdist = holdheight = turnradius = 0.0f;
    evasize = 0;
    pitch = 0.0f;
    m_fscale = 1.10f; // don't ask - they appear to work.
    m_dpitch = -0.0872665f;

    PID_MX = 0.0F;
    PID_lastErr = 0.0F;
    PID_error = 0.0F;
    PID_Output = 0.0F;
    MaxG = 0.0F;
    MinG = 0.0F;
    gAlt = 0.0F;
    lookingAngle = 0.0F;
    gDist2 = 0.0F;
    gammaCorr = 0.0F;
    gamma = 0.0F;
    min_Radius = 0.0F;
    min_safe_dist = 0.0F;
    SpeedUp = FALSE;
    m_hudIr = NULL;
    m_hudTex = NULL;
    m_hudTexSize = 0;
    m_hudTexValid = false;
    flirCool = 0.0F;
    flirGain = 50.0F;
    flirLevel = 100.0F;
    flirManual = false;
    hudBright = 0.6F;
    bsgt = false;
    hudTrimPitch = hudTrimYaw = 0.0F;
    coolTime = 0.0F;
    powerInit = false;
    powerSeen = true;
    limitExceeded = false;
    turnAdvisory = 0;
    sideFrame = 0;
    roll = 0.0F;
    featureDistance = 0.0F;
    featureDistance2 = 0.0F;
    featureDistance3 = 0.0F;
    featureHeight = 0.0F;
    featureHeight2 = 0.0F;
    featureHeight3 = 0.0F;
    featureAngle = 0.0F;
    featureAngle2 = 0.0F;
    featureAngle3 = 0.0F;
}

LantirnClass::~LantirnClass()
{
    DisplayExit();

    if (m_hudIr)
    {
        m_hudIr->Cleanup();
        delete m_hudIr;
        m_hudIr = NULL;
    }

    if (m_hudTex)
    {
        delete m_hudTex;
        m_hudTex = NULL;
    }

    // Artscout - 2026: free the off-screen FLIR/TGP RTT.
    if (m_pRTT)
    {
        m_pRTT->Cleanup();
        delete m_pRTT;
        m_pRTT = NULL;
    }
}

void LantirnClass::DisplayInit(ImageBuffer *image)
{
    RenderIR *irrend = (RenderIR *)privateDisplay;

    // Already set up for this MFD surface?
    if (irrend and m_pMfdImage == image)
        return;

    DisplayExit();

    if (m_pRTT)
    {
        m_pRTT->Cleanup();
        delete m_pRTT;
        m_pRTT = NULL;
    }

    irrend = new RenderIR;
    privateDisplay = irrend;
    m_pMfdImage = image;

    // Artscout - 2026 (D3D11 purge): the D3D11-only off-screen-RTT path for the FLIR/TGP scene was removed
    // (m_pRTT stays NULL under D3D12; useRtt is hardcoded false, so the sensor goes through the shared MFD atlas).
    ImageBuffer *target = image;

    irrend->Setup(target, OTWDriver.GetViewpoint());
    irrend->SetColor(0xffffffff);
    irrend->SetFOV(10.0F * DTR);
}


void LantirnClass::Display(VirtualDisplay *newDisplay)
{
    display = newDisplay;
    DrawTerrain();
}


void LantirnClass::DrawTerrain()
{
    AircraftClass *playerAC = SimDriver.GetPlayerAircraft();

    Tpoint cameraPos;
    GetCameraPos(&cameraPos);

    // Artscout - 2026: THIS class is the HUD navigation FLIR (cockpit.cpp:78 -> theLantirn->Display),
    // NOT the MFD TGP (that is LaserPodClass/laserpod.cpp). It is SUPPOSED to render onto the HUD
    // viewport on screen (cockpit.cpp sets SetViewport(hudViewportBounds) before this). My earlier RTT
    // redirect mistook that intended HUD render for a "leak" and read the off-screen RTT back into the
    // FULL HUD image (0,0,w,h, ignoring the HUD viewport) -> the FLIR scene/target appeared as a grey
    // blob next to the HUD. Render directly to the HUD viewport like the original (no RTT here).
    extern bool g_bUseD3D12, g_bSensorSceneD3D12;
    // #DX12 A5: like laserpod/mavdisp/lantmfd, the sensor 3D-scene render runs only under D3D11 or when the
    // D3D12 sensor scene is explicitly enabled. This HUD FLIR renders the forward world a SECOND time via
    // DrawScene straight into the HUD viewport; under D3D12 it was NOT gated, so flying an AG pass at a target
    // airfield fed the airbase feature (a composite BSP) through the object path -> a building draw hung the
    // GPU (DEVICE_HUNG). Gate it off by default (FLIR shows no scene, same as the other sensors under D3D12).
    // Artscout - 2026: gate Vulkan off too. The Vulkan object/terrain path has no RTT routing at all
    // (VkBeginScenePass routes to menu/tail/scene only), so a sensor DrawScene records the IR-grey world into
    // the EYE's scene pass (black fill over the lower cockpit) and the ReBindRttTarget/SetViewportSize(atlas)
    // pair leaks the 512x512 metric into the composite (whole shrunk atlas inside the MFD). Same class of
    // breakage that hung D3D12; symbology-only until a real RTT object path exists.
    extern bool g_bUseVulkan, g_bSensorSceneVulkan;
    // Artscout - 2026: the HUD FLIR is NOT an RTT page -- it renders the forward world straight into
    // the on-screen HUD viewport by design, which under VR is garbage in the eyes (upside-down airbase
    // buildings, black fill). g_bSensorSceneVulkan does NOT enable it; only the explicit debug knob does.
    const bool doA5 = !(g_bUseD3D12 or g_bUseVulkan) || g_bSensorSceneD3D12;
    bool useRtt = false;
    RenderIR *pRender =
        useRtt ? (RenderIR *)privateDisplay : (RenderIR *)display;

    if (not pRender)
        return;

    if (useRtt)
    {
        // Artscout - 2026: the off-screen renderer (irrend) must use the SAME viewport+FOV as the
        // cockpit MFD renderer, otherwise the object-path projection (DrawScene uses the renderer's
        // current viewport) is wrong -> objects land off-place while the terrain (screen-path) looks
        // OK. Copy them from the cockpit display, then bind the off-screen RTT.
        if (display)
        {
            float vl, vt, vr, vb;
            ((RenderIR *)display)->GetViewport(&vl, &vt, &vr, &vb);
            pRender->SetViewport(vl, vt, vr, vb);
            pRender->SetFOV(((RenderIR *)display)->GetFOV());
        }

        pRender->context.StartFrame(); // bind + clear the off-screen RTT
    }

    pRender->StartDraw();
#if 0
    display->SetColor(0x03000000);
    display->Tri(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F);
    display->Tri(-1.0F, -1.0F, 1.0F, -1.0F, 1.0F, 1.0F);
    display->SetColor(0xffffffff);
#endif
    Trotation viewRotation;
    mlTrig tha, psi, phi;
    mlSinCos(&tha, playerAC->Pitch() + m_dpitch);
    mlSinCos(&psi, playerAC->Yaw());
    mlSinCos(&phi, playerAC->Roll());


    viewRotation.M11 = psi.cos * tha.cos;
    viewRotation.M21 = psi.sin * tha.cos;
    viewRotation.M31 = -tha.sin;

    viewRotation.M12 = -psi.sin * phi.cos + psi.cos * tha.sin * phi.sin;
    viewRotation.M22 = psi.cos * phi.cos + psi.sin * tha.sin * phi.sin;
    viewRotation.M32 = tha.cos * phi.sin;

    viewRotation.M13 = psi.sin * phi.sin + psi.cos * tha.sin * phi.cos;
    viewRotation.M23 = -psi.cos * phi.sin + psi.sin * tha.sin * phi.cos;
    viewRotation.M33 = tha.cos * phi.cos;

    Tpoint p;
    Trotation *r = &viewRotation;
    p.x = cameraPos.x;
    p.y = cameraPos.y;
    p.z = cameraPos.z;
    cameraPos.x = p.x * r->M11 + p.y * r->M12 + p.z * r->M13;
    cameraPos.y = p.x * r->M21 + p.y * r->M22 + p.z * r->M23;
    cameraPos.z = p.x * r->M31 + p.y * r->M32 + p.z * r->M33;

    if (doA5) // #DX12 A5: skip the sensor 3D-scene under D3D12 (default) -> no airfield-object hang
    {
        pRender->DrawScene(&cameraPos, &viewRotation);

        //JAM 12Dec03 - ZBUFFERING OFF
        // Artscout - 2026: in RTT mode ALWAYS flush the queued objects here, inside the off-screen RTT
        // bracket. Otherwise (bZBuffering off) the FLIR/TGP objects stay in TheDXEngine's global buffers
        // and get flushed later by the MAIN render against the back buffer -> objects appear mid-screen
        // instead of in the MFD. Flushing here both draws them into m_pRTT and empties the buffers.
        if (DisplayOptions.bZBuffering or useRtt)
            pRender->context.FlushPolyLists();
    }

    //    pRender->PostSceneCloudOcclusion();
    pRender->EndDraw();

    // Artscout - 2026: pull the FLIR/TGP scene out of the off-screen RTT into the MFD's 2D surface,
    // then restore the back buffer. (Munitions 3D-viewer readback pattern, BlitRttTo565.)
    if (useRtt)
    {
        pRender->context.FinishFrame(
            NULL); // restores the back buffer as the active target

        if (m_pMfdImage and m_pRTT)
        {
            unsigned short *dst = (unsigned short *)m_pMfdImage->Lock();

            if (dst)
            {
                int w = m_pMfdImage->targetXres();
                int h = m_pMfdImage->targetYres();
                m_pRTT->BlitRttTo565(dst, w, h, 0, 0, w, h);
                m_pMfdImage->Unlock();
            }
        }
    }
}

// Artscout - 2026: HUD FLIR raster, in two halves.
//
// RenderHudFlir draws the nav pod's IR view into a render target of its OWN (m_hudTex, square, HudFlirRes px) --
// not into the HUD's patch of the shared display atlas. That first attempt never showed (the IR scene drawn into
// the zone did not survive to the composite) and it upset the other displays sharing the atlas. vcock.cpp calls it
// once per frame (first eye only; the second eye reuses the texture), INSIDE the pit's atlas pass at the HUD. It swaps
// m_hudTex in as the RTT target so the proven sensor sequence (TGP / Maverick pages: StartDraw, rebind, confine,
// DrawScene, confine, flush 2D + objects inside the bracket, rebind, EndDraw) runs on it unchanged.
//
// DrawHudFlirQuad composites that texture on the HUD glass with the HUD's own canvas, so it gets the same
// collimation, combiner stencil and canopy occlusion as the symbology; it is drawn just before the symbology quad,
// additively (black = nothing), green, scaled by the BRT wheel and the gain/level.
//
// Conformal: the camera looks along the aircraft's x axis, pitched down by the HUD's own boresight offset (so the
// nose lands under the boresight cross), with the HUD's field of view. HudFlirTrim (degrees, up positive) and the
// FLIR page's BSGT trim the aim; HudFlirScale scales the field of view.
bool LantirnClass::HudFlirWanted(void)
{
    extern bool g_bUseD3D12, g_bUseVulkan, g_bSensorSceneD3D12, g_bSensorSceneVulkan;

    if (not g_bHudFlir or not IsFLIR() or hudBright < 0.02F or not SimDriver.GetPlayerAircraft())
        return false;

    // the same gate as every other sensor scene: off where the backend has no working sensor-scene path
    if ((g_bUseD3D12 or g_bUseVulkan) and not(g_bSensorSceneD3D12 or (g_bUseVulkan and g_bSensorSceneVulkan)))
        return false;

    return true;
}

// Before the atlas pass opens (vcock.cpp): make the IR renderer and the target. Setting a renderer up or creating a
// render target inside the pass blanked every display in the atlas (first flight).
void LantirnClass::PrepareHudFlir(Render3D *mainRenderer)
{
    extern int g_nHudFlirRes;

    if (not mainRenderer or not HudFlirWanted())
        return;

    ImageBuffer *image = mainRenderer->GetImageBuffer();

    if (not image)
        return;

    if (m_hudIr and m_hudIr->GetImageBuffer() not_eq image)
    {
        m_hudIr->Cleanup();
        delete m_hudIr;
        m_hudIr = NULL;
    }

    if (not m_hudIr)
    {
        m_hudIr = new RenderIR;
        m_hudIr->Setup(image, OTWDriver.GetViewpoint());
        m_hudIr->SetColor(0xffffffff);
    }

    const int size = g_nHudFlirRes < 128 ? 128 : (g_nHudFlirRes > 2048 ? 2048 : g_nHudFlirRes);

    if (m_hudTex and m_hudTexSize not_eq size)
    {
        delete m_hudTex;
        m_hudTex = NULL;
        m_hudTexValid = false;
    }

    if (not m_hudTex)
    {
        m_hudTex = VirtualDisplay::CreateExtraRtt(size, size);
        m_hudTexSize = size;
        m_hudTexValid = false;

        char buf[96];
        sprintf(buf, "HUD FLIR: own render target %dx%d %s\n", size, size, m_hudTex ? "created" : "FAILED");
        FFDebugLog(buf);
    }
}

// INSIDE the atlas pass, at the HUD (vcock.cpp), first eye only: draw the IR scene into m_hudTex as a sub-render,
// exactly as the MFD sensor pages draw theirs inside the pass, only with m_hudTex swapped in as the target. The
// caller re-binds the atlas afterwards (vHUDrenderer->ReBindRttTarget / AdjustRttViewport). A separate
// StartRtt/FinishRtt pass on the eye's list before the atlas pass broke that eye's displays (second flight).
void LantirnClass::RenderHudFlir(Render3D *mainRenderer, float halfAngleDeg, float boresightRad)
{
    extern bool g_bUseD3D12, g_bUseVulkan;
    AircraftClass *playerAC = SimDriver.GetPlayerAircraft();

    if (not mainRenderer or not playerAC or halfAngleDeg <= 0.0F or not m_hudIr or not m_hudTex or
        not HudFlirWanted() or m_hudIr->GetImageBuffer() not_eq mainRenderer->GetImageBuffer())
        return;

    const int size = m_hudTexSize;

    Tpoint cameraPos;
    GetCameraPos(&cameraPos);

    Trotation viewRotation;
    mlTrig tha, psi, phi;
    mlSinCos(&tha, playerAC->Pitch() - boresightRad + g_fHudFlirTrim * DTR + hudTrimPitch);
    mlSinCos(&psi, playerAC->Yaw() + hudTrimYaw);
    mlSinCos(&phi, playerAC->Roll());

    viewRotation.M11 = psi.cos * tha.cos;
    viewRotation.M21 = psi.sin * tha.cos;
    viewRotation.M31 = -tha.sin;

    viewRotation.M12 = -psi.sin * phi.cos + psi.cos * tha.sin * phi.sin;
    viewRotation.M22 = psi.cos * phi.cos + psi.sin * tha.sin * phi.sin;
    viewRotation.M32 = tha.cos * phi.sin;

    viewRotation.M13 = psi.sin * phi.sin + psi.cos * tha.sin * phi.cos;
    viewRotation.M23 = -psi.cos * phi.sin + psi.sin * tha.sin * phi.cos;
    viewRotation.M33 = tha.cos * phi.cos;

    Tpoint p = cameraPos;
    cameraPos.x = p.x * viewRotation.M11 + p.y * viewRotation.M12 + p.z * viewRotation.M13;
    cameraPos.y = p.x * viewRotation.M21 + p.y * viewRotation.M22 + p.z * viewRotation.M23;
    cameraPos.z = p.x * viewRotation.M31 + p.y * viewRotation.M32 + p.z * viewRotation.M33;

    extern void FF_SetIRGrey(bool);
    extern void FF_SetTerrainRadiusCap(int);

    // our target in place of the atlas for the sensor sequence (TGP page: StartDraw, rebind, confine, DrawScene,
    // confine, flush 2D + objects inside the bracket, rebind, EndDraw)
    TextureHandle *atlas = VirtualDisplay::SwapRttTexture(m_hudTex);

    m_hudIr->SetRttRect(0, 0, size, size);
    m_hudIr->SetViewport(-1.0F, 1.0F, 1.0F, -1.0F); // pixel bounds from the new rect, as StartRtt does
    m_hudIr->SetFOV(2.0F * halfAngleDeg * DTR * g_fHudFlirScale);

    m_hudIr->StartDraw();
    ((VirtualDisplay *)m_hudIr)->ReBindRttTarget(); // binds m_hudTex
    m_hudIr->SetBackground(0x00000000);
    m_hudIr->ClearDraw(); // clears the bound target (we are inside the atlas pass, so the clear is armed)
    ((VirtualDisplay *)m_hudIr)->ConfineObjectViewportToZone();
    FF_SetIRGrey(true);
    FF_SetTerrainRadiusCap(32);
    m_hudIr->DrawScene(&cameraPos, &viewRotation);
    ((VirtualDisplay *)m_hudIr)->ConfineObjectViewportToZone();
    m_hudIr->context.FlushPending();

    if (DisplayOptions.bZBuffering or g_bUseD3D12 or g_bUseVulkan)
        m_hudIr->context.FlushPolyLists();

    // as on the MFD pages: Vulkan queues the sensor's objects in the main renderer's lists
    if (g_bUseVulkan)
        mainRenderer->context.FlushPolyLists();

    FF_SetIRGrey(false);
    FF_SetTerrainRadiusCap(0);
    ((VirtualDisplay *)m_hudIr)->ReBindRttTarget();
    m_hudIr->EndDraw();

    // m_hudTex -> sampleable, then the atlas back in place (the caller re-binds it)
    VirtualDisplay::ReleaseExtraRtt(m_hudTex);
    VirtualDisplay::SwapRttTexture(atlas);

    if (not m_hudTexValid)
        FFDebugLog("HUD FLIR: first frame rendered into the own target (inside the atlas pass)\n");

    m_hudTexValid = true;
}

// HudFlirDebug 1: the raw FLIR target in a corner of the screen (top-left quarter of it, magnified) -- tells "the
// scene is not being drawn" from "it is drawn but the HUD composite loses it" in one screenshot.
void LantirnClass::DrawHudFlirDebug(VirtualDisplay *anyDisplay)
{
    extern bool g_bHudFlirDebug;

    if (not g_bHudFlirDebug or not anyDisplay or not m_hudTex or not m_hudTexValid)
        return;

    TextureHandle *atlas = VirtualDisplay::SwapRttTexture(m_hudTex);
    anyDisplay->DrawRttDebugOverlay();
    VirtualDisplay::SwapRttTexture(atlas);
}

void LantirnClass::DrawHudFlirQuad(VirtualDisplay *hudCanvas)
{
    extern float g_rttHudGain, g_rttHudTint[3];

    if (not hudCanvas or not m_hudTex or not m_hudTexValid or not HudFlirWanted())
        return;

    int zl, zt, zr, zb;
    hudCanvas->GetRttRect(&zl, &zt, &zr, &zb);

    const float savedGain = g_rttHudGain;
    float v = hudBright * FlirIntensity();
    v = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
    g_rttHudGain = v;
    g_rttHudTint[0] = 0.25F;
    g_rttHudTint[1] = 1.0F;
    g_rttHudTint[2] = 0.25F;

    TextureHandle *atlas = VirtualDisplay::SwapRttTexture(m_hudTex);
    hudCanvas->SetRttRect(0, 0, m_hudTexSize, m_hudTexSize);
    hudCanvas->DrawRttQuad();
    VirtualDisplay::SwapRttTexture(atlas);
    hudCanvas->SetRttRect(zl, zt, zr, zb);

    g_rttHudTint[0] = g_rttHudTint[1] = g_rttHudTint[2] = 1.0F;
    g_rttHudGain = savedGain;
}

void LantirnClass::SetFOV(float fov)
{
    if (privateDisplay)
        ((Render3D *)privateDisplay)->SetFOV(fov * m_fscale);
}

float LantirnClass::GetFov()
{
    if (privateDisplay)
        return ((Render3D *)privateDisplay)->GetFOV() * m_fscale;

    return 0;
}

bool LantirnClass::FlirPowered(AircraftClass* self)
{
    extern bool g_bRealisticAvionics;
    return not g_bRealisticAvionics or (self and self->HasPower(AircraftClass::LeftHptPower));
}

void LantirnClass::SetFlirOper(bool on)
{
    AircraftClass* ac = SimDriver.GetPlayerAircraft();

    if (on == IsFLIR())
        return;

    if (on)
    {
        if (not FlirPowered(ac) or not IsFlirTimedOut())
            return; // not powered, or NOT TIMED OUT

        m_flags or_eq FLIR_ON;
    }
    else
    {
        DisplayExit();
        display = NULL;
        m_flags and_eq compl FLIR_ON;
    }
}

void LantirnClass::ToggleFLIR()
{
    SetFlirOper(not IsFLIR());
}

// How bright the picture is, 0..1: the fixed auto values unless GRAY has the manual gain and level on. Gain 50 and
// level 100 (the defaults) give 1.0.
float LantirnClass::FlirIntensity(void)
{
    if (not flirManual)
        return 1.0F;

    float v = (0.5F + flirGain / 100.0F) * (flirLevel / 100.0F);
    return v < 0.05F ? 0.05F : (v > 1.0F ? 1.0F : v);
}

void LantirnClass::AdjustFlirGain(int steps)
{
    flirGain += 5.0F * steps;
    flirGain = flirGain < 0.0F ? 0.0F : (flirGain > 100.0F ? 100.0F : flirGain);
}

void LantirnClass::AdjustFlirLevel(int steps)
{
    flirLevel += 5.0F * steps;
    flirLevel = flirLevel < 0.0F ? 0.0F : (flirLevel > 100.0F ? 100.0F : flirLevel);
}

void LantirnClass::AdjustHudBright(int steps)
{
    hudBright += 0.1F * steps;
    hudBright = hudBright < 0.0F ? 0.0F : (hudBright > 1.0F ? 1.0F : hudBright);
}

void LantirnClass::BsgtSlew(float dPitchRad, float dYawRad)
{
    const float limit = 10.0F * DTR;
    hudTrimPitch += dPitchRad;
    hudTrimYaw += dYawRad;
    hudTrimPitch = hudTrimPitch < -limit ? -limit : (hudTrimPitch > limit ? limit : hudTrimPitch);
    hudTrimYaw = hudTrimYaw < -limit ? -limit : (hudTrimYaw > limit ? limit : hudTrimYaw);
}

void LantirnClass::StepTFRRide()
{
    switch (m_tfr_ride)
    {
    case TFR_SOFT:
        m_tfr_ride = TFR_MED;
        break;

    case TFR_MED:
        m_tfr_ride = TFR_HARD;
        break;

    case TFR_HARD:
        m_tfr_ride = TFR_SOFT;
        break;
    }
}

void LantirnClass::StepTFRMode()
{
    switch (m_tfrmode)
    {
    case TFR_NORM:
        m_tfrmode = TFR_LP1;
        break;

    case TFR_LP1:
        m_tfrmode = TFR_STBY;
        break;

    case TFR_STBY:
        m_tfrmode = TFR_WX;
        break;

    case TFR_WX:
        m_tfrmode = TFR_ECCM;
        break;

    case TFR_ECCM:
    default:
        m_tfrmode = TFR_NORM;
        break;
    }
}

void LantirnClass::GetCameraPos(Tpoint *pos)
{
    //MI externalized
#if 0
    pos->x = Origin.x;
    pos->y = Origin.y;
    pos->z = Origin.z;

    // JB 010325 This places the camera directly below the aircraft.  The
    // aircraft will itself be seen in the camera when pointed directly up.
    //The correct fix would be to project a point from the Origin based on
    //the aircraft's yaw, pitch, and size.
    pos->z += 10.0f;
#else
    AircraftClass *playerAC = SimDriver.GetPlayerAircraft();

    if (playerAC)
    {
        pos->x = playerAC->af->GetLantirnCameraX() *
                 OTWDriver.Scale(); //external variables in a/c .dat file
        pos->y = playerAC->af->GetLantirnCameraY() *
                 OTWDriver.Scale(); //that describe camera position for each
        pos->z = playerAC->af->GetLantirnCameraZ() *
                 OTWDriver.Scale(); //aircraft that has LANTIRN
    }
    else
    {
        pos->x = Origin.x * OTWDriver.Scale();
        pos->y = Origin.y * OTWDriver.Scale();
        pos->z = Origin.z * OTWDriver.Scale();
        pos->z += 10.0f;
    }

#endif
}

// Artscout - 2026: the nav pod needs about three minutes after its LEFT HDPT power comes on before the
// TFR can leave standby (BMS: "NOT TIMED OUT"; only ride and clearance can be set meanwhile). A jet that
// spawns with the pod already powered counts as warmed up, so hot starts and air starts are unchanged.
static const float kTfrWarmupSeconds = 180.0F;

void LantirnClass::UpdateCooldown(AircraftClass *self)
{
    extern bool g_bRealisticAvionics;

    if (not g_bRealisticAvionics or self not_eq SimDriver.GetPlayerAircraft())
    {
        coolTime = 0.0F;
        flirCool = 0.0F;
        return;
    }

    const bool powered = self->HasPower(AircraftClass::LeftHptPower) ? true : false;

    if (not powerInit)
    {
        powerInit = true;
        powerSeen = powered;
        coolTime = 0.0F;
        flirCool = 0.0F;
    }

    if (powered and not powerSeen)
    {
        coolTime = kTfrWarmupSeconds;
        flirCool = g_fFlirCooldown;
    }

    powerSeen = powered;

    // the FLIR cools down while the pod is powered, and drops out of OPER if the pod loses power or has not cooled
    if (powered and flirCool > 0.0F)
    {
        flirCool -= SimLibMajorFrameTime;

        if (flirCool < 0.0F)
            flirCool = 0.0F;
    }

    if (m_flags bitand FLIR_ON and (not powered or flirCool > 0.0F))
        m_flags and_eq compl FLIR_ON;

    if (powered and coolTime > 0.0F)
    {
        coolTime -= SimLibMajorFrameTime;

        if (coolTime < 0.0F)
            coolTime = 0.0F;

        m_tfrmode = TFR_STBY;

        if (self->AutopilotType() == AircraftClass::LantirnAP)
            self->SetAutopilot(AircraftClass::APOff);
    }
}

// HUD LIMIT and TERRAIN advisories. The bank/dive limits are an interpretation (the manual only says the
// antenna scan limits valid commands in roll, dive, speed and turn rate); TERRAIN looks 20 degrees to each
// side, every fourth frame, for ground inside 1.5 turn radii.
void LantirnClass::UpdateAdvisories(AircraftClass *self)
{
    if (self->AutopilotType() not_eq AircraftClass::LantirnAP)
    {
        limitExceeded = false;
        turnAdvisory = 0;
        return;
    }

    limitExceeded = ((float)fabs(self->Roll()) > 60.0F * DTR) or (self->Pitch() < -20.0F * DTR);

    if (++sideFrame >= 4)
    {
        sideFrame = 0;
        const float zOff = (gAlt > self->af->GetTFR_Clearance()) ? self->af->GetTFR_Clearance() : gAlt / 2.0F;
        const float spread = 20.0F * DTR;
        const float dl = GetGroundDistance(self, zOff, self->Yaw() - spread, self->af->gmma);
        const float dr = GetGroundDistance(self, zOff, self->Yaw() + spread, self->af->gmma);
        const float reach = 1.5F * turnradius;
        const bool left = dl > 0.0F and dl < reach;
        const bool right = dr > 0.0F and dr < reach;
        turnAdvisory = (left and right) ? 2 : (left ? 1 : (right ? -1 : 0));
    }
}

// JB 010325 Rewritten
void LantirnClass::Exec(AircraftClass *self)
{
    UpdateCooldown(self);

    if (g_bTFRFixes)
    {
        SpeedUp = FALSE;
        gamma = self->af->gmma;

        if (m_tfrmode == TFR_STBY)
        {
            evasize = 0; //MI
            return;
        }

        MoveBeam();

        //Flash SLOW warrning if we get too slow
        if (self->GetKias() <
            self->af->GetTFR_Corner() * self->af->GetSlowPercent())
            SpeedUp = TRUE;

        float groundAlt = -self->af->groundZ;

        float vt = self->GetVt();
        pitch = self->Pitch();

        turnradius = vt * vt / (GRAVITY * (GetGLimit() - 1));
        //Calculate absolute minimum radius...
        min_Radius = vt * vt / (GRAVITY * 0.8F * (self->af->MaxGs() - 1));
        min_safe_dist = 1.2F * min_Radius * (float)sin(-self->af->gmma);
        //min_safe_dist = min_Radius * (1.2F - (float)cos(self->af->gmma));
        //min_safe_dist *= min_safe_dist;
        //min_safe_dist = (float) sqrt(min_safe_dist - min_Radius * min_Radius);

        holdheight = m_tfr_alt + groundAlt;
        gdist = -1.0f;

        //float angle = self->af->gmma;
        //if(self->af->gmma > 0)
        // angle /= 2.0F;
        //make sure we pass at least TFR_Clearance ft above top of the hill
        gAlt = -self->ZPos() + self->af->groundZ;

        if (gAlt > self->af->GetTFR_Clearance())
            gdist = GetGroundDistance(self, self->af->GetTFR_Clearance(),
                                      self->Yaw(), self->af->gmma);
        else
            gdist = GetGroundDistance(self, gAlt / 2.0F, self->Yaw(),
                                      self->af->gmma);

        //get all the important distances to features.
        //Measure distance to feature we're about to collide with, consider all objectives within 5 miles,
        //Feature box size is increased by a safety margin (50%).
        //Check 3 times, first at our position to see which feature our vector is pointing at.
        //Then also at TFR_Clearance below us to be sure we pass over it, and also to calculate inclination needed to overfly it.
        //And finally at current holdheight as a backup fallback to make sure we don't miss a feature under us.
        featureDistance = FeatureCollisionPrediction(
            self, 0.0F, FALSE, FALSE, self->af->GetTFR_Clearance(), 5.0F, 1.5F,
            &featureHeight);
        featureDistance2 = FeatureCollisionPrediction(
            self, self->af->GetTFR_Clearance(), TRUE, TRUE,
            self->af->GetTFR_Clearance(), 5.0F, 1.5F, &featureHeight2);
        featureDistance3 = FeatureCollisionPrediction(
            self, gAlt - m_tfr_alt, TRUE, FALSE, self->af->GetTFR_Clearance(),
            5.0F, 1.5F, &featureHeight3);

        featureAngle = featureAngle2 = featureAngle3 = 0.0F;

        if (featureDistance > 0)
            featureAngle =
                RTD * (float)atan2(featureHeight +
                                       self->af->GetTFR_Clearance() - gAlt,
                                   featureDistance);

        if (featureDistance2 > 0)
            featureAngle2 =
                RTD * (float)atan2(featureHeight2 +
                                       self->af->GetTFR_Clearance() - gAlt,
                                   featureDistance2);

        if (featureDistance3 > 0)
            featureAngle3 =
                RTD * (float)atan2(featureHeight3 +
                                       self->af->GetTFR_Clearance() - gAlt,
                                   featureDistance3);

        //Set new holdheight value to be the greatest of feature heights
        float featureHeightOffset = 0;

        if (featureDistance <
            2.5 *
                min_Radius) // now would be a good time to start avoiding feature...
            featureHeightOffset = max(featureHeightOffset, featureHeight);

        if (featureDistance2 <
            2.5 *
                min_Radius) // now would be a good time to start avoiding feature...
            featureHeightOffset = max(featureHeightOffset, featureHeight2);

        if (featureDistance3 <
            2.5 *
                min_Radius) // now would be a good time to start avoiding feature...
            featureHeightOffset = max(featureHeightOffset, featureHeight3);

        holdheight += featureHeightOffset;

        //calculate ground inclination (gammaCorr)
        lookingAngle = RTD * (float)atan2(gAlt, self->af->GetTFR_lookAhead());
        gDist2 =
            GetGroundDistance(self, 0.0F, self->Yaw(), -lookingAngle * DTR);

        //compare feature distance and ground distance
        //If distance to feature is closer then ground distance use feature distance instead
        if (featureDistance > 0 and featureDistance < gdist and
            featureAngle > 0)
            if (featureHeight > gAlt)
                gdist = featureDistance;

        if (gDist2 > 0.0F)
            gammaCorr =
                RTD * (float)atan2(gAlt - gDist2 * sin(lookingAngle * DTR),
                                   gDist2 * cos(lookingAngle * DTR));
        else
            gammaCorr = 0;

        //adjust gamma correction according to feature distance
        //Adjust gammaCorr for the angle to avoid feature if we are below it.
        if ((featureDistance2 < 2.5 * min_Radius and featureDistance2 > 0 and
             featureAngle2 > 0) or
            featureAngle2 > 1.0F)
        {
            gammaCorr += featureAngle2 * 1.3F;
        }

        evasize = -1;

        if (gdist > 0)
        {
            evasize = 0; // assume we're ok...

            //use the new GetEVAFactor() function
            if (gdist <
                turnradius * GetEVAFactor(self, 1)) // coming up to something
                evasize = 1;

            if (gdist < turnradius * GetEVAFactor(self, 2)) // danger danger
                evasize = 2;

            if (gdist < min_Radius * 1.2) // danger danger danger
                evasize = 3;

            if (gdist < min_safe_dist) // danger danger danger
                evasize = 3;
        }
        else if (-self->ZPos() - groundAlt < 0.5 * m_tfr_alt)
            evasize = 1;

        UpdateAdvisories(self);
    }
    else
    {
        if (m_tfrmode == TFR_STBY)
            return;

        MoveBeam();

        float groundAlt = -self->af->groundZ;

        float vt = self->GetVt();
        pitch = self->Pitch();

        turnradius = vt * vt / (GRAVITY * (GetGLimit() - 1));

        holdheight = m_tfr_alt + groundAlt;
        gdist = -1.0f;

        int type = 0;
        gdist = GetGroundIntersection(
            self, self->Yaw(), pitch - self->GetAlpha() * DTR, groundAlt, type);

        evasize = 0;

        if (gdist >= 0)
        {
            float rangetopos = sqrt(gdist * gdist + m_tfr_alt * m_tfr_alt);

            if (gdist > 0 and rangetopos < turnradius and type) // danger danger
                evasize = 2;
            else if (rangetopos - m_tfr_alt * 2 > turnradius)
                evasize = 0;
            else if (rangetopos <
                     turnradius + m_tfr_alt) // coming up to something
                evasize = 1;
        }
        else if (-self->ZPos() - groundAlt < m_tfr_alt)
            evasize = 1;
    }
}

void LantirnClass::MoveBeam()
{
    if (scanpos > 1.0f)
        scandir = -1.0f;

    if (scanpos < -1.0f)
        scandir = 1.0f;

    scanpos += scandir * scanrate * SimLibMajorFrameTime;
}

float LantirnClass::GetGLimit()
{
    AircraftClass *playerAC = SimDriver.GetPlayerAircraft();

    if (g_bTFRFixes)
    {
        if (playerAC and playerAC->af)
        {
            switch (m_tfr_ride)
            {
            case TFR_SOFT:
                return playerAC->af->GetTFR_SoftG();

            default:
            case TFR_MED:
                return playerAC->af->GetTFR_MedG();

            case TFR_HARD:
                return playerAC->af->GetTFR_HardG();
            }
        }
        else
        {
            switch (m_tfr_ride)
            {
            case TFR_SOFT:
                return 2.0f;

            default:
            case TFR_MED:
                return 4.0f;

            case TFR_HARD:
                return 6.0f;
            }
        }
    }
    else
    {
        switch (m_tfr_ride)
        {
        case TFR_SOFT:
            return 2.0f;

        default:
        case TFR_MED:
            return 4.0f;

        case TFR_HARD:
            return 6.0f;
        }
    }
}

// JB 010325 Rewritten
float LantirnClass::GetGroundIntersection(AircraftClass *self, float yaw,
                                          float pitch, float galt, int &type)
{
    type = 1;
    float dx;
    float dy;
    float dz;
    float gdist1 = -1;
    float gdist2 = -1;

    Tpoint airframepos;
    airframepos.x = self->XPos();
    airframepos.y = self->YPos();
    airframepos.z = self->ZPos();

    RViewPoint *vpp;
    vpp = OTWDriver.GetViewpoint();
    vpp->Update(&airframepos);

    Tpoint tfrviewDir;
    Tpoint point;
    mlTrig trigYaw, trigPitch;

    mlSinCos(&trigYaw, yaw);
    mlSinCos(&trigPitch, pitch);

    tfrviewDir.x = trigYaw.cos * trigPitch.cos;
    tfrviewDir.y = trigYaw.sin * trigPitch.cos;
    tfrviewDir.z = -trigPitch.sin;

    if (vpp->GroundIntersection(&tfrviewDir, &point))
    {
        dx = self->XPos() - point.x;
        dy = self->YPos() - point.y;
        dz = self->ZPos() - point.z;
        gdist1 = (float)sqrt(dx * dx + dy * dy);
    }

    if (airframepos.z + m_tfr_alt > galt)
        return 0;

    if (pitch < 0)
        pitch = 0;
    else
        pitch /= 4;

    mlSinCos(&trigPitch, pitch);

    tfrviewDir.x = trigYaw.cos * trigPitch.cos;
    tfrviewDir.y = trigYaw.sin * trigPitch.cos;
    tfrviewDir.z = -trigPitch.sin;

    airframepos.z += m_tfr_alt;
    vpp->Update(&airframepos);
    int res = vpp->GroundIntersection(&tfrviewDir, &point);

    if (not res)
    {
        type = 0;
        return gdist1;
    }

    dx = self->XPos() - point.x;
    dy = self->YPos() - point.y;
    dz = self->ZPos() - point.z;
    gdist2 = (float)sqrt(dx * dx + dy * dy);

    if (gdist1 >= 0 and gdist1 < gdist2)
        return gdist1;

    return gdist2;
}

float LantirnClass::GetGroundDistance(AircraftClass *self, float zOffset,
                                      float yaw, float pitch)
{
    float dx;
    float dy;
    float dz;
    float gdist = -1;

    Tpoint airframepos;
    airframepos.x = self->XPos();
    airframepos.y = self->YPos();
    airframepos.z = self->ZPos() + zOffset;

    RViewPoint *vpp;
    vpp = OTWDriver.GetViewpoint();
    vpp->Update(&airframepos);

    Tpoint tfrviewDir;
    Tpoint point;
    mlTrig trigYaw, trigPitch;

    mlSinCos(&trigYaw, yaw);
    mlSinCos(&trigPitch, pitch);

    tfrviewDir.x = trigYaw.cos * trigPitch.cos;
    tfrviewDir.y = trigYaw.sin * trigPitch.cos;
    tfrviewDir.z = -trigPitch.sin;

    if (vpp->GroundIntersection(&tfrviewDir, &point))
    {
        dx = self->XPos() - point.x;
        dy = self->YPos() - point.y;
        dz = self->ZPos() - point.z;
        gdist = (float)sqrt(dx * dx + dy * dy + dz * dz);
    }

    return gdist;
}
float LantirnClass::FeatureCollisionPrediction(
    AircraftClass *self, float zOffset, BOOL MeasureHorizontally,
    BOOL GreatestAspect, float Clearance, float GridSizeNM, float boxScale,
    float *featureHeight)
{
    CampBaseClass *objective;
    //get all objectives within GridSizeNM miles
#ifdef VU_GRID_TREE_Y_MAJOR
    VuGridIterator gridIt(ObjProxList, self->YPos(), self->XPos(),
                          GridSizeNM * NM_TO_FT);
#else
    VuGridIterator gridIt(ObjProxList, self->XPos(), self->YPos(),
                          GridSizeNM * NM_TO_FT);
#endif
    SimBaseClass *foundFeature = NULL;
    SimBaseClass *testFeature;
    float radius;
    Tpoint pos, fpos, vec, p3, collide;
    BOOL firstFeature;
    float MaxDistance = 2 * GridSizeNM * NM_TO_FT;
    float ClosestDistance = 10 * GridSizeNM * NM_TO_FT;
    float move_vector;
    float Aspect = -90.0F;

    *featureHeight = 0.0F;
    // get the 1st objective and iterate through the list
    objective = (CampBaseClass *)gridIt.GetFirst();

    while (objective)
    {
        if (objective->GetComponents())
        {
            pos.x = self->XPos();
            pos.y = self->YPos();
            pos.z = self->ZPos() + zOffset;

            //check for MaxDistance in the direction of our vector
            vec.x = self->XDelta();
            vec.y = self->YDelta();
            vec.z = self->ZDelta();
            move_vector =
                (float)sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);
            vec.x *= MaxDistance / move_vector;
            vec.y *= MaxDistance / move_vector;
            vec.z *= MaxDistance / move_vector;

            if (MeasureHorizontally)
                vec.z = 0;

            p3.x = (float)fabs(vec.x);
            p3.y = (float)fabs(vec.y);
            p3.z = (float)fabs(vec.z);

            // loop thru each element in the objective
            VuListIterator featureWalker(objective->GetComponents());
            testFeature = (SimBaseClass *)featureWalker.GetFirst();
            firstFeature = TRUE;

            while (testFeature)
            {
                if (testFeature->drawPointer)
                {
                    // get feature's radius and position
                    radius = testFeature->drawPointer->Radius();

                    if (self->drawPointer)
                        radius += self->drawPointer->Radius();

                    radius += Clearance;
                    testFeature->drawPointer->GetPosition(&fpos);

                    // test with gross level bounds of object
                    if (fabs(pos.x - fpos.x) < radius + p3.x and
                        fabs(pos.y - fpos.y) < radius + p3.y and
                        fabs(pos.z - fpos.z) < radius + p3.z)
                    {
                        //Check for tall objects when doing horizontal check
                        float sizeX, sizeY, sizeZ;
                        sizeX = ((DrawableBSP *)(testFeature->drawPointer))
                                    ->instance.BoxFront();
                        sizeX -= ((DrawableBSP *)(testFeature->drawPointer))
                                     ->instance.BoxBack();
                        sizeX = (float)(fabs)(sizeX);
                        sizeY = ((DrawableBSP *)(testFeature->drawPointer))
                                    ->instance.BoxRight();
                        sizeY -= ((DrawableBSP *)(testFeature->drawPointer))
                                     ->instance.BoxLeft();
                        sizeY = (float)(fabs)(sizeY);
                        sizeZ = ((DrawableBSP *)(testFeature->drawPointer))
                                    ->instance.BoxTop();
                        sizeZ -= ((DrawableBSP *)(testFeature->drawPointer))
                                     ->instance.BoxBottom();
                        sizeZ = (float)(fabs)(sizeZ);
                        float groundRadius = max(sizeX, sizeY);
                        //only for horizontal checks
                        float NewBoxScale = boxScale;

                        if ((groundRadius < 2.2F * Clearance) and
                            MeasureHorizontally)
                            NewBoxScale = boxScale *
                                          (2.2F * Clearance + groundRadius) /
                                          groundRadius;

                        //Check to see if out flight vector line intersects feature's box * boxScale (safety margin)
                        if (testFeature->drawPointer->GetRayHit(
                                &pos, &vec, &collide, NewBoxScale))
                        {
                            collide.x = (float)fabs(collide.x - pos.x);
                            collide.y = (float)fabs(collide.y - pos.y);
                            collide.z = (float)fabs(collide.z - pos.z);
                            float Distance = (float)sqrt(collide.x * collide.x +
                                                         collide.y * collide.y +
                                                         collide.z * collide.z);

                            //Remember this one, either closest one, or tallest one from our point of view.
                            if (not GreatestAspect)
                            {
                                if (Distance < ClosestDistance)
                                {
                                    ClosestDistance = Distance;
                                    *featureHeight = sizeZ;
                                }
                            }
                            else
                            {
                                float newAspect =
                                    RTD *
                                    (float)atan2(sizeZ - (-self->ZPos() +
                                                          self->af->groundZ),
                                                 Distance);

                                if (newAspect > Aspect and
                                    Distance < 2 * MaxDistance)
                                {
                                    Aspect = newAspect;
                                    ClosestDistance = Distance;
                                    *featureHeight = sizeZ;
                                }
                            }
                        }
                    }
                }

                testFeature = (SimBaseClass *)featureWalker.GetNext();
                firstFeature = FALSE;
            }
        }

        objective = (CampBaseClass *)gridIt.GetNext();
    }

    if (ClosestDistance < 2 * MaxDistance)
        return ClosestDistance; //yes We found a feature within range...
    else
        return -1.0F; //we ain't found sh*t...
}
float LantirnClass::GetEVAFactor(AircraftClass *self, int eva = 1)
{
    if (eva == 1)
    {
        switch (m_tfr_ride)
        {
        case TFR_SOFT:
            return self->af->GetEVA1_SoftFactor();

        case TFR_MED:
            return self->af->GetEVA1_MedFactor();

        case TFR_HARD:
            return self->af->GetEVA1_HardFactor();
        }
    }
    else if (eva == 2)
    {
        switch (m_tfr_ride)
        {
        case TFR_SOFT:
            return self->af->GetEVA2_SoftFactor();

        case TFR_MED:
            return self->af->GetEVA2_MedFactor();

        case TFR_HARD:
            return self->af->GetEVA2_HardFactor();
        }
    }

    return 1.0F;
}
