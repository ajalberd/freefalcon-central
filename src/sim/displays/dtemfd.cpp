#include "stdhdr.h"
#include "mfd.h"
#include "hud.h"
#include "sms.h"
#include "simdrive.h"
#include "camp2sim.h"
#include "graphics/include/render2d.h"
#include "otwdrive.h" //MI
#include "cpmanager.h" //MI
#include "icp.h" //MI
#include "aircrft.h" //MI
#include "fcc.h" //MI
#include "radardoppler.h" //MI

//MI
void DrawBullseyeCircle(VirtualDisplay* display, float cursorX, float cursorY);


void DteMfdDrawable::Display(VirtualDisplay* newDisplay)
{
    //MI
    float cX, cY = 0;
    AircraftClass* playerAC = SimDriver.GetPlayerAircraft();
    RadarDopplerClass* theRadar =
        (RadarDopplerClass*)FindSensor(playerAC, SensorClass::Radar);

    if (not theRadar)
    {
        ShiWarning("Oh Oh shouldn't be here without a radar");
        return;
    }
    else
    {
        theRadar->GetCursorPosition(&cX, &cY);
    }

    display = newDisplay;
    display->TextCenter(0.0f, 0.2f, "DTC ID");
    float height = display->TextHeight();

    // a six-digit id taken from the player's flight, so it differs between packages
    char dtcId[12] = "126534";

    if (playerAC and playerAC->GetCampaignObject())
        sprintf(dtcId, "%06lu", 100000UL + ((unsigned long)playerAC->GetCampaignObject()->Id().num_ * 7919UL) % 900000UL);

    display->TextCenter(0.0f, 0.2f - height, dtcId);

    // LOAD: light each subsystem in turn (counter-clockwise from LOAD), then apply the defaults
    static const int sequence[] = {19, 18, 17, 16, 15, 9, 6, 5}; // MPD COMM INV PROF MSMD GPS DLNK FCR
    static const unsigned long stepMs = 600;
    const int steps = sizeof(sequence) / sizeof(sequence[0]);
    int lit = -1;
    bool done = false;

    if (loadStart)
    {
        const unsigned long elapsed = vuxRealTime - loadStart;
        lit = (int)(elapsed / stepMs);

        if (lit >= steps)
        {
            if (not loadApplied and OTWDriver.pCockpitManager and OTWDriver.pCockpitManager->mpIcp)
            {
                OTWDriver.pCockpitManager->mpIcp->InitStuff();
                loadApplied = true;
            }

            done = true;

            if (elapsed > steps * stepMs + 3000)
            {
                loadStart = 0;
                loadApplied = false;
            }
        }
    }

    const int litButton = (lit >= 0 and lit < steps) ? sequence[lit] : -1;
    LabelButton(0, "ON");
    LabelButton(2, "LOAD", NULL, loadStart ? 1 : 0);
    LabelButton(5, "FCR", NULL, litButton == 5);
    LabelButton(6, "DLNK", NULL, litButton == 6);
    LabelButton(9, "----", "GPS", litButton == 9);
    BottomRow();
    LabelButton(15, "MSMD", NULL, litButton == 15);
    LabelButton(16, "PROF", NULL, litButton == 16);
    LabelButton(17, "INV", NULL, litButton == 17);
    LabelButton(18, "COMM", NULL, litButton == 18);
    LabelButton(19, "MPD", NULL, litButton == 19);

    if (done)
        display->TextCenter(0.0f, -0.6f, "DTC LOAD COMPLETE");

    //MI changed
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
}

void DteMfdDrawable::PushButton(int whichButton, int whichMFD)
{
    if (whichButton == 2 and not loadStart) // LOAD
    {
        loadStart = vuxRealTime ? vuxRealTime : 1;
        loadApplied = false;
        return;
    }

    MfdDrawable::PushButton(whichButton, whichMFD);
}
