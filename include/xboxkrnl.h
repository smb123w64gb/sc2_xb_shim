#ifndef XBOXKRNL_H
#define XBOXKRNL_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Xbox NTSTATUS codes
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((uint32_t)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL              ((uint32_t)0xC0000001L)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED           ((uint32_t)0xC0000002L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER         ((uint32_t)0xC000000DL)
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY                 ((uint32_t)0xC0000017L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND     ((uint32_t)0xC0000034L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL          ((uint32_t)0xC0000023L)
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE               ((uint32_t)0xC0000011L)
#endif

// String structures
typedef struct _ANSI_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    char*    Buffer;
} ANSI_STRING, *PANSI_STRING;

typedef struct _UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    wchar_t* Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    HANDLE          RootDirectory;
    PANSI_STRING    ObjectName;
    uint32_t        Attributes;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    union {
        uint32_t Status;
        void*    Pointer;
    };
    uint32_t Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _TIME_FIELDS {
    int16_t Year;
    int16_t Month;
    int16_t Day;
    int16_t Hour;
    int16_t Minute;
    int16_t Second;
    int16_t Milliseconds;
    int16_t Weekday;
} TIME_FIELDS, *PTIME_FIELDS;

// Xbox-specific non-volatile settings
#define XC_VIDEO_FLAGS_WIDESCREEN        0x00000001
#define XC_VIDEO_FLAGS_HDTV_720p         0x00000002
#define XC_VIDEO_FLAGS_HDTV_1080i        0x00000004
#define XC_VIDEO_FLAGS_HDTV_480p         0x00000008
#define XC_VIDEO_FLAGS_LETTERBOX         0x00000010
#define XC_VIDEO_FLAGS_PAL_60Hz          0x00000040

#define XC_VIDEO_STANDARD_NTSC_M         1
#define XC_VIDEO_STANDARD_NTSC_J         2
#define XC_VIDEO_STANDARD_PAL_I          3

#define XC_AUDIO_FLAGS_BASIC(f)          ((f) & 0x0000FFFF)
#define XC_AUDIO_FLAGS_ENABLE_AC3        0x00010000
#define XC_AUDIO_FLAGS_ENABLE_DTS        0x00020000

// Kernel Thunk Table Base in Soulcalibur II (.rdata)
#define SC2_THUNK_TABLE_ADDR             0x00175D20
#define SC2_THUNK_TABLE_COUNT            120
#define SC2_ENTRY_POINT                  0x00105423
#define SC2_GAME_MAIN                    0x00033C90

// Function pointer typedef for resolved ordinals
typedef void* (*XboxKernelThunkFunc)(void);

// Table entry mapping
typedef struct _XBOX_THUNK_MAP {
    uint32_t ordinal;
    const char* name;
    void* handler;
} XBOX_THUNK_MAP;

void XboxKrnl_Init(void);
void XboxKrnl_SetGameDirectory(const char* dir);
void* XboxKrnl_GetProcAddressByOrdinal(uint32_t ordinal);
void* XboxKrnl_GetThunkHandlerByIndex(uint32_t index);
const char* XboxKrnl_GetThunkNameByIndex(uint32_t index);
void* XboxKrnl_GpuToCpu(uint32_t gpuAddr);

#ifdef __cplusplus
}
#endif

#endif // XBOXKRNL_H
