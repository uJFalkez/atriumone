#include "calsession.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define G             9.80665
#define D2R           (M_PI / 180.0)

/* still / poses: 0.5 s blocks; same stillness thresholds as the bench tools (~3x noise) */
#define BLOCK_S       0.5
#define STILL_ACC_STD 0.12
#define STILL_GYR_STD (0.5 * D2R)
#define STILL_NEED_S  5.0
#define STILL_TMO_S   30.0
#define STILL_G_TOL   1.5      /* |a| must be ~g (uncalibrated accel): resting, not accelerating */
#define GYRO_BIAS_MAX (10.0 * D2R)   /* MPU6500 spec is +-5 deg/s; more means a broken reading */
/* figure8 (as calib_mag) */
#define F8_MIN_STEP   0.5      /* uT between stored points */
#define F8_CONE_COS   0.82
#define F8_MIN_PER    0.005
#define F8_COV_DONE   13
#define F8_COV_MIN    12
#define F8_RESID_MAX  0.05
#define F8_MIN_PTS    300
#define F8_TMO_S      90.0
#define F8_B_MIN      15.0     /* uT: plausible raw |B| (Earth 20-65 + hard iron); wider = glitch */
#define F8_B_MAX      200.0
#define F8_JUMP_MAX   40.0     /* uT between consecutive samples: not a rotation, a glitch */
#define F8_OUTLIER    0.10     /* second fit pass drops points farther than 10 % from the radius */
/* poses (as calib_accel) */
#define POSE_HOLD_S   2.0
#define POSE_DOMINANT 8.5      /* m/s^2 on the up axis: tilt below ~30 deg */
#define POSE_TMO_S    120.0
#define POSE_MAX_ERR  0.3      /* m/s^2, |a| error per face after the fit */
#define POSE_MIN_PART 4        /* faces for a partial (offset-only) calibration */

static double mag_pts[CS_MAG_MAX][3];

static const double dirs[14][3] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    { .577, .577, .577 }, { .577, .577, -.577 }, { .577, -.577, .577 }, { .577, -.577, -.577 },
    { -.577, .577, .577 }, { -.577, .577, -.577 }, { -.577, -.577, .577 }, { -.577, -.577, -.577 },
};

static double n3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static int coverage(int n, const double c[3], const double (*W)[3])
{
    int cnt[14] = { 0 }, hit = 0;
    for (int i = 0; i < n; i++) {
        double d[3], u[3];
        for (int k = 0; k < 3; k++) d[k] = mag_pts[i][k] - c[k];
        for (int k = 0; k < 3; k++) u[k] = W ? W[k][0] * d[0] + W[k][1] * d[1] + W[k][2] * d[2] : d[k];
        double nn = n3(u);
        if (nn <= 0) continue;
        for (int j = 0; j < 14; j++)
            if ((u[0] * dirs[j][0] + u[1] * dirs[j][1] + u[2] * dirs[j][2]) / nn > F8_CONE_COS) cnt[j]++;
    }
    int need = (int)fmax(3, n * F8_MIN_PER);
    for (int j = 0; j < 14; j++) hit += cnt[j] >= need;
    return hit;
}

static int face_of(const double m[3])
{
    int ax = 0;
    for (int i = 1; i < 3; i++)
        if (fabs(m[i]) > fabs(m[ax])) ax = i;
    if (fabs(m[ax]) < POSE_DOMINANT) return -1;
    return ax * 2 + (m[ax] < 0);
}

static void block_reset(calsession_t *cs)
{
    cs->blk_n = 0;
    memset(cs->blk_g, 0, sizeof cs->blk_g); memset(cs->blk_g2, 0, sizeof cs->blk_g2);
    memset(cs->blk_a, 0, sizeof cs->blk_a); memset(cs->blk_a2, 0, sizeof cs->blk_a2);
}

static void make_summary(calsession_t *cs)
{
    const char *miss[3];
    int nm = 0;
    if (!cs->gyro_ok) miss[nm++] = "giroscópio";
    if (!cs->mag_ok) miss[nm++] = "magnetômetro";
    if (!cs->accel_ok) miss[nm++] = "acelerômetro";
    if (nm == 0) snprintf(cs->summary, sizeof cs->summary, "Calibração concluída");
    else if (nm == 3) snprintf(cs->summary, sizeof cs->summary,
                               "Calibração sem efeito: nenhum sensor atualizado (a anterior continua valendo)");
    else snprintf(cs->summary, sizeof cs->summary, "Calibração concluída; não atualizado: %s%s%s",
                  miss[0], nm > 1 ? ", " : "", nm > 1 ? miss[1] : "");
    if (cs->accel_ok && cs->accel_partial) {
        size_t n = strlen(cs->summary);
        snprintf(cs->summary + n, sizeof cs->summary - n, " (acelerômetro parcial: %d de 6 faces)", cs->nfaces);
    }
    snprintf(cs->instruction, sizeof cs->instruction, "%s", cs->summary);
    cs->progress = 1;
    cs->remaining_s = 0;
}

static void enter(calsession_t *cs, cs_step_t s, double t)
{
    cs->step = s;
    cs->step_t0 = t;
    cs->progress = 0;
    block_reset(cs);
    if (s == CS_FIGURE8) {
        cs->mag_n = 0;
        cs->mag_cov = 0;
        cs->next_cov_t = t + 1.0;
        for (int k = 0; k < 3; k++) { cs->mag_last[k] = 1e9; cs->mag_lo[k] = 1e9; cs->mag_hi[k] = -1e9; }
        cs->mag_have_prev = false;
    }
    if (s == CS_POSES) { cs->hold_face = -1; cs->hold_blocks = 0; }
    if (s == CS_DONE) make_summary(cs);
}

void cs_start(calsession_t *cs, double t, double rate_hz)
{
    memset(cs, 0, sizeof *cs);
    cs->rate_hz = rate_hz;
    cs->t_start = cs->t_last = t;
    enter(cs, CS_STILL, t);
    cs_tick(cs, t);
}

void cs_abort(calsession_t *cs) { cs->step = CS_OFF; }
bool cs_active(const calsession_t *cs) { return cs->step != CS_OFF && cs->step != CS_DONE; }
bool cs_wants_mag(const calsession_t *cs) { return cs->step == CS_FIGURE8; }

const char *cs_step_name(const calsession_t *cs)
{
    switch (cs->step) {
    case CS_STILL: return "still";
    case CS_FIGURE8: return "figure8";
    case CS_POSES: return "poses";
    case CS_DONE: return "done";
    default: return "off";
    }
}

/* ---------------------------------------------------------------- figure8 -> fit */
static void finish_mag(calsession_t *cs)
{
    cs->mag_fit_pts = cs->mag_n;
    if (cs->mag_n < 200) return;
    la_ellipsoid_t e;
    if (la_fit_ellipsoid((const double (*)[3])mag_pts, cs->mag_n, 0, 0, &e) < 0) return;
    /* Second pass: drop glitches that slipped through the sample gate, then refit. */
    int keep = 0;
    for (int i = 0; i < cs->mag_n; i++) {
        double d[3], u[3];
        for (int k = 0; k < 3; k++) d[k] = mag_pts[i][k] - e.offset[k];
        for (int k = 0; k < 3; k++) u[k] = e.W[k][0] * d[0] + e.W[k][1] * d[1] + e.W[k][2] * d[2];
        if (fabs(n3(u) - e.radius) <= F8_OUTLIER * e.radius) {
            if (keep != i) memcpy(mag_pts[keep], mag_pts[i], sizeof mag_pts[0]);
            keep++;
        }
    }
    cs->mag_outliers = cs->mag_n - keep;
    if (cs->mag_outliers > 0 && keep >= 200) {
        cs->mag_n = keep;
        if (la_fit_ellipsoid((const double (*)[3])mag_pts, cs->mag_n, 0, 0, &e) < 0) return;
    }
    cs->mag_fit_pts = cs->mag_n;
    int hit = coverage(cs->mag_n, e.offset, (const double (*)[3])e.W);
    cs->mag_coverage = hit / 14.0;
    cs->mag_resid_rel = e.residual / e.radius;
    if (hit >= F8_COV_MIN && cs->mag_resid_rel <= F8_RESID_MAX) {
        cs->mag_fit = e;
        cs->mag_ok = true;
    }
}

/* ---------------------------------------------------------------- poses -> fit */
/* Offset-only fit for 4-5 faces: minimize sum(|f - c| - g)^2 over c (Gauss-Newton), scale = 1.
 * The zero offset (up to ~0.6 m/s^2 out of the factory) matters far more than the scale
 * (a few %), and 4 faces are enough to solve for it. */
static int fit_offset_only(const double (*pts)[3], const bool *use, double c[3])
{
    c[0] = c[1] = c[2] = 0;
    for (int it = 0; it < 20; it++) {
        double JtJ[9] = { 0 }, Jtr[3] = { 0 };
        for (int f = 0; f < 6; f++) {
            if (!use[f]) continue;
            double d[3] = { pts[f][0] - c[0], pts[f][1] - c[1], pts[f][2] - c[2] }, n = n3(d);
            if (n <= 0) return -1;
            double r = n - G, J[3] = { -d[0] / n, -d[1] / n, -d[2] / n };
            for (int i = 0; i < 3; i++) {
                Jtr[i] += J[i] * r;
                for (int j = 0; j < 3; j++) JtJ[i * 3 + j] += J[i] * J[j];
            }
        }
        double step[3] = { -Jtr[0], -Jtr[1], -Jtr[2] };
        if (la_solve(3, JtJ, step) < 0) return -1;
        for (int i = 0; i < 3; i++) c[i] += step[i];
        if (n3(step) < 1e-7) break;
    }
    return 0;
}

static void finish_accel(calsession_t *cs)
{
    if (cs->nfaces < POSE_MIN_PART) return;
    la_ellipsoid_t e;
    memset(&e, 0, sizeof e);
    bool partial = cs->nfaces < 6;
    if (!partial) {
        if (la_fit_ellipsoid((const double (*)[3])cs->face, 6, 1, G, &e) < 0) return;
    } else {
        if (fit_offset_only((const double (*)[3])cs->face, cs->have, e.offset) < 0) return;
        for (int i = 0; i < 3; i++) e.W[i][i] = 1.0;
        e.radius = G;
    }
    double worst = 0;
    for (int f = 0; f < 6; f++) {
        if (!cs->have[f]) continue;
        double c[3];
        for (int i = 0; i < 3; i++) c[i] = (cs->face[f][i] - e.offset[i]) * e.W[i][i];
        worst = fmax(worst, fabs(n3(c) - G));
    }
    bool sane = true;
    for (int i = 0; i < 3; i++)
        sane &= fabs(e.offset[i]) < 2.0 && fabs(e.W[i][i] - 1) < 0.1;
    cs->accel_worst = worst;
    if (sane && worst < POSE_MAX_ERR) {
        cs->accel_fit = e;
        cs->accel_ok = true;
        cs->accel_partial = partial;
    }
}

static void capture_face(calsession_t *cs, const double mean[3])
{
    int f = face_of(mean);
    if (f >= 0 && !cs->have[f]) {
        memcpy(cs->face[f], mean, sizeof cs->face[f]);
        cs->have[f] = true;
        cs->nfaces++;
    }
}

/* ---------------------------------------------------------------- samples */
void cs_imu(calsession_t *cs, double t, const double g[3], const double a[3])
{
    if (cs->step != CS_STILL && cs->step != CS_POSES) return;
    cs->t_last = t;
    for (int k = 0; k < 3; k++) {
        cs->blk_g[k] += g[k]; cs->blk_g2[k] += g[k] * g[k];
        cs->blk_a[k] += a[k]; cs->blk_a2[k] += a[k] * a[k];
    }
    int need = (int)lround(BLOCK_S * cs->rate_hz);
    if (++cs->blk_n < need) return;

    int n = cs->blk_n;
    double gm[3], am[3], gs = 0, as = 0;
    for (int k = 0; k < 3; k++) {
        gm[k] = cs->blk_g[k] / n;
        am[k] = cs->blk_a[k] / n;
        gs = fmax(gs, sqrt(fmax(0, cs->blk_g2[k] / n - gm[k] * gm[k])));
        as = fmax(as, sqrt(fmax(0, cs->blk_a2[k] / n - am[k] * am[k])));
    }
    bool still = gs < STILL_GYR_STD && as < STILL_ACC_STD;
    if (cs->step == CS_STILL)            /* plausible rest: |a| ~ g and a sane gyro offset */
        still = still && fabs(n3(am) - G) < STILL_G_TOL && n3(gm) < GYRO_BIAS_MAX;
    double blk_g2[3];
    for (int k = 0; k < 3; k++) blk_g2[k] = cs->blk_g2[k] / n;
    block_reset(cs);

    if (cs->step == CS_STILL) {
        if (!still) {
            cs->still_blocks = 0;
            cs->still_n = 0;
            memset(cs->still_g, 0, sizeof cs->still_g);
            memset(cs->still_a, 0, sizeof cs->still_a);
            memset(cs->gyro_std, 0, sizeof cs->gyro_std);
            return;
        }
        cs->still_blocks++;
        for (int k = 0; k < 3; k++) {
            cs->still_g[k] += gm[k] * n;
            cs->still_a[k] += am[k] * n;
            cs->gyro_std[k] += blk_g2[k] * n;     /* second moment, normalized below */
        }
        cs->still_n += n;
        int need_blocks = (int)lround(STILL_NEED_S / BLOCK_S);
        cs->progress = fmin(1.0, (double)cs->still_blocks / need_blocks);
        if (cs->still_blocks >= need_blocks) {
            double a_mean[3];
            for (int k = 0; k < 3; k++) {
                cs->gyro_bias[k] = cs->still_g[k] / cs->still_n;
                cs->gyro_std[k] = sqrt(fmax(0, cs->gyro_std[k] / cs->still_n - cs->gyro_bias[k] * cs->gyro_bias[k]));
                a_mean[k] = cs->still_a[k] / cs->still_n;
            }
            cs->gyro_ok = true;
            capture_face(cs, a_mean);      /* the resting face counts for the accel step */
            enter(cs, CS_FIGURE8, t);
        }
        return;
    }

    /* poses: capture a face after POSE_HOLD_S still on it */
    int f = still ? face_of(am) : -1;
    if (f < 0 || f != cs->hold_face) {
        cs->hold_face = f;
        cs->hold_blocks = 0;
        memset(cs->hold_sum, 0, sizeof cs->hold_sum);
    }
    if (f >= 0) {
        for (int k = 0; k < 3; k++) cs->hold_sum[k] += am[k];
        cs->hold_blocks++;
        if (cs->hold_blocks >= (int)lround(POSE_HOLD_S / BLOCK_S) && !cs->have[f]) {
            double m[3];
            for (int k = 0; k < 3; k++) m[k] = cs->hold_sum[k] / cs->hold_blocks;
            capture_face(cs, m);
        }
    }
    cs->progress = cs->nfaces / 6.0;
    if (cs->nfaces == 6) {
        finish_accel(cs);
        enter(cs, CS_DONE, t);
    }
}

void cs_mag(calsession_t *cs, double t, const double m[3])
{
    if (cs->step != CS_FIGURE8) return;
    cs->t_last = t;
    /* Glitch gates: exact zero (corruption signature, docs/module/CLAUDE.md), implausible magnitude, or a
     * jump no hand rotation can produce between two samples (loose wire, bus corruption). */
    double b = n3(m);
    if (m[0] == 0.0 || m[1] == 0.0 || m[2] == 0.0 || b < F8_B_MIN || b > F8_B_MAX) {
        cs->mag_rejected_pts++;
        return;
    }
    double j = 0;
    for (int k = 0; k < 3; k++) j += (m[k] - cs->mag_prev[k]) * (m[k] - cs->mag_prev[k]);
    bool jump = cs->mag_have_prev && sqrt(j) > F8_JUMP_MAX;
    memcpy(cs->mag_prev, m, sizeof cs->mag_prev);
    cs->mag_have_prev = true;
    if (jump) {                          /* (the sample right after a glitch is lost too) */
        cs->mag_rejected_pts++;
        return;
    }
    double d = 0;
    for (int k = 0; k < 3; k++) d += (m[k] - cs->mag_last[k]) * (m[k] - cs->mag_last[k]);
    if (sqrt(d) < F8_MIN_STEP || cs->mag_n >= CS_MAG_MAX) return;
    for (int k = 0; k < 3; k++) {
        mag_pts[cs->mag_n][k] = cs->mag_last[k] = m[k];
        cs->mag_lo[k] = fmin(cs->mag_lo[k], m[k]);
        cs->mag_hi[k] = fmax(cs->mag_hi[k], m[k]);
    }
    cs->mag_n++;
}

/* ---------------------------------------------------------------- transitions / text */
void cs_tick(calsession_t *cs, double t)
{
    double el = t - cs->step_t0;
    switch (cs->step) {
    case CS_STILL:
        cs->remaining_s = fmax(0, STILL_TMO_S - el);
        snprintf(cs->instruction, sizeof cs->instruction, "Deixe o módulo parado, apoiado numa superfície");
        if (el >= STILL_TMO_S) enter(cs, CS_FIGURE8, t);       /* gyro not updated */
        break;
    case CS_FIGURE8:
        if (t >= cs->next_cov_t && cs->mag_n > 20) {
            double c[3];
            for (int k = 0; k < 3; k++) c[k] = (cs->mag_lo[k] + cs->mag_hi[k]) / 2;
            cs->mag_cov = coverage(cs->mag_n, c, NULL);
            cs->next_cov_t = t + 1.0;
        }
        cs->progress = cs->mag_cov / 14.0;
        cs->remaining_s = fmax(0, F8_TMO_S - el);
        snprintf(cs->instruction, sizeof cs->instruction,
                 "Gire o módulo fazendo um oito, passando por todas as orientações (%d de 14 direções)",
                 cs->mag_cov);
        if ((cs->mag_cov >= F8_COV_DONE && cs->mag_n >= F8_MIN_PTS) || el >= F8_TMO_S) {
            finish_mag(cs);
            enter(cs, CS_POSES, t);
        }
        break;
    case CS_POSES:
        cs->remaining_s = fmax(0, POSE_TMO_S - el);
        snprintf(cs->instruction, sizeof cs->instruction,
                 "Apoie o módulo em cada uma das 6 faces e espere 2 s em cada (faltam %d)", 6 - cs->nfaces);
        if (el >= POSE_TMO_S) {
            finish_accel(cs);                                   /* < 6 faces: accel not updated */
            enter(cs, CS_DONE, t);
        }
        break;
    case CS_DONE:
        break;                      /* text set once, on entry (see make_summary) */
    default:
        break;
    }
}

int cs_apply(const calsession_t *cs, calib_t *cal)
{
    int n = 0;
    if (cs->gyro_ok) {
        for (int k = 0; k < 3; k++) { cal->gyro_bias[k] = cs->gyro_bias[k]; cal->gyro_std[k] = cs->gyro_std[k]; }
        cal->gyro_valid = true;
        calib_timestamp(cal->gyro_time);
        n++;
    }
    if (cs->mag_ok) {
        memcpy(cal->mag_offset, cs->mag_fit.offset, sizeof cal->mag_offset);
        memcpy(cal->mag_matrix, cs->mag_fit.W, sizeof cal->mag_matrix);
        cal->mag_field = cs->mag_fit.radius;
        cal->mag_residual = cs->mag_fit.residual;
        cal->mag_coverage = cs->mag_coverage;
        cal->mag_valid = true;
        calib_timestamp(cal->mag_time);
        n++;
    }
    if (cs->accel_ok) {
        for (int k = 0; k < 3; k++) {
            cal->accel_offset[k] = cs->accel_fit.offset[k];
            cal->accel_scale[k] = cs->accel_fit.W[k][k];
        }
        cal->accel_residual = cs->accel_worst;
        cal->accel_valid = true;
        calib_timestamp(cal->accel_time);
        n++;
    }
    return n;
}
