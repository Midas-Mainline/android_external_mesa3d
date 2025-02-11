#include "zink_batch.h"

#include "zink_context.h"
#include "zink_fence.h"
#include "zink_framebuffer.h"
#include "zink_query.h"
#include "zink_program.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_surface.h"

#include "util/hash_table.h"
#include "util/u_debug.h"
#include "util/set.h"

#include "wsi_common.h"

static void
batch_usage_unset(struct zink_batch_usage *u, enum zink_queue queue, uint32_t batch_id)
{
   p_atomic_cmpxchg(&u->usage[queue], batch_id, 0);
}

void
zink_batch_state_clear_resources(struct zink_screen *screen, struct zink_batch_state *bs)
{
   /* unref all used resources */
   set_foreach(bs->resources, entry) {
      struct zink_resource_object *obj = (struct zink_resource_object *)entry->key;
      batch_usage_unset(&obj->reads, !!bs->is_compute, bs->batch_id);
      batch_usage_unset(&obj->writes, !!bs->is_compute, bs->batch_id);
      zink_resource_object_reference(screen, &obj, NULL);
      _mesa_set_remove(bs->resources, entry);
   }
}

void
zink_reset_batch_state(struct zink_context *ctx, struct zink_batch_state *bs)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);

   zink_batch_state_clear_resources(screen, bs);

   set_foreach(bs->active_queries, entry) {
      struct zink_query *query = (void*)entry->key;
      zink_prune_query(screen, query);
      _mesa_set_remove(bs->active_queries, entry);
   }

   set_foreach(bs->surfaces, entry) {
      struct zink_surface *surf = (struct zink_surface *)entry->key;
      batch_usage_unset(&surf->batch_uses, !!bs->is_compute, bs->batch_id);
      pipe_surface_reference((struct pipe_surface**)&surf, NULL);
      _mesa_set_remove(bs->surfaces, entry);
   }
   set_foreach(bs->bufferviews, entry) {
      struct zink_buffer_view *buffer_view = (struct zink_buffer_view *)entry->key;
      batch_usage_unset(&buffer_view->batch_uses, !!bs->is_compute, bs->batch_id);
      zink_buffer_view_reference(screen, &buffer_view, NULL);
      _mesa_set_remove(bs->bufferviews, entry);
   }

   util_dynarray_foreach(&bs->zombie_samplers, VkSampler, samp) {
      vkDestroySampler(screen->dev, *samp, NULL);
   }
   util_dynarray_clear(&bs->zombie_samplers);
   util_dynarray_clear(&bs->persistent_resources);

   set_foreach(bs->desc_sets, entry) {
      struct zink_descriptor_set *zds = (void*)entry->key;
      batch_usage_unset(&zds->batch_uses, !!bs->is_compute, bs->batch_id);
      /* reset descriptor pools when no bs is using this program to avoid
       * having some inactive program hogging a billion descriptors
       */
      pipe_reference(&zds->reference, NULL);
      zink_descriptor_set_recycle(zds);
      _mesa_set_remove(bs->desc_sets, entry);
   }

   set_foreach(bs->programs, entry) {
      if (bs->is_compute) {
         struct zink_compute_program *comp = (struct zink_compute_program*)entry->key;
         bool in_use = comp == ctx->curr_compute;
         if (zink_compute_program_reference(screen, &comp, NULL) && in_use)
            ctx->curr_compute = NULL;
      } else {
         struct zink_gfx_program *prog = (struct zink_gfx_program*)entry->key;
         bool in_use = prog == ctx->curr_program;
         if (zink_gfx_program_reference(screen, &prog, NULL) && in_use)
            ctx->curr_program = NULL;
      }
      _mesa_set_remove(bs->programs, entry);
   }

   set_foreach(bs->fbs, entry) {
      struct zink_framebuffer *fb = (void*)entry->key;
      zink_framebuffer_reference(screen, &fb, NULL);
      _mesa_set_remove(bs->fbs, entry);
   }

   bs->flush_res = NULL;

   bs->descs_used = 0;
   ctx->resource_size[bs->is_compute] -= bs->resource_size;
   bs->resource_size = 0;
}

void
zink_batch_reset_all(struct zink_context *ctx, enum zink_queue queue)
{
   hash_table_foreach(&ctx->batch_states[queue], entry) {
      struct zink_batch_state *bs = entry->data;
      zink_reset_batch_state(ctx, bs);
      _mesa_hash_table_remove(&ctx->batch_states[queue], entry);
      util_dynarray_append(&ctx->free_batch_states[queue], struct zink_batch_state *, bs);
   }
}

void
zink_batch_state_destroy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!bs)
      return;

   if (bs->cmdbuf)
      vkFreeCommandBuffers(screen->dev, bs->cmdpool, 1, &bs->cmdbuf);
   if (bs->cmdpool)
      vkDestroyCommandPool(screen->dev, bs->cmdpool, NULL);

   _mesa_set_destroy(bs->fbs, NULL);
   _mesa_set_destroy(bs->resources, NULL);
   util_dynarray_fini(&bs->zombie_samplers);
   _mesa_set_destroy(bs->surfaces, NULL);
   _mesa_set_destroy(bs->bufferviews, NULL);
   _mesa_set_destroy(bs->programs, NULL);
   _mesa_set_destroy(bs->desc_sets, NULL);
   _mesa_set_destroy(bs->active_queries, NULL);
   ralloc_free(bs);
}

static struct zink_batch_state *
create_batch_state(struct zink_context *ctx, enum zink_queue queue)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_batch_state *bs = rzalloc(NULL, struct zink_batch_state);
   VkCommandPoolCreateInfo cpci = {};
   cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cpci.queueFamilyIndex = screen->gfx_queue;
   cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   if (vkCreateCommandPool(screen->dev, &cpci, NULL, &bs->cmdpool) != VK_SUCCESS)
      goto fail;

   VkCommandBufferAllocateInfo cbai = {};
   cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbai.commandPool = bs->cmdpool;
   cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbai.commandBufferCount = 1;

   if (vkAllocateCommandBuffers(screen->dev, &cbai, &bs->cmdbuf) != VK_SUCCESS)
      goto fail;

#define SET_CREATE_OR_FAIL(ptr) \
   ptr = _mesa_pointer_set_create(bs); \
   if (!ptr) \
      goto fail

   SET_CREATE_OR_FAIL(bs->fbs);
   SET_CREATE_OR_FAIL(bs->resources);
   SET_CREATE_OR_FAIL(bs->surfaces);
   SET_CREATE_OR_FAIL(bs->bufferviews);
   SET_CREATE_OR_FAIL(bs->programs);
   SET_CREATE_OR_FAIL(bs->desc_sets);
   SET_CREATE_OR_FAIL(bs->active_queries);
   util_dynarray_init(&bs->zombie_samplers, NULL);
   util_dynarray_init(&bs->persistent_resources, NULL);

   if (!zink_create_fence(screen, bs))
      /* this destroys the batch state on failure */
      return NULL;

   bs->is_compute = queue == ZINK_QUEUE_COMPUTE;

   return bs;
fail:
   zink_batch_state_destroy(screen, bs);
   return NULL;
}

static bool
find_unused_state(struct hash_entry *entry)
{
   struct zink_fence *fence = entry->data;
   /* we can't reset these from fence_finish because threads */
   bool submitted = p_atomic_read(&fence->submitted);
   return !submitted;
}

static void
init_batch_state(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_batch_state *bs = NULL;

   if (util_dynarray_num_elements(&ctx->free_batch_states[batch->queue], struct zink_batch_state*))
      bs = util_dynarray_pop(&ctx->free_batch_states[batch->queue], struct zink_batch_state*);
   if (!bs) {
      struct hash_entry *he = _mesa_hash_table_random_entry(&ctx->batch_states[batch->queue], find_unused_state);
      if (he) { //there may not be any entries available
         bs = he->data;
         _mesa_hash_table_remove(&ctx->batch_states[batch->queue], he);
         zink_reset_batch_state(ctx, bs);
      }
   }
   if (!bs) {
      if (!batch->state) {
         /* this is batch init, so create a few more states for later use */
         for (int i = 0; i < 3; i++) {
            struct zink_batch_state *state = create_batch_state(ctx, batch->queue);
            util_dynarray_append(&ctx->free_batch_states[batch->queue], struct zink_batch_state *, state);
         }
      }
      bs = create_batch_state(ctx, batch->queue);
   }
   batch->state = bs;
}

void
zink_reset_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   bool fresh = !batch->state;

   init_batch_state(ctx, batch);
   assert(batch->state);

   if (!fresh) {
      if (vkResetCommandPool(screen->dev, batch->state->cmdpool, 0) != VK_SUCCESS)
         debug_printf("vkResetCommandPool failed\n");
   }
   batch->has_work = false;
}

void
zink_start_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   zink_reset_batch(ctx, batch);

   VkCommandBufferBeginInfo cbbi = {};
   cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   if (vkBeginCommandBuffer(batch->state->cmdbuf, &cbbi) != VK_SUCCESS)
      debug_printf("vkBeginCommandBuffer failed\n");

   batch->state->batch_id = ctx->curr_batch;
   if (ctx->last_fence[batch->queue]) {
      struct zink_batch_state *last_state = zink_batch_state(ctx->last_fence[batch->queue]);
      batch->last_batch_id = last_state->batch_id;
   }
   if (!ctx->queries_disabled)
      zink_resume_queries(ctx, batch);
}

void
zink_end_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   if (!ctx->queries_disabled)
      zink_suspend_queries(ctx, batch);

   if (vkEndCommandBuffer(batch->state->cmdbuf) != VK_SUCCESS) {
      debug_printf("vkEndCommandBuffer failed\n");
      return;
   }

   zink_fence_init(ctx, batch);

   util_dynarray_foreach(&batch->state->persistent_resources, struct zink_resource*, res) {
       struct zink_screen *screen = zink_screen(ctx->base.screen);
       assert(!(*res)->obj->offset);
       VkMappedMemoryRange range = {
          VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
          NULL,
          (*res)->obj->mem,
          (*res)->obj->offset,
          VK_WHOLE_SIZE,
       };
       vkFlushMappedMemoryRanges(screen->dev, 1, &range);
   }

   VkSubmitInfo si = {};
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.waitSemaphoreCount = 0;
   si.pWaitSemaphores = NULL;
   si.signalSemaphoreCount = 0;
   si.pSignalSemaphores = NULL;
   si.pWaitDstStageMask = NULL;
   si.commandBufferCount = 1;
   si.pCommandBuffers = &batch->state->cmdbuf;

   struct wsi_memory_signal_submit_info mem_signal = {
      .sType = VK_STRUCTURE_TYPE_WSI_MEMORY_SIGNAL_SUBMIT_INFO_MESA,
      .pNext = si.pNext,
   };

   if (batch->state->flush_res) {
      mem_signal.memory = batch->state->flush_res->obj->mem;
      si.pNext = &mem_signal;
   }

   if (vkQueueSubmit(ctx->queue, 1, &si, batch->state->fence.fence) != VK_SUCCESS) {
      debug_printf("ZINK: vkQueueSubmit() failed\n");
      ctx->is_device_lost = true;

      if (ctx->reset.reset) {
         ctx->reset.reset(ctx->reset.data, PIPE_GUILTY_CONTEXT_RESET);
      }
   }

   ctx->last_fence[batch->queue] = &batch->state->fence;
   _mesa_hash_table_insert_pre_hashed(&ctx->batch_states[batch->queue], batch->state->batch_id, (void*)(uintptr_t)batch->state->batch_id, batch->state);
   ctx->resource_size[batch->queue] += batch->state->resource_size;
}

/* returns a queue based on whether a resource
   has usage on a different queue than 'batch' belongs to
 */
enum zink_queue
zink_batch_reference_resource_rw(struct zink_batch *batch, struct zink_resource *res, bool write)
{
   enum zink_queue batch_to_flush = 0;

   /* u_transfer_helper unrefs the stencil buffer when the depth buffer is unrefed,
    * so we add an extra ref here to the stencil buffer to compensate
    */
   struct zink_resource *stencil;

   zink_get_depth_stencil_resources((struct pipe_resource*)res, NULL, &stencil);

   if (batch->queue == ZINK_QUEUE_COMPUTE) {
      if ((write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_RW, ZINK_QUEUE_GFX)) ||
          (!write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_WRITE, ZINK_QUEUE_GFX)))
         batch_to_flush = ZINK_QUEUE_GFX;
   } else {
      if ((write && zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_READ, ZINK_QUEUE_COMPUTE)) ||
          zink_resource_has_usage(res, ZINK_RESOURCE_ACCESS_WRITE, ZINK_QUEUE_COMPUTE))
         batch_to_flush = ZINK_QUEUE_COMPUTE;
   }

   /* if the resource already has usage of any sort set for this batch, we can skip hashing */
   if (!zink_batch_usage_matches(&res->obj->reads, batch->queue, batch->state->batch_id) &&
       !zink_batch_usage_matches(&res->obj->writes, batch->queue, batch->state->batch_id)) {
      bool found = false;
      _mesa_set_search_and_add(batch->state->resources, res->obj, &found);
      if (!found) {
         pipe_reference(NULL, &res->obj->reference);
         if (!batch->last_batch_id || !zink_batch_usage_matches(&res->obj->reads, batch->queue, batch->last_batch_id))
            /* only add resource usage if it's "new" usage, though this only checks the most recent usage
             * and not all pending usages
             */
            batch->state->resource_size += res->obj->size;
         if (stencil) {
            pipe_reference(NULL, &stencil->obj->reference);
            if (!batch->last_batch_id || !zink_batch_usage_matches(&stencil->obj->reads, batch->queue, batch->last_batch_id))
               batch->state->resource_size += stencil->obj->size;
         }
      }
       }
   if (write) {
      if (stencil)
         zink_batch_usage_set(&stencil->obj->writes, batch->queue, batch->state->batch_id);
      zink_batch_usage_set(&res->obj->writes, batch->queue, batch->state->batch_id);
   } else {
      if (stencil)
         zink_batch_usage_set(&stencil->obj->reads, batch->queue, batch->state->batch_id);
      zink_batch_usage_set(&res->obj->reads, batch->queue, batch->state->batch_id);
   }
   /* multiple array entries are fine */
   if (res->obj->persistent_maps)
      util_dynarray_append(&batch->state->persistent_resources, struct zink_resource*, res);

   batch->has_work = true;
   return batch_to_flush;
}

static bool
ptr_add_usage(struct zink_batch *batch, struct set *s, void *ptr, struct zink_batch_usage *u)
{
   bool found = false;
   if (zink_batch_usage_matches(u, batch->queue, batch->state->batch_id))
      return false;
   _mesa_set_search_and_add(s, ptr, &found);
   assert(!found);
   zink_batch_usage_set(u, batch->queue, batch->state->batch_id);
   return true;
}

void
zink_batch_reference_sampler_view(struct zink_batch *batch,
                                  struct zink_sampler_view *sv)
{
   if (sv->base.target == PIPE_BUFFER) {
      if (!ptr_add_usage(batch, batch->state->bufferviews, sv->buffer_view, &sv->buffer_view->batch_uses))
         return;
      pipe_reference(NULL, &sv->buffer_view->reference);
   } else {
      if (!ptr_add_usage(batch, batch->state->surfaces, sv->image_view, &sv->image_view->batch_uses))
         return;
      pipe_reference(NULL, &sv->image_view->base.reference);
   }
   batch->has_work = true;
}

void
zink_batch_reference_framebuffer(struct zink_batch *batch,
                                 struct zink_framebuffer *fb)
{
   bool found;
   _mesa_set_search_or_add(batch->state->fbs, fb, &found);
   if (!found)
      pipe_reference(NULL, &fb->reference);
}

void
zink_batch_reference_program(struct zink_batch *batch,
                             struct zink_program *pg)
{
   bool found = false;
   _mesa_set_search_and_add(batch->state->programs, pg, &found);
   if (!found)
      pipe_reference(NULL, &pg->reference);
   batch->has_work = true;
}

bool
zink_batch_add_desc_set(struct zink_batch *batch, struct zink_descriptor_set *zds)
{
   if (!ptr_add_usage(batch, batch->state->desc_sets, zds, &zds->batch_uses))
      return false;
   pipe_reference(NULL, &zds->reference);
   return true;
}

void
zink_batch_reference_image_view(struct zink_batch *batch,
                                struct zink_image_view *image_view)
{
   if (image_view->base.resource->target == PIPE_BUFFER) {
      if (!ptr_add_usage(batch, batch->state->bufferviews, image_view->buffer_view, &image_view->buffer_view->batch_uses))
         return;
      pipe_reference(NULL, &image_view->buffer_view->reference);
   } else {
      if (!ptr_add_usage(batch, batch->state->surfaces, image_view->surface, &image_view->surface->batch_uses))
         return;
      pipe_reference(NULL, &image_view->surface->base.reference);
   }
   batch->has_work = true;
}

void
zink_batch_usage_set(struct zink_batch_usage *u, enum zink_queue queue, uint32_t batch_id)
{
   if (queue == ZINK_QUEUE_ANY) {
      p_atomic_set(&u->usage[ZINK_QUEUE_GFX], batch_id);
      p_atomic_set(&u->usage[ZINK_QUEUE_COMPUTE], batch_id);
   } else
      p_atomic_set(&u->usage[queue], batch_id);
}

bool
zink_batch_usage_matches(struct zink_batch_usage *u, enum zink_queue queue, uint32_t batch_id)
{
   if (queue < ZINK_QUEUE_ANY) {
      uint32_t usage = p_atomic_read(&u->usage[queue]);
      return usage == batch_id;
   }
   for (unsigned i = 0; i < ZINK_QUEUE_ANY; i++) {
      uint32_t usage = p_atomic_read(&u->usage[queue]);
      if (usage == batch_id)
         return true;
   }
   return false;
}

bool
zink_batch_usage_exists(struct zink_batch_usage *u)
{
   uint32_t usage = p_atomic_read(&u->usage[ZINK_QUEUE_GFX]);
   if (usage)
      return true;
   usage = p_atomic_read(&u->usage[ZINK_QUEUE_COMPUTE]);
   return !!usage;
}
