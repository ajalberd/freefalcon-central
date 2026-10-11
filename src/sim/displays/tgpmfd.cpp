#include "stdhdr.h"
#include "mfd.h"
#include "hud.h"
#include "sms.h"
#include "simdrive.h"
#include "camp2sim.h"
#include "graphics/include/render2d.h"
#include "graphics/include/canvas3d.h"
#include "graphics/include/tviewpnt.h"
#include "graphics/include/renderir.h"
#include "otwdrive.h"
#include "cpmanager.h"
#include "icp.h"
#include "aircrft.h"
#include "fcc.h"
#include "radardoppler.h"
#include "laserpod.h"
#include "airframe.h"
#include "navsystem.h"
#include "phyconst.h"

void DrawBullseyeCircle(VirtualDisplay* display, float cursorX, float cursorY);
SensorClass* FindLaserPod(SimMoverClass* theObject);

extern bool g_bRealisticAvionics;
extern bool g_bGreyMFD;
extern bool g_bGreyScaleMFD;
extern bool bNVGmode;

int TgpMfdDrawable::flash = FALSE;

void TgpMfdDrawable::DisplayInit(ImageBuffer* image)
{
    DisplayExit();

    if (not g_bGreyScaleMFD)
        g_bGreyMFD = false;

    privateDisplay = new RenderIR;
    ((RenderIR*)privateDisplay)->Setup(image, OTWDriver.GetViewpoint());

    if ((g_bGreyMFD) and (not bNVGmode))
        privateDisplay->SetColor(GetMfdColor(MFD_WHITE));
    else
        privateDisplay->SetColor(0xffffffff);

    ((Render3D*)privateDisplay)->SetFOV(6.0f * DTR);
}
TgpMfdDrawable::TgpMfdDrawable()
{
    StbyMode = TRUE;
    MenuMode = FALSE;
    SP = FALSE;
    CntlMode = FALSE;
    NorthPtr = TRUE;
    IrPointer = FALSE;
    LstMode = FALSE;
}

void TgpMfdDrawable::Display(VirtualDisplay* newDisplay)
{
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();
    float cX, cY = 0;
    sprintf(Str, "");

    if (not g_bRealisticAvionics)
    {
        display = privateDisplay;
        OffMode(display);
        return;
    }

    self = ((AircraftClass*)playerAC);
    theRadar = (RadarDopplerClass*)FindSensor(playerAC, SensorClass::Radar);
    laserPod = (LaserPodClass*)FindLaserPod(playerAC->Sms->Ownship());
    pFCC = playerAC->Sms->Ownship()->GetFCC();
    Sms = playerAC->Sms;
    display = newDisplay;

    if (not theRadar or not pFCC or not self or not Sms)
    {
        ShiWarning(
            "Oh Oh shouldn't be here without a radar or FCC or player or SMS");
        return;
    }
    else if (not laserPod)
    {
        //display = privateDisplay; //Wombat778 3-24-04  CTD Fix in virtual cockpit.
        OffMode(display);
        return;
    }
    else if (not self->HasPower(AircraftClass::RightHptPower))
    {
        OffMode(display);
        BottomRow();
        StbyMode = TRUE;
        return;
    }

    flash = (vuxRealTime bitand 0x200);

    //Check if we're in standby
    if (self->PodCooling > 0.0F) //always stby
        pFCC->InhibitFire = TRUE;

    if (pFCC->InhibitFire)
        StbyMode = TRUE;

    if (SP)
        pFCC->preDesignate = FALSE;

    laserPod->MenuMode = MenuMode;

    if (LstMode and not StbyMode)
        LstSearch();

    //pod image
    // Artscout - 2026: in STBY the pod is not imaging -- skip the 3D scene. It
    // rendered regardless of the mode, so objects showed through in standby.
    if (laserPod and self->PodCooling <= 0.0F and not StbyMode)
    {
        laserPod->SetIntensity(GetIntensity());
        laserPod->Display(display);
    }
    else if (self->PodCooling > 0.0F)
    {
        CoolingDisplay(display);
        OSBLabels(display);
        BottomRow();
        return;
    }

    // FRB - B&W display
    if ((g_bGreyMFD) and (not bNVGmode))
        display->SetColor(GetMfdColor(MFD_WHITE));
    else
        display->SetColor(0xffffffff);

    // OSB Button Labels
    OSBLabels(display);

    //Bottom Row
    BottomRow();

    //Master Arm
    DrawMasterArm(display);

    //Ralt/Laser/Impact time/weapons indication
    if (not MenuMode)
    {
        DrawRALT(display);
        LaserIndicator(display);
        ImpactTime(display);
        DrawHDPT(display, Sms);
        DrawSniperReadouts(display);
    }

    theRadar->GetCursorPosition(&cX, &cY);

    if (OTWDriver.pCockpitManager and OTWDriver.pCockpitManager->mpIcp and
        OTWDriver.pCockpitManager->mpIcp->ShowBullseyeInfo)
        DrawBullseyeCircle(display, cX, cY);
    else
        DrawReference(MfdDisplay[OnMFD()]->GetOwnShip());

    //Laser Ranging
    DrawRange(display);

    //Booster 18/09/2004 - Draw Pull Up cross on MFD-SMS if ground Collision
    if (playerAC->mFaults->GetFault(alt_low))
        DrawRedBreak(display);

    // RV - I-Hawk - if in TGP mode (should be only in AG mode) and pitch < -20 degrees or
    // Roll > 75 degrees and pitch < 0 degrees, show attiutde warning
    if (playerAC and
        ((fabs(playerAC->Roll()) > 75.0f * DTR and playerAC->Pitch() < 0.0f) or
         (playerAC->Pitch() < -20.0f * DTR)))
    {
        for (int i = 0; i < 4; i++)
        {
            (MfdDisplay[i])->SetTGPWarning(true); // Assign to all
        }
    }

    else
    {
        for (int i = 0; i < 4; i++)
        {
            (MfdDisplay[i])->SetTGPWarning(false); // Remove from all
        }
    }

    // Check, if 1 of the MFDs is showing TGP and the attitude warning is set, show it...
    // (works on all MFDs and not only here)
    for (int i = 0; i < 4; i++)
    {
        if ((MfdDisplay[i])->GetTGPWarning() and
            (MfdDisplay[i])->CurMode() == MFDClass::TGPMode)
        {
            TGPAttitudeWarning(display);
            break;
        }
    }
}

void TgpMfdDrawable::PushButton(int whichButton, int whichMFD)
{
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();
    FireControlComputer* pFCC = playerAC->Sms->Ownship()->GetFCC();
    LaserPodClass* laserPod =
        (LaserPodClass*)FindLaserPod(playerAC->Sms->Ownship());

    switch (whichButton)
    {
    case 0:
        MenuMode = not MenuMode;
        break;

    case 1: // PTR: IR pointer
        IrPointer = not IrPointer;
        break;

    case 4: // CNTL: control page
        CntlMode = not CntlMode;
        MenuMode = FALSE;
        break;

    case 2:
        if (laserPod)
            laserPod->ToggleFOV();

        break;

    case 3:
        pFCC->LaserFire = FALSE;
        pFCC->InhibitFire = not pFCC->InhibitFire;
        StbyMode = not StbyMode;
        break;

    case 5:
        if (MenuMode)
        {
            StbyMode = FALSE;
            MenuMode = FALSE;
            pFCC->InhibitFire = FALSE;
        }
        else if (laserPod)
            laserPod->BHOT = not laserPod->BHOT;

        break;

    case 7:
        pFCC->preDesignate = not pFCC->preDesignate;
        SP = not SP;
        break;

    case 8:
        pFCC->RecalcPos();
        break;

    case 9:
        StbyMode = not StbyMode;
        //pFCC->InhibitFire = not pFCC->InhibitFire;
        MenuMode = not MenuMode;
        break;

    case 19:
        if (CntlMode)
            break;

        LstMode = not LstMode; // LST: laser spot tracker
        break;

    case 18: // control page: GRAY
        if (CntlMode)
        {
            if ((g_bGreyMFD) or (not g_bGreyScaleMFD))
                g_bGreyMFD = false;
            else
                g_bGreyMFD = true;
        }

        break;

    case 17: // control page: north pointer / meterstick
        if (CntlMode)
            NorthPtr = not NorthPtr;

        break;

    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
        MfdDrawable::PushButton(whichButton, whichMFD);
        break;

    default:
        break;
    }
}
void TgpMfdDrawable::OffMode(VirtualDisplay* display)
{
    float cX, cY = 0;
    display->TextCenterVertical(0.0f, 0.2f,
                                "TGP"); // JPG 14 Dec 03 - Just changed to CAPs
    int ofont = display->CurFont();
    display->SetFont(2);
    display->TextCenterVertical(0.0f, 0.0f, "OFF"); // this too
    display->SetFont(ofont);
    theRadar->GetCursorPosition(&cX, &cY);

    if (OTWDriver.pCockpitManager and
        OTWDriver.pCockpitManager
            ->mpIcp and // JPG 14 Dec 03 - Added BE/ownship info
        OTWDriver.pCockpitManager->mpIcp->ShowBullseyeInfo)
        DrawBullseyeCircle(display, cX, cY);
    else
        DrawReference(MfdDisplay[OnMFD()]->GetOwnShip());

    BottomRow();
}
VirtualDisplay* TgpMfdDrawable::GetDisplay(void)
{
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();

    if (not playerAC or not playerAC->Sms)
        return privateDisplay;

    SensorClass* laserPod = FindLaserPod(playerAC->Sms->Ownship());

    if (laserPod)
    {
        if (not laserPod->GetDisplay())
        {
            if (privateDisplay)
            {
                laserPod->DisplayInit(
                    ((Render2D*)(privateDisplay))->GetImageBuffer());

                if ((g_bGreyMFD) and (not bNVGmode))
                    privateDisplay->SetColor(0xff000000);
            }

            laserPod->viewPoint = viewPoint;
        }

        return laserPod->GetDisplay();
    }

    return privateDisplay;
}
void TgpMfdDrawable::DrawRALT(VirtualDisplay* display)
{
    if (TheHud and
        not(self->mFaults and
            self->mFaults->GetFault(FaultClass::ralt_fault)) and
        self->af->platform->RaltReady() and
        TheHud->FindRollAngle(-TheHud->hat) and
        TheHud->FindPitchAngle(-TheHud->hat))
    {
        float x, y = 0;
        GetButtonPos(5, &x, &y);
        y += display->TextHeight();
        x -= 0.05F;
        int RALT = (int)-TheHud->hat;
        char tempstr[10] = "";

        if (RALT > 9990)
            RALT = 9990;

        sprintf(tempstr, "%d", RALT);
        display->TextRight(x, y, tempstr);
    }
}
void TgpMfdDrawable::DrawHDPT(VirtualDisplay* display, SMSClass* Sms)
{
    float leftEdge;
    float width = display->TextWidth("M ");
    float x = 0, y = 0;
    GetButtonPos(8, &x, &y); // use button 8 as a reference (lower rhs)
    y -= display->TextHeight() * 2;

    for (int i = 0; i < Sms->NumHardpoints(); i++)
    {
        char c = HdptStationSym(i, Sms);

        if (c == ' ')
            continue; // Don't bother drawing blanks.

        Str[0] = c;
        Str[1] = '\0';

        if (i < 6)
        {
            leftEdge = -x + width * (i - 1);
            display->TextLeft(leftEdge, y, Str,
                              (Sms->CurHardpoint() == i ? 2 : 0));
        }
        else
        {
            leftEdge = x - width * (Sms->NumHardpoints() - i - 1);
            // Box the current station
            display->TextRight(leftEdge, y, Str,
                               (Sms->CurHardpoint() == i ? 2 : 0));
        }
    }
}
char TgpMfdDrawable::HdptStationSym(int n, SMSClass* Sms) // JPO new routine
{
    if (Sms->hardPoint[n] == NULL)
        return ' '; // empty hp

    if (Sms->hardPoint[n]->weaponCount <= 0)
        return ' '; //MI don't bother drawing empty hardpoints

    if (Sms->StationOK(n) == FALSE)
        return 'F'; // malfunction on  HP

    if (Sms->hardPoint[n]->GetWeaponType() == Sms->curWeaponType)
        return '0' + n; // exact match for weapon

    if (Sms->hardPoint[n]->GetWeaponClass() == Sms->curWeaponClass)
        return 'M'; // weapon of similar class

    return ' ';
}
// Artscout - 2026: laser spot tracker. With LST selected and the pod not on a point of its own, look for a
// target another aircraft is lasing (IS_LASED, set by FalconLaserDesignateMsg) within 40 nm and move the pod's
// point of interest onto it. Only matters when someone else lases (multiplayer, or a human wingman).
void TgpMfdDrawable::LstSearch(void)
{
    if (not laserPod or not self or laserPod->IsLocked() or not SimDriver.combinedList)
        return;

    VuListIterator it(SimDriver.combinedList);
    SimBaseClass* best = NULL;
    float bestRange = 40.0f * NM_TO_FT;

    for (SimBaseClass* e = (SimBaseClass*)it.GetFirst(); e; e = (SimBaseClass*)it.GetNext())
    {
        if (not e->IsSetFlag(IS_LASED) or e == self)
            continue;

        const float dx = e->XPos() - self->XPos();
        const float dy = e->YPos() - self->YPos();
        const float r = (float)sqrt(dx * dx + dy * dy);

        if (r < bestRange)
        {
            bestRange = r;
            best = e;
        }
    }

    if (best and pFCC)
    {
        pFCC->groundDesignateX = best->XPos();
        pFCC->groundDesignateY = best->YPos();
        pFCC->groundDesignateZ = best->ZPos();
        pFCC->preDesignate = FALSE;
        laserPod->SetTargetPosition(best->XPos(), best->YPos(), best->ZPos());
    }
}

// Artscout - 2026: the readouts of the Sniper XR base page that this pod page lacked
// (TO 1F-16CMAM-34-1-1 2.6.1.2, figure on p.316): the sensor point of interest's latitude,
// longitude and elevation top left, the zoom setting, the north pointer, and the laser mode,
// status and code along the bottom. Positions are read off that figure (-1..1 across the screen).
// Display only; nothing here changes how the pod behaves.
void TgpMfdDrawable::DrawSniperReadouts(VirtualDisplay* display)
{
    if (not laserPod or StbyMode)
        return;

    char buf[32];

    // Sensor point of interest: where the pod is looking on the ground
    float px, py, pz;
    laserPod->GetTargetPosition(&px, &py, &pz);

    float lat, lon;
    ApproxLatLong(px, py, &lat, &lon);
    lat *= RTD;
    lon *= RTD;

    const float latMin = (float)fabs(lat - (int)lat) * DEG_TO_MIN;
    const float lonMin = (float)fabs(lon - (int)lon) * DEG_TO_MIN;

    sprintf(buf, "%d:%06.3f", (int)fabs(lat), latMin);
    display->TextLeft(-0.72f, 0.62f, buf);
    sprintf(buf, "%d:%06.3f", (int)fabs(lon), lonMin);
    display->TextLeft(-0.72f, 0.51f, buf);
    sprintf(buf, "%+d", (int)(-pz));
    display->TextLeft(-0.72f, 0.40f, buf);

    // zoom, as a multiple of the widest field of view (6 degrees)
    const float fov = laserPod->CurFOV();

    if (fov > 0.0f)
    {
        sprintf(buf, "%.1fX", (6.0f * DTR) / fov);
        display->TextCenter(0.02f, 0.51f, buf);
    }

    float yaw, pitch, roll;
    laserPod->GetYPR(&yaw, &pitch, &roll);

    if (NorthPtr)
    {
        // north pointer: the view is ground-stabilised with screen-up along the pod's heading, so north lies at
        // -heading, clockwise from up
        const float cx = 0.46f, cy = 0.5f, r = 0.07f;
        const float nx = (float)sin(-yaw), ny = (float)cos(-yaw);
        display->Line(cx, cy, cx + nx * r, cy + ny * r);
        display->TextCenter(cx + nx * (r + 0.05f), cy + ny * (r + 0.05f), "N");
    }
    else
    {
        // meterstick: ground distance from the cross to the end of its right arm (x = 0.55 of the half width)
        float ax, ay, az;
        AircraftClass* me = SimDriver.GetPlayerAircraft();

        if (me)
        {
            ax = px - me->XPos();
            ay = py - me->YPos();
            az = pz - me->ZPos();
            const float slant = (float)sqrt(ax * ax + ay * ay + az * az);
            const float feet = 0.55f * slant * (float)tan(0.5f * fov);
            sprintf(buf, "%d", (int)(feet + 0.5f));
            display->Line(0.30f, 0.0f, 0.55f, 0.0f);
            display->TextLeft(0.57f, 0.04f, buf);
        }
    }

    if (IrPointer)
        display->TextCenter(0.64f, -0.80f, "IR PTR");

    // laser: mode, status, code
    display->TextCenter(0.0f, -0.80f, "CMBT");

    if (pFCC and pFCC->LaserArm)
        display->TextCenter(0.0f, -0.88f, pFCC->LaserFire ? "LASE" : "RDY");

    if (OTWDriver.pCockpitManager and OTWDriver.pCockpitManager->mpIcp)
    {
        sprintf(buf, "%d", OTWDriver.pCockpitManager->mpIcp->LaserCode);
        display->TextCenter(0.30f, -0.80f, buf);
    }
}

void TgpMfdDrawable::LaserIndicator(VirtualDisplay* display)
{
    float x, y = 0;
    GetButtonPos(9, &x, &y);
    y += display->TextHeight();

#if 0
    display->TextLeft(.25f, .80f, "LaserArm         ", (pFCC->LaserArm          ? 2 : 0));
    display->TextLeft(.25f, .70f, "CheckForLaserFire", (pFCC->CheckForLaserFire ? 2 : 0));
    display->TextLeft(.25f, .60f, "InhibitFire      ", (pFCC->InhibitFire       ? 2 : 0));
    display->TextLeft(.25f, .50f, "LaserWasFired    ", (pFCC->LaserWasFired     ? 2 : 0));
    display->TextLeft(.25f, .40f, "LaserFire        ", (pFCC->LaserFire         ? 2 : 0));
    display->TextLeft(.25f, .30f, "ManualFire       ", (pFCC->ManualFire        ? 2 : 0));
#endif

    if (laserPod and pFCC->LaserArm)
    {
        //armed and fired
        if (pFCC->LaserFire)
        {
            if (flash)
                display->TextLeft(x - 0.75F, y, "L");
        }
        //armed, put it there steady
        else if (pFCC->LaserArm)
            display->TextLeft(x - 0.75F, y, "L");
    }

    /* display->TextLeft(.25f, .80f, "LaserArm         ", (pFCC->LaserArm          ? 2 : 0));
     display->TextLeft(.25f, .70f, "CheckForLaserFire", (pFCC->CheckForLaserFire ? 2 : 0));
     display->TextLeft(.25f, .60f, "InhibitFire      ", (pFCC->InhibitFire       ? 2 : 0));
     display->TextLeft(.25f, .50f, "LaserWasFired    ", (pFCC->LaserWasFired     ? 2 : 0));
     display->TextLeft(.25f, .40f, "LaserFire        ", (pFCC->LaserFire         ? 2 : 0));
     display->TextLeft(.25f, .30f, "ManualFire       ", (pFCC->ManualFire        ? 2 : 0)); */
}
void TgpMfdDrawable::ImpactTime(VirtualDisplay* display)
{
    float x, y = 0;
    GetButtonPos(9, &x, &y);

    if (pFCC->ImpactTime >= 10.0F)
        sprintf(Str, "00:%2.0f", pFCC->ImpactTime);
    else if (pFCC->ImpactTime > 0 and pFCC->ImpactTime < 10.0F)
        sprintf(Str, "00:0%1.0f", pFCC->ImpactTime);
    else
        sprintf(Str, "00:00");

    y -= display->TextHeight();
    display->TextLeft(x - 0.55F, y, Str);
}
void TgpMfdDrawable::DrawMasterArm(VirtualDisplay* display)
{
    float x, y = 0;
    char* mode = "";

    if (Sms->CurStationOK())
    {
        switch (Sms->MasterArm())
        {
        case SMSBaseClass::Safe:

            //MI not here in real
            if (not g_bRealisticAvionics)
                mode = "SAF";
            else
                mode = "";

            break;

        case SMSBaseClass::Sim:
            mode = "SIM";
            break;

        case SMSBaseClass::Arm:
            mode = "RDY";
            break;
        }
    }
    else
        mode = "MAL";

    GetButtonPos(12, &x, &y);
    // JPO - do this ourselves, so we can pass the rest off to the superclass.
    display->TextCenter(x, y + display->TextHeight(), mode);
}
void TgpMfdDrawable::CoolingDisplay(VirtualDisplay* display)
{
    int ofont = display->CurFont();
    display->SetFont(2);
    display->TextCenterVertical(0.0f, 0.2f, "NOT TIMED OUT");
    display->SetFont(ofont);
}
void TgpMfdDrawable::OSBLabels(VirtualDisplay* display)
{
    if (MenuMode)
    {
        if (StbyMode)
            LabelButton(0, "STBY");
        else
            LabelButton(0, "A-G");

        if (laserPod->CurFOV() < 1.7F * DTR)
            sprintf(Str, "EXP");
        else
            sprintf(Str, "%s",
                    (laserPod->CurFOV() < (3.5F * DTR)) ? "NARO" : "WIDE");

        LabelButton(2, Str);

        if (pFCC->InhibitFire)
            LabelButton(3, "OVRD", NULL, 2);
        else
            LabelButton(3, "OVRD");

        if (pFCC->IsAGMasterMode())
        {
            if (StbyMode)
                LabelButton(5, "A-G");
            else
                LabelButton(5, "A-G", NULL, 2);

            if (StbyMode)
                LabelButton(9, "STBY", NULL, 2);
            else
                LabelButton(9, "STBY");
        }
        else if (pFCC->IsAAMasterMode())
        {
            LabelButton(19, "A-A");

            if (StbyMode)
                LabelButton(9, "STBY", NULL, 2);
            else
                LabelButton(9, "STBY");
        }
        else //NAV
        {
            if (StbyMode)
                LabelButton(5, "A-G");
            else
                LabelButton(5, "A-G", NULL, 2);

            if (StbyMode)
                LabelButton(9, "STBY", NULL, 2);
            else
                LabelButton(9, "STBY");

            LabelButton(19, "A-A");
        }
    }
    else if (StbyMode)
    {
        LabelButton(0, "STBY");

        if (laserPod->CurFOV() < 1.7F * DTR)
            sprintf(Str, "EXP");
        else
            sprintf(Str, "%s",
                    (laserPod->CurFOV() < (3.5F * DTR)) ? "NARO" : "WIDE");

        LabelButton(2, Str);

        if (pFCC->InhibitFire)
            LabelButton(3, "OVRD", NULL, 2);
        else
            LabelButton(3, "OVRD");

        LabelButton(4, "CNTL");
        sprintf(Str, "%s", laserPod->BHOT ? "BHOT" : "WHOT");
        LabelButton(5, Str);

        if (SP)
            LabelButton(7, "S", "P", 2);
        else
            LabelButton(7, "S", "P");

        LabelButton(8, "C", "Z");
        LabelButton(9, "T", "G");

    }
    else
    {
        LabelButton(0,
                    "A-G"); //would be either A-A or A-G.. but AA isn't modelled

        if (laserPod->CurFOV() < 1.7F * DTR)
            sprintf(Str, "EXP");
        else
            sprintf(Str, "%s",
                    (laserPod->CurFOV() < (3.5F * DTR)) ? "NARO" : "WIDE");

        LabelButton(2, Str);

        if (pFCC->InhibitFire)
            LabelButton(3, "OVRD", NULL, 2);
        else
            LabelButton(3, "OVRD");

        LabelButton(4, "CNTL");

        if (SP)
            LabelButton(7, "S", "P", 2);
        else
            LabelButton(7, "S", "P");

        LabelButton(8, "C", "Z");
        LabelButton(9, "T", "G");


        //BHOT/WHOT
        sprintf(Str, "%s", laserPod->BHOT ? "BHOT" : "WHOT");
        LabelButton(5, Str);
    }
    // Artscout - 2026: Sniper base page extras (TO 1F-16CMAM-34-1-1 2.6.1.2): PTR on OSB 2, LST on OSB 20; the
    // control page puts GRAY on OSB 19 and the north pointer / meterstick select on OSB 18.
    if (not MenuMode)
    {
        LabelButton(1, "PTR", NULL, IrPointer);

        if (CntlMode)
        {
            LabelButton(4, "CNTL", NULL, 2);
            LabelButton(18, "GRAY", g_bGreyMFD ? "OFF" : "ON");
            LabelButton(17, NorthPtr ? "N" : "M", NorthPtr ? "(M)" : "(N)");
        }
        else
            LabelButton(19, "L", "ST", LstMode);
    }
}
void TgpMfdDrawable::DrawRange(VirtualDisplay* display)
{
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();
    FireControlComputer* pFCC = playerAC->Sms->Ownship()->GetFCC();
    float x, y = 0;

    if (not pFCC)
        return;

    //don't draw range in slave mode
    if (pFCC->preDesignate)
        return;

    float range = pFCC->LaserRange * FT_TO_NM;

    if (range <= 0.0F)
    {
        if (pFCC->LaserFire)
            sprintf(Str, "L 00.0");
        else
            sprintf(Str, "T 00.0");
    }
    else if (range < 10.0F)
    {
        if (pFCC->LaserFire)
            sprintf(Str, "L 0%1.1f", range);
        else
            sprintf(Str, "T 0%1.1f", range);
    }
    else if (range >= 10.0F)
    {
        if (pFCC->LaserFire)
            sprintf(Str, "L %2.1f", range);
        else
            sprintf(Str, "T %2.1f", range);
    }

    GetButtonPos(15, &x, &y);
    y -= display->TextHeight();
    display->TextCenter(x + 0.60F, y, Str);
}
