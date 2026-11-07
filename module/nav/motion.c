#include "motion.h"

#include <math.h>
#include <string.h>

#define G   9.80665
#define D2R (M_PI / 180.0)

void motion_init(motion_t *m, double rate_hz)
{
    memset(m, 0, sizeof *m);
    m->rate_hz = rate_hz;
    /* Photo gate, operator's choice (2026-11-06): 1 deg/s (PIPELINE suggested 0.15 rad/s =
     * 8.6 deg/s). Bench gyro noise is ~0.15 deg/s; a hovering drone may vibrate more. */
    m->stable_rate = 1.0 * D2R;
    m->stable_hold = 0.5;
    m->stop_speed = 0.3;
    m->stop_hold = 3.0;
    m->landed_hold = 3.0;
    /* ~3-4x the bench noise (gyro 0.15 °/s, accel 0.04 m/s²): motors running exceed this. */
    m->still_gyro_std = 0.6 * D2R;
    m->still_gyro_mean = 1.5 * D2R;
    m->still_accel_std = 0.12;
    m->still_g_tol = 0.4;
    motion_reset(m, 0);
}

void motion_reset(motion_t *m, double t)
{
    m->t0 = t;
    m->last_rot_t = m->last_move_t = t;
    m->still_since = -1;
    m->w_i = m->w_n = m->win_i = m->win_n = 0;
    m->stable = m->stopped = m->landed = false;
    m->stable_for = m->stopped_for = m->landed_for = 0;
    m->angular_speed = 0;
}

static int window_len(const motion_t *m, double seconds)
{
    int n = (int)lround(seconds * m->rate_hz);
    return n < 1 ? 1 : (n > MOTION_MAX_WIN ? MOTION_MAX_WIN : n);
}

void motion_update_imu(motion_t *m, double t, const double g[3], const double a[3])
{
    /* stable: every sample in the last stable_hold below stable_rate */
    double w = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (w >= m->stable_rate) m->last_rot_t = t;
    int nw = window_len(m, m->stable_hold);
    m->w_ring[m->w_i] = w;
    m->w_i = (m->w_i + 1) % nw;
    if (m->w_n < nw) m->w_n++;
    double sum = 0;
    for (int k = 0; k < m->w_n; k++) sum += m->w_ring[k];
    m->angular_speed = sum / m->w_n;
    double calm = t - m->last_rot_t;
    m->stable = calm >= m->stable_hold;
    m->stable_for = m->stable ? calm : 0;

    /* landed: 1 s window of bench-level stillness, held for landed_hold */
    int nl = window_len(m, 1.0);
    for (int i = 0; i < 3; i++) { m->win[m->win_i][i] = g[i]; m->win[m->win_i][3 + i] = a[i]; }
    m->win_i = (m->win_i + 1) % nl;
    if (m->win_n < nl) m->win_n++;
    double mean[6] = { 0 }, var[6] = { 0 };
    for (int k = 0; k < m->win_n; k++)
        for (int i = 0; i < 6; i++) mean[i] += m->win[k][i];
    for (int i = 0; i < 6; i++) mean[i] /= m->win_n;
    for (int k = 0; k < m->win_n; k++)
        for (int i = 0; i < 6; i++) var[i] += (m->win[k][i] - mean[i]) * (m->win[k][i] - mean[i]);
    double gs = 0, as = 0, gm = 0;
    for (int i = 0; i < 3; i++) {
        gs = fmax(gs, sqrt(var[i] / m->win_n));
        as = fmax(as, sqrt(var[3 + i] / m->win_n));
        gm = fmax(gm, fabs(mean[i]));
    }
    m->gyro_std = gs; m->gyro_mean = gm; m->accel_std = as;
    double an = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    bool still = m->win_n == nl && gs < m->still_gyro_std && gm < m->still_gyro_mean &&
                 as < m->still_accel_std && fabs(an - G) < m->still_g_tol;
    if (!still) m->still_since = -1;
    else if (m->still_since < 0) m->still_since = t;
    double sf = m->still_since < 0 ? 0 : t - m->still_since;
    m->landed = m->still_since >= 0 && sf >= m->landed_hold;
    m->landed_for = m->landed ? sf : 0;
}

void motion_update_speed(motion_t *m, double t, double v)
{
    if (v >= m->stop_speed) m->last_move_t = t;
    double calm = t - m->last_move_t;
    m->stopped = calm >= m->stop_hold;
    m->stopped_for = m->stopped ? calm : 0;
}
