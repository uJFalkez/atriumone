/* Offline tests of the navigation library: NMEA parser, attitude filter, Kalman filter. */
#include "attitude.h"
#include "calsession.h"
#include "frames.h"
#include "motion.h"
#include "geo.h"
#include "gps.h"
#include "kf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { int ok_ = (cond) ? 1 : 0; \
        printf("  [%s] ", ok_ ? " ok " : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
        if (!ok_) failures++; } while (0)

#define D2R (M_PI / 180.0)
#define G   9.80665

static double randn(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static void test_gps(void)
{
    printf("gps: NMEA parser\n");
    gps_t g;
    memset(&g, 0, sizeof g);
    g.fd = -1;

    CHECK(gps_parse_sentence(&g, "$GPGGA,,,,,,0,00,99.99,,,,,,*48\r\n", 1), "no-fix GGA from our NEO-6M parses");
    CHECK(g.fix.quality == 0 && g.fix.sats == 0, "no-fix GGA: quality 0, 0 sats");
    CHECK(gps_parse_sentence(&g, "$GPRMC,,V,,,,,,,,,,N*53", 2) && !g.fix.rmc_valid, "no-fix RMC: status V");

    CHECK(gps_parse_sentence(&g, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", 3),
          "fix GGA parses");
    CHECK(fabs(g.fix.lat - 48.1173) < 1e-6 && fabs(g.fix.lon - 11.516667) < 1e-6,
          "lat %.6f lon %.6f", g.fix.lat, g.fix.lon);
    CHECK(g.fix.quality == 1 && g.fix.sats == 8 && fabs(g.fix.hdop - 0.9) < 1e-9 && fabs(g.fix.alt - 545.4) < 1e-9,
          "quality %d sats %d hdop %.1f alt %.1f", g.fix.quality, g.fix.sats, g.fix.hdop, g.fix.alt);

    CHECK(gps_parse_sentence(&g, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A", 4),
          "fix RMC parses");
    CHECK(g.fix.rmc_valid && fabs(g.fix.speed - 22.4 * 0.514444) < 1e-6 && fabs(g.fix.course - 84.4) < 1e-9,
          "speed %.3f m/s course %.1f", g.fix.speed, g.fix.course);
    CHECK(g.fix.year == 2094 && g.fix.month == 3 && g.fix.day == 23 && g.fix.hour == 12 && g.fix.min == 35,
          "date/time %04d-%02d-%02d %02d:%02d:%04.1f (2-digit year read as 20yy)", g.fix.year, g.fix.month, g.fix.day, g.fix.hour,
          g.fix.min, g.fix.sec);

    unsigned bad = g.fix.bad_checksum;
    CHECK(!gps_parse_sentence(&g, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48", 5)
          && g.fix.bad_checksum == bad + 1, "bad checksum rejected");
    CHECK(!gps_parse_sentence(&g, "$GPGGA,123519,4807.0", 6), "truncated sentence (no *) rejected");

    gps_t s;
    memset(&s, 0, sizeof s);
    CHECK(gps_parse_sentence(&s, "$GPGGA,000000,2330.000,S,04637.800,W,1,05,1.2,760.0,M,0,M,,*68", 7), "southern/western hemisphere GGA parses");
    CHECK(fabs(s.fix.lat + 23.5) < 1e-9 && fabs(s.fix.lon + 46.63) < 1e-9, "S/W are negative: %.4f %.4f",
          s.fix.lat, s.fix.lon);
    CHECK(gps_parse_sentence(&s, "$GPRMC,123519,A,2330.000,S,04637.800,W,000.0,000.0,061126,,,A*7C", 8) && s.fix.year == 2026 && s.fix.month == 11,
          "RMC date 061126 -> %04d-%02d-%02d", s.fix.year, s.fix.month, s.fix.day);
}

static void test_geo(void)
{
    printf("geo: ENU round trip\n");
    geo_origin_t o = { true, -23.55, -46.63, 760 };
    double e[3] = { 1234.5, -987.6, 42.0 }, lat, lon, alt, back[3];
    geo_from_enu(&o, e, &lat, &lon, &alt);
    geo_to_enu(&o, lat, lon, alt, back);
    CHECK(fabs(back[0] - e[0]) < 1e-6 && fabs(back[1] - e[1]) < 1e-6 && fabs(back[2] - e[2]) < 1e-9,
          "round trip error < 1 um");
}

/* Body specific force at rest for a given attitude: R^T (0,0,g). */
static void rest_force(const attitude_t *truth, double f[3])
{
    const double *q = truth->q;
    double w = q[0], x = q[1], y = q[2], z = q[3];
    f[0] = 2 * (x * z - w * y) * G;
    f[1] = 2 * (y * z + w * x) * G;
    f[2] = (1 - 2 * (x * x + y * y)) * G;
}

static void quat_from_euler(double roll, double pitch, double yaw_ccw, double q[4])
{
    /* R = Rz(yaw) Ry(-pitch) Rx(roll): pitch > 0 = nose up with Y-left body. */
    double cr = cos(roll / 2), sr = sin(roll / 2), cp = cos(-pitch / 2), sp = sin(-pitch / 2);
    double cy = cos(yaw_ccw / 2), sy = sin(yaw_ccw / 2);
    q[0] = cy * cp * cr + sy * sp * sr;
    q[1] = cy * cp * sr - sy * sp * cr;
    q[2] = cy * sp * cr + sy * cp * sr;
    q[3] = sy * cp * cr - cy * sp * sr;
}

static void test_attitude(void)
{
    printf("attitude: Mahony filter\n");
    attitude_t a, truth;
    double r, p, h, f[3], zero[3] = { 0, 0, 0 };

    attitude_init(&truth);
    quat_from_euler(10 * D2R, -5 * D2R, 0, truth.q);
    attitude_euler(&truth, &r, &p, &h);
    CHECK(fabs(r - 10 * D2R) < 1e-9 && fabs(p + 5 * D2R) < 1e-9, "euler convention: roll %.2f pitch %.2f deg",
          r / D2R, p / D2R);

    rest_force(&truth, f);
    attitude_init(&a);
    attitude_reset_from_accel(&a, f);
    attitude_euler(&a, &r, &p, &h);
    CHECK(fabs(r - 10 * D2R) < 1e-6 && fabs(p + 5 * D2R) < 1e-6, "level from accel: roll %.3f pitch %.3f deg",
          r / D2R, p / D2R);

    /* Converge from a wrong start (level) towards a 20° roll, with noise. */
    attitude_init(&a);
    quat_from_euler(20 * D2R, 0, 0, truth.q);
    for (int i = 0; i < 50 * 10; i++) {
        double fn[3], gn[3];
        rest_force(&truth, f);
        for (int k = 0; k < 3; k++) { fn[k] = f[k] + 0.03 * randn(); gn[k] = 0.002 * randn(); }
        attitude_update(&a, gn, fn, 0.02);
    }
    attitude_euler(&a, &r, &p, &h);
    CHECK(fabs(r - 20 * D2R) < 0.5 * D2R && fabs(p) < 0.5 * D2R, "converged in 10 s: roll %.2f pitch %.2f deg",
          r / D2R, p / D2R);

    /* Yaw by gyro: 10 °/s about +Z (CCW seen from above) for 9 s = heading -90° (i.e. 270°). */
    attitude_init(&a);
    double gz[3] = { 0, 0, 10 * D2R }, up[3] = { 0, 0, G };
    double h0;
    attitude_euler(&a, &r, &p, &h0);
    for (int i = 0; i < 450; i++) attitude_update(&a, gz, up, 0.02);
    attitude_euler(&a, &r, &p, &h);
    double dh = fmod(h - h0 + 4 * M_PI, 2 * M_PI) / D2R;
    CHECK(fabs(dh - 270) < 0.1, "gyro yaw: heading changed by %.3f deg (expect 270 = -90)", dh);

    /* Mag: field in Brazil-like geometry (horizontal 18 uT north, 12 uT up). Truth heading
     * 60° clockwise from north (yaw_ccw from east = 90-60 = 30°), tilted 8°/-4°. */
    double m_world[3] = { 0, 18, 12 }, m_body[3];
    attitude_init(&truth);
    quat_from_euler(8 * D2R, -4 * D2R, 30 * D2R, truth.q);
    /* m_body = R^T m_world */
    {
        attitude_t inv = truth;
        inv.q[1] = -inv.q[1]; inv.q[2] = -inv.q[2]; inv.q[3] = -inv.q[3];
        attitude_rotate(&inv, m_world, m_body);
    }
    rest_force(&truth, f);
    attitude_init(&a);
    attitude_reset_from_accel(&a, f);           /* right tilt, wrong yaw */
    double err = attitude_mag_correct(&a, m_body);
    attitude_euler(&a, &r, &p, &h);
    CHECK(a.yaw_valid && fabs(h / D2R - 60) < 0.01, "mag snap: heading %.3f deg (expect 60), error was %.1f",
          h / D2R, err / D2R);
    CHECK(fabs(r - 8 * D2R) < 1e-6 && fabs(p + 4 * D2R) < 1e-6, "mag correction keeps roll/pitch");

    /* Declination: true heading = magnetic heading + declination (east positive). */
    attitude_init(&a);
    a.declination = -21 * D2R;
    attitude_reset_from_accel(&a, f);
    attitude_mag_correct(&a, m_body);
    attitude_euler(&a, &r, &p, &h);
    CHECK(fabs(h / D2R - 39) < 0.01, "declination -21: true heading %.3f (expect 39)", h / D2R);
    (void)zero;
}

static void test_kf(void)
{
    printf("kf: position/velocity filter\n");
    kf_t k = { .accel_sigma = 0.3, .bias_rw = 0.002 };
    double a[3], dt = 0.02;

    /* IMU only with a 0.05 m/s^2 bias: pure dead reckoning drifts ~0.5 b t^2. */
    kf_reset(&k, 0.1);
    for (int i = 0; i < 50 * 120; i++) {
        a[0] = 0.05; a[1] = 0; a[2] = 0;
        kf_predict(&k, a, dt);
    }
    CHECK(fabs(k.ax[0].x[0] - 360) < 1, "IMU only, bias 0.05, 120 s: drift %.0f m (expect ~360, accepted)",
          k.ax[0].x[0]);

    /* ZUPT on the ground for 20 s estimates the bias. */
    kf_reset(&k, 0.1);
    for (int i = 0; i < 50 * 20; i++) {
        a[0] = 0.05 + 0.03 * randn(); a[1] = -0.04 + 0.03 * randn(); a[2] = 0.03 * randn();
        kf_predict(&k, a, dt);
        for (int ax = 0; ax < 3; ax++) kf_update(&k, ax, 1, 0.0, 0.02);
    }
    CHECK(fabs(k.ax[0].x[2] - 0.05) < 0.01 && fabs(k.ax[1].x[2] + 0.04) < 0.01,
          "ZUPT bias estimate: %.4f %.4f (truth 0.05 -0.04)", k.ax[0].x[2], k.ax[1].x[2]);

    /* Flight with GPS: true motion a = 0.5 sin(0.2 t) on E; accel bias 0.05; GPS 1 Hz, 3 m. */
    kf_reset(&k, 0.1);
    double p = 0, v = 0, maxerr = 0, sumsq = 0;
    int n = 0;
    for (int i = 0; i < 50 * 300; i++) {
        double t = i * dt, at = 0.5 * sin(0.2 * t);
        p += v * dt + 0.5 * at * dt * dt;
        v += at * dt;
        a[0] = at + 0.05 + 0.05 * randn(); a[1] = a[2] = 0;
        kf_predict(&k, a, dt);
        if (i % 50 == 0) {
            kf_update(&k, 0, 0, p + 3 * randn(), 3);
            kf_update(&k, 0, 1, v + 0.2 * randn(), 0.3);
        }
        if (t > 60) {
            double e = fabs(k.ax[0].x[0] - p);
            maxerr = fmax(maxerr, e); sumsq += e * e; n++;
        }
    }
    CHECK(sqrt(sumsq / n) < 2.0, "with GPS: rms position error %.2f m, max %.2f m (GPS noise 3 m)",
          sqrt(sumsq / n), maxerr);
    CHECK(fabs(k.ax[0].x[2] - 0.05) < 0.02, "with GPS: bias estimate %.4f (truth 0.05)", k.ax[0].x[2]);

    /* GPS lost for 10 s after convergence: error grows, but slowly (bias already removed). */
    double p0err = k.ax[0].x[0] - p;
    for (int i = 0; i < 50 * 10; i++) {
        double at = 0.0;
        p += v * dt; v += at * dt;
        a[0] = at + 0.05 + 0.05 * randn();
        kf_predict(&k, a, dt);
    }
    double gap_err = fabs(k.ax[0].x[0] - p);
    CHECK(gap_err < 15, "10 s GPS outage: error %.2f m (was %.2f)", gap_err, fabs(p0err));
}


static void test_frames(void)
{
    printf("frames: camera -> NED output\n");
    /* Default mounting guess: MPU flat (z up), camera looking down, image top = MPU +X. */
    const double M[3][3] = { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } };
    cam_attitude_t c;
    double q[4] = { 1, 0, 0, 0 };   /* MPU x = East, y = North, z = Up */
    frames_camera_ned(q, M, &c);
    CHECK(fabs(c.roll_deg) < 1e-9 && fabs(c.pitch_deg) < 1e-9 && fabs(c.yaw_deg - 90) < 1e-9,
          "level, image top east: roll %.3f pitch %.3f yaw %.3f (expect 0 0 90)", c.roll_deg, c.pitch_deg, c.yaw_deg);

    double qn[4] = { cos(M_PI / 4), 0, 0, sin(M_PI / 4) };   /* +90° about Up: MPU x -> North */
    frames_camera_ned(qn, M, &c);
    CHECK(fabs(c.yaw_deg) < 1e-9 || fabs(c.yaw_deg - 360) < 1e-9, "image top north: yaw %.3f (expect 0)", c.yaw_deg);

    double qw[4] = { cos(-M_PI / 4), 0, 0, sin(-M_PI / 4) };  /* MPU x -> South */
    frames_camera_ned(qw, M, &c);
    CHECK(fabs(c.yaw_deg - 180) < 1e-9, "image top south: yaw %.3f (expect 180)", c.yaw_deg);

    double a = -10 * D2R;                                       /* MPU x (image top) tilted up */
    double qp[4] = { cos(a / 2), 0, sin(a / 2), 0 };
    frames_camera_ned(qp, M, &c);
    CHECK(fabs(c.pitch_deg - 10) < 1e-9 && fabs(c.roll_deg) < 1e-9 && fabs(c.yaw_deg - 90) < 1e-9,
          "image top up 10 deg: roll %.3f pitch %.3f yaw %.3f (expect 0 10 90)", c.roll_deg, c.pitch_deg,
          c.yaw_deg);

    /* Quaternion output must rotate camera vectors exactly like the matrix chain. */
    double qr[4] = { 0.8, 0.2, -0.3, 0.4 }, n = 0;
    for (int i = 0; i < 4; i++) n += qr[i] * qr[i];
    for (int i = 0; i < 4; i++) qr[i] /= sqrt(n);
    frames_camera_ned(qr, M, &c);
    double Rq[3][3], Rb[3][3], err = 0;
    frames_quat_to_mat(c.q, Rq);
    frames_quat_to_mat(qr, Rb);
    for (int j = 0; j < 3; j++) {          /* camera axis j -> body (M^T col j = row j of M) -> ENU -> NED */
        double vb[3] = { M[j][0], M[j][1], M[j][2] }, ve[3] = { 0 }, vn[3];
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) ve[i] += Rb[i][k] * vb[k];
        frames_enu_to_ned(ve, vn);
        for (int i = 0; i < 3; i++) err = fmax(err, fabs(vn[i] - Rq[i][j]));
    }
    CHECK(err < 1e-12 && c.q[0] >= 0, "quaternion matches matrix chain (err %.1e), w >= 0", err);

    const double I[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    const double refl[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 } };
    const double scaled[3][3] = { { 1.1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    CHECK(frames_is_rotation(I, 1e-6) && frames_is_rotation(M, 1e-6) && !frames_is_rotation(refl, 1e-6) &&
          !frames_is_rotation(scaled, 1e-6), "mounting matrix validation (reflection and scale rejected)");
}

static void test_motion(void)
{
    printf("motion: stable / stopped / landed\n");
    motion_t m;
    motion_init(&m, 50);
    motion_reset(&m, 0);
    double g[3] = { 0.01, 0, 0 }, a[3] = { 0, 0, G }, t = 0;
    for (int i = 1; i <= 20; i++) motion_update_imu(&m, t = i * 0.02, g, a);
    CHECK(!m.stable, "not stable before 0.5 s (t=%.2f)", t);
    for (int i = 21; i <= 30; i++) motion_update_imu(&m, t = i * 0.02, g, a);
    CHECK(m.stable && fabs(m.angular_speed - 0.01) < 1e-9, "stable after 0.5 s below 1 deg/s (w %.3f)",
          m.angular_speed);
    double spike[3] = { 0, 0.03, 0 };
    motion_update_imu(&m, t += 0.02, spike, a);
    CHECK(!m.stable, "one sample at 1.7 deg/s breaks stability");
    for (int i = 0; i < 24; i++) motion_update_imu(&m, t += 0.02, g, a);
    CHECK(!m.stable, "still not stable 0.48 s after the spike");
    motion_update_imu(&m, t += 0.02, g, a);
    CHECK(m.stable, "stable again 0.5 s after the spike");

    motion_reset(&m, 0);
    motion_update_speed(&m, 0.0, 0.5);
    motion_update_speed(&m, 2.9, 0.1);
    CHECK(!m.stopped, "not stopped 2.9 s after moving");
    motion_update_speed(&m, 3.1, 0.1);
    CHECK(m.stopped, "stopped after 3 s below 0.3 m/s");

    motion_reset(&m, 0);
    double z[3] = { 0, 0, 0 };
    srand(7);
    for (t = 0.02; t < 4.5; t += 0.02) {
        double an[3] = { 0.03 * randn(), 0.03 * randn(), G + 0.03 * randn() };
        motion_update_imu(&m, t, z, an);
    }
    CHECK(m.landed, "landed after ~4 s of bench-level stillness (for %.1f s)", m.landed_for);
    for (int i = 0; i < 10; i++) {
        double vib[3] = { 0.6 * randn(), 0.6 * randn(), G + 0.6 * randn() };
        motion_update_imu(&m, t += 0.02, z, vib);
    }
    CHECK(!m.landed, "motor-like vibration (0.6 m/s2) clears landed");
}

static void test_calsession(void)
{
    printf("calsession: guided calibration (still -> figure8 -> poses)\n");
    static calsession_t cs;
    const double rate = 50, dt = 1 / rate;
    const double gbias[3] = { 0.02, -0.03, 0.01 };
    const double aoff[3] = { 0.10, -0.20, 0.15 }, ascale[3] = { 1.02, 0.98, 1.01 };
    const double moff[3] = { 12.0, -30.0, 7.0 };
    const double A[3][3] = { { 1.10, 0.05, 0.00 }, { 0.05, 0.92, 0.03 }, { 0.00, 0.03, 1.00 } };
    double t = 0;

    /* raw accel for a true specific force f: raw = f / scale + offset */
    #define RAW_ACC(f, out) for (int k = 0; k < 3; k++) (out)[k] = (f)[k] / ascale[k] + aoff[k] + 0.02 * randn()

    cs_start(&cs, t, rate);
    CHECK(!strcmp(cs_step_name(&cs), "still") && cs.instruction[0], "starts in 'still': \"%s\"", cs.instruction);

    /* 2 s of handling, then 6 s still on the +Z face */
    for (int i = 0; i < 100; i++) {
        double g[3] = { 0.3 * randn(), 0.3 * randn(), 0.3 * randn() }, f[3] = { 2 * randn(), 2 * randn(), G }, a[3];
        RAW_ACC(f, a);
        cs_imu(&cs, t += dt, g, a);
        cs_tick(&cs, t);
    }
    CHECK(cs.step == CS_STILL && cs.progress == 0, "handling does not count as still");
    for (int i = 0; i < 300 && cs.step == CS_STILL; i++) {
        double g[3], f[3] = { 0, 0, G }, a[3];
        for (int k = 0; k < 3; k++) g[k] = gbias[k] + 0.002 * randn();
        RAW_ACC(f, a);
        cs_imu(&cs, t += dt, g, a);
        cs_tick(&cs, t);
    }
    CHECK(cs.gyro_ok && fabs(cs.gyro_bias[0] - 0.02) < 1e-3 && fabs(cs.gyro_bias[1] + 0.03) < 1e-3,
          "gyro bias %.4f %.4f %.4f (truth 0.02 -0.03 0.01)", cs.gyro_bias[0], cs.gyro_bias[1], cs.gyro_bias[2]);
    CHECK(cs.step == CS_FIGURE8 && cs_wants_mag(&cs) && cs.nfaces == 1, "then figure8 (MPU paused), +Z face kept");

    /* figure 8: a continuous tumbling rotation (like a hand), 45 uT field, soft + hard iron */
    const double Fw[3] = { 20, 0, -40 };
    double qt[4] = { 1, 0, 0, 0 };
    for (int i = 0; i < 6000 && cs.step == CS_FIGURE8; i++) {
        double R[3][3], b[3] = { 0 }, raw[3], tt = i * 0.01;
        double w[3] = { 2.0 * sin(0.3 * tt), 2.0 * cos(0.5 * tt), 1.5 * sin(0.7 * tt + 1) };   /* rad/s */
        double h = 0.005, dq[4] = { qt[0] - h * (w[0] * qt[1] + w[1] * qt[2] + w[2] * qt[3]),
                                    qt[1] + h * (w[0] * qt[0] + w[2] * qt[2] - w[1] * qt[3]),
                                    qt[2] + h * (w[1] * qt[0] - w[2] * qt[1] + w[0] * qt[3]),
                                    qt[3] + h * (w[2] * qt[0] + w[1] * qt[1] - w[0] * qt[2]) }, qn = 0;
        for (int k = 0; k < 4; k++) qn += dq[k] * dq[k];
        for (int k = 0; k < 4; k++) qt[k] = dq[k] / sqrt(qn);
        frames_quat_to_mat(qt, R);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) b[r] += R[c][r] * Fw[c];           /* body = R^T world */
        for (int r = 0; r < 3; r++)
            raw[r] = A[r][0] * b[0] + A[r][1] * b[1] + A[r][2] * b[2] + moff[r] + 0.2 * randn();
        if (i % 50 == 25) {                       /* glitches: 450 uT spike, X stuck at 0, jump */
            double bad[3][3] = { { 450, -80, 300 }, { 0, -15, 60 }, { raw[0] + 60, raw[1], raw[2] } };
            cs_mag(&cs, t += 0.01, bad[(i / 50) % 3]);
        }
        cs_mag(&cs, t += 0.01, raw);
        cs_tick(&cs, t);
    }
    CHECK(cs.mag_ok && cs.mag_rejected_pts > 0, "mag fit accepted despite %d glitches (coverage %.0f/14, "
          "residual %.1f%%)", cs.mag_rejected_pts, cs.mag_coverage * 14, cs.mag_resid_rel * 100);
    CHECK(fabs(cs.mag_fit.offset[0] - 12) < 0.5 && fabs(cs.mag_fit.offset[1] + 30) < 0.5 &&
          fabs(cs.mag_fit.offset[2] - 7) < 0.5, "hard iron %.2f %.2f %.2f (truth 12 -30 7)",
          cs.mag_fit.offset[0], cs.mag_fit.offset[1], cs.mag_fit.offset[2]);
    CHECK(cs.step == CS_POSES && !cs_wants_mag(&cs), "then poses (MPU back)");

    /* poses: the 5 missing faces, 3 s each, with handling in between */
    const int order[5] = { 5, 0, 1, 2, 3 };    /* -Z +X -X +Y -Y */
    for (int j = 0; j < 5 && cs.step == CS_POSES; j++) {
        int fc = order[j];
        double f[3] = { 0, 0, 0 }, a[3], z[3] = { 0, 0, 0 };
        f[fc / 2] = (fc % 2) ? -G : G;
        for (int i = 0; i < 25; i++) {                     /* 0.5 s moving */
            double fm[3] = { 3 * randn(), 3 * randn(), 3 * randn() }, gm[3] = { randn(), randn(), randn() };
            RAW_ACC(fm, a);
            cs_imu(&cs, t += dt, gm, a);
            cs_tick(&cs, t);
        }
        for (int i = 0; i < 150; i++) {                    /* 3 s on the face */
            RAW_ACC(f, a);
            cs_imu(&cs, t += dt, z, a);
            cs_tick(&cs, t);
        }
    }
    CHECK(cs.accel_ok && cs.step == CS_DONE, "accel fit accepted, done (worst |a| err %.3f)", cs.accel_worst);
    CHECK(fabs(cs.accel_fit.offset[0] - 0.10) < 0.02 && fabs(cs.accel_fit.offset[1] + 0.20) < 0.02 &&
          fabs(cs.accel_fit.W[1][1] - 0.98) < 0.005, "accel offset %.3f %.3f %.3f scale_y %.4f",
          cs.accel_fit.offset[0], cs.accel_fit.offset[1], cs.accel_fit.offset[2], cs.accel_fit.W[1][1]);
    CHECK(!strcmp(cs.summary, "Calibração concluída"), "summary: \"%s\"", cs.summary);
    calib_t cal;
    calib_defaults(&cal);
    CHECK(cs_apply(&cs, &cal) == 3 && cal.gyro_valid && cal.mag_valid && cal.accel_valid,
          "cs_apply updates gyro, mag and accel");

    /* Partial accel: only 4 faces before the timeout -> offsets only, scale 1. */
    {
        const double off[3] = { 0.30, -0.25, 0.40 };
        cs_start(&cs, t = 0, rate);
        cs.step = CS_POSES; cs.step_t0 = 0; cs.hold_face = -1;
        const int faces4[4] = { 4, 5, 0, 2 };           /* +Z -Z +X +Y */
        for (int j = 0; j < 4; j++) {
            double f[3] = { 0, 0, 0 }, z[3] = { 0, 0, 0 }, a[3];
            f[faces4[j] / 2] = (faces4[j] % 2) ? -G : G;
            for (int i = 0; i < 25; i++) {
                double mv[3] = { 2 * randn(), 2 * randn(), 2 * randn() };
                cs_imu(&cs, t += dt, mv, mv);
            }
            for (int i = 0; i < 150; i++) {
                for (int k = 0; k < 3; k++) a[k] = f[k] + off[k] + 0.02 * randn();
                cs_imu(&cs, t += dt, z, a);
                cs_tick(&cs, t);
            }
        }
        for (; cs.step != CS_DONE; t += dt) cs_tick(&cs, t);
        CHECK(cs.accel_ok && cs.accel_partial && fabs(cs.accel_fit.offset[0] - 0.30) < 0.02 &&
              fabs(cs.accel_fit.offset[1] + 0.25) < 0.02 && fabs(cs.accel_fit.offset[2] - 0.40) < 0.02,
              "4 faces -> partial accel: offset %.3f %.3f %.3f (truth 0.30 -0.25 0.40)",
              cs.accel_fit.offset[0], cs.accel_fit.offset[1], cs.accel_fit.offset[2]);
        CHECK(strstr(cs.summary, "parcial") != NULL, "summary: \"%s\"", cs.summary);
    }

    /* Nothing done at all: every step times out, nothing is updated. */
    cs_start(&cs, t = 0, rate);
    double gm[3] = { 1, 1, 1 }, am[3] = { 3, 3, 3 };
    for (t = 0; t < 250 && cs.step != CS_DONE; t += dt) {
        if (cs.step == CS_STILL || cs.step == CS_POSES) cs_imu(&cs, t, gm, am);
        cs_tick(&cs, t);
    }
    calib_defaults(&cal);
    CHECK(cs.step == CS_DONE && cs_apply(&cs, &cal) == 0 && !cal.mag_valid,
          "all steps time out (%.0f s): nothing updated, \"%s\"", t, cs.summary);
    #undef RAW_ACC
}

int main(void)
{
    srand(12345);
    test_gps();
    test_geo();
    test_attitude();
    test_kf();
    test_frames();
    test_motion();
    test_calsession();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
