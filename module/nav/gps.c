#include "gps.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int gps_open(gps_t *g, const char *dev)
{
    memset(g, 0, sizeof *g);
    g->fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (g->fd < 0)
        return -errno;
    struct termios t;
    if (tcgetattr(g->fd, &t) < 0) {
        int e = -errno;
        close(g->fd);
        g->fd = -1;
        return e;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    cfsetispeed(&t, B9600);
    cfsetospeed(&t, B9600);
    tcsetattr(g->fd, TCSANOW, &t);
    tcflush(g->fd, TCIFLUSH);
    return 0;
}

void gps_close(gps_t *g)
{
    if (g->fd >= 0)
        close(g->fd);
    g->fd = -1;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Splits in place on ','; empty fields become "". Returns the field count. */
static int split(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < max; p++)
        if (*p == ',') {
            *p = '\0';
            f[n++] = p + 1;
        }
    return n;
}

/* ddmm.mmmm + hemisphere -> signed degrees */
static double nmea_deg(const char *v, const char *h)
{
    double x = atof(v);
    int d = (int)(x / 100);
    double deg = d + (x - d * 100) / 60.0;
    return (*h == 'S' || *h == 'W') ? -deg : deg;
}

bool gps_parse_sentence(gps_t *g, const char *in, uint64_t t_ns)
{
    char s[128];
    size_t n = strcspn(in, "\r\n");
    if (n < 7 || n >= sizeof s || in[0] != '$')
        return false;
    memcpy(s, in, n);
    s[n] = '\0';

    char *star = strrchr(s, '*');
    if (!star || star[1] == '\0' || star[2] == '\0')
        return false;
    unsigned char ck = 0;
    for (char *p = s + 1; p < star; p++) ck ^= (unsigned char)*p;
    int h1 = hexval(star[1]), h2 = hexval(star[2]);
    if (h1 < 0 || h2 < 0 || ck != (h1 << 4 | h2)) {
        g->fix.bad_checksum++;
        return false;
    }
    *star = '\0';

    char *f[24];
    int nf = split(s + 1, f, 24);
    const char *type = f[0] + 2;          /* skip talker (GP, GN, ...) */
    gps_fix_t *x = &g->fix;

    if (!strcmp(type, "GGA") && nf >= 10) {
        x->quality = atoi(f[6]);
        x->sats = atoi(f[7]);
        x->hdop = *f[8] ? atof(f[8]) : 99.99;
        if (x->quality > 0 && *f[2] && *f[4]) {
            x->lat = nmea_deg(f[2], f[3]);
            x->lon = nmea_deg(f[4], f[5]);
            x->alt = atof(f[9]);
        }
        x->gga_t_ns = t_ns;
        g->new_gga++;
    } else if (!strcmp(type, "RMC") && nf >= 10) {
        x->rmc_valid = f[2][0] == 'A';
        x->speed = atof(f[7]) * 0.514444;
        x->course = atof(f[8]);
        if (strlen(f[1]) >= 6) {
            x->hour = (f[1][0] - '0') * 10 + (f[1][1] - '0');
            x->min = (f[1][2] - '0') * 10 + (f[1][3] - '0');
            x->sec = atof(f[1] + 4);
        }
        if (strlen(f[9]) == 6) {
            x->day = (f[9][0] - '0') * 10 + (f[9][1] - '0');
            x->month = (f[9][2] - '0') * 10 + (f[9][3] - '0');
            x->year = 2000 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
        }
        x->rmc_t_ns = t_ns;
        g->new_rmc++;
    } else {
        x->sentences++;
        return true;
    }
    x->sentences++;
    return true;
}

void gps_poll(gps_t *g)
{
    char buf[256];
    ssize_t r;
    if (g->fd < 0)
        return;
    while ((r = read(g->fd, buf, sizeof buf)) > 0) {
        uint64_t now = mono_ns();
        g->fix.last_byte_ns = now;
        for (ssize_t i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '$')
                g->len = 0;
            if (g->len < sizeof g->line - 1)
                g->line[g->len++] = c;
            else
                g->len = 0;                   /* overlong: drop */
            if (c == '\n') {
                g->line[g->len] = '\0';
                gps_parse_sentence(g, g->line, now);
                g->len = 0;
            }
        }
    }
}
