/*
 * Copyright © 2020 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir/nir_builder.h"

#include "ir3/ir3_nir.h"
#include "tu_device.h"
#include "tu_shader.h"

/* Some a6xx variants cannot support a non-contiguous multiview mask. Instead,
 * inside the shader something like this needs to be inserted:
 *
 * gl_Position = ((1ull << gl_ViewIndex) & view_mask) ? gl_Position : vec4(0.);
 *
 * Scan backwards until we find the gl_Position write (there should only be
 * one). This also needs to happen with multi-position, which doesn't respect
 * the view mask.
 */
static bool
lower_multiview_mask(nir_shader *nir, uint32_t *mask)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   if (util_is_power_of_two_or_zero(*mask + 1)) {
      return nir_no_progress(impl);
   }

   nir_builder b = nir_builder_create(impl);

   uint32_t old_mask = *mask;
   *mask = BIT(util_logbase2(old_mask) + 1) - 1;

   nir_foreach_block_reverse(block, impl) {
      nir_foreach_instr_reverse(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_store_deref)
            continue;

         nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
         if (!nir_deref_mode_is(deref, nir_var_shader_out))
            continue;

         nir_variable *var = nir_deref_instr_get_variable(deref);
         if (var->data.location != VARYING_SLOT_POS)
            continue;

         nir_def *orig_src = intrin->src[1].ssa;
         b.cursor = nir_before_instr(instr);

         /* ((1ull << gl_ViewIndex) & mask) != 0 */
         nir_def *cmp =
            nir_i2b(&b, nir_iand(&b, nir_imm_int(&b, old_mask),
                                  nir_ishl(&b, nir_imm_int(&b, 1),
                                           nir_load_view_index(&b))));

         nir_def *src = nir_bcsel(&b, cmp, orig_src, nir_imm_float(&b, 0.));
         nir_src_rewrite(&intrin->src[1], src);

         return nir_progress(true, impl, nir_metadata_control_flow);
      }
   }

   return nir_no_progress(impl);
}

bool
tu_nir_lower_multiview(nir_shader *nir, uint32_t mask, struct tu_device *dev,
                       bool last_stage)
{
   bool progress = false;
   nir_lower_multiview_options options = {
      .view_mask = mask,
      .allowed_per_view_outputs =
         last_stage ? VARYING_BIT_POS : ~0ull,
   };

   if (!dev->physical_device->info->props.supports_multiview_mask &&
       last_stage)
      NIR_PASS(progress, nir, lower_multiview_mask, &options.view_mask);

   unsigned num_views = util_logbase2(mask) + 1;

   /* Blob doesn't apply multipos optimization starting from 11 views
    * even on a650, however in practice, with the limit of 16 views,
    * tests pass on a640/a650 and fail on a630.
    */
   unsigned max_views_for_multipos =
      dev->physical_device->info->props.supports_multiview_mask ? 16 : 10;

   /* Speculatively assign output locations so that we know num_outputs. We
    * will assign output locations for real after this pass.
    */
   nir_assign_io_var_locations(nir, nir_var_shader_out);

   if (!last_stage) {
      /* We will store outputs per-view and loop over all active views in the
       * shader.
       */
      NIR_PASS(progress, nir, nir_lower_multiview, options);

   /* In addition to the generic checks done by NIR, check that we don't
    * overflow VPC with the extra copies of gl_Position.
    */
   } else if (!dev->physical_device->compiler_options.no_multi_pos &&
       num_views <= max_views_for_multipos && nir->num_outputs + (num_views - 1) <= 32 &&
       nir_can_lower_multiview(nir, options)) {
      /* It appears that the multiview mask is ignored when multi-position
       * output is enabled, so we have to write 0 to inactive views ourselves.
       */
      NIR_PASS(progress, nir, lower_multiview_mask, &options.view_mask);

      NIR_PASS(_, nir, nir_lower_multiview, options);

      /* nir_lower_multiview creates a loop that loops over the view mask and
       * uses indirect stores. Since we only support direct
       * store_per_view_output, we have to make sure these indirects are gone.
       * Lowering the IO vars to temporaries will replace them with indirects
       * on registers.
       */
      ir3_nir_lower_io_vars_to_temporaries(nir);

      progress = true;
   }

   return progress;
}

/*
 * Software multiview lowering for devices without HW multiview support.
 *
 * Unlike the HW multiview path which uses per-view position output and the
 * GPU's built-in multiview mode, the SW path works by duplicating draws on
 * the CPU side (one draw per view).  The view index is passed via a driver
 * param (view_index at dword 5, placed after the vec4 that
 * CP_DRAW_INDIRECT_MULTI overwrites), and the VS writes it to
 * VARYING_SLOT_LAYER so the rasterizer targets the correct layer.
 */

static bool
lower_view_index_to_layer_id_filter(const nir_instr *instr,
                                    UNUSED const void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   return nir_instr_as_intrinsic(instr)->intrinsic ==
          nir_intrinsic_load_view_index;
}

static nir_def *
lower_view_index_to_layer_id(nir_builder *b, nir_instr *instr,
                             UNUSED void *data)
{
   return nir_load_layer_id(b);
}

bool
tu_nir_lower_multiview_sw_vs(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_VERTEX);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   /* If the shader already writes gl_Layer, demote the existing output to a
    * temp so our store takes precedence.
    */
   nir_variable *existing_layer =
      nir_find_variable_with_location(nir, nir_var_shader_out,
                                     VARYING_SLOT_LAYER);
   if (existing_layer) {
      existing_layer->data.mode = nir_var_shader_temp;
      existing_layer->data.location = 0;
      nir_fixup_deref_modes(nir);
   }

   nir_builder b = nir_builder_at(nir_after_impl(impl));

   /* Write gl_Layer = view_index so the rasterizer targets the correct
    * layer.  load_view_index survives to IR3 where it becomes a driver
    * param read.
    */
   nir_variable *layer_var =
      nir_variable_create(nir, nir_var_shader_out,
                          glsl_int_type(), "gl_Layer");
   layer_var->data.location = VARYING_SLOT_LAYER;
   nir_store_var(&b, layer_var, nir_load_view_index(&b), 0x1);
   nir->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_LAYER);

   return true;
}

bool
tu_nir_lower_multiview_sw_fs(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);

   /* In the fragment shader, replace load_view_index with load_layer_id.
    * The VS wrote the view index to gl_Layer, so the FS can read it back
    * from the rasterizer-provided layer ID.
    */
   return nir_shader_lower_instructions(nir,
                                        lower_view_index_to_layer_id_filter,
                                        lower_view_index_to_layer_id, NULL);
}

/* VK_QCOM_render_pass_transform.
 *
 * The application gives positions in the logical (unrotated) space but the
 * framebuffer is in the native (rotated) space. The transform is
 * 0/1/2/3 for identity/90/180/270 degrees, matching the Qualcomm driver:
 *
 *   90:  (X, Y) -> (-Y,  X)
 *   180: (X, Y) -> (-X, -Y)
 *   270: (X, Y) -> ( Y, -X)
 */
bool
tu_nir_lower_rp_transform_position(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_create(impl);

   nir_foreach_block_reverse(block, impl) {
      nir_foreach_instr_reverse(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_store_deref)
            continue;

         nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
         if (!nir_deref_mode_is(deref, nir_var_shader_out))
            continue;

         nir_variable *var = nir_deref_instr_get_variable(deref);
         if (var->data.location != VARYING_SLOT_POS)
            continue;

         b.cursor = nir_before_instr(instr);
         nir_def *pos = intrin->src[1].ssa;
         nir_def *x = nir_channel(&b, pos, 0);
         nir_def *y = nir_channel(&b, pos, 1);
         nir_def *tr = nir_load_rp_transform_ir3(&b);

         nir_def *is90 = nir_ieq_imm(&b, tr, 1);
         nir_def *is180 = nir_ieq_imm(&b, tr, 2);
         nir_def *is270 = nir_ieq_imm(&b, tr, 3);

         nir_def *nx = nir_bcsel(&b, is90, nir_fneg(&b, y),
                       nir_bcsel(&b, is180, nir_fneg(&b, x),
                       nir_bcsel(&b, is270, y, x)));
         nir_def *ny = nir_bcsel(&b, is90, x,
                       nir_bcsel(&b, is180, nir_fneg(&b, y),
                       nir_bcsel(&b, is270, nir_fneg(&b, x), y)));

         nir_def *new_pos = nir_vec4(&b, nx, ny, nir_channel(&b, pos, 2),
                                     nir_channel(&b, pos, 3));
         nir_src_rewrite(&intrin->src[1], new_pos);
         return nir_progress(true, impl, nir_metadata_control_flow);
      }
   }

   return nir_no_progress(impl);
}

/* Fragment shader side: gl_FragCoord, gl_PointCoord and derivatives are
 * reported in the logical space. With the native size NWxNH and fx,fy the
 * native position:
 *
 *   90:  (x, y) = (fy, NW - fx)      270: (x, y) = (NH - fy, fx)
 *   180: (x, y) = (NW - fx, NH - fy)
 *
 * and the point coordinate (s, t) and derivatives follow the same mapping.
 */
static nir_def *
rpt_select4(nir_builder *b, nir_def *tr, nir_def *v0, nir_def *v1,
            nir_def *v2, nir_def *v3)
{
   return nir_bcsel(b, nir_ieq_imm(b, tr, 1), v1,
          nir_bcsel(b, nir_ieq_imm(b, tr, 2), v2,
          nir_bcsel(b, nir_ieq_imm(b, tr, 3), v3, v0)));
}

static bool
lower_rp_transform_fs_instr(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   b->cursor = nir_after_instr(instr);

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      if (intrin->intrinsic == nir_intrinsic_load_frag_coord) {
         nir_def *fc = &intrin->def;
         nir_def *fx = nir_channel(b, fc, 0);
         nir_def *fy = nir_channel(b, fc, 1);
         nir_def *tr = nir_load_rp_transform_ir3(b);
         nir_def *size = nir_load_rp_native_size_ir3(b);
         nir_def *nw = nir_u2f32(b, nir_iand_imm(b, size, 0xffff));
         nir_def *nh = nir_u2f32(b, nir_ushr_imm(b, size, 16));

         nir_def *x = rpt_select4(b, tr, fx, fy, nir_fsub(b, nw, fx),
                                  nir_fsub(b, nh, fy));
         nir_def *y = rpt_select4(b, tr, fy, nir_fsub(b, nw, fx),
                                  nir_fsub(b, nh, fy), fx);
         nir_def *new_fc = nir_vec4(b, x, y, nir_channel(b, fc, 2),
                                    nir_channel(b, fc, 3));
         nir_def_rewrite_uses_after(fc, new_fc);
         return true;
      }
      if (intrin->intrinsic == nir_intrinsic_load_point_coord) {
         nir_def *pc = &intrin->def;
         nir_def *s = nir_channel(b, pc, 0);
         nir_def *t = nir_channel(b, pc, 1);
         nir_def *one = nir_imm_float(b, 1.0f);
         nir_def *tr = nir_load_rp_transform_ir3(b);
         nir_def *ns = rpt_select4(b, tr, s, t, nir_fsub(b, one, s),
                                   nir_fsub(b, one, t));
         nir_def *nt = rpt_select4(b, tr, t, nir_fsub(b, one, s),
                                   nir_fsub(b, one, t), s);
         nir_def *new_pc = nir_vec2(b, ns, nt);
         nir_def_rewrite_uses_after(pc, new_pc);
         return true;
      }
   } else if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      bool is_x = alu->op == nir_op_fddx || alu->op == nir_op_fddx_fine ||
                  alu->op == nir_op_fddx_coarse;
      bool is_y = alu->op == nir_op_fddy || alu->op == nir_op_fddy_fine ||
                  alu->op == nir_op_fddy_coarse;
      if (!is_x && !is_y)
         return false;

      nir_op other = alu->op == nir_op_fddx ? nir_op_fddy :
                     alu->op == nir_op_fddx_fine ? nir_op_fddy_fine :
                     alu->op == nir_op_fddx_coarse ? nir_op_fddy_coarse :
                     alu->op == nir_op_fddy ? nir_op_fddx :
                     alu->op == nir_op_fddy_fine ? nir_op_fddx_fine :
                     nir_op_fddx_coarse;
      nir_def *self = &alu->def;
      nir_def *src = alu->src[0].src.ssa;
      nir_def *neg_self = nir_fneg(b, self);
      nir_def *oth = nir_build_alu1(b, other, src);
      nir_def *neg_oth = nir_fneg(b, oth);
      nir_def *tr = nir_load_rp_transform_ir3(b);
      nir_def *res;
      if (is_x) {
         /* ddx_logical: native ddx, ddy, -ddx, -ddy */
         res = rpt_select4(b, tr, self, oth, neg_self, neg_oth);
      } else {
         /* ddy_logical: native ddy, -ddx, -ddy, ddx */
         res = rpt_select4(b, tr, self, neg_oth, neg_self, oth);
      }
      nir_def_rewrite_uses_after(self, res);
      return true;
   }

   return false;
}

bool
tu_nir_lower_rp_transform_fs(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);
   return nir_shader_instructions_pass(nir, lower_rp_transform_fs_instr,
                                       nir_metadata_control_flow, NULL);
}
