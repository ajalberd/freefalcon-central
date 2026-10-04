/**********************************************************************************
*
* cmpevent.cpp
*
* Campaign event manager
*
***********************************************************************************/

#include <io.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "falclib.h"
#include "cmpglobl.h"
#include "cmpevent.h"
#include "team.h"
#include "atm.h"
#include "f4find.h"
#include "cuievent.h"
#include "campaign.h"
#include "cmpclass.h"
#include "find.h"
#include "debuggr.h"
#include "team.h"
#include "brief.h"
#include "gtmobj.h"
#include "falcsess.h"
#include "dispcfg.h"
#include "falcuser.h"
#include "playerop.h"
#include "msginc/campeventdatamsg.h"

//sfr: for checks
#include "invalidbufferexception.h"

#include "debuggr.h"

EventClass** CampEvents = NULL;
short CE_Events = 0;

// ============================
// Event history
// ============================
// Which events fired and which news clips played, and when. The .evt member only keeps one
// flag per event; the history is appended after that table (magic, count, entries) so older
// readers, which stop after the flags, still load the file. Used to rebuild the News Report
// when a save is loaded and by the campaign editor.
#define EVT_LOG_MAX 64
#define EVT_LOG_MAGIC 0x32545645 // "EVT2"

struct EventLogEntry
{
    short kind; // 0 = event fired, 1 = news clip played
    short id; // event number, or movie id for kind 1
    unsigned int time; // campaign time, ms
};

static EventLogEntry EventLog[EVT_LOG_MAX];
static int EventLogCount = 0;

void EventLogAdd(int kind, int id, unsigned long time)
{
    for (int i = 0; i < EventLogCount; i++)
        if (EventLog[i].kind == kind and EventLog[i].id == id and EventLog[i].time == (unsigned int)time)
            return;

    if (EventLogCount >= EVT_LOG_MAX)
        return;

    EventLog[EventLogCount].kind = (short)kind;
    EventLog[EventLogCount].id = (short)id;
    EventLog[EventLogCount].time = (unsigned int)time;
    EventLogCount++;
}

int EventLogSize(void)
{
    return EventLogCount;
}

int EventLogGet(int i, int* kind, int* id, unsigned long* time)
{
    if (i < 0 or i >= EventLogCount)
        return 0;

    *kind = EventLog[i].kind;
    *id = EventLog[i].id;
    *time = EventLog[i].time;
    return 1;
}

// ============================
// Condition history
// ============================
// Which branch of the script did it. When an action that changes the war runs (an event fires
// for the first time, a movie plays, relations change, the game ends), every #IF enclosing it is
// recorded: its line in the .tri, whether it was taken through its #IF or its #ELSE, and the values
// it compared (supply %, both sides' strength, the roll, ...). Two branches of one script often
// do the same thing -- China joins on low supply OR on a lost air war -- and the event flag alone
// cannot tell them apart. Saved as a second trailer after the event history; readers that do not
// know it stop before it.
#define COND_LOG_MAX 256
#define COND_LOG_MAGIC 0x31444E43 // "CND1"
#define COND_NO_VALUE ((int)0x80000000)

struct CondLogEntry
{
    int line; // line of the #IF in the .tri, from 1
    short branch; // 0 = its condition held, 1 = its #ELSE branch
    short depth; // nesting level, 1 = outermost
    unsigned int time; // campaign time, ms
    int a, b; // what the condition measured, COND_NO_VALUE if nothing
};

static CondLogEntry CondLog[COND_LOG_MAX];
static int CondLogCount = 0;

static void CondLogAdd(int line, int branch, int depth, unsigned long time, int a, int b)
{
    for (int i = 0; i < CondLogCount; i++)
        if (CondLog[i].line == line and CondLog[i].branch == branch and CondLog[i].time == (unsigned int)time)
            return;

    if (CondLogCount >= COND_LOG_MAX)
        return;

    CondLogEntry& e = CondLog[CondLogCount++];
    e.line = line;
    e.branch = (short)branch;
    e.depth = (short)depth;
    e.time = (unsigned int)time;
    e.a = a;
    e.b = b;
}

int CondLogSize(void)
{
    return CondLogCount;
}

int CondLogGet(int i, int* line, int* branch, int* depth, unsigned long* time, int* a, int* b)
{
    if (i < 0 or i >= CondLogCount)
        return 0;

    *line = CondLog[i].line;
    *branch = CondLog[i].branch;
    *depth = CondLog[i].depth;
    *time = CondLog[i].time;
    *a = CondLog[i].a;
    *b = CondLog[i].b;
    return 1;
}

// The script reader's ReadComments + ReadToken (brief.cpp) in one, counting lines so a condition
// can be named by where it is: skips blank lines and lines starting with '/', then returns the next
// line, at most len - 1 characters, without its line end. *line is that line's number.
static void ReadScriptLine(FILE* fp, char* token, int len, int* line, int* nextLine)
{
    char buffer[256];
    int skipping = 0;

    token[0] = 0;

    while (fgets(buffer, sizeof(buffer), fp))
    {
        int start = *nextLine;
        int whole = strchr(buffer, '\n') not_eq NULL;

        if (whole)
            (*nextLine)++;

        if (skipping)
        {
            skipping = not whole;
            continue;
        }

        if (buffer[0] == '\n')
            continue;

        if (buffer[0] == '/')
        {
            skipping = not whole;
            continue;
        }

        *line = start;
        strncpy(token, buffer, len);
        token[len - 1] = 0;

        char* sptr = strchr(token, '\n');

        if (sptr)
            *sptr = 0;

        if ((sptr = strchr(token, '\r')) not_eq NULL)
            *sptr = 0;

        // A line longer than the buffer continues on the next read, as it did with ReadToken.
        return;
    }
}

#define CE_MAX_TRIGGERED 3

// ============================
// External Function Prototypes
// ============================

extern void UI_AddMovieToList(long ID, long timestamp, _TCHAR* Description);
extern void ReadComments(FILE* fh);
extern char* ReadToken(FILE* fp, char name[], int len);
extern char* ReadMemToken(char** data, char name[], int len);

// ================================
// External variables 2002-04-17 MN
// ================================

extern float FLOTDrawDistance;
extern int FLOTSortDirection;
extern int TheaterXPosition;
extern int TheaterYPosition;

// ============================
// Other prototypes
// ============================

int ReadScriptedTriggerFile(char* scenario);

// =========================================
// Event Class
// =========================================

EventClass::EventClass(short id)
{
    event = id;
    flags = 0;
}

EventClass::EventClass(FILE* file)
{
    if (not file)
        return;

    fread(&event, sizeof(short), 1, file);
    fread(&flags, sizeof(short), 1, file);
}

EventClass::EventClass(uchar** stream, long* rem)
{
    // Artscout - 2026 (Linux port): `rem` is a pointer; `rem <= 0` was a null check (a valid pointer
    // is never <= 0). clang rejects the ordered pointer/int comparison, so spell it as a null test.
    if ((not rem) or (not stream))
    {
        return;
    }

    memcpychk(&event, stream, sizeof(short), rem);
    memcpychk(&flags, stream, sizeof(short), rem);
}

EventClass::~EventClass(void)
{
}

int EventClass::Save(FILE* file)
{
    if (not file)
        return 0;

    fwrite(&event, sizeof(short), 1, file);
    fwrite(&flags, sizeof(short), 1, file);
    return 1;
}

void EventClass::DoEvent(void)
{
    SetEvent(1);
    MonoPrint("CampEvent: Event %d activated.\n", event);
}

void EventClass::SetEvent(int status)
{
    CampEventDataMessage* msg =
        new CampEventDataMessage(vuLocalSession, FalconLocalGame);
    msg->dataBlock.message = CampEventDataMessage::eventMessage;
    msg->dataBlock.event = event;

    if (status)
    {
        if (not (flags bitand CE_FIRED))
            EventLogAdd(0, event, TheCampaign.CurrentTime);

        flags or_eq CE_FIRED;
        msg->dataBlock.status = 1;
    }
    else
    {
        flags and_eq compl CE_FIRED;
        msg->dataBlock.status = 0;
    }

    FalconSendMessage(msg, TRUE);
}


// ======================================
// Global functions
// ======================================

int CheckTriggers(char* scenario)
{
    if (not FalconLocalGame or not FalconLocalGame->IsLocal())
        return 0;

    if (FalconLocalSession->GetTeam() == 255)
        return 0;

    ReadScriptedTriggerFile(scenario);
    return 0;
}

int ReadNumberOfEvents(char* scenario)
{
    char token[121];
    int done = 0;
    FILE* fp;

    CE_Events = 0;

    if ((fp = OpenCampFile(scenario, "tri", "r")) == NULL)
        return 0;

    while (not done)
    {
        ReadComments(fp);
        ReadToken(fp, token, 120);

        // #104: these parse loops only terminated on a specific end-token (#ENDINIT / #END...). A missing or
        // unmatched end-token (e.g. a CRLF '\r' left on it on Linux, or a truncated file) made ReadToken return
        // stale/empty at EOF forever -> campaign-entry hang (main thread spinning in ReadComments/fgetc).
        // Terminate at end-of-file regardless.
        if (feof(fp))
            break;

        if (not token[0])
            continue;

        if (strncmp(token, "#TOTAL_EVENTS", 13) == 0)
        {
            char* sptr = strchr(token, ' ');

            if (sptr)
                sptr++;

            CE_Events = atoi(sptr);
            done = 1;
        }
    }

    CloseCampFile(fp);

    if (CampEvents not_eq NULL)
        delete[] CampEvents;

    CampEvents = new EventClass*[CE_Events];
    return CE_Events;
}

void SetInitialEvents(char* scenario)
{
    FILE* fp;
    char token[121];
    int done = 0;

    if ((fp = OpenCampFile(scenario, "tri", "r")) == NULL)
        return;

    while (not done)
    {
        ReadComments(fp);
        ReadToken(fp, token, 120);

        // #104: these parse loops only terminated on a specific end-token (#ENDINIT / #END...). A missing or
        // unmatched end-token (e.g. a CRLF '\r' left on it on Linux, or a truncated file) made ReadToken return
        // stale/empty at EOF forever -> campaign-entry hang (main thread spinning in ReadComments/fgetc).
        // Terminate at end-of-file regardless.
        if (feof(fp))
            break;

        if (not token[0])
            continue;

        if (strncmp(token, "#SET_EVENT", 10) == 0)
        {
            char* sptr = strchr(token, ' ');
            int i = 0;

            if (sptr)
                sptr++;

            i = atoi(sptr);
            CampEvents[i]->SetEvent(1);
        }
        else if (strncmp(token, "#RESET_EVENT", 10) == 0)
        {
            char* sptr = strchr(token, ' ');
            int i = 0;

            if (sptr)
                sptr++;

            i = atoi(sptr);
            CampEvents[i]->SetEvent(0);
        }
        else if (strncmp(token, "#SET_TEMPO", 10) == 0)
        {
            /* char *sptr;
             if (sptr = strchr(token,' '))
             sptr++;
             TheCampaign.Tempo = atoi (sptr);
            */
        }
        else if (strncmp(token, "#CHANGE_PRIORITIES", 18) == 0)
        {
            /* char *sptr;
             int team,i;
             if (sptr = strchr(token,' '))
             sptr++;
             team = atoi(sptr);
             if (sptr = strchr(sptr,' '))
             sptr++;
             i = atoi(sptr);
             if (TeamInfo[team])
             TeamInfo[team]->ReadPriorityFile(i);
            */
        }

        if (strcmp(token, "#ENDINIT") == 0)
            done = 1;
    }

    CloseCampFile(fp);
}


void ReadSpecialCampaignData(char* scenario)
{
    FILE* fp;
    char token[121];
    int done = 0;

    // Initialise variables to default values
    TheaterXPosition = Map_Max_X / 2;
    TheaterYPosition = Map_Max_Y / 2;
    FLOTSortDirection = 0;
    FLOTDrawDistance = 50.0f;

    if ((fp = OpenCampFile(scenario, "tri", "r")) == NULL)
        return;

    while (not done)
    {
        ReadComments(fp);
        ReadToken(fp, token, 120);

        // #104: these parse loops only terminated on a specific end-token (#ENDINIT / #END...). A missing or
        // unmatched end-token (e.g. a CRLF '\r' left on it on Linux, or a truncated file) made ReadToken return
        // stale/empty at EOF forever -> campaign-entry hang (main thread spinning in ReadComments/fgetc).
        // Terminate at end-of-file regardless.
        if (feof(fp))
            break;

        if (not token[0])
            continue;

        // 2002-04-17 MN these have originally been read from Falcon4.AII - but now from trigger files so we can
        // set them individually for each campaign :-)
        if (strncmp(token, "#BULLSEYE_X", 11) ==
            0) // bullseye reference point X position
        {
            char* sptr = strchr(token, ' ');
            int i = 0;

            if (sptr)
                sptr++;

            i = atoi(sptr);
            TheaterXPosition = i;
        }
        else if (strncmp(token, "#BULLSEYE_Y", 11) ==
                 0) // bullseye reference point Y position
        {
            char* sptr = strchr(token, ' ');
            int i = 0;

            if (sptr)
                sptr++;

            i = atoi(sptr);
            TheaterYPosition = i;
        }
        else if (strncmp(token, "#FLOT_SORTDIRECTION", 19) ==
                 0) // 0 = West-East, 1 = North-South
        {
            char* sptr = strchr(token, ' ');
            int i = 0;

            if (sptr)
                sptr++;

            i = atoi(sptr);
            FLOTSortDirection = i;
        }
        else if (strncmp(token, "#FLOT_DRAWDISTANCE", 11) ==
                 0) // bullseye reference point X position
        {
            char* sptr = strchr(token, ' ');
            float i = 0.0f;

            if (sptr)
                sptr++;

            i = (float)atof(sptr);
            FLOTDrawDistance = i;
        }

        if (strcmp(token, "#ENDINIT") == 0)
            done = 1;
    }

    CloseCampFile(fp);
}


int NewCampaignEvents(char* scenario)
{
    EventLogCount = 0;
    CondLogCount = 0;

    // Read in and allocate the event database
    ReadNumberOfEvents(scenario);

    for (int i = 0; i < CE_Events; i++)
        CampEvents[i] = new EventClass(i);

    // Set any events we want initially flagged
    SetInitialEvents(scenario);
    return 1;
}

// Load both events and triggers
int LoadCampaignEvents(char* filename, char* scenario)
{
    uchar /* *data,*/* data_ptr;
    short i, events;

    ReadNumberOfEvents(scenario);
    EventLogCount = 0;
    CondLogCount = 0;
    CampaignData cd = ReadCampFile(filename, "evt");

    if (cd.dataSize == -1)
    {
        return 0;
    }

    data_ptr = (uchar*)cd.data;

    events = *((short*)data_ptr);
    short savedEvents = events;
    uchar* historyAt = data_ptr + sizeof(short) + 4 * (savedEvents > 0 ? savedEvents : 0);
    long historyLeft = cd.dataSize - (long)(historyAt - (uchar*)cd.data);
    data_ptr += sizeof(short);
    long dataSize = cd.dataSize - sizeof(short);

    if (events > CE_Events)
        events = CE_Events;

    for (i = 0; i < events; i++)
        CampEvents[i] = new EventClass(&data_ptr, &dataSize);

    for (; i < CE_Events; i++)
        CampEvents[i] = new EventClass(i);

    // History trailer (absent in older saves)
    if (historyLeft >= 6 and *((int*)historyAt) == EVT_LOG_MAGIC)
    {
        short n = *((short*)(historyAt + 4));
        EventLogEntry* e = (EventLogEntry*)(historyAt + 6);

        for (int k = 0; k < n and k < EVT_LOG_MAX and 6 + (k + 1) * (long)sizeof(EventLogEntry) <= historyLeft; k++)
            EventLogAdd(e[k].kind, e[k].id, e[k].time);

        // Condition trailer (absent in saves from before it existed)
        long condAt = 6 + (n > 0 ? n : 0) * (long)sizeof(EventLogEntry);
        uchar* c = historyAt + condAt;

        if (historyLeft - condAt >= 6 and *((int*)c) == COND_LOG_MAGIC)
        {
            short m = *((short*)(c + 4));
            CondLogEntry* ce = (CondLogEntry*)(c + 6);

            for (int k = 0; k < m and condAt + 6 + (k + 1) * (long)sizeof(CondLogEntry) <= historyLeft; k++)
                CondLogAdd(ce[k].line, ce[k].branch, ce[k].depth, ce[k].time, ce[k].a, ce[k].b);
        }
    }

    delete cd.data;
    return 1;
}

// Save both events and triggers
int SaveCampaignEvents(char* filename)
{
    FILE* fp;
    int i;

    if ((fp = OpenCampFile(filename, "evt", "wb")) == NULL)
        return 0;

    if (CampEvents)
    {
        fwrite(&CE_Events, sizeof(short), 1, fp);

        for (i = 0; i < CE_Events; i++)
            CampEvents[i]->Save(fp);

        int magic = EVT_LOG_MAGIC;
        short n = (short)EventLogCount;
        fwrite(&magic, sizeof(int), 1, fp);
        fwrite(&n, sizeof(short), 1, fp);
        fwrite(EventLog, sizeof(EventLogEntry), EventLogCount, fp);

        int cmagic = COND_LOG_MAGIC;
        short m = (short)CondLogCount;
        fwrite(&cmagic, sizeof(int), 1, fp);
        fwrite(&m, sizeof(short), 1, fp);
        fwrite(CondLog, sizeof(CondLogEntry), CondLogCount, fp);
    }
    else
    {
        CE_Events = 0;
        fwrite(&CE_Events, sizeof(short), 1, fp);
    }

    CloseCampFile(fp);
    return 1;
}

void DisposeCampaignEvents(void)
{
    int i;

    if (not CampEvents or not CE_Events)
        return;

    for (i = 0; i < CE_Events; i++)
        delete CampEvents[i];
    ;

    delete[] CampEvents;
    CampEvents = NULL;
}


int ReadScriptedTriggerFile(char* filename)
{
    FILE* fp;
    int i, done = 0, initdone = 0, curr_stack = 0,
           stack_active[MAX_STACK] = {1};
    char token[128], *sptr;
    _TCHAR eol[2] = {'\n', 0};
    Objective o;
    Team team;

    // For the condition history: the #IF that opened each stack level, whether we are in its
    // #ELSE, and what it measured.
    int line = 0, nextLine = 1;
    int condLine[MAX_STACK + 1] = {0}, condElse[MAX_STACK + 1] = {0};
    int condA[MAX_STACK + 1], condB[MAX_STACK + 1];

    // Records the #IF chain above the action about to run.
    auto logChain = [&]()
    {
        for (int k = 1; k <= curr_stack and k <= MAX_STACK; k++)
            if (condLine[k])
                CondLogAdd(condLine[k], condElse[k], k, TheCampaign.CurrentTime, condA[k], condB[k]);
    };

    if ((fp = OpenCampFile(filename, "tri", "r")) == NULL)
    {
        ShiAssert(0);
        return 0;
    }

    ShiAssert(CE_Events > 0);

    // Read # of events (the first line, whatever it is, as ReadToken did)
    {
        char first[256];

        if (fgets(first, sizeof(first), fp) and strchr(first, '\n'))
            nextLine++;
    }

    while (not done)
    {
        ReadScriptLine(fp, token, 120, &line, &nextLine);

        // #104: these parse loops only terminated on a specific end-token (#ENDINIT / #END...). A missing or
        // unmatched end-token (e.g. a CRLF '\r' left on it on Linux, or a truncated file) made ReadToken return
        // stale/empty at EOF forever -> campaign-entry hang (main thread spinning in ReadComments/fgetc).
        // Terminate at end-of-file regardless.
        if (feof(fp))
            break;

        if (not token[0])
            continue;

        // Check for still in init section
        if (strcmp(token, "#ENDINIT") == 0)
            initdone = 1;
        else if (strcmp(token, "#ENDSCRIPT") == 0)
        {
            done = 1;
            continue;
        }

        if (not initdone)
            continue;

        // Handle standard tokens
        if (strncmp(token, "#IF", 3) == 0)
        {
            curr_stack++;

            if (not stack_active[curr_stack - 1])
                stack_active[curr_stack] = 0;
            else
                stack_active[curr_stack] = 1;

            if (curr_stack <= MAX_STACK)
            {
                condLine[curr_stack] = line;
                condElse[curr_stack] = 0;
                condA[curr_stack] = condB[curr_stack] = COND_NO_VALUE;
            }
        }
        else if (strcmp(token, "#ELSE") == 0)
        {
            if (curr_stack > 0 and stack_active[curr_stack - 1])
                stack_active[curr_stack] = not stack_active[curr_stack];

            if (curr_stack > 0 and curr_stack <= MAX_STACK)
                condElse[curr_stack] = 1;

            continue;
        }
        else if (strcmp(token, "#ENDIF") == 0)
        {
            if (not curr_stack)
                MonoPrint("<script reading Error - unmatched #ENDIF>\n");
            else
                curr_stack--;

            continue;
        }

        // Check for section activity
        if (stack_active[curr_stack])
        {
            // This section is active, handle tokens
            if (strncmp(token, "#IF", 3) == 0)
            {
                if (curr_stack >= MAX_STACK)
                {
                    MonoPrint("<Brief Reading Error - stack overflow. Max "
                              "stacks = %d",
                              MAX_STACK);
                    CloseCampFile(fp);
                    return 0;
                }

                // Add all our if conditions here
                if (strncmp(token, "#IF_EVENT_PLAYED", 16) == 0)
                {
                    if (sptr = strchr(token, ' '))
                        sptr++;

                    i = atoi(sptr);

                    if (i > CE_Events or not CampEvents[i]->HasFired())
                        stack_active[curr_stack] = 0;
                    else
                        stack_active[curr_stack] = 1;

                    condA[curr_stack] = stack_active[curr_stack];
                }
                else if (strncmp(token, "#IF_MAIN_TARGET", 15) == 0)
                {
                    if (sptr = strchr(token, ' '))
                        sptr++;

                    if (*sptr == 'F')
                        team = FalconLocalSession->GetTeam();
                    else
                        team = GetEnemyTeam(FalconLocalSession->GetTeam());

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    i = atoi(sptr);

                    if (TeamInfo[team]->gtm->priorityObj == i)
                        stack_active[curr_stack] = 1;
                    else
                        stack_active[curr_stack] = 0;
                }
                else if (strncmp(token, "#IF_TROOPS_COMMITTED", 20) == 0)
                {
                    if (sptr = strchr(token, ' '))
                        sptr++;

                    if (*sptr == 'F')
                        team = FalconLocalSession->GetTeam();
                    else
                        team = GetEnemyTeam(FalconLocalSession->GetTeam());

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    i = atoi(sptr);
                    o = (Objective)GetEntityByCampID(i);

                    if (not o)
                        stack_active[curr_stack] = 0;
                    else
                    {
                        POData pod = GetPOData(o);

                        if (pod and pod->ground_assigned[team])
                            stack_active[curr_stack] = 1;
                        else
                            stack_active[curr_stack] = 0;
                    }
                }
                else if (strncmp(token, "#IF_CONTROLLED", 14) == 0)
                {
                    char type;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    team = atoi(sptr);

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    type = *sptr;

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    if (type == 'A')
                    {
                        // And logic
                        stack_active[curr_stack] = 1;

                        while (stack_active[curr_stack] and sptr and atoi(sptr))
                        {
                            o = (Objective)GetEntityByCampID(atoi(sptr));

                            if (o and o->GetTeam() not_eq team)
                            {
                                stack_active[curr_stack] = 0;
                                // the first one not held, and who holds it
                                condA[curr_stack] = atoi(sptr);
                                condB[curr_stack] = o->GetTeam();
                            }

                            if (sptr = strchr(sptr, ' '))
                                sptr++;
                        }
                    }
                    else
                    {
                        // Or logic
                        stack_active[curr_stack] = 0;

                        while (not stack_active[curr_stack] and sptr and
                               atoi(sptr))
                        {
                            o = (Objective)GetEntityByCampID(atoi(sptr));

                            if (o and o->GetTeam() == team)
                            {
                                stack_active[curr_stack] = 1;
                                // the one held
                                condA[curr_stack] = atoi(sptr);
                                condB[curr_stack] = team;
                            }

                            if (sptr = strchr(sptr, ' '))
                                sptr++;
                        }
                    }
                }
                else if (strncmp(token, "#IF_INITIATIVE", 14) == 0)
                {
                    stack_active[curr_stack] = 0;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    team = *sptr - '0';

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    condA[curr_stack] = TeamInfo[team]->GetInitiative();

                    if (*sptr == 'G')
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        i = atoi(sptr);

                        if (TeamInfo[team]->GetInitiative() >= i)
                            stack_active[curr_stack] = 1;
                    }
                    else
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        i = atoi(sptr);

                        if (TeamInfo[team]->GetInitiative() <= i)
                            stack_active[curr_stack] = 1;
                    }
                }
                else if (strncmp(token, "#IF_SUPPLY", 10) == 0)
                {
                    stack_active[curr_stack] = 0;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    team = *sptr - '0';

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    condA[curr_stack] = TeamInfo[team]->GetCurrentStats()->supplyLevel;

                    if (*sptr == 'G')
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        i = atoi(sptr);

                        if (TeamInfo[team]->GetCurrentStats()->supplyLevel >= i)
                            stack_active[curr_stack] = 1;
                    }
                    else
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        i = atoi(sptr);

                        if (TeamInfo[team]->GetCurrentStats()->supplyLevel <= i)
                            stack_active[curr_stack] = 1;
                    }
                }
                else if (strncmp(token, "#IF_PLAYER_DIFFICULTY", 21) == 0)
                {
                    stack_active[curr_stack] = 0;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    condA[curr_stack] = PlayerOptions.CampaignEnemyGroundExperience();

                    if (*sptr == 'G')
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        i = atoi(sptr);

                        if (PlayerOptions.CampaignEnemyGroundExperience() >= i)
                            stack_active[curr_stack] = 1;
                    }
                    else
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        i = atoi(sptr);

                        if (PlayerOptions.CampaignEnemyGroundExperience() <= i)
                            stack_active[curr_stack] = 1;
                    }
                }
                else if (strncmp(token, "#IF_PRI_CONTROLLED_LT", 21) == 0)
                {
                    int controlled = 0;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    if (*sptr == 'F')
                        team = FalconLocalSession->GetTeam();
                    else
                        team = GetEnemyTeam(FalconLocalSession->GetTeam());

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    i = atoi(sptr);
                    VuListIterator poit(POList);
                    o = GetFirstObjective(&poit);

                    while (o)
                    {
                        if (o->GetTeam() == team)
                            controlled++;

                        o = GetNextObjective(&poit);
                    }

                    if (controlled < i)
                        stack_active[curr_stack] = 1;
                    else
                        stack_active[curr_stack] = 0;

                    condA[curr_stack] = controlled;
                }
                else if (strncmp(token, "#IF_ON_OFFENSIVE", 16) == 0)
                {
                    if (sptr = strchr(token, ' '))
                        sptr++;

                    team = atoi(sptr);

                    if (TeamInfo[team] and TeamInfo[team]->GetGroundAction() and
                        TeamInfo[team]->GetGroundAction()->actionType ==
                            GACTION_OFFENSIVE)
                        stack_active[curr_stack] = 1;
                    else
                        stack_active[curr_stack] = 0;

                    /* int offensive_assigned = 0;
                     if (sptr = strchr(token,' '))
                     sptr++;
                     if (*sptr == 'F')
                     team = FalconLocalSession->GetTeam();
                     else
                     team = GetEnemyTeam(FalconLocalSession->GetTeam());
                     if (sptr = strchr(sptr,' '))
                     sptr++;
                     // Check if we have offensive units assigned
                     if (TeamInfo[team]->GetGroundAction()->actionType not_eq GACTION_OFFENSIVE)
                     stack_active[curr_stack] = 0;
                     else
                     stack_active[curr_stack] = 1;
                    */
                }
                else if (strncmp(token, "#IF_FORCE_RATIO", 15) == 0)
                {
                    Team opposite;
                    char type, func;
                    int os, ts, ratio;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    type = *sptr;

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    team = atoi(sptr);

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    opposite = atoi(sptr);

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    func = *sptr;

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    i = atoi(sptr);

                    switch (type)
                    {
                    case 'A':
                        os = TeamInfo[team]->GetCurrentStats()->aircraft;
                        ts = TeamInfo[opposite]->GetCurrentStats()->aircraft;
                        break;

                    case 'G':
                        os = TeamInfo[team]->GetCurrentStats()->groundVehs;
                        ts = TeamInfo[opposite]->GetCurrentStats()->groundVehs;
                        break;

                    case 'N':
                        os = TeamInfo[team]->GetCurrentStats()->ships;
                        ts = TeamInfo[opposite]->GetCurrentStats()->ships;
                        break;

                    default:
                        os = TeamInfo[team]->GetCurrentStats()->groundVehs +
                             TeamInfo[team]->GetCurrentStats()->aircraft;
                        ts = TeamInfo[opposite]->GetCurrentStats()->groundVehs +
                             TeamInfo[opposite]->GetCurrentStats()->aircraft;
                        break;
                    }

                    // The other side having none at all divided by zero; that is as large as a ratio gets.
                    ratio = ts > 0 ? os * 10 / ts : (os > 0 ? INT_MAX : 0);
                    stack_active[curr_stack] = 0;
                    condA[curr_stack] = os;
                    condB[curr_stack] = ts;

                    if (func == 'G' and ratio >= i)
                        stack_active[curr_stack] = 1;
                    else if (func == 'L' and ratio <= i)
                        stack_active[curr_stack] = 1;
                }
                else if (strncmp(token, "#IF_BORDOM_HOURS", 16) == 0)
                {
                    if (sptr = strchr(token, ' '))
                        sptr++;

                    if (((TheCampaign.CurrentTime -
                          TheCampaign.lastMajorEvent) /
                         CampaignHours) > static_cast<CampaignTime>(atoi(sptr)))
                        stack_active[curr_stack] = 1;
                    else
                        stack_active[curr_stack] = 0;

                    condA[curr_stack] = (int)((TheCampaign.CurrentTime - TheCampaign.lastMajorEvent) / CampaignHours);
                }
                else if (strncmp(token, "#IF_CAMPAIGN_DAY", 16) == 0)
                {
                    stack_active[curr_stack] = 0;
                    condA[curr_stack] = TheCampaign.GetCampaignDay();

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    if (*sptr == 'G')
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        if (TheCampaign.GetCampaignDay() >= atoi(sptr))
                            stack_active[curr_stack] = 1;
                    }
                    else
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        if (TheCampaign.GetCampaignDay() <= atoi(sptr))
                            stack_active[curr_stack] = 1;
                    }
                }
                else if (strncmp(token, "#IF_REINFORCEMENT", 16) == 0)
                {
                    stack_active[curr_stack] = 0;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    team = atoi(sptr);

                    if (sptr = strchr(sptr, ' '))
                        sptr++;

                    condA[curr_stack] = TeamInfo[team]->GetReinforcement();

                    if (*sptr == 'G')
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        if (TeamInfo[team]->GetReinforcement() >= atoi(sptr))
                            stack_active[curr_stack] = 1;
                    }
                    else
                    {
                        if (sptr = strchr(sptr, ' '))
                            sptr++;

                        if (TeamInfo[team]->GetReinforcement() <= atoi(sptr))
                            stack_active[curr_stack] = 1;
                    }
                }
                else if (strncmp(token, "#IF_RANDOM_CHANCE", 16) == 0)
                {
                    stack_active[curr_stack] = 0;

                    if (sptr = strchr(token, ' '))
                        sptr++;

                    condA[curr_stack] = rand() % 100;

                    if (condA[curr_stack] < atoi(sptr))
                        stack_active[curr_stack] = 1;
                }
                else
                    stack_active[curr_stack] = 0;

                continue;
            }

            // special tokens
            if (strncmp(token, "#PLAY_MOVIE", 11) == 0)
            {
                // _TCHAR str[128] = {0};
                CampEventDataMessage* msg =
                    new CampEventDataMessage(vuLocalSession, FalconLocalGame);

                if (sptr = strchr(token, ' '))
                    sptr++;

                i = atoi(sptr);
                logChain();
                // queue movie
                // AddIndexedStringToBuffer(1160+i-100,str);
                // UI_AddMovieToList(i,TheCampaign.CurrentTime,str);
                msg->dataBlock.message = CampEventDataMessage::playMovie;
                msg->dataBlock.event = i;
                FalconSendMessage(msg, TRUE);
                continue;
            }
            else if (strncmp(token, "#CHANGE_RELATIONS", 17) == 0)
            {
                int with, rel;

                if (sptr = strchr(token, ' '))
                    sptr++;

                team = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                with = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                rel = atoi(sptr);

                if (TeamInfo[team] and TeamInfo[with])
                {
                    logChain();
                    SetTTRelations(team, with, rel);

                    if (rel == Allied)
                    {
                        SetTeam(team, with);
                        TeamInfo[team]->flags and_eq compl TEAM_ACTIVE;
                    }
                }
            }
            else if (strncmp(token, "#DO_EVENT", 9) == 0)
            {
                if (sptr = strchr(token, ' '))
                    sptr++;

                i = atoi(sptr);

                if (i < CE_Events and i > 0)
                {
                    if (not CampEvents[i]->HasFired())
                        logChain();

                    CampEvents[i]->DoEvent();
                }

                continue;
            }
            else if (strncmp(token, "#SHIFT_INITIATIVE", 16) == 0)
            {
                int to, amount;

                if (sptr = strchr(token, ' '))
                    sptr++;

                team = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                to = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                amount = atoi(sptr);

                if (TeamInfo[team] and TeamInfo[to])
                    TransferInitiative(team, to, amount);

                continue;
            }
            else if (strncmp(token, "#RESET_EVENT", 12) == 0)
            {
                if (sptr = strchr(token, ' '))
                    sptr++;

                i = atoi(sptr);

                if (i < CE_Events and i > 0)
                    CampEvents[i]->SetEvent(0);

                continue;
            }
            else if (strncmp(token, "#END_GAME", 9) == 0)
            {
                if (sptr = strchr(token, ' '))
                    sptr++;

                logChain();
                // Post the campaign over message
                PostMessage(FalconDisplay.appWin, FM_CAMPAIGN_OVER, atoi(sptr),
                            1);
                continue;
            }
            else if (strcmp(token, "#RESET_BORDOM_TIMEOUT") == 0)
            {
                TheCampaign.lastMajorEvent = TheCampaign.CurrentTime;
                continue;
            }
            else if (strncmp(token, "#CHANGE_PRIORITIES", 18) == 0)
            {
                /* if (sptr = strchr(token,' '))
                 sptr++;
                 team = atoi(sptr);
                 if (sptr = strchr(sptr,' '))
                 sptr++;
                 i = atoi(sptr);
                 if (TeamInfo[team])
                 TeamInfo[team]->ReadPriorityFile(i);
                */
            }
            else if (strncmp(token, "#SET_MINIMUM_SUPPLIES", 18) == 0)
            {
                int s, f, r;

                if (sptr = strchr(token, ' '))
                    sptr++;

                team = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                s = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                f = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                r = atoi(sptr);

                if (TeamInfo[team]->GetSupplyAvail() < s)
                    TeamInfo[team]->SetSupplyAvail(s);

                if (TeamInfo[team]->GetFuelAvail() < f)
                    TeamInfo[team]->SetFuelAvail(f);

                if (TeamInfo[team]->GetReplacementsAvail() < r)
                    TeamInfo[team]->SetReplacementsAvail(r);
            }
            else if (strncmp(token, "#SET_PAK_PRIORITY", 17) == 0)
            {
                CampEntity e;
                POData pod;

                if (sptr = strchr(token, ' '))
                    sptr++;

                team = atoi(sptr);

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                e = (CampEntity)GetEntityByCampID(atoi(sptr));

                if (sptr = strchr(sptr, ' '))
                    sptr++;

                i = atoi(sptr);

                if (e and e->IsObjective())
                {
                    pod = GetPOData((Objective)e);
#ifdef DEBUG
                    ShiAssert(pod);

                    if (not pod)
                        continue;

#endif
                    pod->ground_priority[team] = pod->air_priority[team] = i;
                    // KCK: player_priority only used now if >= 0
                    // if ( not (pod->flags bitand GTMOBJ_PLAYER_SET_PRIORITY))
                    // pod->player_priority[team] =  i;
                    pod->flags or_eq GTMOBJ_SCRIPTED_PRIORITY;
                }
            }
            else if (strncmp(token, "#SET_TEMPO", 10) == 0)
            {
                /* if (sptr = strchr(token,' '))
                 sptr++;
                 TheCampaign.Tempo = atoi (sptr);
                */
            }
            else if (strncmp(token, "#TOTAL_EVENTS", 13) == 0 or
                     strncmp(token, "#SET", 4) == 0 or
                     strcmp(token, "#ENDINIT") == 0)
            {
                // KCK: For initialization only.
            }
            else
            {
                MonoPrint("CampEvent.cpp: Unrecognized token: %s\n", token);
            }

            // End active stack section
        }

        // End token handler
    }

    CloseCampFile(fp);
    return 1;
}
