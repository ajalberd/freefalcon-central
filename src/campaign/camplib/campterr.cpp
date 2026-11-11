#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <vector>
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

// Artscout - 2026 (g_bLandPieces, off = stock): label the cover grid's land masses once per theater load.
// Planning treated "the unit's nearest objective is the target" as zero cost, so a battalion on an islet
// (Taiwan save2: an air-defence battalion off the east coast, a China battalion on a 2-cell-wide peninsula)
// was handed the nearest objective across the water and failed the grid path every tick. Ground planning
// now refuses a target on another piece.
bool g_bLandPieces = true;
static unsigned short *TheaterPieces = NULL;

static void FreeLandPieces(void)
{
    delete[] TheaterPieces;
    TheaterPieces = NULL;
}

static void BuildLandPieces(void)
{
    const int w = Map_Max_X, h = Map_Max_Y;

    if (not TheaterCells or w <= 0 or h <= 0)
        return;

    TheaterPieces = new unsigned short[w * h];
    memset(TheaterPieces, 0, sizeof(unsigned short) * w * h);

    std::vector<int> stack;
    int next = 1;

    for (int start = 0; start < w * h; start++)
    {
        if (TheaterPieces[start])
            continue;

        const int sx = start / h, sy = start % h;

        if (GetCover(sx, sy) == Water and not GetRoad(sx, sy))
            continue;

        const unsigned short label = (unsigned short)(next < 65535 ? next++ : 65535);
        TheaterPieces[start] = label;
        stack.push_back(start);

        while (not stack.empty())
        {
            const int c = stack.back();
            stack.pop_back();
            const int cx = c / h, cy = c % h;

            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    const int nx = cx + dx, ny = cy + dy;

                    if (nx < 0 or nx >= w or ny < 0 or ny >= h or TheaterPieces[nx * h + ny])
                        continue;

                    if (GetCover(nx, ny) == Water and not GetRoad(nx, ny))
                        continue;

                    TheaterPieces[nx * h + ny] = label;
                    stack.push_back(nx * h + ny);
                }
        }
    }
}

int LandPiece(GridIndex x, GridIndex y)
{
    if (x < 0 or x >= Map_Max_X or y < 0 or y >= Map_Max_Y)
        return 0;

    if (not TheaterPieces)
        BuildLandPieces();

    // a water cell with a road bit is a bridge or port, not land to stand on
    if (not TheaterPieces or GetCover(x, y) == Water)
        return 0;

    return TheaterPieces[x * Map_Max_Y + y];
}

// The piece a unit or objective belongs to: its own cell's, or for a cell the grid calls open water (a battalion
// saved on a coastal water cell -- Taiwan's air-defence battalion off Taitung -- or a port) the first piece found
// within 3 cells, as the unstick-from-water step would put it.
static int NearPiece(GridIndex x, GridIndex y)
{
    if (x < 0 or x >= Map_Max_X or y < 0 or y >= Map_Max_Y)
        return 0;

    if (not TheaterPieces)
        BuildLandPieces();

    if (not TheaterPieces)
        return 0;

    if (TheaterPieces[x * Map_Max_Y + y])
        return TheaterPieces[x * Map_Max_Y + y];

    for (int r = 1; r <= 3; r++)
        for (int iy = -r; iy <= r; iy++)
            for (int ix = -r; ix <= r; ix++)
            {
                if (abs(ix) != r and abs(iy) != r)
                    continue;

                const int nx = x + ix, ny = y + iy;

                if (nx >= 0 and nx < Map_Max_X and ny >= 0 and ny < Map_Max_Y and TheaterPieces[nx * Map_Max_Y + ny])
                    return TheaterPieces[nx * Map_Max_Y + ny];
            }

    return 0;
}

int GroundSeparated(GridIndex x, GridIndex y, GridIndex xx, GridIndex yy)
{
    if (not g_bLandPieces)
        return 0;

    const int a = NearPiece(x, y), b = NearPiece(xx, yy);

    return a and b and a != b;
}

void FreeTheaterTerrain(void)
{
    if (TheaterCells)
        delete[] TheaterCells;

    TheaterCells = NULL;
    FreeSeaMask();
    FreeLandPieces();
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
