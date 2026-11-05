#ifndef NATIVESC2_MOD_ENGINE_H
#define NATIVESC2_MOD_ENGINE_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the HLE OLK Mod Engine (scans <gameDir>\mods and <gameDir>\mods_xbox)
void ModEngine_Init(const char* gameDir, BOOL logAllFileLoads);

// Install HLE hooks into Xbox SC2's OLK file system (0x00020F80, 0x00020ED0, 0x00020750)
void ModEngine_InstallHooks(void);

// Force an immediate rescan of the mod directories and re-sync the in-RAM OLK TOC
void ModEngine_ForceRefresh(void);

#ifdef __cplusplus
}
#endif

#endif // NATIVESC2_MOD_ENGINE_H
