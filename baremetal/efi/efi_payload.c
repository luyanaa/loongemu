/*
 * Read the guest firmware payload from the boot volume instead of linking it
 * into the EFI image.
 *
 * The integrated image embeds OVMF with .incbin, which makes every build
 * 130 MB and forces a rebuild to change guest firmware.  With
 * LOONGEMU_PAYLOAD_FROM_ESP defined, vmm_entry.c calls these helpers before
 * ExitBootServices and the payload lives on the ESP as ordinary files:
 *
 *     \EFI\loongemu\ovmf_code.fd
 *     \EFI\loongemu\ovmf_vars.fd
 *
 * Everything here runs with boot services alive.  The buffers handed back are
 * page-granular loader allocations that survive ExitBootServices, which is the
 * same contract the .incbin path already relies on: vmm_entry.c copies into
 * them (or, here, reads straight into them) and passes their physical
 * addresses in LoongemuVmmHandoff.
 */

#include <stdint.h>

#include "loongemu/loongarch_paging.h"
#include "efi_min.h"

#define PAYLOAD_MAX_BYTES (16u * 1024u * 1024u)
#define PAYLOAD_CHUNK     (64u * 1024u)

static uint16_t *utf8_to_utf16(const char *src, uint16_t *dst, uint32_t cap)
{
    uint32_t n = 0;

    while (*src != '\0') {
        if (n + 1u >= cap) {
            return 0;
        }
        dst[n++] = (uint16_t)(uint8_t)*src++;
    }
    dst[n] = 0;
    return dst;
}

/*
 * Open `path` on the volume of `image_handle` and read it whole into a newly
 * allocated page buffer.  Returns 0 on any failure; the caller reports it.
 *
 * Path syntax follows EFI_FILE_PROTOCOL::Open: backslashes, no leading one
 * required but accepted.
 */
int efi_payload_read(EFI_BOOT_SERVICES *bs, EFI_HANDLE image_handle,
                     const char *path, EFI_PHYSICAL_ADDRESS *out_base,
                     uint64_t *out_size)
{
    static const EFI_GUID fss_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static const EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
    static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fss = 0;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_FILE_PROTOCOL *file = 0;
    EFI_PHYSICAL_ADDRESS base = 0;
    uint64_t size = 0;
    uint64_t pages;
    uint16_t name[128];
    uint8_t info[128];
    UINTN info_size = sizeof(info);
    EFI_STATUS status;

    if (bs == 0 || path == 0 || out_base == 0 || out_size == 0) {
        return 0;
    }
    *out_base = 0;
    *out_size = 0;

    /* The image's own device handle carries the volume it was loaded from. */
    if (bs->HandleProtocol == 0 || bs->AllocatePages == 0 ||
        bs->FreePages == 0) {
        return 0;
    }
    if (EFI_ERROR(bs->HandleProtocol(image_handle, &li_guid, (void **)&li)) ||
        li == 0 || li->DeviceHandle == 0) {
        return 0;
    }
    if (EFI_ERROR(bs->HandleProtocol(li->DeviceHandle, &fss_guid,
                                     (void **)&fss)) ||
        fss == 0 || fss->OpenVolume == 0) {
        return 0;
    }
    if (EFI_ERROR(fss->OpenVolume(fss, &root)) || root == 0) {
        return 0;
    }
    if (utf8_to_utf16(path, name, sizeof(name) / sizeof(name[0])) == 0) {
        (void)root->Close(root);
        return 0;
    }
    if (EFI_ERROR(root->Open(root, &file, name, EFI_FILE_MODE_READ, 0)) ||
        file == 0) {
        (void)root->Close(root);
        return 0;
    }
    (void)root->Close(root);

    /* EFI_FILE_INFO gives the exact length; the buffer is grown to match. */
    if (EFI_ERROR(file->GetInfo(file, &file_info_guid, &info_size, info)) ||
        info_size < sizeof(EFI_FILE_INFO_HEAD)) {
        (void)file->Close(file);
        return 0;
    }
    size = ((const EFI_FILE_INFO_HEAD *)info)->FileSize;
    if (size == 0 || size > PAYLOAD_MAX_BYTES) {
        (void)file->Close(file);
        return 0;
    }
    pages = (size + LAPG_PAGE_SIZE - 1u) >> LAPG_PAGE_BITS;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                               (UINTN)pages, &base);
    if (EFI_ERROR(status) || base == 0) {
        (void)file->Close(file);
        return 0;
    }

    /*
     * Read in bounded chunks: one Read fills at most the remaining bytes, and
     * an EFI file may report a short read at any size, so loop until done.
     */
    {
        uint64_t done = 0;
        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)base;

        while (done < size) {
            UINTN want = (UINTN)((size - done) < PAYLOAD_CHUNK
                                     ? (size - done)
                                     : PAYLOAD_CHUNK);
            UINTN got = want;

            if (EFI_ERROR(file->Read(file, &got, (void *)(uintptr_t)(dst + done))) ||
                got == 0) {
                (void)file->Close(file);
                (void)bs->FreePages(base, (UINTN)pages);
                return 0;
            }
            done += got;
        }
        /* The tail of the last page must not leak stale bytes into the ROM. */
        for (uint64_t off = size; off < pages * LAPG_PAGE_SIZE; off++) {
            dst[off] = 0xffu;
        }
    }

    (void)file->Close(file);
    *out_base = base;
    *out_size = pages * LAPG_PAGE_SIZE;
    return 1;
}
