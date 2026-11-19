/* Self-check for the PTP framing, live-view parsing and frame geometry. No camera needed. */
#undef NDEBUG /* Release builds define it, which would silently turn every check below into a no-op */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/image.h"
#include "../src/ptp.h"

/* Fake bulk endpoints: IN transfers are scripted, OUT transfers are logged. */
static uint8_t script[8][2048], sent[512];
static int script_len[8], nscript, next_in, nsent;

static int fake_bulk(void *ctx, uint8_t ep, uint8_t *buf, int len, int *got, unsigned timeout_ms)
{
    (void)ctx, (void)timeout_ms;
    if (ep & 0x80) {
        if (next_in >= nscript || script_len[next_in] > len)
            return -1;
        memcpy(buf, script[next_in], (size_t)script_len[next_in]);
        *got = script_len[next_in++];
    } else {
        memcpy(sent + nsent, buf, (size_t)len);
        nsent += *got = len;
    }
    return 0;
}

static void put32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> 8 * i); }

static void push_container(uint16_t type, uint16_t code, const uint8_t *payload, uint32_t n, int split)
{
    static uint8_t c[2048];
    put32(c, 12 + n);
    c[4] = (uint8_t)type, c[5] = 0, c[6] = (uint8_t)code, c[7] = (uint8_t)(code >> 8);
    put32(c + 8, 0);
    if (n)
        memcpy(c + 12, payload, n);
    for (uint32_t off = 0, step = split ? (12 + n + 1) / 2 : 12 + n; off < 12 + n; off += step) {
        uint32_t m = 12 + n - off < step ? 12 + n - off : step;
        memcpy(script[nscript], c + off, m);
        script_len[nscript++] = (int)m;
    }
}

static void test_ptp(void)
{
    ptp_t p = {.bulk = fake_bulk, .ep_in = 0x81, .ep_out = 0x02, .buf = malloc(PTP_BUF)};
    uint8_t lv[64] = {0};
    const uint8_t *jpeg;
    uint32_t len;

    /* GetViewFinderData: one non-image block, then a JPEG block; data arrives in two reads, then a
       zero-length packet, then the response. */
    put32(lv, 12), put32(lv + 4, 5);
    put32(lv + 12, 8 + 4), put32(lv + 16, 1);
    lv[20] = 0xFF, lv[21] = 0xD8, lv[22] = 0xFF, lv[23] = 0xD9;
    push_container(2, 0x9153, lv, 24, 1);
    script_len[nscript++] = 0;
    push_container(3, 0x2001, NULL, 0, 0);
    assert(eos_frame(&p, &jpeg, &len) == 1);
    assert(len == 4 && jpeg[0] == 0xFF && jpeg[1] == 0xD8);
    /* command container: length 24, type 1, code 0x9153, tid 0, params 0x00100000 0 0 */
    assert(nsent == 24 && sent[0] == 24 && sent[4] == 1 && sent[6] == 0x53 && sent[7] == 0x91);
    assert(sent[8] == 0 && sent[14] == 0x10);
    assert(p.tid == 1);

    /* Camera not ready yet: response only, no data phase. */
    push_container(3, 0xA102, NULL, 0, 0);
    assert(eos_frame(&p, &jpeg, &len) == 0);

    /* SetPropValueEx sends a 12-byte data container after the command. */
    nsent = 0;
    push_container(3, 0x2001, NULL, 0, 0);
    assert(eos_set_prop(&p, 0xD103, 0x48) == PTP_OK);
    assert(nsent == 12 + 24 && sent[12 + 4] == 2 && sent[12 + 12] == 12 && sent[12 + 16] == 0x03 && sent[12 + 20] == 0x48);

    /* Transport error marks the session dead. */
    assert(eos_frame(&p, &jpeg, &len) == -1 && p.dead);
    free(p.buf);

    assert(eos_setting("iso") == 0xD103 && eos_setting("nope") == -1);
}

static uint8_t *px(uint8_t *img, int w, int x, int y) { return img + ((size_t)y * w + x) * 4; }

static void test_render(void)
{
    uint8_t src[2 * 1 * 4] = {255, 0, 0, 0, 0, 0, 255, 0}; /* left blue, right red (BGRX) */
    uint8_t dst[64 * 64 * 4];
    view_t v = {1, 2, 0, 90, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0};

    render(src, 2, 1, &v, dst); /* 90 deg clockwise: left side goes to the top */
    assert(px(dst, 1, 0, 0)[0] == 255 && px(dst, 1, 0, 1)[2] == 255);
    v.rotate = 270;
    render(src, 2, 1, &v, dst);
    assert(px(dst, 1, 0, 0)[2] == 255);

    v = (view_t){2, 1, 0, 0, 1, 0, {0, 0, 1, 1}, 0, 0, 0, 0};
    render(src, 2, 1, &v, dst); /* mirror */
    assert(px(dst, 2, 0, 0)[2] == 255 && px(dst, 2, 1, 0)[0] == 255);

    /* 3:2 source letterboxed into 16:9: 64x36 output gets 54 px of picture and 5 px bars each side. */
    static uint8_t white[30 * 20 * 4];
    memset(white, 255, sizeof white);
    v = (view_t){64, 36, 0, 0, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0};
    render(white, 30, 20, &v, dst);
    assert(px(dst, 64, 4, 18)[1] == 0 && px(dst, 64, 5, 18)[1] == 255 && px(dst, 64, 58, 18)[1] == 255 &&
           px(dst, 64, 59, 18)[1] == 0);
    v.fill = 1;
    render(white, 30, 20, &v, dst);
    assert(px(dst, 64, 0, 0)[1] == 255 && px(dst, 64, 63, 35)[1] == 255);
}

static void test_yuv(void)
{
    uint8_t white[4 * 4] = {255, 255, 255, 0, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t yuyv[8], nv12[2 * 2 + 2];
    view_t v = {0};
    to_yuyv(white, 4, 1, &v, yuyv);
    assert(yuyv[0] == 235 && yuyv[1] == 128 && yuyv[3] == 128 && yuyv[4] == 16);
    to_nv12(white, 2, 2, &v, nv12); /* top row white, bottom row black */
    assert(nv12[0] == 235 && nv12[2] == 16 && nv12[4] == 128 && nv12[5] == 128);
    v.brightness = 20;
    to_yuyv(white + 8, 2, 1, &v, yuyv);
    assert(yuyv[0] == 36);
}

int main(void)
{
    test_ptp();
    test_render();
    test_yuv();
    puts("all tests passed");
    return 0;
}
