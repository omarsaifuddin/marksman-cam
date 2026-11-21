#include "image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

#define STATUS_Y 57 /* the grey vcam_probe reports as "no camera found" */

static tjhandle tj;

int jpeg_size(const uint8_t *jpg, uint32_t len, int *w, int *h)
{
    int subsamp, cs;
    if (!tj && !(tj = tjInitDecompress()))
        return -1;
    return tjDecompressHeader3(tj, jpg, len, w, h, &subsamp, &cs) ? -1 : 0;
}

int jpeg_decode(const uint8_t *jpg, uint32_t len, yuv_t *f)
{
    int w, h, subsamp, cs, strides[3] = {0, 0, 0};
    size_t ysz, csz;

    if (!tj && !(tj = tjInitDecompress()))
        return -1;
    if (tjDecompressHeader3(tj, jpg, len, &w, &h, &subsamp, &cs) || (cs != TJCS_YCbCr && cs != TJCS_GRAY))
        return -1;
    f->w = w;
    f->h = h;
    f->cw = subsamp == TJSAMP_GRAY ? 0 : tjPlaneWidth(1, w, subsamp);
    f->ch = subsamp == TJSAMP_GRAY ? 0 : tjPlaneHeight(1, h, subsamp);
    ysz = (size_t)w * h;
    csz = (size_t)f->cw * f->ch;
    if (ysz + 2 * csz > f->cap) {
        free(f->buf);
        f->cap = (f->buf = malloc(ysz + 2 * csz)) ? ysz + 2 * csz : 0;
        if (!f->buf)
            return -1;
    }
    f->p[0] = f->buf;
    f->p[1] = f->buf + ysz;
    f->p[2] = f->buf + ysz + csz;
    if (tjDecompressToYUVPlanes(tj, jpg, len, f->p, w, strides, h, TJFLAG_FASTDCT))
        return tjGetErrorCode(tj) == TJERR_FATAL ? -1 : 0; /* warnings = recoverable corrupt data */
    return 0;
}

/* Where the output picture sits: crop rectangle (source pixels), its size after rotation, scale and offset. */
typedef struct {
    float cx, cy, cw, ch, rw, rh, s, ox, oy;
    int q;
} geom_t;

static geom_t geometry(int sw, int sh, const view_t *v)
{
    geom_t g;
    float c[4] = {v->crop[0], v->crop[1], v->crop[2], v->crop[3]};
    if (v->zoom > 100) { /* shrink the crop around its centre */
        const float z = 100.f / v->zoom, mx = (c[0] + c[2]) / 2, my = (c[1] + c[3]) / 2;
        const float hw = (c[2] - c[0]) * z / 2, hh = (c[3] - c[1]) * z / 2;
        c[0] = mx - hw, c[2] = mx + hw, c[1] = my - hh, c[3] = my + hh;
    }
    g.q = v->rotate / 90 & 3;
    g.cx = c[0] * sw, g.cy = c[1] * sh;
    g.cw = (c[2] - c[0]) * sw, g.ch = (c[3] - c[1]) * sh;
    g.rw = g.q & 1 ? g.ch : g.cw, g.rh = g.q & 1 ? g.cw : g.ch;
    g.s = v->fill ? fmaxf(v->out_w / g.rw, v->out_h / g.rh) : fminf(v->out_w / g.rw, v->out_h / g.rh);
    g.ox = (v->out_w - g.rw * g.s) / 2, g.oy = (v->out_h - g.rh * g.s) / 2;
    return g;
}

/* One bilinear tap along a source axis: two indices (or row offsets) and the weight of the second, 0..256.
   i0 < 0 marks an output sample outside the picture (letterbox bar). */
typedef struct {
    int i0, i1, f;
} tap_t;

static tap_t cols[IMG_MAX], rows[IMG_MAX];

/* Taps for n output samples spaced d output pixels apart along output axis `oy` (0 = x, 1 = y), into a
   source plane of pw x ph that is kx, ky times the luma size. The geometry is separable: every output
   axis maps onto exactly one source axis, whatever the rotation. Taps onto the source y axis hold row
   offsets, so the sampler just adds them. */
static void make_taps(const geom_t *g, const view_t *v, int oy, int n, int d, int pw, int ph, float kx, float ky,
                      tap_t *t)
{
    const float lim = oy ? g->rh : g->rw, off = oy ? g->oy : g->ox;
    const int flipped = oy ? v->flip : v->mirror;
    /* Source x is fed by output x for 0/180 degrees and by output y for 90/270. */
    const int to_x = (g->q & 1) == oy;
    const int rev = to_x ? (oy ? g->q == 3 : g->q == 2) : (oy ? g->q == 2 : g->q == 1);
    const float base = to_x ? g->cx : g->cy, span = to_x ? g->cw : g->ch, k = to_x ? kx : ky;
    const int size = to_x ? pw : ph, mul = to_x ? 1 : pw;

    for (int i = 0; i < n; i++) {
        float u = ((i + 0.5f) * d - off) / g->s, pos;
        int i0;
        if (u < 0 || u >= lim) {
            t[i].i0 = -1;
            continue;
        }
        if (flipped)
            u = lim - u;
        pos = (base + (rev ? span - u : u)) * k - 0.5f;
        pos = pos < 0 ? 0 : pos > size - 1 ? (float)(size - 1) : pos;
        i0 = (int)pos;
        t[i].f = (int)((pos - (float)i0) * 256 + 0.5f);
        t[i].i0 = i0 * mul;
        t[i].i1 = (i0 + (i0 < size - 1)) * mul;
    }
}

static inline int bilerp(const uint8_t *p, const tap_t *x, const tap_t *y)
{
    const uint8_t *a = p + y->i0, *b = p + y->i1;
    const int top = a[x->i0] * (256 - x->f) + a[x->i1] * x->f;
    const int bot = b[x->i0] * (256 - x->f) + b[x->i1] * x->f;
    return (top * (256 - y->f) + bot * y->f + 32768) >> 16;
}

static int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Output plane layout: sample (x, y) lives at p + y * stride + x * step. */
typedef struct {
    uint8_t *y, *u, *v;
    int ystep, ystride, cstep, cstride;
    int cdy; /* output rows per chroma row: 2 for NV12, 1 for YUYV */
} out_t;

static void render(const yuv_t *f, const view_t *vw, const out_t *o)
{
    const int W = vw->out_w, H = vw->out_h, CW = W / 2, CH = H / o->cdy;
    uint8_t ylut[256];

    if (W > IMG_MAX || H > IMG_MAX)
        return;
    if (!f) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                o->y[(size_t)y * o->ystride + (size_t)x * o->ystep] = STATUS_Y;
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++)
                o->u[(size_t)y * o->cstride + (size_t)x * o->cstep] = o->v[(size_t)y * o->cstride + (size_t)x * o->cstep] = 128;
        return;
    }

    /* Luma: full range -> limited, contrast pivoting on mid grey, then brightness. */
    const float c = 1 + vw->contrast / 100.f;
    for (int i = 0; i < 256; i++)
        ylut[i] = (uint8_t)clamp((int)lroundf(126 + (i * 219.f / 255 - 110) * c) + vw->brightness, 16, 235);

    const geom_t g = geometry(f->w, f->h, vw);
    const int swap = g.q & 1;
    make_taps(&g, vw, 0, W, 1, f->w, f->h, 1, 1, cols);
    make_taps(&g, vw, 1, H, 1, f->w, f->h, 1, 1, rows);
    for (int y = 0; y < H; y++) {
        uint8_t *d = o->y + (size_t)y * o->ystride;
        const tap_t *ry = &rows[y];
        if (!swap && ry->i0 >= 0) { /* 0/180 degrees: one pair of source rows for the whole output row */
            const uint8_t *a = f->p[0] + ry->i0, *b = f->p[0] + ry->i1;
            const int fy = ry->f;
            for (int x = 0; x < W; x++, d += o->ystep) {
                const tap_t *cx = &cols[x];
                if (cx->i0 < 0) {
                    *d = 16;
                    continue;
                }
                const int top = a[cx->i0] * (256 - cx->f) + a[cx->i1] * cx->f;
                const int bot = b[cx->i0] * (256 - cx->f) + b[cx->i1] * cx->f;
                *d = ylut[(top * (256 - fy) + bot * fy + 32768) >> 16];
            }
            continue;
        }
        for (int x = 0; x < W; x++, d += o->ystep) {
            const tap_t *cx = &cols[x];
            *d = cx->i0 < 0 || ry->i0 < 0 ? 16 : ylut[bilerp(f->p[0], swap ? ry : cx, swap ? cx : ry)];
        }
    }

    /* Chroma: full range -> limited (224/255) folded into the saturation/hue rotation, fixed point /256. */
    const float s = (1 + vw->saturation / 100.f) * 224.f / 255, r = vw->hue * 3.14159265f / 180;
    const int m00 = (int)lroundf(cosf(r) * s * 256), m01 = (int)lroundf(-sinf(r) * s * 256);
    const int m10 = (int)lroundf(sinf(r) * s * 256), m11 = m00;
    const float kx = f->cw ? (float)f->cw / f->w : 1, ky = f->ch ? (float)f->ch / f->h : 1;
    make_taps(&g, vw, 0, CW, 2, f->cw, f->ch, kx, ky, cols);
    make_taps(&g, vw, 1, CH, o->cdy, f->cw, f->ch, kx, ky, rows);
    for (int y = 0; y < CH; y++) {
        uint8_t *du = o->u + (size_t)y * o->cstride, *dv = o->v + (size_t)y * o->cstride;
        for (int x = 0; x < CW; x++, du += o->cstep, dv += o->cstep) {
            const tap_t *cx = &cols[x], *ry = &rows[y];
            if (!f->cw || cx->i0 < 0 || ry->i0 < 0) {
                *du = *dv = 128;
                continue;
            }
            const int cb = bilerp(f->p[1], swap ? ry : cx, swap ? cx : ry) - 128;
            const int cr = bilerp(f->p[2], swap ? ry : cx, swap ? cx : ry) - 128;
            *du = (uint8_t)clamp(128 + (cb * m00 + cr * m01) / 256, 16, 240);
            *dv = (uint8_t)clamp(128 + (cb * m10 + cr * m11) / 256, 16, 240);
        }
    }
}

void render_nv12(const yuv_t *f, const view_t *v, uint8_t *out)
{
    uint8_t *uv = out + (size_t)v->out_w * v->out_h;
    const out_t o = {out, uv, uv + 1, 1, v->out_w, 2, v->out_w, 2};
    render(f, v, &o);
}

void render_yuyv(const yuv_t *f, const view_t *v, uint8_t *out)
{
    const out_t o = {out, out + 1, out + 3, 2, v->out_w * 2, 4, v->out_w * 2, 1};
    render(f, v, &o);
}
