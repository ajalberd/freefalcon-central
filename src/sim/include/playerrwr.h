/***************************************************************************\
    PlayerRWR.h

    This provides the special abilities required for a player's RWR.  Most
 of the added functionality involves Display and Mode control.
\***************************************************************************/
#ifndef _PLAYERRWR_H_
#define _PLAYERRWR_H_

#include "vehrwr.h"

class SimBaseClass;

class PlayerRwrClass : public VehRwrClass
{
public:
    PlayerRwrClass(int idx, SimMoverClass *self);
    virtual ~PlayerRwrClass(void);

    virtual SimObjectType *Exec(SimObjectType *targetList);
    virtual void Display(VirtualDisplay *activeDisplay);
    virtual BOOL IsOn();

    void SetGridVisible(BOOL flag)
    {
        mGridVisible = flag;
    };

    // State Access Functions
    int HasActivity(void)
    {
        return detectionList[0].entity not_eq NULL;
    };
    int LaunchIndication(void)
    {
        return missileActivity;
    };

    //MI EWS function
    void CheckEWS(void);
    bool InEWSLoop;
    bool SaidJammer;
    int ReleaseManual;
    bool LaunchDetected;
    bool ChaffCheck;
    bool FlareCheck;

    // Artscout - 2026: CMS consent (TO 1F-16CMAM-34-1-1 2.7.1.1, 2.7.2.1). One state for every ECM system and the
    // CMDS: CMS aft gives it (airborne), CMS right removes it; the MODE knob never moves. AUTO dispenses only with it,
    // SEMI runs the program once on CMS aft when a dispense is warranted, the ECM transmits only with it. The RF switch
    // leaving NORM, or landing, drops it; coming back to NORM does not restore it. Auto dispensing also stops with the
    // gear down.
    bool CmsConsent;
    int CmsRfSeen; // RF switch position last frame, -1 = not yet seen
    unsigned long CounterTime; // vuxRealTime of the last "COUNTER" request (a dispense was warranted)
    bool DispenseWarranted(void);
    void GiveCmsConsent(void);
    void RemoveCmsConsent(const char* why);
    void UpdateCmsConsent(void);

    int IsPriority(void)
    {
        return priorityMode;
    };
    int TargetSep(void)
    {
        return targetSep;
    };
    int ShowUnknowns(void)
    {
        return showUnknowns;
    };
    int ShowNaval(void)
    {
        return showNaval;
    };
    int ShowSearch(void)
    {
        return showSearch;
    };
    int ShowLowAltPriority(void)
    {
        return lowAltPriority;
    };
    int IsFiltered(FalconEntity *);
    int ManualSelect(void);
    int LightSearch(void);
    int LightUnknowns(void);

    virtual void TogglePriority(void)
    {
        priorityMode = not priorityMode;
    };
    virtual void ToggleTargetSep(void);
    virtual void ToggleUnknowns(void)
    {
        showUnknowns = not showUnknowns;
    };
    virtual void ToggleNaval(void)
    {
        showNaval = not showNaval;
    };
    virtual void ToggleSearch(void)
    {
        showSearch = not showSearch;
    };
    virtual void SelectNextEmitter(void);
    virtual void ToggleLowAltPriority(void)
    {
        lowAltPriority = not lowAltPriority;
    };
    virtual void ToggleAutoDrop(void) {};

    // Artscout - 2026: SYS TEST button -- the ALR-56M self-test, two screens
    // (TO 1F-16CMAM-34-1-1 2.7.3.2.2). Needs the RWR powered.
    void StartSysTest(void);
    int InSysTest(void)
    {
        return sysTestStart not_eq 0;
    };

protected:
    unsigned long sysTestStart; // vuxRealTime the self-test began; 0 = not running
    void DrawSysTest(void);

    int flipFactor; // -1 when AC is upside down, 1 when normal
    int mGridVisible;

    // Button States
    int priorityMode;
    int targetSep;
    int showUnknowns;
    int showNaval;
    int showSearch;
    int missileActivity;
    int newGuy;

    // Select low or high altitude priority
    virtual void AutoSelectAltitudePriority(void);

    // Output Functions
    void DrawStatusSymbol(int);
    void DrawGrid(void);
    void DrawContact(DetectListElement *record);

    void DoAudio(DetectListElement *record);
    void DoAudio(); // JB 010718
};

#endif // _PLAYERRWR_H_
