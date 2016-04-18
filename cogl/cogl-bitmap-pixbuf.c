/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2007,2008,2009 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-util.h"
#include "cogl-bitmap-private.h"
#include "cogl-context-private.h"
#include "cogl-private.h"
#include "cogl-error-private.h"

#include <string.h>

#if defined(USE_GDKPIXBUF)
#include <gdk-pixbuf/gdk-pixbuf.h>

CoglBool
_cogl_bitmap_get_size_from_file (const char *filename,
                                 int        *width,
                                 int        *height)
{
  _COGL_RETURN_VAL_IF_FAIL (filename != NULL, FALSE);

  if (gdk_pixbuf_get_file_info (filename, width, height) != NULL)
    return TRUE;

  return FALSE;
}

CoglBitmap *
_cogl_bitmap_from_file (CoglContext *ctx,
                        const char *filename,
			CoglError **error)
{
  static CoglUserDataKey pixbuf_key;
  GdkPixbuf *pixbuf;
  CoglBool has_alpha;
  GdkColorspace color_space;
  CoglPixelFormat pixel_format;
  int width;
  int height;
  int rowstride;
  int bits_per_sample;
  int n_channels;
  CoglBitmap *bmp;
  GError *glib_error = NULL;

  /* Load from file using GdkPixbuf */
  pixbuf = gdk_pixbuf_new_from_file (filename, &glib_error);
  if (pixbuf == NULL)
    {
      _cogl_propagate_gerror (error, glib_error);
      return FALSE;
    }

  /* Get pixbuf properties */
  has_alpha       = gdk_pixbuf_get_has_alpha (pixbuf);
  color_space     = gdk_pixbuf_get_colorspace (pixbuf);
  width           = gdk_pixbuf_get_width (pixbuf);
  height          = gdk_pixbuf_get_height (pixbuf);
  rowstride       = gdk_pixbuf_get_rowstride (pixbuf);
  bits_per_sample = gdk_pixbuf_get_bits_per_sample (pixbuf);
  n_channels      = gdk_pixbuf_get_n_channels (pixbuf);

  /* According to current docs this should be true and so
   * the translation to cogl pixel format below valid */
  g_assert (bits_per_sample == 8);

  if (has_alpha)
    g_assert (n_channels == 4);
  else
    g_assert (n_channels == 3);

  /* Translate to cogl pixel format */
  switch (color_space)
    {
    case GDK_COLORSPACE_RGB:
      /* The only format supported by GdkPixbuf so far */
      pixel_format = has_alpha ?
	COGL_PIXEL_FORMAT_RGBA_8888 :
	COGL_PIXEL_FORMAT_RGB_888;
      break;

    default:
      /* Ouch, spec changed! */
      g_object_unref (pixbuf);
      return FALSE;
    }

  /* We just use the data directly from the pixbuf so that we don't
     have to copy to a seperate buffer. Note that Cogl is expected not
     to read past the end of bpp*width on the last row even if the
     rowstride is much larger so we don't need to worry about
     GdkPixbuf's semantics that it may under-allocate the buffer. */
  bmp = cogl_bitmap_new_for_data (ctx,
                                  width,
                                  height,
                                  pixel_format,
                                  rowstride,
                                  gdk_pixbuf_get_pixels (pixbuf));

  cogl_object_set_user_data (COGL_OBJECT (bmp),
                             &pixbuf_key,
                             pixbuf,
                             g_object_unref);

  return bmp;
}

#else

#include "stb_image.c"

CoglBool
_cogl_bitmap_get_size_from_file (const char *filename,
                                 int        *width,
                                 int        *height)
{
  if (width)
    *width = 0;

  if (height)
    *height = 0;

  return TRUE;
}

/* stb_image.c supports an STBI_grey_alpha format which we don't have
 * a corresponding CoglPixelFormat for so as a special case we
 * convert this to rgba8888.
 *
 * If we have a use case where this is an important format to consider
 * then it could be worth adding a corresponding CoglPixelFormat
 * instead.
 */
static uint8_t *
convert_ra_88_to_rgba_8888 (uint8_t *pixels,
                            int width,
                            int height)
{
  int x, y;
  uint8_t *buf;
  size_t in_stride = width * 2;
  size_t out_stride = width * 4;

  buf = malloc (width * height * 4);
  if (buf)
    return NULL;

  for (y = 0; y < height; y++)
    for (x = 0; x < width; x++)
      {
        uint8_t *src = pixels + in_stride * y + 2 * x;
        uint8_t *dst = buf + out_stride * y + 4 * x;

        dst[0] = src[0];
        dst[1] = src[0];
        dst[2] = src[0];
        dst[3] = src[1];
      }

  return buf;
}

static CoglBitmap *
_cogl_bitmap_new_from_stb_pixels (CoglContext *ctx,
                                  uint8_t *pixels,
                                  int stb_pixel_format,
                                  int width,
                                  int height,
                                  CoglError **error)
{
  static CoglUserDataKey bitmap_data_key;
  CoglBitmap *bmp;
  CoglPixelFormat cogl_format;
  size_t stride;

  if (pixels == NULL)
    {
      _cogl_set_error_literal (error,
                               COGL_BITMAP_ERROR,
                               COGL_BITMAP_ERROR_FAILED,
                               "Failed to load image with stb image library");
      return NULL;
    }

  switch (stb_pixel_format)
    {
    case STBI_grey:
      cogl_format = COGL_PIXEL_FORMAT_A_8;
      break;
    case STBI_grey_alpha:
      {
        uint8_t *tmp = pixels;

        pixels = convert_ra_88_to_rgba_8888 (pixels, width, height);
        free (tmp);

        if (!pixels)
          {
            _cogl_set_error_literal (error,
                                     COGL_BITMAP_ERROR,
                                     COGL_BITMAP_ERROR_FAILED,
                                     "Failed to alloc memory to convert "
                                     "gray_alpha to rgba8888");
            return NULL;
          }

        cogl_format = COGL_PIXEL_FORMAT_RGBA_8888;
        break;
      }
    case STBI_rgb:
      cogl_format = COGL_PIXEL_FORMAT_RGB_888;
      break;
    case STBI_rgb_alpha:
      cogl_format = COGL_PIXEL_FORMAT_RGBA_8888;
      break;

    default:
      g_warn_if_reached ();
      return NULL;
    }

  stride = width * _cogl_pixel_format_get_bytes_per_pixel (cogl_format);

  /* Store bitmap info */
  bmp = cogl_bitmap_new_for_data (ctx,
                                  width, height,
                                  cogl_format,
                                  stride,
                                  pixels);

  /* Register a destroy function so the pixel data will be freed
     automatically when the bitmap object is destroyed */
  cogl_object_set_user_data (COGL_OBJECT (bmp), &bitmap_data_key, pixels, free);

  return bmp;
}

CoglBitmap *
_cogl_bitmap_from_file (CoglContext *ctx,
                        const char *filename,
			CoglError **error)
{
  int stb_pixel_format;
  int width;
  int height;
  uint8_t *pixels;

  pixels = stbi_load (filename,
                      &width, &height, &stb_pixel_format,
                      STBI_default);

  return _cogl_bitmap_new_from_stb_pixels (ctx, pixels, stb_pixel_format,
                                           width, height,
                                           error);
}

#endif
