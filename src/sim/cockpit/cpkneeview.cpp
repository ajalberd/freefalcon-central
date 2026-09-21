#include "stdafx.h"
#include "cpmanager.h"
#include "kneeboard.h"
#include "cpkneeview.h"
#include "dispcfg.h"
#include "otwdrive.h"
#include "simdrive.h" //MI
#include "navsystem.h" //MI
#include "graphics/include/renderow.h"
#include "graphics/include/tmap.h"
#include "brief.h"
#include "flight.h"
#include "aircrft.h"
// Artscout - 2026 (NAVAIDS page): the campaign side of the kneeboard. The
// objective list for the airbases, the TACAN list for their channels and ILS,
// the point tables for their runway headings, and the campaign for bullseye.
#include "campaign/include/camplist.h"
#include "campaign/include/objectiv.h"
#include "campaign/include/cmpclass.h"
#include "campaign/include/find.h"
#include "campaign/include/ptdata.h"
#include "campaign/include/team.h"
#include "falclib/include/classtbl.h"
#include "falclib/include/entity.h"
#include "tacan.h"
#include "phyconst.h"
#include "simlib.h"


// sfr: moved the f***** globals from kneeboard to kneeview
extern bool g_bRealisticAvionics; //MI
extern bool g_bINS; //MI

static const UInt32 WP_COLOR = 0xFF0000FF; // The color of the waypoint marks
static const float WP_SIZE = 0.03f; // The radius of the waypoint marker symbol
static const float ORIDE_WP_SIZE =
    0.08f; // The size of the override waypoint marker
static const UInt32 AC_COLOR =
    0xFF00FFFF; // The color of the aircraft location marker
static const float AC_SIZE =
    0.06f; // The radius of the aircraft location marker
static const float BORDER_PERCENT =
    0.05f; // How much map to display outside the bounding box of the waypoints
static const float KNEEBOARD_SMALLEST_MAP_FRACTION =
    4.0f; // What is the smallest fraction of the map we'll zoom to


// Artscout - 2026 (3D kneeboard): the page colour behind the two TEXT pages. DrawMissionText inks in
// BLACK -- on the 2D board it lands on the kneeboard art painted into the cockpit bitmap, but the RTT
// atlas is cleared to black at the top of the pass, so without a page under it the text is invisible.
// ABGR, like every other colour in this file (WP_COLOR 0xFF0000FF is red). Warm paper white.
static const UInt32 KNEE_PAGE_COLOR = 0xFFD0E4E8;

// Artscout - 2026 (NAVAIDS page): Korea ships 90 airbases and the page shows
// the nearest screenful of them, so the cap is a safety net rather than a
// budget. The rebuild interval is what keeps a whole-theater objective sweep
// off the frame.
static const int MAX_NAVAIDS = 256;
static const unsigned long NAVAID_REBUILD_MS = 5000;

// The page runs -0.95..0.95, so a row has 1.90 to live in. The header is the
// widest line the page ever draws -- every data row is built to the same column
// widths -- so fitting the header fits the page.
static const float NAVAID_LEFT = -0.95f;
static const float NAVAID_TOP = 0.95f;
static const float NAVAID_WIDTH = 1.90f;
static const char NAVAID_HEADER[] =
    "BASE        TCN   ILS   RWY  BULLS  NM";

// Artscout - 2026: a zeroed ObjectInitStr with the two fields CPObject actually scales by. Used by the
// RTT constructor, which has no 2D cockpit dat block behind it. A base-class initialiser cannot take the
// address of a temporary, hence the static -- CPObject copies every field out of it in its constructor,
// so nothing outlives this call and the one 3D kneeview is built once, on the sim thread.
static const ObjectInitStr *KneeRttInitStr(int w, int h)
{
    static ObjectInitStr s;
    memset(&s, 0, sizeof(s));
    s.hScale = 1.0f;
    s.vScale = 1.0f;
    s.bsurface = -1;
    s.callbackSlot = -1;
    s.destRect.top = 0;
    s.destRect.left = 0;
    // CPObject adds 1 to bottom/right, so subtract it here to land on exactly w x h.
    s.destRect.bottom = h - 1;
    s.destRect.right = w - 1;
    return &s;
}

CPKneeView::CPKneeView(ObjectInitStr *pobjectInitStr, KneeBoard *pboard)
    : CPObject(pobjectInitStr)
{
    mapImageBuffer = NULL;
    mRtt = false;
    mRttPixels = NULL;
    mRttW = mRttH = 0;
    mRttMapV = mRttMapH = mRttMapVS = mRttMapHS = 0.0f;
    mRttTick = 0;
    mNavaids = NULL;
    mNavaidCount = 0;
    mNavaidBuilt = 0;
    mpKneeBoard = pboard;

    Setup(&FalconDisplay.theDisplayDevice, mDestRect.top, mDestRect.left,
          mDestRect.bottom, mDestRect.right);
}

CPKneeView::CPKneeView(KneeBoard *board, int w, int h)
    : CPObject(KneeRttInitStr(w, h))
{
    mapImageBuffer = NULL;
    mRtt = false;
    mRttPixels = NULL;
    mRttW = mRttH = 0;
    mRttMapV = mRttMapH = mRttMapVS = mRttMapHS = 0.0f;
    mRttTick = 0;
    mNavaids = NULL;
    mNavaidCount = 0;
    mNavaidBuilt = 0;
    mpKneeBoard = board;

    SetupRtt(board, w, h);
}

CPKneeView::~CPKneeView()
{
    Cleanup();

    // Artscout - 2026 (NAVAIDS page): allocated lazily on first use of the page,
    // so most kneeviews never own one.
    if (mNavaids)
    {
        delete[] mNavaids;
        mNavaids = NULL;
    }

    mNavaidCount = 0;
}

void CPKneeView::Setup(DisplayDevice *device, int top, int left, int bottom,
                       int right)
{
    mpKneeBoard->Setup();
    dstRect.top = top;
    dstRect.left = left;
    dstRect.bottom = bottom;
    dstRect.right = right;

    srcRect.top = 0;
    srcRect.left = 0;
    srcRect.bottom = bottom - top;
    srcRect.right = right - left;

    // Setup our off screen map buffer and renderer
    MPRSurfaceType front =
        FalconDisplay.theDisplayDevice.IsHardware() ? VideoMem : SystemMem;
    mapImageBuffer = new ImageBuffer;
    mapImageBuffer->Setup(device, srcRect.right, srcRect.bottom, front, None);
    Render2D::Setup(mapImageBuffer);
}

// Artscout - 2026 (3D kneeboard): the RTT twin of Setup. Same rects -- UpdateMapDimensions and the map
// rasteriser both size themselves off srcRect/dstRect, so pointing those at the atlas zone is all it
// takes for the map window maths to work at the 3D board's resolution. No ImageBuffer and no
// Render2D::Setup: this instance never draws through its own base, only through the canvas it is given.
void CPKneeView::SetupRtt(KneeBoard *board, int w, int h)
{
    mpKneeBoard = board;
    mpKneeBoard->Setup();

    dstRect.top = 0;
    dstRect.left = 0;
    dstRect.bottom = h;
    dstRect.right = w;

    srcRect = dstRect;

    mRttW = w;
    mRttH = h;
    mRttPixels = new UInt32[(size_t)w * (size_t)h];
    memset(mRttPixels, 0, (size_t)w * (size_t)h * sizeof(UInt32));
    mRtt = true;
}

void CPKneeView::Cleanup()
{
    if (mapImageBuffer)
    {
        mapImageBuffer->Cleanup();
        delete mapImageBuffer;
        mapImageBuffer = NULL;
    }

    if (mRttPixels)
    {
        delete[] mRttPixels;
        mRttPixels = NULL;
    }

    // The RTT instance never ran Render2D::Setup, so it has no image/context to tear down.
    if (not mRtt)
        Render2D::Cleanup();

    mRtt = false;

    if (mpKneeBoard)
        mpKneeBoard->Cleanup();
}

void CPKneeView::Refresh(SimVehicleClass *platform)
{
    RenderMap(platform);
}

void CPKneeView::Exec(SimBaseClass *pOwnship)
{
    mpOwnship = pOwnship;
    RenderMap((SimVehicleClass *)mpOwnship);
}

void CPKneeView::DisplayBlit(void)
{
    if (mpKneeBoard->GetPage() == KneeBoard::MAP)
    {
        // BLT in the map
        mpOTWImage->Compose(mapImageBuffer, &srcRect, &dstRect);
    }
}

// Artscout - 2026 (3D kneeboard): refresh the off-screen page. Only the map costs anything -- the two
// text pages are drawn straight from the briefing data every frame, as they are on the 2D board.
void CPKneeView::ExecRtt(SimVehicleClass *platform)
{
    if (not mRtt or not mpKneeBoard or not platform or not mRttPixels)
        return;

    if (mpKneeBoard->GetPage() not_eq KneeBoard::MAP)
        return;

    UpdateMapDimensions(platform);

    // Cheap so far -- UpdateMapDimensions is pure arithmetic. The blit below is a megapixel of palette
    // lookups, so only run it when the window it draws has actually moved (or once in a while, for
    // lighting). mRttTick starting at 0 makes the first frame after a page change always rasterise.
    const bool moved =
        (wsVcenter not_eq mRttMapV) or (wsHcenter not_eq mRttMapH) or
        (wsVsize not_eq mRttMapVS) or (wsHsize not_eq mRttMapHS);

    if (moved or (mRttTick % 32) == 0)
    {
        RasteriseMap(mRttPixels, mRttW * (int)sizeof(UInt32), true);
        mRttMapV = wsVcenter;
        mRttMapH = wsHcenter;
        mRttMapVS = wsVsize;
        mRttMapHS = wsHsize;
    }

    mRttTick++;
}


// Artscout - 2026 (3D kneeboard): draw the current page into canvas's atlas zone. Call inside the RTT
// pass, between StartRtt and FinishRtt -- AdjustRttViewport binds the atlas and points this display's
// -1..1 space at its own sub-zone, and the screen metric is the full atlas, which is the space
// Render2DBitmap's destination is measured in.
void CPKneeView::DisplayRtt(Render2D *canvas, SimVehicleClass *platform)
{
    if (not mRtt or not canvas or not mpKneeBoard or not platform)
        return;

    canvas->AdjustRttViewport();

    // The canvas is reused frame to frame and DrawCurrentPosition below leaves the origin shifted;
    // SetViewport does not reset it, so start from a known transform every frame.
    canvas->CenterOriginInViewport();
    canvas->ZeroRotationAboutOrigin();

    int oldFont = VirtualDisplay::CurFont();

    if (mpKneeBoard->GetPage() == KneeBoard::MAP)
    {
        int zLeft = 0, zTop = 0, zRight = 0, zBottom = 0;
        canvas->GetRttRect(&zLeft, &zTop, &zRight, &zBottom);

        int w = min(mRttW, zRight - zLeft);
        int h = min(mRttH, zBottom - zTop);

        if (mRttPixels and w > 0 and h > 0)
        {
            canvas->Render2DBitmap(0, 0, zLeft, zTop, w, h, mRttW,
                                   (DWORD *)mRttPixels);
        }

        DrawWaypoints(canvas, platform);

        // Same realism gate as the 2D board: no own-position symbol under full realism.
        // M.N. Added Full realism mode
        if (PlayerOptions.GetAvionicsType() not_eq ATRealistic and
            PlayerOptions.GetAvionicsType() not_eq ATRealisticAV)
        {
            DrawCurrentPosition(NULL, canvas, platform);
        }
    }
    else
    {
        // Lay the page down first -- see KNEE_PAGE_COLOR.
        //
        // NOT at +/-1.0. Render2D::Render2DTri does not CLIP, it REJECTS: if any vertex falls outside
        // the viewport the whole triangle is dropped. A vertex at exactly +/-1.0 maps to exactly
        // rightPixel/topPixel, so whether it survives is a floating-point coin flip -- and it lost, which
        // is why the 3D board composited black instead of paper. Inset by a pixel's worth and it is
        // deterministic. (Render2DLine has no such test, so the route overlay was never affected.)
        static const float PAGE = 0.997f;
        DWORD paper =
            OTWDriver.pCockpitManager->ApplyLighting(KNEE_PAGE_COLOR, false);
        canvas->SetColor(paper);
        canvas->Tri(-PAGE, -PAGE, PAGE, -PAGE, PAGE, PAGE);
        canvas->Tri(-PAGE, -PAGE, PAGE, PAGE, -PAGE, PAGE);

        DrawMissionText(canvas, platform);
    }

    // Artscout - 2026: COMMIT the page while the atlas is still bound. The 2D primitives here -- the
    // route lines and circles, the paper-fill tris, the text -- only BATCH into the context vertex
    // buffer; they flush lazily on the next SelectTexture1 / RestoreState / EndDraw. Under D3D12 nothing
    // in this draw does any of those, so they reached the eye only on frames where something else
    // happened to trigger a flush -- the route overlay FLICKERED while the map, which goes through
    // Render2DBitmap's immediate DrawTL, stayed rock solid. Same trap and same fix as the composite quad
    // in DrawRttQuad; see the "#DX12: COMMIT the composite quad NOW" note there.
    canvas->context.FlushPending();

    VirtualDisplay::SetFont(oldFont);
}


void CPKneeView::DisplayDraw(void)
{
    // Set the viewport to the active region of our display
    RenderOTW *renderer = OTWDriver.renderer;
    renderer->SetViewport(
        (float)dstRect.left / mpOTWImage->targetXres() * (2.0f) - 1.0f,
        (float)dstRect.top / mpOTWImage->targetYres() * (-2.0f) + 1.0f,
        (float)dstRect.right / mpOTWImage->targetXres() * (2.0f) - 1.0f,
        (float)dstRect.bottom / mpOTWImage->targetYres() * (-2.0f) + 1.0f);

    if (mpKneeBoard->GetPage() == KneeBoard::MAP)
    {
        // If we're not in Realistic mode, draw the current position marker
        // M.N. Added Full realism mode
        if (PlayerOptions.GetAvionicsType() not_eq ATRealistic and
            PlayerOptions.GetAvionicsType() not_eq ATRealisticAV)
        {
            DrawCurrentPosition(mpOTWImage, renderer,
                                (SimVehicleClass *)mpOwnship);
        }
    }
    else
    {
        DrawMissionText(renderer, (SimVehicleClass *)mpOwnship);
    }
}


// sfr moved functions to here
void CPKneeView::DrawMissionText(Render2D *renderer, SimVehicleClass *platform)
{
    // sfr: check at beginning
    if (SimDriver.RunningDogfight() or SimDriver.RunningInstantAction())
    {
        return;
    }

    float LINE_HEIGHT = renderer->TextHeight();
    int lines;
    float v = 0.80f;
    int oldFont = VirtualDisplay::CurFont();
    DWORD iColor = OTWDriver.pCockpitManager->ApplyLighting(0xFF000000, false);
    renderer->SetColor(iColor); // Black (ink color)

    // Artscout - 2026: the 3D board gets its own font. DrawMissionText is shared with the 2D board, and
    // that one should keep whatever the cockpit dat asked for -- this only diverges for the RTT instance.
    extern int g_nKnee3DFont;
    VirtualDisplay::SetFont((mRtt and g_nKnee3DFont >= 0) ?
                                g_nKnee3DFont :
                                OTWDriver.pCockpitManager->KneeFont());

    // Display the players call sign and assignment
    char string[1024];

    if (mpKneeBoard->GetPage() == KneeBoard::NAVAIDS)
    {
        // Artscout - 2026: shares the ink, the font and the paper with the other
        // text pages, and nothing else.
        DrawNavaids(renderer, platform);
        VirtualDisplay::SetFont(oldFont);
        return;
    }

    if (mpKneeBoard->GetPage() ==
        KneeBoard::STEERPOINT) // JPO new kneeboard page
    {
        v = 0.95f - LINE_HEIGHT;

        if (GetBriefingData(GBD_PACKAGE_STPTHDR, 0, string,
                            sizeof(string)) not_eq -1)
        {
            renderer->TextLeft(-0.95f, v, string);
            v -= LINE_HEIGHT;
        }

        for (lines = 0; GetBriefingData(GBD_PACKAGE_STPT, lines, string,
                                        sizeof(string)) not_eq -1;
             ++lines)
        {
            renderer->TextLeft(-0.95f, v, string);
            v -= LINE_HEIGHT;
        }

        //MI display GPS coords when on ground, for INS alignment stuff
        if (g_bRealisticAvionics and g_bINS)
        {
            v -= 2 * LINE_HEIGHT;

            if (((AircraftClass *)SimDriver.GetPlayerEntity()) and
                ((AircraftClass *)SimDriver.GetPlayerEntity())->OnGround())
            {
                char latStr[20] = "";
                char longStr[20] = "";
                char tempstr[10] = "";
                float latitude =
                    (FALCON_ORIGIN_LAT * FT_PER_DEGREE + cockpitFlightData.x) /
                    EARTH_RADIUS_FT;
                float cosLatitude = (float)cos(latitude);
                float longitude = ((FALCON_ORIGIN_LONG * DTR * EARTH_RADIUS_FT *
                                    cosLatitude) +
                                   cockpitFlightData.y) /
                                  (EARTH_RADIUS_FT * cosLatitude);

                latitude *= RTD;
                longitude *= RTD;

                int longDeg = FloatToInt32(longitude);
                float longMin = (float)fabs(longitude - longDeg) * DEG_TO_MIN;

                int latDeg = FloatToInt32(latitude);
                float latMin = (float)fabs(latitude - latDeg) * DEG_TO_MIN;

                // format lat/long here
                if (latMin < 10.0F)
                {
                    sprintf(latStr, "LAT  N %3d\x03 0%2.2f\'\n", latDeg,
                            latMin);
                }
                else
                {
                    sprintf(latStr, "LAT  N %3d\x03 %2.2f\'\n", latDeg, latMin);
                }

                if (longMin < 10.0F)
                {
                    sprintf(longStr, "LNG  E %3d\x03 0%2.2f\'\n", longDeg,
                            longMin);
                }
                else
                {
                    sprintf(longStr, "LNG  E %3d\x03 %2.2f\'\n", longDeg,
                            longMin);
                }

                renderer->TextLeft(-0.95F, v, latStr);
                v -= LINE_HEIGHT;
                renderer->TextLeft(-0.95F, v, longStr);
                v -= LINE_HEIGHT;
                sprintf(tempstr, "SALT %dFT", (long)-cockpitFlightData.z);
                renderer->TextLeft(-0.95F, v, tempstr);
            }
        }
    }
    else
    {
        if (GetBriefingData(GBD_PLAYER_ELEMENT, 0, string,
                            sizeof(string)) not_eq -1)
        {
            renderer->TextLeft(-0.9f, v, string);
            v -= LINE_HEIGHT;
        }

        if (GetBriefingData(GBD_PLAYER_TASK, 0, string, sizeof(string)) not_eq
            -1)
        {
            lines = renderer->TextWrap(-0.8f, v, string, LINE_HEIGHT, 1.7f);
            v -= (lines + 1) * LINE_HEIGHT;
        }

        // Display the package info (if we are part of a package)
        if (GetBriefingData(GBD_PACKAGE_LABEL, 0, string, sizeof(string)) not_eq
            -1)
        {
            renderer->TextLeft(-0.9f, v, string);
            v -= LINE_HEIGHT;

            // Package mission statement
            if (GetBriefingData(GBD_PACKAGE_MISSION, 0, string,
                                sizeof(string)) not_eq -1)
            {
                lines = renderer->TextWrap(-0.8f, v, string, LINE_HEIGHT, 1.7f);
                v -= lines * LINE_HEIGHT;
            }

            // List the flights in the package
            lines = 0;

            while (GetBriefingData(GBD_PACKAGE_ELEMENT_NAME, lines, string,
                                   sizeof(string)) not_eq -1)
            {
                renderer->TextLeft(-0.8f, v, string);

                if (GetBriefingData(GBD_PACKAGE_ELEMENT_TASK, lines, string,
                                    sizeof(string)) not_eq -1)
                    renderer->TextLeft(-0.1f, v, string);

                lines++;
                v -= LINE_HEIGHT;
            }
        }
    }

    VirtualDisplay::SetFont(oldFont);
}

// Artscout - 2026 (NAVAIDS page): flatten every airbase in the theater into a
// list the draw can walk without touching the campaign.
//
// Why a cached list at all: the only way to enumerate objectives is a grid
// iterator, and covering the whole 1024 km theater means visiting every
// objective there is -- several thousand of them, most of them bridges. That is
// nowhere near a per-frame cost, but the answer only changes when the ownship
// moves or a base changes hands, so a slow rebuild is plenty.
// Artscout - 2026: 270 becomes 27, and 0 reads as 36 the way the painted
// number on a runway does.
static int RunwayDesignator(int headingDeg)
{
    int designator = ((headingDeg % 360) + 5) / 10;

    if (designator == 0 or designator > 36)
    {
        designator = 36;
    }

    return designator;
}


void CPKneeView::BuildNavaidList(SimVehicleClass *platform)
{
    if (not platform or not gTacanList)
    {
        mNavaidCount = 0;
        return;
    }

    if (not mNavaids)
    {
        mNavaids = new NavaidEntry[MAX_NAVAIDS];
    }

    const float ownX = platform->XPos();
    const float ownY = platform->YPos();
    const Team ownTeam = platform->GetTeam();

    mNavaidCount = 0;

    // GridToSim(1024) is the whole theater: the grid iterator wants a radius in
    // feet about a centre, and half the map from the middle reaches every
    // corner. Centre on the ownship so that, if the list ever does overflow, what
    // survives is what is nearest -- which is what a divert page is for.
    VuGridIterator it(ObjProxList, ownX, ownY, (BIG_SCALAR)GridToSim(1024));

    for (Objective o = (Objective)it.GetFirst();
         o not_eq NULL and mNavaidCount < MAX_NAVAIDS;
         o = (Objective)it.GetNext())
    {
        if (o->GetType() not_eq TYPE_AIRBASE and
            o->GetType() not_eq TYPE_AIRSTRIP)
        {
            continue;
        }

        NavaidEntry *e = &mNavaids[mNavaidCount];
        memset(e, 0, sizeof(*e));

        o->GetName(e->name, sizeof(e->name) - 1, 0);
        e->name[sizeof(e->name) - 1] = 0;

        e->friendly = (GetTTRelations(o->GetTeam(), ownTeam) <= Neutral);

        // TACAN and ILS. A base without a station simply has no row in
        // stations.dat, and GetChannelFromVUID leaves the outputs alone -- hence
        // the zeroed struct above rather than trusting it to write.
        int channel = 0, range = 0, ttype = 0;
        TacanList::StationSet set = TacanList::X;
        TacanList::Domain domain = TacanList::AG;
        float ils = 0.0f;

        if (gTacanList->GetChannelFromVUID(o->Id(), &channel, &set, &domain,
                                           &range, &ttype, &ils))
        {
            e->channel = channel;
            e->band = (set == TacanList::Y) ? 'Y' : 'X';
            e->ils = ils;
        }

        // The runway heading lives in the point-header chain hanging off the
        // objective's CLASS, and objectives are never rotated -- TranslatePointData
        // only offsets -- so the class heading is the heading on the map.
        //
        // Each end of a runway is its own RunwayPt header carrying the reciprocal
        // in `data`, in whole degrees, and runwayNum says which physical strip it
        // belongs to. Take both ends of strip 0.
        e->runway = -1;
        e->runwayOpp = -1;

        ObjClassDataType *oc = o->GetObjectiveClassData();

        if (oc)
        {
            for (int rw = oc->PtDataIndex; rw;
                 rw = PtHeaderDataTable[rw].nextHeader)
            {
                if (PtHeaderDataTable[rw].type not_eq RunwayPt or
                    PtHeaderDataTable[rw].runwayNum not_eq 0)
                {
                    continue;
                }

                const short hdg = PtHeaderDataTable[rw].data;

                if (e->runway < 0)
                {
                    e->runway = hdg;
                }
                else if (hdg not_eq e->runway)
                {
                    e->runwayOpp = hdg;
                    break;
                }
            }

            // Keep the lower-numbered end first, the way a plate reads.
            if (e->runwayOpp >= 0 and e->runwayOpp < e->runway)
            {
                const short t = e->runway;
                e->runway = e->runwayOpp;
                e->runwayOpp = t;
            }
        }

        // Position, twice over. Bullseye is what gets read on the radio; range
        // from the ownship is what decides whether a base is worth diverting to,
        // and is the order the page is sorted in.
        // BearingToBullseyeDeg returns the bearing FROM the place TO the
        // bullseye, as a raw atan2 in -180..180. What gets read on the radio is
        // the other way round -- bullseye to the place -- so flip it, which is
        // exactly what the AWACS list in urefresh.cpp does with the same call.
        int brg = 180 + TheCampaign.BearingToBullseyeDeg(o->XPos(), o->YPos());

        while (brg < 0)
        {
            brg += 360;
        }

        e->bullsBearing = brg % 360;
        e->bullsRange = FloatToInt32(
            (float)TheCampaign.RangeToBullseyeFt(o->XPos(), o->YPos()) *
            FT_TO_NM);

        const float dx = o->XPos() - ownX;
        const float dy = o->YPos() - ownY;
        e->ownRange =
            FloatToInt32((float)sqrt(dx * dx + dy * dy) * FT_TO_NM);

        mNavaidCount++;
    }

    // Nearest first. An insertion sort looks lazy next to qsort, but the list is
    // ~90 entries in Korea and this runs once every few seconds.
    for (int i = 1; i < mNavaidCount; i++)
    {
        NavaidEntry tmp = mNavaids[i];
        int j = i - 1;

        while (j >= 0 and mNavaids[j].ownRange > tmp.ownRange)
        {
            mNavaids[j + 1] = mNavaids[j];
            j--;
        }

        mNavaids[j + 1] = tmp;
    }

    mNavaidBuilt = SimLibElapsedTime;
}


// Artscout - 2026: the NAVAIDS page itself. Same ink, font and paper as the
// other two text pages -- DrawMissionText has already set all three up.
void CPKneeView::DrawNavaids(Render2D *renderer, SimVehicleClass *platform)
{
    char line[128];

    // Artscout - 2026: this page is a TABLE, and the other two are prose, so it
    // is the one page whose font has to follow the content rather than the
    // cockpit's preference. There are exactly three sizes (6x4, 8x6, 10x7 --
    // index 3 is a different typeface, not a fourth size), so measure the header
    // at each and take the largest that fits. At 10x7 the row is about twice the
    // page wide, which is why the first cut ran off the edge.
    extern int g_nKneeNavaidFont;
    int font = g_nKneeNavaidFont;

    if (font < 0)
    {
        font = 0;

        for (int f = 2; f >= 0; f--)
        {
            VirtualDisplay::SetFont(f);

            if (renderer->TextWidth((char *)NAVAID_HEADER) <= NAVAID_WIDTH)
            {
                font = f;
                break;
            }
        }
    }

    VirtualDisplay::SetFont(font);

    // Only meaningful once the font is chosen.
    const float LINE_HEIGHT = renderer->TextHeight();
    float v = NAVAID_TOP - LINE_HEIGHT;

    // Rebuild on a slow tick. SimLibElapsedTime is milliseconds.
    if (not mNavaids or mNavaidCount == 0 or
        (SimLibElapsedTime - mNavaidBuilt) > NAVAID_REBUILD_MS)
    {
        BuildNavaidList(platform);
    }

    if (mNavaidCount == 0)
    {
        renderer->TextLeft(NAVAID_LEFT, v, "NO NAVAID DATA");
        return;
    }

    renderer->TextLeft(NAVAID_LEFT, v, (char *)NAVAID_HEADER);
    v -= LINE_HEIGHT;

    // How many rows fit. The page runs from just under the header down to the
    // bottom margin; anything past that would draw off the board.
    const int rows = FloatToInt32((v + 0.95f) / LINE_HEIGHT);

    for (int i = 0; i < mNavaidCount and i < rows; i++)
    {
        const NavaidEntry *e = &mNavaids[i];

        char tcn[8];

        if (e->channel)
        {
            sprintf(tcn, "%03d%c", e->channel, e->band);
        }
        else
        {
            strcpy(tcn, "   -");
        }

        char ils[8];

        if (e->ils > 0.0f)
        {
            sprintf(ils, "%6.2f", e->ils);
        }
        else
        {
            strcpy(ils, "     -");
        }

        char rwy[8];

        if (e->runway >= 0 and e->runwayOpp >= 0)
        {
            sprintf(rwy, "%02d/%02d", RunwayDesignator(e->runway),
                    RunwayDesignator(e->runwayOpp));
        }
        else if (e->runway >= 0)
        {
            sprintf(rwy, "   %02d", RunwayDesignator(e->runway));
        }
        else
        {
            strcpy(rwy, "    -");
        }

        // A hostile base is still worth knowing about -- it is where the SAMs
        // and the MiGs live -- but it must not read as somewhere to land.
        // Column widths match NAVAID_HEADER exactly; change one and change
        // both, or the fitting above measures the wrong thing.
        sprintf(line, "%-11.11s%c%4s %6s %5s %03d/%-3d%4d", e->name,
                e->friendly ? ' ' : '*', tcn, ils, rwy, e->bullsBearing,
                e->bullsRange, e->ownRange);

        renderer->TextLeft(NAVAID_LEFT, v, line);
        v -= LINE_HEIGHT;
    }

    if (v > -NAVAID_TOP + LINE_HEIGHT)
    {
        renderer->TextLeft(NAVAID_LEFT, v - LINE_HEIGHT * 0.5f,
                           "* HOSTILE   BULLS = BRG/RNG FROM BULLSEYE");
    }
}


void CPKneeView::UpdateMapDimensions(SimVehicleClass *platform)
{

    WayPointClass *wp;
    float x, y, z;
    float left, right, top, bottom;

    m_pixel2nmY = (TheMap.NorthEdge() - TheMap.SouthEdge()) * FT_TO_KM;
    m_pixel2nmX = (TheMap.EastEdge() - TheMap.WestEdge()) * FT_TO_KM;

    CImageFileMemory &mapImageFile = mpKneeBoard->GetImageFile();
    m_pixel2nmY /= mapImageFile.image.height;
    m_pixel2nmX /= mapImageFile.image.width;
    // Start with our current location
    top = bottom = platform->XPos();
    right = left = platform->YPos();

    // Walk the waypoints and get min/max info
    for (wp = platform->waypoint; wp; wp = wp->GetNextWP())
    {
        wp->GetLocation(&x, &y, &z);

        right = max(right, y);
        left = min(left, y);
        top = max(top, x);
        bottom = min(bottom, x);
    }

    // Add the position of the override waypoint (if any)
    ShiAssert(platform->GetCampaignObject());
    ShiAssert(platform->GetCampaignObject()->IsFlight());
    wp = ((FlightClass *)platform->GetCampaignObject())->GetOverrideWP();

    if (wp)
    {
        wp->GetLocation(&x, &y, &z);

        right = max(right, y);
        left = min(left, y);
        top = max(top, x);
        bottom = min(bottom, x);
    }

    // Now get the center of the map we want to display
    wsHcenter = (right + left) * 0.5f;
    wsVcenter = (top + bottom) * 0.5f;

    // Now figure out the minimum width and height we want (as a distance for center for now)
    wsHsize = (right - left) * 0.5f * (1.0f + BORDER_PERCENT);
    wsVsize = (top - bottom) * 0.5f * (1.0f + BORDER_PERCENT);

    if (wsHsize >= TheMap.EastEdge() - TheMap.WestEdge())
    {
        wsHsize = TheMap.EastEdge() - TheMap.WestEdge() -
                  1.0f; // -1 is for rounding safety...
    }

    if (wsVsize >= TheMap.NorthEdge() - TheMap.SouthEdge())
    {
        wsVsize = TheMap.NorthEdge() - TheMap.SouthEdge() -
                  1.0f; // -1 is for rounding safety...
    }

    // See how many source pixels we're talking about and round down to an even divisor of the dest pixels
    float hSourcePixels = wsHsize * 2.0f * FT_TO_KM / m_pixel2nmX;
    float vSourcePixels = wsVsize * 2.0f * FT_TO_KM / m_pixel2nmY;

    float hPixelMag = srcRect.right / hSourcePixels;
    float vPixelMag = srcRect.bottom / vSourcePixels;

    // Cap the pixel magnification at a reasonable level
    float mapPixels =
        (TheMap.NorthEdge() - TheMap.SouthEdge()) * FT_TO_KM / m_pixel2nmY;
    float drawPixels = (float)srcRect.bottom;
    float maxMag = drawPixels / mapPixels * KNEEBOARD_SMALLEST_MAP_FRACTION;
    pixelMag =
        FloatToInt32((float)floor(min(min(hPixelMag, vPixelMag), maxMag)));

    // Detect the case where the whole desired image won't fit on screen
    if (pixelMag < 1)
    {
        pixelMag = 1;

        // Recenter on the current position to ensure its visible
        wsHcenter = platform->YPos();
        wsVcenter = platform->XPos();
    }

    // Now readjust our world space dimensions to reflect what we'll actually draw
    wsHsize = 0.5f * srcRect.right / (float)pixelMag * m_pixel2nmX * KM_TO_FT;
    wsVsize = 0.5f * srcRect.bottom / (float)pixelMag * m_pixel2nmY * KM_TO_FT;

    // Finally shift the center point as necessary to ensure we won't try to draw off the edge
    if (wsHcenter - wsHsize <= TheMap.WestEdge())
    {
        wsHcenter = TheMap.WestEdge() + wsHsize +
                    0.5f; // +1/2 is for rounding safety...
    }

    if (wsHcenter + wsHsize >= TheMap.EastEdge())
    {
        wsHcenter = TheMap.EastEdge() - wsHsize -
                    0.5f; // -1/2 is for rounding safety...
    }

    if (wsVcenter - wsVsize <= TheMap.SouthEdge())
    {
        wsVcenter = TheMap.SouthEdge() + wsVsize +
                    0.5f; // +1/2 is for rounding safety...
    }

    if (wsVcenter + wsVsize >= TheMap.NorthEdge())
    {
        wsVcenter = TheMap.NorthEdge() - wsVsize -
                    0.5f; // -1/2 is for rounding safety...
    }
}


void CPKneeView::RenderMap(SimVehicleClass *platform)
{
    // Recompute what portion of the world map we need to draw
    UpdateMapDimensions(platform);

    // Copy the map image into the target buffer
    DrawMap();

    // OW FIXME: the following StartFrame() call will result in a call to IDirect3DDevice7::SetRenderTarget. We can't do this on the Voodoo 1 bitand 2 ;(
    DeviceManager::DDDriverInfo *pDI =
        FalconDisplay.devmgr.GetDriver(DisplayOptions.DispVideoDriver);

    if (not pDI->SupportsSRT())
        return;

    // Draw in the waypoints
    StartDraw();
    DrawWaypoints(this, platform);
    EndDraw();
}


void CPKneeView::DrawMap()
{
    //pmvstrm
    bool m_imageloaded = false;
    ShiAssert(m_imageloaded);

    // Lock the back buffer and fill it through the shared rasteriser. Pixel(ptr, row, 0) is
    // ptr + row * lPitch, so the stride is the buffer's own, not width * 4.
    UInt32 *ptr = (UInt32 *)mapImageBuffer->Lock();
    RasteriseMap(ptr, mapImageBuffer->targetStride(), false);
    mapImageBuffer->Unlock();
}


// Artscout - 2026: the body DrawMap used to inline, so the 2D cockpit surface and the 3D board's CPU
// page are filled by one piece of code. dstPitchBytes is in BYTES.
void CPKneeView::RasteriseMap(UInt32 *dst, int dstPitchBytes, bool forRtt)
{
    // Artscout - 2026: InvertRGBOrder MASKS OFF the alpha byte -- it keeps only bits 0..23 and swaps R
    // with B -- so every map pixel comes out with alpha 0. That is harmless for the 2D board, whose
    // ImageBuffer::Compose is a straight blit that ignores alpha, and fatal for the 3D one, whose
    // Render2DBitmap -> DrawBitmap2D draws with BLEND_ALPHA: the entire map was being composited fully
    // transparent while the out-of-range fill (0xff000000, alpha 255) was the only thing that drew.
    // Force opacity for the RTT path only, so the 2D board's bytes are untouched.
    const UInt32 rttAlpha = forRtt ? 0xff000000u : 0u;
    // And out-of-map should be PAPER on a kneeboard, not black -- the window routinely overhangs the
    // source image (the theater map is square, the page is not), so a black fill would letterbox it.
    const UInt32 outside = forRtt ? KNEE_PAGE_COLOR : 0xff000000u;
    if (mpKneeBoard == NULL or dst == NULL)
    {
        return;
    }

    //light factors
    float eLight[3], iLight[3]; //iLight is not used here...
    OTWDriver.pCockpitManager->ComputeLightFactors(eLight, iLight);

    //we load map palette and apply lighting to it
    CImageFileMemory &mapImageFile = mpKneeBoard->GetImageFile();
    DWORD *inColor = mapImageFile.image.palette;
    DWORD outColor[256];
    ApplyLightingToPalette(inColor, outColor, eLight[0], eLight[1], eLight[2]);

    //now we apply lighting to palette and copy only the portion we want from the map
    int w = mapImageFile.image.width;
    int h = mapImageFile.image.height;

    // Decide where to start in the source image
    // Artscout - 2026: the two scales were SWAPPED here -- the ROW (vertical) offset divided by
    // m_pixel2nmX (the horizontal km/pixel) and the COLUMN offset by m_pixel2nmY. Invisible on Korea,
    // whose map and theater are both square so the two are equal to six decimals (0.999737), and wrong
    // on any theater that is not.
    int srcRowInitOffset = (int)((TheMap.NorthEdge() - (wsVsize + wsVcenter)) *
                                 FT_TO_KM / m_pixel2nmY);
    int srcColInitOffset =
        (int)((wsHcenter - wsHsize) * FT_TO_KM / m_pixel2nmX);

    // Artscout - 2026: honour pixelMag. UpdateMapDimensions sizes the world window as
    // 0.5 * width / pixelMag * m_pixel2nm -- it ASSUMES the source is magnified by pixelMag on the way
    // in. This loop copied 1:1 and always had, so the page showed pixelMag times more world than the
    // route overlay was scaled for: the map could never line up with its own waypoints, and the window
    // overran the image edge and letterboxed the page. Nearest-neighbour is enough -- the source is an
    // 8-bit palettised map and pixelMag is a small integer (3 on Korea).
    const int mag = (pixelMag > 0) ? pixelMag : 1;

    // The source image may not have loaded (KneeBoard::LoadKneeImage tolerates a missing map).
    if (mapImageFile.image.image == NULL or inColor == NULL)
    {
        return;
    }

    //here we get a pointer to the initial position of the map(0,0)
    UInt8 *mapFirstPointer = mapImageFile.image.image;
    mapFirstPointer += 0; //srcRowInitOffset*w + srcColInitOffset;

    // Copy from map to kneeboard
    for (int dstRow = 0; dstRow < (dstRect.bottom - dstRect.top); dstRow++)
    {
        const int srcRow = srcRowInitOffset + dstRow / mag;
        const bool rowInside = (srcRow >= 0) and (srcRow < h);
        //find the first pointer of that row in the map
        UInt8 *rowFirstPointer = mapFirstPointer + srcRow * w;
        //first destination pointer
        UInt32 *dstPix = (UInt32 *)((UInt8 *)dst + dstRow * dstPitchBytes);

        for (int dstCol = 0; dstCol < (dstRect.right - dstRect.left); dstCol++)
        {
            const int srcCol = srcColInitOffset + dstCol / mag;

            if (not rowInside or (srcCol >= w) or (srcCol < 0))
            {
                //off the edge of the source map
                dstPix[0] = outside;
            }
            else
            {
                //this is destination pixel
                dstPix[0] =
                    InvertRGBOrder(outColor[rowFirstPointer[srcCol]]) bitor
                    rttAlpha;
            }

            dstPix++;
        }
    }

}

void CPKneeView::DrawWaypoints(Render2D *renderer, SimVehicleClass *platform)
{
    WayPointClass *wp = NULL;
    BOOL isFirst = TRUE;
    float x1 = 0.0F, y1 = 0.0F, x2 = 0.0F, y2 = 0.0F;

    if (not renderer or not platform)
        return;

    DWORD color = OTWDriver.pCockpitManager->ApplyLighting(WP_COLOR, false);
    //OTWDriver.renderer->SetColor(color);
    renderer->SetColor(color);

    // Draw in the waypoints
    for (wp = platform->waypoint; wp; wp = wp->GetNextWP())
    {

        // Get the display coordinates of this waypoint
        MapWaypointToDisplay(wp, &x1, &y1);

        // Draw the waypoint marker and the connecting line if this isn't the first one
        renderer->Circle(x1, y1, WP_SIZE);

        if (not isFirst)
        {
            renderer->Line(x1, y1, x2, y2);
        }

        // Step to the next waypoint
        x2 = x1;
        y2 = y1;
        isFirst = FALSE;
    }

    // Draw the override waypoint marker (if any)
    // Artscout - 2026: these two were assert-only, so a release build walked straight into the cast --
    // and outside a campaign flight (dogfight, instant action) there is no flight object to cast. The
    // 2D board has always run this; the 3D board now runs it too, so make the check real.
    if (not platform->GetCampaignObject() or
        not platform->GetCampaignObject()->IsFlight())
    {
        return;
    }

    wp = ((FlightClass *)platform->GetCampaignObject())->GetOverrideWP();

    if (wp)
    {

        // Get the display coordinates of this waypoint
        MapWaypointToDisplay(wp, &x1, &y1);
        x2 = x1 + ORIDE_WP_SIZE;
        y2 = y1 + ORIDE_WP_SIZE;
        x1 = x1 - ORIDE_WP_SIZE;
        y1 = y1 - ORIDE_WP_SIZE;

        // Draw the waypoint marker
        renderer->Line(x1, y1, x2, y1);
        renderer->Line(x2, y1, x2, y2);
        renderer->Line(x2, y2, x1, y2);
        renderer->Line(x1, y2, x1, y1);
    }
}


void CPKneeView::DrawCurrentPosition(ImageBuffer *targetBuffer,
                                     Render2D *renderer,
                                     SimVehicleClass *platform)
{
    const float aspect = (float)srcRect.right / (float)srcRect.bottom;
    float h, v;
    mlTrig trig;
    static const struct
    {
        float x, y;
    } pos[] = {
        0.0f,  1.0f, // nose
        0.0f,  -1.0f, // tail
        -1.0f, -0.4f, // left wing tip
        1.0f,  -0.4f, // right wing tip
        0.0f,  0.3f, // leading edge at fuselage
        -0.5f, -1.0f, // left stab
        -0.5f, -1.0f, // right stab
    };
    static const int numPoints = sizeof(pos) / sizeof(pos[0]);
    float x[numPoints];
    float y[numPoints];

    // Convert our position into display space within the destination rect
    v = (platform->XPos() - wsVcenter) / wsVsize;
    h = (platform->YPos() - wsHcenter) / wsHsize;

    if (fabs(v) > 0.95f)
    {
        if (v > 0.95f)
        {
            v = 0.95f;
        }
        else
        {
            v = -0.95f;
        }

        if (vuxRealTime bitand 0x200)
        {
            // Don't draw to implement a flashing icon when the real postion is off screen.
            return;
        }
    }

    if (fabs(h) > 0.95f)
    {
        if (h > 0.95f)
        {
            h = 0.95f;
        }
        else
        {
            h = -0.95f;
        }

        if (vuxRealTime bitand 0x200)
        {
            // Don't draw to implement a flashing icon when the real postion is off screen.
            return;
        }
    }

    // Setup local coordinates for the symbol drawing
    renderer->CenterOriginInViewport();
    renderer->AdjustOriginInViewport(h, v);

    // Transform all our verts
    mlSinCos(&trig, platform->Yaw());
    trig.sin *= AC_SIZE;
    trig.cos *= AC_SIZE;

    for (int i = numPoints - 1; i >= 0; i--)
    {
        x[i] = (pos[i].x * trig.cos + pos[i].y * trig.sin);
        y[i] = (-pos[i].x * trig.sin + pos[i].y * trig.cos) * aspect;
    }

    // Draw the aircraft symbol
    DWORD acColor = OTWDriver.pCockpitManager->ApplyLighting(AC_COLOR, false);
    renderer->SetColor(acColor);
    renderer->Line(x[0], y[0], x[1], y[1]); // Body
    renderer->Line(x[5], y[5], x[6], y[6]); // Tail
    renderer->Tri(x[2], y[2], x[3], y[3], x[4], y[4]); // Wing

    // Draw a ring around the aircraft symbol to highlight it
    DWORD rColor = OTWDriver.pCockpitManager->ApplyLighting(0xFF00FF00, false);
    renderer->SetColor(rColor);
    renderer->Circle(0.0f, 0.0f, 1.5f * AC_SIZE);
}


void CPKneeView::MapWaypointToDisplay(WayPointClass *pwaypoint, float *h,
                                      float *v)
{
    float wpX;
    float wpY;
    float wpZ;

    pwaypoint->GetLocation(&wpX, &wpY, &wpZ);

    // Return values are in screen space, while the waypoint location is in FreeFalcon (X North, Y East)
    *v = (wpX - wsVcenter) / wsVsize;
    *h = (wpY - wsHcenter) / wsHsize;
}
