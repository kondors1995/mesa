/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2026 Freedreno / Turnip contributors
 * SPDX-License-Identifier: MIT
 */

#define A6XX_PERFCNTRS_XML
#include "tu_shader.h"

#include "tu_cs.h"
#include "tu_device.h"
#include "tu_pipeline.h"

#include "ir3/ir3_shader.h"

#include "common/fd5_hw.h"

static enum a4xx_state_block
tu5_stage2shadersb(mesa_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return SB4_VS_SHADER;
   case MESA_SHADER_TESS_CTRL:
      return SB4_HS_SHADER;
   case MESA_SHADER_TESS_EVAL:
      return SB4_DS_SHADER;
   case MESA_SHADER_GEOMETRY:
      return SB4_GS_SHADER;
   case MESA_SHADER_FRAGMENT:
      return SB4_FS_SHADER;
   case MESA_SHADER_COMPUTE:
      return SB4_CS_SHADER;
   default:
      UNREACHABLE("invalid shader stage");
   }
}

static uint32_t
tu5_xs_instrlen(const struct ir3_shader_variant *xs, mesa_shader_stage stage)
{
   if (!xs)
      return 0;

   /* Partition instruction RAM: [0..31] for VS/CS, [32..63] for FS.
    * Shaders larger than 32 units (512 instructions) run from external memory
    * directly (instrlen = 0).
    */
   (void)stage;
   return xs->instrlen <= 32 ? xs->instrlen : 0;
}

static void
tu5_emit_shader_preload(struct tu_cs *cs,
                        mesa_shader_stage stage,
                        uint32_t instrlen,
                        uint64_t binary_iova)
{
   if (!instrlen)
      return;

   assert((binary_iova & 0x7f) == 0);

   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(tu5_stage2shadersb(stage)) |
                  CP_LOAD_STATE4_0_NUM_UNIT(instrlen));
   tu_cs_emit_qw(cs, binary_iova |
                     static_cast<uint64_t>(CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER)));
}

static void
tu5_emit_shader_obj_regs(struct tu_cs *cs,
                         uint32_t start_reg,
                         const struct ir3_shader_variant *xs,
                         const struct tu_pvtmem_config *pvtmem,
                         uint64_t binary_iova)
{
   tu_cs_emit_pkt4(cs, start_reg, 6);
   tu_cs_emit_qw(cs, binary_iova);
   tu_cs_emit(cs, A5XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM(pvtmem->per_fiber_size) |
                  A5XX_SP_VS_PVT_MEM_PARAM_HWSTACKOFFSET(pvtmem->per_sp_size));
   if (xs && xs->pvtmem_size > 0) {
      tu_cs_emit_qw(cs, pvtmem->iova);
   } else {
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
   }
   tu_cs_emit(cs, A5XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(pvtmem->per_sp_size));
}

static void
tu5_emit_xs_immediates(struct tu_cs *cs,
                       mesa_shader_stage stage,
                       const struct ir3_shader_variant *xs)
{
   if (!xs)
      return;

   const struct ir3_const_state *const_state = ir3_const_state(xs);
   uint32_t base = const_state->allocs.max_const_offset_vec4;
   const struct ir3_imm_const_state *imm_state = &xs->imm_state;
   int32_t imm_vec4s = xs->compiler->info->props.load_shader_consts_via_preamble ?
      0 : DIV_ROUND_UP(imm_state->count, 4);
   imm_vec4s = MAX2(MIN2(imm_vec4s + static_cast<int32_t>(base),
                         static_cast<int32_t>(xs->constlen)) -
                    static_cast<int32_t>(base), 0);
   unsigned immediate_size = imm_vec4s * 4;

   if (immediate_size > 0) {
      assert((base % 4) == 0 || stage != MESA_SHADER_COMPUTE);
      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + immediate_size);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(base) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(tu5_stage2shadersb(stage)) |
                     CP_LOAD_STATE4_0_NUM_UNIT(immediate_size / 4));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                     CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
      tu_cs_emit_array(cs, imm_state->values, immediate_size);
   }

   if (stage == MESA_SHADER_FRAGMENT && const_state->num_driver_params > 0) {
      uint32_t dp_base =
         const_state->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4;
      int32_t size = DIV_ROUND_UP(MAX2(const_state->num_driver_params, 4), 4);
      size = MAX2(MIN2(size + static_cast<int32_t>(dp_base),
                       static_cast<int32_t>(xs->constlen)) -
                  static_cast<int32_t>(dp_base), 0);
      if (size > 0) {
         tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 4);
         tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(dp_base) |
                        CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                        CP_LOAD_STATE4_0_STATE_BLOCK(tu5_stage2shadersb(stage)) |
                        CP_LOAD_STATE4_0_NUM_UNIT(size));
         tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                        CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
         tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
         tu_cs_emit(cs, xs->info.subgroup_size);
         tu_cs_emit(cs, 0);
         tu_cs_emit(cs, 0);
         tu_cs_emit(cs, 0);
      }
   }
}

static void
tu5_emit_vfd_dest(struct tu_cs *cs, const struct ir3_shader_variant *vs)
{
   int32_t input_for_attr[MAX_VERTEX_ATTRIBS];
   for (unsigned i = 0; i < MAX_VERTEX_ATTRIBS; i++)
      input_for_attr[i] = -1;

   for (unsigned i = 0; i < vs->inputs_count; i++) {
      if (vs->inputs[i].sysval || vs->inputs[i].regid == regid(63, 0))
         continue;

      assert(vs->inputs[i].slot >= VERT_ATTRIB_GENERIC0);
      unsigned loc = vs->inputs[i].slot - VERT_ATTRIB_GENERIC0;
      if (loc < MAX_VERTEX_ATTRIBS)
         input_for_attr[loc] = i;
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_CONTROL_0, 1);
   tu_cs_emit(cs, A5XX_VFD_CONTROL_0_VTXCNT(vs->attr_in));

   if (vs->attr_in) {
      tu_cs_emit_pkt4(cs, REG_A5XX_VFD_DEST_CNTL(0), vs->attr_in);
      for (unsigned i = 0; i < vs->attr_in; i++) {
         if (input_for_attr[i] >= 0) {
            unsigned input_idx = input_for_attr[i];
            tu_cs_emit(cs, A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(vs->inputs[input_idx].compmask) |
                           A5XX_VFD_DEST_CNTL_INSTR_REGID(vs->inputs[input_idx].regid));
         } else {
            tu_cs_emit(cs, A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(0) |
                           A5XX_VFD_DEST_CNTL_INSTR_REGID(regid(63, 0)));
         }
      }
   }
}

static void
tu5_emit_vs_state(struct tu_cs *cs,
                  const struct ir3_shader_variant *vs,
                  const struct tu_pvtmem_config *pvtmem,
                  uint64_t binary_iova)
{
   tu5_emit_shader_obj_regs(cs, REG_A5XX_SP_VS_OBJ_START, vs, pvtmem, binary_iova);
   tu5_emit_vfd_dest(cs, vs);

   uint32_t vertex_regid = ir3_find_sysval_regid(vs, SYSTEM_VALUE_VERTEX_ID);
   if (!VALIDREG(vertex_regid))
      vertex_regid = ir3_find_sysval_regid(vs, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);
   const uint32_t instance_regid =
      ir3_find_sysval_regid(vs, SYSTEM_VALUE_INSTANCE_ID);

   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_CONTROL_1, 5);
   tu_cs_emit(cs, A5XX_VFD_CONTROL_1_REGID4VTX(vertex_regid) |
                  A5XX_VFD_CONTROL_1_REGID4INST(instance_regid) |
                  0xfc0000);
   tu_cs_emit(cs, 0x0000fcfc);
   tu_cs_emit(cs, 0x0000fcfc);
   tu_cs_emit(cs, 0x000000fc);
   tu_cs_emit(cs, 0x00000000);

   tu5_emit_xs_immediates(cs, MESA_SHADER_VERTEX, vs);
}

static inline uint32_t
tu5_next_regid(uint32_t reg, uint32_t increment)
{
   if (VALIDREG(reg))
      return reg + increment;
   else
      return regid(63, 0);
}

static void
tu5_emit_fs_state(struct tu_cs *cs,
                  const struct ir3_shader_variant *fs,
                  const struct tu_pvtmem_config *pvtmem,
                  uint64_t binary_iova)
{
   tu5_emit_shader_obj_regs(cs, REG_A5XX_SP_FS_OBJ_START, fs, pvtmem, binary_iova);

   enum a3xx_threadsize fssz = fs->info.double_threadsize ? FOUR_QUADS : TWO_QUADS;

   const uint32_t face_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRONT_FACE);
   const uint32_t coord_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_COORD);
   const uint32_t zwcoord_regid = tu5_next_regid(coord_regid, 2);
   const uint32_t samp_id_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_ID);
   const uint32_t samp_mask_regid =
      ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_MASK_IN);

   uint32_t ij_regid[IJ_COUNT];
   for (unsigned i = 0; i < ARRAY_SIZE(ij_regid); i++)
      ij_regid[i] = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + i);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CONTROL_0_REG, 5);
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(fssz) |
                  A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(TWO_QUADS) |
                  0x00000880);
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD(63));
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_2_REG_FACEREGID(face_regid) |
                  A5XX_HLSQ_CONTROL_2_REG_SAMPLEID(samp_id_regid) |
                  A5XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(samp_mask_regid) |
                  A5XX_HLSQ_CONTROL_2_REG_CENTERRHW(ij_regid[IJ_PERSP_CENTER_RHW]));
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(ij_regid[IJ_PERSP_PIXEL]) |
                  A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(ij_regid[IJ_LINEAR_PIXEL]) |
                  A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(ij_regid[IJ_PERSP_CENTROID]) |
                  A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(ij_regid[IJ_LINEAR_CENTROID]));
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(coord_regid) |
                  A5XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwcoord_regid) |
                  A5XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE(ij_regid[IJ_PERSP_SAMPLE]) |
                  A5XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE(ij_regid[IJ_LINEAR_SAMPLE]));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CNTL, 1);
   tu_cs_emit(cs,
              CONDREG(ij_regid[IJ_PERSP_PIXEL], A5XX_GRAS_CNTL_IJ_PERSP_PIXEL) |
              CONDREG(ij_regid[IJ_PERSP_CENTROID], A5XX_GRAS_CNTL_IJ_PERSP_CENTROID) |
              CONDREG(ij_regid[IJ_PERSP_SAMPLE], A5XX_GRAS_CNTL_IJ_PERSP_SAMPLE) |
              CONDREG(ij_regid[IJ_LINEAR_PIXEL], A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL) |
              CONDREG(ij_regid[IJ_LINEAR_CENTROID], A5XX_GRAS_CNTL_IJ_LINEAR_CENTROID) |
              CONDREG(ij_regid[IJ_LINEAR_SAMPLE], A5XX_GRAS_CNTL_IJ_LINEAR_SAMPLE) |
              COND(fs->fragcoord_compmask != 0,
                   A5XX_GRAS_CNTL_COORD_MASK(fs->fragcoord_compmask) |
                   A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL) |
              COND(fs->frag_face, A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL) |
              CONDREG(ij_regid[IJ_PERSP_CENTER_RHW], A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL) |
              CONDREG(ij_regid[IJ_LINEAR_PIXEL], A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_CONTROL0, 2);
   tu_cs_emit(cs,
              CONDREG(ij_regid[IJ_PERSP_PIXEL], A5XX_RB_RENDER_CONTROL0_IJ_PERSP_PIXEL) |
              CONDREG(ij_regid[IJ_PERSP_CENTROID], A5XX_RB_RENDER_CONTROL0_IJ_PERSP_CENTROID) |
              CONDREG(ij_regid[IJ_PERSP_SAMPLE], A5XX_RB_RENDER_CONTROL0_IJ_PERSP_SAMPLE) |
              CONDREG(ij_regid[IJ_LINEAR_PIXEL], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
              CONDREG(ij_regid[IJ_LINEAR_CENTROID], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_CENTROID) |
              CONDREG(ij_regid[IJ_LINEAR_SAMPLE], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_SAMPLE) |
              COND(fs->fragcoord_compmask != 0,
                   A5XX_RB_RENDER_CONTROL0_COORD_MASK(fs->fragcoord_compmask) |
                   A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
              COND(fs->frag_face, A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
              CONDREG(ij_regid[IJ_PERSP_CENTER_RHW], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
              CONDREG(ij_regid[IJ_LINEAR_PIXEL], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL));
   tu_cs_emit(cs,
              CONDREG(samp_mask_regid, A5XX_RB_RENDER_CONTROL1_SAMPLEMASK) |
              COND(fs->frag_face, A5XX_RB_RENDER_CONTROL1_FACENESS) |
              CONDREG(samp_id_regid, A5XX_RB_RENDER_CONTROL1_SAMPLEID) |
              CONDREG(ij_regid[IJ_PERSP_CENTER_RHW], A5XX_RB_RENDER_CONTROL1_CENTERRHW) |
              COND(fs->sample_shading, A5XX_RB_RENDER_CONTROL1_SAMPLEMODE(3)));

   uint32_t color_regid[8];
   if (fs->color0_mrt) {
      uint32_t r = ir3_find_output_regid(fs, FRAG_RESULT_COLOR);
      for (unsigned i = 0; i < 8; i++)
         color_regid[i] = r;
   } else {
      for (unsigned i = 0; i < 8; i++)
         color_regid[i] = ir3_find_output_regid(fs, FRAG_RESULT_DATA0 + i);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_OUTPUT_REG(0), 8);
   for (unsigned i = 0; i < 8; i++) {
      tu_cs_emit(cs, A5XX_SP_FS_OUTPUT_REG_REGID(color_regid[i]) |
                     COND(color_regid[i] & HALF_REG_ID,
                          A5XX_SP_FS_OUTPUT_REG_HALF_PRECISION));
   }

   tu5_emit_xs_immediates(cs, MESA_SHADER_FRAGMENT, fs);
}

static void
tu5_emit_cs_state(struct tu_cs *cs,
                  const struct ir3_shader_variant *v,
                  const struct tu_pvtmem_config *pvtmem,
                  uint64_t binary_iova)
{
   enum a3xx_threadsize thrsz = v->info.double_threadsize ? FOUR_QUADS : TWO_QUADS;
   uint32_t instrlen = tu5_xs_instrlen(v, MESA_SHADER_COMPUTE);
   uint32_t constlen = DIV_ROUND_UP(v->constlen, 4);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_SP_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CONTROL_0_REG, 1);
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(TWO_QUADS) |
                  A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(thrsz) |
                  0x00000880);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_CS_CTRL_REG0, 1);
   tu_cs_emit(cs, A5XX_SP_CS_CTRL_REG0_THREADSIZE(thrsz) |
                  A5XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(v->info.max_half_reg + 1) |
                  A5XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(v->info.max_reg + 1) |
                  A5XX_SP_CS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(v)) |
                  COND(instrlen != 0, A5XX_SP_CS_CTRL_REG0_BUFFER) |
                  0x2);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_CONFIG, 1);
   tu_cs_emit(cs, A5XX_HLSQ_CS_CONFIG_CONSTOBJECTOFFSET(0) |
                  A5XX_HLSQ_CS_CONFIG_SHADEROBJOFFSET(0) |
                  A5XX_HLSQ_CS_CONFIG_ENABLED);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_CNTL, 1);
   tu_cs_emit(cs, A5XX_HLSQ_CS_CNTL_INSTRLEN(instrlen) |
                  COND(v->has_ssbo, A5XX_HLSQ_CS_CNTL_SSBO_ENABLE));

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_CS_CONFIG, 1);
   tu_cs_emit(cs, A5XX_SP_CS_CONFIG_CONSTOBJECTOFFSET(0) |
                  A5XX_SP_CS_CONFIG_SHADEROBJOFFSET(0) |
                  A5XX_SP_CS_CONFIG_ENABLED);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_CONSTLEN, 2);
   tu_cs_emit(cs, constlen);
   tu_cs_emit(cs, instrlen);

   tu5_emit_shader_obj_regs(cs, REG_A5XX_SP_CS_OBJ_START, v, pvtmem, binary_iova);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_UPDATE_CNTL, 1);
   tu_cs_emit(cs, 0x1f00000);

   uint32_t local_invocation_id =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
   uint32_t work_group_id =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_CNTL_0, 2);
   tu_cs_emit(cs, A5XX_HLSQ_CS_CNTL_0_WGIDCONSTID(work_group_id) |
                  A5XX_HLSQ_CS_CNTL_0_UNK0(regid(63, 0)) |
                  A5XX_HLSQ_CS_CNTL_0_UNK1(regid(63, 0)) |
                  A5XX_HLSQ_CS_CNTL_0_LOCALIDREGID(local_invocation_id));
   tu_cs_emit(cs, 0x1);

   tu5_emit_shader_preload(cs, MESA_SHADER_COMPUTE, instrlen, binary_iova);
   tu5_emit_xs_immediates(cs, MESA_SHADER_COMPUTE, v);
}

void
tu5_emit_variant(struct tu_cs *cs,
                 mesa_shader_stage stage,
                 const struct ir3_shader_variant *xs,
                 const struct tu_pvtmem_config *pvtmem_config,
                 uint32_t view_mask,
                 uint64_t binary_iova)
{
   (void)view_mask;
   if (!xs)
      return;

   switch (stage) {
   case MESA_SHADER_VERTEX:
      tu5_emit_vs_state(cs, xs, pvtmem_config, binary_iova);
      break;
   case MESA_SHADER_FRAGMENT:
      tu5_emit_fs_state(cs, xs, pvtmem_config, binary_iova);
      break;
   case MESA_SHADER_COMPUTE:
      tu5_emit_cs_state(cs, xs, pvtmem_config, binary_iova);
      break;
   default:
      break;
   }
}

void
tu5_emit_program_config(struct tu_cs *cs,
                        const struct ir3_shader_variant *vs,
                        const struct ir3_shader_variant *fs,
                        uint64_t vs_iova,
                        uint64_t fs_iova)
{
   uint32_t vs_constlen = vs ? DIV_ROUND_UP(vs->constlen, 4) : 0;
   uint32_t fs_constlen = fs ? DIV_ROUND_UP(fs->constlen, 4) : 0;
   uint32_t vs_instrlen = tu5_xs_instrlen(vs, MESA_SHADER_VERTEX);
   uint32_t fs_instrlen = tu5_xs_instrlen(fs, MESA_SHADER_FRAGMENT);

   uint32_t vs_constoff = 0;
   uint32_t fs_constoff = vs_constlen;
   uint32_t other_constoff = fs_constoff + fs_constlen;
   uint32_t vs_instroff = 0;
   uint32_t fs_instroff = 64 - fs_instrlen;

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_VS_CONFIG, 5);
   tu_cs_emit(cs, A5XX_HLSQ_VS_CONFIG_CONSTOBJECTOFFSET(vs_constoff) |
                  A5XX_HLSQ_VS_CONFIG_SHADEROBJOFFSET(vs_instroff) |
                  COND(vs, A5XX_HLSQ_VS_CONFIG_ENABLED));
   tu_cs_emit(cs, A5XX_HLSQ_FS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                  A5XX_HLSQ_FS_CONFIG_SHADEROBJOFFSET(fs_instroff) |
                  COND(fs, A5XX_HLSQ_FS_CONFIG_ENABLED));
   tu_cs_emit(cs, A5XX_HLSQ_HS_CONFIG_CONSTOBJECTOFFSET(other_constoff) |
                  A5XX_HLSQ_HS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   tu_cs_emit(cs, A5XX_HLSQ_DS_CONFIG_CONSTOBJECTOFFSET(other_constoff) |
                  A5XX_HLSQ_DS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   tu_cs_emit(cs, A5XX_HLSQ_GS_CONFIG_CONSTOBJECTOFFSET(other_constoff) |
                  A5XX_HLSQ_GS_CONFIG_SHADEROBJOFFSET(fs_instroff));

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_CONFIG, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_VS_CNTL, 5);
   tu_cs_emit(cs, A5XX_HLSQ_VS_CNTL_INSTRLEN(vs_instrlen) |
                  COND(vs && vs->has_ssbo, A5XX_HLSQ_VS_CNTL_SSBO_ENABLE));
   tu_cs_emit(cs, A5XX_HLSQ_FS_CNTL_INSTRLEN(fs_instrlen) |
                  COND(fs && fs->has_ssbo, A5XX_HLSQ_FS_CNTL_SSBO_ENABLE));
   tu_cs_emit(cs, A5XX_HLSQ_HS_CNTL_INSTRLEN(0));
   tu_cs_emit(cs, A5XX_HLSQ_DS_CNTL_INSTRLEN(0));
   tu_cs_emit(cs, A5XX_HLSQ_GS_CNTL_INSTRLEN(0));

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_VS_CONFIG, 5);
   tu_cs_emit(cs, A5XX_SP_VS_CONFIG_CONSTOBJECTOFFSET(vs_constoff) |
                  A5XX_SP_VS_CONFIG_SHADEROBJOFFSET(vs_instroff) |
                  COND(vs, A5XX_SP_VS_CONFIG_ENABLED));
   tu_cs_emit(cs, A5XX_SP_FS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                  A5XX_SP_FS_CONFIG_SHADEROBJOFFSET(fs_instroff) |
                  COND(fs, A5XX_SP_FS_CONFIG_ENABLED));
   tu_cs_emit(cs, A5XX_SP_HS_CONFIG_CONSTOBJECTOFFSET(other_constoff) |
                  A5XX_SP_HS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   tu_cs_emit(cs, A5XX_SP_DS_CONFIG_CONSTOBJECTOFFSET(other_constoff) |
                  A5XX_SP_DS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   tu_cs_emit(cs, A5XX_SP_GS_CONFIG_CONSTOBJECTOFFSET(other_constoff) |
                  A5XX_SP_GS_CONFIG_SHADEROBJOFFSET(fs_instroff));

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_CS_CONFIG, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_VS_CONSTLEN, 2);
   tu_cs_emit(cs, vs_constlen);
   tu_cs_emit(cs, vs_instrlen);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_FS_CONSTLEN, 2);
   tu_cs_emit(cs, fs_constlen);
   tu_cs_emit(cs, fs_instrlen);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_HS_CONSTLEN, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_DS_CONSTLEN, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_GS_CONSTLEN, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_CONSTLEN, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   if (vs) {
      tu_cs_emit_pkt4(cs, REG_A5XX_SP_VS_CTRL_REG0, 1);
      tu_cs_emit(cs,
                 A5XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(vs->info.max_half_reg + 1) |
                 A5XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(vs->info.max_reg + 1) |
                 COND(vs_instrlen != 0, A5XX_SP_VS_CTRL_REG0_BUFFER) |
                 0x2 |
                 A5XX_SP_VS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(vs)) |
                 COND(vs->need_pixlod, A5XX_SP_VS_CTRL_REG0_PIXLODENABLE));
      if (vs_iova)
         tu5_emit_shader_preload(cs, MESA_SHADER_VERTEX, vs_instrlen, vs_iova);
   }

   if (fs) {
      enum a3xx_threadsize fssz =
         fs->info.double_threadsize ? FOUR_QUADS : TWO_QUADS;
      tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_CTRL_REG0, 1);
      tu_cs_emit(cs,
                 COND(fs->total_in > 0, A5XX_SP_FS_CTRL_REG0_VARYING) |
                 0x40002 |
                 COND(fs_instrlen != 0, A5XX_SP_FS_CTRL_REG0_BUFFER) |
                 A5XX_SP_FS_CTRL_REG0_THREADSIZE(fssz) |
                 A5XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(fs->info.max_half_reg + 1) |
                 A5XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(fs->info.max_reg + 1) |
                 A5XX_SP_FS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(fs)) |
                 COND(fs->need_pixlod, A5XX_SP_FS_CTRL_REG0_PIXLODENABLE));
      if (fs_iova)
         tu5_emit_shader_preload(cs, MESA_SHADER_FRAGMENT, fs_instrlen, fs_iova);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_UPDATE_CNTL, 1);
   tu_cs_emit(cs, 0x020fffff);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_GS_SIV_CNTL, 1);
   tu_cs_emit(cs, 0x0000ffff);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_SP_CNTL, 1);
   tu_cs_emit(cs, 0x00000010);
}

struct tu5_vpc_linkage {
   struct ir3_shader_linkage linkage;
   uint32_t varmask[4];
   uint32_t pos_regid;
   uint8_t pos_loc;
   uint8_t psize_loc;
   uint8_t clip0_loc;
   uint8_t clip1_loc;
   uint8_t clip_mask;
   uint8_t cull_mask;
   uint8_t clip_cull_mask;
};

static void
tu5_build_vpc_linkage(const struct ir3_shader_variant *vs,
                      const struct ir3_shader_variant *fs,
                      struct tu5_vpc_linkage *out)
{
   struct ir3_shader_variant dummy_fs = {};
   if (!fs)
      fs = &dummy_fs;

   *out = (struct tu5_vpc_linkage){
      .linkage = {
         .primid_loc = 0xff,
         .clip0_loc = 0xff,
         .clip1_loc = 0xff,
      },
      .psize_loc = 0xff,
   };
   ir3_link_shaders(&out->linkage, vs, fs, true);
   memcpy(out->varmask, out->linkage.varmask, sizeof(out->varmask));

   out->clip_mask = vs->clip_mask;
   out->cull_mask = vs->cull_mask;
   out->clip_cull_mask = out->clip_mask | out->cull_mask;
   out->clip0_loc = out->linkage.clip0_loc;
   out->clip1_loc = out->linkage.clip1_loc;

   out->pos_regid = ir3_find_output_regid(vs, VARYING_SLOT_POS);
   uint32_t psize_regid = ir3_find_output_regid(vs, VARYING_SLOT_PSIZ);
   uint32_t clip0_regid = ir3_find_output_regid(vs, VARYING_SLOT_CLIP_DIST0);
   uint32_t clip1_regid = ir3_find_output_regid(vs, VARYING_SLOT_CLIP_DIST1);

   out->pos_loc = out->linkage.max_loc;
   if (VALIDREG(out->pos_regid))
      ir3_link_add(&out->linkage, VARYING_SLOT_POS, out->pos_regid, 0xf,
                   out->linkage.max_loc);

   if (VALIDREG(psize_regid)) {
      out->psize_loc = out->linkage.max_loc;
      ir3_link_add(&out->linkage, VARYING_SLOT_PSIZ, psize_regid, 0x1,
                   out->linkage.max_loc);
   }

   /* Append clip/cull distances not read by the FS, matching fd5_program. */
   if (out->clip0_loc == 0xff && VALIDREG(clip0_regid) &&
       (out->clip_cull_mask & 0xf) != 0) {
      out->clip0_loc = out->linkage.max_loc;
      ir3_link_add(&out->linkage, VARYING_SLOT_CLIP_DIST0, clip0_regid,
                   out->clip_cull_mask & 0xf, out->linkage.max_loc);
   }
   if (out->clip1_loc == 0xff && VALIDREG(clip1_regid) &&
       (out->clip_cull_mask >> 4) != 0) {
      out->clip1_loc = out->linkage.max_loc;
      ir3_link_add(&out->linkage, VARYING_SLOT_CLIP_DIST1, clip1_regid,
                   out->clip_cull_mask >> 4, out->linkage.max_loc);
   }

   if (out->linkage.cnt == 0)
      ir3_link_add(&out->linkage, 0, 0, 0x1, out->linkage.max_loc);
}

uint32_t
tu5_calc_stride_in_vpc(const struct ir3_shader_variant *vs,
                       const struct ir3_shader_variant *fs)
{
   if (!vs)
      return 0;

   struct tu5_vpc_linkage l;
   tu5_build_vpc_linkage(vs, fs, &l);
   return l.linkage.max_loc;
}

void
tu5_emit_vpc(struct tu_cs *cs,
             const struct ir3_shader_variant *vs,
             const struct ir3_shader_variant *fs)
{
   struct ir3_shader_variant dummy_fs = {};
   if (!fs)
      fs = &dummy_fs;

   if (!vs)
      return;

   struct tu5_vpc_linkage l;
   tu5_build_vpc_linkage(vs, fs, &l);

   struct ir3_shader_linkage &linkage = l.linkage;
   uint32_t pos_regid = l.pos_regid;
   uint8_t pos_loc = l.pos_loc;
   uint8_t psize_loc = l.psize_loc;

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VAR_DISABLE(0), 4);
   tu_cs_emit(cs, ~l.varmask[0]);
   tu_cs_emit(cs, ~l.varmask[1]);
   tu_cs_emit(cs, ~l.varmask[2]);
   tu_cs_emit(cs, ~l.varmask[3]);

   for (unsigned i = 0, j = 0; i < 16 && j < linkage.cnt; i++) {
      uint32_t reg = 0;
      tu_cs_emit_pkt4(cs, REG_A5XX_SP_VS_OUT_REG(i), 1);
      reg |= A5XX_SP_VS_OUT_REG_A_REGID(linkage.var[j].regid) |
             A5XX_SP_VS_OUT_REG_A_COMPMASK(linkage.var[j].compmask);
      j++;
      if (j < linkage.cnt) {
         reg |= A5XX_SP_VS_OUT_REG_B_REGID(linkage.var[j].regid) |
                A5XX_SP_VS_OUT_REG_B_COMPMASK(linkage.var[j].compmask);
         j++;
      }
      tu_cs_emit(cs, reg);
   }

   for (unsigned i = 0, j = 0; i < 8 && j < linkage.cnt; i++) {
      uint32_t reg = 0;
      tu_cs_emit_pkt4(cs, REG_A5XX_SP_VS_VPC_DST_REG(i), 1);
      for (unsigned k = 0; k < 4 && j < linkage.cnt; k++, j++)
         reg |= (linkage.var[j].loc & 0xffu) << (k * 8);
      tu_cs_emit(cs, reg);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_PRIM_VTX_CNTL, 1);
   tu_cs_emit(cs, COND(vs->writes_psize, A5XX_PC_PRIM_VTX_CNTL_PSIZE));

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_PRIMITIVE_CNTL, 1);
   tu_cs_emit(cs, A5XX_SP_PRIMITIVE_CNTL_VSOUT(linkage.cnt));

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_CNTL_0, 1);
   tu_cs_emit(cs, A5XX_VPC_CNTL_0_STRIDE_IN_VPC(linkage.max_loc) |
                  COND(fs->total_in > 0, A5XX_VPC_CNTL_0_VARYING) |
                  0x10000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_PACK, 1);
   tu_cs_emit(cs, A5XX_VPC_PACK_NUMNONPOSVAR(pos_loc) |
                  A5XX_VPC_PACK_PSIZELOC(psize_loc));

   uint32_t vinterp[8] = {}, vpsrepl[8] = {};
   for (int j = -1; (j = ir3_next_varying(fs, j)) < static_cast<int>(fs->inputs_count);) {
      unsigned compmask = fs->inputs[j].compmask;
      uint32_t inloc = fs->inputs[j].inloc;

      if (fs->inputs[j].flat) {
         uint32_t loc = inloc;
         for (unsigned i = 0; i < 4; i++) {
            if (compmask & (1 << i)) {
               vinterp[loc / 16] |= 1u << ((loc % 16) * 2);
               loc++;
            }
         }
      }

      if (fs->inputs[j].slot == VARYING_SLOT_PNTC) {
         unsigned mask = 0b1001;
         uint32_t loc = inloc;
         if (compmask & 0x1) {
            vpsrepl[loc / 16] |= ((mask >> 0) & 0x3) << ((loc % 16) * 2);
            loc++;
         }
         if (compmask & 0x2) {
            vpsrepl[loc / 16] |= ((mask >> 2) & 0x3) << ((loc % 16) * 2);
            loc++;
         }
         if (compmask & 0x4) {
            vinterp[loc / 16] |= 0b10u << ((loc % 16) * 2);
            loc++;
         }
         if (compmask & 0x8) {
            vinterp[loc / 16] |= 0b11u << ((loc % 16) * 2);
            loc++;
         }
      }
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VARYING_INTERP_MODE(0), 8);
   tu_cs_emit_array(cs, vinterp, 8);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VARYING_PS_REPL_MODE(0), 8);
   tu_cs_emit_array(cs, vpsrepl, 8);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_VS_CL_CNTL, 1);
   tu_cs_emit(cs, A5XX_GRAS_VS_CL_CNTL_CLIP_MASK(l.clip_mask) |
                  A5XX_GRAS_VS_CL_CNTL_CULL_MASK(l.cull_mask));

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_CLIP_CNTL, 1);
   tu_cs_emit(cs, A5XX_VPC_CLIP_CNTL_CLIP_MASK(l.clip_cull_mask) |
                  A5XX_VPC_CLIP_CNTL_CLIP_DIST_03_LOC(l.clip0_loc) |
                  A5XX_VPC_CLIP_CNTL_CLIP_DIST_47_LOC(l.clip1_loc));

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_CLIP_CNTL, 1);
   tu_cs_emit(cs, A5XX_PC_CLIP_CNTL_CLIP_MASK(l.clip_mask));

   if (tu_dbg_log_enabled()) {
      tu_dbg_log("A5_VPC vs=%p(in=%u,inst=%u,c=%u,clip=0x%x,cull=0x%x,pos=0x%x) fs=%p(tin=%u,inst=%u,c=%u,kill=%u) cnt=%u max=%u posloc=%u",
                 vs, vs->attr_in, vs->instrlen, vs->constlen, vs->clip_mask, vs->cull_mask, pos_regid,
                 fs, fs->total_in, fs->instrlen, fs->constlen, fs->has_kill,
                 linkage.cnt, linkage.max_loc, pos_loc);
   }
}
