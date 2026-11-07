/* Motion detectors used by the mission (docs/PIPELINE.md §3.4 and §3.5).
 *
 *   stable  : |angular rate| below stable_rate for the last stable_hold seconds (photo gate;
 *             navd adds a speed gate when the GPS has a fix).
 *   stopped : horizontal speed below stop_speed for stop_hold seconds (return, with GPS).
 *   landed  : bench-level stillness (no motor vibration, |a| ~ g) for landed_hold seconds
 *             (return without GPS; also enables ZUPT).
 * All thresholds are configuration constants.
 */
#ifndef NAV_MOTION_H
#define NAV_MOTION_H

#include <stdbool.h>

#define MOTION_MAX_WIN 64

typedef struct {
    /* configuration */
    double rate_hz;
    double stable_rate;       /* rad/s */
    double stable_hold;       /* s */
    double stop_speed;        /* m/s */
    double stop_hold;         /* s */
    double landed_hold;       /* s */
    double still_gyro_std;    /* rad/s */
    double still_gyro_mean;   /* rad/s */
    double still_accel_std;   /* m/s^2 */
    double still_g_tol;       /* m/s^2 */

    /* outputs */
    bool   stable, stopped, landed;
    double angular_speed;     /* rad/s, mean |w| over the stable_hold window */
    double stable_for, stopped_for, landed_for;   /* s, 0 when false */
    double gyro_std, gyro_mean, accel_std;        /* 1 s window, worst axis */

    /* internal */
    double t0;                /* reset time */
    double last_rot_t, last_move_t, still_since;
    double w_ring[MOTION_MAX_WIN];
    int    w_i, w_n;
    double win[MOTION_MAX_WIN][6];
    int    win_i, win_n;
} motion_t;

/* Defaults: stable = |w| < 1 deg/s for 0.5 s (operator's choice); stopped = 0.3 m/s for 3 s
 * (PIPELINE); landed after 3 s. */
void motion_init(motion_t *m, double rate_hz);
void motion_reset(motion_t *m, double t);
void motion_update_imu(motion_t *m, double t, const double gyro[3], const double accel[3]);
void motion_update_speed(motion_t *m, double t, double horizontal_speed);

#endif
