// mfplay.h needs Windows 7+ declarations; the project defines an older target.
#undef WINVER
#undef _WIN32_WINNT
#undef NTDDI_VERSION
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000000
#include <iso646.h>
#include <windows.h>
#include <string.h>
#include <stdio.h>

extern "C" void F4SilenceVoices();

// Movies under the GPU backends: the DDraw player (movie/) has no surface to blit to, so the
// clip is played by Media Foundation (MFPlay) in a borderless window laid over the game window.
// MFPlay decodes H.264/AAC, so each movie is expected as <name>.mp4 beside <name>.avi.
#include <mfapi.h>
#include <mfplay.h>
#include <mferror.h>
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

namespace
{
volatile bool gMfStop = false;

LRESULT CALLBACK MovieWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_KEYUP:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        gMfStop = true; // any key || click skips the clip
        return 0;

    case WM_ERASEBKGND:
    {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    }

    return DefWindowProc(h, m, w, l);
}

struct MovieCallback : IMFPMediaPlayerCallback
{
    IMFPMediaPlayer *player = NULL;
    volatile bool done = false;
    STDMETHODIMP QueryInterface(REFIID r, void **pp)
    {
        if (r == IID_IUnknown || r == __uuidof(IMFPMediaPlayerCallback))
        {
            *pp = this;
            return S_OK;
        }

        *pp = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return 2; }   // lives on the stack of PlayMovieGpu
    STDMETHODIMP_(ULONG) Release() { return 1; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER *h)
    {
        if (FAILED(h->hrEvent))
        {
            done = true;
            return;
        }

        if (h->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET)
            player->Play();
        else if (h->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED)
            done = true;
    }
};
}

void PlayMovieMF(const char *filename, HWND game)
{
    char path[MAX_PATH];
    char *dot;
    wchar_t wpath[MAX_PATH];

    strncpy(path, filename, MAX_PATH - 5);
    path[MAX_PATH - 5] = 0;
    dot = strrchr(path, '.');

    if (dot)
    {
        strcpy(dot, ".mp4");

        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            strncpy(path, filename, MAX_PATH - 1);
    }

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
        OutputDebugStringA("movie: file not found\n");
        return;
    }

    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (FAILED(MFStartup(MF_VERSION)))
    {
        OutputDebugStringA("movie: MFStartup failed\n");

        if (SUCCEEDED(co))
            CoUninitialize();

        return;
    }

    RECT rc;
    POINT pt = {0, 0};
    GetClientRect(game, &rc);
    ClientToScreen(game, &pt);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = MovieWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "FFMovieWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND win = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "FFMovieWindow", "movie", WS_POPUP | WS_VISIBLE,
                               pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top, game, NULL, wc.hInstance, NULL);
    MovieCallback cb;
    IMFPMediaPlayer *player = NULL;

    if (win && SUCCEEDED(MFPCreateMediaPlayer(wpath, FALSE, MFP_OPTION_NONE, &cb, win, &player)) && player)
    {
        cb.player = player;
        gMfStop = false;
        SetForegroundWindow(win);
        F4SilenceVoices();
        DWORD start = GetTickCount();
        MSG msg;

        while (not cb.done && !gMfStop && GetTickCount() - start < 10 * 60 * 1000)
        {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            Sleep(5);
        }

        player->Shutdown();
        player->Release();
    }
    else
        OutputDebugStringA("movie: could not create the media player\n");

    if (win)
        DestroyWindow(win);

    MFShutdown();

    if (SUCCEEDED(co))
        CoUninitialize();

    SetForegroundWindow(game);
    SetFocus(game);
}
