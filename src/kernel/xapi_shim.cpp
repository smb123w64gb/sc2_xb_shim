#include "xapi_shim.h"
#include "xboxkrnl.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

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

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, XINPUT_STATE* pState);
static PFN_XInputGetState g_pfnXInputGetState = NULL;
static XINPUT_STATE g_latestPadState = {0};
static volatile DWORD g_padConnected = 0;

static DWORD WINAPI XInputPollThread(LPVOID lpParam) {
    (void)lpParam;
    while (1) {
        if (g_pfnXInputGetState) {
            XINPUT_STATE st;
            memset(&st, 0, sizeof(st));
            DWORD res = g_pfnXInputGetState(0, &st);
            if (res == ERROR_SUCCESS) {
                g_latestPadState = st;
                g_padConnected = 1;
            } else {
                g_padConnected = 0;
            }
        }
        Sleep(8); // Poll at 125 Hz
    }
    return 0;
}

static void InitModernXInput(void) {
    if (g_pfnXInputGetState) return;
    HMODULE hXInput = LoadLibraryA("xinput1_3.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput9_1_0.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput1_4.dll");
    if (hXInput) {
        g_pfnXInputGetState = (PFN_XInputGetState)GetProcAddress(hXInput, "XInputGetState");
        logFmt("[XAPI] Loaded XInput from %p (XInputGetState=%p)\n", hXInput, g_pfnXInputGetState);
        CreateThread(NULL, 0, XInputPollThread, NULL, 0, NULL);
    }
}

extern "C" {

void __stdcall Shim_XInitDevices(DWORD dwNumDeviceTypes, void* pDeviceTypes) {
    (void)dwNumDeviceTypes; (void)pDeviceTypes;
    InitModernXInput();
    logFmt("[XAPI] XInitDevices called.\n");
}

DWORD __stdcall Shim_XGetDevices(void* pDeviceType) {
    if (pDeviceType == (void*)0x0016DF30) {
        // Memory Unit (MU) query: report no MUs inserted so save subsystem does not stall
        return 0;
    }
    // Gamepad query (0x0016DFAC or other): report Port 0 connected
    return 0x00000001;
}

HANDLE __stdcall Shim_XInputOpen(void* pDeviceType, DWORD dwPort, DWORD dwSlot, void* pFeedbackBuffer) {
    (void)pDeviceType; (void)dwSlot; (void)pFeedbackBuffer;
    if (dwPort == 0) {
        static bool s_loggedOpen = false;
        if (!s_loggedOpen) {
            logFmt("[XAPI] XInputOpen: Port 0 opened successfully.\n");
            s_loggedOpen = true;
        }
        return (HANDLE)(uintptr_t)1;
    }
    SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    return NULL;
}

void __stdcall Shim_XInputClose(HANDLE hDevice) {
    (void)hDevice;
}

DWORD __stdcall Shim_XInputGetCapabilities(HANDLE hDevice, void* pCapabilities) {
    (void)hDevice;
    if (pCapabilities) {
        memset(pCapabilities, 0, 32);
        ((uint8_t*)pCapabilities)[0] = 0;
        ((uint8_t*)pCapabilities)[1] = 0;
        ((uint8_t*)pCapabilities)[2] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        ((uint8_t*)pCapabilities)[3] = 1; // Standard Gamepad
    }
    return ERROR_SUCCESS;
}

DWORD __stdcall Shim_XInputGetState(HANDLE hDevice, PXINPUT_STATE_XBOX pState) {
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        logFmt("[NativeSC2] Escape pressed. Terminating process.\n");
        TerminateProcess(GetCurrentProcess(), 0);
    }

    if (!pState) return ERROR_INVALID_PARAMETER;
    DWORD port = ((DWORD)(uintptr_t)hDevice) - 1;
    if (port != 0) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    static bool s_firstGetState = true;
    if (s_firstGetState) {
        logFmt("[XAPI] Shim_XInputGetState first call: hDevice=%p, port=%lu\n", hDevice, port);
        s_firstGetState = false;
    }

    memset(pState, 0, sizeof(XINPUT_STATE_XBOX));

    static DWORD s_kbPacket = 0;
    s_kbPacket++;
    pState->dwPacketNumber = s_kbPacket;

    if (g_padConnected) {
        XINPUT_STATE winState;
        winState.dwPacketNumber = g_latestPadState.dwPacketNumber;
        winState.Gamepad.wButtons = g_latestPadState.Gamepad.wButtons;
        winState.Gamepad.bLeftTrigger = g_latestPadState.Gamepad.bLeftTrigger;
        winState.Gamepad.bRightTrigger = g_latestPadState.Gamepad.bRightTrigger;
        winState.Gamepad.sThumbLX = g_latestPadState.Gamepad.sThumbLX;
        winState.Gamepad.sThumbLY = g_latestPadState.Gamepad.sThumbLY;
        winState.Gamepad.sThumbRX = g_latestPadState.Gamepad.sThumbRX;
        winState.Gamepad.sThumbRY = g_latestPadState.Gamepad.sThumbRY;

        pState->dwPacketNumber = winState.dwPacketNumber;
        pState->Gamepad.wButtons = winState.Gamepad.wButtons & 0x00FF;

        pState->Gamepad.bAnalogButtons[0] = (winState.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[1] = (winState.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[2] = (winState.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[3] = (winState.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[4] = (winState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[5] = (winState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[6] = winState.Gamepad.bLeftTrigger;
        pState->Gamepad.bAnalogButtons[7] = winState.Gamepad.bRightTrigger;

        if (winState.Gamepad.wButtons & XINPUT_GAMEPAD_A) pState->Gamepad.wButtons |= 0x1000;
        if (winState.Gamepad.wButtons & XINPUT_GAMEPAD_B) pState->Gamepad.wButtons |= 0x2000;
        if (winState.Gamepad.wButtons & XINPUT_GAMEPAD_X) pState->Gamepad.wButtons |= 0x4000;
        if (winState.Gamepad.wButtons & XINPUT_GAMEPAD_Y) pState->Gamepad.wButtons |= 0x8000;

        pState->Gamepad.sThumbLX = winState.Gamepad.sThumbLX;
        pState->Gamepad.sThumbLY = winState.Gamepad.sThumbLY;
        pState->Gamepad.sThumbRX = winState.Gamepad.sThumbRX;
        pState->Gamepad.sThumbRY = winState.Gamepad.sThumbRY;
    }

    // Keyboard fallback for Port 0
    if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
        pState->Gamepad.wButtons |= 0x0010; // START
        pState->Gamepad.bAnalogButtons[0] = 255;
        pState->Gamepad.wButtons |= 0x1000;
    }
    if (GetAsyncKeyState(VK_BACK) & 0x8000)   pState->Gamepad.wButtons |= 0x0020; // BACK
    if (GetAsyncKeyState(VK_UP) & 0x8000)     pState->Gamepad.wButtons |= 0x0001; // UP
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)   pState->Gamepad.wButtons |= 0x0002; // DOWN
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)   pState->Gamepad.wButtons |= 0x0004; // LEFT
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000)  pState->Gamepad.wButtons |= 0x0008; // RIGHT

    if ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState('Z') & 0x8000) || (GetAsyncKeyState('J') & 0x8000)) {
        pState->Gamepad.bAnalogButtons[0] = 255;
        pState->Gamepad.wButtons |= 0x1000;
    }
    if ((GetAsyncKeyState('X') & 0x8000) || (GetAsyncKeyState('K') & 0x8000)) {
        pState->Gamepad.bAnalogButtons[1] = 255;
        pState->Gamepad.wButtons |= 0x2000;
    }
    if ((GetAsyncKeyState('C') & 0x8000) || (GetAsyncKeyState('U') & 0x8000)) {
        pState->Gamepad.bAnalogButtons[2] = 255;
        pState->Gamepad.wButtons |= 0x4000;
    }
    if ((GetAsyncKeyState('V') & 0x8000) || (GetAsyncKeyState('I') & 0x8000)) {
        pState->Gamepad.bAnalogButtons[3] = 255;
        pState->Gamepad.wButtons |= 0x8000;
    }

    if (GetAsyncKeyState('W') & 0x8000 || GetAsyncKeyState(VK_UP) & 0x8000)    pState->Gamepad.sThumbLY = 32000;
    if (GetAsyncKeyState('S') & 0x8000 || GetAsyncKeyState(VK_DOWN) & 0x8000)  pState->Gamepad.sThumbLY = -32000;
    if (GetAsyncKeyState('D') & 0x8000 || GetAsyncKeyState(VK_RIGHT) & 0x8000) pState->Gamepad.sThumbLX = 32000;
    if (GetAsyncKeyState('A') & 0x8000 || GetAsyncKeyState(VK_LEFT) & 0x8000)  pState->Gamepad.sThumbLX = -32000;

    static bool s_lastA = false;
    static bool s_lastStart = false;
    bool curA = (pState->Gamepad.bAnalogButtons[0] > 0);
    bool curStart = ((pState->Gamepad.wButtons & 0x0010) != 0);
    if (curA && !s_lastA) {
        logFmt("[INPUT] Button A Pressed (Confirm)!\n");
    }
    if (curStart && !s_lastStart) {
        logFmt("[INPUT] Button Start Pressed!\n");
    }
    s_lastA = curA;
    s_lastStart = curStart;

    return ERROR_SUCCESS;
}

DWORD __stdcall Shim_XInputSetState(HANDLE hDevice, void* pFeedback) {
    (void)hDevice; (void)pFeedback;
    return ERROR_SUCCESS;
}

DWORD __stdcall Shim_XGetVideoStandard(void) {
    return XC_VIDEO_STANDARD_NTSC_M;
}

DWORD __stdcall Shim_XGetVideoFlags(void) {
    // 480p Widescreen (16:9). Original Xbox Soulcalibur II supports 16:9 widescreen ONLY in 480p,
    // while our Direct3D 9 backend renders the backbuffer natively at 1280x720 HD.
    return XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV_480p;
}

DWORD __stdcall Shim_XGetAudioFlags(void) {
    return 0; // Stereo
}

} // extern "C"
