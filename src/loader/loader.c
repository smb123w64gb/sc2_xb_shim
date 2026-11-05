#include <windows.h>

// 128 MB BSS array reserving 0x00010000 - 0x08010000 for the Xbox address space
__declspec(dllexport) volatile unsigned char g_XboxMemoryRange[128 * 1024 * 1024];

typedef int (__cdecl *PFN_NATIVESC2_RUN)(const char* xbe_path);

static void printStr(const char* s) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    DWORD len = 0;
    while (s[len]) len++;
    WriteFile(hOut, s, len, &written, NULL);
}

static void printHex(const char* prefix, DWORD val) {
    char buf[64];
    char hexDigits[] = "0123456789ABCDEF";
    int p = 0;
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 7; i >= 0; i--) {
        buf[p++] = hexDigits[(val >> (i * 4)) & 0xF];
    }
    buf[p++] = '\n';
    buf[p] = 0;
    printStr(buf);
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    (void)ctrlType;
    TerminateProcess(GetCurrentProcess(), 0);
    return TRUE;
}

void __stdcall rawMain(void) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    printStr("[LOADER] NativeSC2 Bootstrap Loader Started.\n");

    // Touch memory to ensure symbol is retained
    g_XboxMemoryRange[0] = 0;

    printStr("[LOADER] Testing VirtualProtect on .data (0x00013000)...\n");
    DWORD oldProtect;
    if (!VirtualProtect((void*)0x00013000, 64 * 1024 * 1024, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        printHex("[LOADER] VirtualProtect on 0x00013000 failed: ", GetLastError());
    } else {
        printStr("[LOADER] VirtualProtect on 0x00013000 succeeded.\n");
    }

    // Ensure working directory is set to the EXE directory
    char exePath[260];
    DWORD len = GetModuleFileNameA(NULL, exePath, 260);
    if (len > 0 && len < 260) {
        char* lastSlash = NULL;
        for (char* p = exePath; *p; p++) {
            if (*p == '\\' || *p == '/') lastSlash = p;
        }
        if (lastSlash) {
            *lastSlash = '\0';
            SetCurrentDirectoryA(exePath);
        }
    }

    printStr("[LOADER] Loading NativeSC2_core.dll...\n");
    HMODULE hCore = LoadLibraryA("NativeSC2_core.dll");
    if (!hCore) {
        printHex("[LOADER] LoadLibraryA('NativeSC2_core.dll') failed: ", GetLastError());
        hCore = LoadLibraryA("bin\\NativeSC2_core.dll");
    }

    if (!hCore) {
        printHex("[LOADER] LoadLibraryA('bin\\NativeSC2_core.dll') failed: ", GetLastError());
        ExitProcess(1);
    }

    printStr("[LOADER] NativeSC2_core.dll loaded successfully.\n");
    PFN_NATIVESC2_RUN pfnRun = (PFN_NATIVESC2_RUN)GetProcAddress(hCore, "NativeSC2_Run");
    if (!pfnRun) {
        printHex("[LOADER] GetProcAddress('NativeSC2_Run') failed: ", GetLastError());
        ExitProcess(2);
    }

    // Parse command line argument (if user passed a non-option XBE path)
    // Do NOT mutate GetCommandLineA() in-place so NativeSC2_core.dll can parse all CLI flags.
    const char* cmdLine = GetCommandLineA();
    char xbeArgBuf[260];
    xbeArgBuf[0] = '\0';
    const char* arg = NULL;
    if (cmdLine) {
        const char* p = cmdLine;
        // Skip executable path
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        // Scan remaining tokens for an explicit .xbe file path (ignoring '-' flags and their values)
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;

            const char* tokStart = p;
            const char* tokEnd = p;
            if (*p == '"') {
                p++;
                tokStart = p;
                while (*p && *p != '"') p++;
                tokEnd = p;
                if (*p == '"') p++;
            } else {
                while (*p && *p != ' ' && *p != '\t') p++;
                tokEnd = p;
            }

            if (tokStart < tokEnd && *tokStart == '-') {
                // Check if this flag takes a separate value token (--scale <N>, --res <WxH>, --width <W>, --height <H>)
                DWORD tokLen = (DWORD)(tokEnd - tokStart);
                int takesVal = 0;
                if ((tokLen == 7 && (tokStart[1] == '-' && (tokStart[2] == 's' || tokStart[2] == 'S') && (tokStart[3] == 'c' || tokStart[3] == 'C'))) ||
                    (tokLen == 6 && (tokStart[1] == 's' || tokStart[1] == 'S') && (tokStart[2] == 'c' || tokStart[2] == 'C')) ||
                    (tokLen == 2 && (tokStart[1] == 's' || tokStart[1] == 'S' || tokStart[1] == 'w' || tokStart[1] == 'W' || tokStart[1] == 'h' || tokStart[1] == 'H')) ||
                    (tokLen == 5 && tokStart[1] == '-' && (tokStart[2] == 'r' || tokStart[2] == 'R') && (tokStart[3] == 'e' || tokStart[3] == 'E')) ||
                    (tokLen == 4 && (tokStart[1] == 'r' || tokStart[1] == 'R') && (tokStart[2] == 'e' || tokStart[2] == 'E')) ||
                    (tokLen == 7 && tokStart[1] == '-' && (tokStart[2] == 'w' || tokStart[2] == 'W') && (tokStart[3] == 'i' || tokStart[3] == 'I')) ||
                    (tokLen == 8 && tokStart[1] == '-' && (tokStart[2] == 'h' || tokStart[2] == 'H') && (tokStart[3] == 'e' || tokStart[3] == 'E'))) {
                    takesVal = 1;
                }
                if (takesVal) {
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p && *p != '-') {
                        if (*p == '"') {
                            p++;
                            while (*p && *p != '"') p++;
                            if (*p == '"') p++;
                        } else {
                            while (*p && *p != ' ' && *p != '\t') p++;
                        }
                    }
                }
                continue;
            }

            if (tokStart < tokEnd && !arg) {
                DWORD copyLen = (DWORD)(tokEnd - tokStart);
                if (copyLen >= sizeof(xbeArgBuf)) copyLen = sizeof(xbeArgBuf) - 1;
                for (DWORD i = 0; i < copyLen; i++) xbeArgBuf[i] = tokStart[i];
                xbeArgBuf[copyLen] = '\0';
                arg = xbeArgBuf;
            }
        }
    }

    printStr("[LOADER] Handing execution over to NativeSC2_Run...\n\n");
    int exitCode = pfnRun(arg);
    ExitProcess(exitCode);
}
