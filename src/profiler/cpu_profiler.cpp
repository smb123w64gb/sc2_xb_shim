#include "cpu_profiler.h"
#include "xboxkrnl.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>

// ============================================================================
// NativeSC2 Built-In Statistical CPU Profiler & Thread Spinloop Analyzer
// ============================================================================
// Provides:
// 1. Per-Thread CPU % measurement via Win32 GetThreadTimes() (User + Kernel)
// 2. 1000 Hz (1 ms) Statistical EIP & Stack-Caller Sampling across Xbox threads
// 3. Sub-millisecond Main Thread Frame Breakdown (GameLogic vs DrawFrame vs D3D9)
// 4. Kernel Thunk Frequency Counter & Top Xbox Caller Attribution
// 5. HLE Background Thread Spinloop Mitigations (Toggleable live via F10, Dump via F11)
// ============================================================================

static void profLog(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        DWORD written;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, len, &written, nullptr);
        static FILE* s_fLog = nullptr;
        if (!s_fLog) s_fLog = fopen("native_sc2.log", "a");
        if (s_fLog) {
            fwrite(buf, 1, len, s_fLog);
            fflush(s_fLog);
        }
    }
}

struct KnownSymbolRange {
    uint32_t startAddr;
    uint32_t endAddr;
    const char* name;
};

static const KnownSymbolRange s_knownSymbols[] = {
    // Specific hot functions & thread loops
    { 0x00011000, 0x0001105F, "Namco_SinCos_AngleLUT (0x00011000)" },
    { 0x00012700, 0x0001285F, "Namco_Matrix4x4_TransformVec (0x00012700)" },
    { 0x00023A00, 0x00023CFF, "Namco_CPU_VertexNormal_Skinning (0x00023af0)" },
    { 0x001546E0, 0x001547AF, "D3D_FloatLogExp_Helper (0x001546e0)" },
    { 0x001547B0, 0x0015482F, "FastInvSqrt_NewtonRaphson (0x001547b0)" },
    { 0x00109E70, 0x00109EAF, "CRI_ADXM_LockGuardSpin (FUN_00109e70)" },
    { 0x00109EB0, 0x00109F7F, "CRI_ADXM_WorkerServer (FUN_00109eb0)" },
    { 0x00109F80, 0x00109F8F, "CRI_VBlankThunk (0x00109f80)" },
    { 0x0010A340, 0x0010A3AF, "CRI_ADXM_VSyncThread1 (FUN_0010a340)" },
    { 0x0010A3B0, 0x0010A40F, "CRI_ADXM_VSyncThread2 (FUN_0010a3b0)" },
    { 0x0010CB20, 0x0010CBDF, "CRI_ADXM_ExecServerCallbacks (FUN_0010cb20)" },
    { 0x00118D10, 0x00118E2F, "NWFSX_ProcessAsyncFileReq (FUN_00118d10)" },
    { 0x00118E30, 0x00118E5F, "NWFSX_PollRequestTable (FUN_00118e30)" },
    { 0x00118E60, 0x00118E8F, "NWFSX_AsyncFilePollThread (FUN_00118e60)" },
    { 0x00014C80, 0x000152DF, "Namco_TaskScheduler_Core" },
    { 0x000152E0, 0x0001593F, "Namco_TaskRunner_Exec (FUN_000152e0)" },
    { 0x00015940, 0x00017FFF, "Namco_SceneGraph_3DTraversal" },
    { 0x000272C0, 0x0002799F, "Namco_TextPrint_Renderer (Print)" },
    { 0x00028D40, 0x00029CFF, "Namco_ViewportCamera_Setup" },
    { 0x0002AA30, 0x0002CAFF, "Namco_2D3D_SpriteStrip_Primitives" },
    { 0x0002D4B0, 0x0002DD3F, "Namco_3DStage_Character_Render (FUN_0002dab0)" },
    { 0x0002DD40, 0x0002E89F, "Namco_DrawFrame_Main (0x0002dd40)" },
    { 0x0002EBD0, 0x0002F8FF, "Namco_Geometric_Primitives (Box2D/Line/Ball)" },
    { 0x000313B0, 0x000333FF, "Namco_D3DState_PrimitiveDispatcher" },
    { 0x00033C90, 0x00033CCF, "SC2_Game_Main (0x00033c90)" },
    { 0x000E21B0, 0x000F0EBF, "SC2_TaskLogic_Audio_Input_Update" },
    { 0x000F0EC0, 0x000F1200, "SC2_GameLogic_Main (0x000f0ec0)" },
    { 0x0014E000, 0x0015EFFF, "Xbox_D3D8_PushBuffer_Library" },
    { 0x00160000, 0x0016EFFF, "Xbox_DirectSound_APU_Library" },
    // Broad subsystem fallbacks
    { 0x00011000, 0x00033FFF, "Namco_Engine_Core_Graphics" },
    { 0x00034000, 0x000EFFFF, "SC2_Gameplay_Character_Physics_AI" },
    { 0x000F0000, 0x00102FFF, "SC2_Menu_UI_Subsystems" },
    { 0x00103000, 0x00109CFF, "Xbox_XAPI_CRT_Runtime" },
    { 0x00109D00, 0x0013FFFF, "CRI_ADX_Sofdec_Middleware" },
};

static const char* ResolveXboxSymbol(uint32_t addr) {
    if (addr < 0x00010000 || addr >= 0x001A0000) return "External/Win32_DLL";
    for (size_t i = 0; i < sizeof(s_knownSymbols) / sizeof(s_knownSymbols[0]); i++) {
        if (addr >= s_knownSymbols[i].startAddr && addr <= s_knownSymbols[i].endAddr) {
            return s_knownSymbols[i].name;
        }
    }
    return "SC2_Unknown_XBE_Code";
}

static const char* GuessThreadName(void* startRoutine, const char* customName) {
    if (customName && customName[0]) return customName;
    uint32_t addr = (uint32_t)(uintptr_t)startRoutine;
    if (addr == 0x00109E70) return "CRI_LockGuardSpin_00109E70";
    if (addr == 0x0010A340) return "CRI_VSyncThread1_0010A340";
    if (addr == 0x0010A3B0) return "CRI_VSyncThread2_0010A3B0";
    if (addr == 0x00109EB0) return "CRI_WorkerServer_00109EB0";
    if (addr == 0x00118E60) return "NWFSX_FilePoll_00118E60";
    if (addr == 0x00033C90 || addr == 0x00105423) return "SC2_MainGameThread";
    return "Xbox_WorkerThread";
}

#define MAX_TRACKED_THREADS 32
struct TrackedThread {
    bool active;
    bool sampleXboxCode;
    HANDLE hThread;
    DWORD threadId;
    uint32_t startAddr;
    char name[48];
    uint64_t prevKernelTime;
    uint64_t prevUserTime;
    uint32_t activeSamples;
    uint32_t lastEip;
    uint32_t topEip;
};

#define EIP_BUCKET_COUNT 512
struct EipSampleBucket {
    uint32_t bucketAddr; // 64-byte aligned XBE address
    uint32_t exactEip;   // most recent exact EIP in bucket
    uint32_t callerAddr; // Xbox caller if sampled in kernel/shim
    uint32_t hitCount;
    DWORD lastThreadId;
};

struct ThunkStat {
    uint32_t count;
    uint32_t lastCaller;
};

static CRITICAL_SECTION s_profLock;
static bool s_profInitialized = false;
static volatile bool s_spinloopFixesEnabled = true;
static HANDLE s_hVBlankEvent = nullptr;
static HANDLE s_hProfThread = nullptr;
static DWORD s_mainThreadId = 0;
static uint32_t s_numLogicalCores = 1;

static TrackedThread s_threads[MAX_TRACKED_THREADS];
static int s_threadCount = 0;

static EipSampleBucket s_buckets[EIP_BUCKET_COUNT];
static ThunkStat s_thunkStats[120];
static volatile LONG s_vblankWaitCalls = 0;

// Per-frame main thread QPC accumulators
static LARGE_INTEGER s_qpcFreq = { 0 };
static volatile int64_t s_accGameLogicTicks = 0;
static volatile int64_t s_accDrawFrameTicks = 0;
static volatile int64_t s_accPushBufferTicks = 0;
static volatile int64_t s_accPresentTicks = 0;
static volatile uint32_t s_measuredFrames = 0;

static uint64_t FileTimeToU64(const FILETIME& ft) {
    return ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
}

static void RecordEipSample(uint32_t xbeAddr, uint32_t callerAddr, DWORD threadId) {
    uint32_t bucketKey = xbeAddr & ~0x3Fu; // 64-byte function block granularity
    int emptySlot = -1;
    for (int i = 0; i < EIP_BUCKET_COUNT; i++) {
        if (s_buckets[i].bucketAddr == bucketKey) {
            s_buckets[i].hitCount++;
            s_buckets[i].exactEip = xbeAddr;
            if (callerAddr) s_buckets[i].callerAddr = callerAddr;
            s_buckets[i].lastThreadId = threadId;
            return;
        }
        if (emptySlot == -1 && s_buckets[i].hitCount == 0) {
            emptySlot = i;
        }
    }
    if (emptySlot != -1) {
        s_buckets[emptySlot].bucketAddr = bucketKey;
        s_buckets[emptySlot].exactEip = xbeAddr;
        s_buckets[emptySlot].callerAddr = callerAddr;
        s_buckets[emptySlot].hitCount = 1;
        s_buckets[emptySlot].lastThreadId = threadId;
    }
}

extern "C" void CpuProfiler_RegisterThread(HANDLE hThread, DWORD threadId, void* startRoutine, const char* customName) {
    if (!s_profInitialized) return;
    EnterCriticalSection(&s_profLock);
    if (s_threadCount < MAX_TRACKED_THREADS) {
        HANDLE hDup = nullptr;
        DuplicateHandle(GetCurrentProcess(), hThread, GetCurrentProcess(), &hDup,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        if (hDup) {
            TrackedThread& t = s_threads[s_threadCount++];
            memset(&t, 0, sizeof(TrackedThread));
            t.active = true;
            t.hThread = hDup;
            t.threadId = threadId ? threadId : GetThreadId(hDup);
            t.startAddr = (uint32_t)(uintptr_t)startRoutine;
            const char* label = GuessThreadName(startRoutine, customName);
            strncpy(t.name, label, sizeof(t.name) - 1);

            // Sample Xbox game threads + main thread
            t.sampleXboxCode = (t.startAddr >= 0x00010000 && t.startAddr < 0x001A0000) ||
                               (t.threadId == s_mainThreadId);

            FILETIME ftCreate, ftExit, ftKernel, ftUser;
            if (GetThreadTimes(hDup, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                t.prevKernelTime = FileTimeToU64(ftKernel);
                t.prevUserTime = FileTimeToU64(ftUser);
            }
            profLog("[CPU_PROF] Registered Thread #%d: tid=%lu, entry=0x%08X (%s)\n",
                    s_threadCount - 1, t.threadId, t.startAddr, t.name);
        }
    }
    LeaveCriticalSection(&s_profLock);
}

extern "C" void CpuProfiler_RecordThunkCall(uint32_t thunkIndex, void* callerAddr) {
    if (thunkIndex < 120) {
        InterlockedIncrement((volatile LONG*)&s_thunkStats[thunkIndex].count);
        s_thunkStats[thunkIndex].lastCaller = (uint32_t)(uintptr_t)callerAddr;
    }
}

extern "C" void CpuProfiler_RecordPushBufferTicks(int64_t ticks) {
    s_accPushBufferTicks += ticks;
}

extern "C" void CpuProfiler_RecordPresentTicks(int64_t ticks) {
    s_accPresentTicks += ticks;
}

static volatile LONG s_vblankCounter = 0;
static __declspec(thread) LONG t_lastSeenVBlank = 0;

extern "C" void CpuProfiler_OnFrameSwap(void) {
    InterlockedIncrement((volatile LONG*)&s_measuredFrames);
    InterlockedIncrement(&s_vblankCounter);
    if (s_hVBlankEvent) {
        // Signal all Xbox VBlank waiter threads (CRI_VSyncThread1 @ 0x0010A340, CRI_VSyncThread2 @ 0x0010A3B0)
        PulseEvent(s_hVBlankEvent);
    }
}

extern "C" void CpuProfiler_WaitForVBlank(void) {
    InterlockedIncrement(&s_vblankWaitCalls);
    if (!s_spinloopFixesEnabled) {
        // Unthrottled legacy behavior (returns immediately, causing CRI VSync threads to spin at 100% CPU)
        return;
    }
    // On the main game thread, NativeSC2_OnFrameSwap already paces frames to 60Hz
    if (GetCurrentThreadId() == s_mainThreadId) {
        return;
    }
    LONG curVBlank = s_vblankCounter;
    if (curVBlank == t_lastSeenVBlank) {
        for (int tries = 0; tries < 16 && s_vblankCounter == curVBlank; ++tries) {
            Sleep(1);
        }
    }
    t_lastSeenVBlank = s_vblankCounter;
}

extern "C" bool CpuProfiler_AreSpinloopFixesEnabled(void) {
    return s_spinloopFixesEnabled;
}

extern "C" void CpuProfiler_ToggleSpinloopFixes(void) {
    s_spinloopFixesEnabled = !s_spinloopFixesEnabled;
    profLog("\n================================================================================\n");
    profLog("[CPU_PROF] HLE Background Thread Spinloop Mitigations: %s (Press F10 to toggle)\n",
            s_spinloopFixesEnabled ? "ENABLED (Low CPU Mode)" : "DISABLED (Raw Xbox Spinloops - ~30% CPU)");
    profLog("================================================================================\n\n");
}

// ============================================================================
// HLE Hook for CRI ADXM Lock Guard Spin Thread (FUN_00109e70 @ 0x00109E70)
// ============================================================================
// Original Xbox code:
//   while (DAT_00608830 == 0) { DAT_00608818++; }
// On single-core Xbox, this priority-2 thread blocks lower-priority threads when resumed.
// On multi-core PCs, it burns 100% of a dedicated CPU core forever if unthrottled.
static void __stdcall Hook_CRI_LockGuardThread_00109E70(void) {
    volatile uint32_t* pStopFlag = (volatile uint32_t*)0x00608830;
    volatile uint32_t* pCounter  = (volatile uint32_t*)0x00608818;
    volatile uint32_t* pDoneFlag = (volatile uint32_t*)0x00608834;

    while (*pStopFlag == 0) {
        (*pCounter)++;
        if (s_spinloopFixesEnabled) {
            Sleep(1);
        }
    }
    *pDoneFlag = 1;
    ExitThread(0xF0000001u);
}

static CRITICAL_SECTION s_criAdxmLock;

// HLE multi-core safe replacement for CRI ADXM Lock (0x00109D60) and Unlock (0x00109DB0).
// On single-core Xbox, CRI boosted thread priority to 15 and resumed the priority-2 spin thread (0x00109E70).
// On multi-core PCs, a real recursive CRITICAL_SECTION guarantees mutual exclusion across cores without
// thousands of NtResumeThread/NtSuspendThread/NtSetInformationThread syscalls per second.
static void __cdecl Hook_CRI_Lock_00109D60(void) {
    EnterCriticalSection(&s_criAdxmLock);
    (*(volatile uint32_t*)0x00608810)++;
}

static void __cdecl Hook_CRI_Unlock_00109DB0(void) {
    volatile uint32_t* pDepth = (volatile uint32_t*)0x00608810;
    if (*pDepth > 0) {
        (*pDepth)--;
    }
    LeaveCriticalSection(&s_criAdxmLock);
}

// ============================================================================
// Main Thread Timing Wrappers for GameLogic (0x000F0EC0) & DrawFrame (0x0002DD40)
// ============================================================================
static void __cdecl Timed_GameLogic_Wrapper(void) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    typedef void (__cdecl *GameLogicFn)(void);
    ((GameLogicFn)0x000F0EC0)();
    QueryPerformanceCounter(&t1);
    s_accGameLogicTicks += (t1.QuadPart - t0.QuadPart);
}

static int __cdecl Timed_DrawFrame_Wrapper(void) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    typedef int (__cdecl *DrawFrameFn)(void);
    int res = ((DrawFrameFn)0x0002DD40)();
    QueryPerformanceCounter(&t1);
    s_accDrawFrameTicks += (t1.QuadPart - t0.QuadPart);
    return res;
}

static void PatchCallRel32(uint32_t callInstrAddr, void* targetFn) {
    DWORD oldProtect;
    VirtualProtect((void*)callInstrAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    int32_t rel = (int32_t)((uintptr_t)targetFn - (callInstrAddr + 5));
    *(uint8_t*)callInstrAddr = 0xE8;
    memcpy((void*)(callInstrAddr + 1), &rel, 4);
}

static void PatchJmpRel32(uint32_t targetAddr, void* hookFn) {
    DWORD oldProtect;
    VirtualProtect((void*)targetAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    int32_t rel = (int32_t)((uintptr_t)hookFn - (targetAddr + 5));
    *(uint8_t*)targetAddr = 0xE9;
    memcpy((void*)(targetAddr + 1), &rel, 4);
}

// ============================================================================
// Report Generator (Console + native_sc2.log)
// ============================================================================
static uint64_t s_lastReportWallTime = 0;
static uint64_t s_lastProcKernel = 0;
static uint64_t s_lastProcUser = 0;

extern "C" void CpuProfiler_DumpReport(void) {
    if (!s_profInitialized) return;

    FILETIME ftNow, ftCreation, ftExit, ftProcKernel, ftProcUser;
    GetSystemTimeAsFileTime(&ftNow);
    uint64_t curWall = FileTimeToU64(ftNow);
    uint64_t wallDelta = curWall - s_lastReportWallTime;
    if (wallDelta < 100000) wallDelta = 100000; // minimum 10ms to avoid div-by-zero
    double wallSec = (double)wallDelta / 10000000.0;

    double totalSystemCpuPct = 0.0;
    double totalCoresUsed = 0.0;
    if (GetProcessTimes(GetCurrentProcess(), &ftCreation, &ftExit, &ftProcKernel, &ftProcUser)) {
        uint64_t k = FileTimeToU64(ftProcKernel);
        uint64_t u = FileTimeToU64(ftProcUser);
        uint64_t procDelta = (k - s_lastProcKernel) + (u - s_lastProcUser);
        s_lastProcKernel = k;
        s_lastProcUser = u;
        totalCoresUsed = (double)procDelta / (double)wallDelta;
        totalSystemCpuPct = (totalCoresUsed * 100.0) / (double)s_numLogicalCores;
    }
    s_lastReportWallTime = curWall;

    EnterCriticalSection(&s_profLock);

    profLog("\n+==============================================================================================+\n");
    profLog("| [CPU_PROFILER] Window: %.2fs | Total CPU: %5.1f%% (%4.2f / %u cores) | SpinFixes(F10): %-3s |\n",
            wallSec, totalSystemCpuPct, totalCoresUsed, s_numLogicalCores,
            s_spinloopFixesEnabled ? "ON" : "OFF");
    profLog("+----------------------------------------------------------------------------------------------+\n");
    profLog("| PER-THREAD CPU USAGE (100%% Core = 1 Full Logical Core, Sys%% = Task Manager Total CPU %%)      |\n");
    profLog("| %-6s | %-28s | %-10s | %7s | %7s | %7s | %-10s |\n",
            "TID", "Thread Name", "EntryAddr", "Core %", "Sys %", "Samples", "Last EIP");
    profLog("+--------+------------------------------+------------+---------+---------+---------+------------+\n");

    for (int i = 0; i < s_threadCount; i++) {
        TrackedThread& t = s_threads[i];
        if (!t.active) continue;

        FILETIME ftC, ftE, ftK, ftU;
        double corePct = 0.0;
        double sysPct = 0.0;
        if (GetThreadTimes(t.hThread, &ftC, &ftE, &ftK, &ftU)) {
            uint64_t k = FileTimeToU64(ftK);
            uint64_t u = FileTimeToU64(ftU);
            uint64_t delta = (k - t.prevKernelTime) + (u - t.prevUserTime);
            t.prevKernelTime = k;
            t.prevUserTime = u;
            corePct = ((double)delta * 100.0) / (double)wallDelta;
            sysPct = corePct / (double)s_numLogicalCores;
        } else {
            t.active = false;
            continue;
        }

        profLog("| %-6lu | %-28s | 0x%08X | %6.1f%% | %6.2f%% | %7u | 0x%08X |\n",
                t.threadId, t.name, t.startAddr, corePct, sysPct, t.activeSamples, t.lastEip);
        t.activeSamples = 0;
    }

    // Main Thread Frame Timing Breakdown
    uint32_t frames = s_measuredFrames;
    int64_t logicTicks = s_accGameLogicTicks;
    int64_t drawTicks  = s_accDrawFrameTicks;
    int64_t pushTicks  = s_accPushBufferTicks;
    int64_t presTicks  = s_accPresentTicks;
    s_measuredFrames = 0;
    s_accGameLogicTicks = 0;
    s_accDrawFrameTicks = 0;
    s_accPushBufferTicks = 0;
    s_accPresentTicks = 0;

    if (frames > 0 && s_qpcFreq.QuadPart > 0) {
        double toMs = 1000.0 / ((double)s_qpcFreq.QuadPart * (double)frames);
        double logicMs = (double)logicTicks * toMs;
        double drawTotalMs = (double)drawTicks * toMs;
        double pushMs = (double)pushTicks * toMs;
        double presMs = (double)presTicks * toMs;
        double sc2DrawCpuMs = drawTotalMs - pushMs - presMs;
        if (sc2DrawCpuMs < 0.0) sc2DrawCpuMs = 0.0;

        profLog("+----------------------------------------------------------------------------------------------+\n");
        profLog("| MAIN THREAD PER-FRAME BREAKDOWN (Avg over %u frames, %.1f FPS):                              |\n",
                frames, (double)frames / wallSec);
        profLog("|   GameLogic (0x000F0EC0): %5.2f ms | SC2 DrawFrame CPU: %5.2f ms | PushBuf: %5.2f ms | Present: %5.2f ms |\n",
                logicMs, sc2DrawCpuMs, pushMs, presMs);
    }

    // Top 10 Hot Xbox Code Buckets
    profLog("+----------------------------------------------------------------------------------------------+\n");
    profLog("| TOP HOT XBOX XBE FUNCTIONS (Statistical EIP & Stack-Caller Samples - Candidates for HLE)     |\n");
    profLog("| %-4s | %-10s | %-10s | %-7s | %-6s | %-40s |\n",
            "Rank", "FuncBlock", "Exact EIP", "Samples", "TID", "Symbol / Subsystem");
    profLog("+------+------------+------------+---------+--------+------------------------------------------+\n");

    for (int rank = 1; rank <= 10; rank++) {
        int bestIdx = -1;
        uint32_t bestHits = 0;
        for (int i = 0; i < EIP_BUCKET_COUNT; i++) {
            if (s_buckets[i].hitCount > bestHits) {
                bestHits = s_buckets[i].hitCount;
                bestIdx = i;
            }
        }
        if (bestIdx == -1 || bestHits == 0) break;

        EipSampleBucket b = s_buckets[bestIdx];
        s_buckets[bestIdx].hitCount = 0; // reset for next window
        profLog("| #%-3d | 0x%08X | 0x%08X | %7u | %-6lu | %-40s |\n",
                rank, b.bucketAddr, b.exactEip, b.hitCount, b.lastThreadId, ResolveXboxSymbol(b.exactEip));
    }

    // Clear remaining buckets
    memset(s_buckets, 0, sizeof(s_buckets));

    // Top Kernel Thunk Calls/Sec
    profLog("+----------------------------------------------------------------------------------------------+\n");
    profLog("| TOP KERNEL / VBLANK CALL RATES (Calls/sec & Last Xbox Caller):                               |\n");
    LONG vbCalls = InterlockedExchange(&s_vblankWaitCalls, 0);
    if (vbCalls > 0) {
        profLog("|   %-30s : %8.0f calls/sec\n", "D3DDevice_BlockUntilVBlank", (double)vbCalls / wallSec);
    }
    for (int top = 0; top < 5; top++) {
        int bestT = -1;
        uint32_t bestC = 0;
        for (int i = 0; i < 120; i++) {
            if (s_thunkStats[i].count > bestC) {
                bestC = s_thunkStats[i].count;
                bestT = i;
            }
        }
        if (bestT == -1 || bestC == 0) break;
        uint32_t caller = s_thunkStats[bestT].lastCaller;
        InterlockedExchange((volatile LONG*)&s_thunkStats[bestT].count, 0);
        profLog("|   %-30s : %8.0f calls/sec (Last Caller: 0x%08X - %s)\n",
                XboxKrnl_GetThunkNameByIndex(bestT), (double)bestC / wallSec,
                caller, ResolveXboxSymbol(caller));
    }
    for (int i = 0; i < 120; i++) s_thunkStats[i].count = 0;

    profLog("+==============================================================================================+\n\n");
    LeaveCriticalSection(&s_profLock);
}

// ============================================================================
// 1000 Hz Background Statistical Sampler Thread
// ============================================================================
static DWORD WINAPI ProfilerSamplerThread(LPVOID lpParam) {
    (void)lpParam;
    DWORD lastDumpTick = GetTickCount();

    while (true) {
        Sleep(5); // 200 Hz low-overhead statistical sampling

        EnterCriticalSection(&s_profLock);
        for (int i = 0; i < s_threadCount; i++) {
            TrackedThread& t = s_threads[i];
            if (!t.active || !t.sampleXboxCode || !t.hThread) continue;

            if (SuspendThread(t.hThread) != (DWORD)-1) {
                CONTEXT ctx;
                memset(&ctx, 0, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(t.hThread, &ctx)) {
                    uint32_t eip = (uint32_t)ctx.Eip;
                    uint32_t esp = (uint32_t)ctx.Esp;
                    uint32_t xbeEip = 0;
                    uint32_t caller = 0;

                    if (eip >= 0x00011000 && eip < 0x0019B000) {
                        xbeEip = eip;
                    } else if (esp >= 0x00010000 && !IsBadReadPtr((const void*)esp, 64)) {
                        // Scan top 16 stack slots for an Xbox XBE return address
                        const uint32_t* stack = (const uint32_t*)esp;
                        for (int s = 0; s < 16; s++) {
                            uint32_t val = stack[s];
                            if (val >= 0x00011000 && val < 0x0019B000) {
                                caller = val;
                                break;
                            }
                        }
                    }

                    ResumeThread(t.hThread);

                    if (xbeEip != 0) {
                        t.lastEip = xbeEip;
                        t.activeSamples++;
                        RecordEipSample(xbeEip, 0, t.threadId);
                    } else if (caller != 0) {
                        // Ignore intentional blocking waits (BlockUntilVBlank @ 0x0010A340/0x0010A3B0, SleepEx, WaitForSingleObject)
                        if (caller < 0x00105500 || caller > 0x00105650) {
                            t.lastEip = caller;
                        }
                    }
                } else {
                    ResumeThread(t.hThread);
                }
            }
        }
        LeaveCriticalSection(&s_profLock);

        DWORD nowTick = GetTickCount();
        if (nowTick - lastDumpTick >= 10000) { // Auto-dump every 10 seconds
            lastDumpTick = nowTick;
            CpuProfiler_DumpReport();
        }
    }
    return 0;
}

extern "C" void CpuProfiler_Init(void) {
    if (s_profInitialized) return;

    InitializeCriticalSection(&s_profLock);
    InitializeCriticalSection(&s_criAdxmLock);
    QueryPerformanceFrequency(&s_qpcFreq);

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    s_numLogicalCores = sysInfo.dwNumberOfProcessors > 0 ? sysInfo.dwNumberOfProcessors : 1;

    // Manual-reset event pulsed on every 60Hz frame swap so multiple VBlank waiter threads wake together
    s_hVBlankEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    s_mainThreadId = GetCurrentThreadId();

    FILETIME ftNow, ftC, ftE, ftK, ftU;
    GetSystemTimeAsFileTime(&ftNow);
    s_lastReportWallTime = FileTimeToU64(ftNow);
    if (GetProcessTimes(GetCurrentProcess(), &ftC, &ftE, &ftK, &ftU)) {
        s_lastProcKernel = FileTimeToU64(ftK);
        s_lastProcUser = FileTimeToU64(ftU);
    }

    s_profInitialized = true;

    // Register the Main Game Thread
    CpuProfiler_RegisterThread(GetCurrentThread(), s_mainThreadId, (void*)0x00033C90, "SC2_MainGameThread");

    // Install HLE VBlank Wait Hook @ 0x0014FA10 (D3DDevice_BlockUntilVerticalBlank)
    PatchJmpRel32(0x0014FA10, (void*)CpuProfiler_WaitForVBlank);
    profLog("[CPU_PROF] Hooked D3DDevice_BlockUntilVerticalBlank @ 0x0014FA10 -> 60Hz Event Sync\n");

    // Install HLE Hook for CRI ADXM Lock Guard Spin Thread @ 0x00109E70 & Multi-Core Lock/Unlock @ 0x00109D60, 0x00109DB0
    PatchJmpRel32(0x00109E70, (void*)Hook_CRI_LockGuardThread_00109E70);
    PatchJmpRel32(0x00109D60, (void*)Hook_CRI_Lock_00109D60);
    PatchJmpRel32(0x00109DB0, (void*)Hook_CRI_Unlock_00109DB0);
    profLog("[CPU_PROF] Hooked CRI_ADXM_LockGuardSpin @ 0x00109E70 & CRITICAL_SECTION Lock/Unlock @ 0x00109D60/0x00109DB0\n");

    // Install Main Thread QPC Timing Wrappers in Game_Main (0x00033C90)
    PatchCallRel32(0x00033CAB, (void*)Timed_DrawFrame_Wrapper);
    PatchCallRel32(0x00033CB5, (void*)Timed_GameLogic_Wrapper);
    PatchCallRel32(0x00033CBA, (void*)Timed_DrawFrame_Wrapper);
    profLog("[CPU_PROF] Installed QPC Frame Timing Wrappers on GameLogic (0x000F0EC0) & DrawFrame (0x0002DD40)\n");

    s_hProfThread = CreateThread(nullptr, 0, ProfilerSamplerThread, nullptr, 0, nullptr);
    profLog("[CPU_PROF] Statistical Sampler Thread started (Hotkeys: F11 = Dump Report, F10 = Toggle Spinloop HLE)\n");
}
