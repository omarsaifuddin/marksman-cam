/* Self-check for the PTP framing, live-view parsing and frame geometry. No camera needed. */
#undef NDEBUG /* Release builds define it, which would silently turn every check below into a no-op */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <turbojpeg.h>

#include "../src/ctl.h"
#include "../src/image.h"
#include "../src/ptp.h"

/* Fake bulk endpoints: IN transfers are scripted, OUT transfers are logged. */
static uint8_t script[16][2048], sent[512];
static int script_len[16], nscript, next_in, nsent;

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
static void reset_script(void) { nscript = next_in = nsent = 0; }

/* Little-endian builder for synthetic camera payloads. */
static uint8_t pl[1024];
static uint32_t pn;
static void b8(unsigned v) { pl[pn++] = (uint8_t)v; }
static void b16(unsigned v) { b8(v & 0xFF), b8(v >> 8 & 0xFF); }
static void b32(uint32_t v) { b16(v & 0xFFFF), b16(v >> 16); }

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

    assert(eos_setting(0, "iso") == 0xD103 && eos_setting(1, "iso") == 0xD21E && eos_setting(0, "nope") == -1);
    assert(eos_setting(0, "p_D1B0") == 0xD1B0 && eos_setting(1, "0x5007") == 0x5007);

    /* Sony: GetObject(0xFFFFC002) returns [u32 offset][u32 size] then the JPEG. */
    ptp_t s = {.bulk = fake_bulk, .ep_in = 0x81, .ep_out = 0x02, .buf = malloc(PTP_BUF), .sony = 1};
    uint8_t sv[20] = {0};
    put32(sv, 16), put32(sv + 4, 4);
    sv[16] = 0xFF, sv[17] = 0xD8, sv[18] = 0xFF, sv[19] = 0xD9;
    nsent = 0;
    push_container(2, 0x1009, sv, 20, 0);
    push_container(3, 0x2001, NULL, 0, 0);
    assert(eos_frame(&s, &jpeg, &len) == 1 && len == 4 && jpeg == s.buf + 12 + 16);
    assert(sent[6] == 0x09 && sent[7] == 0x10 && sent[12] == 0x02 && sent[13] == 0xC0 && sent[15] == 0xFF);
    push_container(3, 0x200F, NULL, 0, 0); /* live view not up yet */
    assert(eos_frame(&s, &jpeg, &len) == 0);
    assert(eos_set_prop(&s, 0xD21E, 1) == 0x2005);
    assert(sony_find_jpeg(sv + 16, 4, &len) == sv + 16 && len == 4); /* no header: SOI..EOI scan */
    assert(!sony_find_jpeg(sv, 16, &len));
    free(s.buf);
}

/* 4:4:4 test frame: Y given per pixel, neutral chroma. */
static yuv_t frame444(int w, int h, const uint8_t *y)
{
    static uint8_t buf[3 * 64 * 64];
    yuv_t f = {w, h, w, h, {buf, buf + w * h, buf + 2 * w * h}, buf, sizeof buf};
    memcpy(buf, y, (size_t)w * h);
    memset(buf + w * h, 128, (size_t)2 * w * h);
    return f;
}

static uint8_t luma(const uint8_t *nv12, int w, int x, int y) { return nv12[(size_t)y * w + x]; }

static void test_render(void)
{
    static uint8_t out[64 * 64 * 2];
    const uint8_t lr[2] = {200, 50}; /* left bright, right dark */
    yuv_t f = frame444(2, 1, lr);
    view_t v = {2, 2, 0, 90, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0, 0};

    render_nv12(&f, &v, out); /* 90 deg clockwise: the left side goes to the top */
    assert(luma(out, 2, 0, 0) > luma(out, 2, 0, 1));
    v.rotate = 270;
    render_nv12(&f, &v, out);
    assert(luma(out, 2, 0, 0) < luma(out, 2, 0, 1));

    v = (view_t){2, 2, 0, 0, 1, 0, {0, 0, 1, 1}, 0, 0, 0, 0, 0};
    render_nv12(&f, &v, out); /* mirror */
    assert(luma(out, 2, 0, 0) < luma(out, 2, 1, 0));

    /* 3:2 source letterboxed into 16:9: 64x36 output gets 54 px of picture and 5 px bars each side. */
    static uint8_t white[30 * 20];
    memset(white, 255, sizeof white);
    f = frame444(30, 20, white);
    v = (view_t){64, 36, 0, 0, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0, 0};
    render_nv12(&f, &v, out);
    assert(luma(out, 64, 4, 18) == 16 && luma(out, 64, 5, 18) == 235 && luma(out, 64, 58, 18) == 235 &&
           luma(out, 64, 59, 18) == 16);
    v.fill = 1;
    render_nv12(&f, &v, out);
    assert(luma(out, 64, 0, 0) == 235 && luma(out, 64, 63, 35) == 235);
    v.fill = 0;
    for (int y = 0; y < 20; y++)
        memset(white + y * 30, 0, 5); /* dark left edge: visible unzoomed, cropped away at 2x */
    f = frame444(30, 20, white);
    render_nv12(&f, &v, out);
    assert(luma(out, 64, 6, 18) == 16);
    v.zoom = 200;
    render_nv12(&f, &v, out);
    assert(luma(out, 64, 6, 18) == 235 && luma(out, 64, 4, 18) == 16); /* same aspect: bars stay */

    /* No camera: the status grey, neutral chroma. YUYV interleaves Y U Y V. */
    v = (view_t){4, 2, 0, 0, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0, 0};
    render_yuyv(NULL, &v, out);
    assert(out[0] == 57 && out[1] == 128 && out[2] == 57 && out[3] == 128);
}

static void test_yuv(void)
{
    static uint8_t out[16 * 16 * 2];
    const uint8_t bw[4] = {255, 255, 0, 0};
    yuv_t f = frame444(2, 2, bw); /* top row white, bottom row black */
    view_t v = {2, 2, 0, 0, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0, 0};
    render_nv12(&f, &v, out);
    assert(out[0] == 235 && out[2] == 16 && out[4] == 128 && out[5] == 128);
    v.brightness = 20;
    render_nv12(&f, &v, out);
    assert(out[2] == 36);

    /* Real JPEG through libjpeg-turbo: solid red, 4:2:2 like the cameras. BT.601 limited red = 81,90,240. */
    static uint8_t rgb[16 * 16 * 3];
    unsigned char *jpg = NULL;
    unsigned long jl = 0;
    yuv_t d = {0};
    for (int i = 0; i < 16 * 16; i++)
        rgb[3 * i] = 255, rgb[3 * i + 1] = rgb[3 * i + 2] = 0;
    tjhandle c = tjInitCompress();
    assert(c && !tjCompress2(c, rgb, 16, 0, 16, TJPF_RGB, &jpg, &jl, TJSAMP_422, 95, 0));
    assert(!jpeg_decode(jpg, (uint32_t)jl, &d) && d.w == 16 && d.cw == 8 && d.ch == 16);
    v = (view_t){16, 16, 0, 0, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0, 0};
    render_nv12(&d, &v, out);
    const uint8_t *uv = out + 16 * 16;
    assert(abs(out[8 * 16 + 8] - 81) <= 2 && abs(uv[0] - 90) <= 3 && abs(uv[1] - 240) <= 3);
    tjFree(jpg);
    tjDestroy(c);
    free(d.buf);
}

static void test_props(void)
{
    ptp_t c = {.bulk = fake_bulk, .ep_in = 0x81, .ep_out = 0x02, .buf = malloc(PTP_BUF)};
    ptp_t s = {.bulk = fake_bulk, .ep_in = 0x81, .ep_out = 0x02, .buf = malloc(PTP_BUF), .sony = 1};
    const ptp_prop_t *pr;
    char lab[32];

    /* Canon GetEvent: ISO value, ISO list, an unrelated record, terminator. */
    pn = 0;
    b32(16), b32(0xC189), b32(0xD103), b32(0x48);
    b32(32), b32(0xC18A), b32(0xD103), b32(3), b32(3), b32(0), b32(0x48), b32(0x50);
    b32(12), b32(0xC1A7), b32(1);
    b32(8), b32(0);
    canon_parse_events(&c, pl, pn);
    assert((pr = eos_prop(&c, 0xD103)) && pr->value == 0x48 && pr->form == 2 && pr->nopts == 3 && pr->opts[2] == 0x50);
    assert(c.nprops == 1 && c.props_seq == 1);

    /* Sony 0x9209: ISO (UINT32 list, followed by a protocol-3.00 second list), then aperture (UINT16 range). */
    for (int second = 0; second < 2; second++) {
        s.nprops = 0;
        pn = 0;
        b32(2), b32(0);
        b16(0xD21E), b16(6), b8(1), b8(1), b32(0xFFFFFF), b32(400), b8(2);
        b16(3), b32(100), b32(200), b32(400);
        if (second)
            b16(2), b32(100), b32(400);
        b16(0x5007), b16(4), b8(1), b8(1), b16(280), b16(560), b8(1), b16(140), b16(2200), b16(10);
        sony_parse_props(&s, pl, pn);
        assert(s.nprops == 2);
        assert((pr = eos_prop(&s, 0xD21E)) && pr->value == 400 && pr->nopts == 3 && pr->opts[1] == 200 && !pr->ro);
        assert((pr = eos_prop(&s, 0x5007)) && pr->value == 560 && pr->form == 1 && pr->min == 140 && pr->max == 2200);
    }

    /* Sony set: SetExtDevicePropValue(0xD21E) with a 4-byte value; unknown property refused locally. */
    reset_script();
    push_container(3, 0x2001, NULL, 0, 0);
    assert(eos_set_prop(&s, 0xD21E, 800) == PTP_OK);
    assert(sent[6] == 0x05 && sent[7] == 0x92 && sent[12] == 0x1E && sent[13] == 0xD2);
    assert(nsent == 16 + 16 && sent[16 + 4] == 2 && sent[16 + 12] == 0x20 && sent[16 + 13] == 0x03); /* 800 */
    assert(eos_set_prop(&s, 0xD2FF, 1) == 0x2005);

    /* Canon autofocus: AfCancel then DoAf. Lens drive: 2 nearer = param 2, 3 farther = 0x8003. */
    reset_script();
    push_container(3, 0x2001, NULL, 0, 0);
    push_container(3, 0x2001, NULL, 0, 0);
    assert(eos_autofocus(&c) == PTP_OK && sent[6] == 0x60 && sent[7] == 0x91 && sent[12 + 6] == 0x54);
    reset_script();
    push_container(3, 0x2001, NULL, 0, 0);
    push_container(3, 0x2001, NULL, 0, 0);
    assert(eos_focus(&c, -2) == PTP_OK && sent[6] == 0x55 && sent[7] == 0x91 && sent[12] == 2);
    assert(eos_focus(&c, 3) == PTP_OK && sent[16 + 12] == 3 && sent[16 + 13] == 0x80);
    assert(eos_focus(&c, 0) == 0x2005 && eos_focus(&c, 4) == 0x2005);

    /* Labels. */
    eos_value_label(0, 0xD103, 0x4B, lab, sizeof lab), assert(!strcmp(lab, "125"));
    eos_value_label(0, 0xD101, 0x30, lab, sizeof lab), assert(!strcmp(lab, "f/5.6"));
    eos_value_label(0, 0xD102, 0x70, lab, sizeof lab), assert(!strcmp(lab, "1/125"));
    eos_value_label(0, 0xD102, 0x10, lab, sizeof lab), assert(!strcmp(lab, "30\""));
    eos_value_label(0, 0xD104, 0xFD, lab, sizeof lab), assert(!strcmp(lab, "-0.3"));
    eos_value_label(1, 0xD20D, 0x0001007D, lab, sizeof lab), assert(!strcmp(lab, "1/125"));
    eos_value_label(1, 0xD21E, 0xFFFFFF, lab, sizeof lab), assert(!strcmp(lab, "Auto"));
    eos_value_label(1, 0x5007, 560, lab, sizeof lab), assert(!strcmp(lab, "f/5.6"));

    /* Control protocol: validation, picture controls, camera commands queued for the capture thread. */
    view_t v = {0};
    char *r;
    assert((r = ctl_reply("list")) && strstr(r, "\"brightness\"") && !strstr(r, "\"iso\"") && strstr(r, "\"connected\":false")), free(r);
    assert((r = ctl_reply("set brightness 20")) && !strcmp(r, "ok\n")), free(r);
    assert(ctl_take_view(&v) && v.brightness == 20 && !ctl_take_view(&v));
    assert((r = ctl_reply("set brightness 500")) && !strncmp(r, "error", 5)), free(r);
    assert((r = ctl_reply("set brightness 2x")) && !strncmp(r, "error", 5)), free(r);
    assert((r = ctl_reply("set rotate 45")) && !strncmp(r, "error", 5)), free(r);
    assert((r = ctl_reply("POST / HTTP/1.1")) && !strncmp(r, "error", 5)), free(r);
    assert((r = ctl_reply("set af 1")) && !strncmp(r, "error", 5)), free(r); /* no camera yet */
    ctl_camera(&c);
    assert((r = ctl_reply("list")) && strstr(r, "\"iso\"") && strstr(r, "[80,\"200\"]") && strstr(r, "\"af\"")), free(r);
    assert((r = ctl_reply("set iso 0x51")) && !strncmp(r, "error", 5)), free(r); /* not in the camera's list */
    assert((r = ctl_reply("set iso 0x50")) && !strcmp(r, "ok\n")), free(r);
    reset_script();
    push_container(3, 0x2001, NULL, 0, 0);
    ctl_camera(&c); /* runs the queued SetPropValue */
    assert(sent[6] == 0x10 && sent[7] == 0x91 && sent[12 + 16] == 0x03 && sent[12 + 20] == 0x50);
    assert(eos_prop(&c, 0xD103)->value == 0x50);
    ctl_camera(NULL);
    free(c.props), free(c.buf), free(s.props), free(s.buf);
}

int main(void)
{
    test_ptp();
    reset_script();
    test_props();
    test_render();
    test_yuv();
    puts("all tests passed");
    return 0;
}
