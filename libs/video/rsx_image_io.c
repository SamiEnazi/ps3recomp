/*
 * rsx_image_io -- PNG files for texture packs (rsx_draw_engine.c, PS3_MODS /
 * PS3_MODS_DUMP) on the backends that have no image API of their own (the
 * Vulkan backend; Metal uses ImageIO).
 *
 * stb_image 2.30 / stb_image_write 1.16 (libs/codec, public domain or MIT),
 * compiled here as static functions so they cannot collide with the copies
 * cellPngDec.c / cellPngEnc.c build for the game's own PNG HLE.
 *
 * Load: any PNG (8/16-bit, grey, palette, alpha) as tightly packed straight
 * R,G,B,A bytes, no colour management -- what the dump writes reads back byte
 * for byte. Save: such rows as a PNG, synchronously (a dump is a tool run).
 */
#include "rsx_image_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../codec/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../codec/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

int rsx_image_load_png(const char* path, unsigned char** rgba, unsigned* w, unsigned* h)
{
    *rgba = NULL; *w = *h = 0;
    if (!path) return 0;
    int x = 0, y = 0, n = 0;
    stbi_uc* px = stbi_load(path, &x, &y, &n, 4);
    if (!px || x <= 0 || y <= 0 || x > 16384 || y > 16384) { if (px) stbi_image_free(px); return 0; }
    /* stb allocates with malloc (STBI_MALLOC default): the caller's free() matches. */
    *rgba = px; *w = (unsigned)x; *h = (unsigned)y;
    return 1;
}

/* The same from PNG bytes in memory (the save-data list's icons, vk_overlay_saves.cpp). */
int rsx_image_load_png_mem(const unsigned char* png, unsigned size, unsigned char** rgba,
                           unsigned* w, unsigned* h)
{
    *rgba = NULL; *w = *h = 0;
    if (!png || size < 8 || size > 0x7FFFFFFFu) return 0;
    int x = 0, y = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(png, (int)size, &x, &y, &n, 4);
    if (!px || x <= 0 || y <= 0 || x > 16384 || y > 16384) { if (px) stbi_image_free(px); return 0; }
    *rgba = px; *w = (unsigned)x; *h = (unsigned)y;
    return 1;
}

int rsx_image_save_png(const char* path, const unsigned char* rgba, unsigned w, unsigned h,
                       unsigned row_bytes)
{
    if (!path || !rgba || !w || !h) return 0;
    stbi_write_png_compression_level = 4;   /* fast: a dump writes hundreds */
    if (!stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)row_bytes)) {
        fprintf(stderr, "[mods] cannot write %s\n", path);
        return 0;
    }
    return 1;
}
