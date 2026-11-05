#include "mod_engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <map>

// Xbox SC2 NTSC-U OLK File System Addresses
static const uint32_t PTR_XBOX_TOC_BASE     = 0x003766E0; // DAT_003766e0 (OLK TOC Array, stride 0x18)
static const uint32_t ADDR_OLK_PROCESS_PKT  = 0x00020750; // FUN_00020750 (Processes active I/O packet)
static const uint32_t ADDR_OLK_ENQUEUE_PKT  = 0x00020A00; // FUN_00020a00 (Allocates/enqueues I/O packet)
static const uint32_t ADDR_OLK_QUEUE_READ   = 0x00020ED0; // FUN_00020ed0 (Calculates OLK offset/size & queues read)
static const uint32_t ADDR_OLK_GET_FILESIZE = 0x00020F80; // FUN_00020f80 (Returns file byte size from TOC)

struct OlkContainerDef {
    uint16_t internalSubOlk;
    const char* primaryFolder;
    const char* alias1;
    const char* alias2;
};

static const OlkContainerDef kOlkContainers[6] = {
    { 0, "1_root",    "root",    "root.olk"    },
    { 1, "2_motinfo", "motinfo", "motinfo.olk" },
    { 2, "3_cpudata", "cpudata", "cpudata.olk" },
    { 3, "4_cdata",   "cdata",   "cdata.olk"   },
    { 4, "5_stage",   "stage",   "stage.olk"   },
    { 5, "6_human",   "human",   "human.olk"   },
};

struct ModAsset {
    uint16_t internalSubOlk;
    uint16_t internalFileNum;
    uint16_t userSubOlk;
    uint16_t userFileNum;
    std::string filePath;
    std::vector<uint8_t> data;
    FILETIME lastWriteTime;
    uint32_t vanillaSize;
    bool tocPatched;
};

static std::string s_gameDir;
static BOOL s_logAllLoads = FALSE;
static std::map<uint32_t, ModAsset> s_modRegistry; // Key: (uint32_t(subOlk) << 16) | uint32_t(fileNum)
static DWORD s_lastRefreshTick = 0;

static inline uint32_t MakeModKey(uint16_t subOlk, uint16_t fileNum) {
    return ((uint32_t)subOlk << 16) | (uint32_t)fileNum;
}

static const char* GetContainerName(uint16_t subOlk) {
    if (subOlk < 6) {
        return kOlkContainers[subOlk].primaryFolder;
    }
    return "unknown";
}

static bool IsValidXboxRam(uint32_t addr) {
    return addr >= 0x00010000 && addr < 0x40000000;
}

static bool ReadFileBytes(const char* path, std::vector<uint8_t>& outData, FILETIME& outWriteTime) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        return false;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        return false;
    }
    outData.resize(fileSize);
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, outData.data(), fileSize, &bytesRead, NULL);
    CloseHandle(hFile);
    if (!ok || bytesRead != fileSize) {
        return false;
    }
    outWriteTime = fad.ftLastWriteTime;
    return true;
}

static bool ExtractFileNumber(const char* fileName, uint16_t& outUserFileNum) {
    // Extract digits from filename stem (before the last '.')
    const char* dot = strrchr(fileName, '.');
    size_t stemLen = dot ? (size_t)(dot - fileName) : strlen(fileName);
    uint32_t val = 0;
    bool hasDigits = false;
    for (size_t i = 0; i < stemLen; i++) {
        unsigned char c = (unsigned char)fileName[i];
        if (isdigit(c)) {
            hasDigits = true;
            val = val * 10 + (c - '0');
            if (val > 65535) return false;
        }
    }
    if (!hasDigits) return false;
    outUserFileNum = (uint16_t)val;
    return true;
}

static void EnsureModDirectories(void) {
    if (s_gameDir.empty()) return;

    // Check if mods_xbox already exists; otherwise create standard mods/ directory structure
    std::string modsXboxRoot = s_gameDir + "\\mods_xbox";
    std::string modsRoot = s_gameDir + "\\mods";

    DWORD attrXbox = GetFileAttributesA(modsXboxRoot.c_str());
    const std::string& targetRoot = (attrXbox != INVALID_FILE_ATTRIBUTES && (attrXbox & FILE_ATTRIBUTE_DIRECTORY))
        ? modsXboxRoot
        : modsRoot;

    CreateDirectoryA(targetRoot.c_str(), NULL);
    for (int i = 0; i < 6; i++) {
        std::string subDir = targetRoot + "\\" + kOlkContainers[i].primaryFolder;
        CreateDirectoryA(subDir.c_str(), NULL);
    }
}

static void ScanModRoot(const std::string& rootDir) {
    DWORD rootAttr = GetFileAttributesA(rootDir.c_str());
    if (rootAttr == INVALID_FILE_ATTRIBUTES || !(rootAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    for (int i = 0; i < 6; i++) {
        uint16_t subOlk = kOlkContainers[i].internalSubOlk;
        const char* aliases[3] = {
            kOlkContainers[i].primaryFolder,
            kOlkContainers[i].alias1,
            kOlkContainers[i].alias2
        };

        for (int a = 0; a < 3; a++) {
            std::string folderPath = rootDir + "\\" + aliases[a];
            DWORD folderAttr = GetFileAttributesA(folderPath.c_str());
            if (folderAttr == INVALID_FILE_ATTRIBUTES || !(folderAttr & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }

            std::string searchPattern = folderPath + "\\*";
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE) {
                continue;
            }

            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                const char* fname = fd.cFileName;
                if (fname[0] == '.') {
                    continue;
                }
                size_t len = strlen(fname);
                if (len >= 4 && _stricmp(fname + len - 4, ".txt") == 0) {
                    continue;
                }

                uint16_t userFileNum = 0;
                if (!ExtractFileNumber(fname, userFileNum)) {
                    continue;
                }

                uint16_t internalFileNum = (userFileNum >= 1) ? (userFileNum - 1) : 0;
                uint32_t key = MakeModKey(subOlk, internalFileNum);
                std::string fullPath = folderPath + "\\" + fname;

                auto it = s_modRegistry.find(key);
                if (it != s_modRegistry.end()) {
                    ModAsset& existing = it->second;
                    if (CompareFileTime(&existing.lastWriteTime, &fd.ftLastWriteTime) != 0 ||
                        existing.filePath != fullPath) {
                        std::vector<uint8_t> newData;
                        FILETIME newTime;
                        if (ReadFileBytes(fullPath.c_str(), newData, newTime)) {
                            existing.filePath = fullPath;
                            existing.data.swap(newData);
                            existing.lastWriteTime = newTime;
                            existing.tocPatched = false;
                            printf("[MOD] Hot-Reloaded: Container %u (%s) | File #%u [LL%u:%03u] -> %s (%zu bytes)\n",
                                   existing.userSubOlk, GetContainerName(subOlk), existing.userFileNum,
                                   subOlk, internalFileNum, fname, existing.data.size());
                        }
                    }
                } else {
                    std::vector<uint8_t> data;
                    FILETIME writeTime;
                    if (ReadFileBytes(fullPath.c_str(), data, writeTime)) {
                        ModAsset asset;
                        asset.internalSubOlk = subOlk;
                        asset.internalFileNum = internalFileNum;
                        asset.userSubOlk = subOlk + 1;
                        asset.userFileNum = internalFileNum + 1;
                        asset.filePath = fullPath;
                        asset.data.swap(data);
                        asset.lastWriteTime = writeTime;
                        asset.vanillaSize = 0;
                        asset.tocPatched = false;
                        s_modRegistry[key] = std::move(asset);
                    }
                }
            } while (FindNextFileA(hFind, &fd));

            FindClose(hFind);
        }
    }
}

static void RefreshCatalog(void) {
    if (s_gameDir.empty()) return;
    ScanModRoot(s_gameDir + "\\mods");
    ScanModRoot(s_gameDir + "\\mods_xbox");
}

static void SyncInRamToc(void) {
    for (auto& kv : s_modRegistry) {
        ModAsset& asset = kv.second;
        uint32_t tocEntryPtr = PTR_XBOX_TOC_BASE + ((uint32_t)asset.internalSubOlk * 0x18);
        uint32_t tocBase = *(uint32_t*)tocEntryPtr;

        if (!IsValidXboxRam(tocBase)) {
            // Container TOC not loaded yet or was reset
            asset.tocPatched = false;
            continue;
        }

        // Verify OLK header magic ("olnk" = 0x6B6E6C6F at tocBase + 4) and entry bounds
        uint32_t fileCount = *(uint32_t*)tocBase;
        uint32_t magic = *(uint32_t*)(tocBase + 4);
        if (magic != 0x6B6E6C6F || asset.internalFileNum >= fileCount) {
            continue;
        }

        uint32_t sizeAddr = tocBase + (((uint32_t)asset.internalFileNum + 2) * 16) + 4;
        if (!IsValidXboxRam(sizeAddr)) {
            continue;
        }

        uint32_t currentTocSize = *(uint32_t*)sizeAddr;
        if (currentTocSize == 0) {
            continue;
        }

        if (asset.vanillaSize == 0) {
            asset.vanillaSize = currentTocSize;
        }

        uint32_t modSize = (uint32_t)asset.data.size();
        uint32_t targetSize = (modSize > asset.vanillaSize) ? modSize : asset.vanillaSize;

        if (!asset.tocPatched || currentTocSize != targetSize) {
            if (modSize > asset.vanillaSize) {
                *(uint32_t*)sizeAddr = modSize;
                printf("[MOD] TOC EXPANDED: Container %u (%s) | File #%u | Vanilla: %u B -> Mod: %u B (TOC @ 0x%08X)\n",
                       asset.userSubOlk, GetContainerName(asset.internalSubOlk), asset.userFileNum,
                       asset.vanillaSize, modSize, sizeAddr);
            } else if (!asset.tocPatched) {
                if (currentTocSize != asset.vanillaSize) {
                    *(uint32_t*)sizeAddr = asset.vanillaSize;
                }
                printf("[MOD] TOC VERIFIED: Container %u (%s) | File #%u | Vanilla: %u B (Payload fits: %u B)\n",
                       asset.userSubOlk, GetContainerName(asset.internalSubOlk), asset.userFileNum,
                       asset.vanillaSize, modSize);
            }
            asset.tocPatched = true;
        }
    }
}

static void RefreshAndSyncIfNeeded(bool forceRefresh) {
    DWORD now = GetTickCount();
    if (forceRefresh || (now - s_lastRefreshTick >= 1000)) {
        s_lastRefreshTick = now;
        RefreshCatalog();
    }
    if (!s_modRegistry.empty()) {
        SyncInRamToc();
    }
}

// ============================================================================
// HLE Hooks into Xbox SC2 OLK Engine
// ============================================================================

// Hook 1: FUN_00020f80 (Olk_GetFileSize)
static uint32_t __cdecl Hook_Olk_GetFileSize(uint32_t param_1, int sub_olk, int file_num) {
    (void)param_1;
    RefreshAndSyncIfNeeded(false);

    uint32_t tocBase = *(uint32_t*)(PTR_XBOX_TOC_BASE + sub_olk * 0x18);
    if (!IsValidXboxRam(tocBase)) {
        return 0;
    }
    if (file_num == -1) {
        return *(uint32_t*)(tocBase + 0x14);
    }

    uint32_t tocSize = *(uint32_t*)(tocBase + file_num * 0x10 + 0x24);
    if (sub_olk >= 0 && sub_olk < 6 && file_num >= 0) {
        auto it = s_modRegistry.find(MakeModKey((uint16_t)sub_olk, (uint16_t)file_num));
        if (it != s_modRegistry.end()) {
            uint32_t modSize = (uint32_t)it->second.data.size();
            if (modSize > tocSize) {
                tocSize = modSize;
            }
        }
    }
    return tocSize;
}

// Hook 2: FUN_00020ed0 (Olk_QueueFileRead)
typedef int (__cdecl *OlkEnqueueFn)(uint32_t handle, uint32_t cmd, uint32_t diskOffset, uint32_t readSize,
                                    uint32_t destBuf, uint32_t complPtr, uint32_t p7, uint32_t p8,
                                    uint32_t p9, uint16_t p10, uint16_t subOlk, uint16_t fileNum);

static void __cdecl Hook_Olk_QueueFileRead(uint32_t param_1, int sub_olk, int file_num, int offset, uint32_t read_size,
                                           uint32_t dest_buf, uint32_t compl_ptr, uint32_t param_8, uint32_t param_9,
                                           uint32_t param_10, uint32_t param_11) {
    (void)param_1; (void)param_10;
    RefreshAndSyncIfNeeded(false);

    uint32_t handle = *(uint32_t*)(0x003766DC + sub_olk * 0x18);
    uint32_t tocBase = *(uint32_t*)(PTR_XBOX_TOC_BASE + sub_olk * 0x18);
    uint32_t diskOffset = 0;

    if (file_num == -1) {
        if (read_size == 0) {
            read_size = ((*(uint32_t*)(tocBase + 0x14) - (uint32_t)offset) + 0x7FFu) & 0xFFFFF800u;
        }
        diskOffset = (uint32_t)offset + *(uint32_t*)(tocBase + 0x10);
    } else {
        if (read_size == 0) {
            uint32_t entrySize = *(uint32_t*)(tocBase + (file_num + 2) * 0x10 + 4);
            if (sub_olk >= 0 && sub_olk < 6 && file_num >= 0) {
                auto it = s_modRegistry.find(MakeModKey((uint16_t)sub_olk, (uint16_t)file_num));
                if (it != s_modRegistry.end()) {
                    uint32_t modSize = (uint32_t)it->second.data.size();
                    if (modSize > entrySize) entrySize = modSize;
                }
            }
            read_size = ((entrySize - (uint32_t)offset) + 0x7FFu) & 0xFFFFF800u;
        }
        diskOffset = (uint32_t)offset + *(uint32_t*)(tocBase + (file_num + 2) * 0x10) + *(uint32_t*)(tocBase + 0x10);
        if (read_size == 0) {
            read_size = 0x800; // Prevent zero-length spinloop
        }
    }

    OlkEnqueueFn pfnEnqueue = (OlkEnqueueFn)ADDR_OLK_ENQUEUE_PKT;
    pfnEnqueue(handle, 1, diskOffset, read_size, dest_buf, compl_ptr, param_8, param_9, param_11,
               0, (uint16_t)sub_olk, (uint16_t)file_num);
}

// Hook 3: FUN_00020750 (Olk_ProcessIoPacket)
static void* s_trampoline_OlkProcessPkt = NULL;
typedef uint32_t (__cdecl *OlkProcessPktFn)(void* queue, uint32_t pkt);

static uint32_t __cdecl Hook_Olk_ProcessIoPacket(void* queue, uint32_t pkt) {
    if (!IsValidXboxRam(pkt)) {
        return ((OlkProcessPktFn)s_trampoline_OlkProcessPkt)(queue, pkt);
    }

    uint16_t cmd      = *(uint16_t*)(pkt + 0x10);
    uint32_t diskOff  = *(uint32_t*)(pkt + 0x14);
    uint32_t origSize = *(uint32_t*)(pkt + 0x18);
    uint32_t destBuf  = *(uint32_t*)(pkt + 0x1C);
    uint32_t complPtr = *(uint32_t*)(pkt + 0x20);
    int16_t  subOlk   = *(int16_t*)(pkt + 0x3E);
    int16_t  fileNum  = *(int16_t*)(pkt + 0x40);

    // Check if this is an OLK sub-file read (cmd == 1) with a registered mod replacement
    if (cmd == 1 && subOlk >= 0 && subOlk < 6 && fileNum >= 0 && IsValidXboxRam(destBuf)) {
        uint32_t key = MakeModKey((uint16_t)subOlk, (uint16_t)fileNum);
        auto it = s_modRegistry.find(key);
        if (it != s_modRegistry.end()) {
            ModAsset& asset = it->second;

            // Check if the file on disk was modified since we cached it (instant hot-reload)
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(asset.filePath.c_str(), GetFileExInfoStandard, &fad) &&
                CompareFileTime(&asset.lastWriteTime, &fad.ftLastWriteTime) != 0) {
                std::vector<uint8_t> newData;
                FILETIME newTime;
                if (ReadFileBytes(asset.filePath.c_str(), newData, newTime)) {
                    asset.data.swap(newData);
                    asset.lastWriteTime = newTime;
                    asset.tocPatched = false;
                    SyncInRamToc();
                    printf("[MOD] Hot-Reloaded on read: Container %u (%s) | File #%u (%zu bytes)\n",
                           asset.userSubOlk, GetContainerName((uint16_t)subOlk), asset.userFileNum, asset.data.size());
                }
            }

            // Compute offset within sub-file (almost always 0)
            uint32_t offsetInFile = 0;
            uint32_t tocBase = *(uint32_t*)(PTR_XBOX_TOC_BASE + subOlk * 0x18);
            if (IsValidXboxRam(tocBase)) {
                uint32_t fileBase = *(uint32_t*)(tocBase + (fileNum + 2) * 0x10) + *(uint32_t*)(tocBase + 0x10);
                if (diskOff >= fileBase) {
                    offsetInFile = diskOff - fileBase;
                }
            }

            uint32_t modSize = (uint32_t)asset.data.size();
            uint32_t copyBytes = (offsetInFile < modSize) ? (modSize - offsetInFile) : 0;

            if (copyBytes > 0 && copyBytes <= origSize) {
                memcpy((void*)destBuf, asset.data.data() + offsetInFile, copyBytes);
                *(uint32_t*)(pkt + 0x18) = copyBytes;
                if (IsValidXboxRam(complPtr)) {
                    *(uint32_t*)(complPtr + 0x0C) = copyBytes;
                }
                printf("[MOD] INJECTED: Container %u (%s) | File #%u [Engine: LL%d:%03d] -> %u bytes into 0x%08X (Alloc: %u B)\n",
                       asset.userSubOlk, GetContainerName((uint16_t)subOlk), asset.userFileNum,
                       subOlk, fileNum, copyBytes, destBuf, origSize);
                return 1;
            } else {
                printf("[MOD] WARNING: Payload (%u B) larger than allocated buffer (%u B) for Container %u File #%u!\n",
                       copyBytes, origSize, asset.userSubOlk, asset.userFileNum);
            }
        }
    }

    uint32_t completed = ((OlkProcessPktFn)s_trampoline_OlkProcessPkt)(queue, pkt);
    if (completed) {
        // Re-sync TOC immediately whenever any I/O packet completes (catches container TOC header loads)
        if (!s_modRegistry.empty()) {
            SyncInRamToc();
        }

        if (s_logAllLoads && cmd == 1) {
            uint32_t finalSize = *(uint32_t*)(pkt + 0x18);
            if (subOlk < 0) {
                printf("[Disc I/O] [Raw DVD Sector]            | Sectors: %3u | Size: %8u B | Dest: 0x%08X\n",
                       finalSize / 2048, finalSize, destBuf);
            } else if (fileNum < 0) {
                printf("[Disc I/O] Container %-2d (%-10s) | [Container Header] | Size: %8u B | Dest: 0x%08X\n",
                       subOlk + 1, GetContainerName((uint16_t)subOlk), finalSize, destBuf);
            } else {
                printf("[I/O Load] Container %-2d (%-10s) | File #%-4d [Engine: LL%d:%03d] | Size: %8u B | Dest: 0x%08X\n",
                       subOlk + 1, GetContainerName((uint16_t)subOlk), fileNum + 1, subOlk, fileNum, finalSize, destBuf);
            }
        }
    }
    return completed;
}

extern "C" void ModEngine_Init(const char* gameDir, BOOL logAllFileLoads) {
    s_gameDir = gameDir ? gameDir : ".";
    s_logAllLoads = logAllFileLoads;
    s_modRegistry.clear();

    EnsureModDirectories();
    RefreshCatalog();
    s_lastRefreshTick = GetTickCount();

    printf("===================================================================\n");
    printf(" [MOD] NativeSC2 HLE OLK Mod Engine Initialized\n");
    printf(" [MOD] Watched Folders: %s\\mods & %s\\mods_xbox\n", s_gameDir.c_str(), s_gameDir.c_str());
    printf(" [MOD] Armed with %zu replacement payload(s) [1-Based Indexing]:\n", s_modRegistry.size());
    for (const auto& kv : s_modRegistry) {
        const ModAsset& asset = kv.second;
        printf("   [+] Container %u (%s) | File #%-3u [Engine: LL%u:%03u] -> %s (%zu bytes)\n",
               asset.userSubOlk,
               GetContainerName(asset.internalSubOlk),
               asset.userFileNum,
               asset.internalSubOlk,
               asset.internalFileNum,
               asset.filePath.c_str(),
               asset.data.size());
    }
    if (s_logAllLoads) {
        printf(" [MOD] Live I/O File Logging: ENABLED (--log-files)\n");
    }
    printf("===================================================================\n");
}

static void WriteJmp(uint32_t targetAddr, void* hookFn, size_t patchLen) {
    DWORD oldProtect;
    VirtualProtect((void*)targetAddr, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect);
    uint8_t buf[16];
    memset(buf, 0x90, sizeof(buf));
    buf[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)hookFn - (targetAddr + 5));
    memcpy(&buf[1], &rel, 4);
    memcpy((void*)targetAddr, buf, patchLen);
}

extern "C" void ModEngine_InstallHooks(void) {
    // 1. Hook FUN_00020f80 (Olk_GetFileSize)
    WriteJmp(ADDR_OLK_GET_FILESIZE, (void*)Hook_Olk_GetFileSize, 7);

    // 2. Hook FUN_00020ed0 (Olk_QueueFileRead)
    WriteJmp(ADDR_OLK_QUEUE_READ, (void*)Hook_Olk_QueueFileRead, 7);

    // 3. Trampoline Hook FUN_00020750 (Olk_ProcessIoPacket - first 6 bytes: 83 EC 08 53 55 56)
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 32, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    s_trampoline_OlkProcessPkt = tramp;
    memcpy(tramp, (void*)ADDR_OLK_PROCESS_PKT, 6);
    tramp[6] = 0xE9;
    int32_t relBack = (int32_t)((ADDR_OLK_PROCESS_PKT + 6) - ((uintptr_t)tramp + 11));
    memcpy(&tramp[7], &relBack, 4);

    WriteJmp(ADDR_OLK_PROCESS_PKT, (void*)Hook_Olk_ProcessIoPacket, 6);

    printf("[HLE_PATCH] OLK Mod Engine hooked @ 0x00020F80 (GetFileSize), 0x00020ED0 (QueueRead), 0x00020750 (ProcessPkt)\n");
}

extern "C" void ModEngine_ForceRefresh(void) {
    printf("[MOD] Manual hot-reload triggered (F5)...\n");
    RefreshAndSyncIfNeeded(true);
    printf("[MOD] Active replacement payloads: %zu\n", s_modRegistry.size());
}
