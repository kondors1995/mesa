/*
 * Copyright © 2026 Freedreno / Turnip contributors
 * SPDX-License-Identifier: MIT
 */

#define A6XX_PERFCNTRS_XML

#include "tu5_clear_blit.h"

#include <string.h>

#include "ir3/ir3_nir.h"
#include "util/u_math.h"

#include "tu_clear_blit.h"
#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_formats.h"
#include "tu_image.h"
#include "tu_pass.h"
#include "tu_shader.h"
#include "tu5_formats.h"

#include "common/fd5_hw.h"
#include "common/freedreno_gpu_event.h"

static uint32_t
tu5_pack_float32_for_unorm(float val, int bits)
{
   uint32_t m = BITFIELD_MASK(bits);
   if (!(val > 0.0f))
      return 0;
   if (val >= 1.0f)
      return m;

   double bias = 1.0 / static_cast<double>(m + 1ull);
   double scaled = static_cast<double>(val) * static_cast<double>(m) + 0.5 + bias;
   return MIN2(static_cast<uint32_t>(floor(scaled)), m);
}

static float
tu5_quantize_depth_unorm(float val, int bits)
{
   return static_cast<float>(tu5_pack_float32_for_unorm(val, bits)) /
          static_cast<float>(BITFIELD_MASK(bits));
}

static void
tu5_desc_set_format(uint32_t *desc, enum a5xx_tex_fmt fmt)
{
   desc[0] &= ~A5XX_TEX_CONST_0_FMT__MASK;
   desc[0] |= A5XX_TEX_CONST_0_FMT(fmt);
}

static void
tu5_desc_set_swap(uint32_t *desc, enum a3xx_color_swap swap)
{
   desc[0] &= ~A5XX_TEX_CONST_0_SWAP__MASK;
   desc[0] |= A5XX_TEX_CONST_0_SWAP(swap);
}

static void
tu5_desc_set_identity_swiz(uint32_t *desc)
{
   desc[0] &= ~(A5XX_TEX_CONST_0_SWIZ_X__MASK |
                A5XX_TEX_CONST_0_SWIZ_Y__MASK |
                A5XX_TEX_CONST_0_SWIZ_Z__MASK |
                A5XX_TEX_CONST_0_SWIZ_W__MASK);
   desc[0] |= A5XX_TEX_CONST_0_SWIZ_X(A5XX_TEX_X) |
              A5XX_TEX_CONST_0_SWIZ_Y(A5XX_TEX_Y) |
              A5XX_TEX_CONST_0_SWIZ_Z(A5XX_TEX_Z) |
              A5XX_TEX_CONST_0_SWIZ_W(A5XX_TEX_W);
}

static uint8_t
tu5_aspect_write_mask(enum pipe_format format, VkImageAspectFlags aspect_mask)
{
   uint8_t mask = 0xf;
   if (format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
      if (aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT)
         mask = 0x7;
      else if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT)
         mask = 0x8;
   }
   return mask;
}

static struct tu5_native_format
tu5_blit_color_format(enum pipe_format format, bool linear)
{
   struct tu5_native_format fmt = tu5_format_color(format, linear);
   if (format == PIPE_FORMAT_Z24X8_UNORM ||
       format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
      fmt.fmt = RB5_R8G8B8A8_UNORM;
   }
   return fmt;
}

static struct tu5_native_format
tu5_blit_tex_format(enum pipe_format format, bool linear)
{
   struct tu5_native_format fmt = tu5_format_texture(format, linear);
   if (format == PIPE_FORMAT_Z24X8_UNORM ||
       format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
      fmt.fmt = TFMT5_8_8_8_8_UNORM;
   }
   return fmt;
}

static bool
tu5_is_d24s8(enum pipe_format format)
{
   return format == PIPE_FORMAT_Z24_UNORM_S8_UINT ||
          format == PIPE_FORMAT_Z24_UNORM_S8_UINT_AS_R8G8B8A8;
}

static void
tu5_fixup_src_format(enum pipe_format *src_format,
                     enum pipe_format dst_format,
                     struct tu5_native_format *fmt)
{
   if (*src_format == PIPE_FORMAT_S8_UINT && tu5_is_d24s8(dst_format)) {
      fmt->fmt = TFMT5_A8_UNORM;
      fmt->swap = WZYX;
      *src_format = PIPE_FORMAT_A8_UNORM;
   }
}

static void
tu5_fixup_dst_format(enum pipe_format src_format,
                     enum pipe_format *dst_format,
                     struct tu5_native_format *fmt)
{
   if (*dst_format == PIPE_FORMAT_S8_UINT && tu5_is_d24s8(src_format)) {
      *dst_format = PIPE_FORMAT_A8_UNORM;
      fmt->fmt = RB5_A8_UNORM;
      fmt->swap = WZYX;
   }
}

static enum global_shader
tu5_pick_blit_fs(uint32_t rts_mask,
                 bool clear,
                 bool z_scale,
                 bool swap_coords,
                 VkSampleCountFlagBits src_samples,
                 VkSampleCountFlagBits dst_samples)
{
   if (clear) {
      return static_cast<enum global_shader>(
         GLOBAL_SH_FS_CLEAR0 + util_bitcount(rts_mask));
   }

   if (z_scale)
      return swap_coords ? GLOBAL_SH_FS_BLIT_ZSCALE_SWAP_COORDS
                         : GLOBAL_SH_FS_BLIT_ZSCALE;
   if (swap_coords)
      return GLOBAL_SH_FS_BLIT_SWAP_COORDS;
   if (src_samples != VK_SAMPLE_COUNT_1_BIT) {
      return dst_samples == VK_SAMPLE_COUNT_1_BIT
                ? static_cast<enum global_shader>(GLOBAL_SH_FS_RESOLVE_MS2 +
                                                  util_logbase2(src_samples) - 1)
                : GLOBAL_SH_FS_COPY_MS;
   }
   return GLOBAL_SH_FS_BLIT;
}

static void
tu5_r3d_common(struct tu_cmd_buffer *cmd,
               struct tu_cs *cs,
               bool clear,
               uint32_t rts_mask,
               bool z_scale,
               bool swap_coords,
               VkSampleCountFlagBits src_samples,
               VkSampleCountFlagBits dst_samples)
{
   enum global_shader vs_id = clear ? GLOBAL_SH_VS_CLEAR : GLOBAL_SH_VS_BLIT;
   enum global_shader fs_id =
      tu5_pick_blit_fs(rts_mask, clear, z_scale, swap_coords,
                       src_samples, dst_samples);

   struct ir3_shader_variant *vs = cmd->device->global_shader_variants[vs_id];
   struct ir3_shader_variant *fs = cmd->device->global_shader_variants[fs_id];
   uint64_t vs_iova = cmd->device->global_shader_va[vs_id];
   uint64_t fs_iova = cmd->device->global_shader_va[fs_id];

   tu5_emit_program_config(cs, vs, fs, vs_iova, fs_iova);

   struct tu_pvtmem_config pvtmem = {};
   tu5_emit_variant(cs, MESA_SHADER_VERTEX, vs, &pvtmem, 0, vs_iova);
   tu5_emit_variant(cs, MESA_SHADER_FRAGMENT, fs, &pvtmem, 0, fs_iova);
   tu5_emit_vpc(cs, vs, fs);

   /* Ensure PC primitive stride matches VPC_CNTL_0 stride_in_vpc. */
   struct ir3_shader_linkage linkage = {
      .primid_loc = 0xff,
      .clip0_loc = 0xff,
      .clip1_loc = 0xff,
   };
   ir3_link_shaders(&linkage, vs, fs, true);
   uint32_t pos_regid = ir3_find_output_regid(vs, VARYING_SLOT_POS);
   if (VALIDREG(pos_regid))
      ir3_link_add(&linkage, VARYING_SLOT_POS, pos_regid, 0xf, linkage.max_loc);
   if (linkage.cnt == 0)
      ir3_link_add(&linkage, 0, 0, 0x1, linkage.max_loc);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_PRIMITIVE_CNTL, 1);
   tu_cs_emit(cs, A5XX_PC_PRIMITIVE_CNTL_STRIDE_IN_VPC(linkage.max_loc));

   /* REPL_MODE for varying with RECTLIST (2 vertices only). */
   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VARYING_INTERP_MODE(0), 1);
   tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VARYING_PS_REPL_MODE(0), 1);
   tu_cs_emit(cs, (2u << 2) | (1u << 0));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CL_CNTL, 1);
   tu_cs_emit(cs, 0x00000181);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_RASTER_CNTL, 1);
   tu_cs_emit(cs, 0x00000012);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_OVERRIDE, 1);
   tu_cs_emit(cs, A5XX_VPC_SO_OVERRIDE_SO_DISABLE);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(0) |
                  A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(0));
   tu_cs_emit(cs, A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_X(0x7fff) |
                  A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_Y(0x7fff));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(0) |
                  A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(0));
   tu_cs_emit(cs, A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_X(0x7fff) |
                  A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_Y(0x7fff));

   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_INDEX_OFFSET, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   if (rts_mask) {
      unsigned rts_count = util_last_bit(rts_mask);
      tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_OUTPUT_REG(0), rts_count);
      unsigned rt = 0;
      for (unsigned i = 0; i < rts_count; i++) {
         uint32_t out_regid = regid(63, 0);
         if (rts_mask & (1u << i))
            out_regid = ir3_find_output_regid(fs, FRAG_RESULT_DATA0 + rt++);
         tu_cs_emit(cs, A5XX_SP_FS_OUTPUT_REG_REGID(out_regid) |
                        COND(out_regid & HALF_REG_ID,
                             A5XX_SP_FS_OUTPUT_REG_HALF_PRECISION));
      }
   }

   tu5_emit_msaa(cs, dst_samples, false);
}

static void
tu5_r3d_src_common(struct tu_cs *cs,
                   const uint32_t *tex_const,
                   uint32_t offset_base,
                   VkFilter filter)
{
   uint32_t desc[12] = {};
   memcpy(desc, tex_const, 6 * sizeof(uint32_t));

   uint64_t addr = (static_cast<uint64_t>(desc[5] & 0x1ffffu) << 32) | desc[4];
   addr += offset_base;
   desc[4] = static_cast<uint32_t>(addr & 0xffffffffu);
   desc[5] = (desc[5] & ~0x1ffffu) | A5XX_TEX_CONST_5_BASE_HI(addr >> 32);

   enum a5xx_tex_filter f =
      (filter == VK_FILTER_NEAREST) ? A5XX_TEX_NEAREST : A5XX_TEX_LINEAR;

   const uint32_t samp[4] = {
      A5XX_TEX_SAMP_0_XY_MAG(f) |
      A5XX_TEX_SAMP_0_XY_MIN(f) |
      A5XX_TEX_SAMP_0_WRAP_S(A5XX_TEX_CLAMP_TO_EDGE) |
      A5XX_TEX_SAMP_0_WRAP_T(A5XX_TEX_CLAMP_TO_EDGE) |
      A5XX_TEX_SAMP_0_WRAP_R(A5XX_TEX_CLAMP_TO_EDGE),
      A5XX_TEX_SAMP_1_UNNORM_COORDS |
      A5XX_TEX_SAMP_1_MIPFILTER_LINEAR_FAR,
      0,
      0,
   };

   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 4);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(SB4_FS_TEX) |
                  CP_LOAD_STATE4_0_NUM_UNIT(1));
   tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                  CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER));
   tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
   tu_cs_emit_array(cs, samp, 4);

   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 12);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(SB4_FS_TEX) |
                  CP_LOAD_STATE4_0_NUM_UNIT(1));
   tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                  CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
   tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
   tu_cs_emit_array(cs, desc, 12);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_VS_TEX_COUNT, 4);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_FS_TEX_COUNT, 2);
   tu_cs_emit(cs, 1);
   tu_cs_emit(cs, 0);
}

static void
tu5_emit_mrt0(struct tu_cs *cs,
              uint32_t mrt_buf_info,
              uint32_t pitch,
              uint32_t array_pitch,
              uint64_t va)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_BUF_INFO(0), 5);
   tu_cs_emit(cs, mrt_buf_info);
   tu_cs_emit(cs, A5XX_RB_MRT_PITCH(pitch));
   tu_cs_emit(cs, A5XX_RB_MRT_ARRAY_PITCH(array_pitch));
   tu_cs_emit_qw(cs, va);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_FLAG_BUFFER_ADDR(0), 4);
   tu_cs_emit_qw(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_CNTL, 1);
   tu_cs_emit(cs, 0x8);
}

void
tu5_emit_blit_consts_load(struct tu_cmd_buffer *cmd,
                          struct tu_cs *cs,
                          bool is_frag,
                          uint32_t offset,
                          const void *consts,
                          uint32_t size_vec4)
{
   struct tu_cs_memory mem = {};
   VkResult result = tu_cs_alloc(&cmd->sub_cs, size_vec4, 4, &mem);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   memcpy(mem.map, consts, size_vec4 * 4 * sizeof(uint32_t));

   enum a4xx_state_block block = is_frag ? SB4_FS_SHADER : SB4_VS_SHADER;
   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(offset) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(block) |
                  CP_LOAD_STATE4_0_NUM_UNIT(size_vec4));
   tu_cs_emit_qw(cs, mem.iova |
                     static_cast<uint64_t>(CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS)));
}

void
tu5_r3d_src(struct tu_cmd_buffer *cmd,
            struct tu_cs *cs,
            const struct fdl6_view *iview,
            uint32_t layer,
            VkFilter filter,
            enum pipe_format dst_format)
{
   uint32_t desc[FDL6_TEX_CONST_DWORDS];
   memcpy(desc, iview->descriptor, sizeof(desc));

   enum pipe_format src_format = iview->format;
   bool src_linear = (desc[0] & A5XX_TEX_CONST_0_TILE_MODE__MASK) ==
                     A5XX_TEX_CONST_0_TILE_MODE(TILE5_LINEAR);
   struct tu5_native_format fmt = tu5_blit_tex_format(src_format, src_linear);
   bool is_s8_to_d24s8 =
      src_format == PIPE_FORMAT_S8_UINT && tu5_is_d24s8(dst_format);
   tu5_fixup_src_format(&src_format, dst_format, &fmt);
   tu5_desc_set_format(desc, static_cast<enum a5xx_tex_fmt>(fmt.fmt));
   if (is_s8_to_d24s8) {
      tu5_desc_set_swap(desc, static_cast<enum a3xx_color_swap>(fmt.swap));
      tu5_desc_set_identity_swiz(desc);
   }

   tu5_r3d_src_common(cs, desc, iview->layer_size * layer, filter);
}

void
tu5_r3d_src_buffer(struct tu_cmd_buffer *cmd,
                   struct tu_cs *cs,
                   enum pipe_format format,
                   uint64_t va,
                   uint32_t pitch,
                   uint32_t width,
                   uint32_t height,
                   enum pipe_format dst_format)
{
   uint32_t desc[12] = {};
   struct tu5_native_format fmt = tu5_blit_tex_format(format, true);
   bool is_s8_to_d24s8 =
      format == PIPE_FORMAT_S8_UINT && tu5_is_d24s8(dst_format);
   tu5_fixup_src_format(&format, dst_format, &fmt);

   uint32_t aligned_pitch = pitch ? align(pitch, 64) : 0;

   desc[0] = A5XX_TEX_CONST_0_TILE_MODE(TILE5_LINEAR) |
             COND(util_format_is_srgb(format), A5XX_TEX_CONST_0_SRGB) |
             A5XX_TEX_CONST_0_FMT(static_cast<enum a5xx_tex_fmt>(fmt.fmt)) |
             A5XX_TEX_CONST_0_SWAP(static_cast<enum a3xx_color_swap>(fmt.swap)) |
             A5XX_TEX_CONST_0_SWIZ_X(A5XX_TEX_X) |
             A5XX_TEX_CONST_0_SWIZ_Y(A5XX_TEX_Y) |
             A5XX_TEX_CONST_0_SWIZ_Z(A5XX_TEX_Z) |
             A5XX_TEX_CONST_0_SWIZ_W(A5XX_TEX_W);
   if (is_s8_to_d24s8)
      tu5_desc_set_identity_swiz(desc);

   desc[1] = A5XX_TEX_CONST_1_WIDTH(width) | A5XX_TEX_CONST_1_HEIGHT(height);
   desc[2] = A5XX_TEX_CONST_2_PITCHALIGN(0) |
             A5XX_TEX_CONST_2_PITCH(aligned_pitch) |
             A5XX_TEX_CONST_2_TYPE(A5XX_TEX_2D);
   desc[3] = 0;
   desc[4] = static_cast<uint32_t>(va & 0xffffffffu);
   desc[5] = A5XX_TEX_CONST_5_BASE_HI(va >> 32) | A5XX_TEX_CONST_5_DEPTH(1);

   tu5_r3d_src_common(cs, desc, 0, VK_FILTER_NEAREST);
}

void
tu5_r3d_src_load(struct tu_cmd_buffer *cmd,
                 struct tu_cs *cs,
                 const struct fdl6_view *iview,
                 uint32_t layer,
                 VkFilter filter)
{
   uint32_t desc[FDL6_TEX_CONST_DWORDS];
   memcpy(desc, iview->descriptor, sizeof(desc));

   enum pipe_format format = iview->format;
   if (format == PIPE_FORMAT_X24S8_UINT ||
       format == PIPE_FORMAT_Z24X8_UNORM ||
       format == PIPE_FORMAT_Z24_UNORM_S8_UINT) {
      tu5_desc_set_format(desc, TFMT5_8_8_8_8_UNORM);
   }

   tu5_desc_set_identity_swiz(desc);
   tu5_r3d_src_common(cs, desc, iview->layer_size * layer, filter);
}

void
tu5_r3d_dst(struct tu_cs *cs,
            const struct fdl6_view *iview,
            uint32_t layer,
            enum pipe_format src_format)
{
   uint32_t mrt_buf_info = iview->RB_MRT_BUF_INFO;
   enum pipe_format dst_format = iview->format;

   if (dst_format == PIPE_FORMAT_R9G9B9E5_FLOAT &&
       src_format == PIPE_FORMAT_R32_UINT) {
      mrt_buf_info &= ~(A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK |
                        A5XX_RB_MRT_BUF_INFO_COLOR_SWAP__MASK);
      mrt_buf_info |=
         A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(RB5_R32_UINT) |
         A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX);
   } else if (dst_format == PIPE_FORMAT_S8_UINT && tu5_is_d24s8(src_format)) {
      mrt_buf_info &= ~(A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK |
                        A5XX_RB_MRT_BUF_INFO_COLOR_SWAP__MASK);
      mrt_buf_info |=
         A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(RB5_A8_UNORM) |
         A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX);
   }

   tu5_emit_mrt0(cs, mrt_buf_info, iview->pitch, iview->layer_size,
                 tu_layer_address(iview, layer));
}

void
tu5_r3d_dst_buffer(struct tu_cs *cs,
                   enum pipe_format format,
                   uint64_t va,
                   uint32_t pitch,
                   enum pipe_format src_format)
{
   struct tu5_native_format fmt = tu5_blit_color_format(format, true);
   tu5_fixup_dst_format(src_format, &format, &fmt);

   uint32_t mrt_buf_info =
      A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(static_cast<enum a5xx_color_fmt>(fmt.fmt)) |
      A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(TILE5_LINEAR) |
      A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(static_cast<enum a3xx_color_swap>(fmt.swap)) |
      COND(util_format_is_srgb(format), A5XX_RB_MRT_BUF_INFO_COLOR_SRGB);

   tu5_emit_mrt0(cs, mrt_buf_info, pitch, 0, va);
}

void
tu5_r3d_setup(struct tu_cmd_buffer *cmd,
              struct tu_cs *cs,
              enum pipe_format src_format,
              enum pipe_format dst_format,
              VkImageAspectFlags aspect_mask,
              bool z_scale,
              bool clear,
              bool swap_coords,
              VkSampleCountFlagBits src_samples,
              VkSampleCountFlagBits dst_samples,
              bool outside_pass)
{
   struct tu5_native_format fmt = tu5_blit_color_format(dst_format, true);
   tu5_fixup_dst_format(src_format, &dst_format, &fmt);

   if (!cmd->state.pass || outside_pass) {
      tu_emit_cache_flush_ccu<A5XX>(cmd, cs, TU_CMD_CCU_SYSMEM);
      tu5_emit_rb_ccu_cntl(cs, cmd->device->physical_device, false);
      tu5_emit_cache_invalidate(cs);
      tu5_emit_window_scissor(cs, 0, 0, 0x3fff, 0x3fff);
      tu5_emit_bin_size(cs, 0, 0, true);
      tu5_emit_window_offset(cs, 0, 0);
   } else {
      tu5_emit_bin_size(cs, 0, 0, true);
   }

   tu5_r3d_common(cmd, cs, clear, 1, z_scale, swap_coords,
                  src_samples, dst_samples);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_OUTPUT_CNTL, 1);
   tu_cs_emit(cs, A5XX_SP_FS_OUTPUT_CNTL_MRT(1) |
                  A5XX_SP_FS_OUTPUT_CNTL_DEPTH_REGID(0xfc) |
                  A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(0xfc));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_FS_OUTPUT_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_FS_OUTPUT_CNTL_MRT(1));

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_BLEND_CNTL, 1);
   tu_cs_emit(cs, A5XX_SP_BLEND_CNTL_UNK8);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLEND_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_BLEND_CNTL_SAMPLE_MASK(0xffff));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_PLANE_CNTL, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_CONTROL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCILREFMASK, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   uint8_t write_mask = tu5_aspect_write_mask(dst_format, aspect_mask);
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_COMPONENTS, 1);
   tu_cs_emit(cs, A5XX_RB_RENDER_COMPONENTS_RT0(write_mask));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_CONTROL(0), 2);
   tu_cs_emit(cs, A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(write_mask));
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_MRT_REG(0), 1);
   tu_cs_emit(cs,
              A5XX_SP_FS_MRT_REG_COLOR_FORMAT(static_cast<enum a5xx_color_fmt>(fmt.fmt)) |
              COND(util_format_is_pure_sint(dst_format), A5XX_SP_FS_MRT_REG_COLOR_SINT) |
              COND(util_format_is_pure_uint(dst_format), A5XX_SP_FS_MRT_REG_COLOR_UINT) |
              COND(util_format_is_srgb(dst_format), A5XX_SP_FS_MRT_REG_COLOR_SRGB));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_CNTL, 1);
   tu_cs_emit(cs, 0x8);

   if (cmd->state.predication_active) {
      tu_cs_emit_pkt7(cs, CP_DRAW_PRED_ENABLE_LOCAL, 1);
      tu_cs_emit(cs, 0);
   }
}

void
tu5_r3d_teardown(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   if (cmd->state.predication_active) {
      tu_cs_emit_pkt7(cs, CP_DRAW_PRED_ENABLE_LOCAL, 1);
      tu_cs_emit(cs, 1);
   }
}

static void
tu5_r3d_clear_rects(struct tu_cmd_buffer *cmd,
                    struct tu_cs *cs,
                    uint32_t clear_rts,
                    uint32_t clear_components,
                    const uint32_t clear_value[MAX_RTS][4],
                    bool z_clear,
                    float z_clear_val,
                    bool s_clear,
                    uint8_t s_clear_val,
                    uint32_t rect_count,
                    const VkClearRect *rects)
{
   const struct tu_subpass *subpass = cmd->state.subpass;
   const uint32_t mrt_count = subpass->color_count;

   if (clear_rts == 0 && !z_clear && !s_clear)
      return;

   tu_cs_emit_pkt7(cs, CP_SET_DRAW_STATE, 3 * (TU_DRAW_STATE_COUNT - 2));
   for (uint32_t i = 0; i < TU_DRAW_STATE_COUNT; i++) {
      if (i == TU_DRAW_STATE_INPUT_ATTACHMENTS_GMEM ||
          i == TU_DRAW_STATE_INPUT_ATTACHMENTS_SYSMEM)
         continue;
      tu_cs_emit(cs, CP_SET_DRAW_STATE__0_GROUP_ID(i) |
                     CP_SET_DRAW_STATE__0_DISABLE);
      tu_cs_emit_qw(cs, 0);
   }
   cmd->state.dirty |= TU_CMD_DIRTY_DRAW_STATE;

   tu5_r3d_common(cmd, cs, true, clear_rts, false, false,
                  subpass->samples, subpass->samples);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_OUTPUT_CNTL, 1);
   tu_cs_emit(cs, A5XX_SP_FS_OUTPUT_CNTL_MRT(mrt_count) |
                  A5XX_SP_FS_OUTPUT_CNTL_DEPTH_REGID(0xfc) |
                  A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(0xfc));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_FS_OUTPUT_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_FS_OUTPUT_CNTL_MRT(mrt_count));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_COMPONENTS, 1);
   tu_cs_emit(cs, clear_components);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_BLEND_CNTL, 1);
   tu_cs_emit(cs, A5XX_SP_BLEND_CNTL_UNK8);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLEND_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_BLEND_CNTL_INDEPENDENT_BLEND |
                  A5XX_RB_BLEND_CNTL_SAMPLE_MASK(0xffff));

   for (uint32_t i = 0; i < mrt_count; i++) {
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_CONTROL(i), 2);
      tu_cs_emit(cs, A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(
                        (clear_rts & (1u << i)) ? 0xf : 0));
      tu_cs_emit(cs, 0);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_PLANE_CNTL, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, COND(z_clear,
                       A5XX_RB_DEPTH_CNTL_Z_TEST_ENABLE |
                       A5XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE) |
                  A5XX_RB_DEPTH_CNTL_ZFUNC(FUNC_ALWAYS));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CL_VIEWPORT_ZCLAMP_NEAR_0, 2);
   tu_cs_emit(cs, fui(0.0f));
   tu_cs_emit(cs, fui(1.0f));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_VIEWPORT_ZCLAMP_NEAR, 2);
   tu_cs_emit(cs, fui(0.0f));
   tu_cs_emit(cs, fui(1.0f));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_CONTROL, 1);
   tu_cs_emit(cs, COND(s_clear, A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE) |
                  A5XX_RB_STENCIL_CONTROL_FUNC(FUNC_ALWAYS) |
                  A5XX_RB_STENCIL_CONTROL_ZPASS(STENCIL_REPLACE));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCILREFMASK, 2);
   tu_cs_emit(cs, A5XX_RB_STENCILREFMASK_STENCILREF(s_clear_val) |
                  A5XX_RB_STENCILREFMASK_STENCILMASK(0xff) |
                  A5XX_RB_STENCILREFMASK_STENCILWRITEMASK(0xff));
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_CNTL, 1);
   tu_cs_emit(cs, 0x8);

   unsigned num_rts = util_bitcount(clear_rts);
   uint32_t packed_clear_value[MAX_RTS][4];
   uint32_t idx = 0;
   u_foreach_bit (b, clear_rts) {
      memcpy(&packed_clear_value[idx], &clear_value[b], 4 * sizeof(uint32_t));
      idx++;
   }

   if (num_rts > 0)
      tu5_emit_blit_consts_load(cmd, cs, true, 0, packed_clear_value, num_rts);

   for (uint32_t i = 0; i < rect_count; i++) {
      assert(!subpass->multiview_mask || rects[i].baseArrayLayer == 0);
      for_each_layer (layer, subpass->multiview_mask, rects[i].layerCount) {
         const float coords[] = {
            static_cast<float>(rects[i].rect.offset.x),
            static_cast<float>(rects[i].rect.offset.y),
            z_clear_val,
            uif(rects[i].baseArrayLayer + layer),
            static_cast<float>(rects[i].rect.offset.x + rects[i].rect.extent.width),
            static_cast<float>(rects[i].rect.offset.y + rects[i].rect.extent.height),
            z_clear_val,
            1.0f,
         };

         tu5_emit_blit_consts_load(cmd, cs, false, 0, coords, 2);

         tu_cs_emit_pkt7(cs, CP_DRAW_INDX_OFFSET, 3);
         tu_cs_emit(cs, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(DI_PT_RECTLIST) |
                        CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_AUTO_INDEX) |
                        CP_DRAW_INDX_OFFSET_0_VIS_CULL(IGNORE_VISIBILITY));
         tu_cs_emit(cs, 1);
         tu_cs_emit(cs, 2);
      }
   }
}

void
tu5_clear_sysmem_attachments(struct tu_cmd_buffer *cmd,
                             uint32_t attachment_count,
                             const VkClearAttachment *attachments,
                             uint32_t rect_count,
                             const VkClearRect *rects)
{
   const struct tu_subpass *subpass = cmd->state.subpass;
   struct tu_cs *cs = &cmd->draw_cs;
   uint32_t clear_value[MAX_RTS][4];
   float z_clear_val = 0.0f;
   uint8_t s_clear_val = 0;
   uint32_t clear_rts = 0, clear_components = 0;
   bool z_clear = false;
   bool s_clear = false;

   for (uint32_t i = 0; i < attachment_count; i++) {
      uint32_t a;
      if (attachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
         uint32_t c = attachments[i].colorAttachment;
         a = subpass->color_attachments[c].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         uint32_t remapped = cmd->vk.dynamic_graphics_state.cal.color_map[c];
         if (remapped == MESA_VK_ATTACHMENT_UNUSED)
            continue;

         clear_rts |= 1u << remapped;
         clear_components |= 0xfu << (remapped * 4);
         memcpy(clear_value[remapped], &attachments[i].clearValue,
                4 * sizeof(uint32_t));
      } else {
         a = subpass->depth_stencil_attachment.attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         if (attachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
            z_clear = true;
            z_clear_val = attachments[i].clearValue.depthStencil.depth;
            if (cmd->state.pass->attachments[a].format == VK_FORMAT_D16_UNORM)
               z_clear_val = tu5_quantize_depth_unorm(z_clear_val, 16);
         }

         if (attachments[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
            s_clear = true;
            s_clear_val = attachments[i].clearValue.depthStencil.stencil & 0xff;
         }
      }
   }

   tu5_r3d_clear_rects(cmd, cs, clear_rts, clear_components, clear_value,
                       z_clear, z_clear_val, s_clear, s_clear_val,
                       rect_count, rects);
}

static enum pipe_format
tu5_gmem_restore_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_Z24X8_UNORM:
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_X8Z24_UNORM:
      return PIPE_FORMAT_R8G8B8A8_UNORM;
   case PIPE_FORMAT_Z16_UNORM:
      return PIPE_FORMAT_R8G8_UNORM;
   case PIPE_FORMAT_Z32_FLOAT:
      return PIPE_FORMAT_R32_FLOAT;
   case PIPE_FORMAT_S8_UINT:
      return PIPE_FORMAT_R8_UNORM;
   default:
      return format;
   }
}

static void
tu5_emit_blit_state(struct tu_cs *cs,
                    uint32_t tile_w,
                    uint32_t tile_h,
                    VkSampleCountFlagBits samples,
                    bool bypass)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_CNTL_WIDTH(tile_w) |
                  A5XX_RB_CNTL_HEIGHT(tile_h) |
                  COND(bypass, A5XX_RB_CNTL_BYPASS));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu5_emit_msaa(cs, samples, false);
}

static void
tu5_emit_blit_dst_and_resolve(struct tu_cmd_buffer *cmd,
                              struct tu_cs *cs,
                              enum a5xx_blit_buf buf,
                              uint32_t resolve_cntl_3,
                              uint64_t dst_addr,
                              uint32_t dst_pitch,
                              uint32_t dst_layer_size,
                              uint32_t clear_cntl)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLIT_FLAG_DST, 4);
   tu_cs_emit_qw(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RESOLVE_CNTL_3, 5);
   tu_cs_emit(cs, resolve_cntl_3);
   tu_cs_emit_qw(cs, dst_addr);
   tu_cs_emit(cs, A5XX_RB_BLIT_DST_PITCH(dst_pitch));
   tu_cs_emit(cs, A5XX_RB_BLIT_DST_ARRAY_PITCH(dst_layer_size));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLIT_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_BLIT_CNTL_BUF(buf));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_CNTL, 1);
   tu_cs_emit(cs, clear_cntl);

   tu_emit_event_write<A5XX>(cmd, cs, FD_CCU_RESOLVE);
}

static void
tu5_emit_mem2gmem_mrt0_blit(struct tu_cmd_buffer *cmd,
                            struct tu_cs *cs,
                            uint32_t buf_info,
                            uint64_t src_addr,
                            uint32_t src_pitch,
                            uint32_t src_layer_size,
                            uint64_t gmem_dst,
                            uint32_t gmem_pitch,
                            uint32_t gmem_layer_size)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_BUF_INFO(0), 5);
   tu_cs_emit(cs, buf_info);
   tu_cs_emit(cs, A5XX_RB_MRT_PITCH(src_pitch));
   tu_cs_emit(cs, A5XX_RB_MRT_ARRAY_PITCH(src_layer_size));
   tu_cs_emit_qw(cs, src_addr);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_FLAG_BUFFER_ADDR(0), 4);
   tu_cs_emit_qw(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu5_emit_blit_dst_and_resolve(cmd, cs, BLIT_MRT0, 0, gmem_dst, gmem_pitch,
                                 gmem_layer_size, 0);
}

void
tu5_load_gmem_attachment(struct tu_cmd_buffer *cmd,
                         struct tu_cs *cs,
                         uint32_t a,
                         uint32_t gmem_a,
                         bool force_load)
{
   const struct tu_image_view *iview = cmd->state.attachments[a];
   const struct tu_render_pass_attachment *att =
      &cmd->state.pass->attachments[gmem_a];
   const uint32_t tile_w = cmd->state.tiling->tile0.width;
   const uint32_t tile_h = cmd->state.tiling->tile0.height;
   const bool is_zs = vk_format_is_depth_or_stencil(att->format);
   const bool is_d32s8 = att->format == VK_FORMAT_D32_SFLOAT_S8_UINT;

   if (is_d32s8) {
      if (!att->load && !att->load_stencil && !force_load)
         return;
   } else if (is_zs) {
      if (!att->load && !att->load_stencil && !force_load)
         return;
   } else if (!att->load && !force_load) {
      return;
   }

   /* mem2gmem transfers sysmem -> GMEM via RB_MRT -> RB_BLIT_DST in bypass. */
   tu5_emit_blit_state(cs, tile_w, tile_h, att->samples, true);

   if (is_d32s8) {
      if (att->load || force_load) {
         const struct fdl6_view *dview = tu_image_view_fdl_view(iview, false);
         enum a5xx_tile_mode tile_mode = (enum a5xx_tile_mode)
            fdl_tile_mode(&iview->image->layout[0], iview->vk.base_mip_level);
         struct tu5_native_format fmt =
            tu5_format_color(PIPE_FORMAT_R32_FLOAT, tile_mode == TILE5_LINEAR);
         uint32_t buf_info =
            A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT((enum a5xx_color_fmt) fmt.fmt) |
            A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(tile_mode) |
            A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX);
         uint32_t gmem_pitch = tile_w * 4 * att->samples;
         uint32_t gmem_layer_size = gmem_pitch * tile_h;

         for_each_layer (i, att->used_views, cmd->state.framebuffer->layers) {
            tu5_emit_mem2gmem_mrt0_blit(cmd, cs, buf_info,
                                        tu_layer_address(dview, i),
                                        dview->pitch, dview->layer_size,
                                        tu_attachment_gmem_offset(cmd, att, i),
                                        gmem_pitch, gmem_layer_size);
         }
      }
      if (att->load_stencil || force_load) {
         const struct fdl6_view *sview = tu_image_view_fdl_view(iview, true);
         enum a5xx_tile_mode tile_mode = (enum a5xx_tile_mode)
            fdl_tile_mode(&iview->image->layout[1], iview->vk.base_mip_level);
         struct tu5_native_format fmt =
            tu5_format_color(PIPE_FORMAT_R8_UNORM, tile_mode == TILE5_LINEAR);
         uint32_t buf_info =
            A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT((enum a5xx_color_fmt) fmt.fmt) |
            A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(tile_mode) |
            A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX);
         uint32_t gmem_pitch = tile_w * att->samples;
         uint32_t gmem_layer_size = gmem_pitch * tile_h;

         for_each_layer (i, att->used_views, cmd->state.framebuffer->layers) {
            tu5_emit_mem2gmem_mrt0_blit(cmd, cs, buf_info,
                                        tu_layer_address(sview, i),
                                        sview->pitch, sview->layer_size,
                                        tu_attachment_gmem_offset_stencil(cmd, att, i),
                                        gmem_pitch, gmem_layer_size);
         }
      }
      return;
   }

   uint32_t buf_info;
   const struct fdl6_view *src_view;
   if (is_zs) {
      src_view = tu_image_view_fdl_view(iview, false);
      enum pipe_format restore_fmt =
         tu5_gmem_restore_format(vk_format_to_pipe_format(att->format));
      enum a5xx_tile_mode tile_mode = (enum a5xx_tile_mode)
         fdl_tile_mode(&iview->image->layout[0], iview->vk.base_mip_level);
      struct tu5_native_format fmt =
         tu5_format_color(restore_fmt, tile_mode == TILE5_LINEAR);
      buf_info =
         A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT((enum a5xx_color_fmt) fmt.fmt) |
         A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(tile_mode) |
         A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX);
   } else {
      src_view = &iview->view;
      buf_info = src_view->RB_MRT_BUF_INFO;
   }

   uint32_t gmem_pitch = tile_w * att->cpp;
   uint32_t gmem_layer_size = gmem_pitch * tile_h;
   for_each_layer (i, att->used_views, cmd->state.framebuffer->layers) {
      tu5_emit_mem2gmem_mrt0_blit(cmd, cs, buf_info,
                                  tu_layer_address(src_view, i),
                                  src_view->pitch, src_view->layer_size,
                                  tu_attachment_gmem_offset(cmd, att, i),
                                  gmem_pitch, gmem_layer_size);
   }
}

static void
tu5_emit_fast_clear_zs(struct tu_cs *cs,
                       enum a5xx_depth_format dfmt,
                       uint64_t base,
                       uint32_t pitch,
                       uint32_t layer_size)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(dfmt));
   tu_cs_emit_qw(cs, base);
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_PITCH(pitch));
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(layer_size));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
   tu_cs_emit(cs, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(dfmt));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
   tu_cs_emit_qw(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 1);
   tu_cs_emit(cs, 0);
}

static void
tu5_emit_fast_clear_mrt0(struct tu_cs *cs,
                         uint32_t buf_info,
                         uint64_t base,
                         uint32_t pitch,
                         uint32_t layer_size)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_BUF_INFO(0), 5);
   tu_cs_emit(cs, buf_info);
   tu_cs_emit(cs, A5XX_RB_MRT_PITCH(pitch));
   tu_cs_emit(cs, A5XX_RB_MRT_ARRAY_PITCH(layer_size));
   tu_cs_emit_qw(cs, base);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_FLAG_BUFFER_ADDR(0), 4);
   tu_cs_emit_qw(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
}

static void
tu5_emit_fast_clear_resolve(struct tu_cmd_buffer *cmd,
                            struct tu_cs *cs,
                            uint32_t clear_mask,
                            const uint32_t clear_vals[4])
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
                  A5XX_RB_CLEAR_CNTL_MASK(clear_mask));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
   tu_cs_emit_array(cs, clear_vals, 4);

   tu_emit_event_write<A5XX>(cmd, cs, FD_CCU_RESOLVE);
}

bool
tu5_clear_sysmem_attachment(struct tu_cmd_buffer *cmd,
                            struct tu_cs *cs,
                            uint32_t a)
{
   const struct tu_render_pass_attachment *att =
      &cmd->state.pass->attachments[a];
   const struct tu_image_view *iview = cmd->state.attachments[a];
   const struct tu_framebuffer *fb = cmd->state.framebuffer;

   if (!att->clear_mask)
      return true;

   if (cmd->state.per_layer_render_area)
      return false;

   const VkRect2D *area = &cmd->state.render_areas[0];
   if (area->offset.x != 0 || area->offset.y != 0 ||
       area->extent.width != fb->width || area->extent.height != fb->height ||
       area->extent.width == 0 || area->extent.height == 0)
      return false;

   if (vk_format_is_depth_or_stencil(att->format) ||
       att->format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
      return false;

   uint32_t clear_vals[4] = {};
   tu_pack_blit_clear_value(&cmd->state.clear_values[a],
                            vk_format_to_pipe_format(att->format),
                            clear_vals);

   tu5_emit_blit_state(cs, 0, 0, att->samples, true);
   tu5_emit_window_scissor(cs, 0, 0, area->extent.width - 1,
                           area->extent.height - 1);

   tu_cs_emit_pkt4(cs, REG_A5XX_VSC_RESOLVE_CNTL, 2);
   tu_cs_emit(cs, A5XX_VSC_RESOLVE_CNTL_X(area->extent.width) |
                  A5XX_VSC_RESOLVE_CNTL_Y(area->extent.height));
   tu_cs_emit(cs, 0);

   bool is_zs = vk_format_is_depth_or_stencil(att->format);
   enum a5xx_depth_format dfmt = DEPTH5_NONE;
   uint32_t clear_mask = 0xf;
   if (is_zs) {
      dfmt = (enum a5xx_depth_format)
         tu5_pipe2depth(vk_format_to_pipe_format(att->format));
      clear_mask =
         ((att->clear_mask & VK_IMAGE_ASPECT_DEPTH_BIT) ? 0x1 : 0) |
         ((att->clear_mask & VK_IMAGE_ASPECT_STENCIL_BIT) ? 0x2 : 0);
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLIT_CNTL, 1);
      tu_cs_emit(cs, A5XX_RB_BLIT_CNTL_BUF(BLIT_ZS));
   } else {
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLIT_CNTL, 1);
      tu_cs_emit(cs, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));
   }

   for_each_layer (i, att->used_views, fb->layers) {
      if (is_zs) {
         tu5_emit_fast_clear_zs(cs, dfmt, tu_layer_address(&iview->view, i),
                                iview->view.pitch, iview->view.layer_size);
      } else {
         tu5_emit_fast_clear_mrt0(cs, iview->view.RB_MRT_BUF_INFO,
                                  tu_layer_address(&iview->view, i),
                                  iview->view.pitch, iview->view.layer_size);
      }

      tu5_emit_fast_clear_resolve(cmd, cs, clear_mask, clear_vals);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_CNTL, 1);
   tu_cs_emit(cs, 0);
   return true;
}

void
tu5_clear_gmem_attachment(struct tu_cmd_buffer *cmd,
                          struct tu_cs *cs,
                          bool per_layer_render_area,
                          uint32_t a)
{
   const struct tu_render_pass_attachment *att =
      &cmd->state.pass->attachments[a];
   const struct tu_image_view *iview = cmd->state.attachments[a];
   const struct tu_subpass *subpass = cmd->state.subpass;
   const struct tu_tiling_config *tiling = cmd->state.tiling;

   if (!att->clear_mask)
      return;

   bool use_r3d =
      att->format == VK_FORMAT_D32_SFLOAT ||
      att->format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
      att->format == VK_FORMAT_S8_UINT ||
      per_layer_render_area;

   const VkClearValue *att_clear = &cmd->state.clear_values[a];

   if (!use_r3d) {
      uint32_t clear_vals[4] = {};
      tu_pack_blit_clear_value(att_clear,
                               vk_format_to_pipe_format(att->format),
                               clear_vals);

      /* GMEM fast-clear targets RB_MRT / RB_DEPTH_BUFFER directly, not bypass. */
      tu5_emit_blit_state(cs, tiling->tile0.width, tiling->tile0.height,
                          att->samples, false);

      enum a5xx_blit_buf buf;
      uint32_t clear_mask = 0xf;
      bool is_zs = vk_format_is_depth_or_stencil(att->format);
      enum a5xx_depth_format dfmt = DEPTH5_NONE;
      uint32_t zs_pitch = 0, zs_layer_size = 0;
      uint32_t mrt_buf_info = 0, mrt_pitch = 0, mrt_layer_size = 0;
      if (is_zs) {
         dfmt = (enum a5xx_depth_format)
            tu5_pipe2depth(vk_format_to_pipe_format(att->format));
         zs_pitch = tiling->tile0.width * att->cpp;
         zs_layer_size = zs_pitch * tiling->tile0.height;
         buf = BLIT_ZS;
         clear_mask =
            ((att->clear_mask & VK_IMAGE_ASPECT_DEPTH_BIT) ? 0x1 : 0) |
            ((att->clear_mask & VK_IMAGE_ASPECT_STENCIL_BIT) ? 0x2 : 0);
      } else {
         mrt_buf_info = iview->view.RB_MRT_BUF_INFO;
         mrt_buf_info &= ~A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__MASK;
         mrt_buf_info |= A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(TILE5_2) | 0x800;
         mrt_pitch = tiling->tile0.width * att->cpp;
         mrt_layer_size = mrt_pitch * tiling->tile0.height;
         buf = BLIT_MRT0;
      }

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLIT_CNTL, 1);
      tu_cs_emit(cs, A5XX_RB_BLIT_CNTL_BUF(buf));

      for_each_layer (i, att->used_views, cmd->state.framebuffer->layers) {
         if (is_zs) {
            tu5_emit_fast_clear_zs(cs, dfmt, tu_attachment_gmem_offset(cmd, att, i),
                                   zs_pitch, zs_layer_size);
         } else {
            tu5_emit_fast_clear_mrt0(cs, mrt_buf_info,
                                     tu_attachment_gmem_offset(cmd, att, i),
                                     mrt_pitch, mrt_layer_size);
         }

         tu5_emit_fast_clear_resolve(cmd, cs, clear_mask, clear_vals);
      }

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_CNTL, 1);
      tu_cs_emit(cs, 0);
      return;
   }

   /* Fallback to 3D clear draw for Z32 / D32S8 / S8 or per-layer render areas:
    * bind each layer's GMEM target and render clipped rects.
    */
   tu5_emit_bin_size(cs, tiling->tile0.width, tiling->tile0.height, false);
   tu5_emit_render_cntl(cmd, subpass, cs, false);

   uint32_t clear_value[MAX_RTS][4] = {};
   uint32_t clear_rts = 0, clear_components = 0;
   bool z_clear = false, s_clear = false;
   float z_clear_val = 0.0f;
   uint8_t s_clear_val = 0;

   if (vk_format_is_depth_or_stencil(att->format)) {
      if (att->clear_mask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         z_clear = true;
         z_clear_val = att_clear->depthStencil.depth;
         if (att->format == VK_FORMAT_D16_UNORM)
            z_clear_val = tu5_quantize_depth_unorm(z_clear_val, 16);
      }
      if (att->clear_mask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         s_clear = true;
         s_clear_val = att_clear->depthStencil.stencil & 0xff;
      }
   } else {
      for (uint32_t i = 0; i < subpass->color_count; i++) {
         if (subpass->color_attachments[i].attachment != a)
            continue;
         unsigned remapped = cmd->vk.dynamic_graphics_state.cal.color_map[i];
         if (remapped == MESA_VK_ATTACHMENT_UNUSED)
            continue;
         clear_rts |= 1u << remapped;
         clear_components |= 0xfu << (remapped * 4);
         memcpy(clear_value[remapped], &att_clear->color,
                4 * sizeof(uint32_t));
      }
   }

   for_each_layer (i, att->used_views, cmd->state.framebuffer->layers) {
      tu5_emit_zs(cmd, subpass, cs, true, i);
      tu5_emit_mrt(cmd, subpass, cs, true, i);

      VkClearRect rect = {
         .rect = cmd->state.render_areas[per_layer_render_area ? i : 0],
         .baseArrayLayer = 0,
         .layerCount = 1,
      };
      tu5_r3d_clear_rects(cmd, cs, clear_rts, clear_components, clear_value,
                          z_clear, z_clear_val, s_clear, s_clear_val, 1, &rect);
   }
}

void
tu5_store_gmem_attachment(struct tu_cmd_buffer *cmd,
                          struct tu_cs *cs,
                          uint32_t a,
                          uint32_t gmem_a,
                          uint32_t layers,
                          uint32_t layer_mask,
                          bool per_layer_render_area)
{
   struct tu_render_pass_attachment *dst = &cmd->state.pass->attachments[a];
   const struct tu_image_view *dst_iview = cmd->state.attachments[a];
   struct tu_render_pass_attachment *src = &cmd->state.pass->attachments[gmem_a];
   const struct tu_image_view *src_iview = cmd->state.attachments[gmem_a];

   bool resolve = (a != gmem_a);
   if (!resolve && !src->store && !src->store_stencil)
      return;

   if (resolve) {
      if (src == dst)
         return;
      if (dst->clear_mask)
         return;
      assert(!(src->format == VK_FORMAT_D24_UNORM_S8_UINT &&
               dst->format == VK_FORMAT_S8_UINT));
   }

   const uint32_t tile_w = cmd->state.tiling->tile0.width;
   const uint32_t tile_h = cmd->state.tiling->tile0.height;
   const bool msaa_resolve = resolve && src->samples > dst->samples;
   const uint32_t clear_cntl =
      COND(msaa_resolve, A5XX_RB_CLEAR_CNTL_MSAA_RESOLVE);
   const bool is_zs = vk_format_is_depth_or_stencil(src->format);
   const bool is_d32s8 = src->format == VK_FORMAT_D32_SFLOAT_S8_UINT;

   /* gmem2mem resolves GMEM -> RB_BLIT_DST sysmem without RB_CNTL_BYPASS. */
   tu5_emit_blit_state(cs, tile_w, tile_h, src->samples, false);

   if (is_d32s8) {
      enum a5xx_depth_format fmt = (enum a5xx_depth_format)
         tu5_pipe2depth(vk_format_to_pipe_format(src->format));
      uint32_t depth_pitch = tile_w * 4 * src->samples;
      uint32_t depth_layer_size = depth_pitch * tile_h;
      uint32_t stencil_pitch = tile_w * src->samples;
      uint32_t stencil_layer_size = stencil_pitch * tile_h;

      bool store_d = resolve ? (dst->format != VK_FORMAT_S8_UINT) : src->store;
      bool store_s = resolve ? (dst->format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                                dst->format == VK_FORMAT_S8_UINT)
                             : src->store_stencil;

      const struct fdl6_view *dview = tu_image_view_fdl_view(dst_iview, false);
      const struct fdl6_view *sview = tu_image_view_fdl_view(dst_iview, true);
      bool d_tiled =
         fdl_tile_mode(&dst_iview->image->layout[0], dst_iview->vk.base_mip_level) != 0;
      bool s_tiled =
         fdl_tile_mode(&dst_iview->image->layout[dst->format == VK_FORMAT_D32_SFLOAT_S8_UINT ? 1 : 0],
                       dst_iview->vk.base_mip_level) != 0;

      for_each_layer (i, layer_mask, layers) {
         if (store_d) {
            tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
            tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));
            tu_cs_emit_qw(cs, tu_attachment_gmem_offset(cmd, src, i));
            tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_PITCH(depth_pitch));
            tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(depth_layer_size));

            tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
            tu_cs_emit(cs, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));

            tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
            tu_cs_emit_qw(cs, 0);
            tu_cs_emit(cs, 0);

            tu5_emit_blit_dst_and_resolve(
               cmd, cs, BLIT_ZS, 0x4 | COND(d_tiled, A5XX_RB_RESOLVE_CNTL_3_TILED),
               tu_layer_address(dview, i), dview->pitch, dview->layer_size,
               clear_cntl);
         }
         if (store_s) {
            tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 5);
            tu_cs_emit(cs, A5XX_RB_STENCIL_INFO_SEPARATE_STENCIL);
            tu_cs_emit_qw(cs, tu_attachment_gmem_offset_stencil(cmd, src, i));
            tu_cs_emit(cs, A5XX_RB_STENCIL_PITCH(stencil_pitch));
            tu_cs_emit(cs, A5XX_RB_STENCIL_ARRAY_PITCH(stencil_layer_size));

            tu5_emit_blit_dst_and_resolve(
               cmd, cs, BLIT_S, 0x4 | COND(s_tiled, A5XX_RB_RESOLVE_CNTL_3_TILED),
               tu_layer_address(sview, i), sview->pitch, sview->layer_size,
               clear_cntl);
         }
      }
   } else if (is_zs) {
      enum a5xx_depth_format fmt = (enum a5xx_depth_format)
         tu5_pipe2depth(vk_format_to_pipe_format(src->format));
      bool separate_s = src->format == VK_FORMAT_S8_UINT;
      uint32_t pitch = tile_w * src->cpp;
      uint32_t layer_size = pitch * tile_h;
      const struct fdl6_view *dview =
         tu_image_view_fdl_view(dst_iview, separate_s);
      bool tiled =
         fdl_tile_mode(&dst_iview->image->layout[0], dst_iview->vk.base_mip_level) != 0;

      for_each_layer (i, layer_mask, layers) {
         if (separate_s) {
            tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 5);
            tu_cs_emit(cs, A5XX_RB_STENCIL_INFO_SEPARATE_STENCIL);
            tu_cs_emit_qw(cs, tu_attachment_gmem_offset(cmd, src, i));
            tu_cs_emit(cs, A5XX_RB_STENCIL_PITCH(pitch));
            tu_cs_emit(cs, A5XX_RB_STENCIL_ARRAY_PITCH(layer_size));
         } else {
            tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
            tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));
            tu_cs_emit_qw(cs, tu_attachment_gmem_offset(cmd, src, i));
            tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_PITCH(pitch));
            tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(layer_size));

            tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
            tu_cs_emit(cs, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));

            tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
            tu_cs_emit_qw(cs, 0);
            tu_cs_emit(cs, 0);

            tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 1);
            tu_cs_emit(cs, 0);
         }

         tu5_emit_blit_dst_and_resolve(
            cmd, cs, separate_s ? BLIT_S : BLIT_ZS,
            0x4 | COND(tiled, A5XX_RB_RESOLVE_CNTL_3_TILED),
            tu_layer_address(dview, i), dview->pitch, dview->layer_size,
            clear_cntl);
      }
   } else {
      uint32_t buf_info = src_iview->view.RB_MRT_BUF_INFO;
      buf_info &= ~A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__MASK;
      buf_info |= A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(TILE5_2) | 0x800;
      uint32_t pitch = tile_w * src->cpp;
      uint32_t layer_size = pitch * tile_h;
      const struct fdl6_view *dview = &dst_iview->view;
      bool tiled =
         fdl_tile_mode(&dst_iview->image->layout[0], dst_iview->vk.base_mip_level) != 0;

      for_each_layer (i, layer_mask, layers) {
         tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_BUF_INFO(0), 5);
         tu_cs_emit(cs, buf_info);
         tu_cs_emit(cs, A5XX_RB_MRT_PITCH(pitch));
         tu_cs_emit(cs, A5XX_RB_MRT_ARRAY_PITCH(layer_size));
         tu_cs_emit_qw(cs, tu_attachment_gmem_offset(cmd, src, i));

         tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_FLAG_BUFFER_ADDR(0), 4);
         tu_cs_emit_qw(cs, 0);
         tu_cs_emit(cs, 0);
         tu_cs_emit(cs, 0);

         tu5_emit_blit_dst_and_resolve(
            cmd, cs, BLIT_MRT0, 0x4 | COND(tiled, A5XX_RB_RESOLVE_CNTL_3_TILED),
            tu_layer_address(dview, i), dview->pitch, dview->layer_size,
            clear_cntl);
      }
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_CNTL, 1);
   tu_cs_emit(cs, 0);
}
