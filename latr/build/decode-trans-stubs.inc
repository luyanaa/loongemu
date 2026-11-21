/* Generated from QEMU decodetree declarations; do not edit. */
static bool trans_add(DisasContext *ctx, arg_add *a)
{
    ctx->ir1->opcode = LATR_RV_ADD;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_add_uw(DisasContext *ctx, arg_add_uw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_addd(DisasContext *ctx, arg_addd *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_addi(DisasContext *ctx, arg_addi *a)
{
    ctx->ir1->opcode = LATR_RV_ADDI;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_addid(DisasContext *ctx, arg_addid *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_addiw(DisasContext *ctx, arg_addiw *a)
{
    ctx->ir1->opcode = LATR_RV_ADDIW;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_addw(DisasContext *ctx, arg_addw *a)
{
    ctx->ir1->opcode = LATR_RV_ADDW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_aes32dsi(DisasContext *ctx, arg_aes32dsi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes32dsmi(DisasContext *ctx, arg_aes32dsmi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes32esi(DisasContext *ctx, arg_aes32esi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes32esmi(DisasContext *ctx, arg_aes32esmi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes64ds(DisasContext *ctx, arg_aes64ds *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes64dsm(DisasContext *ctx, arg_aes64dsm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes64es(DisasContext *ctx, arg_aes64es *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes64esm(DisasContext *ctx, arg_aes64esm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes64im(DisasContext *ctx, arg_aes64im *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes64ks1i(DisasContext *ctx, arg_aes64ks1i *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_aes64ks2(DisasContext *ctx, arg_aes64ks2 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_amoadd_d(DisasContext *ctx, arg_amoadd_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOADD_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoadd_w(DisasContext *ctx, arg_amoadd_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOADD_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoand_d(DisasContext *ctx, arg_amoand_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOAND_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoand_w(DisasContext *ctx, arg_amoand_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOAND_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amocas_d(DisasContext *ctx, arg_amocas_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_amocas_q(DisasContext *ctx, arg_amocas_q *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_amocas_w(DisasContext *ctx, arg_amocas_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_amomax_d(DisasContext *ctx, arg_amomax_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMAX_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amomax_w(DisasContext *ctx, arg_amomax_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMAX_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amomaxu_d(DisasContext *ctx, arg_amomaxu_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMAXU_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amomaxu_w(DisasContext *ctx, arg_amomaxu_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMAXU_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amomin_d(DisasContext *ctx, arg_amomin_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMIN_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amomin_w(DisasContext *ctx, arg_amomin_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMIN_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amominu_d(DisasContext *ctx, arg_amominu_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMINU_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amominu_w(DisasContext *ctx, arg_amominu_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOMINU_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoor_d(DisasContext *ctx, arg_amoor_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOOR_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoor_w(DisasContext *ctx, arg_amoor_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOOR_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoswap_d(DisasContext *ctx, arg_amoswap_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOSWAP_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoswap_w(DisasContext *ctx, arg_amoswap_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOSWAP_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoxor_d(DisasContext *ctx, arg_amoxor_d *a)
{
    ctx->ir1->opcode = LATR_RV_AMOXOR_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_amoxor_w(DisasContext *ctx, arg_amoxor_w *a)
{
    ctx->ir1->opcode = LATR_RV_AMOXOR_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_and(DisasContext *ctx, arg_and *a)
{
    ctx->ir1->opcode = LATR_RV_AND;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_andi(DisasContext *ctx, arg_andi *a)
{
    ctx->ir1->opcode = LATR_RV_ANDI;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_andn(DisasContext *ctx, arg_andn *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_auipc(DisasContext *ctx, arg_auipc *a)
{
    ctx->ir1->opcode = LATR_RV_AUIPC;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_bclr(DisasContext *ctx, arg_bclr *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_bclri(DisasContext *ctx, arg_bclri *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_beq(DisasContext *ctx, arg_beq *a)
{
    ctx->ir1->opcode = LATR_RV_BEQ;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_bext(DisasContext *ctx, arg_bext *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_bexti(DisasContext *ctx, arg_bexti *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_bge(DisasContext *ctx, arg_bge *a)
{
    ctx->ir1->opcode = LATR_RV_BGE;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_bgeu(DisasContext *ctx, arg_bgeu *a)
{
    ctx->ir1->opcode = LATR_RV_BGEU;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_binv(DisasContext *ctx, arg_binv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_binvi(DisasContext *ctx, arg_binvi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_blt(DisasContext *ctx, arg_blt *a)
{
    ctx->ir1->opcode = LATR_RV_BLT;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_bltu(DisasContext *ctx, arg_bltu *a)
{
    ctx->ir1->opcode = LATR_RV_BLTU;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_bne(DisasContext *ctx, arg_bne *a)
{
    ctx->ir1->opcode = LATR_RV_BNE;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_brev8(DisasContext *ctx, arg_brev8 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_bset(DisasContext *ctx, arg_bset *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_bseti(DisasContext *ctx, arg_bseti *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c64_illegal(DisasContext *ctx, arg_c64_illegal *a)
{
    (void)a;
    ctx->ir1->opcode = LATR_RV_ILLEGAL;
    return true;
}
static bool trans_c_fld(DisasContext *ctx, arg_c_fld *a)
{
    ctx->ir1->opcode = LATR_RV_FLD;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_c_flw(DisasContext *ctx, arg_c_flw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_fsd(DisasContext *ctx, arg_c_fsd *a)
{
    ctx->ir1->opcode = LATR_RV_FSD;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_c_fsw(DisasContext *ctx, arg_c_fsw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_lbu(DisasContext *ctx, arg_c_lbu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_lh(DisasContext *ctx, arg_c_lh *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_lhu(DisasContext *ctx, arg_c_lhu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_mul(DisasContext *ctx, arg_c_mul *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_not(DisasContext *ctx, arg_c_not *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_sb(DisasContext *ctx, arg_c_sb *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_sext_b(DisasContext *ctx, arg_c_sext_b *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_sext_h(DisasContext *ctx, arg_c_sext_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_sh(DisasContext *ctx, arg_c_sh *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_zext_b(DisasContext *ctx, arg_c_zext_b *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_zext_h(DisasContext *ctx, arg_c_zext_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_c_zext_w(DisasContext *ctx, arg_c_zext_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cbo_clean(DisasContext *ctx, arg_cbo_clean *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cbo_flush(DisasContext *ctx, arg_cbo_flush *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cbo_inval(DisasContext *ctx, arg_cbo_inval *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cbo_zero(DisasContext *ctx, arg_cbo_zero *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_clmul(DisasContext *ctx, arg_clmul *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_clmulh(DisasContext *ctx, arg_clmulh *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_clmulr(DisasContext *ctx, arg_clmulr *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_clz(DisasContext *ctx, arg_clz *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_clzw(DisasContext *ctx, arg_clzw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cm_jalt(DisasContext *ctx, arg_cm_jalt *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cm_mva01s(DisasContext *ctx, arg_cm_mva01s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cm_mvsa01(DisasContext *ctx, arg_cm_mvsa01 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cm_pop(DisasContext *ctx, arg_cm_pop *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cm_popret(DisasContext *ctx, arg_cm_popret *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cm_popretz(DisasContext *ctx, arg_cm_popretz *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cm_push(DisasContext *ctx, arg_cm_push *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cpop(DisasContext *ctx, arg_cpop *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_cpopw(DisasContext *ctx, arg_cpopw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_csrrc(DisasContext *ctx, arg_csrrc *a)
{
    ctx->ir1->opcode = LATR_RV_CSRRC;
    ctx->ir1->arg.csr = a->csr;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_csrrci(DisasContext *ctx, arg_csrrci *a)
{
    ctx->ir1->opcode = LATR_RV_CSRRCI;
    ctx->ir1->arg.csr = a->csr;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_csrrs(DisasContext *ctx, arg_csrrs *a)
{
    ctx->ir1->opcode = LATR_RV_CSRRS;
    ctx->ir1->arg.csr = a->csr;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_csrrsi(DisasContext *ctx, arg_csrrsi *a)
{
    ctx->ir1->opcode = LATR_RV_CSRRSI;
    ctx->ir1->arg.csr = a->csr;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_csrrw(DisasContext *ctx, arg_csrrw *a)
{
    ctx->ir1->opcode = LATR_RV_CSRRW;
    ctx->ir1->arg.csr = a->csr;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_csrrwi(DisasContext *ctx, arg_csrrwi *a)
{
    ctx->ir1->opcode = LATR_RV_CSRRWI;
    ctx->ir1->arg.csr = a->csr;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_ctz(DisasContext *ctx, arg_ctz *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_ctzw(DisasContext *ctx, arg_ctzw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_czero_eqz(DisasContext *ctx, arg_czero_eqz *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_czero_nez(DisasContext *ctx, arg_czero_nez *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_div(DisasContext *ctx, arg_div *a)
{
    ctx->ir1->opcode = LATR_RV_DIV;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_divd(DisasContext *ctx, arg_divd *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_divu(DisasContext *ctx, arg_divu *a)
{
    ctx->ir1->opcode = LATR_RV_DIVU;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_divud(DisasContext *ctx, arg_divud *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_divuw(DisasContext *ctx, arg_divuw *a)
{
    ctx->ir1->opcode = LATR_RV_DIVUW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_divw(DisasContext *ctx, arg_divw *a)
{
    ctx->ir1->opcode = LATR_RV_DIVW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_ebreak(DisasContext *ctx, arg_ebreak *a)
{
    ctx->ir1->opcode = LATR_RV_EBREAK;
    return true;
}
static bool trans_ecall(DisasContext *ctx, arg_ecall *a)
{
    ctx->ir1->opcode = LATR_RV_ECALL;
    return true;
}
static bool trans_fadd_d(DisasContext *ctx, arg_fadd_d *a)
{
    ctx->ir1->opcode = LATR_RV_FADD_D;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fadd_h(DisasContext *ctx, arg_fadd_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fadd_s(DisasContext *ctx, arg_fadd_s *a)
{
    ctx->ir1->opcode = LATR_RV_FADD_S;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fclass_d(DisasContext *ctx, arg_fclass_d *a)
{
    ctx->ir1->opcode = LATR_RV_FCLASS_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_fclass_h(DisasContext *ctx, arg_fclass_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fclass_s(DisasContext *ctx, arg_fclass_s *a)
{
    ctx->ir1->opcode = LATR_RV_FCLASS_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_fcvt_bf16_s(DisasContext *ctx, arg_fcvt_bf16_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_d_h(DisasContext *ctx, arg_fcvt_d_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_d_l(DisasContext *ctx, arg_fcvt_d_l *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_D_L;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_d_lu(DisasContext *ctx, arg_fcvt_d_lu *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_D_LU;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_d_s(DisasContext *ctx, arg_fcvt_d_s *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_D_S;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_d_w(DisasContext *ctx, arg_fcvt_d_w *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_D_W;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_d_wu(DisasContext *ctx, arg_fcvt_d_wu *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_D_WU;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_h_d(DisasContext *ctx, arg_fcvt_h_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_h_l(DisasContext *ctx, arg_fcvt_h_l *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_h_lu(DisasContext *ctx, arg_fcvt_h_lu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_h_s(DisasContext *ctx, arg_fcvt_h_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_h_w(DisasContext *ctx, arg_fcvt_h_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_h_wu(DisasContext *ctx, arg_fcvt_h_wu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_l_d(DisasContext *ctx, arg_fcvt_l_d *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_L_D;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_l_h(DisasContext *ctx, arg_fcvt_l_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_l_s(DisasContext *ctx, arg_fcvt_l_s *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_L_S;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_lu_d(DisasContext *ctx, arg_fcvt_lu_d *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_LU_D;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_lu_h(DisasContext *ctx, arg_fcvt_lu_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_lu_s(DisasContext *ctx, arg_fcvt_lu_s *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_LU_S;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_s_bf16(DisasContext *ctx, arg_fcvt_s_bf16 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_s_d(DisasContext *ctx, arg_fcvt_s_d *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_S_D;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_s_h(DisasContext *ctx, arg_fcvt_s_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_s_l(DisasContext *ctx, arg_fcvt_s_l *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_S_L;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_s_lu(DisasContext *ctx, arg_fcvt_s_lu *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_S_LU;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_s_w(DisasContext *ctx, arg_fcvt_s_w *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_S_W;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_s_wu(DisasContext *ctx, arg_fcvt_s_wu *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_S_WU;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_w_d(DisasContext *ctx, arg_fcvt_w_d *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_W_D;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_w_h(DisasContext *ctx, arg_fcvt_w_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_w_s(DisasContext *ctx, arg_fcvt_w_s *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_W_S;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_wu_d(DisasContext *ctx, arg_fcvt_wu_d *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_WU_D;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvt_wu_h(DisasContext *ctx, arg_fcvt_wu_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fcvt_wu_s(DisasContext *ctx, arg_fcvt_wu_s *a)
{
    ctx->ir1->opcode = LATR_RV_FCVT_WU_S;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fcvtmod_w_d(DisasContext *ctx, arg_fcvtmod_w_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fdiv_d(DisasContext *ctx, arg_fdiv_d *a)
{
    ctx->ir1->opcode = LATR_RV_FDIV_D;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fdiv_h(DisasContext *ctx, arg_fdiv_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fdiv_s(DisasContext *ctx, arg_fdiv_s *a)
{
    ctx->ir1->opcode = LATR_RV_FDIV_S;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fence(DisasContext *ctx, arg_fence *a)
{
    ctx->ir1->opcode = LATR_RV_FENCE;
    ctx->ir1->arg.pred = a->pred;
    ctx->ir1->arg.succ = a->succ;
    return true;
}
static bool trans_fence_i(DisasContext *ctx, arg_fence_i *a)
{
    ctx->ir1->opcode = LATR_RV_FENCE_I;
    return true;
}
static bool trans_feq_d(DisasContext *ctx, arg_feq_d *a)
{
    ctx->ir1->opcode = LATR_RV_FEQ_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_feq_h(DisasContext *ctx, arg_feq_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_feq_s(DisasContext *ctx, arg_feq_s *a)
{
    ctx->ir1->opcode = LATR_RV_FEQ_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fld(DisasContext *ctx, arg_fld *a)
{
    ctx->ir1->opcode = LATR_RV_FLD;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fle_d(DisasContext *ctx, arg_fle_d *a)
{
    ctx->ir1->opcode = LATR_RV_FLE_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fle_h(DisasContext *ctx, arg_fle_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fle_s(DisasContext *ctx, arg_fle_s *a)
{
    ctx->ir1->opcode = LATR_RV_FLE_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fleq_d(DisasContext *ctx, arg_fleq_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fleq_h(DisasContext *ctx, arg_fleq_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fleq_s(DisasContext *ctx, arg_fleq_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_flh(DisasContext *ctx, arg_flh *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fli_d(DisasContext *ctx, arg_fli_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fli_h(DisasContext *ctx, arg_fli_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fli_s(DisasContext *ctx, arg_fli_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_flt_d(DisasContext *ctx, arg_flt_d *a)
{
    ctx->ir1->opcode = LATR_RV_FLT_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_flt_h(DisasContext *ctx, arg_flt_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_flt_s(DisasContext *ctx, arg_flt_s *a)
{
    ctx->ir1->opcode = LATR_RV_FLT_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fltq_d(DisasContext *ctx, arg_fltq_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fltq_h(DisasContext *ctx, arg_fltq_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fltq_s(DisasContext *ctx, arg_fltq_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_flw(DisasContext *ctx, arg_flw *a)
{
    ctx->ir1->opcode = LATR_RV_FLW;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fmadd_d(DisasContext *ctx, arg_fmadd_d *a)
{
    ctx->ir1->opcode = LATR_RV_FMADD_D;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fmadd_h(DisasContext *ctx, arg_fmadd_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmadd_s(DisasContext *ctx, arg_fmadd_s *a)
{
    ctx->ir1->opcode = LATR_RV_FMADD_S;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fmax_d(DisasContext *ctx, arg_fmax_d *a)
{
    ctx->ir1->opcode = LATR_RV_FMAX_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fmax_h(DisasContext *ctx, arg_fmax_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmax_s(DisasContext *ctx, arg_fmax_s *a)
{
    ctx->ir1->opcode = LATR_RV_FMAX_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fmaxm_d(DisasContext *ctx, arg_fmaxm_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmaxm_h(DisasContext *ctx, arg_fmaxm_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmaxm_s(DisasContext *ctx, arg_fmaxm_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmin_d(DisasContext *ctx, arg_fmin_d *a)
{
    ctx->ir1->opcode = LATR_RV_FMIN_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fmin_h(DisasContext *ctx, arg_fmin_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmin_s(DisasContext *ctx, arg_fmin_s *a)
{
    ctx->ir1->opcode = LATR_RV_FMIN_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fminm_d(DisasContext *ctx, arg_fminm_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fminm_h(DisasContext *ctx, arg_fminm_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fminm_s(DisasContext *ctx, arg_fminm_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmsub_d(DisasContext *ctx, arg_fmsub_d *a)
{
    ctx->ir1->opcode = LATR_RV_FMSUB_D;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fmsub_h(DisasContext *ctx, arg_fmsub_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmsub_s(DisasContext *ctx, arg_fmsub_s *a)
{
    ctx->ir1->opcode = LATR_RV_FMSUB_S;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fmul_d(DisasContext *ctx, arg_fmul_d *a)
{
    ctx->ir1->opcode = LATR_RV_FMUL_D;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fmul_h(DisasContext *ctx, arg_fmul_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmul_s(DisasContext *ctx, arg_fmul_s *a)
{
    ctx->ir1->opcode = LATR_RV_FMUL_S;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fmv_d_x(DisasContext *ctx, arg_fmv_d_x *a)
{
    ctx->ir1->opcode = LATR_RV_FMV_D_X;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_fmv_h_x(DisasContext *ctx, arg_fmv_h_x *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmv_w_x(DisasContext *ctx, arg_fmv_w_x *a)
{
    ctx->ir1->opcode = LATR_RV_FMV_W_X;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_fmv_x_d(DisasContext *ctx, arg_fmv_x_d *a)
{
    ctx->ir1->opcode = LATR_RV_FMV_X_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_fmv_x_h(DisasContext *ctx, arg_fmv_x_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmv_x_w(DisasContext *ctx, arg_fmv_x_w *a)
{
    ctx->ir1->opcode = LATR_RV_FMV_X_W;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    return true;
}
static bool trans_fmvh_x_d(DisasContext *ctx, arg_fmvh_x_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fmvp_d_x(DisasContext *ctx, arg_fmvp_d_x *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fnmadd_d(DisasContext *ctx, arg_fnmadd_d *a)
{
    ctx->ir1->opcode = LATR_RV_FNMADD_D;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fnmadd_h(DisasContext *ctx, arg_fnmadd_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fnmadd_s(DisasContext *ctx, arg_fnmadd_s *a)
{
    ctx->ir1->opcode = LATR_RV_FNMADD_S;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fnmsub_d(DisasContext *ctx, arg_fnmsub_d *a)
{
    ctx->ir1->opcode = LATR_RV_FNMSUB_D;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fnmsub_h(DisasContext *ctx, arg_fnmsub_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fnmsub_s(DisasContext *ctx, arg_fnmsub_s *a)
{
    ctx->ir1->opcode = LATR_RV_FNMSUB_S;
    ctx->ir1->arg.rs3 = a->rs3;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fround_d(DisasContext *ctx, arg_fround_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fround_h(DisasContext *ctx, arg_fround_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fround_s(DisasContext *ctx, arg_fround_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_froundnx_d(DisasContext *ctx, arg_froundnx_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_froundnx_h(DisasContext *ctx, arg_froundnx_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_froundnx_s(DisasContext *ctx, arg_froundnx_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fsd(DisasContext *ctx, arg_fsd *a)
{
    ctx->ir1->opcode = LATR_RV_FSD;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fsgnj_d(DisasContext *ctx, arg_fsgnj_d *a)
{
    ctx->ir1->opcode = LATR_RV_FSGNJ_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fsgnj_h(DisasContext *ctx, arg_fsgnj_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fsgnj_s(DisasContext *ctx, arg_fsgnj_s *a)
{
    ctx->ir1->opcode = LATR_RV_FSGNJ_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fsgnjn_d(DisasContext *ctx, arg_fsgnjn_d *a)
{
    ctx->ir1->opcode = LATR_RV_FSGNJN_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fsgnjn_h(DisasContext *ctx, arg_fsgnjn_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fsgnjn_s(DisasContext *ctx, arg_fsgnjn_s *a)
{
    ctx->ir1->opcode = LATR_RV_FSGNJN_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fsgnjx_d(DisasContext *ctx, arg_fsgnjx_d *a)
{
    ctx->ir1->opcode = LATR_RV_FSGNJX_D;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fsgnjx_h(DisasContext *ctx, arg_fsgnjx_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fsgnjx_s(DisasContext *ctx, arg_fsgnjx_s *a)
{
    ctx->ir1->opcode = LATR_RV_FSGNJX_S;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_fsh(DisasContext *ctx, arg_fsh *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fsqrt_d(DisasContext *ctx, arg_fsqrt_d *a)
{
    ctx->ir1->opcode = LATR_RV_FSQRT_D;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fsqrt_h(DisasContext *ctx, arg_fsqrt_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fsqrt_s(DisasContext *ctx, arg_fsqrt_s *a)
{
    ctx->ir1->opcode = LATR_RV_FSQRT_S;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fsub_d(DisasContext *ctx, arg_fsub_d *a)
{
    ctx->ir1->opcode = LATR_RV_FSUB_D;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fsub_h(DisasContext *ctx, arg_fsub_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_fsub_s(DisasContext *ctx, arg_fsub_s *a)
{
    ctx->ir1->opcode = LATR_RV_FSUB_S;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rm = a->rm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_fsw(DisasContext *ctx, arg_fsw *a)
{
    ctx->ir1->opcode = LATR_RV_FSW;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_hfence_gvma(DisasContext *ctx, arg_hfence_gvma *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hfence_vvma(DisasContext *ctx, arg_hfence_vvma *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hinval_gvma(DisasContext *ctx, arg_hinval_gvma *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hinval_vvma(DisasContext *ctx, arg_hinval_vvma *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlv_b(DisasContext *ctx, arg_hlv_b *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlv_bu(DisasContext *ctx, arg_hlv_bu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlv_d(DisasContext *ctx, arg_hlv_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlv_h(DisasContext *ctx, arg_hlv_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlv_hu(DisasContext *ctx, arg_hlv_hu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlv_w(DisasContext *ctx, arg_hlv_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlv_wu(DisasContext *ctx, arg_hlv_wu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlvx_hu(DisasContext *ctx, arg_hlvx_hu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hlvx_wu(DisasContext *ctx, arg_hlvx_wu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hsv_b(DisasContext *ctx, arg_hsv_b *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hsv_d(DisasContext *ctx, arg_hsv_d *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hsv_h(DisasContext *ctx, arg_hsv_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_hsv_w(DisasContext *ctx, arg_hsv_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_illegal(DisasContext *ctx, arg_illegal *a)
{
    (void)a;
    ctx->ir1->opcode = LATR_RV_ILLEGAL;
    return true;
}
static bool trans_jal(DisasContext *ctx, arg_jal *a)
{
    ctx->ir1->opcode = LATR_RV_JAL;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_jalr(DisasContext *ctx, arg_jalr *a)
{
    ctx->ir1->opcode = LATR_RV_JALR;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lb(DisasContext *ctx, arg_lb *a)
{
    ctx->ir1->opcode = LATR_RV_LB;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lbu(DisasContext *ctx, arg_lbu *a)
{
    ctx->ir1->opcode = LATR_RV_LBU;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_ld(DisasContext *ctx, arg_ld *a)
{
    ctx->ir1->opcode = LATR_RV_LD;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_ldu(DisasContext *ctx, arg_ldu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_lh(DisasContext *ctx, arg_lh *a)
{
    ctx->ir1->opcode = LATR_RV_LH;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lhu(DisasContext *ctx, arg_lhu *a)
{
    ctx->ir1->opcode = LATR_RV_LHU;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lq(DisasContext *ctx, arg_lq *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_lr_d(DisasContext *ctx, arg_lr_d *a)
{
    ctx->ir1->opcode = LATR_RV_LR_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lr_w(DisasContext *ctx, arg_lr_w *a)
{
    ctx->ir1->opcode = LATR_RV_LR_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lui(DisasContext *ctx, arg_lui *a)
{
    ctx->ir1->opcode = LATR_RV_LUI;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lw(DisasContext *ctx, arg_lw *a)
{
    ctx->ir1->opcode = LATR_RV_LW;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_lwu(DisasContext *ctx, arg_lwu *a)
{
    ctx->ir1->opcode = LATR_RV_LWU;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_max(DisasContext *ctx, arg_max *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_maxu(DisasContext *ctx, arg_maxu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_min(DisasContext *ctx, arg_min *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_minu(DisasContext *ctx, arg_minu *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_mret(DisasContext *ctx, arg_mret *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_mul(DisasContext *ctx, arg_mul *a)
{
    ctx->ir1->opcode = LATR_RV_MUL;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_muld(DisasContext *ctx, arg_muld *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_mulh(DisasContext *ctx, arg_mulh *a)
{
    ctx->ir1->opcode = LATR_RV_MULH;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_mulhsu(DisasContext *ctx, arg_mulhsu *a)
{
    ctx->ir1->opcode = LATR_RV_MULHSU;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_mulhu(DisasContext *ctx, arg_mulhu *a)
{
    ctx->ir1->opcode = LATR_RV_MULHU;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_mulw(DisasContext *ctx, arg_mulw *a)
{
    ctx->ir1->opcode = LATR_RV_MULW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_or(DisasContext *ctx, arg_or *a)
{
    ctx->ir1->opcode = LATR_RV_OR;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_orc_b(DisasContext *ctx, arg_orc_b *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_ori(DisasContext *ctx, arg_ori *a)
{
    ctx->ir1->opcode = LATR_RV_ORI;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_orn(DisasContext *ctx, arg_orn *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_pack(DisasContext *ctx, arg_pack *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_packh(DisasContext *ctx, arg_packh *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_packw(DisasContext *ctx, arg_packw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_pause(DisasContext *ctx, arg_pause *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_rem(DisasContext *ctx, arg_rem *a)
{
    ctx->ir1->opcode = LATR_RV_REM;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_remd(DisasContext *ctx, arg_remd *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_remu(DisasContext *ctx, arg_remu *a)
{
    ctx->ir1->opcode = LATR_RV_REMU;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_remud(DisasContext *ctx, arg_remud *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_remuw(DisasContext *ctx, arg_remuw *a)
{
    ctx->ir1->opcode = LATR_RV_REMUW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_remw(DisasContext *ctx, arg_remw *a)
{
    ctx->ir1->opcode = LATR_RV_REMW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_rev8_32(DisasContext *ctx, arg_rev8_32 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_rev8_64(DisasContext *ctx, arg_rev8_64 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_rol(DisasContext *ctx, arg_rol *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_rolw(DisasContext *ctx, arg_rolw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_ror(DisasContext *ctx, arg_ror *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_rori(DisasContext *ctx, arg_rori *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_roriw(DisasContext *ctx, arg_roriw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_rorw(DisasContext *ctx, arg_rorw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sb(DisasContext *ctx, arg_sb *a)
{
    ctx->ir1->opcode = LATR_RV_SB;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_sc_d(DisasContext *ctx, arg_sc_d *a)
{
    ctx->ir1->opcode = LATR_RV_SC_D;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_sc_w(DisasContext *ctx, arg_sc_w *a)
{
    ctx->ir1->opcode = LATR_RV_SC_W;
    ctx->ir1->arg.aq = a->aq;
    ctx->ir1->arg.rl = a->rl;
    ctx->ir1->arg.rs2 = a->rs2;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_sd(DisasContext *ctx, arg_sd *a)
{
    ctx->ir1->opcode = LATR_RV_SD;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_sext_b(DisasContext *ctx, arg_sext_b *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sext_h(DisasContext *ctx, arg_sext_h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sfence_inval_ir(DisasContext *ctx, arg_sfence_inval_ir *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sfence_vm(DisasContext *ctx, arg_sfence_vm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sfence_vma(DisasContext *ctx, arg_sfence_vma *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sfence_w_inval(DisasContext *ctx, arg_sfence_w_inval *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sh(DisasContext *ctx, arg_sh *a)
{
    ctx->ir1->opcode = LATR_RV_SH;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_sh1add(DisasContext *ctx, arg_sh1add *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sh1add_uw(DisasContext *ctx, arg_sh1add_uw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sh2add(DisasContext *ctx, arg_sh2add *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sh2add_uw(DisasContext *ctx, arg_sh2add_uw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sh3add(DisasContext *ctx, arg_sh3add *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sh3add_uw(DisasContext *ctx, arg_sh3add_uw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha256sig0(DisasContext *ctx, arg_sha256sig0 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha256sig1(DisasContext *ctx, arg_sha256sig1 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha256sum0(DisasContext *ctx, arg_sha256sum0 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha256sum1(DisasContext *ctx, arg_sha256sum1 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sig0(DisasContext *ctx, arg_sha512sig0 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sig0h(DisasContext *ctx, arg_sha512sig0h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sig0l(DisasContext *ctx, arg_sha512sig0l *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sig1(DisasContext *ctx, arg_sha512sig1 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sig1h(DisasContext *ctx, arg_sha512sig1h *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sig1l(DisasContext *ctx, arg_sha512sig1l *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sum0(DisasContext *ctx, arg_sha512sum0 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sum0r(DisasContext *ctx, arg_sha512sum0r *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sum1(DisasContext *ctx, arg_sha512sum1 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sha512sum1r(DisasContext *ctx, arg_sha512sum1r *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sinval_vma(DisasContext *ctx, arg_sinval_vma *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sll(DisasContext *ctx, arg_sll *a)
{
    ctx->ir1->opcode = LATR_RV_SLL;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_slld(DisasContext *ctx, arg_slld *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_slli(DisasContext *ctx, arg_slli *a)
{
    ctx->ir1->opcode = LATR_RV_SLLI;
    ctx->ir1->arg.shamt = a->shamt;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_slli_uw(DisasContext *ctx, arg_slli_uw *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sllid(DisasContext *ctx, arg_sllid *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_slliw(DisasContext *ctx, arg_slliw *a)
{
    ctx->ir1->opcode = LATR_RV_SLLIW;
    ctx->ir1->arg.shamt = a->shamt;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_sllw(DisasContext *ctx, arg_sllw *a)
{
    ctx->ir1->opcode = LATR_RV_SLLW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_slt(DisasContext *ctx, arg_slt *a)
{
    ctx->ir1->opcode = LATR_RV_SLT;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_slti(DisasContext *ctx, arg_slti *a)
{
    ctx->ir1->opcode = LATR_RV_SLTI;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_sltiu(DisasContext *ctx, arg_sltiu *a)
{
    ctx->ir1->opcode = LATR_RV_SLTIU;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_sltu(DisasContext *ctx, arg_sltu *a)
{
    ctx->ir1->opcode = LATR_RV_SLTU;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_sm3p0(DisasContext *ctx, arg_sm3p0 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sm3p1(DisasContext *ctx, arg_sm3p1 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sm4ed(DisasContext *ctx, arg_sm4ed *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sm4ks(DisasContext *ctx, arg_sm4ks *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sq(DisasContext *ctx, arg_sq *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sra(DisasContext *ctx, arg_sra *a)
{
    ctx->ir1->opcode = LATR_RV_SRA;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_srad(DisasContext *ctx, arg_srad *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_srai(DisasContext *ctx, arg_srai *a)
{
    ctx->ir1->opcode = LATR_RV_SRAI;
    ctx->ir1->arg.shamt = a->shamt;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_sraid(DisasContext *ctx, arg_sraid *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_sraiw(DisasContext *ctx, arg_sraiw *a)
{
    ctx->ir1->opcode = LATR_RV_SRAIW;
    ctx->ir1->arg.shamt = a->shamt;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_sraw(DisasContext *ctx, arg_sraw *a)
{
    ctx->ir1->opcode = LATR_RV_SRAW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_sret(DisasContext *ctx, arg_sret *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_srl(DisasContext *ctx, arg_srl *a)
{
    ctx->ir1->opcode = LATR_RV_SRL;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_srld(DisasContext *ctx, arg_srld *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_srli(DisasContext *ctx, arg_srli *a)
{
    ctx->ir1->opcode = LATR_RV_SRLI;
    ctx->ir1->arg.shamt = a->shamt;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_srlid(DisasContext *ctx, arg_srlid *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_srliw(DisasContext *ctx, arg_srliw *a)
{
    ctx->ir1->opcode = LATR_RV_SRLIW;
    ctx->ir1->arg.shamt = a->shamt;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_srlw(DisasContext *ctx, arg_srlw *a)
{
    ctx->ir1->opcode = LATR_RV_SRLW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_sub(DisasContext *ctx, arg_sub *a)
{
    ctx->ir1->opcode = LATR_RV_SUB;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_subd(DisasContext *ctx, arg_subd *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_subw(DisasContext *ctx, arg_subw *a)
{
    ctx->ir1->opcode = LATR_RV_SUBW;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_sw(DisasContext *ctx, arg_sw *a)
{
    ctx->ir1->opcode = LATR_RV_SW;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_unzip(DisasContext *ctx, arg_unzip *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_uret(DisasContext *ctx, arg_uret *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaadd_vv(DisasContext *ctx, arg_vaadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaadd_vx(DisasContext *ctx, arg_vaadd_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaaddu_vv(DisasContext *ctx, arg_vaaddu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaaddu_vx(DisasContext *ctx, arg_vaaddu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vadc_vim(DisasContext *ctx, arg_vadc_vim *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vadc_vvm(DisasContext *ctx, arg_vadc_vvm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vadc_vxm(DisasContext *ctx, arg_vadc_vxm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vadd_vi(DisasContext *ctx, arg_vadd_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vadd_vv(DisasContext *ctx, arg_vadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vadd_vx(DisasContext *ctx, arg_vadd_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesdf_vs(DisasContext *ctx, arg_vaesdf_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesdf_vv(DisasContext *ctx, arg_vaesdf_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesdm_vs(DisasContext *ctx, arg_vaesdm_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesdm_vv(DisasContext *ctx, arg_vaesdm_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesef_vs(DisasContext *ctx, arg_vaesef_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesef_vv(DisasContext *ctx, arg_vaesef_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesem_vs(DisasContext *ctx, arg_vaesem_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesem_vv(DisasContext *ctx, arg_vaesem_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaeskf1_vi(DisasContext *ctx, arg_vaeskf1_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaeskf2_vi(DisasContext *ctx, arg_vaeskf2_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vaesz_vs(DisasContext *ctx, arg_vaesz_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vand_vi(DisasContext *ctx, arg_vand_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vand_vv(DisasContext *ctx, arg_vand_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vand_vx(DisasContext *ctx, arg_vand_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vandn_vv(DisasContext *ctx, arg_vandn_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vandn_vx(DisasContext *ctx, arg_vandn_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vasub_vv(DisasContext *ctx, arg_vasub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vasub_vx(DisasContext *ctx, arg_vasub_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vasubu_vv(DisasContext *ctx, arg_vasubu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vasubu_vx(DisasContext *ctx, arg_vasubu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vbrev8_v(DisasContext *ctx, arg_vbrev8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vbrev_v(DisasContext *ctx, arg_vbrev_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vclmul_vv(DisasContext *ctx, arg_vclmul_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vclmul_vx(DisasContext *ctx, arg_vclmul_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vclmulh_vv(DisasContext *ctx, arg_vclmulh_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vclmulh_vx(DisasContext *ctx, arg_vclmulh_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vclz_v(DisasContext *ctx, arg_vclz_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vcompress_vm(DisasContext *ctx, arg_vcompress_vm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vcpop_m(DisasContext *ctx, arg_vcpop_m *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vcpop_v(DisasContext *ctx, arg_vcpop_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vctz_v(DisasContext *ctx, arg_vctz_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vdiv_vv(DisasContext *ctx, arg_vdiv_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vdiv_vx(DisasContext *ctx, arg_vdiv_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vdivu_vv(DisasContext *ctx, arg_vdivu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vdivu_vx(DisasContext *ctx, arg_vdivu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfadd_vf(DisasContext *ctx, arg_vfadd_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfadd_vv(DisasContext *ctx, arg_vfadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfclass_v(DisasContext *ctx, arg_vfclass_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfcvt_f_x_v(DisasContext *ctx, arg_vfcvt_f_x_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfcvt_f_xu_v(DisasContext *ctx, arg_vfcvt_f_xu_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfcvt_rtz_x_f_v(DisasContext *ctx, arg_vfcvt_rtz_x_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfcvt_rtz_xu_f_v(DisasContext *ctx, arg_vfcvt_rtz_xu_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfcvt_x_f_v(DisasContext *ctx, arg_vfcvt_x_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfcvt_xu_f_v(DisasContext *ctx, arg_vfcvt_xu_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfdiv_vf(DisasContext *ctx, arg_vfdiv_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfdiv_vv(DisasContext *ctx, arg_vfdiv_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfirst_m(DisasContext *ctx, arg_vfirst_m *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmacc_vf(DisasContext *ctx, arg_vfmacc_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmacc_vv(DisasContext *ctx, arg_vfmacc_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmadd_vf(DisasContext *ctx, arg_vfmadd_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmadd_vv(DisasContext *ctx, arg_vfmadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmax_vf(DisasContext *ctx, arg_vfmax_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmax_vv(DisasContext *ctx, arg_vfmax_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmerge_vfm(DisasContext *ctx, arg_vfmerge_vfm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmin_vf(DisasContext *ctx, arg_vfmin_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmin_vv(DisasContext *ctx, arg_vfmin_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmsac_vf(DisasContext *ctx, arg_vfmsac_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmsac_vv(DisasContext *ctx, arg_vfmsac_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmsub_vf(DisasContext *ctx, arg_vfmsub_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmsub_vv(DisasContext *ctx, arg_vfmsub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmul_vf(DisasContext *ctx, arg_vfmul_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmul_vv(DisasContext *ctx, arg_vfmul_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmv_f_s(DisasContext *ctx, arg_vfmv_f_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmv_s_f(DisasContext *ctx, arg_vfmv_s_f *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfmv_v_f(DisasContext *ctx, arg_vfmv_v_f *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_f_f_w(DisasContext *ctx, arg_vfncvt_f_f_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_f_x_w(DisasContext *ctx, arg_vfncvt_f_x_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_f_xu_w(DisasContext *ctx, arg_vfncvt_f_xu_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_rod_f_f_w(DisasContext *ctx, arg_vfncvt_rod_f_f_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_rtz_x_f_w(DisasContext *ctx, arg_vfncvt_rtz_x_f_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_rtz_xu_f_w(DisasContext *ctx, arg_vfncvt_rtz_xu_f_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_x_f_w(DisasContext *ctx, arg_vfncvt_x_f_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvt_xu_f_w(DisasContext *ctx, arg_vfncvt_xu_f_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfncvtbf16_f_f_w(DisasContext *ctx, arg_vfncvtbf16_f_f_w *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmacc_vf(DisasContext *ctx, arg_vfnmacc_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmacc_vv(DisasContext *ctx, arg_vfnmacc_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmadd_vf(DisasContext *ctx, arg_vfnmadd_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmadd_vv(DisasContext *ctx, arg_vfnmadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmsac_vf(DisasContext *ctx, arg_vfnmsac_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmsac_vv(DisasContext *ctx, arg_vfnmsac_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmsub_vf(DisasContext *ctx, arg_vfnmsub_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfnmsub_vv(DisasContext *ctx, arg_vfnmsub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfrdiv_vf(DisasContext *ctx, arg_vfrdiv_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfrec7_v(DisasContext *ctx, arg_vfrec7_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfredmax_vs(DisasContext *ctx, arg_vfredmax_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfredmin_vs(DisasContext *ctx, arg_vfredmin_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfredosum_vs(DisasContext *ctx, arg_vfredosum_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfredusum_vs(DisasContext *ctx, arg_vfredusum_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfrsqrt7_v(DisasContext *ctx, arg_vfrsqrt7_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfrsub_vf(DisasContext *ctx, arg_vfrsub_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsgnj_vf(DisasContext *ctx, arg_vfsgnj_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsgnj_vv(DisasContext *ctx, arg_vfsgnj_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsgnjn_vf(DisasContext *ctx, arg_vfsgnjn_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsgnjn_vv(DisasContext *ctx, arg_vfsgnjn_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsgnjx_vf(DisasContext *ctx, arg_vfsgnjx_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsgnjx_vv(DisasContext *ctx, arg_vfsgnjx_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfslide1down_vf(DisasContext *ctx, arg_vfslide1down_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfslide1up_vf(DisasContext *ctx, arg_vfslide1up_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsqrt_v(DisasContext *ctx, arg_vfsqrt_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsub_vf(DisasContext *ctx, arg_vfsub_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfsub_vv(DisasContext *ctx, arg_vfsub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwadd_vf(DisasContext *ctx, arg_vfwadd_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwadd_vv(DisasContext *ctx, arg_vfwadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwadd_wf(DisasContext *ctx, arg_vfwadd_wf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwadd_wv(DisasContext *ctx, arg_vfwadd_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvt_f_f_v(DisasContext *ctx, arg_vfwcvt_f_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvt_f_x_v(DisasContext *ctx, arg_vfwcvt_f_x_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvt_f_xu_v(DisasContext *ctx, arg_vfwcvt_f_xu_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvt_rtz_x_f_v(DisasContext *ctx, arg_vfwcvt_rtz_x_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvt_rtz_xu_f_v(DisasContext *ctx, arg_vfwcvt_rtz_xu_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvt_x_f_v(DisasContext *ctx, arg_vfwcvt_x_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvt_xu_f_v(DisasContext *ctx, arg_vfwcvt_xu_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwcvtbf16_f_f_v(DisasContext *ctx, arg_vfwcvtbf16_f_f_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmacc_vf(DisasContext *ctx, arg_vfwmacc_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmacc_vv(DisasContext *ctx, arg_vfwmacc_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmaccbf16_vf(DisasContext *ctx, arg_vfwmaccbf16_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmaccbf16_vv(DisasContext *ctx, arg_vfwmaccbf16_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmsac_vf(DisasContext *ctx, arg_vfwmsac_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmsac_vv(DisasContext *ctx, arg_vfwmsac_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmul_vf(DisasContext *ctx, arg_vfwmul_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwmul_vv(DisasContext *ctx, arg_vfwmul_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwnmacc_vf(DisasContext *ctx, arg_vfwnmacc_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwnmacc_vv(DisasContext *ctx, arg_vfwnmacc_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwnmsac_vf(DisasContext *ctx, arg_vfwnmsac_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwnmsac_vv(DisasContext *ctx, arg_vfwnmsac_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwredosum_vs(DisasContext *ctx, arg_vfwredosum_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwredusum_vs(DisasContext *ctx, arg_vfwredusum_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwsub_vf(DisasContext *ctx, arg_vfwsub_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwsub_vv(DisasContext *ctx, arg_vfwsub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwsub_wf(DisasContext *ctx, arg_vfwsub_wf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vfwsub_wv(DisasContext *ctx, arg_vfwsub_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vghsh_vv(DisasContext *ctx, arg_vghsh_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vgmul_vv(DisasContext *ctx, arg_vgmul_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vid_v(DisasContext *ctx, arg_vid_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_viota_m(DisasContext *ctx, arg_viota_m *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl1re16_v(DisasContext *ctx, arg_vl1re16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl1re32_v(DisasContext *ctx, arg_vl1re32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl1re64_v(DisasContext *ctx, arg_vl1re64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl1re8_v(DisasContext *ctx, arg_vl1re8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl2re16_v(DisasContext *ctx, arg_vl2re16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl2re32_v(DisasContext *ctx, arg_vl2re32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl2re64_v(DisasContext *ctx, arg_vl2re64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl2re8_v(DisasContext *ctx, arg_vl2re8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl4re16_v(DisasContext *ctx, arg_vl4re16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl4re32_v(DisasContext *ctx, arg_vl4re32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl4re64_v(DisasContext *ctx, arg_vl4re64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl4re8_v(DisasContext *ctx, arg_vl4re8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl8re16_v(DisasContext *ctx, arg_vl8re16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl8re32_v(DisasContext *ctx, arg_vl8re32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl8re64_v(DisasContext *ctx, arg_vl8re64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vl8re8_v(DisasContext *ctx, arg_vl8re8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle16_v(DisasContext *ctx, arg_vle16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle16ff_v(DisasContext *ctx, arg_vle16ff_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle32_v(DisasContext *ctx, arg_vle32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle32ff_v(DisasContext *ctx, arg_vle32ff_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle64_v(DisasContext *ctx, arg_vle64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle64ff_v(DisasContext *ctx, arg_vle64ff_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle8_v(DisasContext *ctx, arg_vle8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vle8ff_v(DisasContext *ctx, arg_vle8ff_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlm_v(DisasContext *ctx, arg_vlm_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlse16_v(DisasContext *ctx, arg_vlse16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlse32_v(DisasContext *ctx, arg_vlse32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlse64_v(DisasContext *ctx, arg_vlse64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlse8_v(DisasContext *ctx, arg_vlse8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlxei16_v(DisasContext *ctx, arg_vlxei16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlxei32_v(DisasContext *ctx, arg_vlxei32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlxei64_v(DisasContext *ctx, arg_vlxei64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vlxei8_v(DisasContext *ctx, arg_vlxei8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmacc_vv(DisasContext *ctx, arg_vmacc_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmacc_vx(DisasContext *ctx, arg_vmacc_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmadc_vim(DisasContext *ctx, arg_vmadc_vim *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmadc_vvm(DisasContext *ctx, arg_vmadc_vvm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmadc_vxm(DisasContext *ctx, arg_vmadc_vxm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmadd_vv(DisasContext *ctx, arg_vmadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmadd_vx(DisasContext *ctx, arg_vmadd_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmand_mm(DisasContext *ctx, arg_vmand_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmandn_mm(DisasContext *ctx, arg_vmandn_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmax_vv(DisasContext *ctx, arg_vmax_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmax_vx(DisasContext *ctx, arg_vmax_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmaxu_vv(DisasContext *ctx, arg_vmaxu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmaxu_vx(DisasContext *ctx, arg_vmaxu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmerge_vim(DisasContext *ctx, arg_vmerge_vim *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmerge_vvm(DisasContext *ctx, arg_vmerge_vvm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmerge_vxm(DisasContext *ctx, arg_vmerge_vxm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfeq_vf(DisasContext *ctx, arg_vmfeq_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfeq_vv(DisasContext *ctx, arg_vmfeq_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfge_vf(DisasContext *ctx, arg_vmfge_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfgt_vf(DisasContext *ctx, arg_vmfgt_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfle_vf(DisasContext *ctx, arg_vmfle_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfle_vv(DisasContext *ctx, arg_vmfle_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmflt_vf(DisasContext *ctx, arg_vmflt_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmflt_vv(DisasContext *ctx, arg_vmflt_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfne_vf(DisasContext *ctx, arg_vmfne_vf *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmfne_vv(DisasContext *ctx, arg_vmfne_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmin_vv(DisasContext *ctx, arg_vmin_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmin_vx(DisasContext *ctx, arg_vmin_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vminu_vv(DisasContext *ctx, arg_vminu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vminu_vx(DisasContext *ctx, arg_vminu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmnand_mm(DisasContext *ctx, arg_vmnand_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmnor_mm(DisasContext *ctx, arg_vmnor_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmor_mm(DisasContext *ctx, arg_vmor_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmorn_mm(DisasContext *ctx, arg_vmorn_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsbc_vvm(DisasContext *ctx, arg_vmsbc_vvm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsbc_vxm(DisasContext *ctx, arg_vmsbc_vxm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsbf_m(DisasContext *ctx, arg_vmsbf_m *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmseq_vi(DisasContext *ctx, arg_vmseq_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmseq_vv(DisasContext *ctx, arg_vmseq_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmseq_vx(DisasContext *ctx, arg_vmseq_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsgt_vi(DisasContext *ctx, arg_vmsgt_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsgt_vx(DisasContext *ctx, arg_vmsgt_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsgtu_vi(DisasContext *ctx, arg_vmsgtu_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsgtu_vx(DisasContext *ctx, arg_vmsgtu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsif_m(DisasContext *ctx, arg_vmsif_m *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsle_vi(DisasContext *ctx, arg_vmsle_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsle_vv(DisasContext *ctx, arg_vmsle_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsle_vx(DisasContext *ctx, arg_vmsle_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsleu_vi(DisasContext *ctx, arg_vmsleu_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsleu_vv(DisasContext *ctx, arg_vmsleu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsleu_vx(DisasContext *ctx, arg_vmsleu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmslt_vv(DisasContext *ctx, arg_vmslt_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmslt_vx(DisasContext *ctx, arg_vmslt_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsltu_vv(DisasContext *ctx, arg_vmsltu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsltu_vx(DisasContext *ctx, arg_vmsltu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsne_vi(DisasContext *ctx, arg_vmsne_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsne_vv(DisasContext *ctx, arg_vmsne_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsne_vx(DisasContext *ctx, arg_vmsne_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmsof_m(DisasContext *ctx, arg_vmsof_m *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmul_vv(DisasContext *ctx, arg_vmul_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmul_vx(DisasContext *ctx, arg_vmul_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmulh_vv(DisasContext *ctx, arg_vmulh_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmulh_vx(DisasContext *ctx, arg_vmulh_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmulhsu_vv(DisasContext *ctx, arg_vmulhsu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmulhsu_vx(DisasContext *ctx, arg_vmulhsu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmulhu_vv(DisasContext *ctx, arg_vmulhu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmulhu_vx(DisasContext *ctx, arg_vmulhu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv1r_v(DisasContext *ctx, arg_vmv1r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv2r_v(DisasContext *ctx, arg_vmv2r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv4r_v(DisasContext *ctx, arg_vmv4r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv8r_v(DisasContext *ctx, arg_vmv8r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv_s_x(DisasContext *ctx, arg_vmv_s_x *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv_v_i(DisasContext *ctx, arg_vmv_v_i *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv_v_v(DisasContext *ctx, arg_vmv_v_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv_v_x(DisasContext *ctx, arg_vmv_v_x *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmv_x_s(DisasContext *ctx, arg_vmv_x_s *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmxnor_mm(DisasContext *ctx, arg_vmxnor_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vmxor_mm(DisasContext *ctx, arg_vmxor_mm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnclip_wi(DisasContext *ctx, arg_vnclip_wi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnclip_wv(DisasContext *ctx, arg_vnclip_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnclip_wx(DisasContext *ctx, arg_vnclip_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnclipu_wi(DisasContext *ctx, arg_vnclipu_wi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnclipu_wv(DisasContext *ctx, arg_vnclipu_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnclipu_wx(DisasContext *ctx, arg_vnclipu_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnmsac_vv(DisasContext *ctx, arg_vnmsac_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnmsac_vx(DisasContext *ctx, arg_vnmsac_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnmsub_vv(DisasContext *ctx, arg_vnmsub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnmsub_vx(DisasContext *ctx, arg_vnmsub_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnsra_wi(DisasContext *ctx, arg_vnsra_wi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnsra_wv(DisasContext *ctx, arg_vnsra_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnsra_wx(DisasContext *ctx, arg_vnsra_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnsrl_wi(DisasContext *ctx, arg_vnsrl_wi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnsrl_wv(DisasContext *ctx, arg_vnsrl_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vnsrl_wx(DisasContext *ctx, arg_vnsrl_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vor_vi(DisasContext *ctx, arg_vor_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vor_vv(DisasContext *ctx, arg_vor_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vor_vx(DisasContext *ctx, arg_vor_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredand_vs(DisasContext *ctx, arg_vredand_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredmax_vs(DisasContext *ctx, arg_vredmax_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredmaxu_vs(DisasContext *ctx, arg_vredmaxu_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredmin_vs(DisasContext *ctx, arg_vredmin_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredminu_vs(DisasContext *ctx, arg_vredminu_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredor_vs(DisasContext *ctx, arg_vredor_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredsum_vs(DisasContext *ctx, arg_vredsum_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vredxor_vs(DisasContext *ctx, arg_vredxor_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrem_vv(DisasContext *ctx, arg_vrem_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrem_vx(DisasContext *ctx, arg_vrem_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vremu_vv(DisasContext *ctx, arg_vremu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vremu_vx(DisasContext *ctx, arg_vremu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrev8_v(DisasContext *ctx, arg_vrev8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrgather_vi(DisasContext *ctx, arg_vrgather_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrgather_vv(DisasContext *ctx, arg_vrgather_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrgather_vx(DisasContext *ctx, arg_vrgather_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrgatherei16_vv(DisasContext *ctx, arg_vrgatherei16_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrol_vv(DisasContext *ctx, arg_vrol_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrol_vx(DisasContext *ctx, arg_vrol_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vror_vi(DisasContext *ctx, arg_vror_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vror_vv(DisasContext *ctx, arg_vror_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vror_vx(DisasContext *ctx, arg_vror_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrsub_vi(DisasContext *ctx, arg_vrsub_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vrsub_vx(DisasContext *ctx, arg_vrsub_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vs1r_v(DisasContext *ctx, arg_vs1r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vs2r_v(DisasContext *ctx, arg_vs2r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vs4r_v(DisasContext *ctx, arg_vs4r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vs8r_v(DisasContext *ctx, arg_vs8r_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsadd_vi(DisasContext *ctx, arg_vsadd_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsadd_vv(DisasContext *ctx, arg_vsadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsadd_vx(DisasContext *ctx, arg_vsadd_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsaddu_vi(DisasContext *ctx, arg_vsaddu_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsaddu_vv(DisasContext *ctx, arg_vsaddu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsaddu_vx(DisasContext *ctx, arg_vsaddu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsbc_vvm(DisasContext *ctx, arg_vsbc_vvm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsbc_vxm(DisasContext *ctx, arg_vsbc_vxm *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vse16_v(DisasContext *ctx, arg_vse16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vse32_v(DisasContext *ctx, arg_vse32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vse64_v(DisasContext *ctx, arg_vse64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vse8_v(DisasContext *ctx, arg_vse8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsetivli(DisasContext *ctx, arg_vsetivli *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsetvl(DisasContext *ctx, arg_vsetvl *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsetvli(DisasContext *ctx, arg_vsetvli *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsext_vf2(DisasContext *ctx, arg_vsext_vf2 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsext_vf4(DisasContext *ctx, arg_vsext_vf4 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsext_vf8(DisasContext *ctx, arg_vsext_vf8 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsha2ch_vv(DisasContext *ctx, arg_vsha2ch_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsha2cl_vv(DisasContext *ctx, arg_vsha2cl_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsha2ms_vv(DisasContext *ctx, arg_vsha2ms_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vslide1down_vx(DisasContext *ctx, arg_vslide1down_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vslide1up_vx(DisasContext *ctx, arg_vslide1up_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vslidedown_vi(DisasContext *ctx, arg_vslidedown_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vslidedown_vx(DisasContext *ctx, arg_vslidedown_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vslideup_vi(DisasContext *ctx, arg_vslideup_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vslideup_vx(DisasContext *ctx, arg_vslideup_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsll_vi(DisasContext *ctx, arg_vsll_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsll_vv(DisasContext *ctx, arg_vsll_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsll_vx(DisasContext *ctx, arg_vsll_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsm3c_vi(DisasContext *ctx, arg_vsm3c_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsm3me_vv(DisasContext *ctx, arg_vsm3me_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsm4k_vi(DisasContext *ctx, arg_vsm4k_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsm4r_vs(DisasContext *ctx, arg_vsm4r_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsm4r_vv(DisasContext *ctx, arg_vsm4r_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsm_v(DisasContext *ctx, arg_vsm_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsmul_vv(DisasContext *ctx, arg_vsmul_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsmul_vx(DisasContext *ctx, arg_vsmul_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsra_vi(DisasContext *ctx, arg_vsra_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsra_vv(DisasContext *ctx, arg_vsra_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsra_vx(DisasContext *ctx, arg_vsra_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsrl_vi(DisasContext *ctx, arg_vsrl_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsrl_vv(DisasContext *ctx, arg_vsrl_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsrl_vx(DisasContext *ctx, arg_vsrl_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsse16_v(DisasContext *ctx, arg_vsse16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsse32_v(DisasContext *ctx, arg_vsse32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsse64_v(DisasContext *ctx, arg_vsse64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsse8_v(DisasContext *ctx, arg_vsse8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssra_vi(DisasContext *ctx, arg_vssra_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssra_vv(DisasContext *ctx, arg_vssra_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssra_vx(DisasContext *ctx, arg_vssra_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssrl_vi(DisasContext *ctx, arg_vssrl_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssrl_vv(DisasContext *ctx, arg_vssrl_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssrl_vx(DisasContext *ctx, arg_vssrl_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssub_vv(DisasContext *ctx, arg_vssub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssub_vx(DisasContext *ctx, arg_vssub_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssubu_vv(DisasContext *ctx, arg_vssubu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vssubu_vx(DisasContext *ctx, arg_vssubu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsub_vv(DisasContext *ctx, arg_vsub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsub_vx(DisasContext *ctx, arg_vsub_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsxei16_v(DisasContext *ctx, arg_vsxei16_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsxei32_v(DisasContext *ctx, arg_vsxei32_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsxei64_v(DisasContext *ctx, arg_vsxei64_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vsxei8_v(DisasContext *ctx, arg_vsxei8_v *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwadd_vv(DisasContext *ctx, arg_vwadd_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwadd_vx(DisasContext *ctx, arg_vwadd_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwadd_wv(DisasContext *ctx, arg_vwadd_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwadd_wx(DisasContext *ctx, arg_vwadd_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwaddu_vv(DisasContext *ctx, arg_vwaddu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwaddu_vx(DisasContext *ctx, arg_vwaddu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwaddu_wv(DisasContext *ctx, arg_vwaddu_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwaddu_wx(DisasContext *ctx, arg_vwaddu_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmacc_vv(DisasContext *ctx, arg_vwmacc_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmacc_vx(DisasContext *ctx, arg_vwmacc_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmaccsu_vv(DisasContext *ctx, arg_vwmaccsu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmaccsu_vx(DisasContext *ctx, arg_vwmaccsu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmaccu_vv(DisasContext *ctx, arg_vwmaccu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmaccu_vx(DisasContext *ctx, arg_vwmaccu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmaccus_vx(DisasContext *ctx, arg_vwmaccus_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmul_vv(DisasContext *ctx, arg_vwmul_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmul_vx(DisasContext *ctx, arg_vwmul_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmulsu_vv(DisasContext *ctx, arg_vwmulsu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmulsu_vx(DisasContext *ctx, arg_vwmulsu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmulu_vv(DisasContext *ctx, arg_vwmulu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwmulu_vx(DisasContext *ctx, arg_vwmulu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwredsum_vs(DisasContext *ctx, arg_vwredsum_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwredsumu_vs(DisasContext *ctx, arg_vwredsumu_vs *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsll_vi(DisasContext *ctx, arg_vwsll_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsll_vv(DisasContext *ctx, arg_vwsll_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsll_vx(DisasContext *ctx, arg_vwsll_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsub_vv(DisasContext *ctx, arg_vwsub_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsub_vx(DisasContext *ctx, arg_vwsub_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsub_wv(DisasContext *ctx, arg_vwsub_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsub_wx(DisasContext *ctx, arg_vwsub_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsubu_vv(DisasContext *ctx, arg_vwsubu_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsubu_vx(DisasContext *ctx, arg_vwsubu_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsubu_wv(DisasContext *ctx, arg_vwsubu_wv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vwsubu_wx(DisasContext *ctx, arg_vwsubu_wx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vxor_vi(DisasContext *ctx, arg_vxor_vi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vxor_vv(DisasContext *ctx, arg_vxor_vv *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vxor_vx(DisasContext *ctx, arg_vxor_vx *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vzext_vf2(DisasContext *ctx, arg_vzext_vf2 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vzext_vf4(DisasContext *ctx, arg_vzext_vf4 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_vzext_vf8(DisasContext *ctx, arg_vzext_vf8 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_wfi(DisasContext *ctx, arg_wfi *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_wrs_nto(DisasContext *ctx, arg_wrs_nto *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_wrs_sto(DisasContext *ctx, arg_wrs_sto *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_xnor(DisasContext *ctx, arg_xnor *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_xor(DisasContext *ctx, arg_xor *a)
{
    ctx->ir1->opcode = LATR_RV_XOR;
    ctx->ir1->arg.rd = a->rd;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rs2 = a->rs2;
    return true;
}
static bool trans_xori(DisasContext *ctx, arg_xori *a)
{
    ctx->ir1->opcode = LATR_RV_XORI;
    ctx->ir1->arg.imm = a->imm;
    ctx->ir1->arg.rs1 = a->rs1;
    ctx->ir1->arg.rd = a->rd;
    return true;
}
static bool trans_xperm4(DisasContext *ctx, arg_xperm4 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_xperm8(DisasContext *ctx, arg_xperm8 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_zext_h_32(DisasContext *ctx, arg_zext_h_32 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_zext_h_64(DisasContext *ctx, arg_zext_h_64 *a)
{
    (void)ctx;
    (void)a;
    return false;
}
static bool trans_zip(DisasContext *ctx, arg_zip *a)
{
    (void)ctx;
    (void)a;
    return false;
}
