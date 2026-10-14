#pragma once

/*
 * libimage: loading pictures, all decoded by our own code.
 *
 *   PNG    all of it: every colour type, bit depths 1 to 16, transparency,
 *          interlacing (and our own zlib/DEFLATE decoder)
 *   JPEG   baseline and progressive, any chroma subsampling, grey, YCbCr,
 *          CMYK; turned upright from the camera's EXIF orientation
 *   WebP   lossy (VP8), lossless (VP8L), with alpha; the first frame of
 *          an animation
 *   GIF    the first frame, with transparency
 *   BMP    1/4/8-bit palette, 16/24/32-bit, RLE4/RLE8, bit fields
 *   TIFF   uncompressed, PackBits, LZW, Deflate; grey, RGB(A), palette,
 *          CMYK, 1-16 bits, strips or tiles, the first page
 *
 * Every decoder hands back premultiplied ARGB (struct gfx_surface).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gfx.h>

/* Any supported format, recognised by its contents. NULL on failure, with a reason in *error. */
struct gfx_surface *image_load(const char *path, const char **error);
struct gfx_surface *image_decode(const uint8_t *data, size_t size, const char **error);

/* Read a whole file (malloc'd). NULL on failure, with a reason in *error. */
uint8_t *image_read_file(const char *path, size_t *size, const char **error);

/* "PNG", "JPEG", "WebP", "GIF", "BMP", "TIFF" for data in that format, or NULL. */
const char *image_format(const uint8_t *data, size_t size);
/* Does the file name end in an extension we can show (.png, .jpg, .jpeg, .webp, ...)? */
bool image_supported_name(const char *name);

/* Save a picture as a PNG (RGB if it's all opaque, else RGBA). 0 or -errno. Encoded by
   stb_image_write (user/libs/image/vendor). */
int image_save_png(const char *path, const struct gfx_surface *s);

/* One format at a time. */
struct gfx_surface *image_load_png(const char *path, const char **error);
struct gfx_surface *image_decode_png(const uint8_t *data, size_t size, const char **error);
struct gfx_surface *image_decode_jpeg(const uint8_t *data, size_t size, const char **error);
struct gfx_surface *image_decode_webp(const uint8_t *data, size_t size, const char **error);
struct gfx_surface *image_decode_gif(const uint8_t *data, size_t size, const char **error);
struct gfx_surface *image_decode_bmp(const uint8_t *data, size_t size, const char **error);
struct gfx_surface *image_decode_tiff(const uint8_t *data, size_t size, const char **error);

/*
 * DEFLATE inside a zlib wrapper (RFC 1950/1951). Returns the number of
 * bytes written to out, or -1 if the data is broken or doesn't fit.
 */
long zlib_decompress(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size);
