/* Attitude estimator (Mahony complementary filter on a quaternion).
 *
 * Frames: body = MPU6500 axes. World = ENU (x East, y North, z Up).
 * q rotates body vectors into the world frame: v_world = R(q) v_body.
 *
 * Roll/pitch: gyro integration corrected towards the measured gravity, but only while
 * |accel| is close to g (linear acceleration in flight would bias it otherwise).
 * Yaw: gyro integration, corrected by the magnetometer whenever a mag burst is available
 * (the mag is read only on demand). Until the first mag correction, yaw is relative to
 * the start attitude and yaw_valid is false.
 */
#ifndef NAV_ATTITUDE_H
#define NAV_ATTITUDE_H

#include <stdbool.h>

typedef struct {
    double q[4];          /* w, x, y, z */
    double kp;            /* accel correction gain (rad/s per unit error) */
    double accel_gate;    /* use accel only if | |a|/g - 1 | < accel_gate */
    double mag_gain;      /* fraction of the mag yaw error removed per burst (0..1] */
    double declination;   /* rad, magnetic -> true north (east positive) */
    bool   yaw_valid;     /* a mag correction has been applied */
    unsigned accel_used, accel_skipped;
} attitude_t;

void attitude_init(attitude_t *a);
/* Levels the filter from one (averaged, at rest) accel vector; yaw = 0. */
void attitude_reset_from_accel(attitude_t *a, const double f_body[3]);
/* gyro rad/s (bias-corrected), specific force m/s^2 (calibrated), dt seconds. */
void attitude_update(attitude_t *a, const double gyro[3], const double f_body[3], double dt);
/* Mag field in body axes (calibrated and mapped to the MPU axes). The first call snaps the
 * yaw; later calls remove mag_gain of the error. Returns the yaw error in rad. */
double attitude_mag_correct(attitude_t *a, const double m_body[3]);

void attitude_rotate(const attitude_t *a, const double v_body[3], double v_world[3]);
/* Body assumed X forward, Y left, Z up (to be checked against the mounting).
 * Roll, pitch (rad) and heading (rad, clockwise from true north, of the body +X axis
 * projected on the horizontal plane, 0..2pi). */
void attitude_euler(const attitude_t *a, double *roll, double *pitch, double *heading);

#endif
