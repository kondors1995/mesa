/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2026 Freedreno / Turnip contributors
 * SPDX-License-Identifier: MIT
 */

#define A6XX_PERFCNTRS_XML
#include "tu_cmd_buffer.h"

#include "tu_cs.h"
#include "tu_descriptor_set.h"
#include "tu_device.h"
#include "tu_image.h"
#include "tu_pass.h"
#include "tu_pipeline.h"
#include "tu_shader.h"
#include "tu5_formats.h"

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

static enum a4xx_state_block
tu5_stage2texsb(mesa_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return SB4_VS_TEX;
   case MESA_SHADER_TESS_CTRL:
      return SB4_HS_TEX;
   case MESA_SHADER_TESS_EVAL:
      return SB4_DS_TEX;
   case MESA_SHADER_GEOMETRY:
      return SB4_GS_TEX;
   case MESA_SHADER_FRAGMENT:
      return SB4_FS_TEX;
   case MESA_SHADER_COMPUTE:
      return SB4_CS_TEX;
   default:
      UNREACHABLE("invalid shader stage");
   }
}

static uint32_t
tu5_dynamic_set_offset_bytes(const struct tu_cmd_buffer *cmd,
                             const struct tu_shader *shader,
                             const struct tu_descriptor_state *descriptors_state,
                             bool compute,
                             uint8_t set_idx)
{
   if (!compute)
      return cmd->state.program.dynamic_descriptor_offsets[set_idx];

   uint32_t offset = 0;
   for (uint8_t i = 0; i < set_idx; i++) {
      if (shader && shader->dynamic_descriptor_sizes[i] >= 0)
         offset += static_cast<uint32_t>(shader->dynamic_descriptor_sizes[i]);
      else if (descriptors_state->sets[i])
         offset += descriptors_state->sets[i]->layout->dynamic_offset_size;
   }
   return offset;
}

static const uint32_t *
tu5_lookup_desc(const struct tu_cmd_buffer *cmd,
                const struct tu_shader *shader,
                const struct tu_descriptor_state *descriptors_state,
                const struct tu5_binding_ref *ref,
                bool compute,
                VkDescriptorType *out_type)
{
   const struct tu_descriptor_set *set = descriptors_state->sets[ref->set];
   if (!set || !set->layout || ref->binding >= set->layout->binding_count)
      return NULL;

   const struct tu_descriptor_set_binding_layout *binding =
      &set->layout->binding[ref->binding];
   if (out_type)
      *out_type = binding->type;

   uint32_t elem_dwords = binding->size / 4;
   if (ref->is_dynamic) {
      uint32_t dyn_base_bytes =
         tu5_dynamic_set_offset_bytes(cmd, shader, descriptors_state, compute,
                                      ref->set);
      return descriptors_state->dynamic_descriptors +
             (dyn_base_bytes + binding->dynamic_offset_offset) / 4 +
             ref->array_index * elem_dwords;
   }

   if (!set->mapped_ptr)
      return NULL;

   return set->mapped_ptr + binding->offset / 4 + ref->array_index * elem_dwords;
}

static void
tu5_emit_load_consts_direct(struct tu_cs *cs,
                            enum a4xx_state_block sb,
                            uint32_t dst_vec4,
                            uint32_t num_vec4,
                            const uint32_t *data,
                            uint32_t num_dwords)
{
   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + num_vec4 * 4);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(dst_vec4) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                  CP_LOAD_STATE4_0_NUM_UNIT(num_vec4));
   tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                  CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
   tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   uint32_t total_dwords = num_vec4 * 4;
   uint32_t copy_dwords = MIN2(num_dwords, total_dwords);
   for (uint32_t i = 0; i < copy_dwords; i++)
      tu_cs_emit(cs, data[i]);
   for (uint32_t i = copy_dwords; i < total_dwords; i++)
      tu_cs_emit(cs, 0);
}

static uint32_t
tu5_stage_consts_max_dwords(const struct tu_shader *shader)
{
   if (!shader || !shader->variant || !shader->tu5_bindings)
      return 0;

   const struct ir3_shader_variant *xs = shader->variant;
   const struct ir3_const_state *ir_const = ir3_const_state(xs);
   const struct tu_const_state *tu_const = &shader->const_state;
   uint32_t dwords = 0;

   if (tu_const->push_consts.type == IR3_PUSH_CONSTS_PER_STAGE &&
       tu_const->push_consts.dwords > 0) {
      dwords += 4 + align(tu_const->push_consts.dwords, 4);
   }

   dwords += 8 * tu_const->num_inline_ubos;

   if (ir3_const_can_upload(&ir_const->allocs, IR3_CONST_ALLOC_UBO_PTRS,
                            xs->constlen) &&
       ir_const->num_ubos > 0) {
      dwords += 4 + 2 * align(ir_const->num_ubos, 2);
   }

   dwords += 4 * ir_const->ubo_state.num_enabled;

   if (ir_const->image_dims.count > 0 &&
       ir3_const_can_upload(&ir_const->allocs, IR3_CONST_ALLOC_IMAGE_DIMS,
                            xs->constlen)) {
      dwords += 4 + align(ir_const->image_dims.count, 4);
   }

   return dwords;
}

static void
tu5_emit_stage_consts(struct tu_cmd_buffer *cmd,
                      struct tu_cs *cs,
                      const struct tu_shader *shader,
                      mesa_shader_stage stage,
                      bool compute,
                      struct tu_descriptor_state *descriptors_state)
{
   if (!shader || !shader->variant || !shader->tu5_bindings)
      return;

   const struct ir3_shader_variant *xs = shader->variant;
   const struct ir3_const_state *ir_const = ir3_const_state(xs);
   const struct tu_const_state *tu_const = &shader->const_state;
   const struct tu5_shader_bindings *bindings = shader->tu5_bindings;
   enum a4xx_state_block sb = tu5_stage2shadersb(stage);

   if (tu_const->push_consts.type == IR3_PUSH_CONSTS_PER_STAGE &&
       tu_const->push_consts.dwords > 0) {
      uint32_t offset_vec4 =
         ir_const->allocs.consts[IR3_CONST_ALLOC_PUSH_CONSTS].offset_vec4;
      if (offset_vec4 < xs->constlen) {
         uint32_t avail_vec4 = xs->constlen - offset_vec4;
         uint32_t num_vec4 =
            MIN2(DIV_ROUND_UP(tu_const->push_consts.dwords, 4), avail_vec4);
         if (num_vec4 > 0) {
            tu5_emit_load_consts_direct(
               cs, sb, offset_vec4, num_vec4,
               cmd->push_constants + tu_const->push_consts.lo_dwords,
               tu_const->push_consts.dwords);
         }
      }
   }

   for (unsigned i = 0; i < tu_const->num_inline_ubos; i++) {
      const struct tu_inline_ubo *ubo = &tu_const->ubos[i];
      if (xs->constlen <= ubo->const_offset_vec4)
         continue;

      uint64_t va = (descriptors_state->set_iova[ubo->base] & ~0x3full) + ubo->offset;
      uint32_t num_unit = MIN2(ubo->size_vec4, xs->constlen - ubo->const_offset_vec4);
      if (ubo->push_address) {
         uint32_t addr_dwords[4] = {
            static_cast<uint32_t>(va),
            static_cast<uint32_t>(va >> 32),
            0,
            0,
         };
         tu5_emit_load_consts_direct(cs, sb, ubo->const_offset_vec4, num_unit,
                                     addr_dwords, 4);
      } else {
         tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3);
         tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(ubo->const_offset_vec4) |
                        CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                        CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                        CP_LOAD_STATE4_0_NUM_UNIT(num_unit));
         tu_cs_emit_qw(cs, va |
                           static_cast<uint64_t>(CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS)));
      }
   }

   uint64_t ubo_vas[IR3_MAX_UBO_PUSH_RANGES > 64 ? IR3_MAX_UBO_PUSH_RANGES : 64] = {};
   uint64_t ubo_sizes[ARRAY_SIZE(ubo_vas)] = {};
   uint32_t ptr_num_ubos = MIN2(ir_const->num_ubos, ARRAY_SIZE(ubo_vas));
   uint32_t consts_ubo_count =
      ir_const->consts_ubo.idx >= 0 ? static_cast<uint32_t>(ir_const->consts_ubo.idx) + 1 : 0u;
   uint32_t total_ubos =
      MIN2(MAX3(ptr_num_ubos, bindings->num_ubo, consts_ubo_count), ARRAY_SIZE(ubo_vas));

   for (uint32_t i = 0; i < total_ubos; i++) {
      if (static_cast<int>(i) == ir_const->consts_ubo.idx) {
         ubo_vas[i] = shader->tu5_binary_iova + xs->info.constant_data_offset;
         ubo_sizes[i] = xs->constant_data_size;
         continue;
      }

      if (i < bindings->num_ubo) {
         const uint32_t *desc =
            tu5_lookup_desc(cmd, shader, descriptors_state, &bindings->ubo[i],
                            compute, NULL);
         if (desc) {
            uint32_t size_vec4 = (desc[1] >> 17) & 0x7fffu;
            if (size_vec4 > 0) {
               ubo_vas[i] = desc[0] |
                            (static_cast<uint64_t>(desc[1] & 0x1ffffu) << 32);
               ubo_sizes[i] = static_cast<uint64_t>(size_vec4) * 16;
            }
         }
      }
   }

   if (ir_const->num_ubos > 0 &&
       ir3_const_can_upload(&ir_const->allocs, IR3_CONST_ALLOC_UBO_PTRS,
                            xs->constlen)) {
      uint32_t ptr_off =
         ir_const->allocs.consts[IR3_CONST_ALLOC_UBO_PTRS].offset_vec4;
      uint32_t anum = align(ptr_num_ubos, 2);
      uint32_t avail_vec4 = xs->constlen - ptr_off;
      uint32_t num_vec4 = MIN2(anum / 2, avail_vec4);

      if (num_vec4 > 0) {
         tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 4 * num_vec4);
         tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(ptr_off) |
                        CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                        CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                        CP_LOAD_STATE4_0_NUM_UNIT(num_vec4));
         tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                        CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
         tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

         uint32_t ptr_slots = num_vec4 * 2;
         for (uint32_t i = 0; i < ptr_slots; i++) {
            if (i < ptr_num_ubos && ubo_vas[i] != 0)
               tu_cs_emit_qw(cs, ubo_vas[i]);
            else
               tu_cs_emit_qw(cs, 0xffffffffffffffffull);
         }
      }
   }

   uint32_t uploaded_ranges = 0;
   for (uint32_t i = 0; i < ir_const->ubo_state.num_enabled; i++) {
      const struct ir3_ubo_range *r = &ir_const->ubo_state.range[i];
      if (r->ubo.bindless || r->ubo.global)
         continue;

      uint32_t dst_vec4 = r->offset / 16;
      if (dst_vec4 >= xs->constlen)
         continue;

      uint32_t size_vec4 = (r->end - r->start) / 16;
      size_vec4 = MIN2(size_vec4, xs->constlen - dst_vec4);
      if (size_vec4 == 0)
         continue;

      uint32_t ubo_idx = r->ubo.block;
      if (ubo_idx >= total_ubos || ubo_vas[ubo_idx] == 0 ||
          r->start >= ubo_sizes[ubo_idx]) {
         continue;
      }

      uint32_t valid_vec4 =
         DIV_ROUND_UP(static_cast<uint32_t>(ubo_sizes[ubo_idx] - r->start), 16);
      size_vec4 = MIN2(size_vec4, valid_vec4);
      if (size_vec4 == 0)
         continue;

      uploaded_ranges++;
      if (tu_dbg_log_enabled() && uploaded_ranges <= 4) {
         tu_dbg_log("A5_CONST_RNG st=%u i=%u dst=%u szv=%u r=(%u..%u) ubo=%u va=0x%llx",
                    (unsigned)stage, i, dst_vec4, size_vec4, r->start, r->end,
                    ubo_idx, (unsigned long long)(ubo_vas[ubo_idx] + r->start));
      }
      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(dst_vec4) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                     CP_LOAD_STATE4_0_NUM_UNIT(size_vec4));
      tu_cs_emit_qw(cs, (ubo_vas[ubo_idx] + r->start) |
                        static_cast<uint64_t>(CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS)));
   }

   if (tu_dbg_log_enabled()) {
      tu_dbg_log("A5_CONST st=%u clen=%u ptype=%u pdw=%u num_ubo=%u bind_ubo=%u rng=%u/%u inline=%u c_ubo=%d va0=0x%llx sz0=%llu",
                 (unsigned)stage, xs->constlen, (unsigned)tu_const->push_consts.type,
                 tu_const->push_consts.dwords, ir_const->num_ubos, bindings->num_ubo,
                 uploaded_ranges, ir_const->ubo_state.num_enabled,
                 tu_const->num_inline_ubos, ir_const->consts_ubo.idx,
                 (unsigned long long)ubo_vas[0], (unsigned long long)ubo_sizes[0]);
   }

   if (ir_const->image_dims.count > 0 &&
       ir3_const_can_upload(&ir_const->allocs, IR3_CONST_ALLOC_IMAGE_DIMS,
                            xs->constlen)) {
      uint32_t offset_vec4 =
         ir_const->allocs.consts[IR3_CONST_ALLOC_IMAGE_DIMS].offset_vec4;
      uint32_t num_vec4 =
         MIN2(DIV_ROUND_UP(ir_const->image_dims.count, 4),
              xs->constlen - offset_vec4);
      if (num_vec4 > 0) {
         uint32_t dims[align(ir_const->image_dims.count, 4)];
         memset(dims, 0, sizeof(dims));
         unsigned mask = ir_const->image_dims.mask;
         while (mask) {
            unsigned index = u_bit_scan(&mask);
            if (index >= bindings->num_image)
               continue;
            const uint32_t *desc =
               tu5_lookup_desc(cmd, shader, descriptors_state,
                               &bindings->image[index], compute, NULL);
            if (!desc)
               continue;

            unsigned off = ir_const->image_dims.off[index];
            if (off + 2 < ARRAY_SIZE(dims)) {
               dims[off + 0] = desc[15] & 0xffu;
               dims[off + 1] = desc[15] >> 8;
               dims[off + 2] = desc[14];
            }
         }
         tu5_emit_load_consts_direct(cs, sb, offset_vec4, num_vec4,
                                     dims, ARRAY_SIZE(dims));
      }
   }
}

struct tu_draw_state
tu5_emit_consts(struct tu_cmd_buffer *cmd, bool compute)
{
   struct tu_descriptor_state *descriptors =
      tu_get_descriptors_state(cmd, compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                            : VK_PIPELINE_BIND_POINT_GRAPHICS);

   uint32_t max_dwords = 0;
   if (compute) {
      max_dwords = tu5_stage_consts_max_dwords(cmd->state.shaders[MESA_SHADER_COMPUTE]);
   } else {
      for (uint32_t stage = MESA_SHADER_VERTEX; stage <= MESA_SHADER_FRAGMENT; stage++)
         max_dwords += tu5_stage_consts_max_dwords(cmd->state.shaders[stage]);
   }

   if (max_dwords == 0)
      return (struct tu_draw_state) {};

   struct tu_cs cs;
   VkResult result = tu_cs_begin_sub_stream(&cmd->sub_cs, max_dwords, &cs);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return (struct tu_draw_state) {};
   }

   if (compute) {
      tu5_emit_stage_consts(cmd, &cs, cmd->state.shaders[MESA_SHADER_COMPUTE],
                            MESA_SHADER_COMPUTE, true, descriptors);
   } else {
      for (uint32_t stage = MESA_SHADER_VERTEX; stage <= MESA_SHADER_FRAGMENT; stage++) {
         tu5_emit_stage_consts(cmd, &cs, cmd->state.shaders[stage],
                               static_cast<mesa_shader_stage>(stage), false,
                               descriptors);
      }
   }

   return tu_cs_end_draw_state(&cmd->sub_cs, &cs);
}

static void
tu5_emit_stage_tex_and_samp(const struct tu_cmd_buffer *cmd,
                            struct tu_cs *cs,
                            const struct tu_shader *shader,
                            mesa_shader_stage stage,
                            bool compute,
                            const struct tu_descriptor_state *descriptors_state)
{
   if (!shader || !shader->variant || !shader->tu5_bindings)
      return;

   const struct ir3_shader_variant *xs = shader->variant;
   const struct tu5_shader_bindings *b = shader->tu5_bindings;
   enum a4xx_state_block sb = tu5_stage2texsb(stage);

   if (b->num_samp > 0) {
      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 4 * b->num_samp);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                     CP_LOAD_STATE4_0_NUM_UNIT(b->num_samp));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                     CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

      for (uint32_t i = 0; i < b->num_samp; i++) {
         if (b->samp[i].has_immutable_sampler) {
            tu_cs_emit_array(cs, b->imm_samp[i].dword, 4);
            continue;
         }

         VkDescriptorType dtype = VK_DESCRIPTOR_TYPE_SAMPLER;
         const uint32_t *desc =
            tu5_lookup_desc(cmd, shader, descriptors_state, &b->samp[i],
                            compute, &dtype);
         if (desc && dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            desc += FDL6_TEX_CONST_DWORDS;

         if (desc) {
            tu_cs_emit_array(cs, desc, 4);
         } else {
            static const uint32_t zero_samp[4] = {};
            tu_cs_emit_array(cs, zero_samp, 4);
         }
      }
   }

   if (b->num_tex > 0) {
      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 12 * b->num_tex);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                     CP_LOAD_STATE4_0_NUM_UNIT(b->num_tex));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                     CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

      for (uint32_t i = 0; i < b->num_tex; i++) {
         const uint32_t *desc =
            tu5_lookup_desc(cmd, shader, descriptors_state, &b->tex[i],
                            compute, NULL);
         if (desc) {
            tu_cs_emit_array(cs, desc, 12);
         } else {
            static const uint32_t zero_tex[12] = {};
            tu_cs_emit_array(cs, zero_tex, 12);
         }
      }
   }

   const struct ir3_ibo_mapping *m = &xs->image_mapping;
   for (uint32_t i = 0; i < b->num_image; i++) {
      if (m->image_to_tex[i] == UAV_INVALID)
         continue;

      uint32_t slot = m->tex_base + m->image_to_tex[i];
      const uint32_t *desc =
         tu5_lookup_desc(cmd, shader, descriptors_state, &b->image[i],
                         compute, NULL);

      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 12);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(slot) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                     CP_LOAD_STATE4_0_NUM_UNIT(1));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                     CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
      if (desc) {
         tu_cs_emit_array(cs, desc, 12);
      } else {
         static const uint32_t zero_tex[12] = {};
         tu_cs_emit_array(cs, zero_tex, 12);
      }
   }
}

static void
tu5_emit_uavs(const struct tu_cmd_buffer *cmd,
              struct tu_cs *cs,
              const struct tu_shader *shader,
              enum a4xx_state_block sb,
              bool compute,
              const struct tu_descriptor_state *descriptors_state)
{
   if (!shader || !shader->tu5_bindings)
      return;

   const struct tu5_shader_bindings *b = shader->tu5_bindings;
   uint32_t total = b->num_ssbo + b->num_image;
   if (total == 0)
      return;

   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 2 * total);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                  CP_LOAD_STATE4_0_NUM_UNIT(total));
   tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                  CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
   tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   for (uint32_t i = 0; i < b->num_ssbo; i++) {
      const uint32_t *desc =
         tu5_lookup_desc(cmd, shader, descriptors_state, &b->ssbo[i],
                         compute, NULL);
      tu_cs_emit(cs, desc ? desc[14] : 0);
      tu_cs_emit(cs, desc ? desc[15] : 0);
   }
   for (uint32_t i = 0; i < b->num_image; i++) {
      const uint32_t *desc =
         tu5_lookup_desc(cmd, shader, descriptors_state, &b->image[i],
                         compute, NULL);
      tu_cs_emit(cs, desc ? desc[12] : 0);
      tu_cs_emit(cs, desc ? desc[13] : 0);
   }

   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 2 * total);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                  CP_LOAD_STATE4_0_NUM_UNIT(total));
   tu_cs_emit(cs, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                  CP_LOAD_STATE4_1_STATE_TYPE(ST4_UBO));
   tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   for (uint32_t i = 0; i < b->num_ssbo; i++) {
      const uint32_t *desc =
         tu5_lookup_desc(cmd, shader, descriptors_state, &b->ssbo[i],
                         compute, NULL);
      tu_cs_emit(cs, desc ? desc[12] : 0);
      tu_cs_emit(cs, desc ? desc[13] : 0);
   }
   for (uint32_t i = 0; i < b->num_image; i++) {
      const uint32_t *desc =
         tu5_lookup_desc(cmd, shader, descriptors_state, &b->image[i],
                         compute, NULL);
      tu_cs_emit(cs, desc ? desc[4] : 0);
      tu_cs_emit(cs, desc ? (desc[5] & 0x1ffffu) : 0);
   }
}

static uint32_t
tu5_stage_desc_max_dwords(const struct tu_shader *shader)
{
   if (!shader || !shader->variant || !shader->tu5_bindings)
      return 0;

   const struct tu5_shader_bindings *b = shader->tu5_bindings;
   uint32_t dwords = 0;
   if (b->num_samp > 0)
      dwords += 4 + 4 * b->num_samp;
   if (b->num_tex > 0)
      dwords += 4 + 12 * b->num_tex;
   dwords += 16 * b->num_image;
   if (b->num_ssbo + b->num_image > 0)
      dwords += (4 + 2 * (b->num_ssbo + b->num_image)) * 2;
   return dwords;
}

static uint32_t
tu5_tex_count_val(const struct tu_shader *shader)
{
   if (!shader || !shader->tu5_bindings)
      return 0;
   return shader->tu5_bindings->num_image > 0
             ? ~0u
             : shader->tu5_bindings->num_tex;
}

void
tu5_emit_descriptor_sets(struct tu_cmd_buffer *cmd,
                         VkPipelineBindPoint bind_point)
{
   bool compute = (bind_point == VK_PIPELINE_BIND_POINT_COMPUTE);
   struct tu_descriptor_state *descriptors_state =
      tu_get_descriptors_state(cmd, bind_point);

   struct tu_cs *cs, state_cs;
   if (!compute) {
      uint32_t max_dwords = 16;
      for (uint32_t stage = MESA_SHADER_VERTEX; stage <= MESA_SHADER_FRAGMENT; stage++)
         max_dwords += tu5_stage_desc_max_dwords(cmd->state.shaders[stage]);

      VkResult result = tu_cs_begin_sub_stream(&cmd->sub_cs, max_dwords, &state_cs);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }
      cs = &state_cs;
   } else {
      cs = &cmd->cs;
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_TP_BORDER_COLOR_BASE_ADDR, 2);
   tu_cs_emit_qw(cs, cmd->device->global_bo->iova + gb_offset(bcolor_builtin));

   if (compute) {
      struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];
      tu5_emit_stage_tex_and_samp(cmd, cs, shader, MESA_SHADER_COMPUTE, true,
                                  descriptors_state);

      tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_VS_TEX_COUNT, 4);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_FS_TEX_COUNT, 2);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, tu5_tex_count_val(shader));

      tu5_emit_uavs(cmd, cs, shader, SB4_CS_SSBO, true, descriptors_state);
      return;
   }

   struct tu_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   struct tu_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];

   tu5_emit_stage_tex_and_samp(cmd, cs, vs, MESA_SHADER_VERTEX, false,
                               descriptors_state);
   tu5_emit_stage_tex_and_samp(cmd, cs, fs, MESA_SHADER_FRAGMENT, false,
                               descriptors_state);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_VS_TEX_COUNT, 4);
   tu_cs_emit(cs, tu5_tex_count_val(vs));
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_FS_TEX_COUNT, 2);
   tu_cs_emit(cs, tu5_tex_count_val(fs));
   tu_cs_emit(cs, 0);

   const struct tu_shader *uav_shader =
      (fs && fs->tu5_bindings &&
       (fs->tu5_bindings->num_ssbo + fs->tu5_bindings->num_image > 0))
         ? fs
         : vs;
   tu5_emit_uavs(cmd, cs, uav_shader, SB4_SSBO, false, descriptors_state);

   if (tu_dbg_log_enabled()) {
      const struct tu5_shader_bindings *vb = vs ? vs->tu5_bindings : NULL;
      const struct tu5_shader_bindings *fb = fs ? fs->tu5_bindings : NULL;
      tu_dbg_log("A5_DESC cmd=%p vs=%p(tex=%u,samp=%u,ubo=%u,ssbo=%u,img=%u,hss=%d) fs=%p(tex=%u,samp=%u,img=%u,ubo=%u,ssbo=%u,hss=%d) uav=%p",
                 cmd, vs, vb ? vb->num_tex : 0, vb ? vb->num_samp : 0, vb ? vb->num_ubo : 0,
                 vb ? vb->num_ssbo : 0, vb ? vb->num_image : 0,
                 (vs && vs->variant) ? vs->variant->has_ssbo : -1,
                 fs, fb ? fb->num_tex : 0, fb ? fb->num_samp : 0, fb ? fb->num_image : 0,
                 fb ? fb->num_ubo : 0, fb ? fb->num_ssbo : 0,
                 (fs && fs->variant) ? fs->variant->has_ssbo : -1,
                 uav_shader);
      if (vb && vb->num_ssbo > 0) {
         for (uint32_t i = 0; i < vb->num_ssbo; i++) {
            VkDescriptorType dt = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            const uint32_t *d = tu5_lookup_desc(cmd, vs, descriptors_state, &vb->ssbo[i], false, &dt);
            tu_dbg_log("  A5_VS_SSBO[%u] set=%u bind=%u arr=%u dyn=%u dt=%d ptr=%p d12..15=[%08x %08x %08x %08x]",
                       i, vb->ssbo[i].set, vb->ssbo[i].binding, vb->ssbo[i].array_index,
                       vb->ssbo[i].is_dynamic, (int)dt, d,
                       d ? d[12] : 0, d ? d[13] : 0, d ? d[14] : 0, d ? d[15] : 0);
         }
      }
      if (fb) {
         for (uint32_t i = 0; i < fb->num_tex; i++) {
            const uint32_t *d = tu5_lookup_desc(cmd, fs, descriptors_state, &fb->tex[i], false, NULL);
            if (d) {
               uint64_t addr = ((uint64_t)(d[5] & 0x1ffff) << 32) | (d[4] & ~0x3fu);
               uint32_t fmt = (d[0] >> 22) & 0xff;
               uint32_t swap = (d[0] >> 20) & 0x3;
               uint32_t tile = d[0] & 0x3;
               tu_dbg_log("  A5_FS_TEX[%u] set=%u bind=%u ptr=%p fmt=0x%x swap=%u tile=%u addr=0x%" PRIx64 " d=[%08x %08x %08x %08x %08x %08x]",
                          i, fb->tex[i].set, fb->tex[i].binding, d, fmt, swap, tile, addr,
                          d[0], d[1], d[2], d[3], d[4], d[5]);
            } else {
               tu_dbg_log("  A5_FS_TEX[%u] set=%u bind=%u ptr=NULL",
                          i, fb->tex[i].set, fb->tex[i].binding);
            }
         }
         for (uint32_t i = 0; i < fb->num_samp; i++) {
            if (fb->samp[i].has_immutable_sampler) {
               const uint32_t *s = fb->imm_samp[i].dword;
               tu_dbg_log("  A5_FS_SAMP[%u] IMM [%08x %08x %08x %08x]", i, s[0], s[1], s[2], s[3]);
            } else {
               VkDescriptorType dt = VK_DESCRIPTOR_TYPE_SAMPLER;
               const uint32_t *s = tu5_lookup_desc(cmd, fs, descriptors_state, &fb->samp[i], false, &dt);
               if (s && dt == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                  s += FDL6_TEX_CONST_DWORDS;
               if (s)
                  tu_dbg_log("  A5_FS_SAMP[%u] dt=%d [%08x %08x %08x %08x]", i, dt, s[0], s[1], s[2], s[3]);
               else
                  tu_dbg_log("  A5_FS_SAMP[%u] dt=%d NULL", i, dt);
            }
         }
      }
   }

   cmd->state.desc_sets = tu_cs_end_draw_state(&cmd->sub_cs, &state_cs);
   if (!(cmd->state.dirty & TU_CMD_DIRTY_DRAW_STATE)) {
      tu_cs_emit_pkt7(&cmd->draw_cs, CP_SET_DRAW_STATE, 3);
      tu_cs_emit_draw_state(&cmd->draw_cs, TU_DRAW_STATE_DESC_SETS,
                            cmd->state.desc_sets);
   }
}

void
tu5_emit_vfd_index_offset(struct tu_cs *cs,
                          uint32_t vertex_offset,
                          uint32_t first_instance)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_INDEX_OFFSET, 2);
   tu_cs_emit(cs, vertex_offset);
   tu_cs_emit(cs, first_instance);
}

void
tu5_emit_vertex_buffers(struct tu_cmd_buffer *cmd)
{
   const struct tu_shader *vs_shader = cmd->state.shaders[MESA_SHADER_VERTEX];
   unsigned attr_count =
      (vs_shader && vs_shader->variant) ? vs_shader->variant->attr_in : 0;

   if (attr_count == 0) {
      cmd->state.vertex_buffers = {};
      return;
   }

   const struct vk_vertex_input_state *vi = cmd->vk.dynamic_graphics_state.vi;
   const uint16_t *strides = cmd->vk.dynamic_graphics_state.vi_binding_strides;

   struct tu_cs cs;
   cmd->state.vertex_buffers =
      tu_cs_draw_state(&cmd->sub_cs, &cs, 8 * attr_count);

   for (unsigned loc = 0; loc < attr_count; loc++) {
      if (vi && (vi->attributes_valid & (1u << loc))) {
         const struct vk_vertex_attribute_state *attr = &vi->attributes[loc];
         const struct vk_vertex_binding_state *binding =
            &vi->bindings[attr->binding];
         enum pipe_format pipe_format = vk_format_to_pipe_format(attr->format);
         const struct tu5_native_format fmt = tu5_format_vtx(pipe_format);

         uint64_t vb_base = cmd->state.vb[attr->binding].base;
         uint32_t vb_size = cmd->state.vb[attr->binding].size;
         uint64_t addr = vb_base ? (vb_base + attr->offset) : 0;
         uint32_t size = (vb_base && vb_size > attr->offset)
                            ? (vb_size - attr->offset)
                            : 0;
         /* Per Vulkan spec, vkCmdBindVertexBuffers2 pStrides is ignored unless
          * VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE (or VERTEX_INPUT_EXT)
          * is enabled on the bound pipeline; use static binding->stride then.
          */
         uint32_t stride = cmd->state.static_vi_strides
                              ? binding->stride
                              : strides[attr->binding];
         bool instanced = binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;

         tu_cs_emit_pkt4(&cs, REG_A5XX_VFD_FETCH(loc), 4);
         tu_cs_emit_qw(&cs, addr);
         tu_cs_emit(&cs, size);
         tu_cs_emit(&cs, stride);

         tu_cs_emit_pkt4(&cs, REG_A5XX_VFD_DECODE(loc), 2);
         tu_cs_emit(&cs,
                    A5XX_VFD_DECODE_INSTR_IDX(loc) |
                    A5XX_VFD_DECODE_INSTR_FORMAT(static_cast<enum a5xx_vtx_fmt>(fmt.fmt)) |
                    COND(instanced, A5XX_VFD_DECODE_INSTR_INSTANCED) |
                    A5XX_VFD_DECODE_INSTR_SWAP(static_cast<enum a3xx_color_swap>(fmt.swap)) |
                    A5XX_VFD_DECODE_INSTR_UNK30 |
                    COND(!util_format_is_pure_integer(pipe_format),
                         A5XX_VFD_DECODE_INSTR_FLOAT));
         tu_cs_emit(&cs, instanced ? binding->divisor : 1);

         if (tu_dbg_log_enabled()) {
            tu_dbg_log("A5_VFD loc=%u/%u valid=0x%x bind=%u vkfmt=%u a5fmt=0x%x off=%u base=0x%llx sz=%u str=%u(vi=%u) inst=%u",
                       loc, attr_count, vi->attributes_valid, attr->binding,
                       (unsigned)attr->format, fmt.fmt, attr->offset,
                       (unsigned long long)vb_base, size, stride, binding->stride, instanced ? 1u : 0u);
         }
      } else {
         tu_cs_emit_pkt4(&cs, REG_A5XX_VFD_FETCH(loc), 4);
         tu_cs_emit(&cs, 0);
         tu_cs_emit(&cs, 0);
         tu_cs_emit(&cs, 0);
         tu_cs_emit(&cs, 0);

         tu_cs_emit_pkt4(&cs, REG_A5XX_VFD_DECODE(loc), 2);
         tu_cs_emit(&cs, 0);
         tu_cs_emit(&cs, 0);

         if (tu_dbg_log_enabled()) {
            tu_dbg_log("A5_VFD loc=%u/%u INVALID vi=%p valid=0x%x",
                       loc, attr_count, vi, vi ? vi->attributes_valid : 0u);
         }
      }
   }
}

void
tu5_emit_msaa(struct tu_cs *cs,
              VkSampleCountFlagBits vk_samples,
              bool msaa_disable)
{
   const enum a3xx_msaa_samples samples = tu_msaa_samples(vk_samples);
   msaa_disable |= (samples == MSAA_ONE);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_TP_RAS_MSAA_CNTL, 2);
   tu_cs_emit(cs, A5XX_TPL1_TP_RAS_MSAA_CNTL_SAMPLES(samples));
   tu_cs_emit(cs, A5XX_TPL1_TP_DEST_MSAA_CNTL_SAMPLES(samples) |
                  COND(msaa_disable, A5XX_TPL1_TP_DEST_MSAA_CNTL_MSAA_DISABLE));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_RAS_MSAA_CNTL, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_RAS_MSAA_CNTL_SAMPLES(samples));
   tu_cs_emit(cs, A5XX_GRAS_SC_DEST_MSAA_CNTL_SAMPLES(samples) |
                  COND(msaa_disable, A5XX_GRAS_SC_DEST_MSAA_CNTL_MSAA_DISABLE));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RAS_MSAA_CNTL, 2);
   tu_cs_emit(cs, A5XX_RB_RAS_MSAA_CNTL_SAMPLES(samples));
   tu_cs_emit(cs, A5XX_RB_DEST_MSAA_CNTL_SAMPLES(samples) |
                  COND(msaa_disable, A5XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE));
}

void
tu5_emit_depth_plane(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_shader *fs_shader = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   const struct ir3_shader_variant *fs = fs_shader ? fs_shader->variant : NULL;

   bool fragz = fs && (fs->no_earlyz || fs->has_kill || fs->writes_pos);
   bool has_fragcoord = fs && (fs->fragcoord_compmask != 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_PLANE_CNTL, 1);
   tu_cs_emit(cs, COND(fragz, A5XX_RB_DEPTH_PLANE_CNTL_FRAG_WRITES_Z) |
                  COND(fragz && has_fragcoord, A5XX_RB_DEPTH_PLANE_CNTL_UNK1));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
   tu_cs_emit(cs, COND(fragz, A5XX_GRAS_SU_DEPTH_PLANE_CNTL_FRAG_WRITES_Z) |
                  COND(fragz && has_fragcoord, A5XX_GRAS_SU_DEPTH_PLANE_CNTL_UNK1));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_CNTL, 1);
   tu_cs_emit(cs, 0);
}

void
tu5_emit_pc_cntl(struct tu_cmd_buffer *cmd,
                 struct tu_cs *cs,
                 bool primitive_restart,
                 bool provoking_vtx_last)
{
   const struct tu_shader *vs_shader = cmd->state.shaders[MESA_SHADER_VERTEX];
   const struct tu_shader *fs_shader = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   const struct ir3_shader_variant *vs = vs_shader ? vs_shader->variant : NULL;
   const struct ir3_shader_variant *fs = fs_shader ? fs_shader->variant : NULL;

   uint32_t stride_in_vpc = tu5_calc_stride_in_vpc(vs, fs);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_PRIMITIVE_CNTL, 1);
   tu_cs_emit(cs, A5XX_PC_PRIMITIVE_CNTL_STRIDE_IN_VPC(stride_in_vpc) |
                  COND(primitive_restart, A5XX_PC_PRIMITIVE_CNTL_PRIMITIVE_RESTART) |
                  COND(provoking_vtx_last, A5XX_PC_PRIMITIVE_CNTL_PROVOKING_VTX_LAST));
}

void
tu5_emit_restart_index(struct tu_cs *cs, uint32_t restart_index)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_PC_RESTART_INDEX, 1);
   tu_cs_emit(cs, restart_index);
}

void
tu5_emit_fs_output_cntl(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_shader *fs_shader = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   const struct ir3_shader_variant *fs = fs_shader ? fs_shader->variant : NULL;

   uint32_t posz_regid =
      fs ? ir3_find_output_regid(fs, FRAG_RESULT_DEPTH) : regid(63, 0);
   uint32_t smask_regid =
      fs ? ir3_find_output_regid(fs, FRAG_RESULT_SAMPLE_MASK) : regid(63, 0);
   bool writes_pos = fs ? fs->writes_pos : false;
   bool writes_smask = fs ? fs->writes_smask : false;

   if (cmd->vk.dynamic_graphics_state.ms.rasterization_samples <= VK_SAMPLE_COUNT_1_BIT) {
      smask_regid = regid(63, 0);
      writes_smask = false;
   }

   bool dual = fs && fs->dual_src_blend;
   uint32_t nr =
      cmd->vk.dynamic_graphics_state.rs.rasterizer_discard_enable
         ? 0
         : (dual ? 2 : cmd->state.vk_rp.color_attachment_count);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_FS_OUTPUT_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_FS_OUTPUT_CNTL_MRT(nr) |
                  COND(dual, A5XX_RB_FS_OUTPUT_CNTL_DUAL_COLOR_IN_ENABLE) |
                  COND(writes_pos, A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_Z) |
                  COND(writes_smask, A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_SAMPMASK));

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_OUTPUT_CNTL, 1);
   tu_cs_emit(cs, A5XX_SP_FS_OUTPUT_CNTL_MRT(nr) |
                  COND(dual, A5XX_SP_FS_OUTPUT_CNTL_DUAL_COLOR_IN_ENABLE) |
                  A5XX_SP_FS_OUTPUT_CNTL_DEPTH_REGID(posz_regid) |
                  A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(smask_regid));
}

void
tu5_emit_cs_ndrange(struct tu_cs *cs,
                    const uint16_t local_size[3],
                    const uint32_t num_groups[3])
{
   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_NDRANGE_0, 7);
   tu_cs_emit(cs, A5XX_HLSQ_CS_NDRANGE_0_KERNELDIM(3) |
                  A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX(local_size[0] - 1) |
                  A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY(local_size[1] - 1) |
                  A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(local_size[2] - 1));
   tu_cs_emit(cs, A5XX_HLSQ_CS_NDRANGE_1_GLOBALSIZE_X(local_size[0] * num_groups[0]));
   tu_cs_emit(cs, A5XX_HLSQ_CS_NDRANGE_2_GLOBALOFF_X(0));
   tu_cs_emit(cs, A5XX_HLSQ_CS_NDRANGE_3_GLOBALSIZE_Y(local_size[1] * num_groups[1]));
   tu_cs_emit(cs, A5XX_HLSQ_CS_NDRANGE_4_GLOBALOFF_Y(0));
   tu_cs_emit(cs, A5XX_HLSQ_CS_NDRANGE_5_GLOBALSIZE_Z(local_size[2] * num_groups[2]));
   tu_cs_emit(cs, A5XX_HLSQ_CS_NDRANGE_6_GLOBALOFF_Z(0));

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CS_KERNEL_GROUP_X, 3);
   tu_cs_emit(cs, 1);
   tu_cs_emit(cs, 1);
   tu_cs_emit(cs, 1);
}

void
tu5_emit_cache_invalidate(struct tu_cs *cs)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_UCHE_CACHE_INVALIDATE_MIN, 5);
   tu_cs_emit(cs, 0x00000000); /* UCHE_CACHE_INVALIDATE_MIN_LO */
   tu_cs_emit(cs, 0x00000000); /* UCHE_CACHE_INVALIDATE_MIN_HI */
   tu_cs_emit(cs, 0x00000000); /* UCHE_CACHE_INVALIDATE_MAX_LO */
   tu_cs_emit(cs, 0x00000000); /* UCHE_CACHE_INVALIDATE_MAX_HI */
   tu_cs_emit(cs, 0x00000012); /* UCHE_CACHE_INVALIDATE */
   tu_cs_emit_wfi(cs);
}

void
tu5_emit_lrz_flush(struct tu_cs *cs)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_CNTL, 1);
   tu_cs_emit(cs, A5XX_GRAS_LRZ_CNTL_ENABLE);

   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(LRZ_FLUSH_INVALIDATE));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);
}

void
tu5_emit_rb_ccu_cntl(struct tu_cs *cs,
                     const struct tu_physical_device *phys_dev,
                     bool gmem)
{
   uint32_t gpu_id = phys_dev->dev_id.gpu_id;
   uint32_t val;

   if (gmem) {
      uint32_t offset = phys_dev->config_gmem.color_ccu_offset;
      val = ((offset * 0x800) & 0xffe00000) | 0x0013c080;
   } else {
      if (gpu_id >= 530)
         val = 0x10000000;
      else if (gpu_id >= 509)
         val = 0x08000000;
      else
         val = 0x04000000;
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CCU_CNTL, 1);
   tu_cs_emit(cs, val);
}

void
tu5_set_render_mode(struct tu_cs *cs, enum render_mode_cmd mode)
{
   tu_cs_emit_pkt7(cs, CP_SET_RENDER_MODE, 5);
   tu_cs_emit(cs, CP_SET_RENDER_MODE_0_MODE(mode));
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, COND(mode == GMEM, CP_SET_RENDER_MODE_3_GMEM_ENABLE) |
                  COND(mode == BINNING, CP_SET_RENDER_MODE_3_VSC_ENABLE));
   tu_cs_emit(cs, 0x00000000);
}

uint32_t
tu5_always_on_counter_reg(void)
{
   return REG_A5XX_RBBM_ALWAYSON_COUNTER_LO;
}

void
tu5_emit_sample_counter_cntl(struct tu_cs *cs)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_SAMPLE_COUNT_CONTROL, 1);
   tu_cs_emit(cs, A5XX_RB_SAMPLE_COUNT_CONTROL_COPY);
}

void
tu5_emit_sample_counter_addr(struct tu_cs *cs, uint64_t iova)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_SAMPLE_COUNT_ADDR, 2);
   tu_cs_emit_qw(cs, iova);
}

void
tu5_init_hw(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   struct tu_device *dev = cmd->device;
   const struct tu_physical_device *phys_dev = dev->physical_device;
   const struct fd_dev_info *info = phys_dev->info;

   tu5_set_render_mode(cs, BYPASS);
   tu5_emit_cache_invalidate(cs);

   if (dev->dbg_cmdbuf_stomp_cs)
      tu_cs_emit_call(cs, dev->dbg_cmdbuf_stomp_cs);

   cmd->state.cache.pending_flush_bits &=
      ~(TU_CMD_FLAG_WAIT_FOR_IDLE | TU_CMD_FLAG_CACHE_INVALIDATE);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_UPDATE_CNTL, 1);
   tu_cs_emit(cs, 0xfffff);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_RESTART_INDEX, 1);
   tu_cs_emit(cs, 0xffffffff);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_RASTER_CNTL, 1);
   tu_cs_emit(cs, 0x00000012);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_POINT_MINMAX, 2);
   tu_cs_emit(cs, A5XX_GRAS_SU_POINT_MINMAX_MIN(1.0f) |
                  A5XX_GRAS_SU_POINT_MINMAX_MAX(4092.0f));
   tu_cs_emit(cs, A5XX_GRAS_SU_POINT_SIZE(0.5f));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_CONSERVATIVE_RAS_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_ALPHA_CONTROL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_VS_CONFIG_MAX_CONST, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_CONFIG_MAX_CONST, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E292, 2);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);

   uint32_t hbb = phys_dev->ubwc_config.highest_bank_bit;
   uint32_t hbb_enc = hbb >= 13 ? ((hbb - 13) & 3) : 1;

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_MODE_CNTL, 1);
   tu_cs_emit(cs, 0x00000040 | (hbb_enc << 1));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DBG_ECO_CNTL, 1);
   tu_cs_emit(cs, info->num_sp_cores == 1 ? 0x00100200 : 0x00100000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_MODE_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_MODE_CNTL, 1);
   tu_cs_emit(cs, 0x0000001f);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_MODE_CNTL, 1);
   tu_cs_emit(cs, 0x0000001e);

   if (phys_dev->dev_id.gpu_id == 540) {
      tu_cs_emit_pkt4(cs, REG_A5XX_SP_DBG_ECO_CNTL, 1);
      tu_cs_emit(cs, 0x800);

      tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_DBG_ECO_CNTL, 1);
      tu_cs_emit(cs, 0x0);

      tu_cs_emit_pkt4(cs, REG_A5XX_VPC_DBG_ECO_CNTL, 1);
      tu_cs_emit(cs, 0x800400);
   } else {
      tu_cs_emit_pkt4(cs, REG_A5XX_SP_DBG_ECO_CNTL, 1);
      tu_cs_emit(cs, 0x40000800);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_MODE_CNTL, 1);
   tu_cs_emit(cs, 0x00000444 | (hbb_enc << 7));

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_TIMEOUT_THRESHOLD_0, 2);
   tu_cs_emit(cs, 0x00000080);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_DBG_ECO_CNTL, 1);
   tu_cs_emit(cs, 0x00000400);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_MODE_CNTL, 1);
   tu_cs_emit(cs, 0x00000001);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_MODE_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_BIN_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_FS_PRIMITIVEID_CNTL, 1);
   tu_cs_emit(cs, 0x000000ff);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_OVERRIDE, 1);
   tu_cs_emit(cs, A5XX_VPC_SO_OVERRIDE_SO_DISABLE);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_BUFFER_BASE(0), 3);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_FLUSH_BASE(0), 2);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_GS_PARAM, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_HS_PARAM, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_TP_FS_ROTATION_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E004, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_LAYERED, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_BUF_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_BUFFER_OFFSET(0), 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_PC_GS_LAYERED, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E5AB, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E5C2, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_BUFFER_BASE(1), 3);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_BUFFER_OFFSET(1), 6);
   for (unsigned i = 0; i < 6; i++)
      tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_BUFFER_OFFSET(2), 6);
   for (unsigned i = 0; i < 6; i++)
      tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_SO_BUFFER_OFFSET(3), 3);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E5DB, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_HS_CTRL_REG0, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_GS_CTRL_REG0, 1);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_VS_TEX_COUNT, 4);
   for (unsigned i = 0; i < 4; i++)
      tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_FS_TEX_COUNT, 2);
   tu_cs_emit(cs, 0x00000000);
   tu_cs_emit(cs, 0x00000000);

   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E7C0, 3);
   tu_cs_emit(cs, 0); tu_cs_emit(cs, 0); tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E7C5, 3);
   tu_cs_emit(cs, 0); tu_cs_emit(cs, 0); tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E7CA, 3);
   tu_cs_emit(cs, 0); tu_cs_emit(cs, 0); tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E7CF, 3);
   tu_cs_emit(cs, 0); tu_cs_emit(cs, 0); tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E7D4, 3);
   tu_cs_emit(cs, 0); tu_cs_emit(cs, 0); tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_UNKNOWN_E7D9, 3);
   tu_cs_emit(cs, 0); tu_cs_emit(cs, 0); tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_CNTL, 1);
   tu_cs_emit(cs, 0x00000000);

   uint32_t power_cntl = MAX2(info->num_sp_cores, 1u) - 1;
   tu_cs_emit_pkt4(cs, REG_A5XX_PC_POWER_CNTL, 1);
   tu_cs_emit(cs, power_cntl);

   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_POWER_CNTL, 1);
   tu_cs_emit(cs, power_cntl);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_TP_BORDER_COLOR_BASE_ADDR, 2);
   tu_cs_emit_qw(cs, dev->global_bo->iova + gb_offset(bcolor_builtin));

   tu_cs_emit_wfi(cs);
   tu5_emit_rb_ccu_cntl(cs, phys_dev, false);
   cmd->state.ccu_state = TU_CMD_CCU_SYSMEM;

   tu_disable_draw_states(cmd, cs);
}

void
tu5_emit_zs(struct tu_cmd_buffer *cmd,
            const struct tu_subpass *subpass,
            struct tu_cs *cs)
{
   const uint32_t a = subpass->depth_stencil_attachment.attachment;
   if (a == VK_ATTACHMENT_UNUSED) {
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
      tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH5_NONE));
      tu_cs_emit_qw(cs, 0);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
      tu_cs_emit(cs, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH5_NONE));

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
      tu_cs_emit_qw(cs, 0);
      tu_cs_emit(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 1);
      tu_cs_emit(cs, 0);
      return;
   }

   const struct tu_image_view *iview = cmd->state.attachments[a];
   const struct tu_render_pass_attachment *attachment =
      &cmd->state.pass->attachments[a];
   enum a5xx_depth_format fmt =
      (enum a5xx_depth_format) tu5_pipe2depth(vk_format_to_pipe_format(attachment->format));
   const uint32_t layer = cmd->state.sw_multiview ? cmd->state.sw_view_index : 0;
   const struct fdl6_view *depth_view = tu_image_view_fdl_view(iview, false);
   const struct fdl6_view *stencil_view = tu_image_view_fdl_view(iview, true);

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));
   tu_cs_emit_qw(cs, tu_layer_address(depth_view, layer));
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_PITCH(depth_view->pitch));
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(depth_view->layer_size));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
   tu_cs_emit(cs, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
   tu_cs_emit_qw(cs, 0);
   tu_cs_emit(cs, 0);

   if (attachment->format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
       attachment->format == VK_FORMAT_S8_UINT) {
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 5);
      tu_cs_emit(cs, A5XX_RB_STENCIL_INFO_SEPARATE_STENCIL);
      tu_cs_emit_qw(cs, tu_layer_address(stencil_view, layer));
      tu_cs_emit(cs, A5XX_RB_STENCIL_PITCH(stencil_view->pitch));
      tu_cs_emit(cs, A5XX_RB_STENCIL_ARRAY_PITCH(stencil_view->layer_size));
   } else {
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 1);
      tu_cs_emit(cs, 0);
   }
}

void
tu5_emit_mrt(struct tu_cmd_buffer *cmd,
             const struct tu_subpass *subpass,
             struct tu_cs *cs)
{
   const uint32_t layer = cmd->state.sw_multiview ? cmd->state.sw_view_index : 0;
   const bool dither = subpass->legacy_dithering_enabled;
   uint32_t written = 0;

   for (uint32_t i = 0; i < subpass->color_count; ++i) {
      uint32_t a = subpass->color_attachments[i].attachment;
      unsigned remapped = cmd->vk.dynamic_graphics_state.cal.color_map[i];
      if (a == VK_ATTACHMENT_UNUSED ||
          remapped == MESA_VK_ATTACHMENT_UNUSED)
         continue;

      const struct tu_image_view *iview = cmd->state.attachments[a];
      uint32_t buf_info = iview->view.RB_MRT_BUF_INFO;
      if (dither)
         buf_info |= A5XX_RB_MRT_BUF_INFO_DITHER_MODE(DITHER_ALWAYS);

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_BUF_INFO(remapped), 5);
      tu_cs_emit(cs, buf_info);
      tu_cs_emit(cs, A5XX_RB_MRT_PITCH(iview->view.pitch));
      tu_cs_emit(cs, A5XX_RB_MRT_ARRAY_PITCH(iview->view.layer_size));
      tu_cs_emit_qw(cs, tu_layer_address(&iview->view, layer));

      tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_MRT_REG(remapped), 1);
      tu_cs_emit(cs, iview->view.SP_PS_MRT_REG);

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_FLAG_BUFFER_ADDR(remapped), 4);
      tu_cs_emit_qw(cs, 0);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);

      written |= 1u << remapped;
   }

   u_foreach_bit (i, ~written) {
      if (i >= MAX_RTS)
         break;

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_BUF_INFO(i), 5);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
      tu_cs_emit_qw(cs, 0);

      tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_MRT_REG(i), 1);
      tu_cs_emit(cs, 0);
   }
}

void
tu5_emit_bin_size(struct tu_cs *cs,
                  uint32_t bin_w,
                  uint32_t bin_h,
                  bool bypass)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CNTL, 1);
   tu_cs_emit(cs, A5XX_RB_CNTL_WIDTH(bin_w) |
                  A5XX_RB_CNTL_HEIGHT(bin_h) |
                  COND(bypass, A5XX_RB_CNTL_BYPASS));
}

void
tu5_emit_render_cntl(struct tu_cmd_buffer *cmd,
                     const struct tu_subpass *subpass,
                     struct tu_cs *cs,
                     bool binning)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_CNTL, 1);
   tu_cs_emit(cs, 0x00000008 |
                  COND(binning, A5XX_RB_RENDER_CNTL_BINNING_PASS |
                                A5XX_RB_RENDER_CNTL_DISABLE_COLOR_PIPE));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_CNTL, 1);
   tu_cs_emit(cs, 0x00000008 |
                  COND(binning, A5XX_GRAS_SC_CNTL_BINNING_PASS));
}

void
tu5_emit_window_scissor(struct tu_cs *cs,
                        uint32_t x1,
                        uint32_t y1,
                        uint32_t x2,
                        uint32_t y2)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_WINDOW_SCISSOR_TL_X(x1) |
                  A5XX_GRAS_SC_WINDOW_SCISSOR_TL_Y(y1));
   tu_cs_emit(cs, A5XX_GRAS_SC_WINDOW_SCISSOR_BR_X(x2) |
                  A5XX_GRAS_SC_WINDOW_SCISSOR_BR_Y(y2));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RESOLVE_CNTL_1, 2);
   tu_cs_emit(cs, A5XX_RB_RESOLVE_CNTL_1_X(x1) |
                  A5XX_RB_RESOLVE_CNTL_1_Y(y1));
   tu_cs_emit(cs, A5XX_RB_RESOLVE_CNTL_2_X(x2) |
                  A5XX_RB_RESOLVE_CNTL_2_Y(y2));
}

void
tu5_emit_window_offset(struct tu_cs *cs, uint32_t x1, uint32_t y1)
{
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_WINDOW_OFFSET, 1);
   tu_cs_emit(cs, A5XX_RB_WINDOW_OFFSET_X(x1) |
                  A5XX_RB_WINDOW_OFFSET_Y(y1));
}
