/* Output frames required by docs/CONTRATO.md §3.
 *
 * Internally the attitude filter works with body = MPU axes and world = ENU. The contract
 * wants the orientation of the RGB camera frame (x = image top, y = image right,
 * z = optical axis towards the scene) relative to NED, as a Hamilton quaternion
 * (camera -> NED) and ZYX Euler angles, yaw = bearing of the image top, clockwise from north.
 *
 * imu_to_camera is the fixed mounting rotation inside the module: v_cam = M * v_imu.
 */
#ifndef NAV_FRAMES_H
#define NAV_FRAMES_H

#include <stdbool.h>

typedef struct {
    double q[4];        /* w, x, y, z: camera -> NED */
    double roll_deg, pitch_deg, yaw_deg;   /* ZYX; yaw in [0, 360) */
} cam_attitude_t;

void frames_quat_to_mat(const double q[4], double R[3][3]);
void frames_mat_to_quat(const double R[3][3], double q[4]);   /* returns w >= 0 */
/* True if m is a proper rotation (orthonormal, det +1) within tol. */
bool frames_is_rotation(const double m[3][3], double tol);

/* q_body_enu: attitude filter quaternion (MPU body -> ENU). */
void frames_camera_ned(const double q_body_enu[4], const double imu_to_camera[3][3], cam_attitude_t *out);

/* ENU vector -> NED vector. */
static inline void frames_enu_to_ned(const double enu[3], double ned[3])
{
    double e = enu[0], n = enu[1], u = enu[2];
    ned[0] = n; ned[1] = e; ned[2] = -u;
}

#endif
