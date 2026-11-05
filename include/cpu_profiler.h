#ifndef CPU_PROFILER_H
#define CPU_PROFILER_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the sampling profiler, register the main game thread, and install XBE timing hooks
void CpuProfiler_Init(void);

// Register a newly created thread for CPU % tracking and EIP sampling
void CpuProfiler_RegisterThread(HANDLE hThread, DWORD threadId, void* startRoutine, const char* customName);

// Notify profiler of a frame swap (pulses 60Hz VBlank event for background threads & updates frame stats)
void CpuProfiler_OnFrameSwap(void);

// Record timed sections on the main thread (in QPC ticks)
void CpuProfiler_RecordPushBufferTicks(int64_t ticks);
void CpuProfiler_RecordPresentTicks(int64_t ticks);

// Dump an immediate CPU & Hot-Function report to stdout and native_sc2.log
void CpuProfiler_DumpReport(void);

// Toggle HLE spinloop mitigations at runtime (F10 hotkey)
void CpuProfiler_ToggleSpinloopFixes(void);
bool CpuProfiler_AreSpinloopFixesEnabled(void);

// Wait until next VBlank (used by HLE D3DDevice_BlockUntilVerticalBlank @ 0x0014FA10)
void CpuProfiler_WaitForVBlank(void);

// Record kernel thunk call frequency
void CpuProfiler_RecordThunkCall(uint32_t thunkIndex, void* callerAddr);

#ifdef __cplusplus
}
#endif

#endif // CPU_PROFILER_H
