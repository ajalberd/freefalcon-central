#include "stdhdr.h"
#include "mfd.h"
#include "hud.h"
#include "sms.h"
#include "simdrive.h"
#include "camp2sim.h"
#include "graphics/include/render2d.h"
#include "graphics/include/renderir.h"
#include "lantirn.h"
#include "otwdrive.h" //MI
#include "cpmanager.h" //MI
#include "icp.h" //MI
#include "aircrft.h" //MI
#include "fcc.h" //MI
#include "radardoppler.h" //MI
#include "falclib/include/dispopts.h"

//MI
void DrawBullseyeCircle(VirtualDisplay* display, float cursorX, float cursorY);

// Artscout - 2026: the FLIR page as the BMS manual draws it (TO 1F-16CMAM-34-1-1 2.6.2.1, figure on p.336).
//
//   top        OSB 1  mode (OPER / STBY / OFF)         OSB 2-3  HUD raster brightness - / +
//   left       OSB 20 OPER   OSB 18 STBY   OSB 16 GRAY (manual gain and level)
//   right      OSB 6-9 gain + / - and level + / -  (while BSGT is lit: slew UP / DN / LT / RT)   OSB 10 BSGT
//   top left   gain and level readout ("50G" "100L")
//   centre     NOT TIMED OUT while the pod is cooling down
//
// The ICP does the same without the page: BRT wheel = SimBrtWheelUp/Dn, FLIR arrows = SimFlirLevelUp/Down.
// The picture is the TFR page's sensor display (LantirnDrawable::DrawTerrain), grey.

void FlirMfdDrawable::Display(VirtualDisplay* newDisplay)
{
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();
    RadarDopplerClass* theRadar = (RadarDopplerClass*)FindSensor(playerAC, SensorClass::Radar);

    if (not theRadar or not theLantirn)
    {
        ShiWarning("Oh Oh shouldn't be here without a radar");
        return;
    }

    float cX, cY = 0;
    theRadar->GetCursorPosition(&cX, &cY);
    display = newDisplay;

    const bool powered = theLantirn->FlirPowered(playerAC);
    const bool oper = theLantirn->IsFLIR();
    const bool cooling = powered and not theLantirn->IsFlirTimedOut();

    // the picture: same gate as the TFR page, and only in OPER
    extern bool g_bUseD3D12, g_bUseVulkan, g_bSensorSceneD3D12, g_bSensorSceneVulkan;

    if (oper and
        (!(g_bUseD3D12 or g_bUseVulkan) || g_bSensorSceneD3D12 or (g_bUseVulkan and g_bSensorSceneVulkan)) and
        display->type == VirtualDisplay::DISPLAY_GENERAL)
    {
        const unsigned int v = (unsigned int)(255.0f * theLantirn->FlirIntensity());
        display->SetColor(0xff000000 | (v << 16) | (v << 8) | v);
        DrawTerrain();
    }

    display->SetColor(GetMfdColor(MFD_DEFAULT));

    char buf[32];

    // top row
    LabelButton(0, powered ? (oper ? "OPER" : "STBY") : "OFF");
    LabelButton(1, "HUD", "-");
    LabelButton(2, "HUD", "+");

    // left column
    LabelButton(19, "OPER", NULL, oper);
    LabelButton(17, "STBY", NULL, not oper);
    LabelButton(15, "GRAY", NULL, theLantirn->flirManual);

    // right column
    if (theLantirn->bsgt)
    {
        LabelButton(5, "UP");
        LabelButton(6, "DN");
        LabelButton(7, "LT");
        LabelButton(8, "RT");
    }
    else
    {
        LabelButton(5, "GAIN", "+");
        LabelButton(6, "GAIN", "-");
        LabelButton(7, "LVL", "+");
        LabelButton(8, "LVL", "-");
    }

    LabelButton(9, "BSGT", NULL, theLantirn->bsgt);

    // gain / level readout above OPER
    {
        float x, y;
        GetButtonPos(19, &x, &y);
        const float h = display->TextHeight();
        sprintf(buf, "%dG", (int)theLantirn->flirGain);
        display->TextLeft(x, y + 2.2f * h, buf);
        sprintf(buf, "%dL", (int)theLantirn->flirLevel);
        display->TextLeft(x, y + 1.1f * h, buf);
    }

    if (cooling)
    {
        display->TextCenter(0.0f, 0.0f, "NOT TIMED OUT");
        const int s = (int)theLantirn->flirCool;
        sprintf(buf, "%d:%02d", s / 60, s % 60);
        display->TextCenter(0.0f, -display->TextHeight() * 1.2f, buf);
    }
    else if (not powered)
        display->TextCenter(0.0f, 0.0f, "FLIR OFF");

    if (g_bRealisticAvionics)
    {
        if (OTWDriver.pCockpitManager and OTWDriver.pCockpitManager->mpIcp and
            OTWDriver.pCockpitManager->mpIcp->ShowBullseyeInfo)
        {
            DrawBullseyeCircle(display, cX, cY);
        }
        else
            DrawReference(MfdDisplay[OnMFD()]->GetOwnShip());
    }
    else
        DrawReference(MfdDisplay[OnMFD()]->GetOwnShip());

    BottomRow();
}

void FlirMfdDrawable::PushButton(int whichButton, int whichMFD)
{
    if (not theLantirn)
    {
        MfdDrawable::PushButton(whichButton, whichMFD);
        return;
    }

    const float step = 0.25f * DTR; // BSGT slew per press

    switch (whichButton)
    {
    case 19: // OPER (needs the pod powered and cooled down)
        theLantirn->SetFlirOper(true);
        break;

    case 17: // STBY
        theLantirn->SetFlirOper(false);
        break;

    case 15: // GRAY: manual gain and level
        theLantirn->flirManual = not theLantirn->flirManual;
        break;

    case 9: // BSGT
        theLantirn->bsgt = not theLantirn->bsgt;
        break;

    case 1: // HUD brightness
        theLantirn->AdjustHudBright(-1);
        break;

    case 2:
        theLantirn->AdjustHudBright(1);
        break;

    case 5:
        if (theLantirn->bsgt)
            theLantirn->BsgtSlew(step, 0.0f);
        else
            theLantirn->AdjustFlirGain(1);

        break;

    case 6:
        if (theLantirn->bsgt)
            theLantirn->BsgtSlew(-step, 0.0f);
        else
            theLantirn->AdjustFlirGain(-1);

        break;

    case 7:
        if (theLantirn->bsgt)
            theLantirn->BsgtSlew(0.0f, -step);
        else
            theLantirn->AdjustFlirLevel(1);

        break;

    case 8:
        if (theLantirn->bsgt)
            theLantirn->BsgtSlew(0.0f, step);
        else
            theLantirn->AdjustFlirLevel(-1);

        break;

    default:
        MfdDrawable::PushButton(whichButton, whichMFD);
        break;
    }
}
