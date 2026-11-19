#include "image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

int jpeg_decode(const uint8_t *jpg, uint32_t len, uint8_t **buf, size_t *cap, int *w, int *h)
{
    static tjhandle tj;
    int subsamp, colorspace;
    size_t need;

    if (!tj && !(tj = tjInitDecompress()))
        return -1;
    if (tjDecompressHeader3(tj, jpg, len, w, h, &subsamp, &colorspace))
        return -1;
    need = (size_t)*w * (size_t)*h * 4;
    if (need > *cap) {
        free(*buf);
        *cap = (*buf = malloc(need)) ? need : 0;
        if (!*buf)
            return -1;
    }
    if (tjDecompress2(tj, jpg, len, *buf, *w, 0, *h, TJPF_BGRX, TJFLAG_FASTDCT))
        return tjGetErrorCode(tj) == TJERR_FATAL ? -1 : 0; /* warnings = recoverable corrupt data */
    return 0;
}

static void sample(const uint8_t *s, int w, int h, float x, float y, uint8_t *d)
{
    int x0, y0, x1, y1;
    float fx, fy;
    x = x < 0 ? 0 : x > w - 1 ? (float)(w - 1) : x;
    y = y < 0 ? 0 : y > h - 1 ? (float)(h - 1) : y;
    x0 = (int)x;
    y0 = (int)y;
    x1 = x0 + (x0 < w - 1);
    y1 = y0 + (y0 < h - 1);
    fx = x - (float)x0;
    fy = y - (float)y0;
    const uint8_t *a = s + ((size_t)y0 * w + x0) * 4, *b = s + ((size_t)y0 * w + x1) * 4;
    const uint8_t *c = s + ((size_t)y1 * w + x0) * 4, *e = s + ((size_t)y1 * w + x1) * 4;
    for (int k = 0; k < 3; k++)
        d[k] = (uint8_t)((a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + e[k] * fx) * fy + 0.5f);
    d[3] = 255;
}

void render(const uint8_t *src, int sw, int sh, const view_t *v, uint8_t *dst)
{
    const int W = v->out_w, H = v->out_h, q = v->rotate / 90 & 3;
    const float cx = v->crop[0] * sw, cy = v->crop[1] * sh;
    const float cw = (v->crop[2] - v->crop[0]) * sw, ch = (v->crop[3] - v->crop[1]) * sh;
    const float rw = q & 1 ? ch : cw, rh = q & 1 ? cw : ch; /* size after rotation */
    const float s = v->fill ? fmaxf(W / rw, H / rh) : fminf(W / rw, H / rh);
    const float ox = (W - rw * s) / 2, oy = (H - rh * s) / 2;

    /* ponytail: bilinear, no prefilter; downscaling by more than 2x aliases. Add box prefilter if needed. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *d = dst + ((size_t)y * W + x) * 4;
            float u = (x + 0.5f - ox) / s, t = (y + 0.5f - oy) / s, a, b;
            if (u < 0 || t < 0 || u >= rw || t >= rh) {
                d[0] = d[1] = d[2] = 0;
                d[3] = 255;
                continue;
            }
            if (v->mirror)
                u = rw - u;
            if (v->flip)
                t = rh - t;
            switch (q) { /* undo the clockwise rotation */
            case 0: a = u; b = t; break;
            case 1: a = t; b = ch - u; break;
            case 2: a = cw - u; b = ch - t; break;
            default: a = cw - t; b = u; break;
            }
            sample(src, sw, sh, cx + a - 0.5f, cy + b - 0.5f, d);
        }
    }
}

void status_frame(uint8_t *dst, int w, int h)
{
    memset(dst, 0x30, (size_t)w * h * 4);
}

typedef struct { int ymul, yadd, uu, uv, vu, vv; } adjust_t; /* fixed point, 8 fractional bits */

static adjust_t make_adjust(const view_t *v)
{
    const float c = 1 + v->contrast / 100.f, s = 1 + v->saturation / 100.f, r = v->hue * 3.14159265f / 180;
    adjust_t a = {(int)lroundf(c * 256), v->brightness,
                  (int)lroundf(cosf(r) * s * 256), (int)lroundf(-sinf(r) * s * 256),
                  (int)lroundf(sinf(r) * s * 256), (int)lroundf(cosf(r) * s * 256)};
    return a;
}

static int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void yuv(const uint8_t *p, const adjust_t *a, int *y, int *u, int *v)
{
    const int r = p[2], g = p[1], b = p[0];
    const int Y = (66 * r + 129 * g + 25 * b + 128) / 256;  /* 0..219 */
    const int U = (-38 * r - 74 * g + 112 * b) / 256;       /* -112..112 */
    const int V = (112 * r - 94 * g - 18 * b) / 256;
    *y = clamp(16 + 110 + (Y - 110) * a->ymul / 256 + a->yadd, 16, 235); /* contrast pivots on mid grey */
    *u = clamp(128 + (U * a->uu + V * a->uv) / 256, 16, 240);
    *v = clamp(128 + (U * a->vu + V * a->vv) / 256, 16, 240);
}

void to_yuyv(const uint8_t *bgrx, int w, int h, const view_t *v, uint8_t *out)
{
    const adjust_t a = make_adjust(v);
    for (size_t i = 0, n = (size_t)w * h; i < n; i += 2, bgrx += 8, out += 4) {
        int y0, u0, v0, y1, u1, v1;
        yuv(bgrx, &a, &y0, &u0, &v0);
        yuv(bgrx + 4, &a, &y1, &u1, &v1);
        out[0] = (uint8_t)y0;
        out[1] = (uint8_t)((u0 + u1 + 1) / 2);
        out[2] = (uint8_t)y1;
        out[3] = (uint8_t)((v0 + v1 + 1) / 2);
    }
}

void to_nv12(const uint8_t *bgrx, int w, int h, const view_t *v, uint8_t *out)
{
    const adjust_t a = make_adjust(v);
    uint8_t *uv = out + (size_t)w * h;
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            int yy, uu, vv, su = 0, sv = 0;
            for (int k = 0; k < 4; k++) {
                const int px = x + (k & 1), py = y + (k >> 1);
                yuv(bgrx + ((size_t)py * w + px) * 4, &a, &yy, &uu, &vv);
                out[(size_t)py * w + px] = (uint8_t)yy;
                su += uu;
                sv += vv;
            }
            uv[(size_t)(y / 2) * w + x] = (uint8_t)((su + 2) / 4);
            uv[(size_t)(y / 2) * w + x + 1] = (uint8_t)((sv + 2) / 4);
        }
    }
}
