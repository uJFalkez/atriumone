#include "frames.h"

#include <math.h>

#define R2D (180.0 / M_PI)

void frames_quat_to_mat(const double q[4], double R[3][3])
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - w * z);     R[0][2] = 2 * (x * z + w * y);
    R[1][0] = 2 * (x * y + w * z);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - w * x);
    R[2][0] = 2 * (x * z - w * y);     R[2][1] = 2 * (y * z + w * x);     R[2][2] = 1 - 2 * (x * x + y * y);
}

void frames_mat_to_quat(const double R[3][3], double q[4])
{
    double tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) {
        double s = sqrt(tr + 1.0) * 2;
        q[0] = 0.25 * s;
        q[1] = (R[2][1] - R[1][2]) / s;
        q[2] = (R[0][2] - R[2][0]) / s;
        q[3] = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        double s = sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2;
        q[0] = (R[2][1] - R[1][2]) / s;
        q[1] = 0.25 * s;
        q[2] = (R[0][1] + R[1][0]) / s;
        q[3] = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        double s = sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2;
        q[0] = (R[0][2] - R[2][0]) / s;
        q[1] = (R[0][1] + R[1][0]) / s;
        q[2] = 0.25 * s;
        q[3] = (R[1][2] + R[2][1]) / s;
    } else {
        double s = sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2;
        q[0] = (R[1][0] - R[0][1]) / s;
        q[1] = (R[0][2] + R[2][0]) / s;
        q[2] = (R[1][2] + R[2][1]) / s;
        q[3] = 0.25 * s;
    }
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    double sgn = q[0] < 0 ? -1.0 : 1.0;
    for (int i = 0; i < 4; i++) q[i] = sgn * q[i] / n;
}

bool frames_is_rotation(const double m[3][3], double tol)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double d = 0;
            for (int k = 0; k < 3; k++) d += m[i][k] * m[j][k];
            if (fabs(d - (i == j)) > tol) return false;
        }
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                 m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return fabs(det - 1.0) < tol;
}

void frames_camera_ned(const double q_body_enu[4], const double M[3][3], cam_attitude_t *out)
{
    /* R_ned_cam = R_ned_enu * R_enu_body * M^T  (M^T: camera -> body) */
    static const double NED_ENU[3][3] = { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } };
    double Rb[3][3], A[3][3] = { { 0 } }, R[3][3] = { { 0 } };
    frames_quat_to_mat(q_body_enu, Rb);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) A[i][j] += NED_ENU[i][k] * Rb[k][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) R[i][j] += A[i][k] * M[j][k];

    frames_mat_to_quat(R, out->q);
    /* ZYX Euler of camera -> NED. */
    double s = -R[2][0];
    out->pitch_deg = asin(s > 1 ? 1 : (s < -1 ? -1 : s)) * R2D;
    out->roll_deg = atan2(R[2][1], R[2][2]) * R2D;
    double yaw = atan2(R[1][0], R[0][0]) * R2D;
    out->yaw_deg = yaw < 0 ? yaw + 360.0 : yaw;
}
