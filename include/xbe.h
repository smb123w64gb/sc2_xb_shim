#ifndef XBE_H
#define XBE_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

typedef struct _XBE_HEADER {
    uint32_t Magic;                     // 'XBEH'
    uint8_t  Signature[256];
    uint32_t BaseAddress;               // Typically 0x00010000
    uint32_t SizeOfHeaders;
    uint32_t SizeOfImage;
    uint32_t SizeOfImageHeader;
    uint32_t TimeDateStamp;
    uint32_t CertificateAddress;
    uint32_t NumberOfSections;
    uint32_t SectionHeadersAddress;
    uint32_t InitFlags;
    uint32_t EntryPoint;                // XORed in retail
    uint32_t TlsAddress;
    uint32_t StackCommit;
    uint32_t HeapReserve;
    uint32_t HeapCommit;
    uint32_t SizeOfPeVirtual;
    uint32_t PeCheckSum;
    uint32_t PeTimeDateStamp;
    uint32_t DebugPathAddress;
    uint32_t DebugFileNameAddress;
    uint32_t DebugUnicodeFileNameAddress;
    uint32_t KernelImageThunkAddress;   // XORed in retail
    uint32_t NonKernelImportDirectoryAddress;
    uint32_t NumberOfLibraryVersions;
    uint32_t LibraryVersionsAddress;
    uint32_t KernelLibraryVersionAddress;
    uint32_t XapiLibraryVersionAddress;
    uint32_t MicrosoftLogoAddress;
    uint32_t SizeOfMicrosoftLogo;
} XBE_HEADER, *PXBE_HEADER;

typedef struct _XBE_SECTION_HEADER {
    uint32_t Flags;
    uint32_t VirtualAddress;
    uint32_t VirtualSize;
    uint32_t RawAddress;
    uint32_t RawSize;
    uint32_t SectionNameAddress;
    uint32_t SectionNameRefCount;
    uint32_t HeadSharedRefCountAddress;
    uint32_t TailSharedRefCountAddress;
    uint8_t  Digest[20];
} XBE_SECTION_HEADER, *PXBE_SECTION_HEADER;

#pragma pack(pop)

typedef struct _XBE_LOADED_IMAGE {
    uint8_t* FileData;
    size_t   FileSize;
    XBE_HEADER Header;
    uint32_t DecryptedEntryPoint;
    uint32_t DecryptedKernelThunk;
} XBE_LOADED_IMAGE;

BOOL Xbe_LoadAndMap(const char* xbeFilePath, XBE_LOADED_IMAGE* outImage);
BOOL Xbe_ApplyHlePatches(void);
void NativeSC2_SetRenderConfig(uint32_t width, uint32_t height, BOOL fullscreen);
void NativeSC2_InitRenderer(void);

#ifdef __cplusplus
}
#endif

#endif // XBE_H
