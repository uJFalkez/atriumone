#include "linalg.h"

#include <math.h>
#include <string.h>

int la_solve(int n, double *A, double *b)
{
    for (int c = 0; c < n; c++) {
        int p = c;
        double best = fabs(A[c * n + c]);
        for (int r = c + 1; r < n; r++)
            if (fabs(A[r * n + c]) > best) { best = fabs(A[r * n + c]); p = r; }
        if (best < 1e-12)
            return -1;
        if (p != c) {
            for (int k = 0; k < n; k++) {
                double t = A[c * n + k]; A[c * n + k] = A[p * n + k]; A[p * n + k] = t;
            }
            double t = b[c]; b[c] = b[p]; b[p] = t;
        }
        for (int r = c + 1; r < n; r++) {
            double f = A[r * n + c] / A[c * n + c];
            if (f == 0.0) continue;
            for (int k = c; k < n; k++) A[r * n + k] -= f * A[c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < n; k++) s -= A[r * n + k] * b[k];
        b[r] = s / A[r * n + r];
    }
    return 0;
}

void la_eig_sym3(const double S[3][3], double w[3], double V[3][3])
{
    double a[3][3];
    memcpy(a, S, sizeof a);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            V[i][j] = (i == j);

    for (int sweep = 0; sweep < 50; sweep++) {
        double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; p++) {
            for (int q = p + 1; q < 3; q++) {
                if (fabs(a[p][q]) < 1e-300) continue;
                double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                double c = 1.0 / sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 3; k++) {   /* A <- A P */
                    double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; k++) {   /* A <- P^T A */
                    double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; k++) {   /* V <- V P */
                    double vkp = V[k][p], vkq = V[k][q];
                    V[k][p] = c * vkp - s * vkq;
                    V[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int i = 0; i < 3; i++) w[i] = a[i][i];
}

int la_fit_ellipsoid(const double (*pts)[3], int n, int axis_aligned,
                     double target_radius, la_ellipsoid_t *e)
{
    const int m = axis_aligned ? 6 : 9;
    if (n < m)
        return -1;

    /* Normalize (translate + uniform scale) for numerical conditioning. */
    double mean[3] = {0}, s = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) mean[k] += pts[i][k];
    for (int k = 0; k < 3; k++) mean[k] /= n;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) s += (pts[i][k] - mean[k]) * (pts[i][k] - mean[k]);
    s = sqrt(s / n);
    if (!(s > 0))
        return -1;

    /* Normal equations for  sum_j coef_j * row_j(u) = 1. */
    double N[81] = {0}, r[9] = {0};
    for (int i = 0; i < n; i++) {
        double x = (pts[i][0] - mean[0]) / s, y = (pts[i][1] - mean[1]) / s, z = (pts[i][2] - mean[2]) / s;
        double row[9];
        if (axis_aligned) {
            double t[6] = { x * x, y * y, z * z, 2 * x, 2 * y, 2 * z };
            memcpy(row, t, sizeof t);
        } else {
            double t[9] = { x * x, y * y, z * z, 2 * y * z, 2 * x * z, 2 * x * y, 2 * x, 2 * y, 2 * z };
            memcpy(row, t, sizeof t);
        }
        for (int a = 0; a < m; a++) {
            r[a] += row[a];
            for (int b = 0; b < m; b++) N[a * m + b] += row[a] * row[b];
        }
    }
    if (la_solve(m, N, r) < 0)
        return -2;

    double M[3][3], v[3];
    if (axis_aligned) {
        double Mt[3][3] = { { r[0], 0, 0 }, { 0, r[1], 0 }, { 0, 0, r[2] } };
        memcpy(M, Mt, sizeof M);
        v[0] = r[3]; v[1] = r[4]; v[2] = r[5];
    } else {
        double Mt[3][3] = { { r[0], r[5], r[4] }, { r[5], r[1], r[3] }, { r[4], r[3], r[2] } };
        memcpy(M, Mt, sizeof M);
        v[0] = r[6]; v[1] = r[7]; v[2] = r[8];
    }

    /* Center: M c = -v. */
    double Mc[9], c[3] = { -v[0], -v[1], -v[2] };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) Mc[i * 3 + j] = M[i][j];
    if (la_solve(3, Mc, c) < 0)
        return -3;

    double k = 1.0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) k += c[i] * M[i][j] * c[j];
    if (!(k > 0))
        return -4;

    double A[3][3], w[3], Q[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) A[i][j] = M[i][j] / k;
    la_eig_sym3(A, w, Q);
    if (!(w[0] > 0 && w[1] > 0 && w[2] > 0))
        return -5;  /* not an ellipsoid (e.g. too little rotation coverage) */

    double Ru = pow(w[0] * w[1] * w[2], -1.0 / 6.0);
    double R  = target_radius > 0 ? target_radius : Ru * s;

    for (int i = 0; i < 3; i++) {
        e->offset[i] = mean[i] + s * c[i];
        e->axes[i]   = s / sqrt(w[i]);
        for (int j = 0; j < 3; j++) {
            double sq = 0;  /* sqrt(A) = Q diag(sqrt w) Q^T */
            for (int l = 0; l < 3; l++) sq += Q[i][l] * sqrt(w[l]) * Q[j][l];
            e->W[i][j] = sq * R / s;
        }
    }
    e->radius = R;
    e->natural_radius = Ru * s;

    double ss = 0;
    for (int i = 0; i < n; i++) {
        double d[3], cor[3];
        for (int a = 0; a < 3; a++) d[a] = pts[i][a] - e->offset[a];
        for (int a = 0; a < 3; a++) cor[a] = e->W[a][0] * d[0] + e->W[a][1] * d[1] + e->W[a][2] * d[2];
        double err = sqrt(cor[0] * cor[0] + cor[1] * cor[1] + cor[2] * cor[2]) - R;
        ss += err * err;
    }
    e->residual = sqrt(ss / n);
    return 0;
}
