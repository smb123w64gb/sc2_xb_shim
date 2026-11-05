#include "d3d9_internal.h"

struct D3D9TexCacheEntry {
    uint32_t texGpu;
    uint32_t palGpu;
    uint32_t contentHash;
    uint32_t lastFrame;
    uint16_t w;
    uint16_t h;
    uint8_t fmt;
    uint8_t pad;
    IDirect3DTexture9* pTex;
};

static constexpr uint32_t MAX_D3D9_TEX_CACHE = 4096;
static D3D9TexCacheEntry s_texCache[MAX_D3D9_TEX_CACHE];
static uint32_t s_texCacheCount = 0;

void D3D9_ReleaseAllTextures(void) {
    for (uint32_t i = 0; i < s_texCacheCount; i++) {
        if (s_texCache[i].pTex) {
            s_texCache[i].pTex->Release();
            s_texCache[i].pTex = nullptr;
        }
    }
    s_texCacheCount = 0;
}

static inline void GenerateSwizzleMasks(uint32_t width, uint32_t height, uint32_t* maskX, uint32_t* maskY) {
    uint32_t x = 0, y = 0, bit = 1, maskBit = 1;
    bool done = false;
    while (!done) {
        done = true;
        if (bit < width)  { x |= maskBit; maskBit <<= 1; done = false; }
        if (bit < height) { y |= maskBit; maskBit <<= 1; done = false; }
        bit <<= 1;
    }
    *maskX = x;
    *maskY = y;
}

static inline uint32_t FillSwizzlePattern(uint32_t pattern, uint32_t value) {
    uint32_t result = 0, bit = 1;
    while (value != 0) {
        if (pattern & bit) {
            if (value & 1) result |= bit;
            value >>= 1;
        }
        bit <<= 1;
    }
    return result;
}

static inline uint32_t Expand565To888(uint16_t c) {
    uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint32_t g = ((c >> 5)  & 0x3F) * 255 / 63;
    uint32_t b = (c         & 0x1F) * 255 / 31;
    return (r << 16) | (g << 8) | b;
}

static uint32_t ComputeTextureHash(const void* pTexCpu, uint32_t w, uint32_t h, uint32_t fmt, const void* pPalCpu) {
    uint32_t hash = 2166136261u;
    if (pTexCpu && (uintptr_t)pTexCpu >= 0x00010000) {
        uint32_t byteSize = w * h;
        if (fmt == 0x06 || fmt == 0x07 || fmt == 0x12 || fmt == 0x1E) byteSize = w * h * 4;
        else if (fmt == 0x02 || fmt == 0x03 || fmt == 0x04 || fmt == 0x05 || fmt == 0x1A || fmt == 0x24) byteSize = w * h * 2;
        else if (fmt == 0x0C) byteSize = (w * h) / 2;
        if (byteSize < 64) byteSize = 64;

        uintptr_t maxBytes = 0xFFFFFFFFu - (uintptr_t)pTexCpu;
        if (byteSize > maxBytes) byteSize = (uint32_t)maxBytes;

        const uint32_t* dwords = (const uint32_t*)pTexCpu;
        uint32_t totalDwords = byteSize / 4;
        uint32_t step = totalDwords / 64;
        if (step == 0) step = 1;
        for (uint32_t i = 0; i < totalDwords; i += step) {
            hash = (hash ^ dwords[i]) * 16777619u;
        }
    }
    if (fmt == 0x0B && pPalCpu && (uintptr_t)pPalCpu >= 0x00010000) {
        const uint32_t* pal = (const uint32_t*)pPalCpu;
        for (uint32_t i = 0; i < 256; i += 4) {
            hash = (hash ^ pal[i]) * 16777619u;
        }
    }
    return hash;
}

static void DecodeTexturePixels(IDirect3DTexture9* pTex, const void* pTexCpu, const void* pPalCpu, uint32_t w, uint32_t h, uint32_t fmt) {
    D3DLOCKED_RECT lr;
    if (FAILED(pTex->LockRect(0, &lr, nullptr, 0))) return;

    uint32_t maskX = 0, maskY = 0;
    GenerateSwizzleMasks(w, h, &maskX, &maskY);
    uint32_t* dstRow = (uint32_t*)lr.pBits;
    uint32_t pitchPixels = lr.Pitch / 4;

    static uint32_t s_swizX[2048];
    for (uint32_t x = 0; x < w && x < 2048; x++) {
        s_swizX[x] = FillSwizzlePattern(maskX, x);
    }

    if (fmt == 0x0B && pPalCpu) {
        // P8 Swizzled Paletted (Xbox native ARGB palette)
        const uint8_t* srcIndices = (const uint8_t*)pTexCpu;
        const uint32_t* pal = (const uint32_t*)pPalCpu;

        bool palHasAlpha = false;
        for (int i = 0; i < 256; i++) {
            if ((pal[i] >> 24) != 0) {
                palHasAlpha = true;
                break;
            }
        }

        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint8_t idx = srcIndices[s_swizX[x] | swizY];
                uint32_t color = pal[idx];
                if (!palHasAlpha) {
                    color |= 0xFF000000u;
                }
                dstRow[y * pitchPixels + x] = color;
            }
        }
    } else if (fmt == 0x06 || fmt == 0x07) {
        // Swizzled A8R8G8B8 / X8R8G8B8
        const uint32_t* srcPixels = (const uint32_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t c = srcPixels[s_swizX[x] | swizY];
                if (fmt == 0x07) c |= 0xFF000000u;
                dstRow[y * pitchPixels + x] = c;
            }
        }
    } else if (fmt == 0x02 || fmt == 0x03) {
        // Swizzled A1R5G5B5 / X1R5G5B5
        const uint16_t* src16 = (const uint16_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = src16[s_swizX[x] | swizY];
                uint32_t a = (fmt == 0x03) ? 255 : ((c & 0x8000) ? 255 : 0);
                uint32_t r = ((c >> 10) & 0x1F) * 255 / 31;
                uint32_t g = ((c >> 5)  & 0x1F) * 255 / 31;
                uint32_t b = (c         & 0x1F) * 255 / 31;
                dstRow[y * pitchPixels + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    } else if (fmt == 0x04) {
        // Swizzled A4R4G4B4
        const uint16_t* src16 = (const uint16_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = src16[s_swizX[x] | swizY];
                uint32_t a = ((c >> 12) & 0xF) * 17;
                uint32_t r = ((c >> 8)  & 0xF) * 17;
                uint32_t g = ((c >> 4)  & 0xF) * 17;
                uint32_t b = (c         & 0xF) * 17;
                dstRow[y * pitchPixels + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    } else if (fmt == 0x05) {
        // Swizzled R5G6B5
        const uint16_t* src16 = (const uint16_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = src16[s_swizX[x] | swizY];
                dstRow[y * pitchPixels + x] = 0xFF000000u | Expand565To888(c);
            }
        }
    } else if (fmt == 0x00 || fmt == 0x01 || fmt == 0x19) {
        // Swizzled L8 / AL8 / A8
        const uint8_t* src8 = (const uint8_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = src8[s_swizX[x] | swizY];
                uint32_t a = (fmt == 0x00) ? 255 : v;
                uint32_t l = (fmt == 0x19) ? 255 : v;
                dstRow[y * pitchPixels + x] = (a << 24) | (l << 16) | (l << 8) | l;
            }
        }
    } else if (fmt == 0x1A) {
        // Swizzled A8L8
        const uint16_t* src16 = (const uint16_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = src16[s_swizX[x] | swizY];
                uint32_t a = (c >> 8) & 0xFF;
                uint32_t l = c & 0xFF;
                dstRow[y * pitchPixels + x] = (a << 24) | (l << 16) | (l << 8) | l;
            }
        }
    } else if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {
        // Linear Compressed: DXT1 (0x0C), DXT3 (0x0E), DXT5 (0x0F)
        const uint8_t* blockPtr = (const uint8_t*)pTexCpu;
        uint32_t blocksX = (w + 3) / 4;
        uint32_t blocksY = (h + 3) / 4;
        uint32_t blockSize = (fmt == 0x0C) ? 8 : 16;

        for (uint32_t by = 0; by < blocksY; by++) {
            for (uint32_t bx = 0; bx < blocksX; bx++) {
                const uint8_t* b = blockPtr + (by * blocksX + bx) * blockSize;
                const uint8_t* colorBlock = (fmt == 0x0C) ? b : (b + 8);

                uint16_t c0 = *(const uint16_t*)(colorBlock + 0);
                uint16_t c1 = *(const uint16_t*)(colorBlock + 2);
                uint32_t bits = *(const uint32_t*)(colorBlock + 4);

                uint32_t rgb[4];
                rgb[0] = Expand565To888(c0);
                rgb[1] = Expand565To888(c1);
                uint32_t r0 = (rgb[0] >> 16) & 0xFF, g0 = (rgb[0] >> 8) & 0xFF, b0 = rgb[0] & 0xFF;
                uint32_t r1 = (rgb[1] >> 16) & 0xFF, g1 = (rgb[1] >> 8) & 0xFF, b1 = rgb[1] & 0xFF;

                if (c0 > c1 || fmt != 0x0C) {
                    rgb[2] = (((2 * r0 + r1) / 3) << 16) | (((2 * g0 + g1) / 3) << 8) | ((2 * b0 + b1) / 3);
                    rgb[3] = (((r0 + 2 * r1) / 3) << 16) | (((g0 + 2 * g1) / 3) << 8) | ((b0 + 2 * b1) / 3);
                } else {
                    rgb[2] = (((r0 + r1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((b0 + b1) / 2);
                    rgb[3] = 0;
                }

                uint8_t aLut[8];
                uint64_t aBits = 0;
                if (fmt == 0x0F) {
                    aLut[0] = b[0];
                    aLut[1] = b[1];
                    if (aLut[0] > aLut[1]) {
                        for (int k = 1; k <= 6; k++) aLut[k + 1] = (uint8_t)(((7 - k) * aLut[0] + k * aLut[1]) / 7);
                    } else {
                        for (int k = 1; k <= 4; k++) aLut[k + 1] = (uint8_t)(((5 - k) * aLut[0] + k * aLut[1]) / 5);
                        aLut[6] = 0;
                        aLut[7] = 255;
                    }
                    for (int k = 0; k < 6; k++) aBits |= ((uint64_t)b[2 + k]) << (8 * k);
                }

                for (uint32_t py = 0; py < 4; py++) {
                    uint32_t dy = by * 4 + py;
                    if (dy >= h) continue;
                    uint16_t dxt3AlphaRow = (fmt == 0x0E) ? *(const uint16_t*)(b + py * 2) : 0;
                    for (uint32_t px = 0; px < 4; px++) {
                        uint32_t dx = bx * 4 + px;
                        if (dx >= w) continue;
                        uint32_t pIdx = py * 4 + px;
                        uint32_t cIdx = (bits >> (pIdx * 2)) & 3;
                        uint32_t alpha = 255;
                        if (fmt == 0x0C) {
                            if (c0 <= c1 && cIdx == 3) alpha = 0;
                        } else if (fmt == 0x0E) {
                            alpha = ((dxt3AlphaRow >> (px * 4)) & 0xF) * 17;
                        } else if (fmt == 0x0F) {
                            alpha = aLut[(aBits >> (pIdx * 3)) & 7];
                        }
                        dstRow[dy * pitchPixels + dx] = (alpha << 24) | rgb[cIdx];
                    }
                }
            }
        }
    } else if (fmt == 0x24) {
        // Linear YUY2 video frame
        const uint8_t* src = (const uint8_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x += 2) {
                uint32_t i = y * w + x;
                int c0 = (int)src[i * 2 + 0] - 16;
                int d  = (int)src[i * 2 + 1] - 128;
                int c1 = (int)src[i * 2 + 2] - 16;
                int e  = (int)src[i * 2 + 3] - 128;

                int r0 = (298 * c0 + 409 * e + 128) >> 8;
                int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
                int b0 = (298 * c0 + 516 * d + 128) >> 8;
                int r1 = (298 * c1 + 409 * e + 128) >> 8;
                int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
                int b1 = (298 * c1 + 516 * d + 128) >> 8;

                uint32_t cr0 = (r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
                uint32_t cg0 = (g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
                uint32_t cb0 = (b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));
                uint32_t cr1 = (r1 < 0 ? 0 : (r1 > 255 ? 255 : r1));
                uint32_t cg1 = (g1 < 0 ? 0 : (g1 > 255 ? 255 : g1));
                uint32_t cb1 = (b1 < 0 ? 0 : (b1 > 255 ? 255 : b1));

                dstRow[y * pitchPixels + x]     = 0xFF000000u | (cr0 << 16) | (cg0 << 8) | cb0;
                if (x + 1 < w) {
                    dstRow[y * pitchPixels + x + 1] = 0xFF000000u | (cr1 << 16) | (cg1 << 8) | cb1;
                }
            }
        }
    } else {
        // Linear 32-bit fallback (0x12, 0x1E, etc.)
        const uint32_t* srcPixels = (const uint32_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t c = srcPixels[y * w + x];
                if (fmt == 0x1E) c |= 0xFF000000u;
                dstRow[y * pitchPixels + x] = c;
            }
        }
    }

    pTex->UnlockRect(0);
}

extern "C" {

IDirect3DTexture9* D3D9_GetOrCreateTexture(uint32_t texOffset, uint32_t texFormat, uint32_t texPalette) {
    if (!g_pDevice || !texOffset) return nullptr;

    void* pTexCpu = XboxKrnl_GpuToCpu(texOffset);
    if (!pTexCpu || (uintptr_t)pTexCpu < 0x00010000) {
        return nullptr;
    }

    uint32_t fmt = (texFormat >> 8) & 0xFF;
    uint32_t w = 1 << ((texFormat >> 20) & 0xF);
    uint32_t h = 1 << ((texFormat >> 24) & 0xF);

    if (w == 0 || h == 0 || w > 2048 || h > 2048) {
        return nullptr;
    }

    void* pPalCpu = nullptr;
    if (fmt == 0x0B) {
        if (texPalette >= 0x00010000) {
            pPalCpu = XboxKrnl_GpuToCpu(texPalette & ~0x3FU);
        }
        if (!pPalCpu || (uintptr_t)pPalCpu < 0x00010000) {
            static uint32_t s_fallbackPal[256] = {0};
            pPalCpu = s_fallbackPal;
        }
    } else {
        texPalette = 0;
    }

    uint32_t contentHash = ComputeTextureHash(pTexCpu, w, h, fmt, pPalCpu);

    // 1. Exact match in cache
    for (uint32_t i = 0; i < s_texCacheCount; i++) {
        if (s_texCache[i].texGpu == texOffset &&
            s_texCache[i].w == (uint16_t)w &&
            s_texCache[i].h == (uint16_t)h &&
            s_texCache[i].fmt == (uint8_t)fmt &&
            s_texCache[i].palGpu == texPalette &&
            s_texCache[i].pTex != nullptr) {

            s_texCache[i].lastFrame = g_frameCounter;

            if (s_texCache[i].contentHash != contentHash || fmt == 0x24) {
                DecodeTexturePixels(s_texCache[i].pTex, pTexCpu, pPalCpu, w, h, fmt);
                s_texCache[i].contentHash = contentHash;
            }
            return s_texCache[i].pTex;
        }
    }

    // 2. Select slot for new texture
    uint32_t slot = 0xFFFFFFFFu;
    if (s_texCacheCount < MAX_D3D9_TEX_CACHE) {
        slot = s_texCacheCount++;
    } else {
        uint32_t oldestFrame = 0xFFFFFFFFu;
        uint32_t lruIdx = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < s_texCacheCount; i++) {
            if (s_texCache[i].lastFrame < oldestFrame) {
                oldestFrame = s_texCache[i].lastFrame;
                lruIdx = i;
            }
        }
        slot = (lruIdx != 0xFFFFFFFFu) ? lruIdx : 0;
    }

    if (s_texCache[slot].pTex) {
        if (s_texCache[slot].w == (uint16_t)w && s_texCache[slot].h == (uint16_t)h) {
            DecodeTexturePixels(s_texCache[slot].pTex, pTexCpu, pPalCpu, w, h, fmt);
            s_texCache[slot].texGpu = texOffset;
            s_texCache[slot].palGpu = texPalette;
            s_texCache[slot].fmt = (uint8_t)fmt;
            s_texCache[slot].contentHash = contentHash;
            s_texCache[slot].lastFrame = g_frameCounter;
            return s_texCache[slot].pTex;
        }

        for (DWORD s = 0; s < 4; s++) {
            if (g_boundTextures[s] == s_texCache[slot].pTex) {
                g_pDevice->SetTexture(s, nullptr);
                g_boundTextures[s] = nullptr;
            }
        }
        s_texCache[slot].pTex->Release();
        s_texCache[slot].pTex = nullptr;
    }

    IDirect3DTexture9* pNewTex = nullptr;
    HRESULT hr = g_pDevice->CreateTexture(
        w, h,
        1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &pNewTex,
        nullptr
    );

    if (FAILED(hr) || !pNewTex) {
        D3D9_Log("[D3D9_TEX] Fatal: CreateTexture failed (%ux%u, hr=0x%08lX)\n", w, h, hr);
        return nullptr;
    }

    DecodeTexturePixels(pNewTex, pTexCpu, pPalCpu, w, h, fmt);

    s_texCache[slot].texGpu = texOffset;
    s_texCache[slot].palGpu = texPalette;
    s_texCache[slot].w = (uint16_t)w;
    s_texCache[slot].h = (uint16_t)h;
    s_texCache[slot].fmt = (uint8_t)fmt;
    s_texCache[slot].pad = 0;
    s_texCache[slot].contentHash = contentHash;
    s_texCache[slot].lastFrame = g_frameCounter;
    s_texCache[slot].pTex = pNewTex;

    return pNewTex;
}

IDirect3DTexture9* D3D9_ResolveNamcoTexture(uint32_t texParam, uint16_t subIdx, uint16_t fmtFlag) {
    if (!texParam || texParam < 0x00010000 || (texParam & 3) != 0) return nullptr;

    const uint32_t* texStruct = nullptr;

    if (fmtFlag == 0) {
        // VXT container pointer (FUN_00031570 / FUN_0002d8a0)
        const uint8_t* vxtHeader = (const uint8_t*)XboxKrnl_GpuToCpu(texParam);
        if (!vxtHeader) return nullptr;
        if (vxtHeader[4] == 2) {
            uint32_t arrayBase = *(const uint32_t*)(vxtHeader + 0x0C);
            if (arrayBase < 0x00010000) arrayBase += texParam;
            const uint8_t* pArray = (const uint8_t*)XboxKrnl_GpuToCpu(arrayBase);
            if (!pArray) return nullptr;
            texStruct = (const uint32_t*)(pArray + (uint32_t)subIdx * 0x24);
        } else {
            uint32_t defVxtAddr = *(const uint32_t*)0x0038E580;
            if (defVxtAddr >= 0x00010000) {
                const uint8_t* defVxt = (const uint8_t*)XboxKrnl_GpuToCpu(defVxtAddr);
                if (defVxt && defVxt[4] == 2) {
                    uint32_t arrayBase = *(const uint32_t*)(defVxt + 0x0C);
                    if (arrayBase < 0x00010000) arrayBase += defVxtAddr;
                    const uint8_t* pArray = (const uint8_t*)XboxKrnl_GpuToCpu(arrayBase);
                    if (pArray) {
                        texStruct = (const uint32_t*)(pArray + (uint32_t)subIdx * 0x24);
                    }
                }
            }
            if (!texStruct) {
                texStruct = (const uint32_t*)vxtHeader;
            }
        }
    } else {
        // Direct 0x24-byte texture descriptor struct (FUN_00031670 / FUN_00025460)
        texStruct = (const uint32_t*)XboxKrnl_GpuToCpu(texParam);
    }

    if (!texStruct) return nullptr;

    uint32_t palObjAddr = texStruct[0];
    uint32_t texGpu     = texStruct[5];
    uint32_t texFormat  = texStruct[7];
    uint32_t texPalette = 0;

    if (palObjAddr >= 0x00010000) {
        const uint32_t* palObj = (const uint32_t*)XboxKrnl_GpuToCpu(palObjAddr);
        if (palObj) {
            uint32_t cand0 = palObj[0];
            uint32_t cand1 = palObj[1];
            if (cand1 >= 0x00010000) {
                texPalette = cand1 & ~0x3FU;
            } else if (cand0 >= 0x00010000) {
                texPalette = cand0 & ~0x3FU;
            } else {
                texPalette = palObjAddr & ~0x3FU;
            }
        } else {
            texPalette = palObjAddr & ~0x3FU;
        }
    }

    if (!texGpu) return nullptr;
    return D3D9_GetOrCreateTexture(texGpu, texFormat, texPalette);
}

} // extern "C"
