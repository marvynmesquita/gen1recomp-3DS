#include "texture_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static const unsigned coordsTable[0x40] = {
    0,  1,  4,  5,  16, 17, 20, 21,
    2,  3,  6,  7,  18, 19, 22, 23,
    8,  9,  12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
};

static unsigned indexOfTile(unsigned tex_w, unsigned tex_h, unsigned x, unsigned y) {
    unsigned tileX = x / 8;
    unsigned tileY = y / 8;
    unsigned subX  = x % 8;
    unsigned subY  = y % 8;
    return ((tex_w / 8) * tileY + tileX) * 64 + coordsTable[subY * 8 + subX];
}

static inline int next_pow2(int v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return ++v;
}

// GR1T: the raw RGBA8 texture written by the runtime ROM importer's
// love.image (source/love_image.cpp).  Header = "GR1T" + u32 LE width +
// u32 LE height + raw RGBA8 (row 0 = top, pixel = R,G,B,A).  No PNG encoder
// exists on the 3DS build, so imported assets are stored this way and
// detected by magic bytes rather than by extension.
static bool try_load_gr1t(const unsigned char* buf, size_t size,
                          int* out_w, int* out_h, const uint8_t** pixels) {
    if (size < 12) return false;
    if (memcmp(buf, "GR1T", 4) != 0) return false;
    unsigned w = (unsigned)buf[4] | ((unsigned)buf[5] << 8)
               | ((unsigned)buf[6] << 16) | ((unsigned)buf[7] << 24);
    unsigned h = (unsigned)buf[8] | ((unsigned)buf[9] << 8)
               | ((unsigned)buf[10] << 16) | ((unsigned)buf[11] << 24);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return false;
    if (12 + (size_t)w * h * 4 > size) return false;
    *out_w = (int)w;
    *out_h = (int)h;
    *pixels = buf + 12;
    return true;
}

int texture_load_to_tex(const char* path, C3D_Tex* out_tex, int* out_w, int* out_h) {
    int w, h, channels;
    bool is_gr1t = false;

    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("Failed to open: %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(size ? size : 1);
    if (!buf) { fclose(f); return -1; }
    if (size) fread(buf, 1, size, f);
    fclose(f);

    // Raw importer texture?
    const uint8_t* gr1tPixels = NULL;
    if (try_load_gr1t(buf, size, &w, &h, &gr1tPixels)) {
        is_gr1t = true;
    }

    uint8_t* pixels = NULL;
    if (is_gr1t) {
        // pixels point into buf; we must NOT stbi_image_free them.
    } else {
        stbi_set_flip_vertically_on_load(0);
        pixels = stbi_load_from_memory(buf, size, &w, &h, &channels, 4);
        if (!pixels) {
            printf("Failed to decode image: %s\n", path);
            free(buf);
            return -1;
        }
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    int tex_w = next_pow2(w);
    int tex_h = next_pow2(h);
    if (tex_w > 1024 || tex_h > 1024) {
        if (!is_gr1t) stbi_image_free(pixels);
        free(buf);
        return -1;
    }

    printf("TexInit: %dx%d RGBA8\n", tex_w, tex_h);
    bool tex_ok = C3D_TexInit(out_tex, (u16)tex_w, (u16)tex_h, GPU_RGBA8);
    printf("TexInit ret=%d data=%p size=%u\n", (int)tex_ok, out_tex->data, (unsigned)out_tex->size);
    if (!tex_ok) {
        if (!is_gr1t) stbi_image_free(pixels);
        free(buf);
        return -1;
    }

    uint32_t* dest = (uint32_t*)out_tex->data;
    uint32_t  totalPixels = out_tex->size / 4;

    /* Clear to transparent black first */
    for (uint32_t i = 0; i < totalPixels; i++) dest[i] = 0x00000000;

    // Write pixels in natural PNG order (y=0 = top of image).
    // C2D uses UV with high-value = top, so subtex->top = h/H means
    // "draw from UV=0 (row0, PNG-top) up to h/H". Morton swizzle handles
    // the hardware tile layout.
    const uint8_t* srcBase = is_gr1t ? gr1tPixels : (const uint8_t*)pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* src = srcBase + (y * w + x) * 4;
            uint8_t r = src[0], g = src[1], b = src[2], a = src[3];

            /* GPU_RGBA8 in memory (little-endian ARM): byte0=A, byte1=B, byte2=G, byte3=R */
            uint32_t pixel = (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)g << 16) | ((uint32_t)r << 24);

            unsigned idx = indexOfTile(tex_w, tex_h, (unsigned)x, (unsigned)y);
            if (idx < totalPixels)
                dest[idx] = pixel;
        }
    }

    if (!is_gr1t) stbi_image_free(pixels);
    free(buf); // also releases gr1tPixels (it points into buf)
    C3D_TexFlush(out_tex);
    
    printf("Texture loaded OK: %dx%d (tex %dx%d)\n", w, h, tex_w, tex_h);
    return 0;
}
