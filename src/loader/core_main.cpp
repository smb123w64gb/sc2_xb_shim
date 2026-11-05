#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include "xbe.h"
#include "xboxkrnl.h"
#include "xapi_shim.h"
#include "audio_bridge.h"
#include "cpu_profiler.h"
#include "mod_engine.h"

static void printCore(const char* msg) {
    DWORD written;
    DWORD len = 0;
    while (msg[len]) len++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msg, len, &written, NULL);
}

static void printFmt(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printCore(buf);
}

static void printHex(const char* prefix, DWORD val) {
    char buf[64];
    const char hexDigits[] = "0123456789ABCDEF";
    int p = 0;
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 7; i >= 0; i--) {
        buf[p++] = hexDigits[(val >> (i * 4)) & 0xF];
    }
    buf[p++] = '\n';
    buf[p] = 0;
    printCore(buf);
}

// Hardware MMIO and General Exception Handler
static LONG WINAPI HardwareExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 || code == 0x40010006 || code == 0x4001000A) { // C++ runtime throw or OutputDebugString
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t eip = (uintptr_t)pExceptionInfo->ContextRecord->Eip;
    if (eip == 0) {
        uintptr_t* pStack = (uintptr_t*)pExceptionInfo->ContextRecord->Esp;
        uintptr_t retAddr = *pStack;
        pExceptionInfo->ContextRecord->Eip = retAddr;
        pExceptionInfo->ContextRecord->Esp += 4;
        pExceptionInfo->ContextRecord->Eax = 0;
        printHex("  Absorbed call to NULL pointer. Resuming at return address: ", retAddr);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t faultAddr = (uintptr_t)pExceptionInfo->ExceptionRecord->ExceptionInformation[1];

        // Xbox physical memory alias (0x80000000 - 0x8FFFFFFF uncached, 0xF0000000+ write-combining)
        if ((faultAddr & 0xF0000000) == 0x80000000 || (faultAddr & 0xF0000000) == 0xF0000000) {
            uintptr_t physAddr = faultAddr & 0x0FFFFFFF;
            int fixed = 0;
            if (pExceptionInfo->ContextRecord->Eax == faultAddr) { pExceptionInfo->ContextRecord->Eax = physAddr; fixed = 1; }
            if (pExceptionInfo->ContextRecord->Ecx == faultAddr) { pExceptionInfo->ContextRecord->Ecx = physAddr; fixed = 1; }
            if (pExceptionInfo->ContextRecord->Edx == faultAddr) { pExceptionInfo->ContextRecord->Edx = physAddr; fixed = 1; }
            if (pExceptionInfo->ContextRecord->Ebx == faultAddr) { pExceptionInfo->ContextRecord->Ebx = physAddr; fixed = 1; }
            if (pExceptionInfo->ContextRecord->Esi == faultAddr) { pExceptionInfo->ContextRecord->Esi = physAddr; fixed = 1; }
            if (pExceptionInfo->ContextRecord->Edi == faultAddr) { pExceptionInfo->ContextRecord->Edi = physAddr; fixed = 1; }
            if (pExceptionInfo->ContextRecord->Ebp == faultAddr) { pExceptionInfo->ContextRecord->Ebp = physAddr; fixed = 1; }
            if (fixed) {
                printCore("  Translated 0x8xxxxxxx physical register alias to user-space. Resuming...\n");
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        // Fast silent auto-commit for dynamic Xbox memory ranges (e.g. Sofdec movie buffers at 0x08D68000+)
        if (faultAddr >= 0x00010000 && faultAddr < 0x40000000) {
            MEMORY_BASIC_INFORMATION faultMbi;
            if (VirtualQuery((void*)faultAddr, &faultMbi, sizeof(faultMbi))) {
                void* p = NULL;
                if (faultMbi.State == MEM_RESERVE) {
                    // Commit the entire reserved sub-region (up to 8 MB at once) to avoid per-64KB page fault storms
                    SIZE_T commitSize = faultMbi.RegionSize;
                    if (commitSize > 0x800000) commitSize = 0x800000;
                    p = VirtualAlloc(faultMbi.BaseAddress, commitSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    if (!p) {
                        uintptr_t chunk = faultAddr & ~0xFFFF;
                        p = VirtualAlloc((void*)chunk, 0x100000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    }
                    if (!p) {
                        uintptr_t page = faultAddr & ~0xFFF;
                        p = VirtualAlloc((void*)page, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    }
                } else if (faultMbi.State == MEM_FREE) {
                    uintptr_t chunkBase = faultAddr & ~0xFFFFF;
                    p = VirtualAlloc((void*)chunkBase, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    if (!p) {
                        p = VirtualAlloc((void*)chunkBase, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                    }
                } else if (faultMbi.State == MEM_COMMIT &&
                           (faultMbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) == 0) {
                    DWORD oldP;
                    if (VirtualProtect(faultMbi.BaseAddress, faultMbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldP)) {
                        p = faultMbi.BaseAddress;
                    }
                }

                if (p) {
                    printHex("[MEM] Auto-committed Xbox memory region at: ", (DWORD)p);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
    }

    printHex("[EXCEPTION] Code: ", code);
    printHex("  EIP: ", eip);

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)eip, &mbi, sizeof(mbi))) {
        printHex("  EIP BaseAddress: ", (DWORD)mbi.BaseAddress);
        printHex("  EIP AllocationBase: ", (DWORD)mbi.AllocationBase);
        printHex("  EIP Protect: ", mbi.Protect);
        printHex("  EIP State: ", mbi.State);
        char modName[MAX_PATH] = { 0 };
        if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, modName, sizeof(modName))) {
            printCore("  Module: ");
            printCore(modName);
            printCore("\n");
        }
    }

    uintptr_t esp = (uintptr_t)pExceptionInfo->ContextRecord->Esp;
    printHex("  ESP: ", esp);
    uintptr_t* stack = (uintptr_t*)esp;
    for (int i = 0; i < 8; i++) {
        printHex("    Stack: ", stack[i]);
    }

    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t faultAddr = (uintptr_t)pExceptionInfo->ExceptionRecord->ExceptionInformation[1];
        DWORD isWrite = (DWORD)pExceptionInfo->ExceptionRecord->ExceptionInformation[0];
        printHex("  Fault Address: ", faultAddr);
        printHex("  Is Write: ", isWrite);

        // Xbox NV2A MMIO range: 0xFD000000 - 0xFDFFFFFF
        if (faultAddr >= 0xFD000000 && faultAddr < 0xFE000000) {
            printCore("  Access in NV2A MMIO range. Absorbing...\n");
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    (void)ctrlType;
    TerminateProcess(GetCurrentProcess(), 0);
    return TRUE;
}

static char s_exeDir[MAX_PATH] = { 0 };
static char s_resolvedXbePath[MAX_PATH] = { 0 };
static BOOL s_logAllFileLoads = FALSE;

static void InitExeAndGameDirectory(const char* userXbePath) {
    char exePath[MAX_PATH] = { 0 };
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char* lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) lastSlash = strrchr(exePath, '/');
        if (lastSlash) {
            *lastSlash = '\0';
            strncpy(s_exeDir, exePath, MAX_PATH - 1);
        } else {
            GetCurrentDirectoryA(MAX_PATH, s_exeDir);
        }
    } else {
        GetCurrentDirectoryA(MAX_PATH, s_exeDir);
    }

    SetCurrentDirectoryA(s_exeDir);
    XboxKrnl_SetGameDirectory(s_exeDir);

    if (userXbePath && userXbePath[0] != '\0' && userXbePath[0] != '-') {
        if ((userXbePath[1] == ':' && (userXbePath[2] == '\\' || userXbePath[2] == '/')) ||
            (userXbePath[0] == '\\' && userXbePath[1] == '\\')) {
            strncpy(s_resolvedXbePath, userXbePath, MAX_PATH - 1);
        } else {
            snprintf(s_resolvedXbePath, sizeof(s_resolvedXbePath), "%s\\%s", s_exeDir, userXbePath);
        }
    } else {
        snprintf(s_resolvedXbePath, sizeof(s_resolvedXbePath), "%s\\default.xbe", s_exeDir);
        if (GetFileAttributesA(s_resolvedXbePath) == INVALID_FILE_ATTRIBUTES) {
            char altPath[MAX_PATH];
            snprintf(altPath, sizeof(altPath), "%s\\Default.xbe", s_exeDir);
            if (GetFileAttributesA(altPath) != INVALID_FILE_ATTRIBUTES) {
                strncpy(s_resolvedXbePath, altPath, MAX_PATH - 1);
            }
        }
    }
}

static void ParseCommandLineRenderConfig(void) {
    uint32_t renderW = 1280;
    uint32_t renderH = 720;
    BOOL fullscreen = FALSE;

    const char* cmdLine = GetCommandLineA();
    if (!cmdLine) {
        NativeSC2_SetRenderConfig(renderW, renderH, fullscreen);
        return;
    }

    // Tokenize command line
    char tokens[32][128];
    int numTokens = 0;
    const char* p = cmdLine;
    while (*p && numTokens < 32) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int len = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && len < 127) tokens[numTokens][len++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && len < 127) tokens[numTokens][len++] = *p++;
        }
        tokens[numTokens][len] = '\0';
        numTokens++;
    }

    for (int i = 1; i < numTokens; i++) {
        const char* tok = tokens[i];
        if (_stricmp(tok, "--4k") == 0 || _stricmp(tok, "-4k") == 0 ||
            _stricmp(tok, "--2160p") == 0 || _stricmp(tok, "-2160p") == 0) {
            renderW = 3840;
            renderH = 2160;
        } else if (_stricmp(tok, "--1440p") == 0 || _stricmp(tok, "-1440p") == 0 ||
                   _stricmp(tok, "--2k") == 0 || _stricmp(tok, "-2k") == 0) {
            renderW = 2560;
            renderH = 1440;
        } else if (_stricmp(tok, "--1080p") == 0 || _stricmp(tok, "-1080p") == 0) {
            renderW = 1920;
            renderH = 1080;
        } else if (_stricmp(tok, "--720p") == 0 || _stricmp(tok, "-720p") == 0) {
            renderW = 1280;
            renderH = 720;
        } else if (_stricmp(tok, "--fullscreen") == 0 || _stricmp(tok, "-fullscreen") == 0 ||
                   _stricmp(tok, "--borderless") == 0 || _stricmp(tok, "-borderless") == 0 ||
                   _stricmp(tok, "-f") == 0) {
            fullscreen = TRUE;
        } else if (_strnicmp(tok, "--scale=", 8) == 0 || _strnicmp(tok, "-scale=", 7) == 0) {
            const char* valStr = strchr(tok, '=') + 1;
            double s = atof(valStr);
            if (s > 0.1 && s <= 16.0) {
                // If s >= 5.0 (e.g. 6x), scale relative to Xbox 640x360 base (6x = 3840x2160);
                // otherwise scale relative to 1280x720 HD base (3x = 3840x2160, 2x = 2560x1440, 1.5x = 1920x1080).
                double baseW = (s >= 5.0) ? 640.0 : 1280.0;
                double baseH = (s >= 5.0) ? 360.0 : 720.0;
                renderW = (uint32_t)(baseW * s + 0.5);
                renderH = (uint32_t)(baseH * s + 0.5);
            }
        } else if ((_stricmp(tok, "--scale") == 0 || _stricmp(tok, "-scale") == 0 || _stricmp(tok, "-s") == 0) && i + 1 < numTokens) {
            double s = atof(tokens[++i]);
            if (s > 0.1 && s <= 16.0) {
                double baseW = (s >= 5.0) ? 640.0 : 1280.0;
                double baseH = (s >= 5.0) ? 360.0 : 720.0;
                renderW = (uint32_t)(baseW * s + 0.5);
                renderH = (uint32_t)(baseH * s + 0.5);
            }
        } else if (_strnicmp(tok, "--res=", 6) == 0 || _strnicmp(tok, "-res=", 5) == 0 || _strnicmp(tok, "--resolution=", 13) == 0) {
            const char* valStr = strchr(tok, '=') + 1;
            uint32_t w = 0, h = 0;
            if (sscanf(valStr, "%ux%u", &w, &h) == 2 || sscanf(valStr, "%uX%u", &w, &h) == 2) {
                if (w >= 320 && h >= 200 && w <= 7680 && h <= 4320) {
                    renderW = w;
                    renderH = h;
                }
            }
        } else if ((_stricmp(tok, "--res") == 0 || _stricmp(tok, "-res") == 0 || _stricmp(tok, "--resolution") == 0) && i + 1 < numTokens) {
            const char* valStr = tokens[++i];
            uint32_t w = 0, h = 0;
            if (sscanf(valStr, "%ux%u", &w, &h) == 2 || sscanf(valStr, "%uX%u", &w, &h) == 2) {
                if (w >= 320 && h >= 200 && w <= 7680 && h <= 4320) {
                    renderW = w;
                    renderH = h;
                }
            }
        } else if ((_stricmp(tok, "--width") == 0 || _stricmp(tok, "-w") == 0) && i + 1 < numTokens) {
            uint32_t w = (uint32_t)atoi(tokens[++i]);
            if (w >= 320 && w <= 7680) renderW = w;
        } else if ((_stricmp(tok, "--height") == 0 || _stricmp(tok, "-h") == 0) && i + 1 < numTokens) {
            uint32_t h = (uint32_t)atoi(tokens[++i]);
            if (h >= 200 && h <= 4320) renderH = h;
        } else if (_stricmp(tok, "--log-files") == 0 || _stricmp(tok, "-log-files") == 0 ||
                   _stricmp(tok, "--log") == 0 || _stricmp(tok, "-l") == 0) {
            s_logAllFileLoads = TRUE;
        }
    }

    printFmt("[CORE] Internal Render Resolution: %ux%u (Fullscreen: %s)\n",
        renderW, renderH, fullscreen ? "ON" : "OFF");
    NativeSC2_SetRenderConfig(renderW, renderH, fullscreen);
}

extern "C" __declspec(dllexport) int NativeSC2_Run(const char* xbePath) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    printCore("[CORE] NativeSC2_Run started!\n");

    printCore("===================================================================\n");
    printCore(" NativeSC2 - Native Win32 Compatibility Shim for Soulcalibur II    \n");
    printCore(" Target Binary: Soulcalibur II (USA) [Xbox x86]                    \n");
    printCore(" Architecture: 2-Stage Low-Memory Bootstrap Loader                 \n");
    printCore("===================================================================\n\n");

    InitExeAndGameDirectory(xbePath);
    xbePath = s_resolvedXbePath;

    printFmt("[CORE] Executable Directory: %s\n", s_exeDir);
    printFmt("[CORE] Game Data Directory: %s\n", s_exeDir);
    printFmt("[CORE] Target XBE: %s\n", xbePath);
    ParseCommandLineRenderConfig();

    printCore("[CORE] Registering exception handler...\n");
    AddVectoredExceptionHandler(1, HardwareExceptionHandler);

    printCore("[CORE] Initializing Xbox kernel shim...\n");
    XboxKrnl_Init();

    printCore("[CORE] Allocating NV2A hardware buffer at 0xFD000000...\n");
    void* nv2a = VirtualAlloc((void*)0xFD000000, 0x01000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (nv2a) {
        printCore("[NativeSC2] Pre-allocated NV2A MMIO range at 0xFD000000 - 0xFE000000\n");
    }

    printCore("[CORE] Allocating APU hardware buffer at 0xFE800000...\n");
    void* apu = VirtualAlloc((void*)0xFE800000, 0x00800000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (apu) {
        printCore("[NativeSC2] Pre-allocated APU MMIO range at 0xFE800000 - 0xFF000000\n");
    }

    VirtualAlloc((void*)0x80000000, 0x00100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc((void*)0xA0000000, 0x00100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    printCore("[CORE] Initializing Win32 presentation renderer...\n");
    NativeSC2_InitRenderer();

    printCore("[CORE] Calling Xbe_LoadAndMap...\n");
    XBE_LOADED_IMAGE image;
    if (!Xbe_LoadAndMap(xbePath, &image)) {
        printCore("[NativeSC2] Fatal: Failed to load and map XBE.\n");
        return 1;
    }

    printCore("[CORE] Applying HLE patches...\n");
    Xbe_ApplyHlePatches();

    printCore("[CORE] Initializing HLE OLK Mod Engine...\n");
    ModEngine_Init(s_exeDir, s_logAllFileLoads);
    ModEngine_InstallHooks();

    printCore("[CORE] Installing DirectSound audio bridge hooks...\n");
    Audio_InstallHooks();

    printCore("\n[NativeSC2] XBE environment prepared successfully!\n");
    printCore("[NativeSC2] Creating Xbox CRT default heap...\n");
    typedef void* (__cdecl *RtlCreateHeapFunc)(uint32_t, void*, uint32_t, uint32_t, void*, void*);
    RtlCreateHeapFunc RtlCreateHeap = (RtlCreateHeapFunc)0x001086EB;
    uint32_t heapParams[12] = { 0 };
    heapParams[0] = 0x30;
    void* crtHeap = RtlCreateHeap(2, NULL, 0x1000000, 0x10000, NULL, heapParams);
    printHex("[NativeSC2] CRT Heap created: ", (DWORD)(uintptr_t)crtHeap);
    *(void**)0x0062C934 = crtHeap;

    printCore("[NativeSC2] Initializing C runtime and C++ constructors (_rtinit, _cinit)...\n");
    typedef void (__cdecl *InitFunc)(void);
    InitFunc _rtinit = (InitFunc)0x00106D62;
    InitFunc _cinit = (InitFunc)0x00106D0A;
    _rtinit();
    _cinit();
    printCore("[NativeSC2]CRT initialization complete!\n");

    printCore("[NativeSC2] Initializing CPU Statistical Profiler & Thread Spinloop Mitigations...\n");
    CpuProfiler_Init();

    printCore("[NativeSC2] Executing Soulcalibur II Game_Main at 0x00033C90...\n\n");

    typedef int (__cdecl *GameMainFunc)(int, int, int);
    GameMainFunc Game_Main = (GameMainFunc)SC2_GAME_MAIN;

    int result = Game_Main(0, 0, 0);
    printCore("\n[NativeSC2] Game_Main exited.\n");

    Audio_Shutdown();

    return result;
}
