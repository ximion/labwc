// SPDX-License-Identifier: GPL-2.0-only
/*
 * xx-zones-v1 implementation
 *
 * A zone corresponds to the usable area of an output (i.e. the output
 * minus exclusive layer-shell surfaces). Its handle is the output name,
 * so all clients requesting a zone on the same output share it and can
 * exchange the handle between processes.
 *
 * Item positions are the top-left corner of the window frame (including
 * server-side decorations) relative to the zone origin, in logical pixels.
 */

#include "zones.h"
#include <assert.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include "common/border.h"
#include "common/macros.h"
#include "common/mem.h"
#include "labwc.h"
#include "output.h"
#include "ssd.h"
#include "view.h"
#include "window-rules.h"
#include "xx-zones-v1-protocol.h"

#define ZONE_MANAGER_VERSION 1

struct zone {
	struct wl_list link;       /* zones */
	char *handle;              /* output name */
	struct wlr_box area;       /* usable area of output in layout coords */
	struct wl_list resources;  /* xx_zone_v1 resources, one per client */
	struct wl_list items;      /* struct zone_item.link */
};

struct zone_item {
	struct wl_resource *resource;
	struct view *view;         /* NULL if inert (toplevel was destroyed) */
	struct zone *zone;         /* NULL if not associated with a zone */
	struct wl_list link;       /* zone.items */

	/* Last state sent to the client */
	struct border extents;
	int x, y;
	bool initial_sent;         /* extents+position sent since add_item */
	bool force_next;           /* send position on next update */
	unsigned int sent_counter; /* incremented on every position event */

	/* Double-buffered set_position request */
	bool has_pending_position;
	int pending_x, pending_y;

	struct wl_listener destroy; /* view->events.destroy */
	struct wl_listener moved;   /* view->events.moved */
	struct wl_listener commit;  /* view->surface->events.commit */
};

static struct wl_global *zone_manager_global;
/*
 * struct zone.link. Statically initialized as zones_update() may be
 * called (via desktop_arrange_all_views()) before zones_init().
 */
static struct wl_list zones = { &zones, &zones };

/* Zones */

static struct zone *
zone_from_handle(const char *handle)
{
	struct zone *zone;
	wl_list_for_each(zone, &zones, link) {
		if (!strcmp(zone->handle, handle)) {
			return zone;
		}
	}
	return NULL;
}

static struct zone *
zone_get_or_create(struct output *output)
{
	if (!output_is_usable(output)) {
		return NULL;
	}
	const char *name = output->wlr_output->name;
	struct zone *zone = zone_from_handle(name);
	if (zone) {
		return zone;
	}
	zone = znew(*zone);
	zone->handle = xstrdup(name);
	zone->area = output_usable_area_in_layout_coords(output);
	wl_list_init(&zone->resources);
	wl_list_init(&zone->items);
	wl_list_insert(&zones, &zone->link);
	return zone;
}

static void
zone_maybe_free(struct zone *zone)
{
	if (!wl_list_empty(&zone->resources) || !wl_list_empty(&zone->items)) {
		return;
	}
	wl_list_remove(&zone->link);
	zfree(zone->handle);
	free(zone);
}

static struct wlr_box
zone_resolve_area(struct zone *zone)
{
	struct output *output = output_from_name(zone->handle);
	if (!output_is_usable(output)) {
		/* The output is gone, fall back to another one */
		output = output_nearest_to_cursor();
	}
	if (!output_is_usable(output)) {
		/* Keep the last known area; never send a negative size */
		return zone->area;
	}
	return output_usable_area_in_layout_coords(output);
}

/*
 * Send an item event on all resources of the zone that belong to the
 * client owning the item, or just on 'only' if that is non-NULL.
 */
static void
zone_send_item_event(struct zone *zone, struct zone_item *item,
		void (*send)(struct wl_resource *, struct wl_resource *),
		struct wl_resource *only)
{
	if (only) {
		send(only, item->resource);
		return;
	}
	struct wl_client *client = wl_resource_get_client(item->resource);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &zone->resources) {
		if (wl_resource_get_client(resource) == client) {
			send(resource, item->resource);
		}
	}
}

/* Items */

static bool
border_equal(const struct border *a, const struct border *b)
{
	return a->top == b->top && a->bottom == b->bottom
		&& a->left == b->left && a->right == b->right;
}

static void
item_send_position(struct zone_item *item, int x, int y)
{
	item->x = x;
	item->y = y;
	item->sent_counter++;
	xx_zone_item_v1_send_position(item->resource, x, y);
}

/*
 * Send frame_extents if they changed and position if it changed (or if
 * 'force' is set). Does nothing for items without a zone or with an
 * unmapped view, since neither decorations nor position are known then.
 */
static void
item_update(struct zone_item *item, bool force)
{
	if (!item->zone || !item->view || !item->view->mapped) {
		return;
	}

	struct border extents = ssd_thickness(item->view);
	struct wlr_box frame = ssd_max_extents(item->view);
	int x = frame.x - item->zone->area.x;
	int y = frame.y - item->zone->area.y;

	force |= item->force_next;
	item->force_next = false;
	bool extents_changed = !item->initial_sent
		|| !border_equal(&extents, &item->extents);
	if (extents_changed) {
		item->extents = extents;
		xx_zone_item_v1_send_frame_extents(item->resource,
			extents.top, extents.bottom, extents.left, extents.right);
	}
	if (extents_changed || force || x != item->x || y != item->y) {
		item_send_position(item, x, y);
	}
	item->initial_sent = true;
}

static void
item_leave_zone(struct zone_item *item, struct wl_resource *notify_only)
{
	struct zone *zone = item->zone;
	if (!zone) {
		return;
	}
	wl_list_remove(&item->link);
	item->zone = NULL;
	item->initial_sent = false;
	item->has_pending_position = false;
	zone_send_item_event(zone, item, xx_zone_v1_send_item_left, notify_only);
	zone_maybe_free(zone);
}

/* Returns true if compositor policy allows moving the item by request */
static bool
item_may_move(struct zone_item *item)
{
	struct view *view = item->view;
	if (!view->mapped || !view_is_floating(view)) {
		return false;
	}
	if (view == server.grabbed_view
			&& (server.input_mode == LAB_INPUT_STATE_MOVE
			|| server.input_mode == LAB_INPUT_STATE_RESIZE)) {
		return false;
	}
	if (window_rules_get_property(view, "fixedPosition") == LAB_PROP_TRUE) {
		return false;
	}
	if (view_has_strut_partial(view) || !output_is_usable(view->output)) {
		return false;
	}
	return true;
}

/*
 * Move the item so that the top-left corner of its frame is at layout
 * coordinates (fx, fy), constrained to the zone area: if the frame fits
 * into the area it is kept fully inside, otherwise it is aligned with
 * the top/left edge of the area. Always ends with a position event.
 */
static void
item_move_frame_to(struct zone_item *item, int fx, int fy)
{
	struct view *view = item->view;
	struct wlr_box *area = &item->zone->area;
	struct border border = ssd_thickness(view);
	int fw = view->current.width + border.left + border.right;
	int fh = view_effective_height(view, /* use_pending */ false)
		+ border.top + border.bottom;

	if (fw <= area->width) {
		fx = MAX(area->x, MIN(fx, area->x + area->width - fw));
	} else {
		fx = area->x;
	}
	if (fh <= area->height) {
		fy = MAX(area->y, MIN(fy, area->y + area->height - fh));
	} else {
		fy = area->y;
	}

	unsigned int counter = item->sent_counter;
	int x = fx + border.left;
	int y = fy + border.top;
	if (x != view->current.x || y != view->current.y) {
		/* Triggers view->events.moved and thus item_update() */
		view_move(view, x, y);
	}
	if (counter != item->sent_counter) {
		return;
	}
	if (view->pending_configure_serial) {
		/*
		 * The move is applied once the client acknowledges the
		 * pending configure (or it times out), which calls
		 * view_moved(). Send the position then instead of now.
		 */
		item->force_next = true;
	} else {
		item_update(item, /* force */ true);
	}
}

static void
item_detach_view(struct zone_item *item)
{
	if (!item->view) {
		return;
	}
	wl_list_remove(&item->destroy.link);
	wl_list_remove(&item->moved.link);
	wl_list_remove(&item->commit.link);
	item->view = NULL;
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct zone_item *item = wl_container_of(listener, item, destroy);
	item_leave_zone(item, NULL);
	item_detach_view(item);
	xx_zone_item_v1_send_closed(item->resource);
}

static void
handle_moved(struct wl_listener *listener, void *data)
{
	struct zone_item *item = wl_container_of(listener, item, moved);
	item_update(item, /* force */ false);
}

static void
handle_commit(struct wl_listener *listener, void *data)
{
	struct zone_item *item = wl_container_of(listener, item, commit);
	if (!item->has_pending_position || !item->zone || !item->view->mapped) {
		/* Keep the request pending until the view is mapped */
		return;
	}
	item->has_pending_position = false;

	if (!item_may_move(item)) {
		xx_zone_item_v1_send_position_failed(item->resource);
		return;
	}
	item_move_frame_to(item, item->zone->area.x + item->pending_x,
		item->zone->area.y + item->pending_y);
}

static void
item_handle_destroy_request(struct wl_client *client,
		struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
item_handle_set_position(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y)
{
	struct zone_item *item = wl_resource_get_user_data(resource);
	if (!item || !item->view) {
		return;
	}
	if (!item->zone) {
		xx_zone_item_v1_send_position_failed(resource);
		return;
	}
	item->pending_x = x;
	item->pending_y = y;
	item->has_pending_position = true;
}

static const struct xx_zone_item_v1_interface item_impl = {
	.destroy = item_handle_destroy_request,
	.set_position = item_handle_set_position,
};

static void
item_handle_resource_destroy(struct wl_resource *resource)
{
	struct zone_item *item = wl_resource_get_user_data(resource);
	item_leave_zone(item, NULL);
	item_detach_view(item);
	free(item);
}

/* Zone resources */

static void
zone_handle_destroy_request(struct wl_client *client,
		struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
zone_handle_add_item(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *item_resource)
{
	struct zone *zone = wl_resource_get_user_data(resource);
	struct zone_item *item = wl_resource_get_user_data(item_resource);
	if (!item->view) {
		return;
	}
	if (!zone) {
		wl_resource_post_error(resource, XX_ZONE_V1_ERROR_INVALID,
			"cannot add item to invalid zone");
		return;
	}

	if (item->zone != zone) {
		item_leave_zone(item, NULL);
		item->zone = zone;
		wl_list_insert(&zone->items, &item->link);
	}
	item->initial_sent = false;
	xx_zone_v1_send_item_entered(resource, item_resource);

	if (!item->view->mapped) {
		/* Initial extents/position are sent once the view is mapped */
		return;
	}
	if (item_may_move(item)) {
		/* Ensure the item is within the zone boundary */
		struct wlr_box frame = ssd_max_extents(item->view);
		item_move_frame_to(item, frame.x, frame.y);
	} else {
		item_update(item, /* force */ true);
	}
}

static void
zone_handle_remove_item(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *item_resource)
{
	struct zone *zone = wl_resource_get_user_data(resource);
	struct zone_item *item = wl_resource_get_user_data(item_resource);
	if (!item->view) {
		return;
	}
	if (zone && item->zone == zone) {
		item_leave_zone(item, resource);
	} else {
		/* Must be sent even if the item never was in this zone */
		xx_zone_v1_send_item_left(resource, item_resource);
	}
}

static const struct xx_zone_v1_interface zone_impl = {
	.destroy = zone_handle_destroy_request,
	.add_item = zone_handle_add_item,
	.remove_item = zone_handle_remove_item,
};

static void
zone_handle_resource_destroy(struct wl_resource *resource)
{
	struct zone *zone = wl_resource_get_user_data(resource);
	if (!zone) {
		return;
	}
	wl_list_remove(wl_resource_get_link(resource));
	zone_maybe_free(zone);
}

/*
 * Create a zone resource. If 'zone' is NULL, the resource represents an
 * invalid zone which cannot be used for item placement.
 */
static void
zone_resource_create(struct wl_resource *manager_resource, uint32_t id,
		struct zone *zone)
{
	struct wl_client *client = wl_resource_get_client(manager_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&xx_zone_v1_interface, wl_resource_get_version(manager_resource), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &zone_impl, zone,
		zone_handle_resource_destroy);

	if (zone) {
		wl_list_insert(&zone->resources, wl_resource_get_link(resource));
		xx_zone_v1_send_size(resource, zone->area.width, zone->area.height);
		xx_zone_v1_send_handle(resource, zone->handle);
	} else {
		wlr_log(WLR_INFO, "no usable output, creating invalid zone");
		xx_zone_v1_send_size(resource, -1, -1);
		xx_zone_v1_send_handle(resource, "");
	}
	xx_zone_v1_send_done(resource);
}

/* Manager */

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
manager_handle_get_zone_item(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *toplevel_resource)
{
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_from_resource(toplevel_resource);
	/* wlr_xdg_surface->data is the view, see handle_new_xdg_toplevel() */
	struct view *view = toplevel ? toplevel->base->data : NULL;

	struct wl_resource *resource = wl_resource_create(client,
		&xx_zone_item_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct zone_item *item = znew(*item);
	item->resource = resource;
	wl_resource_set_implementation(resource, &item_impl, item,
		item_handle_resource_destroy);

	if (!view) {
		/* The toplevel is already gone; the item is inert */
		xx_zone_item_v1_send_closed(resource);
		return;
	}
	item->view = view;
	CONNECT_SIGNAL(view, item, destroy);
	CONNECT_SIGNAL(view, item, moved);
	CONNECT_SIGNAL(view->surface, item, commit);
}

static void
manager_handle_get_zone(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *output_resource)
{
	struct output *output = NULL;
	if (output_resource) {
		struct wlr_output *wlr_output =
			wlr_output_from_resource(output_resource);
		if (wlr_output) {
			output = output_from_wlr_output(wlr_output);
		}
	}
	if (!output_is_usable(output)) {
		output = output_nearest_to_cursor();
	}
	zone_resource_create(manager_resource, id, zone_get_or_create(output));
}

static void
manager_handle_get_zone_from_handle(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		const char *handle)
{
	struct zone *zone = zone_from_handle(handle);
	if (!zone) {
		zone = zone_get_or_create(output_from_name(handle));
	}
	if (!zone) {
		/* Unknown handle: create a new zone without output preference */
		zone = zone_get_or_create(output_nearest_to_cursor());
	}
	zone_resource_create(manager_resource, id, zone);
}

static const struct xx_zone_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_zone_item = manager_handle_get_zone_item,
	.get_zone = manager_handle_get_zone,
	.get_zone_from_handle = manager_handle_get_zone_from_handle,
};

static void
handle_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&xx_zone_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

/* Public API */

void
zones_update(void)
{
	struct zone *zone;
	wl_list_for_each(zone, &zones, link) {
		struct wlr_box area = zone_resolve_area(zone);
		if (wlr_box_equal(&area, &zone->area)) {
			continue;
		}
		bool size_changed = area.width != zone->area.width
			|| area.height != zone->area.height;
		zone->area = area;

		if (size_changed) {
			struct wl_resource *resource;
			wl_resource_for_each(resource, &zone->resources) {
				xx_zone_v1_send_size(resource,
					area.width, area.height);
			}
		}
		/* Positions are relative to the (possibly moved) zone origin */
		struct zone_item *item;
		wl_list_for_each(item, &zone->items, link) {
			item_update(item, /* force */ false);
		}
	}
}

void
zones_init(void)
{
	zone_manager_global = wl_global_create(server.wl_display,
		&xx_zone_manager_v1_interface, ZONE_MANAGER_VERSION,
		NULL, handle_bind);
	if (!zone_manager_global) {
		wlr_log(WLR_ERROR, "unable to create xx_zone_manager_v1 global");
	}
}

void
zones_finish(void)
{
	if (zone_manager_global) {
		wl_global_destroy(zone_manager_global);
		zone_manager_global = NULL;
	}
	/* Clients are gone at this point, so all zones should be unused */
	struct zone *zone, *tmp;
	wl_list_for_each_safe(zone, tmp, &zones, link) {
		assert(wl_list_empty(&zone->resources));
		assert(wl_list_empty(&zone->items));
		wl_list_remove(&zone->link);
		zfree(zone->handle);
		free(zone);
	}
}
