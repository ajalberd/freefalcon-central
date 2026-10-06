// ====================
// Campaign Terrain ADT
// ====================

#ifndef CAMPTERR
#define CAMPTERR

#include "cmpglobl.h"
#include "campcell.h"

// ---------------------------------------
// Type and External Function Declarations
// ---------------------------------------

#define GroundCoverMask 0x0F // 0xF0
#define GroundCoverShift 0
#define ReliefMask 0x30 // 0xCF
#define ReliefShift 4
#define RoadMask 0x40 // 0xBF
#define RoadShift 6
#define RailMask 0x80 // 0x7F
#define RailShift 7

typedef Int16 GridIndex;

typedef struct gridloctype
{
    GridIndex x;
    GridIndex y;
} GridLocation;

extern short Map_Max_X; // World Size, in grid coordinates
extern short Map_Max_Y;

extern void InitTheaterTerrain(void);

extern void FreeTheaterTerrain(void);

extern int LoadTheaterTerrain(char* FileName);

extern int LoadTheaterTerrainLight(char* name);

extern int SaveTheaterTerrain(char* FileName);

extern CellData GetCell(GridIndex x, GridIndex y);

extern ReliefType GetRelief(GridIndex x, GridIndex y);

extern CoverType GetCover(GridIndex x, GridIndex y);
// Artscout - 2026: water a ship can use -- the cover cell is Water and, with g_bNavalSeaMask, the theater's
// sea mask (<theater>.SEA, tools/terrain/make_sea_mask.py) does not mark it as land in the 3D terrain
extern int ShipWater(GridIndex x, GridIndex y);
extern int SeaMaskLand(GridIndex x, GridIndex y);
extern int gSeaMaskSuspend; // set while a ship's route is retried on the cover grid alone

extern char GetRoad(GridIndex x, GridIndex y);

extern char GetRail(GridIndex x, GridIndex y);

#endif
