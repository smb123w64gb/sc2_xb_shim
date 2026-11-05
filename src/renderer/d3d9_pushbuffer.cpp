#include "d3d9_internal.h"

static uint32_t s_lastPushPut = 0;
static uint32_t s_texOffset[4] = { 0, 0, 0, 0 };
static uint32_t s_texFormat[4] = { 0, 0, 0, 0 };
static uint32_t s_texPalette[4] = { 0, 0, 0, 0 };
static bool     s_texEnabled[4] = { false, false, false, false };
static bool     s_texAlphaKill[4] = { false, false, false, false };
static uint32_t s_combinerFactor0Alpha = 255;
static uint32_t s_psRgbInput1 = 0;
static bool     s_texMatrixEnable[4] = { false, false, false, false };
static uint32_t s_texGenMode[4] = { 0, 0, 0, 0 };
static float    s_texMatrix[4][16] = {
    { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 },
    { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 },
    { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 },
    { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }
};

static uint32_t s_vtxOffset[16] = {0};
static uint32_t s_vtxStride[16] = {0};
static uint32_t s_vtxFormat[16] = {0};
static bool     s_is3DMode = false;
static uint32_t s_vsConstLoad = 0;
static uint32_t s_clearZStencil = 0;
static uint32_t s_clearColor = 0;
static float    s_sceneAmbient[3] = { 0.5f, 0.5f, 0.5f };
static float    s_materialAlpha = 1.0f;

struct NV2ALightState {
    int enabled;
    int type; // 1 = directional, 2 = point, 3 = spot
    float ambient[3];
    float diffuse[3];
    float specular[3];
    float dir[3];
    float pos[3];
    float range;
};

static int s_lightingEnabled = 1;
static uint32_t s_lightEnableMask = 0;
static NV2ALightState s_lights[8] = {0};
static bool s_fogEnabled = false;

static bool s_alphaTestEnable = false;
static D3DCMPFUNC s_alphaFunc = D3DCMP_GREATER;
static DWORD s_alphaRef = 0;
static bool s_stencilEnable = false;
static bool s_cullEnable = false;
static uint32_t s_cullFace = 0x0405; // 0x404 = FRONT (D3DCULL_CW), 0x405 = BACK (D3DCULL_CCW)
static DWORD s_colorWriteMask = 0x0F;
static bool s_zWriteEnable = true;

static constexpr uint32_t MAX_BATCH_VERTS = 2048;
static uint16_t s_quadIndices[MAX_BATCH_VERTS * 6 / 4];
static bool s_indicesInitialized = false;

static constexpr uint32_t MAX_INDEX_BATCH = 32768;
static uint16_t s_indexBatch[MAX_INDEX_BATCH];
static uint16_t s_rebasedIndices[MAX_INDEX_BATCH];
static uint32_t s_indexBatchCount = 0;
static uint32_t s_currentD3DPrimitive = 0;

struct D3D9CompiledVertex {
    float x, y, z;
    float nx, ny, nz;
    uint32_t color;
    float u, v;
    float u1, v1;
};

#define D3DFVF_COMPILED (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2)
static constexpr uint32_t MAX_COMPILED_VERTS = 32768;
static D3D9CompiledVertex s_compiledVerts[MAX_COMPILED_VERTS];

static void InitQuadIndices(void) {
    if (s_indicesInitialized) return;
    for (uint32_t i = 0, v = 0; i < (MAX_BATCH_VERTS * 6 / 4); i += 6, v += 4) {
        s_quadIndices[i + 0] = (uint16_t)(v + 0);
        s_quadIndices[i + 1] = (uint16_t)(v + 1);
        s_quadIndices[i + 2] = (uint16_t)(v + 2);
        s_quadIndices[i + 3] = (uint16_t)(v + 0);
        s_quadIndices[i + 4] = (uint16_t)(v + 2);
        s_quadIndices[i + 5] = (uint16_t)(v + 3);
    }
    s_indicesInitialized = true;
}

static D3DTEXTUREADDRESS ConvertNv2aTexAddressToD3D(uint32_t mode) {
    switch (mode) {
        case 1: return D3DTADDRESS_WRAP;
        case 2: return D3DTADDRESS_MIRROR;
        case 3: return D3DTADDRESS_CLAMP;
        case 4: return D3DTADDRESS_BORDER;
        case 5: return D3DTADDRESS_CLAMP;
        default: return D3DTADDRESS_WRAP;
    }
}

static void ApplyNv2aTexAddress(uint32_t stage, uint32_t val) {
    if (!g_pDevice || stage >= 4) return;
    uint32_t uMode = val & 0x07;
    uint32_t vMode = (val >> 8) & 0x07;
    uint32_t pMode = (val >> 16) & 0x07;
    g_pDevice->SetSamplerState(stage, D3DSAMP_ADDRESSU, ConvertNv2aTexAddressToD3D(uMode));
    g_pDevice->SetSamplerState(stage, D3DSAMP_ADDRESSV, ConvertNv2aTexAddressToD3D(vMode));
    g_pDevice->SetSamplerState(stage, D3DSAMP_ADDRESSW, ConvertNv2aTexAddressToD3D(pMode));
}

static void ApplyNv2aTexFilter(uint32_t stage, uint32_t val) {
    if (!g_pDevice || stage >= 4) return;

    uint32_t mag = (val >> 24) & 0x0F;
    D3DTEXTUREFILTERTYPE magFilter = (mag == 1) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    g_pDevice->SetSamplerState(stage, D3DSAMP_MAGFILTER, magFilter);

    uint32_t min = (val >> 16) & 0xFF;
    D3DTEXTUREFILTERTYPE minFilter = D3DTEXF_LINEAR;
    D3DTEXTUREFILTERTYPE mipFilter = D3DTEXF_LINEAR;

    switch (min) {
        case 1: minFilter = D3DTEXF_POINT;       mipFilter = D3DTEXF_NONE;   break;
        case 2: minFilter = D3DTEXF_LINEAR;      mipFilter = D3DTEXF_NONE;   break;
        case 3: minFilter = D3DTEXF_POINT;       mipFilter = D3DTEXF_POINT;  break;
        case 4: minFilter = D3DTEXF_LINEAR;      mipFilter = D3DTEXF_POINT;  break;
        case 5: minFilter = D3DTEXF_POINT;       mipFilter = D3DTEXF_LINEAR; break;
        case 6: minFilter = D3DTEXF_LINEAR;      mipFilter = D3DTEXF_LINEAR; break;
        case 7: minFilter = D3DTEXF_ANISOTROPIC; mipFilter = D3DTEXF_LINEAR; break;
        default: minFilter = D3DTEXF_LINEAR;     mipFilter = D3DTEXF_LINEAR; break;
    }
    g_pDevice->SetSamplerState(stage, D3DSAMP_MINFILTER, minFilter);
    g_pDevice->SetSamplerState(stage, D3DSAMP_MIPFILTER, mipFilter);
}

static void FlushD3D9Strip(void);

static void ApplyNv2aTexControl0(uint32_t stage, uint32_t ctrl0) {
    if (stage < 4) {
        bool newEnabled = ((ctrl0 & 0x40000000u) != 0);
        bool newAlphaKill = ((ctrl0 & 0x04u) != 0);
        if (s_indexBatchCount > 0 && (s_texEnabled[stage] != newEnabled || s_texAlphaKill[stage] != newAlphaKill)) {
            FlushD3D9Strip();
        }
        s_texEnabled[stage] = newEnabled;
        s_texAlphaKill[stage] = newAlphaKill;
    }
    uint32_t anisoBits = (ctrl0 >> 4) & 3;
    DWORD aniso = (anisoBits == 0) ? 1 : (1 << anisoBits);
    if (aniso < 4) aniso = 8;
    if (g_pDevice && stage < 4) {
        g_pDevice->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, aniso);
    }
}

static D3DPRIMITIVETYPE ConvertNv2aPrimitiveToD3D(uint32_t prim) {
    switch (prim) {
        case 1: return D3DPT_POINTLIST;
        case 2: return D3DPT_LINELIST;
        case 4: return D3DPT_LINESTRIP;
        case 5: return D3DPT_TRIANGLELIST;
        case 6: return D3DPT_TRIANGLESTRIP;
        case 7: return D3DPT_TRIANGLEFAN;
        default: return D3DPT_TRIANGLELIST;
    }
}

static D3DCMPFUNC ConvertNv2aCompareFuncToD3D(uint32_t func) {
    switch (func) {
        case 0x0200: return D3DCMP_NEVER;
        case 0x0201: return D3DCMP_LESS;
        case 0x0202: return D3DCMP_EQUAL;
        case 0x0203: return D3DCMP_LESSEQUAL;
        case 0x0204: return D3DCMP_GREATER;
        case 0x0205: return D3DCMP_NOTEQUAL;
        case 0x0206: return D3DCMP_GREATEREQUAL;
        case 0x0207: return D3DCMP_ALWAYS;
        default:     return D3DCMP_ALWAYS;
    }
}

static D3DBLEND ConvertNv2aBlendFactorToD3D(uint32_t factor) {
    switch (factor) {
        case 0x0000: return D3DBLEND_ZERO;
        case 0x0001: return D3DBLEND_ONE;
        case 0x0300: return D3DBLEND_SRCCOLOR;
        case 0x0301: return D3DBLEND_INVSRCCOLOR;
        case 0x0302: return D3DBLEND_SRCALPHA;
        case 0x0303: return D3DBLEND_INVSRCALPHA;
        case 0x0304: return D3DBLEND_DESTALPHA;
        case 0x0305: return D3DBLEND_INVDESTALPHA;
        case 0x0306: return D3DBLEND_DESTCOLOR;
        case 0x0307: return D3DBLEND_INVDESTCOLOR;
        case 0x0308: return D3DBLEND_SRCALPHASAT;
        default:     return D3DBLEND_ONE;
    }
}

static D3DSTENCILOP ConvertNv2aStencilOpToD3D(uint32_t op) {
    switch (op) {
        case 0x0000: return D3DSTENCILOP_ZERO;
        case 0x1E00: return D3DSTENCILOP_KEEP;
        case 0x1E01: return D3DSTENCILOP_REPLACE;
        case 0x1E02: return D3DSTENCILOP_INCRSAT;
        case 0x1E03: return D3DSTENCILOP_DECRSAT;
        case 0x150A: return D3DSTENCILOP_INVERT;
        case 0x8507: return D3DSTENCILOP_INCR;
        case 0x8508: return D3DSTENCILOP_DECR;
        default:     return D3DSTENCILOP_KEEP;
    }
}

static void UpdateD3D9CullMode(void) {
    if (!g_pDevice) return;
    if (s_cullEnable) {
        D3DCULL mode = (s_cullFace == 0x0404) ? D3DCULL_CW : D3DCULL_CCW;
        g_pDevice->SetRenderState(D3DRS_CULLMODE, mode);
    } else {
        g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
}

static void FlushD3D9Strip(void) {
    if (!g_pDevice || s_indexBatchCount < 3) {
        s_indexBatchCount = 0;
        return;
    }

    uint32_t posGpu = s_vtxOffset[0];
    if (!posGpu) {
        s_indexBatchCount = 0;
        return;
    }
    const uint8_t* pPosBase = (const uint8_t*)XboxKrnl_GpuToCpu(posGpu);
    if (!pPosBase) {
        s_indexBatchCount = 0;
        return;
    }
    uint32_t stridePos = s_vtxStride[0] ? s_vtxStride[0] : 12;

    const uint8_t* pNormBase = (s_vtxFormat[2] != 2 && s_vtxOffset[2]) ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[2]) : nullptr;
    uint32_t strideNorm = s_vtxStride[2] ? s_vtxStride[2] : stridePos;

    const uint8_t* pDiffBase = (s_vtxFormat[3] != 2 && s_vtxOffset[3]) ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[3]) : nullptr;
    uint32_t strideDiff = s_vtxStride[3] ? s_vtxStride[3] : stridePos;

    const uint8_t* pUvBase = (s_vtxFormat[9] != 2 && s_vtxOffset[9]) ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[9]) : nullptr;
    uint32_t strideUv = s_vtxStride[9] ? s_vtxStride[9] : stridePos;

    uint32_t minIdx = 0xFFFFFFFFu;
    uint32_t maxIdx = 0;
    for (uint32_t i = 0; i < s_indexBatchCount; i++) {
        uint32_t idx = s_indexBatch[i];
        if (idx < minIdx) minIdx = idx;
        if (idx > maxIdx) maxIdx = idx;
    }
    if (minIdx > maxIdx) {
        s_indexBatchCount = 0;
        return;
    }

    uint32_t numVerts = (maxIdx - minIdx) + 1;
    if (numVerts > MAX_COMPILED_VERTS) numVerts = MAX_COMPILED_VERTS;

    for (uint32_t i = 0; i < s_indexBatchCount; i++) {
        uint32_t rebased = s_indexBatch[i] - minIdx;
        if (rebased >= numVerts) rebased = numVerts - 1;
        s_rebasedIndices[i] = (uint16_t)rebased;
    }

    bool hasTex0 = (s_texEnabled[0] && s_texOffset[0] != 0);
    // Untextured .VMX meshes use PS_0Tex (0x00193F60) which sets PSRGBOutputs[0] = 0x00010C00 (SHIFTLEFT_1 = 2X),
    // while textured meshes get 2X from D3DTOP_MODULATE2X. Scale untextured vertex RGB by 2.0f to match PS_0Tex.
    float untexScale = hasTex0 ? 1.0f : 2.0f;

    for (uint32_t v = 0; v < numVerts; v++) {
        uint32_t srcIdx = minIdx + v;
        const float* pPos = (const float*)(pPosBase + srcIdx * stridePos);
        s_compiledVerts[v].x = pPos[0];
        s_compiledVerts[v].y = pPos[1];
        s_compiledVerts[v].z = pPos[2];

        float nx = 0.0f, ny = 1.0f, nz = 0.0f;
        if (pNormBase) {
            const float* pN = (const float*)(pNormBase + srcIdx * strideNorm);
            nx = pN[0];
            ny = pN[1];
            nz = pN[2];
        }
        s_compiledVerts[v].nx = nx;
        s_compiledVerts[v].ny = ny;
        s_compiledVerts[v].nz = nz;

        uint32_t color = 0xFFFFFFFFu;
        float matR = 1.0f, matG = 1.0f, matB = 1.0f;
        uint32_t vtxAlpha = 255u;
        if (pDiffBase) {
            color = *(const uint32_t*)(pDiffBase + srcIdx * strideDiff);
            vtxAlpha = (color >> 24) & 0xFFu;
            matR = (float)((color >> 16) & 0xFFu) / 255.0f;
            matG = (float)((color >> 8) & 0xFFu) / 255.0f;
            matB = (float)(color & 0xFFu) / 255.0f;
        }
        uint32_t finalAlpha = (vtxAlpha * s_combinerFactor0Alpha + 127u) / 255u;
        if (finalAlpha > 255u) finalAlpha = 255u;
        uint32_t alpha = finalAlpha << 24;

        // Xbox D3D (0x001521C0) transposes the row-major D3DMATRIX when emitting 0x0480 (NV097_SET_MODELVIEW_MATRIX),
        // so rows 0, 1, 2 of g_modelViewMatrix are [0..3], [4..7], [8..11].
        float ep_x = pPos[0] * g_modelViewMatrix[0] + pPos[1] * g_modelViewMatrix[1] + pPos[2] * g_modelViewMatrix[2]  + g_modelViewMatrix[3];
        float ep_y = pPos[0] * g_modelViewMatrix[4] + pPos[1] * g_modelViewMatrix[5] + pPos[2] * g_modelViewMatrix[6]  + g_modelViewMatrix[7];
        float ep_z = pPos[0] * g_modelViewMatrix[8] + pPos[1] * g_modelViewMatrix[9] + pPos[2] * g_modelViewMatrix[10] + g_modelViewMatrix[11];

        float en_x = 0.0f, en_y = 1.0f, en_z = 0.0f;
        if (pNormBase) {
            en_x = nx * g_modelViewMatrix[0] + ny * g_modelViewMatrix[1] + nz * g_modelViewMatrix[2];
            en_y = nx * g_modelViewMatrix[4] + ny * g_modelViewMatrix[5] + nz * g_modelViewMatrix[6];
            en_z = nx * g_modelViewMatrix[8] + ny * g_modelViewMatrix[9] + nz * g_modelViewMatrix[10];
            float lenSq = en_x * en_x + en_y * en_y + en_z * en_z;
            if (lenSq > 1e-6f) {
                float invLen = 1.0f / sqrtf(lenSq);
                en_x *= invLen; en_y *= invLen; en_z *= invLen;
            } else {
                en_x = 0.0f; en_y = 1.0f; en_z = 0.0f;
            }

            if (s_lightEnableMask == 0) {
                // Unlit stage/backdrop geometry: SetupVmxMaterial (0x000241F1) stores 0.5f * (matAmb + stageAmb)
                // in D3DRS_AMBIENT (0x0015E55C -> emitted to 0x03A8 s_sceneAmbient), modulated by 2X in PS_1Tex.
                float ambR = 0.5f, ambG = 0.5f, ambB = 0.5f;
                if (s_lightingEnabled && (s_sceneAmbient[0] + s_sceneAmbient[1] + s_sceneAmbient[2] > 1e-4f)) {
                    ambR = s_sceneAmbient[0];
                    ambG = s_sceneAmbient[1];
                    ambB = s_sceneAmbient[2];
                }
                int ir = (int)(matR * ambR * untexScale * 255.0f + 0.5f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
                int ig = (int)(matG * ambG * untexScale * 255.0f + 0.5f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
                int ib = (int)(matB * ambB * untexScale * 255.0f + 0.5f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;
                color = alpha | ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
            } else {
                // Lit 3D geometry (stages, characters, weapons, props):
                // SetupVmxMaterial (0x00024280) pre-scales all light ambient/diffuse/specular colors by 0.5f
                // because PS_1Tex (0x00194050) applies 2X modulation: R0.rgb = 2.0 * (V0.rgb + V1.rgb) * T0.rgb.
                float totR = s_sceneAmbient[0];
                float totG = s_sceneAmbient[1];
                float totB = s_sceneAmbient[2];
                float specR = 0.0f, specG = 0.0f, specB = 0.0f;

                float matPower = 16.0f;
                const uint8_t* pDevBytes = *(const uint8_t**)0x0015E628;
                if (pDevBytes) {
                    float pwr = *(const float*)(pDevBytes + 0xB20);
                    if (pwr >= 0.5f && pwr <= 128.0f) matPower = pwr;
                }
                bool specEnable = (*(const uint32_t*)0x0015E52C != 0);

                for (int i = 0; i < 8; i++) {
                    uint32_t ltype = (s_lightEnableMask >> (i * 2)) & 3;
                    if (ltype == 0 && !s_lights[i].enabled) continue;

                    totR += s_lights[i].ambient[0];
                    totG += s_lights[i].ambient[1];
                    totB += s_lights[i].ambient[2];

                    float lx = s_lights[i].dir[0];
                    float ly = s_lights[i].dir[1];
                    float lz = s_lights[i].dir[2];
                    float nl = en_x * lx + en_y * ly + en_z * lz;
                    if (nl > 0.0f) {
                        float dr = s_lights[i].diffuse[0];
                        float dg = s_lights[i].diffuse[1];
                        float db = s_lights[i].diffuse[2];
                        totR += nl * dr;
                        totG += nl * dg;
                        totB += nl * db;

                        if (specEnable) {
                            float sr = s_lights[i].specular[0];
                            float sg = s_lights[i].specular[1];
                            float sb = s_lights[i].specular[2];
                            if (sr + sg + sb > 1e-4f) {
                                float vx = -ep_x, vy = -ep_y, vz = -ep_z;
                                float vLenSq = vx * vx + vy * vy + vz * vz;
                                if (vLenSq > 1e-6f) {
                                    float invV = 1.0f / sqrtf(vLenSq);
                                    vx *= invV; vy *= invV; vz *= invV;
                                } else {
                                    vz = 1.0f;
                                }
                                float hx = lx + vx, hy = ly + vy, hz = lz + vz;
                                float hLenSq = hx * hx + hy * hy + hz * hz;
                                if (hLenSq > 1e-6f) {
                                    float invH = 1.0f / sqrtf(hLenSq);
                                    hx *= invH; hy *= invH; hz *= invH;
                                }
                                float nh = en_x * hx + en_y * hy + en_z * hz;
                                if (nh > 0.0f) {
                                    float sp = powf(nh, matPower);
                                    specR += sp * sr;
                                    specG += sp * sg;
                                    specB += sp * sb;
                                }
                            }
                        }
                    }
                }

                float outR = (totR * matR + specR) * untexScale;
                float outG = (totG * matG + specG) * untexScale;
                float outB = (totB * matB + specB) * untexScale;

                int ir = (int)(outR * 255.0f + 0.5f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
                int ig = (int)(outG * 255.0f + 0.5f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
                int ib = (int)(outB * 255.0f + 0.5f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;
                color = alpha | ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
            }
        } else {
            int ir = (int)(matR * 128.0f * untexScale + 0.5f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
            int ig = (int)(matG * 128.0f * untexScale + 0.5f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
            int ib = (int)(matB * 128.0f * untexScale + 0.5f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;
            color = alpha | ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
        }
        s_compiledVerts[v].color = color;

        float rawU = 0.0f, rawV = 0.0f;
        if (pUvBase) {
            const float* pUv = (const float*)(pUvBase + srcIdx * strideUv);
            rawU = pUv[0];
            rawV = pUv[1];
        }

        for (uint32_t st = 0; st < 2; st++) {
            float outU = rawU, outV = rawV;
            uint32_t tg = s_texGenMode[st];
            if (tg == 0x8511) { // NV097_SET_TEXGEN_S_NORMAL_MAP (D3DTSS_TCI_CAMERASPACENORMAL)
                if (s_texMatrixEnable[st]) {
                    outU = s_texMatrix[st][0] * en_x + s_texMatrix[st][1] * en_y + s_texMatrix[st][2] * en_z + s_texMatrix[st][3];
                    outV = s_texMatrix[st][4] * en_x + s_texMatrix[st][5] * en_y + s_texMatrix[st][6] * en_z + s_texMatrix[st][7];
                } else {
                    outU = 0.5f * en_x + 0.5f;
                    outV = 0.5f * en_y + 0.5f;
                }
            } else if (tg == 0x8512 || tg == 0x2401) { // REFLECTION_MAP / SPHERE_MAP
                float vx = ep_x, vy = ep_y, vz = ep_z;
                float epLenSq = vx * vx + vy * vy + vz * vz;
                if (epLenSq > 1e-6f) {
                    float invEp = 1.0f / sqrtf(epLenSq);
                    vx *= invEp; vy *= invEp; vz *= invEp;
                }
                float dotIN = vx * en_x + vy * en_y + vz * en_z;
                float rx = vx - 2.0f * dotIN * en_x;
                float ry = vy - 2.0f * dotIN * en_y;
                float rz = vz - 2.0f * dotIN * en_z;
                if (s_texMatrixEnable[st]) {
                    outU = s_texMatrix[st][0] * rx + s_texMatrix[st][1] * ry + s_texMatrix[st][2] * rz + s_texMatrix[st][3];
                    outV = s_texMatrix[st][4] * rx + s_texMatrix[st][5] * ry + s_texMatrix[st][6] * rz + s_texMatrix[st][7];
                } else {
                    outU = 0.5f * rx + 0.5f;
                    outV = 0.5f * ry + 0.5f;
                }
            } else if (s_texMatrixEnable[st]) {
                outU = s_texMatrix[st][0] * rawU + s_texMatrix[st][1] * rawV + s_texMatrix[st][3];
                outV = s_texMatrix[st][4] * rawU + s_texMatrix[st][5] * rawV + s_texMatrix[st][7];
            }
            if (st == 0) {
                s_compiledVerts[v].u = outU;
                s_compiledVerts[v].v = outV;
            } else {
                s_compiledVerts[v].u1 = outU;
                s_compiledVerts[v].v1 = outV;
            }
        }
    }

    IDirect3DTexture9* pTex0 = hasTex0 ? D3D9_GetOrCreateTexture(s_texOffset[0], s_texFormat[0], s_texPalette[0]) : nullptr;
    bool hasTex1 = (pTex0 != nullptr && s_texEnabled[1] && s_texOffset[1] != 0 && s_psRgbInput1 != 0);
    IDirect3DTexture9* pTex1 = hasTex1 ? D3D9_GetOrCreateTexture(s_texOffset[1], s_texFormat[1], s_texPalette[1]) : nullptr;

    if (pTex0) {
        D3D9_SetTexture(0, pTex0);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        if (pTex1) {
            // PS_2Tex_A (0x194140) and PS_2Tex_B (0x194230) use T0.a in Stage 1 to blend/modulate T1.rgb,
            // and then multiply by V0.a * C0.a in Stage 1 Alpha.
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

            D3D9_SetTexture(1, pTex1);
            g_pDevice->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
            if (s_psRgbInput1 == 0x18C9CC38u) {
                // PS_2Tex_A (0x194140): Out.rgb = T0.a * T1.rgb + (1 - T0.a) * R0.rgb
                g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_BLENDCURRENTALPHA);
                g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
            } else {
                // PS_2Tex_B (0x194230): Out.rgb = R0.rgb + T0.a * T1.rgb
                g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATEALPHA_ADDCOLOR);
                g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
                g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
            }
            g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
            g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        } else {
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            D3D9_SetTexture(1, nullptr);
            g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    } else {
        D3D9_SetTexture(0, nullptr);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        D3D9_SetTexture(1, nullptr);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    static const float s_identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    D3D9_SetTransform(D3DTS_WORLD, s_identity);
    D3D9_SetTransform(D3DTS_VIEW, s_identity);
    D3D9_SetTransform(D3DTS_PROJECTION, g_projMatrix);

    uint32_t activeFvf = *(const uint32_t*)0x0038E85C;
    if (activeFvf == 2) {
        g_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    } else if (s_fogEnabled) {
        g_pDevice->SetRenderState(D3DRS_FOGENABLE, TRUE);
    } else {
        g_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    }

    D3DPRIMITIVETYPE d3dPrim = ConvertNv2aPrimitiveToD3D(s_currentD3DPrimitive);
    uint32_t primCount = 0;
    if (d3dPrim == D3DPT_TRIANGLESTRIP) {
        primCount = s_indexBatchCount - 2;
    } else if (d3dPrim == D3DPT_TRIANGLELIST) {
        primCount = s_indexBatchCount / 3;
    } else if (d3dPrim == D3DPT_LINESTRIP) {
        primCount = s_indexBatchCount - 1;
    } else if (d3dPrim == D3DPT_LINELIST) {
        primCount = s_indexBatchCount / 2;
    }

    if (primCount > 0) {
        D3D9_BeginScene();

        // Honor s_alphaTestEnable (0x0300) and per-texture D3DTSS_ALPHAKILL (0x1B0C bit 2 = ALPHA_KILL_ENABLE):
        // Opaque 3D character/stage meshes store a specular/glossiness mask in texture alpha (alpha == 0 = matte),
        // while cutout stage meshes (e.g. ivy foliage) enable D3DTSS_ALPHAKILL in BindVmxTexture (0x000254A9).
        if (s_alphaTestEnable) {
            g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            g_pDevice->SetRenderState(D3DRS_ALPHAFUNC, s_alphaFunc);
            g_pDevice->SetRenderState(D3DRS_ALPHAREF, s_alphaRef);
        } else if (s_texEnabled[0] && s_texAlphaKill[0]) {
            g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            g_pDevice->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
            g_pDevice->SetRenderState(D3DRS_ALPHAREF, 0);
        } else {
            g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        }

        g_pDevice->SetFVF(D3DFVF_COMPILED);
        g_pDevice->DrawIndexedPrimitiveUP(
            d3dPrim,
            0,
            numVerts,
            primCount,
            s_rebasedIndices,
            D3DFMT_INDEX16,
            s_compiledVerts,
            sizeof(D3D9CompiledVertex)
        );

        if (pTex1) {
            D3D9_SetTexture(1, nullptr);
            g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }

        g_drawCount++;
        g_vertexCount += s_indexBatchCount;
    }

    s_indexBatchCount = 0;
}

static void HandleD3D9ArrayElement16(const uint32_t* args, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t pair = args[i];
        uint16_t idx[2] = { (uint16_t)(pair & 0xFFFF), (uint16_t)((pair >> 16) & 0xFFFF) };
        for (int k = 0; k < 2; k++) {
            uint16_t vi = idx[k];
            if (vi == 0xFFFF) {
                FlushD3D9Strip();
                continue;
            }
            if (s_indexBatchCount < MAX_INDEX_BATCH) {
                s_indexBatch[s_indexBatchCount++] = vi;
            }
        }
    }
}

static void HandleD3D9ArrayElementSingle(uint32_t arg) {
    uint16_t vi = (uint16_t)(arg & 0xFFFF);
    if (vi == 0xFFFF) {
        FlushD3D9Strip();
        return;
    }
    if (s_indexBatchCount < MAX_INDEX_BATCH) {
        s_indexBatch[s_indexBatchCount++] = vi;
    }
}

static void HandleD3D9SetBeginEnd(uint32_t op) {
    if (op == 0) {
        FlushD3D9Strip();
        s_currentD3DPrimitive = 0;
    } else {
        if (s_indexBatchCount > 0) {
            FlushD3D9Strip();
        }
        s_currentD3DPrimitive = op;
    }
}

static void HandleD3D9InlineArray(const uint32_t* args, uint32_t count) {
    if (!g_pDevice || count == 0) return;
    g_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    InitQuadIndices();

    // Determine vertex stride from active Namco FVF (0x0038E85C) and NV2A vertex attribute formats:
    // - FVF 0x02: 12 bytes (3 dwords): X, Y, Z (StripVOL stencil shadow volumes)
    // - FVF 0x42: 16 bytes (4 dwords): X, Y, Z, Color (Box2D, Strip2DG, Circle2D, Circle3D, Strip2DNT, shadow quad)
    // - FVF 0x142: 24 bytes (6 dwords): X, Y, Z, Color, U, V (Strip2D, Print, Kanji, Button, Sprite2D)
    // - FVF 0xFFFFFFFF: Strip3D / Strip3DNT custom vertex shaders (6 dwords/vertex)
    uint32_t activeFvf = *(const uint32_t*)0x0038E85C;
    uint32_t stride = 4;
    // When color writes are disabled (s_colorWriteMask == 0) during stencil shadow volume extrusion,
    // vertices are always 3 floats (12 bytes: X, Y, Z), regardless of what CPU global 0x0038E85C was
    // later changed to by MdfV-End on the CPU before pushbuffer playback!
    if (s_colorWriteMask == 0 && (count % 3 == 0)) {
        stride = 3;
    } else if (activeFvf == 2) {
        stride = 3;
    } else if (activeFvf == 0x42) {
        stride = 4;
    } else if (activeFvf == 0x142 || activeFvf == 0xFFFFFFFF) {
        stride = 6;
    } else if (activeFvf == 0x112 || activeFvf == 0x242) {
        stride = 8;
    } else if (s_vtxFormat[9] != 2 && s_vtxFormat[9] != 0) {
        stride = 6;
    } else if (s_vtxFormat[3] != 2 && s_vtxFormat[3] != 0) {
        stride = 4;
    } else if (s_vtxStride[0] == 12) {
        stride = 3;
    } else if (s_vtxStride[0] == 16) {
        stride = 4;
    } else if (s_vtxStride[0] == 24) {
        stride = 6;
    } else if (s_texEnabled[0] && s_texOffset[0] != 0 && (count % 6 == 0)) {
        stride = 6;
    } else if (count % 3 == 0 && (count % 4 != 0) && (count % 6 != 0)) {
        stride = 3;
    } else if (count % 6 == 0) {
        stride = 6;
    } else if (count % 4 == 0) {
        stride = 4;
    }

    uint32_t numVerts = count / stride;
    if (numVerts > MAX_COMPILED_VERTS) numVerts = MAX_COMPILED_VERTS;
    if (numVerts == 0) return;

    for (uint32_t v = 0; v < numVerts; v++) {
        const uint32_t* vert = &args[v * stride];
        float x = *(const float*)&vert[0];
        float y = *(const float*)&vert[1];
        float z = (stride >= 3) ? *(const float*)&vert[2] : 0.5f;
        if (s_currentD3DPrimitive == 8) {
            if (z < 0.0f) z = 0.0f;
            if (z > 1.0f) z = 1.0f;
        }
        s_compiledVerts[v].x = x;
        s_compiledVerts[v].y = y;
        s_compiledVerts[v].z = z;
        s_compiledVerts[v].nx = 0.0f;
        s_compiledVerts[v].ny = 1.0f;
        s_compiledVerts[v].nz = 0.0f;
        s_compiledVerts[v].color = (stride >= 4) ? vert[3] : 0xFF808080u;
        if (stride >= 6) {
            s_compiledVerts[v].u = *(const float*)&vert[4];
            s_compiledVerts[v].v = *(const float*)&vert[5];
        } else {
            s_compiledVerts[v].u = 0.0f;
            s_compiledVerts[v].v = 0.0f;
        }
        s_compiledVerts[v].u1 = 0.0f;
        s_compiledVerts[v].v1 = 0.0f;
    }

    bool useTex = (stride >= 6 && s_texEnabled[0] && s_texOffset[0] != 0);
    if (useTex) {
        IDirect3DTexture9* pTex = D3D9_GetOrCreateTexture(s_texOffset[0], s_texFormat[0], s_texPalette[0]);
        D3D9_SetTexture(0, pTex);
        bool zeroDiffuseRgb = (numVerts > 0 && (s_compiledVerts[0].color & 0x00FFFFFFu) == 0);
        if (zeroDiffuseRgb) {
            g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        } else {
            g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        }
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    } else {
        D3D9_SetTexture(0, nullptr);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }

    static const float s_identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    D3D9_SetTransform(D3DTS_WORLD, s_identity);
    D3D9_SetTransform(D3DTS_VIEW, s_identity);
    D3D9_SetTransform(D3DTS_PROJECTION, g_projMatrix);

    D3D9_BeginScene();

    uint32_t texFmt = (s_texFormat[0] >> 8) & 0xFF;
    bool enableAlphaTest = s_alphaTestEnable || (useTex && D3D9_FormatHasAlpha(texFmt));
    if (enableAlphaTest) {
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHAFUNC, s_alphaTestEnable ? s_alphaFunc : D3DCMP_GREATER);
        g_pDevice->SetRenderState(D3DRS_ALPHAREF, s_alphaTestEnable ? s_alphaRef : 0);
    } else {
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }

    g_pDevice->SetFVF(D3DFVF_COMPILED);

    if (s_currentD3DPrimitive == 6) { // D3DPT_TRIANGLESTRIP
        if (numVerts >= 3) {
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, numVerts - 2, s_compiledVerts, sizeof(D3D9CompiledVertex));
        }
    } else if (s_currentD3DPrimitive == 2) { // D3DPT_LINELIST
        if (numVerts >= 2) {
            g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, numVerts / 2, s_compiledVerts, sizeof(D3D9CompiledVertex));
        }
    } else if (s_currentD3DPrimitive == 4) { // D3DPT_LINESTRIP
        if (numVerts >= 2) {
            g_pDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, numVerts - 1, s_compiledVerts, sizeof(D3D9CompiledVertex));
        }
    } else if (s_currentD3DPrimitive == 7) { // D3DPT_TRIANGLEFAN
        if (numVerts >= 3) {
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, numVerts - 2, s_compiledVerts, sizeof(D3D9CompiledVertex));
        }
    } else if (numVerts >= 4 && (numVerts % 4 == 0)) { // NV2A QUADS (8) -> Indexed Triangle List
        uint32_t numQuads = numVerts / 4;
        g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_pDevice->DrawIndexedPrimitiveUP(
            D3DPT_TRIANGLELIST,
            0,
            numVerts,
            numQuads * 2,
            s_quadIndices,
            D3DFMT_INDEX16,
            s_compiledVerts,
            sizeof(D3D9CompiledVertex)
        );
        g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, s_zWriteEnable ? TRUE : FALSE);
        UpdateD3D9CullMode();
    } else if (numVerts >= 3) {
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, numVerts - 2, s_compiledVerts, sizeof(D3D9CompiledVertex));
    }

    if (enableAlphaTest && !s_alphaTestEnable) {
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }
    g_drawCount++;
    g_vertexCount += numVerts;
}

extern "C" {

void D3D9_FlushPushBuffer(void) {
    uint32_t** ppDev = (uint32_t**)0x0015E628;
    if (ppDev && *ppDev) {
        D3D9_ProcessPushBuffer(*ppDev);
    }
}

void D3D9_ProcessPushBuffer(uint32_t* pDev) {
    if (!pDev || !g_d3d9Initialized) return;

    uint32_t currentPut = pDev[0];
    uint32_t pushBase = pDev[9];
    uint32_t pushEnd = pDev[10];

    if (s_lastPushPut == 0) s_lastPushPut = pushBase;
    if (currentPut == s_lastPushPut) return;

    uint32_t* cur = (uint32_t*)s_lastPushPut;
    uint32_t* end = (uint32_t*)currentPut;
    uint32_t* ringEnd = (uint32_t*)pushEnd;
    uint32_t* ringBase = (uint32_t*)pushBase;

    while (cur != end) {
        if (cur >= ringEnd) {
            cur = ringBase;
            if (cur == end) break;
        }

        uint32_t token = *cur++;
        if ((token & 0x03) == 1) {
            uint32_t target = token & ~0x03;
            if (target < pushBase) {
                target = (target & 0x0FFFFFFF) | (pushBase & 0xF0000000);
            }
            if (target < pushBase || target >= pushEnd) target = pushBase;
            cur = (uint32_t*)target;
            continue;
        }

        uint32_t method = token & 0x1FFC;
        uint32_t count = (token >> 18) & 0x7FF;
        if (count == 0) continue;

        const uint32_t* args = cur;
        cur += count;

        switch (method) {
            case 0x029C: // NV097_SET_FOG_COLOR
                if (count >= 1 && g_pDevice) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_FOGCOLOR, args[0]);
                }
                break;

            case 0x02A0: // NV097_SET_FOG_GEN_MODE
                if (count >= 1 && g_pDevice) {
                    g_pDevice->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
                    g_pDevice->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
                }
                break;

            case 0x02A4: // NV097_SET_FOG_ENABLE
                if (count >= 1 && g_pDevice) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_fogEnabled = (args[0] != 0);
                    if (s_fogEnabled) {
                        float fStart = *(const float*)0x0015E508;
                        float fEnd = *(const float*)0x0015E50C;
                        if (fEnd > fStart && fEnd > 10.0f && fEnd < 100000.0f) {
                            g_pDevice->SetRenderState(D3DRS_FOGSTART, *(const DWORD*)&fStart);
                            g_pDevice->SetRenderState(D3DRS_FOGEND, *(const DWORD*)&fEnd);
                        } else {
                            float defStart = 800.0f;
                            float defEnd = 5000.0f;
                            g_pDevice->SetRenderState(D3DRS_FOGSTART, *(const DWORD*)&defStart);
                            g_pDevice->SetRenderState(D3DRS_FOGEND, *(const DWORD*)&defEnd);
                        }
                    }
                    g_pDevice->SetRenderState(D3DRS_FOGENABLE, s_fogEnabled ? TRUE : FALSE);
                }
                break;

            case 0x0300: // ALPHA_TEST_ENABLE
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_alphaTestEnable = (args[0] != 0);
                    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, s_alphaTestEnable ? TRUE : FALSE);
                }
                break;

            case 0x0304: // BLEND_ENABLE
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, args[0] ? TRUE : FALSE);
                }
                break;

            case 0x0308: // CULL_FACE_ENABLE
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_cullEnable = (args[0] != 0);
                    UpdateD3D9CullMode();
                }
                break;

            case 0x030C: // DEPTH_TEST_ENABLE
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_ZENABLE, args[0] ? D3DZB_TRUE : D3DZB_FALSE);
                }
                break;

            case 0x0314: // NV097_SET_LIGHTING_ENABLE
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_lightingEnabled = (args[0] != 0);
                }
                break;

            case 0x032C: // STENCIL_TEST_ENABLE
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_stencilEnable = (args[0] != 0);
                    g_pDevice->SetRenderState(D3DRS_STENCILENABLE, s_stencilEnable ? TRUE : FALSE);
                    UpdateD3D9CullMode();
                }
                break;

            case 0x033C: // ALPHA_FUNC
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_alphaFunc = ConvertNv2aCompareFuncToD3D(args[0]);
                    g_pDevice->SetRenderState(D3DRS_ALPHAFUNC, s_alphaFunc);
                }
                break;

            case 0x0340: // ALPHA_REF
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_alphaRef = args[0] & 0xFF;
                    g_pDevice->SetRenderState(D3DRS_ALPHAREF, s_alphaRef);
                }
                break;

            case 0x0344: // BLEND_FUNC_SFACTOR
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_SRCBLEND, ConvertNv2aBlendFactorToD3D(args[0]));
                }
                break;

            case 0x0348: // BLEND_FUNC_DFACTOR
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_DESTBLEND, ConvertNv2aBlendFactorToD3D(args[0]));
                }
                break;

            case 0x0350: // BLEND_EQUATION
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    D3DBLENDOP op = D3DBLENDOP_ADD;
                    if (args[0] == 0x800A) op = D3DBLENDOP_SUBTRACT;
                    else if (args[0] == 0x800B) op = D3DBLENDOP_REVSUBTRACT;
                    else if (args[0] == 0xF005 || args[0] == 0x8007) op = D3DBLENDOP_MIN;
                    else if (args[0] == 0xF006 || args[0] == 0x8008) op = D3DBLENDOP_MAX;
                    g_pDevice->SetRenderState(D3DRS_BLENDOP, op);
                }
                break;

            case 0x0354: // DEPTH_FUNC
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_ZFUNC, ConvertNv2aCompareFuncToD3D(args[0]));
                }
                break;

            case 0x0358: // COLOR_MASK (NV097_SET_COLOR_MASK)
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    DWORD mask = 0;
                    if (args[0] & 0x000000FFu) mask |= D3DCOLORWRITEENABLE_BLUE;
                    if (args[0] & 0x0000FF00u) mask |= D3DCOLORWRITEENABLE_GREEN;
                    if (args[0] & 0x00FF0000u) mask |= D3DCOLORWRITEENABLE_RED;
                    if (args[0] & 0xFF000000u) mask |= D3DCOLORWRITEENABLE_ALPHA;
                    s_colorWriteMask = mask;
                    g_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
                }
                break;

            case 0x035C: // DEPTH_MASK
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_zWriteEnable = (args[0] & 1) != 0;
                    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, s_zWriteEnable ? TRUE : FALSE);
                }
                break;

            case 0x0360: // STENCIL_MASK
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK, args[0] & 0xFF);
                }
                break;

            case 0x0364: // STENCIL_FUNC
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_STENCILFUNC, ConvertNv2aCompareFuncToD3D(args[0]));
                }
                break;

            case 0x0368: // STENCIL_FUNC_REF
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_STENCILREF, args[0] & 0xFF);
                }
                break;

            case 0x036C: // STENCIL_FUNC_MASK
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_STENCILMASK, args[0] & 0xFF);
                }
                break;

            case 0x0370: // STENCIL_OP_FAIL
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_STENCILFAIL, ConvertNv2aStencilOpToD3D(args[0]));
                }
                break;

            case 0x0374: // STENCIL_OP_ZFAIL
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_STENCILZFAIL, ConvertNv2aStencilOpToD3D(args[0]));
                }
                break;

            case 0x0378: // STENCIL_OP_ZPASS
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    g_pDevice->SetRenderState(D3DRS_STENCILPASS, ConvertNv2aStencilOpToD3D(args[0]));
                }
                break;

            case 0x037C: // NV097_SET_SHADE_MODE
                if (g_pDevice && count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    DWORD sm = (args[0] == 0x1D00) ? D3DSHADE_FLAT : D3DSHADE_GOURAUD;
                    g_pDevice->SetRenderState(D3DRS_SHADEMODE, sm);
                }
                break;

            case 0x039C: // CULL_FACE
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_cullFace = args[0];
                    UpdateD3D9CullMode();
                }
                break;

            case 0x03A8: // NV097_SET_SCENE_AMBIENT_COLOR
                if (count >= 3) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    const float* fArgs = (const float*)args;
                    s_sceneAmbient[0] = fArgs[0];
                    s_sceneAmbient[1] = fArgs[1];
                    s_sceneAmbient[2] = fArgs[2];
                    if (count >= 4) {
                        s_materialAlpha = fArgs[3];
                    }
                }
                break;

            case 0x03B4: // NV097_SET_MATERIAL_ALPHA
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_materialAlpha = *(const float*)&args[0];
                }
                break;

            case 0x03BC: // NV097_SET_LIGHT_ENABLE_MASK
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_lightEnableMask = args[0];
                    for (int i = 0; i < 8; i++) {
                        s_lights[i].type = (s_lightEnableMask >> (i * 2)) & 3;
                        s_lights[i].enabled = (s_lights[i].type != 0);
                    }
                }
                break;

            case 0x03C0: case 0x03D0: case 0x03E0: case 0x03F0: { // NV097_SET_TEXGEN_S
                uint32_t stage = (method - 0x03C0) / 0x10;
                if (stage < 4 && count >= 1) {
                    if (s_indexBatchCount > 0 && s_texGenMode[stage] != args[0]) FlushD3D9Strip();
                    s_texGenMode[stage] = args[0];
                }
                break;
            }

            case 0x0420: case 0x0424: case 0x0428: case 0x042C: { // NV097_SET_TEXTURE_MATRIX_ENABLE
                if (s_indexBatchCount > 0) FlushD3D9Strip();
                uint32_t stage = (method - 0x0420) / 4;
                for (uint32_t i = 0; i < count && (stage + i) < 4; i++) {
                    s_texMatrixEnable[stage + i] = (args[i] != 0);
                }
                break;
            }

            case 0x0480: // NV097_SET_MODELVIEW_MATRIX
                if (count >= 16) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    memcpy(g_modelViewMatrix, args, 16 * sizeof(float));
                }
                break;

            case 0x0680: // NV097_SET_COMPOSITE_MATRIX
                if (count >= 16) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    D3D9_SetXboxCompositeMatrix((const float*)args, true);
                }
                break;

            case 0x06C0: case 0x0700: case 0x0740: case 0x0780: { // NV097_SET_TEXTURE_MATRIX
                uint32_t stage = (method - 0x06C0) / 0x40;
                if (stage < 4 && count >= 16) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    memcpy(s_texMatrix[stage], args, 16 * sizeof(float));
                }
                break;
            }

            case 0x09C0: // NV097_SET_FOG_PARAMS
                if (g_pDevice) {
                    float fStart = *(const float*)0x0015E508;
                    float fEnd = *(const float*)0x0015E50C;
                    if (fEnd > fStart && fEnd > 10.0f && fEnd < 100000.0f) {
                        g_pDevice->SetRenderState(D3DRS_FOGSTART, *(const DWORD*)&fStart);
                        g_pDevice->SetRenderState(D3DRS_FOGEND, *(const DWORD*)&fEnd);
                    }
                }
                break;

            case 0x0A60: // NV097_SET_COMBINER_FACTOR0 (emitted by D3DDevice_SetPixelShader at 0x001520C5)
                if (count >= 1) {
                    uint32_t newC0A = (args[0] >> 24) & 0xFFu;
                    uint32_t newRgbIn1 = (count >= 26) ? args[25] : s_psRgbInput1;
                    if (s_indexBatchCount > 0 && (s_combinerFactor0Alpha != newC0A || s_psRgbInput1 != newRgbIn1)) {
                        FlushD3D9Strip();
                    }
                    s_combinerFactor0Alpha = newC0A;
                    if (count >= 26) {
                        s_psRgbInput1 = newRgbIn1;
                    }
                }
                break;

            case 0x0C00: // NV097_SET_SCENE_AMBIENT_COLOR
                if (count >= 3) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    s_sceneAmbient[0] = *(const float*)&args[0];
                    s_sceneAmbient[1] = *(const float*)&args[1];
                    s_sceneAmbient[2] = *(const float*)&args[2];
                }
                break;

            case 0x0B80: // NV097_SET_TRANSFORM_CONSTANT
                if (s_vsConstLoad == 0xA7 && count >= 16) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    D3D9_SetXboxCompositeMatrix((const float*)args, false);
                }
                s_vsConstLoad += count / 4;
                break;

            case 0x1B00: case 0x1B40: case 0x1B80: case 0x1BC0: { // TEXTURE_OFFSET
                uint32_t stage = (method - 0x1B00) / 0x40;
                if (stage < 4) {
                    if (count >= 1 && s_texOffset[stage] != args[0]) {
                        if (s_indexBatchCount > 0) FlushD3D9Strip();
                        s_texOffset[stage] = args[0];
                    }
                    if (count >= 2) s_texFormat[stage] = args[1];
                }
                if (count >= 3) ApplyNv2aTexAddress(stage, args[2]);
                if (count >= 4) ApplyNv2aTexControl0(stage, args[3]);
                break;
            }

            case 0x1B04: case 0x1B44: case 0x1B84: case 0x1BC4: { // TEXTURE_FORMAT
                uint32_t stage = (method - 0x1B04) / 0x40;
                if (stage < 4 && count >= 1) {
                    if (s_indexBatchCount > 0 && s_texFormat[stage] != args[0]) FlushD3D9Strip();
                    s_texFormat[stage] = args[0];
                }
                if (count >= 2) ApplyNv2aTexAddress(stage, args[1]);
                if (count >= 3) ApplyNv2aTexControl0(stage, args[2]);
                break;
            }

            case 0x1B08: case 0x1B48: case 0x1B88: case 0x1BC8: { // NV097_SET_TEXTURE_ADDRESS
                uint32_t stage = (method - 0x1B08) / 0x40;
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    ApplyNv2aTexAddress(stage, args[0]);
                }
                if (count >= 2) {
                    ApplyNv2aTexControl0(stage, args[1]);
                }
                if (count >= 4) {
                    ApplyNv2aTexFilter(stage, args[3]);
                }
                break;
            }

            case 0x1B0C: case 0x1B4C: case 0x1B8C: case 0x1BCC: { // TEXTURE_CONTROL0
                uint32_t stage = (method - 0x1B0C) / 0x40;
                if (count >= 1) {
                    ApplyNv2aTexControl0(stage, args[0]);
                }
                break;
            }

            case 0x1B14: case 0x1B54: case 0x1B94: case 0x1BD4: { // NV097_SET_TEXTURE_FILTER
                uint32_t stage = (method - 0x1B14) / 0x40;
                if (count >= 1) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    ApplyNv2aTexFilter(stage, args[0]);
                }
                break;
            }

            case 0x1B20: case 0x1B60: case 0x1BA0: case 0x1BE0: { // TEXTURE_PALETTE
                if (s_indexBatchCount > 0) FlushD3D9Strip();
                uint32_t stage = (method - 0x1B20) / 0x40;
                if (stage < 4 && count >= 1) s_texPalette[stage] = args[0];
                break;
            }

            case 0x1720: case 0x1724: case 0x1728: case 0x172C:
            case 0x1730: case 0x1734: case 0x1738: case 0x173C:
            case 0x1740: case 0x1744: case 0x1748: case 0x174C:
            case 0x1750: case 0x1754: case 0x1758: case 0x175C: {
                if (s_indexBatchCount > 0) FlushD3D9Strip();
                uint32_t slot = (method - 0x1720) / 4;
                for (uint32_t i = 0; i < count && (slot + i) < 16; i++) {
                    s_vtxOffset[slot + i] = args[i];
                }
                break;
            }

            case 0x1760: case 0x1764: case 0x1768: case 0x176C:
            case 0x1770: case 0x1774: case 0x1778: case 0x177C:
            case 0x1780: case 0x1784: case 0x1788: case 0x178C:
            case 0x1790: case 0x1794: case 0x1798: case 0x179C: {
                if (s_indexBatchCount > 0) FlushD3D9Strip();
                uint32_t slot = (method - 0x1760) / 4;
                for (uint32_t i = 0; i < count && (slot + i) < 16; i++) {
                    s_vtxStride[slot + i] = (args[i] >> 8) & 0xFF;
                    s_vtxFormat[slot + i] = args[i] & 0xFF;
                }
                break;
            }

            case 0x17FC: // NV097_SET_BEGIN_END
                for (uint32_t i = 0; i < count; i++) {
                    HandleD3D9SetBeginEnd(args[i]);
                }
                break;

            case 0x1800: // NV097_ARRAY_ELEMENT16
                HandleD3D9ArrayElement16(args, count);
                break;

            case 0x1808: // NV097_ARRAY_ELEMENT32
                for (uint32_t i = 0; i < count; i++) {
                    HandleD3D9ArrayElementSingle(args[i]);
                }
                break;

            case 0x1818: // INLINE_ARRAY (Batched 2D/3D geometry)
                HandleD3D9InlineArray(args, count);
                break;

            case 0x1D7C: // NV097_SET_ANTI_ALIASING_CONTROL
                if (count >= 1 && g_pDevice) {
                    g_pDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, (args[0] & 1) ? TRUE : FALSE);
                }
                break;

            case 0x1D8C: // NV097_SET_ZSTENCIL_CLEAR_VALUE
                if (s_indexBatchCount > 0) FlushD3D9Strip();
                if (count >= 1) s_clearZStencil = args[0];
                if (count >= 2) s_clearColor = args[1];
                if (count >= 3) {
                    float clearZ = (float)(s_clearZStencil >> 8) / 16777215.0f;
                    D3D9_Clear(args[2], s_clearColor, clearZ, s_clearZStencil & 0xFF);
                }
                break;

            case 0x1D90: // NV097_SET_COLOR_CLEAR_VALUE
                if (s_indexBatchCount > 0) FlushD3D9Strip();
                if (count >= 1) s_clearColor = args[0];
                if (count >= 2) {
                    float clearZ = (float)(s_clearZStencil >> 8) / 16777215.0f;
                    D3D9_Clear(args[1], s_clearColor, clearZ, s_clearZStencil & 0xFF);
                }
                break;

            case 0x1D94: // NV097_CLEAR_SURFACE
                if (s_indexBatchCount > 0) FlushD3D9Strip();
                if (count >= 1) {
                    float clearZ = (float)(s_clearZStencil >> 8) / 16777215.0f;
                    D3D9_Clear(args[0], s_clearColor, clearZ, s_clearZStencil & 0xFF);
                }
                break;

            case 0x1E94: // TRANSFORM_EXECUTION_MODE
                if (count >= 1) s_is3DMode = (args[0] == 6);
                break;

            case 0x1EA4: // TRANSFORM_CONSTANT_LOAD
                if (count >= 1) s_vsConstLoad = args[0];
                break;

            default:
                if (method >= 0x1000 && method <= 0x13FC) {
                    if (s_indexBatchCount > 0) FlushD3D9Strip();
                    uint32_t lightIdx = (method - 0x1000) / 0x80;
                    uint32_t lightSub = (method - 0x1000) % 0x80;
                    if (lightIdx < 8) {
                        if (lightSub == 0x00) {
                            if (count >= 3) {
                                s_lights[lightIdx].ambient[0] = *(const float*)&args[0];
                                s_lights[lightIdx].ambient[1] = *(const float*)&args[1];
                                s_lights[lightIdx].ambient[2] = *(const float*)&args[2];
                            }
                            if (count >= 6) {
                                s_lights[lightIdx].diffuse[0] = *(const float*)&args[3];
                                s_lights[lightIdx].diffuse[1] = *(const float*)&args[4];
                                s_lights[lightIdx].diffuse[2] = *(const float*)&args[5];
                            }
                            if (count >= 9) {
                                s_lights[lightIdx].specular[0] = *(const float*)&args[6];
                                s_lights[lightIdx].specular[1] = *(const float*)&args[7];
                                s_lights[lightIdx].specular[2] = *(const float*)&args[8];
                            }
                        } else if (lightSub == 0x0C) {
                            if (count >= 3) {
                                s_lights[lightIdx].diffuse[0] = *(const float*)&args[0];
                                s_lights[lightIdx].diffuse[1] = *(const float*)&args[1];
                                s_lights[lightIdx].diffuse[2] = *(const float*)&args[2];
                            }
                        } else if (lightSub == 0x18) {
                            if (count >= 3) {
                                s_lights[lightIdx].specular[0] = *(const float*)&args[0];
                                s_lights[lightIdx].specular[1] = *(const float*)&args[1];
                                s_lights[lightIdx].specular[2] = *(const float*)&args[2];
                            }
                        } else if (lightSub == 0x24) {
                            if (count >= 1) s_lights[lightIdx].range = *(const float*)&args[0];
                        } else if (lightSub == 0x28) {
                            if (count >= 6) {
                                s_lights[lightIdx].dir[0] = *(const float*)&args[3];
                                s_lights[lightIdx].dir[1] = *(const float*)&args[4];
                                s_lights[lightIdx].dir[2] = *(const float*)&args[5];
                            }
                        } else if (lightSub == 0x34) {
                            if (count >= 3) {
                                s_lights[lightIdx].dir[0] = *(const float*)&args[0];
                                s_lights[lightIdx].dir[1] = *(const float*)&args[1];
                                s_lights[lightIdx].dir[2] = *(const float*)&args[2];
                            }
                        } else if (lightSub == 0x5C) {
                            if (count >= 3) {
                                s_lights[lightIdx].pos[0] = *(const float*)&args[0];
                                s_lights[lightIdx].pos[1] = *(const float*)&args[1];
                                s_lights[lightIdx].pos[2] = *(const float*)&args[2];
                            }
                        }
                    }
                }
                break;
        }
    }

    s_lastPushPut = currentPut;
}

} // extern "C"
