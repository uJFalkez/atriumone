/* camgrab: one photo from a UVC (USB) camera in MJPEG, without decoding or re-encoding.
 *
 * Opens the device, starts streaming at the requested size, drops `warmup` frames (the
 * camera needs ~5 frames before exposure settles), writes the next complete frame and
 * closes the device. The frame is the camera's own JPEG, unchanged, except that the
 * standard Huffman tables are inserted when the camera omits them (common in UVC MJPEG;
 * without them most JPEG decoders refuse the file). No pixel is altered.
 *
 * Small on purpose: no ffmpeg, no libjpeg. ffmpeg on this Pi caused page-cache thrashing
 * on the slow USB boot drive and hung the system (see docs/module/CLAUDE.md).
 *
 * Capture settings requested by the station (fixed white balance so NDVI is comparable across
 * photos; fixed focus at infinity for aerial shots) are applied before streaming, and the
 * values read back are reported for metadata.json (camera.rgb).
 *
 * usage: camgrab -d DEVICE -W WIDTH -H HEIGHT [-n WARMUP_FRAMES] [-t TIMEOUT_MS]
 *                [-w WB_KELVIN | -w auto] [-f FOCUS_POSITION | -f auto]
 *                [-e EXPOSURE_100US | -e auto] -o OUT.jpg
 *        (-w / -f / -e omitted: leave the camera's current setting untouched; "-e auto" is the
 *         camera's aperture-priority mode, its default)
 * stdout: one JSON line, {"ok":true,...} or {"ok":false,"error":"..."}; exit code 0 / 1.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NBUF 4

/* JPEG Annex K standard Huffman tables, as one DHT segment (0xFFC4). */
static const uint8_t dc_lum_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_chr_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_lum_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_lum_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
static const uint8_t ac_chr_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_chr_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

static uint8_t dht[512];
static size_t dht_len;

static void put_table(uint8_t cls_id, const uint8_t bits[16], const uint8_t *vals, size_t nvals)
{
    dht[dht_len++] = cls_id;
    memcpy(dht + dht_len, bits, 16);
    dht_len += 16;
    memcpy(dht + dht_len, vals, nvals);
    dht_len += nvals;
}

static int build_dht(void)
{
    const uint8_t *bits[4] = { dc_lum_bits, ac_lum_bits, dc_chr_bits, ac_chr_bits };
    size_t nv[4] = { sizeof dc_vals, sizeof ac_lum_vals, sizeof dc_vals, sizeof ac_chr_vals };
    for (int t = 0; t < 4; t++) {                 /* self-check: counts must match */
        unsigned s = 0;
        for (int i = 0; i < 16; i++) s += bits[t][i];
        if (s != nv[t]) return -1;
    }
    dht_len = 0;
    dht[dht_len++] = 0xFF;
    dht[dht_len++] = 0xC4;
    dht_len += 2;                                 /* length, filled below */
    put_table(0x00, dc_lum_bits, dc_vals, sizeof dc_vals);
    put_table(0x10, ac_lum_bits, ac_lum_vals, sizeof ac_lum_vals);
    put_table(0x01, dc_chr_bits, dc_vals, sizeof dc_vals);
    put_table(0x11, ac_chr_bits, ac_chr_vals, sizeof ac_chr_vals);
    size_t seg = dht_len - 2;
    dht[2] = (uint8_t)(seg >> 8);
    dht[3] = (uint8_t)(seg & 0xFF);
    return 0;
}

/* Walks the markers up to SOS. Returns the SOS offset, or -1 if the frame is malformed. */
static long find_sos(const uint8_t *p, size_t n, int *has_dht)
{
    *has_dht = 0;
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return -1;
    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) return -1;
        uint8_t m = p[i + 1];
        if (m == 0xFF) { i++; continue; }         /* fill byte */
        if (m == 0xDA) return (long)i;
        if (m == 0xC4) *has_dht = 1;
        size_t len = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (len < 2) return -1;
        i += 2 + len;
    }
    return -1;
}

static void fail(const char *what, int err)
{
    printf("{\"ok\":false,\"error\":\"%s%s%s\"}\n", what, err ? ": " : "", err ? strerror(err) : "");
    exit(1);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

static int set_ctrl(int fd, unsigned id, int value)
{
    struct v4l2_control c = { .id = id, .value = value };
    return xioctl(fd, VIDIOC_S_CTRL, &c);
}

/* Reads a control into *out; returns 0, or -1 if the camera does not have it. */
static int get_ctrl(int fd, unsigned id, int *out)
{
    struct v4l2_control c = { .id = id };
    if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0) return -1;
    *out = c.value;
    return 0;
}

/* null when the camera lacks the control or marks it inactive (e.g. the exposure time while
 * auto exposure is on: the value read back is then a stale default, not the real exposure). */
static void json_ctrl(const char *name, int fd, unsigned id, int last)
{
    int v;
    struct v4l2_queryctrl q = { .id = id };
    int inactive = xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0 && (q.flags & V4L2_CTRL_FLAG_INACTIVE);
    if (!inactive && get_ctrl(fd, id, &v) == 0) printf("\"%s\":%d%s", name, v, last ? "" : ",");
    else printf("\"%s\":null%s", name, last ? "" : ",");
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *out = NULL;
    unsigned w = 0, h = 0, warmup = 5, timeout_ms = 3000;
    const char *wb = NULL, *focus = NULL, *expo = NULL;
    int c;
    while ((c = getopt(argc, argv, "d:W:H:n:t:o:w:f:e:")) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'W': w = (unsigned)atoi(optarg); break;
        case 'H': h = (unsigned)atoi(optarg); break;
        case 'n': warmup = (unsigned)atoi(optarg); break;
        case 't': timeout_ms = (unsigned)atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'w': wb = optarg; break;
        case 'f': focus = optarg; break;
        case 'e': expo = optarg; break;
        default: fail("usage: camgrab -d DEV -W W -H H [-n WARMUP] [-t MS] [-w K|auto] [-f POS|auto] [-e E|auto] -o OUT", 0);
        }
    }
    if (!dev || !out || !w || !h) fail("usage: camgrab -d DEV -W W -H H [-n WARMUP] [-t TIMEOUT_MS] -o OUT", 0);
    if (build_dht() < 0) fail("internal: bad Huffman tables", 0);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) fail("open", errno);

    /* Fixed white balance / focus. Order matters: switch the automatic mode off first. */
    if (wb) {
        if (!strcmp(wb, "auto")) {
            if (set_ctrl(fd, V4L2_CID_AUTO_WHITE_BALANCE, 1) < 0) fail("set white_balance_automatic", errno);
        } else {
            if (set_ctrl(fd, V4L2_CID_AUTO_WHITE_BALANCE, 0) < 0) fail("set white_balance_automatic", errno);
            if (set_ctrl(fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, atoi(wb)) < 0)
                fail("set white_balance_temperature", errno);
        }
    }
    if (expo) {
        if (!strcmp(expo, "auto")) {
            if (set_ctrl(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_APERTURE_PRIORITY) < 0)
                fail("set auto_exposure", errno);
        } else {
            if (set_ctrl(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL) < 0) fail("set auto_exposure", errno);
            if (set_ctrl(fd, V4L2_CID_EXPOSURE_ABSOLUTE, atoi(expo)) < 0) fail("set exposure_time_absolute", errno);
        }
    }
    if (focus) {
        if (!strcmp(focus, "auto")) {
            if (set_ctrl(fd, V4L2_CID_FOCUS_AUTO, 1) < 0) fail("set focus_automatic_continuous", errno);
        } else {
            if (set_ctrl(fd, V4L2_CID_FOCUS_AUTO, 0) < 0) fail("set focus_automatic_continuous", errno);
            if (set_ctrl(fd, V4L2_CID_FOCUS_ABSOLUTE, atoi(focus)) < 0) fail("set focus_absolute", errno);
        }
    }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) fail("VIDIOC_S_FMT", errno);
    if (fmt.fmt.pix.width != w || fmt.fmt.pix.height != h || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG)
        fail("camera refused the requested MJPEG size", 0);

    struct v4l2_requestbuffers rb = { .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                      .memory = V4L2_MEMORY_MMAP };
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) fail("VIDIOC_REQBUFS", errno);
    void *mem[NBUF] = { 0 };
    size_t memlen[NBUF] = { 0 };
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) fail("VIDIOC_QUERYBUF", errno);
        mem[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (mem[i] == MAP_FAILED) fail("mmap", errno);
        memlen[i] = b.length;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) fail("VIDIOC_QBUF", errno);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) fail("VIDIOC_STREAMON", errno);

    unsigned frames = 0, bad = 0;
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)timeout_ms);
        if (pr == 0) fail("timeout waiting for a frame", 0);
        if (pr < 0) fail("poll", errno);
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) fail("VIDIOC_DQBUF", errno);
        frames++;

        const uint8_t *p = mem[b.index];
        size_t n = b.bytesused <= memlen[b.index] ? b.bytesused : 0;
        /* Some UVC cameras pad the buffer after the JPEG end (EOI): look for EOI near the end
         * and drop the padding instead of rejecting the frame. */
        size_t eoi = 0;
        for (size_t i = n; i >= 2 && n - i < 8192; i--)
            if (p[i - 2] == 0xFF && p[i - 1] == 0xD9) { eoi = i; break; }
        int has_dht = 0;
        long sos = -1;
        if (frames > warmup && !(b.flags & V4L2_BUF_FLAG_ERROR) && eoi > 1024) {
            n = eoi;
            sos = find_sos(p, n, &has_dht);
        }
        if (frames > warmup && sos < 0) bad++;

        if (sos >= 0) {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s.part", out);
            int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (ofd < 0) fail("open output", errno);
            /* JPEG = header up to SOS (+ standard DHT if missing) + scan data */
            struct { const void *p; size_t n; } parts[3] = {
                { p, (size_t)sos }, { dht, has_dht ? 0 : dht_len }, { p + sos, n - (size_t)sos } };
            for (int k = 0; k < 3; k++) {
                const uint8_t *q = parts[k].p;
                size_t left = parts[k].n;
                while (left) {
                    ssize_t wr = write(ofd, q, left);
                    if (wr <= 0) fail("write", errno);
                    q += wr;
                    left -= (size_t)wr;
                }
            }
            close(ofd);
            if (rename(tmp, out) < 0) fail("rename", errno);
            xioctl(fd, VIDIOC_STREAMOFF, &type);

            double ts = b.timestamp.tv_sec + b.timestamp.tv_usec / 1e6;   /* CLOCK_MONOTONIC */
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double took = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            printf("{\"ok\":true,\"width\":%u,\"height\":%u,\"bytes\":%zu,\"frames\":%u,\"bad_frames\":%u,"
                   "\"dht_added\":%s,\"frame_t_mono\":%.6f,\"took_s\":%.3f,\"controls\":{",
                   w, h, n + (has_dht ? 0 : dht_len), frames, bad, has_dht ? "false" : "true", ts, took);
            /* Values as read back after the frame (exposure_time_absolute is in 100 us units;
             * in auto-exposure modes some cameras do not update it). */
            json_ctrl("auto_exposure", fd, V4L2_CID_EXPOSURE_AUTO, 0);
            json_ctrl("exposure_time_absolute", fd, V4L2_CID_EXPOSURE_ABSOLUTE, 0);
            json_ctrl("gain", fd, V4L2_CID_GAIN, 0);
            json_ctrl("white_balance_automatic", fd, V4L2_CID_AUTO_WHITE_BALANCE, 0);
            json_ctrl("white_balance_temperature", fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, 0);
            json_ctrl("focus_automatic_continuous", fd, V4L2_CID_FOCUS_AUTO, 0);
            json_ctrl("focus_absolute", fd, V4L2_CID_FOCUS_ABSOLUTE, 1);
            printf("}}\n");
            close(fd);
            return 0;
        }
        if (bad > 10) fail("too many malformed frames", 0);
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) fail("VIDIOC_QBUF", errno);
    }
}
