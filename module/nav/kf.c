#include "kf.h"

#include <string.h>

void kf_reset(kf_t *k, double bias_sigma)
{
    double as = k->accel_sigma, rw = k->bias_rw;
    memset(k, 0, sizeof *k);
    k->accel_sigma = as > 0 ? as : 0.5;
    k->bias_rw = rw > 0 ? rw : 0.002;
    for (int i = 0; i < 3; i++) {
        k->ax[i].P[0][0] = 0.01;          /* origin is defined, ~10 cm */
        k->ax[i].P[1][1] = 0.01;          /* at rest */
        k->ax[i].P[2][2] = bias_sigma * bias_sigma;
    }
}

static void predict_axis(kf_axis_t *s, double a, double dt, double qa, double qb)
{
    double h = 0.5 * dt * dt;
    double ac = a - s->x[2];
    s->x[0] += s->x[1] * dt + h * ac;
    s->x[1] += dt * ac;

    /* F = [[1, dt, -h], [0, 1, -dt], [0, 0, 1]] */
    double F[3][3] = { { 1, dt, -h }, { 0, 1, -dt }, { 0, 0, 1 } };
    double FP[3][3] = { { 0 } }, P[3][3] = { { 0 } };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int m = 0; m < 3; m++) FP[i][j] += F[i][m] * s->P[m][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int m = 0; m < 3; m++) P[i][j] += FP[i][m] * F[j][m];
    /* Q: acceleration noise enters through G = [h, dt, 0]; bias random walk. */
    double G[3] = { h, dt, 0 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) P[i][j] += G[i] * G[j] * qa;
    P[2][2] += qb * dt;
    memcpy(s->P, P, sizeof P);
}

void kf_predict(kf_t *k, const double a[3], double dt)
{
    double qa = k->accel_sigma * k->accel_sigma, qb = k->bias_rw * k->bias_rw;
    for (int i = 0; i < 3; i++) predict_axis(&k->ax[i], a[i], dt, qa, qb);
}

double kf_update(kf_t *k, int axis, int what, double z, double sigma)
{
    kf_axis_t *s = &k->ax[axis];
    int m = what;                         /* H selects state m */
    double y = z - s->x[m];
    double S = s->P[m][m] + sigma * sigma;
    double K[3];
    for (int i = 0; i < 3; i++) K[i] = s->P[i][m] / S;
    for (int i = 0; i < 3; i++) s->x[i] += K[i] * y;
    /* Joseph-free form is fine here: P = P - K H P, then symmetrize. */
    double Prow[3] = { s->P[m][0], s->P[m][1], s->P[m][2] };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) s->P[i][j] -= K[i] * Prow[j];
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++) s->P[i][j] = s->P[j][i] = 0.5 * (s->P[i][j] + s->P[j][i]);
    return y * y / S;
}

void kf_shift(kf_t *k, const double d[3])
{
    for (int i = 0; i < 3; i++) k->ax[i].x[0] += d[i];
}
