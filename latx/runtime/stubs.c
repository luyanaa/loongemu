/*
 * Tree-owned copies of the LATX prologue/epilogue builders.  They translate
 * the context-switch stubs into the supplied code buffers and publish the
 * well-known entry points used by generated exits:
 *   context_switch_bt_to_native       -> callable from C as (tb_code, env)
 *   context_switch_native_to_bt_ret_0 -> returned-to stub on TB exit
 */
#include "qemu/osdep.h"
#include "exec/exec-all.h"
#include "cpu.h"
#include "lsenv.h"
#include "translate.h"

int latx_gen_prologue(void *code_buf)
{
    TRANSLATION_DATA *lat_ctx = lsenv->tr_data;
    int code_nr;

    context_switch_bt_to_native = (ADDR)code_buf;
    tr_init(NULL);
    generate_context_switch_bt_to_native(code_buf);
    label_dispose(NULL, lat_ctx);
    code_nr = tr_ir2_assemble(code_buf, lat_ctx->first_ir2);
    tr_fini(false);
    return code_nr;
}

int latx_gen_epilogue(void *code_buf)
{
    TRANSLATION_DATA *lat_ctx = lsenv->tr_data;
    int code_nr;

    context_switch_native_to_bt_ret_0 = (ADDR)code_buf;
    context_switch_native_to_bt_ret_id_3 = (ADDR)code_buf + 12;
    context_switch_native_to_bt_ret_id_1 = (ADDR)code_buf + 20;
    context_switch_native_to_bt_ret_id_0 = (ADDR)code_buf + 28;
    context_switch_native_to_bt = (ADDR)code_buf + 32;

    tr_init(NULL);
    generate_context_switch_native_to_bt();
    label_dispose(NULL, lat_ctx);
    code_nr = tr_ir2_assemble(code_buf, lat_ctx->first_ir2);
    tr_fini(false);
    return code_nr;
}
