#include "nv2a_bridge.h"
#include "xboxkrnl.h"
#include <windows.h>
#include <gl/gl.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "opengl32.lib")

static HGLRC s_hGLRC = NULL;
static bool s_glInitialized = false;
static uint32_t s_lastPushPut = 0;
static bool s_inBegin = false;
static int s_currentPrimitive = 0;
static uint32_t s_clearColor = 0;
static uint32_t s_vertexCount = 0;
static uint32_t s_drawCount = 0;
static uint32_t s_totalFrames = 0;
static uint32_t s_count_17fc = 0;
static uint32_t s_count_1818 = 0;
static uint32_t s_count_1d98 = 0;
static uint32_t s_count_other = 0;
static uint32_t s_lastMethod = 0;

static uint32_t s_texOffset = 0;
static uint32_t s_texFormat = 0;
static uint32_t s_texPalette = 0;
static uint32_t s_texControl0 = 0;
static bool s_texEnabled = false;

static void logFmt(const char* fmt, ...);

static void LogMemInfo(const char* label, uint32_t addr) {
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
        logFmt("[MEM_INFO] %s (0x%08X): AllocBase=0x%08X, Base=0x%08X, Size=0x%08X, State=0x%X, Protect=0x%X\n",
               label, addr, (uint32_t)mbi.AllocationBase, (uint32_t)mbi.BaseAddress, mbi.RegionSize, mbi.State, mbi.Protect);
    } else {
        logFmt("[MEM_INFO] %s (0x%08X): VirtualQuery failed (%lu)\n", label, addr, GetLastError());
    }
}
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

bool NV2A_InitGL(HWND hWnd, HDC hDC) {
    if (s_glInitialized) return true;

    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),
        1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA,
        32,
        0, 0, 0, 0, 0, 0,
        0,
        0,
        0,
        0, 0, 0, 0,
        24, // 24-bit Z-buffer
        8,  // 8-bit Stencil
        0,
        PFD_MAIN_PLANE,
        0,
        0, 0, 0
    };

    int format = ChoosePixelFormat(hDC, &pfd);
    if (!format) {
        logFmt("[NV2A_GL] Error: ChoosePixelFormat failed: %lu\n", GetLastError());
        return false;
    }

    if (!SetPixelFormat(hDC, format, &pfd)) {
        logFmt("[NV2A_GL] Error: SetPixelFormat failed: %lu\n", GetLastError());
        return false;
    }

    s_hGLRC = wglCreateContext(hDC);
    if (!s_hGLRC) {
        logFmt("[NV2A_GL] Error: wglCreateContext failed: %lu\n", GetLastError());
        return false;
    }

    if (!wglMakeCurrent(hDC, s_hGLRC)) {
        logFmt("[NV2A_GL] Error: wglMakeCurrent failed: %lu\n", GetLastError());
        return false;
    }

    const char* rend = (const char*)glGetString(GL_RENDERER);
    const char* ver  = (const char*)glGetString(GL_VERSION);
    const char* vend = (const char*)glGetString(GL_VENDOR);
    logFmt("[NV2A_GL] OpenGL initialized successfully!\n");
    logFmt("  Renderer: %s\n", rend ? rend : "unknown");
    logFmt("  Version:  %s\n", ver  ? ver  : "unknown");
    logFmt("  Vendor:   %s\n", vend ? vend : "unknown");

    // Setup default 640x480 coordinate space
    RECT rc;
    GetClientRect(hWnd, &rc);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    if (winW <= 0) winW = 1280;
    if (winH <= 0) winH = 960;

    glViewport(0, 0, winW, winH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 640.0, 480.0, 0.0, 0.1, -1.1); // 640x480 top-down left-handed (Z=0..1 maps to depth 0..1)
    glDepthRange(0.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.01f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    s_glInitialized = true;
    return true;
}

static GLenum ConvertNv2aPrimitive(uint32_t op) {
    switch (op) {
        case 1:  return GL_POINTS;
        case 2:  return GL_LINES;
        case 3:  return GL_LINE_LOOP;
        case 4:  return GL_LINE_STRIP;
        case 5:  return GL_TRIANGLES;
        case 6:  return GL_TRIANGLE_STRIP;
        case 7:  return GL_TRIANGLE_FAN;
        case 8:  return GL_QUADS;
        case 9:  return GL_QUAD_STRIP;
        case 10: return GL_POLYGON;
        default: return GL_TRIANGLES;
    }
}

static GLenum ConvertNv2aCompareFunc(uint32_t func) {
    switch (func) {
        case 0x0200: return GL_NEVER;
        case 0x0201: return GL_LESS;
        case 0x0202: return GL_EQUAL;
        case 0x0203: return GL_LEQUAL;
        case 0x0204: return GL_GREATER;
        case 0x0205: return GL_NOTEQUAL;
        case 0x0206: return GL_GEQUAL;
        case 0x0207: return GL_ALWAYS;
        default:     return GL_LEQUAL;
    }
}

static GLenum ConvertNv2aBlendFactor(uint32_t factor) {
    switch (factor) {
        case 0x0000: return GL_ZERO;
        case 0x0001: return GL_ONE;
        case 0x0300: return GL_SRC_COLOR;
        case 0x0301: return GL_ONE_MINUS_SRC_COLOR;
        case 0x0302: return GL_SRC_ALPHA;
        case 0x0303: return GL_ONE_MINUS_SRC_ALPHA;
        case 0x0304: return GL_DST_ALPHA;
        case 0x0305: return GL_ONE_MINUS_DST_ALPHA;
        case 0x0306: return GL_DST_COLOR;
        case 0x0307: return GL_ONE_MINUS_DST_COLOR;
        case 0x0308: return GL_SRC_ALPHA_SATURATE;
        default:     return GL_ONE;
    }
}

static GLenum s_alphaFunc = GL_GREATER;
static float  s_alphaRef = 0.01f;
static GLenum s_blendSrc = GL_SRC_ALPHA;
static GLenum s_blendDst = GL_ONE_MINUS_SRC_ALPHA;
static uint32_t s_unhandledMethods[0x2000] = {0};

static uint32_t s_vtxOffset[16] = {0};
static uint32_t s_vtxStride[16] = {0};
static uint32_t s_vtxFormat[16] = {0};
static bool s_is3DMode = false;
static float s_projMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static float s_modelViewMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static uint32_t s_count_1800 = 0;
static uint32_t s_count_1808 = 0;

typedef struct {
    uint32_t texGpu;
    uint32_t palGpu;
    uint32_t format;
    GLuint glTexId;
} TEX_CACHE_ENTRY;

#define MAX_TEX_CACHE 1024
static TEX_CACHE_ENTRY s_texCache[MAX_TEX_CACHE];
static uint32_t s_texCacheCount = 0;

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

static GLuint GetOrCreateTexture(uint32_t texOffset, uint32_t texFormat, uint32_t texPalette) {
    if (!texOffset) return 0;

    void* pTexCpu = XboxKrnl_GpuToCpu(texOffset);
    void* pPalCpu = XboxKrnl_GpuToCpu(texPalette);
    uint32_t fmt = (texFormat >> 8) & 0xFF;
    uint32_t w = 1 << ((texFormat >> 20) & 0xF);
    uint32_t h = 1 << ((texFormat >> 24) & 0xF);

    // If P8 paletted texture, verify palette population
    if (fmt == 0x0B) {
        uint32_t nonZeroPal = 0;
        if (pPalCpu) {
            const uint32_t* p32 = (const uint32_t*)pPalCpu;
            for (int i = 0; i < 256; i++) {
                if (p32[i] != 0) nonZeroPal++;
            }
        }
        // If current palette is completely blank, fallback to populated palette if available
        if (nonZeroPal == 0 && texPalette >= 0x400) {
            void* pAltCpu = XboxKrnl_GpuToCpu(texPalette - 0x400);
            if (pAltCpu) {
                const uint32_t* p32Alt = (const uint32_t*)pAltCpu;
                uint32_t altNonZero = 0;
                for (int i = 0; i < 256; i++) {
                    if (p32Alt[i] != 0) altNonZero++;
                }
                if (altNonZero > 0) {
                    pPalCpu = pAltCpu;
                    texPalette = texPalette - 0x400;
                    logFmt("[NV2A_TEX] Switched to populated palette 0x%08X (%u colors)\n", texPalette, altNonZero);
                }
            }
        }
    }

    for (uint32_t i = 0; i < s_texCacheCount; i++) {
        if (s_texCache[i].texGpu == texOffset &&
            s_texCache[i].palGpu == texPalette &&
            s_texCache[i].format == texFormat) {
            if (fmt == 0x24 && s_texCache[i].glTexId != 0) {
                // Dynamic YUY2 video frame: fall through to decode and glTexSubImage2D
                break;
            }
            return s_texCache[i].glTexId;
        }
    }

    logFmt("[GetOrCreateTexture] NEW: tex=0x%08X, fmt=0x%08X, pal=0x%08X -> pTex=%p, pPal=%p, %ux%u (fmt=0x%02X)\n",
           texOffset, texFormat, texPalette, pTexCpu, pPalCpu, w, h, fmt);

    if (!pTexCpu) return 0;

    if (w == 0 || h == 0 || w > 2048 || h > 2048) {
        return 0;
    }

    static uint32_t* s_rgbaBuf = NULL;
    if (!s_rgbaBuf) {
        s_rgbaBuf = (uint32_t*)malloc(2048 * 2048 * sizeof(uint32_t));
    }

    uint32_t nonZero = 0;

    uint32_t maskX, maskY;
    GenerateSwizzleMasks(w, h, &maskX, &maskY);
    static uint32_t s_swizX[2048];
    for (uint32_t x = 0; x < w; x++) {
        s_swizX[x] = FillSwizzlePattern(maskX, x);
    }

    if (fmt == 0x0B) { // P8 Swizzled (SZ_I8_A8R8G8B8)
        const uint8_t* tex8 = (const uint8_t*)pTexCpu;
        const uint32_t* pal32 = (const uint32_t*)pPalCpu;

        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t swiz = swizY | s_swizX[x];
                uint8_t idx = tex8[swiz];
                if (idx != 0) nonZero++;
                uint32_t color = pal32 ? pal32[idx] : 0;
                uint8_t a = (color >> 24) & 0xFF;
                uint8_t r = (color >> 16) & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = color & 0xFF;
                // If the color index is non-zero but palette alpha is 0, synthesize opaque/visible text
                if (idx != 0 && a == 0 && (r != 0 || g != 0 || b != 0)) {
                    a = 255;
                } else if (idx != 0 && color == 0) {
                    // Fallback white glyph if palette is unpopulated
                    r = 255; g = 255; b = 255; a = 255;
                }
                s_rgbaBuf[y * w + x] = (a << 24) | (b << 16) | (g << 8) | r;
            }
        }
    } else if (fmt == 0x06 || fmt == 0x07) { // A8R8G8B8 / X8R8G8B8 Swizzled
        const uint32_t* tex32 = (const uint32_t*)pTexCpu;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t swizY = FillSwizzlePattern(maskY, y);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t swiz = swizY | s_swizX[x];
                uint32_t color = tex32[swiz];
                if (color != 0) nonZero++;
                uint8_t a = (fmt == 0x07) ? 255 : ((color >> 24) & 0xFF);
                uint8_t r = (color >> 16) & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = color & 0xFF;
                s_rgbaBuf[y * w + x] = (a << 24) | (b << 16) | (g << 8) | r;
            }
        }
    } else if (fmt == 0x24) { // D3DFMT_YUY2 (Linear YUV 4:2:2 movie frames)
        const uint8_t* src = (const uint8_t*)pTexCpu;
        uint32_t totalPixels = w * h;
        for (uint32_t i = 0; i < totalPixels; i += 2) {
            uint8_t y0 = src[i * 2 + 0];
            uint8_t u  = src[i * 2 + 1];
            uint8_t y1 = src[i * 2 + 2];
            uint8_t v  = src[i * 2 + 3];
            if (y0 != 16 || u != 128 || y1 != 16 || v != 128) nonZero++;

            int c0 = (int)y0 - 16;
            int c1 = (int)y1 - 16;
            int d  = (int)u - 128;
            int e  = (int)v - 128;

            int r0 = (298 * c0 + 409 * e + 128) >> 8;
            int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
            int b0 = (298 * c0 + 516 * d + 128) >> 8;

            int r1 = (298 * c1 + 409 * e + 128) >> 8;
            int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c1 + 516 * d + 128) >> 8;

            uint8_t cr0 = (r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
            uint8_t cg0 = (g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
            uint8_t cb0 = (b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));

            uint8_t cr1 = (r1 < 0 ? 0 : (r1 > 255 ? 255 : r1));
            uint8_t cg1 = (g1 < 0 ? 0 : (g1 > 255 ? 255 : g1));
            uint8_t cb1 = (b1 < 0 ? 0 : (b1 > 255 ? 255 : b1));

            s_rgbaBuf[i]     = (255 << 24) | (cb0 << 16) | (cg0 << 8) | cr0;
            s_rgbaBuf[i + 1] = (255 << 24) | (cb1 << 16) | (cg1 << 8) | cr1;
        }
    } else if (fmt == 0x12) { // 8-bit Alpha / Luminance
        const uint8_t* src = (const uint8_t*)pTexCpu;
        for (uint32_t i = 0; i < w * h; i++) {
            uint8_t val = src[i];
            if (val != 0) nonZero++;
            s_rgbaBuf[i] = (val << 24) | (val << 16) | (val << 8) | val;
        }
    } else if (fmt == 0x04) { // R5G6B5 16-bit
        const uint16_t* src = (const uint16_t*)pTexCpu;
        for (uint32_t i = 0; i < w * h; i++) {
            uint16_t c = src[i];
            if (c != 0) nonZero++;
            uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
            uint8_t g = ((c >> 5) & 0x3F) * 255 / 63;
            uint8_t b = (c & 0x1F) * 255 / 31;
            s_rgbaBuf[i] = (255 << 24) | (b << 16) | (g << 8) | r;
        }
    } else if (fmt == 0x02 || fmt == 0x05) { // A1R5G5B5 16-bit
        const uint16_t* src = (const uint16_t*)pTexCpu;
        for (uint32_t i = 0; i < w * h; i++) {
            uint16_t c = src[i];
            if (c != 0) nonZero++;
            uint8_t a = (fmt == 0x05) ? 255 : ((c & 0x8000) ? 255 : 0);
            uint8_t r = ((c >> 10) & 0x1F) * 255 / 31;
            uint8_t g = ((c >> 5) & 0x1F) * 255 / 31;
            uint8_t b = (c & 0x1F) * 255 / 31;
            s_rgbaBuf[i] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    } else { // Linear Fallback
        const uint32_t* tex32 = (const uint32_t*)pTexCpu;
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t color = tex32[i];
            if (color != 0) nonZero++;
            uint8_t a = (color >> 24) & 0xFF;
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            s_rgbaBuf[i] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }

    if (nonZero == 0) {
        // Cache empty texture with texId = 0 so we don't re-decode every single call
        if (s_texCacheCount < MAX_TEX_CACHE) {
            s_texCache[s_texCacheCount].texGpu = texOffset;
            s_texCache[s_texCacheCount].palGpu = texPalette;
            s_texCache[s_texCacheCount].format = texFormat;
            s_texCache[s_texCacheCount].glTexId = 0;
            s_texCacheCount++;
        }
        return 0;
    }

    // If updating an already allocated dynamic texture (such as YUY2 video):
    if (fmt == 0x24) {
        for (uint32_t i = 0; i < s_texCacheCount; i++) {
            if (s_texCache[i].texGpu == texOffset && s_texCache[i].format == texFormat && s_texCache[i].glTexId != 0) {
                glBindTexture(GL_TEXTURE_2D, s_texCache[i].glTexId);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_rgbaBuf);
                return s_texCache[i].glTexId;
            }
        }
    }

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_rgbaBuf);

    if (s_texCacheCount < MAX_TEX_CACHE) {
        s_texCache[s_texCacheCount].texGpu = texOffset;
        s_texCache[s_texCacheCount].palGpu = texPalette;
        s_texCache[s_texCacheCount].format = texFormat;
        s_texCache[s_texCacheCount].glTexId = texId;
        s_texCacheCount++;
    }

    logFmt("[NV2A_TEX] Created OpenGL texture #%u: GPU tex=0x%08X, pal=0x%08X, %ux%u (fmt=0x%02X, nonZero=%u)\n",
           texId, texOffset, texPalette, w, h, fmt, nonZero);

    return texId;
}

static inline void TransposeMatrix(float dst[16], const float src[16]) {
    float tmp[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            tmp[c * 4 + r] = src[r * 4 + c];
        }
    }
    memcpy(dst, tmp, 16 * sizeof(float));
}

static void HandleSetBeginEnd(uint32_t op) {
    if (op == 0) {
        if (s_inBegin) {
            glEnd();
            s_inBegin = false;
        }
    } else {
        if (s_inBegin) {
            glEnd();
        }

        // Apply matrix BEFORE calling glBegin
        bool use3D = (s_is3DMode || s_vtxOffset[0] != 0) && (op != 8 /* GL_QUADS */);
        if (use3D) {
            glMatrixMode(GL_PROJECTION);
            glLoadMatrixf(s_projMatrix);
            glMatrixMode(GL_MODELVIEW);
            glLoadMatrixf(s_modelViewMatrix);
        } else {
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, 640.0, 480.0, 0.0, 0.1, -1.1);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
        }

        if (s_texEnabled && s_texOffset != 0) {
            GLuint texId = GetOrCreateTexture(s_texOffset, s_texFormat, s_texPalette);
            if (texId) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, texId);
            } else {
                glDisable(GL_TEXTURE_2D);
            }
        } else {
            glDisable(GL_TEXTURE_2D);
        }

        s_currentPrimitive = op;
        glBegin(ConvertNv2aPrimitive(op));
        s_inBegin = true;
        s_drawCount++;
    }
}

static void HandleInlineArray(const uint32_t* args, uint32_t count) {
    if (!s_inBegin || count == 0) return;

    if (s_is3DMode) {
        s_is3DMode = false;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, 640.0, 480.0, 0.0, 0.1, -1.1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }

    // Detect vertex stride:
    // When texturing is disabled, vertices NEVER contain UV coordinates.
    // Untextured primitives (Line2D, Box2D, Strip2DG, Circle2D) always use stride 4 (X, Y, Z, Color).
    // Only when texturing is enabled and bound do textured quads use stride 6 (X, Y, Z, Color, U, V).
    uint32_t stride = 4;
    if (s_texEnabled && s_texOffset != 0) {
        stride = (count % 6 == 0) ? 6 : 4;
    }
    uint32_t numVerts = count / stride;

    static bool s_loggedQuad = false;
    if (!s_loggedQuad && s_currentPrimitive == 8 && count >= 24) {
        s_loggedQuad = true;
        logFmt("[NV2A_QUAD] Logging first quad (prim=%u, count=%u, stride=%u):\n", s_currentPrimitive, count, stride);
        for (int i = 0; i < 4; i++) {
            const uint32_t* v = &args[i * stride];
            float vx = *(const float*)&v[0];
            float vy = *(const float*)&v[1];
            float vz = *(const float*)&v[2];
            uint32_t col = v[3];
            float vu = (stride >= 6) ? *(const float*)&v[4] : 0.0f;
            float vv = (stride >= 6) ? *(const float*)&v[5] : 0.0f;
            logFmt("  V%d: Pos=(%.2f, %.2f, %.2f) Col=0x%08X UV=(%.4f, %.4f)\n",
                   i, vx, vy, vz, col, vu, vv);
        }
    }

    for (uint32_t v = 0; v < numVerts; v++) {
        const uint32_t* vert = &args[v * stride];
        float x = *(const float*)&vert[0];
        float y = *(const float*)&vert[1];
        float z = (stride >= 3) ? *(const float*)&vert[2] : 0.0f;

        if (stride >= 4) {
            uint32_t color = vert[3];
            uint32_t a = (color >> 24) & 0xFF;
            uint32_t r = (color >> 16) & 0xFF;
            uint32_t g = (color >> 8) & 0xFF;
            uint32_t b = color & 0xFF;

            // Fixed-function 2X scale (Modulate2X: 0x80 = 1.0f full identity)
            r *= 2; if (r > 255) r = 255;
            g *= 2; if (g > 255) g = 255;
            b *= 2; if (b > 255) b = 255;
            // Modulate2X only scales RGB, preserving original alpha for transparency and smooth fades

            glColor4ub((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
        } else {
            glColor4ub(255, 255, 255, 255);
        }

        if (stride >= 6) {
            float u = *(const float*)&vert[4];
            float v_coord = *(const float*)&vert[5];
            glTexCoord2f(u, v_coord);
        }

        glVertex3f(x, y, z);
        s_vertexCount++;
    }
}

static void HandleArrayElement16(const uint32_t* args, uint32_t count) {
    if (!s_inBegin || count == 0) return;

    uint32_t posGpu = s_vtxOffset[0];
    if (!posGpu) return;
    const uint8_t* posBase = (const uint8_t*)XboxKrnl_GpuToCpu(posGpu);
    if (!posBase) return;
    uint32_t posStride = s_vtxStride[0];
    if (posStride == 0) posStride = 12;

    const uint8_t* uvBase = s_vtxOffset[9] ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[9]) : NULL;
    uint32_t uvStride = s_vtxStride[9];
    if (uvStride == 0) uvStride = 8;

    const uint8_t* normBase = s_vtxOffset[2] ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[2]) : NULL;
    uint32_t normStride = s_vtxStride[2];
    if (normStride == 0) normStride = 12;

    const uint8_t* colBase = s_vtxOffset[3] ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[3]) : NULL;
    uint32_t colStride = s_vtxStride[3];
    if (colStride == 0) colStride = 4;

    static int s_loggedElem16 = 0;
    if (s_loggedElem16 < 3) {
        s_loggedElem16++;
        logFmt("[NV2A_ELEM16 #%d] count=%u, prim=%u, is3D=%d, posBase=%p (gpu 0x%08X), stride=%u, fmt=0x%02X\n",
               s_loggedElem16, count, s_currentPrimitive, s_is3DMode, posBase, posGpu, posStride, s_vtxFormat[0]);
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t pair = args[i];
        uint16_t idx[2] = { (uint16_t)(pair & 0xFFFF), (uint16_t)((pair >> 16) & 0xFFFF) };
        for (int k = 0; k < 2; k++) {
            uint16_t vi = idx[k];
            if (vi == 0xFFFF) {
                // Primitive Restart: end current strip and restart a new strip
                if (s_currentPrimitive == 6 /* GL_TRIANGLE_STRIP */ || s_currentPrimitive == 9 /* GL_QUAD_STRIP */ || s_currentPrimitive == 4 /* GL_LINE_STRIP */) {
                    glEnd();
                    glBegin(ConvertNv2aPrimitive(s_currentPrimitive));
                }
                continue;
            }

            if (uvBase) {
                const float* uv = (const float*)(uvBase + vi * uvStride);
                glTexCoord2f(uv[0], uv[1]);
            }
            if (colBase) {
                uint32_t c = *(const uint32_t*)(colBase + vi * colStride);
                uint32_t a = (c >> 24) & 0xFF;
                uint32_t r = (c >> 16) & 0xFF;
                uint32_t g = (c >> 8) & 0xFF;
                uint32_t b = c & 0xFF;
                r *= 2; if (r > 255) r = 255;
                g *= 2; if (g > 255) g = 255;
                b *= 2; if (b > 255) b = 255;
                glColor4ub((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
            } else {
                glColor4ub(255, 255, 255, 255);
            }
            if (normBase) {
                const float* n = (const float*)(normBase + vi * normStride);
                glNormal3f(n[0], n[1], n[2]);
            }
            const float* p = (const float*)(posBase + vi * posStride);
            glVertex3f(p[0], p[1], p[2]);
            s_vertexCount++;
        }
    }
}

static void HandleArrayElementSingle(uint32_t arg) {
    if (!s_inBegin) return;

    uint16_t vi = (uint16_t)(arg & 0xFFFF);
    if (vi == 0xFFFF) {
        if (s_currentPrimitive == 6 /* GL_TRIANGLE_STRIP */ || s_currentPrimitive == 9 /* GL_QUAD_STRIP */ || s_currentPrimitive == 4 /* GL_LINE_STRIP */) {
            glEnd();
            glBegin(ConvertNv2aPrimitive(s_currentPrimitive));
        }
        return;
    }

    uint32_t posGpu = s_vtxOffset[0];
    if (!posGpu) return;
    const uint8_t* posBase = (const uint8_t*)XboxKrnl_GpuToCpu(posGpu);
    if (!posBase) return;
    uint32_t posStride = s_vtxStride[0];
    if (posStride == 0) posStride = 12;

    const uint8_t* uvBase = s_vtxOffset[9] ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[9]) : NULL;
    uint32_t uvStride = s_vtxStride[9];
    if (uvStride == 0) uvStride = 8;

    const uint8_t* normBase = s_vtxOffset[2] ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[2]) : NULL;
    uint32_t normStride = s_vtxStride[2];
    if (normStride == 0) normStride = 12;

    const uint8_t* colBase = s_vtxOffset[3] ? (const uint8_t*)XboxKrnl_GpuToCpu(s_vtxOffset[3]) : NULL;
    uint32_t colStride = s_vtxStride[3];
    if (colStride == 0) colStride = 4;

    if (uvBase) {
        const float* uv = (const float*)(uvBase + vi * uvStride);
        glTexCoord2f(uv[0], uv[1]);
    }
    if (colBase) {
        uint32_t c = *(const uint32_t*)(colBase + vi * colStride);
        uint32_t a = (c >> 24) & 0xFF;
        uint32_t r = (c >> 16) & 0xFF;
        uint32_t g = (c >> 8) & 0xFF;
        uint32_t b = c & 0xFF;
        r *= 2; if (r > 255) r = 255;
        g *= 2; if (g > 255) g = 255;
        b *= 2; if (b > 255) b = 255;
        glColor4ub((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
    } else {
        glColor4ub(255, 255, 255, 255);
    }
    if (normBase) {
        const float* n = (const float*)(normBase + vi * normStride);
        glNormal3f(n[0], n[1], n[2]);
    }
    const float* p = (const float*)(posBase + vi * posStride);
    glVertex3f(p[0], p[1], p[2]);
    s_vertexCount++;
}

void NV2A_ProcessPushBuffer(uint32_t* pDev) {
    if (!pDev || !s_glInitialized) return;

    s_totalFrames++;
    uint32_t currentPut = pDev[0];
    uint32_t pushBase = pDev[9];
    uint32_t pushEnd = pDev[10];

    if (s_lastPushPut == 0) {
        s_lastPushPut = pushBase;
    }

    if (currentPut == s_lastPushPut) {
        return;
    }

    s_vertexCount = 0;
    s_drawCount = 0;

    // Process from s_lastPushPut to currentPut (handling ring wrap)
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

        // Jump token: bits [0:1] == 1
        if ((token & 0x03) == 1) {
            uint32_t target = token & ~0x03;
            // D3D strips the high nibble (writing a physical offset 0x00xxxxxx).
            // Re-apply the base address prefix if below pushBase.
            if (target < pushBase) {
                target = (target & 0x0FFFFFFF) | (pushBase & 0xF0000000);
            }
            if (target < pushBase || target >= pushEnd) {
                target = pushBase;
            }
            cur = (uint32_t*)target;
            continue;
        }

        // Method Header
        uint32_t method = token & 0x1FFC;
        uint32_t count = (token >> 18) & 0x7FF;
        bool non_inc = ((token & 0x40000000) != 0);

        if (count == 0) {
            continue;
        }

        // Arguments pointer
        const uint32_t* args = cur;
        cur += count;

        s_lastMethod = method;

        // End any open glBegin block if changing state, textures, or clearing
        if (s_inBegin && method != 0x1818 && method != 0x1880 && method != 0x18C8 &&
            method != 0x17FC && method != 0x1800 && method != 0x1808) {
            static int s_loggedBreak = 0;
            if (s_loggedBreak < 30) {
                s_loggedBreak++;
                logFmt("[NV2A_BEGIN_BREAK #%d] Prim=%u broken by M:0x%04X, count=%u, arg[0]=0x%08X\n",
                       s_loggedBreak, s_currentPrimitive, method, count, count ? args[0] : 0);
            }
            glEnd();
            s_inBegin = false;
        }

        // Dispatch NV2A methods
        switch (method) {
            case 0x0300: // NV097_SET_ALPHA_TEST_ENABLE
                if (count >= 1) {
                    if (args[0]) glEnable(GL_ALPHA_TEST);
                    else glDisable(GL_ALPHA_TEST);
                }
                break;

            case 0x0304: // NV097_SET_BLEND_ENABLE
                if (count >= 1) {
                    if (args[0]) glEnable(GL_BLEND);
                    else glDisable(GL_BLEND);
                }
                break;

            case 0x0308: // NV097_SET_CULL_FACE_ENABLE
                if (count >= 1) {
                    if (args[0]) glEnable(GL_CULL_FACE);
                    else glDisable(GL_CULL_FACE);
                }
                break;

            case 0x030C: // NV097_SET_DEPTH_TEST_ENABLE
                if (count >= 1) {
                    if (args[0]) glEnable(GL_DEPTH_TEST);
                    else glDisable(GL_DEPTH_TEST);
                }
                break;

            case 0x033C: // NV097_SET_ALPHA_FUNC
                if (count >= 1) {
                    s_alphaFunc = ConvertNv2aCompareFunc(args[0]);
                    glAlphaFunc(s_alphaFunc, s_alphaRef);
                }
                break;

            case 0x0340: // NV097_SET_ALPHA_REF
                if (count >= 1) {
                    s_alphaRef = (float)(args[0] & 0xFF) / 255.0f;
                    glAlphaFunc(s_alphaFunc, s_alphaRef);
                }
                break;

            case 0x0344: // NV097_SET_BLEND_FUNC_SFACTOR
                if (count >= 1) {
                    s_blendSrc = ConvertNv2aBlendFactor(args[0]);
                    glBlendFunc(s_blendSrc, s_blendDst);
                }
                break;

            case 0x0348: // NV097_SET_BLEND_FUNC_DFACTOR
                if (count >= 1) {
                    s_blendDst = ConvertNv2aBlendFactor(args[0]);
                    glBlendFunc(s_blendSrc, s_blendDst);
                }
                break;

            case 0x0354: // NV097_SET_DEPTH_FUNC
                if (count >= 1) {
                    glDepthFunc(ConvertNv2aCompareFunc(args[0]));
                }
                break;

            case 0x0358: // NV097_SET_COLOR_MASK
                if (count >= 1) {
                    uint32_t m = args[0];
                    glColorMask((m & 1) ? GL_TRUE : GL_FALSE,
                                ((m >> 8) & 1) ? GL_TRUE : GL_FALSE,
                                ((m >> 16) & 1) ? GL_TRUE : GL_FALSE,
                                ((m >> 24) & 1) ? GL_TRUE : GL_FALSE);
                }
                break;

            case 0x035C: // NV097_SET_DEPTH_MASK
                if (count >= 1) {
                    glDepthMask((args[0] & 1) ? GL_TRUE : GL_FALSE);
                }
                break;

            case 0x039C: // NV097_SET_CULL_FACE
                if (count >= 1) {
                    if (args[0] == 0x0404) glCullFace(GL_FRONT);
                    else if (args[0] == 0x0405) glCullFace(GL_BACK);
                    else if (args[0] == 0x0408) glCullFace(GL_FRONT_AND_BACK);
                }
                break;

            case 0x0680: // NV097_SET_MODELVIEW_MATRIX
                if (count >= 16) {
                    TransposeMatrix(s_modelViewMatrix, (const float*)args);
                    if (s_is3DMode) {
                        glMatrixMode(GL_MODELVIEW);
                        glLoadMatrixf(s_modelViewMatrix);
                    }
                }
                break;

            case 0x0700: // NV097_SET_PROJECTION_MATRIX
                if (count >= 16) {
                    TransposeMatrix(s_projMatrix, (const float*)args);
                    if (s_is3DMode) {
                        glMatrixMode(GL_PROJECTION);
                        glLoadMatrixf(s_projMatrix);
                    }
                }
                break;

            case 0x1720: case 0x1724: case 0x1728: case 0x172C:
            case 0x1730: case 0x1734: case 0x1738: case 0x173C:
            case 0x1740: case 0x1744: case 0x1748: case 0x174C:
            case 0x1750: case 0x1754: case 0x1758: case 0x175C: {
                uint32_t slot = (method - 0x1720) / 4;
                if (count >= 1 && slot < 16) {
                    s_vtxOffset[slot] = args[0];
                }
                break;
            }

            case 0x1760: case 0x1764: case 0x1768: case 0x176C:
            case 0x1770: case 0x1774: case 0x1778: case 0x177C:
            case 0x1780: case 0x1784: case 0x1788: case 0x178C:
            case 0x1790: case 0x1794: case 0x1798: case 0x179C: {
                uint32_t slot = (method - 0x1760) / 4;
                for (uint32_t i = 0; i < count && (slot + i) < 16; i++) {
                    s_vtxStride[slot + i] = (args[i] >> 8) & 0xFF;
                    s_vtxFormat[slot + i] = args[i] & 0xFF;
                }
                break;
            }

            case 0x1800: // NV097_ARRAY_ELEMENT16
                s_count_1800++;
                HandleArrayElement16(args, count);
                break;

            case 0x1808: // NV097_ARRAY_ELEMENT32 (single element)
                s_count_1808++;
                if (count >= 1) {
                    HandleArrayElementSingle(args[0]);
                }
                break;

            case 0x1B00: // NV097_SET_TEXTURE_OFFSET
                if (count >= 1) s_texOffset = args[0];
                if (count >= 2) s_texFormat = args[1];
                break;

            case 0x1B04: // NV097_SET_TEXTURE_FORMAT
                if (count >= 1) s_texFormat = args[0];
                break;

            case 0x1B0C: // NV097_SET_TEXTURE_CONTROL0
                if (count >= 1) {
                    s_texControl0 = args[0];
                    s_texEnabled = (args[0] != 0);
                }
                break;

            case 0x1B20: // NV097_SET_TEXTURE_PALETTE
                if (count >= 1) {
                    uint32_t newPal = args[0];
                    void* pCpu = XboxKrnl_GpuToCpu(newPal);
                    uint32_t nonZeroColors = 0;
                    if (pCpu) {
                        const uint32_t* pal = (const uint32_t*)pCpu;
                        for (int c = 0; c < 256; c++) {
                            if (pal[c] != 0) nonZeroColors++;
                        }
                    }
                    if (nonZeroColors > 0 || s_texPalette == 0) {
                        s_texPalette = newPal;
                    }
                }
                break;

            case 0x17FC: // NV097_SET_BEGIN_END
                s_count_17fc++;
                for (uint32_t i = 0; i < count; i++) {
                    HandleSetBeginEnd(args[i]);
                }
                break;

            case 0x1818: // NV097_INLINE_ARRAY
                s_count_1818++;
                HandleInlineArray(args, count);
                break;

            case 0x18C8: // NV097_SET_VERTEX_DATA2F_M(9) (TexCoord UV)
                if (count >= 2) {
                    float u = *(const float*)&args[0];
                    float v = *(const float*)&args[1];
                    if (u > 1.0f) u /= 2560.0f;
                    if (v > 1.0f) v /= 1920.0f;
                    glTexCoord2f(u, v);
                }
                break;

            case 0x1880: // NV097_SET_VERTEX_DATA2F_M(0) (Position 2F -> Emits vertex)
                if (count >= 2) {
                    float x = *(const float*)&args[0];
                    float y = *(const float*)&args[1];
                    if (x > 640.0f || y > 480.0f) {
                        x *= 0.25f;
                        y *= 0.25f;
                    }
                    glColor4ub(255, 255, 255, 255);
                    glVertex3f(x, y, 0.0f);
                    s_vertexCount++;
                }
                break;

            case 0x1D8C: // NV097_SET_ZSTENCIL_CLEAR_VALUE (and multi-packet clear)
                if (count >= 1) {
                    uint32_t zVal = args[0];
                    glClearDepth((double)zVal / 4294967295.0);
                }
                if (count >= 2) {
                    s_clearColor = args[1];
                    float a = ((s_clearColor >> 24) & 0xFF) / 255.0f;
                    float r = ((s_clearColor >> 16) & 0xFF) / 255.0f;
                    float g = ((s_clearColor >> 8) & 0xFF) / 255.0f;
                    float b = (s_clearColor & 0xFF) / 255.0f;
                    glClearColor(r, g, b, a);
                }
                if (count >= 3) {
                    s_count_1d98++;
                    uint32_t rawMask = args[2];
                    GLbitfield mask = 0;
                    if (rawMask & 0x01) mask |= GL_DEPTH_BUFFER_BIT;
                    if (rawMask & 0x02) mask |= GL_STENCIL_BUFFER_BIT;
                    if (rawMask & 0xF0) mask |= GL_COLOR_BUFFER_BIT;
                    if (mask) glClear(mask);
                }
                break;

            case 0x1D90: // NV097_SET_COLOR_CLEAR_VALUE
                if (count >= 1) {
                    s_clearColor = args[0];
                    float a = ((s_clearColor >> 24) & 0xFF) / 255.0f;
                    float r = ((s_clearColor >> 16) & 0xFF) / 255.0f;
                    float g = ((s_clearColor >> 8) & 0xFF) / 255.0f;
                    float b = (s_clearColor & 0xFF) / 255.0f;
                    glClearColor(r, g, b, a);
                }
                break;

            case 0x1D94: // NV097_CLEAR_SURFACE
                s_count_1d98++;
                if (count >= 1) {
                    uint32_t rawMask = args[0];
                    GLbitfield mask = 0;
                    if (rawMask & 0x01) mask |= GL_DEPTH_BUFFER_BIT;
                    if (rawMask & 0x02) mask |= GL_STENCIL_BUFFER_BIT;
                    if (rawMask & 0xF0) mask |= GL_COLOR_BUFFER_BIT;
                    if (!mask) mask = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
                    glClear(mask);
                }
                break;

            case 0x1D98: // NV097_SET_CLEAR_RECT_HORIZONTAL
                break;

            case 0x1E94: // NV097_SET_TRANSFORM_EXECUTION_MODE
                if (count >= 1) {
                    s_is3DMode = (args[0] == 6);
                }
                break;

            default:
                s_count_other++;
                if (method < 0x2000) s_unhandledMethods[method]++;
                break;
        }
    }

    if (s_inBegin) {
        glEnd();
        s_inBegin = false;
    }

    s_lastPushPut = currentPut;

    if (s_totalFrames == 1 || s_totalFrames % 60 == 0) {
        logFmt("[NV2A_GL] Frame %u: draws=%u, verts=%u | (total SET_BEGIN=%u, INLINE=%u, IDX16=%u, CLEAR=%u, OTHER=%u)\n",
               s_totalFrames, s_drawCount, s_vertexCount, s_count_17fc, s_count_1818, s_count_1800, s_count_1d98, s_count_other);
        char unhMsg[256] = "[NV2A_UNH] Top unhandled:";
        for (int t = 0; t < 6; t++) {
            uint32_t maxCount = 0, maxMeth = 0;
            for (uint32_t m = 0; m < 0x2000; m++) {
                if (s_unhandledMethods[m] > maxCount) {
                    maxCount = s_unhandledMethods[m];
                    maxMeth = m;
                }
            }
            if (maxCount > 0) {
                char part[40];
                snprintf(part, sizeof(part), " 0x%04X(%u)", maxMeth, maxCount);
                strncat(unhMsg, part, sizeof(unhMsg) - strlen(unhMsg) - 1);
                s_unhandledMethods[maxMeth] = 0;
            }
        }
        logFmt("%s\n", unhMsg);
    }
}

static void SaveScreenshotBMP(const char* filename, int w, int h) {
    uint8_t* pixels = (uint8_t*)malloc(w * h * 3);
    if (!pixels) return;
    glReadPixels(0, 0, w, h, 0x80E0 /*GL_BGR*/, GL_UNSIGNED_BYTE, pixels);

    BITMAPFILEHEADER bfh = {0};
    BITMAPINFOHEADER bih = {0};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + w * h * 3;

    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    FILE* f = fopen(filename, "wb");
    if (f) {
        fwrite(&bfh, sizeof(bfh), 1, f);
        fwrite(&bih, sizeof(bih), 1, f);
        fwrite(pixels, w * h * 3, 1, f);
        fclose(f);
        logFmt("[NV2A_GL] Screenshot saved to %s (%dx%d)\n", filename, w, h);
    }
    free(pixels);
}

void NV2A_SwapBuffers(HDC hDC) {
    if (s_glInitialized && hDC) {
        SwapBuffers(hDC);
    }
}
