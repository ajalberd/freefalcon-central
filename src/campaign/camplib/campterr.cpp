#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include "cmpglobl.h"
#include "campcell.h"
#include "campterr.h"
#include "f4find.h"
#include "entity.h"
#include "asearch.h"
#include "campaign.h"
//sfr: checks
#include "invalidbufferexception.h"

#ifdef DEBUG
#include "cmpclass.h"
#endif

// =============================================
// Campaign Terrain ADT - Private Implementation
// =============================================

CellDataType *TheaterCells = NULL;
unsigned char EastLongitude;
unsigned char SouthLatitude;
float Latitude;
float Longitude;
float CellSizeInKilometers;

short Map_Max_X = 0;
short Map_Max_Y = 0;

// -------------------------
// Local Function Prototypes
// =========================

// -------------------------
// External Function Prototypes
// =========================

// ---------------------------------
// Global Function (ADT) Definitions
// =================================

void InitTheaterTerrain(void)
{
    if (TheaterCells)
        FreeTheaterTerrain();

    TheaterCells = new CellDataType[Map_Max_X * Map_Max_Y];
    memset(TheaterCells, 0, sizeof(CellDataType) * Map_Max_X * Map_Max_Y);
}

// Artscout - 2026 (g_bNavalSeaMask): cells the 1 km cover grid calls water but the 3D terrain shows as land
// (<theater>.SEA, built by tools/terrain/make_sea_mask.py; same [x][y] order as TheaterCells, 1 byte a cell).
// Ships routed by the cover grid alone sailed over peninsulas and moored on airfields in 3D -- ship_debug.cam:
// the 65th Destroyer task force on the Kalma peninsula at Wonsan, a cell 14% water in 3D. No file, no mask.
static unsigned char *TheaterSeaMask = NULL;
int gSeaMaskSuspend = 0;

static void FreeSeaMask(void)
{
    delete[] TheaterSeaMask;
    TheaterSeaMask = NULL;
}

static void LoadSeaMask(char *name)
{
    FreeSeaMask();

    CampaignData cd = ReadCampFile(name, "sea");

    if (cd.dataSize == -1)
        return;

    short mx = 0, my = 0;

    if (cd.dataSize >= (long)(2 * sizeof(short) + Map_Max_X * Map_Max_Y)) // ReadCampFile adds a trailing 0
    {
        memcpy(&mx, cd.data, sizeof(short));
        memcpy(&my, cd.data + sizeof(short), sizeof(short));
    }

    if (mx == Map_Max_X and my == Map_Max_Y)
    {
        TheaterSeaMask = new unsigned char[Map_Max_X * Map_Max_Y];
        memcpy(TheaterSeaMask, cd.data + 2 * sizeof(short), Map_Max_X * Map_Max_Y);
    }

    delete cd.data;
}

int SeaMaskLand(GridIndex x, GridIndex y)
{
    extern bool g_bNavalSeaMask;

    if (not g_bNavalSeaMask or gSeaMaskSuspend or not TheaterSeaMask or x < 0 or x >= Map_Max_X or y < 0 or y >= Map_Max_Y)
        return 0;

    return TheaterSeaMask[x * Map_Max_Y + y];
}

int ShipWater(GridIndex x, GridIndex y)
{
    return GetCover(x, y) == Water and not SeaMaskLand(x, y);
}

void FreeTheaterTerrain(void)
{
    if (TheaterCells)
        delete[] TheaterCells;

    TheaterCells = NULL;
    FreeSeaMask();
}

int LoadTheaterTerrain(char *name)
{
    //char *data, *data_ptr;

    FreeTheaterTerrain();

    CampaignData cd = ReadCampFile(name, "thr");

    if (cd.dataSize == -1)
    {
        return 0;
    }

    long rem = cd.dataSize;
    VU_BYTE *data_ptr = (VU_BYTE *)cd.data;

    memcpychk(&Map_Max_X, &data_ptr, sizeof(short), &rem);
    memcpychk(&Map_Max_Y, &data_ptr, sizeof(short), &rem);

#ifdef DEBUG
    ShiAssert(Map_Max_X == TheCampaign.TheaterSizeX);
    ShiAssert(Map_Max_Y == TheCampaign.TheaterSizeY);
#endif

    InitTheaterTerrain();

    memcpychk(TheaterCells, &data_ptr,
              sizeof(CellDataType) * Map_Max_X * Map_Max_Y, &rem);

    delete cd.data;

    LoadSeaMask(name);

    return 1;
}

int LoadTheaterTerrainLight(char *name)
{
    FILE *fp;

    FreeTheaterTerrain();

    if ((fp = OpenCampFile(name, "thr", "rb")) == NULL)
        return 0;

    fread(&Map_Max_X, sizeof(short), 1, fp);
    fread(&Map_Max_Y, sizeof(short), 1, fp);
    CloseCampFile(fp);
    return 1;
}

int SaveTheaterTerrain(char *name)
{
    FILE *fp;

    if (not TheaterCells)
        return 0;

    if ((fp = OpenCampFile(name, "thr", "wb")) == NULL)
        return 0;

    fwrite(&Map_Max_X, sizeof(short), 1, fp);
    fwrite(&Map_Max_Y, sizeof(short), 1, fp);
    fwrite(TheaterCells, sizeof(CellDataType), Map_Max_X * Map_Max_Y, fp);
    CloseCampFile(fp);
    return 1;
}

CellData GetCell(GridIndex x, GridIndex y)
{
    ShiAssert(x >= 0 and x < Map_Max_X and y >= 0 and y < Map_Max_Y);
    return &TheaterCells[x * Map_Max_Y + y];
}

ReliefType GetRelief(GridIndex x, GridIndex y)
{
    ShiAssert(x >= 0 and x < Map_Max_X and y >= 0 and y < Map_Max_Y);
    return (ReliefType)((TheaterCells[x * Map_Max_Y + y] bitand ReliefMask) >>
                        ReliefShift);
}

CoverType GetCover(GridIndex x, GridIndex y)
{
    if ((x < 0) or (x >= Map_Max_X) or (y < 0) or (y >= Map_Max_Y))
        return (CoverType)Water;
    else
        return (CoverType)((TheaterCells[x * Map_Max_Y + y] bitand
                            GroundCoverMask) >>
                           GroundCoverShift);
}

char GetRoad(GridIndex x, GridIndex y)
{
    ShiAssert(x >= 0 and x < Map_Max_X and y >= 0 and y < Map_Max_Y);
    return (char)((TheaterCells[x * Map_Max_Y + y] bitand RoadMask) >>
                  RoadShift);
}

char GetRail(GridIndex x, GridIndex y)
{
    ShiAssert(x >= 0 and x < Map_Max_X and y >= 0 and y < Map_Max_Y);
    return (char)((TheaterCells[x * Map_Max_Y + y] bitand RailMask) >>
                  RailShift);
}
