/* rsx_image_io -- PNG load/save for texture packs (see rsx_image_io.c). */
#ifndef PS3RECOMP_RSX_IMAGE_IO_H
#define PS3RECOMP_RSX_IMAGE_IO_H

#ifdef __cplusplus
extern "C" {
#endif

/* A PNG as tightly packed straight R,G,B,A rows (malloc'd; the caller frees
 * them). 1 = loaded. */
int rsx_image_load_png(const char* path, unsigned char** rgba, unsigned* w, unsigned* h);
/* The same from PNG bytes in memory. */
int rsx_image_load_png_mem(const unsigned char* png, unsigned size, unsigned char** rgba,
                           unsigned* w, unsigned* h);
/* R,G,B,A rows (row_bytes apart) as a PNG. 1 = written. */
int rsx_image_save_png(const char* path, const unsigned char* rgba, unsigned w, unsigned h,
                       unsigned row_bytes);

#ifdef __cplusplus
}
#endif

#endif
