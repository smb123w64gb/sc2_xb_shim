#include "xbe.h"
#include "xboxkrnl.h"
#include "xapi_shim.h"
#include "nv2a_bridge.h"
#include "d3d9_backend.h"
#include "namco_renderer.h"
#include "cpu_profiler.h"
#include "mod_engine.h"

#define USE_D3D9_BACKEND 1
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>

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

extern "C" BOOL Xbe_LoadAndMap(const char* xbeFilePath, XBE_LOADED_IMAGE* outImage) {
    if (!xbeFilePath || !outImage) return FALSE;
    memset(outImage, 0, sizeof(XBE_LOADED_IMAGE));

    logFmt("[XBE_LOADER] Opening XBE file: %s\n", xbeFilePath);

    FILE* f = fopen(xbeFilePath, "rb");
    if (!f) {
        logFmt("[XBE_LOADER] Error: Could not open XBE file!\n");
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    outImage->FileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    outImage->FileData = (uint8_t*)malloc(outImage->FileSize);
    if (!outImage->FileData) {
        logFmt("[XBE_LOADER] Error: Out of memory reading XBE.\n");
        fclose(f);
        return FALSE;
    }

    if (fread(outImage->FileData, 1, outImage->FileSize, f) != outImage->FileSize) {
        logFmt("[XBE_LOADER] Error reading full XBE file.\n");
        free(outImage->FileData);
        fclose(f);
        return FALSE;
    }
    fclose(f);

    memcpy(&outImage->Header, outImage->FileData, sizeof(XBE_HEADER));
    if (outImage->Header.Magic != 0x48454258) { // 'XBEH'
        logFmt("[XBE_LOADER] Error: Invalid XBE Magic 0x%08X (expected 'XBEH')\n", outImage->Header.Magic);
        free(outImage->FileData);
        return FALSE;
    }

    logFmt("[XBE_LOADER] XBE Header Verified.\n");
    logFmt("  Image Base Address:   0x%08X\n", outImage->Header.BaseAddress);
    logFmt("  Size of Image:        0x%08X (%u KB)\n", outImage->Header.SizeOfImage, outImage->Header.SizeOfImage / 1024);
    logFmt("  Number of Sections:   %u\n", outImage->Header.NumberOfSections);

    DWORD oldProtect;
    logFmt("[XBE_LOADER] Unprotecting 0x00010000 - 0x08000000...\n");
    VirtualProtect((void*)0x00010000, 0x1000, PAGE_EXECUTE_READWRITE, &oldProtect);
    VirtualProtect((void*)0x00011000, 0x1000, PAGE_EXECUTE_READWRITE, &oldProtect);
    VirtualProtect((void*)0x00012000, 0x1000, PAGE_EXECUTE_READWRITE, &oldProtect);
    VirtualProtect((void*)0x00013000, 127 * 1024 * 1024, PAGE_EXECUTE_READWRITE, &oldProtect);

    uint8_t savedPeHeader[512];
    memcpy(savedPeHeader, (void*)0x00010000, sizeof(savedPeHeader));

    memcpy((void*)outImage->Header.BaseAddress, outImage->FileData, outImage->Header.SizeOfHeaders);
    memcpy((void*)outImage->Header.BaseAddress, savedPeHeader, 0x100);

    logFmt("[XBE_LOADER] Successfully mapped XBE Header at 0x%08X (Size 0x%08X, PE header preserved)\n",
           outImage->Header.BaseAddress, outImage->Header.SizeOfHeaders);

    uint32_t secHdrsOffset = outImage->Header.SectionHeadersAddress - outImage->Header.BaseAddress;
    PXBE_SECTION_HEADER secHdrs = (PXBE_SECTION_HEADER)(outImage->FileData + secHdrsOffset);

    for (uint32_t i = 0; i < outImage->Header.NumberOfSections; i++) {
        PXBE_SECTION_HEADER sec = &secHdrs[i];
        const char* secName = (const char*)(outImage->FileData + (sec->SectionNameAddress - outImage->Header.BaseAddress));
        void* mapped = (void*)(uintptr_t)sec->VirtualAddress;

        logFmt("[XBE_LOADER] Section [%02u] %-12s @ 0x%08X - 0x%08X (Size 0x%08X)\n",
               i, secName, sec->VirtualAddress, sec->VirtualAddress + sec->VirtualSize, sec->VirtualSize);

        VirtualProtect(mapped, sec->VirtualSize, PAGE_EXECUTE_READWRITE, &oldProtect);

        if (sec->RawSize > 0 && sec->RawAddress < outImage->FileSize) {
            size_t copyBytes = (sec->RawSize < sec->VirtualSize) ? sec->RawSize : sec->VirtualSize;
            memcpy(mapped, outImage->FileData + sec->RawAddress, copyBytes);
        }
        if (sec->VirtualSize > sec->RawSize) {
            memset((uint8_t*)mapped + sec->RawSize, 0, sec->VirtualSize - sec->RawSize);
        }
    }

    logFmt("[XBE_LOADER] Resolving 120 Kernel Thunks at 0x%08X...\n", SC2_THUNK_TABLE_ADDR);
    uint32_t* thunkTable = (uint32_t*)SC2_THUNK_TABLE_ADDR;
    VirtualProtect(thunkTable, SC2_THUNK_TABLE_COUNT * sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtect);

    for (uint32_t i = 0; i < SC2_THUNK_TABLE_COUNT; i++) {
        void* handler = XboxKrnl_GetThunkHandlerByIndex(i);
        thunkTable[i] = (uint32_t)(uintptr_t)handler;
    }
    logFmt("[XBE_LOADER] Kernel thunks successfully patched.\n");

    return TRUE;
}

static void Hook_D3DDevice_Swap_Trampoline(void);
static void __stdcall Hook_D3DDevice_InsertCallback(int type, void (__cdecl *pfnCallback)(uint32_t), uint32_t arg);
static void InstallJmpHook(uint32_t targetAddr, void* hookFn);
void InstallHBoxHook(void);
void InstallSysTaskCreateHook(void);

extern "C" BOOL Xbe_ApplyHlePatches(void) {
    DWORD oldProtect;

    // Patch 1: FUN_0014fed0 (Raster / Scanline check in DrawFrame)
    const uint8_t patch_raster[] = {
        0x8B, 0x44, 0x24, 0x04,
        0xC7, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x40, 0x04, 0x0F, 0x27, 0x00, 0x00,
        0xC2, 0x04, 0x00
    };
    VirtualProtect((void*)0x0014FED0, sizeof(patch_raster), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0014FED0, patch_raster, sizeof(patch_raster));
    logFmt("[HLE_PATCH] Applied FUN_0014fed0 raster bypass @ 0x0014FED0\n");

    // Patch 2: FUN_00156c70 entry point restoration (Standard prologue)
    const uint8_t patch_156c70_entry[] = { 0x83, 0xEC, 0x0C };
    VirtualProtect((void*)0x00156C70, sizeof(patch_156c70_entry), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00156C70, patch_156c70_entry, sizeof(patch_156c70_entry));

    // Patch 3: FUN_00156c70 port 0x80C0 bypass
    const uint8_t patch_156c70_port[] = { 0xEB, 0x30 };
    VirtualProtect((void*)0x00156CE7, sizeof(patch_156c70_port), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00156CE7, patch_156c70_port, sizeof(patch_156c70_port));
    logFmt("[HLE_PATCH] Applied FUN_00156c70 port 0x80c0 bypass @ 0x00156CE7\n");

    // Patch 4: FUN_00156c70 PCRTC spinloop bypass
    const uint8_t patch_156c70_pcrtc[] = { 0x90, 0x90 };
    VirtualProtect((void*)0x00156D2C, sizeof(patch_156c70_pcrtc), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00156D2C, patch_156c70_pcrtc, sizeof(patch_156c70_pcrtc));
    logFmt("[HLE_PATCH] Applied FUN_00156c70 PCRTC spinloop bypass @ 0x00156D2C\n");

    // Patch 5: FUN_00156a70 (ISR) port 0x80C0 bypass
    const uint8_t patch_156a70_isr[] = { 0xEB, 0x76 };
    VirtualProtect((void*)0x00156AE6, sizeof(patch_156a70_isr), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00156AE6, patch_156a70_isr, sizeof(patch_156a70_isr));
    logFmt("[HLE_PATCH] Applied FUN_00156a70 ISR port 0x80c0 bypass @ 0x00156AE6\n");

    // Patch 6-8: WBINVD neutralizations
    const uint8_t patch_wbinvd1[] = { 0xC3, 0x90, 0x90 };
    VirtualProtect((void*)0x0002DAA0, sizeof(patch_wbinvd1), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0002DAA0, patch_wbinvd1, sizeof(patch_wbinvd1));

    const uint8_t patch_wbinvd2[] = { 0x90, 0x90 };
    VirtualProtect((void*)0x00025452, sizeof(patch_wbinvd2), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00025452, patch_wbinvd2, sizeof(patch_wbinvd2));

    const uint8_t patch_wbinvd3[] = { 0x90, 0x90 };
    VirtualProtect((void*)0x00155C34, sizeof(patch_wbinvd3), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00155C34, patch_wbinvd3, sizeof(patch_wbinvd3));
    logFmt("[HLE_PATCH] All WBINVD instructions safely neutralized.\n");

    // Patch 9: Neutralize Xbox FS:[0x20] profiler/tracer hooks
    const uint32_t fs20_vaddrs[] = {
        0x00103BDA, 0x001053B9, 0x00105BAC, 0x001070FD,
        0x0010924B, 0x001092BF, 0x00109B3E, 0x00109B6D,
        0x00155D21, 0x00156C21
    };
    const uint8_t patch_fs20[] = {
        0x31, 0xC0,
        0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    for (size_t i = 0; i < sizeof(fs20_vaddrs)/sizeof(fs20_vaddrs[0]); i++) {
        VirtualProtect((void*)fs20_vaddrs[i], sizeof(patch_fs20), PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy((void*)fs20_vaddrs[i], patch_fs20, sizeof(patch_fs20));
    }
    const uint8_t patch_fs20_ret[] = { 0x5E, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    VirtualProtect((void*)0x00106E9E, sizeof(patch_fs20_ret), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00106E9E, patch_fs20_ret, sizeof(patch_fs20_ret));
    logFmt("[HLE_PATCH] All Xbox FS:[0x20] hooks safely neutralized.\n");

    // Patch 10: APU Hardware Spinloop Bypass (FUN_00168855)
    const uint8_t patch_apu_spin[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x00168855, sizeof(patch_apu_spin), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00168855, patch_apu_spin, sizeof(patch_apu_spin));
    logFmt("[HLE_PATCH] APU pause/spinloop bypassed @ 0x00168855\n");

    // Patch 11: APU Hardware Init Bypass (FUN_00164130)
    const uint8_t patch_apu_hw_init[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x00164130, sizeof(patch_apu_hw_init), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00164130, patch_apu_hw_init, sizeof(patch_apu_hw_init));
    logFmt("[HLE_PATCH] APU hardware init bypassed @ 0x00164130 (returns S_OK)\n");

    // Patch 12: APU Mixer Hardware Setup Bypass (FUN_00162deb)
    const uint8_t patch_apu_mixer[] = { 0x31, 0xC0, 0xC2, 0x04, 0x00 };
    VirtualProtect((void*)0x00162DEB, sizeof(patch_apu_mixer), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00162DEB, patch_apu_mixer, sizeof(patch_apu_mixer));
    logFmt("[HLE_PATCH] APU mixer hardware setup bypassed @ 0x00162DEB (returns S_OK)\n");

    // Patch 13: CMiniport_InitHardware port 0x80C0 write bypass
    const uint8_t patch_port_write[] = { 0x90 };
    VirtualProtect((void*)0x00157C80, sizeof(patch_port_write), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00157C80, patch_port_write, sizeof(patch_port_write));
    logFmt("[HLE_PATCH] CMiniport_InitHardware port 0x80C0 write bypassed @ 0x00157C80\n");

    // Patch 14: CDevice_KickOff NV_PFIFO CACHE1 push spinloop bypass
    const uint8_t patch_kickoff_spin[] = { 0x90, 0x90 };
    VirtualProtect((void*)0x001525BA, sizeof(patch_kickoff_spin), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001525BA, patch_kickoff_spin, sizeof(patch_kickoff_spin));
    logFmt("[HLE_PATCH] CDevice_KickOff NV_PFIFO spinloop bypassed @ 0x001525BA\n");

    // Patch 15: FUN_00152630 NV_PFIFO CACHE1 push spinloop bypass
    const uint8_t patch_push_spin[] = { 0x90, 0x90 };
    VirtualProtect((void*)0x0015267A, sizeof(patch_push_spin), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0015267A, patch_push_spin, sizeof(patch_push_spin));
    logFmt("[HLE_PATCH] FUN_00152630 NV_PFIFO spinloop bypassed @ 0x0015267A\n");

    // Patch 16: D3DDevice_IsBusy bypass
    const uint8_t patch_isbusy[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x00150B20, sizeof(patch_isbusy), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00150B20, patch_isbusy, sizeof(patch_isbusy));
    logFmt("[HLE_PATCH] D3DDevice_IsBusy bypassed @ 0x00150B20 (returns false)\n");

    // Patch 17-18: Cursor wait bypasses
    const uint8_t patch_init_cursor[] = { 0xEB, 0x1A };
    VirtualProtect((void*)0x00155C50, sizeof(patch_init_cursor), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00155C50, patch_init_cursor, sizeof(patch_init_cursor));
    logFmt("[HLE_PATCH] FUN_001558e0 cursor wait bypassed @ 0x00155C50\n");

    const uint8_t patch_free_cursor[] = { 0xEB, 0x1B };
    VirtualProtect((void*)0x00155DDE, sizeof(patch_free_cursor), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00155DDE, patch_free_cursor, sizeof(patch_free_cursor));
    logFmt("[HLE_PATCH] FUN_00155da0 cursor wait bypassed @ 0x00155DDE\n");

    // Patch 19: D3D_BlockOnTime GPU fence bypass
    const uint8_t patch_block_fence[] = { 0xC2, 0x08, 0x00 };
    VirtualProtect((void*)0x00152750, sizeof(patch_block_fence), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00152750, patch_block_fence, sizeof(patch_block_fence));
    logFmt("[HLE_PATCH] D3D_BlockOnTime GPU fence bypassed @ 0x00152750\n");

    // Patch 20: CMiniport_IsFlipPending bypass
    const uint8_t patch_flip_pending[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x00156A40, sizeof(patch_flip_pending), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00156A40, patch_flip_pending, sizeof(patch_flip_pending));
    logFmt("[HLE_PATCH] CMiniport_IsFlipPending bypassed @ 0x00156A40 (returns false)\n");

    // Patch 21: FUN_00157750 W1C Register Clear Fix
    const uint8_t patch_pmc_clear[] = { 0x31, 0xDB, 0x89, 0x9E, 0x00, 0x02, 0x00, 0x00, 0x4B };
    VirtualProtect((void*)0x0015776C, sizeof(patch_pmc_clear), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0015776C, patch_pmc_clear, sizeof(patch_pmc_clear));

    const uint8_t patch_pgraph_clear[] = { 0x89, 0xAE, 0x00, 0x01, 0x40, 0x00 };
    VirtualProtect((void*)0x001577A3, sizeof(patch_pgraph_clear), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001577A3, patch_pgraph_clear, sizeof(patch_pgraph_clear));

    const uint8_t patch_pfifo_clear[] = { 0x89, 0xAE, 0x00, 0x21, 0x00, 0x00 };
    VirtualProtect((void*)0x001577B6, sizeof(patch_pfifo_clear), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001577B6, patch_pfifo_clear, sizeof(patch_pfifo_clear));
    logFmt("[HLE_PATCH] FUN_00157750 W1C interrupt clears redirected to write 0\n");

    // Patch 22: FUN_00157260 PGRAPH interrupt handler stub
    const uint8_t patch_pgraph_isr[] = {
        0x8B, 0x01,
        0xC7, 0x80, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x80, 0x20, 0x07, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x31, 0xC0,
        0xC3
    };
    VirtualProtect((void*)0x00157260, sizeof(patch_pgraph_isr), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00157260, patch_pgraph_isr, sizeof(patch_pgraph_isr));
    logFmt("[HLE_PATCH] FUN_00157260 PGRAPH interrupt handler stubbed @ 0x00157260\n");

    // Patch 23: FUN_0015b84f recursion bypass
    const uint8_t patch_b84f_skip[] = { 0xEB, 0x05 };
    VirtualProtect((void*)0x0015B862, sizeof(patch_b84f_skip), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0015B862, patch_b84f_skip, sizeof(patch_b84f_skip));
    logFmt("[HLE_PATCH] FUN_0015b84f interrupt recursion bypassed @ 0x0015B862\n");

    // Patch 24: FUN_001579a1 PFIFO channel sync wait bypass
    const uint8_t patch_channel_sync[] = {
        0x8B, 0x54, 0x24, 0x04,
        0x8B, 0x01,
        0x8B, 0x80, 0x20, 0x32, 0x00, 0x00,
        0x89, 0x02,
        0xC2, 0x04, 0x00
    };
    VirtualProtect((void*)0x001579A1, sizeof(patch_channel_sync), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001579A1, patch_channel_sync, sizeof(patch_channel_sync));
    logFmt("[HLE_PATCH] FUN_001579a1 PFIFO channel sync bypassed @ 0x001579A1\n");

    // Patch 25-26: GetLastError & SetLastError HLE stubs
    const uint8_t patch_getlasterror[] = { 0xA1, 0x24, 0x86, 0x60, 0x00, 0xC3 };
    VirtualProtect((void*)0x001039A3, sizeof(patch_getlasterror), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001039A3, patch_getlasterror, sizeof(patch_getlasterror));
    logFmt("[HLE_PATCH] GetLastError redirected to static storage @ 0x001039A3\n");

    const uint8_t patch_setlasterror[] = {
        0x8B, 0x44, 0x24, 0x04,
        0xA3, 0x24, 0x86, 0x60, 0x00,
        0xC2, 0x04, 0x00
    };
    VirtualProtect((void*)0x001039CB, sizeof(patch_setlasterror), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001039CB, patch_setlasterror, sizeof(patch_setlasterror));
    logFmt("[HLE_PATCH] SetLastError redirected to static storage @ 0x001039CB\n");

    // Patch 27: CMcpxAPU_ServiceDeferredCommandsLow bypass
    const uint8_t patch_dsound_deferred[] = { 0xC3 };
    VirtualProtect((void*)0x00163BD8, sizeof(patch_dsound_deferred), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00163BD8, patch_dsound_deferred, sizeof(patch_dsound_deferred));
    logFmt("[HLE_PATCH] CMcpxAPU_ServiceDeferredCommandsLow bypassed @ 0x00163BD8\n");

    // Patch 28: D3DDevice_Swap Presentation Hook
    uint8_t patch_swap[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    int32_t relSwap = (int32_t)((uintptr_t)Hook_D3DDevice_Swap_Trampoline - (0x00154240 + 5));
    memcpy(&patch_swap[1], &relSwap, 4);
    VirtualProtect((void*)0x00154240, sizeof(patch_swap), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00154240, patch_swap, sizeof(patch_swap));
    logFmt("[HLE_PATCH] D3DDevice_Swap hooked for Win32 presentation @ 0x00154240\n");

    // Patch 29: D3DDevice_BlockUntilVerticalBlank bypass
    const uint8_t patch_vblank[] = { 0xC3 };
    VirtualProtect((void*)0x0014FA10, sizeof(patch_vblank), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0014FA10, patch_vblank, sizeof(patch_vblank));
    logFmt("[HLE_PATCH] D3DDevice_BlockUntilVerticalBlank bypassed @ 0x0014FA10\n");

    // Patch 30: FUN_000327c0 GPU unfinished fence counter stub
    const uint8_t patch_fence_count[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x000327C0, sizeof(patch_fence_count), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x000327C0, patch_fence_count, sizeof(patch_fence_count));
    logFmt("[HLE_PATCH] FUN_000327c0 GPU unfinished fence count stubbed to 0 @ 0x000327C0\n");

    // Patch 31: D3DDevice_InsertCallback immediate callback hook
    uint8_t patch_insert_cb[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    int32_t relInsertCb = (int32_t)((uintptr_t)Hook_D3DDevice_InsertCallback - (0x001524D0 + 5));
    memcpy(&patch_insert_cb[1], &relInsertCb, 4);
    VirtualProtect((void*)0x001524D0, sizeof(patch_insert_cb), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001524D0, patch_insert_cb, sizeof(patch_insert_cb));
    logFmt("[HLE_PATCH] D3DDevice_InsertCallback hooked for immediate execution @ 0x001524D0\n");

    // Patch 32-38: XAPI Input Hooks
    InstallJmpHook(0x0016F649, (void*)Shim_XInitDevices);
    logFmt("[HLE_PATCH] XInitDevices hooked @ 0x0016F649\n");
    InstallJmpHook(0x0016F64E, (void*)Shim_XGetDevices);
    logFmt("[HLE_PATCH] XGetDevices hooked @ 0x0016F64E\n");
    InstallJmpHook(0x0016EE92, (void*)Shim_XInputOpen);
    logFmt("[HLE_PATCH] XInputOpen hooked @ 0x0016EE92\n");
    InstallJmpHook(0x0016EEE8, (void*)Shim_XInputClose);
    logFmt("[HLE_PATCH] XInputClose hooked @ 0x0016EEE8\n");
    InstallJmpHook(0x0016F0CC, (void*)Shim_XInputGetState);
    logFmt("[HLE_PATCH] XInputGetState hooked @ 0x0016F0CC\n");
    InstallJmpHook(0x0016F13F, (void*)Shim_XInputSetState);
    logFmt("[HLE_PATCH] XInputSetState hooked @ 0x0016F13F\n");
    InstallJmpHook(0x0016EEF4, (void*)Shim_XInputGetCapabilities);
    logFmt("[HLE_PATCH] XInputGetCapabilities hooked @ 0x0016EEF4\n");

    // Patch 39-42: Disc & CRI error bypasses
    const uint8_t patch_disc_auth[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x0010668F, sizeof(patch_disc_auth), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0010668F, patch_disc_auth, sizeof(patch_disc_auth));
    logFmt("[HLE_PATCH] FUN_0010668f retail disc authentication bypassed @ 0x0010668F\n");

    const uint8_t patch_cri_err[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x00128700, sizeof(patch_cri_err), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00128700, patch_cri_err, sizeof(patch_cri_err));
    logFmt("[HLE_PATCH] FUN_00128700 CRI error dispatcher bypassed @ 0x00128700\n");

    const uint8_t patch_cri_cb[] = { 0xC3 };
    VirtualProtect((void*)0x00128670, sizeof(patch_cri_cb), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00128670, patch_cri_cb, sizeof(patch_cri_cb));
    logFmt("[HLE_PATCH] FUN_00128670 CRI error callback neutralized @ 0x00128670\n");

    const uint8_t patch_cri_status[] = { 0x31, 0xC0, 0xC3 };
    VirtualProtect((void*)0x00128690, sizeof(patch_cri_status), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00128690, patch_cri_status, sizeof(patch_cri_status));
    logFmt("[HLE_PATCH] FUN_00128690 CRI status check bypassed @ 0x00128690\n");

    // Patch 43: FUN_0008e500 boot AutoLoad enabled (sets [PTR_DAT_0032fab0 + 0x40] = 1 on completion
    // so save/load UI prompts and AutoSave work properly)
    logFmt("[HLE_PATCH] FUN_0008e500 boot AutoLoad active @ 0x0008E500\n");

    // Patch 44: Diagnostic hooks
    InstallHBoxHook();
    InstallSysTaskCreateHook();

    // Patch 45-49: DiskErr bypasses
    const uint8_t patch_err_cb[] = { 0xC3 };
    VirtualProtect((void*)0x00020400, sizeof(patch_err_cb), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00020400, patch_err_cb, sizeof(patch_err_cb));
    logFmt("[HLE_PATCH] FUN_00020400 error trigger callback neutralized @ 0x00020400\n");

    const uint8_t patch_tray_ok[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    VirtualProtect((void*)0x0002FD00, sizeof(patch_tray_ok), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0002FD00, patch_tray_ok, sizeof(patch_tray_ok));
    logFmt("[HLE_PATCH] FUN_0002fd00 media/tray check stubbed to 1 (disc OK) @ 0x0002FD00\n");

    const uint8_t patch_diskerr_init[] = { 0xC3 };
    VirtualProtect((void*)0x00087220, sizeof(patch_diskerr_init), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00087220, patch_diskerr_init, sizeof(patch_diskerr_init));
    logFmt("[HLE_PATCH] FUN_00087220 DiskErr task creator neutralized @ 0x00087220\n");

    const uint8_t patch_diskerr_draw[] = { 0xC3 };
    VirtualProtect((void*)0x00086FF0, sizeof(patch_diskerr_draw), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00086FF0, patch_diskerr_draw, sizeof(patch_diskerr_draw));
    logFmt("[HLE_PATCH] FUN_00086ff0 DiskErr draw loop neutralized @ 0x00086FF0\n");

    const uint8_t patch_diskerr_mgr[] = {
        0xC7, 0x41, 0x14, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x41, 0x04, 0x00, 0x00, 0x00, 0x00,
        0xC3
    };
    VirtualProtect((void*)0x00087290, sizeof(patch_diskerr_mgr), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00087290, patch_diskerr_mgr, sizeof(patch_diskerr_mgr));
    logFmt("[HLE_PATCH] FUN_00087290 DiskErr manager loop neutralized @ 0x00087290\n");

    // Patch 50: FUN_00020ed0 zero-length archive entry assert bypass @ 0x00020F76
    const uint8_t patch_zerolen_archive[] = {
        0xB8, 0x00, 0x08, 0x00, 0x00,
        0xE9, 0x97, 0xFF, 0xFF, 0xFF
    };
    VirtualProtect((void*)0x00020F76, sizeof(patch_zerolen_archive), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00020F76, patch_zerolen_archive, sizeof(patch_zerolen_archive));
    logFmt("[HLE_PATCH] FUN_00020ed0 zero-length package entry spinloop bypassed @ 0x00020F76\n");

    // Patch 51: DrawFrame raster scanline spinloop skip @ 0x0002E796
    const uint8_t patch_raster_skip[] = { 0xEB, 0x2F };
    VirtualProtect((void*)0x0002E796, sizeof(patch_raster_skip), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0002E796, patch_raster_skip, sizeof(patch_raster_skip));
    logFmt("[HLE_PATCH] DrawFrame raster scanline spinloop bypassed @ 0x0002E796 (jmp 0x0002E7C7)\n");

    // Patch 53: FUN_00033fb0 out-of-bounds assert bypass @ 0x00033FC2
    const uint8_t patch_safe_33fc2[] = {
        0x8D, 0x42, 0xFF,
        0xEB, 0x02,
        0x90, 0x90
    };
    VirtualProtect((void*)0x00033FC2, sizeof(patch_safe_33fc2), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00033FC2, patch_safe_33fc2, sizeof(patch_safe_33fc2));
    logFmt("[HLE_PATCH] FUN_00033fb0 safe package lookup applied @ 0x00033FC2 (preserving negative index size calc)\n");

    // Patch 54: FUN_000f1ce0 safe package entry lookup @ 0x000F1CEC
    const uint8_t patch_safe_f1ce0[] = {
        0x8B, 0x01,
        0x48,
        0x85, 0xC0,
        0x79, 0x00
    };
    VirtualProtect((void*)0x000F1CEC, sizeof(patch_safe_f1ce0), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x000F1CEC, patch_safe_f1ce0, sizeof(patch_safe_f1ce0));
    logFmt("[HLE_PATCH] FUN_000f1ce0 safe package lookup applied @ 0x000F1CEC\n");

    // Patch 60-63: Strip 0x80000000 / 0xF0000000 physical aliases
    const uint8_t nops_6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    VirtualProtect((void*)0x00156289, sizeof(nops_6), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00156289, nops_6, sizeof(nops_6));
    logFmt("[HLE_PATCH] D3DResource_Lock2D 0x80000000 alias bypassed @ 0x00156289\n");

    VirtualProtect((void*)0x001564E8, sizeof(nops_6), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001564E8, nops_6, sizeof(nops_6));
    logFmt("[HLE_PATCH] Lock2DSurface 0xF0000000 alias bypassed @ 0x001564E8\n");

    const uint8_t nops_5[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    VirtualProtect((void*)0x00155F65, sizeof(nops_5), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00155F65, nops_5, sizeof(nops_5));
    logFmt("[HLE_PATCH] D3DPalette_Lock2 0x80000000 alias bypassed @ 0x00155F65\n");

    VirtualProtect((void*)0x0015310D, sizeof(nops_5), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0015310D, nops_5, sizeof(nops_5));
    VirtualProtect((void*)0x00153145, sizeof(nops_6), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00153145, nops_6, sizeof(nops_6));
    logFmt("[HLE_PATCH] D3D_DestroyResource 0x80000000 alias bypassed @ 0x0015310D, 0x00153145\n");

    // Patch 64-65: CRI Sofdec movie audio & VSync clock sync (mode 1) + repeat-frame redraw fixes
    // 1) Keep MWSFD Audio Stream (property 6 = 1) and ADX/RNA voice output fully enabled so movie audio plays via DirectSound.
    // 2) Switch MWSFD Master Clock (property 0xF) from 2 (ADX audio sample clock) to 1 (VSync timer clock 0x00124760)
    //    at 0x0011CCD9 (FUN_0011cc50), 0x001270ED (FUN_001270e0), and 0x0011FF33 (FUN_0011ff00 runtime audio-start check)
    //    so video pacing stays locked to 30 FPS while audio plays.
    // 3) Bypass MWSFD MPEG frame-skip drop check in FUN_001229a0 (0x001229A0 -> xor eax, eax; ret) so the decoder never
    //    skips/drops video frames when comparing presentation timestamps against the VSync clock.
    const uint8_t patch_push_1[] = { 0x6A, 0x01 };
    VirtualProtect((void*)0x0011CCD9, sizeof(patch_push_1), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0011CCD9, patch_push_1, sizeof(patch_push_1));
    VirtualProtect((void*)0x001270ED, sizeof(patch_push_1), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001270ED, patch_push_1, sizeof(patch_push_1));
    VirtualProtect((void*)0x0011FF33, sizeof(patch_push_1), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0011FF33, patch_push_1, sizeof(patch_push_1));

    const uint8_t patch_no_frameskip[] = { 0x31, 0xC0, 0xC3 }; // xor eax, eax; ret
    VirtualProtect((void*)0x001229A0, sizeof(patch_no_frameskip), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x001229A0, patch_no_frameskip, sizeof(patch_no_frameskip));

    // 4) In mwPlyGetCurFrm (0x0011BE00 @ 0x0011BE56): jump past the 0x0011BE62 catch-up frame-drop loop (jmp 0x0011BE9C)
    //    so mwPlyGetCurFrm never discards decoded frames in the ring buffer when mwPlyIsNextFrmReady returns 1.
    const uint8_t patch_nodrop_11be56[] = { 0xEB, 0x44 };
    VirtualProtect((void*)0x0011BE56, sizeof(patch_nodrop_11be56), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0011BE56, patch_nodrop_11be56, sizeof(patch_nodrop_11be56));

    // 5) In FUN_0012db60 (0x0012DB60 @ 0x0012DBF5): NOP `xor edi, edi` so the very first decoded frame is returned
    //    immediately even when only 1 frame is in the ring buffer (ebx == 1) and [esi + 0x80] == 0.
    const uint8_t patch_nop2[] = { 0x90, 0x90 };
    VirtualProtect((void*)0x0012DBF5, sizeof(patch_nop2), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0012DBF5, patch_nop2, sizeof(patch_nop2));

    // 6) In mwPlyIsNextFrmReady (0x0012DC10 @ 0x0012DCAD): NOP the redundant clock threshold check (JNE 0x0012DCB6)
    //    because FUN_00090280 already paces video at 30 FPS by toggling [esi+0xF8] every 2 VSync frames (prevents 3-VSync 20 FPS stutter).
    VirtualProtect((void*)0x0012DCAD, sizeof(patch_nop2), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0012DCAD, patch_nop2, sizeof(patch_nop2));
    logFmt("[HLE_PATCH] CRI Sofdec SFD audio enabled, Master Clock=VSync @ 0x0011CCD9/0x001270ED/0x0011FF33, NoDrop @ 0x0011BE56/0x001229A0/0x0012DBF5, 30FPS pacing @ 0x0012DCAD\n");

    // Patch 66: FUN_00031670 direct texture descriptor address check @ 0x0003167C
    const uint8_t patch_3167c[] = { 0x3D, 0x00, 0x00, 0x10, 0x00 };
    VirtualProtect((void*)0x0003167C, sizeof(patch_3167c), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0003167C, patch_3167c, sizeof(patch_3167c));
    logFmt("[HLE_PATCH] FUN_00031670 texture address check lowered to 0x00100000 @ 0x0003167C\n");

    // Patch 67: FUN_0002cfc0 VXT non-pow2 UV scale address check @ 0x0002CFDB
    const uint8_t patch_2cfdb[] = {
        0x90, 0x90, 0x90, 0x90, 0x90,
        0x3D, 0x00, 0x00, 0x10, 0x00,
        0x72, 0x72
    };
    VirtualProtect((void*)0x0002CFDB, sizeof(patch_2cfdb), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0002CFDB, patch_2cfdb, sizeof(patch_2cfdb));
    logFmt("[HLE_PATCH] FUN_0002cfc0 VXT UV scale address check patched @ 0x0002CFDB\n");

    // Patch 68: 3D scene graph pointer range checks @ 0x0001797B, 0x00017CB7, 0x000177CB
    const uint8_t patch_cmp_80m[] = { 0x3D, 0x00, 0x00, 0x00, 0x80 };
    VirtualProtect((void*)0x0001797B, sizeof(patch_cmp_80m), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0001797B, patch_cmp_80m, sizeof(patch_cmp_80m));
    VirtualProtect((void*)0x00017CB7, sizeof(patch_cmp_80m), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00017CB7, patch_cmp_80m, sizeof(patch_cmp_80m));
    VirtualProtect((void*)0x000177CB, sizeof(patch_cmp_80m), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x000177CB, patch_cmp_80m, sizeof(patch_cmp_80m));
    logFmt("[HLE_PATCH] 3D scene graph pointer range expanded to <0x80000000 @ 0x0001797B, 0x00017CB7, 0x000177CB\n");

    // Patch 69: FUN_00060f70 force full-screen shadow volume quad @ 0x00061174, 0x0006117D
    const uint8_t patch_shadow_quad[] = { 0x90, 0x90 };
    VirtualProtect((void*)0x00061174, sizeof(patch_shadow_quad), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00061174, patch_shadow_quad, sizeof(patch_shadow_quad));
    VirtualProtect((void*)0x0006117D, sizeof(patch_shadow_quad), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0006117D, patch_shadow_quad, sizeof(patch_shadow_quad));
    logFmt("[HLE_PATCH] FUN_00060f70 forced full-screen shadow quad @ 0x00061174, 0x0006117D\n");

    // Patch 70: FUN_00061b10 bypass 720p/supersampling shadow volume culling & extrusion halving @ 0x00061F80 -> JMP 0x00062341
    const uint8_t patch_shadow_cull_bypass[] = { 0xE9, 0xBC, 0x03, 0x00, 0x00 };
    VirtualProtect((void*)0x00061F80, sizeof(patch_shadow_cull_bypass), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00061F80, patch_shadow_cull_bypass, sizeof(patch_shadow_cull_bypass));
    logFmt("[HLE_PATCH] FUN_00061b10 shadow volume culling/halving bypassed @ 0x00061F80 (JMP 0x00062341)\n");

    // Patch 71: Lens Flare / SunGlow / Flare depth readback fix:
    // Stub offscreen 8x8 depth copy FUN_00033840 (RET 0x14) and initialize depth readback buffer to 0x00000000 (Reversed-Z far plane)
    const uint8_t patch_ret14[] = { 0xC2, 0x14, 0x00 };
    VirtualProtect((void*)0x00033840, sizeof(patch_ret14), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00033840, patch_ret14, sizeof(patch_ret14));
    const uint8_t patch_xor_eax[] = { 0x33, 0xC0, 0x90 }; // XOR EAX, EAX; NOP (replaces OR EAX, 0xFFFFFFFF)
    VirtualProtect((void*)0x0005DFDC, sizeof(patch_xor_eax), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0005DFDC, patch_xor_eax, sizeof(patch_xor_eax));
    VirtualProtect((void*)0x0005E24D, sizeof(patch_xor_eax), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x0005E24D, patch_xor_eax, sizeof(patch_xor_eax));
    logFmt("[HLE_PATCH] SunGlow/Flare depth occlusion readback unoccluded @ 0x00033840, 0x0005DFDC, 0x0005E24D\n");

    // Patch 72: X-Fade (0x00028f60) high-resolution HLE capture support:
    // Stub XFade_CopyFrontBufferToVXT (0x00033750, __thiscall with 3 stack args -> RET 0x0C) so
    // XFade_ProcessPendingCapture (0x0002fb10) marks the capture complete (ce = 0xFF) while D3D9_Present()
    // captures the full-resolution GPU backbuffer directly into g_pXFadeTexture.
    const uint8_t patch_ret0c[] = { 0xC2, 0x0C, 0x00 };
    VirtualProtect((void*)0x00033750, sizeof(patch_ret0c), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)0x00033750, patch_ret0c, sizeof(patch_ret0c));
    logFmt("[HLE_PATCH] XFade_CopyFrontBufferToVXT stubbed for high-res GPU capture @ 0x00033750\n");

#if USE_D3D9_BACKEND
    NamcoRenderer_InstallHooks();
#endif

    return TRUE;
}

static void InstallJmpHook(uint32_t targetAddr, void* hookFn) {
    DWORD oldProtect;
    uint8_t jmpBytes[5];
    jmpBytes[0] = 0xE9; // JMP rel32
    int32_t rel = (int32_t)((uintptr_t)hookFn - (targetAddr + 5));
    memcpy(&jmpBytes[1], &rel, 4);
    VirtualProtect((void*)targetAddr, sizeof(jmpBytes), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)targetAddr, jmpBytes, sizeof(jmpBytes));
}

static void* s_trampoline_HBOX_addr = NULL;

static void* __cdecl Hook_HBOX_Func(void* p1, void* p2, void* p3, void* p4, void* p5, void* p6, void* p7, void* p8, void* p9, void* p10, void* p11, void* p12) {
    void* caller = __builtin_return_address(0);
    logFmt("[HBOX_HOOK] HBOX_Func called from %p! (p1=%p, p2=%p, p3=%p)\n", caller, p1, p2, p3);
    typedef void* (__cdecl *HBOXFunc)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
    return ((HBOXFunc)s_trampoline_HBOX_addr)(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12);
}

void InstallHBoxHook(void) {
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 32, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    s_trampoline_HBOX_addr = tramp;
    memcpy(tramp, (void*)0x000B81B0, 5);
    tramp[5] = 0xE9;
    int32_t relBack = (int32_t)(0x000B81B5 - ((uintptr_t)tramp + 10));
    memcpy(&tramp[6], &relBack, 4);

    InstallJmpHook(0x000B81B0, (void*)Hook_HBOX_Func);
    logFmt("[HLE_PATCH] HBOX_Func hooked with diagnostic logging @ 0x000B81B0\n");
}

static void* s_trampoline_Sys_TaskCreate = NULL;

static void* __cdecl Hook_Sys_TaskCreate(void* parent, int priority, int param3, const char* name, void* func1, void* func2, void* func3) {
    const char* taskName = name ? name : "(null)";
    logFmt("[TASK] Sys_TaskCreate: name='%s', prio=%d, parent=%p, f1=%p, f2=%p, f3=%p\n",
           taskName, priority, parent, func1, func2, func3);
    typedef void* (__cdecl *SysTaskCreateFn)(void*, int, int, const char*, void*, void*, void*);
    return ((SysTaskCreateFn)s_trampoline_Sys_TaskCreate)(parent, priority, param3, name, func1, func2, func3);
}

void InstallSysTaskCreateHook(void) {
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 32, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    s_trampoline_Sys_TaskCreate = tramp;
    memcpy(tramp, (void*)0x00014C80, 8);
    tramp[8] = 0xE9;
    int32_t relBack = (int32_t)(0x00014C88 - ((uintptr_t)tramp + 13));
    memcpy(&tramp[9], &relBack, 4);

    DWORD oldProtect;
    VirtualProtect((void*)0x00014C80, 8, PAGE_EXECUTE_READWRITE, &oldProtect);
    uint8_t jmpBytes[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)((uintptr_t)Hook_Sys_TaskCreate - (0x00014C80 + 5));
    memcpy(&jmpBytes[1], &rel, 4);
    memcpy((void*)0x00014C80, jmpBytes, 8);
    logFmt("[HLE_PATCH] Sys_TaskCreate hooked with diagnostic logging @ 0x00014C80\n");
}

static void __stdcall Hook_D3DDevice_InsertCallback(int type, void (__cdecl *pfnCallback)(uint32_t), uint32_t arg) {
    (void)type;
    if (pfnCallback) {
        pfnCallback(arg);
    }
}

static HWND s_hRenderWnd = NULL;
static HDC s_hRenderDC = NULL;
static HANDLE s_hRenderThread = NULL;
static uint32_t s_swapCount = 0;
static uint32_t s_renderWidth = 1280;
static uint32_t s_renderHeight = 720;
static BOOL s_fullscreen = FALSE;
static WINDOWPLACEMENT s_savedWndPlacement = { sizeof(WINDOWPLACEMENT) };

extern "C" void NativeSC2_SetRenderConfig(uint32_t width, uint32_t height, BOOL fullscreen) {
    if (width >= 320 && height >= 200) {
        s_renderWidth = width;
        s_renderHeight = height;
    }
    s_fullscreen = fullscreen;
}

static void ToggleBorderlessFullscreen(HWND hWnd) {
    DWORD style = (DWORD)GetWindowLongA(hWnd, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hWnd, &s_savedWndPlacement) &&
            GetMonitorInfoA(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(hWnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hWnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            s_fullscreen = TRUE;
        }
    } else {
        SetWindowLongA(hWnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hWnd, &s_savedWndPlacement);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        s_fullscreen = FALSE;
    }
}

static LRESULT CALLBACK RenderWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_SIZE: {
            uint32_t w = LOWORD(lParam);
            uint32_t h = HIWORD(lParam);
            if (wParam != SIZE_MINIMIZED && w > 0 && h > 0) {
                D3D9_Resize(w, h);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            logFmt("[NativeSC2] Window close requested. Terminating process.\n");
            TerminateProcess(GetCurrentProcess(), 0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            TerminateProcess(GetCurrentProcess(), 0);
            return 0;
        case WM_SYSKEYDOWN:
            if (wParam == VK_RETURN && (lParam & (1 << 29))) {
                ToggleBorderlessFullscreen(hWnd);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                logFmt("[NativeSC2] Escape pressed. Terminating process.\n");
                TerminateProcess(GetCurrentProcess(), 0);
            } else if (wParam == VK_F12) {
                ToggleBorderlessFullscreen(hWnd);
            } else if (wParam == VK_F11) {
                CpuProfiler_DumpReport();
            } else if (wParam == VK_F10) {
                CpuProfiler_ToggleSpinloopFixes();
            } else if (wParam == VK_F5) {
                ModEngine_ForceRefresh();
            }
            break;
    }
    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    (void)dwCtrlType;
    TerminateProcess(GetCurrentProcess(), 0);
    return TRUE;
}

static DWORD WINAPI RenderWindowThread(LPVOID lpParam) {
    (void)lpParam;
    logFmt("[NativeSC2] RenderWindowThread entered!\n");

    // Enable High-DPI awareness so 4K monitors render 1:1 physical pixels without DWM scaling blur
    typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        PFN_SetProcessDPIAware pfnDPIAware = (PFN_SetProcessDPIAware)GetProcAddress(hUser32, "SetProcessDPIAware");
        if (pfnDPIAware) pfnDPIAware();
    }

    WNDCLASSA wc = { 0 };
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = RenderWindowProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "NativeSC2_WindowClass";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    uint32_t winClientW = s_renderWidth;
    uint32_t winClientH = s_renderHeight;
    if (!s_fullscreen && screenW > 0 && screenH > 0) {
        if ((int)winClientW > screenW || (int)winClientH > screenH) {
            winClientW = (uint32_t)screenW;
            winClientH = (uint32_t)screenH;
        }
    }

    char titleBuf[160];
    snprintf(titleBuf, sizeof(titleBuf),
        "Soulcalibur II (USA) - Native Win32 [%ux%u Internal | F12/Alt+Enter: Fullscreen]",
        s_renderWidth, s_renderHeight);

    RECT rc = { 0, 0, (LONG)winClientW, (LONG)winClientH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    s_hRenderWnd = CreateWindowExA(
        0,
        "NativeSC2_WindowClass",
        titleBuf,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, wc.hInstance, NULL
    );

    if (s_fullscreen) {
        s_fullscreen = FALSE; // ToggleBorderlessFullscreen flips this to TRUE
        ToggleBorderlessFullscreen(s_hRenderWnd);
    }

    s_hRenderDC = GetDC(s_hRenderWnd);
    ShowWindow(s_hRenderWnd, SW_SHOW);
    UpdateWindow(s_hRenderWnd);

    logFmt("[NativeSC2] Win32 Game Window Created on UI thread: %p (Window: %ux%u, Backbuffer: %ux%u)\n",
        (void*)s_hRenderWnd, winClientW, winClientH, s_renderWidth, s_renderHeight);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    ExitProcess(0);
    return 0;
}

extern "C" void NativeSC2_InitRenderer(void) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    if (!s_hRenderThread) {
        logFmt("[NativeSC2] Spawning RenderWindowThread before XBE mapping...\n");
        s_hRenderThread = CreateThread(NULL, 0, RenderWindowThread, NULL, 0, NULL);
        if (!s_hRenderThread) {
            logFmt("[NativeSC2] CreateThread failed: %lu\n", GetLastError());
            return;
        }
        logFmt("[NativeSC2] RenderWindowThread handle: %p\n", s_hRenderThread);

        while (!s_hRenderWnd || !s_hRenderDC) {
            Sleep(5);
        }

#if USE_D3D9_BACKEND
        RECT clientRc;
        GetClientRect(s_hRenderWnd, &clientRc);
        uint32_t initW = clientRc.right - clientRc.left;
        uint32_t initH = clientRc.bottom - clientRc.top;
        if (initW == 0 || initH == 0) {
            initW = s_renderWidth;
            initH = s_renderHeight;
        }
        logFmt("[NativeSC2] Initializing Direct3D 9 hardware renderer on game thread (Backbuffer: %ux%u, Window: %ux%u)...\n",
            s_renderWidth, s_renderHeight, initW, initH);
        D3D9_Init(s_hRenderWnd, s_renderWidth, s_renderHeight);
#else
        logFmt("[NativeSC2] Initializing OpenGL hardware renderer on game thread...\n");
        NV2A_InitGL(s_hRenderWnd, s_hRenderDC);
#endif
    }
}

extern "C" void NativeSC2_OnFrameSwap(uint32_t swapFlags) {
    // D3DSWAP_BYPASS (2) is called before D3DDevice_CopyRects and immediately followed by
    // D3DSWAP_FINISH (4) inside FUN_000323b0 during framebuffer freeze/fade-out and X-Fade capture.
    // Flush any pending pushbuffer draws now (before FUN_00153f00(2) switches RenderTarget to the
    // 1x FrontBuffer and alters pDev[0x151] scaleX from 2.0f to 1.0f), then return so we only
    // present once on D3DSWAP_FINISH (4).
    if (swapFlags == 2) {
#if USE_D3D9_BACKEND
        D3D9_FlushPushBuffer();
#endif
        return;
    }

    s_swapCount++;

    uint32_t** ppDev = (uint32_t**)0x0015E628;
    if (ppDev && *ppDev) {
        uint32_t* pDev = *ppDev;

#if USE_D3D9_BACKEND
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        D3D9_ProcessPushBuffer(pDev);
        QueryPerformanceCounter(&t1);
        D3D9_Present();
        QueryPerformanceCounter(&t2);
        CpuProfiler_RecordPushBufferTicks(t1.QuadPart - t0.QuadPart);
        CpuProfiler_RecordPresentTicks(t2.QuadPart - t1.QuadPart);
#else
        NV2A_ProcessPushBuffer(pDev);
        NV2A_SwapBuffers(s_hRenderDC);
#endif

        if (s_swapCount == 1 || s_swapCount % 60 == 0) {
            logFmt("[NativeSC2] Frame %u swapped (pushPut: 0x%08X, curSwap: %u)\n", s_swapCount, pDev[0], pDev[0x816]);
        }
    }

    CpuProfiler_OnFrameSwap();

    // 60 FPS pacing
    static LARGE_INTEGER s_lastTime = {0};
    static LARGE_INTEGER s_freq = {0};
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_lastTime);
    } else {
        LARGE_INTEGER curTime;
        QueryPerformanceCounter(&curTime);
        double elapsedMs = (double)(curTime.QuadPart - s_lastTime.QuadPart) * 1000.0 / (double)s_freq.QuadPart;
        if (elapsedMs < 16.0) {
            Sleep((DWORD)(16.0 - elapsedMs));
        }
        QueryPerformanceCounter(&s_lastTime);
    }
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((naked)) static void Hook_D3DDevice_Swap_Trampoline(void) {
    __asm__ volatile (
        "pushal \n\t"
        "pushl 36(%esp) \n\t"
        "call _NativeSC2_OnFrameSwap \n\t"
        "addl $4, %esp \n\t"
        "popal \n\t"
        "push %esi \n\t"
        "mov 0x0015e628, %esi \n\t"
        "push $0x00154247 \n\t"
        "ret \n\t"
    );
}
#else
__declspec(naked) static void Hook_D3DDevice_Swap_Trampoline(void) {
    __asm {
        pushad
        push dword ptr [esp + 36]
        call NativeSC2_OnFrameSwap
        add esp, 4
        popad
        push esi
        mov eax, 0x0015e628
        mov esi, dword ptr [eax]
        mov eax, 0x00154247
        jmp eax
    }
}
#endif
