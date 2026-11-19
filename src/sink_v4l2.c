#include "sink.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct sink {
    int fd, w, h;
    uint8_t *buf;
};

sink_t *sink_open(const char *device, int w, int h, int fps)
{
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    struct v4l2_streamparm sp = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    sink_t *s = calloc(1, sizeof *s);
    const char *dev = device ? device : "/dev/video10";

    if (!s)
        return NULL;
    s->fd = -1;
    if (!(s->buf = malloc((size_t)w * h * 2)))
        goto fail;
    if ((s->fd = open(dev, O_RDWR)) < 0) {
        perror(dev);
        fprintf(stderr, "marksman-cam: load v4l2loopback first (see README)\n");
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
        goto fail;
    }
    sp.parm.output.timeperframe.numerator = 1;
    sp.parm.output.timeperframe.denominator = (unsigned)fps;
    ioctl(s->fd, VIDIOC_S_PARM, &sp); /* advisory only */
    s->w = w;
    s->h = h;
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

int sink_watched(sink_t *s)
{
    (void)s;
    return 1; /* ponytail: always streams; v4l2loopback's client-usage events could make this demand-driven */
}

int sink_write(sink_t *s, const uint8_t *bgrx, const view_t *v)
{
    const ssize_t n = (ssize_t)s->w * s->h * 2;
    to_yuyv(bgrx, s->w, s->h, v, s->buf);
    return write(s->fd, s->buf, (size_t)n) == n ? 0 : -1;
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
