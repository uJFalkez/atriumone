/* Accelerometer offset + scale calibration (bench, once).
 * Place the board with each axis pointing up and down (6 faces) in any order;
 * each face is captured automatically after 2 s of stillness. Tilt up to ~30 deg
 * is tolerated (axis-aligned ellipsoid fit), so a flat table is enough.
 * Usage: calib_accel [-f CALIB.json] [-b IMU_BUS] [-T TIMEOUT_S] [-n (dry run)] */
#include "common.h"
#include "linalg.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define BLOCK        50     /* 0.25 s at 200 Hz */
#define HOLD_BLOCKS  8      /* 2 s still to capture */
#define DOMINANT_MIN 8.5    /* m/s^2 on the up axis: tilt below ~30 deg */

static const char *face_name[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static int face_of(const double m[3])
{
    int ax = 0;
    for (int i = 1; i < 3; i++)
        if (fabs(m[i]) > fabs(m[ax])) ax = i;
    if (fabs(m[ax]) < DOMINANT_MIN) return -1;
    return ax * 2 + (m[ax] < 0);
}

int main(int argc, char **argv)
{
    const char *path = CALIB_PATH_DEFAULT, *bus = IMU_BUS_DEFAULT;
    double timeout = 300;
    int dry = 0, opt;

    while ((opt = getopt(argc, argv, "f:b:T:n")) != -1) {
        if (opt == 'f') path = optarg;
        else if (opt == 'b') bus = optarg;
        else if (opt == 'T') timeout = atof(optarg);
        else if (opt == 'n') dry = 1;
        else { fprintf(stderr, "uso: %s [-f ARQ] [-b BUS] [-T SEG] [-n]\n", argv[0]); return 1; }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    calib_t cal;
    int r = calib_load(&cal, path);
    if (r < 0) { fprintf(stderr, "erro: lendo %s: %s\n", path, strerror(-r)); return 1; }

    mpu6500_t imu;
    if (open_imu(&imu, bus) < 0) return 1;

    printf("calib_accel: apoie a placa com cada face para CIMA (+X -X +Y -Y +Z -Z),\n"
           "em qualquer ordem. Cada uma e capturada apos 2 s parada. Ctrl+C cancela.\n\n");

    double cap[6][3];
    int have[6] = {0}, ncap = 0, hold_face = -1, hold_n = 0;
    double hold_sum[3] = {0};
    stats3_t ba, bg;
    stats3_reset(&ba);
    stats3_reset(&bg);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t deadline = mono_ns() + (uint64_t)(timeout * 1e9);

    while (running && ncap < 6 && mono_ns() < deadline) {
        mpu6500_sample_t s;
        if (mpu6500_read(&imu, &s) == 0) {
            stats3_add(&ba, s.accel);
            stats3_add(&bg, s.gyro);
        }
        if (ba.n >= BLOCK) {
            int still = stats3_max_std(&ba) < STILL_ACCEL_STD && stats3_max_std(&bg) < STILL_GYRO_STD;
            int face = face_of(ba.mean);
            if (!still || face < 0 || face != hold_face) {
                hold_n = 0;
                memset(hold_sum, 0, sizeof hold_sum);
                hold_face = still ? face : -1;
            }
            if (still && face >= 0) {
                for (int i = 0; i < 3; i++) hold_sum[i] += ba.mean[i];
                hold_n++;
            }
            if (hold_n >= HOLD_BLOCKS && !have[hold_face]) {
                for (int i = 0; i < 3; i++) cap[hold_face][i] = hold_sum[i] / hold_n;
                have[hold_face] = 1;
                ncap++;
                printf("\r\033[K  capturado %s  (%d/6)  a = %+.3f %+.3f %+.3f\a\n", face_name[hold_face], ncap,
                       cap[hold_face][0], cap[hold_face][1], cap[hold_face][2]);
            }

            char missing[32] = "";
            for (int f = 0; f < 6; f++)
                if (!have[f]) { strcat(missing, face_name[f]); strcat(missing, " "); }
            printf("\r\033[K  %s | face %s | faltam: %s",
                   !still ? "movendo" : face < 0 ? "inclinado demais" :
                   have[face] ? "ja capturada" : "segurando...",
                   face >= 0 ? face_name[face] : "--", missing);
            fflush(stdout);
            stats3_reset(&ba);
            stats3_reset(&bg);
        }
        tick_wait(&next, 5000000L);
    }
    mpu6500_close(&imu);
    printf("\n");

    if (ncap < 6) {
        fprintf(stderr, "cancelado: %d/6 faces capturadas, nada salvo\n", ncap);
        return 2;
    }

    la_ellipsoid_t e;
    if ((r = la_fit_ellipsoid((const double (*)[3])cap, 6, 1, GRAVITY, &e)) < 0) {
        fprintf(stderr, "FALHA: ajuste invalido (%d)\n", r);
        return 2;
    }

    printf("resultado:\n  offset: %+.4f %+.4f %+.4f m/s2\n  escala: %.5f %.5f %.5f\n",
           e.offset[0], e.offset[1], e.offset[2], e.W[0][0], e.W[1][1], e.W[2][2]);
    printf("  |a| corrigido por face (esperado %.4f):\n", GRAVITY);
    double worst = 0;
    for (int f = 0; f < 6; f++) {
        double c[3];
        for (int i = 0; i < 3; i++) c[i] = (cap[f][i] - e.offset[i]) * e.W[i][i];
        double err = norm3d(c) - GRAVITY;
        worst = fmax(worst, fabs(err));
        printf("    %s: %.4f (%+.4f)\n", face_name[f], norm3d(c), err);
    }

    int warn = 0;
    for (int i = 0; i < 3; i++) {
        if (fabs(e.offset[i]) > 2.0) { printf("  AVISO: offset do eixo %c alto\n", 'X' + i); warn = 1; }
        if (fabs(e.W[i][i] - 1) > 0.1) { printf("  AVISO: escala do eixo %c fora de +-10%%\n", 'X' + i); warn = 1; }
    }
    if (warn) printf("  (valores fora do tipico: confira se a placa estava mesmo parada em cada face)\n");

    for (int i = 0; i < 3; i++) {
        cal.accel_offset[i] = e.offset[i];
        cal.accel_scale[i] = e.W[i][i];
    }
    cal.accel_residual = worst;
    cal.accel_valid = true;
    calib_timestamp(cal.accel_time);

    if (dry) { printf("(dry run: nada salvo)\n"); return 0; }
    if ((r = calib_save(&cal, path)) < 0) {
        fprintf(stderr, "erro: salvando %s: %s\n", path, strerror(-r));
        return 1;
    }
    printf("OK: salvo em %s\n", path);
    return 0;
}
