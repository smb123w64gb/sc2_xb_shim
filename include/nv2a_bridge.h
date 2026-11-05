#ifndef NV2A_BRIDGE_H
#define NV2A_BRIDGE_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize NV2A OpenGL rendering context for the window DC
bool NV2A_InitGL(HWND hWnd, HDC hDC);

// Process pushbuffer commands generated up to current pDev[0]
void NV2A_ProcessPushBuffer(uint32_t* pDev);

// Perform frame swap presentation
void NV2A_SwapBuffers(HDC hDC);

#ifdef __cplusplus
}
#endif

#endif // NV2A_BRIDGE_H
