/*
 * Copyright © 2026 Freedreno / Turnip contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TU5_FORMATS_H
#define TU5_FORMATS_H

#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_format.h"
#include "vulkan/vulkan_core.h"

#include "tu_common.h"

struct fdl_layout;
struct fdl_view_args;
struct fdl6_view;
struct tu_image_view;
struct tu_sampler;

struct tu5_native_format {
   uint8_t fmt;
   uint8_t swap;
   bool valid;
};

bool tu5_format_vtx_supported(enum pipe_format format);
bool tu5_format_tex_supported(enum pipe_format format);
bool tu5_format_color_supported(enum pipe_format format);

struct tu5_native_format tu5_format_vtx(enum pipe_format format);
struct tu5_native_format tu5_format_color(enum pipe_format format,
                                          bool linear);
struct tu5_native_format tu5_format_texture(enum pipe_format format);
uint32_t tu5_pipe2depth(enum pipe_format format);
uint32_t tu5_ssbo_tex_fmt(enum pipe_format format);

void tu5_sampler_init(struct tu_sampler *sampler,
                      const VkSamplerCreateInfo *pCreateInfo,
                      uint32_t border_color);

void tu5_buffer_view_init(uint32_t *dst,
                          enum pipe_format format,
                          const uint8_t swiz[4],
                          uint64_t va,
                          uint64_t range);

void tu5_view_init(struct fdl6_view *iview,
                   const struct fdl_layout **layouts,
                   const struct fdl_view_args *args);

void tu5_fill_storage_image_desc(uint32_t ssbo0[4],
                                 uint32_t ssbo1[2],
                                 const struct tu_image_view *iview);

void tu5_fill_storage_buffer_desc(uint32_t ssbo0[4],
                                  uint32_t ssbo1[2],
                                  uint64_t va,
                                  uint64_t range,
                                  enum pipe_format format);

#endif /* TU5_FORMATS_H */
