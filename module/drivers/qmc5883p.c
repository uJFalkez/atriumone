#include "qmc5883p.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    REG_CHIP_ID = 0x00,
    REG_XOUT_L  = 0x01,  /* X,Y,Z little-endian int16, 0x01..0x06 */
    REG_STATUS  = 0x09,  /* bit0 DRDY, bit1 OVFL */
    REG_CTRL1   = 0x0A,  /* [7:6] OSR2 [5:4] OSR1 [3:2] ODR [1:0] MODE */
    REG_CTRL2   = 0x0B,  /* [7] SOFT_RST [6] SELF_TEST [3:2] RNG [1:0] SET/RESET */
    REG_AXIS    = 0x29,  /* axis sign definition */
};

#define MODE_NORMAL    0x01
#define SETRESET_ON    0x00
#define GAUSS_TO_UT    100.0f
#define CHECK_EVERY_NS 1000000000ull  /* verify CTRL1/CTRL2 once per second */

static const float    lsb_per_gauss[] = { 1000.0f, 2500.0f, 3750.0f, 15000.0f };
static const unsigned odr_period_ms[] = { 100, 20, 10, 5 };

#define TRY(x) do { int _r = (x); if (_r < 0) return _r; } while (0)

static int configure(qmc5883p_t *dev)
{
    uint8_t c[2];

    TRY(i2c_write_reg(&dev->i2c, REG_CTRL2, 0x80));  /* soft reset */
    sleep_ms(50);
    /* Observed on our unit: the first CTRL2 write after a soft reset is swallowed
     * (it completes the reset and reloads defaults), so issue a dummy one first. */
    TRY(i2c_write_reg(&dev->i2c, REG_CTRL2, 0x00));
    TRY(i2c_write_reg(&dev->i2c, REG_AXIS, 0x06));   /* datasheet-recommended axis sign */
    TRY(i2c_write_reg(&dev->i2c, REG_CTRL2, dev->ctrl2));
    TRY(i2c_write_reg(&dev->i2c, REG_CTRL1, dev->ctrl1));
    sleep_ms(20);

    TRY(i2c_read_regs(&dev->i2c, REG_CTRL1, c, 2));
    if (c[0] != dev->ctrl1 || (c[1] & 0x7F) != dev->ctrl2)
        return -EIO;  /* configuration did not stick */

    uint64_t now = mono_ns();
    dev->last_data_ns  = now;
    dev->last_check_ns = now;
    return 0;
}

int qmc5883p_init(qmc5883p_t *dev, const char *bus, uint8_t addr, const qmc5883p_config_t *cfg)
{
    uint8_t id;

    if (cfg->range > QMC_RANGE_2G || cfg->odr > QMC_ODR_200HZ || cfg->osr1 > 3 || cfg->osr2 > 3)
        return -EINVAL;

    TRY(i2c_open(&dev->i2c, bus, addr));
    int r = i2c_read_reg(&dev->i2c, REG_CHIP_ID, &id);
    if (r == 0 && id != QMC5883P_CHIP_ID_VAL)
        r = -ENODEV;
    if (r < 0) {
        i2c_close(&dev->i2c);
        return r;
    }

    dev->cfg   = *cfg;
    dev->ctrl1 = (uint8_t)((cfg->osr2 << 6) | (cfg->osr1 << 4) | (cfg->odr << 2) | MODE_NORMAL);
    dev->ctrl2 = (uint8_t)((cfg->range << 2) | SETRESET_ON);
    dev->lsb_per_gauss = lsb_per_gauss[cfg->range];
    dev->recoveries = dev->rec_config = dev->rec_stall = 0;

    /* Measured: healthy chip occasionally skips samples, gaps up to ~90 ms at 100 Hz.
     * Real stalls seen earlier were permanent, so a generous timeout loses nothing. */
    unsigned stall_ms = odr_period_ms[cfg->odr] * 25;
    dev->stall_ns = (uint64_t)(stall_ms < 250 ? 250 : stall_ms) * 1000000ull;

    r = configure(dev);
    if (r < 0)
        i2c_close(&dev->i2c);
    return r;
}

static int recover(qmc5883p_t *dev)
{
    dev->recoveries++;
    return configure(dev);
}

int qmc5883p_read(qmc5883p_t *dev, qmc5883p_sample_t *s)
{
    uint8_t st, b[6];
    uint64_t now = mono_ns();

    if (now - dev->last_check_ns >= CHECK_EVERY_NS) {
        uint8_t c[2];
        dev->last_check_ns = now;
        TRY(i2c_read_regs(&dev->i2c, REG_CTRL1, c, 2));
        if (c[0] != dev->ctrl1 || (c[1] & 0x7F) != dev->ctrl2) {
            dev->rec_config++;
            TRY(recover(dev));
            return -EAGAIN;
        }
    }

    TRY(i2c_read_reg(&dev->i2c, REG_STATUS, &st));
    if (!(st & 0x01)) {
        if (now - dev->last_data_ns > dev->stall_ns) {
            dev->rec_stall++;
            TRY(recover(dev));
        }
        return -EAGAIN;
    }

    TRY(i2c_read_regs(&dev->i2c, REG_XOUT_L, b, sizeof b));
    s->t_ns = mono_ns();
    s->overflow = (st & 0x02) != 0;
    dev->last_data_ns = s->t_ns;

    for (int i = 0; i < 3; i++) {
        s->raw[i]   = (int16_t)(b[i * 2] | (b[i * 2 + 1] << 8));
        s->field[i] = s->raw[i] / dev->lsb_per_gauss * GAUSS_TO_UT;
    }
    return 0;
}

void qmc5883p_close(qmc5883p_t *dev)
{
    i2c_write_reg(&dev->i2c, REG_CTRL1, 0x00); /* suspend */
    i2c_close(&dev->i2c);
}

/* ---------------------------------------------------------------- on-demand burst */

#define MODE_MASK 0x03
#define BURST_MAX 64

int qmc5883p_suspend(qmc5883p_t *dev)
{
    return i2c_write_reg(&dev->i2c, REG_CTRL1, (uint8_t)(dev->ctrl1 & ~MODE_MASK));
}

int qmc5883p_resume(qmc5883p_t *dev)
{
    int r = i2c_write_reg(&dev->i2c, REG_CTRL1, dev->ctrl1);
    uint64_t now = mono_ns();
    dev->last_data_ns = now;
    dev->last_check_ns = now;
    return r;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

int qmc5883p_burst(qmc5883p_t *dev, const qmc5883p_burst_opts_t *o, qmc5883p_burst_t *out)
{
    uint8_t c[2], st, b[6];
    float f[BURST_MAX][3];
    uint64_t ts[BURST_MAX];
    unsigned n = 0, seen = 0, want = o->n_avg < BURST_MAX ? o->n_avg : BURST_MAX;
    int ovf = 0, r;

    memset(out, 0, sizeof *out);
    uint64_t t0 = mono_ns();

    TRY(i2c_read_regs(&dev->i2c, REG_CTRL1, c, 2));
    if ((c[0] & ~MODE_MASK) != (dev->ctrl1 & ~MODE_MASK) || (c[1] & 0x7F) != dev->ctrl2) {
        dev->rec_config++;
        TRY(recover(dev));              /* leaves the chip in normal mode */
    } else {
        TRY(i2c_write_reg(&dev->i2c, REG_CTRL1, dev->ctrl1));
    }

    uint64_t deadline = t0 + (uint64_t)o->timeout_ms * 1000000ull;
    while (n < want && mono_ns() < deadline) {
        r = i2c_read_reg(&dev->i2c, REG_STATUS, &st);
        if (r == 0 && (st & 0x01)) {
            r = i2c_read_regs(&dev->i2c, REG_XOUT_L, b, sizeof b);
            if (r == 0 && ++seen > o->n_drop) {
                ts[n] = mono_ns();
                for (int i = 0; i < 3; i++)
                    f[n][i] = (int16_t)(b[i * 2] | (b[i * 2 + 1] << 8)) / dev->lsb_per_gauss * GAUSS_TO_UT;
                ovf |= (st & 0x02) != 0;
                n++;
            }
        }
        sleep_ms(2);
    }
    r = qmc5883p_suspend(dev);
    out->duration_ms = (unsigned)((mono_ns() - t0) / 1000000ull);
    if (r < 0)
        return r;
    if (n == 0)
        return -EBADMSG;

    /* Robust mean: reject samples far from the per-axis median vector. */
    float med[3], tmp[BURST_MAX];
    for (int i = 0; i < 3; i++) {
        for (unsigned k = 0; k < n; k++) tmp[k] = f[k][i];
        qsort(tmp, n, sizeof tmp[0], cmp_float);
        med[i] = n % 2 ? tmp[n / 2] : 0.5f * (tmp[n / 2 - 1] + tmp[n / 2]);
    }
    double sum[3] = { 0 }, sq[3] = { 0 }, tsum = 0;
    for (unsigned k = 0; k < n; k++) {
        float dx = f[k][0] - med[0], dy = f[k][1] - med[1], dz = f[k][2] - med[2];
        /* An axis reading exactly 0 was a corruption signature on the shared bus. */
        int zero = f[k][0] == 0.0f || f[k][1] == 0.0f || f[k][2] == 0.0f;
        if (zero || sqrtf(dx * dx + dy * dy + dz * dz) > o->outlier_ut) {
            out->n_rejected++;
            continue;
        }
        for (int i = 0; i < 3; i++) { sum[i] += f[k][i]; sq[i] += (double)f[k][i] * f[k][i]; }
        tsum += (double)(ts[k] - t0);
        out->n_used++;
    }
    out->overflow = ovf;
    if (out->n_used * 10 < want * 6)
        return -EBADMSG;
    for (int i = 0; i < 3; i++) {
        double m = sum[i] / out->n_used;
        out->field[i] = (float)m;
        out->std[i] = (float)sqrt(fmax(0.0, sq[i] / out->n_used - m * m));
    }
    out->t_ns = t0 + (uint64_t)(tsum / out->n_used);
    dev->last_data_ns = mono_ns();
    return 0;
}
