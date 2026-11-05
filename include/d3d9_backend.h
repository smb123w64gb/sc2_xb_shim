#ifndef D3D9_BACKEND_H
#define D3D9_BACKEND_H

#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Direct3D 9 Device Lifecycle
bool D3D9_Init(HWND hWnd, uint32_t width, uint32_t height);
void D3D9_Shutdown(void);
void D3D9_Resize(uint32_t width, uint32_t height);
bool D3D9_BeginScene(void);
void D3D9_EndScene(void);
void D3D9_Present(void);

// Clear backbuffer, depth, stencil
void D3D9_Clear(uint32_t flags, D3DCOLOR color, float z, uint32_t stencil);

// Direct3D 9 Render / Texture states
void D3D9_SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
void D3D9_SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
void D3D9_SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value);

// Matrix transforms (Direct3D 9 native row-major matrices)
void D3D9_SetTransform(D3DTRANSFORMSTATETYPE state, const float matrix[16]);
void D3D9_SetXboxCompositeMatrix(const float* mXbox, bool isTransposed);

// Textures
IDirect3DTexture9* D3D9_GetOrCreateTexture(uint32_t texGpu, uint32_t texFormat, uint32_t texPalette);
IDirect3DTexture9* D3D9_ResolveNamcoTexture(uint32_t texParam, uint16_t palParam, uint16_t fmtFlag);
IDirect3DTexture9* D3D9_GetXFadeTexture(void);
void D3D9_SetTexture(DWORD stage, IDirect3DTexture9* pTex);
void D3D9_ApplyNamcoBlendMode(uint32_t mode);

// 2D Transformed Vertices rendering (XYZRHW, Diffuse, Tex0)
void D3D9_Draw2DVertices(D3DPRIMITIVETYPE primType, uint32_t primitiveCount, const void* pVertexStreamZeroData, uint32_t vertexStride);

// 3D Geometry rendering
void D3D9_DrawIndexed3D(D3DPRIMITIVETYPE primType, uint32_t minVertexIndex, uint32_t numVertices, uint32_t startIndex, uint32_t primCount,
                        const void* pVertices, uint32_t vtxStride, DWORD fvf, const uint16_t* pIndices);

// Pushbuffer processing (allows hybrid operation while HLE hooks are being mapped)
void D3D9_ProcessPushBuffer(uint32_t* pDev);
void D3D9_FlushPushBuffer(void);

// Access to underlying IDirect3DDevice9
IDirect3DDevice9* D3D9_GetDevice(void);
uint32_t D3D9_GetBackBufferWidth(void);
uint32_t D3D9_GetBackBufferHeight(void);

#ifdef __cplusplus
}
#endif

#endif // D3D9_BACKEND_H
