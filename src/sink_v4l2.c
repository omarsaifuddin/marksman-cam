/* Linux output: YUYV into a v4l2loopback device.

   Device choice mirrors the Windows virtual cameras: -d takes a path (/dev/video10) or a card label
   ("Marksman Cam (Sony)"). Without -d, the label for --camera is looked up ("Marksman Cam (Canon)",
   "Marksman Cam (Sony)", "Marksman Cam"); with no --camera, then any "Marksman Cam" device, then /dev/video10.

   Demand: v4l2loopback >= 0.13 sends a client-usage event when a reader starts or stops streaming;
   the camera is woken only then, and released WATCH_GRACE_S after the last reader stops. Older modules
   have no such event, so other processes holding the device open (/proc scan) count as readers. */
#include "sink.h"
#include "ptp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* From v4l2loopback.h; not shipped as a uapi header by most distros. */
#define V4L2_EVENT_PRI_CLIENT_USAGE (V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1)
struct v4l2_event_client_usage {
    __u32 count;
};

#define WATCH_GRACE_S 3.0 /* same as Windows: keep the camera up briefly so app restarts are seamless */
#define PROC_SCAN_S   0.5

struct sink {
    int fd, w, h;
    int events;          /* client-usage events subscribed */
    int readers;         /* last known reader count */
    double last_reader;  /* when a reader was last seen */
    double last_scan;
    dev_t rdev;          /* for the /proc fallback */
    uint8_t *buf;
};

static double mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* /dev/videoN whose card label is `label`; prefix != 0 accepts any label starting with it. */
static int find_by_label(const char *label, int prefix, char *path, size_t n)
{
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    int found = 0;
    if (!d)
        return 0;
    while (!found && (e = readdir(d))) {
        char f[PATH_MAX], name[128] = "";
        FILE *fp;
        if (strncmp(e->d_name, "video", 5))
            continue;
        snprintf(f, sizeof f, "/sys/class/video4linux/%s/name", e->d_name);
        if (!(fp = fopen(f, "r")))
            continue;
        if (fgets(name, sizeof name, fp))
            name[strcspn(name, "\n")] = 0;
        fclose(fp);
        if (prefix ? !strncmp(name, label, strlen(label)) : !strcmp(name, label)) {
            snprintf(path, n, "/dev/%s", e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static const char *default_label(void)
{
    return eos_only_vid == 0x04A9 ? "Marksman Cam (Canon)" : eos_only_vid == 0x054C ? "Marksman Cam (Sony)" : "Marksman Cam";
}

static int resolve(const char *device, char *path, size_t n)
{
    if (device && device[0] == '/') {
        snprintf(path, n, "%s", device);
        return 1;
    }
    if (device) {
        if (find_by_label(device, 0, path, n))
            return 1;
        fprintf(stderr, "marksman-cam: no video device labelled \"%s\"\n", device);
        return 0;
    }
    if (find_by_label(default_label(), 0, path, n))
        return 1;
    if (eos_only_vid) {
        /* never fall back to another camera's device: that service already feeds it */
        fprintf(stderr, "marksman-cam: no video device labelled \"%s\"; run install.sh, or pass -d\n", default_label());
        return 0;
    }
    if (find_by_label("Marksman Cam", 1, path, n))
        return 1;
    snprintf(path, n, "/dev/video10");
    return 1;
}

static int is_loopback(int fd)
{
    struct v4l2_capability cap = {0};
    return ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && !strcmp((const char *)cap.driver, "v4l2 loopback");
}

sink_t *sink_open(const char *device, int w, int h, int fps)
{
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    struct v4l2_streamparm sp = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    struct v4l2_event_subscription sub = {.type = V4L2_EVENT_PRI_CLIENT_USAGE, .flags = V4L2_EVENT_SUB_FL_SEND_INITIAL};
    struct stat st;
    char dev[PATH_MAX];
    sink_t *s = calloc(1, sizeof *s);

    if (!s)
        return NULL;
    s->fd = -1;
    if (!resolve(device, dev, sizeof dev))
        goto fail;
    if (!(s->buf = malloc((size_t)w * h * 2)))
        goto fail;
    if ((s->fd = open(dev, O_RDWR | O_CLOEXEC)) < 0) {
        perror(dev);
        fprintf(stderr, "marksman-cam: load v4l2loopback first: sudo modprobe v4l2loopback (or run install.sh)\n");
        goto fail;
    }
    if (!is_loopback(s->fd)) {
        fprintf(stderr, "marksman-cam: %s is not a v4l2loopback device (a real webcam?); pass -d\n", dev);
        goto fail;
    }
    f.fmt.pix.width = (unsigned)w;
    f.fmt.pix.height = (unsigned)h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.bytesperline = (unsigned)w * 2;
    f.fmt.pix.sizeimage = (unsigned)(w * h * 2);
    f.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
    if (ioctl(s->fd, VIDIOC_S_FMT, &f) < 0) {
        perror("VIDIOC_S_FMT");
        if (errno == EBUSY)
            fprintf(stderr, "marksman-cam: %s is in use; another marksman-cam (or OBS) already feeds it\n", dev);
        goto fail;
    }
    sp.parm.output.timeperframe.numerator = 1;
    sp.parm.output.timeperframe.denominator = (unsigned)fps;
    ioctl(s->fd, VIDIOC_S_PARM, &sp); /* advisory only */
    s->events = ioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0;
    if (!fstat(s->fd, &st))
        s->rdev = st.st_rdev;
    s->w = w;
    s->h = h;
    fprintf(stderr, "marksman-cam: writing %dx%d to %s; apps wake the camera %s\n", w, h, dev,
            s->events ? "when they start streaming" : "when they open the device (old v4l2loopback)");
    return s;
fail:
    sink_close(s);
    return NULL;
}

void sink_size(sink_t *s, int *w, int *h)
{
    *w = s->w;
    *h = s->h;
}

/* Fallback for v4l2loopback without client-usage events: count other processes with the device open. */
static int proc_readers(sink_t *s)
{
    DIR *proc = opendir("/proc");
    struct dirent *p;
    const pid_t self = getpid();
    int n = 0;
    if (!proc)
        return 0;
    while (!n && (p = readdir(proc))) {
        char dir[64];
        DIR *fds;
        struct dirent *f;
        pid_t pid = (pid_t)atoi(p->d_name);
        if (pid <= 0 || pid == self)
            continue;
        snprintf(dir, sizeof dir, "/proc/%d/fd", (int)pid);
        if (!(fds = opendir(dir)))
            continue; /* another user's process: not visible, so a root-run app is missed */
        while (!n && (f = readdir(fds))) {
            struct stat st;
            if (f->d_name[0] != '.' && !fstatat(dirfd(fds), f->d_name, &st, 0) && S_ISCHR(st.st_mode) &&
                st.st_rdev == s->rdev)
                n = 1;
        }
        closedir(fds);
    }
    closedir(proc);
    return n;
}

int sink_watched(sink_t *s)
{
    const double now = mono();
    if (s->events) {
        struct pollfd pfd = {s->fd, POLLPRI, 0};
        struct v4l2_event ev;
        while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLPRI) && ioctl(s->fd, VIDIOC_DQEVENT, &ev) == 0)
            if (ev.type == V4L2_EVENT_PRI_CLIENT_USAGE)
                s->readers = (int)((struct v4l2_event_client_usage *)&ev.u)->count;
    } else if (now - s->last_scan >= PROC_SCAN_S) {
        s->last_scan = now;
        s->readers = proc_readers(s);
    }
    if (s->readers)
        s->last_reader = now;
    return s->readers || (s->last_reader && now - s->last_reader < WATCH_GRACE_S);
}

int sink_write(sink_t *s, const yuv_t *f, const view_t *v)
{
    render_yuyv(f, v, s->buf);
    return sink_repeat(s);
}

int sink_repeat(sink_t *s)
{
    const ssize_t n = (ssize_t)s->w * s->h * 2;
    return write(s->fd, s->buf, (size_t)n) == n ? 0 : -1;
}

/* v4l2loopback has no way to pass an app's control changes to the writer; the control port covers that. */
int sink_controls(sink_t *s, view_t *v)
{
    (void)s, (void)v;
    return 0;
}

void sink_set_controls(sink_t *s, const view_t *v)
{
    (void)s, (void)v;
}

void sink_close(sink_t *s)
{
    if (!s)
        return;
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    free(s);
}
