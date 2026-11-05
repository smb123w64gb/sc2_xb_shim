#include "namco_renderer.h"
#include "d3d9_internal.h"
#include "xboxkrnl.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>

// ============================================================================
// Soulcalibur II (Xbox) Primitive Dispatch Table @ 0x0019BDC0 (PTR_Line2D_0019bdc0)
// Complete Re-Evaluation of All 20 Engine Primitives:
// ----------------------------------------------------------------------------
// [0]  0x0019BDC0 -> 0x0002EBD0 : Line2D     (Hooked: Unified 2D Projection)
// [1]  0x0019BDC4 -> 0x0002F750 : Line3D     (Hooked: 3D Projection)
// [2]  0x0019BDC8 -> 0x0002EC90 : Box2D      (Hooked: Unified 2D Projection, Beveled Borders/Boxes)
// [3]  0x0019BDCC -> 0x0002ED80 : Strip2DG   (Hooked: Unified 2D Projection, Menu/Dialog Backgrounds)
// [4]  0x0019BDD0 -> 0x0002EF50 : Circle2D   (Native Pass-Through -> 0x1818 Inline Array FVF 0x42)
// [5]  0x0019BDD4 -> 0x0002F0C0 : Circle3D   (Native Pass-Through -> 0x1818 Inline Array FVF 0x42)
// [6]  0x0019BDD8 -> 0x0002F280 : Ball3D     (Native Pass-Through -> 0x1818 Inline Array FVF 0x42)
// [7]  0x0019BDDC -> 0x0002F610 : Pole3D     (Native Pass-Through -> 0x1818 Inline Array FVF 0x42)
// [8]  0x0019BDE0 -> 0x0002AA30 : Sprite2D   (Hooked: Unified 2D Projection)
// [9]  0x0019BDE4 -> 0x0002ABC0 : Sprite2DR  (Hooked: Unified 2D Projection + FUN_00011000 Angle LUT)
// [10] 0x0019BDE8 -> 0x0002C8F0 : Sprite3D   (Hooked: Unified 2D Projection + ProjectNamcoZ)
// [11] 0x0019BDEC -> 0x0002CA40 : Sprite3DR  (Hooked: Unified 2D Projection + Pre-Rotated Vectors)
// [12] 0x0019BDF0 -> 0x0002AD90 : Strip2D    (Hooked: Unified 2D Projection via p + 0x04 Matrix)
// [13] 0x0019BDF4 -> 0x0002AF00 : Strip2DNT  (Hooked: Unified 2D Projection via p + 0x04 Matrix)
// [14] 0x0019BDF8 -> 0x0002B050 : Strip3D    (Hooked: 3D Projection via p + 0x04 Matrix)
// [15] 0x0019BDFC -> 0x0002B2F0 : Strip3DNT  (Hooked: 3D Projection via p + 0x04 Matrix)
// [16] 0x0019BE00 -> 0x0002B670 : StripVOL   (Native Pass-Through -> Two-Pass Stencil Shadow Volumes)
// [17] 0x0019BE04 -> 0x000272C0 : Print      (Hooked Wrapper: Ensures Font VXT Rebind -> 0x000272C0)
// [18] 0x0019BE08 -> 0x000161A0 : Kanji      (Hooked Wrapper: Ensures Kanji VXT Rebind -> 0x000161A0)
// [19] 0x0019BE0C -> 0x0001B150 : Button     (Hooked Wrapper: Ensures Button VXT Rebind -> 0x0001B150)
// ============================================================================

static void logFmt(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        DWORD written;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, len, &written, nullptr);
        static FILE* s_fLog = nullptr;
        if (!s_fLog) s_fLog = fopen("native_sc2.log", "a");
        if (s_fLog) {
            fwrite(buf, 1, len, s_fLog);
            fflush(s_fLog);
        }
    }
}

struct D3D9Vertex3D {
    float x, y, z;
    uint32_t color;
    float u, v;
};

#define D3DFVF_V3D (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct D3D9Vertex2D {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};

#define D3DFVF_V2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static inline float ClampZ(float z) {
    if (z < 0.0f) return 0.0f;
    if (z > 1.0f) return 1.0f;
    return z;
}

// Replicate FUN_0002b940 (0x0002B940): converts 3D Z to reversed-Z [0..1]
static inline float ProjectNamcoZ(float z) {
    if (z <= 0.0f) return 1.0f;
    uint32_t ctx = *(const uint32_t*)0x0038E520;
    if (ctx >= 0x00010000) {
        const float* pMat = *(const float**)(ctx + 0x698);
        if ((uint32_t)pMat >= 0x00010000) {
            float m10 = pMat[10];
            float m14 = pMat[14];
            z = m14 / z + m10;
            if (z >= 1.0f) return 1.0f;
        }
    }
    return ClampZ(z);
}

// Safely invalidate Namco's cached D3D state globals (matching FUN_000314d0) so subsequent
// native functions (Print, Kanji, Button, Circle2D, etc.) always re-emit their state packets.
// IMPORTANT: Never overwrite 0x0038E854 (which is the engine's render pass mode 0..4)!
static inline void InvalidateNamcoStateCache(void) {
    *(uint32_t*)0x0038E738 = 0;           // cached matrix pointer
    *(uint32_t*)0x0038E73C = 0;           // cached matrix param2
    *(uint32_t*)0x0038E740 = 0;           // cached matrix param3
    *(uint32_t*)0x0038E744 = 0;           // cached VXT texture pointer
    *(uint32_t*)0x0038E748 = 99;          // cached VXT sub-index
    *(uint32_t*)0x0038E74C = 99;          // cached VXT format flag
    *(uint32_t*)0x0038E750 = 0xFFFFFFFFu; // cached blend mode
    *(uint32_t*)0x0015E458 = 0xFFFFFFFFu; // cached D3D Stage 0 texture
    *(uint32_t*)0x0038DEB0 = 0xFFFFFFFFu; // cached Print palette index
    *(uint32_t*)0x0038DEB4 = 0xFFFFFFFFu; // cached Print color index
}

// RAII Guard to save and restore Direct3D 9 render & texture stage states across HLE primitive calls
class D3D9StateGuard {
public:
    explicit D3D9StateGuard(IDirect3DDevice9* dev) : m_dev(dev) {
        if (!m_dev) return;
        m_dev->GetRenderState(D3DRS_ALPHATESTENABLE, &m_alphaTest);
        m_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &m_alphaBlend);
        m_dev->GetRenderState(D3DRS_SRCBLEND, &m_srcBlend);
        m_dev->GetRenderState(D3DRS_DESTBLEND, &m_destBlend);
        m_dev->GetRenderState(D3DRS_BLENDOP, &m_blendOp);
        m_dev->GetRenderState(D3DRS_ZENABLE, &m_zEnable);
        m_dev->GetRenderState(D3DRS_ZFUNC, &m_zFunc);
        m_dev->GetRenderState(D3DRS_ZWRITEENABLE, &m_zWrite);
        m_dev->GetRenderState(D3DRS_CULLMODE, &m_cullMode);
        m_dev->GetRenderState(D3DRS_LIGHTING, &m_lighting);
        m_dev->GetRenderState(D3DRS_FOGENABLE, &m_fogEnable);
        m_dev->GetRenderState(D3DRS_SHADEMODE, &m_shadeMode);
        m_dev->GetRenderState(D3DRS_COLORWRITEENABLE, &m_colorWrite);
        m_dev->GetRenderState(D3DRS_STENCILENABLE, &m_stencilEnable);
        m_dev->GetRenderState(D3DRS_STENCILFUNC, &m_stencilFunc);
        m_dev->GetRenderState(D3DRS_STENCILREF, &m_stencilRef);
        m_dev->GetRenderState(D3DRS_STENCILMASK, &m_stencilMask);
        m_dev->GetRenderState(D3DRS_STENCILWRITEMASK, &m_stencilWriteMask);
        m_dev->GetRenderState(D3DRS_STENCILFAIL, &m_stencilFail);
        m_dev->GetRenderState(D3DRS_STENCILZFAIL, &m_stencilZFail);
        m_dev->GetRenderState(D3DRS_STENCILPASS, &m_stencilPass);
        m_dev->GetTextureStageState(0, D3DTSS_COLOROP, &m_colorOp0);
        m_dev->GetTextureStageState(0, D3DTSS_COLORARG1, &m_colorArg1_0);
        m_dev->GetTextureStageState(0, D3DTSS_COLORARG2, &m_colorArg2_0);
        m_dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &m_alphaOp0);
        m_dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &m_alphaArg1_0);
        m_dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &m_alphaArg2_0);
    }

    ~D3D9StateGuard() {
        if (!m_dev) return;
        m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, m_alphaTest);
        m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, m_alphaBlend);
        m_dev->SetRenderState(D3DRS_SRCBLEND, m_srcBlend);
        m_dev->SetRenderState(D3DRS_DESTBLEND, m_destBlend);
        m_dev->SetRenderState(D3DRS_BLENDOP, m_blendOp);
        m_dev->SetRenderState(D3DRS_ZENABLE, m_zEnable);
        m_dev->SetRenderState(D3DRS_ZFUNC, m_zFunc);
        m_dev->SetRenderState(D3DRS_ZWRITEENABLE, m_zWrite);
        m_dev->SetRenderState(D3DRS_CULLMODE, m_cullMode);
        m_dev->SetRenderState(D3DRS_LIGHTING, m_lighting);
        m_dev->SetRenderState(D3DRS_FOGENABLE, m_fogEnable);
        m_dev->SetRenderState(D3DRS_SHADEMODE, m_shadeMode);
        m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, m_colorWrite);
        m_dev->SetRenderState(D3DRS_STENCILENABLE, m_stencilEnable);
        m_dev->SetRenderState(D3DRS_STENCILFUNC, m_stencilFunc);
        m_dev->SetRenderState(D3DRS_STENCILREF, m_stencilRef);
        m_dev->SetRenderState(D3DRS_STENCILMASK, m_stencilMask);
        m_dev->SetRenderState(D3DRS_STENCILWRITEMASK, m_stencilWriteMask);
        m_dev->SetRenderState(D3DRS_STENCILFAIL, m_stencilFail);
        m_dev->SetRenderState(D3DRS_STENCILZFAIL, m_stencilZFail);
        m_dev->SetRenderState(D3DRS_STENCILPASS, m_stencilPass);
        m_dev->SetTextureStageState(0, D3DTSS_COLOROP, m_colorOp0);
        m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, m_colorArg1_0);
        m_dev->SetTextureStageState(0, D3DTSS_COLORARG2, m_colorArg2_0);
        m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, m_alphaOp0);
        m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, m_alphaArg1_0);
        m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, m_alphaArg2_0);
    }

private:
    IDirect3DDevice9* m_dev;
    DWORD m_alphaTest = 0, m_alphaBlend = 0, m_srcBlend = 0, m_destBlend = 0, m_blendOp = 0;
    DWORD m_zEnable = 0, m_zFunc = 0, m_zWrite = 0, m_cullMode = 0, m_lighting = 0;
    DWORD m_fogEnable = 0, m_shadeMode = 0, m_colorWrite = 0x0F;
    DWORD m_stencilEnable = 0, m_stencilFunc = 0, m_stencilRef = 0, m_stencilMask = 0, m_stencilWriteMask = 0;
    DWORD m_stencilFail = 0, m_stencilZFail = 0, m_stencilPass = 0;
    DWORD m_colorOp0 = 0, m_colorArg1_0 = 0, m_colorArg2_0 = 0;
    DWORD m_alphaOp0 = 0, m_alphaArg1_0 = 0, m_alphaArg2_0 = 0;
};

static inline void SetupUntexturedStage(IDirect3DDevice9* dev) {
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

static inline void SetupTexturedStageModulate2X(IDirect3DDevice9* dev) {
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    dev->SetRenderState(D3DRS_ALPHAREF, 0x00);
}

// ---------------------------------------------------------------------------
// 1. Slot [0]: Line2D (0x0002EBD0) - 2-Point 2D Line
// ---------------------------------------------------------------------------
static void __cdecl Hook_Line2D(const float* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();

    uint32_t c0 = *(const uint32_t*)&p[8];
    uint32_t c1 = *(const uint32_t*)&p[9];
    uint32_t blendMode = *(const uint32_t*)&p[10];

    D3D9_SetTexture(0, nullptr);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[2];
    v[0].x = p[0]; v[0].y = p[1]; v[0].z = ProjectNamcoZ(p[2]); v[0].color = c0; v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].x = p[4]; v[1].y = p[5]; v[1].z = ProjectNamcoZ(p[6]); v[1].color = c1; v[1].u = 0.0f; v[1].v = 0.0f;

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0 || *(const uint32_t*)0x00397F40 == 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        SetupUntexturedStage(dev);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 2;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 2. Slot [1]: Line3D (0x0002F750) - 3D Line
// ---------------------------------------------------------------------------
static void __cdecl Hook_Line3D(const float* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();
    uint32_t blendMode = *(const uint32_t*)&p[8];

    D3D9_SetTexture(0, nullptr);

    const float* pMatrix = nullptr;
    uint32_t ctx = *(const uint32_t*)0x0038E520;
    if (ctx >= 0x00010000) {
        pMatrix = *(const float**)(ctx + 0x68C);
    }
    D3D9_SetXboxCompositeMatrix(pMatrix, false);

    D3D9Vertex3D v[2];
    v[0].x = p[0]; v[0].y = p[1]; v[0].z = p[2]; v[0].color = *(const uint32_t*)&p[3]; v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].x = p[4]; v[1].y = p[5]; v[1].z = p[6]; v[1].color = *(const uint32_t*)&p[7]; v[1].u = 0.0f; v[1].v = 0.0f;

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        SetupUntexturedStage(dev);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 2;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 3. Slot [2]: Box2D (0x0002EC90) - Untextured 2D Quad (Beveled Borders & Boxes)
//    Note: In immediate Early-Z mode (0x00397F40 == 0, _DAT_0015E490 == 1), Box2D
//    projects Z via FUN_0002b940 (ProjectNamcoZ) and writes to the depth buffer
//    so 3D stage/characters rendered later in FUN_000152e0 are occluded behind it.
// ---------------------------------------------------------------------------
static void __cdecl Hook_Box2D(const float* p) {
    if (!p) return;
    uint32_t finalColor = *(const uint32_t*)&p[16];
    uint32_t blendMode = *(const uint32_t*)&p[17];

    static int s_box2dLog = 0;
    if (s_box2dLog++ < 30 || (s_box2dLog % 300 == 0)) {
        logFmt("[HLE_BOX2D] (%.1f,%.1f)-(%.1f,%.1f) color=0x%08X blend=%u z=%.4f->%.4f\n",
            p[0], p[1], p[8], p[9], finalColor, blendMode, p[2], ProjectNamcoZ(p[2]));
    }
    D3D9_FlushPushBuffer();

    D3D9_SetTexture(0, nullptr);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[4];
    v[0].x = p[0];  v[0].y = p[1];  v[0].z = ProjectNamcoZ(p[2]);  v[0].color = finalColor; v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].x = p[4];  v[1].y = p[5];  v[1].z = ProjectNamcoZ(p[6]);  v[1].color = finalColor; v[1].u = 0.0f; v[1].v = 0.0f;
    v[2].x = p[8];  v[2].y = p[9];  v[2].z = ProjectNamcoZ(p[10]); v[2].color = finalColor; v[2].u = 0.0f; v[2].v = 0.0f;
    v[3].x = p[12]; v[3].y = p[13]; v[3].z = ProjectNamcoZ(p[14]); v[3].color = finalColor; v[3].u = 0.0f; v[3].v = 0.0f;

    static const uint16_t s_quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        // Strictly honor blendMode (in FUN_00025770, blendMode == 0 sets alpha byte to 0x80 for opaque boxes!)
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0 || *(const uint32_t*)0x00397F40 == 0 || blendMode == 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        SetupUntexturedStage(dev);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_quadIdx, D3DFMT_INDEX16, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 4;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 4. Slot [3]: Strip2DG (0x0002ED80) - Untextured N-Vertex 2D Gradient Strip
//    Used by Pause Menu (Menu00 @ 0x000B8360), Bottom Help Box (HBOX @ 0x000B6010),
//    Attack Data Box (0x00013FC0), and screen fades.
// ---------------------------------------------------------------------------
static void __cdecl Hook_Strip2DG(const float* p) {
    if (!p) return;
    uint32_t blendMode = *(const uint32_t*)&p[0];
    uint32_t vtxCount = *(const uint32_t*)&p[1] & 0x0FFFFFFFu;
    if (vtxCount < 3 || vtxCount > 2048) return;

    const float* vtxSrc = p + 4;
    static int s_strip2dgLog = 0;
    if (s_strip2dgLog++ < 30 || (s_strip2dgLog % 300 == 0)) {
        logFmt("[HLE_STRIP2DG] count=%u blend=%u z=%.4f->%.4f v0=(%.1f,%.1f,c=0x%08X) v1=(%.1f,%.1f,c=0x%08X)\n",
            vtxCount, blendMode, vtxSrc[2], ProjectNamcoZ(vtxSrc[2]),
            vtxSrc[0], vtxSrc[1], *(const uint32_t*)&vtxSrc[3],
            vtxSrc[4], vtxSrc[5], *(const uint32_t*)&vtxSrc[7]);
    }
    D3D9_FlushPushBuffer();

    D3D9_SetTexture(0, nullptr);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[2048];
    for (uint32_t i = 0; i < vtxCount; i++) {
        v[i].x = vtxSrc[i * 4 + 0];
        v[i].y = vtxSrc[i * 4 + 1];
        v[i].z = ProjectNamcoZ(vtxSrc[i * 4 + 2]);
        v[i].color = *(const uint32_t*)&vtxSrc[i * 4 + 3];
        v[i].u = 0.0f;
        v[i].v = 0.0f;
    }

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        // Strictly honor blendMode (Pause Menu and HBOX pass blendMode == 0 for Early-Z occlusion)
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0 || *(const uint32_t*)0x00397F40 == 0 || blendMode == 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        SetupUntexturedStage(dev);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, vtxCount - 2, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += vtxCount;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 5. Slot [8]: Sprite2D (0x0002AA30) - Textured Axis-Aligned 2D Quad
// ---------------------------------------------------------------------------
static void __cdecl Hook_Sprite2D(const float* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();

    float x = p[0];
    float y = p[1];
    float z = ClampZ(p[2]);
    float hw = p[12];
    float hh = p[13];
    uint32_t flags = *(const uint32_t*)&p[18];

    // Native anamorphic 16:9 aspect adjustment when widescreen mode is enabled (FUN_00020250 returns uint8_t in AL)
    int8_t flag49 = *((const int8_t*)p + 0x49);
    uint8_t isWide = ((uint8_t(__cdecl*)(void))0x00020250)();
    if (isWide && flag49 != 0) {
        if (flag49 < 0) {
            x = (x - 320.0f) * 0.8055556f + 320.0f;
        }
        hw *= 0.8055556f;
    }

    float x0, x1, y0, y1;
    if ((flags & 0x30) == 0) {
        x0 = x - hw; x1 = x + hw;
        y0 = y - hh; y1 = y + hh;
    } else if ((flags & 0x30) == 0x10) {
        x0 = x;      x1 = x + hw;
        y0 = y;      y1 = y + hh;
    } else {
        x0 = x - hw; x1 = x;
        y0 = y;      y1 = y + hh;
    }

    uint32_t finalColor = *(const uint32_t*)&p[17];
    uint32_t texParam = *(const uint32_t*)((const uint8_t*)p + 0x3c);
    uint32_t blendMode = *(const uint32_t*)((const uint8_t*)p + 0x40);
    uint8_t flag4A = *((const uint8_t*)p + 0x4a);
    uint8_t pal = *((const uint8_t*)p + 0x4c);
    uint8_t fmt = *((const uint8_t*)p + 0x4d);

    // High-Res HLE X-Fade (0x00028f60) full-screen screenshot cross-fade:
    // When texParam references the built-in screenshot VXT header (0x0019BFC0 / *0x0038F984),
    // bind the full-resolution GPU capture texture (g_pXFadeTexture) and render 1:1 across
    // the entire 16:9 backbuffer (up to 4K) with normalized UVs and diffuse-driven alpha.
    if (texParam == 0x0019BFC0 || (texParam != 0 && texParam == *(const uint32_t*)0x0038F984)) {
        IDirect3DTexture9* pXFadeTex = D3D9_GetXFadeTexture();
        IDirect3DDevice9* dev = D3D9_GetDevice();
        if (!pXFadeTex || !dev) return;

        float bbW = (float)D3D9_GetBackBufferWidth();
        float bbH = (float)D3D9_GetBackBufferHeight();
        D3D9Vertex2D xv[4] = {
            { -0.5f,       -0.5f,       z, 1.0f, finalColor, 0.0f, 0.0f },
            { bbW - 0.5f,  -0.5f,       z, 1.0f, finalColor, 1.0f, 0.0f },
            { bbW - 0.5f,  bbH - 0.5f,  z, 1.0f, finalColor, 1.0f, 1.0f },
            { -0.5f,       bbH - 0.5f,  z, 1.0f, finalColor, 0.0f, 1.0f }
        };
        static const uint16_t s_xfadeIdx[6] = { 0, 1, 2, 0, 2, 3 };

        D3D9StateGuard guard(dev);
        D3D9_SetTexture(0, pXFadeTex);
        D3D9_ApplyNamcoBlendMode(blendMode);
        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        SetupTexturedStageModulate2X(dev);
        // Backbuffer capture may have undefined texture alpha; drive cross-fade opacity from diffuse alpha * 2
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_ADD);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_ZENABLE, (flag4A & 0x40) ? D3DZB_FALSE : D3DZB_TRUE);
        dev->SetFVF(D3DFVF_V2D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_xfadeIdx, D3DFMT_INDEX16, xv, sizeof(D3D9Vertex2D));
        g_drawCount++;
        g_vertexCount += 4;
        InvalidateNamcoStateCache();
        return;
    }

    IDirect3DTexture9* pTex = D3D9_ResolveNamcoTexture(texParam, pal, fmt);
    if (!pTex) return;
    D3D9_SetTexture(0, pTex);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[4];
    v[0].x = x0; v[0].y = y0; v[0].z = z; v[0].color = finalColor; v[0].u = p[4];  v[0].v = p[5];
    v[1].x = x1; v[1].y = y0; v[1].z = z; v[1].color = finalColor; v[1].u = p[6];  v[1].v = p[7];
    v[2].x = x1; v[2].y = y1; v[2].z = z; v[2].color = finalColor; v[2].u = p[8];  v[2].v = p[9];
    v[3].x = x0; v[3].y = y1; v[3].z = z; v[3].color = finalColor; v[3].u = p[10]; v[3].v = p[11];

    static const uint16_t s_quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        SetupTexturedStageModulate2X(dev);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_ZENABLE, (flag4A & 0x40) ? D3DZB_FALSE : D3DZB_TRUE);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_quadIdx, D3DFMT_INDEX16, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 4;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 6. Slot [9]: Sprite2DR (0x0002ABC0) - Rotated 2D Quad
//    Uses the engine's native Bam angle lookup function FUN_00011000 (0x00011000)
// ---------------------------------------------------------------------------
static void __cdecl Hook_Sprite2DR(const float* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();

    float x = p[0];
    float y = p[1];
    float z = ClampZ(p[2]);
    float hw = p[12];
    float hh = p[13];
    float angle = p[14];

    // Call native FUN_00011000(float angle, float* outSin, float* outCos)
    float sinA = 0.0f, cosA = 1.0f;
    typedef void (__cdecl *PFN_SinCos)(float, float*, float*);
    ((PFN_SinCos)0x00011000)(angle, &sinA, &cosA);

    float sin_hh = sinA * hh;
    float cos_hh = cosA * hh;
    float sin_hw = sinA * hw;
    float cos_hw = cosA * hw;

    int8_t flag49 = *((const int8_t*)p + 0x49);
    uint8_t isWide = ((uint8_t(__cdecl*)(void))0x00020250)();
    if (isWide && flag49 != 0) {
        if (flag49 < 0) {
            x = (x - 320.0f) * 0.8055556f + 320.0f;
        }
        cos_hw *= 0.8055556f;
        sin_hh *= 0.8055556f;
    }

    float vx0 = (x - cos_hw) - sin_hh;
    float vx1 = (x + cos_hw) - sin_hh;
    float vx2 = (x + cos_hw) + sin_hh;
    float vx3 = (x - cos_hw) + sin_hh;

    float vy0 = (y + sin_hw) - cos_hh;
    float vy1 = (y - sin_hw) - cos_hh;
    float vy2 = (y - sin_hw) + cos_hh;
    float vy3 = (y + sin_hw) + cos_hh;

    uint32_t finalColor = *(const uint32_t*)&p[17];
    uint32_t texParam = *(const uint32_t*)((const uint8_t*)p + 0x3c);
    uint32_t blendMode = *(const uint32_t*)((const uint8_t*)p + 0x40);
    uint8_t flag4A = *((const uint8_t*)p + 0x4a);
    uint8_t pal = *((const uint8_t*)p + 0x4c);
    uint8_t fmt = *((const uint8_t*)p + 0x4d);

    // High-Res HLE X-Fade (0x00028f60) full-screen screenshot cross-fade:
    // When texParam references the built-in screenshot VXT header (0x0019BFC0 / *0x0038F984),
    // bind the full-resolution GPU capture texture (g_pXFadeTexture) and render 1:1 across
    // the entire 16:9 backbuffer (up to 4K) with normalized UVs and diffuse-driven alpha.
    if (texParam == 0x0019BFC0 || (texParam != 0 && texParam == *(const uint32_t*)0x0038F984)) {
        IDirect3DTexture9* pXFadeTex = D3D9_GetXFadeTexture();
        IDirect3DDevice9* dev = D3D9_GetDevice();
        if (!pXFadeTex || !dev) return;

        float bbW = (float)D3D9_GetBackBufferWidth();
        float bbH = (float)D3D9_GetBackBufferHeight();
        D3D9Vertex2D xv[4] = {
            { -0.5f,       -0.5f,       z, 1.0f, finalColor, 0.0f, 0.0f },
            { bbW - 0.5f,  -0.5f,       z, 1.0f, finalColor, 1.0f, 0.0f },
            { bbW - 0.5f,  bbH - 0.5f,  z, 1.0f, finalColor, 1.0f, 1.0f },
            { -0.5f,       bbH - 0.5f,  z, 1.0f, finalColor, 0.0f, 1.0f }
        };
        static const uint16_t s_xfadeIdx[6] = { 0, 1, 2, 0, 2, 3 };

        D3D9StateGuard guard(dev);
        D3D9_SetTexture(0, pXFadeTex);
        D3D9_ApplyNamcoBlendMode(blendMode);
        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        SetupTexturedStageModulate2X(dev);
        // Backbuffer capture may have undefined texture alpha; drive cross-fade opacity from diffuse alpha * 2
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_ADD);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_ZENABLE, (flag4A & 0x40) ? D3DZB_FALSE : D3DZB_TRUE);
        dev->SetFVF(D3DFVF_V2D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_xfadeIdx, D3DFMT_INDEX16, xv, sizeof(D3D9Vertex2D));
        g_drawCount++;
        g_vertexCount += 4;
        InvalidateNamcoStateCache();
        return;
    }

    IDirect3DTexture9* pTex = D3D9_ResolveNamcoTexture(texParam, pal, fmt);
    if (!pTex) return;
    D3D9_SetTexture(0, pTex);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[4];
    v[0].x = vx0; v[0].y = vy0; v[0].z = z; v[0].color = finalColor; v[0].u = p[4];  v[0].v = p[5];
    v[1].x = vx1; v[1].y = vy1; v[1].z = z; v[1].color = finalColor; v[1].u = p[6];  v[1].v = p[7];
    v[2].x = vx2; v[2].y = vy2; v[2].z = z; v[2].color = finalColor; v[2].u = p[8];  v[2].v = p[9];
    v[3].x = vx3; v[3].y = vy3; v[3].z = z; v[3].color = finalColor; v[3].u = p[10]; v[3].v = p[11];

    static const uint16_t s_quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        SetupTexturedStageModulate2X(dev);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_ZENABLE, (flag4A & 0x40) ? D3DZB_FALSE : D3DZB_TRUE);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_quadIdx, D3DFMT_INDEX16, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 4;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 7. Slot [10]: Sprite3D (0x0002C8F0) - Pre-projected 3D Billboarding Quad
// ---------------------------------------------------------------------------
static void __cdecl Hook_Sprite3D(const float* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();

    float x = p[0];
    float y = p[1];
    float z = ProjectNamcoZ(p[2]);
    float hw = p[4];
    float hh = p[5];
    uint32_t finalColor = *(const uint32_t*)&p[16];

    uint32_t texParam = *(const uint32_t*)((const uint8_t*)p + 0x44);
    uint32_t blendMode = *(const uint32_t*)((const uint8_t*)p + 0x48);
    uint8_t pal = *((const uint8_t*)p + 0x4c);
    uint8_t fmt = *((const uint8_t*)p + 0x4d);

    IDirect3DTexture9* pTex = D3D9_ResolveNamcoTexture(texParam, pal, fmt);
    if (!pTex) return;
    D3D9_SetTexture(0, pTex);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[4];
    v[0].x = x - hw; v[0].y = y - hh; v[0].z = z; v[0].color = finalColor; v[0].u = p[8];  v[0].v = p[9];
    v[1].x = x + hw; v[1].y = y - hh; v[1].z = z; v[1].color = finalColor; v[1].u = p[10]; v[1].v = p[11];
    v[2].x = x + hw; v[2].y = y + hh; v[2].z = z; v[2].color = finalColor; v[2].u = p[12]; v[2].v = p[13];
    v[3].x = x - hw; v[3].y = y + hh; v[3].z = z; v[3].color = finalColor; v[3].u = p[14]; v[3].v = p[15];

    static const uint16_t s_quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        SetupTexturedStageModulate2X(dev);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_quadIdx, D3DFMT_INDEX16, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 4;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 8. Slot [11]: Sprite3DR (0x0002CA40) - Rotated Pre-projected 3D Billboarding Quad
//    Note: In 0x0002CA40, p[4..7] hold (cos*hw, sin*hw, cos*hh, sin*hh) from FUN_0002d2b0/0002d5d0!
// ---------------------------------------------------------------------------
static void __cdecl Hook_Sprite3DR(const float* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();

    float x = p[0];
    float y = p[1];
    float z = ProjectNamcoZ(p[2]);
    float f1 = p[4]; // cos * hw
    float f2 = p[5]; // sin * hw
    float f3 = p[6]; // cos * hh
    float f4 = p[7]; // sin * hh
    uint32_t finalColor = *(const uint32_t*)&p[16];

    uint32_t texParam = *(const uint32_t*)((const uint8_t*)p + 0x44);
    uint32_t blendMode = *(const uint32_t*)((const uint8_t*)p + 0x48);
    uint8_t pal = *((const uint8_t*)p + 0x4c);
    uint8_t fmt = *((const uint8_t*)p + 0x4d);

    IDirect3DTexture9* pTex = D3D9_ResolveNamcoTexture(texParam, pal, fmt);
    if (!pTex) return;
    D3D9_SetTexture(0, pTex);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[4];
    v[0].x = (x - f1) - f4; v[0].y = (y + f2) - f3; v[0].z = z; v[0].color = finalColor; v[0].u = p[8];  v[0].v = p[9];
    v[1].x = (x + f1) - f4; v[1].y = (y - f2) - f3; v[1].z = z; v[1].color = finalColor; v[1].u = p[10]; v[1].v = p[11];
    v[2].x = (x + f1) + f4; v[2].y = (y - f2) + f3; v[2].z = z; v[2].color = finalColor; v[2].u = p[12]; v[2].v = p[13];
    v[3].x = (x - f1) + f4; v[3].y = (y + f2) + f3; v[3].z = z; v[3].color = finalColor; v[3].u = p[14]; v[3].v = p[15];

    static const uint16_t s_quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        SetupTexturedStageModulate2X(dev);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_quadIdx, D3DFMT_INDEX16, v, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 4;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 9. Slot [12]: Strip2D (0x0002AD90) - 2D Textured Triangle Strips
// ---------------------------------------------------------------------------
static void __cdecl Hook_Strip2D(const uint8_t* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();

    const float* pMatrix = *(const float**)(p + 0x04);
    uint32_t texParam = *(const uint32_t*)(p + 0x08);
    uint16_t palParam = *(const uint16_t*)(p + 0x0c);
    uint16_t fmtFlag = *(const uint16_t*)(p + 0x0e);
    int stripCount = *(const int*)(p + 0x10);
    uint32_t blendMode = *(const uint32_t*)(p + 0x18);
    uint8_t shadeFlag = p[0x1e];
    const uint8_t* pStrip = p + 0xa0;

    if (stripCount <= 0) return;

    IDirect3DTexture9* pTex = D3D9_ResolveNamcoTexture(texParam, palParam, fmtFlag);
    if (!pTex) return;
    D3D9_SetTexture(0, pTex);
    D3D9_SetXboxCompositeMatrix(pMatrix, false);

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (!dev) return;

    {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_SHADEMODE, (shadeFlag == 0) ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
        SetupTexturedStageModulate2X(dev);
        dev->SetFVF(D3DFVF_V3D);

        for (int s = 0; s < stripCount; s++) {
            const float* stripFloats = (const float*)pStrip;
            uint32_t vtxCount = *(const uint32_t*)&stripFloats[6];
            if (vtxCount >= 3 && vtxCount < 2048) {
                D3D9Vertex3D v[2048];
                for (uint32_t i = 0; i < vtxCount; i++) {
                    const float* src = &stripFloats[i * 7];
                    v[i].x = src[0];
                    v[i].y = src[1];
                    v[i].z = ClampZ(src[2]);
                    v[i].color = *(const uint32_t*)&src[3];
                    v[i].u = src[4];
                    v[i].v = src[5];
                }
                dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, vtxCount - 2, v, sizeof(D3D9Vertex3D));
                g_drawCount++;
                g_vertexCount += vtxCount;
            }
            if (vtxCount == 0) break;
            pStrip += vtxCount * 28;
        }
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 10. Slot [13]: Strip2DNT (0x0002AF00) - 2D Non-Textured Triangle Strips
// ---------------------------------------------------------------------------
static void __cdecl Hook_Strip2DNT(const uint8_t* p) {
    if (!p) return;
    int stripCount = *(const int*)(p + 0x10);
    uint32_t blendMode = *(const uint32_t*)(p + 0x18);
    uint8_t shadeFlag = p[0x1e];
    if (stripCount <= 0) return;

    D3D9_FlushPushBuffer();

    const float* pMatrix = *(const float**)(p + 0x04);
    const uint8_t* pStrip = p + 0xa0;

    D3D9_SetTexture(0, nullptr);
    D3D9_SetXboxCompositeMatrix(pMatrix, false);

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (!dev) return;

    {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        SetupUntexturedStage(dev);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_SHADEMODE, (shadeFlag == 0) ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
        dev->SetFVF(D3DFVF_V3D);

        for (int s = 0; s < stripCount; s++) {
            const float* stripFloats = (const float*)pStrip;
            uint32_t vtxCount = *(const uint32_t*)&stripFloats[6];
            if (vtxCount >= 3 && vtxCount < 2048) {
                D3D9Vertex3D v[2048];
                for (uint32_t i = 0; i < vtxCount; i++) {
                    const float* src = &stripFloats[i * 7];
                    v[i].x = src[0];
                    v[i].y = src[1];
                    v[i].z = ClampZ(src[2]);
                    v[i].color = *(const uint32_t*)&src[3];
                    v[i].u = 0.0f;
                    v[i].v = 0.0f;
                }
                dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, vtxCount - 2, v, sizeof(D3D9Vertex3D));
                g_drawCount++;
                g_vertexCount += vtxCount;
            }
            if (vtxCount == 0) break;
            pStrip += vtxCount * 28;
        }
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 11. Slot [14]: Strip3D (0x0002B050) - 3D Textured Triangle Strips
// ---------------------------------------------------------------------------
static void __cdecl Hook_Strip3D(const uint8_t* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();

    const float* pMatrix = *(const float**)(p + 0x04);
    uint32_t texParam = *(const uint32_t*)(p + 0x08);
    uint16_t palParam = *(const uint16_t*)(p + 0x0c);
    uint16_t fmtFlag = *(const uint16_t*)(p + 0x0e);
    int stripCount = *(const int*)(p + 0x10);
    float zParam = *(const float*)(p + 0x14);
    uint32_t blendMode = *(const uint32_t*)(p + 0x18);
    uint8_t zModeFlag = p[0x1c];
    uint8_t shadeFlag = p[0x1e];
    const uint8_t* pStrip = p + 0xb0;

    if (stripCount <= 0) return;

    IDirect3DTexture9* pTex = D3D9_ResolveNamcoTexture(texParam, palParam, fmtFlag);
    if (!pTex) return;
    D3D9_SetTexture(0, pTex);

    float biasedMat[16];
    if (pMatrix && (uint32_t)pMatrix >= 0x00010000 && zModeFlag == 0 && zParam > 0.0f && zParam != 1.0f) {
        memcpy(biasedMat, pMatrix, sizeof(biasedMat));
        biasedMat[2]  *= zParam;
        biasedMat[6]  *= zParam;
        biasedMat[10] *= zParam;
        biasedMat[14] *= zParam;
        D3D9_SetXboxCompositeMatrix(biasedMat, false);
    } else {
        D3D9_SetXboxCompositeMatrix(pMatrix, false);
    }

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (!dev) return;

    {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_SHADEMODE, (shadeFlag == 0) ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
        SetupTexturedStageModulate2X(dev);
        dev->SetFVF(D3DFVF_V3D);

        for (int s = 0; s < stripCount; s++) {
            const float* stripFloats = (const float*)pStrip;
            uint32_t vtxCount = *(const uint32_t*)&stripFloats[6];
            if (vtxCount >= 3 && vtxCount < 2048) {
                D3D9Vertex3D v[2048];
                for (uint32_t i = 0; i < vtxCount; i++) {
                    const float* src = &stripFloats[i * 7];
                    v[i].x = src[0];
                    v[i].y = src[1];
                    v[i].z = src[2];
                    v[i].color = *(const uint32_t*)&src[3];
                    v[i].u = src[4];
                    v[i].v = src[5];
                }
                if (shadeFlag == 0) {
                    for (uint32_t i = 2; i < vtxCount; i++) {
                        v[i - 2].color = v[i].color;
                    }
                }
                dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, vtxCount - 2, v, sizeof(D3D9Vertex3D));
                g_drawCount++;
                g_vertexCount += vtxCount;
            }
            if (vtxCount == 0) break;
            pStrip += vtxCount * 28;
        }
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 12. Slot [15]: Strip3DNT (0x0002B2F0) - 3D Non-Textured Triangle Strips
// ---------------------------------------------------------------------------
static void __cdecl Hook_Strip3DNT(const uint8_t* p) {
    if (!p) return;
    int stripCount = *(const int*)(p + 0x10);
    if (stripCount <= 0) return;

    D3D9_FlushPushBuffer();

    const float* pMatrix = *(const float**)(p + 0x04);
    float zParam = *(const float*)(p + 0x14);
    uint32_t blendMode = *(const uint32_t*)(p + 0x18);
    uint8_t zModeFlag = p[0x1c];
    uint8_t shadeFlag = p[0x1e];
    const uint8_t* pStrip = p + 0xb0;

    D3D9_SetTexture(0, nullptr);

    float biasedMat[16];
    if (pMatrix && (uint32_t)pMatrix >= 0x00010000 && zModeFlag == 0 && zParam > 0.0f && zParam != 1.0f) {
        memcpy(biasedMat, pMatrix, sizeof(biasedMat));
        biasedMat[2]  *= zParam;
        biasedMat[6]  *= zParam;
        biasedMat[10] *= zParam;
        biasedMat[14] *= zParam;
        D3D9_SetXboxCompositeMatrix(biasedMat, false);
    } else {
        D3D9_SetXboxCompositeMatrix(pMatrix, false);
    }

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (!dev) return;

    {
        D3D9StateGuard guard(dev);
        D3D9_ApplyNamcoBlendMode(blendMode);

        BOOL zWrite = (*(const uint32_t*)0x0015E490 != 0) ? TRUE : FALSE;
        D3D9_BeginScene();
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        SetupUntexturedStage(dev);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
        dev->SetRenderState(D3DRS_SHADEMODE, (shadeFlag == 0) ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
        dev->SetFVF(D3DFVF_V3D);

        for (int s = 0; s < stripCount; s++) {
            const float* stripFloats = (const float*)pStrip;
            uint32_t vtxCount = *(const uint32_t*)&stripFloats[6];
            if (vtxCount >= 3 && vtxCount < 2048) {
                D3D9Vertex3D v[2048];
                for (uint32_t i = 0; i < vtxCount; i++) {
                    const float* src = &stripFloats[i * 7];
                    v[i].x = src[0];
                    v[i].y = src[1];
                    v[i].z = src[2];
                    v[i].color = *(const uint32_t*)&src[3];
                    v[i].u = 0.0f;
                    v[i].v = 0.0f;
                }
                if (shadeFlag == 0) {
                    for (uint32_t i = 2; i < vtxCount; i++) {
                        v[i - 2].color = v[i].color;
                    }
                }
                dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, vtxCount - 2, v, sizeof(D3D9Vertex3D));
                g_drawCount++;
                g_vertexCount += vtxCount;
            }
            if (vtxCount == 0) break;
            pStrip += vtxCount * 28;
        }
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 12b. Slot [16]: StripVOL (0x0002B670) & Shadow Quad (0x0002B4F0)
//      Two-pass Stencil Modifier Volume Shadow extrusion & fullscreen shadow quad
// ---------------------------------------------------------------------------
static void DrawStripVOLSubStrips(IDirect3DDevice9* dev, uint32_t vertBase, uint32_t totalVerts) {
    uint32_t curAddr = vertBase;
    uint32_t rem = totalVerts;
    while (rem > 0) {
        uint32_t subCount = 0;
        const int32_t* pFlag = (const int32_t*)(curAddr + 0x0C);
        do {
            if (*pFlag != 0 && subCount > 1) break;
            pFlag += 16; // 0x40 bytes per vertex
            subCount++;
        } while (subCount < rem);

        if (subCount >= 3 && subCount <= 2048) {
            D3D9Vertex3D v[2048];
            for (uint32_t i = 0; i < subCount; i++) {
                const float* pPos = (const float*)(curAddr + i * 0x40 + 0x10);
                v[i].x = pPos[0];
                v[i].y = pPos[1];
                v[i].z = pPos[2];
                v[i].color = 0xFFFFFFFFu;
                v[i].u = 0.0f;
                v[i].v = 0.0f;
            }
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, subCount - 2, v, sizeof(D3D9Vertex3D));
            g_drawCount++;
            g_vertexCount += subCount;
        }
        curAddr += subCount * 0x40;
        rem -= subCount;
    }
}

static void __cdecl Hook_StripVOL(const uint32_t* param_1) {
    if (!param_1) return;
    uint32_t taskAddr = param_1[0];
    if (taskAddr < 0x00010000) return;

    uint16_t totalVerts = *(const uint16_t*)(taskAddr + 0x58);
    uint32_t vertBase = *(const uint32_t*)(taskAddr + 0x5c);
    if (totalVerts < 3 || vertBase < 0x00010000) return;

    D3D9_FlushPushBuffer();

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (!dev) return;

    uint32_t curRef = *(uint32_t*)0x0038E4EC;
    if (curRef == 0xFF) {
        // First shadow volume capsule after MdfV-Stt (0x000613C0): ensure stencil buffer is 0!
        dev->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 0.0f, 0);
    }
    if (curRef > 1) {
        curRef = (curRef - 1) & 0xFF;
        *(uint32_t*)0x0038E4EC = curRef;
    }

    static int s_stripVolLog = 0;
    if (s_stripVolLog++ < 20 || (s_stripVolLog % 600 == 0)) {
        logFmt("[HLE_STRIPVOL] task=0x%08X verts=%u vertBase=0x%08X stencilRef=0x%02X\n",
            taskAddr, totalVerts, vertBase, curRef);
    }

    const float* pMatrix = nullptr;
    uint32_t ctx = *(const uint32_t*)0x0038E520;
    if (ctx >= 0x00010000) {
        pMatrix = *(const float**)(ctx + 0x68C);
    }
    D3D9_SetTexture(0, nullptr);
    D3D9_SetXboxCompositeMatrix(pMatrix, false);

    {
        D3D9StateGuard guard(dev);
        D3D9_BeginScene();
        SetupUntexturedStage(dev);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        dev->SetRenderState(D3DRS_STENCILREF, curRef);
        dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
        dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
        dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        dev->SetFVF(D3DFVF_V3D);

        // Pass 1: Back faces of shadow volume capsule (Reversed-Z LESSEQUAL: behind scene surface)
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_GREATER);
        dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
        DrawStripVOLSubStrips(dev, vertBase, totalVerts);

        // Pass 2: Front faces of shadow volume capsule (Reversed-Z LESSEQUAL: behind scene surface)
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_ZERO);
        DrawStripVOLSubStrips(dev, vertBase, totalVerts);
    }
    InvalidateNamcoStateCache();
}

static void __cdecl Hook_DrawShadowQuad(float x, float y, float w, float h, uint32_t color) {
    D3D9_FlushPushBuffer();

    if (w <= 1.0f || h <= 1.0f) {
        x = 0.0f;
        y = 0.0f;
        w = 640.0f;
        h = 448.0f;
    }

    static int s_shadowQuadLog = 0;
    if (s_shadowQuadLog++ < 20 || (s_shadowQuadLog % 300 == 0)) {
        logFmt("[HLE_SHADOW_QUAD] (%.1f,%.1f) size=(%.1f,%.1f) color=0x%08X\n", x, y, w, h, color);
    }

    D3D9_SetTexture(0, nullptr);
    D3D9_SetXboxCompositeMatrix(nullptr, false);

    D3D9Vertex3D v[4];
    v[0].x = x;     v[0].y = y;     v[0].z = 1.0f; v[0].color = color; v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].x = x + w; v[1].y = y;     v[1].z = 1.0f; v[1].color = color; v[1].u = 0.0f; v[1].v = 0.0f;
    v[2].x = x + w; v[2].y = y + h; v[2].z = 1.0f; v[2].color = color; v[2].u = 0.0f; v[2].v = 0.0f;
    v[3].x = x;     v[3].y = y + h; v[3].z = 1.0f; v[3].color = color; v[3].u = 0.0f; v[3].v = 0.0f;

    static const uint16_t s_quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (dev) {
        {
            D3D9StateGuard guard(dev);
            D3D9_ApplyNamcoBlendMode(1);

            D3D9_BeginScene();
            SetupUntexturedStage(dev);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
            dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
            dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
            dev->SetRenderState(D3DRS_STENCILREF, 0);
            dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
            dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
            dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
            dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
            dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
            dev->SetFVF(D3DFVF_V3D);
            dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_quadIdx, D3DFMT_INDEX16, v, sizeof(D3D9Vertex3D));
            g_drawCount++;
            g_vertexCount += 4;
        }
        // Replicate post-MdfV-End state restoration from FUN_0002b4f0 / FUN_00060f70:
        *(uint32_t*)0x0015E490 = 1;          // ZWRITEENABLE = TRUE
        *(uint32_t*)0x0015E474 = 0x206;      // ZFUNC = GREATEREQUAL
        *(uint32_t*)0x0015E49C = 0x01010101; // COLORWRITEENABLE = 0x0F
        *(uint32_t*)0x0015E4A4 = 0x1E00;     // STENCILPASS = KEEP
        *(uint32_t*)0x0015E4A8 = 0x207;      // STENCILFUNC = ALWAYS
        *(uint32_t*)0x0015E4AC = 0;          // STENCILREF = 0
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// 13. Slot [17]: Print (0x000272C0) - ASCII Text Rendering Wrapper
// ---------------------------------------------------------------------------
static void __cdecl Hook_Print(uint8_t* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();
    uint32_t fontVxt = *(const uint32_t*)0x0038E3E0;
    if (fontVxt >= 0x00010000) {
        InvalidateNamcoStateCache();
        p[3] = 1; // Force re-bind of font texture
    }
    ((void(__cdecl*)(uint8_t*))0x000272C0)(p);
    D3D9_FlushPushBuffer();
}

// ---------------------------------------------------------------------------
// 14. Slot [18]: Kanji (0x000161A0) - Kanji / Localized Text Rendering Wrapper
// ---------------------------------------------------------------------------
static void __cdecl Hook_Kanji(uint32_t* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();
    InvalidateNamcoStateCache();
    ((void(__cdecl*)(uint32_t*))0x000161A0)(p);
    D3D9_FlushPushBuffer();
}

// ---------------------------------------------------------------------------
// 15. Slot [19]: Button (0x0001B150) - Controller Button Icon Wrapper
// ---------------------------------------------------------------------------
static void __cdecl Hook_Button(uint32_t* p) {
    if (!p) return;
    D3D9_FlushPushBuffer();
    InvalidateNamcoStateCache();
    ((void(__cdecl*)(uint32_t*))0x0001B150)(p);
    D3D9_FlushPushBuffer();
}

// ---------------------------------------------------------------------------
// 16. CRI Sofdec Movie Frame Renderer (FUN_0008ffc0 @ 0x0008FFC0)
// ---------------------------------------------------------------------------
// Replaces FUN_0008ffc0 which used Xbox D3DFMT_LIN_A8R8G8B8 (0x12) with
// unnormalized pixel UVs (0..width, 0..height) and D3DFVF_XYZRHW | D3DFVF_TEX1 (0x104).
static IDirect3DTexture9* s_pMovieTex = nullptr;
static uint32_t s_movieTexW = 0;
static uint32_t s_movieTexH = 0;
static bool s_movieTexDirty = false;
static bool s_movieHasValidFrame = false;

static void ConvertPlanarI420ToARGB(const uint8_t* yPlane, uint32_t yStride,
                                    const uint8_t* uPlane, const uint8_t* vPlane, uint32_t uvStride,
                                    uint32_t width, uint32_t height,
                                    uint8_t* dstBytes, uint32_t dstPitchBytes) {
    auto clamp8 = [](int val) -> uint32_t {
        return val < 0 ? 0u : (val > 255 ? 255u : (uint32_t)val);
    };
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* yRow = yPlane + y * yStride;
        const uint8_t* uRow = uPlane + (y >> 1) * uvStride;
        const uint8_t* vRow = vPlane + (y >> 1) * uvStride;
        uint32_t* dstRow = (uint32_t*)(dstBytes + y * dstPitchBytes);
        for (uint32_t x = 0; x < width; x++) {
            int yVal = (int)yRow[x] - 16;
            int uVal = (int)uRow[x >> 1] - 128;
            int vVal = (int)vRow[x >> 1] - 128;
            int c = yVal < 0 ? 0 : yVal * 298;
            uint32_t r = clamp8((c + 409 * vVal + 128) >> 8);
            uint32_t g = clamp8((c - 100 * uVal - 208 * vVal + 128) >> 8);
            uint32_t b = clamp8((c + 516 * uVal + 128) >> 8);
            dstRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static void __cdecl Hook_mwPlyFxCnvFrmARGB8888(uint32_t ply, const uint32_t* frmobj, uint32_t outBufAddr) {
    (void)ply;
    if (!frmobj || (uintptr_t)frmobj < 0x00010000 || outBufAddr < 0x00010000) return;

    uint32_t bufadr  = frmobj[0];
    uint32_t buftype = frmobj[1];
    uint32_t fw      = frmobj[2];
    uint32_t fh      = frmobj[3];
    if (bufadr < 0x00010000 || fw < 16 || fw > 2048 || fh < 16 || fh > 2048) return;

    uint8_t* dstBytes = (uint8_t*)XboxKrnl_GpuToCpu(outBufAddr);
    const uint8_t* srcBase = (const uint8_t*)XboxKrnl_GpuToCpu(bufadr);
    if (!dstBytes || !srcBase) return;

    uint32_t mbW = (fw + 15u) & ~15u;
    uint32_t mbH = (fh + 15u) & ~15u;
    uint32_t yStride = (mbW + 31u) & ~31u;
    uint32_t uvStride = ((mbW / 2u) + 31u) & ~31u;
    const uint8_t* yPlane = srcBase;
    const uint8_t* uPlane = yPlane + yStride * mbH;
    const uint8_t* vPlane = uPlane + uvStride * (mbH / 2u);

    static int s_cnvLog = 0;
    if (s_cnvLog++ < 15 || (s_cnvLog % 60 == 0)) {
        uint32_t nzYRows = 0, actYRows = 0, nzURows = 0, nzVRows = 0;
        for (uint32_t y = 0; y < fh; y++) {
            const uint8_t* r = yPlane + y * yStride;
            bool hasNz = false, hasAct = false;
            for (uint32_t x = 0; x < fw; x += 4) {
                if (r[x] != 0) hasNz = true;
                if (r[x] > 16) hasAct = true;
            }
            if (hasNz) nzYRows++;
            if (hasAct) actYRows++;
        }
        for (uint32_t y = 0; y < fh / 2; y++) {
            const uint8_t* ur = uPlane + y * uvStride;
            const uint8_t* vr = vPlane + y * uvStride;
            bool uNz = false, vNz = false;
            for (uint32_t x = 0; x < fw / 2; x += 4) {
                if (ur[x] != 0 && ur[x] != 128) uNz = true;
                if (vr[x] != 0 && vr[x] != 128) vNz = true;
            }
            if (uNz) nzURows++;
            if (vNz) nzVRows++;
        }
        logFmt("[HLE_MOVIE_CNV] #%d: buf=0x%08X type=%u %ux%u -> out=0x%08X | Y_nz=%u/%u Y_act=%u U_act=%u V_act=%u\n",
               s_cnvLog, bufadr, buftype, fw, fh, outBufAddr, nzYRows, fh, actYRows, nzURows, nzVRows);
    }

    ConvertPlanarI420ToARGB(yPlane, yStride, uPlane, vPlane, uvStride, fw, fh, dstBytes, fw * 4u);
    s_movieTexDirty = true;
    s_movieHasValidFrame = true;
}

static void __cdecl Hook_DrawMovieFrame(uint32_t param_1) {
    if (param_1 < 0x00010000) return;

    D3D9_FlushPushBuffer();

    const float* vtx = (const float*)param_1;
    uint32_t texW = (uint32_t)(vtx[10] + 0.5f);
    uint32_t texH = (uint32_t)(vtx[17] + 0.5f);
    if (texW < 64 || texW > 2048) texW = 640;
    if (texH < 64 || texH > 2048) texH = 448;

    uint32_t bufIdx = *(const uint32_t*)(param_1 + 0x88) & 1u;
    int fmtMode = *(const int*)(param_1 + 0x8C);
    const uint32_t* pD3DTex = (const uint32_t*)(param_1 + 0x60 + bufIdx * 0x14);
    uint32_t dataAddr = pD3DTex[1];
    const uint8_t* srcBytes = (const uint8_t*)XboxKrnl_GpuToCpu(dataAddr);
    if (!srcBytes || (uintptr_t)srcBytes < 0x00010000) return;

    const uint32_t* frmobj = *(const uint32_t* const*)(param_1 + 0x9C);

    IDirect3DDevice9* dev = D3D9_GetDevice();
    if (!dev) return;

    if (!s_pMovieTex || s_movieTexW != texW || s_movieTexH != texH) {
        if (s_pMovieTex) {
            s_pMovieTex->Release();
            s_pMovieTex = nullptr;
        }
        if (FAILED(dev->CreateTexture(texW, texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_pMovieTex, nullptr))) {
            return;
        }
        s_movieTexW = texW;
        s_movieTexH = texH;
        s_movieTexDirty = true;
    }

    // Only upload to D3D9 texture when a new frame was converted (or for YUY2 mode 0)
    if (s_movieTexDirty || fmtMode == 0) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(s_pMovieTex->LockRect(0, &lr, nullptr, 0))) {
            if (fmtMode != 0 && s_movieHasValidFrame) {
                // Fast copy from 32-bit ARGB8888 buffer already converted once by Hook_mwPlyFxCnvFrmARGB8888
                uint32_t srcPitch = texW * 4;
                if ((uint32_t)lr.Pitch == srcPitch) {
                    memcpy(lr.pBits, srcBytes, srcPitch * texH);
                } else {
                    for (uint32_t y = 0; y < texH; y++) {
                        memcpy((uint8_t*)lr.pBits + y * lr.Pitch, srcBytes + y * srcPitch, srcPitch);
                    }
                }
            } else if ((uintptr_t)frmobj >= 0x00010000 && frmobj[0] >= 0x00010000 && frmobj[1] == 3) {
                uint32_t fw = frmobj[2] ? frmobj[2] : texW;
                uint32_t fh = frmobj[3] ? frmobj[3] : texH;
                if (fw > texW) fw = texW;
                if (fh > texH) fh = texH;
                uint32_t mbW = (fw + 15u) & ~15u;
                uint32_t mbH = (fh + 15u) & ~15u;
                uint32_t yStride = (mbW + 31u) & ~31u;
                uint32_t uvStride = ((mbW / 2u) + 31u) & ~31u;
                const uint8_t* yPlane = (const uint8_t*)XboxKrnl_GpuToCpu(frmobj[0]);
                const uint8_t* uPlane = yPlane + yStride * mbH;
                const uint8_t* vPlane = uPlane + uvStride * (mbH / 2u);
                ConvertPlanarI420ToARGB(yPlane, yStride, uPlane, vPlane, uvStride, fw, fh, (uint8_t*)lr.pBits, (uint32_t)lr.Pitch);
                s_movieHasValidFrame = true;
            } else if (fmtMode == 0) {
                // 16-bit YUY2 linear buffer
                uint32_t srcPitch = texW * 2;
                for (uint32_t y = 0; y < texH; y++) {
                    const uint8_t* srcRow = srcBytes + y * srcPitch;
                    uint32_t* dstRow = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
                    for (uint32_t x = 0; x < texW; x += 2) {
                        int y0 = (int)srcRow[x * 2 + 0] - 16;
                        int u  = (int)srcRow[x * 2 + 1] - 128;
                        int y1 = (int)srcRow[x * 2 + 2] - 16;
                        int v  = (int)srcRow[x * 2 + 3] - 128;
                        auto clamp8 = [](int val) -> uint32_t {
                            return val < 0 ? 0u : (val > 255 ? 255u : (uint32_t)val);
                        };
                        int c0 = y0 < 0 ? 0 : y0 * 298;
                        int c1 = y1 < 0 ? 0 : y1 * 298;
                        uint32_t r0 = clamp8((c0 + 409 * v + 128) >> 8);
                        uint32_t g0 = clamp8((c0 - 100 * u - 208 * v + 128) >> 8);
                        uint32_t b0 = clamp8((c0 + 516 * u + 128) >> 8);
                        uint32_t r1 = clamp8((c1 + 409 * v + 128) >> 8);
                        uint32_t g1 = clamp8((c1 - 100 * u - 208 * v + 128) >> 8);
                        uint32_t b1 = clamp8((c1 + 516 * u + 128) >> 8);
                        dstRow[x] = 0xFF000000u | (r0 << 16) | (g0 << 8) | b0;
                        if (x + 1 < texW) {
                            dstRow[x + 1] = 0xFF000000u | (r1 << 16) | (g1 << 8) | b1;
                        }
                    }
                }
                s_movieHasValidFrame = true;
            }
            s_pMovieTex->UnlockRect(0);
            s_movieTexDirty = false;
        }
    }

    if (!s_movieHasValidFrame) return;

    static int s_movieLog = 0;
    if (s_movieLog++ < 15 || (s_movieLog % 120 == 0)) {
        logFmt("[HLE_MOVIE] Frame %d: tex=%ux%u buf=%u fmt=%d data=0x%08X frmobj=0x%08X\n",
               s_movieLog, texW, texH, bufIdx, fmtMode, dataAddr, (uint32_t)(uintptr_t)frmobj);
    }

    float x0 = vtx[0];
    float y0 = vtx[1];
    float x1 = vtx[6];
    float y1 = vtx[13];
    if (x1 <= x0 || y1 <= y0) {
        x0 = 0.0f; y0 = 0.0f;
        x1 = 640.0f; y1 = 448.0f;
    }

    D3D9Vertex3D quad[4];
    quad[0].x = x0; quad[0].y = y0; quad[0].z = 0.5f; quad[0].color = 0xFFFFFFFFu; quad[0].u = 0.0f; quad[0].v = 0.0f;
    quad[1].x = x1; quad[1].y = y0; quad[1].z = 0.5f; quad[1].color = 0xFFFFFFFFu; quad[1].u = 1.0f; quad[1].v = 0.0f;
    quad[2].x = x1; quad[2].y = y1; quad[2].z = 0.5f; quad[2].color = 0xFFFFFFFFu; quad[2].u = 1.0f; quad[2].v = 1.0f;
    quad[3].x = x0; quad[3].y = y1; quad[3].z = 0.5f; quad[3].color = 0xFFFFFFFFu; quad[3].u = 0.0f; quad[3].v = 1.0f;

    static const uint16_t s_quadIdx[6] = { 0, 1, 2, 0, 2, 3 };
    D3D9_SetXboxCompositeMatrix(nullptr, false);
    {
        D3D9StateGuard guard(dev);
        D3D9_BeginScene();
        D3D9_SetTexture(0, s_pMovieTex);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetFVF(D3DFVF_V3D);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, s_quadIdx, D3DFMT_INDEX16, quad, sizeof(D3D9Vertex3D));
        g_drawCount++;
        g_vertexCount += 4;
    }
    InvalidateNamcoStateCache();
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------
extern "C" void NamcoRenderer_InstallHooks(void) {
    uint32_t* pTable = (uint32_t*)0x0019BDC0;
    DWORD oldProtect;
    if (!VirtualProtect(pTable, 128, PAGE_READWRITE, &oldProtect)) {
        logFmt("[NAMCO_HLE] Error: VirtualProtect failed @ 0x0019BDC0 (%lu)\n", GetLastError());
        return;
    }

    logFmt("[NAMCO_HLE] Hooking Namco Primitive Dispatch Table @ 0x0019BDC0...\n");

    pTable[0]  = (uint32_t)Hook_Line2D;
    pTable[1]  = (uint32_t)Hook_Line3D;
    pTable[2]  = (uint32_t)Hook_Box2D;
    pTable[3]  = (uint32_t)Hook_Strip2DG;
    // Slots 4..7 (Circle2D, Circle3D, Ball3D, Pole3D) use native procedural vertex generation -> 0x1818
    pTable[8]  = (uint32_t)Hook_Sprite2D;
    pTable[9]  = (uint32_t)Hook_Sprite2DR;
    pTable[10] = (uint32_t)Hook_Sprite3D;
    pTable[11] = (uint32_t)Hook_Sprite3DR;
    pTable[12] = (uint32_t)Hook_Strip2D;
    pTable[13] = (uint32_t)Hook_Strip2DNT;
    pTable[14] = (uint32_t)Hook_Strip3D;
    pTable[15] = (uint32_t)Hook_Strip3DNT;
    pTable[16] = (uint32_t)Hook_StripVOL;
    pTable[17] = (uint32_t)Hook_Print;
    pTable[18] = (uint32_t)Hook_Kanji;
    pTable[19] = (uint32_t)Hook_Button;

    VirtualProtect(pTable, 128, oldProtect, &oldProtect);

    // Also install direct JMP hooks on FUN_0002b4f0 (MdfV-End shadow quad), Box2D (0x0002EC90), and FUN_0008ffc0 (Movie Frame)
    auto installJmp = [](uint32_t addr, void* fn) {
        DWORD prot;
        uint8_t jmp[5];
        jmp[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)fn - (addr + 5));
        memcpy(&jmp[1], &rel, 4);
        VirtualProtect((void*)addr, 5, PAGE_EXECUTE_READWRITE, &prot);
        memcpy((void*)addr, jmp, 5);
        VirtualProtect((void*)addr, 5, prot, &prot);
    };
    installJmp(0x0002B4F0, (void*)Hook_DrawShadowQuad);
    installJmp(0x0002EC90, (void*)Hook_Box2D);
    installJmp(0x0008FFC0, (void*)Hook_DrawMovieFrame);
    installJmp(0x0011B640, (void*)Hook_mwPlyFxCnvFrmARGB8888);
    installJmp(0x0011B6D0, (void*)Hook_mwPlyFxCnvFrmARGB8888);

    logFmt("[NAMCO_HLE] Unified 2D & 3D HLE hooks installed across all 20 primitives (including StripVOL, ShadowQuad & Movie)!\n");
}
