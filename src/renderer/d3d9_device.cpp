#include "d3d9_internal.h"

IDirect3D9* g_pD3D = nullptr;
IDirect3DDevice9* g_pDevice = nullptr;
D3DPRESENT_PARAMETERS g_d3dpp = {0};
HWND g_hWnd = nullptr;
bool g_d3d9Initialized = false;
bool g_inScene = false;

uint32_t g_totalFrames = 0;
uint32_t g_drawCount = 0;
uint32_t g_vertexCount = 0;
uint32_t g_frameCounter = 1;

IDirect3DTexture9* g_boundTextures[4] = { nullptr, nullptr, nullptr, nullptr };
float g_projMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
float g_modelViewMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

static IDirect3DTexture9* g_pHistoryTexture = nullptr;
static IDirect3DTexture9* g_pXFadeTexture = nullptr;
static bool g_hasValidHistory = false;

static void ReleaseFrameCaptureTextures(void) {
    if (g_pHistoryTexture) {
        g_pHistoryTexture->Release();
        g_pHistoryTexture = nullptr;
    }
    if (g_pXFadeTexture) {
        g_pXFadeTexture->Release();
        g_pXFadeTexture = nullptr;
    }
    g_hasValidHistory = false;
}

static bool EnsureFrameCaptureTextures(void) {
    if (!g_pDevice) return false;
    uint32_t w = g_d3dpp.BackBufferWidth ? g_d3dpp.BackBufferWidth : 1280;
    uint32_t h = g_d3dpp.BackBufferHeight ? g_d3dpp.BackBufferHeight : 720;
    if (!g_pHistoryTexture) {
        if (FAILED(g_pDevice->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_pHistoryTexture, nullptr))) {
            g_pDevice->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_pHistoryTexture, nullptr);
        }
    }
    if (!g_pXFadeTexture) {
        if (FAILED(g_pDevice->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_pXFadeTexture, nullptr))) {
            g_pDevice->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_pXFadeTexture, nullptr);
        }
    }
    return (g_pHistoryTexture != nullptr && g_pXFadeTexture != nullptr);
}

void D3D9_Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("%s", buf);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Namco Engine Blend Mode Translation
// ---------------------------------------------------------------------------
struct NamcoBlendMode {
    uint32_t enable;
    uint32_t blendOp;
    uint32_t srcBlend;
    uint32_t destBlend;
};

static const NamcoBlendMode s_blendTable[8] = {
    { 0, 0, 0, 0 },                                                  // 0: Disabled
    { 1, D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA },  // 1: Normal Alpha Blend
    { 1, D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_ONE },          // 2: Additive
    { 1, D3DBLENDOP_REVSUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_ONE },  // 3: Subtractive
    { 1, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_SRCALPHA },         // 4: Dest-alpha modulation
    { 1, D3DBLENDOP_ADD, D3DBLEND_DESTCOLOR, D3DBLEND_SRCALPHA },    // 5: Color-blend
    { 1, D3DBLENDOP_ADD, D3DBLEND_SRCCOLOR, D3DBLEND_ONE },          // 6: Color add
    { 1, D3DBLENDOP_ADD, D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA } // 7: Dest-alpha blend
};

extern "C" {

IDirect3DDevice9* D3D9_GetDevice(void) {
    return g_pDevice;
}

uint32_t D3D9_GetBackBufferWidth(void) {
    return g_d3dpp.BackBufferWidth ? g_d3dpp.BackBufferWidth : 1280;
}

uint32_t D3D9_GetBackBufferHeight(void) {
    return g_d3dpp.BackBufferHeight ? g_d3dpp.BackBufferHeight : 720;
}

IDirect3DTexture9* D3D9_GetXFadeTexture(void) {
    EnsureFrameCaptureTextures();
    return g_pXFadeTexture;
}

void D3D9_ApplyNamcoBlendMode(uint32_t mode) {
    if (mode >= 8) mode = 0;
    const NamcoBlendMode& bm = s_blendTable[mode];
    if (bm.enable) {
        D3D9_SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        D3D9_SetRenderState(D3DRS_BLENDOP, bm.blendOp);
        D3D9_SetRenderState(D3DRS_SRCBLEND, bm.srcBlend);
        D3D9_SetRenderState(D3DRS_DESTBLEND, bm.destBlend);
    } else {
        D3D9_SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    }
}

bool D3D9_Init(HWND hWnd, uint32_t width, uint32_t height) {
    if (g_d3d9Initialized) return true;

    g_hWnd = hWnd;
    D3D9_Log("[D3D9] Initializing Direct3D 9 backend...\n");

    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) {
        D3D9_Log("[D3D9] Error: Direct3DCreate9 failed!\n");
        return false;
    }

    D3DADAPTER_IDENTIFIER9 ident;
    if (SUCCEEDED(g_pD3D->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident))) {
        D3D9_Log("[D3D9] Adapter: %s (%s)\n", ident.Description, ident.Driver);
    }

    memset(&g_d3dpp, 0, sizeof(g_d3dpp));
    g_d3dpp.BackBufferWidth = width ? width : 1280;
    g_d3dpp.BackBufferHeight = height ? height : 720;
    g_d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_d3dpp.BackBufferCount = 1;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.hDeviceWindow = hWnd;
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    D3DMULTISAMPLE_TYPE msType = D3DMULTISAMPLE_NONE;
    DWORD msQuality = 0;
    DWORD qLevels = 0;
    if (SUCCEEDED(g_pD3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES, &qLevels)) &&
        SUCCEEDED(g_pD3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_D24S8, TRUE, D3DMULTISAMPLE_4_SAMPLES, nullptr)) && qLevels > 0) {
        msType = D3DMULTISAMPLE_4_SAMPLES;
        msQuality = qLevels - 1;
        D3D9_Log("[D3D9] 4x MSAA Hardware Anti-Aliasing enabled (Quality: %lu)\n", msQuality);
    } else if (SUCCEEDED(g_pD3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_2_SAMPLES, &qLevels)) &&
        SUCCEEDED(g_pD3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_D24S8, TRUE, D3DMULTISAMPLE_2_SAMPLES, nullptr)) && qLevels > 0) {
        msType = D3DMULTISAMPLE_2_SAMPLES;
        msQuality = qLevels - 1;
        D3D9_Log("[D3D9] 2x MSAA Hardware Anti-Aliasing enabled (Quality: %lu)\n", msQuality);
    }
    g_d3dpp.MultiSampleType = msType;
    g_d3dpp.MultiSampleQuality = msQuality;

    HRESULT hr = g_pD3D->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        hWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &g_d3dpp,
        &g_pDevice
    );

    if (FAILED(hr)) {
        D3D9_Log("[D3D9] Hardware VP creation failed (0x%08lX), trying Software VP...\n", hr);
        hr = g_pD3D->CreateDevice(
            D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL,
            hWnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &g_d3dpp,
            &g_pDevice
        );
    }

    if (FAILED(hr) || !g_pDevice) {
        D3D9_Log("[D3D9] Fatal: CreateDevice failed with error 0x%08lX\n", hr);
        g_pD3D->Release();
        g_pD3D = nullptr;
        return false;
    }

    D3D9_Log("[D3D9] Direct3D 9 Device created successfully! (%ux%u)\n", g_d3dpp.BackBufferWidth, g_d3dpp.BackBufferHeight);

    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_pDevice->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    g_pDevice->SetRenderState(D3DRS_ALPHAREF, 0);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, (msType != D3DMULTISAMPLE_NONE) ? TRUE : FALSE);

    for (DWORD s = 0; s < 4; s++) {
        g_pDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, 8);
    }

    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    D3D9_SetXboxCompositeMatrix(nullptr, false);
    g_pDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x00000000, 0.0f, 0);

    g_d3d9Initialized = true;
    return true;
}

void D3D9_Shutdown(void) {
    if (!g_d3d9Initialized) return;

    ReleaseFrameCaptureTextures();
    D3D9_ReleaseAllTextures();

    if (g_pDevice) {
        g_pDevice->Release();
        g_pDevice = nullptr;
    }
    if (g_pD3D) {
        g_pD3D->Release();
        g_pD3D = nullptr;
    }
    g_d3d9Initialized = false;
    D3D9_Log("[D3D9] Backend shut down.\n");
}

bool D3D9_BeginScene(void) {
    if (!g_pDevice || g_inScene) return false;
    if (SUCCEEDED(g_pDevice->BeginScene())) {
        g_inScene = true;
        return true;
    }
    return false;
}

void D3D9_EndScene(void) {
    if (!g_pDevice || !g_inScene) return;
    g_pDevice->EndScene();
    g_inScene = false;
}

void D3D9_Resize(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
    // Internal backbuffer remains at 1280x720 HD; Present scales to window client rect.
}

void D3D9_Present(void) {
    if (!g_pDevice) return;
    g_frameCounter++;

    // -----------------------------------------------------------------------
    // 1. Framebuffer Freeze (DAT_00373f41 / DAT_00373f42) & High-Res X-Fade Capture
    // -----------------------------------------------------------------------
    uint32_t pFadeStruct = *(const uint32_t*)0x0038E570;
    uint32_t pMgr        = *(const uint32_t*)0x0038E574;
    uint8_t ce           = (pMgr >= 0x00010000) ? *(const uint8_t*)(pMgr + 0xCE) : 0;
    uint32_t wasFrozen   = (pMgr >= 0x00010000) ? *(const uint32_t*)(pMgr + 0x144) : 0;
    uint8_t freezeNext   = *(const uint8_t*)0x00373F41 | *(const uint8_t*)0x00373F42;

    IDirect3DSurface9* pBackBuffer = nullptr;
    IDirect3DSurface9* pHistorySurf = nullptr;
    IDirect3DSurface9* pXFadeSurf = nullptr;

    if (EnsureFrameCaptureTextures()) {
        g_pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
        g_pHistoryTexture->GetSurfaceLevel(0, &pHistorySurf);
        g_pXFadeTexture->GetSurfaceLevel(0, &pXFadeSurf);

        if (pBackBuffer && pHistorySurf && pXFadeSurf) {
            if ((ce == 1 || ce == 5) && g_hasValidHistory) {
                // X-Fade (0x00028f60) screenshot request: copy the previous completed frame
                // (FrontBuffer / g_pHistoryTexture) into g_pXFadeTexture and onto pBackBuffer.
                g_pDevice->StretchRect(pHistorySurf, nullptr, pXFadeSurf, nullptr, D3DTEXF_NONE);
                g_pDevice->StretchRect(pHistorySurf, nullptr, pBackBuffer, nullptr, D3DTEXF_NONE);
                D3D9_Log("[HLE_XFADE] Captured %ux%u frame into X-Fade texture (ce=%u)\n",
                         D3D9_GetBackBufferWidth(), D3D9_GetBackBufferHeight(), ce);
            } else if (wasFrozen == 0 && g_drawCount > 0) {
                // Normal actively rendered frame: save clean pre-gamma backbuffer into history texture
                g_pDevice->StretchRect(pBackBuffer, nullptr, pHistorySurf, nullptr, D3DTEXF_NONE);
                g_hasValidHistory = true;
            } else if ((wasFrozen != 0 || freezeNext != 0) && g_drawCount == 0 && g_hasValidHistory) {
                // Frozen frame where DrawFrame suppressed scene rendering (DAT_00373f40 = 1):
                // restore the frozen frame into pBackBuffer before applying the gamma-ramp fade.
                g_pDevice->StretchRect(pHistorySurf, nullptr, pBackBuffer, nullptr, D3DTEXF_NONE);
            }
        }
    }

    // -----------------------------------------------------------------------
    // 2. Hardware Gamma-Ramp Brightness / Black Fade Emulation (FUN_0002dab0)
    //    On Xbox, black fade-ins (FUN_00034040(2, 0x3c, 0, 0, 0)) and master
    //    fade-outs (FUN_00050240(0x3c)) skip drawing Box2D and instead dim the
    //    hardware DAC via D3DDevice_SetGammaRamp. Emulate this in windowed D3D9.
    // -----------------------------------------------------------------------
    if (pFadeStruct >= 0x00010000 && *(const uint8_t*)(pFadeStruct + 0xFD) == 0) {
        float masterBright = *(const float*)(pFadeStruct + 0x50);
        float fadeR = *(const float*)(pFadeStruct + 0x90);
        float fadeG = *(const float*)(pFadeStruct + 0x94);
        float fadeB = *(const float*)(pFadeStruct + 0x98);
        float fadeA = *(const float*)(pFadeStruct + 0x9C);

        float effectiveBright = masterBright;
        if (fadeR == 0.0f && fadeG == 0.0f && fadeB == 0.0f && fadeA > 0.0f) {
            float oneMinusA = 1.0f - fadeA;
            if (oneMinusA < 0.0f) oneMinusA = 0.0f;
            if (oneMinusA > 1.0f) oneMinusA = 1.0f;
            effectiveBright *= oneMinusA;
        }

        if (effectiveBright < 0.999f || effectiveBright > 1.001f) {
            uint32_t overlayColor = 0;
            if (effectiveBright < 0.999f) {
                float b = effectiveBright < 0.0f ? 0.0f : effectiveBright;
                uint8_t a = (uint8_t)((1.0f - b) * 255.0f + 0.5f);
                overlayColor = ((uint32_t)a << 24);
            } else {
                float w = (effectiveBright - 1.0f) > 1.0f ? 1.0f : (effectiveBright - 1.0f);
                uint8_t a = (uint8_t)(w * 255.0f + 0.5f);
                overlayColor = ((uint32_t)a << 24) | 0x00FFFFFFu;
            }

            if ((overlayColor >> 24) > 0) {
                if (!g_inScene) {
                    D3D9_BeginScene();
                }
                float w = (float)D3D9_GetBackBufferWidth();
                float h = (float)D3D9_GetBackBufferHeight();
                struct GammaQuadVtx { float x, y, z, rhw; DWORD color; };
                const GammaQuadVtx q[4] = {
                    { -0.5f,     -0.5f,     0.5f, 1.0f, overlayColor },
                    { w - 0.5f,  -0.5f,     0.5f, 1.0f, overlayColor },
                    { -0.5f,     h - 0.5f,  0.5f, 1.0f, overlayColor },
                    { w - 0.5f,  h - 0.5f,  0.5f, 1.0f, overlayColor }
                };
                g_pDevice->SetTexture(0, nullptr);
                g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
                g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                g_pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
                g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_pDevice->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
                g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                g_pDevice->SetVertexShader(nullptr);
                g_pDevice->SetPixelShader(nullptr);
                g_pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(GammaQuadVtx));
                g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                g_boundTextures[0] = nullptr;
            }
        }
    }

    if (g_inScene) {
        D3D9_EndScene();
    }
    g_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);

    HRESULT hr = g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
    if (hr == D3DERR_DEVICELOST) {
        hr = g_pDevice->TestCooperativeLevel();
        if (hr == D3DERR_DEVICENOTRESET) {
            if (pXFadeSurf) { pXFadeSurf->Release(); pXFadeSurf = nullptr; }
            if (pHistorySurf) { pHistorySurf->Release(); pHistorySurf = nullptr; }
            if (pBackBuffer) { pBackBuffer->Release(); pBackBuffer = nullptr; }
            ReleaseFrameCaptureTextures();
            g_pDevice->Reset(&g_d3dpp);
        }
    }

    // Prepare backbuffer and reversed-Z depth buffer (far = 0.0f) for the next frame
    g_pDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x00000000, 0.0f, 0);

    // If the next frame is frozen (DAT_00373f41 / DAT_00373f42 != 0), pre-populate the backbuffer
    // with the preserved history surface to match Xbox D3DDevice_CopyRects(FrontBuffer -> BackBuffer).
    if (freezeNext != 0 && g_hasValidHistory && pHistorySurf && pBackBuffer) {
        g_pDevice->StretchRect(pHistorySurf, nullptr, pBackBuffer, nullptr, D3DTEXF_NONE);
    }

    if (pXFadeSurf) pXFadeSurf->Release();
    if (pHistorySurf) pHistorySurf->Release();
    if (pBackBuffer) pBackBuffer->Release();

    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_pDevice->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);

    g_totalFrames++;
    if (g_totalFrames == 1 || g_totalFrames % 60 == 0) {
        D3D9_Log("[D3D9] Frame %u: %u draws, %u verts batched\n", g_totalFrames, g_drawCount, g_vertexCount);
    }
    g_drawCount = 0;
    g_vertexCount = 0;
}

void D3D9_Clear(uint32_t flags, D3DCOLOR color, float z, uint32_t stencil) {
    if (!g_pDevice) return;
    DWORD d3dFlags = 0;
    if (flags & 0xF0) d3dFlags |= D3DCLEAR_TARGET;
    if (flags & 0x01) d3dFlags |= D3DCLEAR_ZBUFFER;
    if (flags & 0x02) d3dFlags |= D3DCLEAR_STENCIL;
    if (d3dFlags == 0) d3dFlags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER;

    // A mid-frame color clear (g_drawCount > 0) is aimed at an offscreen surface
    // (such as the 8x8 sun occlusion buffer in FUN_00033840). Preserve color target,
    // but allow D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL for Camera Passes 1..4 (FUN_00029900)!
    if (g_drawCount > 0) {
        d3dFlags &= ~D3DCLEAR_TARGET;
    }
    if (d3dFlags == 0) return;

    g_pDevice->Clear(0, nullptr, d3dFlags, color, z, stencil);
}

void D3D9_SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    if (g_pDevice) g_pDevice->SetRenderState(state, value);
}

void D3D9_SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (g_pDevice) g_pDevice->SetTextureStageState(stage, type, value);
}

void D3D9_SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    if (g_pDevice) g_pDevice->SetSamplerState(sampler, type, value);
}

void D3D9_SetTransform(D3DTRANSFORMSTATETYPE state, const float matrix[16]) {
    if (!g_pDevice || !matrix) return;
    D3DMATRIX m;
    memcpy(&m, matrix, sizeof(m));
    g_pDevice->SetTransform(state, &m);
}

void D3D9_SetTexture(DWORD stage, IDirect3DTexture9* pTex) {
    if (!g_pDevice) return;
    if (stage < 4) {
        g_boundTextures[stage] = pTex;
    }
    g_pDevice->SetTexture(stage, pTex);
    if (pTex) {
        g_pDevice->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, 8);
    }
    if (stage == 0) {
        if (pTex) {
            uint32_t activeFvf = *(const uint32_t*)0x0038E85C;
            uint32_t engineColorOp = *(const uint32_t*)0x0015E1C0;
            DWORD op = (engineColorOp == 5 || activeFvf == 0x142) ? D3DTOP_MODULATE2X : D3DTOP_MODULATE;
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, op);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, op);
        } else {
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        }
    }
}

#define D3DFVF_VERTEX2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

void D3D9_Draw2DVertices(D3DPRIMITIVETYPE primType, uint32_t primitiveCount, const void* pVertexStreamZeroData, uint32_t vertexStride) {
    if (!g_pDevice || !pVertexStreamZeroData || primitiveCount == 0) return;
    D3D9_BeginScene();
    g_pDevice->SetFVF(D3DFVF_VERTEX2D);
    g_pDevice->DrawPrimitiveUP(primType, primitiveCount, pVertexStreamZeroData, vertexStride);
    g_drawCount++;
}

void D3D9_DrawIndexed3D(D3DPRIMITIVETYPE primType, uint32_t minVertexIndex, uint32_t numVertices, uint32_t startIndex, uint32_t primCount,
                        const void* pVertices, uint32_t vtxStride, DWORD fvf, const uint16_t* pIndices) {
    if (!g_pDevice || !pVertices || !pIndices || primCount == 0) return;
    D3D9_BeginScene();
    g_pDevice->SetFVF(fvf);
    g_pDevice->DrawIndexedPrimitiveUP(
        primType,
        minVertexIndex,
        numVertices,
        primCount,
        pIndices + startIndex,
        D3DFMT_INDEX16,
        pVertices,
        vtxStride
    );
    g_drawCount++;
}

// Convert an Xbox Composite Matrix (which maps vertices to Xbox screen space [0..640*scaleX, 0..480*scaleY, 0..16777215])
// into a Direct3D 9 Projection Matrix (which maps vertices to D3D9 NDC [-1..1, +1..-1, 0..1]).
// D3D9's viewport then maps NDC [-1..1] to the full 1280x720 HD backbuffer!
void D3D9_SetXboxCompositeMatrix(const float* mXbox, bool isTransposed) {
    static const float s_default2DMatrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.7790178f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    if (!mXbox) {
        uint32_t ctx = *(const uint32_t*)0x0038E520;
        if (ctx >= 0x00010000) {
            const float* pDef2D = *(const float**)(ctx + 0x6A8);
            if ((uint32_t)pDef2D >= 0x00010000) {
                mXbox = pDef2D;
            }
        }
        if (!mXbox) {
            mXbox = s_default2DMatrix;
        }
        isTransposed = false;
    }

    float m[16];
    if (isTransposed) {
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                m[r * 4 + c] = mXbox[c * 4 + r];
            }
        }
    } else {
        memcpy(m, mXbox, sizeof(m));
    }

    // Read Xbox D3DDevice backbuffer super-sampling scale factors (set by D3DDevice_SetBackBufferScale @ 0x00154330).
    // In 480i/480p modes, SC2 renders into an active viewport of 640x448 (DAT_0038f948 x DAT_0038f94c, set by FUN_000320b0).
    // Both 3D projection-viewport matrices (D3D_UpdateProjectionViewportTransform @ 0x0014E2F0) and 2D UI coordinates
    // span [0..640, 0..448] (halfW = 320.0f * scaleX, halfH = 224.0f * scaleY).
    // Read Xbox D3DDevice backbuffer super-sampling scale factors (set by D3DDevice_SetBackBufferScale @ 0x00154330).
    // Note: During D3DDevice_Swap(2) (D3DSWAP_BYPASS), FUN_00153f00 temporarily binds the 1x FrontBuffer (pDev[0x57e])
    // as pDev[0x579], which sets pDev[0x151..0x152] to 1.0f while pDev[0x155..0x156] retain the true backbuffer scale.
    const float* pDev = *(const float**)0x0015E628;
    const uint32_t* pDevU = (const uint32_t*)pDev;
    float scaleX = (pDev && pDev[0x151] > 0.01f) ? pDev[0x151] : 1.0f;
    float scaleY = (pDev && pDev[0x152] > 0.01f) ? pDev[0x152] : 1.7790178f;
    if (pDevU && pDevU[0x579] != pDevU[0x57d] && pDev[0x155] > 0.01f && pDev[0x156] > 0.01f) {
        scaleX = pDev[0x155];
        scaleY = pDev[0x156];
        uint32_t msMode = *(const uint32_t*)0x0015E5F8;
        if (msMode != 0 && (pDevU[0x57b] & 0x200) == 0) {
            scaleX *= 0.5f;
            if (msMode == 2) scaleY *= 0.5f;
        }
    }

    // Detect if this is a 2D orthographic matrix (column 3 == [0, 0, 0, 1]) vs 3D perspective matrix
    bool is2D = (fabsf(m[3]) < 1e-4f && fabsf(m[7]) < 1e-4f && fabsf(m[11]) < 1e-4f && fabsf(m[15] - 1.0f) < 1e-4f);

    // Check if a custom unscaled identity 2D matrix was passed (m[0] == 1.0f, m[5] == 1.0f)
    float effScaleX = (is2D && fabsf(m[0] - 1.0f) < 0.05f && fabsf(m[4]) < 1e-4f) ? 1.0f : scaleX;
    float effScaleY = (is2D && fabsf(m[5] - 1.0f) < 0.05f && fabsf(m[1]) < 1e-4f) ? 1.0f : scaleY;

    // Use the engine's active viewport dimensions (DAT_0038f948 = 640, DAT_0038f94c = 448)
    const int vpW = *(const int*)0x0038F948;
    const int vpH = *(const int*)0x0038F94C;
    const int fbH = *(const int*)0x0038F954;
    const float xboxW = (vpW > 0) ? (float)vpW : 640.0f;
    const float xboxH = (vpH > 0) ? (float)vpH : 448.0f;
    const float vpOffsetY = (fbH > vpH && vpH > 0) ? (float)((fbH - vpH) / 2) : 16.0f;

    float halfW = (xboxW * 0.5f) * effScaleX;
    float halfH = (xboxH * 0.5f) * effScaleY;
    float centerX = halfW;
    // Non-transposed 3D matrices from ctx->m694 (FUN_00029100 @ 0x00029673) add unscaled +16.0f
    // when (0x0038F90B & 2) != 0, whereas transposed 0x0680 matrices from pDev+0x158 do not.
    float centerY = halfH + ((!is2D && !isTransposed && (*(const uint8_t*)0x0038F90B & 2)) ? vpOffsetY : 0.0f);

    // Detect if column 2 (Z output) is scaled by Xbox 24-bit depth buffer max (16777215.0f)
    float zMag = fabsf(m[2]) + fabsf(m[6]) + fabsf(m[10]) + fabsf(m[14]);
    float invZScale = (zMag > 100.0f) ? (1.0f / 16777215.0f) : 1.0f;

    for (int i = 0; i < 4; i++) {
        float x = m[i * 4 + 0];
        float y = m[i * 4 + 1];
        float z = m[i * 4 + 2];
        float w = m[i * 4 + 3];

        g_projMatrix[i * 4 + 0] = (x - w * centerX) / halfW;
        g_projMatrix[i * 4 + 1] = (w * centerY - y) / halfH;
        g_projMatrix[i * 4 + 2] = z * invZScale;
        g_projMatrix[i * 4 + 3] = w;
    }

    if (g_pDevice) {
        static const float s_identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        D3D9_SetTransform(D3DTS_WORLD, s_identity);
        D3D9_SetTransform(D3DTS_VIEW, s_identity);
        D3D9_SetTransform(D3DTS_PROJECTION, g_projMatrix);
    }
}

} // extern "C"
