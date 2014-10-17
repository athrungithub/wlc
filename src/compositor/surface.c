#include "surface.h"
#include "compositor.h"
#include "output.h"
#include "view.h"
#include "region.h"
#include "buffer.h"
#include "callback.h"
#include "macros.h"

#include "render/render.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <wayland-server.h>

static void
create_notify(struct wlc_surface *surface)
{
   if (surface->created)
      return;

   struct wlc_view *view;
   if (!(view = wlc_view_for_surface_in_list(surface, &surface->compositor->unmapped)))
      return;

   // FIXME: decouple views from surfaces more, I don't think they belong here, nor we should use lists in first place.
   if (!view->shell_surface && !view->xdg_surface && !view->x11_window)
      return;

   wl_list_remove(&view->link);
   wl_list_insert(surface->space->views.prev, &view->link);

   view->pending.geometry.size = surface->size;

   if (surface->compositor->interface.view.created &&
      !surface->compositor->interface.view.created(surface->compositor, view)) {
      wlc_surface_free(surface);
      return;
   }

   surface->created = true;
}

static void
wlc_surface_state_set_buffer(struct wlc_surface_state *state, struct wlc_buffer *buffer)
{
   if (state->buffer == buffer)
      return;

   if (state->buffer)
      wlc_buffer_free(state->buffer);

   state->buffer = wlc_buffer_use(buffer);
}

static void
wlc_surface_attach(struct wlc_surface *surface, struct wlc_buffer *buffer)
{
   if (buffer) {
      create_notify(surface);
   } else {
      /* TODO: unmap surface if mapped */
   }

   surface->compositor->render->api.attach(surface, buffer);

   struct wlc_size size = { 0, 0 };
   if (buffer) {
#if 0
      switch (transform) {
         case WL_OUTPUT_TRANSFORM_90:
         case WL_OUTPUT_TRANSFORM_270:
         case WL_OUTPUT_TRANSFORM_FLIPPED_90:
         case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            width = surface->buffer_ref.buffer->height / vp->buffer.scale;
            height = surface->buffer_ref.buffer->width / vp->buffer.scale;
            break;
         default:
            width = surface->buffer_ref.buffer->width / vp->buffer.scale;
            height = surface->buffer_ref.buffer->height / vp->buffer.scale;
            break;
      }
#endif
      size = buffer->size;
   }

   surface->size = size;
}

static void
wlc_surface_commit_state(struct wlc_surface *surface, struct wlc_surface_state *pending, struct wlc_surface_state *out)
{
   if (pending->newly_attached)
      wlc_surface_attach(surface, pending->buffer);

   wlc_surface_state_set_buffer(out, pending->buffer);
   wlc_surface_state_set_buffer(pending, NULL);

   pending->sx = pending->sy = 0;
   pending->newly_attached = false;

   wl_list_insert_list(&out->frame_cb_list, &pending->frame_cb_list);
   wl_list_init(&pending->frame_cb_list);

   pixman_region32_union(&out->damage, &out->damage, &pending->damage);
   pixman_region32_intersect_rect(&out->damage, &out->damage, 0, 0, surface->size.w, surface->size.h);
   pixman_region32_clear(&surface->pending.damage);

   pixman_region32_t opaque;
   pixman_region32_init(&opaque);
   pixman_region32_intersect_rect(&opaque, &pending->opaque, 0, 0, surface->size.w, surface->size.h);

   if (!pixman_region32_equal(&opaque, &out->opaque))
      pixman_region32_copy(&out->opaque, &opaque);

   pixman_region32_fini(&opaque);

   pixman_region32_intersect_rect(&out->input, &pending->input, 0, 0, surface->size.w, surface->size.h);
}

static void
wl_cb_surface_destroy(struct wl_client *wl_client, struct wl_resource *resource)
{
   (void)wl_client;
   wl_resource_destroy(resource);
}

static void
wl_cb_surface_attach(struct wl_client *wl_client, struct wl_resource *resource, struct wl_resource *buffer_resource, int32_t x, int32_t y)
{
   struct wlc_surface *surface = wl_resource_get_user_data(resource);

   // XXX: We can't set or get buffer_resource user data.
   // It seems to be owned by somebody else?
   // What use is user data which we can't use...
   //
   // According to #wayland, user data isn't actually user data, but internal data of the resource.
   // We only own the user data if the resource was created by us.

   struct wlc_buffer *buffer = NULL;
   if (buffer_resource && !(buffer = wlc_buffer_resource_get_container(buffer_resource)) && !(buffer = wlc_buffer_new(buffer_resource))) {
      wl_client_post_no_memory(wl_client);
      return;
   }

   wlc_surface_state_set_buffer(&surface->pending, buffer);

   surface->pending.sx = x;
   surface->pending.sy = y;
   surface->pending.newly_attached = true;
}

static void
wl_cb_surface_damage(struct wl_client *wl_client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height)
{
   (void)wl_client;
   struct wlc_surface *surface = wl_resource_get_user_data(resource);
   pixman_region32_union_rect(&surface->pending.damage, &surface->pending.damage, x, y, width, height);
}

static void
wl_cb_surface_frame(struct wl_client *wl_client, struct wl_resource *resource, uint32_t callback_id)
{
   struct wl_resource *callback_resource;
   if (!(callback_resource = wl_resource_create(wl_client, &wl_callback_interface, 1, callback_id)))
      goto fail;

   struct wlc_callback *callback;
   if (!(callback = wlc_callback_new(callback_resource)))
      goto fail;

   wlc_callback_implement(callback);

   struct wlc_surface *surface = wl_resource_get_user_data(resource);
   wl_list_insert(&surface->pending.frame_cb_list, &callback->link);

   return;

fail:
   if (callback_resource)
      wl_resource_destroy(callback_resource);
   wl_resource_post_no_memory(resource);
}

static void
wl_cb_surface_set_opaque_region(struct wl_client *wl_client, struct wl_resource *resource, struct wl_resource *region_resource)
{
   (void)wl_client;
   struct wlc_surface *surface = wl_resource_get_user_data(resource);

   if (region_resource) {
      struct wlc_region *region = wl_resource_get_user_data(region_resource);
      pixman_region32_copy(&surface->pending.opaque, &region->region);
   } else {
      pixman_region32_clear(&surface->pending.opaque);
   }
}

static void
wl_cb_surface_set_input_region(struct wl_client *wl_client, struct wl_resource *resource, struct wl_resource *region_resource)
{
   (void)wl_client;
   struct wlc_surface *surface = wl_resource_get_user_data(resource);

   if (region_resource) {
      struct wlc_region *region = wl_resource_get_user_data(region_resource);
      pixman_region32_copy(&surface->pending.input, &region->region);
   } else {
      pixman_region32_fini(&surface->pending.input);
      pixman_region32_init_rect(&surface->pending.input, INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX);
   }
}

static void
wl_cb_surface_commit(struct wl_client *wl_client, struct wl_resource *resource)
{
   (void)wl_client;
   struct wlc_surface *surface = wl_resource_get_user_data(resource);
   wlc_surface_commit_state(surface, &surface->pending, &surface->commit);
}

static void
wl_cb_surface_set_buffer_transform(struct wl_client *wl_client, struct wl_resource *resource, int32_t transform)
{
   (void)wl_client, (void)resource, (void)transform;
   STUBL(resource);
}

static void
wl_cb_surface_set_buffer_scale(struct wl_client *wl_client, struct wl_resource *resource, int32_t scale)
{
   (void)wl_client, (void)resource, (void)scale;
   STUBL(resource);
}

static const struct wl_surface_interface wl_surface_implementation = {
   wl_cb_surface_destroy,
   wl_cb_surface_attach,
   wl_cb_surface_damage,
   wl_cb_surface_frame,
   wl_cb_surface_set_opaque_region,
   wl_cb_surface_set_input_region,
   wl_cb_surface_commit,
   wl_cb_surface_set_buffer_transform,
   wl_cb_surface_set_buffer_scale
};

static void
wl_cb_surface_destructor(struct wl_resource *resource)
{
   assert(resource);
   struct wlc_surface *surface = wl_resource_get_user_data(resource);

   if (surface) {
      surface->resource = NULL;
      wlc_surface_free(surface);
   }
}

void
wlc_surface_implement(struct wlc_surface *surface, struct wl_resource *resource)
{
   assert(surface);

   if (surface->resource == resource)
      return;

   if (surface->resource)
      wl_resource_destroy(surface->resource);

   surface->resource = resource;
   wl_resource_set_implementation(surface->resource, &wl_surface_implementation, surface, wl_cb_surface_destructor);
}

void
wlc_surface_free(struct wlc_surface *surface)
{
   assert(surface);

   if (surface->resource) {
      wl_resource_destroy(surface->resource);
      return;
   }

   struct wlc_view *view;
   if ((surface->space && (view = wlc_view_for_surface_in_list(surface, &surface->space->views))) ||
       (view = wlc_view_for_surface_in_list(surface, &surface->compositor->unmapped))) {

      if (surface->created) {
         wl_list_remove(&view->link);
         wl_list_insert(&surface->compositor->unmapped, &view->link);
      }

      if (surface->created && surface->compositor->interface.view.destroyed)
         surface->compositor->interface.view.destroyed(surface->compositor, view);

      wlc_view_free(view);
   }

   if (surface->compositor && surface->compositor->render)
      surface->compositor->render->api.destroy(surface);

   if (surface->commit.buffer)
      wlc_buffer_free(surface->commit.buffer);

   if (surface->pending.buffer)
      wlc_buffer_free(surface->pending.buffer);

   free(surface);
}

struct wlc_surface*
wlc_surface_new(struct wlc_compositor *compositor, struct wlc_space *space)
{
   struct wlc_surface *surface;
   if (!(surface = calloc(1, sizeof(struct wlc_surface))))
      return NULL;

   surface->space = space;
   surface->compositor = compositor;
   wl_list_init(&surface->commit.frame_cb_list);
   wl_list_init(&surface->pending.frame_cb_list);
   return surface;
}
