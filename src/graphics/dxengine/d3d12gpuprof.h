// Artscout - 2026: D3D12 per-pass GPU timing, written to FFDebug.log as "[GPUPROF] ..." every few seconds.
//
// Only the Vulkan backend had a profiler; the D3D12 path (the one the headset runs) had nothing, so "what is
// eating the frame" could only be guessed. This brackets the command list with GPU timestamps and attributes
// each interval to the pass that was active when it started, then reports ms per pass.
//
//   * A "slot" is one command list: one eye in the per-eye VR path, the whole stereo frame under view
//     instancing, one frame on the flat path. Numbers are per slot.
//   * Marks are written only on a change of label, so the cost is a few timestamp queries per slot.
//   * cfg: "set g_bGpuProf 0" turns it off.
#ifndef D3D12GPUPROF_H
#define D3D12GPUPROF_H

struct ID3D12GraphicsCommandList;

// Attribute the time since the previous mark to the PREVIOUS label, and start `label`. No-op if profiling is
// off or the list is not recording. `label` must be a string literal (it is stored by pointer).
void GpuProf_Mark(ID3D12GraphicsCommandList* cl, const char* label);

// Per-slot draw statistics, summed into the next report.
void GpuProf_Count(int draws, int triangles, int terrainChunks);

// CPU-side spans measured elsewhere (ms), summed into the next report.
void GpuProf_AddDrawSceneMs(double ms);   // RenderOTW::DrawScene CPU time
void GpuProf_AddXrWaitMs(double ms);      // xrWaitFrame (the compositor pacing the loop); one call per frame; also the frame tick

// Sim-thread spans (SimulationLoopControl): which part of the CPU frame is it?
enum GpuProfCpu { GPCPU_CAMP_WAIT = 0, GPCPU_REALTIME, GPCPU_SIM_CYCLE, GPCPU_OTW_CYCLE, GPCPU_COUNT };
void GpuProf_AddCpu(int what, double ms);
double GpuProf_NowMs();                   // QPC, for the callers' timers

#endif
