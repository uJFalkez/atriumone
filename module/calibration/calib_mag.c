/* Magnetometer hard/soft-iron calibration (once, ideally mounted on the drone).
 * Rotate the assembly slowly through every orientation; live feedback shows which
 * directions are still missing. Fits a general ellipsoid.
 * Usage: calib_mag [-t SECONDS] [-f CALIB.json] [-m MAG_BUS] [-F (save even if coverage is low)] [-n] */
#include "common.h"
#include "linalg.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_PTS      20000
#define MIN_STEP_UT  0.5    /* store a point only after moving this far (avoid over-weighting rest) */
#define CONE_COS     0.82   /* ~35 deg around each of 14 directions */
#define MIN_PER_DIR  0.005  /* fraction of points needed to count a direction as covered */
#define COVERAGE_MIN (12.0 / 14.0)
#define RESID_MAX    0.05   /* residual / radius */

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static double pts[MAX_PTS][3];

/* 6 axes + 8 diagonals */
static const double dirs[14][3] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    { .577, .577, .577 }, { .577, .577, -.577 }, { .577, -.577, .577 }, { .577, -.577, -.577 },
    { -.577, .577, .577 }, { -.577, .577, -.577 }, { -.577, -.577, .577 }, { -.577, -.577, -.577 },
};
static const char *dir_name[14] = {
    "+X", "-X", "+Y", "-Y", "+Z", "-Z",
    "+++", "++-", "+-+", "+--", "-++", "-+-", "--+", "---",
};

/* Coverage of directions of W*(p - center); W may be NULL (identity). */
static int coverage(int n, const double c[3], const double (*W)[3], int covered[14])
{
    int cnt[14] = {0}, hit = 0;
    for (int i = 0; i < n; i++) {
        double d[3], u[3];
        for (int k = 0; k < 3; k++) d[k] = pts[i][k] - c[k];
        for (int k = 0; k < 3; k++) u[k] = W ? W[k][0] * d[0] + W[k][1] * d[1] + W[k][2] * d[2] : d[k];
        double nn = norm3d(u);
        if (nn <= 0) continue;
        for (int j = 0; j < 14; j++)
            if ((u[0] * dirs[j][0] + u[1] * dirs[j][1] + u[2] * dirs[j][2]) / nn > CONE_COS) cnt[j]++;
    }
    int need = (int)fmax(3, n * MIN_PER_DIR);
    for (int j = 0; j < 14; j++) {
        covered[j] = cnt[j] >= need;
        hit += covered[j];
    }
    return hit;
}

int main(int argc, char **argv)
{
    const char *path = CALIB_PATH_DEFAULT, *bus = MAG_BUS_DEFAULT;
    double secs = 60;
    int force = 0, dry = 0, opt;

    while ((opt = getopt(argc, argv, "t:f:m:Fn")) != -1) {
        if (opt == 't') secs = atof(optarg);
        else if (opt == 'f') path = optarg;
        else if (opt == 'm') bus = optarg;
        else if (opt == 'F') force = 1;
        else if (opt == 'n') dry = 1;
        else { fprintf(stderr, "uso: %s [-t SEG] [-f ARQ] [-m BUS] [-F] [-n]\n", argv[0]); return 1; }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    calib_t cal;
    int r = calib_load(&cal, path);
    if (r < 0) { fprintf(stderr, "erro: lendo %s: %s\n", path, strerror(-r)); return 1; }

    qmc5883p_t mag;
    if (open_mag(&mag, bus) < 0) return 1;

    printf("calib_mag: gire o conjunto devagar em TODAS as direcoes por %.0f s\n"
           "(como desenhar um 8 no ar, virando de cabeca para baixo tambem).\n"
           "Longe de metal, celulares e cabos com corrente. Ctrl+C termina antes.\n\n", secs);

    int n = 0, covered[14];
    double lo[3] = { 1e9, 1e9, 1e9 }, hi[3] = { -1e9, -1e9, -1e9 }, last[3] = { 1e9, 1e9, 1e9 };
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t start = mono_ns(), end = start + (uint64_t)(secs * 1e9), last_ui = 0;

    while (running && mono_ns() < end && n < MAX_PTS) {
        qmc5883p_sample_t s;
        if (qmc5883p_read(&mag, &s) == 0) {
            double d = 0;
            for (int k = 0; k < 3; k++) d += (s.field[k] - last[k]) * (s.field[k] - last[k]);
            if (sqrt(d) >= MIN_STEP_UT) {
                for (int k = 0; k < 3; k++) {
                    pts[n][k] = last[k] = s.field[k];
                    lo[k] = fmin(lo[k], s.field[k]);
                    hi[k] = fmax(hi[k], s.field[k]);
                }
                n++;
            }
        }
        uint64_t now = mono_ns();
        if (now - last_ui >= 1000000000ull && n > 20) {
            double c[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
            int hit = coverage(n, c, NULL, covered);
            char missing[80] = "";
            for (int j = 0; j < 14; j++)
                if (!covered[j]) { strcat(missing, dir_name[j]); strcat(missing, " "); }
            printf("\r\033[K  %3.0fs  pontos %5d  cobertura %2d/14  faltam: %s",
                   (now - start) / 1e9, n, hit, hit == 14 ? "nada" : missing);
            fflush(stdout);
            last_ui = now;
        }
        tick_wait(&next, 10000000L);  /* 100 Hz */
    }
    qmc5883p_close(&mag);
    printf("\n");

    if (n < 200) {
        fprintf(stderr, "FALHA: poucos pontos (%d). Gire mais o conjunto.\n", n);
        return 2;
    }

    la_ellipsoid_t e;
    if ((r = la_fit_ellipsoid((const double (*)[3])pts, n, 0, 0, &e)) < 0) {
        fprintf(stderr, "FALHA: os pontos nao formam um elipsoide (%d): cobertura insuficiente\n", r);
        return 2;
    }
    int hit = coverage(n, e.offset, (const double (*)[3])e.W, covered);
    double cov = hit / 14.0, rel = e.residual / e.radius;
    double amax = fmax(e.axes[0], fmax(e.axes[1], e.axes[2]));
    double amin = fmin(e.axes[0], fmin(e.axes[1], e.axes[2]));

    printf("resultado (%d pontos):\n", n);
    printf("  hard iron: %+.2f %+.2f %+.2f uT\n", e.offset[0], e.offset[1], e.offset[2]);
    printf("  soft iron: [%+.4f %+.4f %+.4f]\n", e.W[0][0], e.W[0][1], e.W[0][2]);
    printf("             [%+.4f %+.4f %+.4f]\n", e.W[1][0], e.W[1][1], e.W[1][2]);
    printf("             [%+.4f %+.4f %+.4f]\n", e.W[2][0], e.W[2][1], e.W[2][2]);
    printf("  campo: %.2f uT | residuo %.3f uT (%.1f%%) | eixos max/min %.3f | cobertura %d/14\n",
           e.radius, e.residual, rel * 100, amax / amin, hit);

    int bad = 0;
    if (cov < COVERAGE_MIN) { printf("  RUIM: cobertura baixa, gire em mais direcoes\n"); bad = 1; }
    if (rel > RESID_MAX)    { printf("  RUIM: residuo alto (interferencia ou movimento brusco perto de metal)\n"); bad = 1; }
    if (e.radius < 15 || e.radius > 75)
        printf("  AVISO: campo %.1f uT fora do esperado para a Terra (~20-65 uT)\n", e.radius);
    if (amax / amin > 1.5)
        printf("  AVISO: distorcao forte (soft iron): ha metal/ima muito perto do sensor?\n");
    if (bad && !force) {
        fprintf(stderr, "nao salvo (use -F para salvar mesmo assim)\n");
        return 2;
    }

    memcpy(cal.mag_offset, e.offset, sizeof cal.mag_offset);
    memcpy(cal.mag_matrix, e.W, sizeof cal.mag_matrix);
    cal.mag_field = e.radius;
    cal.mag_residual = e.residual;
    cal.mag_coverage = cov;
    cal.mag_valid = true;
    calib_timestamp(cal.mag_time);

    if (dry) { printf("(dry run: nada salvo)\n"); return 0; }
    if ((r = calib_save(&cal, path)) < 0) {
        fprintf(stderr, "erro: salvando %s: %s\n", path, strerror(-r));
        return 1;
    }
    printf("OK: salvo em %s\n", path);
    return 0;
}
