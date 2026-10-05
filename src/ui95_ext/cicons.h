#ifndef _MAPICON_H_
#define _MAPICON_H_

#define ICON_LABEL (30)
#define NUM_DIRECTIONS (8)

class ARC_REC
{
#ifdef USE_SH_POOLS
public:
    // Overload new/delete to use a SmartHeap pool
    void *operator new(size_t size)
    {
        return MemAllocPtr(UI_Pools[UI_CONTROL_POOL], size, FALSE);
    };
    void operator delete(void *mem)
    {
        if (mem)
            MemFreePtr(mem);
    };
#endif
public:
    short arc;
    float range;
};

typedef struct
{
    short numarcs;
    ARC_REC *arcs;
} ARC_LIST;

typedef struct
{
    ARC_LIST *LowRadar;

    float LowSam;
    float HighSam;
    float HighRadar;
} DETECTOR;

class C_MapIcon;

class MAPICONLIST
{
#ifdef USE_SH_POOLS
public:
    // Overload new/delete to use a SmartHeap pool
    void *operator new(size_t size)
    {
        return MemAllocPtr(UI_Pools[UI_CONTROL_POOL], size, FALSE);
    };
    void operator delete(void *mem)
    {
        if (mem)
            MemFreePtr(mem);
    };
#endif
public:
    long ID;
    long Type;
    long Flags;
    long state;
    short x, y;
    float worldx, worldy;
    long Status;
    long Ghost; // Artscout - 2026: 1 = drawn dimmed at its last known position (an enemy ship nobody sees now)
    long ImageID;
    short Dragable;
    DETECTOR *Detect;
    C_MapIcon *Owner;
    O_Output *Icon;
    O_Output *Div;
    O_Output *Brig;
    O_Output *Bat;
    O_Output *Label;
};

// Artscout - 2026: if set, builds the hover tooltip for a map icon from its entity (returns a
// string id, or 0 to fall back to the icon's name label). Installed by the campaign UI.
extern long (*gMapIconTipHook)(long iconID);

class C_MapIcon : public C_Control
{
#ifdef USE_SH_POOLS
public:
    // Overload new/delete to use a SmartHeap pool
    void *operator new(size_t size)
    {
        return MemAllocPtr(UI_Pools[UI_CONTROL_POOL], size, FALSE);
    };
    void operator delete(void *mem)
    {
        if (mem)
            MemFreePtr(mem);
    };
#endif
private:
    long Font_;
    short Team_;
    float scale_;
    short ShowCircles_;
    long LastTime_, CurTime_;
    long DefaultFlags_;
    C_Hash *Root_;
    MAPICONLIST *Last_;
    MAPICONLIST *OverLast_;
    C_Resmgr *Icons_[NUM_DIRECTIONS][2];
    bool ShadeByStatus_; // Artscout - 2026: darken each icon by its Status (objective health)

public:
    enum
    {
        LOW_SAM = 0x0001,
        HIGH_SAM = 0x0002,
        LOW_RADAR = 0x0004,
        HIGH_RADAR = 0x0008,
    };

    C_MapIcon();
    C_MapIcon(char **stream);
    C_MapIcon(FILE *fp);
    ~C_MapIcon();
    long Size();
    void Save(char **)
    {
        ;
    }
    void Save(FILE *)
    {
        ;
    }

    // Setup Functions
    void Setup(long ID, short Type);
    void Cleanup(void);
    void SetMainImage(short idx, long OffID, long OnID);
    void SetMainImage(long OffID, long OnID);
    void SetDefaultFlags()
    {
        SetFlags(DefaultFlags_);
    }
    // Artscout - 2026: objective icon sets darken with damage (Status = objective status 0..100).
    void SetShadeByStatus(bool on)
    {
        ShadeByStatus_ = on;
    }
    long GetDefaultFlags()
    {
        return (DefaultFlags_);
    }

    void ShowCircles(short val)
    {
        ShowCircles_ = val;
    }
    short GetShowCircles()
    {
        return (ShowCircles_);
    }

    MAPICONLIST *AddIconToList(long CampID, short type, long ImageID, float x,
                               float y, short Dragable, _TCHAR *str, long DivID,
                               long BrigID, long BatID, long newstatus,
                               long newstate, DETECTOR *detptr);
    MAPICONLIST *AddIconToList(long CampID, short type, long ImageID, float x,
                               float y, short Dragable, _TCHAR *str, long DivID,
                               long BrigID, long BatID, long newstatus,
                               long newstate)
    {
        return (AddIconToList(CampID, type, ImageID, x, y, Dragable, str, DivID,
                              BrigID, BatID, newstatus, newstate, NULL));
    }
    MAPICONLIST *AddIconToList(long CampID, short type, long ImageID, float x,
                               float y, short Dragable, _TCHAR *str,
                               long newstatus, long newstate)
    {
        return (AddIconToList(CampID, type, ImageID, x, y, Dragable, str, 0, 0,
                              0, newstatus, newstate, NULL));
    }
    BOOL UpdateInfo(long ID, float x, float y, long newstatus, long newstate);
    BOOL UpdateInfo(MAPICONLIST *icon, float x, float y, long newstatus,
                    long newstate);
    void RemoveIcon(long CampID);
    BOOL ShowByType(long mask);
    BOOL HideByType(long mask);
    void Show();
    void Hide();
    void SetScaleFactor(float scale);
    MAPICONLIST *FindID(long ID);
    MAPICONLIST *GetLastItem()
    {
        return (Last_);
    }
    void SetLabel(long ID, _TCHAR *txt);
    void SetColor(long ID, COLORREF color);
    _TCHAR *GetLabel(long ID);
    void SetTextOffset(long ID, short x, short y);
    long GetIconID()
    {
        if (Last_)
            return (Last_->ID);

        return (0);
    }
    void Refresh(MAPICONLIST *icon);
    void RepaintIcon(MAPICONLIST *icon); // Artscout - 2026: Refresh under the UI lock, for state changes that do not move the icon

    // Use SetMainImage... and when done, call this to update the IMAGE_RSC pointers
    void RemapIconImages();

    C_Hash *GetRoot()
    {
        return (Root_);
    }

    void SetTeam(short team)
    {
        Team_ = team;
    }
    short GetTeam()
    {
        return (Team_);
    }

    void SetFont(long ID)
    {
        Font_ = ID;
    }
    long GetFont()
    {
        return (Font_);
    }

    long GetHelpText();

    // Handler/Window Functions
    long CheckHotSpots(long relX, long relY);
    BOOL Dragable(long)
    {
        return (GetFlags() bitand C_BIT_DRAGABLE);
    }
    BOOL Process(long ID, short ButtonHitType);
    BOOL MouseOver(long relX, long relY, C_Base *);
    void GetItemXY(long ID, long *x, long *y);
    BOOL Drag(GRABBER *Drag, WORD MouseX, WORD MouseY, C_Window *over);
    BOOL Drop(GRABBER *, WORD, WORD, C_Window *)
    {
        return FALSE;
    }
    void Refresh();
    void Draw(SCREEN *surface, UI95_RECT *cliprect);
};

#endif
