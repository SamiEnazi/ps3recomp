/*
 * mod_preview -- a game mod's preview picture for the menu's Mods page
 * (vk_overlay.cpp; the path comes from ps3_game_mods_image, which keeps it
 * inside the mod's folder).
 *
 * The file (at most 8 MB, a PNG or a JPEG by its first bytes, whatever its
 * name says) is decoded with stb_image -- libs/codec/cellPngDec.c builds it
 * with PNG and JPEG -- after its size is read from the header: at most
 * 16384 x 16384 and 64 M pixels, so a small file cannot unpack into gigabytes.
 * Then it is shrunk to at most max_side pixels on its longer side (a box
 * filter, alpha-weighted so transparent pixels do not darken the edges).
 * Runs on the overlay's picture thread, never the render thread.
 */
#ifndef PS3RECOMP_MOD_PREVIEW_H
#define PS3RECOMP_MOD_PREVIEW_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" {
unsigned char* stbi_load_from_memory(unsigned char const* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels);
int stbi_info_from_memory(unsigned char const* buffer, int len, int* x, int* y, int* comp);
void stbi_image_free(void* retval_from_stbi_load);
}

static const long kModPreviewMaxFile = 8L << 20;

/* 1 = rgba holds w x h straight R,G,B,A rows (w, h <= max_side). */
static int mod_preview_load(const char* path, int max_side, std::vector<unsigned char>& rgba, int& w, int& h, std::string* why)
{
    rgba.clear(); w = h = 0;
    auto no = [&](const char* s) { if (why) *why = s; return 0; };
    FILE* f = path ? fopen(path, "rb") : NULL;
    if (!f) return no("it cannot be opened");
    std::vector<unsigned char> b;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n > 0 && n <= kModPreviewMaxFile && fseek(f, 0, SEEK_SET) == 0) {
        b.resize((size_t)n);
        if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    }
    fclose(f);
    if (n > kModPreviewMaxFile) return no("it is bigger than 8 MB");
    if (b.size() < 16) return no("it cannot be read");
    static const unsigned char kPng[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const bool png = !memcmp(b.data(), kPng, 8), jpg = b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
    if (!png && !jpg) return no("it is not a PNG or JPEG picture");
    int x = 0, y = 0, c = 0;
    if (!stbi_info_from_memory(b.data(), (int)b.size(), &x, &y, &c)) return no("its header cannot be read");
    if (x <= 0 || y <= 0 || x > 16384 || y > 16384 || (int64_t)x * y > (64ll << 20)) return no("the picture is too big");
    unsigned char* px = stbi_load_from_memory(b.data(), (int)b.size(), &x, &y, &c, 4);
    if (!px) return no("it does not decode");
    if (max_side < 1) max_side = 1;
    const int big = x > y ? x : y;
    int nw = x, nh = y;
    if (big > max_side) {
        nw = (int)((int64_t)x * max_side / big); nh = (int)((int64_t)y * max_side / big);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
    }
    rgba.resize((size_t)nw * nh * 4);
    for (int dy = 0; dy < nh; dy++) {
        const int y0 = (int)((int64_t)dy * y / nh);
        int y1 = (int)((int64_t)(dy + 1) * y / nh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < nw; dx++) {
            const int x0 = (int)((int64_t)dx * x / nw);
            int x1 = (int)((int64_t)(dx + 1) * x / nw);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t r = 0, g = 0, bl = 0, a = 0, cnt = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const unsigned char* p = px + ((size_t)sy * x + sx) * 4;
                    r += (uint64_t)p[0] * p[3]; g += (uint64_t)p[1] * p[3]; bl += (uint64_t)p[2] * p[3]; a += p[3]; cnt++;
                }
            unsigned char* o = &rgba[((size_t)dy * nw + dx) * 4];
            o[0] = a ? (unsigned char)(r / a) : 0;
            o[1] = a ? (unsigned char)(g / a) : 0;
            o[2] = a ? (unsigned char)(bl / a) : 0;
            o[3] = (unsigned char)(a / cnt);
        }
    }
    stbi_image_free(px);
    w = nw; h = nh;
    return 1;
}

#endif
