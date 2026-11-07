#include "attitude.h"

#include <math.h>

#define G 9.80665

static void q_normalize(double q[4])
{
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
}

/* r = a ⊗ b */
static void q_mul(const double a[4], const double b[4], double r[4])
{
    double t[4] = {
        a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
        a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
        a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
        a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
    };
    for (int i = 0; i < 4; i++) r[i] = t[i];
}

void attitude_init(attitude_t *a)
{
    a->q[0] = 1; a->q[1] = a->q[2] = a->q[3] = 0;
    a->kp = 1.0;
    a->accel_gate = 0.10;
    a->mag_gain = 0.5;
    a->declination = 0.0;
    a->yaw_valid = false;
    a->accel_used = a->accel_skipped = 0;
}

void attitude_rotate(const attitude_t *a, const double v[3], double o[3])
{
    const double w = a->q[0], x = a->q[1], y = a->q[2], z = a->q[3];
    o[0] = (1 - 2 * (y * y + z * z)) * v[0] + 2 * (x * y - w * z) * v[1] + 2 * (x * z + w * y) * v[2];
    o[1] = 2 * (x * y + w * z) * v[0] + (1 - 2 * (x * x + z * z)) * v[1] + 2 * (y * z - w * x) * v[2];
    o[2] = 2 * (x * z - w * y) * v[0] + 2 * (y * z + w * x) * v[1] + (1 - 2 * (x * x + y * y)) * v[2];
}

void attitude_reset_from_accel(attitude_t *a, const double f[3])
{
    /* Shortest rotation taking the measured "up" (body) onto world +Z, yaw = 0. */
    double n = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    double u[3] = { f[0] / n, f[1] / n, f[2] / n };
    /* axis = u × z, cos = u·z */
    double ax = u[1], ay = -u[0], c = u[2];
    if (c < -0.999999) {            /* upside down: rotate 180° about X */
        a->q[0] = 0; a->q[1] = 1; a->q[2] = 0; a->q[3] = 0;
    } else {
        a->q[0] = 1 + c; a->q[1] = ax; a->q[2] = ay; a->q[3] = 0;
        q_normalize(a->q);
    }
    a->yaw_valid = false;
}

void attitude_update(attitude_t *a, const double g[3], const double f[3], double dt)
{
    double w[3] = { g[0], g[1], g[2] };
    double n = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);

    if (n > 0 && fabs(n / G - 1.0) < a->accel_gate) {
        const double qw = a->q[0], qx = a->q[1], qy = a->q[2], qz = a->q[3];
        /* Estimated world-up expressed in body = third row of R. */
        double v[3] = { 2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy) };
        double u[3] = { f[0] / n, f[1] / n, f[2] / n };
        double e[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        for (int i = 0; i < 3; i++) w[i] += a->kp * e[i];
        a->accel_used++;
    } else {
        a->accel_skipped++;
    }

    double h = 0.5 * dt;
    double dq[4] = { 0, w[0] * h, w[1] * h, w[2] * h };
    double qd[4];
    q_mul(a->q, dq, qd);
    for (int i = 0; i < 4; i++) a->q[i] += qd[i];
    q_normalize(a->q);
}

static double wrap_pi(double x)
{
    while (x > M_PI) x -= 2 * M_PI;
    while (x < -M_PI) x += 2 * M_PI;
    return x;
}

double attitude_mag_correct(attitude_t *a, const double m_body[3])
{
    double m[3];
    attitude_rotate(a, m_body, m);
    if (hypot(m[0], m[1]) < 1e-6)
        return 0.0;
    /* Azimuth of the horizontal field, clockwise from estimated north. The true azimuth of
     * magnetic north is the declination. A CCW world rotation by phi lowers it by phi. */
    double phi = wrap_pi(atan2(m[0], m[1]) - a->declination);
    double k = a->yaw_valid ? a->mag_gain : 1.0;
    double ang = k * phi;
    double rz[4] = { cos(ang / 2), 0, 0, sin(ang / 2) };
    q_mul(rz, a->q, a->q);
    q_normalize(a->q);
    a->yaw_valid = true;
    return phi;
}

void attitude_euler(const attitude_t *a, double *roll, double *pitch, double *heading)
{
    const double w = a->q[0], x = a->q[1], y = a->q[2], z = a->q[3];
    /* Body assumed X forward, Y left, Z up. Roll > 0 = right side down,
     * pitch > 0 = nose (+X) up. r2j = world-up component of body axis j. */
    double r20 = 2 * (x * z - w * y), r21 = 2 * (y * z + w * x), r22 = 1 - 2 * (x * x + y * y);
    *roll = atan2(r21, r22);
    *pitch = asin(fmax(-1.0, fmin(1.0, r20)));
    double xb[3] = { 1, 0, 0 }, xw[3];
    attitude_rotate(a, xb, xw);
    double h = atan2(xw[0], xw[1]);       /* clockwise from north */
    *heading = h < 0 ? h + 2 * M_PI : h;
}
