#include "audio_bridge.h"
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void logFmt(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("%s", buf);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Xbox ADPCM Decoder (IMA ADPCM variant)
// ---------------------------------------------------------------------------
static const int s_imaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int s_imaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static void DecodeXboxAdpcmBlock(const uint8_t* src, int16_t* dst, int stride) {
    int sample = (int16_t)(src[0] | (src[1] << 8));
    int stepIndex = (int8_t)src[2];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;

    *dst = (int16_t)sample;
    dst += stride;

    for (int i = 0; i < 32; i++) {
        uint8_t b = src[4 + i];
        for (int nib = 0; nib < 2; nib++) {
            uint8_t code = (nib == 0) ? (b & 0x0F) : (b >> 4);
            int step = s_imaStepTable[stepIndex];
            stepIndex += s_imaIndexTable[code];
            if (stepIndex < 0) stepIndex = 0;
            if (stepIndex > 88) stepIndex = 88;

            int diff = step >> 3;
            if (code & 4) diff += step;
            if (code & 2) diff += (step >> 1);
            if (code & 1) diff += (step >> 2);
            if (code & 8) sample -= diff;
            else sample += diff;

            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            *dst = (int16_t)sample;
            dst += stride;
        }
    }
}

// ---------------------------------------------------------------------------
// Emulated Sound Buffer
// ---------------------------------------------------------------------------
#define SOUND_BUFFER_MAGIC 0x534E4442 // 'SNDB'
#define MAX_SOUND_BUFFERS  256

typedef struct EmulatedSoundBuffer {
    uint32_t magic;
    WAVEFORMATEX format;
    uint8_t* pRawData;
    uint32_t rawBytes;
    bool ownsRawData;

    int16_t* pPcmSamples;
    uint32_t totalSamples; // sample frames (per channel)
    uint32_t channels;
    uint32_t sampleRate;
    bool ownsPcmSamples;

    float playCursor;      // in sample frames
    uint32_t status;       // DSBSTATUS_PLAYING (1)
    bool isLooping;
    bool isStreaming;
    int32_t volume;        // millibels (-10000..0)
    float linearVol;
    uint32_t frequency;    // current playback frequency in Hz

    uint32_t loopStartSample;
    uint32_t loopLengthSamples;

    struct EmulatedSoundBuffer* next;
} EmulatedSoundBuffer;

static CRITICAL_SECTION s_audioLock;
static EmulatedSoundBuffer* s_activeBuffers = NULL;

static float MillibelsToLinear(int32_t mb) {
    if (mb <= -10000) return 0.0f;
    if (mb >= 0) return 1.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

// ---------------------------------------------------------------------------
// Win32 waveOut Mixer Engine
// ---------------------------------------------------------------------------
#define MIX_BUFFER_FRAMES 1024
#define MIX_BUFFER_COUNT  4

static HWAVEOUT s_hWaveOut = NULL;
static WAVEHDR s_waveHeaders[MIX_BUFFER_COUNT];
static int16_t s_mixBuffers[MIX_BUFFER_COUNT][MIX_BUFFER_FRAMES * 2]; // stereo 16-bit
static HANDLE s_hMixThread = NULL;
static HANDLE s_hMixEvent = NULL;
static volatile bool s_bAudioRunning = false;

static void CALLBACK WaveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    (void)hwo; (void)dwInstance; (void)dwParam1; (void)dwParam2;
    if (uMsg == WOM_DONE && s_hMixEvent) {
        SetEvent(s_hMixEvent);
    }
}

static DWORD WINAPI AudioMixerThread(LPVOID lpParam) {
    (void)lpParam;
    logFmt("[AUDIO] WaveOut mixer thread started.\n");

    int32_t accumL[MIX_BUFFER_FRAMES];
    int32_t accumR[MIX_BUFFER_FRAMES];

    while (s_bAudioRunning) {
        for (int b = 0; b < MIX_BUFFER_COUNT && s_bAudioRunning; b++) {
            WAVEHDR* hdr = &s_waveHeaders[b];
            if ((hdr->dwFlags & WHDR_DONE) || !(hdr->dwFlags & WHDR_PREPARED)) {
                if (hdr->dwFlags & WHDR_PREPARED) {
                    waveOutUnprepareHeader(s_hWaveOut, hdr, sizeof(WAVEHDR));
                }

                memset(accumL, 0, sizeof(accumL));
                memset(accumR, 0, sizeof(accumR));

                EnterCriticalSection(&s_audioLock);
                EmulatedSoundBuffer* buf = s_activeBuffers;
                while (buf) {
                    if ((buf->status & 1) != 0) {
                        float freqRatio = (float)buf->frequency / 48000.0f;
                        float vol = buf->linearVol;

                        if (buf->isStreaming && buf->pRawData) {
                            // Circular streaming buffer (CRI ADX): read directly from 16-bit PCM byte buffer
                            const int16_t* pcm = (const int16_t*)buf->pRawData;
                            uint32_t totalFrames = buf->rawBytes / (buf->channels * 2);
                            if (totalFrames > 0) {
                                for (int i = 0; i < MIX_BUFFER_FRAMES; i++) {
                                    uint32_t frameIdx = (uint32_t)buf->playCursor;
                                    if (frameIdx >= totalFrames) frameIdx %= totalFrames;

                                    int16_t sL = 0, sR = 0;
                                    if (buf->channels == 1) {
                                        sL = sR = pcm[frameIdx];
                                    } else {
                                        sL = pcm[frameIdx * 2];
                                        sR = pcm[frameIdx * 2 + 1];
                                    }

                                    accumL[i] += (int32_t)(sL * vol);
                                    accumR[i] += (int32_t)(sR * vol);

                                    buf->playCursor += freqRatio;
                                    if (buf->playCursor >= (float)totalFrames) {
                                        buf->playCursor -= (float)totalFrames;
                                    }
                                }
                            }
                        } else if (buf->pPcmSamples && buf->totalSamples > 0) {
                            // Pre-decoded / one-shot SFX sample
                            for (int i = 0; i < MIX_BUFFER_FRAMES; i++) {
                                uint32_t idx = (uint32_t)buf->playCursor;
                                if (idx >= buf->totalSamples) {
                                    if (buf->isLooping && buf->loopLengthSamples > 0) {
                                        buf->playCursor = (float)buf->loopStartSample;
                                        idx = buf->loopStartSample;
                                    } else {
                                        buf->status = 0; // stopped
                                        buf->playCursor = 0.0f;
                                        break;
                                    }
                                }

                                int16_t sL = 0, sR = 0;
                                if (buf->channels == 1) {
                                    sL = sR = buf->pPcmSamples[idx];
                                } else {
                                    sL = buf->pPcmSamples[idx * 2];
                                    sR = buf->pPcmSamples[idx * 2 + 1];
                                }

                                accumL[i] += (int32_t)(sL * vol);
                                accumR[i] += (int32_t)(sR * vol);

                                buf->playCursor += freqRatio;
                            }
                        }
                    }
                    buf = buf->next;
                }
                LeaveCriticalSection(&s_audioLock);

                // Convert 32-bit mixed accumulator to 16-bit clamped stereo
                int16_t* outSamples = s_mixBuffers[b];
                for (int i = 0; i < MIX_BUFFER_FRAMES; i++) {
                    int32_t l = accumL[i];
                    int32_t r = accumR[i];
                    if (l > 32767) l = 32767;
                    if (l < -32768) l = -32768;
                    if (r > 32767) r = 32767;
                    if (r < -32768) r = -32768;
                    outSamples[i * 2 + 0] = (int16_t)l;
                    outSamples[i * 2 + 1] = (int16_t)r;
                }

                memset(hdr, 0, sizeof(WAVEHDR));
                hdr->lpData = (LPSTR)outSamples;
                hdr->dwBufferLength = MIX_BUFFER_FRAMES * 2 * sizeof(int16_t);
                waveOutPrepareHeader(s_hWaveOut, hdr, sizeof(WAVEHDR));
                waveOutWrite(s_hWaveOut, hdr, sizeof(WAVEHDR));
            }
        }
        WaitForSingleObject(s_hMixEvent, 10);
    }

    logFmt("[AUDIO] WaveOut mixer thread terminated.\n");
    return 0;
}

bool Audio_Init(void) {
    if (s_hWaveOut) return true;

    InitializeCriticalSection(&s_audioLock);

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = 48000 * 4;
    wfx.cbSize = 0;

    s_hMixEvent = CreateEventA(NULL, FALSE, FALSE, NULL);

    MMRESULT res = waveOutOpen(&s_hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)WaveOutProc, 0, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR || !s_hWaveOut) {
        logFmt("[AUDIO] Warning: waveOutOpen failed (%u), audio will be disabled.\n", res);
        return false;
    }

    logFmt("[AUDIO] Win32 waveOut device opened successfully (48000Hz, 16-bit Stereo).\n");

    s_bAudioRunning = true;
    s_hMixThread = CreateThread(NULL, 0, AudioMixerThread, NULL, 0, NULL);
    if (s_hMixThread) {
        SetThreadPriority(s_hMixThread, THREAD_PRIORITY_TIME_CRITICAL);
    }

    return true;
}

void Audio_Shutdown(void) {
    s_bAudioRunning = false;
    if (s_hMixEvent) SetEvent(s_hMixEvent);
    if (s_hMixThread) {
        WaitForSingleObject(s_hMixThread, 500);
        CloseHandle(s_hMixThread);
        s_hMixThread = NULL;
    }
    if (s_hWaveOut) {
        waveOutReset(s_hWaveOut);
        for (int i = 0; i < MIX_BUFFER_COUNT; i++) {
            if (s_waveHeaders[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(s_hWaveOut, &s_waveHeaders[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(s_hWaveOut);
        s_hWaveOut = NULL;
    }
    if (s_hMixEvent) {
        CloseHandle(s_hMixEvent);
        s_hMixEvent = NULL;
    }
    DeleteCriticalSection(&s_audioLock);
}

// ---------------------------------------------------------------------------
// DirectSound Hook Implementations
// ---------------------------------------------------------------------------
typedef struct _DSBUFFERDESC_XBOX {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    LPVOID lpwfxFormat;
} DSBUFFERDESC_XBOX;

static HRESULT __stdcall Hook_DirectSoundCreate(void* pcGuidDevice, void** ppDS, void* pUnkOuter) {
    (void)pcGuidDevice; (void)pUnkOuter;
    if (!ppDS) return 0x80070057; // E_INVALIDARG
    *ppDS = (void*)0x12340001; // dummy interface handle
    logFmt("[AUDIO] DirectSoundCreate intercepted -> returns dummy device @ %p\n", *ppDS);
    return 0; // DS_OK
}

static ULONG __stdcall Hook_DirectSound_Release(void* pDS) {
    (void)pDS;
    return 0;
}

static HRESULT __stdcall Hook_DirectSound_CreateSoundBuffer(void* pDS, const DSBUFFERDESC_XBOX* pDesc, EmulatedSoundBuffer** ppBuffer, void* pUnkOuter) {
    (void)pDS; (void)pUnkOuter;
    if (!pDesc || !ppBuffer) return 0x80070057;

    EmulatedSoundBuffer* buf = (EmulatedSoundBuffer*)calloc(1, sizeof(EmulatedSoundBuffer));
    if (!buf) return 0x8007000E; // E_OUTOFMEMORY

    buf->magic = SOUND_BUFFER_MAGIC;
    buf->volume = 0;
    buf->linearVol = 1.0f;
    buf->frequency = 44100;

    if (pDesc->lpwfxFormat) {
        const WAVEFORMATEX* wfx = (const WAVEFORMATEX*)pDesc->lpwfxFormat;
        memcpy(&buf->format, wfx, sizeof(WAVEFORMATEX));
        buf->channels = wfx->nChannels ? wfx->nChannels : 2;
        buf->sampleRate = wfx->nSamplesPerSec ? wfx->nSamplesPerSec : 44100;
        buf->frequency = buf->sampleRate;
    } else {
        buf->channels = 2;
        buf->sampleRate = 44100;
        buf->format.wFormatTag = WAVE_FORMAT_PCM;
        buf->format.nChannels = 2;
        buf->format.nSamplesPerSec = 44100;
        buf->format.wBitsPerSample = 16;
        buf->format.nBlockAlign = 4;
    }

    if (pDesc->dwBufferBytes > 0) {
        buf->rawBytes = pDesc->dwBufferBytes;
        buf->pRawData = (uint8_t*)malloc(pDesc->dwBufferBytes);
        buf->ownsRawData = true;
        buf->isStreaming = true; // CRI ADX circular streaming buffer
        if (buf->pRawData) {
            memset(buf->pRawData, 0, pDesc->dwBufferBytes);
        }
    }

    EnterCriticalSection(&s_audioLock);
    buf->next = s_activeBuffers;
    s_activeBuffers = buf;
    LeaveCriticalSection(&s_audioLock);

    static int s_createLog = 0;
    if (s_createLog++ < 20) {
        logFmt("[AUDIO] CreateSoundBuffer #%d: buf=%p fmt=0x%04X ch=%u rate=%u bytes=%u stream=%d\n",
               s_createLog, buf, buf->format.wFormatTag, buf->channels, buf->sampleRate, pDesc->dwBufferBytes, buf->isStreaming ? 1 : 0);
    }

    *ppBuffer = buf;
    return 0; // DS_OK
}

static ULONG __stdcall Hook_DirectSoundBuffer_Release(EmulatedSoundBuffer* pBuffer) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0;

    EnterCriticalSection(&s_audioLock);
    if (s_activeBuffers == pBuffer) {
        s_activeBuffers = pBuffer->next;
    } else if (s_activeBuffers) {
        EmulatedSoundBuffer* cur = s_activeBuffers;
        while (cur->next && cur->next != pBuffer) cur = cur->next;
        if (cur->next == pBuffer) cur->next = pBuffer->next;
    }
    LeaveCriticalSection(&s_audioLock);

    if (pBuffer->ownsRawData && pBuffer->pRawData) {
        free(pBuffer->pRawData);
        pBuffer->pRawData = NULL;
    }
    if (pBuffer->ownsPcmSamples && pBuffer->pPcmSamples) {
        free(pBuffer->pPcmSamples);
        pBuffer->pPcmSamples = NULL;
    }
    free(pBuffer);
    return 0;
}

static HRESULT __stdcall Hook_DirectSoundBuffer_SetBufferData(EmulatedSoundBuffer* pBuffer, void* pData, DWORD dwSize) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC || !pData || dwSize == 0) return 0x80070057;

    EnterCriticalSection(&s_audioLock);
    pBuffer->pRawData = (uint8_t*)pData;
    pBuffer->rawBytes = dwSize;
    pBuffer->ownsRawData = false;
    pBuffer->isStreaming = false;

    if (pBuffer->ownsPcmSamples && pBuffer->pPcmSamples) {
        free(pBuffer->pPcmSamples);
        pBuffer->pPcmSamples = NULL;
        pBuffer->ownsPcmSamples = false;
        pBuffer->totalSamples = 0;
    }

    if (pBuffer->format.wFormatTag == 0x0069) {
        // Xbox ADPCM: Decode into 16-bit PCM buffer
        uint32_t blockAlign = pBuffer->format.nBlockAlign ? pBuffer->format.nBlockAlign : (36 * pBuffer->channels);
        uint32_t numBlocks = dwSize / blockAlign;
        uint32_t samplesPerBlock = 65;
        uint32_t totalSamples = numBlocks * samplesPerBlock;

        pBuffer->pPcmSamples = (int16_t*)malloc(totalSamples * pBuffer->channels * sizeof(int16_t));
        if (pBuffer->pPcmSamples) {
            pBuffer->ownsPcmSamples = true;
            pBuffer->totalSamples = totalSamples;
            const uint8_t* src = (const uint8_t*)pData;
            int16_t* dst = pBuffer->pPcmSamples;

            for (uint32_t b = 0; b < numBlocks; b++) {
                if (pBuffer->channels == 1) {
                    DecodeXboxAdpcmBlock(src + b * 36, dst + b * 65, 1);
                } else {
                    DecodeXboxAdpcmBlock(src + b * 72, dst + b * 65 * 2, 2);
                    DecodeXboxAdpcmBlock(src + b * 72 + 36, dst + b * 65 * 2 + 1, 2);
                }
            }
        }
    } else {
        // Raw 16-bit PCM
        pBuffer->ownsPcmSamples = false;
        pBuffer->pPcmSamples = (int16_t*)pData;
        pBuffer->totalSamples = dwSize / (pBuffer->channels * 2);
    }

    pBuffer->loopStartSample = 0;
    pBuffer->loopLengthSamples = pBuffer->totalSamples;
    LeaveCriticalSection(&s_audioLock);

    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_Lock(
    EmulatedSoundBuffer* pBuffer, DWORD dwOffset, DWORD dwBytes,
    void** ppvAudioPtr1, DWORD* pdwAudioBytes1,
    void** ppvAudioPtr2, DWORD* pdwAudioBytes2, DWORD dwFlags)
{
    (void)dwFlags;
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC || !ppvAudioPtr1 || !pdwAudioBytes1) return 0x80070057;

    if (!pBuffer->pRawData || pBuffer->rawBytes == 0) return 0x80004005;

    if (dwOffset >= pBuffer->rawBytes) dwOffset = 0;

    if (dwOffset + dwBytes <= pBuffer->rawBytes) {
        *ppvAudioPtr1 = pBuffer->pRawData + dwOffset;
        *pdwAudioBytes1 = dwBytes;
        if (ppvAudioPtr2) *ppvAudioPtr2 = NULL;
        if (pdwAudioBytes2) *pdwAudioBytes2 = 0;
    } else {
        *ppvAudioPtr1 = pBuffer->pRawData + dwOffset;
        *pdwAudioBytes1 = pBuffer->rawBytes - dwOffset;
        if (ppvAudioPtr2 && pdwAudioBytes2) {
            *ppvAudioPtr2 = pBuffer->pRawData;
            *pdwAudioBytes2 = dwBytes - *pdwAudioBytes1;
        }
    }

    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_Unlock(
    EmulatedSoundBuffer* pBuffer,
    void* pvAudioPtr1, DWORD dwAudioBytes1,
    void* pvAudioPtr2, DWORD dwAudioBytes2)
{
    (void)pBuffer; (void)pvAudioPtr1; (void)dwAudioBytes1; (void)pvAudioPtr2; (void)dwAudioBytes2;
    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_Play(EmulatedSoundBuffer* pBuffer, DWORD r1, DWORD r2, DWORD dwFlags) {
    (void)r1; (void)r2;
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0x80070057;

    EnterCriticalSection(&s_audioLock);
    pBuffer->isLooping = (dwFlags & 1) != 0;
    if (!pBuffer->isStreaming && (dwFlags & 2) != 0) {
        pBuffer->playCursor = 0.0f; // DSBPLAY_FROMSTART
    }
    pBuffer->status = 1; // DSBSTATUS_PLAYING
    LeaveCriticalSection(&s_audioLock);

    static int s_playLog = 0;
    if (s_playLog++ < 25) {
        logFmt("[AUDIO] Play #%d: buf=%p stream=%d loop=%d freq=%u bytes=%u vol=%d(%.2f) flags=0x%X\n",
               s_playLog, pBuffer, pBuffer->isStreaming ? 1 : 0, pBuffer->isLooping ? 1 : 0,
               pBuffer->frequency, pBuffer->rawBytes, pBuffer->volume, pBuffer->linearVol, dwFlags);
    }

    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_Stop(EmulatedSoundBuffer* pBuffer) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0x80070057;

    EnterCriticalSection(&s_audioLock);
    pBuffer->status = 0;
    LeaveCriticalSection(&s_audioLock);

    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_StopEx(EmulatedSoundBuffer* pBuffer, DWORD r1, DWORD r2, DWORD dwFlags) {
    (void)r1; (void)r2; (void)dwFlags;
    return Hook_DirectSoundBuffer_Stop(pBuffer);
}

static HRESULT __stdcall Hook_DirectSoundBuffer_GetStatus(EmulatedSoundBuffer* pBuffer, DWORD* pdwStatus) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC || !pdwStatus) return 0x80070057;
    *pdwStatus = pBuffer->status;
    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_GetCurrentPosition(EmulatedSoundBuffer* pBuffer, DWORD* pdwPlay, DWORD* pdwWrite) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0x80070057;

    EnterCriticalSection(&s_audioLock);
    uint32_t bytePos = (uint32_t)pBuffer->playCursor * (pBuffer->channels * 2);
    if (pBuffer->rawBytes > 0) bytePos %= pBuffer->rawBytes;
    LeaveCriticalSection(&s_audioLock);

    if (pdwPlay) *pdwPlay = bytePos;
    if (pdwWrite) {
        uint32_t writePos = bytePos + 2048;
        if (pBuffer->rawBytes > 0) writePos %= pBuffer->rawBytes;
        *pdwWrite = writePos;
    }
    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_SetCurrentPosition(EmulatedSoundBuffer* pBuffer, DWORD dwNewPosition) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0x80070057;

    EnterCriticalSection(&s_audioLock);
    uint32_t frameSize = pBuffer->channels * 2;
    pBuffer->playCursor = frameSize ? (float)(dwNewPosition / frameSize) : 0.0f;
    LeaveCriticalSection(&s_audioLock);

    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_SetVolume(EmulatedSoundBuffer* pBuffer, LONG lVolume) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0x80070057;

    EnterCriticalSection(&s_audioLock);
    pBuffer->volume = lVolume;
    pBuffer->linearVol = MillibelsToLinear(lVolume);
    LeaveCriticalSection(&s_audioLock);

    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_SetFrequency(EmulatedSoundBuffer* pBuffer, DWORD dwFrequency) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0x80070057;

    if (dwFrequency >= 100 && dwFrequency <= 192000) {
        EnterCriticalSection(&s_audioLock);
        pBuffer->frequency = dwFrequency;
        LeaveCriticalSection(&s_audioLock);
    }
    return 0; // DS_OK
}

static HRESULT __stdcall Hook_DirectSoundBuffer_SetLoopRegion(EmulatedSoundBuffer* pBuffer, DWORD dwLoopStart, DWORD dwLoopLength) {
    if (!pBuffer || pBuffer->magic != SOUND_BUFFER_MAGIC) return 0x80070057;

    EnterCriticalSection(&s_audioLock);
    if (pBuffer->format.wFormatTag == 0x0069) {
        uint32_t blockAlign = pBuffer->channels * 36;
        if (blockAlign > 0) {
            pBuffer->loopStartSample = (dwLoopStart / blockAlign) * 65;
            pBuffer->loopLengthSamples = (dwLoopLength / blockAlign) * 65;
        }
    } else {
        uint32_t frameSize = pBuffer->channels * 2;
        if (frameSize > 0) {
            pBuffer->loopStartSample = dwLoopStart / frameSize;
            pBuffer->loopLengthSamples = dwLoopLength / frameSize;
        }
    }
    LeaveCriticalSection(&s_audioLock);

    return 0; // DS_OK
}

// ---------------------------------------------------------------------------
// Hardware APU Stubs (Proper __stdcall stack cleanups)
// ---------------------------------------------------------------------------
static void __stdcall Hook_Stub_Void(void) {
}

static HRESULT __stdcall Hook_Stub_Ret08(void* a, void* b) {
    (void)a; (void)b;
    return 0; // DS_OK
}

static HRESULT __stdcall Hook_Stub_Ret0C(void* a, void* b, void* c) {
    (void)a; (void)b; (void)c;
    return 0; // DS_OK
}

static HRESULT __stdcall Hook_Stub_Ret14(void* a, void* b, void* c, void* d, void* e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return 0; // DS_OK
}

static HRESULT __stdcall Hook_Stub_Ret18(void* a, void* b, void* c, void* d, void* e, void* f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0; // DS_OK
}

// ---------------------------------------------------------------------------
// JMP Hook Installation Helper
// ---------------------------------------------------------------------------
static void InstallAudioJmpHook(uint32_t targetAddr, void* hookFn) {
    DWORD oldProtect;
    uint8_t jmpBytes[5];
    jmpBytes[0] = 0xE9; // JMP rel32
    int32_t rel = (int32_t)((uintptr_t)hookFn - (targetAddr + 5));
    memcpy(&jmpBytes[1], &rel, 4);
    VirtualProtect((void*)targetAddr, sizeof(jmpBytes), PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)targetAddr, jmpBytes, sizeof(jmpBytes));
}

void Audio_InstallHooks(void) {
    logFmt("[AUDIO] Installing DirectSound hooks...\n");

    // Initialize waveOut mixer
    Audio_Init();

    // 1. DirectSound device lifecycle
    InstallAudioJmpHook(0x00162F91, Hook_DirectSoundCreate);
    InstallAudioJmpHook(0x00160F76, Hook_DirectSound_Release);
    InstallAudioJmpHook(0x00162DA7, Hook_DirectSound_CreateSoundBuffer);
    InstallAudioJmpHook(0x00161ED5, Hook_Stub_Void); // DirectSoundDoWork (RET 0x0)

    // 2. DirectSoundBuffer lifecycle & playback
    InstallAudioJmpHook(0x00160F91, Hook_DirectSoundBuffer_Release);
    InstallAudioJmpHook(0x00161DCD, Hook_DirectSoundBuffer_Play);
    InstallAudioJmpHook(0x00161DF1, Hook_DirectSoundBuffer_Stop);
    InstallAudioJmpHook(0x00161E09, Hook_DirectSoundBuffer_StopEx);
    InstallAudioJmpHook(0x00161E4D, Hook_DirectSoundBuffer_GetStatus);
    InstallAudioJmpHook(0x00161E69, Hook_DirectSoundBuffer_GetCurrentPosition);
    InstallAudioJmpHook(0x00161E89, Hook_DirectSoundBuffer_SetCurrentPosition);

    // 3. Buffer data & streaming
    InstallAudioJmpHook(0x00161EA5, Hook_DirectSoundBuffer_Lock);
    InstallAudioJmpHook(0x00160F8C, Hook_DirectSoundBuffer_Unlock);
    InstallAudioJmpHook(0x00162815, Hook_DirectSoundBuffer_SetBufferData);

    // 4. Buffer properties
    InstallAudioJmpHook(0x00161D5D, Hook_DirectSoundBuffer_SetVolume);
    InstallAudioJmpHook(0x001622C7, Hook_DirectSoundBuffer_SetFrequency);
    InstallAudioJmpHook(0x00161E2D, Hook_DirectSoundBuffer_SetLoopRegion);

    // 5. Hardware APU features (no-op stubs with exact stack argument cleanups)
    InstallAudioJmpHook(0x00161CC5, Hook_Stub_Ret14); // DownloadEffectsImage (RET 0x14)
    InstallAudioJmpHook(0x00161D3D, Hook_Stub_Ret0C); // SetMixBinHeadroom (RET 0x0C)
    InstallAudioJmpHook(0x00161D13, Hook_Stub_Ret18); // SetEffectData (RET 0x18)
    InstallAudioJmpHook(0x00161CEC, Hook_Stub_Ret14); // GetEffectData (RET 0x14)
    InstallAudioJmpHook(0x00162DCB, Hook_Stub_Ret0C); // SetI3DL2Listener (RET 0x0C)
    InstallAudioJmpHook(0x001622E3, Hook_Stub_Ret08); // Buffer::SetMixBins (RET 0x08)
    InstallAudioJmpHook(0x00161DB1, Hook_Stub_Ret08); // Buffer::SetHeadroom (RET 0x08)
    InstallAudioJmpHook(0x00161D79, Hook_Stub_Ret08); // Buffer::SetLFO (RET 0x08)
    InstallAudioJmpHook(0x00161D95, Hook_Stub_Ret08); // Buffer::SetEG (RET 0x08)

    logFmt("[AUDIO] All DirectSound hooks installed successfully.\n");
}
