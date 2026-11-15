/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2026 Freedreno / Turnip contributors
 * SPDX-License-Identifier: MIT
 */

#define A6XX_PERFCNTRS_XML
#include "tu_pipeline.h"

#include "tu_cs.h"
#include "tu_device.h"
#include "tu_util.h"

#include "common/fd5_hw.h"
#include "common/freedreno_guardband.h"

static bool
tu5_logic_op_reads_dst(VkLogicOp op)
{
   switch (op) {
   case VK_LOGIC_OP_CLEAR:
   case VK_LOGIC_OP_COPY:
   case VK_LOGIC_OP_COPY_INVERTED:
   case VK_LOGIC_OP_SET:
      return false;
   default:
      return true;
   }
}

static bool
tu5_blend_state_is_dual_src(const struct vk_color_blend_state *cb)
{
   for (unsigned i = 0; i < cb->attachment_count; i++) {
      if (tu_blend_factor_is_dual_src(static_cast<VkBlendFactor>(cb->attachments[i].src_color_blend_factor)) ||
          tu_blend_factor_is_dual_src(static_cast<VkBlendFactor>(cb->attachments[i].dst_color_blend_factor)) ||
          tu_blend_factor_is_dual_src(static_cast<VkBlendFactor>(cb->attachments[i].src_alpha_blend_factor)) ||
          tu_blend_factor_is_dual_src(static_cast<VkBlendFactor>(cb->attachments[i].dst_alpha_blend_factor)))
         return true;
   }
   return false;
}

unsigned
tu5_viewport_size(const struct vk_viewport_state *vp)
{
   return vp->viewport_count ? 18 : 0;
}

void
tu5_emit_viewport(struct tu_cs *cs,
                  const struct vk_viewport_state *vp,
                  const struct vk_rasterization_state *rs)
{
   if (!vp->viewport_count)
      return;

   const VkViewport *viewport = &vp->viewports[0];
   float offsets[3];
   float scales[3];
   scales[0] = viewport->width / 2.0f;
   scales[1] = viewport->height / 2.0f;
   if (vp->depth_clip_negative_one_to_one)
      scales[2] = 0.5f * (viewport->maxDepth - viewport->minDepth);
   else
      scales[2] = viewport->maxDepth - viewport->minDepth;

   offsets[0] = viewport->x + scales[0];
   offsets[1] = viewport->y + scales[1];
   if (vp->depth_clip_negative_one_to_one)
      offsets[2] = 0.5f * (viewport->minDepth + viewport->maxDepth);
   else
      offsets[2] = viewport->minDepth;

   uint32_t gb_w = MIN2(511u, fd_calc_guardband(offsets[0], scales[0], false));
   uint32_t gb_h = MIN2(511u, fd_calc_guardband(offsets[1], scales[1], false));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CL_VPORT_XOFFSET_0, 6);
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_XOFFSET_0(offsets[0]));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_XSCALE_0(scales[0]));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_YOFFSET_0(offsets[1]));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_YSCALE_0(scales[1]));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_ZOFFSET_0(offsets[2]));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_ZSCALE_0(scales[2]));

   VkOffset2D min, max;
   min.x = static_cast<int32_t>(viewport->x);
   max.x = static_cast<int32_t>(ceilf(viewport->x + viewport->width));
   if (viewport->height >= 0.0f) {
      min.y = static_cast<int32_t>(viewport->y);
      max.y = static_cast<int32_t>(ceilf(viewport->y + viewport->height));
   } else {
      min.y = static_cast<int32_t>(viewport->y + viewport->height);
      max.y = static_cast<int32_t>(ceilf(viewport->y));
   }
   if (min.y == max.y)
      max.y++;
   if (min.x == max.x)
      max.x++;

   min.x = MAX2(min.x, 0);
   min.y = MAX2(min.y, 0);
   max.x = MAX2(max.x, 1);
   max.y = MAX2(max.y, 1);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(min.x) |
                  A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(min.y));
   tu_cs_emit(cs, A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_X(max.x - 1) |
                  A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_Y(max.y - 1));

   float zmin, zmax;
   if (vp->depth_clamp_mode == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT) {
      zmin = vp->depth_clamp_range.minDepthClamp;
      zmax = vp->depth_clamp_range.maxDepthClamp;
   } else {
      zmin = MIN2(viewport->minDepth, viewport->maxDepth);
      zmax = MAX2(viewport->minDepth, viewport->maxDepth);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CL_VIEWPORT_ZCLAMP_NEAR_0, 2);
   tu_cs_emit(cs, A5XX_GRAS_CL_VIEWPORT_ZCLAMP_NEAR_0(zmin));
   tu_cs_emit(cs, A5XX_GRAS_CL_VIEWPORT_ZCLAMP_FAR_0(zmax));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_VIEWPORT_ZCLAMP_NEAR, 2);
   tu_cs_emit(cs, A5XX_RB_VIEWPORT_ZCLAMP_NEAR(zmin));
   tu_cs_emit(cs, A5XX_RB_VIEWPORT_ZCLAMP_FAR(zmax));

   if (tu_dbg_log_enabled()) {
      tu_dbg_log("A5_VP xywh=(%.1f,%.1f,%.1f,%.1f) d=(%.3f,%.3f) zc=(%.3f,%.3f) neg1=%u",
                 viewport->x, viewport->y, viewport->width, viewport->height,
                 viewport->minDepth, viewport->maxDepth, zmin, zmax,
                 vp->depth_clip_negative_one_to_one ? 1u : 0u);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CL_GUARDBAND_CLIP_ADJ, 1);
   tu_cs_emit(cs, A5XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ(gb_w) |
                  A5XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT(gb_h));
}

unsigned
tu5_scissor_size(const struct vk_viewport_state *vp)
{
   return vp->scissor_count ? 3 : 0;
}

void
tu5_emit_scissor(struct tu_cs *cs, const struct vk_viewport_state *vp)
{
   if (!vp->scissor_count)
      return;

   const VkRect2D *scissor = &vp->scissors[0];
   uint32_t min_x = scissor->offset.x;
   uint32_t min_y = scissor->offset.y;
   uint32_t max_x = min_x + scissor->extent.width - 1;
   uint32_t max_y = min_y + scissor->extent.height - 1;

   if (!scissor->extent.width || !scissor->extent.height) {
      min_x = min_y = 1;
      max_x = max_y = 0;
   } else {
      uint32_t scissor_max = BITFIELD_MASK(15);
      min_x = MIN2(scissor_max, min_x);
      min_y = MIN2(scissor_max, min_y);
      max_x = MIN2(scissor_max, max_x);
      max_y = MIN2(scissor_max, max_y);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(min_x) |
                  A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(min_y));
   tu_cs_emit(cs, A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_X(max_x) |
                  A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_Y(max_y));
}

unsigned
tu5_depth_bias_size(void)
{
   return 4;
}

void
tu5_emit_depth_bias(struct tu_cs *cs, const struct vk_rasterization_state *rs)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_POLY_OFFSET_SCALE, 3);
   tu_cs_emit(cs, A5XX_GRAS_SU_POLY_OFFSET_SCALE(rs->depth_bias.slope_factor));
   tu_cs_emit(cs, A5XX_GRAS_SU_POLY_OFFSET_OFFSET(rs->depth_bias.constant_factor));
   tu_cs_emit(cs, A5XX_GRAS_SU_POLY_OFFSET_OFFSET_CLAMP(rs->depth_bias.clamp));
}

unsigned
tu5_blend_size(void)
{
   return 6 + 3 * MAX_RTS;
}

void
tu5_emit_blend(struct tu_cs *cs,
               const struct vk_color_blend_state *cb,
               const struct vk_color_attachment_location_state *cal,
               const struct vk_render_pass_state *rp,
               bool alpha_to_coverage_enable,
               uint32_t sample_mask)
{
   bool rop_reads_dst = cb->logic_op_enable &&
      tu5_logic_op_reads_dst(static_cast<VkLogicOp>(cb->logic_op));
   enum a3xx_rop_code rop = tu6_rop(static_cast<VkLogicOp>(cb->logic_op));
   if (cs->device->physical_device->instance->drirc.misc.emulate_alpha_to_coverage)
      alpha_to_coverage_enable = false;

   uint32_t blend_enable_mask = 0;
   uint32_t mrt_control[MAX_RTS] = {};
   uint32_t mrt_blend_control[MAX_RTS] = {};
   uint8_t mrt_comp[MAX_RTS] = {};

   unsigned num_rts = alpha_to_coverage_enable ?
      MAX2(cb->attachment_count, 1) : cb->attachment_count;
   bool dual_src_blend = tu5_blend_state_is_dual_src(cb);

   for (unsigned i = 0; i < num_rts; i++) {
      if (cal->color_map[i] == MESA_VK_ATTACHMENT_UNUSED)
         continue;
      unsigned remapped_idx = cal->color_map[i];
      if (remapped_idx >= MAX_RTS)
         continue;

      mrt_comp[remapped_idx] = 0xf;

      if ((cb->color_write_enables & (1u << i)) && i < cb->attachment_count) {
         const struct vk_color_blend_attachment_state *att = &cb->attachments[i];
         VkFormat att_format = rp->color_attachment_formats[i];
         enum pipe_format pipe_fmt = vk_format_to_pipe_format(att_format);
         bool has_alpha = att_format == VK_FORMAT_UNDEFINED ||
                          util_format_has_alpha(pipe_fmt);
         bool is_int = att_format != VK_FORMAT_UNDEFINED &&
                       util_format_is_pure_integer(pipe_fmt);
         bool is_float_or_srgb = vk_format_is_float(att_format) ||
                                 vk_format_is_srgb(att_format);

         if ((att->blend_enable && !cb->logic_op_enable) ||
             (rop_reads_dst && !is_float_or_srgb)) {
            blend_enable_mask |= 1u << remapped_idx;
         }

         const enum a3xx_rb_blend_opcode color_op =
            tu6_blend_op(static_cast<VkBlendOp>(att->color_blend_op));
         const enum adreno_rb_blend_factor src_color_factor =
            tu6_blend_factor(static_cast<VkBlendFactor>(att->src_color_blend_factor));
         const enum adreno_rb_blend_factor dst_color_factor =
            tu6_blend_factor(static_cast<VkBlendFactor>(att->dst_color_blend_factor));
         const enum a3xx_rb_blend_opcode alpha_op =
            tu6_blend_op(static_cast<VkBlendOp>(att->alpha_blend_op));
         const enum adreno_rb_blend_factor src_alpha_factor =
            tu6_blend_factor(static_cast<VkBlendFactor>(att->src_alpha_blend_factor));
         const enum adreno_rb_blend_factor dst_alpha_factor =
            tu6_blend_factor(static_cast<VkBlendFactor>(att->dst_alpha_blend_factor));

         bool blend_enable = att->blend_enable && !cb->logic_op_enable;
         bool logic_op_enable = cb->logic_op_enable && !is_float_or_srgb;

         if (is_int) {
            mrt_control[remapped_idx] =
               A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(att->write_mask) |
               A5XX_RB_MRT_CONTROL_ROP_CODE(ROP_COPY);
         } else {
            mrt_control[remapped_idx] =
               COND(blend_enable, A5XX_RB_MRT_CONTROL_BLEND) |
               COND(blend_enable && has_alpha, A5XX_RB_MRT_CONTROL_BLEND2) |
               COND(logic_op_enable, A5XX_RB_MRT_CONTROL_ROP_ENABLE) |
               A5XX_RB_MRT_CONTROL_ROP_CODE(rop) |
               A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(att->write_mask);
         }

         mrt_blend_control[remapped_idx] =
            A5XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(src_color_factor) |
            A5XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(color_op) |
            A5XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(dst_color_factor) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(src_alpha_factor) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(alpha_op) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR(dst_alpha_factor);
      }
   }

   if (dual_src_blend)
      mrt_comp[1] = 0xf;

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_BLEND_CNTL, 1);
   tu_cs_emit(cs, A5XX_SP_BLEND_CNTL_ENABLE_BLEND(blend_enable_mask) |
                  A5XX_SP_BLEND_CNTL_UNK8 |
                  COND(dual_src_blend, A5XX_SP_BLEND_CNTL_DUAL_COLOR_IN_ENABLE) |
                  COND(alpha_to_coverage_enable, A5XX_SP_BLEND_CNTL_ALPHA_TO_COVERAGE));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLEND_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_BLEND_CNTL_ENABLE_BLEND(blend_enable_mask) |
                  A5XX_RB_BLEND_CNTL_INDEPENDENT_BLEND |
                  COND(dual_src_blend, A5XX_RB_BLEND_CNTL_DUAL_COLOR_IN_ENABLE) |
                  COND(alpha_to_coverage_enable, A5XX_RB_BLEND_CNTL_ALPHA_TO_COVERAGE) |
                  A5XX_RB_BLEND_CNTL_SAMPLE_MASK(sample_mask));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_COMPONENTS, 1);
   tu_cs_emit(cs, A5XX_RB_RENDER_COMPONENTS_RT0(mrt_comp[0]) |
                  A5XX_RB_RENDER_COMPONENTS_RT1(mrt_comp[1]) |
                  A5XX_RB_RENDER_COMPONENTS_RT2(mrt_comp[2]) |
                  A5XX_RB_RENDER_COMPONENTS_RT3(mrt_comp[3]) |
                  A5XX_RB_RENDER_COMPONENTS_RT4(mrt_comp[4]) |
                  A5XX_RB_RENDER_COMPONENTS_RT5(mrt_comp[5]) |
                  A5XX_RB_RENDER_COMPONENTS_RT6(mrt_comp[6]) |
                  A5XX_RB_RENDER_COMPONENTS_RT7(mrt_comp[7]));

   for (unsigned i = 0; i < MAX_RTS; i++) {
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_CONTROL(i), 2);
      tu_cs_emit(cs, mrt_control[i]);
      tu_cs_emit(cs, mrt_blend_control[i]);
   }

   if (tu_dbg_log_enabled()) {
      tu_dbg_log("A5_BLEND nrts=%u att=%u cw=0x%x bmask=0x%x mrt0=0x%08x b0=0x%08x comp0=0x%x a2c=%u rop=%u",
                 num_rts, cb->attachment_count, cb->color_write_enables, blend_enable_mask,
                 mrt_control[0], mrt_blend_control[0], mrt_comp[0],
                 alpha_to_coverage_enable ? 1u : 0u, cb->logic_op_enable ? 1u : 0u);
   }
}

unsigned
tu5_blend_constants_size(void)
{
   return 9;
}

void
tu5_emit_blend_constants(struct tu_cs *cs, const struct vk_color_blend_state *cb)
{
   const float *c = cb->blend_constants;

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLEND_RED, 8);
   tu_cs_emit(cs, A5XX_RB_BLEND_RED_FLOAT(c[0]) |
                  A5XX_RB_BLEND_RED_UINT(CLAMP(c[0], 0.0f, 1.0f) * 0xff) |
                  A5XX_RB_BLEND_RED_SINT(CLAMP(c[0], -1.0f, 1.0f) * 0x7f));
   tu_cs_emit(cs, A5XX_RB_BLEND_RED_F32(c[0]));
   tu_cs_emit(cs, A5XX_RB_BLEND_GREEN_FLOAT(c[1]) |
                  A5XX_RB_BLEND_GREEN_UINT(CLAMP(c[1], 0.0f, 1.0f) * 0xff) |
                  A5XX_RB_BLEND_GREEN_SINT(CLAMP(c[1], -1.0f, 1.0f) * 0x7f));
   tu_cs_emit(cs, A5XX_RB_BLEND_GREEN_F32(c[1]));
   tu_cs_emit(cs, A5XX_RB_BLEND_BLUE_FLOAT(c[2]) |
                  A5XX_RB_BLEND_BLUE_UINT(CLAMP(c[2], 0.0f, 1.0f) * 0xff) |
                  A5XX_RB_BLEND_BLUE_SINT(CLAMP(c[2], -1.0f, 1.0f) * 0x7f));
   tu_cs_emit(cs, A5XX_RB_BLEND_BLUE_F32(c[2]));
   tu_cs_emit(cs, A5XX_RB_BLEND_ALPHA_FLOAT(c[3]) |
                  A5XX_RB_BLEND_ALPHA_UINT(CLAMP(c[3], 0.0f, 1.0f) * 0xff) |
                  A5XX_RB_BLEND_ALPHA_SINT(CLAMP(c[3], -1.0f, 1.0f) * 0x7f));
   tu_cs_emit(cs, A5XX_RB_BLEND_ALPHA_F32(c[3]));
}

static enum adreno_pa_su_sc_draw
tu5_polygon_mode(VkPolygonMode mode)
{
   switch (mode) {
   case VK_POLYGON_MODE_POINT:
      return PC_DRAW_POINTS;
   case VK_POLYGON_MODE_LINE:
      return PC_DRAW_LINES;
   case VK_POLYGON_MODE_FILL:
      return PC_DRAW_TRIANGLES;
   default:
      UNREACHABLE("bad polygon mode");
   }
}

unsigned
tu5_rast_size(void)
{
   return 9;
}

void
tu5_emit_rast(struct tu_cs *cs,
              const struct vk_rasterization_state *rs,
              const struct vk_viewport_state *vp)
{
   enum a5xx_line_mode line_mode =
      rs->line.mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR ?
      BRESENHAM : RECTANGULAR;

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_CNTL, 1);
   tu_cs_emit(cs, COND(rs->cull_mode & VK_CULL_MODE_FRONT_BIT, A5XX_GRAS_SU_CNTL_CULL_FRONT) |
                  COND(rs->cull_mode & VK_CULL_MODE_BACK_BIT, A5XX_GRAS_SU_CNTL_CULL_BACK) |
                  COND(rs->front_face == VK_FRONT_FACE_CLOCKWISE, A5XX_GRAS_SU_CNTL_FRONT_CW) |
                  A5XX_GRAS_SU_CNTL_LINEHALFWIDTH(rs->line.width / 2.0f) |
                  COND(rs->depth_bias.enable, A5XX_GRAS_SU_CNTL_POLY_OFFSET) |
                  A5XX_GRAS_SU_CNTL_LINE_MODE(line_mode));

   bool depth_clip_enable = vk_rasterization_state_depth_clip_enable(rs);
   uint32_t gras_cl_cntl =
      COND(!depth_clip_enable,
           A5XX_GRAS_CL_CNTL_ZNEAR_CLIP_DISABLE |
           A5XX_GRAS_CL_CNTL_ZFAR_CLIP_DISABLE) |
      COND(rs->depth_clamp_enable, A5XX_GRAS_CL_CNTL_Z_CLAMP_ENABLE) |
      COND(!vp->depth_clip_negative_one_to_one, A5XX_GRAS_CL_CNTL_ZERO_GB_SCALE_Z);
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CL_CNTL, 1);
   tu_cs_emit(cs, gras_cl_cntl);

   if (tu_dbg_log_enabled()) {
      tu_dbg_log("A5_RAST cull=0x%x front=%u poly=%u dclip=%u dclamp=%u neg1=%u cl=0x%x",
                 rs->cull_mode, (unsigned)rs->front_face, (unsigned)rs->polygon_mode,
                 depth_clip_enable ? 1u : 0u, rs->depth_clamp_enable ? 1u : 0u,
                 vp->depth_clip_negative_one_to_one ? 1u : 0u, gras_cl_cntl);
   }

   enum adreno_pa_su_sc_draw polymode = tu5_polygon_mode(rs->polygon_mode);
   tu_cs_emit_pkt4(cs, REG_A5XX_PC_RASTER_CNTL, 1);
   tu_cs_emit(cs, A5XX_PC_RASTER_CNTL_POLYMODE_FRONT_PTYPE(polymode) |
                  A5XX_PC_RASTER_CNTL_POLYMODE_BACK_PTYPE(polymode) |
                  COND(rs->polygon_mode != VK_POLYGON_MODE_FILL,
                       A5XX_PC_RASTER_CNTL_POLYMODE_ENABLE));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_POINT_MINMAX, 2);
   tu_cs_emit(cs, A5XX_GRAS_SU_POINT_MINMAX_MIN(1.0f / 16.0f) |
                  A5XX_GRAS_SU_POINT_MINMAX_MAX(4092.0f));
   tu_cs_emit(cs, A5XX_GRAS_SU_POINT_SIZE(1.0f));
}

unsigned
tu5_ds_size(void)
{
   return 5;
}

void
tu5_emit_ds(struct tu_cs *cs,
            const struct vk_depth_stencil_state *ds,
            const struct vk_render_pass_state *rp)
{
   bool stencil_test_enable =
      ds->stencil.test_enable && (rp->attachments & MESA_VK_RP_ATTACHMENT_STENCIL_BIT);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_CONTROL, 1);
   tu_cs_emit(cs,
              COND(stencil_test_enable,
                   A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE |
                   A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF |
                   A5XX_RB_STENCIL_CONTROL_STENCIL_READ) |
              A5XX_RB_STENCIL_CONTROL_FUNC(tu6_compare_func(static_cast<VkCompareOp>(ds->stencil.front.op.compare))) |
              A5XX_RB_STENCIL_CONTROL_FAIL(tu6_stencil_op(static_cast<VkStencilOp>(ds->stencil.front.op.fail))) |
              A5XX_RB_STENCIL_CONTROL_ZPASS(tu6_stencil_op(static_cast<VkStencilOp>(ds->stencil.front.op.pass))) |
              A5XX_RB_STENCIL_CONTROL_ZFAIL(tu6_stencil_op(static_cast<VkStencilOp>(ds->stencil.front.op.depth_fail))) |
              A5XX_RB_STENCIL_CONTROL_FUNC_BF(tu6_compare_func(static_cast<VkCompareOp>(ds->stencil.back.op.compare))) |
              A5XX_RB_STENCIL_CONTROL_FAIL_BF(tu6_stencil_op(static_cast<VkStencilOp>(ds->stencil.back.op.fail))) |
              A5XX_RB_STENCIL_CONTROL_ZPASS_BF(tu6_stencil_op(static_cast<VkStencilOp>(ds->stencil.back.op.pass))) |
              A5XX_RB_STENCIL_CONTROL_ZFAIL_BF(tu6_stencil_op(static_cast<VkStencilOp>(ds->stencil.back.op.depth_fail))));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCILREFMASK, 2);
   tu_cs_emit(cs,
              A5XX_RB_STENCILREFMASK_STENCILREF(ds->stencil.front.reference) |
              A5XX_RB_STENCILREFMASK_STENCILMASK(ds->stencil.front.compare_mask) |
              A5XX_RB_STENCILREFMASK_STENCILWRITEMASK(ds->stencil.front.write_mask));
   tu_cs_emit(cs,
              A5XX_RB_STENCILREFMASK_BF_STENCILREF(ds->stencil.back.reference) |
              A5XX_RB_STENCILREFMASK_BF_STENCILMASK(ds->stencil.back.compare_mask) |
              A5XX_RB_STENCILREFMASK_BF_STENCILWRITEMASK(ds->stencil.back.write_mask));

   if (tu_dbg_log_enabled()) {
      tu_dbg_log("A5_STENCIL en=%u att=0x%x f=(op=%u ref=%u m=0x%x wm=0x%x) b=(op=%u ref=%u m=0x%x wm=0x%x)",
                 stencil_test_enable ? 1u : 0u, rp->attachments,
                 ds->stencil.front.op.compare, ds->stencil.front.reference,
                 ds->stencil.front.compare_mask, ds->stencil.front.write_mask,
                 ds->stencil.back.op.compare, ds->stencil.back.reference,
                 ds->stencil.back.compare_mask, ds->stencil.back.write_mask);
   }
}

unsigned
tu5_rb_depth_cntl_size(void)
{
   return 2;
}

void
tu5_emit_rb_depth_cntl(struct tu_cs *cs,
                       const struct vk_depth_stencil_state *ds,
                       const struct vk_render_pass_state *rp,
                       const struct vk_rasterization_state *rs)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_CNTL, 1);
   uint32_t val = 0;
   if (rp->attachments & MESA_VK_RP_ATTACHMENT_DEPTH_BIT) {
      enum adreno_compare_func zfunc = tu6_compare_func(ds->depth.compare_op);
      val = COND(ds->depth.test_enable, A5XX_RB_DEPTH_CNTL_Z_TEST_ENABLE) |
            COND(ds->depth.test_enable && ds->depth.write_enable,
                 A5XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE) |
            A5XX_RB_DEPTH_CNTL_ZFUNC(zfunc) |
            COND(rs->depth_clamp_enable, A5XX_RB_DEPTH_CNTL_Z_CLAMP_ENABLE) |
            COND(ds->depth.test_enable &&
                 zfunc != FUNC_NEVER && zfunc != FUNC_ALWAYS,
                 A5XX_RB_DEPTH_CNTL_Z_READ_ENABLE);
   }
   tu_cs_emit(cs, val);

   if (tu_dbg_log_enabled()) {
      tu_dbg_log("A5_DEPTH att=0x%x test=%u write=%u op=%u bnd=%u(%.3f,%.3f) clamp=%u rb=0x%x",
                 rp->attachments, ds->depth.test_enable ? 1u : 0u,
                 ds->depth.write_enable ? 1u : 0u, (unsigned)ds->depth.compare_op,
                 ds->depth.bounds_test.enable ? 1u : 0u,
                 ds->depth.bounds_test.min, ds->depth.bounds_test.max,
                 rs->depth_clamp_enable ? 1u : 0u, val);
   }
}
