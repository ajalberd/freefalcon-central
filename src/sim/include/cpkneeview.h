#ifndef _CPKNEEVIEW_H
#define _CPKNEEVIEW_H

//#include <windows.h>

#include "cpobject.h"
#include "graphics/include/image.h"

#ifdef USE_SH_POOLS
extern MEM_POOL gCockMemPool;
#endif


//====================================================//
// CPLight Class Definition
//====================================================//

class CPKneeView : public CPObject, Render2D
{
#ifdef USE_SH_POOLS
public:
    // Overload new/delete to use a SmartHeap pool
    void *operator new(size_t size)
    {
        return MemAllocPtr(gCockMemPool, size, FALSE);
    };
    void operator delete(void *mem)
    {
        if (mem)
            MemFreePtr(mem);
    };
#endif
public:
    // kneeboards are shared among all kneeviews
    KneeBoard *mpKneeBoard;

    //====================================================//
    // Runtime Member Functions
    //====================================================//

    virtual void Exec(SimBaseClass *);
    virtual void DisplayBlit(void);
    virtual void DisplayDraw(void);
    virtual void Refresh(SimVehicleClass *platform);

    virtual void Setup(DisplayDevice *device, int top, int left, int bottom,
                       int right);
    virtual void Cleanup(void);

    // Artscout - 2026 (3D kneeboard): the same view, mounted on an RTT canvas in the 3D pit instead of
    // blitted into the 2D cockpit art. The 2D path owns a video-memory ImageBuffer and composes it into
    // the panel; there is no panel in the 3D pit, so this variant rasterises the map into a plain CPU
    // buffer and hands it to the canvas's atlas zone via Render2DBitmap. Everything downstream of that --
    // the map window maths, the route overlay, both text pages -- is the code the 2D board already runs.
    // The MOUNT (canvas, atlas zone, hotspot) is deliberately kept apart from the PAGE SOURCE so the page
    // model can be replaced later without disturbing the part that has been proven in the headset.
    void SetupRtt(KneeBoard *board, int w, int h);
    // Refresh the page's off-screen content. Cheap unless the page is MAP.
    void ExecRtt(SimVehicleClass *platform);
    // Draw the current page into canvas's atlas zone. Must be inside the RTT pass.
    void DisplayRtt(Render2D *canvas, SimVehicleClass *platform);

    //====================================================//
    // Constructors and Destructors
    //====================================================//

    CPKneeView(ObjectInitStr *, KneeBoard *);
    // Artscout - 2026: the RTT variant. CPObject is pure assignment from the init struct, so a zeroed
    // one (unit scales, no callback slot) is all the base needs -- this instance is never in
    // CockpitManager's object list and is never Exec'd by the 2D panel cycle.
    CPKneeView(KneeBoard *board, int w, int h);
    virtual ~CPKneeView();

    // sfr: moved some information from kneeboard to kneeview to allow
    // multiple kneeview in the same pit
private:
    RECT srcRect;
    RECT dstRect;

    // image buffer from kneeboard map
    ImageBuffer *mapImageBuffer;
    // Artscout - 2026 (3D kneeboard): RTT variant -- no ImageBuffer, a CPU page instead.
    bool mRtt;
    UInt32 *mRttPixels;
    int mRttW, mRttH;
    // The map window is derived from the flight's waypoint bounding box, so it is effectively CONSTANT
    // for a mission -- re-running the palette-lookup blit every frame would burn a megapixel for nothing.
    // Re-rasterise when the window moves, and on a slow tick so a dusk/dawn lighting change still lands.
    float mRttMapV, mRttMapH, mRttMapVS, mRttMapHS;
    int mRttTick;
    // Real world map dimensions (R/O externally)
    float wsVcenter; //vertical center
    float wsHcenter; //horizontal center
    float wsHsize; //horizontal size
    float wsVsize; //vertical size
    int pixelMag;
    float m_pixel2nmX, m_pixel2nmY;

    // Internal worker functions for the text page
    void DrawMissionText(Render2D *renderer, SimVehicleClass *platform);

    // Artscout - 2026 (NAVAIDS page): one airbase, flattened out of the campaign
    // so that drawing it does no lookups at all.
    struct NavaidEntry
    {
        char name[24];
        int channel; // TACAN channel, 0 when the base has none
        char band; // 'X' or 'Y'
        float ils; // ILS localiser frequency in MHz, 0 when none
        // The primary runway, both ends. The point data stores each end as its
        // own header with the reciprocal heading, in whole degrees. Not `ltrt`:
        // that is which side the traffic pattern is flown, not an L/R
        // designator -- the painted number lives in texIdx and is a texture.
        short runway; // heading of one end, -1 if the base has no runway data
        short runwayOpp; // the other end, -1 if only one was found
        int bullsBearing; // degrees from the theater bullseye
        int bullsRange; // nautical miles from the bullseye
        int ownRange; // nautical miles from the ownship, at build time
        bool friendly;
    };

    // Walking the objective list is far too expensive to do per frame, and the
    // answer only moves when the ownship does. Rebuilt on a slow tick while the
    // page is up, and not at all while it is not.
    NavaidEntry *mNavaids;
    int mNavaidCount;
    unsigned long mNavaidBuilt;

    void BuildNavaidList(SimVehicleClass *platform);
    void DrawNavaids(Render2D *renderer, SimVehicleClass *platform);

    // Internal worker funtions for the map page
    void RenderMap(SimVehicleClass *platform);
    void UpdateMapDimensions(SimVehicleClass *platform);
    void DrawMap();
    // Artscout - 2026: the pixel loop DrawMap used to inline, so the 2D surface and the 3D CPU page are
    // filled by one piece of code. Pitch is in BYTES (the ImageBuffer's stride is not width * 4).
    // forRtt: force alpha opaque and fill out-of-map with paper. InvertRGBOrder drops the alpha
    // byte, which the 2D board's Compose ignores but the 3D board's alpha-blended blit does not.
    void RasteriseMap(UInt32 *dst, int dstPitchBytes, bool forRtt);
    // Artscout - 2026: was drawing through CPKneeView's own Render2D base, which the RTT instance does
    // not set up. Takes the target explicitly now; the 2D caller passes `this` as before.
    void DrawWaypoints(Render2D *renderer, SimVehicleClass *platform);
    void DrawCurrentPosition(ImageBuffer *targetBuffer, Render2D *renderer,
                             SimVehicleClass *platform);
    void MapWaypointToDisplay(WayPointClass *curWaypoint, float *x, float *y);
    // void ApplyLighting(DWORD *inColor, DWORD *outColor);
};

#endif
