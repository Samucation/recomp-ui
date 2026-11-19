// launcher_gl.c — image->GL-texture loader (stb_image + GL 1.1).

#include "launcher_gl.h"

#include "launcher_sdlcompat.h"   // GL header for the active SDL version

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // GL 1.2 token; runtime driver supports it
#endif

// STB_IMAGE_STATIC gives the launcher its OWN private (static) stb_image, so it
// never collides with a host that also embeds stb_image (the MMX game's
// glsl_shader.c does — sharing one impl caused LNK2005, and relying on the
// host's format config left TGA box art unloadable). Static = self-contained
// impl with our own STBI_ONLY_* set, identical behaviour standalone or in-process.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "third_party/stb_image.h"

static LauncherTexture upload_rgba(const unsigned char* pixels, int w, int h, int nearest) {
    LauncherTexture t = { 0, 0, 0 };
    if (!pixels || w <= 0 || h <= 0) return t;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    t.id = (unsigned int)tex;
    t.w = w; t.h = h;
    return t;
}

LauncherTexture launcher_texture_from_rgba(const unsigned char* rgba, int w, int h, int nearest) {
    return upload_rgba(rgba, w, h, nearest);
}

LauncherTexture launcher_texture_load_gray(const char* path, float dim) {
    LauncherTexture t = { 0, 0, 0 };
    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &comp, 4);
    if (!pixels) return t;
    if (dim < 0.0f) dim = 0.0f;
    if (dim > 1.0f) dim = 1.0f;
    for (int i = 0; i < w * h; ++i) {
        unsigned char* p = pixels + i * 4;
        float y = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * dim;
        unsigned char v = (unsigned char)(y > 255.0f ? 255.0f : y);
        p[0] = p[1] = p[2] = v;
    }
    t = upload_rgba(pixels, w, h, 0);
    stbi_image_free(pixels);
    return t;
}

// Fundo do launcher (19/11/2026): reduz, borra (3 passadas de caixa ~ gaussiana) e escurece.
static void box_blur_rgb(float* img, float* tmp, int w, int h, int r) {
    const float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < h; ++y) {               // horizontal
        float* row = img + (size_t)y * w * 3;
        for (int c = 0; c < 3; ++c) {
            float acc = 0.0f;
            for (int k = -r; k <= r; ++k) { int x = k < 0 ? 0 : (k >= w ? w - 1 : k); acc += row[x * 3 + c]; }
            for (int x = 0; x < w; ++x) {
                tmp[((size_t)y * w + x) * 3 + c] = acc * inv;
                int xo = x - r, xi = x + r + 1;
                if (xo < 0) xo = 0;
                if (xi >= w) xi = w - 1;
                acc += row[xi * 3 + c] - row[xo * 3 + c];
            }
        }
    }
    for (int x = 0; x < w; ++x) {               // vertical
        for (int c = 0; c < 3; ++c) {
            float acc = 0.0f;
            for (int k = -r; k <= r; ++k) { int y = k < 0 ? 0 : (k >= h ? h - 1 : k); acc += tmp[((size_t)y * w + x) * 3 + c]; }
            for (int y = 0; y < h; ++y) {
                img[((size_t)y * w + x) * 3 + c] = acc * inv;
                int yo = y - r, yi = y + r + 1;
                if (yo < 0) yo = 0;
                if (yi >= h) yi = h - 1;
                acc += tmp[((size_t)yi * w + x) * 3 + c] - tmp[((size_t)yo * w + x) * 3 + c];
            }
        }
    }
}

LauncherTexture launcher_texture_load_blurred(const char* path, int max_w, int radius, float dim) {
    LauncherTexture t = { 0, 0, 0 };
    int w = 0, h = 0, comp = 0;
    unsigned char* src = stbi_load(path, &w, &h, &comp, 4);
    if (!src) return t;
    int f = 1;                                   // fator de redução inteiro (média por bloco)
    while (max_w > 0 && w / (f + 1) >= max_w) ++f;
    const int ow = w / f, oh = h / f;
    float* img = (float*)malloc((size_t)ow * oh * 3 * sizeof(float));
    float* tmp = (float*)malloc((size_t)ow * oh * 3 * sizeof(float));
    unsigned char* out = (unsigned char*)malloc((size_t)ow * oh * 4);
    if (ow > 0 && oh > 0 && img && tmp && out) {
        const float n = (float)(f * f);
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                float s[3] = { 0, 0, 0 };
                for (int yy = 0; yy < f; ++yy)
                    for (int xx = 0; xx < f; ++xx) {
                        const unsigned char* p = src + ((size_t)(y * f + yy) * w + (x * f + xx)) * 4;
                        s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
                    }
                float* d = img + ((size_t)y * ow + x) * 3;
                d[0] = s[0] / n; d[1] = s[1] / n; d[2] = s[2] / n;
            }
        if (radius > 0)
            for (int pass = 0; pass < 3; ++pass) box_blur_rgb(img, tmp, ow, oh, radius);
        if (dim < 0.0f) dim = 0.0f;
        if (dim > 1.0f) dim = 1.0f;
        for (int i = 0; i < ow * oh; ++i) {
            for (int c = 0; c < 3; ++c) {
                float v = img[i * 3 + c] * dim;
                out[i * 4 + c] = (unsigned char)(v > 255.0f ? 255.0f : (v < 0.0f ? 0.0f : v));
            }
            out[i * 4 + 3] = 255;
        }
        t = upload_rgba(out, ow, oh, 0);
    }
    free(img); free(tmp); free(out);
    stbi_image_free(src);
    return t;
}

// Shared upload path. `colorkey_tol` < 0 disables color-keying.
static LauncherTexture load_impl(const char* path, int colorkey_tol) {
    LauncherTexture t = { 0, 0, 0 };
    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &comp, 4); // force RGBA
    if (!pixels) return t;

    if (colorkey_tol >= 0 && w > 0 && h > 0) {
        // Key out the backdrop using the top-left pixel as the reference.
        const unsigned char kr = pixels[0], kg = pixels[1], kb = pixels[2];
        for (int i = 0; i < w * h; ++i) {
            unsigned char* p = pixels + i * 4;
            int dr = (int)p[0] - kr, dg = (int)p[1] - kg, db = (int)p[2] - kb;
            if (dr < 0) dr = -dr;  if (dg < 0) dg = -dg;  if (db < 0) db = -db;
            if (dr <= colorkey_tol && dg <= colorkey_tol && db <= colorkey_tol)
                p[3] = 0;   // transparent
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    t.id = (unsigned int)tex;
    t.w = w; t.h = h;
    return t;
}

LauncherTexture launcher_texture_load(const char* path) {
    return load_impl(path, -1);
}

LauncherTexture launcher_texture_load_colorkey(const char* path, int tolerance) {
    return load_impl(path, tolerance < 0 ? 0 : tolerance);
}

void launcher_texture_free(LauncherTexture* t) {
    if (t && t->id) {
        GLuint id = (GLuint)t->id;
        glDeleteTextures(1, &id);
        t->id = 0;
    }
}
