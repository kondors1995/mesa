/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2021 Valve Corporation
 * Copyright © 2026 Freedreno / Turnip contributors
 * SPDX-License-Identifier: MIT
 */

#define A6XX_PERFCNTRS_XML
#include "tu5_formats.h"

#include "tu_image.h"
#include "tu_sampler.h"
#include "tu_util.h"
#include "fdl/freedreno_layout.h"

#include "common/fd5_hw.h"

static inline enum a5xx_tex_filter
tu5_tex_filter(VkFilter filter, unsigned aniso)
{
   switch (filter) {
   case VK_FILTER_NEAREST:
      return A5XX_TEX_NEAREST;
   case VK_FILTER_LINEAR:
      return aniso ? A5XX_TEX_ANISO : A5XX_TEX_LINEAR;
   default:
      return A5XX_TEX_NEAREST;
   }
}

static inline enum a5xx_tex_clamp
tu5_tex_wrap(VkSamplerAddressMode address_mode)
{
   static const enum a5xx_tex_clamp lookup[] = {
      [VK_SAMPLER_ADDRESS_MODE_REPEAT]                = A5XX_TEX_REPEAT,
      [VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT]       = A5XX_TEX_MIRROR_REPEAT,
      [VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE]         = A5XX_TEX_CLAMP_TO_EDGE,
      [VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER]       = A5XX_TEX_CLAMP_TO_BORDER,
      [VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE]  = A5XX_TEX_MIRROR_CLAMP,
   };
   assert(address_mode < ARRAY_SIZE(lookup));
   return lookup[address_mode];
}

static inline enum a5xx_tex_swiz
tu5_tex_swiz(unsigned swiz)
{
   switch (swiz) {
   default:
   case PIPE_SWIZZLE_X:
      return A5XX_TEX_X;
   case PIPE_SWIZZLE_Y:
      return A5XX_TEX_Y;
   case PIPE_SWIZZLE_Z:
      return A5XX_TEX_Z;
   case PIPE_SWIZZLE_W:
      return A5XX_TEX_W;
   case PIPE_SWIZZLE_0:
      return A5XX_TEX_ZERO;
   case PIPE_SWIZZLE_1:
      return A5XX_TEX_ONE;
   }
}

static inline enum a5xx_tex_type
tu5_tex_type(enum fdl_view_type type, bool storage)
{
   switch (type) {
   default:
   case FDL_VIEW_TYPE_1D:
      return A5XX_TEX_1D;
   case FDL_VIEW_TYPE_2D:
      return A5XX_TEX_2D;
   case FDL_VIEW_TYPE_CUBE:
      return storage ? A5XX_TEX_2D : A5XX_TEX_CUBE;
   case FDL_VIEW_TYPE_3D:
      return A5XX_TEX_3D;
   case FDL_VIEW_TYPE_BUFFER:
      return A5XX_TEX_BUFFER;
   }
}

void
tu5_sampler_init(struct tu_sampler *sampler,
                 const VkSamplerCreateInfo *pCreateInfo,
                 uint32_t border_color)
{
   unsigned aniso = pCreateInfo->anisotropyEnable ?
      util_last_bit(MIN2(static_cast<uint32_t>(pCreateInfo->maxAnisotropy) >> 1, 8)) : 0;
   bool miplinear = (pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
   float min_lod = CLAMP(pCreateInfo->minLod, 0.0f, 4095.0f / 256.0f);
   float max_lod = CLAMP(pCreateInfo->maxLod, 0.0f, 4095.0f / 256.0f);

   memset(sampler->descriptor, 0, sizeof(sampler->descriptor));

   sampler->descriptor[0] =
      COND(miplinear, A5XX_TEX_SAMP_0_MIPFILTER_LINEAR_NEAR) |
      A5XX_TEX_SAMP_0_XY_MAG(tu5_tex_filter(pCreateInfo->magFilter, aniso)) |
      A5XX_TEX_SAMP_0_XY_MIN(tu5_tex_filter(pCreateInfo->minFilter, aniso)) |
      A5XX_TEX_SAMP_0_WRAP_S(tu5_tex_wrap(pCreateInfo->addressModeU)) |
      A5XX_TEX_SAMP_0_WRAP_T(tu5_tex_wrap(pCreateInfo->addressModeV)) |
      A5XX_TEX_SAMP_0_WRAP_R(tu5_tex_wrap(pCreateInfo->addressModeW)) |
      A5XX_TEX_SAMP_0_ANISO((enum a5xx_tex_aniso) aniso) |
      A5XX_TEX_SAMP_0_LOD_BIAS(pCreateInfo->mipLodBias);

   sampler->descriptor[1] =
      COND(pCreateInfo->compareEnable,
           A5XX_TEX_SAMP_1_COMPARE_FUNC(tu6_compare_func(pCreateInfo->compareOp))) |
      COND(pCreateInfo->unnormalizedCoordinates, A5XX_TEX_SAMP_1_UNNORM_COORDS) |
      COND(pCreateInfo->unnormalizedCoordinates, A5XX_TEX_SAMP_1_MIPFILTER_LINEAR_FAR) |
      A5XX_TEX_SAMP_1_MAX_LOD(max_lod) |
      A5XX_TEX_SAMP_1_MIN_LOD(min_lod);

   if (pCreateInfo->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT)
      sampler->descriptor[1] |= A5XX_TEX_SAMP_1_CUBEMAPSEAMLESSFILTOFF;

   sampler->descriptor[2] = A5XX_TEX_SAMP_2_BCOLOR_OFFSET(border_color);
   sampler->descriptor[3] = 0;
}

void
tu5_buffer_view_init(uint32_t *dst,
                     enum pipe_format format,
                     const uint8_t swiz[4],
                     uint64_t va,
                     uint64_t range)
{
   memset(dst, 0, FDL6_TEX_CONST_DWORDS * sizeof(uint32_t));

   struct tu5_native_format tex_fmt = tu5_format_texture(format);
   uint32_t blocksize = util_format_get_blocksize(format);
   uint32_t elements = blocksize ? MIN2(range / blocksize, (1u << 27)) : 0;

   unsigned char format_swiz[4];
   fdl6_format_swiz(format, false, format_swiz);

   unsigned char view_swiz[4];
   util_format_compose_swizzles(format_swiz, swiz, view_swiz);

   dst[0] =
      A5XX_TEX_CONST_0_TILE_MODE(TILE5_LINEAR) |
      COND(util_format_is_srgb(format), A5XX_TEX_CONST_0_SRGB) |
      A5XX_TEX_CONST_0_FMT((enum a5xx_tex_fmt) tex_fmt.fmt) |
      A5XX_TEX_CONST_0_SWAP((enum a3xx_color_swap) tex_fmt.swap) |
      A5XX_TEX_CONST_0_SWIZ_X(tu5_tex_swiz(view_swiz[0])) |
      A5XX_TEX_CONST_0_SWIZ_Y(tu5_tex_swiz(view_swiz[1])) |
      A5XX_TEX_CONST_0_SWIZ_Z(tu5_tex_swiz(view_swiz[2])) |
      A5XX_TEX_CONST_0_SWIZ_W(tu5_tex_swiz(view_swiz[3]));
   dst[1] =
      A5XX_TEX_CONST_1_WIDTH(elements & MASK(15)) |
      A5XX_TEX_CONST_1_HEIGHT(elements >> 15);
   dst[2] =
      A5XX_TEX_CONST_2_BUFFER |
      A5XX_TEX_CONST_2_TYPE(A5XX_TEX_BUFFER);
   dst[4] = (uint32_t)(va & 0xffffffffu);
   dst[5] = A5XX_TEX_CONST_5_BASE_HI(va >> 32);

   uint32_t ssbo0[4], ssbo1[2];
   tu5_fill_storage_buffer_desc(ssbo0, ssbo1, va, range, format);
   dst[12] = ssbo1[0];
   dst[13] = ssbo1[1];
   dst[14] = 0;
   dst[15] = (blocksize & 0xffu) | ((blocksize ? (ffs(blocksize) - 1) : 0) << 8);
}

void
tu5_view_init(struct fdl6_view *iview,
              const struct fdl_layout **layouts,
              const struct fdl_view_args *args)
{
   const struct fdl_layout *layout = layouts[0];

   memset(iview, 0, sizeof(*iview));

   enum pipe_format format = args->format;
   struct tu5_native_format tex_fmt = tu5_format_texture(format);
   bool linear = !fdl_tile_mode(layout, args->base_miplevel);
   struct tu5_native_format color_fmt = tu5_format_color(format, linear);

   uint32_t width = u_minify(layout->width0, args->base_miplevel);
   uint32_t height = u_minify(layout->height0, args->base_miplevel);
   uint32_t pitch = fdl_pitch(layout, args->base_miplevel);
   uint32_t offset =
      fdl_surface_offset(layout, args->base_miplevel, args->base_array_layer);
   uint64_t base_addr = args->iova + offset;

   iview->offset = offset;
   iview->base_addr = base_addr;
   iview->width = width;
   iview->height = height;
   iview->pitch = pitch;
   iview->format = format;
   iview->color_swap = color_fmt.valid ? color_fmt.swap : WZYX;
   iview->layer_size = layout->layer_first
                          ? layout->layer_size
                          : layout->slices[args->base_miplevel].size0;

   if (color_fmt.valid) {
      enum a5xx_tile_mode tile_mode =
         (enum a5xx_tile_mode) fdl_tile_mode(layout, args->base_miplevel);
      iview->RB_MRT_BUF_INFO =
         A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT((enum a5xx_color_fmt) color_fmt.fmt) |
         A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(tile_mode) |
         A5XX_RB_MRT_BUF_INFO_COLOR_SWAP((enum a3xx_color_swap) color_fmt.swap) |
         COND(util_format_is_srgb(format), A5XX_RB_MRT_BUF_INFO_COLOR_SRGB);
      iview->SP_PS_MRT_REG =
         A5XX_SP_FS_MRT_REG_COLOR_FORMAT((enum a5xx_color_fmt) color_fmt.fmt) |
         COND(util_format_is_pure_sint(format), A5XX_SP_FS_MRT_REG_COLOR_SINT) |
         COND(util_format_is_pure_uint(format), A5XX_SP_FS_MRT_REG_COLOR_UINT) |
         COND(util_format_is_srgb(format), A5XX_SP_FS_MRT_REG_COLOR_SRGB);
   }

   unsigned char format_swiz[4];
   fdl6_format_swiz(format, false, format_swiz);

   const struct util_format_description *desc = util_format_description(format);
   if (desc->nr_channels == 4 && desc->swizzle[3] == PIPE_SWIZZLE_1)
      format_swiz[3] = PIPE_SWIZZLE_1;

   unsigned char view_swiz[4];
   util_format_compose_swizzles(format_swiz, args->swiz, view_swiz);

   enum a5xx_tile_mode tile_mode =
      (enum a5xx_tile_mode) fdl_tile_mode(layout, args->base_miplevel);

   uint32_t texconst0 =
      A5XX_TEX_CONST_0_TILE_MODE(tile_mode) |
      COND(util_format_is_srgb(format), A5XX_TEX_CONST_0_SRGB) |
      A5XX_TEX_CONST_0_FMT((enum a5xx_tex_fmt) tex_fmt.fmt) |
      A5XX_TEX_CONST_0_SWAP((enum a3xx_color_swap) tex_fmt.swap) |
      A5XX_TEX_CONST_0_SAMPLES(tu_msaa_samples(layout->nr_samples)) |
      A5XX_TEX_CONST_0_MIPLVLS(args->level_count - 1) |
      A5XX_TEX_CONST_0_SWIZ_X(tu5_tex_swiz(view_swiz[0])) |
      A5XX_TEX_CONST_0_SWIZ_Y(tu5_tex_swiz(view_swiz[1])) |
      A5XX_TEX_CONST_0_SWIZ_Z(tu5_tex_swiz(view_swiz[2])) |
      A5XX_TEX_CONST_0_SWIZ_W(tu5_tex_swiz(view_swiz[3]));

   uint32_t texconst1 =
      A5XX_TEX_CONST_1_WIDTH(width) |
      A5XX_TEX_CONST_1_HEIGHT(height);

   uint32_t pitchalign = MAX2(layout->pitchalign, 6) - 6;
   uint32_t texconst2_base =
      A5XX_TEX_CONST_2_PITCHALIGN(pitchalign) |
      A5XX_TEX_CONST_2_PITCH(pitch);

   uint32_t texconst3 = 0;
   uint32_t depth = 1;
   uint32_t storage_depth = 1;

   switch (args->type) {
   case FDL_VIEW_TYPE_1D:
   case FDL_VIEW_TYPE_2D:
      texconst3 = A5XX_TEX_CONST_3_ARRAY_PITCH(layout->layer_size);
      depth = args->layer_count;
      storage_depth = args->layer_count;
      break;
   case FDL_VIEW_TYPE_CUBE:
      texconst3 = A5XX_TEX_CONST_3_ARRAY_PITCH(layout->layer_size);
      depth = MAX2(args->layer_count / 6, 1);
      storage_depth = args->layer_count;
      break;
   case FDL_VIEW_TYPE_3D:
      texconst3 =
         A5XX_TEX_CONST_3_MIN_LAYERSZ(layout->slices[layout->mip_levels - 1].size0) |
         A5XX_TEX_CONST_3_ARRAY_PITCH(layout->slices[args->base_miplevel].size0);
      depth = u_minify(layout->depth0, args->base_miplevel);
      storage_depth = depth;
      break;
   default:
      UNREACHABLE("unexpected view type");
   }

   if (layout->tile_all)
      texconst3 |= A5XX_TEX_CONST_3_TILE_ALL;

   iview->descriptor[0] = texconst0;
   iview->descriptor[1] = texconst1;
   iview->descriptor[2] = texconst2_base | A5XX_TEX_CONST_2_TYPE(tu5_tex_type(args->type, false));
   iview->descriptor[3] = texconst3;
   iview->descriptor[4] = (uint32_t)(base_addr & 0xffffffffu);
   iview->descriptor[5] =
      A5XX_TEX_CONST_5_BASE_HI(base_addr >> 32) |
      A5XX_TEX_CONST_5_DEPTH(depth);

   memcpy(iview->storage_descriptor, iview->descriptor, sizeof(iview->descriptor));
   iview->storage_descriptor[2] =
      texconst2_base | A5XX_TEX_CONST_2_TYPE(tu5_tex_type(args->type, true));
   iview->storage_descriptor[5] =
      A5XX_TEX_CONST_5_BASE_HI(base_addr >> 32) |
      A5XX_TEX_CONST_5_DEPTH(storage_depth);

   enum a5xx_tex_fmt ssbo_fmt = (enum a5xx_tex_fmt) tu5_ssbo_tex_fmt(format);
   uint32_t bpp = util_format_get_blocksize(format);
   iview->storage_descriptor[12] =
      A5XX_SSBO_1_0_FMT(ssbo_fmt) | A5XX_SSBO_1_0_WIDTH(width);
   iview->storage_descriptor[13] =
      A5XX_SSBO_1_1_HEIGHT(height) | A5XX_SSBO_1_1_DEPTH(storage_depth);
   iview->storage_descriptor[14] = iview->layer_size;
   iview->storage_descriptor[15] = (bpp & 0xffu) | (pitch << 8);
}

void
tu5_fill_storage_image_desc(uint32_t ssbo0[4],
                            uint32_t ssbo1[2],
                            const struct tu_image_view *iview)
{
   const struct fdl6_view *view = &iview->view;
   const struct fdl_layout *layout = &iview->image->layout[0];
   enum a5xx_tex_fmt fmt = (enum a5xx_tex_fmt) tu5_ssbo_tex_fmt(view->format);
   uint32_t depth = iview->vk.extent.depth;
   if (depth <= 1)
      depth = iview->vk.layer_count;

   ssbo0[0] = (uint32_t)(view->base_addr & 0xffffffffu);
   ssbo0[1] = (uint32_t)(view->base_addr >> 32);
   ssbo0[2] = view->pitch;
   ssbo0[3] = layout->layer_first
                 ? layout->layer_size
                 : layout->slices[iview->vk.base_mip_level].size0;

   ssbo1[0] =
      A5XX_SSBO_1_0_FMT(fmt) |
      A5XX_SSBO_1_0_WIDTH(view->width);
   ssbo1[1] =
      A5XX_SSBO_1_1_HEIGHT(view->height) |
      A5XX_SSBO_1_1_DEPTH(depth);
}

void
tu5_fill_storage_buffer_desc(uint32_t ssbo0[4],
                             uint32_t ssbo1[2],
                             uint64_t va,
                             uint64_t range,
                             enum pipe_format format)
{
   uint32_t sz = (uint32_t) MIN2(range, UINT32_MAX);
   enum a5xx_tex_fmt fmt =
      format != PIPE_FORMAT_NONE ? (enum a5xx_tex_fmt) tu5_ssbo_tex_fmt(format) : TFMT5_NONE;

   ssbo0[0] = (uint32_t)(va & 0xffffffffu);
   ssbo0[1] = (uint32_t)(va >> 32);
   ssbo0[2] = 0;
   ssbo0[3] = 0;

   if (format != PIPE_FORMAT_NONE) {
      uint32_t blocksize = util_format_get_blocksize(format);
      uint32_t elements = blocksize ? sz / blocksize : 0;
      ssbo1[0] = A5XX_SSBO_1_0_FMT(fmt) | A5XX_SSBO_1_0_WIDTH(elements & MASK(16));
      ssbo1[1] = A5XX_SSBO_1_1_HEIGHT(elements >> 16);
   } else {
      ssbo1[0] = A5XX_SSBO_1_0_WIDTH(sz & MASK(16));
      ssbo1[1] = A5XX_SSBO_1_1_HEIGHT(sz >> 16);
   }
}
