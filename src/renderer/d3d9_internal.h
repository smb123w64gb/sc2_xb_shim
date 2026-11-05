#ifndef D3D9_INTERNAL_H
#define D3D9_INTERNAL_H

#include "d3d9_backend.h"
#include "xboxkrnl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>

// Shared D3D9 Backend State
extern IDirect3D9* g_pD3D;
extern IDirect3DDevice9* g_pDevice;
extern D3DPRESENT_PARAMETERS g_d3dpp;
extern HWND g_hWnd;
extern bool g_d3d9Initialized;
extern bool g_inScene;

extern uint32_t g_totalFrames;
extern uint32_t g_drawCount;
extern uint32_t g_vertexCount;
extern uint32_t g_frameCounter;

extern IDirect3DTexture9* g_boundTextures[4];
extern float g_projMatrix[16];
extern float g_modelViewMatrix[16];

// Internal helpers
void D3D9_Log(const char* fmt, ...);
void D3D9_ReleaseAllTextures(void);

static inline bool D3D9_FormatHasAlpha(uint32_t fmt) {
    return (fmt == 0x01 || fmt == 0x02 || fmt == 0x04 || fmt == 0x06 ||
            fmt == 0x0B || fmt == 0x0C || fmt == 0x0E || fmt == 0x0F ||
            fmt == 0x19 || fmt == 0x1A);
}

#endif // D3D9_INTERNAL_H
