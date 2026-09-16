#ifndef _SCALEBITMAP_UI_H_
#define _SCALEBITMAP_UI_H_

class C_ScaleBitmap : public C_Base
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
    // Save from Here
    long ImageID_;
    long DefaultFlags_;
    BOOL UseOverlay_;

    // Don't Save from here down
    short Rows_[800];
    short Cols_[800];
    WORD r_shift_;
    WORD g_shift_;
    WORD b_shift_;

    BYTE *Overlay_;
    WORD *Palette_[16];
    O_Output *Image_;

    // Artscout - 2026: optional higher-resolution stand-in for whatever the source rect
    // currently shows. Set, Draw() uses these instead of the base image and overlay;
    // GetW(), GetH() and GetImage() keep reporting the base image either way, so an
    // owner's coordinate system never sees it. Not owned here -- the caller that builds
    // them keeps them alive.
    IMAGE_RSC *Detail_;
    BYTE *DetailOverlay_;
    long *DetailRows_;
    long *DetailCols_;

public:
    C_ScaleBitmap();
    C_ScaleBitmap(char **stream);
    C_ScaleBitmap(FILE *fp);
    ~C_ScaleBitmap();
    long Size();
    void Save(char **)
    {
        ;
    }
    void Save(FILE *)
    {
        ;
    }

    // Initialization Function
    void Setup(long ID, short Type, long ImageID);
    void InitOverlay();
    BYTE *GetOverlay()
    {
        return (Overlay_);
    }
    void UseOverlay()
    {
        if (Overlay_)
            UseOverlay_ = TRUE;
    }
    void NoOverlay()
    {
        UseOverlay_ = FALSE;
    }
    BOOL OverlayInUse()
    {
        return (Overlay_ and UseOverlay_);
    }
    void ClearOverlay();
    // Artscout - 2026: the detail stand-in. image/overlay/rows/cols must all be sized
    // together (see O_Output::Blend4BitDetail); passing anything NULL is the same as
    // ClearDetail(). The image must use the base image's palette, since the blended
    // palettes Draw() picks from are derived from that one.
    void SetDetail(IMAGE_RSC *image, BYTE *overlay, long *rows, long *cols);
    void ClearDetail();
    void PreparePalette(COLORREF color);
    void SetImage(long ID);
    void SetImage(IMAGE_RSC *image);
    void SetSrcRect(UI95_RECT *rect)
    {
        if (Image_ not_eq NULL)
            Image_->SetSrcRect(rect);
    }
    void SetDestRect(UI95_RECT *rect)
    {
        if (Image_ not_eq NULL)
            Image_->SetDestRect(rect);
    }
    void SetScaleInfo(long scale)
    {
        if (Image_ not_eq NULL)
            Image_->SetScaleInfo(scale);
    }
    UI95_RECT *GetSrcRect()
    {
        if (Image_ not_eq NULL)
            return (Image_->GetSrcRect());

        return (NULL);
    }
    UI95_RECT *GetDestRect()
    {
        if (Image_ not_eq NULL)
            return (Image_->GetDestRect());

        return (NULL);
    }

    IMAGE_RSC *GetImage(void)
    {
        if (Image_ not_eq NULL)
            return (Image_->GetImage());

        return (NULL);
    }
    // Free Function
    void Cleanup(void);
    void SetDefaultFlags()
    {
        SetFlags(DefaultFlags_);
    }
    long GetDefaultFlags()
    {
        return (DefaultFlags_);
    }
    void SetFlags(long flags);

    void Refresh();
    void Draw(SCREEN *surface, UI95_RECT *cliprect);

#ifdef _UI95_PARSER_

    short LocalFind(char *token);
    void LocalFunction(short ID, long P[], _TCHAR *, C_Handler *);
    void SaveText(HANDLE, C_Parser *)
    {
        ;
    }

#endif // PARSER
};

#endif
