// campsim stubs: the handful of hooks the harness needs that are normally
// driven by the game's window/UI bring-up. Everything else (config globals,
// logbook, weather pointer, message handler) now comes from the real
// f4config.cpp / winmain.cpp that campsim compiles in.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f4find.h"
#include "falcgame.h"
#include "dispcfg.h"
#include "ui/include/uicomms.h"
#include "realweather.h"
#include "weather.h"

static HWND s_campsimWin = NULL;

void CampsimSetSeed(unsigned int seed)
{
    srand(seed);
}

void CampsimSetAppWindow(HWND win)
{
    extern HWND mainAppWnd;
    extern HINSTANCE hInst;
    s_campsimWin = win;
    mainAppWnd = win;
    FalconDisplay.appWin = win;
    hInst = GetModuleHandle(NULL);
}

void CampsimSetupGlobals(void)
{
    gCommsMgr = new UIComms;
    realWeather = new WeatherClass();
}

void CampsimDumpMoveDiagStub(void)
{
}

// winmain.cpp's PlayMovie calls the Media Foundation player (MovieMF.cpp, game only).
void PlayMovieMF(const char *, HWND)
{
}
