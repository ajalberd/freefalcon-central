// Artscout - 2026: scripted UI test harness. See uitest.h for how it is run and UI-OVERHAUL.md for why.
//
// Script: one command per line, '#' starts a comment. IDs are the symbolic names from the .scf
// files and userids.lst (UI_MAIN_SCREEN, CP_PUA_MAP, ...); a plain number also works.
//
//   timeout <sec>                 whole-run limit (default 180); the run is killed when it passes
//   wait <ms>
//   waitwin <WINID> [ms]          until the window is shown (default 15000 ms)
//   waitgone <WINID> [ms]         until it is hidden or removed
//   click <CTRLID> [<WINID>]      centre of the control, through the real WM_ mouse path; with no
//                                 window, the topmost shown window that has the control
//   clickxy <x> <y>               surface pixels
//   rclickxy <x> <y>              right click, surface pixels (map popups)
//   clickin <WINID> <x> <y>       left click at a point inside a window (popup menu rows)
//   clickat <CTRLID> <dx> <dy>    left click at an offset inside a control (tree/list rows)
//   hold <CTRLID> <dy> <ms>       press dy px below a control's centre for ms (panners repeat)
//   text <CTRLID>                 log a text control's current text
//   rclickicon <WINID> [n]        right click the n-th map icon ui95 would hit (default 0)
//   shot <name>                   the ui95 surface as ui95 drew it -> out\<name>.bmp
//   wshot <name>                  the window as it is on screen (PrintWindow) -> out\<name>.bmp
//   layout <name>                 every shown window and control, with lint -> out\<name>.json
//   windows                       one line per shown window into result.log
//   log <text>
//   quit                          end the run (the process is terminated, nothing is saved)
//
// Every command writes one line to out\result.log starting OK, FAIL or INFO; a FAIL does not stop
// the run. The last line is DONE, TIMEOUT or SCRIPT-ERROR. A missing last line means the game died:
// look at FFCrash.log.

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <map>
#include <string>
#include "chandler.h"
#include "cbitmap.h"
#include "ctile.h"
#include "ui95_ext.h"
#include "uitest.h"

extern C_Handler *gMainHandler;
extern C_Parser *gMainParser;

static char sScriptArg[MAX_PATH] = "";
static char sOutDir[MAX_PATH] = "";
static FILE *sLog = NULL;
static DWORD sStartTick = 0;
static DWORD sTimeoutMs = 180000;
static CRITICAL_SECTION sLogLock; // the watchdog can finish the run while the script thread logs

void UiTest_SetScript(const char *arg)
{
    if (arg)
        strncpy_s(sScriptArg, sizeof(sScriptArg), arg, _TRUNCATE);
}

bool UiTest_Requested()
{
    return sScriptArg[0] != 0;
}

static void ExeDir(char *out, size_t n)
{
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *slash = strrchr(out, '\\');

    if (slash)
        slash[1] = 0;
}

static void Log(const char *fmt, ...)
{
    EnterCriticalSection(&sLogLock);

    if (sLog)
    {
        fprintf(sLog, "%7.2f ", (GetTickCount() - sStartTick) / 1000.0);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(sLog, fmt, ap);
        va_end(ap);
        fputc('\n', sLog);
        fflush(sLog);
    }

    LeaveCriticalSection(&sLogLock);
}

static void Finish(const char *how)
{
    Log("%s", how);
    EnterCriticalSection(&sLogLock); // held until the process is gone: nothing logs after the verdict

    if (sLog)
        fclose(sLog);

    sLog = NULL;
    TerminateProcess(GetCurrentProcess(), 0);
}

static bool TimedOut()
{
    return GetTickCount() - sStartTick > sTimeoutMs;
}

// id <-> symbolic name. C_Parser::FindIDStr is a stub that prints the number, and the names the code
// uses (CP_MAIN_CTRL) are an enum in userids.h that the game never sees as text. So the harness reads
// uitest\ids.txt ("NAME value", written from userids.h by run.ps1) first, then the .id files that
// art\userids.lst names. IDs are reused across screens, so a value can carry several names; up to
// three are shown, code names first.
static std::map<long, std::string> sNames;
static std::map<std::string, long> sIds;

static void AddName(const char *name, long value)
{
    if (sIds.find(name) != sIds.end())
        return;

    sIds[name] = value;
    std::string &names = sNames[value];

    if (names.empty())
        names = name;
    else if (std::count(names.begin(), names.end(), '|') < 2)
        names += std::string("|") + name;
}

static void LoadIdFile(const char *path)
{
    FILE *ids = NULL;

    if (fopen_s(&ids, path, "r") or !ids)
        return;

    char idLine[256], name[128];
    long value;

    while (fgets(idLine, sizeof(idLine), ids))
        if (sscanf_s(idLine, "%127s %ld", name, (unsigned)sizeof(name), &value) == 2)
            AddName(name, value);

    fclose(ids);
}

static void LoadNames(const char *exeDir)
{
    char path[MAX_PATH], line[MAX_PATH];
    sprintf_s(path, sizeof(path), "%suitest\\ids.txt", exeDir);
    LoadIdFile(path);
    sprintf_s(path, sizeof(path), "%sart\\userids.lst", exeDir);
    FILE *lst = NULL;

    if (fopen_s(&lst, path, "r") or !lst)
        return;

    while (fgets(line, sizeof(line), lst))
    {
        char file[MAX_PATH];

        if (sscanf_s(line, "%259s", file, (unsigned)sizeof(file)) != 1 or file[0] == '#' or file[0] == '/')
            continue;

        sprintf_s(path, sizeof(path), "%s%s", exeDir, file);
        LoadIdFile(path);
    }

    fclose(lst);
}

static const char *IdName(long id, char *buf, size_t n)
{
    std::map<long, std::string>::const_iterator it = sNames.find(id);

    if (it != sNames.end())
        return it->second.c_str();

    sprintf_s(buf, n, "%ld", id);
    return buf;
}

static long ParseId(const char *tok)
{
    if (!tok or !tok[0])
        return 0;

    if (tok[0] >= '0' and tok[0] <= '9')
        return atol(tok);

    std::map<std::string, long>::const_iterator it = sIds.find(tok);

    if (it != sIds.end())
        return it->second;

    return gMainParser ? gMainParser->FindID((char *)tok) : 0;
}

static bool WindowShown(long id)
{
    return gMainHandler and gMainHandler->FindWindow(id) and
           (gMainHandler->GetWindowFlags(id) bitand C_BIT_ENABLED);
}

// Where a control is drawn, in surface pixels. Mirrors C_Window::GetControl: a control that is not
// C_BIT_ABSOLUTE is offset by its client's scroll origin VX_/VY_.
static void ControlRect(C_Window *win, C_Base *c, long *x, long *y, long *w, long *h)
{
    long cx = c->GetX(), cy = c->GetY();

    if (!(c->GetFlags() bitand C_BIT_ABSOLUTE))
    {
        cx += win->VX_[c->GetClient()];
        cy += win->VY_[c->GetClient()];
    }

    *x = win->GetX() + cx;
    *y = win->GetY() + cy;
    *w = c->GetW();
    *h = c->GetH();
}

// Surface pixels -> window client pixels: the inverse of ClientToSurface in chandler.cpp (the present
// stretches the surface over the whole client). Aims at the pixel's centre so the round trip lands
// back on the same surface pixel.
static void SurfaceToClient(long *x, long *y)
{
    RECT rc;
    int sw, sh;

    if (not GetClientRect(gMainHandler->GetAppWnd(), &rc) or rc.right <= 0 or rc.bottom <= 0)
        return;

    UI95_GetSurfaceSize(&sw, &sh);

    if (rc.right == sw and rc.bottom == sh)
        return;

    *x = (long)(((long long)*x * 2 + 1) * rc.right / (2LL * sw));
    *y = (long)(((long long)*y * 2 + 1) * rc.bottom / (2LL * sh));
}

static void PostClick(long sx, long sy, bool right = false)
{
    SurfaceToClient(&sx, &sy);
    HWND hwnd = gMainHandler->GetAppWnd();
    LPARAM lp = MAKELPARAM((WORD)sx, (WORD)sy);
    PostMessage(hwnd, WM_MOUSEMOVE, 0, lp);
    Sleep(50);
    PostMessage(hwnd, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON, lp);
    Sleep(80);
    PostMessage(hwnd, right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, lp);
}

static bool WriteBmp24(const char *path, const unsigned char *bgr, int w, int h)
{
    FILE *f = NULL;

    if (fopen_s(&f, path, "wb") or !f)
        return false;

    const int row = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h; // bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    unsigned char pad[4] = {};

    for (int y = h - 1; y >= 0; --y)
    {
        fwrite(bgr + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fwrite(pad, 1, row - w * 3, f);
    }

    fclose(f);
    return true;
}

static void OutPath(char *out, size_t n, const char *name, const char *ext)
{
    sprintf_s(out, n, "%s%s.%s", sOutDir, name, ext);
}

static void Shot(const char *name)
{
    ImageBuffer *front = gMainHandler->GetFront();

    if (!front)
    {
        Log("FAIL shot %s: no front surface", name);
        return;
    }

    gMainHandler->EnterCritical();
    const int w = front->targetXres(), h = front->targetYres();
    const int bpp = front->PixelSize();
    const int stride = front->targetStride() / (bpp ? bpp : 1);
    const unsigned char *mem = (const unsigned char *)front->Lock();
    unsigned char *bgr = (mem and w > 0 and h > 0) ? (unsigned char *)malloc((size_t)w * h * 3) : NULL;

    if (bgr)
    {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                unsigned char *o = bgr + ((size_t)y * w + x) * 3;

                if (bpp == 2)
                {
                    unsigned v = ((const unsigned short *)mem)[(size_t)y * stride + x];
                    unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                    o[0] = (unsigned char)((b << 3) | (b >> 2));
                    o[1] = (unsigned char)((g << 2) | (g >> 4));
                    o[2] = (unsigned char)((r << 3) | (r >> 2));
                }
                else
                {
                    unsigned v = ((const unsigned *)mem)[(size_t)y * stride + x];
                    o[0] = (unsigned char)(v & 255);
                    o[1] = (unsigned char)((v >> 8) & 255);
                    o[2] = (unsigned char)((v >> 16) & 255);
                }
            }
    }

    front->Unlock();
    gMainHandler->LeaveCritical();

    char path[MAX_PATH];
    OutPath(path, sizeof(path), name, "bmp");

    if (bgr and WriteBmp24(path, bgr, w, h))
        Log("OK shot %s %dx%d bpp=%d", name, w, h, bpp * 8);
    else
        Log("FAIL shot %s (w=%d h=%d mem=%p)", name, w, h, mem);

    free(bgr);
}

static void WindowShot(const char *name)
{
    HWND hwnd = gMainHandler->GetAppWnd();
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;

    if (w <= 0 or h <= 0)
    {
        Log("FAIL wshot %s: client %dx%d", name, w, h);
        return;
    }

    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    // PW_CLIENTONLY | PW_RENDERFULLCONTENT: the flip-model swap chain is not in the window DC.
    BOOL ok = PrintWindow(hwnd, mdc, 1 | 2);
    SelectObject(mdc, old);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    const int row = (w * 3 + 3) & ~3;
    unsigned char *padded = (unsigned char *)malloc((size_t)row * h);
    unsigned char *bgr = (unsigned char *)malloc((size_t)w * h * 3);
    int got = (padded and bgr) ? GetDIBits(mdc, bmp, 0, h, padded, &bi, DIB_RGB_COLORS) : 0;

    for (int y = 0; got and y < h; ++y)
        memcpy(bgr + (size_t)y * w * 3, padded + (size_t)y * row, (size_t)w * 3);

    char path[MAX_PATH];
    OutPath(path, sizeof(path), name, "bmp");

    if (ok and got and WriteBmp24(path, bgr, w, h))
        Log("OK wshot %s client %dx%d", name, w, h);
    else
        Log("FAIL wshot %s (PrintWindow=%d GetDIBits=%d)", name, ok, got);

    free(padded);
    free(bgr);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
}

// Topmost shown window holding the control, or the named window.
static C_Window *FindControlWindow(long ctrlId, long winId, C_Base **ctrlOut)
{
    C_Window *best = NULL;
    C_Base *bestCtrl = NULL;

    for (C_Window *win = gMainHandler->_GetFirstWindow(); win; win = gMainHandler->_GetNextWindow(win))
    {
        if (winId and win->GetID() != winId)
            continue;

        if (!(gMainHandler->GetWindowFlags(win->GetID()) bitand C_BIT_ENABLED))
            continue;

        C_Base *c = win->FindControl(ctrlId);

        if (c and (!best or win->GetDepth() >= best->GetDepth()))
        {
            best = win;
            bestCtrl = c;
        }
    }

    *ctrlOut = bestCtrl;
    return best;
}

static void Click(long ctrlId, long winId, const char *label)
{
    gMainHandler->EnterCritical();
    C_Base *c = NULL;
    C_Window *win = FindControlWindow(ctrlId, winId, &c);

    if (!win)
    {
        gMainHandler->LeaveCritical();
        Log("FAIL click %s: no shown window has it", label);
        return;
    }

    long x, y, w, h;
    ControlRect(win, c, &x, &y, &w, &h);
    const long sx = x + w / 2, sy = y + h / 2;
    // Check what ui95 itself would hit there before sending anything.
    C_Window *over = gMainHandler->GetWindow((short)sx, (short)sy);
    long hitId = 0;
    C_Base *hit = over ? over->GetControl(&hitId, sx - over->GetX(), sy - over->GetY()) : NULL;
    const bool hidden = (c->GetFlags() bitand C_BIT_INVISIBLE) != 0;
    gMainHandler->LeaveCritical();

    char a[64], b[64];
    Log("%s click %s in %s at %ld,%ld (rect %ld,%ld %ldx%ld)%s%s%s", (hit == c) ? "OK" : "FAIL", label,
        IdName(win->GetID(), a, sizeof(a)), sx, sy, x, y, w, h, hidden ? " [invisible]" : "",
        (hit != c) ? " -- hit test lands on " : "",
        (hit != c) ? (hit ? IdName(hit->GetID(), b, sizeof(b)) : (over ? "no control" : "no window")) : "");

    if (hit == c)
        PostClick(sx, sy);
}

// Right-click the n-th map icon (0-based) in a window that ui95 itself would hit at the icon's
// centre -- i.e. one not hidden under another window or control. The map popups (recon, ...)
// hang off icons, which are not controls of their own.
static void RightClickIcon(long winId, int n, const char *label)
{
    gMainHandler->EnterCritical();
    C_Window *win = gMainHandler->FindWindow(winId);
    long sx = 0, sy = 0, iconId = 0;
    int seen = 0;
    bool found = false;

    for (CONTROLLIST *cur = win ? win->GetControlList() : NULL; cur and not found; cur = cur->Next)
    {
        C_Base *c = cur->Control_;

        if (not c or c->_GetCType_() != _CNTL_MAPICON_)
            continue;

        C_MapIcon *mi = (C_MapIcon *)c;
        C_HASHNODE *node;
        long idx;

        for (MAPICONLIST *ic = (MAPICONLIST *)mi->GetRoot()->GetFirst(&node, &idx); ic and not found;
             ic = (MAPICONLIST *)mi->GetRoot()->GetNext(&node, &idx))
        {
            if ((ic->Flags bitand C_BIT_INVISIBLE) or not(ic->Flags bitand C_BIT_ENABLED) or not ic->Icon)
                continue;

            const long x = win->GetX() + win->VX_[c->GetClient()] + ic->x + ic->Icon->GetX() + ic->Icon->GetW() / 2;
            const long y = win->GetY() + win->VY_[c->GetClient()] + ic->y + ic->Icon->GetY() + ic->Icon->GetH() / 2;
            long hitId = 0;
            C_Window *over = gMainHandler->GetWindow((short)x, (short)y);

            if (over != win or over->GetControl(&hitId, x - win->GetX(), y - win->GetY()) != c)
                continue;

            if (seen++ == n)
                found = true, sx = x, sy = y, iconId = ic->ID;
        }
    }

    gMainHandler->LeaveCritical();

    if (not found)
    {
        Log("FAIL rclickicon %s: %d hittable icons, wanted #%d", label, seen, n);
        return;
    }

    Log("OK rclickicon %s #%d (icon %ld) at %ld,%ld", label, n, iconId, sx, sy);
    PostClick(sx, sy, true);
}

static void Layout(const char *name)
{
    char path[MAX_PATH];
    OutPath(path, sizeof(path), name, "json");
    FILE *f = NULL;

    if (fopen_s(&f, path, "w") or !f)
    {
        Log("FAIL layout %s: cannot write %s", name, path);
        return;
    }

    RECT rc;
    GetClientRect(gMainHandler->GetAppWnd(), &rc);
    ImageBuffer *front = gMainHandler->GetFront();
    const int sw = front ? front->targetXres() : 0, sh = front ? front->targetYres() : 0;
    int nWin = 0, nLint = 0;
    char a[64], b[64];

    gMainHandler->EnterCritical();
    fprintf(f, "{\n\"surface\": [%d, %d],\n\"client\": [%ld, %ld],\n\"windows\": [\n", sw, sh,
            rc.right - rc.left, rc.bottom - rc.top);

    for (C_Window *win = gMainHandler->_GetFirstWindow(); win; win = gMainHandler->_GetNextWindow(win))
    {
        if (!(gMainHandler->GetWindowFlags(win->GetID()) bitand C_BIT_ENABLED))
            continue;

        const long wx = win->GetX(), wy = win->GetY(), ww = win->GetW(), wh = win->GetH();
        const bool offCanvas = wx < 0 or wy < 0 or wx + ww > sw or wy + wh > sh;
        nLint += offCanvas;
        fprintf(f, "%s{\"id\": \"%s\", \"rect\": [%ld, %ld, %ld, %ld], \"depth\": %d%s,\n \"clients\": [",
                nWin++ ? ",\n" : "", IdName(win->GetID(), a, sizeof(a)), wx, wy, ww, wh, win->GetDepth(),
                offCanvas ? ", \"lint\": \"off-canvas\"" : "");

        for (int i = 0; i < WIN_MAX_CLIENTS; ++i)
        {
            const UI95_RECT &cr = win->ClientArea_[i];
            fprintf(f, "%s[%ld, %ld, %ld, %ld]", i ? ", " : "", cr.left, cr.top, cr.right - cr.left,
                    cr.bottom - cr.top);
        }

        fprintf(f, "],\n \"controls\": [");
        int nCtl = 0;

        for (CONTROLLIST *cur = win->GetControlList(); cur; cur = cur->Next)
        {
            C_Base *c = cur->Control_;

            if (!c)
                continue;

            long x, y, w, h;
            ControlRect(win, c, &x, &y, &w, &h);
            const bool invisible = (c->GetFlags() bitand C_BIT_INVISIBLE) != 0;
            // A control is clipped to its window, and unless it is absolute, to its client.
            const long rx = x - wx, ry = y - wy;
            const char *lint = NULL;

            if (!invisible and w > 0 and h > 0)
            {
                if (rx < 0 or ry < 0 or rx + w > ww or ry + h > wh)
                    lint = "outside-window";
                else if (!(c->GetFlags() bitand C_BIT_ABSOLUTE))
                {
                    const UI95_RECT &cr = win->ClientArea_[c->GetClient()];

                    if (rx < cr.left or ry < cr.top or rx + w > cr.right or ry + h > cr.bottom)
                        lint = "outside-client"; // normal for scrolled list rows
                }
            }

            nLint += (lint != NULL and strcmp(lint, "outside-client") != 0);
            char lintField[48] = "";

            if (lint)
                sprintf_s(lintField, sizeof(lintField), ", \"lint\": \"%s\"", lint);

            // Bitmaps: the image behind the box, and how UI95_AdaptWindow told it to fill the box.
            char imgField[96] = "";

            if (c->_GetCType_() == _CNTL_BITMAP_ or c->_GetCType_() == _CNTL_TILE_)
            {
                const bool tile = c->_GetCType_() == _CNTL_TILE_;
                IMAGE_RSC *img = tile ? ((C_Tile *)c)->GetImage() : ((C_Bitmap *)c)->GetImage();
                const short cover = tile ? ((C_Tile *)c)->GetCover() : ((C_Bitmap *)c)->GetCover();

                if (img and img->Header)
                    sprintf_s(imgField, sizeof(imgField), ", \"img\": [%d, %d, %d], \"cover\": %d", img->Header->w,
                              img->Header->h, (img->Header->flags bitand _RSC_8_BIT_) ? 8 : 16, cover);
            }

            fprintf(f, "%s\n  {\"id\": \"%s\", \"type\": %d, \"ctype\": %d, \"client\": %d, \"rect\": [%ld, %ld, %ld, %ld]%s%s%s%s}",
                    nCtl++ ? "," : "", IdName(c->GetID(), b, sizeof(b)), c->GetType(),
                    c->_GetCType_() - _CNTL_ANIMATION_, c->GetClient(), x, y, w, h,
                    invisible ? ", \"invisible\": true" : "",
                    (c->GetFlags() bitand C_BIT_ABSOLUTE) ? ", \"abs\": true" : "", lintField, imgField);
        }

        fprintf(f, "]}");
    }

    fprintf(f, "\n]\n}\n");
    gMainHandler->LeaveCritical();
    fclose(f);
    Log("OK layout %s: surface %dx%d client %ldx%ld, %d windows, %d lint (off-canvas + outside-window)", name, sw,
        sh, rc.right - rc.left, rc.bottom - rc.top, nWin, nLint);
}

static void Windows()
{
    char a[64];
    gMainHandler->EnterCritical();

    for (C_Window *win = gMainHandler->_GetFirstWindow(); win; win = gMainHandler->_GetNextWindow(win))
        if (gMainHandler->GetWindowFlags(win->GetID()) bitand C_BIT_ENABLED)
            Log("INFO window %s at %d,%d %dx%d depth %d", IdName(win->GetID(), a, sizeof(a)), win->GetX(),
                win->GetY(), win->GetW(), win->GetH(), win->GetDepth());

    gMainHandler->LeaveCritical();
}

static bool WaitFor(long id, bool shown, DWORD ms)
{
    const DWORD t0 = GetTickCount();

    while (GetTickCount() - t0 < ms and !TimedOut())
    {
        if (WindowShown(id) == shown)
            return true;

        Sleep(50);
    }

    return false;
}

static void RunLine(char *line, int lineNo)
{
    char *ctx = NULL;
    char *cmd = strtok_s(line, " \t\r\n", &ctx);

    if (!cmd or cmd[0] == '#')
        return;

    char *a1 = strtok_s(NULL, " \t\r\n", &ctx);
    char *a2 = strtok_s(NULL, " \t\r\n", &ctx);

    if (!_stricmp(cmd, "timeout") and a1)
        sTimeoutMs = (DWORD)atol(a1) * 1000;
    else if (!_stricmp(cmd, "wait") and a1)
        Sleep((DWORD)atol(a1));
    else if ((!_stricmp(cmd, "waitwin") or !_stricmp(cmd, "waitgone")) and a1)
    {
        const bool shown = !_stricmp(cmd, "waitwin");
        const long id = ParseId(a1);
        const DWORD t0 = GetTickCount();

        if (!id)
            Log("FAIL %s %s: unknown id", cmd, a1);
        else if (WaitFor(id, shown, a2 ? (DWORD)atol(a2) : 15000))
            Log("OK %s %s (%lu ms)", cmd, a1, GetTickCount() - t0);
        else
            Log("FAIL %s %s: not %s after %lu ms", cmd, a1, shown ? "shown" : "gone", GetTickCount() - t0);
    }
    else if (!_stricmp(cmd, "click") and a1)
    {
        const long id = ParseId(a1), win = a2 ? ParseId(a2) : 0;

        if (!id or (a2 and !win))
            Log("FAIL click %s %s: unknown id", a1, a2 ? a2 : "");
        else
            Click(id, win, a1);
    }
    else if (!_stricmp(cmd, "clickat") and a1 and a2)
    {
        // clickat <CTRLID> <dx> <dy>: a point inside a control's rect (tree and list rows)
        char *a3 = strtok_s(NULL, " \t\r\n", &ctx);
        const long id = ParseId(a1);
        C_Base *c = NULL;
        gMainHandler->EnterCritical();
        C_Window *win = id ? FindControlWindow(id, 0, &c) : NULL;
        long x = 0, y = 0, w = 0, h = 0;

        if (win)
            ControlRect(win, c, &x, &y, &w, &h);

        gMainHandler->LeaveCritical();

        if (!win or !a3)
            Log("FAIL clickat %s: no shown window has it", a1);
        else
        {
            PostClick(x + atol(a2), y + atol(a3));
            Log("OK clickat %s +%s,+%s = %ld,%ld", a1, a2, a3, x + atol(a2), y + atol(a3));
        }
    }
    else if (!_stricmp(cmd, "clickin") and a1 and a2)
    {
        // clickin <WINID> <x> <y>: window-relative, for popup rows (not controls of their own)
        char *a3 = strtok_s(NULL, " \t\r\n", &ctx);
        const long id = ParseId(a1);
        C_Window *win = id ? gMainHandler->FindWindow(id) : NULL;

        if (!win or !a3 or !WindowShown(id))
            Log("FAIL clickin %s: window not shown", a1);
        else
        {
            PostClick(win->GetX() + atol(a2), win->GetY() + atol(a3));
            Log("OK clickin %s %s %s", a1, a2, a3);
        }
    }
    else if ((!_stricmp(cmd, "clickxy") or !_stricmp(cmd, "rclickxy")) and a1 and a2)
    {
        PostClick(atol(a1), atol(a2), cmd[0] == 'r' or cmd[0] == 'R');
        Log("OK %s %s %s", cmd, a1, a2);
    }
    else if (!_stricmp(cmd, "hold") and a1 and a2)
    {
        // hold <CTRLID> <dy> <ms>: press dy px below the control's centre and keep the button down,
        // so the handler's repeat ticks run (panners: zoom, pan)
        char *a3 = strtok_s(NULL, " \t\r\n", &ctx);
        const long id = ParseId(a1);
        C_Base *c = NULL;
        gMainHandler->EnterCritical();
        C_Window *win = id ? FindControlWindow(id, 0, &c) : NULL;
        long x = 0, y = 0, w = 0, h = 0;

        if (win)
            ControlRect(win, c, &x, &y, &w, &h);

        gMainHandler->LeaveCritical();

        if (!win or !a3)
            Log("FAIL hold %s: no shown window has it", a1);
        else
        {
            long sx = x + w / 2, sy = y + h / 2 + atol(a2);
            SurfaceToClient(&sx, &sy);
            const LPARAM lp = MAKELPARAM((WORD)sx, (WORD)sy);
            HWND hwnd = gMainHandler->GetAppWnd();
            PostMessage(hwnd, WM_MOUSEMOVE, 0, lp);
            Sleep(50);
            extern volatile bool g_bUiTestLButtonHeld; // chandler.cpp: stands in for the real button
            g_bUiTestLButtonHeld = true;
            PostMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            Sleep((DWORD)atol(a3));
            g_bUiTestLButtonHeld = false;
            PostMessage(hwnd, WM_LBUTTONUP, 0, lp);
            Log("OK hold %s dy %s for %s ms", a1, a2, a3);
        }
    }
    else if (!_stricmp(cmd, "text") and a1)
    {
        const long id = ParseId(a1);
        C_Base *c = NULL;
        gMainHandler->EnterCritical();
        C_Window *win = id ? FindControlWindow(id, 0, &c) : NULL;
        char buf[128] = "";

        if (win and c->_GetCType_() == _CNTL_TEXT_ and ((C_Text *)c)->GetText())
            strncpy_s(buf, sizeof(buf), ((C_Text *)c)->GetText(), _TRUNCATE);

        gMainHandler->LeaveCritical();

        if (win)
            Log("INFO text %s = \"%s\"", a1, buf);
        else
            Log("FAIL text %s: no shown window has it", a1);
    }
    else if (!_stricmp(cmd, "rclickicon") and a1)
    {
        const long win = ParseId(a1);

        if (!win)
            Log("FAIL rclickicon %s: unknown window", a1);
        else
            RightClickIcon(win, a2 ? atoi(a2) : 0, a1);
    }
    else if (!_stricmp(cmd, "shot") and a1)
        Shot(a1);
    else if (!_stricmp(cmd, "wshot") and a1)
        WindowShot(a1);
    else if (!_stricmp(cmd, "layout") and a1)
        Layout(a1);
    else if (!_stricmp(cmd, "windows"))
        Windows();
    else if (!_stricmp(cmd, "log"))
        Log("INFO %s %s", a1 ? a1 : "", a2 ? a2 : "");
    else if (!_stricmp(cmd, "quit"))
        Finish("DONE");
    else
        Log("FAIL line %d: cannot parse \"%s\"", lineNo, cmd);
}

// A wait inside the game (a modal loop, a hang) must not keep the run alive.
static unsigned __stdcall WatchdogThread(void *)
{
    while (!TimedOut())
        Sleep(250);

    Finish("TIMEOUT");
    return 0;
}

static unsigned __stdcall HarnessThread(void *)
{
    char exeDir[MAX_PATH], scriptPath[MAX_PATH];
    ExeDir(exeDir, sizeof(exeDir));
    sprintf_s(sOutDir, sizeof(sOutDir), "%suitest\\out\\", exeDir);
    char uitestDir[MAX_PATH];
    sprintf_s(uitestDir, sizeof(uitestDir), "%suitest", exeDir);
    CreateDirectoryA(uitestDir, NULL);
    CreateDirectoryA(sOutDir, NULL);

    if (strchr(sScriptArg, ':'))
        strcpy_s(scriptPath, sizeof(scriptPath), sScriptArg);
    else
        sprintf_s(scriptPath, sizeof(scriptPath), "%s\\%s", uitestDir, sScriptArg);

    char logPath[MAX_PATH];
    sprintf_s(logPath, sizeof(logPath), "%sresult.log", sOutDir);
    fopen_s(&sLog, logPath, "w");
    Log("INFO script %s", scriptPath);
    LoadNames(exeDir);
    Log("INFO %u ids named", (unsigned)sNames.size());

    FILE *sf = NULL;

    if (fopen_s(&sf, scriptPath, "r") or !sf)
        Finish("SCRIPT-ERROR cannot open the script");

    _beginthreadex(NULL, 0, WatchdogThread, NULL, 0, NULL);

    char line[512];
    int lineNo = 0;

    while (fgets(line, sizeof(line), sf))
        RunLine(line, ++lineNo);

    fclose(sf);
    Finish("DONE (end of script)");
    return 0;
}

void UiTest_Start()
{
    if (!UiTest_Requested())
        return;

    InitializeCriticalSection(&sLogLock);
    sStartTick = GetTickCount();
    _beginthreadex(NULL, 0, HarnessThread, NULL, 0, NULL);
}
