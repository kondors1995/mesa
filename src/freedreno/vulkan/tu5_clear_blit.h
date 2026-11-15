/*
 * Copyright © 2026 Freedreno / Turnip contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TU5_CLEAR_BLIT_H
#define TU5_CLEAR_BLIT_H

#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_format.h"
#include "vulkan/vulkan_core.h"

struct fdl6_view;
struct tu_cmd_buffer;
struct tu_cs;

void tu5_emit_blit_consts_load(struct tu_cmd_buffer *cmd,
                               struct tu_cs *cs,
                               bool is_frag,
                               uint32_t offset,
                               const void *consts,
                               uint32_t size_vec4);

void tu5_r3d_src(struct tu_cmd_buffer *cmd,
                 struct tu_cs *cs,
                 const struct fdl6_view *iview,
                 uint32_t layer,
                 VkFilter filter,
                 enum pipe_format dst_format);

void tu5_r3d_src_buffer(struct tu_cmd_buffer *cmd,
                        struct tu_cs *cs,
                        enum pipe_format format,
                        uint64_t va,
                        uint32_t pitch,
                        uint32_t width,
                        uint32_t height,
                        enum pipe_format dst_format);

void tu5_r3d_src_load(struct tu_cmd_buffer *cmd,
                      struct tu_cs *cs,
                      const struct fdl6_view *iview,
                      uint32_t layer,
                      VkFilter filter);

void tu5_r3d_dst(struct tu_cs *cs,
                 const struct fdl6_view *iview,
                 uint32_t layer,
                 enum pipe_format src_format);

void tu5_r3d_dst_buffer(struct tu_cs *cs,
                        enum pipe_format format,
                        uint64_t va,
                        uint32_t pitch,
                        enum pipe_format src_format);

void tu5_r3d_setup(struct tu_cmd_buffer *cmd,
                   struct tu_cs *cs,
                   enum pipe_format src_format,
                   enum pipe_format dst_format,
                   VkImageAspectFlags aspect_mask,
                   bool z_scale,
                   bool clear,
                   bool swap_coords,
                   VkSampleCountFlagBits src_samples,
                   VkSampleCountFlagBits dst_samples,
                   bool outside_pass);

void tu5_r3d_teardown(struct tu_cmd_buffer *cmd, struct tu_cs *cs);

void tu5_clear_sysmem_attachments(struct tu_cmd_buffer *cmd,
                                  uint32_t attachment_count,
                                  const VkClearAttachment *attachments,
                                  uint32_t rect_count,
                                  const VkClearRect *rects);

void tu5_load_gmem_attachment(struct tu_cmd_buffer *cmd,
                              struct tu_cs *cs,
                              uint32_t a,
                              uint32_t gmem_a,
                              bool force_load);

void tu5_clear_gmem_attachment(struct tu_cmd_buffer *cmd,
                               struct tu_cs *cs,
                               bool per_layer_render_area,
                               uint32_t a);

void tu5_store_gmem_attachment(struct tu_cmd_buffer *cmd,
                               struct tu_cs *cs,
                               uint32_t a,
                               uint32_t gmem_a,
                               uint32_t layers,
                               uint32_t layer_mask,
                               bool per_layer_render_area);

#endif /* TU5_CLEAR_BLIT_H */
