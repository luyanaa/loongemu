#ifndef QEMU_OSDEP_H
#define QEMU_OSDEP_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include "latx_bare.h"
#include "glib.h"

#include "cpu-features.h"
typedef uint64_t ram_addr_t;
#define TARGET_LONG_BITS 64
typedef uint64_t vaddr;
typedef uint64_t hwaddr;
typedef int MMUAccessType;
#define MMU_DATA_LOAD  0
#define MMU_DATA_STORE 1
#define MMU_INST_FETCH 2
#define TARGET_PHYS_ADDR_SPACE_BITS 40
typedef uint64_t abi_ulong;
typedef int64_t abi_long;
typedef uint64_t abi_ptr;
struct CPUState;

#define FLT_MIN 1.17549435e-38F
#define FLT_MAX 3.40282347e+38F
#define TARGET_FMT_lx "%016" PRIx64
#define TARGET_FMT_ld "%" PRId64
#define TARGET_FMT_lu "%" PRIu64
typedef unsigned long uintptr_t_alias;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define G_N_ELEMENTS(x) ARRAY_SIZE(x)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), "qemu build bug: " #x)
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define QEMU_UNUSED_VAR(x) ((void)(x))
#define QEMU_NORETURN __attribute__((noreturn))
#define QEMU_FLATTEN __attribute__((flatten))
#define QEMU_ALWAYS_INLINE __attribute__((always_inline)) inline
#define QEMU_GNUC_PRINTF(f, a) __attribute__((format(printf, f, a)))
#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif
#define qemu_icache_linesize 64
#define B_STUB_SIZE 8

typedef struct { void *opaque[8]; } sigjmp_buf;
void siglongjmp(sigjmp_buf env, int val);
void cpu_loop_exit(struct CPUState *cs);

#define glue_(a, b) a##b
#define glue(a, b) glue_(a, b)
#define xglue(a, b) glue(a, b)
#define stringify_(x) #x
#define stringify(x) stringify_(x)

static inline int64_t sextract64(uint64_t v, int start, int len)
{
    uint64_t mask = (len == 64) ? ~0ULL : ((UINT64_C(1) << len) - 1);
    uint64_t field = (v >> start) & mask;
    uint64_t sign = UINT64_C(1) << (len - 1);

    return (int64_t)((field ^ sign) - sign);
}


typedef struct IRQState *qemu_irq;
static inline void qemu_irq_lower(qemu_irq irq) { (void)irq; }
static inline void qemu_irq_raise(qemu_irq irq) { (void)irq; }
extern struct CPUState *first_cpu;
#if defined(CONFIG_LATM)
#define GETPC() ((uintptr_t)__builtin_return_address(0))
#else
#define GETPC() ((uintptr_t)0)
#endif

typedef struct QemuMutex { int locked; } QemuMutex;
typedef struct QemuSpin { int value; } QemuSpin;

#endif
