#ifndef _BITMAP_UI_H_
#define _BITMAP_UI_H_

class C_Bitmap : public C_Base
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
    long DefaultFlags_;

    O_Output *Image_;
    BOOL (*TimerCallback_)(C_Base *me);

public:
    C_Bitmap();
    C_Bitmap(char **stream);
    C_Bitmap(FILE *fp);
    ~C_Bitmap();
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
    void SetImage(long ID);
    void SetImage(IMAGE_RSC *image);
    // Artscout - 2026: stretch this bitmap's box to w x h and draw the image to fill it
    // (O_Output::COVER_*), for full-screen backgrounds on a surface larger than the art.
    void SetCover(short mode, long w, long h)
    {
        if (not Image_)
            return;

        Image_->SetCover(mode);
        Image_->SetWH(w, h);
        SetWH(w, h);
    }
    short GetCover()
    {
        return Image_ ? Image_->GetCover() : 0;
    }
    // After an image change: a covering bitmap keeps its box, any other takes the image's size.
    void SizeToImage()
    {
        if (Image_->GetCover())
            Image_->SetWH(GetW(), GetH());
        else
            SetWH(Image_->GetW(), Image_->GetH());
    }
    IMAGE_RSC *GetImage(void)
    {
        if (Image_)
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

    BOOL TimerUpdate();
    void SetTimerCallback(BOOL (*cb)(C_Base *me))
    {
        TimerCallback_ = cb;
    }

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
