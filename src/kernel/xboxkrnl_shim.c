#include "xboxkrnl.h"
#include "cpu_profiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifndef STATUS_NO_MORE_FILES
#define STATUS_NO_MORE_FILES ((uint32_t)0x80000006L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((uint32_t)0xC0000035L)
#endif
#ifndef STATUS_OBJECT_PATH_NOT_FOUND
#define STATUS_OBJECT_PATH_NOT_FOUND ((uint32_t)0xC000003AL)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((uint32_t)0xC0000022L)
#endif

static void logFmt(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        DWORD written;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, len, &written, NULL);
        static FILE* s_fLog = NULL;
        if (!s_fLog) s_fLog = fopen("native_sc2.log", "a");
        if (s_fLog) {
            fwrite(buf, 1, len, s_fLog);
            fflush(s_fLog);
        }
    }
}

static void EnsureDirectoryExists(const char* filePath) {
    char dir[MAX_PATH];
    strncpy(dir, filePath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) lastSlash = strrchr(dir, '/');
    if (lastSlash) {
        *lastSlash = '\0';
        char tmp[MAX_PATH];
        char* p = NULL;
        size_t len = strnlen(dir, MAX_PATH);
        if (len >= MAX_PATH) return;
        strcpy(tmp, dir);
        for (p = tmp + 3; *p; p++) {
            if (*p == '\\' || *p == '/') {
                *p = '\0';
                CreateDirectoryA(tmp, NULL);
                *p = '\\';
            }
        }
        CreateDirectoryA(tmp, NULL);
    }
}

static CRITICAL_SECTION g_MemoryLock;
static char g_GameDirectory[MAX_PATH] = ".";

void XboxKrnl_SetGameDirectory(const char* dir) {
    if (dir && *dir) {
        strncpy(g_GameDirectory, dir, MAX_PATH - 1);
        g_GameDirectory[MAX_PATH - 1] = '\0';
    }
}

// Convert Xbox path (\Device\CdRom0\ or D:\ or UDATA / TDATA) to Windows path
static void TranslateXboxPathToWin32(const char* xboxPath, char* win32Path, size_t maxLen) {
    const char* relPath = xboxPath;

    // Strip NT namespace prefix \??\ if present
    if (strncmp(relPath, "\\??\\", 4) == 0) {
        relPath += 4;
    }

    // Direct match / bypass for known primary archives
    for (const char* p = relPath; *p; p++) {
        if (_strnicmp(p, "root.olk", 8) == 0) {
            snprintf(win32Path, maxLen, "%s\\root.olk", g_GameDirectory);
            return;
        }
        if (_strnicmp(p, "bgm.afs", 7) == 0) {
            snprintf(win32Path, maxLen, "%s\\bgm.afs", g_GameDirectory);
            return;
        }
        if (_strnicmp(p, "movie.afs", 9) == 0) {
            snprintf(win32Path, maxLen, "%s\\movie.afs", g_GameDirectory);
            return;
        }
    }

    if (_strnicmp(relPath, "(null)\\", 7) == 0 || _strnicmp(relPath, "(null)/", 7) == 0) {
        relPath += 7;
        snprintf(win32Path, maxLen, "%s\\%s", g_GameDirectory, relPath);
    } else if (_strnicmp(relPath, "\\Device\\CdRom0\\", 15) == 0) {
        relPath += 15;
        snprintf(win32Path, maxLen, "%s\\%s", g_GameDirectory, relPath);
    } else if (_stricmp(relPath, "\\Device\\CdRom0") == 0) {
        snprintf(win32Path, maxLen, "%s\\root.olk", g_GameDirectory);
    } else if (_strnicmp(relPath, "D:\\", 3) == 0 || _strnicmp(relPath, "D:/", 3) == 0) {
        relPath += 3;
        snprintf(win32Path, maxLen, "%s\\%s", g_GameDirectory, relPath);
    } else if (_strnicmp(relPath, "\\Device\\Harddisk0\\Partition1\\", 29) == 0) {
        relPath += 29;
        snprintf(win32Path, maxLen, "%s\\Save\\Partition1\\%s", g_GameDirectory, relPath);
    } else if (_strnicmp(relPath, "\\Device\\Harddisk0\\Partition2\\", 29) == 0) {
        relPath += 29;
        snprintf(win32Path, maxLen, "%s\\Save\\Partition2\\%s", g_GameDirectory, relPath);
    } else if (_strnicmp(relPath, "U:\\", 3) == 0 || _strnicmp(relPath, "U:/", 3) == 0) {
        relPath += 3;
        snprintf(win32Path, maxLen, "%s\\Save\\UDATA\\%s", g_GameDirectory, relPath);
    } else if (_strnicmp(relPath, "T:\\", 3) == 0 || _strnicmp(relPath, "T:/", 3) == 0) {
        relPath += 3;
        snprintf(win32Path, maxLen, "%s\\Save\\TDATA\\%s", g_GameDirectory, relPath);
    } else {
        snprintf(win32Path, maxLen, "%s\\%s", g_GameDirectory, relPath);
    }

    // Normalize slashes
    for (char* p = win32Path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
}

// -------------------------------------------------------------
// Ordinal Implementations
// -------------------------------------------------------------

// Ordinal 161: KiBugCheckData
static void __stdcall Shim_KiBugCheckData(uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
    printf("[XBOXKRNL] KiBugCheckData: Code=0x%08X (0x%X, 0x%X, 0x%X, 0x%X)\n", code, p1, p2, p3, p4);
    ExitProcess(code);
}

// Ordinal 129: KeSetDisableBoostThread
static void __stdcall Shim_KeSetDisableBoostThread(void* thread, uint8_t disable) {
    (void)thread; (void)disable;
}

// Ordinal 305: RtlCompareMemoryUlong
static size_t __stdcall Shim_RtlCompareMemoryUlong(const void* src, size_t length, uint32_t pattern) {
    const uint32_t* p = (const uint32_t*)src;
    size_t dwords = length / 4;
    size_t matched = 0;
    while (dwords-- && *p++ == pattern) {
        matched += 4;
    }
    return matched;
}

// Ordinal 128: KeSetBasePriorityThread
static int32_t __stdcall Shim_KeSetBasePriorityThread(HANDLE thread, int32_t increment) {
    (void)thread; (void)increment;
    return 0;
}

// Ordinal 304: RtlCompareMemory
static size_t __stdcall Shim_RtlCompareMemory(const void* s1, const void* s2, size_t len) {
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    size_t i = 0;
    for (; i < len && p1[i] == p2[i]; i++);
    return i;
}

// Ordinal 24: ExQueryNonVolatileSetting
static uint32_t __stdcall Shim_ExQueryNonVolatileSetting(uint32_t valIndex, uint32_t* type, void* val, uint32_t valLen, uint32_t* resLen) {
    if (resLen) *resLen = valLen;
    if (type) *type = 4; // REG_DWORD

    // Video Flags
    if (valIndex == 0x01) { // XC_FACTORY_GAME_REGION
        if (val && valLen >= 4) *(uint32_t*)val = 1; // North America NTSC
        return STATUS_SUCCESS;
    }
    if (valIndex == 0x03 || valIndex == 0x08) { // XC_VIDEO_FLAGS
        if (val && valLen >= 4) {
            // Enable 480p + Widescreen 16:9 (Original Xbox SC2 supports 16:9 widescreen only in 480p, not 720p)
            uint32_t flags = XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV_480p;
            *(uint32_t*)val = (flags << 16) | flags;
        }
        return STATUS_SUCCESS;
    }
    if (valIndex == 0x04) { // XC_AUDIO_FLAGS
        if (val && valLen >= 4) {
            *(uint32_t*)val = 0; // Stereo / Default
        }
        return STATUS_SUCCESS;
    }
    if (val && valLen >= 4) *(uint32_t*)val = 0;
    return STATUS_SUCCESS;
}

// Ordinal 301: PsTerminateSystemThread
static void __stdcall Shim_PsTerminateSystemThread(uint32_t exitStatus) {
    ExitThread(exitStatus);
}

// Ordinal 187: MmSetAddressProtect
static uint32_t __stdcall Shim_MmSetAddressProtect(void* base, size_t size, uint32_t newProtect) {
    DWORD old;
    VirtualProtect(base, size, newProtect, &old);
    return STATUS_SUCCESS;
}

// Ordinal 226: NtQueryInformationFile
static uint32_t __stdcall Shim_NtQueryInformationFile(HANDLE fileHandle, PIO_STATUS_BLOCK ioStatus, void* fileInfo, uint32_t length, uint32_t fileInfoClass) {
    if (!fileHandle || fileHandle == INVALID_HANDLE_VALUE) return STATUS_INVALID_PARAMETER;

    LARGE_INTEGER size;
    size.QuadPart = 0;
    GetFileSizeEx(fileHandle, &size);

    if (fileInfo && length > 0) {
        memset(fileInfo, 0, length);
    }

    // FileStandardInformation (class 5)
    if (fileInfoClass == 5 && length >= 24) {
        *(int64_t*)fileInfo = size.QuadPart;         // AllocationSize
        *((int64_t*)fileInfo + 1) = size.QuadPart;   // EndOfFile
        *((uint32_t*)fileInfo + 4) = 1;              // NumberOfLinks
        *((uint8_t*)fileInfo + 20) = 0;              // DeletePending
        *((uint8_t*)fileInfo + 21) = 0;              // Directory
        if (ioStatus) {
            ioStatus->Status = STATUS_SUCCESS;
            ioStatus->Information = length;
        }
        logFmt("[FS] NtQueryInformationFile: Handle=%p, Class=5 (Standard), Size=%lld\n", fileHandle, size.QuadPart);
        return STATUS_SUCCESS;
    }

    // FilePositionInformation (class 14)
    if (fileInfoClass == 14 && length >= 8) {
        LARGE_INTEGER curPos;
        curPos.QuadPart = 0;
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        SetFilePointerEx(fileHandle, zero, &curPos, FILE_CURRENT);
        *(int64_t*)fileInfo = curPos.QuadPart;
        if (ioStatus) {
            ioStatus->Status = STATUS_SUCCESS;
            ioStatus->Information = length;
        }
        return STATUS_SUCCESS;
    }

    // FileEndOfFileInformation (class 20)
    if (fileInfoClass == 20 && length >= 8) {
        *(int64_t*)fileInfo = size.QuadPart;
        if (ioStatus) {
            ioStatus->Status = STATUS_SUCCESS;
            ioStatus->Information = length;
        }
        return STATUS_SUCCESS;
    }

    // FileNetworkOpenInformation (class 34 / 0x22)
    if (fileInfoClass == 34 && length >= 56) {
        BY_HANDLE_FILE_INFORMATION bhfi;
        if (GetFileInformationByHandle(fileHandle, &bhfi)) {
            memcpy((char*)fileInfo + 0, &bhfi.ftCreationTime, sizeof(FILETIME));
            memcpy((char*)fileInfo + 8, &bhfi.ftLastAccessTime, sizeof(FILETIME));
            memcpy((char*)fileInfo + 16, &bhfi.ftLastWriteTime, sizeof(FILETIME));
            memcpy((char*)fileInfo + 24, &bhfi.ftLastWriteTime, sizeof(FILETIME));
            *(int64_t*)((char*)fileInfo + 32) = size.QuadPart;
            *(int64_t*)((char*)fileInfo + 40) = size.QuadPart;
            *(uint32_t*)((char*)fileInfo + 48) = bhfi.dwFileAttributes;
        } else {
            *(int64_t*)((char*)fileInfo + 32) = size.QuadPart;
            *(int64_t*)((char*)fileInfo + 40) = size.QuadPart;
            *(uint32_t*)((char*)fileInfo + 48) = FILE_ATTRIBUTE_NORMAL;
        }
        if (ioStatus) {
            ioStatus->Status = STATUS_SUCCESS;
            ioStatus->Information = length;
        }
        logFmt("[FS] NtQueryInformationFile: Handle=%p, Class=34 (NetworkOpen), Size=%lld\n", fileHandle, size.QuadPart);
        return STATUS_SUCCESS;
    }

    if (ioStatus) {
        ioStatus->Status = STATUS_SUCCESS;
        ioStatus->Information = length;
    }
    return STATUS_SUCCESS;
}

// Ordinal 178: MmMapIoSpace
static void* __stdcall Shim_MmMapIoSpace(uint32_t physAddr, size_t numBytes, uint32_t protect) {
    (void)protect;
    // NV2A MMIO or hardware space
    static uint8_t dummyHardware[0x10000];
    printf("[XBOXKRNL] MmMapIoSpace: Phys=0x%08X, Bytes=%u -> Returning safe dummy\n", physAddr, (unsigned)numBytes);
    return dummyHardware;
}

typedef struct {
    uint32_t virtAddr;
    uint32_t physAddr;
    uint32_t size;
} ALLOC_RECORD;

#define MAX_ALLOC_RECORDS 4096
static ALLOC_RECORD s_allocRecords[MAX_ALLOC_RECORDS];
static uint32_t s_allocCount = 0;

void* XboxKrnl_GpuToCpu(uint32_t gpuAddr) {
    if (gpuAddr < 0x00010000) return NULL;
    uint32_t masked = gpuAddr & 0x0FFFFFFF;
    for (uint32_t i = 0; i < s_allocCount; i++) {
        if (masked >= s_allocRecords[i].physAddr &&
            masked < s_allocRecords[i].physAddr + s_allocRecords[i].size) {
            uint32_t offset = masked - s_allocRecords[i].physAddr;
            return (void*)(uintptr_t)(s_allocRecords[i].virtAddr + offset);
        }
    }
    if (gpuAddr >= 0x80000000) {
        if (masked >= 0x00010000 && masked < 0x08000000) {
            return (void*)(uintptr_t)masked;
        }
        return NULL;
    }
    return (void*)(uintptr_t)gpuAddr;
}

static void RecordAlloc(void* ptr, size_t size) {
    if (!ptr || s_allocCount >= MAX_ALLOC_RECORDS) return;
    uint32_t virt = (uint32_t)(uintptr_t)ptr;
    s_allocRecords[s_allocCount].virtAddr = virt;
    s_allocRecords[s_allocCount].physAddr = virt & 0x0FFFFFFF;
    s_allocRecords[s_allocCount].size = (uint32_t)size;
    s_allocCount++;
}

static void RemoveAlloc(void* ptr) {
    if (!ptr) return;
    uint32_t virt = (uint32_t)(uintptr_t)ptr;
    for (uint32_t i = 0; i < s_allocCount; i++) {
        if (s_allocRecords[i].virtAddr == virt) {
            s_allocRecords[i] = s_allocRecords[s_allocCount - 1];
            s_allocCount--;
            break;
        }
    }
}

// Ordinal 165: MmAllocateContiguousMemory
static void* __stdcall Shim_MmAllocateContiguousMemory(size_t numBytes) {
    void* ptr = VirtualAlloc(NULL, numBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    RecordAlloc(ptr, numBytes);
    return ptr;
}

// Ordinal 164: LaunchDataPage
static void* g_LaunchDataPage = NULL;

// Ordinal 171: MmFreeContiguousMemory
static void __stdcall Shim_MmFreeContiguousMemory(void* base) {
    if (base) {
        RemoveAlloc(base);
        VirtualFree(base, 0, MEM_RELEASE);
    }
}

// Ordinal 49: IoCreateDevice (Xbox ABI: 6 arguments)
static uint32_t __stdcall Shim_IoCreateDevice(void* driver, uint32_t extSize, void* devName, uint32_t devType, uint8_t exclusive, void** devObj) {
    (void)driver; (void)devName; (void)devType; (void)exclusive;
    if (devObj) {
        char* dev = (char*)calloc(1, 0x40 + extSize);
        if (dev) {
            *(void**)(dev + 0x18) = (void*)(dev + 0x40);
            *devObj = (void*)dev;
        }
    }
    return STATUS_SUCCESS;
}

// Ordinal 65: IoInvalidDeviceRequest
static uint32_t __stdcall Shim_IoInvalidDeviceRequest(void* devObj, void* irp) {
    (void)devObj; (void)irp;
    return STATUS_UNSUCCESSFUL;
}

// Ordinal 289: PsCreateSystemThread
static uint32_t __stdcall Shim_PsCreateSystemThread(HANDLE* threadHandle, void* objAttrs, void* threadSecurity, HANDLE procHandle, void* clientId, LPTHREAD_START_ROUTINE startRoutine, void* startContext) {
    (void)objAttrs; (void)threadSecurity; (void)procHandle;
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, startRoutine, startContext, 0, &tid);
    printf("[XBOXKRNL] PsCreateSystemThread: routine=%p, ctx=%p -> handle=%p, tid=%lu (err=%lu)\n", (void*)startRoutine, startContext, (void*)h, tid, h ? 0 : GetLastError());
    fflush(stdout);
    if (!h) return STATUS_UNSUCCESSFUL;
    if (clientId) *(DWORD*)clientId = tid;
    if (threadHandle) *threadHandle = h;
    CpuProfiler_RegisterThread(h, tid, (void*)startRoutine, NULL);
    return STATUS_SUCCESS;
}

// Ordinal 145: KeWaitForMultipleObjects
static uint32_t __stdcall Shim_KeWaitForMultipleObjects(uint32_t count, void* objects[], uint32_t waitType, uint32_t waitReason, uint8_t alertable, LARGE_INTEGER* timeout) {
    (void)waitType; (void)waitReason; (void)alertable;
    DWORD ms = timeout ? (DWORD)(-timeout->QuadPart / 10000) : INFINITE;
    DWORD res = WaitForMultipleObjects(count, (const HANDLE*)objects, (waitType == 0), ms);
    return res;
}

// Ordinal 159: KfRaiseIrql
static uint8_t __fastcall Shim_KfRaiseIrql(uint8_t newIrql) {
    return newIrql;
}

// Ordinal 113: KeQuerySystemTime
static void __stdcall Shim_KeQuerySystemTime(LARGE_INTEGER* curTime) {
    if (curTime) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        curTime->LowPart = ft.dwLowDateTime;
        curTime->HighPart = ft.dwHighDateTime;
    }
}

// Ordinal 107: KePulseEvent
static int32_t __stdcall Shim_KePulseEvent(HANDLE eventHandle, int32_t increment, uint8_t wait) {
    (void)increment; (void)wait;
    PulseEvent(eventHandle);
    return 0;
}

// Ordinal 74: IoStartPacket
static void __stdcall Shim_IoStartPacket(void* devObj, void* irp, uint32_t* key, void* cancelFunc) {
    (void)devObj; (void)irp; (void)key; (void)cancelFunc;
}

// Ordinal 166: MmAllocateContiguousMemoryEx
static void* __stdcall Shim_MmAllocateContiguousMemoryEx(size_t numBytes, uint32_t lowestAcceptable, uint32_t highestAcceptable, uint32_t alignment, uint32_t protect) {
    (void)lowestAcceptable; (void)highestAcceptable; (void)alignment; (void)protect;
    void* ptr = VirtualAlloc(NULL, numBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    RecordAlloc(ptr, numBytes);
    printf("[XBOXKRNL] MmAllocateContiguousMemoryEx: size=0x%08X -> %p (phys=0x%08X)\n",
           (uint32_t)numBytes, ptr, (uint32_t)(uintptr_t)ptr & 0x0FFFFFFF);
    return ptr;
}

// Ordinal 180: MmQueryAddressProtect
static uint32_t __stdcall Shim_MmQueryAddressProtect(void* addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi))) {
        return mbi.Protect;
    }
    return PAGE_READWRITE;
}

// Ordinal 182: MmQueryAllocationSize
static size_t __stdcall Shim_MmQueryAllocationSize(void* base) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(base, &mbi, sizeof(mbi))) {
        return mbi.RegionSize;
    }
    return 0;
}

// Ordinal 179: MmPersistContiguousMemory
static uint32_t __stdcall Shim_MmPersistContiguousMemory(void* base, size_t numBytes, uint8_t persist) {
    (void)base; (void)numBytes; (void)persist;
    return STATUS_SUCCESS;
}

// Ordinal 190: NtAllocateVirtualMemory
static uint32_t __stdcall Shim_NtAllocateVirtualMemory(void** baseAddress, uint32_t zeroBits, size_t* regionSize, uint32_t allocationType, uint32_t protect) {
    (void)zeroBits;
    void* res = VirtualAlloc(baseAddress ? *baseAddress : NULL, *regionSize, allocationType, protect);
    if (!res) return STATUS_NO_MEMORY;
    if (baseAddress) *baseAddress = res;
    return STATUS_SUCCESS;
}

// Ordinal 236: NtResumeThread
static uint32_t __stdcall Shim_NtResumeThread(HANDLE threadHandle, uint32_t* prevSuspendCount) {
    DWORD res = ResumeThread(threadHandle);
    if (prevSuspendCount) *prevSuspendCount = res;
    return STATUS_SUCCESS;
}

// Ordinal 219: NtOpenSymbolicLinkObject
static uint32_t __stdcall Shim_NtOpenSymbolicLinkObject(HANDLE* linkHandle, POBJECT_ATTRIBUTES objAttrs) {
    (void)objAttrs;
    if (linkHandle) *linkHandle = (HANDLE)0x1234;
    return STATUS_SUCCESS;
}

// Ordinal 211: NtFlushBuffersFile
static uint32_t __stdcall Shim_NtFlushBuffersFile(HANDLE fileHandle, PIO_STATUS_BLOCK ioStatus) {
    FlushFileBuffers(fileHandle);
    if (ioStatus) ioStatus->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

// Ordinal 202: NtCreateEvent
static uint32_t __stdcall Shim_NtCreateEvent(HANDLE* eventHandle, uint32_t access, POBJECT_ATTRIBUTES objAttrs, uint32_t eventType, uint8_t initialState) {
    (void)access; (void)objAttrs;
    HANDLE h = CreateEventA(NULL, (eventType == 0), initialState, NULL);
    if (!h) return STATUS_UNSUCCESSFUL;
    if (eventHandle) *eventHandle = h;
    return STATUS_SUCCESS;
}

// Ordinal 233: NtQueryVolumeInformationFile
static uint32_t __stdcall Shim_NtQueryVolumeInformationFile(HANDLE fileHandle, PIO_STATUS_BLOCK ioStatus, void* fsInfo, uint32_t len, uint32_t fsInfoClass) {
    (void)fileHandle;
    logFmt("[FS] NtQueryVolumeInformationFile: h=%p, class=%u, len=%u\n", fileHandle, fsInfoClass, len);
    if (!fsInfo) {
        if (ioStatus) ioStatus->Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }
    memset(fsInfo, 0, len);

    // FileFsSizeInformation (class 3) - queried by Xapi_GetDiskFreeSpaceExA (FUN_00104ad2)
    if (fsInfoClass == 3 || len >= 24) {
        typedef struct {
            int64_t TotalAllocationUnits;
            int64_t AvailableAllocationUnits;
            uint32_t SectorsPerAllocationUnit;
            uint32_t BytesPerSector;
        } XBOX_FS_SIZE_INFO;

        XBOX_FS_SIZE_INFO* psi = (XBOX_FS_SIZE_INFO*)fsInfo;
        psi->TotalAllocationUnits = 1000000;       // ~16 GB total space
        psi->AvailableAllocationUnits = 800000;    // ~12.8 GB free space
        psi->SectorsPerAllocationUnit = 32;        // 32 sectors per cluster (16 KB cluster = 1 Xbox block)
        psi->BytesPerSector = 512;                 // 512 bytes per sector

        if (ioStatus) {
            ioStatus->Status = STATUS_SUCCESS;
            ioStatus->Information = sizeof(XBOX_FS_SIZE_INFO);
        }
        return STATUS_SUCCESS;
    }

    if (ioStatus) {
        ioStatus->Status = STATUS_SUCCESS;
        ioStatus->Information = len;
    }
    return STATUS_SUCCESS;
}

typedef struct {
    HANDLE dirHandle;
    HANDLE findHandle;
    char dirWin32Path[MAX_PATH];
} DIR_SEARCH_ENTRY;

#define MAX_DIR_SEARCHES 16
static DIR_SEARCH_ENTRY s_dirSearches[MAX_DIR_SEARCHES] = {0};

// Ordinal 218: NtOpenFile
static uint32_t __stdcall Shim_NtOpenFile(HANDLE* fileHandle, uint32_t access, POBJECT_ATTRIBUTES objAttrs, PIO_STATUS_BLOCK ioStatus, uint32_t shareAccess, uint32_t openOptions) {
    (void)shareAccess;
    if (!objAttrs || !objAttrs->ObjectName || !objAttrs->ObjectName->Buffer) return STATUS_INVALID_PARAMETER;

    char rawXboxPath[MAX_PATH];
    uint16_t nameLen = objAttrs->ObjectName->Length;
    if (nameLen >= MAX_PATH) nameLen = MAX_PATH - 1;
    memcpy(rawXboxPath, objAttrs->ObjectName->Buffer, nameLen);
    rawXboxPath[nameLen] = '\0';

    char win32Path[MAX_PATH];
    TranslateXboxPathToWin32(rawXboxPath, win32Path, MAX_PATH);

    DWORD dwAccess = GENERIC_READ;
    if (access & GENERIC_WRITE) dwAccess |= GENERIC_WRITE;
    if (access & DELETE) dwAccess |= DELETE;
    DWORD dwShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    if (_strnicmp(rawXboxPath, "U:\\", 3) == 0 || _strnicmp(rawXboxPath, "U:/", 3) == 0 ||
        _strnicmp(rawXboxPath, "T:\\", 3) == 0 || _strnicmp(rawXboxPath, "T:/", 3) == 0) {
        if (nameLen == 3) {
            EnsureDirectoryExists(win32Path);
            CreateDirectoryA(win32Path, NULL);
        }
    }

    DWORD dwAttrib = GetFileAttributesA(win32Path);
    bool isDir = (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) ||
                 (openOptions & 0x00000001) ||
                 (nameLen > 0 && (rawXboxPath[nameLen - 1] == '\\' || rawXboxPath[nameLen - 1] == '/'));

    DWORD fileFlags = isDir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;

    HANDLE h = CreateFileA(win32Path, dwAccess, dwShare, NULL, OPEN_EXISTING, fileFlags, NULL);
    if (h == INVALID_HANDLE_VALUE && (dwAccess & GENERIC_WRITE)) {
        EnsureDirectoryExists(win32Path);
        h = CreateFileA(win32Path, dwAccess, dwShare, NULL, OPEN_ALWAYS, fileFlags, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        logFmt("[FS] NtOpenFile FAILED: '%s' -> '%s' (isDir=%d, err=%lu)\n", rawXboxPath, win32Path, isDir, err);
        uint32_t ntErr = (err == ERROR_PATH_NOT_FOUND) ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
        if (ioStatus) ioStatus->Status = ntErr;
        return ntErr;
    }

    if (isDir) {
        for (int i = 0; i < MAX_DIR_SEARCHES; i++) {
            if (s_dirSearches[i].dirHandle == NULL) {
                s_dirSearches[i].dirHandle = h;
                s_dirSearches[i].findHandle = INVALID_HANDLE_VALUE;
                strncpy(s_dirSearches[i].dirWin32Path, win32Path, MAX_PATH);
                break;
            }
        }
    }

    logFmt("[FS] NtOpenFile: '%s' -> '%s' (isDir=%d, Handle: %p)\n", rawXboxPath, win32Path, isDir, h);
    if (fileHandle) *fileHandle = h;
    if (ioStatus) {
        ioStatus->Status = STATUS_SUCCESS;
        ioStatus->Information = 1; // FILE_OPENED
    }
    return STATUS_SUCCESS;
}

// Ordinal 198: NtClearEvent
static uint32_t __stdcall Shim_NtClearEvent(HANDLE eventHandle) {
    ResetEvent(eventHandle);
    return STATUS_SUCCESS;
}

// Ordinal 232: NtQueryVirtualMemory
static uint32_t __stdcall Shim_NtQueryVirtualMemory(HANDLE proc, void* baseAddr, uint32_t memInfoClass, void* memInfo, size_t memInfoLen, size_t* retLen) {
    (void)proc; (void)memInfoClass;
    size_t res = VirtualQuery(baseAddr, (PMEMORY_BASIC_INFORMATION)memInfo, memInfoLen);
    if (retLen) *retLen = res;
    return STATUS_SUCCESS;
}

// Ordinal 200: NtCreateDirectoryObject
static uint32_t __stdcall Shim_NtCreateDirectoryObject(HANDLE* dirHandle, uint32_t access, POBJECT_ATTRIBUTES objAttrs) {
    (void)access; (void)objAttrs;
    if (dirHandle) *dirHandle = (HANDLE)0x2345;
    return STATUS_SUCCESS;
}

// Ordinal 196: NtCancelTimer
static uint32_t __stdcall Shim_NtCancelTimer(HANDLE timerHandle, uint8_t* curState) {
    CancelWaitableTimer(timerHandle);
    if (curState) *curState = 0;
    return STATUS_SUCCESS;
}

// Ordinal 250: NtSuspendThread
static uint32_t __stdcall Shim_NtSuspendThread(HANDLE threadHandle, uint32_t* prevSuspendCount) {
    DWORD res = SuspendThread(threadHandle);
    if (prevSuspendCount) *prevSuspendCount = res;
    return STATUS_SUCCESS;
}

// Ordinal 246: NtSetInformationFile
static uint32_t __stdcall Shim_NtSetInformationFile(HANDLE fileHandle, PIO_STATUS_BLOCK ioStatus, void* fileInfo, uint32_t length, uint32_t fileInfoClass) {
    (void)length;
    // FilePositionInformation (class 14)
    if (fileInfoClass == 14 && fileInfo) {
        LARGE_INTEGER pos;
        pos.QuadPart = *(int64_t*)fileInfo;
        SetFilePointerEx(fileHandle, pos, NULL, FILE_BEGIN);
    }
    // FileDispositionInformation (class 13)
    if (fileInfoClass == 13 && fileInfo) {
        FILE_DISPOSITION_INFO fdi;
        fdi.DeleteFile = *(BOOLEAN*)fileInfo;
        SetFileInformationByHandle(fileHandle, FileDispositionInfo, &fdi, sizeof(fdi));
    }
    // FileEndOfFileInformation (class 20)
    if (fileInfoClass == 20 && fileInfo) {
        LARGE_INTEGER pos;
        pos.QuadPart = *(int64_t*)fileInfo;
        SetFilePointerEx(fileHandle, pos, NULL, FILE_BEGIN);
        SetEndOfFile(fileHandle);
    }
    if (ioStatus) ioStatus->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

// Ordinal 40: IoCreateSymbolicLink
static uint32_t __stdcall Shim_IoCreateSymbolicLink(PANSI_STRING symLink, PANSI_STRING devName) {
    (void)symLink; (void)devName;
    return STATUS_SUCCESS;
}

// Ordinal 67: IoMarkIrpMustComplete
static void __stdcall Shim_IoMarkIrpMustComplete(void* irp) {
    (void)irp;
}

// Ordinal 215: NtFsControlFile
static uint32_t __stdcall Shim_NtFsControlFile(HANDLE fileHandle, HANDLE event, void* apcRoutine, void* apcContext, PIO_STATUS_BLOCK ioStatus, uint32_t fsControlCode, void* inBuf, uint32_t inLen, void* outBuf, uint32_t outLen) {
    (void)fileHandle; (void)event; (void)apcRoutine; (void)apcContext; (void)fsControlCode; (void)inBuf; (void)inLen; (void)outBuf; (void)outLen;
    if (ioStatus) ioStatus->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

// Ordinal 203: NtCreateFile
static uint32_t __stdcall Shim_NtCreateFile(HANDLE* fileHandle, uint32_t access, POBJECT_ATTRIBUTES objAttrs, PIO_STATUS_BLOCK ioStatus, LARGE_INTEGER* allocSize, uint32_t fileAttrs, uint32_t shareAccess, uint32_t createDisp, uint32_t createOptions) {
    (void)allocSize; (void)fileAttrs; (void)shareAccess;
    if (!objAttrs || !objAttrs->ObjectName || !objAttrs->ObjectName->Buffer) return STATUS_INVALID_PARAMETER;

    char rawXboxPath[MAX_PATH];
    uint16_t nameLen = objAttrs->ObjectName->Length;
    if (nameLen >= MAX_PATH) nameLen = MAX_PATH - 1;
    memcpy(rawXboxPath, objAttrs->ObjectName->Buffer, nameLen);
    rawXboxPath[nameLen] = '\0';

    char win32Path[MAX_PATH];
    TranslateXboxPathToWin32(rawXboxPath, win32Path, MAX_PATH);

    bool isDir = (createOptions & 0x00000001) != 0 ||
                 (nameLen > 0 && (rawXboxPath[nameLen - 1] == '\\' || rawXboxPath[nameLen - 1] == '/'));

    // Self-heal any 0-byte SaveMeta.xbx leftover from previous uninitialized runs
    size_t winLen = strlen(win32Path);
    if (winLen >= 12 && _stricmp(win32Path + winLen - 12, "SaveMeta.xbx") == 0) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(win32Path, GetFileExInfoStandard, &fad) &&
            !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0) {
            DeleteFileA(win32Path);
            char parentDir[MAX_PATH];
            strncpy(parentDir, win32Path, sizeof(parentDir));
            parentDir[sizeof(parentDir) - 1] = '\0';
            char* lastSlash = strrchr(parentDir, '\\');
            if (!lastSlash) lastSlash = strrchr(parentDir, '/');
            if (lastSlash) {
                *lastSlash = '\0';
                RemoveDirectoryA(parentDir);
            }
        }
    }

    if (isDir) {
        if (createDisp == 2 || createDisp == 3 || createDisp == 5) {
            EnsureDirectoryExists(win32Path);
            // Clean up 0-byte SaveMeta.xbx inside target directory if it was left empty earlier
            char metaCheck[MAX_PATH];
            snprintf(metaCheck, sizeof(metaCheck), "%s\\SaveMeta.xbx", win32Path);
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(metaCheck, GetFileExInfoStandard, &fad) &&
                !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0) {
                DeleteFileA(metaCheck);
            }
            CreateDirectoryA(win32Path, NULL);
        }
        HANDLE hDir = CreateFileA(win32Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hDir == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            logFmt("[FS] NtCreateFile (Dir) FAILED: '%s' -> '%s' (disp=%u, err=%lu)\n", rawXboxPath, win32Path, createDisp, err);
            uint32_t ntErr = (err == ERROR_PATH_NOT_FOUND) ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
            if (ioStatus) ioStatus->Status = ntErr;
            return ntErr;
        }
        for (int i = 0; i < MAX_DIR_SEARCHES; i++) {
            if (s_dirSearches[i].dirHandle == NULL) {
                s_dirSearches[i].dirHandle = hDir;
                s_dirSearches[i].findHandle = INVALID_HANDLE_VALUE;
                strncpy(s_dirSearches[i].dirWin32Path, win32Path, MAX_PATH);
                break;
            }
        }
        logFmt("[FS] NtCreateFile (Dir): '%s' -> '%s' (disp=%u, Handle: %p)\n", rawXboxPath, win32Path, createDisp, hDir);
        if (fileHandle) *fileHandle = hDir;
        if (ioStatus) {
            ioStatus->Status = STATUS_SUCCESS;
            ioStatus->Information = (createDisp == 2) ? 2 : 1;
        }
        return STATUS_SUCCESS;
    }

    bool hasWriteAccess = (access == 0) || ((access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0);
    DWORD dwAccess = GENERIC_READ;
    if (hasWriteAccess) dwAccess |= GENERIC_WRITE;
    if (access & DELETE) dwAccess |= DELETE;
    DWORD dwShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    DWORD dwCreationDisposition = OPEN_ALWAYS;
    switch (createDisp) {
        case 1: // FILE_OPEN (OPEN_EXISTING)
            dwCreationDisposition = OPEN_EXISTING;
            break;
        case 2: // FILE_CREATE (CREATE_NEW)
            dwCreationDisposition = CREATE_NEW;
            break;
        case 3: // FILE_OPEN_IF (OPEN_ALWAYS)
            // Note: XAPI FUN_00104699 opens SaveMeta.xbx with GENERIC_READ (0x80100080) and createDisp=3
            // to check if a save exists. Do NOT create a blank 0-byte file on read-only open!
            dwCreationDisposition = hasWriteAccess ? OPEN_ALWAYS : OPEN_EXISTING;
            break;
        case 4: // FILE_OVERWRITE (TRUNCATE_EXISTING)
            dwCreationDisposition = TRUNCATE_EXISTING;
            break;
        case 5: // FILE_OVERWRITE_IF (CREATE_ALWAYS)
            dwCreationDisposition = CREATE_ALWAYS;
            break;
        default:
            dwCreationDisposition = hasWriteAccess ? OPEN_ALWAYS : OPEN_EXISTING;
            break;
    }

    if (dwCreationDisposition != OPEN_EXISTING && hasWriteAccess) {
        EnsureDirectoryExists(win32Path);
    }

    HANDLE h = CreateFileA(win32Path, dwAccess, dwShare, NULL, dwCreationDisposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        logFmt("[FS] NtCreateFile FAILED: '%s' -> '%s' (disp=%u, err=%lu)\n", rawXboxPath, win32Path, createDisp, err);
        if (err == ERROR_PATH_NOT_FOUND) {
            if (ioStatus) ioStatus->Status = STATUS_OBJECT_PATH_NOT_FOUND;
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        if (err == ERROR_FILE_NOT_FOUND) {
            if (ioStatus) ioStatus->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
            if (ioStatus) ioStatus->Status = STATUS_OBJECT_NAME_COLLISION; // 0xC0000035
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if (err == ERROR_ACCESS_DENIED) {
            if (ioStatus) ioStatus->Status = STATUS_ACCESS_DENIED;
            return STATUS_ACCESS_DENIED;
        }
        if (ioStatus) ioStatus->Status = STATUS_UNSUCCESSFUL;
        return STATUS_UNSUCCESSFUL;
    }

    DWORD err = GetLastError();
    logFmt("[FS] NtCreateFile: '%s' -> '%s' (disp=%u, Handle: %p)\n", rawXboxPath, win32Path, createDisp, h);
    if (fileHandle) *fileHandle = h;
    if (ioStatus) {
        ioStatus->Status = STATUS_SUCCESS;
        if (createDisp == 1) {
            ioStatus->Information = 1; // FILE_OPENED
        } else if (createDisp == 2) {
            ioStatus->Information = 2; // FILE_CREATED
        } else if (createDisp == 3) {
            ioStatus->Information = (dwCreationDisposition == OPEN_EXISTING || err == ERROR_ALREADY_EXISTS) ? 1 : 2;
        } else if (createDisp == 4) {
            ioStatus->Information = 3; // FILE_OVERWRITTEN
        } else if (createDisp == 5) {
            ioStatus->Information = (err == ERROR_ALREADY_EXISTS) ? 3 : 2;
        } else {
            ioStatus->Information = 1;
        }
    }
    return STATUS_SUCCESS;
}

// Ordinal 69: IoStartNextPacket
static void __stdcall Shim_IoStartNextPacket(void* devObj, uint8_t cancelable) {
    (void)devObj; (void)cancelable;
}

// Ordinal 91: KeInitializeInterrupt
static void __stdcall Shim_KeInitializeInterrupt(void* interrupt, void* serviceRoutine, void* serviceContext, uint32_t vector, uint8_t irql, uint32_t mode, uint8_t shareVector) {
    (void)interrupt; (void)serviceRoutine; (void)serviceContext; (void)vector; (void)irql; (void)mode; (void)shareVector;
}

// Ordinal 294: PsQueryStatistics
static uint32_t __stdcall Shim_PsQueryStatistics(void* procStats) {
    (void)procStats;
    return STATUS_SUCCESS;
}

// Ordinal 277: ObReferenceObjectByHandle
static uint32_t __stdcall Shim_ObReferenceObjectByHandle(HANDLE handle, void* objType, void** object) {
    (void)objType;
    if (object) *object = (void*)handle;
    return STATUS_SUCCESS;
}

// Ordinal 336: RtlLowerChar
static char __stdcall Shim_RtlLowerChar(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

// -------------------------------------------------------------
// Critical Section Dynamic Mapper
// -------------------------------------------------------------
typedef struct {
    void* xbox_cs;
    CRITICAL_SECTION win32_cs;
} CS_MAP;

#define MAX_CS_MAP 256
static CS_MAP g_CsMap[MAX_CS_MAP];
static int g_CsMapCount = 0;
static CRITICAL_SECTION g_CsMapLock;
static int g_CsMapLockInit = 0;

static CRITICAL_SECTION* GetOrCreateWin32CS(void* xbox_cs) {
    if (!xbox_cs) return NULL;
    if (!g_CsMapLockInit) {
        InitializeCriticalSection(&g_CsMapLock);
        g_CsMapLockInit = 1;
    }
    EnterCriticalSection(&g_CsMapLock);
    for (int i = 0; i < g_CsMapCount; i++) {
        if (g_CsMap[i].xbox_cs == xbox_cs) {
            LeaveCriticalSection(&g_CsMapLock);
            return &g_CsMap[i].win32_cs;
        }
    }
    if (g_CsMapCount < MAX_CS_MAP) {
        int idx = g_CsMapCount++;
        g_CsMap[idx].xbox_cs = xbox_cs;
        InitializeCriticalSection(&g_CsMap[idx].win32_cs);
        LeaveCriticalSection(&g_CsMapLock);
        return &g_CsMap[idx].win32_cs;
    }
    LeaveCriticalSection(&g_CsMapLock);
    return NULL;
}

// Ordinal 335: RtlLeaveCriticalSection
static void __stdcall Shim_RtlLeaveCriticalSection(CRITICAL_SECTION* cs) {
    CRITICAL_SECTION* wcs = GetOrCreateWin32CS(cs);
    if (wcs) LeaveCriticalSection(wcs);
}

// Ordinal 337: RtlMapGenericMask
static void __stdcall Shim_RtlMapGenericMask(uint32_t* accessMask, void* genericMapping) {
    (void)accessMask; (void)genericMapping;
}

// Ordinal 323: RtlFreeAnsiString
static void __stdcall Shim_RtlFreeAnsiString(PANSI_STRING str) {
    if (str && str->Buffer) {
        free(str->Buffer);
        str->Buffer = NULL;
    }
}

// Ordinal 325: RtlGetCallersAddress
static void __stdcall Shim_RtlGetCallersAddress(void** callersAddress, void** callersCaller) {
    if (callersAddress) *callersAddress = _ReturnAddress();
    if (callersCaller) *callersCaller = NULL;
}

// Ordinal 354: RtlTimeFieldsToTime
static uint8_t __stdcall Shim_RtlTimeFieldsToTime(PTIME_FIELDS tf, LARGE_INTEGER* time) {
    (void)tf;
    if (time) time->QuadPart = 0;
    return 1;
}

// Ordinal 149: KeWaitForSingleObject
static uint32_t __stdcall Shim_KeWaitForSingleObject(void* object, uint32_t waitReason, uint8_t waitMode, uint8_t alertable, LARGE_INTEGER* timeout) {
    (void)waitReason; (void)waitMode; (void)alertable;
    CpuProfiler_RecordThunkCall(17, _ReturnAddress());
    DWORD ms = timeout ? (DWORD)(-timeout->QuadPart / 10000) : INFINITE;
    DWORD res = WaitForSingleObject((HANDLE)object, ms);
    return res;
}

// Ordinal 97: KeInitializeTimerEx
static void __stdcall Shim_KeInitializeTimerEx(void* timer, uint32_t type) {
    (void)type;
    HANDLE h = CreateWaitableTimer(NULL, FALSE, NULL);
    *(HANDLE*)timer = h;
}

// Ordinal 16: HalDiskCachePartitionCount
static uint32_t g_HalDiskCachePartitionCount = 0;

// Ordinal 322: RtlEnterCriticalSection
static void __stdcall Shim_RtlEnterCriticalSection(CRITICAL_SECTION* cs) {
    CpuProfiler_RecordThunkCall(45, _ReturnAddress());
    CRITICAL_SECTION* wcs = GetOrCreateWin32CS(cs);
    if (wcs) EnterCriticalSection(wcs);
}

// Ordinal 189: MmUnmapIoSpace
static void __stdcall Shim_MmUnmapIoSpace(void* base, size_t numBytes) {
    (void)base; (void)numBytes;
}

// Ordinal 225: NtQueryFullAttributesFile
static uint32_t __stdcall Shim_NtQueryFullAttributesFile(POBJECT_ATTRIBUTES objAttrs, void* fileInfo) {
    (void)objAttrs; (void)fileInfo;
    return STATUS_SUCCESS;
}

// Ordinal 234: NtReadFile
static uint32_t __stdcall Shim_NtReadFile(HANDLE fileHandle, HANDLE event, void* apcRoutine, void* apcContext, PIO_STATUS_BLOCK ioStatus, void* buffer, uint32_t length, LARGE_INTEGER* byteOffset) {
    static uint32_t s_readCount = 0;
    s_readCount++;

    // Normalize physical aliases
    if ((uintptr_t)buffer >= 0x80000000 && (uintptr_t)buffer < 0x90000000) {
        buffer = (void*)((uintptr_t)buffer & 0x0FFFFFFF);
    }
    if ((uintptr_t)byteOffset >= 0x80000000 && (uintptr_t)byteOffset < 0x90000000) {
        byteOffset = (LARGE_INTEGER*)((uintptr_t)byteOffset & 0x0FFFFFFF);
    }
    if ((uintptr_t)ioStatus >= 0x80000000 && (uintptr_t)ioStatus < 0x90000000) {
        ioStatus = (PIO_STATUS_BLOCK)((uintptr_t)ioStatus & 0x0FFFFFFF);
    }

    // Auto-commit buffer if outside base allocation
    if (buffer && length > 0 && (uintptr_t)buffer >= 0x00010000 && (uintptr_t)buffer < 0x40000000) {
        uintptr_t startChunk = ((uintptr_t)buffer) & ~0xFFFFF;
        uintptr_t endChunk = (((uintptr_t)buffer) + length + 0xFFFFF) & ~0xFFFFF;
        VirtualAlloc((void*)startChunk, endChunk - startChunk, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        VirtualAlloc((void*)startChunk, endChunk - startChunk, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }

    static CRITICAL_SECTION s_fileReadLock;
    static int s_fileReadLockInit = 0;
    if (!s_fileReadLockInit) {
        InitializeCriticalSection(&s_fileReadLock);
        s_fileReadLockInit = 1;
    }

    EnterCriticalSection(&s_fileReadLock);
    if (byteOffset && byteOffset->QuadPart != -1LL) {
        SetFilePointerEx(fileHandle, *byteOffset, NULL, FILE_BEGIN);
    }
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(fileHandle, buffer, length, &bytesRead, NULL);
    LeaveCriticalSection(&s_fileReadLock);
    
    if (s_readCount <= 80 || !ok || (s_readCount % 200 == 0)) {
        logFmt("[FS] NtReadFile #%u: h=%p, buf=%p, len=%u, off=%lld, read=%u, ok=%d (err=%lu)\n",
               s_readCount, fileHandle, buffer, length, byteOffset ? byteOffset->QuadPart : -1LL, bytesRead, ok, ok ? 0 : GetLastError());
    }

    if (!ok) {
        logFmt("[FS] NtReadFile FAILED: handle=%p, len=%u (err=%lu)\n", fileHandle, length, GetLastError());
        if (ioStatus) {
            ioStatus->Status = STATUS_UNSUCCESSFUL;
            ioStatus->Information = 0;
        }
        if (event) SetEvent(event);
        return STATUS_UNSUCCESSFUL;
    }

    if (bytesRead == 0 && length > 0) {
        if (ioStatus) {
            ioStatus->Status = STATUS_END_OF_FILE;
            ioStatus->Information = 0;
        }
        if (event) SetEvent(event);
        return STATUS_END_OF_FILE;
    }

    if (ioStatus) {
        ioStatus->Status = STATUS_SUCCESS;
        ioStatus->Information = bytesRead;
    }
    if (event) SetEvent(event);
    if (apcRoutine) {
        typedef void (__stdcall *PIO_APC_ROUTINE)(void* context, PIO_STATUS_BLOCK ioStatus, uint32_t reserved);
        ((PIO_APC_ROUTINE)apcRoutine)(apcContext, ioStatus, 0);
    }
    return STATUS_SUCCESS;
}

// Ordinal 99: KeInsertDeviceQueue
static uint8_t __stdcall Shim_KeInsertDeviceQueue(void* queue, void* entry) {
    (void)queue; (void)entry;
    return 1;
}

// Ordinal 326: RtlInitAnsiString
static void __stdcall Shim_RtlInitAnsiString(PANSI_STRING dest, const char* src) {
    if (!dest) return;
    if (!src) {
        dest->Length = dest->MaximumLength = 0;
        dest->Buffer = NULL;
        return;
    }
    dest->Length = (uint16_t)strlen(src);
    dest->MaximumLength = dest->Length + 1;
    dest->Buffer = (char*)src;
}

// Ordinal 83: KeDelayExecutionThread
static uint32_t __stdcall Shim_KeDelayExecutionThread(uint8_t waitMode, uint8_t alertable, LARGE_INTEGER* interval) {
    (void)waitMode; (void)alertable;
    CpuProfiler_RecordThunkCall(59, _ReturnAddress());
    if (interval) {
        int64_t rel = -interval->QuadPart;
        DWORD ms = (rel > 0) ? (DWORD)(rel / 10000) : 0;
        if (ms == 0 && rel > 0 && CpuProfiler_AreSpinloopFixesEnabled()) {
            ms = 1; // Prevent sub-millisecond Xbox delays from becoming Sleep(0) busy-spins
        }
        Sleep(ms);
    }
    return STATUS_SUCCESS;
}

// Ordinal 87: KeInitializeDpc
static void __stdcall Shim_KeInitializeDpc(void* dpc, void* routine, void* context) {
    (void)dpc; (void)routine; (void)context;
}

// Ordinal 81: KeConnectInterrupt
static uint8_t __stdcall Shim_KeConnectInterrupt(void* interrupt) {
    (void)interrupt;
    return 1;
}

// Ordinal 15: ExAllocatePoolWithTag
static void* __stdcall Shim_ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag) {
    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NumberOfBytes);
    return p;
}

// Ordinal 16: ExEventObjectType
static uint32_t g_ExEventObjectTypeVal = 0x6C061D00;
static uint32_t* g_ExEventObjectType = &g_ExEventObjectTypeVal;

// Ordinal 17: ExFreePool
static void __stdcall Shim_ExFreePool(void* P) {
    if (P) {
        HeapFree(GetProcessHeap(), 0, P);
    }
}

// Ordinal 23: ExQueryPoolBlockSize
static SIZE_T __stdcall Shim_ExQueryPoolBlockSize(void* P) {
    if (!P) return 0;
    return HeapSize(GetProcessHeap(), 0, P);
}

// Ordinal 356: HalBootSMCVideoMode (1 = XC_AV_PACK_HDTV, enables 480p/720p/1080i)
static uint32_t g_HalBootSMCVideoMode = 1;

// Ordinal 358: HalIsResetOrShutdownPending
static uint8_t __stdcall Shim_HalIsResetOrShutdownPending(void) {
    return 0;
}

// Ordinal 360: HalInitiateShutdown
static void __stdcall Shim_HalInitiateShutdown(void) { printf("[XBOXKRNL] HalInitiateShutdown called!\n"); ExitProcess(0); }

// Ordinal 207: NtDeviceIoControlFile
static uint32_t __stdcall Shim_NtDeviceIoControlFile(HANDLE fileHandle, HANDLE event, void* apcRoutine, void* apcContext, PIO_STATUS_BLOCK ioStatus, uint32_t ioControlCode, void* inBuf, uint32_t inLen, void* outBuf, uint32_t outLen) {
    (void)fileHandle; (void)event; (void)apcRoutine; (void)apcContext; (void)ioControlCode; (void)inBuf; (void)inLen; (void)outBuf; (void)outLen;
    if (ioStatus) ioStatus->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

// Ordinal 344: RtlRaiseException
static void __stdcall Shim_RtlRaiseException(EXCEPTION_RECORD* rec) {
    RaiseException(rec->ExceptionCode, rec->ExceptionFlags, rec->NumberParameters, rec->ExceptionInformation);
}

// Ordinal 355: RtlTimeToTimeFields
static void __stdcall Shim_RtlTimeToTimeFields(LARGE_INTEGER* time, PTIME_FIELDS tf) {
    (void)time;
    if (tf) {
        SYSTEMTIME st;
        GetSystemTime(&st);
        tf->Year = st.wYear;
        tf->Month = st.wMonth;
        tf->Day = st.wDay;
        tf->Hour = st.wHour;
        tf->Minute = st.wMinute;
        tf->Second = st.wSecond;
        tf->Milliseconds = st.wMilliseconds;
        tf->Weekday = st.wDayOfWeek;
    }
}

// Ordinal 47: IoDismountVolumeByName
static uint32_t __stdcall Shim_IoDismountVolumeByName(PANSI_STRING devName) {
    (void)devName;
    return STATUS_SUCCESS;
}

// Ordinal 324: RtlFreeUnicodeString
static void __stdcall Shim_RtlFreeUnicodeString(PUNICODE_STRING str) {
    if (str && str->Buffer) {
        free(str->Buffer);
        str->Buffer = NULL;
    }
}

// Ordinal 143: KeSuspendThread
static uint32_t __stdcall Shim_KeSuspendThread(HANDLE threadHandle) {
    return SuspendThread(threadHandle);
}

// Ordinal 259: NtWriteFile
static uint32_t __stdcall Shim_NtWriteFile(HANDLE fileHandle, HANDLE event, void* apcRoutine, void* apcContext, PIO_STATUS_BLOCK ioStatus, const void* buffer, uint32_t length, LARGE_INTEGER* byteOffset) {
    // Normalize physical aliases
    if ((uintptr_t)buffer >= 0x80000000 && (uintptr_t)buffer < 0x90000000) {
        buffer = (const void*)((uintptr_t)buffer & 0x0FFFFFFF);
    }
    if ((uintptr_t)byteOffset >= 0x80000000 && (uintptr_t)byteOffset < 0x90000000) {
        byteOffset = (LARGE_INTEGER*)((uintptr_t)byteOffset & 0x0FFFFFFF);
    }
    if ((uintptr_t)ioStatus >= 0x80000000 && (uintptr_t)ioStatus < 0x90000000) {
        ioStatus = (PIO_STATUS_BLOCK)((uintptr_t)ioStatus & 0x0FFFFFFF);
    }

    if (byteOffset && byteOffset->QuadPart != -1LL) {
        SetFilePointerEx(fileHandle, *byteOffset, NULL, FILE_BEGIN);
    }
    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(fileHandle, buffer, length, &bytesWritten, NULL);
    logFmt("[FS] NtWriteFile: h=%p, buf=%p, len=%u, off=%lld, written=%u, ok=%d (err=%lu)\n",
           fileHandle, buffer, length, byteOffset ? byteOffset->QuadPart : -1LL, bytesWritten, ok, ok ? 0 : GetLastError());

    if (!ok) {
        if (ioStatus) {
            ioStatus->Status = STATUS_UNSUCCESSFUL;
            ioStatus->Information = 0;
        }
        if (event) SetEvent(event);
        return STATUS_UNSUCCESSFUL;
    }
    if (ioStatus) {
        ioStatus->Status = STATUS_SUCCESS;
        ioStatus->Information = bytesWritten;
    }
    if (event) SetEvent(event);
    if (apcRoutine) {
        typedef void (__stdcall *PIO_APC_ROUTINE)(void* context, PIO_STATUS_BLOCK ioStatus, uint32_t reserved);
        ((PIO_APC_ROUTINE)apcRoutine)(apcContext, ioStatus, 0);
    }
    return STATUS_SUCCESS;
}

// Ordinal 124: KeResetEvent
static int32_t __stdcall Shim_KeResetEvent(HANDLE eventHandle) {
    ResetEvent(eventHandle);
    return 0;
}

// Ordinal 144: KeSynchronizeExecution
static uint8_t __stdcall Shim_KeSynchronizeExecution(void* interrupt, void* syncRoutine, void* syncContext) {
    typedef uint8_t (*SyncFunc)(void*);
    return ((SyncFunc)syncRoutine)(syncContext);
}

// Ordinal 231: NtQuerySymbolicLinkObject
static uint32_t __stdcall Shim_NtQuerySymbolicLinkObject(HANDLE linkHandle, PANSI_STRING target, uint32_t* retLen) {
    (void)linkHandle; (void)target;
    if (retLen) *retLen = 0;
    return STATUS_SUCCESS;
}

// Ordinal 224: NtQueryDirectoryFile
static uint32_t __stdcall Shim_NtQueryDirectoryFile(HANDLE fileHandle, HANDLE event, void* apcRoutine, void* apcContext, PIO_STATUS_BLOCK ioStatus, void* fileInfo, uint32_t length, uint8_t retSingleEntry, PANSI_STRING fileName, uint8_t restartScan) {
    (void)apcRoutine; (void)apcContext; (void)retSingleEntry;
    
    DIR_SEARCH_ENTRY* pEntry = NULL;
    for (int i = 0; i < MAX_DIR_SEARCHES; i++) {
        if (s_dirSearches[i].dirHandle == fileHandle) {
            pEntry = &s_dirSearches[i];
            break;
        }
    }

    if (!pEntry) {
        logFmt("[FS] NtQueryDirectoryFile: unknown dir handle %p\n", fileHandle);
        if (ioStatus) {
            ioStatus->Status = STATUS_NO_MORE_FILES;
            ioStatus->Information = 0;
        }
        if (event) SetEvent(event);
        return STATUS_NO_MORE_FILES;
    }

    WIN32_FIND_DATAA wfd;
    BOOL found = FALSE;

    if (pEntry->findHandle == INVALID_HANDLE_VALUE || restartScan) {
        if (pEntry->findHandle != INVALID_HANDLE_VALUE) {
            FindClose(pEntry->findHandle);
            pEntry->findHandle = INVALID_HANDLE_VALUE;
        }

        char searchPattern[MAX_PATH];
        char patternStr[64] = "*";
        if (fileName && fileName->Buffer && fileName->Length > 0) {
            uint16_t plen = fileName->Length;
            if (plen >= sizeof(patternStr)) plen = sizeof(patternStr) - 1;
            memcpy(patternStr, fileName->Buffer, plen);
            patternStr[plen] = '\0';
        }
        snprintf(searchPattern, sizeof(searchPattern), "%s\\%s", pEntry->dirWin32Path, patternStr);

        pEntry->findHandle = FindFirstFileA(searchPattern, &wfd);
        if (pEntry->findHandle != INVALID_HANDLE_VALUE) {
            found = TRUE;
        }
    } else {
        found = FindNextFileA(pEntry->findHandle, &wfd);
    }

    if (!found) {
        if (ioStatus) {
            ioStatus->Status = STATUS_NO_MORE_FILES;
            ioStatus->Information = 0;
        }
        if (event) SetEvent(event);
        return STATUS_NO_MORE_FILES;
    }

    typedef struct {
        ULONG NextEntryOffset;
        ULONG FileIndex;
        LARGE_INTEGER CreationTime;
        LARGE_INTEGER LastAccessTime;
        LARGE_INTEGER LastWriteTime;
        LARGE_INTEGER ChangeTime;
        LARGE_INTEGER EndOfFile;
        LARGE_INTEGER AllocationSize;
        ULONG FileAttributes;
        ULONG FileNameLength;
        CHAR FileName[1];
    } XBOX_FILE_DIRECTORY_INFORMATION;

    XBOX_FILE_DIRECTORY_INFORMATION* pInfo = (XBOX_FILE_DIRECTORY_INFORMATION*)fileInfo;
    if (fileInfo && length >= sizeof(XBOX_FILE_DIRECTORY_INFORMATION)) {
        pInfo->NextEntryOffset = 0;
        pInfo->FileIndex = 0;
        pInfo->CreationTime.LowPart = wfd.ftCreationTime.dwLowDateTime;
        pInfo->CreationTime.HighPart = wfd.ftCreationTime.dwHighDateTime;
        pInfo->LastAccessTime.LowPart = wfd.ftLastAccessTime.dwLowDateTime;
        pInfo->LastAccessTime.HighPart = wfd.ftLastAccessTime.dwHighDateTime;
        pInfo->LastWriteTime.LowPart = wfd.ftLastWriteTime.dwLowDateTime;
        pInfo->LastWriteTime.HighPart = wfd.ftLastWriteTime.dwHighDateTime;
        pInfo->ChangeTime = pInfo->LastWriteTime;
        pInfo->EndOfFile.LowPart = wfd.nFileSizeLow;
        pInfo->EndOfFile.HighPart = wfd.nFileSizeHigh;
        pInfo->AllocationSize = pInfo->EndOfFile;
        pInfo->FileAttributes = wfd.dwFileAttributes;

        uint32_t nameLen = (uint32_t)strlen(wfd.cFileName);
        pInfo->FileNameLength = nameLen;
        uint32_t maxCopy = length - (uint32_t)((uintptr_t)pInfo->FileName - (uintptr_t)pInfo);
        if (nameLen < maxCopy) {
            memcpy(pInfo->FileName, wfd.cFileName, nameLen);
            pInfo->FileName[nameLen] = '\0';
        }

        if (ioStatus) {
            ioStatus->Status = STATUS_SUCCESS;
            ioStatus->Information = sizeof(XBOX_FILE_DIRECTORY_INFORMATION) + nameLen;
        }
        logFmt("[FS] NtQueryDirectoryFile: '%s' (attrib=0x%X, size=%lu)\n", wfd.cFileName, wfd.dwFileAttributes, wfd.nFileSizeLow);
    }

    if (event) SetEvent(event);
    return STATUS_SUCCESS;
}

// Ordinal 302: RtlAnsiStringToUnicodeString
static uint32_t __stdcall Shim_RtlAnsiStringToUnicodeString(PUNICODE_STRING dest, PANSI_STRING src, uint8_t allocDest) {
    if (!dest || !src || !src->Buffer) return STATUS_INVALID_PARAMETER;
    int len = MultiByteToWideChar(CP_ACP, 0, src->Buffer, src->Length, NULL, 0);
    if (allocDest) {
        dest->Buffer = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
        dest->MaximumLength = (len + 1) * sizeof(wchar_t);
    }
    MultiByteToWideChar(CP_ACP, 0, src->Buffer, src->Length, dest->Buffer, len);
    dest->Buffer[len] = L'\0';
    dest->Length = len * sizeof(wchar_t);
    return STATUS_SUCCESS;
}

// Ordinal 238: NtSetEvent
static uint32_t __stdcall Shim_NtSetEvent(HANDLE eventHandle, uint32_t* prevCount) {
    SetEvent(eventHandle);
    if (prevCount) *prevCount = 0;
    return STATUS_SUCCESS;
}

// Ordinal 258: NtWaitForSingleObjectEx
static uint32_t __stdcall Shim_NtWaitForSingleObjectEx(HANDLE handle, uint8_t waitMode, uint8_t alertable, LARGE_INTEGER* timeout) {
    (void)waitMode; (void)alertable;
    DWORD ms = timeout ? (DWORD)(-timeout->QuadPart / 10000) : INFINITE;
    return WaitForSingleObject(handle, ms);
}

// Ordinal 255: NtWaitForSingleObject
static uint32_t __stdcall Shim_NtWaitForSingleObject(HANDLE handle, uint8_t alertable, LARGE_INTEGER* timeout) {
    (void)alertable;
    DWORD ms = timeout ? (DWORD)(-timeout->QuadPart / 10000) : INFINITE;
    return WaitForSingleObject(handle, ms);
}

// Ordinal 360: XboxKrnlVersion
static uint32_t g_XboxKrnlVersion[4] = { 1, 0, 5838, 1 };

// Ordinal 44: IoDeleteSymbolicLink
static uint32_t __stdcall Shim_IoDeleteSymbolicLink(PANSI_STRING symLink) {
    (void)symLink;
    return STATUS_SUCCESS;
}

// Ordinal 160: KfReleaseSpinLock
static void __fastcall Shim_KfReleaseSpinLock(void* spinlock, uint8_t newIrql) {
    (void)spinlock; (void)newIrql;
}

// Ordinal 98: KeInsertByKeyDeviceQueue
static uint8_t __stdcall Shim_KeInsertByKeyDeviceQueue(void* queue, void* entry, uint32_t sortKey) {
    (void)queue; (void)entry; (void)sortKey;
    return 1;
}

// Ordinal 109: KeQueryBasePriorityThread
static int32_t __stdcall Shim_KeQueryBasePriorityThread(HANDLE thread) {
    return GetThreadPriority(thread);
}

// Ordinal 151: KfAcquireSpinLock
static uint8_t __fastcall Shim_KfAcquireSpinLock(void* spinlock) {
    (void)spinlock;
    return 0;
}

// Ordinal 340: RtlMoveMemory
static void __stdcall Shim_RtlMoveMemory(void* dest, const void* src, size_t len) {
    memmove(dest, src, len);
}

// Ordinal 279: ObfDereferenceObject
static void __fastcall Shim_ObfDereferenceObject(void* object) {
    (void)object;
}

// Ordinal 327: RtlInitUnicodeString
static void __stdcall Shim_RtlInitUnicodeString(PUNICODE_STRING dest, const wchar_t* src) {
    if (!dest) return;
    if (!src) {
        dest->Length = dest->MaximumLength = 0;
        dest->Buffer = NULL;
        return;
    }
    dest->Length = (uint16_t)(wcslen(src) * sizeof(wchar_t));
    dest->MaximumLength = dest->Length + sizeof(wchar_t);
    dest->Buffer = (wchar_t*)src;
}

// Ordinal 328: RtlInitializeCriticalSection
static void __stdcall Shim_RtlInitializeCriticalSection(CRITICAL_SECTION* cs) {
    GetOrCreateWin32CS(cs);
}

// Ordinal 199: NtClose
static uint32_t __stdcall Shim_NtClose(HANDLE handle) {
    for (int i = 0; i < MAX_DIR_SEARCHES; i++) {
        if (s_dirSearches[i].dirHandle == handle) {
            if (s_dirSearches[i].findHandle != INVALID_HANDLE_VALUE) {
                FindClose(s_dirSearches[i].findHandle);
            }
            memset(&s_dirSearches[i], 0, sizeof(DIR_SEARCH_ENTRY));
            break;
        }
    }
    CloseHandle(handle);
    return STATUS_SUCCESS;
}

// Ordinal 184: MmQueryStatistics
static uint32_t __stdcall Shim_MmQueryStatistics(void* stats) {
    (void)stats;
    return STATUS_SUCCESS;
}

// Ordinal 269: NtYieldExecution
static uint32_t __stdcall Shim_NtYieldExecution(void) {
    CpuProfiler_RecordThunkCall(81, _ReturnAddress());
    BOOL switched = SwitchToThread();
    // On single-core Xbox, low-priority worker threads (e.g. NWFSX @ 0x00118E60) call SwitchToThread()
    // in a tight polling loop. On multi-core PCs, SwitchToThread() returns FALSE immediately when no
    // other thread is queued on that core, pegging a full CPU core at 100%.
    if (!switched && CpuProfiler_AreSpinloopFixesEnabled()) {
        Sleep(1);
    }
    return STATUS_SUCCESS;
}

// Ordinal 291: PsCreateSystemThreadEx
static uint32_t __stdcall Shim_PsCreateSystemThreadEx(HANDLE* threadHandle, uint32_t access, void* objAttrs, HANDLE procHandle, void* clientId, LPTHREAD_START_ROUTINE startRoutine, void* startContext, uint8_t createSuspended, uint32_t tlsIndex, void* unknown) {
    (void)access; (void)objAttrs; (void)procHandle; (void)tlsIndex; (void)unknown;
    DWORD flags = createSuspended ? CREATE_SUSPENDED : 0;
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, startRoutine, startContext, flags, &tid);
    printf("[XBOXKRNL] PsCreateSystemThreadEx: routine=%p, ctx=%p, flags=%lu -> handle=%p, tid=%lu (err=%lu)\n", (void*)startRoutine, startContext, flags, (void*)h, tid, h ? 0 : GetLastError());
    fflush(stdout);
    if (!h) return STATUS_UNSUCCESSFUL;
    if (clientId) *(DWORD*)clientId = tid;
    if (threadHandle) *threadHandle = h;
    CpuProfiler_RegisterThread(h, tid, (void*)startRoutine, NULL);
    return STATUS_SUCCESS;
}

// Ordinal 217: NtOpenDirectoryObject
static uint32_t __stdcall Shim_NtOpenDirectoryObject(HANDLE* dirHandle, uint32_t access, POBJECT_ATTRIBUTES objAttrs) {
    (void)access; (void)objAttrs;
    if (dirHandle) *dirHandle = (HANDLE)0x3456;
    return STATUS_SUCCESS;
}

// Ordinal 357: RtlUpperString
static void __stdcall Shim_RtlUpperString(PANSI_STRING dest, const PANSI_STRING src) {
    if (!dest || !src) return;
    for (uint16_t i = 0; i < src->Length; i++) {
        dest->Buffer[i] = (char)toupper((unsigned char)src->Buffer[i]);
    }
    dest->Length = src->Length;
}

// Ordinal 173: MmGetPhysicalAddress
static uint32_t __stdcall Shim_MmGetPhysicalAddress(void* virtAddr) {
    return (uint32_t)(uintptr_t)virtAddr;
}

// Ordinal 175: MmIsAddressValid
static uint8_t __stdcall Shim_MmIsAddressValid(void* addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi))) {
        return (mbi.State == MEM_COMMIT);
    }
    return 0;
}

// Ordinal 119: KeRemoveQueue
static void* __stdcall Shim_KeRemoveQueue(void* queue, uint8_t waitMode, LARGE_INTEGER* timeout) {
    (void)queue; (void)waitMode; (void)timeout;
    return NULL;
}

// Ordinal 176: MmLockUnlockBufferPages
static void __stdcall Shim_MmLockUnlockBufferPages(void* base, size_t numBytes, uint8_t lock) {
    (void)base; (void)numBytes; (void)lock;
}

// Ordinal 210: NtDuplicateObject
static uint32_t __stdcall Shim_NtDuplicateObject(HANDLE srcProc, HANDLE srcHandle, HANDLE tgtProc, HANDLE* tgtHandle, uint32_t access, uint32_t attrs, uint32_t options) {
    (void)srcProc; (void)tgtProc; (void)access; (void)attrs; (void)options;
    if (tgtHandle) *tgtHandle = srcHandle;
    return STATUS_SUCCESS;
}

// Ordinal 1: AvGetSavedDataAddress
static void* __stdcall Shim_AvGetSavedDataAddress(void) {
    static uint32_t s_AvData[8] = {0};
    return s_AvData;
}

// Ordinal 2: AvSendTVEncoderOption
static uint32_t __stdcall Shim_AvSendTVEncoderOption(void* registerBase, uint32_t option, uint32_t param, uint32_t* result) {
    (void)registerBase; (void)option; (void)param;
    if (result) *result = 0;
    return STATUS_SUCCESS;
}

// Ordinal 3: AvSetDisplayMode
static uint32_t __stdcall Shim_AvSetDisplayMode(void* registerBase, uint32_t step, uint32_t mode, uint32_t format, uint32_t pitch, uint32_t frameBuffer) {
    (void)registerBase; (void)step; (void)mode; (void)format; (void)pitch; (void)frameBuffer;
    return STATUS_SUCCESS;
}

// Ordinal 4: AvSetSavedDataAddress
static void __stdcall Shim_AvSetSavedDataAddress(void* addr) {
    (void)addr;
}

// Ordinal 46: IoDismountVolume
static uint32_t __stdcall Shim_IoDismountVolume(void* devObj) {
    (void)devObj;
    return STATUS_SUCCESS;
}

// Ordinal 100: KeInsertHeadQueue
static int32_t __stdcall Shim_KeInsertHeadQueue(void* queue, void* entry) {
    (void)queue; (void)entry;
    return 0;
}

// Ordinal 8: DbgBreakPoint
static void __stdcall Shim_DbgBreakPoint(void) {
    if (IsDebuggerPresent()) DebugBreak();
}

// Ordinal 168: MmClaimGpuInstanceMemory
static void* __stdcall Shim_MmClaimGpuInstanceMemory(size_t numBytes, uint32_t* allocFlags) {
    (void)allocFlags;
    return VirtualAlloc(NULL, numBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

// Ordinal 142: KeStallExecutionProcessor
static void __stdcall Shim_KeStallExecutionProcessor(uint32_t microSeconds) {
    // High-resolution busy spin
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int64_t target = start.QuadPart + (microSeconds * freq.QuadPart) / 1000000;
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target);
}

// Ordinal 139: KeSetTimer
static uint8_t __stdcall Shim_KeSetTimer(HANDLE timer, LARGE_INTEGER dueTime, void* dpc) {
    (void)dpc;
    return (uint8_t)SetWaitableTimer(timer, &dueTime, 0, NULL, NULL, FALSE);
}

// Ordinal 137: KeSetEvent
static int32_t __stdcall Shim_KeSetEvent(HANDLE eventHandle, int32_t increment, uint8_t wait) {
    (void)increment; (void)wait;
    SetEvent(eventHandle);
    return 0;
}

// Ordinal 153: KfLowerIrql
static void __fastcall Shim_KfLowerIrql(uint8_t newIrql) {
    (void)newIrql;
}

// Ordinal 95: KeInitializeQueue
static void __stdcall Shim_KeInitializeQueue(void* queue, uint32_t count) {
    (void)queue; (void)count;
}

// Ordinal 312: RtlEqualString
static uint8_t __stdcall Shim_RtlEqualString(const PANSI_STRING s1, const PANSI_STRING s2, uint8_t caseInsensitive) {
    if (!s1 || !s2 || s1->Length != s2->Length) return 0;
    if (caseInsensitive) {
        return (_strnicmp(s1->Buffer, s2->Buffer, s1->Length) == 0);
    }
    return (memcmp(s1->Buffer, s2->Buffer, s1->Length) == 0);
}

// Generic dummy fallback for unhandled ordinals
static uint32_t __stdcall Shim_DefaultStub(void) {
    printf("[XBOXKRNL] Warning: Shim_DefaultStub called!\n");
    return STATUS_SUCCESS;
}

// -------------------------------------------------------------
// Ordinal Dispatcher Map
// -------------------------------------------------------------


// Additional kernel shims and variables for 100% complete thunk coverage
static uint8_t __stdcall Shim_KeRaiseIrqlToDpcLevel(void) { return 0; }
static uint32_t __stdcall Shim_RtlNtStatusToDosError(uint32_t status) {
    switch (status) {
        case 0x00000000: return 0;     // STATUS_SUCCESS -> ERROR_SUCCESS
        case 0x00000103: return 997;   // STATUS_PENDING -> ERROR_IO_PENDING
        case 0x80000006: return 18;    // STATUS_NO_MORE_FILES -> ERROR_NO_MORE_FILES
        case 0xC000000D: return 87;    // STATUS_INVALID_PARAMETER -> ERROR_INVALID_PARAMETER
        case 0xC000000F: return 2;     // STATUS_NO_SUCH_FILE -> ERROR_FILE_NOT_FOUND
        case 0xC0000011: return 38;    // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
        case 0xC0000017: return 8;     // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
        case 0xC0000022: return 5;     // STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED
        case 0xC0000033: return 123;   // STATUS_OBJECT_NAME_INVALID -> ERROR_INVALID_NAME
        case 0xC0000034: return 2;     // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
        case 0xC0000035: return 183;   // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS (0xB7)
        case 0xC000003A: return 3;     // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
        case 0xC0000043: return 32;    // STATUS_SHARING_VIOLATION -> ERROR_SHARING_VIOLATION
        case 0xC000007F: return 112;   // STATUS_DISK_FULL -> ERROR_DISK_FULL
        case 0xC0000101: return 145;   // STATUS_DIRECTORY_NOT_EMPTY -> ERROR_DIR_NOT_EMPTY
        default:         return 2;     // Default fallback -> ERROR_FILE_NOT_FOUND
    }
}
static void __stdcall Shim_HalReturnToFirmware(uint32_t action) { printf("[XBOXKRNL] HalReturnToFirmware called! Action=%u\n", action); ExitProcess(0); }
static uint32_t __stdcall Shim_NtUserIoApcDispatcher(void* apcContext, PIO_STATUS_BLOCK ioStatus, uint32_t reserved) {
    (void)reserved;
    if (apcContext && ioStatus) {
        typedef void (__stdcall *LPOVERLAPPED_COMPLETION_ROUTINE_XBOX)(uint32_t dwErrorCode, uint32_t dwNumberOfBytesTransfered, void* lpOverlapped);
        uint32_t dosErr = Shim_RtlNtStatusToDosError((uint32_t)ioStatus->Status);
        ((LPOVERLAPPED_COMPLETION_ROUTINE_XBOX)apcContext)(dosErr, (uint32_t)ioStatus->Information, (void*)ioStatus);
    }
    return 0;
}

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
} XBOX_SHA1_CTX;

static uint32_t Sha1Rol32(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void Sha1Transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8)  |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = Sha1Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;            k = 0xCA62C1D6; }
        uint32_t temp = Sha1Rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Sha1Rol32(b, 30); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void __stdcall Shim_XcSHAInit(void* rawCtx) {
    if (!rawCtx) return;
    XBOX_SHA1_CTX* ctx = (XBOX_SHA1_CTX*)rawCtx;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
}

static void __stdcall Shim_XcSHAUpdate(void* rawCtx, const void* input, uint32_t len) {
    if (!rawCtx || !input || len == 0) return;
    XBOX_SHA1_CTX* ctx = (XBOX_SHA1_CTX*)rawCtx;
    const uint8_t* data = (const uint8_t*)input;
    uint32_t j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += (len << 3)) < (len << 3)) ctx->count[1]++;
    ctx->count[1] += (len >> 29);
    uint32_t i = 0;
    if ((j + len) > 63) {
        i = 64 - j;
        memcpy(&ctx->buffer[j], data, i);
        Sha1Transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) {
            Sha1Transform(ctx->state, &data[i]);
        }
        j = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

static void __stdcall Shim_XcSHAFinal(void* rawCtx, void* digest) {
    if (!rawCtx || !digest) return;
    XBOX_SHA1_CTX* ctx = (XBOX_SHA1_CTX*)rawCtx;
    uint8_t finalcount[8];
    for (int i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((ctx->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    uint8_t c = 0x80;
    Shim_XcSHAUpdate(ctx, &c, 1);
    c = 0x00;
    while (((ctx->count[0] >> 3) & 63) != 56) {
        Shim_XcSHAUpdate(ctx, &c, 1);
    }
    Shim_XcSHAUpdate(ctx, finalcount, 8);
    uint8_t* out = (uint8_t*)digest;
    for (int i = 0; i < 20; i++) {
        out[i] = (uint8_t)((ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

static uint8_t g_XboxHDKey[16] = {
    0x53, 0x43, 0x32, 0x5F, 0x48, 0x44, 0x44, 0x5F,
    0x4B, 0x45, 0x59, 0x5F, 0x32, 0x30, 0x30, 0x33
};
static uint8_t g_XboxSignatureKey[16] = {
    0x53, 0x43, 0x32, 0x5F, 0x53, 0x49, 0x47, 0x5F,
    0x4B, 0x45, 0x59, 0x5F, 0x32, 0x30, 0x30, 0x33
};
static uint8_t g_XboxAlternateSignatureKeys[16 * 16] = {0};
static uint8_t __stdcall Shim_KeCancelTimer(HANDLE timer) { return (uint8_t)CancelWaitableTimer(timer); }
static uint32_t g_XboxHardwareInfo = 0x00000000;
static const char g_XeImageFileName[] = "default.xbe";
static void __fastcall Shim_IofCompleteRequest(void* irp, int8_t boost) { (void)irp; (void)boost; }
static uint32_t __stdcall Shim_XcVerifyPKCS1Signature(void* a1, void* a2, void* a3) { (void)a1; (void)a2; (void)a3; return 1; }
static uint8_t g_XePublicKeyData[256] = {0};
static uint32_t __stdcall Shim_HalRegisterShutdownNotification(void* record, uint8_t registerNotice) { (void)record; (void)registerNotice; return 0; }
static uint32_t g_PsThreadObjectType = 0;
static uint32_t __stdcall Shim_HalGetInterruptVector(uint32_t busInterruptLevel, uint8_t* irql) { (void)busInterruptLevel; if (irql) *irql = 0; return 0; }
static void __stdcall Shim_XcHMAC(const uint8_t* key, uint32_t keyLen, const uint8_t* data1, uint32_t len1, const uint8_t* data2, uint32_t len2, uint8_t* digest) {
    if (!digest) return;
    uint8_t k_ipad[64], k_opad[64];
    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    if (key && keyLen > 0) {
        if (keyLen > 64) keyLen = 64;
        memcpy(k_ipad, key, keyLen);
        memcpy(k_opad, key, keyLen);
    }
    for (int i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5C;
    }
    XBOX_SHA1_CTX ctx;
    uint8_t innerDigest[20];
    Shim_XcSHAInit(&ctx);
    Shim_XcSHAUpdate(&ctx, k_ipad, 64);
    if (data1 && len1 > 0) Shim_XcSHAUpdate(&ctx, data1, len1);
    if (data2 && len2 > 0) Shim_XcSHAUpdate(&ctx, data2, len2);
    Shim_XcSHAFinal(&ctx, innerDigest);

    Shim_XcSHAInit(&ctx);
    Shim_XcSHAUpdate(&ctx, k_opad, 64);
    Shim_XcSHAUpdate(&ctx, innerDigest, 20);
    Shim_XcSHAFinal(&ctx, digest);
}
static uint32_t __stdcall Shim_XeLoadSection(void* sec) { (void)sec; return 0; }
static uint32_t __stdcall Shim_XeUnloadSection(void* sec) { (void)sec; return 0; }
static uint32_t __stdcall Shim_NtFreeVirtualMemory(void** baseAddress, size_t* regionSize, uint32_t freeType) {
    if (baseAddress && *baseAddress) {
        VirtualFree(*baseAddress, (freeType == MEM_RELEASE) ? 0 : (regionSize ? *regionSize : 0), freeType);
        *baseAddress = NULL;
    }
    return STATUS_SUCCESS;
}
static uint32_t g_IdexChannelObject = 0;
static uint8_t __stdcall Shim_KeInsertQueueDpc(void* dpc, void* sysArg1, void* sysArg2) { (void)dpc; (void)sysArg1; (void)sysArg2; return 1; }
static void __stdcall Shim_MmLockUnlockPhysicalPage(uint32_t page, uint8_t lock) { (void)page; (void)lock; }
static uint32_t __stdcall Shim_HalReadWritePCISpace(uint32_t bus, uint32_t slot, uint32_t reg, void* buf, uint32_t len, uint8_t write) { (void)bus; (void)slot; (void)reg; (void)buf; (void)len; (void)write; return 0; }
static void __stdcall Shim_KeDisconnectInterrupt(void* interrupt) { (void)interrupt; }
static int __cdecl Shim_DbgPrint(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    logFmt("[DbgPrint] %s\n", buf);
    return 0;
}
static uint32_t __stdcall Shim_KeSaveFloatingPointState(void* state) { (void)state; return 0; }
static uint32_t __stdcall Shim_KeRestoreFloatingPointState(void* state) { (void)state; return 0; }
static uint8_t __stdcall Shim_KeRemoveQueueDpc(void* dpc) { (void)dpc; return 1; }
static void __stdcall Shim_KeBugCheck(uint32_t code) {
    void** pEsp = (void**)&code - 1;
    printf("[XboxKrnl] KeBugCheck: 0x%08X (Caller: %p)\n", code, pEsp ? pEsp[0] : NULL);
    for (int i = 0; i < 32 && pEsp; i++) {
        printf("  Stack[%d]: %p\n", i, pEsp[i]);
    }
    ExitProcess(code);
}
static void __stdcall Shim_RtlUnwind(void* targetFrame, void* targetIp, void* exceptionRecord, void* returnVal) { (void)targetFrame; (void)targetIp; (void)exceptionRecord; (void)returnVal; }

static const XBOX_THUNK_MAP g_ThunkTableByIndex[120] = {
    { 0, "KfLowerIrql", (void*)Shim_KfLowerIrql },
    { 1, "KeRaiseIrqlToDpcLevel", (void*)Shim_KeRaiseIrqlToDpcLevel },
    { 2, "RtlTimeToTimeFields", (void*)Shim_RtlTimeToTimeFields },
    { 3, "KeQuerySystemTime", (void*)Shim_KeQuerySystemTime },
    { 4, "RtlTimeFieldsToTime", (void*)Shim_RtlTimeFieldsToTime },
    { 5, "ExQueryNonVolatileSetting", (void*)Shim_ExQueryNonVolatileSetting },
    { 6, "RtlNtStatusToDosError", (void*)Shim_RtlNtStatusToDosError },
    { 7, "NtClose", (void*)Shim_NtClose },
    { 8, "NtSetInformationFile", (void*)Shim_NtSetInformationFile },
    { 9, "MmPersistContiguousMemory", (void*)Shim_MmPersistContiguousMemory },
    { 10, "MmAllocateContiguousMemory", (void*)Shim_MmAllocateContiguousMemory },
    { 11, "LaunchDataPage", (void*)&g_LaunchDataPage },
    { 12, "MmFreeContiguousMemory", (void*)Shim_MmFreeContiguousMemory },
    { 13, "HalReturnToFirmware", (void*)Shim_HalReturnToFirmware },
    { 14, "IoCreateDevice", (void*)Shim_IoCreateDevice },
    { 15, "RtlInitAnsiString", (void*)Shim_RtlInitAnsiString },
    { 16, "KeSetEvent", (void*)Shim_KeSetEvent },
    { 17, "KeWaitForSingleObject", (void*)Shim_KeWaitForSingleObject },
    { 18, "KeInitializeTimerEx", (void*)Shim_KeInitializeTimerEx },
    { 19, "KeInitializeDpc", (void*)Shim_KeInitializeDpc },
    { 20, "IoInvalidDeviceRequest", (void*)Shim_IoInvalidDeviceRequest },
    { 21, "MmAllocateContiguousMemoryEx", (void*)Shim_MmAllocateContiguousMemoryEx },
    { 22, "MmQueryAllocationSize", (void*)Shim_MmQueryAllocationSize },
    { 23, "MmSetAddressProtect", (void*)Shim_MmSetAddressProtect },
    { 24, "MmQueryAddressProtect", (void*)Shim_MmQueryAddressProtect },
    { 25, "NtCreateFile", (void*)Shim_NtCreateFile },
    { 26, "NtWriteFile", (void*)Shim_NtWriteFile },
    { 27, "NtReadFile", (void*)Shim_NtReadFile },
    { 28, "NtQueryInformationFile", (void*)Shim_NtQueryInformationFile },
    { 29, "NtOpenFile", (void*)Shim_NtOpenFile },
    { 30, "NtWaitForSingleObject", (void*)Shim_NtWaitForSingleObject },
    { 31, "NtQueryVolumeInformationFile", (void*)Shim_NtQueryVolumeInformationFile },
    { 32, "NtFlushBuffersFile", (void*)Shim_NtFlushBuffersFile },
    { 33, "NtUserIoApcDispatcher", (void*)Shim_NtUserIoApcDispatcher },
    { 34, "NtFsControlFile", (void*)Shim_NtFsControlFile },
    { 35, "NtDeviceIoControlFile", (void*)Shim_NtDeviceIoControlFile },
    { 36, "ObfDereferenceObject", (void*)Shim_ObfDereferenceObject },
    { 37, "ObReferenceObjectByHandle", (void*)Shim_ObReferenceObjectByHandle },
    { 38, "HalDiskCachePartitionCount", (void*)&g_HalDiskCachePartitionCount },
    { 39, "IoCreateSymbolicLink", (void*)Shim_IoCreateSymbolicLink },
    { 40, "NtQuerySymbolicLinkObject", (void*)Shim_NtQuerySymbolicLinkObject },
    { 41, "NtOpenSymbolicLinkObject", (void*)Shim_NtOpenSymbolicLinkObject },
    { 42, "IoDeleteSymbolicLink", (void*)Shim_IoDeleteSymbolicLink },
    { 43, "IoDismountVolumeByName", (void*)Shim_IoDismountVolumeByName },
    { 44, "RtlLeaveCriticalSection", (void*)Shim_RtlLeaveCriticalSection },
    { 45, "RtlEnterCriticalSection", (void*)Shim_RtlEnterCriticalSection },
    { 46, "XcSHAUpdate", (void*)Shim_XcSHAUpdate },
    { 47, "XcSHAInit", (void*)Shim_XcSHAInit },
    { 48, "XcSHAFinal", (void*)Shim_XcSHAFinal },
    { 49, "XboxHDKey", (void*)&g_XboxHDKey },
    { 50, "XboxSignatureKey", (void*)&g_XboxSignatureKey },
    { 51, "XboxAlternateSignatureKeys", (void*)&g_XboxAlternateSignatureKeys },
    { 52, "KeSetTimer", (void*)Shim_KeSetTimer },
    { 53, "KeCancelTimer", (void*)Shim_KeCancelTimer },
    { 54, "ExEventObjectType", (void*)&g_ExEventObjectType },
    { 55, "XboxHardwareInfo", (void*)&g_XboxHardwareInfo },
    { 56, "NtCreateEvent", (void*)Shim_NtCreateEvent },
    { 57, "NtSetEvent", (void*)Shim_NtSetEvent },
    { 58, "NtWaitForSingleObjectEx", (void*)Shim_NtWaitForSingleObjectEx },
    { 59, "KeDelayExecutionThread", (void*)Shim_KeDelayExecutionThread },
    { 60, "XeImageFileName", (void*)(void*)g_XeImageFileName },
    { 61, "IoStartPacket", (void*)Shim_IoStartPacket },
    { 62, "IofCompleteRequest", (void*)Shim_IofCompleteRequest },
    { 63, "IoStartNextPacket", (void*)Shim_IoStartNextPacket },
    { 64, "ExFreePool", (void*)Shim_ExFreePool },
    { 65, "ExAllocatePoolWithTag", (void*)Shim_ExAllocatePoolWithTag },
    { 66, "IoMarkIrpMustComplete", (void*)Shim_IoMarkIrpMustComplete },
    { 67, "HalIsResetOrShutdownPending", (void*)Shim_HalIsResetOrShutdownPending },
    { 68, "HalBootSMCVideoMode", (void*)&g_HalBootSMCVideoMode },
    { 69, "NtQueryDirectoryFile", (void*)Shim_NtQueryDirectoryFile },
    { 70, "XcVerifyPKCS1Signature", (void*)Shim_XcVerifyPKCS1Signature },
    { 71, "XePublicKeyData", (void*)&g_XePublicKeyData },
    { 72, "HalRegisterShutdownNotification", (void*)Shim_HalRegisterShutdownNotification },
    { 73, "XboxKrnlVersion", (void*)&g_XboxKrnlVersion },
    { 74, "KeSetBasePriorityThread", (void*)Shim_KeSetBasePriorityThread },
    { 75, "PsThreadObjectType", (void*)&g_PsThreadObjectType },
    { 76, "KeQueryBasePriorityThread", (void*)Shim_KeQueryBasePriorityThread },
    { 77, "KeSetDisableBoostThread", (void*)Shim_KeSetDisableBoostThread },
    { 78, "NtSuspendThread", (void*)Shim_NtSuspendThread },
    { 79, "NtResumeThread", (void*)Shim_NtResumeThread },
    { 80, "RtlRaiseException", (void*)Shim_RtlRaiseException },
    { 81, "NtYieldExecution", (void*)Shim_NtYieldExecution },
    { 82, "PsTerminateSystemThread", (void*)Shim_PsTerminateSystemThread },
    { 83, "PsCreateSystemThreadEx", (void*)Shim_PsCreateSystemThreadEx },
    { 84, "HalInitiateShutdown", (void*)Shim_HalInitiateShutdown },
    { 85, "HalGetInterruptVector", (void*)Shim_HalGetInterruptVector },
    { 86, "KfRaiseIrql", (void*)Shim_KfRaiseIrql },
    { 87, "KeConnectInterrupt", (void*)Shim_KeConnectInterrupt },
    { 88, "KeInitializeInterrupt", (void*)Shim_KeInitializeInterrupt },
    { 89, "KeStallExecutionProcessor", (void*)Shim_KeStallExecutionProcessor },
    { 90, "XcHMAC", (void*)Shim_XcHMAC },
    { 91, "RtlEqualString", (void*)Shim_RtlEqualString },
    { 92, "XeLoadSection", (void*)Shim_XeLoadSection },
    { 93, "XeUnloadSection", (void*)Shim_XeUnloadSection },
    { 94, "NtFreeVirtualMemory", (void*)Shim_NtFreeVirtualMemory },
    { 95, "NtAllocateVirtualMemory", (void*)Shim_NtAllocateVirtualMemory },
    { 96, "RtlCompareMemoryUlong", (void*)Shim_RtlCompareMemoryUlong },
    { 97, "RtlInitializeCriticalSection", (void*)Shim_RtlInitializeCriticalSection },
    { 98, "NtQueryVirtualMemory", (void*)Shim_NtQueryVirtualMemory },
    { 99, "IdexChannelObject", (void*)&g_IdexChannelObject },
    { 100, "MmGetPhysicalAddress", (void*)Shim_MmGetPhysicalAddress },
    { 101, "MmLockUnlockBufferPages", (void*)Shim_MmLockUnlockBufferPages },
    { 102, "KeInsertQueueDpc", (void*)Shim_KeInsertQueueDpc },
    { 103, "MmLockUnlockPhysicalPage", (void*)Shim_MmLockUnlockPhysicalPage },
    { 104, "NtQueryFullAttributesFile", (void*)Shim_NtQueryFullAttributesFile },
    { 105, "AvGetSavedDataAddress", (void*)Shim_AvGetSavedDataAddress },
    { 106, "AvSendTVEncoderOption", (void*)Shim_AvSendTVEncoderOption },
    { 107, "AvSetDisplayMode", (void*)Shim_AvSetDisplayMode },
    { 108, "AvSetSavedDataAddress", (void*)Shim_AvSetSavedDataAddress },
    { 109, "HalReadWritePCISpace", (void*)Shim_HalReadWritePCISpace },
    { 110, "KeDisconnectInterrupt", (void*)Shim_KeDisconnectInterrupt },
    { 111, "DbgPrint", (void*)Shim_DbgPrint },
    { 112, "MmClaimGpuInstanceMemory", (void*)Shim_MmClaimGpuInstanceMemory },
    { 113, "KeSaveFloatingPointState", (void*)Shim_KeSaveFloatingPointState },
    { 114, "KeRestoreFloatingPointState", (void*)Shim_KeRestoreFloatingPointState },
    { 115, "ExQueryPoolBlockSize", (void*)Shim_ExQueryPoolBlockSize },
    { 116, "KeRemoveQueueDpc", (void*)Shim_KeRemoveQueueDpc },
    { 117, "KeSynchronizeExecution", (void*)Shim_KeSynchronizeExecution },
    { 118, "KeBugCheck", (void*)Shim_KeBugCheck },
    { 119, "RtlUnwind", (void*)Shim_RtlUnwind },
};


void* XboxKrnl_GetThunkHandlerByIndex(uint32_t index) {
    if (index < 120) {
        return g_ThunkTableByIndex[index].handler;
    }
    printf("[XBOXKRNL] Warning: Out of bounds thunk index %u requested, returning stub\n", index);
    return (void*)Shim_DefaultStub;
}

const char* XboxKrnl_GetThunkNameByIndex(uint32_t index) {
    if (index < 120) {
        return g_ThunkTableByIndex[index].name;
    }
    return "Unknown";
}

void XboxKrnl_Init(void) {
    InitializeCriticalSection(&g_MemoryLock);
}

void* XboxKrnl_GetProcAddressByOrdinal(uint32_t ordinal) {
    for (size_t i = 0; i < 120; i++) {
        if (g_ThunkTableByIndex[i].ordinal == ordinal) {
            return g_ThunkTableByIndex[i].handler;
        }
    }
    return (void*)Shim_DefaultStub;
}
