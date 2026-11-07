/* Gyro bias calibration + level check. Runs automatically before every mission.
 * The vehicle must be still. Exit codes: 0 ok, 1 hardware/IO error, 2 movement detected.
 * Usage: calib_gyro [-t SECONDS] [-f CALIB.json] [-b IMU_BUS] [-n (dry run, don't save)] */
#include "common.h"

#include <stdlib.h>
#include <unistd.h>

#define BIAS_WARN (10.0 / RAD2DEG)  /* MPU6500 spec is +-5 deg/s typical */

int main(int argc, char **argv)
{
    const char *path = CALIB_PATH_DEFAULT, *bus = IMU_BUS_DEFAULT;
    double secs = 5.0;
    int dry = 0, opt;

    while ((opt = getopt(argc, argv, "t:f:b:n")) != -1) {
        if (opt == 't') secs = atof(optarg);
        else if (opt == 'f') path = optarg;
        else if (opt == 'b') bus = optarg;
        else if (opt == 'n') dry = 1;
        else { fprintf(stderr, "uso: %s [-t SEG] [-f ARQ] [-b BUS] [-n]\n", argv[0]); return 1; }
    }
    if (secs < 1.0) secs = 1.0;

    calib_t cal;
    int r = calib_load(&cal, path);
    if (r < 0) { fprintf(stderr, "erro: lendo %s: %s\n", path, strerror(-r)); return 1; }

    mpu6500_t imu;
    if (open_imu(&imu, bus) < 0) return 1;

    stats3_t g, a;
    stats3_reset(&g);
    stats3_reset(&a);
    double temp = 0;
    unsigned errs = 0, total = (unsigned)(secs * 200);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    printf("calib_gyro: medindo %.1f s, mantenha parado...\n", secs);
    for (unsigned i = 0; i < total; i++) {
        mpu6500_sample_t s;
        if (mpu6500_read(&imu, &s) == 0) {
            stats3_add(&g, s.gyro);
            stats3_add(&a, s.accel);
            temp += s.temp_c;
        } else if (++errs > total / 10) {
            fprintf(stderr, "erro: muitas falhas de leitura do MPU6500\n");
            mpu6500_close(&imu);
            return 1;
        }
        tick_wait(&next, 5000000L);
    }
    mpu6500_close(&imu);
    temp /= g.n;

    double gstd = stats3_max_std(&g), astd = stats3_max_std(&a);
    printf("  ruido: gyro %.3f deg/s, accel %.3f m/s2 (limites %.2f / %.2f)\n",
           gstd * RAD2DEG, astd, STILL_GYRO_STD * RAD2DEG, STILL_ACCEL_STD);
    if (gstd > STILL_GYRO_STD || astd > STILL_ACCEL_STD) {
        fprintf(stderr, "FALHA: movimento detectado durante a calibracao do giroscopio\n");
        return 2;
    }

    printf("  bias: %+.3f %+.3f %+.3f deg/s  @ %.1f C\n",
           g.mean[0] * RAD2DEG, g.mean[1] * RAD2DEG, g.mean[2] * RAD2DEG, temp);
    for (int i = 0; i < 3; i++)
        if (fabs(g.mean[i]) > BIAS_WARN)
            printf("  AVISO: bias do eixo %c acima de %.0f deg/s\n", 'X' + i, BIAS_WARN * RAD2DEG);

    /* Level check (informative): uses accel calibration if available. */
    float am[3] = { (float)a.mean[0], (float)a.mean[1], (float)a.mean[2] }, ac[3];
    calib_apply_accel(&cal, am, ac);
    double an = sqrt(ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2]);
    double tilt = acos(fmin(1.0, fabs(ac[2]) / an)) * RAD2DEG;
    printf("  nivel: |a|=%.3f m/s2 (%+.1f%% de g), inclinacao %.1f deg%s\n",
           an, (an / GRAVITY - 1) * 100, tilt, cal.accel_valid ? "" : " (accel nao calibrado)");

    for (int i = 0; i < 3; i++) {
        cal.gyro_bias[i] = g.mean[i];
        cal.gyro_std[i] = stats3_std(&g, i);
    }
    cal.gyro_temp_c = temp;
    cal.gyro_valid = true;
    calib_timestamp(cal.gyro_time);

    if (dry) {
        printf("  (dry run: nada salvo)\n");
        return 0;
    }
    if ((r = calib_save(&cal, path)) < 0) {
        fprintf(stderr, "erro: salvando %s: %s\n", path, strerror(-r));
        return 1;
    }
    printf("OK: salvo em %s\n", path);
    return 0;
}
