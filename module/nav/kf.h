/* Position/velocity Kalman filter in the local ENU frame (origin = mission start).
 *
 * Three decoupled per-axis filters, state [p, v, b]: b is the accelerometer bias seen in
 * that world axis. Prediction uses the world-frame linear acceleration (attitude-rotated,
 * gravity removed) as a control input. Measurements:
 *   - GPS position and GPS velocity, when there is a fix;
 *   - zero velocity (ZUPT) while the payload is known to be at rest on the ground.
 * Without GPS the filter just dead-reckons, which drifts quadratically. That is accepted:
 * see docs/module/CLAUDE.md. The bias state is only observable with GPS or ZUPT.
 */
#ifndef NAV_KF_H
#define NAV_KF_H

typedef struct {
    double x[3];        /* p (m), v (m/s), b (m/s^2) */
    double P[3][3];
} kf_axis_t;

typedef struct {
    kf_axis_t ax[3];    /* E, N, U */
    double accel_sigma; /* m/s^2, process noise of the acceleration input */
    double bias_rw;     /* m/s^2/sqrt(s), bias random walk */
} kf_t;

/* Origin at zero, at rest, with the given initial bias uncertainty (m/s^2). */
void kf_reset(kf_t *k, double bias_sigma);
void kf_predict(kf_t *k, const double a_world[3], double dt);
/* Measurement on one axis: what = 0 position, 1 velocity. sigma is the 1-sigma noise.
 * Returns the normalized innovation squared (for gating/diagnostics). */
double kf_update(kf_t *k, int axis, int what, double z, double sigma);
/* Shift the position of every axis (used when the geodetic origin is re-anchored). */
void kf_shift(kf_t *k, const double d[3]);

#endif
