/* Live readout of MPU6500 + QMC5883P (raw, or calibrated with -c).
 * Usage: sensor_monitor [-n PRINTS] [-b IMU_BUS] [-m MAG_BUS] [-c CALIB.json]
 *   IMU sampled at 200 Hz, mag polled at 100 Hz; one summary line every 100 ms.
 *   Columns end with: imu_rate/mag_rate, flags (OVF/ERR), temperature, mag watchdog recoveries. */
#include "calib.h"
#include "mpu6500.h"
#include "qmc5883p.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static float norm3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

int main(int argc, char **argv)
{
    const char *imu_bus = "/dev/i2c-1";
    const char *mag_bus = "/dev/i2c-1";
    const char *calib_path = NULL;
    long max_prints = -1;
    int opt;

    while ((opt = getopt(argc, argv, "n:b:m:c:")) != -1) {
        if (opt == 'n') max_prints = strtol(optarg, NULL, 10);
        else if (opt == 'b') imu_bus = optarg;
        else if (opt == 'm') mag_bus = optarg;
        else if (opt == 'c') calib_path = optarg;
        else { fprintf(stderr, "usage: %s [-n PRINTS] [-b IMU_BUS] [-m MAG_BUS] [-c CALIB.json]\n", argv[0]); return 2; }
    }

    calib_t cal;
    calib_defaults(&cal);
    if (calib_path) {
        int cr = calib_load(&cal, calib_path);
        if (cr < 0) { fprintf(stderr, "calib load %s: %s\n", calib_path, strerror(-cr)); return 1; }
        fprintf(stderr, "calibration: gyro %s, accel %s, mag %s\n", cal.gyro_valid ? "yes" : "no",
                cal.accel_valid ? "yes" : "no", cal.mag_valid ? "yes" : "no");
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    mpu6500_t imu;
    qmc5883p_t mag;
    int r;

    if ((r = mpu6500_init(&imu, imu_bus, MPU6500_ADDR_DEFAULT, &MPU6500_CONFIG_DEFAULT)) < 0) {
        fprintf(stderr, "mpu6500 init (%s): %s\n", imu_bus, strerror(-r));
        return 1;
    }
    r = qmc5883p_init(&mag, mag_bus, QMC5883P_ADDR_DEFAULT, &QMC5883P_CONFIG_DEFAULT);
    if (r < 0)
        r = qmc5883p_init(&mag, mag_bus, QMC5883P_ADDR_ALT, &QMC5883P_CONFIG_DEFAULT);
    if (r < 0) {
        fprintf(stderr, "qmc5883p init (%s): %s\n", mag_bus, strerror(-r));
        mpu6500_close(&imu);
        return 1;
    }
    fprintf(stderr, "mpu6500 on %s, qmc5883p on %s @0x%02X\n", imu_bus, mag_bus, mag.i2c.addr);

    mpu6500_sample_t a = {0};
    qmc5883p_sample_t m = {0};
    unsigned imu_n = 0, mag_n = 0, errs = 0, tick = 0;
    long prints = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t last_print = mono_ns();

    printf("%-26s %-26s %-6s %-30s %-6s %s\n",
           "accel m/s2 (x y z)", "gyro dps (x y z)", "|a|", "mag uT (x y z)", "|B|", "rates");

    while (running && (max_prints < 0 || prints < max_prints)) {
        if (mpu6500_read(&imu, &a) == 0) {
            imu_n++;
            calib_apply_accel(&cal, a.accel, a.accel);
            calib_apply_gyro(&cal, a.gyro, a.gyro);
        } else errs++;
        if ((tick++ & 1) == 0) {               /* mag at 100 Hz */
            r = qmc5883p_read(&mag, &m);
            if (r == 0) { mag_n++; calib_apply_mag(&cal, m.field, m.field); }
            else if (r != -EAGAIN) errs++;
        }

        uint64_t now = mono_ns();
        if (now - last_print >= 100000000ull) {
            double dt = (now - last_print) / 1e9;
            const float k = 180.0f / (float)M_PI;
            printf("%7.3f %7.3f %7.3f   %7.2f %7.2f %7.2f   %6.3f %8.2f %8.2f %8.2f   %6.2f %.0f/%.0fHz%s%s T=%.1fC rec=%u(cfg %u stall %u)\n",
                   a.accel[0], a.accel[1], a.accel[2],
                   a.gyro[0] * k, a.gyro[1] * k, a.gyro[2] * k, norm3(a.accel),
                   m.field[0], m.field[1], m.field[2], norm3(m.field),
                   imu_n / dt, mag_n / dt, m.overflow ? " OVF" : "",
                   errs ? " ERR" : "", a.temp_c, mag.recoveries, mag.rec_config, mag.rec_stall);
            fflush(stdout);
            imu_n = mag_n = errs = 0;
            last_print = now;
            prints++;
        }

        next.tv_nsec += 5000000L; /* 200 Hz */
        if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    qmc5883p_close(&mag);
    mpu6500_close(&imu);
    return 0;
}
