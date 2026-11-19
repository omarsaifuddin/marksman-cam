/* marksman-cam: Canon EOS / Sony Alpha live view -> virtual webcam. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "ptp.h"
#include "sink.h"

#ifdef _WIN32
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void sleep_s(double s) { Sleep((DWORD)(s * 1000)); }
#else
#include <time.h>
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
static void sleep_s(double s)
{
    struct timespec t = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
    nanosleep(&t, NULL);
}
#endif

#define MAX_W 3840
#define MAX_H 2160
#define MAX_SETS 16

static const char usage[] =
    "usage: marksman-cam [options]\n"
    "  -d DEV            Linux: v4l2loopback device (/dev/video10); Windows: camera name (\"Marksman Cam\")\n"
    "  -s WxH            output size, even numbers (1280x720; on Windows the app using the camera picks)\n"
    "  -r FPS            output frame rate (30)\n"
    "  --fill            crop to fill the frame instead of letterboxing\n"
    "  --rotate DEG      0, 90, 180 or 270 clockwise\n"
    "  --mirror          flip left-right       --flip   flip upside down\n"
    "  --crop X0,Y0,X1,Y1  source region as fractions, e.g. 0.1,0,0.9,1\n"
    "  --brightness N  --contrast N  --saturation N   -100..100\n"
    "  --hue DEG\n"
    "  --set NAME=VALUE  camera property once connected; NAME is av tv iso ec wb kelvin style or a hex\n"
    "                    property code; VALUE is the Canon raw code (iso 0x48=100 .. 0x70=3200), kelvin in K\n"
    "  --camera canon|sony  use only that make when several cameras are plugged in (put it first)\n"
    "  --snapshot FILE   save one live-view JPEG and exit (camera check, no virtual camera needed)\n"
#ifdef _WIN32
    "  --install [opts]  (admin) permanent \"Marksman Cam\" + background service; opts become its settings\n"
    "  --uninstall       (admin) remove both\n"
#endif
    ;

#ifdef _WIN32
int winsvc_install(int argc, char **argv);
int winsvc_uninstall(void);
int winsvc_run(int (*body)(void), volatile sig_atomic_t *stop);
#endif

/* --snapshot: writes one live-view JPEG; checks the camera side without any virtual camera. */
static int snapshot(const char *path)
{
    const uint8_t *jpeg = NULL;
    uint32_t len = 0;
    uint8_t *rgb = NULL;
    size_t cap = 0;
    int r = 0, w = 0, h = 0, rc;
    ptp_t *cam = eos_open();
    FILE *f;

    if (!cam) {
        fprintf(stderr, "marksman-cam: no camera found (Canon EOS/PowerShot, or Sony in PC Remote mode)\n");
        return 1;
    }
    if ((rc = eos_start_liveview(cam)) != PTP_OK) {
        fprintf(stderr, "marksman-cam: live view start failed (0x%04x)\n", rc);
        eos_close(cam);
        return 1;
    }
    for (int i = 0; i < 100 && (r = eos_frame(cam, &jpeg, &len)) == 0; i++)
        sleep_s(0.05); /* the first frames take a moment after live view starts */
    if (r > 0 && (f = fopen(path, "wb"))) {
        fwrite(jpeg, 1, len, f);
        fclose(f);
        jpeg_decode(jpeg, len, &rgb, &cap, &w, &h);
        printf("wrote %s: %dx%d live view, %u bytes\n", path, w, h, (unsigned)len);
    } else {
        fprintf(stderr, "marksman-cam: %s\n", r < 0 ? "camera error" : r ? path : "no frame within 5 s");
    }
    eos_close(cam);
    free(rgb);
    return r > 0 ? 0 : 1;
}

/* ponytail: plain flags shared with the capture thread; fine on x86/ARM, make them _Atomic if a sanitizer complains */
static volatile sig_atomic_t stop;
static volatile int watched; /* an app is reading frames: only then is the camera woken */
static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/* Newest live-view JPEG, handed from the capture thread to the main loop. */
static struct {
    pthread_mutex_t lock;
    uint8_t *jpeg;
    uint32_t len, cap;
    unsigned seq;  /* bumped on every new frame and on connect/disconnect */
    int connected;
} latest = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0, 0};

static const char *sets[MAX_SETS];
static int nsets;

static void publish(const uint8_t *jpeg, uint32_t len, int connected)
{
    pthread_mutex_lock(&latest.lock);
    if (jpeg && len > latest.cap) {
        uint8_t *b = realloc(latest.jpeg, len);
        if (b) {
            latest.jpeg = b;
            latest.cap = len;
        }
    }
    if (jpeg && len <= latest.cap) {
        memcpy(latest.jpeg, jpeg, len);
        latest.len = len;
    }
    latest.connected = connected;
    latest.seq++;
    pthread_mutex_unlock(&latest.lock);
}

static void apply_settings(ptp_t *cam)
{
    for (int i = 0; i < nsets; i++) {
        char name[32];
        long prop;
        int rc;
        sscanf(sets[i], "%31[^=]", name);
        prop = eos_setting(name) >= 0 ? eos_setting(name) : strtol(name, NULL, 16);
        if ((rc = eos_set_prop(cam, (uint32_t)prop, (uint32_t)strtoul(strchr(sets[i], '=') + 1, NULL, 0))) != PTP_OK)
            fprintf(stderr, "marksman-cam: --set %s rejected (0x%04x)\n", sets[i], rc);
    }
}

/* Owns the camera and pulls frames back to back, so the USB side never waits for decoding or output:
   done serially on one thread, a 700D's 20 fps live view came out at 16. Live view runs only while
   `watched`; otherwise the camera is left alone (asleep, or free for other software). */
static void *capture(void *arg)
{
    (void)arg;
    while (!stop) {
        if (!watched) {
            sleep_s(0.1);
            continue;
        }
        static int waiting_said;
        ptp_t *cam = eos_open();
        double last_poll = now_s();
        int rc;
        if (!cam) {
            if (!waiting_said++)
                fprintf(stderr, "marksman-cam: an app wants the webcam; waiting for a camera on USB\n");
            sleep_s(2);
            continue;
        }
        waiting_said = 0;
        if ((rc = eos_start_liveview(cam)) != PTP_OK) {
            fprintf(stderr, "marksman-cam: live view start failed (0x%04x); is the camera in a shooting mode?\n", rc);
            eos_close(cam);
            sleep_s(2);
            continue;
        }
        fprintf(stderr, "marksman-cam: camera connected\n");
        apply_settings(cam);
        for (;;) {
            const uint8_t *jpeg;
            uint32_t len;
            int r = stop || !watched ? -1 : eos_frame(cam, &jpeg, &len);
            if (r > 0)
                publish(jpeg, len, 1);
            else if (r == 0)
                sleep_s(0.002); /* "not ready" is cheap and the camera sets the pace: poll gap made no difference */
            if (r >= 0 && now_s() - last_poll > 1) {
                last_poll = now_s();
                r = eos_poll(cam) < 0 ? -1 : r;
            }
            if (r < 0)
                break;
        }
        if (!stop)
            fprintf(stderr, watched ? "marksman-cam: camera disconnected\n" : "marksman-cam: no app watching, camera idle\n");
        eos_close(cam);
        publish(NULL, 0, 0);
    }
    return NULL;
}

static view_t v = {1280, 720, 0, 0, 0, 0, {0, 0, 1, 1}, 0, 0, 0, 0};
static int fps = 30;

static int run(sink_t *sink)
{
    uint8_t *canvas = malloc((size_t)MAX_W * MAX_H * 4), *rgb = NULL, *jpeg = NULL;
    size_t rgb_cap = 0, jpeg_cap = 0;
    uint32_t jpeg_len = 0;
    unsigned seen = 0;
    int sw = 0, sh = 0, have = 0, dirty = 1, last_w = 0, last_h = 0;
    double next = now_s(), t;
    pthread_t thread;

    if (!sink || !canvas || pthread_create(&thread, NULL, capture, NULL))
        return 1;
    fprintf(stderr, "marksman-cam: ready; the camera is woken when an app opens the webcam\n");

    while (!stop) {
        int fresh = 0;
        sink_size(sink, &v.out_w, &v.out_h);
        if (!(watched = sink_watched(sink))) {
            sleep_s(0.1); /* nobody watching: no camera traffic, no rendering */
            next = now_s();
            continue;
        }
        pthread_mutex_lock(&latest.lock);
        if (latest.seq != seen) {
            seen = latest.seq;
            if (!latest.connected) {
                have = 0;
                dirty = 1;
            } else {
                if (latest.len > jpeg_cap) {
                    uint8_t *b = realloc(jpeg, latest.len);
                    if (b) {
                        jpeg = b;
                        jpeg_cap = latest.len;
                    }
                }
                if (latest.len <= jpeg_cap) {
                    memcpy(jpeg, latest.jpeg, latest.len);
                    jpeg_len = latest.len;
                    fresh = 1;
                }
            }
        }
        pthread_mutex_unlock(&latest.lock);
        if (fresh && !jpeg_decode(jpeg, jpeg_len, &rgb, &rgb_cap, &sw, &sh))
            have = dirty = 1;

        if (v.out_w != last_w || v.out_h != last_h) {
            last_w = v.out_w;
            last_h = v.out_h;
            dirty = 1;
        }
        if (dirty) {
            if (have)
                render(rgb, sw, sh, &v, canvas);
            else
                status_frame(canvas, v.out_w, v.out_h);
            dirty = 0;
        }
        if (sink_write(sink, canvas, &v)) {
            perror("marksman-cam: writing frame");
            break;
        }

        next += 1.0 / fps;
        t = now_s();
        if (next > t)
            sleep_s(next - t);
        else
            next = t; /* fell behind: don't try to catch up */
    }

    stop = 1;
    pthread_join(thread, NULL); /* closes the camera cleanly */
    sink_close(sink);
    free(canvas);
    free(rgb);
    free(jpeg);
    return 0;
}

#ifdef _WIN32
static int run_service(void)
{
    return run(sink_open_shared(v.out_w, v.out_h));
}
#endif

int main(int argc, char **argv)
{
    const char *device = NULL;
    int install = 0, uninstall = 0, service = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
        int used = 1;
        if (!strcmp(a, "--fill"))
            v.fill = 1, used = 0;
        else if (!strcmp(a, "--mirror"))
            v.mirror = 1, used = 0;
        else if (!strcmp(a, "--flip"))
            v.flip = 1, used = 0;
        else if (!strcmp(a, "--install"))
            install = 1, used = 0;
        else if (!strcmp(a, "--uninstall"))
            uninstall = 1, used = 0;
        else if (!strcmp(a, "--service"))
            service = 1, used = 0;
        else if (!val)
            goto bad;
        else if (!strcmp(a, "-d"))
            device = val;
        else if (!strcmp(a, "-s")) {
            if (sscanf(val, "%dx%d", &v.out_w, &v.out_h) != 2 || v.out_w < 2 || v.out_h < 2 || v.out_w > MAX_W ||
                v.out_h > MAX_H || v.out_w % 2 || v.out_h % 2)
                goto bad;
        } else if (!strcmp(a, "-r")) {
            if ((fps = atoi(val)) < 1 || fps > 60)
                goto bad;
        } else if (!strcmp(a, "--rotate")) {
            if ((v.rotate = atoi(val)) % 90 || v.rotate < 0 || v.rotate > 270)
                goto bad;
        } else if (!strcmp(a, "--crop")) {
            float *c = v.crop;
            if (sscanf(val, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) != 4 || c[0] < 0 || c[1] < 0 ||
                c[2] > 1 || c[3] > 1 || c[2] - c[0] < 0.01f || c[3] - c[1] < 0.01f)
                goto bad;
        } else if (!strcmp(a, "--brightness"))
            v.brightness = atoi(val);
        else if (!strcmp(a, "--contrast"))
            v.contrast = atoi(val);
        else if (!strcmp(a, "--saturation"))
            v.saturation = atoi(val);
        else if (!strcmp(a, "--hue"))
            v.hue = atoi(val);
        else if (!strcmp(a, "--set") && nsets < MAX_SETS && strchr(val, '='))
            sets[nsets++] = val;
        else if (!strcmp(a, "--camera")) {
            if (!strcmp(val, "canon"))
                eos_only_vid = 0x04A9;
            else if (!strcmp(val, "sony"))
                eos_only_vid = 0x054C;
            else
                goto bad;
        } else if (!strcmp(a, "--snapshot"))
            return snapshot(val);
        else
            goto bad;
        i += used;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef _WIN32
    if (install)
        return winsvc_install(argc, argv);
    if (uninstall)
        return winsvc_uninstall();
    if (service)
        return winsvc_run(run_service, &stop);
#else
    if (install || uninstall || service)
        goto bad; /* Linux: systemd unit + udev rule, see README */
#endif
    return run(sink_open(device, v.out_w, v.out_h, fps));

bad:
    fputs(usage, stderr);
    return 2;
}
