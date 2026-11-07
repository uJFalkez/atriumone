/* Offline test of the ellipsoid fits with synthetic, known distortions. No hardware. */
#include "linalg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static double randn(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static void apply(const double M[3][3], const double v[3], double out[3])
{
    for (int i = 0; i < 3; i++) out[i] = M[i][0] * v[0] + M[i][1] * v[1] + M[i][2] * v[2];
}

static int fails;
#define CHECK(cond, ...) do { int _ok = (cond); printf("  [%s] ", _ok ? " ok " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!_ok) fails++; } while (0)

static void test_mag(void)
{
    printf("mag: general ellipsoid, 3000 points, noise 0.2 uT\n");
    const double field = 23.0, off[3] = { 12.5, -30.0, 7.25 };
    /* soft iron: symmetric positive definite with cross terms */
    const double D[3][3] = { { 1.15, 0.08, -0.05 }, { 0.08, 0.90, 0.04 }, { -0.05, 0.04, 1.02 } };
    static double pts[3000][3];
    for (int i = 0; i < 3000; i++) {
        double u[3] = { randn(), randn(), randn() }, nrm = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]), d[3];
        for (int k = 0; k < 3; k++) u[k] *= field / nrm;
        apply(D, u, d);
        for (int k = 0; k < 3; k++) pts[i][k] = d[k] + off[k] + 0.2 * randn();
    }
    la_ellipsoid_t e;
    int r = la_fit_ellipsoid((const double (*)[3])pts, 3000, 0, 0, &e);
    CHECK(r == 0, "fit returned %d", r);
    if (r) return;
    double oerr = 0;
    for (int k = 0; k < 3; k++) oerr = fmax(oerr, fabs(e.offset[k] - off[k]));
    CHECK(oerr < 0.1, "offset error %.4f uT (got %.3f %.3f %.3f)", oerr, e.offset[0], e.offset[1], e.offset[2]);
    CHECK(e.residual < 0.3, "residual %.3f uT (noise 0.2)", e.residual);
    /* W*D should be a scaled rotation: (W D)(W D)^T = (R/field)^2 I */
    double WD[3][3], worst = 0, s = e.radius / field;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            WD[i][j] = 0;
            for (int l = 0; l < 3; l++) WD[i][j] += e.W[i][l] * D[l][j];
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double g = 0;
            for (int l = 0; l < 3; l++) g += WD[i][l] * WD[j][l];
            worst = fmax(worst, fabs(g - (i == j) * s * s) / (s * s));
        }
    CHECK(worst < 0.01, "soft-iron removed: max deviation of (WD)(WD)^T from sphere %.4f", worst);
    CHECK(fabs(e.radius - field * cbrt(1.15 * 0.90 * 1.02)) < 1.0, "radius %.2f uT", e.radius);
}

static void test_accel(void)
{
    printf("accel: axis-aligned fit, 6 positions tilted up to ~15 deg, target g\n");
    const double g = 9.80665, off[3] = { -0.25, 0.12, 0.30 }, sc[3] = { 1.01, 0.985, 1.02 };
    const double dirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    double pts[6][3];
    for (int i = 0; i < 6; i++) {
        double d[3], n = 0;
        for (int k = 0; k < 3; k++) { d[k] = dirs[i][k] + 0.25 * (rand() / (double)RAND_MAX - 0.5); n += d[k] * d[k]; }
        for (int k = 0; k < 3; k++) pts[i][k] = d[k] / sqrt(n) * g * sc[k] + off[k] + 0.002 * randn();
    }
    la_ellipsoid_t e;
    int r = la_fit_ellipsoid((const double (*)[3])pts, 6, 1, g, &e);
    CHECK(r == 0, "fit returned %d", r);
    if (r) return;
    double oerr = 0, serr = 0;
    for (int k = 0; k < 3; k++) {
        oerr = fmax(oerr, fabs(e.offset[k] - off[k]));
        serr = fmax(serr, fabs(e.W[k][k] - 1.0 / sc[k]));
    }
    CHECK(oerr < 0.01, "offset error %.4f m/s2", oerr);
    CHECK(serr < 0.002, "scale error %.5f", serr);
    CHECK(fabs(e.W[0][1]) + fabs(e.W[0][2]) + fabs(e.W[1][2]) < 1e-9, "matrix is diagonal");
}

static void test_degenerate(void)
{
    printf("degenerate: points only in a plane (no rotation coverage)\n");
    static double pts[500][3];
    for (int i = 0; i < 500; i++) {
        double a = i * 0.05;
        pts[i][0] = 20 * cos(a); pts[i][1] = 20 * sin(a); pts[i][2] = 5 + 0.1 * randn();
    }
    la_ellipsoid_t e;
    int r = la_fit_ellipsoid((const double (*)[3])pts, 500, 0, 0, &e);
    CHECK(r != 0 || e.residual > 1 || e.axes[0] > 100 || e.axes[1] > 100 || e.axes[2] > 100,
          "rejected or flagged as unreliable (ret %d)", r);
}

int main(void)
{
    srand(42);
    test_mag();
    test_accel();
    test_degenerate();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
