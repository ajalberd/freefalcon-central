#include "stdhdr.h"
#include "mfd.h"
#include "hud.h"
#include "sms.h"
#include "smsdraw.h"
#include "airframe.h"
#include "aircrft.h"
#include "fcc.h"
#include "otwdrive.h"
#include "playerop.h"
#include "graphics/include/render2d.h"
#include "graphics/include/canvas3d.h"
#include "graphics/include/tviewpnt.h"
#include "graphics/include/renderir.h"
#include "dispcfg.h"
#include "simdrive.h"
#include "camp2sim.h"
#include "digi.h"
#include "lantirn.h"
#include "fack.h"
#include "caution.h"
#include "falclib/include/dispopts.h"

static int flash = FALSE;
static int lantdebug = FALSE;

extern bool g_bLantDebug;

void LantirnDrawable::DisplayInit(ImageBuffer* image)
{
    DisplayExit();

    privateDisplay = new RenderIR;
    ((RenderIR*)privateDisplay)->Setup(image, OTWDriver.GetViewpoint());

    privateDisplay->SetColor(0xffffffff);
    ((Render3D*)privateDisplay)->SetFOV(28.0f * DTR);
}

// Artscout - 2026: the TFR page as TO 1F-16CMAM-34-1-1 2.6.3.1 draws it (figures pp. 338-340, 342):
//
//   top      OSB 1 mode (no function)   OSB 2 ride HARD / SOFT / SMTH   OSB 4 power (OFF lit = off)   OSB 5 CHN1 (N/I)
//   right    OSB 6-10 set clearance 1000 / 500 / 300 / 200 / VLC
//   left     OSB 20 NORM   OSB 19 LPI   OSB 18 STBY (also powers on)   OSB 17 WX
//   centre   the E-squared confidence display: flight path reference line, zero command line, obstacle warning
//            line, terrain video over a pseudo-log range scale (1-10 nm); NOT TIMED OUT while warming up
//   no NVP power (LEFT HDPT): "TFR OFF" and the bottom row only
//
// It used to draw an IR picture of the world behind all this (per eye: two extra world renders a frame), OSB 1
// stepped the mode, OSB 4 toggled the TF autopilot and OSB 16 offered ECCM; none of that is in the manual.
static const char* TfrModeText(LantirnClass::TfrMode m, int scp)
{
    switch (m)
    {
    case LantirnClass::TFR_NORM:
        return scp == 100 ? "VLC" : "NORM"; // VLC is NORM with the VLC clearance (2.6.3.2.4)

    case LantirnClass::TFR_LP1:
        return "LPI";

    case LantirnClass::TFR_WX:
        return "WX";

    case LantirnClass::TFR_ECCM:
        return "ECCM";

    case LantirnClass::TFR_STBY:
    default:
        return "STBY";
    }
}

void LantirnDrawable::Display(VirtualDisplay* newDisplay)
{
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();

    display = newDisplay;
    ShiAssert(theLantirn);
    flash = vuxRealTime bitand 0x200;

    display->SetColor(GetMfdColor(MFD_DEFAULT));

    // no power to the nav pod: TFR OFF (2.6.3.1, left figure p. 339)
    if (not playerAC or not playerAC->HasPower(AircraftClass::LeftHptPower))
    {
        display->TextCenter(0.0f, 0.1f, "TFR");
        display->TextCenter(0.0f, -0.02f, "OFF");
        BottomRow();
        return;
    }

    const bool power = theLantirn->tfrPower;
    const LantirnClass::TfrMode mode = theLantirn->GetTFRMode();
    const int scp = theLantirn->GetTFRAlt();

    DrawWarnings();
    DrawFlightInfo();

    if (power)
        DrawAzimuthScan();

    DrawRangeScale();
    DrawRadarArrows();

    LabelButton(0, power ? TfrModeText(mode, scp) : "OFF");

    switch (theLantirn->GetTFRRide())
    {
    case LantirnClass::TFR_HARD:
        LabelButton(1, "HARD");
        break;

    case LantirnClass::TFR_MED:
        LabelButton(1, "SMTH");
        break;

    case LantirnClass::TFR_SOFT:
        LabelButton(1, "SOFT");
        break;
    }

    LabelButton(3, "OFF", NULL, not power);
    LabelButton(4, "CHN1");

    LabelButton(5, "1000", NULL, scp == 1000);
    LabelButton(6, "500", NULL, scp == 500);
    LabelButton(7, "300", NULL, scp == 300);
    LabelButton(8, "200", NULL, scp == 200);
    LabelButton(9, "VLC", NULL, scp == 100);

    BottomRow();

    LabelButton(16, "WX", NULL, power and mode == LantirnClass::TFR_WX);
    LabelButton(17, "STBY", NULL, power and mode == LantirnClass::TFR_STBY);
    LabelButton(18, "LPI", NULL, power and mode == LantirnClass::TFR_LP1);
    LabelButton(19, "NORM", NULL, power and mode == LantirnClass::TFR_NORM);

    // JB 010325
    //MI reenabled with config var
    if (lantdebug and g_bLantDebug)
    {
        char buf[200];
        float pos = 9.2f;
        float divs = 11;
        sprintf(buf, "HoldAlt %.0f ft", theLantirn->holdheight);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "GndAlt %.0f ft", theLantirn->gAlt);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "GndDist %.0f ft", theLantirn->gdist);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "Eva %d", theLantirn->evasize);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "TurnR %.0f ft", theLantirn->turnradius);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "minR %.0f ft", theLantirn->min_Radius);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "safeD %.0f ft", theLantirn->min_safe_dist);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "GndDist2 %.0f ft /%.1f", theLantirn->gDist2,
                theLantirn->lookingAngle);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "PIDMX %.3f", theLantirn->PID_MX);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "PIDer %.3f", theLantirn->PID_error);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "PIDOut %.3f", theLantirn->PID_Output);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "Min/Max G %.1f/%.1f", theLantirn->MinG, theLantirn->MaxG);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "Roll %.5f", theLantirn->roll * RTD);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "gammaCorr %.2f", theLantirn->gammaCorr);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "gamma %.2f", theLantirn->gamma * RTD);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "f1Ht/Dist %.0f / %.0f", theLantirn->featureHeight,
                theLantirn->featureDistance);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "f2Ht/Dist %.0f / %.0f", theLantirn->featureHeight2,
                theLantirn->featureDistance2);
        display->TextCenter(0, (pos--) / divs, buf);
        sprintf(buf, "f3Ht/Dist %.0f / %.0f", theLantirn->featureHeight3,
                theLantirn->featureDistance3);
        display->TextCenter(0, (pos--) / divs, buf);
        // sprintf (buf, "MinDist %.0f", theLantirn->minavoiddist);
        // display->TextCenter(0, -0.25, buf);
        // sprintf (buf, "ADist %.0f", theLantirn->avoiddist);
        // display->TextCenter(0, -0.375, buf);
    }
    else if (power and mode not_eq LantirnClass::TFR_STBY and theLantirn->IsTimedOut())
    {
        DrawConfidenceDisplay(display, playerAC);
    }
}

// Range bins of the E-squared display: the pseudo-log scale of the figures (ticks 1, 2, 4, 6, 10 nm, measured off
// the STBY figure on p. 340), linear between them.
static float TfrRangeX(float nm)
{
    static const float r[] = {0.0f, 1.0f, 2.0f, 4.0f, 6.0f, 10.0f};
    static const float x[] = {-0.80f, -0.32f, 0.06f, 0.45f, 0.66f, 0.88f};

    if (nm <= 0.0f)
        return x[0];

    for (int i = 1; i < 6; i++)
        if (nm <= r[i])
            return x[i - 1] + (x[i] - x[i - 1]) * (nm - r[i - 1]) / (r[i] - r[i - 1]);

    return x[5];
}

// 2.6.3.3 TFR confidence display (E-squared): terrain along the track against the zero command line (ZCL, terrain
// the TF clears at the set clearance without commanding anything now) and the obstacle warning line (OWL, terrain
// that needs more than 3 g: obstacle fly-up), both relative to the horizontal flight path reference line. Elevation
// is to scale with the set clearance: the SCP sits 0.85 below the reference line. Blanked past 85 deg of bank, 10 deg
// of drift, or a pitch / flight path above 40 deg nose up or below 20 deg nose down.
static const float kE2Ref = 0.30f; // flight path reference line
static const float kE2Base = -0.72f; // range scale (the video's floor)
static const float kE2Top = 0.85f;

void LantirnDrawable::DrawConfidenceDisplay(VirtualDisplay* d, AircraftClass* ac)
{
    if (not ac)
        return;

    d->Line(TfrRangeX(0.0f), kE2Ref, 0.78f, kE2Ref); // flight path reference line

    const float vx = ac->XDelta(), vy = ac->YDelta(), vUp = -ac->ZDelta();
    const float vh = (float)sqrt(vx * vx + vy * vy);

    if (vh < 50.0f)
        return;

    // blanking (2.6.3.3)
    const float track = (float)atan2(vy, vx);
    float drift = track - ac->Yaw();

    while (drift > PI)
        drift -= 2.0f * PI;

    while (drift < -PI)
        drift += 2.0f * PI;

    const float gam = (float)atan2(vUp, vh);

    if ((float)fabs(ac->Roll()) > 85.0f * DTR or (float)fabs(drift) > 10.0f * DTR or ac->Pitch() > 40.0f * DTR or
        ac->Pitch() < -20.0f * DTR or gam > 40.0f * DTR or gam < -20.0f * DTR)
        return;

    const float scp = (float)theLantirn->GetTFRAlt();
    const float vs = 0.85f / (scp > 300.0f ? scp : 300.0f); // display units per foot
    const float alt = -ac->ZPos();
    const float v2 = vh * vh + vUp * vUp;

    // the ride's pull template for the ZCL: HARD lets the terrain come closer before it commands (2.6.3.2.5)
    float nZcl = 0.75f;

    if (theLantirn->GetTFRRide() == LantirnClass::TFR_HARD)
        nZcl = 1.0f;
    else if (theLantirn->GetTFRRide() == LantirnClass::TFR_SOFT)
        nZcl = 0.5f;

    const float rZcl = v2 / (GRAVITY * nZcl), rOwl = v2 / (GRAVITY * 3.0f);
    const float floorFt = scp * 0.5f > 50.0f ? scp * 0.5f : 50.0f;
    const float tanG = (float)tan(gam);
    const float ux = vx / vh, uy = vy / vh;

    float px = 0.0f, pyZ = 0.0f, pyO = 0.0f;
    bool haveZ = false, haveO = false;

    for (int i = 1; i <= 60; i++)
    {
        const float nm = 10.0f * (float)i / 60.0f;
        const float ft = nm * NM_TO_FT;
        const float x = TfrRangeX(nm);

        // terrain video: raw (a line up from the floor) and stored (a dash on top)
        const float rel = -OTWDriver.GetGroundLevel(ac->XPos() + ux * ft, ac->YPos() + uy * ft) - alt;
        float yT = kE2Ref + rel * vs;

        if (yT > kE2Base)
        {
            yT = yT > kE2Top ? kE2Top : yT;
            d->Line(x, kE2Base, x, yT);
            d->Line(x - 0.012f, yT + 0.02f, x + 0.012f, yT + 0.02f);
        }

        // ZCL / OWL
        const float yZ = kE2Ref + (-scp + ft * tanG + ft * ft / (2.0f * rZcl)) * vs;
        const float yO = kE2Ref + (-floorFt + ft * tanG + ft * ft / (2.0f * rOwl)) * vs;

        if (haveZ and yZ <= kE2Top and pyZ >= kE2Base)
            d->Line(px, pyZ, x, yZ > kE2Top ? kE2Top : yZ);

        if (haveO and yO <= kE2Top and pyO >= kE2Base)
            d->Line(px, pyO, x, yO > kE2Top ? kE2Top : yO);

        haveZ = yZ <= kE2Top;
        haveO = yO <= kE2Top;
        px = x;
        pyZ = yZ;
        pyO = yO;
    }
}

void LantirnDrawable::PushButton(int whichButton, int whichMFD)
{
    ShiAssert(theLantirn);
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();

    // with no NVP power the page is TFR OFF: only the bottom row works
    if (whichButton < 10 or whichButton > 14)
        if (not playerAC or not playerAC->HasPower(AircraftClass::LeftHptPower))
            return;

    switch (whichButton)
    {
    case 0: // OSB 1: mode status only
        break;

    case 1: // OSB 2: ride rotary
        theLantirn->StepTFRRide();
        break;

    case 2: // (FF6's TFR debug readout, shown only with the LantDebug cfg)
        lantdebug = not lantdebug;
        break;

    case 3: // OSB 4: TFR power
        theLantirn->SetTFRPower(not theLantirn->tfrPower);
        break;

    case 4:
        break;

    case 5:
        theLantirn->SetTFRAlt(1000);
        break;

    case 6:
        theLantirn->SetTFRAlt(500);
        break;

    case 7:
        theLantirn->SetTFRAlt(300);
        break;

    case 8:
        theLantirn->SetTFRAlt(200);
        break;

    case 9:
        theLantirn->SetTFRAlt(100);
        break;

    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
        MfdDrawable::PushButton(whichButton, whichMFD);
        return;

    // OSB 17-20: modes, filtered by power, the warm-up (only ride, clearance and power meanwhile) and the RF switch
    case 16:
        theLantirn->RequestTFRMode(LantirnClass::TFR_WX);
        break;

    case 17:
        theLantirn->RequestTFRMode(LantirnClass::TFR_STBY);
        break;

    case 18:
        theLantirn->RequestTFRMode(LantirnClass::TFR_LP1);
        break;

    case 19:
        theLantirn->RequestTFRMode(LantirnClass::TFR_NORM);
        break;
    }
}

void LantirnDrawable::DrawRadarArrows()
{
#if 0
    static const float arrowH = 0.0375f;
    static const float arrowW = 0.0433f;

    display->SetColor(GetMfdColor(MFD_LABELS));
    display->TextCenterVertical(RANGE_POSITION, -0.15f, "F1");
    /*----------*/
    /* up arrow */
    /*----------*/
    display->AdjustOriginInViewport(RANGE_POSITION, 0.0F);
    display->Tri(0.0F, arrowH, arrowW, -arrowH, -arrowW, -arrowH);
    /*------------*/
    /* down arrow */
    /*------------*/
    display->AdjustOriginInViewport(0.0f, -0.3F);
    display->Tri(0.0F, -arrowH, arrowW, arrowH, -arrowW, arrowH);
    display->CenterOriginInViewport();
#endif
}

void LantirnDrawable::DrawAzimuthScan()
{
    static const float azminx = 0.58f, azmaxx = 0.90f;
    static const float azstep = (azmaxx - azminx) / 4.0f;
    static const float azy = 0.75f;
    display->Line(azminx, azy, azmaxx, azy);

    for (int i = 0; i < 5; i++)
    {
        display->Line(azminx + i * azstep, azy, azminx + i * azstep,
                      azy + 0.05f);
    }

    float scanline = azminx + (azmaxx - azminx) / 2.0f +
                     theLantirn->GetScanLoc() * (azmaxx - azminx) / 2.0f;
    display->Line(scanline, azy + 0.06f, scanline, azy + 0.08f);
}

// the pseudo-logarithmic range scale under the E-squared display, ticks and labels at 1, 2, 4, 6 and 10 nm (p. 340)
void LantirnDrawable::DrawRangeScale()
{
    static const float nm[] = {1.0f, 2.0f, 4.0f, 6.0f, 10.0f};
    static const char* lbl[] = {"1", "2", "4", "6", "10"};

    display->Line(TfrRangeX(0.0f), kE2Base, TfrRangeX(10.0f), kE2Base);
    display->Line(TfrRangeX(0.0f), kE2Base, TfrRangeX(0.0f), kE2Base + 0.04f);

    for (int i = 0; i < 5; i++)
    {
        const float x = TfrRangeX(nm[i]);
        display->Line(x, kE2Base, x, kE2Base + 0.04f);
        display->TextCenter(x, kE2Base + 0.12f, lbl[i]);
    }
}
void LantirnDrawable::DrawTerrain()
{
    Tpoint cameraPos;
    theLantirn->GetCameraPos(&cameraPos);

    Trotation viewRotation;
    float costha, sintha, cospsi, sinpsi, sinphi, cosphi = 0.0;
    costha = (float)cos(MfdDisplay[OnMFD()]->GetOwnShip()->Pitch());
    sintha = (float)sin(MfdDisplay[OnMFD()]->GetOwnShip()->Pitch());
    cospsi = (float)cos(MfdDisplay[OnMFD()]->GetOwnShip()->Yaw());
    sinpsi = (float)sin(MfdDisplay[OnMFD()]->GetOwnShip()->Yaw());
    cosphi = (float)cos(MfdDisplay[OnMFD()]->GetOwnShip()->Roll());
    sinphi = (float)sin(MfdDisplay[OnMFD()]->GetOwnShip()->Roll());

    viewRotation.M11 = cospsi * costha;
    viewRotation.M21 = sinpsi * costha;
    viewRotation.M31 = -sintha;

    viewRotation.M12 = -sinpsi * cosphi + cospsi * sintha * sinphi;
    viewRotation.M22 = cospsi * cosphi + sinpsi * sintha * sinphi;
    viewRotation.M32 = costha * sinphi;

    viewRotation.M13 = sinpsi * sinphi + cospsi * sintha * cosphi;
    viewRotation.M23 = -cospsi * sinphi + sinpsi * sintha * cosphi;
    viewRotation.M33 = costha * cosphi;

    Tpoint p;
    Trotation* r = &viewRotation;
    p.x = cameraPos.x;
    p.y = cameraPos.y;
    p.z = cameraPos.z;
    cameraPos.x = p.x * r->M11 + p.y * r->M12 + p.z * r->M13;
    cameraPos.y = p.x * r->M21 + p.y * r->M22 + p.z * r->M23;
    cameraPos.z = p.x * r->M31 + p.y * r->M32 + p.z * r->M33;

    //JAM 24Nov03
    //  display->FinishFrame();
    //((RenderIR*)display)->DrawScene(&cameraPos, &OTWDriver.cameraRot);

    // #DX12 A5: the LANTIRN FLIR 3D-scene block runs only under D3D11 or when the D3D12 sensor scene is enabled
    // (g_bSensorSceneD3D12). Under D3D12 with it OFF (default) the open MFD page stays symbology-only -> stable
    // (the unfinished GPU-terrain sensor path -> DEVICE_HUNG). See laserpod.cpp / #91.
    extern void FF_SetIRGrey(bool);
    extern void FF_SetTerrainRadiusCap(int);
    extern bool g_bUseD3D12, g_bSensorSceneD3D12;
    // Artscout - 2026: Vulkan gated too -- its object path cannot target the RTT atlas (see lantirn.cpp doA5 note).
    extern bool g_bUseVulkan, g_bSensorSceneVulkan;
    const bool doA5 = !(g_bUseD3D12 or g_bUseVulkan) || g_bSensorSceneD3D12 ||
        (g_bUseVulkan and g_bSensorSceneVulkan);
    if (doA5)
    {
        // Artscout - 2026: full RTT bracket, parity with laserpod.cpp/mavdisp.cpp. This block had NO bracket at
        // all -- no atlas re-bind, no zone confine, no pending-2D flush, no main-context flush -- so under a GPU
        // backend its objects flushed later against the full atlas (or the eye).
        ((VirtualDisplay*)display)
            ->ReBindRttTarget(); // rebind the shared RTT atlas
        // Confine BEFORE DrawScene on BOTH backends -- the sensor sky flushes from inside DrawScene and needs
        // the zone scissor armed, else its above-horizon fill covers the whole atlas (see laserpod.cpp).
        ((VirtualDisplay*)display)->ConfineObjectViewportToZone();
        FF_SetIRGrey(true);
        FF_SetTerrainRadiusCap(96); // wide FOV: the main view's ring, not the narrow TGP's 32 (see lantirn.cpp)
        extern bool g_bThermalScene; // thermal: the ground at full level, not the night's (terraingpu.cpp)
        g_bThermalScene = true;
        ((RenderIR*)display)->DrawScene(&cameraPos, &viewRotation);
        g_bThermalScene = false;
        // Confine BEFORE the 2D flush: arms the renderer's per-draw zone SCISSOR for the span flush
        // (strays clipped, not splatted over HUD/DED) and the zone viewport for the object flush.
        ((VirtualDisplay*)display)->ConfineObjectViewportToZone();
        // Flush the batched 2D spans INSIDE the RTT bracket -- a leftover VB tail otherwise flushes after
        // FinishRtt and paints the sensor scene over the EYE.
        ((RenderIR*)display)->context.FlushPending();
        if (DisplayOptions.bZBuffering or g_bUseD3D12 or g_bUseVulkan)
            ((RenderIR*)display)->context.FlushPolyLists();
        // The sensor's objects queue into the MAIN renderer's poly lists (DrawableBSP draws through the object's
        // renderer, not this display's) -- flush them inside the bracket so they land in the MFD zone.
        if (g_bUseVulkan and OTWDriver.renderer)
            OTWDriver.renderer->context.FlushPolyLists();
        FF_SetIRGrey(false);
        FF_SetTerrainRadiusCap(0);
        // Artscout - 2026: re-bind the atlas after the scene, as the TGP page does. Without it the scene's screen
        // metric stayed in force and the page's symbology (and the picture) came out shrunk into a corner of the
        // MFD -- seen on the FLIR page, 2026-10-11.
        ((VirtualDisplay*)display)->ReBindRttTarget();
    }

    // ((RenderIR*)display)->PostSceneCloudOcclusion();
    //  ((RenderIR*)display)->FinishFrame();
    display->StartDraw();
}

void LantirnDrawable::DrawWarnings()
{
    // Artscout - 2026: the TFR is still warming up (about three minutes after power-on), across the top (p. 340).
    // TFR LIMITS, the break X and CHECK ATTITUDE are on BOTH MFDs whatever page they show: DrawTfrMfdAdvisories.
    if (theLantirn->tfrPower and not theLantirn->IsTimedOut())
        display->TextCenter(0.0f, 0.32f, "NOT TIMED OUT");
}

// Artscout - 2026: the TFR's MFD advisories (TO 1F-16CMAM-34-1-1 2.6.3.6.4 / 2.6.3.6.5), drawn by MFDClass after
// every page on both MFDs:
//   flashing TFR LIMITS - TF fail, limits exceeded (bank / dive / below 360 KCAS), exceeded too long, vertical
//                         clearance warning, low speed warning
//   flashing break X    - g-limit / obstacle warning
//   flashing CHECK ATTITUDE box - the attitude advisory function (below the ALOW page's TF ADV altitude)
void DrawTfrMfdAdvisories(VirtualDisplay* d)
{
    AircraftClass* ac = SimDriver.GetPlayerAircraft();
    extern bool g_bTFRFixes;

    if (not d or not ac or not theLantirn or not theLantirn->IsEnabled() or not g_bTFRFixes or
        not ac->HasPower(AircraftClass::LeftHptPower))
        return;

    if (not(vuxRealTime bitand 0x200)) // flash
        return;

    const bool tfFail = ac->mFaults and ac->mFaults->GetFault(tf_fail);
    const bool vertClear = theLantirn->tfOperating and theLantirn->gAlt > 0.0f and
                           theLantirn->gAlt < 0.75f * (float)theLantirn->GetTFRAlt();
    const bool limits = tfFail or (theLantirn->tfOperating and (theLantirn->limitExceeded or theLantirn->limitTooLong or
                                                                 vertClear or theLantirn->lowSpeedWarn));
    const DWORD saved = d->Color();

    d->SetColor(0xff00ff00);

    if (limits)
    {
        d->TextCenter(0.0f, 0.62f, "TFR");
        d->TextCenter(0.0f, 0.54f, "LIMITS");
    }

    if (theLantirn->pullupWarn)
    {
        d->Line(-0.6f, -0.6f, 0.6f, 0.6f);
        d->Line(-0.6f, 0.6f, 0.6f, -0.6f);
    }

    if (theLantirn->CheckAttitude(ac))
    {
        d->Line(-0.42f, 0.12f, 0.42f, 0.12f);
        d->Line(0.42f, 0.12f, 0.42f, -0.14f);
        d->Line(0.42f, -0.14f, -0.42f, -0.14f);
        d->Line(-0.42f, -0.14f, -0.42f, 0.12f);
        d->TextCenter(0.0f, 0.08f, "CHECK ATTITUDE");
        d->TextCenter(0.0f, -0.03f, "CHECK ATTITUDE");
    }

    d->SetColor(saved);
}

void LantirnDrawable::DrawFlightInfo()
{
}
