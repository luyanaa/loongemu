#include "loongemu/hyperv.h"

#define HV_SUCCESS 0u
#define HV_INVALID_CODE 2u
#define HV_INVALID_INPUT 3u
#define HV_INVALID_ALIGNMENT 4u
#define HV_INVALID_PARAMETER 5u
#define HV_FLUSH_ALL_PROCESSORS UINT64_C(1)
#define HV_FLUSH_ALL_ADDRESS_SPACES UINT64_C(2)
#define HV_FLUSH_NON_GLOBAL_ONLY UINT64_C(4)

/* The TLFS input block must be naturally aligned and fit in one 4 KiB page. */
typedef struct HypervInput {
    const LoongemuHypervOps *ops;
    uint64_t gpa;
    uint64_t words[2];
    int fast;
} HypervInput;

static int input_word(const HypervInput *input, unsigned index, uint64_t *value)
{
    if (input->fast) {
        if (index < 2) {
            *value = input->words[index];
            return 1;
        }
        if (index >= 14 || !input->ops->read_xmm_qword) {
            return 0;
        }
        *value = input->ops->read_xmm_qword(input->ops->opaque, index - 2);
        return 1;
    }
    if (index >= 512 || (input->gpa & UINT64_C(0xfff)) +
                        ((uint64_t)index + 1u) * 8u > 4096u) {
        return -1; /* Input spans pages. */
    }
    if (input->gpa > UINT64_MAX - (uint64_t)index * 8u) {
        return 0;
    }
    return input->ops->read_guest &&
           input->ops->read_guest(input->ops->opaque,
                                  input->gpa + (uint64_t)index * 8u, value);
}

static uint64_t existing_vps(const LoongemuHypervOps *ops)
{
    if (ops->vp_count >= 64u) {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << ops->vp_count) - 1u;
}

static unsigned popcount64(uint64_t x)
{
    unsigned count = 0;
    while (x) {
        x &= x - 1u;
        count++;
    }
    return count;
}

uint64_t loongemu_hyperv_call(const LoongemuHypervOps *ops, uint64_t control,
                              uint64_t rdx, uint64_t r8)
{
    uint16_t code = (uint16_t)control;
    unsigned variable = (unsigned)((control >> 17) & 0x3ffu);
    unsigned count = (unsigned)((control >> 32) & 0xfffu);
    unsigned start = (unsigned)((control >> 48) & 0xfffu);
    int extended = code == 0x13u || code == 0x14u;
    int list = code == 0x03u || code == 0x14u;
    HypervInput input = {ops, rdx, {rdx, r8}, (control & (UINT64_C(1) << 16)) != 0};
    uint64_t address_space, flags, mask, word;
    unsigned header_words;
    int read_status;

    if (!ops || ops->vp_count > 64u) {
        return HV_INVALID_INPUT;
    }
    if (code != 0x02u && code != 0x03u && code != 0x08u &&
        code != 0x0bu && code != 0x13u && code != 0x14u) {
        return HV_INVALID_CODE;
    }
    /* Reserved and nested bits, plus invalid repetition/header fields. */
    if ((control & UINT64_C(0xf000f000f8000000)) != 0 ||
        (!extended && variable != 0u) ||
        (list ? (count == 0u || start >= count) : (count != 0u || start != 0u))) {
        return HV_INVALID_INPUT;
    }
    if (code == 0x08u) {
        return HV_SUCCESS;
    }
    if (!input.fast && (input.gpa & 7u) != 0u) {
        return HV_INVALID_ALIGNMENT;
    }
    if (code == 0x0bu) {
        uint64_t vector_and_vtl;
        read_status = input_word(&input, 0, &vector_and_vtl);
        if (read_status > 0) read_status = input_word(&input, 1, &mask);
        if (read_status <= 0) {
            return read_status < 0 ? HV_INVALID_ALIGNMENT : HV_INVALID_INPUT;
        }
        word = vector_and_vtl & UINT64_C(0xffffffff);
        /* Only VTL0 is implemented; padding bytes [7:5] are ignored. */
        if (word < 0x10u || word > 0xffu ||
            ((vector_and_vtl >> 32) & 0xe0u) != 0u ||
            (((vector_and_vtl >> 36) & 1u) != 0u &&
             ((vector_and_vtl >> 32) & 0x0fu) != 0u)) {
            return HV_INVALID_INPUT;
        }
        if (!ops->send_ipi) {
            return HV_INVALID_INPUT;
        }
        ops->send_ipi(ops->opaque, mask & existing_vps(ops), (uint8_t)word);
        return HV_SUCCESS;
    }
    read_status = input_word(&input, 0, &address_space);
    if (read_status > 0) read_status = input_word(&input, 1, &flags);
    if (read_status <= 0) {
        return read_status < 0 ? HV_INVALID_ALIGNMENT : HV_INVALID_INPUT;
    }
    if ((flags & ~(HV_FLUSH_ALL_PROCESSORS | HV_FLUSH_ALL_ADDRESS_SPACES |
                   (list ? 0u : HV_FLUSH_NON_GLOBAL_ONLY))) != 0u) {
        return HV_INVALID_PARAMETER;
    }
    if (!extended) {
        read_status = input_word(&input, 2, &mask);
        if (read_status <= 0) {
            return read_status < 0 ? HV_INVALID_ALIGNMENT : HV_INVALID_INPUT;
        }
        header_words = 3;
    } else {
        uint64_t format, banks;
        read_status = input_word(&input, 2, &format);
        if (read_status > 0) read_status = input_word(&input, 3, &banks);
        if (read_status <= 0) {
            return read_status < 0 ? HV_INVALID_ALIGNMENT : HV_INVALID_INPUT;
        }
        if (format > 1u || popcount64(banks) != variable ||
            (format == 0u && (banks & ~UINT64_C(1)) != 0u)) {
            return HV_INVALID_INPUT; /* Only VP indices 0..63 are supported. */
        }
        mask = format == 1u ? existing_vps(ops) : 0u;
        for (unsigned bank = 0, n = 0; bank < 64u; bank++) {
            if ((banks & (UINT64_C(1) << bank)) == 0u) {
                continue;
            }
            read_status = input_word(&input, 4u + n++, &word);
            if (read_status <= 0) {
                return read_status < 0 ? HV_INVALID_ALIGNMENT : HV_INVALID_INPUT;
            }
            if (format == 0u && bank == 0u) {
                mask = word;
            }
        }
        header_words = 4u + variable;
    }
    /* Validate the complete rep block before invalidating anything. */
    for (unsigned n = 0; n < count; n++) {
        read_status = input_word(&input, header_words + n, &word);
        if (read_status <= 0) {
            return read_status < 0 ? HV_INVALID_ALIGNMENT : HV_INVALID_INPUT;
        }
    }
    if ((flags & HV_FLUSH_ALL_PROCESSORS) == 0u && mask == 0u) {
        return HV_INVALID_PARAMETER;
    }
    if ((flags & HV_FLUSH_ALL_PROCESSORS) != 0u) {
        mask = existing_vps(ops);
    } else {
        mask &= existing_vps(ops);
    }
    if (!ops->flush) {
        return HV_INVALID_INPUT;
    }
    /* A full address-space flush is a valid conservative list flush. */
    ops->flush(ops->opaque, mask, address_space, flags);
    return list ? ((uint64_t)count << 32) : HV_SUCCESS;
}
