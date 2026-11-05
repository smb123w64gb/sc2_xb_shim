#ifndef XAPI_SHIM_H
#define XAPI_SHIM_H

#include <windows.h>
#include <stdint.h>
#include <xinput.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
// Xbox Gamepad State
typedef struct _XINPUT_GAMEPAD_XBOX {
    WORD    wButtons;
    BYTE    bAnalogButtons[8]; // A, B, X, Y, BLACK, WHITE, L, R
    SHORT   sThumbLX;
    SHORT   sThumbLY;
    SHORT   sThumbRX;
    SHORT   sThumbRY;
} XINPUT_GAMEPAD_XBOX, *PXINPUT_GAMEPAD_XBOX;

typedef struct _XINPUT_STATE_XBOX {
    DWORD               dwPacketNumber;
    XINPUT_GAMEPAD_XBOX Gamepad;
} XINPUT_STATE_XBOX, *PXINPUT_STATE_XBOX;
#pragma pack(pop)

// XAPI Shims
void     __stdcall Shim_XInitDevices(DWORD dwNumDeviceTypes, void* pDeviceTypes);
DWORD    __stdcall Shim_XGetDevices(void* pDeviceType);
HANDLE   __stdcall Shim_XInputOpen(void* pDeviceType, DWORD dwPort, DWORD dwSlot, void* pFeedbackBuffer);
void     __stdcall Shim_XInputClose(HANDLE hDevice);
DWORD    __stdcall Shim_XInputGetCapabilities(HANDLE hDevice, void* pCapabilities);
DWORD    __stdcall Shim_XInputGetState(HANDLE hDevice, PXINPUT_STATE_XBOX pState);
DWORD    __stdcall Shim_XInputSetState(HANDLE hDevice, void* pFeedback);
DWORD    __stdcall Shim_XGetVideoStandard(void);
DWORD    __stdcall Shim_XGetVideoFlags(void);
DWORD    __stdcall Shim_XGetAudioFlags(void);

#ifdef __cplusplus
}
#endif

#endif // XAPI_SHIM_H
