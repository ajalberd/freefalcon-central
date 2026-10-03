#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "cmpglobl.h"
#include "listadt.h"
#include "f4vu.h"
#include "vutypes.h"
#include "objectiv.h"
#include "strategy.h"
#include "unit.h"
#include "find.h"
#include "path.h"
#include "campaign.h"
#include "update.h"
#include "f4vu.h"
#include "camplist.h"
#include "gtm.h"
#include "team.h"

#ifndef SUPPLY_H
#define SUPPLY_H

#define SUPPLY_PT_FUEL                                                         \
    10000 // How many lbs of fuel each point of supply fuel is worth

// ==================
// Supply functions
// ==================

extern int ProduceSupplies(CampaignTime delta);

extern int SupplyUnits(Team who, CampaignTime delta);

// CAMPSIM DIAGNOSTIC counters (gSupplyDiag[team][...], cumulative since load). Read only by tools/campsim.
enum
{
    SUPDIAG_PROD_SUPPLY,    // supply points added to the pool by production (after the per-day formula)
    SUPDIAG_PROD_FUEL,      // fuel points added by refineries
    SUPDIAG_PROD_REPL,      // replacement vehicles added
    SUPDIAG_SENT_SUPPLY,    // supply drawn from the pool for units
    SUPDIAG_GOT_SUPPLY,     // supply that reached them after road losses
    SUPDIAG_SENT_FUEL,
    SUPDIAG_GOT_FUEL,
    SUPDIAG_RESUPPLIES,     // unit resupply events with something to send
    SUPDIAG_LOST_ALL,       // ... where nothing arrived (no path, or all lost on the way)
    SUPDIAG_NO_SOURCE,      // ... where no supply source / friendly objective was found
    SUPDIAG_REPL_GROUND,    // replacement vehicles given to battalions
    SUPDIAG_REPL_AIR,       // replacement aircraft given to squadrons
    SUPDIAG_LAST
};
extern int gSupplyDiag[NUM_TEAMS][SUPDIAG_LAST];
extern int gSupplyRatio[NUM_TEAMS][3]; // supply, fuel, replacement distribution ratio x1000

#endif
