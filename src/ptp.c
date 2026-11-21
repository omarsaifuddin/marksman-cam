#include "ptp.h"

#include <ctype.h>
#include <libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CANON_VID  0x04A9
#define SONY_VID   0x054C
#define TIMEOUT_MS 5000
#define ALIGN      1024u /* keeps every bulk read a multiple of the max packet size (HS 512, SS 1024) */

enum { CMD = 1, DATA = 2, RESP = 3 };

#define OP_OPEN_SESSION     0x1002
#define OP_CLOSE_SESSION    0x1003
#define OP_GET_OBJECT       0x1009
#define OP_EOS_SET_PROP     0x9110
#define OP_EOS_REMOTE_MODE  0x9114
#define OP_EOS_EVENT_MODE   0x9115
#define OP_EOS_GET_EVENT    0x9116
#define OP_EOS_KEEP_ON      0x911D
#define OP_EOS_VIEWFINDER   0x9153
#define OP_EOS_DO_AF        0x9154
#define OP_EOS_DRIVE_LENS   0x9155 /* param: 1..3 nearer (fine..coarse), 0x8001..0x8003 farther */
#define OP_EOS_AF_CANCEL    0x9160
#define OP_SONY_CONNECT     0x9201 /* SDIO connect, phases 1..3 */
#define OP_SONY_EXT_INFO    0x9202 /* param: protocol version, 0x12C = 3.00, 0xC8 = 2.00 */
#define OP_SONY_SET_PROP    0x9205 /* param: property; data: the value, sized by its datatype */
#define OP_SONY_CONTROL     0x9207 /* same shape, for button-like properties */
#define OP_SONY_ALL_PROPS   0x9209
#define SONY_S1             0xD2C1 /* shutter half-press: 2 = down, 1 = up */
#define SONY_NEAR_FAR       0xD2D1 /* INT16 lens drive, negative nearer */
#define SONY_LIVEVIEW       0xFFFFC002 /* object handle of the current live-view frame */

#define RC_UNSUPPORTED      0x2005
#define RC_ACCESS_DENIED    0x200F
#define RC_BUSY             0x2019
#define RC_SESSION_OPEN     0x201E
#define RC_NOT_READY        0xA102

#define EV_PROP_VALUE       0xC189 /* Canon event records: a property's current value */
#define EV_PROP_AVAIL       0xC18A /* ... and its list of allowed values */

#define PROP_EVF_OUTPUT     0xD1B0
#define PROP_EVF_MODE       0xD1B3
#define EVF_OFF 0
#define EVF_PC  2

static int debug(void) /* MARKSMAN_DEBUG=1: trace PTP operations and USB errors */
{
    static int d = -1;
    if (d < 0)
        d = getenv("MARKSMAN_DEBUG") != NULL;
    return d;
}

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *b, uint32_t v) { wr16(b, (uint16_t)v); wr16(b + 2, (uint16_t)(v >> 16)); }

static int send_container(ptp_t *p, uint16_t type, uint16_t code, const uint8_t *payload, uint32_t n)
{
    uint8_t c[64];
    int got;
    if (n > sizeof c - 12)
        return -1;
    wr32(c, 12 + n);
    wr16(c + 4, type);
    wr16(c + 6, code);
    wr32(c + 8, p->tid);
    if (n)
        memcpy(c + 12, payload, n);
    return p->bulk(p->ctx, p->ep_out, c, (int)(12 + n), &got, TIMEOUT_MS) || got != (int)(12 + n) ? -1 : 0;
}

/* Reads one container into dst (cap bytes, a multiple of ALIGN). Returns its length or -1. */
static long recv_container(ptp_t *p, uint8_t *dst, uint32_t cap)
{
    uint32_t have = 0, len;
    int got;
    while (have < 12) { /* also swallows the zero-length packet that ends an exact-multiple transfer */
        if (p->bulk(p->ctx, p->ep_in, dst + have, (int)(cap - have), &got, TIMEOUT_MS))
            return -1;
        have += (uint32_t)got;
    }
    len = rd32(dst);
    if (len < 12 || len > cap)
        return -1;
    while (have < len) {
        if (p->bulk(p->ctx, p->ep_in, dst + have, (int)(cap - have), &got, TIMEOUT_MS))
            return -1;
        have += (uint32_t)got;
    }
    return (long)len;
}

int ptp_transact(ptp_t *p, uint16_t code, const uint32_t *params, int nparams,
                 const uint8_t *out, uint32_t outlen, const uint8_t **in, uint32_t *inlen)
{
    uint8_t args[20] = {0};
    uint32_t off = 0;
    long len;
    int i;

    if (nparams > 5)
        return -1;
    if (p->op) {
        int rc = p->op(p, code, params, nparams, out, outlen, in, inlen);
        if (rc < 0)
            p->dead = -1;
        return rc;
    }
    for (i = 0; i < nparams; i++)
        wr32(args + 4 * i, params[i]);
    if (in) {
        *in = NULL;
        *inlen = 0;
    }
    if (send_container(p, CMD, code, args, 4u * (uint32_t)nparams) ||
        (out && send_container(p, DATA, code, out, outlen)) ||
        (len = recv_container(p, p->buf, PTP_BUF)) < 0)
        return p->dead = -1;
    if (rd16(p->buf + 4) == DATA) {
        if (in) {
            *in = p->buf + 12;
            *inlen = (uint32_t)len - 12;
        }
        off = ((uint32_t)len + ALIGN - 1) / ALIGN * ALIGN; /* response goes after the data, not over it */
        if (off + ALIGN > PTP_BUF || (len = recv_container(p, p->buf + off, PTP_BUF - off)) < 0)
            return p->dead = -1;
    }
    p->tid++; /* OpenSession must use transaction id 0, so count up after each operation */
    if (debug())
        fprintf(stderr, "ptp: op 0x%04x -> 0x%04x\n", code, rd16(p->buf + off + 4) == RESP ? rd16(p->buf + off + 6) : 0);
    return rd16(p->buf + off + 4) == RESP ? rd16(p->buf + off + 6) : -1;
}

/* ---- property tables -------------------------------------------------------------------------- */

static ptp_prop_t *prop_slot(ptp_t *p, uint16_t code)
{
    ptp_prop_t *r;
    if (!p->props && !(p->props = calloc(PTP_MAX_PROPS, sizeof *p->props)))
        return NULL;
    for (int i = 0; i < p->nprops; i++)
        if (p->props[i].code == code)
            return &p->props[i];
    if (p->nprops == PTP_MAX_PROPS)
        return NULL;
    r = &p->props[p->nprops++];
    memset(r, 0, sizeof *r);
    r->code = code;
    return r;
}

const ptp_prop_t *eos_prop(const ptp_t *p, uint16_t code)
{
    for (int i = 0; i < p->nprops; i++)
        if (p->props[i].code == code)
            return &p->props[i];
    return NULL;
}

/* GetEvent payload: [u32 size][u32 type][...] records, ending with an 8-byte record of type 0.
   Value records carry [u32 property][u32 value]; list records [u32 property][u32 datatype][u32 count]
   then count 4-byte values. The first GetEvent after SetEventMode reports every property. */
void canon_parse_events(ptp_t *p, const uint8_t *d, uint32_t n)
{
    uint32_t off = 0;
    int changed = 0;
    while (n - off >= 8) {
        const uint32_t size = rd32(d + off), type = rd32(d + off + 4);
        const uint8_t *r = d + off + 8;
        ptp_prop_t *pp;
        if (size < 8 || size > n - off || (type == 0 && size == 8))
            break;
        if (type == EV_PROP_VALUE && size >= 16 && rd32(r) <= 0xFFFF && (pp = prop_slot(p, (uint16_t)rd32(r)))) {
            pp->value = rd32(r + 4);
            changed = 1;
        } else if (type == EV_PROP_AVAIL && size >= 20 && rd32(r) <= 0xFFFF && rd32(r + 8) <= (size - 20) / 4 &&
                   (pp = prop_slot(p, (uint16_t)rd32(r)))) {
            const uint32_t cnt = rd32(r + 8);
            pp->type = (uint16_t)rd32(r + 4);
            pp->form = 2;
            pp->nopts = cnt < PTP_MAX_OPTS ? (int)cnt : PTP_MAX_OPTS;
            for (int i = 0; i < pp->nopts; i++)
                pp->opts[i] = rd32(r + 12 + 4 * (uint32_t)i);
            changed = 1;
        }
        off += size;
    }
    p->props_seq += changed;
}

/* PTP datatypes 1..10 are INT8, UINT8, INT16 ... UINT128; 0x4000 | t is an array of t; 0xFFFF a string. */
static uint32_t scalar_size(uint16_t t) { return t >= 1 && t <= 10 ? 1u << ((t - 1) / 2) : 0; }

/* Bytes one value of type t takes at d (n available); 0 if it doesn't fit or the type is unknown. */
static uint32_t value_size(const uint8_t *d, uint32_t n, uint16_t t)
{
    uint32_t s;
    if ((s = scalar_size(t)))
        return s <= n ? s : 0;
    if (t == 0xFFFF) /* [u8 UTF-16 code units incl. NUL][...] */
        return n >= 1 && 1u + 2u * d[0] <= n ? 1u + 2u * d[0] : 0;
    if ((t & 0xFF00) == 0x4000 && (s = scalar_size(t & 0xFF)) && n >= 4 && rd32(d) <= (n - 4) / s)
        return 4 + rd32(d) * s;
    return 0;
}

static int64_t value_of(const uint8_t *d, uint16_t t)
{
    switch (t) {
    case 1: return (int8_t)d[0];
    case 2: return d[0];
    case 3: return (int16_t)rd16(d);
    case 4: return rd16(d);
    case 5: return (int32_t)rd32(d);
    case 6: return rd32(d);
    case 7: case 8: case 9: case 10: return (int64_t)((uint64_t)rd32(d) | (uint64_t)rd32(d + 4) << 32);
    }
    return 0;
}

static int sony_header_ok(const uint8_t *d, uint32_t n)
{
    uint16_t c, t;
    if (n < 6)
        return 0;
    c = rd16(d), t = rd16(d + 2);
    return ((c & 0xF000) == 0x5000 || (c & 0xF000) == 0xD000) &&
           (scalar_size(t) || t == 0xFFFF || ((t & 0xFF00) == 0x4000 && scalar_size(t & 0xFF))) && d[4] <= 1 && d[5] <= 2;
}

/* [u16 count][count values] at d + o; returns the offset after it, 0 if it doesn't fit. Keeps the values in pr. */
static uint32_t enum_list(const uint8_t *d, uint32_t n, uint32_t o, uint16_t t, ptp_prop_t *pr)
{
    uint32_t cnt, s;
    if (n - o < 2)
        return 0;
    cnt = rd16(d + o);
    o += 2;
    for (uint32_t i = 0; i < cnt; i++, o += s) {
        if (!(s = value_size(d + o, n - o, t)))
            return 0;
        if (pr && scalar_size(t) && pr->nopts < PTP_MAX_OPTS)
            pr->opts[pr->nopts++] = value_of(d + o, t);
    }
    return o;
}

/* One property description: [u16 code][u16 type][u8 get/set][u8 enabled][factory value][current value]
   [u8 form][form data]. Returns its length, 0 if it doesn't parse. */
static uint32_t sony_parse_prop(ptp_t *p, const uint8_t *d, uint32_t n)
{
    uint32_t o = 6, s, end;
    ptp_prop_t pr, *slot;
    uint16_t t;
    if (!sony_header_ok(d, n))
        return 0;
    memset(&pr, 0, sizeof pr);
    pr.code = rd16(d);
    pr.type = t = rd16(d + 2);
    pr.ro = !(d[4] == 1 && d[5] == 1);
    if (!(s = value_size(d + o, n - o, t))) /* factory default */
        return 0;
    o += s;
    if (!(s = value_size(d + o, n - o, t)))
        return 0;
    pr.value = scalar_size(t) ? value_of(d + o, t) : 0;
    o += s;
    if (o >= n)
        return 0;
    pr.form = d[o++];
    if (pr.form == 1) {
        s = scalar_size(t);
        if (!s || n - o < 3 * s)
            return 0;
        pr.min = value_of(d + o, t), pr.max = value_of(d + o + s, t), pr.step = value_of(d + o + 2 * s, t);
        o += 3 * s;
    } else if (pr.form == 2) {
        if (!(o = enum_list(d, n, o, t, &pr)))
            return 0;
        /* Protocol 3.00 bodies follow with a second list (the values settable right now); older ones don't.
           Skip it only when what comes after it, and not what comes after the first, parses as a property. */
        if (o < n && !sony_header_ok(d + o, n - o)) {
            if (!(end = enum_list(d, n, o, t, NULL)) || (end < n && !sony_header_ok(d + end, n - end)))
                return 0;
            o = end;
        }
    } else if (pr.form) {
        return 0;
    }
    if (scalar_size(t) && scalar_size(t) <= 8 && (slot = prop_slot(p, pr.code)))
        *slot = pr;
    return o;
}

/* 0x9209 payload: [u64 count] then that many property descriptions. */
void sony_parse_props(ptp_t *p, const uint8_t *d, uint32_t n)
{
    uint32_t o = 8, s, count = n >= 4 ? rd32(d) : 0, got = 0;
    if (n > 8 && !sony_header_ok(d + 8, n - 8) && sony_header_ok(d + 4, n - 4))
        o = 4; /* in case a body sends a 32-bit count */
    for (; o < n && (s = sony_parse_prop(p, d + o, n - o)); o += s)
        got++;
    if (debug() && (o < n || got != count))
        fprintf(stderr, "ptp: sony properties: parsed %u of %u, stopped at byte %u of %u\n", got, count, o, n);
    p->props_seq++;
}

/* Sony values go out little-endian at their datatype's size. */
static int sony_send(ptp_t *p, uint16_t op, uint16_t code, uint16_t t, int64_t v)
{
    const uint32_t c = code, s = scalar_size(t);
    uint8_t d[8];
    if (!s || s > 8)
        return RC_UNSUPPORTED;
    for (uint32_t i = 0; i < s; i++)
        d[i] = (uint8_t)((uint64_t)v >> 8 * i);
    return ptp_transact(p, op, &c, 1, d, s, NULL, NULL);
}

int eos_set_prop(ptp_t *p, uint32_t prop, int64_t value)
{
    uint8_t d[12];
    if (p->sony) {
        const ptp_prop_t *pr = eos_prop(p, (uint16_t)prop);
        return pr ? sony_send(p, OP_SONY_SET_PROP, pr->code, pr->type, value) : RC_UNSUPPORTED;
    }
    wr32(d, 12);
    wr32(d + 4, prop);
    wr32(d + 8, (uint32_t)value);
    return ptp_transact(p, OP_EOS_SET_PROP, NULL, 0, d, sizeof d, NULL, NULL);
}

int eos_autofocus(ptp_t *p)
{
    int rc;
    if (p->sony) {
        if ((rc = sony_send(p, OP_SONY_CONTROL, SONY_S1, 4, 2)) == PTP_OK)
            p->af_hold = 2; /* let go after a poll or two (1-2 s), so focusing can finish */
        return rc;
    }
    ptp_transact(p, OP_EOS_AF_CANCEL, NULL, 0, NULL, 0, NULL, NULL); /* end an earlier run, or DoAf may not refocus */
    return ptp_transact(p, OP_EOS_DO_AF, NULL, 0, NULL, 0, NULL, NULL);
}

int eos_focus(ptp_t *p, int step)
{
    static const int sony_step[4] = {0, 1, 3, 7}; /* Sony takes -7..7 */
    uint32_t arg;
    if (!step || step < -3 || step > 3)
        return RC_UNSUPPORTED;
    if (p->sony)
        return sony_send(p, OP_SONY_CONTROL, SONY_NEAR_FAR, 3, step < 0 ? -sony_step[-step] : sony_step[step]);
    arg = step < 0 ? (uint32_t)-step : 0x8000u | (uint32_t)step;
    return ptp_transact(p, OP_EOS_DRIVE_LENS, &arg, 1, NULL, 0, NULL, NULL);
}

static int get_event(ptp_t *p)
{
    const uint8_t *d;
    uint32_t n;
    int rc = ptp_transact(p, OP_EOS_GET_EVENT, NULL, 0, NULL, 0, &d, &n);
    if (rc == PTP_OK && d)
        canon_parse_events(p, d, n);
    return rc;
}

/* Sony PC Remote handshake. Live view then runs on its own; frames are read as an object. */
static int sony_start(ptp_t *p)
{
    uint32_t a[3] = {1, 0, 0}, ver = 0x12C;
    const uint8_t *d;
    uint32_t n;
    int rc;
    for (a[0] = 1; a[0] <= 2; a[0]++)
        if ((rc = ptp_transact(p, OP_SONY_CONNECT, a, 3, NULL, 0, &d, &n)) != PTP_OK)
            return rc;
    if ((rc = ptp_transact(p, OP_SONY_EXT_INFO, &ver, 1, NULL, 0, &d, &n)) != PTP_OK) {
        ver = 0xC8; /* older bodies only speak 2.00 */
        if ((rc = ptp_transact(p, OP_SONY_EXT_INFO, &ver, 1, NULL, 0, &d, &n)) != PTP_OK)
            return rc;
    }
    a[0] = 3;
    return ptp_transact(p, OP_SONY_CONNECT, a, 3, NULL, 0, &d, &n);
}

int eos_start_liveview(ptp_t *p)
{
    const uint32_t one = 1;
    int rc = p->op ? RC_SESSION_OPEN /* the Windows MTP driver owns the session */
                   : ptp_transact(p, OP_OPEN_SESSION, &one, 1, NULL, 0, NULL, NULL);
    if (rc != PTP_OK && rc != RC_SESSION_OPEN)
        return rc;
    if (p->sony)
        return sony_start(p);
    if ((rc = ptp_transact(p, OP_EOS_REMOTE_MODE, &one, 1, NULL, 0, NULL, NULL)) != PTP_OK)
        return rc;
    if ((rc = ptp_transact(p, OP_EOS_EVENT_MODE, &one, 1, NULL, 0, NULL, NULL)) != PTP_OK)
        return rc;
    if ((rc = get_event(p)) != PTP_OK)
        return rc;
    eos_set_prop(p, PROP_EVF_MODE, 1); /* needed by older bodies, absent on some newer ones */
    return eos_set_prop(p, PROP_EVF_OUTPUT, EVF_PC);
}

int eos_poll(ptp_t *p)
{
    if (p->sony) { /* reading the property block is Sony's keep-alive and connection check */
        const uint8_t *d;
        uint32_t n;
        int rc;
        if (p->af_hold && !--p->af_hold)
            sony_send(p, OP_SONY_CONTROL, SONY_S1, 4, 1);
        if ((rc = ptp_transact(p, OP_SONY_ALL_PROPS, NULL, 0, NULL, 0, &d, &n)) == PTP_OK && d)
            sony_parse_props(p, d, n);
        return rc;
    }
    int rc = get_event(p);
    if (rc < 0)
        return rc;
    ptp_transact(p, OP_EOS_KEEP_ON, NULL, 0, NULL, 0, NULL, NULL); /* unsupported on some bodies; ignore */
    return rc;
}

const uint8_t *eos_find_jpeg(const uint8_t *d, uint32_t n, uint32_t *jlen)
{
    uint32_t i = 0, len, type;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) {
        *jlen = n;
        return d;
    }
    /* GetViewFinderData payload: [u32 length][u32 type][payload] blocks; types 1/9/11 carry the JPEG */
    while (n - i >= 8) {
        len = rd32(d + i);
        type = rd32(d + i + 4);
        if (len < 8 || len > n - i)
            break;
        if (type == 1 || type == 9 || type == 11) {
            *jlen = len - 8;
            return d + i + 8;
        }
        i += len;
    }
    return NULL;
}

/* Sony live-view object: [u32 JPEG offset][u32 JPEG size] ... JPEG. If that header doesn't check out,
   falls back to the first SOI through the last EOI. */
const uint8_t *sony_find_jpeg(const uint8_t *d, uint32_t n, uint32_t *jlen)
{
    if (n >= 8) {
        uint32_t off = rd32(d), len = rd32(d + 4);
        if (off >= 8 && off < n && len >= 4 && len <= n - off && d[off] == 0xFF && d[off + 1] == 0xD8) {
            *jlen = len;
            return d + off;
        }
    }
    for (uint32_t i = 0; i + 1 < n; i++)
        if (d[i] == 0xFF && d[i + 1] == 0xD8) {
            uint32_t e = n;
            while (e >= i + 4 && !(d[e - 2] == 0xFF && d[e - 1] == 0xD9))
                e--;
            if (e < i + 4)
                return NULL;
            *jlen = e - i;
            return d + i;
        }
    return NULL;
}

int eos_frame(ptp_t *p, const uint8_t **jpeg, uint32_t *jlen)
{
    static const uint32_t args[3] = {0x00100000, 0, 0}, sony_lv = SONY_LIVEVIEW;
    const uint8_t *d;
    uint32_t n;
    int rc = p->sony ? ptp_transact(p, OP_GET_OBJECT, &sony_lv, 1, NULL, 0, &d, &n)
                     : ptp_transact(p, OP_EOS_VIEWFINDER, args, 3, NULL, 0, &d, &n);
    if (rc == RC_NOT_READY || rc == RC_BUSY || rc == RC_ACCESS_DENIED) /* Sony: denied until live view is up */
        return 0;
    if (rc != PTP_OK)
        return -1;
    return (*jpeg = p->sony ? sony_find_jpeg(d, n, jlen) : eos_find_jpeg(d, n, jlen)) != NULL;
}

/* ---- names and value labels --------------------------------------------------------------------- */

static const struct {
    uint16_t code;
    uint8_t sony;
    const char *name, *label;
} names[] = {
    {0xD101, 0, "av", "Aperture"},         {0xD102, 0, "tv", "Shutter"},
    {0xD103, 0, "iso", "ISO"},             {0xD104, 0, "ec", "Exposure comp."},
    {0xD105, 0, "mode", "Shooting mode"},  {0xD109, 0, "wb", "White balance"},
    {0xD10A, 0, "kelvin", "Colour temp."}, {0xD110, 0, "style", "Picture style"},
    {0x5007, 1, "av", "Aperture"},         {0xD20D, 1, "tv", "Shutter"},
    {0xD21E, 1, "iso", "ISO"},             {0x5010, 1, "ec", "Exposure comp."},
    {0x5005, 1, "wb", "White balance"},
};

long eos_setting(int sony, const char *name)
{
    char *end;
    long v;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].sony == !!sony && !strcmp(name, names[i].name))
            return names[i].code;
    v = strtol(name + (name[0] == 'p' && name[1] == '_' ? 2 : 0), &end, 16);
    return *end || v <= 0 || v > 0xFFFF ? -1 : v;
}

const char *eos_prop_name(int sony, uint16_t code, char buf[8])
{
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].sony == !!sony && names[i].code == code)
            return names[i].name;
    snprintf(buf, 8, "p_%04X", code);
    return buf;
}

const char *eos_prop_label(int sony, uint16_t code)
{
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].sony == !!sony && names[i].code == code)
            return names[i].label;
    return NULL;
}

/* Cameras step in thirds or halves of a stop; show the marked value nearest the exact one. */
static double nearest(double x, const double *t, size_t n)
{
    double best = t[0];
    for (size_t i = 1; i < n; i++)
        if (fabs(log(x / t[i])) < fabs(log(x / best)))
            best = t[i];
    return best;
}
#define NEAREST(x, t) nearest(x, t, sizeof t / sizeof t[0])

static const double iso_t[] = {50, 64, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000,
                               2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800, 16000, 20000, 25600, 32000, 40000,
                               51200, 64000, 80000, 102400, 204800, 409600};
static const double f_t[] = {1.0, 1.1, 1.2, 1.4, 1.6, 1.8, 2, 2.2, 2.5, 2.8, 3.2, 3.5, 4, 4.5, 5, 5.6, 6.3, 6.7, 7.1, 8,
                             9, 9.5, 10, 11, 13, 14, 16, 18, 19, 20, 22, 25, 27, 29, 32, 36, 40, 45, 51, 57, 64};
static const double sec_t[] = {0.3, 0.4, 0.5, 0.6, 0.8, 1, 1.3, 1.6, 2, 2.5, 3.2, 4, 5, 6, 8, 10, 13, 15, 20, 25, 30};
static const double den_t[] = {4, 5, 6, 8, 10, 13, 15, 20, 25, 30, 40, 45, 50, 60, 80, 90, 100, 125, 160, 180, 200, 250,
                               320, 350, 400, 500, 640, 750, 800, 1000, 1250, 1500, 1600, 2000, 2500, 3000, 3200,
                               4000, 5000, 6000, 6400, 8000, 10000, 12800, 16000, 20000, 25600, 32000};

typedef struct {
    uint16_t v;
    const char *s;
} name_t;

static const char *lookup(const name_t *t, size_t n, int64_t v)
{
    for (size_t i = 0; i < n; i++)
        if (t[i].v == v)
            return t[i].s;
    return NULL;
}
#define LOOKUP(t, v) lookup(t, sizeof t / sizeof t[0], v)

void eos_value_label(int sony, uint16_t code, int64_t raw, char *b, size_t n)
{
    /* ponytail: the common values only; anything else shows as hex, add names as bodies report them */
    static const name_t canon_wb[] = {{0, "Auto"}, {1, "Daylight"}, {2, "Cloudy"}, {3, "Tungsten"}, {4, "Fluorescent"},
                                      {5, "Flash"}, {6, "Custom"}, {8, "Shade"}, {9, "Colour temp."}};
    static const name_t canon_style[] = {{0x81, "Standard"}, {0x82, "Portrait"}, {0x83, "Landscape"}, {0x84, "Neutral"},
                                         {0x85, "Faithful"}, {0x86, "Monochrome"}, {0x87, "Auto"}, {0x88, "Fine detail"},
                                         {0x21, "User 1"}, {0x22, "User 2"}, {0x23, "User 3"}};
    static const name_t canon_mode[] = {{0, "P"}, {1, "Tv"}, {2, "Av"}, {3, "M"}, {4, "Bulb"}, {20, "Movie"}};
    static const name_t ptp_wb[] = {{1, "Manual"}, {2, "Auto"}, {3, "One-push auto"}, {4, "Daylight"},
                                    {5, "Fluorescent"}, {6, "Tungsten"}, {7, "Flash"}}; /* PTP standard values */
    const uint32_t v = (uint32_t)raw;
    const char *s = NULL;
    double t;

    if (!sony) {
        switch (code) {
        case 0xD103: /* eighths of a stop, 0x48 = ISO 100 */
            if (!v)
                snprintf(b, n, "Auto");
            else
                snprintf(b, n, "%g", NEAREST(100 * pow(2, ((int)v - 0x48) / 8.0), iso_t));
            return;
        case 0xD101: /* sixteenths of a stop of f-number, 0x08 = f/1.0 */
            snprintf(b, n, "f/%g", NEAREST(pow(2, ((int)v - 8) / 16.0), f_t));
            return;
        case 0xD102: /* eighths of a stop, 0x38 = 1 s, 0x0C = bulb */
            if (v == 0x0C) {
                snprintf(b, n, "Bulb");
                return;
            }
            t = pow(2, (0x38 - (int)v) / 8.0);
            if (t >= 0.3)
                snprintf(b, n, "%g\"", NEAREST(t, sec_t));
            else
                snprintf(b, n, "1/%g", NEAREST(1 / t, den_t));
            return;
        case 0xD104: { /* signed eighths of a stop: 3 = 1/3, 4 = 1/2, 5 = 2/3 */
            const int e = (int8_t)v, a = abs(e), f = a % 8;
            const double st = a / 8 + (f == 3 ? 1 / 3. : f == 4 ? 0.5 : f == 5 ? 2 / 3. : f / 8.);
            snprintf(b, n, "%+.1f", e < 0 ? -st : st);
            return;
        }
        case 0xD10A:
            snprintf(b, n, "%u K", v);
            return;
        case 0xD109: s = LOOKUP(canon_wb, v); break;
        case 0xD110: s = LOOKUP(canon_style, v); break;
        case 0xD105: s = LOOKUP(canon_mode, v); break;
        }
    } else {
        switch (code) {
        case 0xD21E: /* low 24 bits, all ones = auto */
            if ((v & 0xFFFFFF) == 0xFFFFFF)
                snprintf(b, n, "Auto");
            else
                snprintf(b, n, "%u", v & 0xFFFFFF);
            return;
        case 0x5007: /* f-number x 100 */
            snprintf(b, n, "f/%g", v / 100.0);
            return;
        case 0xD20D: /* numerator << 16 | denominator, 0 = bulb */
            if (!v) {
                snprintf(b, n, "Bulb");
                return;
            }
            if (v >> 16 == 1 && (v & 0xFFFF) > 1) {
                snprintf(b, n, "1/%u", v & 0xFFFF);
                return;
            }
            if (v & 0xFFFF) {
                snprintf(b, n, "%g\"", (double)(v >> 16) / (v & 0xFFFF));
                return;
            }
            break;
        case 0x5010: /* thousandths of a stop */
            snprintf(b, n, "%+.1f", (int16_t)v / 1000.0);
            return;
        case 0x5005: s = LOOKUP(ptp_wb, v); break;
        }
    }
    if (s)
        snprintf(b, n, "%s", s);
    else
        snprintf(b, n, "0x%llX", (unsigned long long)raw);
}

static int usb_bulk(void *ctx, uint8_t ep, uint8_t *buf, int len, int *got, unsigned timeout_ms)
{
    int err = libusb_bulk_transfer(ctx, ep, buf, len, got, timeout_ms);
    if (err && debug())
        fprintf(stderr, "ptp: bulk ep 0x%02x, %d bytes: %s\n", ep, len, libusb_strerror(err));
    return err;
}

uint16_t eos_only_vid;
const char *eos_only_serial;
static libusb_context *usb;

int eos_serial_ok(const char *serial)
{
    const char *a = serial, *b = eos_only_serial;
    if (!b)
        return 1;
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return !*a && !*b;
}

static void report(const char *what, int err)
{
    static int last;
    if (err == last)
        return; /* the reconnect loop retries every few seconds; say each problem once */
    last = err;
#ifdef _WIN32
    if (err == LIBUSB_ERROR_NOT_SUPPORTED || err == LIBUSB_ERROR_NOT_FOUND)
        return; /* MTP-bound camera: handled by the WPD transport instead */
#endif
    fprintf(stderr, "marksman-cam: %s: %s\n", what, libusb_strerror(err));
    if (err == LIBUSB_ERROR_BUSY || err == LIBUSB_ERROR_ACCESS)
        fprintf(stderr, "marksman-cam: camera held by another program (gvfs, gphoto2, camera maker software) or no permission (see README)\n");
}

/* A session cut off mid-transaction (USB glitch, another device enumerating on the same hub, the
   service restarting) leaves its reply queued on the IN endpoint, and the camera then refuses every
   new command (the first bulk OUT times out) until that is read. Read it away before the first command;
   without this only a power cycle brought the camera back. Costs one short timeout on a clean camera. */
static void drain(ptp_t *p)
{
    int got, n = 0;
    while (n < 64 && !libusb_bulk_transfer(p->ctx, p->ep_in, p->buf, PTP_BUF, &got, 200))
        n++;
    if (n && debug())
        fprintf(stderr, "ptp: discarded %d stale transfer(s) from an earlier session\n", n);
}

/* The still-image (PTP) interface of a Canon/Sony device and its bulk endpoints; 0 if it has none.
   A Sony in Mass Storage/MTP mode has no such interface. */
static int ptp_iface(libusb_device *dev, struct libusb_device_descriptor *dd, int *ifnum, uint8_t *in, uint8_t *out)
{
    struct libusb_config_descriptor *cfg;
    int found = 0;
    if (libusb_get_device_descriptor(dev, dd) || (dd->idVendor != CANON_VID && dd->idVendor != SONY_VID) ||
        libusb_get_active_config_descriptor(dev, &cfg))
        return 0;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[0];
        *in = *out = 0;
        if (id->bInterfaceClass != LIBUSB_CLASS_IMAGE)
            continue;
        for (int e = 0; e < id->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
            if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                *in = ep->bEndpointAddress;
            else
                *out = ep->bEndpointAddress;
        }
        *ifnum = id->bInterfaceNumber;
        found = *in && *out;
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

static void usb_string(libusb_device_handle *h, uint8_t index, char *buf, int n)
{
    buf[0] = 0;
    if (index && libusb_get_string_descriptor_ascii(h, index, (unsigned char *)buf, n) < 0)
        buf[0] = 0;
}

static ptp_t *try_device(libusb_device *dev)
{
    struct libusb_device_descriptor dd;
    libusb_device_handle *h;
    char serial[128];
    uint8_t in, out;
    int ifnum, err;
    ptp_t *p;

    if (!ptp_iface(dev, &dd, &ifnum, &in, &out) || (eos_only_vid && dd.idVendor != eos_only_vid))
        return NULL;
    if ((err = libusb_open(dev, &h))) {
        report("open", err);
        return NULL;
    }
    if (eos_only_serial) {
        usb_string(h, dd.iSerialNumber, serial, sizeof serial);
        if (!eos_serial_ok(serial)) {
            libusb_close(h);
            return NULL;
        }
    }
    libusb_set_auto_detach_kernel_driver(h, 1); /* not supported on Windows; harmless */
    if ((err = libusb_claim_interface(h, ifnum))) {
        report("claim", err);
        libusb_close(h);
        return NULL;
    }
    if (!(p = calloc(1, sizeof *p)) || !(p->buf = malloc(PTP_BUF))) {
        free(p);
        libusb_close(h);
        return NULL;
    }
    p->bulk = usb_bulk;
    p->ctx = h;
    p->ep_in = in;
    p->ep_out = out;
    p->sony = dd.idVendor == SONY_VID;
    drain(p);
    return p;
}

/* PTP class request GET_DEVICE_STATUS. A camera that answers "busy" here before any operation has
   gone through is wedged (seen on a Sony a6400 after another host's session) and needs a power cycle. */
const char *eos_hint(ptp_t *p)
{
    uint8_t st[16];
    if (p->op || p->bulk != usb_bulk)
        return NULL;
    if (libusb_control_transfer(p->ctx, 0xA1, 0x67, 0, 0, st, sizeof st, 1000) >= 4 && rd16(st + 2) == RC_BUSY)
        return "the camera reports itself busy over USB; switch it off and on again (or unplug it)";
    return NULL;
}

ptp_t *eos_open(void)
{
    libusb_device **list;
    ptp_t *p = NULL;
    ssize_t n;
#ifdef _WIN32
    if ((p = wpd_open()))
        return p;
#endif
    if (!usb && libusb_init(&usb))
        return NULL;
    if ((n = libusb_get_device_list(usb, &list)) < 0)
        return NULL;
    for (ssize_t i = 0; i < n && !p; i++)
        p = try_device(list[i]);
    libusb_free_device_list(list, 1);
    return p;
}

void eos_list(void)
{
    libusb_device **list;
    ssize_t n;
#ifdef _WIN32
    wpd_list();
#endif
    if ((!usb && libusb_init(&usb)) || (n = libusb_get_device_list(usb, &list)) < 0)
        return;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        libusb_device_handle *h;
        char product[128], serial[128];
        uint8_t in, out;
        int ifnum;
        if (!ptp_iface(list[i], &dd, &ifnum, &in, &out))
            continue;
        if (libusb_open(list[i], &h)) {
#ifndef _WIN32 /* on Windows these are the MTP-driver cameras already listed above */
            printf("%-5s  %04x:%04x  (can't open: in use, or no permission)\n", dd.idVendor == SONY_VID ? "sony" : "canon",
                   dd.idVendor, dd.idProduct);
#endif
            continue;
        }
        usb_string(h, dd.iProduct, product, sizeof product);
        usb_string(h, dd.iSerialNumber, serial, sizeof serial);
        libusb_close(h);
        printf("%-5s  %04x:%04x  %-28s  --serial %s\n", dd.idVendor == SONY_VID ? "sony" : "canon", dd.idVendor,
               dd.idProduct, product, serial[0] ? serial : "(none)");
    }
    libusb_free_device_list(list, 1);
}

#ifdef _WIN32
void eos_wait_device(double seconds)
{
    Sleep((DWORD)(seconds * 1000));
}
#else
#include <sys/time.h>
#include <time.h>

static volatile int arrived;

static int LIBUSB_CALL on_arrival(libusb_context *c, libusb_device *dev, libusb_hotplug_event e, void *u)
{
    struct libusb_device_descriptor dd;
    (void)c, (void)e, (void)u;
    if (!libusb_get_device_descriptor(dev, &dd) && (dd.idVendor == CANON_VID || dd.idVendor == SONY_VID))
        arrived = 1;
    return 0; /* stay registered */
}

/* Waits up to `seconds`, returning as soon as a Canon/Sony device appears instead of polling the bus. */
void eos_wait_device(double seconds)
{
    static int hotplug = -1;
    struct timespec t0, t;
    struct timeval tv = {0, 200000};
    if (hotplug < 0)
        hotplug = (usb || !libusb_init(&usb)) && libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) &&
                  !libusb_hotplug_register_callback(usb, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, 0, LIBUSB_HOTPLUG_MATCH_ANY,
                                                    LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, on_arrival, NULL, NULL);
    if (!hotplug) {
        t.tv_sec = (time_t)seconds;
        t.tv_nsec = (long)((seconds - (double)t.tv_sec) * 1e9);
        nanosleep(&t, NULL);
        return;
    }
    arrived = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        libusb_handle_events_timeout_completed(usb, &tv, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (!arrived && (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 < seconds);
    if (arrived) { /* udev grants the logged-in user access just after the device appears */
        t.tv_sec = 0;
        t.tv_nsec = 500000000; /* ponytail: fixed 0.5 s; retry on EACCES instead if a slow machine needs longer */
        nanosleep(&t, NULL);
    }
}
#endif

void eos_close(ptp_t *p)
{
    const uint32_t zero = 0;
    if (!p->dead) {
        if (!p->sony) {
            eos_set_prop(p, PROP_EVF_OUTPUT, EVF_OFF);
            ptp_transact(p, OP_EOS_REMOTE_MODE, &zero, 1, NULL, 0, NULL, NULL);
        } else if (p->af_hold) {
            sony_send(p, OP_SONY_CONTROL, SONY_S1, 4, 1);
        }
        if (!p->op)
            ptp_transact(p, OP_CLOSE_SESSION, NULL, 0, NULL, 0, NULL, NULL);
    }
    if (p->release)
        p->release(p);
    else if (p->bulk == usb_bulk)
        libusb_close(p->ctx); /* also releases the claimed interface */
    free(p->props);
    free(p->buf);
    free(p);
}
