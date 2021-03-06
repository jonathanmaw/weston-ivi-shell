/*
 * Copyright (C) 2013 DENSO CORPORATION
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * Implementation of ivi-layout library. The actual view on screen is
 * not updated till calling ivi_layout_commitChanges. A overview from
 * calling API for updating properties of surfaces/layer to asking compositor
 * to compose them by using weston_compositor_schedule_repaint,
 * 0/ initialize this library by ivi_layout_initWithCompositor
 *    with (struct weston_compositor *ec) from ivi-shell.
 * 1/ When a API for updating properties of surface/layer, it updates
 *    pending prop of ivi_layout_surface/layer/screen which are structure to
 *    store properties.
 * 2/ Before calling commitChanges, in case of calling a API to get a property,
 *    return current property, not pending property.
 * 3/ At the timing of calling ivi_layout_commitChanges, pending properties
 *    are applied
 *    to properties.
 * 4/ According properties, set transformation by using weston_matrix and
 *    weston_view per surfaces and layers in while loop.
 * 5/ Set damage and trigger transform by using weston_view_geometry_dirty and
 *    weston_view_geometry_dirty.
 * 6/ Notify update of properties.
 * 7/ Trigger composition by weston_compositor_schedule_repaint.
 *
 */

#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#include "compositor.h"
#include "ivi-layout.h"
#include "ivi-layout-export.h"
#include "ivi-layout-private.h"


enum ivi_layout_surface_orientation {
    IVI_LAYOUT_SURFACE_ORIENTATION_0_DEGREES   = 0,
    IVI_LAYOUT_SURFACE_ORIENTATION_90_DEGREES  = 1,
    IVI_LAYOUT_SURFACE_ORIENTATION_180_DEGREES = 2,
    IVI_LAYOUT_SURFACE_ORIENTATION_270_DEGREES = 3,
};

enum ivi_layout_surface_pixelformat {
    IVI_LAYOUT_SURFACE_PIXELFORMAT_R_8       = 0,
    IVI_LAYOUT_SURFACE_PIXELFORMAT_RGB_888   = 1,
    IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_8888 = 2,
    IVI_LAYOUT_SURFACE_PIXELFORMAT_RGB_565   = 3,
    IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_5551 = 4,
    IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_6661 = 5,
    IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_4444 = 6,
    IVI_LAYOUT_SURFACE_PIXELFORMAT_UNKNOWN   = 7,
};

struct link_layer {
    struct ivi_layout_layer *ivilayer;
    struct wl_list link;
    struct wl_list link_to_layer;
};

struct link_screen {
    struct ivi_layout_screen *iviscrn;
    struct wl_list link;
    struct wl_list link_to_screen;
};

struct listener_layoutNotification {
    void *userdata;
    struct wl_listener listener;
};

struct ivi_layout;

struct ivi_layout_screen {
    struct wl_list link;
    struct wl_list link_to_layer;
    uint32_t id_screen;

    struct ivi_layout *layout;
    struct weston_output *output;

    uint32_t event_mask;

    struct {
        struct wl_list list_layer;
        struct wl_list link;
    } pending;

    struct {
        struct wl_list list_layer;
        struct wl_list link;
    } order;
};

struct ivi_layout_notificationCallback {
    void *callback;
    void *data;
};

struct shellWarningArgs {
    uint32_t id_surface;
    enum ivi_layout_warning_flag flag;
};

static struct ivi_layout ivilayout = {0};

struct ivi_layout *
get_instance(void)
{
    return &ivilayout;
}

static void
ivi_layout_emitWarningSignal(uint32_t id_surface,
                             enum ivi_layout_warning_flag flag)
{
    struct ivi_layout *layout = get_instance();
    struct shellWarningArgs *args = malloc(sizeof *args);

    if (args == NULL) {
        weston_log("ivi_layout_emitWarningSignal: failed to allocate memory\n");
        return;
    }

    args->id_surface = id_surface;
    args->flag = flag;

    wl_signal_emit(&layout->warning_signal, args);
    free(args);
}

/**
 * Internal API to add/remove a link to surface from layer.
 */
static void
add_link_to_surface(struct ivi_layout_layer *ivilayer,
                    struct link_layer *link_layer)
{
    struct link_layer *link = NULL;
    int found = 0;

    wl_list_for_each(link, &ivilayer->link_to_surface, link_to_layer) {
        if (link == link_layer) {
            found = 1;
            break;
        }
    }

    if (found == 0) {
        wl_list_init(&link_layer->link_to_layer);
        wl_list_insert(&ivilayer->link_to_surface, &link_layer->link_to_layer);
    }
}

static void
remove_link_to_surface(struct ivi_layout_layer *ivilayer)
{
    struct link_layer *link = NULL;
    struct link_layer *next = NULL;

    wl_list_for_each_safe(link, next, &ivilayer->link_to_surface, link_to_layer) {
        if (!wl_list_empty(&link->link_to_layer)) {
            wl_list_remove(&link->link_to_layer);
        }
        if (!wl_list_empty(&link->link)) {
            wl_list_remove(&link->link);
        }
        free(link);
    }

    wl_list_init(&ivilayer->link_to_surface);
}

/**
 * Internal API to add a link to layer from screen.
 */
static void
add_link_to_layer(struct ivi_layout_screen *iviscrn,
                  struct link_screen *link_screen)
{
    wl_list_init(&link_screen->link_to_screen);
    wl_list_insert(&iviscrn->link_to_layer, &link_screen->link_to_screen);
}

/**
 * Internal API to add/remove a surface from layer.
 */
static void
add_ordersurface_to_layer(struct ivi_layout_surface *ivisurf,
                          struct ivi_layout_layer *ivilayer)
{
    struct link_layer *link_layer = NULL;

    link_layer = malloc(sizeof *link_layer);
    if (link_layer == NULL) {
        weston_log("fails to allocate memory\n");
        return;
    }

    link_layer->ivilayer = ivilayer;
    wl_list_init(&link_layer->link);
    wl_list_insert(&ivisurf->list_layer, &link_layer->link);
    add_link_to_surface(ivilayer, link_layer);
}

static void
remove_ordersurface_from_layer(struct ivi_layout_surface *ivisurf)
{
    struct link_layer *link_layer = NULL;
    struct link_layer *next = NULL;

    wl_list_for_each_safe(link_layer, next, &ivisurf->list_layer, link) {
        if (!wl_list_empty(&link_layer->link)) {
            wl_list_remove(&link_layer->link);
        }
        if (!wl_list_empty(&link_layer->link_to_layer)) {
            wl_list_remove(&link_layer->link_to_layer);
        }
        free(link_layer);
    }
    wl_list_init(&ivisurf->list_layer);
}

/**
 * Internal API to add/remove a layer from screen.
 */
static void
add_orderlayer_to_screen(struct ivi_layout_layer *ivilayer,
                         struct ivi_layout_screen *iviscrn)
{
    struct link_screen *link_scrn = NULL;

    link_scrn = malloc(sizeof *link_scrn);
    if (link_scrn == NULL) {
        weston_log("fails to allocate memory\n");
        return;
    }

    link_scrn->iviscrn = iviscrn;
    wl_list_init(&link_scrn->link);
    wl_list_insert(&ivilayer->list_screen, &link_scrn->link);
    add_link_to_layer(iviscrn, link_scrn);
}

static void
remove_orderlayer_from_screen(struct ivi_layout_layer *ivilayer)
{
    struct link_screen *link_scrn = NULL;
    struct link_screen *next = NULL;

    wl_list_for_each_safe(link_scrn, next, &ivilayer->list_screen, link) {
        if (!wl_list_empty(&link_scrn->link)) {
            wl_list_remove(&link_scrn->link);
        }
        if (!wl_list_empty(&link_scrn->link_to_screen)) {
            wl_list_remove(&link_scrn->link_to_screen);
        }
        free(link_scrn);
    }
    wl_list_init(&ivilayer->list_screen);
}

/**
 * Internal API to add/remove a layer from screen.
 */
static struct ivi_layout_surface *
get_surface(struct wl_list *list_surf, uint32_t id_surface)
{
    struct ivi_layout_surface *ivisurf;

    wl_list_for_each(ivisurf, list_surf, link) {
        if (ivisurf->id_surface == id_surface) {
            return ivisurf;
        }
    }

    return NULL;
}

static struct ivi_layout_layer *
get_layer(struct wl_list *list_layer, uint32_t id_layer)
{
    struct ivi_layout_layer *ivilayer;

    wl_list_for_each(ivilayer, list_layer, link) {
        if (ivilayer->id_layer == id_layer) {
            return ivilayer;
        }
    }

    return NULL;
}

/**
 * Called at destruction of ivi_surface
 */
static void
westonsurface_destroy_from_ivisurface(struct wl_listener *listener, void *data)
{
    struct ivi_layout_surface *ivisurf = NULL;

    ivisurf = container_of(listener, struct ivi_layout_surface,
                           surface_destroy_listener);

    wl_list_init(&ivisurf->surface_rotation.link);
    wl_list_init(&ivisurf->layer_rotation.link);
    wl_list_init(&ivisurf->surface_pos.link);
    wl_list_init(&ivisurf->layer_pos.link);
    wl_list_init(&ivisurf->scaling.link);

    ivisurf->surface = NULL;
    ivi_layout_surfaceRemove(ivisurf);
}

/**
 * Internal API to check layer/surface already added in layer/screen.
 * Called by ivi_layout_layerAddSurface/ivi_layout_screenAddLayer
 */
static int
is_surface_in_layer(struct ivi_layout_surface *ivisurf,
                    struct ivi_layout_layer *ivilayer)
{
    struct ivi_layout_surface *surf = NULL;

    wl_list_for_each(surf, &ivilayer->pending.list_surface, pending.link) {
        if (surf->id_surface == ivisurf->id_surface) {
            return 1;
        }
    }

    return 0;
}

static int
is_layer_in_screen(struct ivi_layout_layer *ivilayer,
                    struct ivi_layout_screen *iviscrn)
{
    struct ivi_layout_layer *layer = NULL;

    wl_list_for_each(layer, &iviscrn->pending.list_layer, pending.link) {
        if (layer->id_layer == ivilayer->id_layer) {
            return 1;
        }
    }

    return 0;
}

/**
 * Internal API to initialize screens found from output_list of weston_compositor.
 * Called by ivi_layout_initWithCompositor.
 */
static void
create_screen(struct weston_compositor *ec)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_screen *iviscrn = NULL;
    struct weston_output *output = NULL;
    int32_t count = 0;

    wl_list_for_each(output, &ec->output_list, link) {
        iviscrn = calloc(1, sizeof *iviscrn);
        if (iviscrn == NULL) {
            weston_log("fails to allocate memory\n");
            continue;
        }

        wl_list_init(&iviscrn->link);
        iviscrn->layout = layout;

        iviscrn->id_screen = count;
        count++;

        iviscrn->output = output;
        iviscrn->event_mask = 0;

        wl_list_init(&iviscrn->pending.list_layer);
        wl_list_init(&iviscrn->pending.link);

        wl_list_init(&iviscrn->order.list_layer);
        wl_list_init(&iviscrn->order.link);

        wl_list_init(&iviscrn->link_to_layer);

        wl_list_insert(&layout->list_screen, &iviscrn->link);
    }
}

/**
 * Internal APIs to initialize properties of surface/layer when they are created.
 */
static void
init_layerProperties(struct ivi_layout_LayerProperties *prop,
                     int32_t width, int32_t height)
{
    memset(prop, 0, sizeof *prop);
    prop->opacity = wl_fixed_from_double(1.0);
    prop->sourceWidth = width;
    prop->sourceHeight = height;
    prop->destWidth = width;
    prop->destHeight = height;
}

static void
init_surfaceProperties(struct ivi_layout_SurfaceProperties *prop)
{
    memset(prop, 0, sizeof *prop);
    prop->opacity = wl_fixed_from_double(1.0);
}

/**
 * Internal APIs to be called from ivi_layout_commitChanges.
 */
static void
update_opacity(struct ivi_layout_layer *ivilayer,
               struct ivi_layout_surface *ivisurf)
{
    double layer_alpha = wl_fixed_to_double(ivilayer->prop.opacity);
    double surf_alpha  = wl_fixed_to_double(ivisurf->prop.opacity);

    if ((ivilayer->event_mask & IVI_NOTIFICATION_OPACITY) ||
        (ivisurf->event_mask  & IVI_NOTIFICATION_OPACITY)) {
        struct weston_view *tmpview = NULL;
        wl_list_for_each(tmpview, &ivisurf->surface->views, surface_link)
        {
            if (tmpview == NULL) {
                continue;
            }
            tmpview->alpha = layer_alpha * surf_alpha;
        }
    }
}

static void
update_surface_orientation(struct ivi_layout_layer *ivilayer,
                           struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_matrix  *matrix = &ivisurf->surface_rotation.matrix;
    float width  = 0.0f;
    float height = 0.0f;
    float v_sin  = 0.0f;
    float v_cos  = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float sx = 1.0f;
    float sy = 1.0f;

    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL) {
            break;
        }
    }

    if (view == NULL) {
        return;
    }

    if ((ivilayer->prop.destWidth == 0) ||
        (ivilayer->prop.destHeight == 0)) {
        return;
    }
    width  = (float)ivilayer->prop.destWidth;
    height = (float)ivilayer->prop.destHeight;

    switch (ivisurf->prop.orientation) {
    case IVI_LAYOUT_SURFACE_ORIENTATION_0_DEGREES:
        v_sin = 0.0f;
        v_cos = 1.0f;
        break;
    case IVI_LAYOUT_SURFACE_ORIENTATION_90_DEGREES:
        v_sin = 1.0f;
        v_cos = 0.0f;
        sx = width / height;
        sy = height / width;
        break;
    case IVI_LAYOUT_SURFACE_ORIENTATION_180_DEGREES:
        v_sin = 0.0f;
        v_cos = -1.0f;
        break;
    case IVI_LAYOUT_SURFACE_ORIENTATION_270_DEGREES:
    default:
        v_sin = -1.0f;
        v_cos = 0.0f;
        sx = width / height;
        sy = height / width;
        break;
    }
    wl_list_remove(&ivisurf->surface_rotation.link);
    weston_view_geometry_dirty(view);

    weston_matrix_init(matrix);
    cx = 0.5f * width;
    cy = 0.5f * height;
    weston_matrix_translate(matrix, -cx, -cy, 0.0f);
    weston_matrix_rotate_xy(matrix, v_cos, v_sin);
    weston_matrix_scale(matrix, sx, sy, 1.0);
    weston_matrix_translate(matrix, cx, cy, 0.0f);
    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->surface_rotation.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);
}

static void
update_layer_orientation(struct ivi_layout_layer *ivilayer,
                         struct ivi_layout_surface *ivisurf)
{
    struct weston_surface *es = ivisurf->surface;
    struct weston_view    *view;
    struct weston_matrix  *matrix = &ivisurf->layer_rotation.matrix;
    struct weston_output  *output = NULL;
    float width  = 0.0f;
    float height = 0.0f;
    float v_sin  = 0.0f;
    float v_cos  = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float sx = 1.0f;
    float sy = 1.0f;

    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL) {
            break;
        }
    }

    if (es == NULL || view == NULL) {
        return;
    }

    output = es->output;
    if (output == NULL) {
        return;
    }
    if ((output->width == 0) || (output->height == 0)) {
        return;
    }
    width = (float)output->width;
    height = (float)output->height;

    switch (ivilayer->prop.orientation) {
    case IVI_LAYOUT_SURFACE_ORIENTATION_0_DEGREES:
        v_sin = 0.0f;
        v_cos = 1.0f;
        break;
    case IVI_LAYOUT_SURFACE_ORIENTATION_90_DEGREES:
        v_sin = 1.0f;
        v_cos = 0.0f;
        sx = width / height;
        sy = height / width;
        break;
    case IVI_LAYOUT_SURFACE_ORIENTATION_180_DEGREES:
        v_sin = 0.0f;
        v_cos = -1.0f;
        break;
    case IVI_LAYOUT_SURFACE_ORIENTATION_270_DEGREES:
    default:
        v_sin = -1.0f;
        v_cos = 0.0f;
        sx = width / height;
        sy = height / width;
        break;
    }
    wl_list_remove(&ivisurf->layer_rotation.link);
    weston_view_geometry_dirty(view);

    weston_matrix_init(matrix);
    cx = 0.5f * width;
    cy = 0.5f * height;
    weston_matrix_translate(matrix, -cx, -cy, 0.0f);
    weston_matrix_rotate_xy(matrix, v_cos, v_sin);
    weston_matrix_scale(matrix, sx, sy, 1.0);
    weston_matrix_translate(matrix, cx, cy, 0.0f);
    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->layer_rotation.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);
}

static void
update_surface_position(struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    float tx  = (float)ivisurf->prop.destX;
    float ty  = (float)ivisurf->prop.destY;
    struct weston_matrix *matrix = &ivisurf->surface_pos.matrix;

    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL) {
            break;
        }
    }

    if (view == NULL) {
        return;
    }

    wl_list_remove(&ivisurf->surface_pos.link);

    weston_matrix_init(matrix);
    weston_matrix_translate(matrix, tx, ty, 0.0f);
    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->surface_pos.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);

#if 0
    /* disable zoom transition */
    weston_zoom_run(es, 0.0, 1.0, NULL, NULL);
#endif

}

static void
update_layer_position(struct ivi_layout_layer *ivilayer,
               struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_matrix *matrix = &ivisurf->layer_pos.matrix;
    float tx  = (float)ivilayer->prop.destX;
    float ty  = (float)ivilayer->prop.destY;

    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL) {
            break;
        }
    }

    if (view == NULL) {
        return;
    }

    wl_list_remove(&ivisurf->layer_pos.link);

    weston_matrix_init(matrix);
    weston_matrix_translate(matrix, tx, ty, 0.0f);
    wl_list_insert(
        &view->geometry.transformation_list,
        &ivisurf->layer_pos.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);
}

static void
update_scale(struct ivi_layout_layer *ivilayer,
               struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_matrix *matrix = &ivisurf->scaling.matrix;
    float sx = 0.0f;
    float sy = 0.0f;
    float lw = 0.0f;
    float sw = 0.0f;
    float lh = 0.0f;
    float sh = 0.0f;

    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL) {
            break;
        }
    }

    if (view == NULL) {
        return;
    }

    if (ivisurf->prop.sourceWidth == 0 && ivisurf->prop.sourceHeight == 0) {
        ivisurf->prop.sourceWidth  = ivisurf->surface->width_from_buffer;
        ivisurf->prop.sourceHeight = ivisurf->surface->height_from_buffer;

        if (ivisurf->prop.destWidth == 0 && ivisurf->prop.destHeight == 0) {
            ivisurf->prop.destWidth  = ivisurf->surface->width_from_buffer;
            ivisurf->prop.destHeight = ivisurf->surface->height_from_buffer;
        }
    }

    lw = ((float)ivilayer->prop.destWidth  / ivilayer->prop.sourceWidth );
    sw = ((float)ivisurf->prop.destWidth   / ivisurf->prop.sourceWidth  );
    lh = ((float)ivilayer->prop.destHeight / ivilayer->prop.sourceHeight);
    sh = ((float)ivisurf->prop.destHeight  / ivisurf->prop.sourceHeight );
    sx = sw * lw;
    sy = sh * lh;

    wl_list_remove(&ivisurf->scaling.link);
    weston_matrix_init(matrix);
    weston_matrix_scale(matrix, sx, sy, 1.0f);

    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->scaling.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);
}

static void
update_prop(struct ivi_layout_layer *ivilayer,
            struct ivi_layout_surface *ivisurf)
{
    if (ivilayer->event_mask | ivisurf->event_mask) {
        update_opacity(ivilayer, ivisurf);
        update_layer_orientation(ivilayer, ivisurf);
        update_layer_position(ivilayer, ivisurf);
        update_surface_position(ivisurf);
        update_surface_orientation(ivilayer, ivisurf);
        update_scale(ivilayer, ivisurf);

        ivisurf->update_count++;

        struct weston_view *tmpview;
        wl_list_for_each(tmpview, &ivisurf->surface->views, surface_link)
        {
            if (tmpview != NULL) {
                break;
            }
        }

        if (tmpview != NULL) {
            weston_view_geometry_dirty(tmpview);
        }

        if (ivisurf->surface != NULL) {
            weston_surface_damage(ivisurf->surface);
        }
    }
}

static void
commit_changes(struct ivi_layout *layout)
{
    struct ivi_layout_screen  *iviscrn  = NULL;
    struct ivi_layout_layer   *ivilayer = NULL;
    struct ivi_layout_surface *ivisurf  = NULL;

    wl_list_for_each(iviscrn, &layout->list_screen, link) {
        wl_list_for_each(ivilayer, &iviscrn->order.list_layer, order.link) {
            wl_list_for_each(ivisurf, &ivilayer->order.list_surface, order.link) {
                update_prop(ivilayer, ivisurf);
            }
        }
    }
}

static void
commit_list_surface(struct ivi_layout *layout)
{
    struct ivi_layout_surface *ivisurf = NULL;

    wl_list_for_each(ivisurf, &layout->list_surface, link) {
        if(ivisurf->pending.prop.transitionType == IVI_LAYOUT_TRANSITION_VIEW_DEFAULT){
            ivi_layout_transition_move_resize_view(ivisurf,
                                                   ivisurf->pending.prop.destX,
                                                   ivisurf->pending.prop.destY,
                                                   ivisurf->pending.prop.destWidth,
                                                   ivisurf->pending.prop.destHeight,
                                                   ivisurf->pending.prop.transitionDuration);

            if(ivisurf->pending.prop.visibility)
            {
                ivi_layout_transition_visibility_on(ivisurf, ivisurf->pending.prop.transitionDuration);
            }
            else
            {
                ivi_layout_transition_visibility_off(ivisurf, ivisurf->pending.prop.transitionDuration);
            }

            int32_t destX = ivisurf->prop.destX;
            int32_t destY = ivisurf->prop.destY;
            int32_t destWidth = ivisurf->prop.destWidth;
            int32_t destHeight = ivisurf->prop.destHeight;

            ivisurf->prop = ivisurf->pending.prop;
            ivisurf->prop.destX = destX;
            ivisurf->prop.destY = destY;
            ivisurf->prop.destWidth = destWidth;
            ivisurf->prop.destHeight = destHeight;
            ivisurf->prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
            ivisurf->pending.prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;

        }
        else if(ivisurf->pending.prop.transitionType == IVI_LAYOUT_TRANSITION_VIEW_DEST_RECT_ONLY){
            ivi_layout_transition_move_resize_view(ivisurf,
                                                   ivisurf->pending.prop.destX,
                                                   ivisurf->pending.prop.destY,
                                                   ivisurf->pending.prop.destWidth,
                                                   ivisurf->pending.prop.destHeight,
                                                   ivisurf->pending.prop.transitionDuration);

            int32_t destX = ivisurf->prop.destX;
            int32_t destY = ivisurf->prop.destY;
            int32_t destWidth = ivisurf->prop.destWidth;
            int32_t destHeight = ivisurf->prop.destHeight;

            ivisurf->prop = ivisurf->pending.prop;
            ivisurf->prop.destX = destX;
            ivisurf->prop.destY = destY;
            ivisurf->prop.destWidth = destWidth;
            ivisurf->prop.destHeight = destHeight;

            ivisurf->prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
            ivisurf->pending.prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;

        }
        else if(ivisurf->pending.prop.transitionType == IVI_LAYOUT_TRANSITION_VIEW_FADE_ONLY){
            if(ivisurf->pending.prop.visibility)
            {
                ivi_layout_transition_visibility_on(ivisurf, ivisurf->pending.prop.transitionDuration);
            }
            else
            {
                ivi_layout_transition_visibility_off(ivisurf, ivisurf->pending.prop.transitionDuration);
            }

            ivisurf->prop = ivisurf->pending.prop;
            ivisurf->prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
            ivisurf->pending.prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
        }
        else{
            ivisurf->prop = ivisurf->pending.prop;
            ivisurf->prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
            ivisurf->pending.prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
        }
    }
}

static void
commit_list_layer(struct ivi_layout *layout)
{
    struct ivi_layout_layer   *ivilayer = NULL;
    struct ivi_layout_surface *ivisurf  = NULL;
    struct ivi_layout_surface *next     = NULL;

    wl_list_for_each(ivilayer, &layout->list_layer, link) {
        if(ivilayer->pending.prop.transitionType == IVI_LAYOUT_TRANSITION_LAYER_MOVE)
        {
            ivi_layout_transition_move_layer(ivilayer, ivilayer->pending.prop.destX, ivilayer->pending.prop.destY, ivilayer->pending.prop.transitionDuration);
        }
        else if(ivilayer->pending.prop.transitionType == IVI_LAYOUT_TRANSITION_LAYER_FADE)
        {
            ivi_layout_transition_fade_layer(ivilayer,ivilayer->pending.prop.isFadeIn,
                                             ivilayer->pending.prop.startAlpha,ivilayer->pending.prop.endAlpha,
                                             NULL, NULL,
                                             ivilayer->pending.prop.transitionDuration);
        }
        ivilayer->pending.prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;

        ivilayer->prop = ivilayer->pending.prop;

        if (!(ivilayer->event_mask &
              (IVI_NOTIFICATION_ADD | IVI_NOTIFICATION_REMOVE)) ) {
            continue;
        }

        if (ivilayer->event_mask & IVI_NOTIFICATION_REMOVE) {
            wl_list_for_each_safe(ivisurf, next,
                &ivilayer->order.list_surface, order.link) {
                remove_ordersurface_from_layer(ivisurf);

                if (!wl_list_empty(&ivisurf->order.link)) {
                    wl_list_remove(&ivisurf->order.link);
                }

                wl_list_init(&ivisurf->order.link);
                ivisurf->event_mask |= IVI_NOTIFICATION_REMOVE;
            }

            wl_list_init(&ivilayer->order.list_surface);
        }

        if (ivilayer->event_mask & IVI_NOTIFICATION_ADD) {
            wl_list_for_each_safe(ivisurf, next,
                &ivilayer->order.list_surface, order.link) {
                remove_ordersurface_from_layer(ivisurf);

                if (!wl_list_empty(&ivisurf->order.link)) {
                    wl_list_remove(&ivisurf->order.link);
                }

                wl_list_init(&ivisurf->order.link);
            }

            wl_list_init(&ivilayer->order.list_surface);
            wl_list_for_each(ivisurf, &ivilayer->pending.list_surface,
                                  pending.link) {
                if(!wl_list_empty(&ivisurf->order.link)){
                    wl_list_remove(&ivisurf->order.link);
                    wl_list_init(&ivisurf->order.link);
                }

                wl_list_insert(&ivilayer->order.list_surface,
                               &ivisurf->order.link);
                add_ordersurface_to_layer(ivisurf, ivilayer);
                ivisurf->event_mask |= IVI_NOTIFICATION_ADD;
            }
        }
    }
}

static void
commit_list_screen(struct ivi_layout *layout)
{
    struct ivi_layout_screen  *iviscrn  = NULL;
    struct ivi_layout_layer   *ivilayer = NULL;
    struct ivi_layout_layer   *next     = NULL;
    struct ivi_layout_surface *ivisurf  = NULL;

    wl_list_for_each(iviscrn, &layout->list_screen, link) {
        if (iviscrn->event_mask & IVI_NOTIFICATION_REMOVE) {
            wl_list_for_each_safe(ivilayer, next,
                     &iviscrn->order.list_layer, order.link) {
                remove_orderlayer_from_screen(ivilayer);

                if (!wl_list_empty(&ivilayer->order.link)) {
                    wl_list_remove(&ivilayer->order.link);
                }

                wl_list_init(&ivilayer->order.link);
                ivilayer->event_mask |= IVI_NOTIFICATION_REMOVE;
            }
        }

        if (iviscrn->event_mask & IVI_NOTIFICATION_ADD) {
            wl_list_for_each_safe(ivilayer, next,
                     &iviscrn->order.list_layer, order.link) {
                remove_orderlayer_from_screen(ivilayer);

                if (!wl_list_empty(&ivilayer->order.link)) {
                    wl_list_remove(&ivilayer->order.link);
                }

                wl_list_init(&ivilayer->order.link);
            }

            wl_list_init(&iviscrn->order.list_layer);
            wl_list_for_each(ivilayer, &iviscrn->pending.list_layer,
                                  pending.link) {
                wl_list_insert(&iviscrn->order.list_layer,
                               &ivilayer->order.link);
                add_orderlayer_to_screen(ivilayer, iviscrn);
                ivilayer->event_mask |= IVI_NOTIFICATION_ADD;
            }
        }

        iviscrn->event_mask = 0;

        /* Clear view list of layout layer */
        wl_list_init(&layout->layout_layer.view_list);

        wl_list_for_each(ivilayer, &iviscrn->order.list_layer, order.link) {

            if (ivilayer->prop.visibility == 0)
                continue;

            wl_list_for_each(ivisurf, &ivilayer->order.list_surface, order.link) {
                struct weston_view *tmpview = NULL;
                wl_list_for_each(tmpview, &ivisurf->surface->views, surface_link)
                {
                    if (tmpview != NULL) {
                        break;
                    }
                }

                if (ivisurf->prop.visibility == 0)
                    continue;
                if (ivisurf->surface == NULL || tmpview == NULL)
                    continue;

                wl_list_insert(&layout->layout_layer.view_list,
                               &tmpview->layer_link);

                ivisurf->surface->output = iviscrn->output;
            }
        }

        break;
    }
}

static void
commit_transition(struct ivi_layout* layout)
{
    if(wl_list_empty(&layout->pending_transition_list)){
        return;
    }

    wl_list_insert_list(&layout->transitions->transition_list,
                        &layout->pending_transition_list);

    wl_list_init(&layout->pending_transition_list);

    wl_event_source_timer_update(layout->transitions->event_source, 1);
}

static void
send_surface_prop(struct ivi_layout_surface *ivisurf)
{
    wl_signal_emit(&ivisurf->property_changed, ivisurf);
    ivisurf->event_mask = 0;
}

static void
send_layer_prop(struct ivi_layout_layer *ivilayer)
{
    wl_signal_emit(&ivilayer->property_changed, ivilayer);
    ivilayer->event_mask = 0;
}

static void
send_prop(struct ivi_layout *layout)
{
    struct ivi_layout_layer   *ivilayer = NULL;
    struct ivi_layout_surface *ivisurf  = NULL;

    wl_list_for_each_reverse(ivilayer, &layout->list_layer, link) {
        send_layer_prop(ivilayer);
    }

    wl_list_for_each_reverse(ivisurf, &layout->list_surface, link) {
        send_surface_prop(ivisurf);
    }
}

static void
clear_surface_pending_list(struct ivi_layout_layer *ivilayer)
{
    struct ivi_layout_surface *surface_link = NULL;
    struct ivi_layout_surface *surface_next = NULL;

    wl_list_for_each_safe(surface_link, surface_next,
                          &ivilayer->pending.list_surface, pending.link) {
        if (!wl_list_empty(&surface_link->pending.link)) {
            wl_list_remove(&surface_link->pending.link);
        }

        wl_list_init(&surface_link->pending.link);
    }

    ivilayer->event_mask |= IVI_NOTIFICATION_REMOVE;
}

static void
clear_surface_order_list(struct ivi_layout_layer *ivilayer)
{
    struct ivi_layout_surface *surface_link = NULL;
    struct ivi_layout_surface *surface_next = NULL;

    wl_list_for_each_safe(surface_link, surface_next,
                          &ivilayer->order.list_surface, order.link) {
        if (!wl_list_empty(&surface_link->order.link)) {
            wl_list_remove(&surface_link->order.link);
        }

        wl_list_init(&surface_link->order.link);
    }

    ivilayer->event_mask |= IVI_NOTIFICATION_REMOVE;
}

static void
layer_created(struct wl_listener *listener, void *data)
{
    struct ivi_layout_layer *ivilayer = (struct ivi_layout_layer *)data;

    struct listener_layoutNotification *notification =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *created_callback =
        (struct ivi_layout_notificationCallback *)notification->userdata;

    ((layerCreateNotificationFunc)created_callback->callback)
        (ivilayer, created_callback->data);
}

static void
layer_removed(struct wl_listener *listener, void *data)
{
    struct ivi_layout_layer *ivilayer = (struct ivi_layout_layer *)data;

    struct listener_layoutNotification *notification =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *removed_callback =
        (struct ivi_layout_notificationCallback *)notification->userdata;

    ((layerRemoveNotificationFunc)removed_callback->callback)
        (ivilayer, removed_callback->data);
}

static void
layer_prop_changed(struct wl_listener *listener, void *data)
{
    struct ivi_layout_layer *ivilayer = (struct ivi_layout_layer *)data;

    struct listener_layoutNotification *layout_listener =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *prop_callback =
        (struct ivi_layout_notificationCallback *)layout_listener->userdata;

    ((layerPropertyNotificationFunc)prop_callback->callback)
        (ivilayer, &ivilayer->prop, ivilayer->event_mask, prop_callback->data);
}

static void
surface_created(struct wl_listener *listener, void *data)
{
    struct ivi_layout_surface *ivisurface = (struct ivi_layout_surface *)data;

    struct listener_layoutNotification *notification =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *created_callback =
        (struct ivi_layout_notificationCallback *)notification->userdata;

    ((surfaceCreateNotificationFunc)created_callback->callback)
        (ivisurface, created_callback->data);
}

static void
surface_removed(struct wl_listener *listener, void *data)
{
    struct ivi_layout_surface *ivisurface = (struct ivi_layout_surface *)data;

    struct listener_layoutNotification *notification =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *removed_callback =
        (struct ivi_layout_notificationCallback *)notification->userdata;

    ((surfaceRemoveNotificationFunc)removed_callback->callback)
        (ivisurface, removed_callback->data);
}

static void
surface_prop_changed(struct wl_listener *listener, void *data)
{
    struct ivi_layout_surface *ivisurf = (struct ivi_layout_surface *)data;

    struct listener_layoutNotification *layout_listener =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *prop_callback =
        (struct ivi_layout_notificationCallback *)layout_listener->userdata;

    ((surfacePropertyNotificationFunc)prop_callback->callback)
        (ivisurf, &ivisurf->prop, ivisurf->event_mask, prop_callback->data);

    ivisurf->event_mask = 0;
}

static void
surface_configure_changed(struct wl_listener *listener,
                          void *data)
{
    struct ivi_layout_surface *ivisurface = (struct ivi_layout_surface *)data;

    struct listener_layoutNotification *notification =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *configure_changed_callback =
        (struct ivi_layout_notificationCallback *)notification->userdata;

    ((surfaceConfigureNotificationFunc)configure_changed_callback->callback)
        (ivisurface, configure_changed_callback->data);
}

static void
warning_occurred(struct wl_listener *listener, void *data)
{
    struct shellWarningArgs *args = (struct shellWarningArgs *)data;

    struct listener_layoutNotification *layout_listener =
        container_of(listener,
                     struct listener_layoutNotification,
                     listener);

    struct ivi_layout_notificationCallback *callback =
        (struct ivi_layout_notificationCallback *)layout_listener->userdata;

    ((shellWarningNotificationFunc)callback->callback)
        (args->id_surface, args->flag, callback->data);
}

static int32_t
add_notification(struct wl_signal *signal,
                 wl_notify_func_t callback,
                 void *userdata)
{
    struct listener_layoutNotification *notification = NULL;

    notification = malloc(sizeof *notification);
    if (notification == NULL) {
        weston_log("fails to allocate memory\n");
        free(userdata);
        return -1;
    }

    notification->listener.notify = callback;
    notification->userdata = userdata;

    wl_signal_add(signal, &notification->listener);

    return 0;
}

static void
remove_notification(struct wl_list *listener_list, void *callback, void *userdata)
{
    struct wl_listener *listener = NULL;
    struct wl_listener *next = NULL;

    wl_list_for_each_safe(listener, next, listener_list, link) {
        struct listener_layoutNotification *notification =
            container_of(listener,
                         struct listener_layoutNotification,
                         listener);

        struct ivi_layout_notificationCallback *notification_callback =
            (struct ivi_layout_notificationCallback *)notification->userdata;

        if ((notification_callback->callback != callback) ||
            (notification_callback->data != userdata)) {
            continue;
        }

        if (!wl_list_empty(&listener->link)) {
            wl_list_remove(&listener->link);
        }

        free(notification->userdata);
        free(notification);
    }
}

static void
remove_all_notification(struct wl_list *listener_list)
{
    struct wl_listener *listener = NULL;
    struct wl_listener *next = NULL;

    wl_list_for_each_safe(listener, next, listener_list, link) {
        if (!wl_list_empty(&listener->link)) {
            wl_list_remove(&listener->link);
        }

        struct listener_layoutNotification *notification =
            container_of(listener,
                         struct listener_layoutNotification,
                         listener);

        free(notification->userdata);
        free(notification);
    }
}

/**
 * Exported APIs of ivi-layout library are implemented from here.
 * Brief of APIs is described in ivi-layout-export.h.
 */
WL_EXPORT int32_t
ivi_layout_addNotificationCreateLayer(layerCreateNotificationFunc callback,
                                      void *userdata)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_notificationCallback *created_callback = NULL;

    if (callback == NULL) {
        weston_log("ivi_layout_addNotificationCreateLayer: invalid argument\n");
        return -1;
    }

    created_callback = malloc(sizeof *created_callback);
    if (created_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    created_callback->callback = callback;
    created_callback->data = userdata;

    return add_notification(&layout->layer_notification.created,
                            layer_created,
                            created_callback);
}

WL_EXPORT void
ivi_layout_removeNotificationCreateLayer(layerCreateNotificationFunc callback,
                                         void *userdata)
{
    struct ivi_layout *layout = get_instance();
    remove_notification(&layout->layer_notification.created.listener_list, callback, userdata);
}

WL_EXPORT int32_t
ivi_layout_addNotificationRemoveLayer(layerRemoveNotificationFunc callback,
                                      void *userdata)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_notificationCallback *removed_callback = NULL;

    if (callback == NULL) {
        weston_log("ivi_layout_addNotificationRemoveLayer: invalid argument\n");
        return -1;
    }

    removed_callback = malloc(sizeof *removed_callback);
    if (removed_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    removed_callback->callback = callback;
    removed_callback->data = userdata;
    add_notification(&layout->layer_notification.removed,
                     layer_removed,
                     removed_callback);

    return 0;
}

WL_EXPORT void
ivi_layout_removeNotificationRemoveLayer(layerRemoveNotificationFunc callback,
                                         void *userdata)
{
    struct ivi_layout *layout = get_instance();
    remove_notification(&layout->layer_notification.removed.listener_list, callback, userdata);
}

WL_EXPORT int32_t
ivi_layout_addNotificationCreateSurface(surfaceCreateNotificationFunc callback,
                                        void *userdata)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_notificationCallback *created_callback = NULL;

    if (callback == NULL) {
        weston_log("ivi_layout_addNotificationCreateSurface: invalid argument\n");
        return -1;
    }

    created_callback = malloc(sizeof *created_callback);
    if (created_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    created_callback->callback = callback;
    created_callback->data = userdata;

    add_notification(&layout->surface_notification.created,
                     surface_created,
                     created_callback);
    return 0;
}

WL_EXPORT void
ivi_layout_removeNotificationCreateSurface(surfaceCreateNotificationFunc callback,
                                           void *userdata)
{
    struct ivi_layout *layout = get_instance();
    remove_notification(&layout->surface_notification.created.listener_list, callback, userdata);
}

WL_EXPORT int32_t
ivi_layout_addNotificationRemoveSurface(surfaceRemoveNotificationFunc callback,
                                        void *userdata)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_notificationCallback *removed_callback = NULL;

    if (callback == NULL) {
        weston_log("ivi_layout_addNotificationRemoveSurface: invalid argument\n");
        return -1;
    }

    removed_callback = malloc(sizeof *removed_callback);
    if (removed_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    removed_callback->callback = callback;
    removed_callback->data = userdata;

    add_notification(&layout->surface_notification.removed,
                     surface_removed,
                     removed_callback);

    return 0;
}

WL_EXPORT void
ivi_layout_removeNotificationRemoveSurface(surfaceRemoveNotificationFunc callback,
                                           void *userdata)
{
    struct ivi_layout *layout = get_instance();
    remove_notification(&layout->surface_notification.removed.listener_list, callback, userdata);
}

WL_EXPORT int32_t
ivi_layout_addNotificationConfigureSurface(surfaceConfigureNotificationFunc callback,
                                           void *userdata)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_notificationCallback *configure_changed_callback = NULL;
    if (callback == NULL) {
        weston_log("ivi_layout_addNotificationConfigureSurface: invalid argument\n");
        return -1;
    }

    configure_changed_callback = malloc(sizeof *configure_changed_callback);
    if (configure_changed_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    configure_changed_callback->callback = callback;
    configure_changed_callback->data = userdata;

    add_notification(&layout->surface_notification.configure_changed,
                     surface_configure_changed,
                     configure_changed_callback);
    return 0;
}

WL_EXPORT void
ivi_layout_removeNotificationConfigureSurface(surfaceConfigureNotificationFunc callback,
                                              void *userdata)
{
    struct ivi_layout *layout = get_instance();
    remove_notification(&layout->surface_notification.configure_changed.listener_list, callback, userdata);
}

WL_EXPORT int32_t
ivi_layout_addNotificationShellWarning(shellWarningNotificationFunc callback,
                                       void *userdata)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_notificationCallback *warn_callback = NULL;

    if (callback == NULL) {
        weston_log("ivi_layout_addNotificationShellWarning: invalid argument\n");
        return -1;
    }

    warn_callback = malloc(sizeof *warn_callback);
    if (warn_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    warn_callback->callback = callback;
    warn_callback->data = userdata;

    return add_notification(&layout->warning_signal,
                            warning_occurred,
                            warn_callback);
}

WL_EXPORT void
ivi_layout_removeNotificationShellWarning(shellWarningNotificationFunc callback,
                                          void *userdata)
{
    struct ivi_layout *layout = get_instance();
    remove_notification(&layout->warning_signal.listener_list, callback, userdata);
}

WL_EXPORT uint32_t
ivi_layout_getIdOfSurface(struct ivi_layout_surface *ivisurf)
{
    return ivisurf->id_surface;
}

WL_EXPORT uint32_t
ivi_layout_getIdOfLayer(struct ivi_layout_layer *ivilayer)
{
    return ivilayer->id_layer;
}

WL_EXPORT struct ivi_layout_layer *
ivi_layout_getLayerFromId(uint32_t id_layer)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_layer *ivilayer = NULL;

    wl_list_for_each(ivilayer, &layout->list_layer, link) {
        if (ivilayer->id_layer == id_layer) {
            return ivilayer;
        }
    }

    return NULL;
}

WL_EXPORT struct ivi_layout_surface *
ivi_layout_getSurfaceFromId(uint32_t id_surface)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_surface *ivisurf = NULL;

    wl_list_for_each(ivisurf, &layout->list_surface, link) {
        if (ivisurf->id_surface == id_surface) {
            return ivisurf;
        }
    }

    return NULL;
}

WL_EXPORT struct ivi_layout_screen *
ivi_layout_getScreenFromId(uint32_t id_screen)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_screen *iviscrn = NULL;
    (void)id_screen;

    wl_list_for_each(iviscrn, &layout->list_screen, link) {
//FIXME : select iviscrn from list_screen by id_screen
        return iviscrn;
        break;
    }

    return NULL;
}

WL_EXPORT int32_t
ivi_layout_getScreenResolution(struct ivi_layout_screen *iviscrn,
                               int32_t *pWidth, int32_t *pHeight)
{
    struct weston_output *output = NULL;

    if (pWidth == NULL || pHeight == NULL) {
        weston_log("ivi_layout_getScreenResolution: invalid argument\n");
        return -1;
    }

    output   = iviscrn->output;
    *pWidth  = output->current_mode->width;
    *pHeight = output->current_mode->height;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceAddNotification(struct ivi_layout_surface *ivisurf,
                                  surfacePropertyNotificationFunc callback,
                                  void *userdata)
{
    struct listener_layoutNotification* notification = NULL;
    struct ivi_layout_notificationCallback *prop_callback = NULL;

    if (ivisurf == NULL || callback == NULL) {
        weston_log("ivi_layout_surfaceAddNotification: invalid argument\n");
        return -1;
    }

    notification = malloc(sizeof *notification);
    if (notification == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    prop_callback = malloc(sizeof *prop_callback);
    if (prop_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    prop_callback->callback = callback;
    prop_callback->data = userdata;

    notification->listener.notify = surface_prop_changed;
    notification->userdata = prop_callback;

    wl_signal_add(&ivisurf->property_changed, &notification->listener);

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceRemoveNotification(struct ivi_layout_surface *ivisurf)
{
    if (ivisurf == NULL) {
        weston_log("ivi_layout_surfaceRemoveNotification: invalid argument\n");
        return -1;
    }

    remove_all_notification(&ivisurf->property_changed.listener_list);

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceRemove(struct ivi_layout_surface *ivisurf)
{
    struct ivi_layout *layout = get_instance();

    if (ivisurf == NULL) {
        weston_log("ivi_layout_surfaceRemove: invalid argument\n");
        return -1;
    }

    if (!wl_list_empty(&ivisurf->pending.link)) {
        wl_list_remove(&ivisurf->pending.link);
    }
    if (!wl_list_empty(&ivisurf->order.link)) {
        wl_list_remove(&ivisurf->order.link);
    }
    if (!wl_list_empty(&ivisurf->link)) {
        wl_list_remove(&ivisurf->link);
    }
    remove_ordersurface_from_layer(ivisurf);

    wl_signal_emit(&layout->surface_notification.removed, ivisurf);

    ivi_layout_surfaceRemoveNotification(ivisurf);

    free(ivisurf);

    return 0;
}

WL_EXPORT int32_t
ivi_layout_UpdateInputEventAcceptanceOn(struct ivi_layout_surface *ivisurf,
                                        int32_t devices, int32_t acceptance)
{
    /* TODO */
    (void)ivisurf;
    (void)devices;
    (void)acceptance;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceInitialize(struct ivi_layout_surface **pSurfaceId)
{
    /* TODO */
    (void)pSurfaceId;
    return 0;
}

WL_EXPORT int32_t
ivi_layout_getPropertiesOfLayer(struct ivi_layout_layer *ivilayer,
                    struct ivi_layout_LayerProperties *pLayerProperties)
{
    if (ivilayer == NULL || pLayerProperties == NULL) {
        weston_log("ivi_layout_getPropertiesOfLayer: invalid argument\n");
        return -1;
    }

    *pLayerProperties = ivilayer->prop;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getNumberOfHardwareLayers(uint32_t id_screen,
                              int32_t *pNumberOfHardwareLayers)
{
    /* TODO */
    (void)id_screen;
    (void)pNumberOfHardwareLayers;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getScreens(int32_t *pLength, struct ivi_layout_screen ***ppArray)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_screen *iviscrn = NULL;
    int32_t length = 0;
    int32_t n = 0;

    if (pLength == NULL || ppArray == NULL) {
        weston_log("ivi_layout_getScreens: invalid argument\n");
        return -1;
    }

    length = wl_list_length(&layout->list_screen);

    if (length != 0){
        /* the Array must be free by module which called this function */
        *ppArray = calloc(length, sizeof(struct ivi_layout_screen *));
        if (*ppArray == NULL) {
            weston_log("fails to allocate memory\n");
            return -1;
        }

        wl_list_for_each(iviscrn, &layout->list_screen, link) {
            (*ppArray)[n++] = iviscrn;
        }
    }

    *pLength = length;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getScreensUnderLayer(struct ivi_layout_layer *ivilayer,
                                   int32_t *pLength,
                                   struct ivi_layout_screen ***ppArray)
{
    struct link_screen *link_scrn = NULL;
    int32_t length = 0;
    int32_t n = 0;

    if (ivilayer == NULL || pLength == NULL || ppArray == NULL) {
        weston_log("ivi_layout_getScreensUnderLayer: invalid argument\n");
        return -1;
    }

    length = wl_list_length(&ivilayer->list_screen);

    if (length != 0){
        /* the Array must be free by module which called this function */
        *ppArray = calloc(length, sizeof(struct ivi_layout_screen *));
        if (*ppArray == NULL) {
            weston_log("fails to allocate memory\n");
            return -1;
        }

        wl_list_for_each(link_scrn, &ivilayer->list_screen, link) {
            (*ppArray)[n++] = link_scrn->iviscrn;
        }
    }

    *pLength = length;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getLayers(int32_t *pLength, struct ivi_layout_layer ***ppArray)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_layer *ivilayer = NULL;
    int32_t length = 0;
    int32_t n = 0;

    if (pLength == NULL || ppArray == NULL) {
        weston_log("ivi_layout_getLayers: invalid argument\n");
        return -1;
    }

    length = wl_list_length(&layout->list_layer);

    if (length != 0){
        /* the Array must be free by module which called this function */
        *ppArray = calloc(length, sizeof(struct ivi_layout_layer *));
        if (*ppArray == NULL) {
            weston_log("fails to allocate memory\n");
            return -1;
        }

        wl_list_for_each(ivilayer, &layout->list_layer, link) {
            (*ppArray)[n++] = ivilayer;
        }
    }

    *pLength = length;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getLayersOnScreen(struct ivi_layout_screen *iviscrn,
                                int32_t *pLength,
                                struct ivi_layout_layer ***ppArray)
{
    struct ivi_layout_layer *ivilayer = NULL;
    int32_t length = 0;
    int32_t n = 0;

    if (iviscrn == NULL || pLength == NULL || ppArray == NULL) {
        weston_log("ivi_layout_getLayersOnScreen: invalid argument\n");
        return -1;
    }

    length = wl_list_length(&iviscrn->order.list_layer);

    if (length != 0){
        /* the Array must be free by module which called this function */
        *ppArray = calloc(length, sizeof(struct ivi_layout_layer *));
        if (*ppArray == NULL) {
            weston_log("fails to allocate memory\n");
            return -1;
        }

        wl_list_for_each(ivilayer, &iviscrn->order.list_layer, link) {
            (*ppArray)[n++] = ivilayer;
        }
    }

    *pLength = length;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getLayersUnderSurface(struct ivi_layout_surface *ivisurf,
                                    int32_t *pLength,
                                    struct ivi_layout_layer ***ppArray)
{
    struct link_layer *link_layer = NULL;
    int32_t length = 0;
    int32_t n = 0;

    if (ivisurf == NULL || pLength == NULL || ppArray == NULL) {
        weston_log("ivi_layout_getLayers: invalid argument\n");
        return -1;
    }

    length = wl_list_length(&ivisurf->list_layer);

    if (length != 0){
        /* the Array must be free by module which called this function */
        *ppArray = calloc(length, sizeof(struct ivi_layout_layer *));
        if (*ppArray == NULL) {
            weston_log("fails to allocate memory\n");
            return -1;
        }

        wl_list_for_each(link_layer, &ivisurf->list_layer, link) {
            (*ppArray)[n++] = link_layer->ivilayer;
        }
    }

    *pLength = length;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getSurfaces(int32_t *pLength, struct ivi_layout_surface ***ppArray)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_surface *ivisurf = NULL;
    int32_t length = 0;
    int32_t n = 0;

    if (pLength == NULL || ppArray == NULL) {
        weston_log("ivi_layout_getSurfaces: invalid argument\n");
        return -1;
    }

    length = wl_list_length(&layout->list_surface);

    if (length != 0){
        /* the Array must be free by module which called this function */
        *ppArray = calloc(length, sizeof(struct ivi_layout_surface *));
        if (*ppArray == NULL) {
            weston_log("fails to allocate memory\n");
            return -1;
        }

        wl_list_for_each(ivisurf, &layout->list_surface, link) {
            (*ppArray)[n++] = ivisurf;
        }
    }

    *pLength = length;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getSurfacesOnLayer(struct ivi_layout_layer *ivilayer,
                                 int32_t *pLength,
                                 struct ivi_layout_surface ***ppArray)
{
    struct ivi_layout_surface *ivisurf = NULL;
    int32_t length = 0;
    int32_t n = 0;

    if (ivilayer == NULL || pLength == NULL || ppArray == NULL) {
        weston_log("ivi_layout_getSurfaceIDsOnLayer: invalid argument\n");
        return -1;
    }

    length = wl_list_length(&ivilayer->order.list_surface);

    if (length != 0) {
        /* the Array must be free by module which called this function */
        *ppArray = calloc(length, sizeof(struct ivi_layout_surface *));
        if (*ppArray == NULL) {
            weston_log("fails to allocate memory\n");
            return -1;
        }

        wl_list_for_each(ivisurf, &ivilayer->order.list_surface, order.link) {
            (*ppArray)[n++] = ivisurf;
        }
    }

    *pLength = length;

    return 0;
}

WL_EXPORT struct ivi_layout_layer *
ivi_layout_layerCreateWithDimension(uint32_t id_layer,
                                       int32_t width, int32_t height)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_layer *ivilayer = NULL;

    ivilayer = get_layer(&layout->list_layer, id_layer);
    if (ivilayer != NULL) {
        weston_log("id_layer is already created\n");
        return ivilayer;
    }

    ivilayer = calloc(1, sizeof *ivilayer);
    if (ivilayer == NULL) {
        weston_log("fails to allocate memory\n");
        return NULL;
    }

    wl_list_init(&ivilayer->link);
    wl_signal_init(&ivilayer->property_changed);
    wl_list_init(&ivilayer->list_screen);
    wl_list_init(&ivilayer->link_to_surface);
    ivilayer->layout = layout;
    ivilayer->id_layer = id_layer;

    init_layerProperties(&ivilayer->prop, width, height);
    ivilayer->event_mask = 0;

    wl_list_init(&ivilayer->pending.list_surface);
    wl_list_init(&ivilayer->pending.link);
    ivilayer->pending.prop = ivilayer->prop;

    wl_list_init(&ivilayer->order.list_surface);
    wl_list_init(&ivilayer->order.link);

    wl_list_insert(&layout->list_layer, &ivilayer->link);

    wl_signal_emit(&layout->layer_notification.created, ivilayer);

    return ivilayer;
}

WL_EXPORT int32_t
ivi_layout_layerRemove(struct ivi_layout_layer *ivilayer)
{
    struct ivi_layout *layout = get_instance();

    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerRemove: invalid argument\n");
        return -1;
    }

    wl_signal_emit(&layout->layer_notification.removed, ivilayer);

    clear_surface_pending_list(ivilayer);
    clear_surface_order_list(ivilayer);

    if (!wl_list_empty(&ivilayer->pending.link)) {
        wl_list_remove(&ivilayer->pending.link);
    }
    if (!wl_list_empty(&ivilayer->order.link)) {
        wl_list_remove(&ivilayer->order.link);
    }
    if (!wl_list_empty(&ivilayer->link)) {
        wl_list_remove(&ivilayer->link);
    }
    remove_orderlayer_from_screen(ivilayer);
    remove_link_to_surface(ivilayer);
    ivi_layout_layerRemoveNotification(ivilayer);

    free(ivilayer);

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerGetType(struct ivi_layout_layer *ivilayer,
                        int32_t *pLayerType)
{
    /* TODO */
    (void)ivilayer;
    (void)pLayerType;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetVisibility(struct ivi_layout_layer *ivilayer,
                              int32_t newVisibility)
{
    struct ivi_layout_LayerProperties *prop = NULL;

    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerSetVisibility: invalid argument\n");
        return -1;
    }

    prop = &ivilayer->pending.prop;
    prop->visibility = newVisibility;

    ivilayer->event_mask |= IVI_NOTIFICATION_VISIBILITY;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerGetVisibility(struct ivi_layout_layer *ivilayer, int32_t *pVisibility)
{
    if (ivilayer == NULL || pVisibility == NULL) {
        weston_log("ivi_layout_layerGetVisibility: invalid argument\n");
        return -1;
    }

    *pVisibility = ivilayer->prop.visibility;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetOpacity(struct ivi_layout_layer *ivilayer,
                           float opacity)
{
    struct ivi_layout_LayerProperties *prop = NULL;

    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerSetOpacity: invalid argument\n");
        return -1;
    }

    prop = &ivilayer->pending.prop;
    prop->opacity = opacity;

    ivilayer->event_mask |= IVI_NOTIFICATION_OPACITY;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerGetOpacity(struct ivi_layout_layer *ivilayer,
                           float *pOpacity)
{
    if (ivilayer == NULL || pOpacity == NULL) {
        weston_log("ivi_layout_layerGetOpacity: invalid argument\n");
        return -1;
    }

    *pOpacity = ivilayer->prop.opacity;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetSourceRectangle(struct ivi_layout_layer *ivilayer,
                            int32_t x, int32_t y,
                            int32_t width, int32_t height)
{
    struct ivi_layout_LayerProperties *prop = NULL;

    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerSetSourceRectangle: invalid argument\n");
        return -1;
    }

    prop = &ivilayer->pending.prop;
    prop->sourceX = x;
    prop->sourceY = y;
    prop->sourceWidth = width;
    prop->sourceHeight = height;

    ivilayer->event_mask |= IVI_NOTIFICATION_SOURCE_RECT;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetDestinationRectangle(struct ivi_layout_layer *ivilayer,
                                 int32_t x, int32_t y,
                                 int32_t width, int32_t height)
{
    struct ivi_layout_LayerProperties *prop = NULL;

    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerSetDestinationRectangle: invalid argument\n");
        return -1;
    }

    prop = &ivilayer->pending.prop;
    prop->destX = x;
    prop->destY = y;
    prop->destWidth = width;
    prop->destHeight = height;

    ivilayer->event_mask |= IVI_NOTIFICATION_DEST_RECT;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerGetDimension(struct ivi_layout_layer *ivilayer,
                             int32_t *pDimension)
{
    if (ivilayer == NULL || &pDimension[0] == NULL || &pDimension[1] == NULL) {
        weston_log("ivi_layout_layerGetDimension: invalid argument\n");
        return -1;
    }

    pDimension[0] = ivilayer->prop.destX;
    pDimension[1] = ivilayer->prop.destY;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetDimension(struct ivi_layout_layer *ivilayer,
                             int32_t *pDimension)
{
    struct ivi_layout_LayerProperties *prop = NULL;

    if (ivilayer == NULL || &pDimension[0] == NULL || &pDimension[1] == NULL) {
        weston_log("ivi_layout_layerSetDimension: invalid argument\n");
        return -1;
    }

    prop = &ivilayer->pending.prop;

    prop->destWidth  = pDimension[0];
    prop->destHeight = pDimension[1];

    ivilayer->event_mask |= IVI_NOTIFICATION_DIMENSION;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerGetPosition(struct ivi_layout_layer *ivilayer, int32_t *pPosition)
{
    if (ivilayer == NULL || pPosition == NULL) {
        weston_log("ivi_layout_layerGetPosition: invalid argument\n");
        return -1;
    }

    pPosition[0] = ivilayer->prop.destX;
    pPosition[1] = ivilayer->prop.destY;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetPosition(struct ivi_layout_layer *ivilayer, int32_t *pPosition)
{
    struct ivi_layout_LayerProperties *prop = NULL;

    if (ivilayer == NULL || pPosition == NULL) {
        weston_log("ivi_layout_layerSetPosition: invalid argument\n");
        return -1;
    }

    prop = &ivilayer->pending.prop;
    prop->destX = pPosition[0];
    prop->destY = pPosition[1];

    ivilayer->event_mask |= IVI_NOTIFICATION_POSITION;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetOrientation(struct ivi_layout_layer *ivilayer,
                               int32_t orientation)
{
    struct ivi_layout_LayerProperties *prop = NULL;

    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerSetOrientation: invalid argument\n");
        return -1;
    }

    prop = &ivilayer->pending.prop;
    prop->orientation = orientation;

    ivilayer->event_mask |= IVI_NOTIFICATION_ORIENTATION;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerGetOrientation(struct ivi_layout_layer *ivilayer,
                               int32_t *pOrientation)
{
    if (ivilayer == NULL || pOrientation == NULL) {
        weston_log("ivi_layout_layerGetOrientation: invalid argument\n");
        return -1;
    }

    *pOrientation = ivilayer->prop.orientation;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetChromaKey(struct ivi_layout_layer *ivilayer, int32_t* pColor)
{
    /* TODO */
    (void)ivilayer;
    (void)pColor;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerSetRenderOrder(struct ivi_layout_layer *ivilayer,
                        struct ivi_layout_surface **pSurface,
                        int32_t number)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_surface *ivisurf = NULL;
    struct ivi_layout_surface *next = NULL;
    uint32_t *id_surface = NULL;
    int32_t i = 0;

    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerSetRenderOrder: invalid argument\n");
        return -1;
    }

    if (pSurface == NULL) {
        wl_list_for_each_safe(ivisurf, next, &ivilayer->pending.list_surface, pending.link) {
            if (!wl_list_empty(&ivisurf->pending.link)) {
                wl_list_remove(&ivisurf->pending.link);
            }

            wl_list_init(&ivisurf->pending.link);
        }
        ivilayer->event_mask |= IVI_NOTIFICATION_REMOVE;
        return 0;
    }

    for (i = 0; i < number; i++) {
        id_surface = &pSurface[i]->id_surface;

        wl_list_for_each_safe(ivisurf, next, &layout->list_surface, pending.link) {
            if (*id_surface != ivisurf->id_surface) {
                continue;
            }

            if (!wl_list_empty(&ivisurf->pending.link)) {
                wl_list_remove(&ivisurf->pending.link);
            }
            wl_list_init(&ivisurf->pending.link);
            wl_list_insert(&ivilayer->pending.list_surface,
                           &ivisurf->pending.link);
            break;
        }
    }

    ivilayer->event_mask |= IVI_NOTIFICATION_ADD;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerGetCapabilities(struct ivi_layout_layer *ivilayer,
                                int32_t *pCapabilities)
{
    /* TODO */
    (void)ivilayer;
    (void)pCapabilities;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerTypeGetCapabilities(int32_t layerType,
                                    int32_t *pCapabilities)
{
    /* TODO */
    (void)layerType;
    (void)pCapabilities;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetVisibility(struct ivi_layout_surface *ivisurf,
                                int32_t newVisibility)
{
    struct ivi_layout_SurfaceProperties *prop = NULL;

    if (ivisurf == NULL) {
        weston_log("ivi_layout_surfaceSetVisibility: invalid argument\n");
        return -1;
    }

    prop = &ivisurf->pending.prop;
    prop->visibility = newVisibility;

    ivisurf->event_mask |= IVI_NOTIFICATION_VISIBILITY;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceGetVisibility(struct ivi_layout_surface *ivisurf,
                                int32_t *pVisibility)
{
    if (ivisurf == NULL || pVisibility == NULL) {
        weston_log("ivi_layout_surfaceGetVisibility: invalid argument\n");
        return -1;
    }

    *pVisibility = ivisurf->prop.visibility;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetOpacity(struct ivi_layout_surface *ivisurf,
                             float opacity)
{
    struct ivi_layout_SurfaceProperties *prop = NULL;

    if (ivisurf == NULL) {
        weston_log("ivi_layout_surfaceSetOpacity: invalid argument\n");
        return -1;
    }

    prop = &ivisurf->pending.prop;
    prop->opacity = opacity;

    ivisurf->event_mask |= IVI_NOTIFICATION_OPACITY;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceGetOpacity(struct ivi_layout_surface *ivisurf,
                             float *pOpacity)
{
    if (ivisurf == NULL || pOpacity == NULL) {
        weston_log("ivi_layout_surfaceGetOpacity: invalid argument\n");
        return -1;
    }

    *pOpacity = ivisurf->prop.opacity;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_SetKeyboardFocusOn(struct ivi_layout_surface *ivisurf)
{
    /* TODO */
    (void)ivisurf;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_GetKeyboardFocusSurfaceId(struct ivi_layout_surface **pSurfaceId)
{
    /* TODO */
    (void)pSurfaceId;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetDestinationRectangle(struct ivi_layout_surface *ivisurf,
                                          int32_t x, int32_t y,
                                          int32_t width, int32_t height)
{
    struct ivi_layout_SurfaceProperties *prop = NULL;

    if (ivisurf == NULL) {
        weston_log("ivi_layout_surfaceSetDestinationRectangle: invalid argument\n");
        return -1;
    }

    prop = &ivisurf->pending.prop;
    prop->startX = prop->destX;
    prop->startY = prop->destY;
    prop->destX = x;
    prop->destY = y;
    prop->startWidth = prop->destWidth;
    prop->startHeight = prop->destHeight;
    prop->destWidth = width;
    prop->destHeight = height;

    ivisurf->event_mask |= IVI_NOTIFICATION_DEST_RECT;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetDimension(struct ivi_layout_surface *ivisurf, int32_t *pDimension)
{
    struct ivi_layout_SurfaceProperties *prop = NULL;

    if (ivisurf == NULL || &pDimension[0] == NULL || &pDimension[1] == NULL) {
        weston_log("ivi_layout_surfaceSetDimension: invalid argument\n");
        return -1;
    }

    prop = &ivisurf->pending.prop;
    prop->destWidth  = pDimension[0];
    prop->destHeight = pDimension[1];

    ivisurf->event_mask |= IVI_NOTIFICATION_DIMENSION;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceGetDimension(struct ivi_layout_surface *ivisurf,
                               int32_t *pDimension)
{
    if (ivisurf == NULL || &pDimension[0] == NULL || &pDimension[1] == NULL) {
        weston_log("ivi_layout_surfaceGetDimension: invalid argument\n");
        return -1;
    }

    pDimension[0] = ivisurf->prop.destWidth;
    pDimension[1] = ivisurf->prop.destHeight;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetPosition(struct ivi_layout_surface *ivisurf,
                              int32_t *pPosition)
{
    struct ivi_layout_SurfaceProperties *prop = NULL;

    if (ivisurf == NULL || pPosition == NULL) {
        weston_log("ivi_layout_surfaceSetPosition: invalid argument\n");
        return -1;
    }

    prop = &ivisurf->pending.prop;
    prop->destX = pPosition[0];
    prop->destY = pPosition[1];

    ivisurf->event_mask |= IVI_NOTIFICATION_POSITION;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceGetPosition(struct ivi_layout_surface *ivisurf,
                              int32_t *pPosition)
{
    if (ivisurf == NULL || pPosition == NULL) {
        weston_log("ivi_layout_surfaceGetPosition: invalid argument\n");
        return -1;
    }

    pPosition[0] = ivisurf->prop.destX;
    pPosition[1] = ivisurf->prop.destY;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetOrientation(struct ivi_layout_surface *ivisurf,
                                 int32_t orientation)
{
    struct ivi_layout_SurfaceProperties *prop = NULL;

    if (ivisurf == NULL) {
        weston_log("ivi_layout_surfaceSetOrientation: invalid argument\n");
        return -1;
    }

    prop = &ivisurf->pending.prop;
    prop->orientation = orientation;

    ivisurf->event_mask |= IVI_NOTIFICATION_ORIENTATION;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceGetOrientation(struct ivi_layout_surface *ivisurf,
                                 int32_t *pOrientation)
{
    if (ivisurf == NULL || pOrientation == NULL) {
        weston_log("ivi_layout_surfaceGetOrientation: invalid argument\n");
        return -1;
    }

    *pOrientation = ivisurf->prop.orientation;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceGetPixelformat(struct ivi_layout_layer *ivisurf, int32_t *pPixelformat)
{
    /* TODO */
    (void)ivisurf;
    (void)pPixelformat;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetChromaKey(struct ivi_layout_surface *ivisurf, int32_t* pColor)
{
    /* TODO */
    (void)ivisurf;
    (void)pColor;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_screenAddLayer(struct ivi_layout_screen *iviscrn,
                          struct ivi_layout_layer *addlayer)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_layer *ivilayer = NULL;
    struct ivi_layout_layer *next = NULL;
    int is_layer_in_scrn = 0;

    if (iviscrn == NULL || addlayer == NULL) {
        weston_log("ivi_layout_screenAddLayer: invalid argument\n");
        return -1;
    }

    is_layer_in_scrn = is_layer_in_screen(addlayer, iviscrn);
    if (is_layer_in_scrn == 1) {
        weston_log("ivi_layout_screenAddLayer: addlayer is already available\n");
        return 0;
    }

    wl_list_for_each_safe(ivilayer, next, &layout->list_layer, link) {
        if (ivilayer->id_layer == addlayer->id_layer) {
            if (!wl_list_empty(&ivilayer->pending.link)) {
                wl_list_remove(&ivilayer->pending.link);
            }
            wl_list_init(&ivilayer->pending.link);
            wl_list_insert(&iviscrn->pending.list_layer,
                           &ivilayer->pending.link);
            break;
        }
    }

    iviscrn->event_mask |= IVI_NOTIFICATION_ADD;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_screenSetRenderOrder(struct ivi_layout_screen *iviscrn,
                                struct ivi_layout_layer **pLayer,
                                const int32_t number)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_layer *ivilayer = NULL;
    struct ivi_layout_layer *next = NULL;
    uint32_t *id_layer = NULL;
    int32_t i = 0;

    if (iviscrn == NULL) {
        weston_log("ivi_layout_screenSetRenderOrder: invalid argument\n");
        return -1;
    }

    wl_list_for_each_safe(ivilayer, next,
                          &iviscrn->pending.list_layer, pending.link) {
        wl_list_init(&ivilayer->pending.link);
    }

    wl_list_init(&iviscrn->pending.list_layer);

    if (pLayer == NULL) {
        wl_list_for_each_safe(ivilayer, next, &iviscrn->pending.list_layer, pending.link) {
            if (!wl_list_empty(&ivilayer->pending.link)) {
                wl_list_remove(&ivilayer->pending.link);
            }

            wl_list_init(&ivilayer->pending.link);
        }

        iviscrn->event_mask |= IVI_NOTIFICATION_REMOVE;
        return 0;
    }

    for (i = 0; i < number; i++) {
        id_layer = &pLayer[i]->id_layer;
        wl_list_for_each(ivilayer, &layout->list_layer, link) {
            if (*id_layer != ivilayer->id_layer) {
                continue;
            }

            if (!wl_list_empty(&ivilayer->pending.link)) {
                wl_list_remove(&ivilayer->pending.link);
            }
            wl_list_init(&ivilayer->pending.link);
            wl_list_insert(&iviscrn->pending.list_layer,
                           &ivilayer->pending.link);
            break;
        }
    }

    iviscrn->event_mask |= IVI_NOTIFICATION_ADD;

    return 0;
}

WL_EXPORT struct weston_output *
ivi_layout_screenGetOutput(struct ivi_layout_screen *iviscrn)
{
    return iviscrn->output;
}

WL_EXPORT int32_t
ivi_layout_SetOptimizationMode(uint32_t id, int32_t mode)
{
    /* TODO */
    (void)id;
    (void)mode;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_GetOptimizationMode(uint32_t id, int32_t *pMode)
{
    /* TODO */
    (void)id;
    (void)pMode;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerAddNotification(struct ivi_layout_layer *ivilayer,
                                layerPropertyNotificationFunc callback,
                                void *userdata)
{
    struct ivi_layout_notificationCallback *prop_callback = NULL;

    if (ivilayer == NULL || callback == NULL) {
        weston_log("ivi_layout_layerAddNotification: invalid argument\n");
        return -1;
    }

    prop_callback = malloc(sizeof *prop_callback);
    if (prop_callback == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    prop_callback->callback = callback;
    prop_callback->data = userdata;

    return add_notification(&ivilayer->property_changed,
                            layer_prop_changed,
                            prop_callback);
}

WL_EXPORT int32_t
ivi_layout_layerRemoveNotification(struct ivi_layout_layer *ivilayer)
{
    if (ivilayer == NULL) {
        weston_log("ivi_layout_layerRemoveNotification: invalid argument\n");
        return -1;
    }

    remove_all_notification(&ivilayer->property_changed.listener_list);

    return 0;
}

WL_EXPORT int32_t
ivi_layout_getPropertiesOfSurface(struct ivi_layout_surface *ivisurf,
                    struct ivi_layout_SurfaceProperties *pSurfaceProperties)
{
    if (ivisurf == NULL || pSurfaceProperties == NULL) {
        weston_log("ivi_layout_getPropertiesOfSurface: invalid argument\n");
        return -1;
    }

    *pSurfaceProperties = ivisurf->prop;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerAddSurface(struct ivi_layout_layer *ivilayer,
                           struct ivi_layout_surface *addsurf)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_surface *ivisurf = NULL;
    struct ivi_layout_surface *next = NULL;
    int is_surf_in_layer = 0;

    if (ivilayer == NULL || addsurf == NULL) {
        weston_log("ivi_layout_layerAddSurface: invalid argument\n");
        return -1;
    }

    is_surf_in_layer = is_surface_in_layer(addsurf, ivilayer);
    if (is_surf_in_layer == 1) {
        weston_log("ivi_layout_layerAddSurface: addsurf is already available\n");
        return 0;
    }

    wl_list_for_each_safe(ivisurf, next, &layout->list_surface, link) {
        if (ivisurf->id_surface == addsurf->id_surface) {
            if (!wl_list_empty(&ivisurf->pending.link)) {
                wl_list_remove(&ivisurf->pending.link);
            }
            wl_list_init(&ivisurf->pending.link);
            wl_list_insert(&ivilayer->pending.list_surface,
                           &ivisurf->pending.link);
            break;
        }
    }

    ivilayer->event_mask |= IVI_NOTIFICATION_ADD;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_layerRemoveSurface(struct ivi_layout_layer *ivilayer,
                              struct ivi_layout_surface *remsurf)
{
    struct ivi_layout_surface *ivisurf = NULL;
    struct ivi_layout_surface *next = NULL;

    if (ivilayer == NULL || remsurf == NULL) {
        weston_log("ivi_layout_layerRemoveSurface: invalid argument\n");
        return -1;
    }

    wl_list_for_each_safe(ivisurf, next,
                          &ivilayer->pending.list_surface, pending.link) {
        if (ivisurf->id_surface == remsurf->id_surface) {
            if (!wl_list_empty(&ivisurf->pending.link)) {
                wl_list_remove(&ivisurf->pending.link);
            }
            wl_list_init(&ivisurf->pending.link);
            break;
        }
    }

    remsurf->event_mask |= IVI_NOTIFICATION_REMOVE;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetSourceRectangle(struct ivi_layout_surface *ivisurf,
                                     int32_t x, int32_t y,
                                     int32_t width, int32_t height)
{
    struct ivi_layout_SurfaceProperties *prop = NULL;

    if (ivisurf == NULL) {
        weston_log("ivi_layout_surfaceSetSourceRectangle: invalid argument\n");
        return -1;
    }

    prop = &ivisurf->pending.prop;
    prop->sourceX = x;
    prop->sourceY = y;
    prop->sourceWidth = width;
    prop->sourceHeight = height;

    ivisurf->event_mask |= IVI_NOTIFICATION_SOURCE_RECT;

    return 0;
}

WL_EXPORT int32_t
ivi_layout_commitChanges(void)
{
    struct ivi_layout *layout = get_instance();

    commit_list_surface(layout);
    commit_list_layer(layout);
    commit_list_screen(layout);

    commit_transition(layout);

    commit_changes(layout);
    send_prop(layout);
    weston_compositor_schedule_repaint(layout->compositor);

    return 0;
}

/***called from ivi-shell**/
static struct weston_view *
ivi_layout_get_weston_view(struct ivi_layout_surface *surface)
{
    if(surface == NULL) return NULL;
    struct weston_view *tmpview = NULL;
    wl_list_for_each(tmpview, &surface->surface->views, surface_link)
    {
        if (tmpview != NULL) {
            break;
        }
    }
    return tmpview;
}

static void
ivi_layout_surfaceConfigure(struct ivi_layout_surface *ivisurf,
                               int32_t width, int32_t height)
{
    struct ivi_layout *layout = get_instance();

    ivisurf->surface->width_from_buffer  = width;
    ivisurf->surface->height_from_buffer = height;

    wl_signal_emit(&layout->surface_notification.configure_changed, ivisurf);
}

static int32_t
ivi_layout_surfaceSetNativeContent(struct weston_surface *surface,
                                      int32_t width,
                                      int32_t height,
                                      uint32_t id_surface)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_surface *ivisurf;

    ivisurf = get_surface(&layout->list_surface, id_surface);
    if (ivisurf == NULL) {
        weston_log("layout surface is not found\n");
        return -1;
    }

    if (ivisurf->surface != NULL) {
        if (surface != NULL) {
            weston_log("id_surface(%d) is already set the native content\n",
                       id_surface);
            return -1;
        }

        wl_list_remove(&ivisurf->surface_destroy_listener.link);

        ivisurf->surface = NULL;

        wl_list_remove(&ivisurf->surface_rotation.link);
        wl_list_remove(&ivisurf->layer_rotation.link);
        wl_list_remove(&ivisurf->surface_pos.link);
        wl_list_remove(&ivisurf->layer_pos.link);
        wl_list_remove(&ivisurf->scaling.link);
        wl_list_init(&ivisurf->surface_rotation.link);
        wl_list_init(&ivisurf->layer_rotation.link);
        wl_list_init(&ivisurf->surface_pos.link);
        wl_list_init(&ivisurf->layer_pos.link);
        wl_list_init(&ivisurf->scaling.link);

    }

    if (surface == NULL) {
        if (ivisurf->content_observer.callback) {
            (*(ivisurf->content_observer.callback))(ivisurf,
                                    0, ivisurf->content_observer.userdata);
        }

        ivi_layout_surfaceRemoveNotification(ivisurf);
        return 0;
    }

    ivisurf->surface = surface;
    ivisurf->surface_destroy_listener.notify =
        westonsurface_destroy_from_ivisurface;
    wl_resource_add_destroy_listener(surface->resource,
                                     &ivisurf->surface_destroy_listener);

    struct weston_view *tmpview = weston_view_create(surface);
    if (tmpview == NULL) {
        weston_log("fails to allocate memory\n");
        return -1;
    }

    ivisurf->surface->width_from_buffer  = width;
    ivisurf->surface->height_from_buffer = height;
    ivisurf->pixelformat = IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_8888;

    wl_signal_emit(&layout->surface_notification.created, ivisurf);

    if (ivisurf->content_observer.callback) {
        (*(ivisurf->content_observer.callback))(ivisurf,
                                     1, ivisurf->content_observer.userdata);
    }

    return 0;
}

WL_EXPORT int32_t
ivi_layout_surfaceSetContentObserver(struct ivi_layout_surface *ivisurf,
                                     ivi_controller_surface_content_callback callback,
                                     void* userdata)
{
    int32_t ret = -1;
    if (ivisurf != NULL) {
        ivisurf->content_observer.callback = callback;
        ivisurf->content_observer.userdata = userdata;
        ret = 0;
    }
    return ret;
}

static struct ivi_layout_surface*
ivi_layout_surfaceCreate(struct weston_surface *wl_surface,
                         uint32_t id_surface)
{
    struct ivi_layout *layout = get_instance();
    struct ivi_layout_surface *ivisurf = NULL;

    if (wl_surface == NULL) {
        weston_log("ivi_layout_surfaceCreate: invalid argument\n");
        return NULL;
    }

    ivisurf = get_surface(&layout->list_surface, id_surface);
    if (ivisurf != NULL) {
        if (ivisurf->surface != NULL) {
            weston_log("id_surface(%d) is already created\n", id_surface);
            return NULL;
        } else {
            /* if ivisurf->surface exist, wl_surface is tied to id_surface again */
            /* This means client destroys ivi_surface once, and then tries to tie
                the id_surface to new wl_surface again. The property of id_surface can
                be inherited.
            */
            ivi_layout_surfaceSetNativeContent(
                wl_surface, wl_surface->width, wl_surface->height, id_surface);
            return ivisurf;
        }
    }

    ivisurf = calloc(1, sizeof *ivisurf);
    if (ivisurf == NULL) {
        weston_log("fails to allocate memory\n");
        return NULL;
    }

    wl_list_init(&ivisurf->link);
    wl_signal_init(&ivisurf->property_changed);
    wl_list_init(&ivisurf->list_layer);
    ivisurf->id_surface = id_surface;
    ivisurf->layout = layout;

    ivisurf->surface = wl_surface;
    ivisurf->surface_destroy_listener.notify =
        westonsurface_destroy_from_ivisurface;
    wl_resource_add_destroy_listener(wl_surface->resource,
                                     &ivisurf->surface_destroy_listener);

    struct weston_view *tmpview = weston_view_create(wl_surface);
    if (tmpview == NULL) {
        weston_log("fails to allocate memory\n");
    }

    ivisurf->surface->width_from_buffer  = 0;
    ivisurf->surface->height_from_buffer = 0;

    weston_matrix_init(&ivisurf->surface_rotation.matrix);
    weston_matrix_init(&ivisurf->layer_rotation.matrix);
    weston_matrix_init(&ivisurf->surface_pos.matrix);
    weston_matrix_init(&ivisurf->layer_pos.matrix);
    weston_matrix_init(&ivisurf->scaling.matrix);

    wl_list_init(&ivisurf->surface_rotation.link);
    wl_list_init(&ivisurf->layer_rotation.link);
    wl_list_init(&ivisurf->surface_pos.link);
    wl_list_init(&ivisurf->layer_pos.link);
    wl_list_init(&ivisurf->scaling.link);

    init_surfaceProperties(&ivisurf->prop);
    ivisurf->pixelformat = IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_8888;
    ivisurf->event_mask = 0;

    ivisurf->pending.prop = ivisurf->prop;
    wl_list_init(&ivisurf->pending.link);

    wl_list_init(&ivisurf->order.link);
    wl_list_init(&ivisurf->order.list_layer);

    wl_list_insert(&layout->list_surface, &ivisurf->link);

    wl_signal_emit(&layout->surface_notification.created, ivisurf);

    return ivisurf;
}

static void
ivi_layout_initWithCompositor(struct weston_compositor *ec)
{
    struct ivi_layout *layout = get_instance();

    layout->compositor = ec;

    wl_list_init(&layout->list_surface);
    wl_list_init(&layout->list_layer);
    wl_list_init(&layout->list_screen);

    wl_signal_init(&layout->layer_notification.created);
    wl_signal_init(&layout->layer_notification.removed);

    wl_signal_init(&layout->surface_notification.created);
    wl_signal_init(&layout->surface_notification.removed);
    wl_signal_init(&layout->surface_notification.configure_changed);

    wl_signal_init(&layout->warning_signal);

    /* Add layout_layer at the last of weston_compositor.layer_list */
    weston_layer_init(&layout->layout_layer, ec->layer_list.prev);

    create_screen(ec);

    struct weston_config *config = weston_config_parse("weston.ini");
    struct weston_config_section *s =
            weston_config_get_section(config, "ivi-shell", NULL, NULL);

    /*A cursor is configured if weston.ini has keys.*/
    char* cursor_theme = NULL;
    weston_config_section_get_string(s, "cursor-theme", &cursor_theme, NULL);
    if (cursor_theme)
        free(cursor_theme);
    else
        wl_list_remove(&ec->cursor_layer.link);
    weston_config_destroy(config);

    layout->transitions = ivi_layout_transition_set_create(ec);
    wl_list_init(&layout->pending_transition_list);

}


WL_EXPORT struct ivi_layout_interface ivi_layout_interface = {
	.get_weston_view = ivi_layout_get_weston_view,
	.surfaceConfigure = ivi_layout_surfaceConfigure,
	.surfaceSetNativeContent = ivi_layout_surfaceSetNativeContent,
	.surfaceCreate = ivi_layout_surfaceCreate,
	.initWithCompositor = ivi_layout_initWithCompositor,
	.emitWarningSignal = ivi_layout_emitWarningSignal
};
