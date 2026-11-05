#ifndef AUDIO_BRIDGE_H
#define AUDIO_BRIDGE_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool Audio_Init(void);
void Audio_Shutdown(void);
void Audio_InstallHooks(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_BRIDGE_H
