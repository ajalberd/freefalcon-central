//-----------------------------------------------------------------------------
// D3D12Backend.cpp -- Artscout - 2026: #DX12 Phase 1 (see d3d12backend.h).
//
// Device + flip-model swap chain + per-frame fence. Comes up, clears a colored
// frame, Presents. Raw D3D12 (no d3dx12.h dependency). Frame-buffering fence
// logic follows the canonical MS "HelloFrameBuffering" pattern.
//-----------------------------------------------------------------------------
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h> // Phase 2: D3DCompile the 2D-quad shaders (DXBC -- D3D12 accepts SM5.x)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <map> // Artscout - 2026: #DX12 A5 -- per-RTT readback slots
// Artscout - 2026: the validation layer is no longer _DEBUG-only -- `set g_bD3D12Debug 1` arms it in ANY build
// (Release is what we actually play/test, and running it blind is what turned the hero-explosion hunt into eight
// wrong guesses). Headers must therefore be unconditional.
#include <d3d12sdklayers.h> // ID3D12Debug / ID3D12InfoQueue
#include <set> // dedupe the InfoQueue drain (PumpD3D12Messages)
#include "../include/fflog.h" // Artscout - 2026: [SHOT] screenshot trace

#include "d3d12backend.h"
#include "d3d12/d3d12texturemanager.h" // #DX12 п.3 RTT: D3D12Texture (external RT bind)
#include "d3d12/d3d12renderer.h" // #DX12 п.5: SetGScreenSize forwards to the renderer's cbViewport
#include "gt7tonemap_hlsl.h" // Artscout - 2026: HDR scene + GT7 tone mapping

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

D3D12Backend* g_pD3D12Backend = NULL;
bool g_bUseD3D12 =
    true; // Artscout - 2026: DX12 is the default backend now (override via cfg "set g_bUseD3D12 0")

// Artscout - 2026: #DX12/#104 -- "GPU render mode" = ANY modern GPU backend (D3D12 or Vulkan), as opposed to
// the dead legacy DDraw7 path. Set true in DXContext::Init for every GPU backend. The pervasive
// `if(g_bUseD3D12){GPU}else{DDraw7}` gates conflate two meanings: (a) "GPU vs dead DDraw" and (b) "the D3D12
// renderer specifically (g_pD3D12*)". Meaning-(a) gates use g_bUseGpu (so no backend falls into the
// null-deref DDraw7 else-branch); meaning-(b) gates stay on g_bUseD3D12 -- Vulkan clears g_bUseD3D12, so those
// D3D12-concrete branches are skipped and the Vulkan path is wired separately (via the neutral g_pRenderer/
// g_pRenderBackend, or a g_bUseVulkan branch). DDraw7 is never initialized in any GPU mode.
//
// DEFAULT true (#104): a GPU backend is ALWAYS used now (D3D12 or Vulkan; DDraw7 is purged) and this flag is never
// set back to false. It must already be true at the FIRST meaning-(a) gate -- e.g. DisplayDevice::Init bypasses the
// dead DDraw mode enumeration on it, which runs BEFORE DXContext::Init (where it was previously first set). Was
// false + set-on-bring-up, which made the resolution bypass miss under Vulkan (g_bUseD3D12 defaulted true and
// masked the timing; g_bUseGpu did not) -> device.cpp:96 "unavailable resolution".
bool g_bUseGpu = true;

// Artscout - 2026: #DX12 -- the active API-neutral backend (D3D11Backend or D3D12Backend). Set in
// DXContext::Init. Defined here (d3d12backend.cpp is always compiled). NULL under legacy D3D7/DDraw.
IRenderBackend* g_pRenderBackend = NULL;

// Artscout - 2026 (D3D11 purge): re-homed from the deleted d3d11backend.cpp. Per-frame sky colour written by
// otwsky (for a sky-coloured VR per-eye clear). The D3D11 backend read it; the D3D12 eye clear does not use it
// yet (currently write-only), so it lives here awaiting a D3D12 eye-clear wire-up.
float g_vrClearColor[3] = {0.0f, 0.0f, 0.0f};

// Local logging -- OutputDebugStringA only, to avoid any MonoPrint signature coupling.
static void D12Log(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    OutputDebugStringA(buf);
}

#define D12_RELEASE(p)                                                         \
    do                                                                         \
    {                                                                          \
        if (p)                                                                 \
        {                                                                      \
            (p)->Release();                                                    \
            (p) = 0;                                                           \
        }                                                                      \
    } while (0)

//=============================================================================
// Artscout - 2026: GPU frame profiler (see d3d12gpuprof.h). One slot = one command list. Timestamps are
// written on every pass-label change, resolved into a readback buffer at the end of the list, and read once
// the fence for that list has passed. Everything here is free functions/statics: no D3D12Backend member
// changes (so a plain Build is enough).
//=============================================================================
#include "d3d12gpuprof.h"
extern bool g_bGpuProf; // cfg "GpuProf" (default on)

namespace
{
const int GP_MAX = 512;    // timestamps per slot
const int GP_LABELS = 24;  // distinct pass labels

struct GpuProfState
{
    bool tried, ok, active, pending, skipSlot;
    ID3D12QueryHeap* heap;
    ID3D12Resource* readback;
    double msPerTick;
    UINT n;
    const char* lab[GP_MAX];
    UINT pendN;
    const char* pendLab[GP_MAX];
    unsigned __int64 pendFence;
    // aggregates over the report window
    const char* names[GP_LABELS];
    double sumMs[GP_LABELS];
    int nNames;
    int slots, frames;
    double busyMs, cpuWaitMs, drawSceneMs, xrWaitMs;
    long long draws, tris, chunks;
    DWORD t0;
    // CPU spans on the sim thread, summed over the window, and the same per frame for hitch attribution
    double cpuMs[GPCPU_COUNT];
    double frCpu[GPCPU_COUNT];     // this frame so far
    double frDraw, frGpuWait;      // this frame so far
    double lastTick;               // ms timestamp of the previous frame tick (0 = none yet)
    double frameSumMs, worstMs;
    double worstCpu[GPCPU_COUNT], worstDraw, worstGpuWait, worstXr;
    int framesTimed, over12, over20, over33;
};
GpuProfState gp;

double GpNowMs()
{
    static LARGE_INTEGER f = {0};
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

void GpuProfReport()
{
    const DWORD now = GetTickCount();
    const double wall = (double)(now - gp.t0);
    if (gp.slots > 0)
    {
        char b[900];
        int o = 0;
        o += _snprintf(b + o, sizeof(b) - o,
                       "[GPUPROF] %.1fs: %d slots, %d frames (%.0f fps) | GPU busy %.1f%% of wall, %.2f ms/slot | "
                       "CPU blocked on GPU %.2f ms/slot | passes (ms/slot):",
                       wall / 1000.0, gp.slots, gp.frames ? gp.frames : gp.slots,
                       wall > 0 ? (gp.frames ? gp.frames : gp.slots) * 1000.0 / wall : 0.0,
                       wall > 0 ? 100.0 * gp.busyMs / wall : 0.0,
                       gp.busyMs / gp.slots, gp.cpuWaitMs / gp.slots);
        for (int i = 0; i < gp.nNames; ++i)
            o += _snprintf(b + o, sizeof(b) - o, " %s=%.2f", gp.names[i],
                           gp.sumMs[i] / gp.slots);
        o += _snprintf(b + o, sizeof(b) - o,
                       " | draws/slot=%lld tris/slot=%lld terrainChunks/slot=%lld | CPU DrawScene %.2f ms/frame, "
                       "xrWaitFrame %.2f ms/frame\n",
                       gp.draws / gp.slots, gp.tris / gp.slots, gp.chunks / gp.slots,
                       gp.frames ? gp.drawSceneMs / gp.frames : 0.0,
                       gp.frames ? gp.xrWaitMs / gp.frames : 0.0);
        b[sizeof(b) - 1] = 0;
        FFDebugLog(b);
    }
    if (gp.framesTimed > 0)
    {
        // Where the CPU frame goes (sim thread), and the worst frame in the window with its own breakdown.
        // "other" = what no timer covers (VR submit, mirror present, the OS). xr = xrWaitFrame, i.e. idle.
        const double n = (double)gp.framesTimed;
        char b[700];
        _snprintf(b, sizeof(b) - 1,
                  "[FRAMEPROF] %d frames, avg %.2f ms | per frame: campaign-wait %.2f, realtime-fn %.2f, "
                  "sim-cycle %.2f, otw-cycle %.2f (draw-scene %.2f of it) | frames over 12ms: %d, 20ms: %d, 33ms: %d | "
                  "WORST %.1f ms = camp %.1f + realtime %.1f + sim %.1f + otw %.1f (draw %.1f, gpu-wait %.1f) + xr %.1f\n",
                  gp.framesTimed, gp.frameSumMs / n, gp.cpuMs[GPCPU_CAMP_WAIT] / n,
                  gp.cpuMs[GPCPU_REALTIME] / n, gp.cpuMs[GPCPU_SIM_CYCLE] / n,
                  gp.cpuMs[GPCPU_OTW_CYCLE] / n, gp.drawSceneMs / n, gp.over12, gp.over20,
                  gp.over33, gp.worstMs, gp.worstCpu[GPCPU_CAMP_WAIT],
                  gp.worstCpu[GPCPU_REALTIME], gp.worstCpu[GPCPU_SIM_CYCLE],
                  gp.worstCpu[GPCPU_OTW_CYCLE], gp.worstDraw, gp.worstGpuWait, gp.worstXr);
        b[sizeof(b) - 1] = 0;
        FFDebugLog(b);
    }
    for (int i = 0; i < GPCPU_COUNT; ++i)
        gp.cpuMs[i] = gp.worstCpu[i] = 0.0;
    gp.frameSumMs = gp.worstMs = gp.worstDraw = gp.worstGpuWait = gp.worstXr = 0.0;
    gp.framesTimed = gp.over12 = gp.over20 = gp.over33 = 0;
    gp.slots = gp.frames = 0;
    gp.busyMs = gp.cpuWaitMs = gp.drawSceneMs = gp.xrWaitMs = 0.0;
    gp.draws = gp.tris = gp.chunks = 0;
    for (int i = 0; i < gp.nNames; ++i)
        gp.sumMs[i] = 0.0;
    gp.t0 = now;
}

// Read the resolved timestamps of the pending slot and fold them into the aggregates.
void GpuProfCollect()
{
    if (!gp.pending || !gp.readback)
        return;
    gp.pending = false;
    const UINT n = gp.pendN;
    if (n < 2)
        return;
    D3D12_RANGE rr = {0, (SIZE_T)n * sizeof(unsigned __int64)};
    unsigned __int64* ts = 0;
    if (FAILED(gp.readback->Map(0, &rr, (void**)&ts)) || !ts)
        return;
    bool sane = true;
    for (UINT i = 1; i < n; ++i)
        if (ts[i] < ts[i - 1])
            sane = false; // different engines / reset: do not trust this slot
    if (sane)
    {
        for (UINT i = 0; i + 1 < n; ++i)
        {
            const char* l = gp.pendLab[i];
            if (!strcmp(l, "end"))
                continue;
            const double ms = (double)(ts[i + 1] - ts[i]) * gp.msPerTick;
            int k = 0;
            while (k < gp.nNames && strcmp(gp.names[k], l))
                ++k;
            if (k == gp.nNames)
            {
                if (gp.nNames >= GP_LABELS)
                    continue;
                gp.names[gp.nNames] = l;
                gp.sumMs[gp.nNames] = 0.0;
                ++gp.nNames;
            }
            gp.sumMs[k] += ms;
        }
        gp.busyMs += (double)(ts[n - 1] - ts[0]) * gp.msPerTick;
        ++gp.slots;
    }
    D3D12_RANGE none = {0, 0};
    gp.readback->Unmap(0, &none);
}

bool GpuProfInit(ID3D12Device* dev, ID3D12CommandQueue* q)
{
    if (gp.tried)
        return gp.ok;
    gp.tried = true;
    if (!dev || !q)
        return false;
    UINT64 freq = 0;
    if (FAILED(q->GetTimestampFrequency(&freq)) || freq == 0)
        return false;
    gp.msPerTick = 1000.0 / (double)freq;

    D3D12_QUERY_HEAP_DESC qd;
    ZeroMemory(&qd, sizeof(qd));
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = GP_MAX;
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&gp.heap))))
        return false;

    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = GP_MAX * sizeof(unsigned __int64);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                            IID_PPV_ARGS(&gp.readback))))
    {
        gp.heap->Release();
        gp.heap = 0;
        return false;
    }
    gp.t0 = GetTickCount();
    gp.ok = true;
    FFDebugLog("[GPUPROF] enabled (D3D12 timestamp queries)\n");
    return true;
}
} // namespace

void GpuProf_Mark(ID3D12GraphicsCommandList* cl, const char* label)
{
    if (!gp.active || gp.skipSlot || !cl || !label || gp.n >= GP_MAX - 2)
        return;
    if (gp.n > 0 && !strcmp(gp.lab[gp.n - 1], label))
        return;
    cl->EndQuery(gp.heap, D3D12_QUERY_TYPE_TIMESTAMP, gp.n);
    gp.lab[gp.n++] = label;
}

void GpuProf_Count(int draws, int triangles, int terrainChunks)
{
    if (!gp.active)
        return;
    gp.draws += draws;
    gp.tris += triangles;
    gp.chunks += terrainChunks;
}

double GpuProf_NowMs()
{
    return GpNowMs();
}

void GpuProf_AddDrawSceneMs(double ms)
{
    if (gp.ok)
    {
        gp.drawSceneMs += ms;
        gp.frDraw += ms;
    }
}

void GpuProf_AddCpu(int what, double ms)
{
    if (!gp.ok || what < 0 || what >= GPCPU_COUNT)
        return;
    gp.cpuMs[what] += ms;
    gp.frCpu[what] += ms;
}

// One call per frame, right after xrWaitFrame returned: that is the frame boundary.
void GpuProf_AddXrWaitMs(double ms)
{
    if (!gp.ok)
        return;
    gp.xrWaitMs += ms;
    ++gp.frames;

    const double now = GpNowMs();
    if (gp.lastTick > 0.0)
    {
        const double frame = now - gp.lastTick;
        ++gp.framesTimed;
        gp.frameSumMs += frame;
        if (frame > 12.0)
            ++gp.over12;
        if (frame > 20.0)
            ++gp.over20;
        if (frame > 33.0)
            ++gp.over33;
        if (frame > gp.worstMs)
        {
            gp.worstMs = frame;
            for (int i = 0; i < GPCPU_COUNT; ++i)
                gp.worstCpu[i] = gp.frCpu[i];
            gp.worstDraw = gp.frDraw;
            gp.worstGpuWait = gp.frGpuWait;
            gp.worstXr = ms;
        }
    }
    gp.lastTick = now;
    for (int i = 0; i < GPCPU_COUNT; ++i)
        gp.frCpu[i] = 0.0;
    gp.frDraw = gp.frGpuWait = 0.0;
}

// A new command list was opened. Called from BeginCommandList.
static void GpuProf_BeginSlot(ID3D12Device* dev, ID3D12CommandQueue* q,
                              ID3D12Fence* fence)
{
    gp.active = false;
    if (!g_bGpuProf || !GpuProfInit(dev, q))
        return;
    if (gp.pending && fence && fence->GetCompletedValue() >= gp.pendFence)
        GpuProfCollect();
    // The previous slot's results are not back yet (async flat path): skip this one rather than overwrite them.
    gp.skipSlot = gp.pending;
    gp.n = 0;
    gp.active = true;
}

// The list is about to be closed. Called before every Close() of a frame list.
static void GpuProf_EndSlot(ID3D12GraphicsCommandList* cl)
{
    if (!gp.active)
        return;
    gp.active = false;
    if (gp.skipSlot || gp.n == 0 || !cl)
        return;
    cl->EndQuery(gp.heap, D3D12_QUERY_TYPE_TIMESTAMP, gp.n);
    gp.lab[gp.n++] = "end";
    cl->ResolveQueryData(gp.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, gp.n, gp.readback, 0);
    gp.pendN = gp.n;
    for (UINT i = 0; i < gp.n; ++i)
        gp.pendLab[i] = gp.lab[i];
    gp.pending = true;
    gp.pendFence = (unsigned __int64)-1; // set by GpuProf_Submitted
}

// The list was executed and `fence` will signal when it retires.
static void GpuProf_Submitted(unsigned __int64 fence)
{
    if (gp.pending && gp.pendFence == (unsigned __int64)-1)
        gp.pendFence = fence;
}

// A synchronous path (VR) has waited for the fence: the data is ready now. cpuWaitMs = how long the CPU sat there.
static void GpuProf_Synced(double cpuWaitMs)
{
    if (!gp.ok)
        return;
    gp.cpuWaitMs += cpuWaitMs;
    gp.frGpuWait += cpuWaitMs;
    GpuProfCollect();
    if (GetTickCount() - gp.t0 >= 5000)
        GpuProfReport();
}

// Flat path: report from the next BeginSlot's collect.
static void GpuProf_MaybeReport()
{
    if (gp.ok && GetTickCount() - gp.t0 >= 5000)
        GpuProfReport();
}

D3D12Backend::D3D12Backend()
    : m_hWnd(0), m_nWidth(0), m_nHeight(0), m_bFullscreen(false),
      m_bRecording(false), m_pDevice(0), m_pQueue(0), m_pSwapChain(0),
      m_pRtvHeap(0), m_rtvDescSize(0), m_pDsvHeap(0), m_pDepthTex(0),
      m_pRttDepthTex(0), m_pRttDsvHeap(0), m_rttDepthW(0), m_rttDepthH(0),
      m_renderEpoch(0), m_pEyeDepthTex(0), m_pEyeDsvHeap(0), m_eyeDepthW(0),
      m_eyeDepthH(0), m_eyeDepthCur(0), m_viColorCur(0), m_viTier(-1),
      m_pList1(0), m_pMenuRtt(0), m_pMenuDepthTex(0), m_pMenuDsvHeap(0),
      m_menuRttW(0), m_menuRttH(0), m_pFpsRtt(0), m_fpsRttW(0), m_fpsRttH(0),
      m_pSubRtt(0), m_subRttW(0), m_subRttH(0),
      m_pMsaaColorTex(0), m_pMsaaRtvHeap(0), m_pMsaaDepthTex(0),
      m_pMsaaDsvHeap(0), m_msaaSamples(1), m_msaaW(0), m_msaaH(0),
      m_curSampleCount(1), m_pEyeResolveImg(0), m_curRtvPtr(0),
      m_sceneRtvPtr(0), m_sceneDsvPtr(0), m_sceneW(0), m_sceneH(0),
      m_pSceneDepthRes(0), m_sceneDepthSlices(1), m_sceneDepthMs(false),
      m_sceneDepthReadable(false), m_pDepthSrvHeap(0), m_depthSrvFor(0),
      m_pPitShadowTex(0), m_pPitShadowDsvHeap(0), m_pPitShadowSrvHeap(0),
      m_pitShadowRes(0), m_pitShadowReadable(false),
      m_pList(0), m_pFence(0), m_fenceCounter(0), m_fenceEvent(0),
      m_frameIndex(0), m_pQuadRS(0), m_pQuadPSO(0), m_pQuadPSOBlend(0),
      m_pSrvHeap(0), m_pQuadTex(0), m_pQuadUpload(0), m_quadTexW(0),
      m_quadTexH(0), m_quadRowPitch(0), m_quadTexState(0)
{
    for (int i = 0; i < kFrameCount; ++i)
    {
        m_pBackBuffer[i] = 0;
        m_pAlloc[i] = 0;
        m_allocFence[i] = 0;
    }
    for (int i = 0; i < 2; ++i)
    {
        m_eyeDepth[i].tex = 0;
        m_eyeDepth[i].dsvHeap = 0;
        m_eyeDepth[i].w = m_eyeDepth[i].h = m_eyeDepth[i].n = 0;
        m_eyeDepth[i].lru = 0;
    }
    for (int i = 0; i < 2; ++i)
    {
        m_viColor[i].tex = 0;
        m_viColor[i].rtvHeap = 0;
        m_viColor[i].arrayRtv = 0;
        m_viColor[i].sliceRtv[0] = m_viColor[i].sliceRtv[1] = 0;
        m_viColor[i].w = m_viColor[i].h = m_viColor[i].fmt = 0;
        m_viColor[i].lru = 0;
    }
    // Artscout - 2026: HDR scene + GT7 (all POD -> zero them in one go)
    ZeroMemory(&m_hdrFlat, sizeof(m_hdrFlat));
    ZeroMemory(m_hdrEye, sizeof(m_hdrEye));
    m_hdrEyeLru = 0;
    m_pHdrCur = 0;
    m_hdrSceneRtvPtr = m_hdrOutRtvPtr = 0;
    m_hdrSceneSamples = 1;
    m_hdrToneMapped = false;
    m_hdrBeginAllowed = false;
    m_pHdrRS = 0;
    m_pHdrVS = m_pHdrPS = 0;
    for (int i = 0; i < 4; ++i)
    {
        m_pHdrPso[i] = 0;
        m_hdrPsoFmt[i] = m_hdrPsoSamples[i] = 0;
    }
    m_hdrPipelineFailed = false;
    m_hdrStatFrames = m_hdrStatToneMapped = m_hdrStatTick = 0;
}

D3D12Backend::~D3D12Backend()
{
    Release();
}

bool D3D12Backend::Init(HWND hWnd, int nWidth, int nHeight, int nDepth,
                        bool bFullscreen)
{
    D12Log("D3D12Backend::Init(0x%p, %d, %d, %d, %d)\n", (void*)hWnd, nWidth,
           nHeight, nDepth, (int)bFullscreen);

    if (nWidth < 1 || nWidth > 16384 || nHeight < 1 || nHeight > 16384)
    {
        D12Log("[D3D12] Init: invalid size %dx%d -> clamping to 1024x768\n",
               nWidth, nHeight);
        nWidth = 1024;
        nHeight = 768;
    }

    if (m_pDevice) // mode change (re-entry): reuse the device, just resize
    {
        m_hWnd = hWnd;
        if (nWidth != m_nWidth || nHeight != m_nHeight)
            Resize(nWidth, nHeight);
        D12Log(
            "D3D12Backend::Init - already initialized, reusing the device\n");
        return true;
    }

    m_hWnd = hWnd;
    m_nWidth = nWidth;
    m_nHeight = nHeight;
    m_bFullscreen = bFullscreen;

    // Artscout - 2026: the validation layer used to be _DEBUG-only, so a RELEASE build (what we actually play and
    // test in) ran BLIND -- during the hero-explosion hunt that cost eight wrong hypotheses read off the source
    // while the runtime could have named the fault outright. `set g_bD3D12Debug 1` in FFViper.cfg now enables it
    // in ANY build; messages are pumped into the D3D12 log (see PumpD3D12Messages, called once per Present).
    // Default OFF: validation costs frame time and must never be on for normal play.
    extern bool g_bD3D12Debug;
    if (g_bD3D12Debug)
    {
        // Load the debug interface DYNAMICALLY -- avoids a link-time D3D12GetDebugInterface dependency
        // (some SDK/import-lib configs don't export it into the app's d3d12.lib, causing LNK2019 even
        // though D3D12CreateDevice links). The validation layer is preserved.
        typedef HRESULT(WINAPI * PFN_D3D12_GET_DEBUG_INTERFACE)(REFIID, void**);
        HMODULE hD12 = GetModuleHandleA("d3d12.dll");
        if (!hD12)
            hD12 = LoadLibraryA("d3d12.dll");
        if (hD12)
        {
            PFN_D3D12_GET_DEBUG_INTERFACE pGetDbg =
                (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(
                    hD12, "D3D12GetDebugInterface");
            ID3D12Debug* dbg = 0;
            if (pGetDbg && SUCCEEDED(pGetDbg(IID_PPV_ARGS(&dbg))) && dbg)
            {
                dbg->EnableDebugLayer();
                dbg->Release();
                D12Log(
                    "[D3D12DBG] validation layer ENABLED (g_bD3D12Debug=1)\n");
            }
            else
                D12Log("[D3D12DBG] validation layer requested but unavailable "
                       "(install the Graphics Tools feature)\n");
        }
    }

    // --- device (feature level 11_0 minimum, chosen or default adapter) ---
    // Artscout - 2026 (#89): honour the video-card selector. GetSelectedDxgiAdapter() (devmgr.cpp) returns the
    // DispVideoCard-th hardware DXGI adapter (AddRef'd) or NULL -> D3D12CreateDevice(NULL) = default adapter.
    extern struct IDXGIAdapter1* GetSelectedDxgiAdapter();
    IDXGIAdapter1* pChosenAdapter = GetSelectedDxgiAdapter();
    HRESULT hr = D3D12CreateDevice(pChosenAdapter, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&m_pDevice));
    if ((FAILED(hr) || !m_pDevice) &&
        pChosenAdapter) // chosen adapter failed -> retry on default
    {
        D12Log("[D3D12] D3D12CreateDevice(chosen adapter) failed 0x%08X, "
               "retrying default\n",
               (unsigned)hr);
        if (m_pDevice)
        {
            m_pDevice->Release();
            m_pDevice = NULL;
        }
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(&m_pDevice));
    }
    if (pChosenAdapter)
        pChosenAdapter->Release();
    if (FAILED(hr) || !m_pDevice)
    {
        D12Log("[D3D12] D3D12CreateDevice failed 0x%08X\n", (unsigned)hr);
        return false;
    }

    // #DX12: with the validation layer on, the InfoQueue can be configured to BREAK on messages. A break per
    // WARNING turns benign spam (#1328 COPY_DEST-ignored, etc.) into a _com_error + debugger halt on EVERY call
    // -> hundreds of stalls at scene load -> the GPU falls behind and the TDR watchdog removes the device
    // (DEVICE_HUNG). So: never break (we READ the queue in PumpD3D12Messages instead), and mute the harmless
    // #1328. Kept alive in Release too so `set g_bD3D12Debug 1` gives a usable log without a debugger attached.
    if (g_bD3D12Debug)
    {
        ID3D12InfoQueue* iq = 0;
        if (SUCCEEDED(m_pDevice->QueryInterface(IID_PPV_ARGS(&iq))) && iq)
        {
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_INFO, FALSE);
            // Artscout - 2026: the validation layer is DEVICE-wide, so it also reports the OpenXR (PVR) runtime's
            // own D3D12 usage on our shared device -- and that runtime is sloppy. Its compositor reads our eye
            // images back ('App Swapchain Texture[...]', an unnamed command list -- ours is named "FF-Main" and
            // never binds an XR image as an SRV) and draws into an _SRGB target with a UNORM pipeline. That is
            // ~9.7k messages in a single session (6509x #538 + 3255x #613), which buries every message of OURS.
            // Nothing here is actionable for us, so deny the three IDs; drop them from this list if a genuinely
            // ours-looking #538/#613/#552 is ever suspected.
            D3D12_MESSAGE_ID deny[] = {
                D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED,
                D3D12_MESSAGE_ID_INVALID_SUBRESOURCE_STATE, // #538  (PVR compositor)
                D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE, // #613  (PVR compositor)
                D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_SYNC, // #552  (PVR compositor)
            };
            D3D12_INFO_QUEUE_FILTER filter;
            ZeroMemory(&filter, sizeof(filter));
            filter.DenyList.NumIDs = (UINT)(sizeof(deny) / sizeof(deny[0]));
            filter.DenyList.pIDList = deny;
            iq->AddStorageFilterEntries(&filter);
            iq->SetMuteDebugOutput(FALSE);
            iq->Release();
            D12Log("[D3D12DBG] InfoQueue armed: ERROR/CORRUPTION/WARNING are "
                   "logged, never break\n");
        }
    }

    // --- direct command queue ---
    D3D12_COMMAND_QUEUE_DESC qd;
    ZeroMemory(&qd, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_pDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_pQueue));
    if (FAILED(hr))
    {
        D12Log("[D3D12] CreateCommandQueue failed 0x%08X\n", (unsigned)hr);
        return false;
    }

    // --- flip swap chain via DXGI factory ---
    IDXGIFactory4* factory = 0;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory)
    {
        D12Log("[D3D12] CreateDXGIFactory2 failed 0x%08X\n", (unsigned)hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd;
    ZeroMemory(&scd, sizeof(scd));
    scd.Width = nWidth;
    scd.Height = nHeight;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferCount = kFrameCount;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1; // no MSAA in Phase 1
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    IDXGISwapChain1* sc1 = 0;
    hr =
        factory->CreateSwapChainForHwnd(m_pQueue, hWnd, &scd, NULL, NULL, &sc1);
    if (FAILED(hr) || !sc1)
    {
        D12Log("[D3D12] CreateSwapChainForHwnd failed 0x%08X\n", (unsigned)hr);
        factory->Release();
        return false;
    }
    factory->MakeWindowAssociation(
        hWnd, DXGI_MWA_NO_ALT_ENTER); // we manage fullscreen
    hr = sc1->QueryInterface(IID_PPV_ARGS(&m_pSwapChain));
    sc1->Release();
    factory->Release();
    if (FAILED(hr) || !m_pSwapChain)
    {
        D12Log("[D3D12] QI IDXGISwapChain3 failed 0x%08X\n", (unsigned)hr);
        return false;
    }
    m_frameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

    // --- RTV descriptor heap (one per back buffer) ---
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    ZeroMemory(&hd, sizeof(hd));
    hd.NumDescriptors = kFrameCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = m_pDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_pRtvHeap));
    if (FAILED(hr))
    {
        D12Log("[D3D12] CreateDescriptorHeap(RTV) failed 0x%08X\n",
               (unsigned)hr);
        return false;
    }
    m_rtvDescSize = m_pDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!CreateBackBufferViews())
        return false;
    if (!CreateDepthBuffer())
        return false; // Artscout - 2026: #DX12 Phase 3 -- scene depth

    // --- per-frame command allocators + one command list (created closed) ---
    for (int i = 0; i < kFrameCount; ++i)
    {
        hr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&m_pAlloc[i]));
        if (FAILED(hr))
        {
            D12Log("[D3D12] CreateCommandAllocator[%d] failed 0x%08X\n", i,
                   (unsigned)hr);
            return false;
        }
    }
    hr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      m_pAlloc[m_frameIndex], NULL,
                                      IID_PPV_ARGS(&m_pList));
    if (FAILED(hr))
    {
        D12Log("[D3D12] CreateCommandList failed 0x%08X\n", (unsigned)hr);
        return false;
    }
    m_pList->SetName(L"FF-Main"); // name the command list
    m_pList->Close();

    // --- fence + event ---
    // Artscout - 2026: #DX12 -- the fence starts at 0 and only ever climbs (SignalQueue); m_allocFence[] stays 0
    // until an allocator's work is actually submitted, so the first Reset of each never waits.
    m_fenceCounter = 0;
    hr = m_pDevice->CreateFence(m_fenceCounter, D3D12_FENCE_FLAG_NONE,
                                IID_PPV_ARGS(&m_pFence));
    if (FAILED(hr))
    {
        D12Log("[D3D12] CreateFence failed 0x%08X\n", (unsigned)hr);
        return false;
    }
    m_fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!m_fenceEvent)
    {
        D12Log("[D3D12] CreateEvent failed\n");
        return false;
    }

    WaitForGpu(); // clean start

    D12Log("D3D12Backend::Init succeeded (%dx%d)\n", nWidth, nHeight);
    return true;
}

bool D3D12Backend::CreateBackBufferViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < kFrameCount; ++i)
    {
        HRESULT hr =
            m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&m_pBackBuffer[i]));
        if (FAILED(hr) || !m_pBackBuffer[i])
        {
            D12Log("[D3D12] GetBuffer[%d] failed 0x%08X\n", i, (unsigned)hr);
            return false;
        }
        m_pDevice->CreateRenderTargetView(m_pBackBuffer[i], NULL, h);
        h.ptr += m_rtvDescSize;
    }
    return true;
}

void D3D12Backend::ReleaseBackBufferViews()
{
    for (int i = 0; i < kFrameCount; ++i)
        D12_RELEASE(m_pBackBuffer[i]);
}

// Artscout - 2026: #DX12 Phase 3 -- (re)create the scene depth-stencil (D32) sized to the back buffer.
bool D3D12Backend::CreateDepthBuffer()
{
    ReleaseDepthBuffer();
    if (!m_pDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_pDevice->CreateDescriptorHeap(&hd,
                                                   IID_PPV_ARGS(&m_pDsvHeap))))
        {
            D12Log("[D3D12] DSV heap failed\n");
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)(m_nWidth < 1 ? 1 : m_nWidth);
    rd.Height = (UINT)(m_nHeight < 1 ? 1 : m_nHeight);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv;
    ZeroMemory(&cv, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 0.0f;
    cv.DepthStencil.Stencil = 0;

    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &cv, IID_PPV_ARGS(&m_pDepthTex))))
    {
        D12Log("[D3D12] depth create failed\n");
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    ZeroMemory(&dv, sizeof(dv));
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_pDevice->CreateDepthStencilView(
        m_pDepthTex, &dv, m_pDsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void D3D12Backend::ReleaseDepthBuffer()
{
    D12_RELEASE(m_pDepthTex);
}

unsigned __int64 D3D12Backend::DepthDsvPtr() const
{
    return m_pDsvHeap ? (unsigned __int64)m_pDsvHeap
                            ->GetCPUDescriptorHandleForHeapStart()
                            .ptr :
                        0;
}
int D3D12Backend::BackBufferFormat()
{
    return (int)DXGI_FORMAT_R8G8B8A8_UNORM;
}
int D3D12Backend::DepthFormat()
{
    return (int)DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

void D3D12Backend::BeginFrame(unsigned long argb)
{
    if (!m_pDevice || !m_pList)
        return;

    BeginCommandList();
    m_pHdrCur = 0; // Artscout - 2026: HDR is re-decided per frame (below)
    m_renderEpoch++; // #DX12 п.5: new render epoch -> the renderer resets its per-frame rings
    // Artscout - 2026 (#65 perf): advance the texture pool's frame clock so freed placed-resource regions become
    // reusable only after the GPU is guaranteed done with the texture that held them.
    extern void D3D12TexMgr_TickFrame(unsigned renderEpoch);
    D3D12TexMgr_TickFrame(m_renderEpoch);

    // back buffer: PRESENT -> RENDER_TARGET
    D3D12_RESOURCE_BARRIER b;
    ZeroMemory(&b, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = m_pBackBuffer[m_frameIndex];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_pList->ResourceBarrier(1, &b);

    // Artscout - 2026: HDR scene + GT7 -- a renderer-opened frame draws into the FP16 scene (MSAA or not);
    // the back buffer receives it in OutputHdrScene (from ResolveMsaaToBackBuffer / Present). Any failure
    // falls through to the unchanged 8-bit paths below.
    extern bool g_bToneMapGT7;
    if (g_bToneMapGT7 && m_hdrBeginAllowed)
    {
        const bool msaa = CreateMsaaTargets(m_nWidth, m_nHeight);
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv =
            m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
        bbRtv.ptr += (SIZE_T)m_frameIndex * m_rtvDescSize;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        dsv.ptr = 0;
        if (msaa)
            dsv = m_pMsaaDsvHeap->GetCPUDescriptorHandleForHeapStart();
        else if (m_pDsvHeap)
            dsv = m_pDsvHeap->GetCPUDescriptorHandleForHeapStart();
        if (BeginHdrScene(m_hdrFlat, m_nWidth, m_nHeight,
                          msaa ? m_msaaSamples : 1,
                          (unsigned __int64)bbRtv.ptr, argb,
                          (unsigned __int64)dsv.ptr))
        {
            // Same depth bookkeeping as BindMsaaScene / the single-sample path below.
            m_pSceneDepthRes = msaa ? m_pMsaaDepthTex : m_pDepthTex;
            m_sceneDepthSlices = 1;
            m_sceneDepthMs = msaa;
            m_sceneDepthReadable = false;
            m_bRecording = true;
            return;
        }
    }

    // Artscout - 2026: MSAA -- render the scene into the MSAA target instead of the backbuffer; the backbuffer
    // (already PRESENT->RENDER_TARGET above) receives the resolved image later in ResolveMsaaToBackBuffer,
    // then the UI composites over it. Off/unsupported -> falls through to the single-sample backbuffer path.
    if (CreateMsaaTargets(m_nWidth, m_nHeight))
    {
        BindMsaaScene(argb, m_nWidth, m_nHeight);
        m_bRecording = true;
        return;
    }
    m_curSampleCount = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)m_frameIndex * m_rtvDescSize;
    m_curRtvPtr =
        (unsigned __int64)rtv.ptr; // #DX12: back buffer is the current RTV
    // Artscout - 2026: #DX12 Phase 3 -- bind + clear the scene depth buffer alongside the RTV.
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_pDsvHeap ? m_pDsvHeap->GetCPUDescriptorHandleForHeapStart() :
                     D3D12_CPU_DESCRIPTOR_HANDLE();
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, m_pDsvHeap ? &dsv : NULL);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(m_pDsvHeap != 0);
    // #DX12 п.5: this frame's scene target is the back buffer (VR overrides it per eye in BeginEyeFrame).
    m_sceneRtvPtr = (unsigned __int64)rtv.ptr;
    m_sceneDsvPtr = m_pDsvHeap ? (unsigned __int64)dsv.ptr : 0;
    // Artscout - 2026: #13 -- record WHICH depth this pass bound, so SceneDepthSrvCpu can view it for the clouds.
    m_pSceneDepthRes = m_pDepthTex;
    m_sceneDepthSlices = 1;
    m_sceneDepthMs = false;
    m_sceneDepthReadable = false;
    m_sceneW = m_nWidth;
    m_sceneH = m_nHeight;

    const float clear[4] = {
        ((argb >> 16) & 0xFF) / 255.0f, // R
        ((argb >> 8) & 0xFF) / 255.0f, // G
        (argb & 0xFF) / 255.0f, // B
        ((argb >> 24) & 0xFF) / 255.0f // A
    };
    m_pList->ClearRenderTargetView(rtv, clear, 0, NULL);
    if (m_pDsvHeap)
        m_pList->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0,
            NULL);

    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)m_nWidth;
    vp.Height = (FLOAT)m_nHeight;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = m_nWidth;
    sc.bottom = m_nHeight;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);

    m_bRecording = true;
}

// Artscout - 2026: drain the validation InfoQueue into the D3D12 log. Called once per Present so a RELEASE build
// with `set g_bD3D12Debug 1` reports faults WITHOUT a debugger attached -- the whole point being that a bad draw
// (wrong state, dropped by the runtime, resource used while in the wrong state) is named by D3D itself instead of
// being guessed at from the source. Deduped by (id,severity) so a per-frame fault logs once, not 90 times/sec.
static void PumpD3D12Messages(ID3D12Device* dev)
{
    extern bool g_bD3D12Debug;
    if (!g_bD3D12Debug || !dev)
        return;
    ID3D12InfoQueue* iq = 0;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&iq))) || !iq)
        return;

    static std::set<unsigned __int64> s_seen;
    const UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i)
    {
        SIZE_T len = 0;
        if (FAILED(iq->GetMessage(i, NULL, &len)) || !len)
            continue;
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (!m)
            continue;
        if (SUCCEEDED(iq->GetMessage(i, m, &len)))
        {
            const unsigned __int64 key =
                ((unsigned __int64)m->ID << 4) | (unsigned)m->Severity;
            if (s_seen.insert(key).second)
            {
                const char* sev =
                    (m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) ?
                        "CORRUPTION" :
                    (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR) ? "ERROR" :
                    (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ?
                                                                    "WARNING" :
                                                                    "INFO";
                D12Log("[D3D12DBG] %s #%d: %.*s\n", sev, (int)m->ID,
                       (int)m->DescriptionByteLength, m->pDescription);
            }
        }
        free(m);
    }
    iq->ClearStoredMessages();
    iq->Release();
}

// Artscout - 2026: a screenshot that actually contains the rendered frame.
//
// OTWDriverClass::TakeScreenShot goes to ImageBuffer::BackBufferToRAW, which Lock()s the image
// buffer and walks it -- and under the GPU backends Lock() returns m_pSysMem, the CPU-side RGB565
// surface. The 3D scene is rendered on the GPU and never lands there, so what that path writes is
// whatever 2D happens to be in system memory, not the picture on screen. In a VR session it is
// worse than useless: the eye images never touch that buffer at all.
//
// Read the swap-chain back buffer instead, which is also the VR mirror -- with XrMirror on it holds
// the eye the compositor was handed, so this is a VR screenshot without a separate capture path.
//
// Deliberately deferred to Present rather than run where the key is pressed: only here is the frame
// finished, the command list submitted, and the back buffer's identity and state known (PRESENT,
// from the barrier above). Capturing from the key handler would race whatever the frame was doing.
//
// One-shot allocator and list, then a full WaitForGpu. That is a hard stall of a few milliseconds
// and completely wrong for anything per-frame -- fine for a keypress, and it keeps the capture from
// touching the frame ring or the backend's own fence bookkeeping.
// Artscout - 2026: open a swap-chain frame for a MENU that is about to render 3D, if one is not
// already open. Needed because ImageBuffer::BindD3D12RenderTarget refuses to bind an off-screen RTT
// off-frame -- deliberately, since forcing a BeginFrame from the sim update or between VR eye frames
// injected an orphan frame and broke xrEndFrame. A menu has the opposite problem: UI95 draws into a
// CPU surface and only opens a GPU frame inside PresentGpu, at the very end, so when a 3D viewer
// runs during the UI's draw pass there is no list open, the RTT bind is skipped, and the model is
// drawn somewhere other than the texture that is about to be read back.
//
// Safe here precisely because it is the menu loop: PresentGpu closes and presents whatever is open a
// moment later. Guarded on IsRecording so it can never reset a list that already holds work.
bool D3D12_EnsureMenuFrame()
{
    if (!g_pD3D12Backend)
        return false;

    if (g_pD3D12Backend->IsRecording())
        return true;

    g_pD3D12Backend->BeginFrame(0xFF000000);
    return g_pD3D12Backend->IsRecording();
}

static char s_capturePath[MAX_PATH] = {0};

bool D3D12_RequestScreenCapture(const char* path)
{
    if (!path || !*path)
        return false;

    if (s_capturePath[0])
        return false; // one already queued for the next Present

    strncpy(s_capturePath, path, MAX_PATH - 1);
    s_capturePath[MAX_PATH - 1] = 0;
    return true;
}

static void WriteBmp24(const char* path, const BYTE* src, unsigned rowPitch,
                       int w, int h, bool bgra)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return;

    // BMP scanlines are 4-byte aligned and stored bottom-up.
    const unsigned stride = (unsigned)((w * 3 + 3) & ~3);
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    ZeroMemory(&bfh, sizeof(bfh));
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = stride * h;
    bfh.bfType = 0x4d42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + bih.biSizeImage;

    DWORD wrote = 0;
    WriteFile(f, &bfh, sizeof(bfh), &wrote, NULL);
    WriteFile(f, &bih, sizeof(bih), &wrote, NULL);

    BYTE* row = new BYTE[stride];

    if (row)
    {
        ZeroMemory(row, stride);

        for (int y = h - 1; y >= 0; --y)
        {
            const BYTE* s = src + (size_t)y * rowPitch;
            BYTE* d = row;

            for (int x = 0; x < w; ++x)
            {
                // BMP wants BGR. A BGRA source is already in that order; an RGBA one is reversed.
                if (bgra)
                {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                }
                else
                {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                }

                d += 3;
                s += 4;
            }

            WriteFile(f, row, stride, &wrote, NULL);
        }

        delete[] row;
    }

    CloseHandle(f);
}

// Artscout - 2026 (VR screenshots): how many Presents the desktop path will wait for the VR eye
// path to claim a pending request before taking it itself. Without this a request made while a
// VR session is up but NOT rendering stereo (the head-locked menu quad, which never calls
// EndEyeFrame) would sit queued forever and the key would look dead.
static int s_captureAge = 0;

// Artscout - 2026 (VR screenshots): one line per screenshot to FFDebug.log, always on. This
// path is a keypress, so the cost is nil, and every way it can fail -- wrong format, no eye
// frame, the request claimed by the path that cannot see the picture -- is otherwise silent
// and produces either nothing at all or a black file, which look identical from the outside.
static void ShotLog(const char* fmt, ...)
{
    char body[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';

    char ln[512];
    _snprintf(ln, sizeof(ln), "[SHOT] %s\n", body);
    ln[sizeof(ln) - 1] = '\0';
    FFDebugLog(ln);
}

void D3D12Backend::ServiceScreenCapture()
{
    if (!s_capturePath[0] || !m_pDevice || !m_pQueue)
        return;

    // In a VR session the desktop back buffer holds only the BeginFrame clear -- the eyes go
    // straight to the compositor's own swapchains and nothing ever mirrors them here. Let
    // EndEyeFrame claim this request and capture the real eye instead.
    {
        extern bool g_bVrFrameActive;
        extern bool g_bXrMirror;

        if (g_bVrFrameActive && g_bXrMirror && ++s_captureAge < 8)
            return; // still pending; the eye path gets first refusal

        if (g_bVrFrameActive)
            ShotLog("desktop path taking a VR request after %d frames -- no eye "
                    "claimed it; expect a black frame",
                    s_captureAge);
    }

    s_captureAge = 0;

    char path[MAX_PATH];
    strncpy(path, s_capturePath, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    s_capturePath[0] = 0; // consume it whatever happens below, so a failure cannot wedge the key

    ID3D12Resource* bb = m_pBackBuffer[m_frameIndex];

    if (!bb)
        return;

    CaptureTextureToBmp(bb, 0, (int)D3D12_RESOURCE_STATE_PRESENT, path);
}

// Artscout - 2026 (VR screenshots): the eye image is complete and still ours here -- the runtime
// takes it back at xrReleaseSwapchainImage, which ReleaseEyes does after both eyes. BeginEyeFrame
// deliberately never transitions it, so it is exactly where the runtime handed it over:
// RENDER_TARGET. We transition to COPY_SOURCE and back, which is well defined from a state we
// know, unlike the COMMON->RT barrier that #DX12 п.5 had to avoid.
//
// Only the first eye of the frame claims the request, so a screenshot is the left eye.
void D3D12Backend::ServiceEyeCapture(void* eyeImg)
{
    extern bool g_bXrMirror;

    if (!s_capturePath[0] || !eyeImg || !g_bXrMirror)
        return;

    char path[MAX_PATH];
    strncpy(path, s_capturePath, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    s_capturePath[0] = 0;
    s_captureAge = 0;

    ShotLog("eye capture claiming request -> %s", path);
    CaptureTextureToBmp((ID3D12Resource*)eyeImg, 0,
                        (int)D3D12_RESOURCE_STATE_RENDER_TARGET, path);
}

bool D3D12Backend::CaptureTextureToBmp(ID3D12Resource* src, unsigned subresource,
                                       int stateBefore, const char* path)
{
    if (!src || !m_pDevice || !m_pQueue || !path || !*path)
        return false;

    D3D12_RESOURCE_DESC bd = src->GetDesc();
    const int w = (int)bd.Width;
    const int h = (int)bd.Height;

    if (w < 1 || h < 1)
        return false;

    // The format we copy WITH, which is not always the one the resource reports. An OpenXR
    // runtime commonly hands out TYPELESS colour images so the app can choose an sRGB or a
    // UNORM view -- openxrbackend.cpp demotes _SRGB to _UNORM when it builds the eye RTV for
    // exactly that reason -- and GetDesc() then reports the typeless family, which is not a
    // legal placed-footprint format. Copying with the concrete UNORM member of the same
    // family is, and the bytes are identical either way.
    DXGI_FORMAT copyFmt;
    bool bgra = false;

    switch (bd.Format)
    {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        copyFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        bgra = true;
        break;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        copyFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;

    default:
        ShotLog("capture DECLINED: unsupported format %d (%dx%d)",
                (int)bd.Format, w, h);
        return false;
    }

    const unsigned rowPitch = (unsigned)(((unsigned)w * 4u + 255u) & ~255u);

    ID3D12Resource* rb = 0;
    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (UINT64)rowPitch * (UINT64)h;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
            __uuidof(ID3D12Resource), (void**)&rb)) ||
        !rb)
        return false;

    bool wrote = false;
    ID3D12CommandAllocator* alloc = 0;
    ID3D12GraphicsCommandList* list = 0;

    if (SUCCEEDED(m_pDevice->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
            (void**)&alloc)) &&
        alloc &&
        SUCCEEDED(m_pDevice->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
            __uuidof(ID3D12GraphicsCommandList), (void**)&list)) &&
        list)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)stateBefore;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dstL, srcL;
        ZeroMemory(&dstL, sizeof(dstL));
        dstL.pResource = rb;
        dstL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstL.PlacedFootprint.Offset = 0;
        dstL.PlacedFootprint.Footprint.Format = copyFmt;
        dstL.PlacedFootprint.Footprint.Width = (UINT)w;
        dstL.PlacedFootprint.Footprint.Height = (UINT)h;
        dstL.PlacedFootprint.Footprint.Depth = 1;
        dstL.PlacedFootprint.Footprint.RowPitch = rowPitch;
        ZeroMemory(&srcL, sizeof(srcL));
        srcL.pResource = src;
        srcL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcL.SubresourceIndex = (UINT)subresource;
        list->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, NULL);

        // Hand it back in the state the caller guaranteed, or the next barrier on it is a lie.
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = (D3D12_RESOURCE_STATES)stateBefore;
        list->ResourceBarrier(1, &b);

        if (SUCCEEDED(list->Close()))
        {
            ID3D12CommandList* lists[] = {(ID3D12CommandList*)list};
            m_pQueue->ExecuteCommandLists(1, lists);
            WaitForGpu();

            void* mapped = 0;

            if (SUCCEEDED(rb->Map(0, NULL, &mapped)) && mapped)
            {
                WriteBmp24(path, (const BYTE*)mapped, rowPitch, w, h, bgra);
                wrote = true;
                D3D12_RANGE wr;
                wr.Begin = 0;
                wr.End = 0;
                rb->Unmap(0, &wr);
            }
        }
    }

    if (list)
        list->Release();

    if (alloc)
        alloc->Release();

    rb->Release();
    ShotLog("%s %dx%d fmt=%d copyFmt=%d bgra=%d -> %s", path, w, h,
            (int)bd.Format, (int)copyFmt, (int)bgra,
            wrote ? "written" : "FAILED (nothing on disk)");
    return wrote;
}

void D3D12Backend::Present(bool bVSync)
{
    if (!m_pDevice || !m_pList || !m_pSwapChain)
        return;
    PumpD3D12Messages(m_pDevice);

    if (m_bRecording)
    {
        // Artscout - 2026: an HDR frame nobody resolved (no UI composite this frame) still has to reach
        // the back buffer. No-op otherwise.
        OutputHdrScene();
        // back buffer: RENDER_TARGET -> PRESENT
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pBackBuffer[m_frameIndex];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_pList->ResourceBarrier(1, &b);
        GpuProf_EndSlot(m_pList);
        m_pList->Close();

        ID3D12CommandList* lists[] = {(ID3D12CommandList*)m_pList};
        // Artscout - 2026 (#65 perf): GPU-side wait so async texture/VB uploads submitted this frame are
        // resident before the render queue draws them (replaces the old per-upload CPU wait).
        extern void D3D12TexMgr_SyncRenderQueue(struct ID3D12CommandQueue * q);
        D3D12TexMgr_SyncRenderQueue(m_pQueue);
        m_pQueue->ExecuteCommandLists(1, lists);
        GpuProf_Submitted(SignalQueue()); // flat path: results are collected by a later BeginCommandList
        GpuProf_MaybeReport();
        m_bRecording = false;
    }

    // Artscout - 2026: while a VR session is presenting, the DESKTOP MIRROR must not vsync. Every caller asks for
    // vsync (correct on a monitor), but in VR the headset compositor already paces the loop through xrWaitFrame --
    // and the mirror's SyncInterval=1 then caps the WHOLE frame loop at the desktop's refresh, which has nothing to
    // do with the headset's. Measured: a 60 Hz desktop pinned CPU_FRAME at 16.58ms with only 1.8ms of CPU work, so
    // the headset ran permanently reprojected -- head-locked content smooth, world-locked terrain and buildings
    // juddering. Nobody is looking at the mirror; let it tear.
    bool vsync = bVSync;
    {
        extern bool g_bVrFrameActive;   // presenting stereo this frame
        extern bool g_bVsyncVrMirror;   // cfg escape hatch, default off
        if (g_bVrFrameActive && !g_bVsyncVrMirror)
            vsync = false;
    }

    // Artscout - 2026: the back buffer is finished and still ours until Present hands it over.
    ServiceScreenCapture();

    m_pSwapChain->Present(vsync ? 1 : 0, 0);
    MoveToNextFrame();
}

// Artscout - 2026: #DX12 -- one strictly-increasing counter feeds EVERY signal on the render queue, so a fence
// value can never be re-posted or go backwards (the old per-index m_fenceValue[] could, once the VR paths began
// bumping it mid-frame). Returns the value posted; the GPU reaching it means all work submitted so far is retired.
unsigned __int64 D3D12Backend::SignalQueue()
{
    if (!m_pQueue || !m_pFence)
        return m_fenceCounter;
    ++m_fenceCounter;
    m_pQueue->Signal(m_pFence, m_fenceCounter);
    return m_fenceCounter;
}

void D3D12Backend::WaitForFence(unsigned __int64 v)
{
    if (!m_pFence || !m_fenceEvent || v == 0)
        return;
    if (m_pFence->GetCompletedValue() >= v)
        return;
    m_pFence->SetEventOnCompletion(v, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

// Artscout - 2026: #DX12 -- THE allocator-recycle gate (fixes debug-layer ERROR #552). Every path that opens a
// command list goes through here, so an allocator is only ever Reset after the GPU has retired the work recorded
// from it. Previously each Begin*Frame reset m_pAlloc[m_frameIndex] outright, trusting that MoveToNextFrame had
// already waited for that index -- which the VR paths (keyed off a swapchain index they never present) broke.
void D3D12Backend::BeginCommandList()
{
    if (!m_pDevice || !m_pList)
        return;
    // A list left open by an early-returning End*Frame still owns its allocator's memory: close (discarding the
    // unsubmitted work) so the Reset below is legal instead of corrupting a live allocator.
    if (m_bRecording)
    {
        // Artscout - 2026: a list that was still open is being thrown away UNSUBMITTED. Whatever was recorded in it
        // is lost -- if that included the terrain, that eye shows no ground for the frame. Say what it held.
        static int s_discards = 0;
        if (s_discards < 80)
        {
            ++s_discards;
            char ln[400];
            int o = _snprintf(ln, sizeof(ln) - 1, "[LIST-DISCARD] t=%lu open list dropped, recorded passes:", GetTickCount());
            for (UINT i = 0; i < gp.n && o < (int)sizeof(ln) - 40; ++i)
                o += _snprintf(ln + o, sizeof(ln) - 1 - o, " %s", gp.lab[i]);
            o += _snprintf(ln + o, sizeof(ln) - 1 - o, " (%u marks, active=%d)\n", gp.n, (int)gp.active);
            ln[sizeof(ln) - 1] = 0;
            FFDebugLog(ln);
        }
        m_pList->Close();
        m_bRecording = false;
    }
    WaitForFence(m_allocFence[m_frameIndex]);
    m_pAlloc[m_frameIndex]->Reset();
    m_pList->Reset(m_pAlloc[m_frameIndex], NULL);
    GpuProf_BeginSlot(m_pDevice, m_pQueue, m_pFence);
    GpuProf_Mark(m_pList, "setup"); // clears, HDR bind, barriers before the first pass
}

void D3D12Backend::WaitForGpu()
{
    if (!m_pQueue || !m_pFence)
        return;
    WaitForFence(
        SignalQueue()); // drains everything, so every m_allocFence[] is now satisfied too
}

//============================ #13 scene depth as an SRV ======================
// Artscout - 2026: the cloud raymarch reads the scene depth to know where the world cuts each ray short. It has
// to: the rasterizer's depth test can only compare ONE depth per pixel, which is meaningless for a volume the
// camera sits inside. Depth WRITE is off in the cloud pass, so DEPTH_READ|PIXEL_SHADER_RESOURCE is legal.
unsigned __int64 D3D12Backend::SceneDepthSrvCpu()
{
    // Only while the depth is actually transitioned for reading -- otherwise it is still DEPTH_WRITE and the
    // debug layer would (rightly) flag the bind.
    if (!m_sceneDepthReadable || !m_pDevice || !m_pSceneDepthRes)
        return 0;
    // Flat MSAA: a multisampled depth cannot be viewed as Texture2DArray. The cloud shader has ONE view type
    // (see the header), so no clamp there -- the clouds still draw, they just do not know about the terrain.
    if (m_sceneDepthMs)
        return 0;

    if (!m_pDepthSrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU staging: FlushConstants copies it into the shader ring
        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pDepthSrvHeap))))
        {
            m_pDepthSrvHeap = 0;
            return 0;
        }
        m_depthSrvFor = 0;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE h =
        m_pDepthSrvHeap->GetCPUDescriptorHandleForHeapStart();
    if (m_depthSrvFor != m_pSceneDepthRes)
    {
        // D32_FLOAT_S8X24 -> read the DEPTH plane only; the stencil plane is X8X24 and not sampled here.
        D3D12_SHADER_RESOURCE_VIEW_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2DARRAY; // a non-array depth is just an array of 1
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize =
            (UINT)(m_sceneDepthSlices > 0 ? m_sceneDepthSlices : 1);
        m_pDevice->CreateShaderResourceView(m_pSceneDepthRes, &sd, h);
        m_depthSrvFor = m_pSceneDepthRes;
    }
    return (unsigned __int64)h.ptr;
}

void D3D12Backend::SetSceneDepthReadable(bool readable)
{
    if (!m_pList || !m_bRecording || !m_pSceneDepthRes)
    {
        m_sceneDepthReadable = false;
        return;
    }
    if (m_sceneDepthReadable == readable)
        return;
    D3D12_RESOURCE_BARRIER b;
    ZeroMemory(&b, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_pSceneDepthRes;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore =
        readable ?
            D3D12_RESOURCE_STATE_DEPTH_WRITE :
            (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    b.Transition.StateAfter =
        readable ?
            (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) :
            D3D12_RESOURCE_STATE_DEPTH_WRITE;
    m_pList->ResourceBarrier(1, &b);
    m_sceneDepthReadable = readable;
}

//============================ cockpit sun shadow map =========================
// Artscout - 2026: a small DEPTH-ONLY target for the 3D pit's sun shadow (see RENDER-LIGHTING.md).
// The pit is one rigid BSP drawn near-Z, and it is lit by one directional light, so its shadow is
// just that light's depth: an ortho fitted to the model's own bounding box, rendered in MODEL space.
// The target therefore never follows the camera, the eye or the aircraft -- only the sun's direction
// in the pit frame changes it. DSV is D32_FLOAT_S8X24 (the format every PSO already bakes, so the
// shadow PSO needs no format of its own); the SRV reads the depth plane as R32_FLOAT.
bool D3D12Backend::EnsurePitShadowTarget(int res)
{
    if (!m_pDevice || res < 1)
        return false;
    if (m_pPitShadowTex && m_pitShadowRes == res)
        return true;

    if (m_pPitShadowTex)
    {
        WaitForGpu(); // it may still be referenced by frames in flight
        D12_RELEASE(m_pPitShadowTex);
    }

    if (!m_pPitShadowDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pPitShadowDsvHeap))))
        {
            D12Log("[D3D12] pit shadow DSV heap failed\n");
            m_pPitShadowDsvHeap = 0;
            return false;
        }
    }
    if (!m_pPitShadowSrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU staging: FlushConstants copies it to the ring
        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pPitShadowSrvHeap))))
        {
            D12Log("[D3D12] pit shadow SRV heap failed\n");
            m_pPitShadowSrvHeap = 0;
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)res;
    rd.Height = (UINT)res;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv;
    ZeroMemory(&cv, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 0.0f; // reversed-Z: 0 is the FAR plane
    cv.DepthStencil.Stencil = 0;

    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &cv, IID_PPV_ARGS(&m_pPitShadowTex))))
    {
        D12Log("[D3D12] pit shadow target create failed\n");
        m_pPitShadowTex = 0;
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    ZeroMemory(&dv, sizeof(dv));
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_pDevice->CreateDepthStencilView(
        m_pPitShadowTex, &dv,
        m_pPitShadowDsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    // R32_FLOAT_X8X24_TYPELESS, NOT R32_FLOAT: an R32G8X24_TYPELESS resource cannot be viewed as
    // plain R32_FLOAT -- asking for it makes CreateShaderResourceView REMOVE THE DEVICE (DXGI
    // _INVALID_CALL, reproduced with the debug layer), which then fails every later PSO with
    // DEVICE_REMOVED and presents a black screen. This is the same view the scene depth uses
    // (SceneDepthSrvCpu); the depth plane is the only part a shader can read anyway.
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    m_pDevice->CreateShaderResourceView(
        m_pPitShadowTex, &sd,
        m_pPitShadowSrvHeap->GetCPUDescriptorHandleForHeapStart());

    m_pitShadowRes = res;
    m_pitShadowReadable = false;
    return true;
}

void D3D12Backend::BindPitShadowTarget()
{
    if (!m_pList || !m_bRecording || !m_pPitShadowTex)
        return;
    if (m_pitShadowReadable)
    {
        // It was sampled last frame -> back to a depth attachment for this frame's replay.
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pPitShadowTex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)(
            D3D12_RESOURCE_STATE_DEPTH_READ |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        m_pList->ResourceBarrier(1, &b);
        m_pitShadowReadable = false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_pPitShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    // ZERO render targets: the shadow PSO is depth-only (NumRenderTargets=0). A null RTV array is
    // exactly what that pipeline expects.
    m_pList->OMSetRenderTargets(0, NULL, FALSE, &dsv);
    // Reversed-Z: 0 is the far plane, so that is what a cleared map holds (nothing lit it).
    m_pList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, NULL);
    m_curSampleCount = 1; // single-sample: GetPSO's SampleDesc must match

    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)m_pitShadowRes;
    vp.Height = (FLOAT)m_pitShadowRes;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = m_pitShadowRes;
    sc.bottom = m_pitShadowRes;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);

    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(true);
}

void D3D12Backend::UnbindPitShadowTarget()
{
    if (!m_pList || !m_bRecording || !m_pPitShadowTex)
        return;
    if (!m_pitShadowReadable)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pPitShadowTex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b.Transition.StateAfter = (D3D12_RESOURCE_STATES)(
            D3D12_RESOURCE_STATE_DEPTH_READ |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_pList->ResourceBarrier(1, &b);
        m_pitShadowReadable = true;
    }
    // Restore whatever scene target was current (back buffer / VR eye / MSAA / RTT) and the
    // matching sample count. The pit flush only ever runs inside the 3D scene pass, so this is
    // the scene target, never a display-atlas RTT.
    BindBackBufferRTV();
}

unsigned __int64 D3D12Backend::PitShadowSrvCpu() const
{
    if (!m_pPitShadowTex || !m_pPitShadowSrvHeap || !m_pitShadowReadable)
        return 0; // still a depth attachment -> FlushConstants substitutes the white SRV
    return (unsigned __int64)m_pPitShadowSrvHeap
        ->GetCPUDescriptorHandleForHeapStart()
        .ptr;
}

// Artscout - 2026: #DX12 -- stamp the fence value that retires THIS frame's allocator, then hand the ring on. The
// wait itself now lives in BeginCommandList (as late as possible), keyed to the allocator rather than the index.
void D3D12Backend::MoveToNextFrame()
{
    m_allocFence[m_frameIndex] = SignalQueue();
    m_frameIndex = m_pSwapChain->GetCurrentBackBufferIndex();
}

bool D3D12Backend::Resize(int nWidth, int nHeight)
{
    if (!m_pSwapChain)
        return false;
    if (nWidth == m_nWidth && nHeight == m_nHeight)
        return true;
    if (nWidth < 1)
        nWidth = 1;
    if (nHeight < 1)
        nHeight = 1;
    WaitForGpu();
    ReleaseBackBufferViews();
    m_nWidth = nWidth;
    m_nHeight = nHeight;
    // Artscout - 2026: #DX12 -- the WaitForGpu above retired every allocator, so clear their fences: nothing is in
    // flight and the back buffers are about to be recreated. (The old code rebased a per-index fence baseline here.)
    for (int i = 0; i < kFrameCount; ++i)
        m_allocFence[i] = 0;
    m_pSwapChain->ResizeBuffers(kFrameCount, nWidth, nHeight,
                                DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    m_frameIndex = m_pSwapChain->GetCurrentBackBufferIndex();
    if (!CreateBackBufferViews())
        return false;
    return CreateDepthBuffer(); // Artscout - 2026: #DX12 Phase 3 -- resize depth to match
}

// --- Neutral backend surface (Phase 1 stubs; real impls arrive with the ported passes) ---
void D3D12Backend::BindBackBuffer(bool bClearDepth)
{
    // Phase 1: no separate render pass yet -- treat "bind back buffer" as beginning a frame with a clear.
    // Later phases split bind (no clear) from BeginFrame(clear) and add the depth buffer.
    (void)bClearDepth;
    if (!m_bRecording)
        BeginFrame(0xFF000000);
}
void D3D12Backend::ClearDepth()
{
    // #DX12 п.5: clear the CURRENT scene depth (eye in VR, back buffer flat) to 1.0. The stale "Phase 1: no
    // depth buffer yet" no-op was fine only because the sole caller (context.cpp clearDepthBeforeObjects) is
    // gated to D3D11 -- implement it for real so the depth-reset path is correct whenever it is wired for D3D12.
    if (!m_pList || !m_bRecording)
        return;
    unsigned __int64 dsvPtr = m_sceneDsvPtr;
    if (!dsvPtr)
        dsvPtr = m_pDsvHeap ? (unsigned __int64)m_pDsvHeap
                                  ->GetCPUDescriptorHandleForHeapStart()
                                  .ptr :
                              0;
    if (!dsvPtr)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    dsv.ptr = (SIZE_T)dsvPtr;
    m_pList->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0,
        NULL);
}
void D3D12Backend::SetViewportRect(int x, int y, int w, int h)
{
    if (!m_pList || !m_bRecording)
        return;
    D3D12_VIEWPORT vp;
    vp.TopLeftX = (FLOAT)x;
    vp.TopLeftY = (FLOAT)y;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    D3D12_RECT sc;
    sc.left = x;
    sc.top = y;
    sc.right = x + w;
    sc.bottom = y + h;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
}
// Artscout - 2026: MSAA -- snap the requested sample count (g_nMsaaSamples) down to a level the device
// supports for the backbuffer format. 1 = MSAA off.
int D3D12Backend::ResolveMsaaSamples()
{
    extern bool g_bMsaaEnable;
    extern int g_nMsaaSamples;
    if (!m_pDevice || !g_bMsaaEnable)
        return 1;
    int want = g_nMsaaSamples;
    if (want < 2)
        return 1;
    if (want > 8)
        want = 8;
    for (int s = want; s >= 2; --s)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql;
        ZeroMemory(&ql, sizeof(ql));
        ql.Format = (DXGI_FORMAT)BackBufferFormat();
        ql.SampleCount = (UINT)s;
        if (SUCCEEDED(m_pDevice->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ql, sizeof(ql))) &&
            ql.NumQualityLevels > 0)
            return s;
    }
    return 1;
}

void D3D12Backend::ReleaseMsaaTargets()
{
    D12_RELEASE(m_pMsaaColorTex);
    D12_RELEASE(m_pMsaaRtvHeap);
    D12_RELEASE(m_pMsaaDepthTex);
    D12_RELEASE(m_pMsaaDsvHeap);
    m_msaaW = m_msaaH = 0;
}

// (Re)create the MSAA color+depth targets at (w,h). Sample count is resolved once (cached in m_msaaSamples).
// Returns true if MSAA is active after the call (targets up), false if off/unsupported.
bool D3D12Backend::CreateMsaaTargets(int w, int h)
{
    if (!m_pDevice)
        return false;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (m_msaaSamples <= 0)
        m_msaaSamples = ResolveMsaaSamples(); // resolve once
    if (m_msaaSamples < 2)
        return false;
    if (m_pMsaaColorTex && m_msaaW == w && m_msaaH == h)
        return true; // already up at this size
    ReleaseMsaaTargets();

    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    // MSAA color (backbuffer format) + RTV
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = (DXGI_FORMAT)BackBufferFormat();
    rd.SampleDesc.Count = (UINT)m_msaaSamples;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE ccv;
    ZeroMemory(&ccv, sizeof(ccv));
    ccv.Format = (DXGI_FORMAT)BackBufferFormat();
    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &ccv, IID_PPV_ARGS(&m_pMsaaColorTex))))
    {
        m_msaaSamples = 1;
        return false;
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pMsaaRtvHeap))))
        {
            ReleaseMsaaTargets();
            m_msaaSamples = 1;
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rv;
        ZeroMemory(&rv, sizeof(rv));
        rv.Format = (DXGI_FORMAT)BackBufferFormat();
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        m_pDevice->CreateRenderTargetView(
            m_pMsaaColorTex, &rv,
            m_pMsaaRtvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // MSAA depth (D24S8) + DSV
    D3D12_RESOURCE_DESC dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = (UINT64)w;
    dd.Height = (UINT)h;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dd.SampleDesc.Count = (UINT)m_msaaSamples;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv;
    ZeroMemory(&dcv, sizeof(dcv));
    dcv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dcv.DepthStencil.Depth = 0.0f;
    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &dcv, IID_PPV_ARGS(&m_pMsaaDepthTex))))
    {
        ReleaseMsaaTargets();
        m_msaaSamples = 1;
        return false;
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pMsaaDsvHeap))))
        {
            ReleaseMsaaTargets();
            m_msaaSamples = 1;
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv;
        ZeroMemory(&dv, sizeof(dv));
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        m_pDevice->CreateDepthStencilView(
            m_pMsaaDepthTex, &dv,
            m_pMsaaDsvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    m_msaaW = w;
    m_msaaH = h;
    D12Log("[D3D12] MSAA targets %dx%d x%d\n", w, h, m_msaaSamples);
    return true;
}

// Bind the MSAA color+depth as the scene target (called from BeginFrame/BeginEyeFrame when MSAA is active),
// clearing both. Sets m_scene* to the MSAA target and m_curSampleCount so the renderer builds MSAA PSOs.
void D3D12Backend::BindMsaaScene(unsigned long argb, int w, int h)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        m_pMsaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_pMsaaDsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(true);
    m_curRtvPtr = (unsigned __int64)rtv.ptr;
    m_sceneRtvPtr = (unsigned __int64)rtv.ptr;
    m_sceneDsvPtr = (unsigned __int64)dsv.ptr;
    // #13: MSAA depth -- multisampled, so it has NO Texture2DArray view (SceneDepthSrvCpu returns 0 -> no clamp).
    m_pSceneDepthRes = m_pMsaaDepthTex;
    m_sceneDepthSlices = 1;
    m_sceneDepthMs = true;
    m_sceneDepthReadable = false;
    m_sceneW = w;
    m_sceneH = h;
    m_curSampleCount = m_msaaSamples;
    const float clr[4] = {((argb >> 16) & 0xFF) / 255.0f,
                          ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                          ((argb >> 24) & 0xFF) / 255.0f};
    m_pList->ClearRenderTargetView(rtv, clr, 0, NULL);
    m_pList->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0,
        NULL);
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = w;
    sc.bottom = h;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
}

// Resolve the MSAA scene color into the single-sample backbuffer, then rebind the backbuffer RTV (no depth)
// so the UI composites over the resolved image. Flat path -- called before CompositeUISurface. No-op if MSAA off.
void D3D12Backend::ResolveMsaaToBackBuffer()
{
    // Artscout - 2026: an HDR frame's scene is FP16 (MSAA or not) -- OutputHdrScene resolves it, writes the
    // back buffer and leaves it bound with no depth, exactly the state the MSAA resolve below leaves.
    if (m_pHdrCur)
    {
        OutputHdrScene();
        return;
    }
    if (!m_pList || !m_bRecording || !MsaaActive())
        return;
    ID3D12Resource* bb = m_pBackBuffer[m_frameIndex];
    D3D12_RESOURCE_BARRIER b[2];
    ZeroMemory(b, sizeof(b));
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = bb;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition.pResource = m_pMsaaColorTex;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    m_pList->ResourceBarrier(2, b);
    m_pList->ResolveSubresource(bb, 0, m_pMsaaColorTex, 0,
                                (DXGI_FORMAT)BackBufferFormat());
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_pList->ResourceBarrier(2, b);
    // rebind the backbuffer RTV (no depth) so CompositeUISurface draws onto the resolved image
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)m_frameIndex * m_rtvDescSize;
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    m_curRtvPtr = (unsigned __int64)rtv.ptr;
    m_curSampleCount = 1;
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)m_nWidth;
    vp.Height = (FLOAT)m_nHeight;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = m_nWidth;
    sc.bottom = m_nHeight;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
}
//============================ HDR scene + GT7 tone mapping ===================
// Artscout - 2026: see the header comment at CurrentRtvFormat and RENDER-LIGHTING.md ("HDR + GT7").
// The whole pipeline in one place:
//   BeginFrame (renderer-opened) / BeginEyeFrame -> BeginHdrScene: bind + clear the FP16 scene.
//   otwloop, 3D -> 2D boundary        -> ToneMapSceneGT7: snapshot, GT7 back into the scene, rebind.
//   ResolveMsaaToBackBuffer / Present / EndEyeFrame -> OutputHdrScene: snapshot, clamp into the 8-bit
//                                        target, which then becomes the scene for whatever follows.
static const DXGI_FORMAT kHdrSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Everything the HDR path says goes to FFDebug.log as well: D12Log is OutputDebugString only, which is
// invisible without a debugger -- the first in-game test found no "HDR frames" line for exactly that reason.
static void HdrLog(const char* fmt, ...)
{
    char body[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';
    char ln[512];
    _snprintf(ln, sizeof(ln), "[HDR] %s\n", body);
    ln[sizeof(ln) - 1] = '\0';
    OutputDebugStringA(ln);
    FFDebugLog(ln);
}

int D3D12Backend::CurrentRtvFormat() const
{
    if (m_pHdrCur && m_curRtvPtr && m_curRtvPtr == m_hdrSceneRtvPtr)
        return (int)kHdrSceneFormat;
    return BackBufferFormat();
}

void D3D12Backend::ReleaseHdrTargets(HdrTargets& t)
{
    D12_RELEASE(t.color);
    D12_RELEASE(t.msaa);
    D12_RELEASE(t.copy);
    D12_RELEASE(t.rtvHeap);
    D12_RELEASE(t.srvHeap);
    t.w = t.h = t.samples = 0;
}

D3D12Backend::HdrTargets& D3D12Backend::HdrEyeSlot(int w, int h)
{
    int pick = -1;
    for (int i = 0; i < 2 && pick < 0; ++i)
        if (m_hdrEye[i].color && m_hdrEye[i].w == w && m_hdrEye[i].h == h)
            pick = i;
    if (pick < 0) // replace the empty / least recently used slot
        pick = (!m_hdrEye[0].color || (m_hdrEye[1].color &&
                                       m_hdrEye[0].lru <= m_hdrEye[1].lru)) ?
                   0 :
                   1;
    m_hdrEye[pick].lru = ++m_hdrEyeLru;
    return m_hdrEye[pick];
}

bool D3D12Backend::EnsureHdrTargets(HdrTargets& t, int w, int h, int samples)
{
    if (!m_pDevice || w < 1 || h < 1)
        return false;
    if (samples < 1)
        samples = 1;
    if (t.color && t.samples == samples && t.w == w && t.h == h)
        return true;

    if (samples > 1)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql;
        ZeroMemory(&ql, sizeof(ql));
        ql.Format = kHdrSceneFormat;
        ql.SampleCount = (UINT)samples;
        if (FAILED(m_pDevice->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ql, sizeof(ql))) ||
            ql.NumQualityLevels == 0)
        {
            static bool s_logged = false;
            if (!s_logged)
                HdrLog("FP16 x%d MSAA unsupported -> 8-bit scene",
                       samples);
            s_logged = true;
            return false;
        }
    }

    if (t.color)
        WaitForGpu(); // the old set may still be referenced by frames in flight
    ReleaseHdrTargets(t);

    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = kHdrSceneFormat;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    // No optimized clear value: the scene clears to the sky/fog colour, which changes every frame.
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    bool ok = SUCCEEDED(m_pDevice->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
        NULL, IID_PPV_ARGS(&t.color)));
    if (ok && samples > 1)
    {
        D3D12_RESOURCE_DESC md = rd;
        md.SampleDesc.Count = (UINT)samples;
        ok = SUCCEEDED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_RENDER_TARGET,
            NULL, IID_PPV_ARGS(&t.msaa)));
    }
    if (ok)
    {
        D3D12_RESOURCE_DESC cd = rd;
        cd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ok = SUCCEEDED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &cd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
            IID_PPV_ARGS(&t.copy)));
    }
    if (ok)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 2;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ok = SUCCEEDED(
            m_pDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.rtvHeap)));
    }
    if (ok)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ok = SUCCEEDED(
            m_pDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.srvHeap)));
    }
    if (!ok)
    {
        HdrLog("target create failed (%dx%d x%d) -> 8-bit scene",
               w, h, samples);
        ReleaseHdrTargets(t);
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        t.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rv;
    ZeroMemory(&rv, sizeof(rv));
    rv.Format = kHdrSceneFormat;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_pDevice->CreateRenderTargetView(t.color, &rv, rtv);
    if (t.msaa)
    {
        rtv.ptr += m_rtvDescSize;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        m_pDevice->CreateRenderTargetView(t.msaa, &rv, rtv);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    ZeroMemory(&sv, sizeof(sv));
    sv.Format = kHdrSceneFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    m_pDevice->CreateShaderResourceView(
        t.copy, &sv, t.srvHeap->GetCPUDescriptorHandleForHeapStart());

    t.w = w;
    t.h = h;
    t.samples = samples;
    HdrLog("scene targets %dx%d x%d (%s)", w, h, samples,
           (&t == &m_hdrFlat) ? "flat" : "eye");
    return true;
}

bool D3D12Backend::EnsureHdrPipeline()
{
    if (m_pHdrRS && m_pHdrVS && m_pHdrPS)
        return true;
    if (m_hdrPipelineFailed || !m_pDevice)
        return false;
    m_hdrPipelineFailed = true; // cleared at the end; any early return leaves HDR off for the session

    // Root signature: t0 (the scene copy) as a PS table + 4 PS root constants (b0).
    D3D12_DESCRIPTOR_RANGE range;
    ZeroMemory(&range, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[2];
    ZeroMemory(params, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ZeroMemory(&rsd, sizeof(rsd));
    rsd.NumParameters = 2;
    rsd.pParameters = params;

    ID3DBlob* rsBlob = 0;
    ID3DBlob* rsErr = 0;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rsBlob, &rsErr)))
    {
        HdrLog("SerializeRootSignature failed");
        if (rsErr)
            rsErr->Release();
        return false;
    }
    HRESULT hr = m_pDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(),
                                                IID_PPV_ARGS(&m_pHdrRS));
    rsBlob->Release();
    if (rsErr)
        rsErr->Release();
    if (FAILED(hr))
    {
        HdrLog("CreateRootSignature failed 0x%08X", (unsigned)hr);
        return false;
    }

    const char* entries[2] = {"VSMain", "PSMain"};
    const char* profiles[2] = {"vs_5_0", "ps_5_0"};
    ID3DBlob** outs[2] = {&m_pHdrVS, &m_pHdrPS};
    for (int i = 0; i < 2; ++i)
    {
        ID3DBlob* err = 0;
        hr = D3DCompile(kGT7ToneMapHlsl, strlen(kGT7ToneMapHlsl), "gt7tonemap",
                        0, 0, entries[i], profiles[i],
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, outs[i], &err);
        if (FAILED(hr))
            HdrLog("GT7 %s compile failed: %s", entries[i],
                   err ? (const char*)err->GetBufferPointer() : "?");
        if (err)
            err->Release();
        if (FAILED(hr))
        {
            D12_RELEASE(m_pHdrVS);
            D12_RELEASE(m_pHdrPS);
            return false;
        }
    }
    m_hdrPipelineFailed = false;
    HdrLog("GT7 tone-map pipeline up");
    return true;
}

ID3D12PipelineState* D3D12Backend::HdrPso(int fmt, int samples)
{
    int freeSlot = -1;
    for (int i = 0; i < 4; ++i)
    {
        if (m_pHdrPso[i] && m_hdrPsoFmt[i] == fmt &&
            m_hdrPsoSamples[i] == samples)
            return m_pHdrPso[i];
        if (!m_pHdrPso[i] && freeSlot < 0)
            freeSlot = i;
    }
    if (freeSlot < 0) // four variants cover flat/eye x in-place/output; never expected to spill
        return 0;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ZeroMemory(&pd, sizeof(pd));
    pd.pRootSignature = m_pHdrRS;
    pd.VS.pShaderBytecode = m_pHdrVS->GetBufferPointer();
    pd.VS.BytecodeLength = m_pHdrVS->GetBufferSize();
    pd.PS.pShaderBytecode = m_pHdrPS->GetBufferPointer();
    pd.PS.BytecodeLength = m_pHdrPS->GetBufferSize();
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = (DXGI_FORMAT)fmt;
    pd.SampleDesc.Count = (UINT)samples;
    ID3D12PipelineState* pso = 0;
    if (FAILED(m_pDevice->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
    {
        HdrLog("PSO create failed (fmt %d x%d)", fmt, samples);
        return 0;
    }
    m_pHdrPso[freeSlot] = pso;
    m_hdrPsoFmt[freeSlot] = fmt;
    m_hdrPsoSamples[freeSlot] = samples;
    return pso;
}

bool D3D12Backend::BeginHdrScene(HdrTargets& t, int w, int h, int samples,
                                 unsigned __int64 outRtvPtr, unsigned long argb,
                                 unsigned __int64 dsvPtr)
{
    D3D12_CPU_DESCRIPTOR_HANDLE dsvH;
    dsvH.ptr = (SIZE_T)dsvPtr;
    D3D12_CPU_DESCRIPTOR_HANDLE* dsv = dsvPtr ? &dsvH : NULL;
    m_pHdrCur = 0;
    if (!EnsureHdrPipeline())
        return false;
    // Both output variants up front: a frame that could begin but not end would leave the target black.
    if (!HdrPso((int)kHdrSceneFormat, samples) || !HdrPso(BackBufferFormat(), 1))
        return false;
    if (!EnsureHdrTargets(t, w, h, samples))
        return false;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        t.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (samples > 1)
        rtv.ptr += m_rtvDescSize;
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, dsv);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(dsv != NULL);
    const float clr[4] = {((argb >> 16) & 0xFF) / 255.0f,
                          ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                          ((argb >> 24) & 0xFF) / 255.0f};
    m_pList->ClearRenderTargetView(rtv, clr, 0, NULL);
    if (dsv)
        m_pList->ClearDepthStencilView(
            *dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0,
            NULL);
    // Not SetViewportRect: the Begin*Frame callers set m_bRecording only after this returns.
    D3D12_VIEWPORT vp = {0.0f, 0.0f, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, w, h};
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);

    m_curRtvPtr = m_sceneRtvPtr = m_hdrSceneRtvPtr = (unsigned __int64)rtv.ptr;
    m_sceneDsvPtr = dsv ? (unsigned __int64)dsv->ptr : 0;
    m_sceneW = w;
    m_sceneH = h;
    m_curSampleCount = samples;
    m_hdrSceneSamples = samples;
    m_hdrOutRtvPtr = outRtvPtr;
    m_hdrToneMapped = false;
    m_pHdrCur = &t;
    ++m_hdrStatFrames;
    {
        static bool s_flatSaid = false, s_eyeSaid = false;
        bool& said = (&t == &m_hdrFlat) ? s_flatSaid : s_eyeSaid;
        if (!said)
            HdrLog("FP16 scene engaged (%s %dx%d x%d)",
                   (&t == &m_hdrFlat) ? "flat" : "eye", w, h, samples);
        said = true;
    }
    return true;
}

// Scene -> copy. Leaves the scene in RENDER_TARGET and the copy in PIXEL_SHADER_RESOURCE (their rest states).
void D3D12Backend::HdrSnapshotScene()
{
    HdrTargets& t = *m_pHdrCur;
    const bool ms = m_hdrSceneSamples > 1;
    ID3D12Resource* src = ms ? t.msaa : t.color;
    D3D12_RESOURCE_BARRIER b[2];
    ZeroMemory(b, sizeof(b));
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = src;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = ms ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE :
                                      D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = t.copy;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.StateAfter = ms ? D3D12_RESOURCE_STATE_RESOLVE_DEST :
                                      D3D12_RESOURCE_STATE_COPY_DEST;
    m_pList->ResourceBarrier(2, b);
    if (ms)
        m_pList->ResolveSubresource(t.copy, 0, src, 0, kHdrSceneFormat);
    else
        m_pList->CopyResource(t.copy, src);
    for (int i = 0; i < 2; ++i)
    {
        D3D12_RESOURCE_STATES s = b[i].Transition.StateBefore;
        b[i].Transition.StateBefore = b[i].Transition.StateAfter;
        b[i].Transition.StateAfter = s;
    }
    m_pList->ResourceBarrier(2, b);
}

void D3D12Backend::HdrDraw(unsigned __int64 rtvPtr, int fmt, int samples,
                           int w, int h, float mode)
{
    ID3D12PipelineState* pso = HdrPso(fmt, samples);
    if (!pso)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)rtvPtr;
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    SetViewportRect(0, 0, w, h);

    extern float g_fToneMapExposure;
    // GT7 frame-buffer units are 100 cd/m^2; scene 1.0 is placed on its SDR paper white (250 cd/m^2).
    const float consts[4] = {g_fToneMapExposure * 2.5f, mode, 0.0f, 0.0f};
    ID3D12DescriptorHeap* heaps[] = {m_pHdrCur->srvHeap};
    m_pList->SetGraphicsRootSignature(m_pHdrRS);
    m_pList->SetDescriptorHeaps(1, heaps);
    m_pList->SetGraphicsRootDescriptorTable(
        0, m_pHdrCur->srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_pList->SetGraphicsRoot32BitConstants(1, 4, consts, 0);
    m_pList->SetPipelineState(pso);
    m_pList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pList->DrawInstanced(3, 1, 0, 0);
    // Our root signature + heap replaced the renderer's; its next draw must re-set them.
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->RequestRebind();
}

bool D3D12Backend::ToneMapSceneGT7()
{
    if (!m_pList || !m_bRecording || !m_pHdrCur || m_hdrToneMapped)
        return false;
    HdrSnapshotScene();
    HdrDraw(m_hdrSceneRtvPtr, (int)kHdrSceneFormat, m_hdrSceneSamples, m_sceneW,
            m_sceneH, 1.0f);
    m_hdrToneMapped = true;
    ++m_hdrStatToneMapped;
    BindBackBufferRTV(); // the scene again, with its depth and full viewport, for the 2D overlays
    return true;
}

void D3D12Backend::OutputHdrScene()
{
    if (!m_pList || !m_bRecording || !m_pHdrCur)
        return;
    GpuProf_Mark(m_pList, "hdr-out");
    HdrSnapshotScene();
    HdrDraw(m_hdrOutRtvPtr, BackBufferFormat(), 1, m_sceneW, m_sceneH, 0.0f);
    m_pHdrCur = 0;

    // The 8-bit target is the scene from here on (UI composite, screenshots, BindBackBufferRTV). No depth:
    // the scene depth may be multisampled, and nothing after the output draws 3D.
    m_curRtvPtr = m_sceneRtvPtr = m_hdrOutRtvPtr;
    m_sceneDsvPtr = 0;
    m_curSampleCount = 1;
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(false);

    // Proof of life in the log: how many frames took the FP16 path, and how many of them reached the
    // GT7 hook. frames > 0 with tonemapped == 0 means the otwloop hook is not being hit.
    const unsigned now = GetTickCount();
    if (!m_hdrStatTick)
        m_hdrStatTick = now;
    if (now - m_hdrStatTick >= 10000)
    {
        HdrLog("frames/10s=%u GT7 tone-mapped=%u", m_hdrStatFrames,
               m_hdrStatToneMapped);
        m_hdrStatFrames = m_hdrStatToneMapped = 0;
        m_hdrStatTick = now;
    }
}

// --- Phase 2: present the RGB565 UI surface as a fullscreen textured quad -----
bool D3D12Backend::EnsureQuadPipeline()
{
    if (m_pQuadPSO)
        return true;
    if (!m_pDevice)
        return false;

    if (!m_pSrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_pDevice->CreateDescriptorHeap(&hd,
                                                   IID_PPV_ARGS(&m_pSrvHeap))))
        {
            D12Log("[D3D12] SRV heap failed\n");
            return false;
        }
    }

    // Root signature: 1 SRV table (t0, PS) + 1 static linear-clamp sampler (s0, PS).
    D3D12_DESCRIPTOR_RANGE range;
    ZeroMemory(&range, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER param;
    ZeroMemory(&param, sizeof(param));
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp;
    ZeroMemory(&samp, sizeof(samp));
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd;
    ZeroMemory(&rsd, sizeof(rsd));
    rsd.NumParameters = 1;
    rsd.pParameters = &param;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* rsBlob = 0;
    ID3DBlob* rsErr = 0;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rsBlob, &rsErr)))
    {
        D12Log("[D3D12] SerializeRootSignature failed\n");
        if (rsErr)
            rsErr->Release();
        return false;
    }
    HRESULT hr = m_pDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                                rsBlob->GetBufferSize(),
                                                IID_PPV_ARGS(&m_pQuadRS));
    rsBlob->Release();
    if (rsErr)
        rsErr->Release();
    if (FAILED(hr))
    {
        D12Log("[D3D12] CreateRootSignature failed 0x%08X\n", (unsigned)hr);
        return false;
    }

    static const char* kSrc =
        "Texture2D gTex : register(t0);\n"
        "SamplerState gSmp : register(s0);\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) {\n"
        "  VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = uv; o.pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1); "
        "return o; }\n"
        // Artscout - 2026: SHARP is the pixel-art "sharp bilinear" filter for the magnified menu
        // (UiScale): each source texel stays a flat block and only the one output pixel across a
        // texel seam blends, so a non-integer scale (1.77) neither blurs like bilinear nor doubles
        // some pixels and not others like nearest. 1:1 reproduces the image exactly; shrinking falls
        // back to plain bilinear. g_nUiFilter 0 = plain bilinear throughout.
        "float4 PSMain(VSOut i) : SV_TARGET {\n"
        "#if SHARP\n"
        "  float w, h; gTex.GetDimensions(w, h); float2 sz = float2(w, h);\n"
        "  float2 pix = i.uv * sz; float2 seam = floor(pix + 0.5);\n"
        "  float2 d = max(fwidth(pix), 1e-5);\n"
        "  pix = seam + clamp((pix - seam) / d, -0.5, 0.5);\n"
        "  return gTex.Sample(gSmp, pix / sz);\n"
        "#else\n"
        "  return gTex.Sample(gSmp, i.uv);\n"
        "#endif\n"
        "}\n";

    extern int g_nUiFilter;
    const D3D_SHADER_MACRO kDefs[] = {{"SHARP", g_nUiFilter == 1 ? "1" : "0"}, {NULL, NULL}};

    ID3DBlob* vs = 0;
    ID3DBlob* ps = 0;
    ID3DBlob* err = 0;
    if (FAILED(D3DCompile(kSrc, strlen(kSrc), "quad", kDefs, 0, "VSMain", "vs_5_0",
                          0, 0, &vs, &err)))
    {
        D12Log("[D3D12] VS compile failed: %s\n",
               err ? (const char*)err->GetBufferPointer() : "?");
        if (err)
            err->Release();
        return false;
    }
    if (err)
    {
        err->Release();
        err = 0;
    }
    if (FAILED(D3DCompile(kSrc, strlen(kSrc), "quad", kDefs, 0, "PSMain", "ps_5_0",
                          0, 0, &ps, &err)))
    {
        D12Log("[D3D12] PS compile failed: %s\n",
               err ? (const char*)err->GetBufferPointer() : "?");
        if (err)
            err->Release();
        if (vs)
            vs->Release();
        return false;
    }
    if (err)
    {
        err->Release();
        err = 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso;
    ZeroMemory(&pso, sizeof(pso));
    pso.pRootSignature = m_pQuadRS;
    pso.VS.pShaderBytecode = vs->GetBufferPointer();
    pso.VS.BytecodeLength = vs->GetBufferSize();
    pso.PS.pShaderBytecode = ps->GetBufferPointer();
    pso.PS.BytecodeLength = ps->GetBufferSize();
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    hr =
        m_pDevice->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pQuadPSO));
    if (FAILED(hr))
    {
        D12Log("[D3D12] CreateGraphicsPipelineState failed 0x%08X\n",
               (unsigned)hr);
        vs->Release();
        ps->Release();
        return false;
    }

    // #DX12 п.2: alpha-blend variant (UI-over-3D composite). Same shaders/root sig; SRC_ALPHA/INV_SRC_ALPHA.
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = m_pDevice->CreateGraphicsPipelineState(&pso,
                                                IID_PPV_ARGS(&m_pQuadPSOBlend));
    vs->Release();
    ps->Release();
    if (FAILED(hr))
    {
        D12Log("[D3D12] CreateGraphicsPipelineState(blend) failed 0x%08X\n",
               (unsigned)hr);
        return false;
    }
    D12Log("D3D12Backend: 2D-quad pipeline up (opaque + blend)\n");
    return true;
}

// #DX12 п.2: composite the UI surface over the 3D scene. 565 black -> alpha 0 (3D shows through), else opaque.
void D3D12Backend::CompositeBitmap565(const void* pSrc565, int srcW, int srcH)
{
    if (!m_bRecording || !pSrc565 || srcW < 1 || srcH < 1)
        return;
    if (!EnsureQuadPipeline())
        return;
    if (!EnsureQuadTexture(srcW, srcH))
        return;

    void* mapped = 0;
    D3D12_RANGE noRead;
    noRead.Begin = 0;
    noRead.End = 0;
    if (FAILED(m_pQuadUpload->Map(0, &noRead, &mapped)) || !mapped)
        return;
    const unsigned short* s = (const unsigned short*)pSrc565;
    for (int y = 0; y < srcH; ++y)
    {
        unsigned* dst =
            (unsigned*)((unsigned char*)mapped + (size_t)y * m_quadRowPitch);
        const unsigned short* row = s + (size_t)y * srcW;
        for (int x = 0; x < srcW; ++x)
        {
            unsigned short v = row[x];
            unsigned a = v ? 0xFF000000u :
                             0u; // black 565 -> transparent (3D behind shows)
            unsigned r = (v >> 11) & 0x1F;
            r = (r << 3) | (r >> 2);
            unsigned g = (v >> 5) & 0x3F;
            g = (g << 2) | (g >> 4);
            unsigned b = v & 0x1F;
            b = (b << 3) | (b >> 2);
            dst[x] = a | (b << 16) | (g << 8) | r;
        }
    }
    m_pQuadUpload->Unmap(0, NULL);

    if (m_quadTexState != (unsigned)D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pQuadTex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)m_quadTexState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        m_pList->ResourceBarrier(1, &b);
        m_quadTexState = (unsigned)D3D12_RESOURCE_STATE_COPY_DEST;
    }
    D3D12_TEXTURE_COPY_LOCATION dstL;
    ZeroMemory(&dstL, sizeof(dstL));
    dstL.pResource = m_pQuadTex;
    dstL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstL.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcL;
    ZeroMemory(&srcL, sizeof(srcL));
    srcL.pResource = m_pQuadUpload;
    srcL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcL.PlacedFootprint.Offset = 0;
    srcL.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcL.PlacedFootprint.Footprint.Width = (UINT)srcW;
    srcL.PlacedFootprint.Footprint.Height = (UINT)srcH;
    srcL.PlacedFootprint.Footprint.Depth = 1;
    srcL.PlacedFootprint.Footprint.RowPitch = m_quadRowPitch;
    m_pList->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, NULL);

    D3D12_RESOURCE_BARRIER b2;
    ZeroMemory(&b2, sizeof(b2));
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition.pResource = m_pQuadTex;
    b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_pList->ResourceBarrier(1, &b2);
    m_quadTexState = (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // The scene RTV + viewport are already bound (the frame was opened by the renderer). Draw the blended quad.
    m_pList->SetGraphicsRootSignature(m_pQuadRS);
    ID3D12DescriptorHeap* heaps[] = {m_pSrvHeap};
    m_pList->SetDescriptorHeaps(1, heaps);
    m_pList->SetGraphicsRootDescriptorTable(
        0, m_pSrvHeap->GetGPUDescriptorHandleForHeapStart());
    m_pList->SetPipelineState(m_pQuadPSOBlend);
    m_pList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pList->DrawInstanced(3, 1, 0, 0);
}

bool D3D12Backend::EnsureQuadTexture(int w, int h)
{
    if (w < 1 || h < 1)
        return false;
    if (m_pQuadTex && w == m_quadTexW && h == m_quadTexH)
        return true;

    WaitForGpu(); // the old texture/upload buffer may still be referenced by an in-flight frame
    D12_RELEASE(m_pQuadTex);
    D12_RELEASE(m_pQuadUpload);
    m_quadTexW = w;
    m_quadTexH = h;

    D3D12_HEAP_PROPERTIES hpDefault;
    ZeroMemory(&hpDefault, sizeof(hpDefault));
    hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT64)w;
    td.Height = (UINT)h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(m_pDevice->CreateCommittedResource(
            &hpDefault, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&m_pQuadTex))))
    {
        D12Log("[D3D12] quad tex create failed\n");
        return false;
    }
    m_quadTexState = (unsigned)D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    ZeroMemory(&srv, sizeof(srv));
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_pDevice->CreateShaderResourceView(
        m_pQuadTex, &srv, m_pSrvHeap->GetCPUDescriptorHandleForHeapStart());

    m_quadRowPitch = (unsigned)(((w * 4) + 255) & ~255);
    UINT64 uploadSize = (UINT64)m_quadRowPitch * (UINT64)h;
    D3D12_HEAP_PROPERTIES hpUpload;
    ZeroMemory(&hpUpload, sizeof(hpUpload));
    hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = uploadSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_pDevice->CreateCommittedResource(
            &hpUpload, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            IID_PPV_ARGS(&m_pQuadUpload))))
    {
        D12Log("[D3D12] quad upload create failed\n");
        return false;
    }
    return true;
}

void D3D12Backend::BlitBitmap565(const void* pSrc565, int srcW, int srcH)
{
    if (!m_bRecording || !pSrc565 || srcW < 1 || srcH < 1)
        return; // must be between BeginFrame/Present
    if (!EnsureQuadPipeline())
        return;
    if (!EnsureQuadTexture(srcW, srcH))
        return;

    // 565 -> RGBA8 into the (256-aligned) upload buffer.
    void* mapped = 0;
    D3D12_RANGE noRead;
    noRead.Begin = 0;
    noRead.End = 0;
    if (FAILED(m_pQuadUpload->Map(0, &noRead, &mapped)) || !mapped)
        return;
    const unsigned short* s = (const unsigned short*)pSrc565;
    for (int y = 0; y < srcH; ++y)
    {
        unsigned* dst =
            (unsigned*)((unsigned char*)mapped + (size_t)y * m_quadRowPitch);
        const unsigned short* row = s + (size_t)y * srcW;
        for (int x = 0; x < srcW; ++x)
        {
            unsigned short v = row[x];
            unsigned r = (v >> 11) & 0x1F;
            r = (r << 3) | (r >> 2);
            unsigned g = (v >> 5) & 0x3F;
            g = (g << 2) | (g >> 4);
            unsigned b = v & 0x1F;
            b = (b << 3) | (b >> 2);
            dst[x] = 0xFF000000u | (b << 16) | (g << 8) |
                     r; // R8G8B8A8 in memory (little-endian 0xAABBGGRR)
        }
    }
    m_pQuadUpload->Unmap(0, NULL);

    if (m_quadTexState != (unsigned)D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pQuadTex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)m_quadTexState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        m_pList->ResourceBarrier(1, &b);
        m_quadTexState = (unsigned)D3D12_RESOURCE_STATE_COPY_DEST;
    }

    D3D12_TEXTURE_COPY_LOCATION dstL;
    ZeroMemory(&dstL, sizeof(dstL));
    dstL.pResource = m_pQuadTex;
    dstL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstL.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcL;
    ZeroMemory(&srcL, sizeof(srcL));
    srcL.pResource = m_pQuadUpload;
    srcL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcL.PlacedFootprint.Offset = 0;
    srcL.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcL.PlacedFootprint.Footprint.Width = (UINT)srcW;
    srcL.PlacedFootprint.Footprint.Height = (UINT)srcH;
    srcL.PlacedFootprint.Footprint.Depth = 1;
    srcL.PlacedFootprint.Footprint.RowPitch = m_quadRowPitch;
    m_pList->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, NULL);

    D3D12_RESOURCE_BARRIER b2;
    ZeroMemory(&b2, sizeof(b2));
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition.pResource = m_pQuadTex;
    b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_pList->ResourceBarrier(1, &b2);
    m_quadTexState = (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Draw the fullscreen triangle sampling the UI texture (RTV + viewport already bound by BeginFrame).
    m_pList->SetGraphicsRootSignature(m_pQuadRS);
    ID3D12DescriptorHeap* heaps[] = {m_pSrvHeap};
    m_pList->SetDescriptorHeaps(1, heaps);
    m_pList->SetGraphicsRootDescriptorTable(
        0, m_pSrvHeap->GetGPUDescriptorHandleForHeapStart());
    m_pList->SetPipelineState(m_pQuadPSO);
    m_pList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pList->DrawInstanced(3, 1, 0, 0);
}
void D3D12Backend::SetGScreenSize(int w, int h)
{
    // #DX12 п.5: gScreenSize is the D3D12 renderer's CBViewport (screenW/H) that VS_Screen uses to map the
    // CPU-projected 2D sky/terrain + 2D overlays from pixel space to NDC. Forward to the renderer (mirrors
    // D3D11Backend::SetGScreenSize) -- it was a no-op stub, so in a VR per-eye pass the eye-sized 2D sky was
    // mapped by the back-buffer size -> horizon mis-scaled/inverted ("dark blue, sky only when inverted").
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetViewportSize(w, h);
}
void D3D12Backend::ClearCurrentRTV(float r, float g, float b, float a)
{
    if (!m_pList || !m_bRecording || !m_curRtvPtr)
        return;
    // #DX12: clear whatever RTV is CURRENTLY bound (the RTT display atlas during an RTT batch, else the back
    // buffer) -- NOT hardcoded to the back buffer. Without this the HUD/MFD atlas was never cleared under D3D12
    // -> symbology accumulated frame to frame ("paint over the whole HUD/MFD").
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)m_curRtvPtr;
    const float c[4] = {r, g, b, a};
    m_pList->ClearRenderTargetView(rtv, c, 0, NULL);
}
void D3D12Backend::FlushContext()
{ /* D3D12 submits explicitly in Present */
}

// #DX12 п.5 (VR): (re)create the per-eye depth buffer (D32) sized to the eye image.
bool D3D12Backend::EnsureEyeDepth(int w, int h)
{
    if (w < 1 || h < 1)
        return false;
    if (m_pEyeDepthTex && w == m_eyeDepthW && h == m_eyeDepthH)
        return true;
    if (m_pEyeDepthTex)
    {
        m_pEyeDepthTex->Release();
        m_pEyeDepthTex = 0;
    }
    if (!m_pEyeDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pEyeDsvHeap))))
            return false;
    }
    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv;
    ZeroMemory(&cv, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 0.0f;
    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &cv, IID_PPV_ARGS(&m_pEyeDepthTex))))
        return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    ZeroMemory(&dv, sizeof(dv));
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_pDevice->CreateDepthStencilView(
        m_pEyeDepthTex, &dv,
        m_pEyeDsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_eyeDepthW = w;
    m_eyeDepthH = h;
    return true;
}

// Artscout - 2026: #DX12 п.5 -- query the device's ViewInstancing tier once. Tier 1+ = the rasterizer can
// replicate primitives to multiple RT-array slices driven by SV_ViewID (single-pass stereo). Cached in m_viTier.
bool D3D12Backend::ViewInstancingSupported()
{
    if (m_viTier < 0)
    {
        m_viTier = 0;
        if (m_pDevice)
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3;
            ZeroMemory(&o3, sizeof(o3));
            if (SUCCEEDED(m_pDevice->CheckFeatureSupport(
                    D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))))
                m_viTier = (int)o3.ViewInstancingTier; // 0 = not supported
        }
        D12Log("[D3D12] ViewInstancingTier = %d\n", m_viTier);
    }
    return m_viTier >= 1;
}

// Artscout - 2026: #DX12 п.5 -- 2-slice ARRAY depth for the single-pass stereo eye target. Three DSVs: [0] the
// whole 2-slice array (VI geometry pass), [1]/[2] the individual slices (per-eye 2D overlay tail). Reversed-Z
// float depth + stencil, same format as the flat/per-eye path.
bool D3D12Backend::EnsureEyeDepthArray(int w, int h, int nViews)
{
    if (w < 1 || h < 1)
        return false;
    if (nViews < 1)
        nViews = 1;
    if (nViews > 4)
        nViews = 4;

    // Slot cache: reuse a slot already at (w,h,nViews) (quad's periphery + focus stay resident across frames -> no
    // realloc of the large committed depth). Otherwise fill an empty slot, else evict the least-recently-used.
    int slot = -1;
    for (int i = 0; i < 2; ++i)
        if (m_eyeDepth[i].tex && m_eyeDepth[i].w == w && m_eyeDepth[i].h == h &&
            m_eyeDepth[i].n == nViews)
        {
            slot = i;
            break;
        }
    if (slot < 0)
        for (int i = 0; i < 2; ++i)
            if (!m_eyeDepth[i].tex)
            {
                slot = i;
                break;
            }
    if (slot < 0)
        slot = (m_eyeDepth[0].lru <= m_eyeDepth[1].lru) ? 0 : 1;

    EyeDepthSlot& S = m_eyeDepth[slot];
    if (S.tex && S.w == w && S.h == h && S.n == nViews) // exact reuse
    {
        S.lru = m_renderEpoch;
        m_eyeDepthCur = slot;
        return true;
    }
    if (S.tex)
    {
        S.tex->Release();
        S.tex = 0;
    }
    if (S.dsvHeap && S.n != nViews)
    {
        S.dsvHeap->Release();
        S.dsvHeap = 0;
    } // re-heap only if slice count changed
    if (!S.dsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1 + nViews;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(
                m_pDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&S.dsvHeap))))
            return false;
    }
    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = (UINT16)nViews;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv;
    ZeroMemory(&cv, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 0.0f;
    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &cv, IID_PPV_ARGS(&S.tex))))
        return false;

    unsigned inc = m_pDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE base =
        S.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    // [0] array covering all N slices.
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv;
        ZeroMemory(&dv, sizeof(dv));
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = 0;
        dv.Texture2DArray.ArraySize = (UINT)nViews;
        m_pDevice->CreateDepthStencilView(S.tex, &dv, base);
    }
    // [1..N] single slices.
    for (int s = 0; s < nViews; ++s)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h1 = base;
        h1.ptr += (SIZE_T)inc * (1 + s);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv;
        ZeroMemory(&dv, sizeof(dv));
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = (UINT)s;
        dv.Texture2DArray.ArraySize = 1;
        m_pDevice->CreateDepthStencilView(S.tex, &dv, h1);
    }
    S.w = w;
    S.h = h;
    S.n = nViews;
    S.lru = m_renderEpoch;
    m_eyeDepthCur = slot;
    return true;
}

// Artscout - 2026: #DX12 п.5 -- open ONE command list bound to the 2-slice ARRAY eye RTV + array depth, clear
// both slices, and set the view instance mask 0b11 so the VI PSOs rasterize both eyes. The world scene draws once.
void D3D12Backend::BeginStereoInstancedFrame(void* arrayImg,
                                             unsigned __int64 arrayRtvPtr,
                                             int w, int h, int nViews)
{
    if (!m_pDevice || !m_pList || !arrayImg || !arrayRtvPtr)
        return;
    if (nViews < 1)
        nViews = 1;
    if (nViews > 4)
        nViews = 4;
    BeginCommandList();
    m_pHdrCur = 0; // Artscout - 2026: view instancing stays on the 8-bit path (no HDR/GT7)
    m_renderEpoch++;
    extern void D3D12TexMgr_TickFrame(unsigned renderEpoch);
    D3D12TexMgr_TickFrame(m_renderEpoch);
    if (!EnsureEyeDepthArray(w, h, nViews))
    {
        D12Log("[D3D12] BeginStereoInstancedFrame: array depth alloc failed\n");
    }
    // Artscout - 2026: #DX12 п.5 -- one-shot confirmation in the NORMAL log (the OpenXR "ACTIVE" line is XrDbg/
    // OutputDebugString only). If this never prints, view instancing fell back to the per-eye loop.
    {
        static bool s_once = false;
        if (!s_once)
        {
            s_once = true;
            D12Log("[VI] single-pass view-instanced ACTIVE (%dx%d, %d "
                   "slices/views)\n",
                   w, h, nViews);
        }
    }

    // Same invariant as BeginEyeFrame: do NOT barrier the XR swapchain image (runtime hands it in RENDER_TARGET).
    (void)arrayImg;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)arrayRtvPtr;
    ID3D12DescriptorHeap* dsvHeap =
        m_eyeDepth[m_eyeDepthCur].dsvHeap; // slot chosen by EnsureEyeDepthArray
    bool haveDsv = (dsvHeap != 0);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        haveDsv ? dsvHeap->GetCPUDescriptorHandleForHeapStart() :
                  D3D12_CPU_DESCRIPTOR_HANDLE();
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, haveDsv ? &dsv : NULL);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(haveDsv);
    const float black[4] = {0, 0, 0, 1};
    m_pList->ClearRenderTargetView(rtv, black, 0,
                                   NULL); // array RTV -> clears both slices
    if (haveDsv)
        m_pList->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0,
            NULL);
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = w;
    sc.bottom = h;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);

    // View instance mask (1<<N)-1 -> all N views active for the VI geometry pass.
    if (!m_pList1)
        m_pList->QueryInterface(IID_PPV_ARGS(&m_pList1));
    if (m_pList1)
        m_pList1->SetViewInstanceMask((UINT)((1u << nViews) - 1u));

    m_curRtvPtr = arrayRtvPtr;
    m_curSampleCount = 1;
    m_sceneRtvPtr = arrayRtvPtr;
    m_sceneDsvPtr = haveDsv ? (unsigned __int64)dsv.ptr : 0;
    // #13: VI depth = a single-sample nView-slice array (m_curSampleCount is 1 here) -> viewable as Texture2DArray.
    m_pSceneDepthRes = m_eyeDepth[m_eyeDepthCur].tex;
    m_sceneDepthSlices = m_eyeDepth[m_eyeDepthCur].n;
    m_sceneDepthMs = false;
    m_sceneDepthReadable = false;
    m_sceneW = w;
    m_sceneH = h;
    m_bRecording = true;
}

// Artscout - 2026: #DX12 п.5 -- re-bind a SINGLE array slice (RTV + that slice's depth) for the per-eye 2D
// overlay tail. View instance mask 1 -> only view 0 rasterizes (the draws are ordinary, non-VI PSOs). No clear
// (the geometry pass already filled both slices; the 2D overlays manage their own Z as on the flat path).
void D3D12Backend::BindEyeSlice(int view, unsigned __int64 sliceRtvPtr, int w,
                                int h)
{
    if (!m_pList || !m_bRecording || !sliceRtvPtr)
        return;
    EyeDepthSlot& S = m_eyeDepth
        [m_eyeDepthCur]; // slot bound by the group's BeginStereoInstancedFrame
    if (view < 0)
        view = 0;
    if (S.n > 0 && view >= S.n)
        view = S.n - 1;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)sliceRtvPtr;
    bool haveDsv = (S.dsvHeap != 0);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = D3D12_CPU_DESCRIPTOR_HANDLE();
    if (haveDsv)
    {
        unsigned inc = m_pDevice->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        dsv = S.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += (SIZE_T)inc * (1 + view); // [1..N] per-slice DSV
    }
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, haveDsv ? &dsv : NULL);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(haveDsv);
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = w;
    sc.bottom = h;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
    if (m_pList1)
        m_pList1->SetViewInstanceMask(0x1); // single view (non-VI draws)
    m_curRtvPtr = sliceRtvPtr;
    m_curSampleCount = 1;
    m_sceneRtvPtr = sliceRtvPtr;
    m_sceneDsvPtr = haveDsv ? (unsigned __int64)dsv.ptr : 0;
    m_sceneW = w;
    m_sceneH = h;
}

// Artscout - 2026: #DX12 п.5 -- close + execute + fence the single stereo command list (both eyes filled).
void D3D12Backend::EndStereoInstancedFrame(void* arrayImg)
{
    if (!m_pList || !m_bRecording || !arrayImg)
        return;
    (void)
        arrayImg; // runtime owns the swapchain image state (no RT->COMMON barrier; see BeginEyeFrame)
    GpuProf_EndSlot(m_pList);
    m_pList->Close();
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)m_pList};
    extern void D3D12TexMgr_SyncRenderQueue(struct ID3D12CommandQueue * q);
    D3D12TexMgr_SyncRenderQueue(m_pQueue);
    // Artscout - 2026: #DX12 -- stamp the fence that retires THIS allocator, then drain on it: the runtime's
    // xrReleaseSwapchainImage assumes our GPU work on the array image is done. Stamping (rather than leaning on
    // the drain) is what lets BeginCommandList recycle this allocator safely on its own terms.
    m_pQueue->ExecuteCommandLists(1, lists);
    m_allocFence[m_frameIndex] = SignalQueue();
    GpuProf_Submitted(m_allocFence[m_frameIndex]);
    {
        const double w0 = GpNowMs();
        WaitForFence(m_allocFence[m_frameIndex]);
        GpuProf_Synced(GpNowMs() - w0); // how long the CPU sat waiting for the GPU
    }
    m_bRecording = false;

    // Artscout - 2026 (VR screenshots): view-instanced stereo renders both eyes into one
    // 2-slice array image; slice 0 is the left eye, so the same capture works on
    // subresource 0.
    //
    // This also covers QUAD, for free: EndViCopyGroup renders each group into a private
    // 2-slice array and finishes by calling us with it, after restoring it to
    // RENDER_TARGET -- which is the state the capture expects. That array is created
    // TYPELESS (ViTypelessOf), so it only works because the capture resolves typeless to
    // the concrete UNORM member of the same family. Whichever group ends first claims a
    // pending request; for quad that is the periphery pair, i.e. the wide view.
    ServiceEyeCapture(arrayImg);
}

// Artscout - 2026: #DX12 п.5 QUAD copy path -- typeless of a RGBA/BGRA swapchain format (so one committed resource
// takes both a UNORM RTV, to render into, and a copy to an SRGB/UNORM swapchain image of the same family).
static DXGI_FORMAT ViTypelessOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default:
        return f;
    }
}
static DXGI_FORMAT ViUnormOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

// Artscout - 2026: #DX12 п.5 QUAD copy path -- private 2-slice ARRAY color target for one VI group (typeless RGBA,
// UNORM RTVs so the scene writes match the direct eye path's no-gamma-re-encode). Two-slot LRU cache keyed by
// (w,h,fmt) so periphery + focus coexist without a per-frame realloc of the large committed target.
bool D3D12Backend::EnsureViColorArray(int w, int h, int fmt)
{
    if (w < 1 || h < 1 || !m_pDevice)
        return false;
    int slot = -1;
    for (int i = 0; i < 2; ++i)
        if (m_viColor[i].tex && m_viColor[i].w == w && m_viColor[i].h == h &&
            m_viColor[i].fmt == fmt)
        {
            slot = i;
            break;
        }
    if (slot < 0)
        for (int i = 0; i < 2; ++i)
            if (!m_viColor[i].tex)
            {
                slot = i;
                break;
            }
    if (slot < 0)
        slot = (m_viColor[0].lru <= m_viColor[1].lru) ? 0 : 1;

    ViColorSlot& S = m_viColor[slot];
    if (S.tex && S.w == w && S.h == h && S.fmt == fmt)
    {
        S.lru = m_renderEpoch;
        m_viColorCur = slot;
        return true;
    }
    if (S.tex)
    {
        S.tex->Release();
        S.tex = 0;
    }
    if (!S.rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 3;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(
                m_pDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&S.rtvHeap))))
            return false;
    }
    const DXGI_FORMAT resFmt = ViTypelessOf((DXGI_FORMAT)fmt);
    const DXGI_FORMAT rtvFmt = ViUnormOf((DXGI_FORMAT)fmt);
    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 2;
    rd.MipLevels = 1;
    rd.Format = resFmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv;
    ZeroMemory(&cv, sizeof(cv));
    cv.Format = rtvFmt;
    cv.Color[0] = cv.Color[1] = cv.Color[2] = 0.0f;
    cv.Color[3] = 1.0f;
    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &cv, IID_PPV_ARGS(&S.tex))))
        return false;

    unsigned inc = m_pDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE base =
        S.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    // [0] array RTV (both slices, the VI geometry pass).
    {
        D3D12_RENDER_TARGET_VIEW_DESC rv;
        ZeroMemory(&rv, sizeof(rv));
        rv.Format = rtvFmt;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.FirstArraySlice = 0;
        rv.Texture2DArray.ArraySize = 2;
        m_pDevice->CreateRenderTargetView(S.tex, &rv, base);
        S.arrayRtv = (unsigned __int64)base.ptr;
    }
    // [1],[2] per-slice RTVs (the per-eye 2D overlay tail).
    for (int s = 0; s < 2; ++s)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h1 = base;
        h1.ptr += (SIZE_T)inc * (1 + s);
        D3D12_RENDER_TARGET_VIEW_DESC rv;
        ZeroMemory(&rv, sizeof(rv));
        rv.Format = rtvFmt;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.FirstArraySlice = (UINT)s;
        rv.Texture2DArray.ArraySize = 1;
        m_pDevice->CreateRenderTargetView(S.tex, &rv, h1);
        S.sliceRtv[s] = (unsigned __int64)h1.ptr;
    }
    S.w = w;
    S.h = h;
    S.fmt = fmt;
    S.lru = m_renderEpoch;
    m_viColorCur = slot;
    return true;
}

// Artscout - 2026: #DX12 п.5 QUAD copy path -- open the VI list on this group's PRIVATE array color target (reuses
// BeginStereoInstancedFrame). The world VI pass + per-slice tail then render into the private array; EndViCopyGroup
// copies its 2 slices into the 2 per-view swapchain images. sliceRtvsOut[0..1] = the private slice RTVs (tail).
void D3D12Backend::BeginViCopyGroup(int w, int h, int fmt, void** sliceRtvsOut)
{
    for (int v = 0; v < 4; ++v)
        if (sliceRtvsOut)
            sliceRtvsOut[v] = 0;
    if (!EnsureViColorArray(w, h, fmt))
    {
        D12Log(
            "[D3D12] BeginViCopyGroup: private VI color array alloc failed\n");
        return;
    }
    ViColorSlot& S = m_viColor[m_viColorCur];
    BeginStereoInstancedFrame((void*)S.tex, S.arrayRtv, w, h, 2);
    for (int v = 0; v < 2; ++v)
        if (sliceRtvsOut)
            sliceRtvsOut[v] = (void*)(SIZE_T)S.sliceRtv[v];
}

// Artscout - 2026: #DX12 п.5 QUAD copy path -- copy the private array's 2 slices into the 2 per-view swapchain
// images (foveated layer wants separate arraySize=1 swapchains), then close+execute+fence. The swapchain images are
// handed by the runtime in RENDER_TARGET (same invariant as the direct eye path); barrier RT->COPY_DEST->RT around
// the copy; the private array RENDER_TARGET->COPY_SOURCE->RT.
void D3D12Backend::EndViCopyGroup(void* dstImg0, void* dstImg1)
{
    if (!m_pList || !m_bRecording)
        return;
    ViColorSlot& S = m_viColor[m_viColorCur];
    if (S.tex)
    {
        ID3D12Resource* dst[2] = {(ID3D12Resource*)dstImg0,
                                  (ID3D12Resource*)dstImg1};
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = S.tex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_pList->ResourceBarrier(1, &b);
        for (int i = 0; i < 2; ++i)
        {
            if (!dst[i])
                continue;
            D3D12_RESOURCE_BARRIER bd;
            ZeroMemory(&bd, sizeof(bd));
            bd.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bd.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bd.Transition.pResource = dst[i];
            bd.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            bd.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            m_pList->ResourceBarrier(1, &bd);
            D3D12_TEXTURE_COPY_LOCATION sl;
            ZeroMemory(&sl, sizeof(sl));
            sl.pResource = S.tex;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.SubresourceIndex = (UINT)i; // array slice i (mip 0)
            D3D12_TEXTURE_COPY_LOCATION dl;
            ZeroMemory(&dl, sizeof(dl));
            dl.pResource = dst[i];
            dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dl.SubresourceIndex = 0;
            m_pList->CopyTextureRegion(&dl, 0, 0, 0, &sl, NULL);
            bd.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            bd.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            m_pList->ResourceBarrier(1, &bd);
        }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_pList->ResourceBarrier(1, &b);
    }
    EndStereoInstancedFrame((void*)(S.tex ? S.tex : (ID3D12Resource*)dstImg0));
}

// #DX12 п.5 A1 (in-scene VR menu): (re)create the off-screen menu color RTT (via the texture manager, so it has
// an SRV to sample + an RTV to draw into) plus a private D32 depth (the exit dialog is a 3D BSP that needs Z).
// Cached; only rebuilt on a size change (the RTV/SRV heap slots are not reclaimed, so avoid per-frame rebuilds).
void D3D12Backend::EnsureMenuRtt(int w, int h)
{
    if (!m_pDevice || w <= 0 || h <= 0)
        return;
    if (m_pMenuRtt && m_pMenuRtt->tex && m_menuRttW == w && m_menuRttH == h)
        return; // already at size

    if (m_pMenuRtt)
    {
        if (g_pD3D12TextureManager)
            g_pD3D12TextureManager->Destroy(*m_pMenuRtt);
        delete m_pMenuRtt;
        m_pMenuRtt = 0;
    }
    D12_RELEASE(m_pMenuDepthTex);
    m_menuRttW = m_menuRttH = 0;
    if (!g_pD3D12TextureManager)
        return;

    m_pMenuRtt = new D3D12Texture();
    if (!g_pD3D12TextureManager->CreateRenderTarget(*m_pMenuRtt, w, h))
    {
        delete m_pMenuRtt;
        m_pMenuRtt = 0;
        return;
    }

    // Depth (D32) + a 1-slot DSV heap, reused across size changes.
    if (!m_pMenuDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pMenuDsvHeap))))
            m_pMenuDsvHeap = 0;
    }
    if (m_pMenuDsvHeap)
    {
        D3D12_HEAP_PROPERTIES hp;
        ZeroMemory(&hp, sizeof(hp));
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd;
        ZeroMemory(&rd, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = (UINT64)w;
        rd.Height = (UINT)h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv;
        ZeroMemory(&cv, sizeof(cv));
        cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        cv.DepthStencil.Depth = 0.0f;
        if (SUCCEEDED(m_pDevice->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                IID_PPV_ARGS(&m_pMenuDepthTex))))
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dv;
            ZeroMemory(&dv, sizeof(dv));
            dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            m_pDevice->CreateDepthStencilView(
                m_pMenuDepthTex, &dv,
                m_pMenuDsvHeap->GetCPUDescriptorHandleForHeapStart());
        }
    }
    m_menuRttW = w;
    m_menuRttH = h;
}

// #DX12 п.5 A1: bind the menu color RTT (+ its depth) as the render target and set the menu viewport. Draws recorded
// after this land in the menu RTT (on the eye command list); the caller rebinds the eye afterward (BindBackBufferRTV).
void D3D12Backend::BindMenuRtt(bool clear)
{
    if (!m_pList || !m_bRecording || !m_pMenuRtt || !m_pMenuRtt->tex)
        return;

    if (m_pMenuRtt->rtState != (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pMenuRtt->tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)m_pMenuRtt->rtState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_pList->ResourceBarrier(1, &b);
        m_pMenuRtt->rtState = (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)m_pMenuRtt->rtvCpuPtr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_pMenuDsvHeap ? m_pMenuDsvHeap->GetCPUDescriptorHandleForHeapStart() :
                         D3D12_CPU_DESCRIPTOR_HANDLE();
    m_curRtvPtr =
        (unsigned __int64)
            rtv.ptr; // ClearCurrentRTV targets the menu RTT while it is bound
    m_curSampleCount = 1; // menu RTT is single-sample
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, m_pMenuDepthTex ? &dsv : NULL);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(m_pMenuDepthTex != 0);
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)m_menuRttW;
    vp.Height = (FLOAT)m_menuRttH;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = m_menuRttW;
    sc.bottom = m_menuRttH;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
    if (clear)
    {
        const float z[4] = {
            0, 0, 0,
            0}; // transparent canvas: the menu alpha-keys the empty area out on the quad
        m_pList->ClearRenderTargetView(rtv, z, 0, NULL);
        if (m_pMenuDepthTex)
            m_pList->ClearDepthStencilView(
                dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0,
                0, NULL);
    }
}

void* D3D12Backend::MenuRttTex()
{
    return (m_pMenuRtt && m_pMenuRtt->tex) ? (void*)m_pMenuRtt : NULL;
}

// Artscout - 2026: #DX12 п.5 -- small colour-only RTT for the head-locked VR FPS quad (2D text, no depth needed).
void D3D12Backend::EnsureFpsRtt(int w, int h)
{
    if (!m_pDevice || w <= 0 || h <= 0)
        return;
    if (m_pFpsRtt && m_pFpsRtt->tex && m_fpsRttW == w && m_fpsRttH == h)
        return; // already at size
    if (m_pFpsRtt)
    {
        if (g_pD3D12TextureManager)
            g_pD3D12TextureManager->Destroy(*m_pFpsRtt);
        delete m_pFpsRtt;
        m_pFpsRtt = 0;
    }
    m_fpsRttW = m_fpsRttH = 0;
    if (!g_pD3D12TextureManager)
        return;
    m_pFpsRtt = new D3D12Texture();
    if (!g_pD3D12TextureManager->CreateRenderTarget(*m_pFpsRtt, w, h))
    {
        delete m_pFpsRtt;
        m_pFpsRtt = 0;
        return;
    }
    m_fpsRttW = w;
    m_fpsRttH = h;
}

// Bind the FPS RTT (colour only, no depth) + set the viewport + clear transparent. Draws land in it on the eye list.
void D3D12Backend::BindFpsRtt(bool clear)
{
    if (!m_pList || !m_bRecording || !m_pFpsRtt || !m_pFpsRtt->tex)
        return;
    if (m_pFpsRtt->rtState != (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pFpsRtt->tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)m_pFpsRtt->rtState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_pList->ResourceBarrier(1, &b);
        m_pFpsRtt->rtState = (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)m_pFpsRtt->rtvCpuPtr;
    m_curRtvPtr = (unsigned __int64)rtv.ptr;
    m_curSampleCount = 1;
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, NULL); // no depth for 2D text
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(false);
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)m_fpsRttW;
    vp.Height = (FLOAT)m_fpsRttH;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = m_fpsRttW;
    sc.bottom = m_fpsRttH;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
    if (clear)
    {
        const float z[4] = {0, 0, 0, 0};
        m_pList->ClearRenderTargetView(rtv, z, 0, NULL);
    } // transparent canvas
}

void* D3D12Backend::FpsRttTex()
{
    return (m_pFpsRtt && m_pFpsRtt->tex) ? (void*)m_pFpsRtt : NULL;
}

// Artscout - 2026: radio-subtitle quad RTT -- a straight copy of the FPS trio above, kept separate because the two
// quads are live at the same time and at different sizes, so sharing one RTT would thrash it every frame.
void D3D12Backend::EnsureSubRtt(int w, int h)
{
    if (!m_pDevice || w <= 0 || h <= 0)
        return;
    if (m_pSubRtt && m_pSubRtt->tex && m_subRttW == w && m_subRttH == h)
        return; // already at size
    if (m_pSubRtt)
    {
        if (g_pD3D12TextureManager)
            g_pD3D12TextureManager->Destroy(*m_pSubRtt);
        delete m_pSubRtt;
        m_pSubRtt = 0;
    }
    m_subRttW = m_subRttH = 0;
    if (!g_pD3D12TextureManager)
        return;
    m_pSubRtt = new D3D12Texture();
    if (!g_pD3D12TextureManager->CreateRenderTarget(*m_pSubRtt, w, h))
    {
        delete m_pSubRtt;
        m_pSubRtt = 0;
        return;
    }
    m_subRttW = w;
    m_subRttH = h;
}

void D3D12Backend::BindSubRtt(bool clear)
{
    if (!m_pList || !m_bRecording || !m_pSubRtt || !m_pSubRtt->tex)
        return;
    if (m_pSubRtt->rtState != (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_pSubRtt->tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)m_pSubRtt->rtState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_pList->ResourceBarrier(1, &b);
        m_pSubRtt->rtState = (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)m_pSubRtt->rtvCpuPtr;
    m_curRtvPtr = (unsigned __int64)rtv.ptr;
    m_curSampleCount = 1;
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, NULL); // no depth for 2D text
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(false);
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)m_subRttW;
    vp.Height = (FLOAT)m_subRttH;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = m_subRttW;
    sc.bottom = m_subRttH;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
    if (clear)
    {
        const float z[4] = {0, 0, 0, 0};
        m_pList->ClearRenderTargetView(rtv, z, 0, NULL);
    } // transparent canvas
}

void* D3D12Backend::SubRttTex()
{
    return (m_pSubRtt && m_pSubRtt->tex) ? (void*)m_pSubRtt : NULL;
}

//-----------------------------------------------------------------------------
// Artscout - 2026: MSAA for the per-eye VR path (cfg "VrMsaaSamples", 0/1 = off).
//
// The eye path rendered single-sample, so every building edge, roof line and alpha-tested window crawled with the
// head's micro-motion even on a paused scene. The pieces were already there: BeginHdrScene takes a sample count
// (the flat path uses it), HdrSnapshotScene resolves a multisampled FP16 scene, PSOs are cached by sample count,
// and SetSceneDepthReadable already copes with a multisampled depth (m_sceneDepthMs). What was missing is a
// MULTISAMPLED EYE DEPTH BUFFER, below. Both eyes share the FP16 target set (HdrEyeSlot picks by size) and this
// depth: the eye command lists run one after the other and each clears both. File-statics, not members, so the
// header does not change.
//-----------------------------------------------------------------------------
static ID3D12Resource* s_eyeMsDepth = 0;
static ID3D12DescriptorHeap* s_eyeMsDsv = 0;
static int s_eyeMsW = 0, s_eyeMsH = 0, s_eyeMsN = 0;

static void ReleaseEyeMsDepth()
{
    D12_RELEASE(s_eyeMsDepth);
    D12_RELEASE(s_eyeMsDsv);
    s_eyeMsW = s_eyeMsH = s_eyeMsN = 0;
}

// The requested count, snapped DOWN to one the device supports for BOTH the FP16 colour and the D32S8 depth.
static int EyeMsaaPick(ID3D12Device* dev)
{
    extern int g_nVrMsaaSamples;
    static int s_want = -1, s_pick = 1;
    int want = g_nVrMsaaSamples;

    if (want < 2 || !dev)
        return 1;
    if (want > 8)
        want = 8;
    if (want == s_want)
        return s_pick;

    s_want = want;
    s_pick = 1;
    const DXGI_FORMAT fmts[2] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};

    for (int s = want; s >= 2 && s_pick == 1; --s)
    {
        bool ok = true;

        for (int f = 0; f < 2; ++f)
        {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql;
            ZeroMemory(&ql, sizeof(ql));
            ql.Format = fmts[f];
            ql.SampleCount = (UINT)s;
            if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ql, sizeof(ql))) ||
                ql.NumQualityLevels == 0)
                ok = false;
        }

        if (ok)
            s_pick = s;
    }

    char ln[120];
    _snprintf(ln, sizeof(ln) - 1, "[VRMSAA] requested x%d -> using x%d\n", want, s_pick);
    ln[sizeof(ln) - 1] = 0;
    FFDebugLog(ln);
    return s_pick;
}

static bool EyeMsDepthWouldRebuild(int w, int h, int n)
{
    return !(s_eyeMsDepth && s_eyeMsW == w && s_eyeMsH == h && s_eyeMsN == n);
}

static bool EnsureEyeMsDepth(ID3D12Device* dev, int w, int h, int n)
{
    if (!EyeMsDepthWouldRebuild(w, h, n))
        return true;

    ReleaseEyeMsDepth(); // the caller has drained the GPU if anything was live

    D3D12_DESCRIPTOR_HEAP_DESC hd;
    ZeroMemory(&hd, sizeof(hd));
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_eyeMsDsv))))
        return false;

    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    rd.SampleDesc.Count = (UINT)n;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv;
    ZeroMemory(&cv, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 0.0f; // reversed-Z: 0 is the far plane
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                            IID_PPV_ARGS(&s_eyeMsDepth))))
    {
        ReleaseEyeMsDepth();
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    ZeroMemory(&dv, sizeof(dv));
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    dev->CreateDepthStencilView(s_eyeMsDepth, &dv, s_eyeMsDsv->GetCPUDescriptorHandleForHeapStart());
    s_eyeMsW = w;
    s_eyeMsH = h;
    s_eyeMsN = n;

    char ln[140];
    _snprintf(ln, sizeof(ln) - 1, "[VRMSAA] eye depth %dx%d x%d up\n", w, h, n);
    ln[sizeof(ln) - 1] = 0;
    FFDebugLog(ln);
    return true;
}

// #DX12 п.5 (VR): open a command list rendering INTO an XR eye image (bind eye RTV + VR depth, clear both).
void D3D12Backend::BeginEyeFrame(void* eyeImg, unsigned __int64 eyeRtvPtr,
                                 int w, int h)
{
    if (!m_pDevice || !m_pList || !eyeImg || !eyeRtvPtr)
        return;
    BeginCommandList();
    m_pHdrCur = 0; // Artscout - 2026: HDR is re-decided per eye (below)
    m_renderEpoch++;
    // Artscout - 2026 (#65 perf): also tick the texture pool in the VR eye path (it may not run BeginFrame), else
    // deferred placed-resource frees never reclaim in VR.
    extern void D3D12TexMgr_TickFrame(unsigned renderEpoch);
    D3D12TexMgr_TickFrame(m_renderEpoch);
    EnsureEyeDepth(w, h);

    // Artscout - 2026: HDR scene + GT7 -- the eye renders into the FP16 eye set (single-sample, like the eye
    // path always has been) and EndEyeFrame writes the result into the XR image. The XR image itself is
    // untouched until then, so the runtime-owned state rule below still holds.
    extern bool g_bToneMapGT7;
    extern bool g_bTerrainCrackDebug; // cfg: holes in the terrain show as magenta (sky dome + filler are skipped)
    const unsigned long eyeClearArgb = g_bTerrainCrackDebug ? 0xFFFF00FFul : 0xFF000000ul;
    unsigned __int64 eyeDsvPtr = m_pEyeDsvHeap ? (unsigned __int64)m_pEyeDsvHeap
                                                      ->GetCPUDescriptorHandleForHeapStart()
                                                      .ptr :
                                                  0;
    // Artscout - 2026: multisampled eye (cfg VrMsaaSamples). Needs the FP16 scene (it is what resolves); any
    // failure drops to the single-sample eye below, never to a broken frame.
    int eyeMs = 1;
    if (g_bToneMapGT7)
    {
        eyeMs = EyeMsaaPick(m_pDevice);

        if (eyeMs > 1)
        {
            if (EyeMsDepthWouldRebuild(w, h, eyeMs) && s_eyeMsDepth)
                WaitForGpu(); // the old depth may be referenced by a frame in flight

            if (EnsureEyeMsDepth(m_pDevice, w, h, eyeMs))
                eyeDsvPtr = (unsigned __int64)s_eyeMsDsv->GetCPUDescriptorHandleForHeapStart().ptr;
            else
                eyeMs = 1;
        }
    }

    if (g_bToneMapGT7 && BeginHdrScene(HdrEyeSlot(w, h), w, h, eyeMs, eyeRtvPtr, eyeClearArgb, eyeDsvPtr))
    {
        m_pSceneDepthRes = (eyeMs > 1) ? s_eyeMsDepth : m_pEyeDepthTex;
        m_sceneDepthSlices = 1;
        m_sceneDepthMs = (eyeMs > 1); // a multisampled depth has no Texture2DArray view: no cloud/shadow depth read
        m_sceneDepthReadable = false;
        m_bRecording = true;
        return;
    }

    // #DX12 п.5: do NOT transition the XR swapchain image. This runtime (PICO/PVR via d3d11on12) hands the
    // color swapchain images to the app already in D3D12_RESOURCE_STATE_RENDER_TARGET -- a COMMON->RT barrier
    // then trips #527 RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH ("Before COMMON does not match RENDER_TARGET").
    // XR_KHR_D3D12_enable leaves the acquired state runtime-defined; we render directly and leave it as-is.
    (void)eyeImg;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)eyeRtvPtr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_pEyeDsvHeap ? m_pEyeDsvHeap->GetCPUDescriptorHandleForHeapStart() :
                        D3D12_CPU_DESCRIPTOR_HANDLE();
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, m_pEyeDsvHeap ? &dsv : NULL);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(m_pEyeDsvHeap != 0);
    const float black[4] = {0, 0, 0, 1};
    const float magenta[4] = {1, 0, 1, 1};
    m_pList->ClearRenderTargetView(rtv, g_bTerrainCrackDebug ? magenta : black, 0, NULL);
    if (m_pEyeDsvHeap)
        m_pList->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0,
            NULL);
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = w;
    sc.bottom = h;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
    m_curRtvPtr = eyeRtvPtr;
    m_curSampleCount =
        1; // Artscout - 2026: VR eye is single-sample for now (MSAA-in-VR = a later increment; flat MSAA works today)
    // #DX12 п.5: the scene target for this eye is the eye image -> FinishRtt rebinds to THIS (not the back buffer).
    m_sceneRtvPtr = eyeRtvPtr;
    m_sceneDsvPtr = m_pEyeDsvHeap ? (unsigned __int64)dsv.ptr : 0;
    // #13: per-eye depth = single-sample, single slice -> viewable as an array of 1.
    m_pSceneDepthRes = m_pEyeDepthTex;
    m_sceneDepthSlices = 1;
    m_sceneDepthMs = false;
    m_sceneDepthReadable = false;
    m_sceneW = w;
    m_sceneH = h;
    m_bRecording = true;
}

// #DX12 п.5 (VR): transition the eye image to COMMON, execute the list, and flush (image ready before release).
void D3D12Backend::EndEyeFrame(void* eyeImg)
{
    if (!m_pList || !m_bRecording || !eyeImg)
        return;
    // #DX12 п.5: no RT->COMMON transition (see BeginEyeFrame) -- the runtime owns the swapchain image state and
    // composites it directly from RENDER_TARGET. Just close + execute + fence so the image is filled on release.
    (void)eyeImg;
    OutputHdrScene(); // Artscout - 2026: HDR eye -> the XR image (no-op on an 8-bit eye)
    GpuProf_EndSlot(m_pList);
    m_pList->Close();
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)m_pList};
    // Artscout - 2026 (#65 perf): GPU-side wait so async texture/VB uploads submitted this eye frame are
    // resident before the render queue draws them (replaces the old per-upload CPU wait).
    extern void D3D12TexMgr_SyncRenderQueue(struct ID3D12CommandQueue * q);
    D3D12TexMgr_SyncRenderQueue(m_pQueue);
    // Artscout - 2026: #DX12 -- see EndStereoInstancedFrame: stamp this allocator's retire fence, then drain on it.
    m_pQueue->ExecuteCommandLists(1, lists);
    m_allocFence[m_frameIndex] = SignalQueue();
    GpuProf_Submitted(m_allocFence[m_frameIndex]);
    {
        const double w0 = GpNowMs();
        WaitForFence(m_allocFence[m_frameIndex]);
        GpuProf_Synced(GpNowMs() - w0); // how long the CPU sat waiting for the GPU
    }
    m_bRecording = false;

    // Artscout - 2026 (VR screenshots): the eye is rendered and fenced and we still hold the
    // image -- this is the only moment the picture the headset is about to show exists in a
    // resource we can read. Costs nothing unless a screenshot is actually pending.
    ServiceEyeCapture(eyeImg);
}

// #DX12 п.3 RTT: bind an external render-target texture as the current target (displays draw into it).
// Artscout - 2026: depth-stencil for an off-screen RTT that holds a 3D SCENE.
//
// The scene depth (CreateDepthBuffer above) is sized to the back buffer, and an RTT is not -- the
// menu model viewer's is the UI surface size. D3D12 expects the bound render target and
// depth-stencil to agree, so the RTT gets its own, resized on demand. One buffer serves whichever
// RTT is current, because only one is ever bound at a time.
//
// D32_FLOAT_S8X24 and a 0.0 clear to match the scene buffer: this is a reversed-Z pipeline, so 0 is
// the FAR plane and the comparison is GREATER_EQUAL.
bool D3D12Backend::EnsureRttDepth(int w, int h)
{
    if (!m_pDevice || w < 1 || h < 1)
        return false;

    if (m_pRttDepthTex && m_rttDepthW == w && m_rttDepthH == h)
        return true;

    if (m_pRttDepthTex)
    {
        WaitForGpu(); // it may still be referenced by frames in flight
        D12_RELEASE(m_pRttDepthTex);
    }

    if (!m_pRttDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(m_pDevice->CreateDescriptorHeap(
                &hd, IID_PPV_ARGS(&m_pRttDsvHeap))))
        {
            D12Log("[D3D12] RTT DSV heap failed\n");
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES hp;
    ZeroMemory(&hp, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv;
    ZeroMemory(&cv, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 0.0f;
    cv.DepthStencil.Stencil = 0;

    if (FAILED(m_pDevice->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &cv, IID_PPV_ARGS(&m_pRttDepthTex))))
    {
        D12Log("[D3D12] RTT depth create failed\n");
        m_pRttDepthTex = 0;
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    ZeroMemory(&dv, sizeof(dv));
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_pDevice->CreateDepthStencilView(
        m_pRttDepthTex, &dv,
        m_pRttDsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_rttDepthW = w;
    m_rttDepthH = h;
    return true;
}

void D3D12Backend::BindSceneRtt(void* handle, int w, int h, bool clear,
                                bool wantDepth)
{
    EnsureFrameStarted();
    if (!m_pList || !m_bRecording || !handle)
        return;
    D3D12Texture* t = (D3D12Texture*)handle;
    if (!t->tex || !t->rtvCpuPtr)
        return;

    if (t->rtState != (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)t->rtState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_pList->ResourceBarrier(1, &b);
        t->rtState = (unsigned)D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)t->rtvCpuPtr;
    m_curRtvPtr =
        (unsigned __int64)rtv
            .ptr; // #DX12: the RTT atlas is now the current RTV (ClearCurrentRTV clears IT)
    m_curSampleCount =
        1; // the RTT atlas is single-sample -> single-sample PSOs
    // Artscout - 2026: a 3D scene in an RTT needs depth, and this path had none -- it was written
    // for the 2D display panels, which do not. Without a DSV the renderer is forced onto depth-off
    // PSOs, so every triangle lands in submission order and far surfaces paint over near ones. That
    // is why the menu model viewer's aircraft looked see-through: the engines and the far side of
    // the fuselage drawing straight through the near skin, which reads as a wireframe.
    const bool rttDepth =
        wantDepth &&
        EnsureRttDepth(w > 0 ? w : t->width, h > 0 ? h : t->height);

    if (rttDepth)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            m_pRttDsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_pList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        // Reversed-Z: 0 is the far plane, so that is what a cleared buffer holds.
        m_pList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0,
                                       NULL);

        if (g_pD3D12Renderer)
            g_pD3D12Renderer->SetDepthTargetBound(true);
    }
    else
    {
        m_pList->OMSetRenderTargets(1, &rtv, FALSE,
                                    NULL); // 2D displays: no depth

        if (g_pD3D12Renderer)
            g_pD3D12Renderer->SetDepthTargetBound(
                false); // no DSV -> force depth-off PSOs (#615)
    }
    if (w < 1)
        w = t->width;
    if (h < 1)
        h = t->height;
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = w;
    sc.bottom = h;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
    if (clear)
    {
        const float black[4] = {0, 0, 0, 0};
        m_pList->ClearRenderTargetView(rtv, black, 0, NULL);
    }
}

void D3D12Backend::UnbindSceneRtt(void* handle)
{
    if (!m_pList || !m_bRecording || !handle)
        return;
    D3D12Texture* t = (D3D12Texture*)handle;
    if (t->tex &&
        t->rtState != (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)t->rtState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_pList->ResourceBarrier(1, &b);
        t->rtState = (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    BindBackBufferRTV();
}

void D3D12Backend::BindBackBufferRTV()
{
    if (!m_pList || !m_bRecording)
        return;
    // #DX12 п.5: rebind the current SCENE target -- the back buffer on desktop, or the eye image in VR (set by
    // BeginFrame / BeginEyeFrame). Called by FinishRtt to return from the display atlas to the scene.
    unsigned __int64 rtvPtr = m_sceneRtvPtr;
    unsigned __int64 dsvPtr = m_sceneDsvPtr;
    int w = m_sceneW, h = m_sceneH;
    if (!rtvPtr) // fallback: the back buffer
    {
        D3D12_CPU_DESCRIPTOR_HANDLE r =
            m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
        r.ptr += (SIZE_T)m_frameIndex * m_rtvDescSize;
        rtvPtr = (unsigned __int64)r.ptr;
        dsvPtr = m_pDsvHeap ? (unsigned __int64)m_pDsvHeap
                                  ->GetCPUDescriptorHandleForHeapStart()
                                  .ptr :
                              0;
        w = m_nWidth;
        h = m_nHeight;
    }
    // MSAA: the scene target is the MSAA color (flat) -> PSOs must be multisample; VR eye / fallback = 1.
    m_curSampleCount = MsaaActive() ? m_msaaSamples : 1;
    // Artscout - 2026: the HDR scene knows its own sample count (the FP16 eye is 1x even with MSAA up).
    if (m_pHdrCur && rtvPtr == m_hdrSceneRtvPtr)
        m_curSampleCount = m_hdrSceneSamples;
    m_curRtvPtr = rtvPtr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv.ptr = (SIZE_T)rtvPtr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    dsv.ptr = (SIZE_T)dsvPtr;
    m_pList->OMSetRenderTargets(1, &rtv, FALSE, dsvPtr ? &dsv : NULL);
    if (g_pD3D12Renderer)
        g_pD3D12Renderer->SetDepthTargetBound(
            dsvPtr != 0); // scene DSV back -> depth allowed
    D3D12_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    D3D12_RECT sc;
    sc.left = 0;
    sc.top = 0;
    sc.right = w;
    sc.bottom = h;
    m_pList->RSSetViewports(1, &vp);
    m_pList->RSSetScissorRects(1, &sc);
}

// Artscout - 2026: #DX12 A5 -- per-RTT readback slot (a READBACK-heap buffer that a frame's copy of the RTT
// lands in). Keyed by the D3D12Texture* of the off-screen RTT. We record a fresh copy every frame and CONVERT
// it only once the main fence has passed the value that frame's Present signals (GetCompletedValue >= copyFence)
// -- ground-truth completion, self-correcting under GPU lag (converts the latest completed copy, 1-2 frames
// latent). Copies to the same buffer are queue-ordered, and we only Map after the LATEST copy is complete, so
// there is no read/overwrite hazard. NOTE: assumes no backend WaitForGpu fires mid-frame between record and
// Present on this path (true for sensor/menu-viewer rendering; the resize WaitForGpu below runs before record).
namespace
{
struct RbSlot
{
    ID3D12Resource* buf; // READBACK-heap buffer (rowPitch * height bytes)
    unsigned rowPitch; // 256-aligned RGBA8 row pitch
    int w, h; // dims the buffer was sized for
    unsigned __int64
        copyFence; // fence value the recording frame's Present will signal (completion target)
    bool pending; // a copy has been recorded and not yet converted
    RbSlot() : buf(0), rowPitch(0), w(0), h(0), copyFence(0), pending(false)
    {
    }
};
static std::map<void*, RbSlot> s_readback;
static inline unsigned Align256(unsigned v)
{
    return (v + 255u) & ~255u;
}
}

void D3D12Backend::ReadbackRttTo565(void* rttHandle, unsigned short* dst,
                                    int dstStridePix, int dstHeightPix, int x,
                                    int y, int w, int h)
{
    // The whole RTT is copied to the readback buffer; x/y/w/h + stride clip the CONVERSION into dst.
    // #DX12 A5: NEVER open a swap-chain frame here (no EnsureFrameStarted). An off-screen readback must only
    // piggy-back on an already-open command list; forcing a backbuffer BeginFrame outside the render loop (sim
    // update / between VR eye frames) injected an orphan frame -> #527 barrier mismatch + xrEndFrame E_INVALIDARG
    // (black headset, mirror fine). The CONVERT step is CPU-only (runs regardless); only the COPY needs a list.
    if (!m_pDevice || !rttHandle || !dst)
        return;
    D3D12Texture* t = (D3D12Texture*)rttHandle;
    if (!t->tex)
        return;

    RbSlot& s = s_readback[rttHandle];
    const unsigned rowPitch = Align256((unsigned)t->width * 4u);

    // (Re)create the readback buffer if missing / resized.
    if (!s.buf || s.w != t->width || s.h != t->height)
    {
        if (s.buf)
        {
            WaitForGpu();
            s.buf->Release();
            s.buf = 0;
            s.pending = false;
        }
        D3D12_HEAP_PROPERTIES hp;
        ZeroMemory(&hp, sizeof(hp));
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd;
        ZeroMemory(&rd, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)rowPitch * (UINT64)t->height;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // #DX12: COMMON, not COPY_DEST -- a BUFFER can't be created COPY_DEST (D3D12 ignores it -> warning #1328,
        // which the Debug InfoQueue BREAKs on = a stall per call). CopyTextureRegion implicitly promotes it.
        if (FAILED(m_pDevice->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                NULL, __uuidof(ID3D12Resource), (void**)&s.buf)))
        {
            s.buf = 0;
            return;
        }
        s.rowPitch = rowPitch;
        s.w = t->width;
        s.h = t->height;
        s.pending = false;
    }

    // 1) If the pending copy is now complete (GPU passed the fence its frame signals), convert it into dst.
    if (s.pending && m_pFence && m_pFence->GetCompletedValue() >= s.copyFence)
    {
        void* mapped = 0;
        if (SUCCEEDED(s.buf->Map(0, NULL, &mapped)) &&
            mapped) // NULL read range = whole resource (no MAP_INVALIDRANGE)
        {
            int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
            int x1 = x + (w > 0 ? w : t->width),
                y1 = y + (h > 0 ? h : t->height);
            if (x1 > t->width)
                x1 = t->width;
            if (y1 > t->height)
                y1 = t->height;
            if (x1 > dstStridePix)
                x1 = dstStridePix;
            if (y1 > dstHeightPix)
                y1 = dstHeightPix;
            const BYTE* base = (const BYTE*)mapped;
            for (int row = y0; row < y1; ++row)
            {
                const BYTE* src =
                    base + (size_t)row * s.rowPitch + (size_t)x0 * 4;
                unsigned short* d = dst + (size_t)row * dstStridePix + x0;
                for (int col = x0; col < x1; ++col)
                {
                    BYTE r = src[0], g = src[1],
                         b = src[2]; // RTT is R8G8B8A8_UNORM
                    *d++ = (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) |
                                            (b >> 3));
                    src += 4;
                }
            }
            D3D12_RANGE wr;
            wr.Begin = 0;
            wr.End = 0;
            s.buf->Unmap(0, &wr); // CPU wrote nothing back
        }
        s.pending = false;
    }

    // 2) Record THIS frame's copy -- ONLY if a command list is already open (never force one). If we're called
    // off-frame the convert above still delivered the last image; the new copy simply waits for an in-frame call.
    if (!m_bRecording || !m_pList)
        return;
    unsigned before = t->rtState ?
                          t->rtState :
                          (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (before != (unsigned)D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        D3D12_RESOURCE_BARRIER b;
        ZeroMemory(&b, sizeof(b));
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_pList->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dstL;
    ZeroMemory(&dstL, sizeof(dstL));
    dstL.pResource = s.buf;
    dstL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstL.PlacedFootprint.Offset = 0;
    dstL.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstL.PlacedFootprint.Footprint.Width = (UINT)t->width;
    dstL.PlacedFootprint.Footprint.Height = (UINT)t->height;
    dstL.PlacedFootprint.Footprint.Depth = 1;
    dstL.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION srcL;
    ZeroMemory(&srcL, sizeof(srcL));
    srcL.pResource = t->tex;
    srcL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcL.SubresourceIndex = 0;
    m_pList->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, NULL);

    D3D12_RESOURCE_BARRIER b2;
    ZeroMemory(&b2, sizeof(b2));
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition.pResource = t->tex;
    b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_pList->ResourceBarrier(1, &b2);
    t->rtState = (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    s.copyFence =
        NextSignalValue(); // value this frame's Present (MoveToNextFrame) will signal for this allocator
    s.pending = true;
}

void D3D12Backend::CopyRtt(void* srcHandle, void* dstHandle)
{
    if (!srcHandle || !dstHandle)
        return;
    D3D12Texture* s = (D3D12Texture*)srcHandle;
    D3D12Texture* d = (D3D12Texture*)dstHandle;
    if (!s->tex || !d->tex)
        return;
    // #DX12 A5: never open a swap-chain frame here -- only record onto an already-open list (see ReadbackRttTo565).
    // If called off-frame (GM snapshot outside the render loop) skip; the sweep persists in the off-screen RTT
    // and the snapshot lands on the next in-frame call.
    if (!m_pList || !m_bRecording)
        return;

    unsigned sBefore = s->rtState ?
                           s->rtState :
                           (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    unsigned dBefore = d->rtState ?
                           d->rtState :
                           (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_RESOURCE_BARRIER pre[2];
    int n = 0;
    if (sBefore != (unsigned)D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        ZeroMemory(&pre[n], sizeof(pre[0]));
        pre[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[n].Transition.pResource = s->tex;
        pre[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pre[n].Transition.StateBefore = (D3D12_RESOURCE_STATES)sBefore;
        pre[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ++n;
    }
    if (dBefore != (unsigned)D3D12_RESOURCE_STATE_COPY_DEST)
    {
        ZeroMemory(&pre[n], sizeof(pre[0]));
        pre[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pre[n].Transition.pResource = d->tex;
        pre[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pre[n].Transition.StateBefore = (D3D12_RESOURCE_STATES)dBefore;
        pre[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        ++n;
    }
    if (n)
        m_pList->ResourceBarrier(n, pre);

    m_pList->CopyResource(
        d->tex,
        s->tex); // requires identical dims/format (GM panel == sweep RTT: RGBA8, GM_TEXTURE_SIZE)

    D3D12_RESOURCE_BARRIER post[2];
    ZeroMemory(&post[0], sizeof(post[0]));
    post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[0].Transition.pResource = s->tex;
    post[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ZeroMemory(&post[1], sizeof(post[1]));
    post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition.pResource = d->tex;
    post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_pList->ResourceBarrier(2, post);

    s->rtState = (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    d->rtState = (unsigned)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void D3D12Backend::ReleaseReadbackFor(void* rttHandle)
{
    std::map<void*, RbSlot>::iterator it = s_readback.find(rttHandle);
    if (it == s_readback.end())
        return;
    if (it->second.buf)
    {
        if (m_pQueue && m_pFence)
            WaitForGpu();
        it->second.buf->Release();
    }
    s_readback.erase(it);
}

void D3D12Backend::Release()
{
    if (m_pQueue && m_pFence)
        WaitForGpu(); // don't destroy in-flight resources
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = 0;
    }
    D12_RELEASE(m_pQuadPSO);
    D12_RELEASE(m_pQuadPSOBlend);
    D12_RELEASE(m_pQuadRS);
    D12_RELEASE(m_pSrvHeap);
    D12_RELEASE(m_pQuadTex);
    D12_RELEASE(m_pQuadUpload);
    D12_RELEASE(m_pList);
    for (int i = 0; i < kFrameCount; ++i)
        D12_RELEASE(m_pAlloc[i]);
    ReleaseBackBufferViews();
    ReleaseDepthBuffer();
    ReleaseMsaaTargets(); // Artscout - 2026: free MSAA color/depth targets + their descriptor heaps
    ReleaseHdrTargets(m_hdrFlat); // Artscout - 2026: HDR scene + GT7
    ReleaseHdrTargets(m_hdrEye[0]);
    ReleaseHdrTargets(m_hdrEye[1]);
    m_pHdrCur = 0;
    for (int i = 0; i < 4; ++i)
        D12_RELEASE(m_pHdrPso[i]);
    D12_RELEASE(m_pHdrRS);
    D12_RELEASE(m_pHdrVS);
    D12_RELEASE(m_pHdrPS);
    D12_RELEASE(m_pEyeDepthTex);
    D12_RELEASE(m_pEyeDsvHeap);
    ReleaseEyeMsDepth(); // Artscout - 2026: the multisampled eye depth (VrMsaaSamples)
    for (int i = 0; i < 2; ++i)
    {
        D12_RELEASE(m_eyeDepth[i].tex);
        D12_RELEASE(m_eyeDepth[i].dsvHeap);
    } // #DX12 п.5 per-group VI array depth
    for (int i = 0; i < 2; ++i)
    {
        D12_RELEASE(m_viColor[i].tex);
        D12_RELEASE(m_viColor[i].rtvHeap);
    } // #DX12 п.5 quad private VI color arrays
    D12_RELEASE(m_pList1);
    if (m_pMenuRtt)
    {
        if (g_pD3D12TextureManager)
            g_pD3D12TextureManager->Destroy(*m_pMenuRtt);
        delete m_pMenuRtt;
        m_pMenuRtt = 0;
    }
    if (m_pFpsRtt)
    {
        if (g_pD3D12TextureManager)
            g_pD3D12TextureManager->Destroy(*m_pFpsRtt);
        delete m_pFpsRtt;
        m_pFpsRtt = 0;
    }
    if (m_pSubRtt)
    {
        if (g_pD3D12TextureManager)
            g_pD3D12TextureManager->Destroy(*m_pSubRtt);
        delete m_pSubRtt;
        m_pSubRtt = 0;
    }
    D12_RELEASE(m_pMenuDepthTex);
    D12_RELEASE(m_pMenuDsvHeap);
    D12_RELEASE(m_pDepthSrvHeap); // Artscout - 2026: #13 cloud depth-read SRV
    m_depthSrvFor = 0;
    m_pSceneDepthRes = 0;
    m_sceneDepthReadable = false;
    D12_RELEASE(m_pPitShadowTex); // Artscout - 2026: cockpit sun shadow map
    D12_RELEASE(m_pPitShadowDsvHeap);
    D12_RELEASE(m_pPitShadowSrvHeap);
    m_pitShadowRes = 0;
    m_pitShadowReadable = false;
    D12_RELEASE(m_pDsvHeap);
    D12_RELEASE(m_pRttDepthTex); // Artscout - 2026: off-screen RTT depth
    D12_RELEASE(m_pRttDsvHeap);
    D12_RELEASE(m_pRtvHeap);
    D12_RELEASE(m_pFence);
    D12_RELEASE(m_pSwapChain);
    D12_RELEASE(m_pQueue);
    D12_RELEASE(m_pDevice);
    m_bRecording = false;
}
