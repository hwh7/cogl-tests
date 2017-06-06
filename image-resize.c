/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Many codes are copy-and-pasted from Mutter and Cogl.
 *
 */

#include <stdlib.h>
#include <cogl/cogl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

typedef enum {
  META_TEXTURE_FLAGS_NONE = 0,
  META_TEXTURE_ALLOW_SLICING = 1 << 1
} MetaTextureFlags;

typedef struct _Data
{
  CoglContext *ctx;
  CoglMatrix view;
  CoglFramebuffer *fb;
  CoglPipeline *pipeline;
  CoglPrimitive *prim;
  CoglIndices *indices;

  int filter_name_label_width;
  int filter_name_label_height;

  unsigned int redraw_idle;
  CoglBool is_dirty;
  CoglBool draw_ready;

  float window_width, window_height;
  int width, height;
} Data;

static CoglColor white;

static gboolean is_pot(int x)
{
  return x > 0 && (x & (x - 1)) == 0;
}

static CoglTexture *
meta_create_texture (CoglContext          *ctx,
    int                   width,
    int                   height,
    CoglTextureComponents components,
    MetaTextureFlags      flags)
{
  CoglTexture *texture;

  gboolean should_use_rectangle = FALSE;

  if (!(is_pot (width) && is_pot (height)) &&
      !cogl_has_feature (ctx, COGL_FEATURE_ID_TEXTURE_NPOT))
  {
    if (cogl_has_feature (ctx, COGL_FEATURE_ID_TEXTURE_RECTANGLE))
      should_use_rectangle = TRUE;
    else
      g_error ("Cannot create texture. Support for GL_ARB_texture_non_power_of_two or "
          "ARB_texture_rectangle is required");
  }

  if (should_use_rectangle)
    texture = COGL_TEXTURE (cogl_texture_rectangle_new_with_size (ctx, width, height));
  else
    texture = COGL_TEXTURE (cogl_texture_2d_new_with_size (ctx, width, height));
  cogl_texture_set_components (texture, components);

  if ((flags & META_TEXTURE_ALLOW_SLICING) != 0)
  {
    /* To find out if we need to slice the texture, we have to go ahead and force storage
     * to be allocated
     */
    CoglError *catch_error = NULL;
    if (!cogl_texture_allocate (texture, &catch_error))
    {
      cogl_error_free (catch_error);
      cogl_object_unref (texture);
      texture = COGL_TEXTURE (cogl_texture_2d_sliced_new_with_size (ctx, width, height, COGL_TEXTURE_MAX_WASTE));
      cogl_texture_set_components (texture, components);
    }
  }

  return texture;
}

static GdkPixbuf *
load_file (const char *path)
{
  GError *error = NULL;
  GdkPixbuf *pixbuf;
  GFileInputStream *stream;

  stream = g_file_read (g_file_new_for_path (path), NULL, &error);
  if (stream == NULL)
    return NULL;

  pixbuf = gdk_pixbuf_new_from_stream (G_INPUT_STREAM (stream), NULL, &error);
  g_object_unref (stream);

  if (pixbuf == NULL)
    return NULL;

  return pixbuf;
}

static gboolean
paint_cb (void *user_data)
{
  Data *data = user_data;

  data->redraw_idle = 0;
  data->is_dirty = FALSE;
  data->draw_ready = FALSE;

  cogl_framebuffer_clear4f (data->fb, COGL_BUFFER_BIT_COLOR, 0, 0, 0, 1);

  cogl_primitive_draw (data->prim, data->fb, data->pipeline);

  cogl_onscreen_swap_buffers (data->fb);

  return G_SOURCE_REMOVE;
}

static void
maybe_redraw (Data *data)
{
  if (data->is_dirty && data->draw_ready && data->redraw_idle == 0) {
    /* We'll draw on idle instead of drawing immediately so that
     * if Cogl reports multiple dirty rectangles we won't
     * redundantly draw multiple frames */
    data->redraw_idle = g_idle_add (paint_cb, data);
  }
}

static void
frame_event_cb (CoglOnscreen *onscreen,
    CoglFrameEvent event,
    CoglFrameInfo *info,
    void *user_data)
{
  Data *data = user_data;

  if (event == COGL_FRAME_EVENT_SYNC) {
    data->draw_ready = TRUE;
    maybe_redraw (data);
  }
}

static void
dirty_cb (CoglOnscreen *onscreen,
    const CoglOnscreenDirtyInfo *info,
    void *user_data)
{
  Data *data = user_data;

  data->is_dirty = TRUE;
  maybe_redraw (data);
}


static CoglPipelineFilter
get_filter_by_idx(int idx)
{
  CoglPipelineFilter filters[] = {
    COGL_PIPELINE_FILTER_NEAREST,
    COGL_PIPELINE_FILTER_LINEAR,
    COGL_PIPELINE_FILTER_NEAREST_MIPMAP_NEAREST,
    COGL_PIPELINE_FILTER_LINEAR_MIPMAP_NEAREST,
    COGL_PIPELINE_FILTER_NEAREST_MIPMAP_LINEAR,
    COGL_PIPELINE_FILTER_LINEAR_MIPMAP_LINEAR
  };

  return filters[idx];
}

static void print_usage(char *exec)
{
  printf ("%s [IMAGE_FILE_PATH] [FILTER_INDEX]\n", exec);
  printf ("FILTER INDEX\n");
  printf ("0: COGL_PIPELINE_FILTER_NEAREST\n");
  printf ("1: COGL_PIPELINE_FILTER_LINEAR\n");
  printf ("2: COGL_PIPELINE_FILTER_NEAREST_MIPMAP_NEAREST\n");
  printf ("3: COGL_PIPELINE_FILTER_LINEAR_MIPMAP_NEAREST\n");
  printf ("4: COGL_PIPELINE_FILTER_NEAREST_MIPMAP_LINEAR\n");
  printf ("5: COGL_PIPELINE_FILTER_LINEAR_MIPMAP_LINEAR\n");
}

int
main (int argc, char **argv)
{
  Data data;

  GdkPixbuf *pixbuf;
  int row_stride;
  gboolean has_alpha;
  guchar *pixels;

  CoglContext *ctx;
  CoglTexture* texture;
  CoglError *error = NULL;
  CoglPipeline *pipeline;
  CoglOnscreen *onscreen;

  GSource *cogl_source;
  GMainLoop *loop;

  CoglVertexP2T2 plane[] =
  {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    { -1.0f, 1.0f, 0.0f, 0.0f },
    { 1.0f, 1.0f, 1.0f, 0.0f },
    { 1.0f, -1.0f, 1.0f, 1.0f },
  };

  if (argc < 3) {
    print_usage (argv[0]);
    return 0;
  }

  data.window_width = 1920;
  data.window_height = 1080;
  data.redraw_idle = 0;

  cogl_color_init_from_4ub (&white, 0xff, 0xff, 0xff, 0xff);

  pixbuf = load_file (argv[1]);

  data.width = gdk_pixbuf_get_width (pixbuf);
  data.height = gdk_pixbuf_get_height (pixbuf);
  has_alpha = gdk_pixbuf_get_has_alpha (pixbuf);
  row_stride = gdk_pixbuf_get_rowstride (pixbuf);
  pixels = gdk_pixbuf_get_pixels (pixbuf);

  printf ("Picture's width: %d, height: %d, has_alpha: %d, row_stride: %d, pixels: %p\n",
      data.width, data.height, has_alpha, row_stride, pixels);

  ctx = cogl_context_new (NULL, &error);
  if (!ctx) {
    fprintf (stderr, "Failed to create context: %s\n", error->message);
    return 1;
  }

  texture = meta_create_texture (
      ctx, data.width, data.height,
      has_alpha ? COGL_TEXTURE_COMPONENTS_RGBA : COGL_TEXTURE_COMPONENTS_RGB,
      META_TEXTURE_ALLOW_SLICING);

  if (!cogl_texture_set_data (texture,
        has_alpha ? COGL_PIXEL_FORMAT_RGBA_8888 : COGL_PIXEL_FORMAT_RGB_888,
        row_stride,
        pixels,
        0,
        &error))
  {
    g_error ("Failed to create texture for background");
    cogl_error_free (error);
    cogl_object_unref (texture);
    return -1;
  }

  onscreen = cogl_onscreen_new (ctx, (int)data.window_width, (int)data.window_height);
  cogl_onscreen_show (onscreen);
  data.fb = onscreen;


  printf ("data.window_width: %d, data.window_height: %d\n",
      (int)data.window_width, (int)data.window_height);

  data.prim = cogl_primitive_new_p2t2 (ctx, COGL_VERTICES_MODE_TRIANGLES, G_N_ELEMENTS (plane), plane);
  data.indices = cogl_get_rectangle_indices (ctx, 1);
  cogl_primitive_set_indices (data.prim, data.indices, 6);

  pipeline = cogl_pipeline_new (ctx);

  cogl_pipeline_set_layer_texture (pipeline, 0, texture);
  cogl_pipeline_set_layer_wrap_mode (pipeline, 0, COGL_PIPELINE_WRAP_MODE_CLAMP_TO_EDGE);
  cogl_pipeline_set_layer_filters (pipeline, 0,
      get_filter_by_idx(atoi(argv[2])), COGL_PIPELINE_FILTER_NEAREST);

  data.pipeline = pipeline;

  cogl_onscreen_add_frame_callback (onscreen,
      frame_event_cb,
      &data,
      NULL); /* destroy notify */

  cogl_onscreen_add_dirty_callback (onscreen,
      dirty_cb,
      &data,
      NULL); /* destroy notify */

  cogl_source = cogl_glib_source_new (ctx, G_PRIORITY_DEFAULT);
  g_source_attach (cogl_source, NULL);

  loop = g_main_loop_new (NULL, TRUE);
  g_main_loop_run (loop);

  return 0;
}
