#include "ctl.h"

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define close_sock closesocket
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define close_sock close
#define SEND_FLAGS MSG_NOSIGNAL /* a client that hangs up must not kill us with SIGPIPE */
#endif

enum { T_RANGE, T_BOOL, T_MENU };

/* Picture controls applied by marksman-cam itself, fields of view_t. */
static const struct {
    const char *name, *label;
    int type, min, max, step;
    size_t off;
} soft[] = {
    {"brightness", "Brightness", T_RANGE, -100, 100, 1, offsetof(view_t, brightness)},
    {"contrast", "Contrast", T_RANGE, -100, 100, 1, offsetof(view_t, contrast)},
    {"saturation", "Saturation", T_RANGE, -100, 100, 1, offsetof(view_t, saturation)},
    {"hue", "Hue", T_RANGE, -180, 180, 1, offsetof(view_t, hue)},
    {"zoom", "Digital zoom %", T_RANGE, 100, 400, 10, offsetof(view_t, zoom)},
    {"mirror", "Mirror", T_BOOL, 0, 1, 1, offsetof(view_t, mirror)},
    {"flip", "Flip", T_BOOL, 0, 1, 1, offsetof(view_t, flip)},
    {"fill", "Fill frame", T_BOOL, 0, 1, 1, offsetof(view_t, fill)},
    {"rotate", "Rotate", T_MENU, 0, 270, 90, offsetof(view_t, rotate)},
};
#define NSOFT (sizeof soft / sizeof soft[0])
#define FIELD(v, i) (*(int *)((char *)(v) + soft[i].off))
#define CFIELD(v, i) (*(const int *)((const char *)(v) + soft[i].off))

enum { CMD_PROP, CMD_AF, CMD_FOCUS };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static view_t view;
static int view_dirty;
static int cam_on, cam_sony;
static ptp_prop_t props[PTP_MAX_PROPS]; /* snapshot of the open camera's properties */
static int nprops;
static const ptp_t *props_from;
static unsigned props_seen;
typedef struct {
    int kind;
    uint16_t code;
    int64_t value;
} cmd_t;
static cmd_t queue[16];
static int nqueue;

void ctl_set_view(const view_t *v)
{
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < NSOFT; i++)
        FIELD(&view, i) = CFIELD(v, i);
    pthread_mutex_unlock(&lock);
}

int ctl_take_view(view_t *v)
{
    int changed;
    pthread_mutex_lock(&lock);
    if ((changed = view_dirty))
        for (size_t i = 0; i < NSOFT; i++)
            FIELD(v, i) = FIELD(&view, i);
    view_dirty = 0;
    pthread_mutex_unlock(&lock);
    return changed;
}

void ctl_camera(ptp_t *cam)
{
    int n, rc;
    char name[8];
    cmd_t q[16];

    pthread_mutex_lock(&lock);
    cam_on = cam != NULL;
    if (!cam) {
        nprops = 0;
        props_from = NULL;
    } else if (cam != props_from || cam->props_seq != props_seen) {
        cam_sony = cam->sony;
        nprops = cam->nprops;
        if (nprops)
            memcpy(props, cam->props, (size_t)nprops * sizeof props[0]);
        props_from = cam;
        props_seen = cam->props_seq;
    }
    n = nqueue;
    memcpy(q, queue, (size_t)n * sizeof q[0]);
    nqueue = 0;
    pthread_mutex_unlock(&lock);

    for (int i = 0; cam && i < n; i++) {
        rc = q[i].kind == CMD_AF ? eos_autofocus(cam) : q[i].kind == CMD_FOCUS ? eos_focus(cam, (int)q[i].value)
                                                                               : eos_set_prop(cam, q[i].code, q[i].value);
        if (rc != PTP_OK) {
            fprintf(stderr, "marksman-cam: camera refused %s (0x%04x)\n",
                    q[i].kind == CMD_AF ? "autofocus" : q[i].kind == CMD_FOCUS ? "focus" : eos_prop_name(cam->sony, q[i].code, name), rc);
        } else if (q[i].kind == CMD_PROP) { /* show it now; the camera confirms with its next event or property block */
            for (int k = 0; k < cam->nprops; k++)
                if (cam->props[k].code == q[i].code) {
                    cam->props[k].value = q[i].value;
                    cam->props_seq++;
                }
        }
    }
}

/* ---- replies ------------------------------------------------------------------------------------ */

typedef struct {
    char *b;
    size_t n, cap;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(o->b ? o->b + o->n : NULL, o->b ? o->cap - o->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0)
            return;
        if (o->b && o->n + (size_t)k < o->cap) {
            o->n += (size_t)k;
            return;
        }
        char *b = realloc(o->b, o->cap = (o->cap + (size_t)k + 1) * 2);
        if (!b) {
            free(o->b);
            *o = (out_t){0};
            return;
        }
        o->b = b;
    }
}

static void put_str(out_t *o, const char *s) /* JSON string; our labels are ASCII, only quotes need escaping */
{
    put(o, "\"");
    for (; *s; s++)
        put(o, *s == '"' || *s == '\\' ? "\\%c" : (unsigned char)*s < 0x20 ? "?" : "%c", *s);
    put(o, "\"");
}

/* Camera properties worth showing: the named ones, and any with a list or range of values. */
static int shown(const ptp_prop_t *p)
{
    return p->form || eos_prop_label(cam_sony, p->code);
}

static void list(out_t *o)
{
    char name[8], label[48];
    int first = 1;
    put(o, "{\"make\":\"%s\",\"connected\":%s,\"controls\":[", cam_on ? (cam_sony ? "sony" : "canon") : "",
        cam_on ? "true" : "false");
    for (size_t i = 0; i < NSOFT; i++, first = 0) {
        const int v = CFIELD(&view, i);
        put(o, "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"picture\",\"type\":\"%s\",\"value\":%d", first ? "" : ",",
            soft[i].name, soft[i].label, soft[i].type == T_RANGE ? "range" : soft[i].type == T_BOOL ? "bool" : "menu",
            soft[i].off == offsetof(view_t, zoom) && v < 100 ? 100 : v);
        if (soft[i].type == T_RANGE)
            put(o, ",\"min\":%d,\"max\":%d,\"step\":%d", soft[i].min, soft[i].max, soft[i].step);
        if (soft[i].type == T_MENU)
            put(o, ",\"options\":[[0,\"0\"],[90,\"90\"],[180,\"180\"],[270,\"270\"]]");
        put(o, "}");
    }
    if (cam_on) {
        put(o, ",{\"name\":\"af\",\"label\":\"Autofocus\",\"group\":\"camera\",\"type\":\"button\"}");
        put(o, ",{\"name\":\"focus\",\"label\":\"Focus (near / far)\",\"group\":\"camera\",\"type\":\"step\",\"min\":-3,\"max\":3}");
    }
    for (int i = 0; cam_on && i < nprops; i++) {
        const ptp_prop_t *p = &props[i];
        const char *lab = eos_prop_label(cam_sony, p->code);
        const int kelvin = !cam_sony && p->code == 0xD10A; /* Canon reports no range for it */
        if (!shown(p))
            continue;
        if (!lab) {
            snprintf(label, sizeof label, "Property 0x%04X", p->code);
            lab = label;
        }
        put(o, ",{\"name\":\"%s\",\"label\":", eos_prop_name(cam_sony, p->code, name));
        put_str(o, lab);
        put(o, ",\"group\":\"camera\",\"ro\":%s,\"value\":%lld", p->ro ? "true" : "false", (long long)p->value);
        eos_value_label(cam_sony, p->code, p->value, label, sizeof label);
        put(o, ",\"text\":");
        put_str(o, label);
        if (p->form == 2) {
            put(o, ",\"type\":\"menu\",\"options\":[");
            for (int k = 0; k < p->nopts; k++) {
                eos_value_label(cam_sony, p->code, p->opts[k], label, sizeof label);
                put(o, "%s[%lld,", k ? "," : "", (long long)p->opts[k]);
                put_str(o, label);
                put(o, "]");
            }
            put(o, "]");
        } else if (p->form == 1 || kelvin) {
            put(o, ",\"type\":\"range\",\"min\":%lld,\"max\":%lld,\"step\":%lld", kelvin ? 2500LL : (long long)p->min,
                kelvin ? 10000LL : (long long)p->max, kelvin ? 100LL : p->step > 0 ? (long long)p->step : 1LL);
        } else {
            put(o, ",\"type\":\"info\"");
        }
        put(o, "}");
    }
    put(o, "]}\n");
}

static int enqueue(int kind, uint16_t code, int64_t value, out_t *o)
{
    if (!cam_on) {
        put(o, "error: no camera connected (it is only woken while an app is using the webcam)\n");
        return 0;
    }
    if (nqueue == (int)(sizeof queue / sizeof queue[0])) {
        put(o, "error: busy, try again\n");
        return 0;
    }
    queue[nqueue].kind = kind;
    queue[nqueue].code = code;
    queue[nqueue++].value = value;
    put(o, "ok\n");
    return 1;
}

/* Called with lock held. */
static void set(const char *name, const char *val, out_t *o)
{
    char *end;
    const long long x = strtoll(val, &end, 0);
    long code;

    if (!*val || *end) {
        put(o, "error: value must be a number\n");
        return;
    }
    for (size_t i = 0; i < NSOFT; i++)
        if (!strcmp(name, soft[i].name)) {
            if (x < soft[i].min || x > soft[i].max || x % soft[i].step) {
                put(o, "error: %s takes %d..%d in steps of %d\n", name, soft[i].min, soft[i].max, soft[i].step);
                return;
            }
            FIELD(&view, i) = (int)x;
            view_dirty = 1;
            put(o, "ok\n");
            return;
        }
    if (!strcmp(name, "af")) {
        enqueue(CMD_AF, 0, 0, o);
        return;
    }
    if (!strcmp(name, "focus")) {
        if (!x || x < -3 || x > 3)
            put(o, "error: focus takes -3..-1 (nearer) or 1..3 (farther)\n");
        else
            enqueue(CMD_FOCUS, 0, x, o);
        return;
    }
    if ((code = eos_setting(cam_sony, name)) >= 0)
        for (int i = 0; i < nprops; i++) {
            const ptp_prop_t *p = &props[i];
            int ok = 0;
            if (p->code != code)
                continue;
            if (p->form == 2)
                for (int k = 0; k < p->nopts && !ok; k++)
                    ok = p->opts[k] == x;
            else if (p->form == 1)
                ok = x >= p->min && x <= p->max;
            else if (!cam_sony && code == 0xD10A)
                ok = x >= 2500 && x <= 10000;
            if (!ok)
                put(o, "error: the camera doesn't offer %lld for %s right now\n", x, name);
            else
                enqueue(CMD_PROP, (uint16_t)code, x, o);
            return;
        }
    put(o, "error: unknown control %s (see: list)\n", name);
}

static void handle(const char *line, out_t *o)
{
    char verb[8], name[32], val[32], extra;
    const int n = sscanf(line, "%7s %31s %31s %c", verb, name, val, &extra);
    pthread_mutex_lock(&lock);
    if (n == 1 && !strcmp(verb, "list")) {
        list(o);
    } else if (n == 3 && !strcmp(verb, "set")) {
        set(name, val, o);
    } else {
        put(o, "error: commands are: list | set NAME VALUE\n");
    }
    pthread_mutex_unlock(&lock);
}

char *ctl_reply(const char *line)
{
    out_t o = {0};
    handle(line, &o);
    return o.b;
}

/* ---- socket ------------------------------------------------------------------------------------- */

static void send_all(sock_t c, const char *b, size_t n)
{
    while (n) {
        const int k = send(c, b, n > 65536 ? 65536 : (int)n, SEND_FLAGS);
        if (k <= 0)
            return;
        b += k, n -= (size_t)k;
    }
}

static void set_timeout(sock_t c, int seconds)
{
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = {seconds, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

/* Only the first line of a connection is read. A web page can make the browser POST to 127.0.0.1, but
   its first line is then "POST / HTTP/1.1", which is not a command; nothing in the body is ever read. */
static void *serve(void *arg)
{
    const sock_t ls = (sock_t)(intptr_t)arg;
    for (;;) {
        char line[512];
        int len = 0, k;
        out_t o = {0};
        sock_t c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) {
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100000);
#endif
            continue;
        }
        set_timeout(c, 2);
        while (len < (int)sizeof line - 1 && (k = recv(c, line + len, (int)sizeof line - 1 - len, 0)) > 0) {
            len += k;
            if (memchr(line, '\n', (size_t)len))
                break;
        }
        line[len] = 0;
        line[strcspn(line, "\r\n")] = 0;
        if ((o.b = ctl_reply(line)))
            send_all(c, o.b, strlen(o.b));
        free(o.b);
        close_sock(c);
    }
    return NULL;
}

static int net_init(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w);
#else
    return 0;
#endif
}

static struct sockaddr_in loopback(int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

int ctl_start(int port)
{
    const struct sockaddr_in a = loopback(port);
    const int one = 1;
    pthread_t t;
    sock_t s;

    if (net_init() || (s = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
        return -1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one); /* no other process may share it */
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); /* rebind straight after a restart */
#endif
    if (bind(s, (const struct sockaddr *)&a, sizeof a) || listen(s, 4) ||
        pthread_create(&t, NULL, serve, (void *)(intptr_t)s)) {
        close_sock(s);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

int ctl_client(int port, const char *cmd)
{
    const struct sockaddr_in a = loopback(port);
    char buf[4096];
    int k, failed = 0, first = 1;
    sock_t s;

    if (net_init() || (s = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
        return 1;
    if (connect(s, (const struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "marksman-cam: nothing is listening on 127.0.0.1:%d (is the service running?)\n", port);
        close_sock(s);
        return 1;
    }
    send_all(s, cmd, strlen(cmd));
    send_all(s, "\n", 1);
    set_timeout(s, 5);
    while ((k = recv(s, buf, sizeof buf, 0)) > 0) {
        if (first)
            failed = !strncmp(buf, "error", (size_t)(k < 5 ? k : 5));
        first = 0;
        fwrite(buf, 1, (size_t)k, stdout);
    }
    close_sock(s);
    return failed;
}
