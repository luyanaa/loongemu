/*
 * Minimal UEFI types for the thin VMM.
 *
 * The BootServices member order below is the one in the UEFI specification
 * (and in MdePkg/Include/Uefi/UefiSpec.h): ExitBootServices is member 26
 * after the table header, not the 20-something position a partial list
 * would suggest.  Getting this wrong calls a random service pointer, so the
 * struct mirrors the real layout completely rather than only the members
 * this file happens to use.
 */
#ifndef LOONGEMU_EFI_MIN_H
#define LOONGEMU_EFI_MIN_H

#include <stdint.h>

/*
 * LoongArch has a single calling convention, so EFIAPI is empty there
 * (MdePkg/Include/LoongArch64/ProcessorBind.h).  Declaring ms_abi here would
 * silently mismatch every firmware call.
 */
#define EFIAPI

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;

#define EFI_SUCCESS            0
#define EFI_ERROR_BIT          (UINT64_C(1) << 63)
#define EFI_INVALID_PARAMETER  (EFI_ERROR_BIT | 2)
#define EFI_BUFFER_TOO_SMALL   (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND          (EFI_ERROR_BIT | 14)

#define EFI_ERROR(s) (((s) & EFI_ERROR_BIT) != 0)

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiMaxMemoryType = 15,
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2,
    MaxAllocateType = 3,
} EFI_ALLOCATE_TYPE;

/*
 * LocateHandleBuffer search type (UefiSpec.h EFI_LOCATE_SEARCH_TYPE).
 * ByProtocol is the only one this loader needs.
 */
typedef enum {
    AllHandles = 0,
    ByRegisterNotify = 1,
    ByProtocol = 2,
} EFI_LOCATE_SEARCH_TYPE;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/*
 * Only the members this VMM calls are given real prototypes; the rest are
 * opaque pointers that exist to keep the offsets right.  A function-pointer
 * slot is one pointer wide regardless of its signature, so `void *` is a
 * faithful stand-in.
 */
typedef struct {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type,
                                       EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages,
                                       EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize,
                                      EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey,
                                      UINTN *DescriptorSize,
                                      uint32_t *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size,
                                      void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle,
                                        const EFI_GUID *Protocol,
                                        void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;

    /* Member 26.  Everything below is unused but must keep its place. */
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle,
                                          UINTN MapKey);

    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType,
                                            const EFI_GUID *Protocol,
                                            void *SearchKey,
                                            UINTN *NoHandles,
                                            EFI_HANDLE **Buffer);
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *Protocol,
                                        void *Registration,
                                        void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* Loaded Image Protocol GUID: {5b1b31a1-9562-11d2-8e3f-00a0c969723b} */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1u, 0x9562u, 0x11d2u, \
      { 0x8eu, 0x3fu, 0x00u, 0xa0u, 0xc9u, 0x69u, 0x72u, 0x3bu } }

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* Graphics Output Protocol GUID: {9042a9de-23dc-4a38-96fb-7aded080516a} */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9deu, 0x23dcu, 0x4a38u, \
      { 0x96u, 0xfbu, 0x7au, 0xdeu, 0xd0u, 0x80u, 0x51u, 0x6au } }

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    uint32_t PixelInformation[4];
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ACPI 2.0 Table GUID: {8868e871-e4f1-11d3-bc22-0080c73c8881} */
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871u, 0xe4f1u, 0x11d3u, \
      { 0xbcu, 0x22u, 0x00u, 0x80u, 0xc7u, 0x3cu, 0x88u, 0x81u } }

/* ACPI 1.0 Table GUID: {eb9d2d30-2d88-11d3-9a16-0090273fc14d} */
#define EFI_ACPI_TABLE_GUID \
    { 0xeb9d2d30u, 0x2d88u, 0x11d3u, \
      { 0x9au, 0x16u, 0x00u, 0x90u, 0x27u, 0x3fu, 0xc1u, 0x4du } }

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    uint32_t Pad;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/*
 * Simple File System Protocol: {964e5b22-6459-11d2-8e39-00a0c969723b}
 *
 * Only the members this loader calls carry real prototypes; the rest stay
 * opaque to keep their slots the right width.
 */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22u, 0x6459u, 0x11d2u, \
      { 0x8eu, 0x39u, 0x00u, 0xa0u, 0xc9u, 0x69u, 0x72u, 0x3bu } }

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This,
                              EFI_FILE_PROTOCOL **NewHandle,
                              const CHAR16 *FileName,
                              uint64_t OpenMode,
                              uint64_t Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This,
                             UINTN *BufferSize,
                             void *Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This,
                               UINTN *BufferSize,
                               const void *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, uint64_t *Position);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, uint64_t Position);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This,
                                 const EFI_GUID *InformationType,
                                 UINTN *BufferSize,
                                 void *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *This,
                                 const EFI_GUID *InformationType,
                                 UINTN BufferSize,
                                 const void *Buffer);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
};

/* EFI_FILE_MODE_READ */
#define EFI_FILE_MODE_READ 0x0000000000000001ULL

/* EFI_FILE_INFO GUID: {09576e92-6d3f-11d2-8e39-00a0c969723b} */
#define EFI_FILE_INFO_GUID \
    { 0x09576e92u, 0x6d3fu, 0x11d2u, \
      { 0x8eu, 0x39u, 0x00u, 0xa0u, 0xc9u, 0x69u, 0x72u, 0x3bu } }

/* Minimal EFI_FILE_INFO prefix: the size and the attributes this loader reads. */
typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
} EFI_FILE_INFO_HEAD;

typedef struct {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/*
 * Loaded Image Protocol is declared above; these complete the two image
 * services the runtime loader needs.  Replacing the `void *` placeholders in
 * EFI_BOOT_SERVICES would change that struct's member types, so the loader
 * casts the slot it wants: the layout (one pointer per member) is what
 * matters for offset correctness.
 */
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(int BootPolicy,
                                            EFI_HANDLE ParentImageHandle,
                                            void *DevicePath,
                                            void *SourceBuffer,
                                            UINTN SourceSize,
                                            EFI_HANDLE *ImageHandle);
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_START)(EFI_HANDLE ImageHandle,
                                             UINTN *ExitDataSize,
                                             CHAR16 **ExitData);

#endif /* LOONGEMU_EFI_MIN_H */
