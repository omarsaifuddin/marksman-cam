/* JPEG decode, frame geometry and picture controls, YUV output. Frames are BGRX, 4 bytes/pixel. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int out_w, out_h;        /* even */
    int fill;                /* 0: letterbox (fit), 1: crop to cover (fill) */
    int rotate;              /* 0, 90, 180, 270 clockwise */
    int mirror, flip;        /* horizontal / vertical, applied to the displayed image */
    float crop[4];           /* x0 y0 x1 y1 as fractions of the source; {0,0,1,1} = whole frame */
    int brightness;          /* luma offset, -100..100 */
    int contrast, saturation;/* percent change, -100..100 */
    int hue;                 /* degrees */
} view_t;

/* Decodes into *buf (grown as needed, *cap bytes). Returns 0 on success. */
int jpeg_decode(const uint8_t *jpg, uint32_t len, uint8_t **buf, size_t *cap, int *w, int *h);

/* Crops, rotates, flips and scales src (sw x sh) into dst (v->out_w x v->out_h). */
void render(const uint8_t *src, int sw, int sh, const view_t *v, uint8_t *dst);

/* "No camera" screen. */
void status_frame(uint8_t *dst, int w, int h);

/* BGRX -> BT.601 limited-range YUV with brightness/contrast/saturation/hue applied. */
void to_yuyv(const uint8_t *bgrx, int w, int h, const view_t *v, uint8_t *out);
void to_nv12(const uint8_t *bgrx, int w, int h, const view_t *v, uint8_t *out);
