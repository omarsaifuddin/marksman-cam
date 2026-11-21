/* JPEG decode, frame geometry and picture controls. Everything stays in YUV: the camera's JPEG is decoded
   to its Y/Cb/Cr planes and each output sample is read straight from them, so there is no RGB step. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define IMG_MAX 4096 /* largest output side render_* accepts */

typedef struct {
    int out_w, out_h;        /* even */
    int fill;                /* 0: letterbox (fit), 1: crop to cover (fill) */
    int rotate;              /* 0, 90, 180, 270 clockwise */
    int mirror, flip;        /* horizontal / vertical, applied to the displayed image */
    float crop[4];           /* x0 y0 x1 y1 as fractions of the source; {0,0,1,1} = whole frame */
    int brightness;          /* luma offset, -100..100 */
    int contrast, saturation;/* percent change, -100..100 */
    int hue;                 /* degrees */
    int zoom;                /* digital zoom in percent around the crop centre; 100 (or 0) = none */
} view_t;

/* A decoded JPEG: full-range BT.601 planes, each tightly packed (stride = plane width). */
typedef struct {
    int w, h;       /* luma */
    int cw, ch;     /* chroma planes; 0 for a greyscale JPEG */
    uint8_t *p[3];  /* Y, Cb, Cr */
    uint8_t *buf;   /* backing store, grown as needed */
    size_t cap;
} yuv_t;

int jpeg_decode(const uint8_t *jpg, uint32_t len, yuv_t *f); /* 0 on success */
int jpeg_size(const uint8_t *jpg, uint32_t len, int *w, int *h);

/* Crops, rotates, flips, scales and colour-adjusts f into a v->out_w x v->out_h frame of BT.601
   limited-range video. f NULL draws the "no camera" grey (luma 57). */
void render_nv12(const yuv_t *f, const view_t *v, uint8_t *out);
void render_yuyv(const yuv_t *f, const view_t *v, uint8_t *out);
