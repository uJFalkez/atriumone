#include "mpu6500.h"
#include "util.h"

#include <errno.h>
#include <math.h>

enum {
    REG_SMPLRT_DIV    = 0x19,
    REG_CONFIG        = 0x1A,
    REG_GYRO_CONFIG   = 0x1B,
    REG_ACCEL_CONFIG  = 0x1C,
    REG_ACCEL_CONFIG2 = 0x1D,
    REG_ACCEL_XOUT_H  = 0x3B,
    REG_SIGNAL_RESET  = 0x68,
    REG_PWR_MGMT_1    = 0x6B,
    REG_PWR_MGMT_2    = 0x6C,
    REG_WHO_AM_I      = 0x75,
};

#define G_TO_MS2   9.80665f
#define DEG_TO_RAD ((float)M_PI / 180.0f)

static const float accel_lsb[] = { 16384.0f, 8192.0f, 4096.0f, 2048.0f };
static const float gyro_lsb[]  = { 131.0f, 65.5f, 32.8f, 16.4f };

#define TRY(x) do { int _r = (x); if (_r < 0) return _r; } while (0)

int mpu6500_init(mpu6500_t *dev, const char *bus, uint8_t addr, const mpu6500_config_t *cfg)
{
    uint8_t who;

    if (cfg->accel_range > MPU_ACCEL_16G || cfg->gyro_range > MPU_GYRO_2000 ||
        cfg->dlpf > 6 || cfg->accel_dlpf > 7)
        return -EINVAL;

    TRY(i2c_open(&dev->i2c, bus, addr));
    int r = i2c_read_reg(&dev->i2c, REG_WHO_AM_I, &who);
    if (r == 0 && who != MPU6500_WHO_AM_I_VAL)
        r = -ENODEV;
    if (r < 0) {
        i2c_close(&dev->i2c);
        return r;
    }

    TRY(i2c_write_reg(&dev->i2c, REG_PWR_MGMT_1, 0x80));   /* device reset */
    sleep_ms(100);
    TRY(i2c_write_reg(&dev->i2c, REG_SIGNAL_RESET, 0x07)); /* reset gyro/accel/temp paths */
    sleep_ms(100);
    TRY(i2c_write_reg(&dev->i2c, REG_PWR_MGMT_1, 0x01));   /* wake, PLL clock */
    TRY(i2c_write_reg(&dev->i2c, REG_PWR_MGMT_2, 0x00));   /* all axes on */
    sleep_ms(10);

    TRY(i2c_write_reg(&dev->i2c, REG_CONFIG, cfg->dlpf));
    TRY(i2c_write_reg(&dev->i2c, REG_SMPLRT_DIV, cfg->smplrt_div));
    TRY(i2c_write_reg(&dev->i2c, REG_GYRO_CONFIG, (uint8_t)(cfg->gyro_range << 3)));  /* FCHOICE_B=00 */
    TRY(i2c_write_reg(&dev->i2c, REG_ACCEL_CONFIG, (uint8_t)(cfg->accel_range << 3)));
    TRY(i2c_write_reg(&dev->i2c, REG_ACCEL_CONFIG2, cfg->accel_dlpf));                /* FCHOICE_B=0 */

    dev->accel_lsb_per_g  = accel_lsb[cfg->accel_range];
    dev->gyro_lsb_per_dps = gyro_lsb[cfg->gyro_range];

    sleep_ms(50); /* let filters settle */
    return 0;
}

static inline int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

int mpu6500_read(mpu6500_t *dev, mpu6500_sample_t *s)
{
    uint8_t b[14];

    TRY(i2c_read_regs(&dev->i2c, REG_ACCEL_XOUT_H, b, sizeof b));
    s->t_ns = mono_ns();

    /* Corrupted frames seen on a shared bus came back with no I/O error. */
    int all00 = 1, allff = 1;
    for (size_t i = 0; i < sizeof b; i++) {
        all00 &= b[i] == 0x00;
        allff &= b[i] == 0xFF;
    }

    for (int i = 0; i < 3; i++) {
        s->raw_accel[i] = be16(&b[i * 2]);
        s->raw_gyro[i]  = be16(&b[8 + i * 2]);
        s->accel[i] = s->raw_accel[i] / dev->accel_lsb_per_g * G_TO_MS2;
        s->gyro[i]  = s->raw_gyro[i] / dev->gyro_lsb_per_dps * DEG_TO_RAD;
    }
    s->raw_temp = be16(&b[6]);
    s->temp_c   = s->raw_temp / 333.87f + 21.0f;
    if (all00 || allff || s->temp_c < -40.0f || s->temp_c > 85.0f)
        return -EBADMSG;
    return 0;
}

void mpu6500_close(mpu6500_t *dev)
{
    i2c_write_reg(&dev->i2c, REG_PWR_MGMT_1, 0x40); /* sleep */
    i2c_close(&dev->i2c);
}
