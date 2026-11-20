#ifndef LATM_EXEC_HELPER_PROTO_H
#define LATM_EXEC_HELPER_PROTO_H
#include "qemu/osdep.h"
#include "cpu.h"

void do_raise_exception(CPUMIPSState *env, uint32_t exception, uintptr_t pc);
void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc);

/* Generate C prototypes from QEMU's helper declarations.  The helper bodies
 * in latm/helpers are compiled directly, so the generated TCG call stubs are
 * not needed. */
#define noreturn void
#define env CPUMIPSState *env
#define tl uint64_t
#define i32 uint32_t
#define i64 uint64_t
#define ptr void *

#define DEF_HELPER_1(name, ret, t1) ret helper_##name(t1);
#define DEF_HELPER_2(name, ret, t1, t2) ret helper_##name(t1, t2);
#define DEF_HELPER_3(name, ret, t1, t2, t3) ret helper_##name(t1, t2, t3);
#define DEF_HELPER_4(name, ret, t1, t2, t3, t4) ret helper_##name(t1, t2, t3, t4);
#define DEF_HELPER_FLAGS_1(name, flags, ret, t1) ret helper_##name(t1);
#define DEF_HELPER_FLAGS_2(name, flags, ret, t1, t2) ret helper_##name(t1, t2);
#define DEF_HELPER_FLAGS_3(name, flags, ret, t1, t2, t3) ret helper_##name(t1, t2, t3);
#define DEF_HELPER_FLAGS_4(name, flags, ret, t1, t2, t3, t4) ret helper_##name(t1, t2, t3, t4);

#include "helper.h"

#undef DEF_HELPER_1
#undef DEF_HELPER_2
#undef DEF_HELPER_3
#undef DEF_HELPER_4
#undef DEF_HELPER_FLAGS_1
#undef DEF_HELPER_FLAGS_2
#undef DEF_HELPER_FLAGS_3
#undef DEF_HELPER_FLAGS_4
#undef noreturn
#undef env
#undef tl
#undef i32
#undef i64
#undef ptr

#endif
