// Artscout - 2026: fit the stock 1024x768 ui95 layout to a larger surface. See UI-OVERHAUL.md.
//
// Every window in the .scf files was placed for a 1024x768 stage, often lined up with artwork painted
// into a full-screen background bitmap. Rather than re-author 141 files, each window is adapted once,
// straight after it is parsed, in one of two modes:
//
// STAGE (the default). The stock layout moves as one block to the middle of the surface, so windows
// stay lined up with each other and with the background art. What frames a screen reaches the edges:
// a window covering the whole stage grows to the whole surface, its background bitmap is drawn 1:1
// in the middle with black margins (O_Output::COVER_CENTER), its title and bottom bars stretch, and
// their buttons split -- left half stays left, right half pins right. Thin bar windows along the top
// or bottom edge do the same.
//
// EDGES, for the .scf files named in UiAdaptEdges (the campaign map screen). Each window and control
// is placed by where it sits on the stage, per axis:
//
//   touches both edges   -> fill: grows by the extra room   (shell, bars, the map)
//   touches one edge     -> pin:  follows that edge         (side panels, toolbars)
//   touches neither      -> centre: moves by half the extra room
//
// "Touches the top" counts the 32-px title bar as the edge and "touches the bottom" the 40-px bottom
// bar, so the map window between them fills. Only shapes that can stretch do (tiles, boxes, lines,
// fills, the map mover); a control bigger than its container (scrolled content, the map bitmap) is
// left alone.
//
// Off with "set g_bUiAdapt 0"; a no-op at 1024x768. Each adapted window is logged to FFDebug.log
// as [UIADAPT] with its mode and rule.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include "chandler.h"
#include "cbitmap.h"
#include "ctile.h"
#include "graphics/include/fflog.h"

extern bool g_bUiAdapt;
extern char g_strUiAdaptEdges[];
DWORD RGB565toRGB8(WORD sc); // imagersc.cpp

// See imagersc.h. Nearest-neighbour in 16.16 fixed point. Called only for the few full-screen
// background bitmaps UI95_AdaptWindow marks, so it favours being obviously right over fast.
void IMAGE_RSC::BlitCover(SCREEN *surface, const UI95_RECT *dest, const UI95_RECT *clip, int fit)
{
    if (not Owner or not Owner->Data_ or not surface or not surface->mem)
        return;

    const long iw = Header->w, ih = Header->h;
    const long dw = dest->right - dest->left, dh = dest->bottom - dest->top;

    if (iw <= 0 or ih <= 0 or dw <= 0 or dh <= 0)
        return;

    // Source pixels per destination pixel, 16.16. 1:1 by default; covering takes the smaller of
    // the two ratios for both axes; stretching takes each axis's own.
    long long stepX = 0x10000, stepY = 0x10000;
    const long long sx = ((long long)iw << 16) / dw, sy = ((long long)ih << 16) / dh;

    if (fit == 1)
        stepX = stepY = sx < sy ? sx : sy;
    else if (fit == 2)
        stepX = sx, stepY = sy;

    // Where destination pixel (dest->left, dest->top) samples, so the image sits centred.
    const long long x0 = (((long long)iw << 16) - dw * stepX) / 2;
    const long long y0 = (((long long)ih << 16) - dh * stepY) / 2;

    const bool eightBit = (Header->flags bitand _RSC_8_BIT_) != 0;
    const bool keyed = (Header->flags bitand _RSC_USECOLORKEY_) != 0;
    const WORD *palette = (const WORD *)(Owner->Data_ + Header->paletteoffset);
    const BYTE *src8 = (const BYTE *)(Owner->Data_ + Header->imageoffset);
    const WORD *src16 = (const WORD *)(Owner->Data_ + Header->imageoffset);

    long top = max(clip->top, dest->top), bottom = min(clip->bottom, dest->bottom);
    long left = max(clip->left, dest->left), right = min(clip->right, dest->right);
    top = max(top, 0L);
    left = max(left, 0L);
    bottom = min(bottom, (long)surface->height);
    right = min(right, (long)surface->width);

    for (long y = top; y < bottom; ++y)
    {
        const long long fy = y0 + (y - dest->top) * stepY;
        const long sy = (long)(fy >> 16);
        const bool rowIn = fy >= 0 and sy < ih;

        for (long x = left; x < right; ++x)
        {
            const long long fx = x0 + (x - dest->left) * stepX;
            const long sx = (long)(fx >> 16);
            WORD pixel = 0; // the margin

            if (rowIn and fx >= 0 and sx < iw)
            {
                if (eightBit)
                {
                    const BYTE index = src8[sy * iw + sx];

                    if (keyed and index == 0)
                        continue;

                    pixel = palette[index];
                }
                else
                {
                    pixel = src16[sy * iw + sx];

                    if (keyed and pixel == Owner->ColorKey_)
                        continue;
                }
            }

            if (surface->bpp == 32)
                ((DWORD *)surface->mem)[y * surface->width + x] = RGB565toRGB8(pixel);
            else
                surface->mem[y * surface->width + x] = pixel;
        }
    }
}

// The bitmap path's destination rect, in surface pixels, and its clip -- as O_Output::Draw works
// them out for _OUT_BITMAP_ -- then BlitCover instead of Blit.
void O_Output::DrawCover(SCREEN *surface, UI95_RECT *cliprect)
{
    if (not Image_ or not Owner_ or not Owner_->Parent_)
        return;

    C_Window *win = Owner_->Parent_;
    UI95_RECT dest, clip = *cliprect;
    dest.left = Owner_->GetX() + GetX();
    dest.top = Owner_->GetY() + GetY();

    if (not(Owner_->GetFlags() bitand C_BIT_ABSOLUTE))
    {
        const UI95_RECT &ca = win->ClientArea_[Owner_->GetClient()];
        dest.left += win->VX_[Owner_->GetClient()];
        dest.top += win->VY_[Owner_->GetClient()];
        clip.left = max(clip.left, ca.left);
        clip.top = max(clip.top, ca.top);
        clip.right = min(clip.right, ca.right);
        clip.bottom = min(clip.bottom, ca.bottom);
    }

    dest.right = dest.left + GetW();
    dest.bottom = dest.top + GetH();
    const long ox = win->GetX(), oy = win->GetY();
    dest.left += ox, dest.right += ox, dest.top += oy, dest.bottom += oy;
    clip.left += ox, clip.right += ox, clip.top += oy, clip.bottom += oy;
    Image_->BlitCover(surface, &dest, &clip, cover_ == COVER_SCALE ? 1 : cover_ == COVER_STRETCH ? 2 : 0);
}


namespace
{
const long kStageW = 1024, kStageH = 768;
const long kTopBar = 33, kBottomBar = 727; // the shells' bars: 0..32 and 728..767
// A title-bar item starts at the top of the stage; something that merely ends above y 33 is body
// (munitions' "FLIGHT: COWBOY1" pair straddles the bar's edge, and the value alone was pulled up).
const long kTopBarStart = 6;
const long kSnap = 8; // how near an edge counts as touching it

enum Rule
{
    CENTRE,
    PIN_NEAR,
    PIN_FAR,
    FILL
};

const char *RuleName(Rule r)
{
    static const char *n[] = {"centre", "near", "far", "fill"};
    return n[r];
}

Rule Classify(long lo, long hi, long nearEdge, long farEdge)
{
    const bool n = lo <= nearEdge, f = hi >= farEdge;

    if (n and f)
        return FILL;

    if (f)
        return PIN_FAR;

    return n ? PIN_NEAR : CENTRE;
}

// Left half stays, right half pins right: how a row of bar buttons splits.
Rule Halves(long lo, long hi, long size)
{
    return (lo + hi) / 2 < size / 2 ? PIN_NEAR : PIN_FAR;
}

// New position and size along one axis; 'grow' is how much the container grew. Something that
// cannot stretch but spanned the container stays centred on it.
void Apply(Rule r, long grow, bool canStretch, long *pos, long *size)
{
    switch (r)
    {
    case FILL:
        if (canStretch)
            *size += grow;
        else
            *pos += grow / 2;

        break;

    case PIN_FAR:
        *pos += grow;
        break;

    case CENTRE:
        *pos += grow / 2;
        break;

    case PIN_NEAR:
        break;
    }
}

bool CanStretch(C_Base *c)
{
    switch (c->_GetCType_())
    {
    case _CNTL_TILE_:
    case _CNTL_BOX_:
    case _CNTL_LINE_:
    case _CNTL_FILL_:
    case _CNTL_MAP_MOVER_:
        return true;
    }

    return false;
}

// The map screens: a map pane between the bars that should fill, with panels pinned to its edges.
// UiAdaptEdges (cfg) names extra .scf files on top of these.
const char *const kEdgesFiles[] = {
    "cp_main", "cp_mspua", "cp_sua", "cp_tool", "cp_miss",   // campaign map
    "rec_eye", "rec_list",                                   // recon
    "tacpmain", "tacptool", "tac_smap", "tac_air", "tac_psua", "tac_team", // TE play map
    "tacemain", "tacetool", "tac_eair",                      // TE editor (tac_eair: its map pane)
    NULL,
};

bool NameIs(const char *base, const char *tok)
{
    const size_t n = strlen(tok);
    return n and _strnicmp(base, tok, n) == 0 and (base[n] == 0 or base[n] == '.');
}

bool EdgesFile(const char *file)
{
    if (not file)
        return false;

    const char *base = max(strrchr(file, '\\'), strrchr(file, '/'));
    base = base ? base + 1 : file;

    for (int i = 0; kEdgesFiles[i]; ++i)
        if (NameIs(base, kEdgesFiles[i]))
            return true;

    char list[0x40];
    strncpy_s(list, sizeof(list), g_strUiAdaptEdges, _TRUNCATE);
    char *ctx = NULL;

    for (char *tok = strtok_s(list, ";, ", &ctx); tok; tok = strtok_s(NULL, ";, ", &ctx))
        if (NameIs(base, tok))
            return true;

    return false;
}

// A picture, as opposed to a pattern: any bitmap, and a tile whose image is at least half the stage
// in either direction. A picture cannot tile or stretch into more room -- the logbook's background
// is a "tile" of one 1024-wide photo, and repeating it put a second cockpit beside the first.
bool IsPicture(C_Base *c)
{
    if (c->_GetCType_() == _CNTL_BITMAP_)
        return true;

    if (c->_GetCType_() != _CNTL_TILE_)
        return false;

    IMAGE_RSC *img = ((C_Tile *)c)->GetImage();
    return img and img->Header and (img->Header->w >= kStageW / 2 or img->Header->h >= kStageH / 2);
}

void SetCoverOn(C_Base *c, short mode, long w, long h)
{
    if (c->_GetCType_() == _CNTL_BITMAP_)
        ((C_Bitmap *)c)->SetCover(mode, w, h);
    else if (c->_GetCType_() == _CNTL_TILE_)
        ((C_Tile *)c)->SetCover(mode, w, h);
}

// A picture covering its whole container: the screen's backdrop.
bool IsBackdrop(C_Base *c, long ow, long oh)
{
    return IsPicture(c) and c->GetX() <= kSnap and c->GetY() <= kSnap and c->GetX() + c->GetW() >= ow - kSnap and
           c->GetY() + c->GetH() >= oh - kSnap;
}

// ---- EDGES: rules relative to each container (the campaign map screen) ----

// Where a container's top and bottom "edges" are, in its own coordinates. A container that starts
// at the stage top or reaches the bottom bar treats the bars as its edges, as windows do: recon's
// 3D pane client (0,32 1024x696 in a 768-tall shell) and the TE editor's map (32..728 in a window
// 0..728) sit exactly between them and must fill, not centre or slide.
struct Frame
{
    bool bars; // stage-tall: its top 32 / bottom 40 px hold bar items
    long nearY, farY;
};

Frame FrameOf(long stageTop, long h)
{
    Frame f;
    f.bars = stageTop <= 2 and h >= kStageH - 2;
    f.nearY = stageTop <= 2 ? kTopBar - stageTop : kSnap;
    f.farY = stageTop + h >= kBottomBar ? kBottomBar - stageTop : h - kSnap;
    return f;
}

void PlaceEdges(const Frame &fr, bool stretch, long ow, long oh, long gw, long gh, long *x, long *y, long *w,
                long *h)
{
    (void)oh;
    const bool topBar = fr.bars and *y <= kTopBarStart and *y + *h <= kTopBar;
    const bool bottomBar = fr.bars and *y >= kBottomBar;
    Rule rx = Classify(*x, *x + *w, kSnap, ow - kSnap);
    Rule ry = Classify(*y, *y + *h, fr.nearY, fr.farY);

    if (topBar or bottomBar)
    {
        ry = topBar ? PIN_NEAR : PIN_FAR;

        if (rx != FILL)
            rx = Halves(*x, *x + *w, ow);
    }

    Apply(rx, gw, stretch, x, w);
    Apply(ry, gh, stretch, y, h);
}

void EdgesControl(C_Base *c, const Frame &fr, long ow, long oh, long gw, long gh)
{
    const bool bars = fr.bars;
    long x = c->GetX(), y = c->GetY(), w = c->GetW(), h = c->GetH();

    if (w > ow + kSnap or h > oh + kSnap)
        return; // content larger than its container (scrolled lists, the map bitmap)

    if (IsBackdrop(c, ow, oh))
    {
        SetCoverOn(c, O_Output::COVER_SCALE, w + gw, h + gh);
        return;
    }

    // A bar drawn as one container-wide picture (the campaign shell's BAR_O/BAR_M tiles are a
    // single 1024-wide image): stretch it along the bar, as STAGE does.
    if (bars and IsPicture(c) and x <= kSnap and x + w >= ow - kSnap and (y + h <= kTopBar or y >= kBottomBar))
    {
        if (y >= kBottomBar)
            c->SetXY(x, y + gh);

        SetCoverOn(c, O_Output::COVER_STRETCH, w + gw, h);
        return;
    }

    PlaceEdges(fr, CanStretch(c) and not IsPicture(c), ow, oh, gw, gh, &x, &y, &w, &h);

    if (x != c->GetX() or y != c->GetY())
        c->SetXY(x, y);

    if (w != c->GetW() or h != c->GetH())
        c->SetWH(w, h);
}

// Returns the rules the window itself got, for the log.
void AdaptEdges(C_Window *win, long gw, long gh, Rule *rxOut, Rule *ryOut)
{
    const long ox = win->GetX(), oy = win->GetY(), ow = win->GetW(), oh = win->GetH();
    // A few px of slack: CP_SUA ends at 1023, and it is plainly a right-hand panel.
    Rule rx = Classify(ox, ox + ow, 2, kStageW - 2);
    Rule ry = Classify(oy, oy + oh, kTopBar, kBottomBar);

    // A panel narrower than half the stage pins rather than stretches: nothing inside it grows, so
    // stretching only adds an empty band (the recon target list grew 200 px of black). Unless it
    // carries bottom-bar buttons -- a tall toolbar strip (TE's TAC_TOOLBAR_WIN, 475x768) must still
    // reach the bottom bar, or its buttons end up under the map.
    bool bottomBarItems = false;

    for (CONTROLLIST *cur = win->GetControlList(); cur and not bottomBarItems; cur = cur->Next)
        bottomBarItems = cur->Control_ and cur->Control_->GetH() > 0 and
                         oy + cur->Control_->GetY() >= kBottomBar;

    if (ow < kStageW / 2 and not bottomBarItems)
    {
        if (rx == FILL)
            rx = PIN_NEAR;

        if (ry == FILL)
            ry = PIN_NEAR;
    }

    // A window at least half the stage wide that touches one side is the main pane beside a panel
    // (the TE editor's map at 328..1024, right of the team panel): it keeps its inner edge and
    // grows to the surface edge. Windows hanging past the stage (CP_TOOLBAR is 548 + 1024) are not.
    if (ow >= kStageW / 2 and ox >= -2 and ox + ow <= kStageW + kSnap and (rx == PIN_NEAR or rx == PIN_FAR))
        rx = FILL;
    long x = ox, y = oy, w = ow, h = oh;
    Apply(rx, gw, true, &x, &w);
    Apply(ry, gh, true, &y, &h);
    *rxOut = rx;
    *ryOut = ry;

    if (w != ow or h != oh)
    {
        win->ResizeSurface((short)w, (short)h);
        win->Area_.right = win->Area_.left + w;
        win->Area_.bottom = win->Area_.top + h;
    }

    win->SetXY((short)x, (short)y);
    const long wgw = w - ow, wgh = h - oh;

    if (wgw == 0 and wgh == 0)
        return; // moved, not grown: the contents come along unchanged

    const Frame wf = FrameOf(oy, oh);
    Frame cf[WIN_MAX_CLIENTS];
    long cgw[WIN_MAX_CLIENTS], cgh[WIN_MAX_CLIENTS], cow[WIN_MAX_CLIENTS], coh[WIN_MAX_CLIENTS];

    for (int i = 0; i < WIN_MAX_CLIENTS; ++i)
    {
        UI95_RECT &cr = win->ClientArea_[i];
        long cx = cr.left, cy = cr.top, cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        cow[i] = cw;
        coh[i] = ch;
        cf[i] = FrameOf(oy + cr.top, ch);
        PlaceEdges(wf, true, ow, oh, wgw, wgh, &cx, &cy, &cw, &ch);
        cgw[i] = cw - cow[i];
        cgh[i] = ch - coh[i];
        win->VX_[i] += cx - cr.left; // a client's controls are drawn relative to VX_/VY_
        win->VY_[i] += cy - cr.top;

        if (win->VW_[i] == cow[i])
            win->VW_[i] = cw;

        if (win->VH_[i] == coh[i])
            win->VH_[i] = ch;

        cr.left = cx, cr.top = cy, cr.right = cx + cw, cr.bottom = cy + ch;
        win->FullClientArea_[i] = cr;
    }

    for (CONTROLLIST *cur = win->GetControlList(); cur; cur = cur->Next)
    {
        C_Base *c = cur->Control_;

        if (not c)
            continue;

        if (c->GetFlags() bitand C_BIT_ABSOLUTE)
            EdgesControl(c, wf, ow, oh, wgw, wgh);
        else
        {
            const int i = c->GetClient() < WIN_MAX_CLIENTS ? c->GetClient() : 0;
            EdgesControl(c, cf[i], cow[i], coh[i], cgw[i], cgh[i]);
        }
    }
}

// ---- STAGE: rules in stage coordinates (every other screen) ----
//
// Each piece is placed by where it sits on the 1024x768 stage, not in its window: bar items go to
// the bars, pictures spanning the stage cover the surface, shapes spanning it stretch, and the rest
// rides with the stage to the middle. The window then becomes whatever rect holds its pieces.

struct Box
{
    long x, y, w, h;
};

// A control with no size yet is sized later by code (the theater picture is an empty button until a
// theater is chosen), so where it sits says nothing: it is body, not bar.
bool InTopBar(const Box &b)
{
    return b.y <= kTopBarStart and b.y + b.h <= kTopBar;
}

bool InBar(const Box &b)
{
    return b.w > 0 and b.h > 0 and (InTopBar(b) or b.y >= kBottomBar);
}

Box StagePlace(Box b, bool stretch, long gw, long gh)
{
    if (b.w <= 0 or b.h <= 0)
    {
        b.x += gw / 2;
        b.y += gh / 2;
        return b;
    }

    const bool top = InTopBar(b), bottom = b.y >= kBottomBar;
    const bool spanX = b.x <= kSnap and b.x + b.w >= kStageW - kSnap;
    const bool spanY = b.y <= kSnap and b.y + b.h >= kStageH - kSnap;
    Rule rx, ry;

    if (top or bottom)
    {
        ry = top ? PIN_NEAR : PIN_FAR;
        rx = spanX ? FILL : Halves(b.x, b.x + b.w, kStageW);

        // A bar strip anchored at one side of the stage reaches halfway into the new room, so the
        // left and right pieces of a split bar still meet in the middle.
        if (stretch and not spanX and (b.x <= kSnap or b.x + b.w >= kStageW - kSnap))
        {
            if (b.x > kSnap)
                b.x += gw / 2;

            b.w += gw / 2;
            rx = PIN_NEAR;
        }
    }
    else
    {
        rx = spanX ? FILL : CENTRE;
        ry = spanY ? FILL : CENTRE;
    }

    Apply(rx, gw, stretch, &b.x, &b.w);
    Apply(ry, gh, stretch, &b.y, &b.h);
    return b;
}

void Grow(Box *u, bool *any, const Box &b)
{
    if (not *any)
    {
        *u = b;
        *any = true;
        return;
    }

    const long r = max(u->x + u->w, b.x + b.w), btm = max(u->y + u->h, b.y + b.h);
    u->x = min(u->x, b.x);
    u->y = min(u->y, b.y);
    u->w = r - u->x;
    u->h = btm - u->y;
}

// Per adapted STAGE window, what an absolute control added after parse needs to be placed: the
// stock origin, the new origin and the growth. (The munitions loadout grid is built this way.)
struct LateInfo
{
    long ox, oy, nx, ny, gw, gh;
};
std::map<C_Window *, LateInfo> sLate;

void AdaptStage(C_Window *win, long sw, long sh, long gw, long gh, bool *fullOut)
{
    const long ox = win->GetX(), oy = win->GetY(), ow = win->GetW(), oh = win->GetH();
    const bool full = ox <= 2 and oy <= 2 and ox + ow >= kStageW - 2 and oy + oh >= kStageH - 2;
    const Box body = {ox + gw / 2, oy + gh / 2, ow, oh};
    *fullOut = full;

    // Clients, in stage coordinates. A client spanning the window on either axis is really "the
    // window" (munitions' main client is 0,0 1024x728 -- the window less its bottom bar): its
    // controls are placed one by one, and its clip reaches every edge it touched so bar items stay
    // visible. Any other client moves as a unit and carries its controls.
    Box cOld[WIN_MAX_CLIENTS], cNew[WIN_MAX_CLIENTS];
    bool whole[WIN_MAX_CLIENTS];
    Box u = {0, 0, 0, 0};
    bool any = false, anyBody = false;

    for (int i = 0; i < WIN_MAX_CLIENTS; ++i)
    {
        const UI95_RECT &cr = win->ClientArea_[i];
        cOld[i].x = ox + cr.left, cOld[i].y = oy + cr.top;
        cOld[i].w = cr.right - cr.left, cOld[i].h = cr.bottom - cr.top;
        whole[i] = (cr.left <= 0 and cr.right >= ow) or (cr.top <= 0 and cr.bottom >= oh);

        if (whole[i])
        {
            // Each edge reaches the surface edge (or bar) it touched on the stage, else rides with it.
            const Box &o = cOld[i];
            long l = o.x <= kSnap ? o.x : o.x + gw / 2;
            long t = o.y <= kTopBar ? o.y : o.y + gh / 2;
            long r = o.x + o.w >= kStageW - kSnap ? o.x + o.w + gw : o.x + o.w + gw / 2;
            long b = o.y + o.h >= kBottomBar - kSnap ? o.y + o.h + gh : o.y + o.h + gh / 2;
            cNew[i].x = l, cNew[i].y = t, cNew[i].w = r - l, cNew[i].h = b - t;

            if (not full)
                Grow(&u, &any, cNew[i]); // the window must hold its clip
        }
        else
        {
            cNew[i] = StagePlace(cOld[i], true, gw, gh);

            if (InBar(cOld[i]))
                Grow(&u, &any, cNew[i]);
            else
                anyBody = true;
        }
    }

    // Controls: their new rect in surface coordinates.
    struct Plan
    {
        C_Base *c;
        Box to;
        short cover; // O_Output::COVER_*
        bool follow;
    };
    int n = 0;

    for (CONTROLLIST *cur = win->GetControlList(); cur; cur = cur->Next)
        n += cur->Control_ != NULL;

    Plan *plan = n ? new Plan[n] : NULL;
    int k = 0;

    for (CONTROLLIST *cur = win->GetControlList(); cur; cur = cur->Next)
    {
        C_Base *c = cur->Control_;

        if (not c)
            continue;

        Plan &p = plan[k++];
        p.c = c;
        p.cover = O_Output::COVER_OFF;
        p.follow = false;
        const bool abs = (c->GetFlags() bitand C_BIT_ABSOLUTE) != 0;
        const int ci = (not abs and c->GetClient() < WIN_MAX_CLIENTS) ? c->GetClient() : 0;
        Box b = {c->GetX(), c->GetY(), c->GetW(), c->GetH()};

        if (not abs)
            b.x += win->VX_[ci], b.y += win->VY_[ci];

        b.x += ox, b.y += oy;

        if (not abs and not whole[ci])
        {
            p.follow = true; // rides with its client
            p.to = b;
            p.to.x += cNew[ci].x - cOld[ci].x;
            p.to.y += cNew[ci].y - cOld[ci].y;
            continue;
        }

        if (full and IsBackdrop(c, ow, oh))
        {
            p.cover = O_Output::COVER_CENTER;
            p.to.x = 0, p.to.y = 0, p.to.w = sw, p.to.h = sh;
        }
        else if (IsPicture(c) and InBar(b) and b.x <= kSnap and
                 b.x + b.w >= kStageW - kSnap)
        {
            // A bar drawn as one stage-wide picture: stretch it along the bar.
            p.cover = O_Output::COVER_STRETCH;
            p.to = StagePlace(b, true, gw, gh);
        }
        else if (b.w > ow + kSnap or b.h > oh + kSnap)
            p.to = b, p.to.x += gw / 2, p.to.y += gh / 2; // oversized content rides with the stage
        else
            p.to = StagePlace(b, CanStretch(c) and not IsPicture(c), gw, gh);

        if (InBar(b))
            Grow(&u, &any, p.to);
        else
            anyBody = true;
    }

    // The window's new rect: the whole surface for a shell, else what holds its pieces -- the bar
    // pieces collected above plus, always, the body: the stock window moved with the stage. Never
    // less than the body, because the game adds controls to some windows after they are parsed
    // (the squadron list on campaign select), at positions measured from the stock window.
    (void)anyBody;
    Box nw;

    if (full)
        nw.x = 0, nw.y = 0, nw.w = sw, nw.h = sh;
    else
    {
        Grow(&u, &any, body);
        nw = u;
    }

    // Where the body sits inside the new window. A whole-window client's scroll origin carries it,
    // so controls added to it later, which measure from the stock window, land on the body.
    const long bdx = body.x - nw.x, bdy = body.y - nw.y;

    // Absolute controls added later ignore that origin: remember how to place them (UI95_AdaptLateControl).
    if (bdx or bdy)
    {
        LateInfo li = {ox, oy, nw.x, nw.y, gw, gh};
        sLate[win] = li;
    }

    if (nw.w != ow or nw.h != oh)
    {
        win->ResizeSurface((short)nw.w, (short)nw.h);
        win->Area_.left = 0, win->Area_.top = 0;
        win->Area_.right = nw.w, win->Area_.bottom = nw.h;
    }

    win->SetXY((short)nw.x, (short)nw.y);

    for (int i = 0; i < WIN_MAX_CLIENTS; ++i)
    {
        UI95_RECT &cr = win->ClientArea_[i];
        const long oldLeft = cr.left, oldTop = cr.top, oldW = cr.right - cr.left, oldH = cr.bottom - cr.top;
        const Box &to = cNew[i];
        cr.left = to.x - nw.x, cr.top = to.y - nw.y;
        cr.right = cr.left + to.w, cr.bottom = cr.top + to.h;
        win->VX_[i] += whole[i] ? bdx : cr.left - oldLeft;
        win->VY_[i] += whole[i] ? bdy : cr.top - oldTop;

        if (win->VW_[i] == oldW)
            win->VW_[i] = to.w;

        if (win->VH_[i] == oldH)
            win->VH_[i] = to.h;

        win->FullClientArea_[i] = cr;
    }

    for (int j = 0; j < k; ++j)
    {
        Plan &p = plan[j];

        if (p.follow)
            continue; // positions are client-relative, and the client moved

        const bool abs = (p.c->GetFlags() bitand C_BIT_ABSOLUTE) != 0;
        const int ci = (not abs and p.c->GetClient() < WIN_MAX_CLIENTS) ? p.c->GetClient() : 0;
        long x = p.to.x - nw.x, y = p.to.y - nw.y;

        if (not abs)
            x -= win->VX_[ci], y -= win->VY_[ci];

        if (p.cover != O_Output::COVER_OFF)
        {
            p.c->SetXY(x, y);
            SetCoverOn(p.c, p.cover, p.to.w, p.to.h);
            continue;
        }

        if (x != p.c->GetX() or y != p.c->GetY())
            p.c->SetXY(x, y);

        if (p.to.w != p.c->GetW() or p.to.h != p.c->GetH())
            p.c->SetWH(p.to.w, p.to.h);

        // A control whose setters do not do what they say would otherwise sit silently in the wrong
        // place; say so, with its type, so the harness log names it.
        if (p.c->GetX() != x or p.c->GetY() != y)
        {
            char line[200];
            sprintf_s(line, sizeof(line), "[UIADAPT]   control %ld (type %d) wanted %ld,%ld, has %ld,%ld\n",
                      p.c->GetID(), p.c->_GetCType_() - _CNTL_ANIMATION_, x, y, p.c->GetX(), p.c->GetY());
            FFDebugLog(line);
        }
    }

    delete[] plan;
}
} // namespace

void UI95_AdaptWindow(C_Window *win, const char *file)
{
    int sw, sh;
    UI95_GetSurfaceSize(&sw, &sh);
    const long gw = sw - kStageW, gh = sh - kStageH;

    if (not g_bUiAdapt or not win or (gw <= 0 and gh <= 0))
        return;

    const bool edges = EdgesFile(file);
    const long ox = win->GetX(), oy = win->GetY(), ow = win->GetW(), oh = win->GetH();

    // Drag ranges, read before anything moves, then opened up: SetXY clamps to them, and the stock
    // ones would pull anything placed past x 1024 back onto the stage.
    short rx1, ry1, rx2, ry2;
    win->GetRanges(&rx1, &ry1, &rx2, &ry2);
    win->SetRanges(-30000, -30000, 30000, 30000, (short)ow, (short)oh);

    Rule rx = CENTRE, ry = CENTRE;
    bool full = false;

    if (edges)
        AdaptEdges(win, gw, gh, &rx, &ry);
    else
        AdaptStage(win, sw, sh, gw, gh, &full);

    // Stage-sized ranges become surface-sized; any other range moves with the window.
    const long dx = win->GetX() - ox, dy = win->GetY() - oy;

    if (rx2 >= kStageW)
        rx2 = (short)(rx2 + gw);
    else
        rx1 = (short)(rx1 + dx), rx2 = (short)(rx2 + dx);

    if (ry2 >= kStageH)
        ry2 = (short)(ry2 + gh);
    else
        ry1 = (short)(ry1 + dy), ry2 = (short)(ry2 + dy);

    const short x = win->GetX(), y = win->GetY();
    win->SetRanges(rx1, ry1, rx2, ry2, win->GetW(), win->GetH()); // also the minimum size
    win->SetXY(x, y); // SetXY clamps to the ranges, so again now they are right

    char line[320];

    if (edges)
        sprintf_s(line, sizeof(line), "[UIADAPT] edges window %ld %ld,%ld %ldx%ld -> %d,%d %dx%d (x %s, y %s) %s\n",
                  win->GetID(), ox, oy, ow, oh, win->GetX(), win->GetY(), win->GetW(), win->GetH(), RuleName(rx),
                  RuleName(ry), file ? file : "");
    else
        sprintf_s(line, sizeof(line), "[UIADAPT] stage window %ld %ld,%ld %ldx%ld -> %d,%d %dx%d%s %s\n",
                  win->GetID(), ox, oy, ow, oh, win->GetX(), win->GetY(), win->GetW(), win->GetH(),
                  full ? " (shell)" : "", file ? file : "");

    FFDebugLog(line);
}

// Called by C_Window::AddControl/AddControlTop. Code builds some controls after the window was
// parsed and adapted, positioned in stock window coordinates. Non-absolute ones follow their client's
// scroll origin, which already carries the body offset; absolute ones do not, so place them here
// exactly as a parsed control would have been.
void UI95_AdaptLateControl(C_Window *win, C_Base *c)
{
    if (not win or not c or not(c->GetFlags() bitand C_BIT_ABSOLUTE))
        return;

    std::map<C_Window *, LateInfo>::const_iterator it = sLate.find(win);

    if (it == sLate.end())
        return;

    const LateInfo &li = it->second;
    Box b = {li.ox + c->GetX(), li.oy + c->GetY(), c->GetW(), c->GetH()};
    const Box to = StagePlace(b, false, li.gw, li.gh); // never stretch: code sized it on purpose
    c->SetXY(to.x - li.nx, to.y - li.ny);
}

// Called by C_Window::Cleanup: a freed window's address may be reused by the next one.
void UI95_ForgetWindow(C_Window *win)
{
    sLate.erase(win);
}
