/* Shared helpers for the calibration tools. */
#ifndef CALIB_COMMON_H
#define CALIB_COMMON_H

#include "calib.h"
#include "mpu6500.h"
#include "qmc5883p.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define IMU_BUS_DEFAULT "/dev/i2c-1"
#define MAG_BUS_DEFAULT "/dev/i2c-1"
#define GRAVITY         9.80665
#define RAD2DEG         (180.0 / M_PI)

/* Stillness thresholds: ~3x the measured stationary noise
 * (accel std 0.016-0.041 m/s^2, gyro std 0.13-0.16 deg/s). */
#define STILL_ACCEL_STD 0.12                   /* m/s^2 */
#define STILL_GYRO_STD  (0.5 / RAD2DEG)        /* rad/s */

static inline int open_imu(mpu6500_t *imu, const char *bus)
{
    int r = mpu6500_init(imu, bus, MPU6500_ADDR_DEFAULT, &MPU6500_CONFIG_DEFAULT);
    if (r < 0) fprintf(stderr, "erro: MPU6500 em %s: %s\n", bus, strerror(-r));
    return r;
}

static inline int open_mag(qmc5883p_t *mag, const char *bus)
{
    int r = qmc5883p_init(mag, bus, QMC5883P_ADDR_DEFAULT, &QMC5883P_CONFIG_DEFAULT);
    if (r < 0) r = qmc5883p_init(mag, bus, QMC5883P_ADDR_ALT, &QMC5883P_CONFIG_DEFAULT);
    if (r < 0) fprintf(stderr, "erro: QMC5883P em %s: %s\n", bus, strerror(-r));
    return r;
}

/* Running mean / variance of 3-vectors (Welford). */
typedef struct { unsigned n; double mean[3], m2[3]; } stats3_t;

static inline void stats3_reset(stats3_t *s) { memset(s, 0, sizeof *s); }

static inline void stats3_add(stats3_t *s, const float v[3])
{
    s->n++;
    for (int i = 0; i < 3; i++) {
        double d = v[i] - s->mean[i];
        s->mean[i] += d / s->n;
        s->m2[i] += d * (v[i] - s->mean[i]);
    }
}

static inline double stats3_std(const stats3_t *s, int i)
{
    return s->n > 1 ? sqrt(s->m2[i] / (s->n - 1)) : 0.0;
}

static inline double stats3_max_std(const stats3_t *s)
{
    return fmax(stats3_std(s, 0), fmax(stats3_std(s, 1), stats3_std(s, 2)));
}

/* Fixed-rate loop pacing on CLOCK_MONOTONIC. */
static inline void tick_wait(struct timespec *next, long period_ns)
{
    next->tv_nsec += period_ns;
    while (next->tv_nsec >= 1000000000L) { next->tv_sec++; next->tv_nsec -= 1000000000L; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

static inline double norm3d(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

#endif
