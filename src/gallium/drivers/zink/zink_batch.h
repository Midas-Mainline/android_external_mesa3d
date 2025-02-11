/*
 * Copyright 2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef ZINK_BATCH_H
#define ZINK_BATCH_H

#include <vulkan/vulkan.h>

#include "util/list.h"
#include "util/u_dynarray.h"

#include "zink_fence.h"

struct pipe_reference;

struct zink_context;
struct zink_descriptor_set;
struct zink_framebuffer;
struct zink_image_view;
struct zink_program;
struct zink_render_pass;
struct zink_resource;
struct zink_sampler_view;
struct zink_surface;

enum zink_queue {
   ZINK_QUEUE_GFX,
   ZINK_QUEUE_COMPUTE,
   ZINK_QUEUE_ANY,
};

struct zink_batch_usage {
   /* this has to be atomic for fence access, so we can't use a bitmask and make everything neat */
   uint32_t usage[2]; //gfx, compute
};

struct zink_batch_state {
   struct zink_fence fence;
   VkCommandPool cmdpool;
   VkCommandBuffer cmdbuf;

   struct zink_resource *flush_res;

   unsigned short descs_used; //number of descriptors currently allocated

   struct set *fbs;
   struct set *programs;

   struct set *resources;
   struct set *surfaces;
   struct set *bufferviews;
   struct set *desc_sets;

   struct util_dynarray persistent_resources;
   struct util_dynarray zombie_samplers;

   struct set *active_queries; /* zink_query objects which were active at some point in this batch */

   VkDeviceSize resource_size;

   uint32_t batch_id;
   bool is_compute;
};

struct zink_batch {
   struct zink_batch_state *state;
   enum zink_queue queue;

   uint32_t last_batch_id;

   bool has_work;
   bool in_rp; //renderpass is currently active
};


static inline struct zink_batch_state *
zink_batch_state(struct zink_fence *fence)
{
   return (struct zink_batch_state *)fence;
}

void
zink_reset_batch_state(struct zink_context *ctx, struct zink_batch_state *bs);

void
zink_batch_reset_all(struct zink_context *ctx, enum zink_queue queue);

void
zink_batch_state_destroy(struct zink_screen *screen, struct zink_batch_state *bs);

void
zink_batch_state_clear_resources(struct zink_screen *screen, struct zink_batch_state *bs);

void
zink_reset_batch(struct zink_context *ctx, struct zink_batch *batch);
void
zink_batch_reference_framebuffer(struct zink_batch *batch,
                                 struct zink_framebuffer *fb);
void
zink_start_batch(struct zink_context *ctx, struct zink_batch *batch);

void
zink_end_batch(struct zink_context *ctx, struct zink_batch *batch);

enum zink_queue
zink_batch_reference_resource_rw(struct zink_batch *batch,
                                 struct zink_resource *res,
                                 bool write);

void
zink_batch_reference_sampler_view(struct zink_batch *batch,
                                  struct zink_sampler_view *sv);

void
zink_batch_reference_program(struct zink_batch *batch,
                             struct zink_program *pg);

void
zink_batch_reference_image_view(struct zink_batch *batch,
                                struct zink_image_view *image_view);

bool
zink_batch_add_desc_set(struct zink_batch *batch, struct zink_descriptor_set *zds);

void
zink_batch_usage_set(struct zink_batch_usage *u, enum zink_queue queue, uint32_t batch_id);
bool
zink_batch_usage_matches(struct zink_batch_usage *u, enum zink_queue queue, uint32_t batch_id);
bool
zink_batch_usage_exists(struct zink_batch_usage *u);
#endif
