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

#include <string.h>

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

struct seat_ctx {
    struct weston_keyboard_grab grab;
    struct wl_listener updated_caps_listener;
    struct wl_listener destroy_listener;
};

struct ivi_layout_notificationCallback {
    void *callback;
    void *data;
};

struct shellWarningArgs {
    uint32_t id_surface;
    enum ivi_layout_warning_flag flag;
};

static struct weston_view *
ivi_layout_get_weston_view(struct ivi_layout_surface *surface);

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
    wl_list_init(&ivisurf->surface_scaling.link);
    wl_list_init(&ivisurf->layer_scaling.link);

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
update_layer_source_position(struct ivi_layout_layer *ivilayer,
                             struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_matrix *matrix = &ivisurf->layer_source_pos.matrix;
    float tx = -(float)ivilayer->prop.sourceX;
    float ty = -(float)ivilayer->prop.sourceY;

    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL) {
            break;
        }
    }

    if (view == NULL) {
        return;
    }

    wl_list_remove(&ivisurf->layer_source_pos.link);

    weston_matrix_init(matrix);
    weston_matrix_translate(matrix, tx, ty, 0.0f);
    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->layer_source_pos.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);
}

static void
update_surface_source_position(struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_matrix *matrix = &ivisurf->surface_source_pos.matrix;
    float tx = -(float)ivisurf->prop.sourceX;
    float ty = -(float)ivisurf->prop.sourceY;

    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL) {
            break;
        }
    }

    if (view == NULL) {
        return;
    }

    wl_list_remove(&ivisurf->surface_source_pos.link);

    weston_matrix_init(matrix);
    weston_matrix_translate(matrix, tx, ty, 0.0f);
    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->surface_source_pos.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);
}

static void
update_surface_scale(struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_matrix *matrix = &ivisurf->surface_scaling.matrix;
    float sx = 0.0f;
    float sy = 0.0f;

    /* Get the first view from the surface */
    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL)
        {
            break;
        }
    }
    if (view == NULL)
    {
        return;
    }

    if (ivisurf->prop.destWidth == 0 && ivisurf->prop.destHeight == 0)
    {
        ivisurf->prop.destWidth  = ivisurf->surface->width_from_buffer;
        ivisurf->prop.destHeight = ivisurf->surface->height_from_buffer;
    }

    sx = ((float)ivisurf->prop.destWidth / (float)ivisurf->prop.sourceWidth);
    sy = ((float)ivisurf->prop.destHeight / (float)ivisurf->prop.sourceHeight);

    wl_list_remove(&ivisurf->surface_scaling.link);
    weston_matrix_init(matrix);
    weston_matrix_scale(matrix, sx, sy, 1.0f);
    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->surface_scaling.link);

    weston_view_set_transform_parent(view, NULL);
    weston_view_update_transform(view);
}

static void
update_layer_scale(struct ivi_layout_layer *ivilayer,
                   struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_matrix *matrix = &ivisurf->layer_scaling.matrix;
    float sx = 0.0f;
    float sy = 0.0f;

    /* Get the first view from the surface */
    wl_list_for_each(view, &ivisurf->surface->views, surface_link)
    {
        if (view != NULL)
        {
            break;
        }
    }
    if (view == NULL)
    {
        return;
    }

    sx = ((float)ivilayer->prop.destWidth / (float)ivilayer->prop.sourceWidth);
    sy = ((float)ivilayer->prop.destHeight / (float)ivilayer->prop.sourceHeight);

    wl_list_remove(&ivisurf->layer_scaling.link);
    weston_matrix_init(matrix);
    weston_matrix_scale(matrix, sx, sy, 1.0f);
    wl_list_insert(&view->geometry.transformation_list,
                   &ivisurf->layer_scaling.link);

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
        update_layer_scale(ivilayer, ivisurf);
        update_layer_source_position(ivilayer, ivisurf);
        update_surface_position(ivisurf);
        update_surface_orientation(ivilayer, ivisurf);
        update_surface_scale(ivisurf);
        update_surface_source_position(ivisurf);

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

static int
compute_bbox(struct weston_matrix *mat, float *left, float *top,
             float *right, float *bottom)
{
    float min_x = HUGE_VALF, min_y = HUGE_VALF;
    float max_x = -HUGE_VALF, max_y = -HUGE_VALF;
    float corners [4][2] = {
        { *left, *top },
        { *left, *bottom },
        { *right, *top },
        { *right, *bottom }
    };
    int i;

    for (i = 0; i < 4; ++i) {
        struct weston_vector vec = { { corners[i][0], corners[i][1],
                                       0.0f, 1.0f } };
        float x, y;
        weston_matrix_transform(mat, &vec);

        if (fabsf(vec.f[3]) < 1e-6) {
            weston_log("warning: numerical instability in %s(), divisor = %g\n",
                       __func__, vec.f[3]);
            *left = *top = *right = *bottom = 0;
            return 0;
        }

        x = vec.f[0] / vec.f[3];
        y = vec.f[1] / vec.f[3];

        if (x < min_x)
            min_x = x;
        if (x > max_x)
            max_x = x;
        if (y < min_y)
            min_y = y;
        if (y > max_y)
            max_y = y;
    }

    *left = min_x;
    *right = max_x;
    *top = min_y;
    *bottom = max_y;

    return 1;
}

static void
set_surface_mask(struct ivi_layout_surface *ivisurf)
{
    struct link_layer *link_layer = NULL;
    struct ivi_layout_layer *ivilayer;

    /* This doesn't make sense if a surface belongs to more than one layer */
    if (wl_list_length(&ivisurf->list_layer) > 1) {
        weston_log("%s: surface %d is in multiple layers! This implementation "
                   "of surface and layer clipping will not make sense!\n",
                   __FUNCTION__, ivisurf->id_surface);
        return;
    }

    wl_list_for_each(link_layer, &ivisurf->list_layer, link)
        break;

    if (link_layer == NULL) {
        return;
    }

    ivilayer = link_layer->ivilayer;

    if (ivisurf->surface != NULL
        && ivisurf->wl_layer_dirty) {
        struct weston_matrix mat;
        struct weston_transform *tform;
        struct weston_view *view;
        float top = 0;
        float left = 0;
        float right = ivisurf->surface->width;
        float bottom = ivisurf->surface->height;
        pixman_region32_t region;

        wl_list_for_each(view, &ivisurf->surface->views, surface_link)
            break;

        if (view == NULL)
            return;

        weston_matrix_init(&mat);

        /* Generate the transformation matrix */
        wl_list_for_each(tform, &view->geometry.transformation_list, link) {
            /* Exclude the surface source region position transform */
            if (tform == &ivisurf->surface_source_pos)
                continue;

            weston_matrix_multiply(&mat, &tform->matrix);
        }

        /* Transform each coordinate */
        compute_bbox(&mat, &left, &top, &right, &bottom);

        /* Assemble the mask */
        pixman_region32_init_rect(&region, left, top, right - left, bottom - top);
        pixman_region32_intersect_rect(&region, &region,
                                       ivilayer->pending.prop.destX,
                                       ivilayer->pending.prop.destY,
                                       ivilayer->pending.prop.destWidth,
                                       ivilayer->pending.prop.destHeight);
        ivisurf->wl_layer.mask = *pixman_region32_extents(&region);

        pixman_region32_fini(&region);
	ivisurf->wl_layer_dirty = 0;
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
                set_surface_mask(ivisurf);
            }
        }
    }
}

static void
check_surface_mask_dirty(struct ivi_layout_surface *ivisurf)
{
    struct link_layer *link_layer = NULL;
    struct ivi_layout_layer *ivilayer;

    /* This doesn't make sense if a surface belongs to more than one layer */
    if (wl_list_length(&ivisurf->list_layer) > 1) {
        weston_log("%s: surface %d is in multiple layers! This implementation "
                   "of surface and layer clipping will not make sense!\n",
                   __FUNCTION__, ivisurf->id_surface);
        return;
    }

    wl_list_for_each(link_layer, &ivisurf->list_layer, link)
        break;

    if (link_layer == NULL) {
        return;
    }

    ivilayer = link_layer->ivilayer;

    if (ivisurf->surface != NULL
        && ivisurf->pending.prop.visibility != 0
        && ivilayer->pending.prop.visibility != 0
        && (ivilayer->prop.destX != ivilayer->pending.prop.destX
            || ivilayer->prop.destY != ivilayer->pending.prop.destY
            || ivilayer->prop.destWidth != ivilayer->pending.prop.destWidth
            || ivilayer->prop.destHeight != ivilayer->pending.prop.destHeight
            || ivilayer->prop.sourceWidth != ivilayer->pending.prop.sourceWidth
            || ivilayer->prop.sourceHeight != ivilayer->pending.prop.sourceHeight
            || ivisurf->prop.destX != ivisurf->pending.prop.destX
            || ivisurf->prop.destY != ivisurf->pending.prop.destY
            || ivisurf->prop.destWidth != ivisurf->pending.prop.destWidth
            || ivisurf->prop.destHeight != ivisurf->pending.prop.destHeight
            || ivisurf->prop.sourceWidth != ivisurf->pending.prop.sourceWidth
            || ivisurf->prop.sourceHeight != ivisurf->pending.prop.sourceHeight)) {
        ivisurf->wl_layer_dirty = 1;
    }
    
}

static void
commit_list_surface(struct ivi_layout *layout)
{
    struct ivi_layout_surface *ivisurf = NULL;

    wl_list_for_each(ivisurf, &layout->list_surface, link) {
        check_surface_mask_dirty(ivisurf);

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

            int32_t configured = 0;
            if (ivisurf->prop.destWidth  != ivisurf->pending.prop.destWidth ||
                ivisurf->prop.destHeight != ivisurf->pending.prop.destHeight) {
                configured = 1;
            }

            ivisurf->prop = ivisurf->pending.prop;
            ivisurf->prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
            ivisurf->pending.prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;

            if (configured && !is_surface_transition(ivisurf))
                wl_signal_emit(&ivisurf->configured, ivisurf);
        }
        else{
            int32_t configured = 0;
            if (ivisurf->prop.destWidth  != ivisurf->pending.prop.destWidth ||
                ivisurf->prop.destHeight != ivisurf->pending.prop.destHeight) {
                configured = 1;
            }

            ivisurf->prop = ivisurf->pending.prop;
            ivisurf->prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;
            ivisurf->pending.prop.transitionType = IVI_LAYOUT_TRANSITION_NONE;

            if (configured && !is_surface_transition(ivisurf))
                wl_signal_emit(&ivisurf->configured, ivisurf);
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
              IVI_NOTIFICATION_RENDER_ORDER) ) {
            continue;
        }

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
            wl_list_for_each(ivisurf, &ivilayer->pending.list_surface,
                                  pending.link) {
                if(!wl_list_empty(&ivisurf->order.link)){
                    wl_list_remove(&ivisurf->order.link);
                    wl_list_init(&ivisurf->order.link);
                }

                wl_list_insert(&ivilayer->order.list_surface,
                               &ivisurf->order.link);
                add_ordersurface_to_layer(ivisurf, ivilayer);
                if (ivisurf->event_mask & IVI_NOTIFICATION_REMOVE) {
                    ivisurf->event_mask ^= IVI_NOTIFICATION_REMOVE;
                } else {
                    ivisurf->event_mask |= IVI_NOTIFICATION_ADD;
                }
             ivilayer->event_mask ^= IVI_NOTIFICATION_RENDER_ORDER;
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
        if (iviscrn->event_mask & IVI_NOTIFICATION_RENDER_ORDER) {
            wl_list_for_each_safe(ivilayer, next,
                     &iviscrn->order.list_layer, order.link) {
                remove_orderlayer_from_screen(ivilayer);

                if (!wl_list_empty(&ivilayer->order.link)) {
                    wl_list_remove(&ivilayer->order.link);
                }

                wl_list_init(&ivilayer->order.link);
                ivilayer->event_mask |= IVI_NOTIFICATION_REMOVE;
            }
        
            wl_list_init(&iviscrn->order.list_layer);
            wl_list_for_each(ivilayer, &iviscrn->pending.list_layer,
                                  pending.link) {
                wl_list_insert(&iviscrn->order.list_layer,
                               &ivilayer->order.link);
                add_orderlayer_to_screen(ivilayer, iviscrn);
                if (ivilayer->event_mask & IVI_NOTIFICATION_REMOVE) {
                    ivilayer->event_mask ^= IVI_NOTIFICATION_REMOVE;
                } else {
                    ivilayer->event_mask |= IVI_NOTIFICATION_ADD;
                }
            }
        iviscrn->event_mask ^= IVI_NOTIFICATION_RENDER_ORDER;
        }

        iviscrn->event_mask = 0;

        wl_list_for_each_reverse(ivilayer, &iviscrn->order.list_layer, order.link) {
            wl_list_for_each_reverse(ivisurf, &ivilayer->order.list_surface, order.link) {
                struct weston_view *tmpview = NULL;

                /* Remove weston layer from the compositor's list */
                if (ivisurf->wl_layer.link.next)
                    wl_list_remove(&ivisurf->wl_layer.link);

                /* Clear layer's view list */
                wl_list_init(&ivisurf->wl_layer.view_list.link);

                if (ivisurf->surface == NULL)
                    continue;

                wl_list_for_each(tmpview, &ivisurf->surface->views, surface_link)
                {
                    if (tmpview != NULL) {
                        break;
                    }
                }
                
                if (ivilayer->prop.visibility == 0
                    || ivisurf->prop.visibility == 0
                    || tmpview == NULL)
                    continue;

                weston_layer_entry_insert(&ivisurf->wl_layer.view_list,
                                          &tmpview->layer_link);
                ivisurf->surface->output = iviscrn->output;
                wl_list_insert(layout->compositor->layer_list.prev,
                               &ivisurf->wl_layer.link);
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

static void
remove_configured_listener(struct ivi_layout_surface *ivisurf)
{
	struct wl_listener *link = NULL;
	struct wl_listener *next = NULL;

	wl_list_for_each_safe(link, next, &ivisurf->configured.listener_list, link) {
		wl_list_remove(&link->link);
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
    weston_log("ivi-shell uses %s as a screen.\n", output->name);
    *pWidth  = output->width;
    *pHeight = output->height;
    weston_log("ivi-shell: screen resolution is (%i,%i).\n", *pWidth,*pHeight);

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

    wl_list_remove(&ivisurf->wl_layer.link);

    wl_signal_emit(&layout->surface_notification.removed, ivisurf);
 
    remove_configured_listener(ivisurf);

    ivi_layout_surfaceRemoveNotification(ivisurf);

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

    clear_surface_pending_list(ivilayer);

    if (pSurface == NULL) {
        return 0;
    }

    for (i = 0; i < number; i++) {
        id_surface = &pSurface[i]->id_surface;

        wl_list_for_each_safe(ivisurf, next, &layout->list_surface, link) {
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

    ivilayer->event_mask |= IVI_NOTIFICATION_RENDER_ORDER;

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
    struct ivi_layout *layout = get_instance();
    struct wl_list *seat_list = &layout->compositor->seat_list;
    struct wl_list *surface_list = &layout->list_surface;
    struct ivi_layout_surface *current_surf;

    if (ivisurf == NULL) {
        weston_log("%s: invalid argument\n", __FUNCTION__);
        return -1;
    }

    if (seat_list == NULL) {
        weston_log("%s: seat list is NULL\n", __FUNCTION__);
        return -1;
    }

    if (ivisurf->surface == NULL) {
        weston_log("%s: ivisurf has no surface\n", __FUNCTION__);
        return -1;
    }

    if (surface_list == NULL) {
        weston_log("%s: surface list is NULL\n", __FUNCTION__);
        return -1;
    }

    wl_list_for_each(current_surf, &layout->list_surface, link) {
        if (current_surf == ivisurf) {
            current_surf->prop.hasKeyboardFocus = 1;
            current_surf->pending.prop.hasKeyboardFocus = 1;
        } else {
            current_surf->prop.hasKeyboardFocus = 0;
            current_surf->pending.prop.hasKeyboardFocus = 0;
        }
        current_surf->event_mask |= IVI_NOTIFICATION_KEYBOARD_FOCUS;
    }

    return 0;
}

WL_EXPORT int32_t
ivi_layout_GetKeyboardFocusSurfaceId(struct ivi_layout_surface **pSurfaceId)
{
    struct wl_list *surface_list = &get_instance()->list_surface;
    struct ivi_layout_surface *current_surf;

    if (surface_list == NULL) {
        weston_log("%s: surface list is NULL\n", __FUNCTION__);
        return -1;
    }

    wl_list_for_each(current_surf, surface_list, link) {
        if (current_surf->prop.hasKeyboardFocus != 0) {
            *pSurfaceId = current_surf;
            break;
        }
    }

    return 0;
}

static void get_surface_position(struct weston_view *view, float sx, float sy,
                                 float *x, float *y)
{
    if (view->transform.enabled) {
        struct weston_vector v = { { sx, sy, 0.0f, 1.0f } };
        weston_matrix_transform(&view->transform.matrix, &v);
        if (fabsf(v.f[3]) < 1e-6) {
            weston_log("warning: numerical instability in "
                       "weston_view_from_global(), divisor = %g\n",
                       v.f[3]);
            *x = 0;
            *y = 0;
        } else {
            *x = v.f[0] / v.f[3];
            *y = v.f[1] / v.f[3];
        }
    } else {
        *x = sx + view->geometry.x;
        *y = sy + view->geometry.y;
    }
}

WL_EXPORT int32_t
ivi_layout_SetPointerFocusOn(struct ivi_layout_surface *ivisurf)
{
    struct weston_view *view;
    struct weston_seat *seat;
    uint32_t found_pointer = 0;

    if (ivisurf == NULL) {
        weston_log("%s: invalid argument\n", __FUNCTION__);
        return -1;
    }

    view = ivi_layout_get_weston_view(ivisurf);

    wl_list_for_each(seat, &get_instance()->compositor->seat_list, link) {
        if (seat->pointer) {
            float x,y;
            get_surface_position(view, 0, 0, &x, &y);
            seat->pointer->x = wl_fixed_from_double(x);
            seat->pointer->y = wl_fixed_from_double(y);
            weston_pointer_set_focus(seat->pointer, view,
                                     wl_fixed_from_int(0),
                                     wl_fixed_from_int(0));
            found_pointer = 1;
        }
    }

    if (!found_pointer) {
        weston_log("%s: Could not find a pointer to set focus\n", __FUNCTION__);
        return -1;
    }

    return 0;
}

WL_EXPORT int32_t
ivi_layout_GetPointerFocusSurfaceId(struct ivi_layout_surface **pSurfaceId)
{
    /* Note: This function will only return the focussed surface for the first
     * pointer found. */
    struct weston_seat *seat;
    struct weston_surface *w_surf;
    struct ivi_layout_surface *layout_surf;

    if (pSurfaceId == NULL) {
        weston_log("%s: invalid argument\n", __FUNCTION__);
        return -1;
    }

    /* Find the first seat that has a pointer */
    wl_list_for_each(seat, &get_instance()->compositor->seat_list, link) {
        if (seat != NULL && seat->pointer != NULL)
            break;
    }
    if (seat == NULL) {
        weston_log("%s: Failed to find a seat\n", __FUNCTION__);
        return -1;
    }

    if (seat->pointer->focus == NULL) {
        *pSurfaceId = NULL;
        return 0;
    }

    if (seat->pointer->focus->surface == NULL) {
        weston_log("%s: focus has no surface\n", __FUNCTION__);
        return -1;
    }

    w_surf = seat->pointer->focus->surface;
    /* Find the layout surface that matches that surface */
    wl_list_for_each(layout_surf, &get_instance()->list_surface, link) {
        if (layout_surf->surface == w_surf) {
            *pSurfaceId = layout_surf;
            return 0;
        }
    }

    return -1;
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

    iviscrn->event_mask |= IVI_NOTIFICATION_RENDER_ORDER;

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

        wl_list_for_each_safe(ivilayer, next, &iviscrn->pending.list_layer, pending.link) {
            if (!wl_list_empty(&ivilayer->pending.link)) {
                wl_list_remove(&ivilayer->pending.link);
            }

            wl_list_init(&ivilayer->pending.link);
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

    iviscrn->event_mask |= IVI_NOTIFICATION_RENDER_ORDER;

    return 0;
}

WL_EXPORT struct weston_output *
ivi_layout_screenGetOutput(struct ivi_layout_screen *iviscrn)
{
    return iviscrn->output;
}

WL_EXPORT struct weston_surface *
ivi_layout_surfaceGetWestonSurface(struct ivi_layout_surface *ivisurf)
{
    return ivisurf != NULL ? ivisurf->surface : NULL;
}

static int32_t
surfaceGetBitPerPixel(struct ivi_layout_surface *ivisurf)
{
    int32_t bpp = 0;

    if (ivisurf == NULL) {
        return bpp;
    }

    switch (ivisurf->pixelformat) {
    case IVI_LAYOUT_SURFACE_PIXELFORMAT_R_8:
        bpp = 8;
        break;

    case IVI_LAYOUT_SURFACE_PIXELFORMAT_RGB_888:
        bpp = 24;
        break;

    case IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_8888:
        bpp = 32;
        break;

    case IVI_LAYOUT_SURFACE_PIXELFORMAT_RGB_565:
        bpp = 16;
        break;

    case IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_5551:
        bpp = 16;
        break;

    case IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_6661:
        bpp = 0;  // 19
        break;

    case IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_4444:
        bpp = 16;
        break;

    case IVI_LAYOUT_SURFACE_PIXELFORMAT_UNKNOWN:
    default:
        bpp = 0;
        break;
    }

    return bpp;
}

WL_EXPORT int32_t
ivi_layout_surfaceGetSize(struct ivi_layout_surface *ivisurf, int32_t *width, int32_t *height, int32_t *stride)
{
    if (ivisurf == NULL) {
        return -1;
    }

    if (width != NULL) {
        *width = ivisurf->prop.sourceWidth;
    }

    if (height != NULL) {
        *height = ivisurf->prop.sourceHeight;
    }

    if (stride != NULL) {
        int32_t bpp = surfaceGetBitPerPixel(ivisurf);
        if ((bpp == 0) ||(bpp % 8 != 0)) {
            return -1;
        }

        *stride = ivisurf->prop.sourceWidth * (bpp / 8);
    }

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

    ivilayer->event_mask |= IVI_NOTIFICATION_RENDER_ORDER;

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

    ivilayer->event_mask |= IVI_NOTIFICATION_RENDER_ORDER;

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
    int32_t in_init = 0;
    ivisurf->surface->width_from_buffer  = width;
    ivisurf->surface->height_from_buffer = height;

    if (ivisurf->prop.sourceWidth == 0 || ivisurf->prop.sourceHeight == 0) {
        in_init = 1;
    }

    /* FIXME: when sourceHeight/Width is used as clipping range in image buffer */
    /* if (ivisurf->prop.sourceWidth == 0 || ivisurf->prop.sourceHeight == 0) { */
        ivisurf->pending.prop.sourceWidth = width;
        ivisurf->pending.prop.sourceHeight = height;
        ivisurf->prop.sourceWidth = width;
        ivisurf->prop.sourceHeight = height;
    /* } */

    ivisurf->event_mask |= IVI_NOTIFICATION_CONFIGURE;

    if (in_init) {
        wl_signal_emit(&layout->surface_notification.configure_changed, ivisurf);
    } else {
        ivi_layout_commitChanges();
    }
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
            weston_log("id_surface(%d) is already created\n", id_surface);
            return NULL;
    }

    ivisurf = calloc(1, sizeof *ivisurf);
    if (ivisurf == NULL) {
        weston_log("fails to allocate memory\n");
        return NULL;
    }

    wl_list_init(&ivisurf->link);
    wl_signal_init(&ivisurf->property_changed);
    wl_signal_init(&ivisurf->configured);
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
    weston_matrix_init(&ivisurf->surface_source_pos.matrix);
    weston_matrix_init(&ivisurf->layer_source_pos.matrix);
    weston_matrix_init(&ivisurf->surface_scaling.matrix);
    weston_matrix_init(&ivisurf->layer_scaling.matrix);

    wl_list_init(&ivisurf->surface_rotation.link);
    wl_list_init(&ivisurf->layer_rotation.link);
    wl_list_init(&ivisurf->surface_pos.link);
    wl_list_init(&ivisurf->layer_pos.link);
    wl_list_init(&ivisurf->surface_source_pos.link);
    wl_list_init(&ivisurf->layer_source_pos.link);
    wl_list_init(&ivisurf->surface_scaling.link);
    wl_list_init(&ivisurf->layer_scaling.link);

    init_surfaceProperties(&ivisurf->prop);
    ivisurf->pixelformat = IVI_LAYOUT_SURFACE_PIXELFORMAT_RGBA_8888;
    ivisurf->event_mask = 0;

    ivisurf->pending.prop = ivisurf->prop;
    wl_list_init(&ivisurf->pending.link);

    wl_list_init(&ivisurf->order.link);
    wl_list_init(&ivisurf->order.list_layer);

    weston_layer_init(&ivisurf->wl_layer, NULL);

    wl_list_insert(&layout->list_surface, &ivisurf->link);

    wl_signal_emit(&layout->surface_notification.created, ivisurf);

    return ivisurf;
}

static void
keyboard_grab_key(struct weston_keyboard_grab *grab, uint32_t time,
                  uint32_t key, uint32_t state)
{
    struct ivi_layout *layout = get_instance();
    struct weston_keyboard *keyboard = grab->keyboard;
    struct wl_display *display = keyboard->seat->compositor->wl_display;
    struct ivi_layout_surface *surf;
    struct wl_resource *resource;
    uint32_t serial;

    wl_list_for_each(surf, &layout->list_surface, link) {
        if (surf->prop.hasKeyboardFocus) {
            resource = wl_resource_find_for_client(
                           &keyboard->resource_list,
                           wl_resource_get_client(surf->surface->resource));
            if (!resource)
                resource = wl_resource_find_for_client(
                               &keyboard->focus_resource_list,
                               wl_resource_get_client(surf->surface->resource));

            if (resource) {
                serial = wl_display_next_serial(display);
                wl_keyboard_send_key(resource, serial, time, key, state);
            } else {
                weston_log("%s: No resource found for surface %d\n",
                           __FUNCTION__, surf->id_surface);
            }
        }
    }
}

static void
keyboard_grab_modifiers(struct weston_keyboard_grab *grab, uint32_t serial,
                        uint32_t mods_depressed, uint32_t mods_latched,
                        uint32_t mods_locked, uint32_t group)
{
    struct ivi_layout *layout = get_instance();
    struct weston_keyboard *keyboard = grab->keyboard;
    struct ivi_layout_surface *surf;
    struct wl_resource *resource;
    int sent_to_pointer_client = 0;
    struct weston_pointer *pointer = keyboard->seat->pointer;

    /* Send modifiers to focussed surface */
    wl_list_for_each(surf, &layout->list_surface, link) {
        if (surf->prop.hasKeyboardFocus) {
            resource = wl_resource_find_for_client(
                           &keyboard->resource_list,
                           wl_resource_get_client(surf->surface->resource));
            if (!resource)
                resource = wl_resource_find_for_client(
                               &keyboard->focus_resource_list,
                               wl_resource_get_client(surf->surface->resource));

            if (resource) {
                wl_keyboard_send_modifiers(resource, serial,
                                           mods_depressed,
                                           mods_latched, mods_locked,
                                           group);
                if (pointer && pointer->focus
                    && pointer->focus->surface->resource
                    && pointer->focus->surface == surf->surface)
                    sent_to_pointer_client = 1;
            } else {
                 weston_log("%s: No resource found for surface %d\n",
                            __FUNCTION__, surf->id_surface);
            }
        }
    }

    /* Send modifiers to pointer's client, if not already sent */
    if (!sent_to_pointer_client && pointer && pointer->focus
        && pointer->focus->surface->resource) {
        struct wl_client *pointer_client =
            wl_resource_get_client(pointer->focus->surface->resource);
        wl_resource_for_each(resource, &keyboard->resource_list) {
            if (wl_resource_get_client(resource) == pointer_client) {
                sent_to_pointer_client = 1;
                wl_keyboard_send_modifiers(resource, serial, mods_depressed,
                                           mods_latched, mods_locked, group);
                break;
            }
        }

        if (!sent_to_pointer_client) {
            wl_resource_for_each(resource, &keyboard->focus_resource_list) {
                wl_keyboard_send_modifiers(resource, serial, mods_depressed,
                                           mods_latched, mods_locked, group);
                break;
            }
        }
    }
}

static void
keyboard_grab_cancel(struct weston_keyboard_grab *grab)
{
}

static struct weston_keyboard_grab_interface keyboard_grab_interface = {
    keyboard_grab_key,
    keyboard_grab_modifiers,
    keyboard_grab_cancel
};

static void
handle_seat_updated_caps(struct wl_listener *listener, void *data)
{
    struct weston_seat *seat = data;
    struct seat_ctx *ctx = wl_container_of(listener, ctx,
                                           updated_caps_listener);
    if (seat->keyboard && seat->keyboard != ctx->grab.keyboard)
        weston_keyboard_start_grab(seat->keyboard, &ctx->grab);
}

static void
handle_seat_destroy(struct wl_listener *listener, void *data)
{
    struct seat_ctx *ctx = wl_container_of(listener, ctx, destroy_listener);
    if (ctx->grab.keyboard)
        keyboard_grab_cancel(&ctx->grab);

    free(ctx);
}

static void
handle_seat_create(struct wl_listener *listener, void *data)
{
    struct weston_seat *seat = data;

    struct seat_ctx *ctx = calloc(1, sizeof *ctx);
    if (ctx == NULL) {
        weston_log("%s: failed to allocate memory\n", __FUNCTION__);
        return;
    }

    ctx->grab.interface = &keyboard_grab_interface;

    ctx->destroy_listener.notify = &handle_seat_destroy;
    wl_signal_add(&seat->destroy_signal, &ctx->destroy_listener);

    ctx->updated_caps_listener.notify = &handle_seat_updated_caps;
    wl_signal_add(&seat->updated_caps_signal, &ctx->updated_caps_listener);
}

static void
handle_pointer_focus(struct wl_listener *listener, void *data)
{
    struct weston_pointer *pointer = data;
    struct ivi_layout_surface *layout_surf = NULL;
    if (pointer->focus) {
        wl_list_for_each(layout_surf, &get_instance()->list_surface, link) {
            if (layout_surf->surface == pointer->focus->surface) {
                layout_surf->prop.hasPointerFocus = 1;
                layout_surf->pending.prop.hasPointerFocus = 1;
            } else {
                layout_surf->prop.hasPointerFocus = 0;
                layout_surf->pending.prop.hasPointerFocus = 0;
            }
            layout_surf->event_mask |= IVI_NOTIFICATION_POINTER_FOCUS;
            wl_signal_emit(&layout_surf->property_changed, layout_surf);
	    layout_surf->event_mask = 0;
        }
    }
}

static void
setup_focus_listener(struct wl_listener *listener, void *data)
{
    struct weston_seat *seat = data;
    if (seat->pointer_device_count > 0 && seat->pointer) {
        add_notification(&seat->pointer->focus_signal, handle_pointer_focus, NULL);
    }
}

static void
setup_pointer_listeners(void)
{
    struct weston_seat *seat;
    wl_list_for_each(seat, &get_instance()->compositor->seat_list, link) {

        /* hook into any updated caps signals */
        add_notification(&seat->updated_caps_signal, setup_focus_listener, NULL);

        /* Iterate over all seats, and set up pointer focus listeners for every seat's pointer */
        if (seat->pointer_device_count > 0) {
            add_notification(&seat->pointer->focus_signal, handle_pointer_focus, NULL);
        }
    }
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

    /* Add a pointer created listener that adds pointer focus listeners */
    setup_pointer_listeners();

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

    /* Listen to seat creation, for grab purposes */
    layout->seat_create_listener.notify = &handle_seat_create;
    wl_signal_add(&ec->seat_created_signal, &layout->seat_create_listener);

    /* Handle existing seats */
    struct weston_seat *seat;
    wl_list_for_each(seat, &ec->seat_list, link) {
        handle_seat_create(NULL, seat);
        wl_signal_emit(&seat->updated_caps_signal, seat);
    }
}

static struct wl_resource *
find_resource_for_surface(struct wl_list *list, struct weston_surface *surface)
{
	if (!surface)
		return NULL;

	if (!surface->resource)
		return NULL;

	return wl_resource_find_for_client(list, wl_resource_get_client(surface->resource));
}

static void
ivi_layout_grabKeyboardKey(struct weston_keyboard_grab *grab,
                uint32_t time, uint32_t key, uint32_t state)
{
    struct weston_keyboard *keyboard = grab->keyboard;
    struct wl_display *display = keyboard->seat->compositor->wl_display;
    uint32_t serial;
    struct wl_resource *resource;

    wl_resource_for_each(resource, &keyboard->focus_resource_list) {
        serial = wl_display_next_serial(display);
        wl_keyboard_send_key(resource,
                             serial,
                             time,
                             key,
                             state);
    }

    wl_resource_for_each(resource, &keyboard->resource_list) {
        serial = wl_display_next_serial(display);
        wl_keyboard_send_key(resource,
                             serial,
                             time,
                             key,
                             state);
    }
}

static void
ivi_layout_surfaceAddConfiguredListener(struct ivi_layout_surface* ivisurf,
                                       struct wl_listener* listener)
{
    wl_signal_add(&ivisurf->configured, listener);
}

WL_EXPORT struct ivi_layout_interface ivi_layout_interface = {
	.get_weston_view = ivi_layout_get_weston_view,
	.surfaceConfigure = ivi_layout_surfaceConfigure,
	.surfaceCreate = ivi_layout_surfaceCreate,
	.initWithCompositor = ivi_layout_initWithCompositor,
	.emitWarningSignal = ivi_layout_emitWarningSignal,
        .grab_keyboard_key = ivi_layout_grabKeyboardKey,
        .get_surface_dimension = ivi_layout_surfaceGetDimension,
        .add_surface_configured_listener = ivi_layout_surfaceAddConfiguredListener,
};
