/* Guided multi-sensor calibration session (docs/CONTRATO.md §1 POST /calibrate, docs/PIPELINE.md §3.1).
 *
 * Steps, each with an operator instruction (Portuguese, shown by the dashboard):
 *   still    keep the module still ~5 s          -> gyro bias
 *   figure8  rotate it through every orientation -> mag hard/soft iron (ellipsoid fit)
 *            (the caller pauses the MPU and streams only the mag: cs_wants_mag())
 *   poses    rest it on each of the 6 faces, 2 s  -> accel offset/scale (axis-aligned fit)
 *   done
 * A step that times out or fits badly does not update that sensor; the previous calibration
 * stays. Pure logic fed with RAW (uncalibrated) samples, so it is testable offline.
 */
#ifndef NAV_CALSESSION_H
#define NAV_CALSESSION_H

#include "calib.h"
#include "linalg.h"

#include <stdbool.h>

typedef enum { CS_OFF = 0, CS_STILL, CS_FIGURE8, CS_POSES, CS_DONE } cs_step_t;

#define CS_MAG_MAX 4000

typedef struct {
    cs_step_t step;
    double rate_hz;            /* IMU sample rate */
    double t_start, step_t0, t_last;

    /* outputs for the status */
    char   instruction[160];
    double progress;           /* 0..1, of the current step */
    double remaining_s;        /* until the current step times out */
    char   summary[160];       /* final message (done) */

    /* still */
    int    blk_n;
    double blk_g[3], blk_g2[3], blk_a[3], blk_a2[3];
    int    still_blocks;
    double still_g[3], still_a[3];
    int    still_n;

    /* figure8 */
    int    mag_n;
    double mag_last[3], mag_lo[3], mag_hi[3];
    int    mag_cov;
    double next_cov_t;
    double mag_prev[3];        /* previous raw sample (accepted or not), for the jump gate */
    bool   mag_have_prev;

    /* poses */
    double face[6][3];
    bool   have[6];
    int    nfaces, hold_face, hold_blocks;
    double hold_sum[3];

    /* results */
    bool   gyro_ok, mag_ok, accel_ok;
    double gyro_bias[3], gyro_std[3];
    la_ellipsoid_t mag_fit, accel_fit;
    double mag_coverage, accel_worst;
    bool   accel_partial;      /* 4-5 faces: offsets only, scale kept at 1 */
    /* diagnostics of the last mag fit (also when rejected) */
    int    mag_fit_pts, mag_rejected_pts, mag_outliers;
    double mag_resid_rel;
} calsession_t;

void cs_start(calsession_t *cs, double t, double rate_hz);
void cs_abort(calsession_t *cs);
bool cs_active(const calsession_t *cs);           /* started and not done */
bool cs_wants_mag(const calsession_t *cs);        /* figure8: stream the mag, pause the MPU */
const char *cs_step_name(const calsession_t *cs); /* "still" | "figure8" | "poses" | "done" */

void cs_imu(calsession_t *cs, double t, const double gyro_raw[3], const double accel_raw[3]);
void cs_mag(calsession_t *cs, double t, const double field_raw[3]);
void cs_tick(calsession_t *cs, double t);         /* timeouts, transitions, status text */

/* Copies the successful results into cal (time stamps set). Returns how many sensors changed. */
int  cs_apply(const calsession_t *cs, calib_t *cal);

#endif
