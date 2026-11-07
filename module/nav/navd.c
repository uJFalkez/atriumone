/* navd: navigation daemon. The only process that touches the I2C bus and the GPS UART.
 *
 * 50 Hz loop: MPU6500 -> median-of-3 glitch filter -> calibration -> attitude (Mahony)
 * -> world-frame acceleration -> Kalman filter (GPS position/velocity when there is a fix,
 * ZUPT while landed, dead reckoning otherwise). The magnetometer shares the bus with the
 * MPU, so it is only read in short bursts while the MPU is paused (see docs/module/CLAUDE.md):
 * periodically for yaw, and on request at photo time.
 *
 * Outputs follow docs/CONTRATO.md §3: position north/east/height from the mission start, with
 * source "gps+imu" | "imu" and sigma_m; orientation of the RGB camera frame relative to NED.
 * No latitude/longitude leaves this process.
 *
 * Control: abstract Unix stream socket "@payload-navd", one command per connection, one
 * JSON line back.
 *   state                    current estimate
 *   mag                      photo-grade mag burst now (~300 ms), then state
 *   reset <id>               mission start (~2.5 s): gyro bias if still (else the saved one),
 *                            level, zero position/height, snap yaw with the mag, GPS origin
 *                            if there is a fix. Never fails because of motion.
 *   resume <id> <n> <e> <h>  continue a mission after a reboot from the last known position
 *   stop                     mission over: stop integrating, close the debug log
 *   calib_start              guided calibration (still -> figure8 -> poses), see calsession.h;
 *                            progress in state.calibration_session; saved to the calibration
 *                            file and applied at once when done
 *   calib_abort              cancel it (nothing saved)
 *   quit                     exit
 */
#include "attitude.h"
#include "calib.h"
#include "calsession.h"
#include "frames.h"
#include "geo.h"
#include "gps.h"
#include "kf.h"
#include "motion.h"
#include "mpu6500.h"
#include "qmc5883p.h"
#include "util.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define G         9.80665
#define D2R       (M_PI / 180.0)
#define R2D       (180.0 / M_PI)
#define RATE_HZ   50
#define SOCK_NAME "payload-navd"
#define GPS_FRESH_S 3.0            /* a GPS update older than this means "no fix" */
#define STABLE_SPEED_M_S 0.15      /* photo gate: horizontal speed, applied only with a GPS fix */

/* ------------------------------------------------------------------ options */
static struct {
    const char *bus, *gps_dev, *calib_path, *log_dir;
    double mag_period_s;           /* periodic yaw burst, 0 = only on request */
    double declination_deg;
    double reset_still_s;
} opt = {
    .bus = "/dev/i2c-1", .gps_dev = "/dev/ttyS0", .calib_path = CALIB_PATH_DEFAULT,
    .log_dir = NULL, .mag_period_s = 10, .declination_deg = 0, .reset_still_s = 2.0,
};

/* ------------------------------------------------------------------ state */
static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static struct {
    mpu6500_t imu;
    qmc5883p_t mag;
    bool mag_ok;
    gps_t gps;
    bool gps_open;
    calib_t cal;

    /* fixed mounting inside the module (calibration file, section "mounting") */
    double imu_to_cam[3][3], mag_to_imu[3][3];
    bool imu_to_cam_measured, mag_to_imu_measured;

    attitude_t att;
    kf_t kf;
    motion_t mo;
    geo_origin_t origin;           /* internal only: never reported */
    double gyro_bias[3];
    const char *gyro_bias_source;  /* "still" | "calibration" | "none" */

    double gyro[3], accel[3];
    uint64_t last_t_ns;
    double rate_hz;
    unsigned imu_errors, imu_bad, imu_consec_err;
    double max_gap_ms, max_gap_ms_1s;
    bool mem_locked;
    double hist[3][6];
    int hist_n;

    unsigned gps_used, gps_rejected, gps_consec_rej;
    double last_gps_update;        /* mono s, 0 = never in this mission */

    struct {
        double t;
        double std[3];
        unsigned n_used;
        bool ok;
        double yaw_err_deg;
    } mag_last;
    double next_mag;

    calsession_t cs;
    bool cs_mag_streaming;         /* mag in continuous mode for the figure-8 step */
    int cs_saved;                  /* sensors updated by the last session, -errno if the save failed */

    char mission[64];
    double mission_t0;
    int log_fd;
    double next_log;
} S;

static double now_s(void) { return mono_ns() / 1e9; }

/* ------------------------------------------------------------------ calibration extras */
static bool read_matrix(cJSON *arr, double m[3][3])
{
    if (cJSON_GetArraySize(arr) != 3) return false;
    for (int i = 0; i < 3; i++) {
        cJSON *row = cJSON_GetArrayItem(arr, i);
        if (cJSON_GetArraySize(row) != 3) return false;
        for (int j = 0; j < 3; j++) {
            cJSON *v = cJSON_GetArrayItem(row, j);
            if (!cJSON_IsNumber(v)) return false;
            m[i][j] = v->valuedouble;
        }
    }
    return frames_is_rotation(m, 1e-3);
}

static void load_mounting(void)
{
    /* Until measured: MPU flat with Z up, camera looking down, image top = MPU +X. */
    const double guess[3][3] = { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } };
    memcpy(S.imu_to_cam, guess, sizeof guess);
    memset(S.mag_to_imu, 0, sizeof S.mag_to_imu);
    for (int i = 0; i < 3; i++) S.mag_to_imu[i][i] = 1;
    S.imu_to_cam_measured = S.mag_to_imu_measured = false;

    FILE *f = fopen(opt.calib_path, "r");
    if (!f) return;
    static char buf[32768];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf);
    cJSON *mnt = cJSON_GetObjectItem(root, "mounting");
    double m[3][3];
    if (read_matrix(cJSON_GetObjectItem(mnt, "imu_to_camera"), m)) {
        memcpy(S.imu_to_cam, m, sizeof m);
        S.imu_to_cam_measured = true;
    } else if (cJSON_GetObjectItem(mnt, "imu_to_camera")) {
        fprintf(stderr, "navd: mounting.imu_to_camera is not a rotation, ignored\n");
    }
    if (read_matrix(cJSON_GetObjectItem(mnt, "mag_to_imu"), m)) {
        memcpy(S.mag_to_imu, m, sizeof m);
        S.mag_to_imu_measured = true;
    } else if (cJSON_GetObjectItem(mnt, "mag_to_imu")) {
        fprintf(stderr, "navd: mounting.mag_to_imu is not a rotation, ignored\n");
    }
    cJSON_Delete(root);
}

static void use_saved_gyro_bias(void)
{
    if (S.cal.gyro_valid) {
        for (int i = 0; i < 3; i++) S.gyro_bias[i] = S.cal.gyro_bias[i];
        S.gyro_bias_source = "calibration";
    } else {
        memset(S.gyro_bias, 0, sizeof S.gyro_bias);
        S.gyro_bias_source = "none";
    }
}

/* ------------------------------------------------------------------ sensors */
static int imu_open(void)
{
    return mpu6500_init(&S.imu, opt.bus, MPU6500_ADDR_DEFAULT, &MPU6500_CONFIG_50HZ);
}

static void mag_open(void)
{
    int r = qmc5883p_init(&S.mag, opt.bus, QMC5883P_ADDR_DEFAULT, &QMC5883P_CONFIG_DEFAULT);
    if (r < 0)
        r = qmc5883p_init(&S.mag, opt.bus, QMC5883P_ADDR_ALT, &QMC5883P_CONFIG_DEFAULT);
    if (r == 0)
        r = qmc5883p_suspend(&S.mag);
    S.mag_ok = r == 0;
    if (r < 0)
        fprintf(stderr, "navd: mag unavailable: %s\n", strerror(-r));
}

static double med3(double a, double b, double c)
{
    return a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
}

/* One raw sample in -> one filtered, calibrated sample in S.gyro / S.accel. */
static void ingest(const mpu6500_sample_t *s)
{
    float a_cal[3];
    calib_apply_accel(&S.cal, s->accel, a_cal);
    memmove(S.hist[1], S.hist[0], sizeof S.hist[0] * 2);
    for (int i = 0; i < 3; i++) {
        S.hist[0][i] = s->gyro[i] - S.gyro_bias[i];
        S.hist[0][3 + i] = a_cal[i];
    }
    if (S.hist_n < 3) S.hist_n++;
    for (int k = 0; k < 6; k++) {
        double v = S.hist_n < 3 ? S.hist[0][k] : med3(S.hist[0][k], S.hist[1][k], S.hist[2][k]);
        if (k < 3) S.gyro[k] = v; else S.accel[k - 3] = v;
    }
}

/* Photo-grade or quick burst; the MPU is not read meanwhile. */
static int do_mag_burst(const qmc5883p_burst_opts_t *o)
{
    if (!S.mag_ok) {
        mag_open();
        if (!S.mag_ok) { S.mag_last.ok = false; return -ENODEV; }
    }
    qmc5883p_burst_t b;
    int r = qmc5883p_burst(&S.mag, o, &b);
    S.last_t_ns = mono_ns() - 1000000000ull / RATE_HZ;    /* planned pause, not a stall */
    if (r < 0) {
        S.mag_last.ok = false;
        if (r != -EBADMSG) S.mag_ok = false;              /* re-open next time */
        return r;
    }
    float raw[3] = { b.field[0], b.field[1], b.field[2] }, cal[3];
    calib_apply_mag(&S.cal, raw, cal);
    double m[3];
    for (int i = 0; i < 3; i++)
        m[i] = S.mag_to_imu[i][0] * cal[0] + S.mag_to_imu[i][1] * cal[1] + S.mag_to_imu[i][2] * cal[2];
    S.mag_last.t = b.t_ns / 1e9;
    for (int i = 0; i < 3; i++) S.mag_last.std[i] = b.std[i];
    S.mag_last.n_used = b.n_used;
    S.mag_last.ok = true;
    S.mag_last.yaw_err_deg = attitude_mag_correct(&S.att, m) * R2D;
    return 0;
}

/* ------------------------------------------------------------------ GPS */
static double gps_age(void) { return S.gps.fix.gga_t_ns ? now_s() - S.gps.fix.gga_t_ns / 1e9 : 1e9; }
static bool gps_fix(void) { return S.gps_open && S.gps.fix.quality > 0 && gps_age() < GPS_FRESH_S; }
static bool gps_present(void)
{
    return S.gps_open && S.gps.fix.last_byte_ns && now_s() - S.gps.fix.last_byte_ns / 1e9 < GPS_FRESH_S;
}
static bool pos_from_gps(void) { return S.last_gps_update > 0 && now_s() - S.last_gps_update < GPS_FRESH_S; }

static void gps_step(void)
{
    if (!S.gps_open) return;
    gps_poll(&S.gps);

    if (S.gps.new_rmc) {
        S.gps.new_rmc = 0;
        gps_fix_t *x = &S.gps.fix;
        if (x->rmc_valid && S.mission[0] && S.origin.valid) {
            double c = x->course * D2R;
            kf_update(&S.kf, 0, 1, x->speed * sin(c), 0.3);
            kf_update(&S.kf, 1, 1, x->speed * cos(c), 0.3);
        }
    }
    if (S.gps.new_gga) {
        S.gps.new_gga = 0;
        gps_fix_t *x = &S.gps.fix;
        if (x->quality <= 0 || !S.mission[0]) return;
        if (!S.origin.valid) {
            /* First fix after the start (or after a resume): anchor the origin so the current
             * estimate maps onto the fix, keeping the trajectory continuous. */
            geo_origin_t fixo = { true, x->lat, x->lon, x->alt };
            double back[3] = { -S.kf.ax[0].x[0], -S.kf.ax[1].x[0], -S.kf.ax[2].x[0] };
            geo_from_enu(&fixo, back, &S.origin.lat, &S.origin.lon, &S.origin.alt);
            S.origin.valid = true;
            S.last_gps_update = now_s();
            return;
        }
        double e[3];
        geo_to_enu(&S.origin, x->lat, x->lon, x->alt, e);
        double sh = fmax(2.5, 2.5 * x->hdop), sv = 2 * sh;
        /* Gate outliers, but accept the GPS again if it keeps disagreeing (IMU drifted). */
        double dE = e[0] - S.kf.ax[0].x[0], dN = e[1] - S.kf.ax[1].x[0];
        double var = S.kf.ax[0].P[0][0] + S.kf.ax[1].P[0][0] + 2 * sh * sh;
        if ((dE * dE + dN * dN) / var > 25.0 && S.gps_consec_rej < 5) {
            S.gps_rejected++;
            S.gps_consec_rej++;
            return;
        }
        if (S.gps_consec_rej >= 5) {
            for (int i = 0; i < 3; i++) {   /* re-seed position and its uncertainty */
                double sg = i < 2 ? sh : sv;
                S.kf.ax[i].x[0] = e[i];
                S.kf.ax[i].P[0][0] = sg * sg;
                S.kf.ax[i].P[0][1] = S.kf.ax[i].P[1][0] = S.kf.ax[i].P[0][2] = S.kf.ax[i].P[2][0] = 0;
            }
        }
        S.gps_consec_rej = 0;
        kf_update(&S.kf, 0, 0, e[0], sh);
        kf_update(&S.kf, 1, 0, e[1], sh);
        kf_update(&S.kf, 2, 0, e[2], sv);
        S.gps_used++;
        S.last_gps_update = now_s();
    }
}

/* ------------------------------------------------------------------ JSON state */
static void put_num_or_null(cJSON *o, const char *k, bool valid, double v)
{
    if (valid && isfinite(v)) cJSON_AddNumberToObject(o, k, v);
    else cJSON_AddNullToObject(o, k);
}

static cJSON *state_json(void)
{
    double t = now_s();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "t_mono", t);
    if (S.mission[0]) cJSON_AddStringToObject(o, "mission", S.mission);
    else cJSON_AddNullToObject(o, "mission");

    /* position (NED from the mission start; height positive up) */
    double enu[3] = { S.kf.ax[0].x[0], S.kf.ax[1].x[0], S.kf.ax[2].x[0] };
    double venu[3] = { S.kf.ax[0].x[1], S.kf.ax[1].x[1], S.kf.ax[2].x[1] };
    cJSON *p = cJSON_AddObjectToObject(o, "position");
    cJSON_AddNumberToObject(p, "north_m", enu[1]);
    cJSON_AddNumberToObject(p, "east_m", enu[0]);
    cJSON_AddNumberToObject(p, "height_m", enu[2]);
    cJSON_AddStringToObject(p, "source", pos_from_gps() ? "gps+imu" : "imu");
    cJSON_AddNumberToObject(p, "sigma_m", sqrt(S.kf.ax[0].P[0][0] + S.kf.ax[1].P[0][0]));
    cJSON_AddNumberToObject(p, "distance_from_start_m", hypot(enu[0], enu[1]));
    cJSON *v = cJSON_AddObjectToObject(o, "velocity");
    cJSON_AddNumberToObject(v, "north_m_s", venu[1]);
    cJSON_AddNumberToObject(v, "east_m_s", venu[0]);
    cJSON_AddNumberToObject(v, "up_m_s", venu[2]);
    cJSON_AddNumberToObject(v, "horizontal_m_s", hypot(venu[0], venu[1]));

    /* orientation of the RGB camera frame */
    cam_attitude_t ca;
    frames_camera_ned(S.att.q, S.imu_to_cam, &ca);
    bool yaw_ok = S.att.yaw_valid;
    cJSON *ori = cJSON_AddObjectToObject(o, "orientation");
    if (yaw_ok) {
        cJSON *q = cJSON_AddObjectToObject(ori, "quaternion");
        cJSON_AddNumberToObject(q, "w", ca.q[0]);
        cJSON_AddNumberToObject(q, "x", ca.q[1]);
        cJSON_AddNumberToObject(q, "y", ca.q[2]);
        cJSON_AddNumberToObject(q, "z", ca.q[3]);
    } else {
        cJSON_AddNullToObject(ori, "quaternion");   /* without yaw the rotation is incomplete */
    }
    cJSON_AddNumberToObject(ori, "roll_deg", ca.roll_deg);
    cJSON_AddNumberToObject(ori, "pitch_deg", ca.pitch_deg);
    put_num_or_null(ori, "yaw_deg", yaw_ok, ca.yaw_deg);
    cJSON_AddStringToObject(ori, "yaw_source",
                            !yaw_ok ? "none" : (S.cal.mag_valid && S.mag_to_imu_measured ? "mag" : "mag_uncalibrated"));
    cJSON_AddBoolToObject(ori, "mounting_measured", S.imu_to_cam_measured && S.mag_to_imu_measured);

    /* motion detectors */
    cJSON *st = cJSON_AddObjectToObject(o, "stability");
    /* Speed gate (operator's choice, 0.15 m/s) only with a GPS fix: the IMU-only velocity
     * drifts to metres per second and would block every photo. */
    bool speed_ok = !pos_from_gps() || hypot(S.kf.ax[0].x[1], S.kf.ax[1].x[1]) < STABLE_SPEED_M_S;
    cJSON_AddBoolToObject(st, "stable", S.mo.stable && speed_ok);
    cJSON_AddBoolToObject(st, "speed_checked", pos_from_gps());
    cJSON_AddNumberToObject(st, "angular_speed_rad_s", S.mo.angular_speed);
    cJSON_AddNumberToObject(st, "stable_for_s", S.mo.stable_for);
    cJSON *mo = cJSON_AddObjectToObject(o, "motion");
    cJSON_AddBoolToObject(mo, "stopped", S.mo.stopped);
    cJSON_AddNumberToObject(mo, "stopped_for_s", S.mo.stopped_for);
    cJSON_AddBoolToObject(mo, "landed", S.mo.landed);
    cJSON_AddNumberToObject(mo, "landed_for_s", S.mo.landed_for);
    cJSON_AddNumberToObject(mo, "gyro_std_dps", S.mo.gyro_std * R2D);
    cJSON_AddNumberToObject(mo, "accel_std_m_s2", S.mo.accel_std);

    /* device health, for status.devices */
    cJSON *dv = cJSON_AddObjectToObject(o, "devices");
    cJSON *imu = cJSON_AddObjectToObject(dv, "imu");
    cJSON_AddBoolToObject(imu, "ok", S.imu_consec_err == 0);
    cJSON_AddNumberToObject(imu, "rate_hz", S.rate_hz);
    cJSON_AddNumberToObject(imu, "errors", S.imu_errors);
    cJSON_AddNumberToObject(imu, "bad_frames", S.imu_bad);
    cJSON_AddNumberToObject(imu, "max_gap_ms", S.max_gap_ms);
    cJSON_AddBoolToObject(imu, "memory_locked", S.mem_locked);
    cJSON *mg = cJSON_AddObjectToObject(dv, "mag");
    cJSON_AddBoolToObject(mg, "ok", S.mag_ok);
    cJSON_AddBoolToObject(mg, "last_burst_ok", S.mag_last.ok);
    put_num_or_null(mg, "last_burst_age_s", S.mag_last.t > 0, t - S.mag_last.t);
    put_num_or_null(mg, "last_yaw_correction_deg", S.mag_last.t > 0, S.mag_last.yaw_err_deg);
    cJSON *gp = cJSON_AddObjectToObject(dv, "gps");
    cJSON_AddBoolToObject(gp, "enabled", S.gps_open);
    cJSON_AddBoolToObject(gp, "present", gps_present());
    cJSON_AddBoolToObject(gp, "fix", gps_fix());
    cJSON_AddNumberToObject(gp, "sats", S.gps.fix.sats);
    cJSON_AddNumberToObject(gp, "hdop", S.gps.fix.hdop);
    cJSON_AddNumberToObject(gp, "used", S.gps_used);
    cJSON_AddNumberToObject(gp, "rejected", S.gps_rejected);

    if (S.cs.step != CS_OFF) {
        cJSON *cs = cJSON_AddObjectToObject(o, "calibration_session");
        cJSON_AddBoolToObject(cs, "active", cs_active(&S.cs));
        cJSON_AddStringToObject(cs, "step", cs_step_name(&S.cs));
        cJSON_AddStringToObject(cs, "instruction", S.cs.instruction);
        cJSON_AddNumberToObject(cs, "progress", S.cs.progress);
        cJSON_AddNumberToObject(cs, "remaining_s", S.cs.remaining_s);
        if (S.cs.step == CS_DONE) {
            cJSON_AddNumberToObject(cs, "updated", S.cs_saved);
            cJSON *u = cJSON_AddObjectToObject(cs, "result");
            cJSON_AddBoolToObject(u, "gyro", S.cs.gyro_ok);
            cJSON_AddBoolToObject(u, "mag", S.cs.mag_ok);
            cJSON_AddBoolToObject(u, "accel", S.cs.accel_ok);
        }
    }
    cJSON *cal = cJSON_AddObjectToObject(o, "calibration");
    cJSON_AddBoolToObject(cal, "accel", S.cal.accel_valid);
    cJSON_AddBoolToObject(cal, "mag", S.cal.mag_valid);
    cJSON_AddStringToObject(cal, "gyro_bias_source", S.gyro_bias_source);
    cJSON_AddBoolToObject(cal, "imu_to_camera_measured", S.imu_to_cam_measured);
    cJSON_AddBoolToObject(cal, "mag_to_imu_measured", S.mag_to_imu_measured);
    return o;
}

/* ------------------------------------------------------------------ optional debug log */
/* The 50 Hz loop never touches the disk: lines go to a memory buffer that a writer thread
 * drains and fdatasync()s once per second (the USB disk can block for seconds).
 * The official track is missiond's track.csv; this richer log is only for debugging (-L). */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char buf[64 * 1024];
    size_t len;
    int fd;
    bool close_req, quit;
    unsigned dropped;
} LW = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .fd = -1 };

static void *log_writer(void *arg)
{
    (void)arg;
    static char local[sizeof LW.buf];
    pthread_mutex_lock(&LW.mu);
    for (;;) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += 1;
        while (!LW.quit && !LW.close_req && LW.len == 0)
            if (pthread_cond_timedwait(&LW.cv, &LW.mu, &dl) == ETIMEDOUT) break;
        size_t n = LW.len;
        int fd = LW.fd;
        bool closing = LW.close_req, quit = LW.quit;
        memcpy(local, LW.buf, n);
        LW.len = 0;
        pthread_mutex_unlock(&LW.mu);

        if (fd >= 0 && n) {
            size_t off = 0;
            while (off < n) {
                ssize_t w = write(fd, local + off, n - off);
                if (w <= 0) break;
                off += (size_t)w;
            }
            fdatasync(fd);
        }
        if (closing && fd >= 0) close(fd);

        pthread_mutex_lock(&LW.mu);
        if (closing) { LW.fd = -1; LW.close_req = false; pthread_cond_broadcast(&LW.cv); }
        if (quit) break;
        pthread_mutex_unlock(&LW.mu);
        sleep_ms(1000);
        pthread_mutex_lock(&LW.mu);
    }
    pthread_mutex_unlock(&LW.mu);
    return NULL;
}

static void log_append(const char *line, size_t n)
{
    pthread_mutex_lock(&LW.mu);
    if (LW.fd >= 0 && LW.len + n <= sizeof LW.buf) { memcpy(LW.buf + LW.len, line, n); LW.len += n; }
    else if (LW.fd >= 0) LW.dropped++;
    pthread_mutex_unlock(&LW.mu);
}

static void log_close(void)
{
    pthread_mutex_lock(&LW.mu);
    if (LW.fd >= 0) { LW.close_req = true; pthread_cond_signal(&LW.cv); }
    pthread_mutex_unlock(&LW.mu);
    S.log_fd = -1;
}

static void log_open_for(const char *id)
{
    log_close();
    if (!opt.log_dir) return;
    mkdir(opt.log_dir, 0755);
    char path[256];
    snprintf(path, sizeof path, "%s/nav_%s.csv", opt.log_dir, id);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    pthread_mutex_lock(&LW.mu);
    while (LW.close_req) pthread_cond_wait(&LW.cv, &LW.mu);
    LW.fd = fd;
    LW.len = 0;
    pthread_mutex_unlock(&LW.mu);
    S.log_fd = fd;
    static const char hdr[] = "t_s,north_m,east_m,height_m,vn,ve,vu,sigma_m,roll,pitch,yaw,yaw_valid,"
                              "source,sats,stable,w_rad_s,stopped,landed\n";
    log_append(hdr, sizeof hdr - 1);
    S.next_log = 0;
}

static void log_line(double t)
{
    if (S.log_fd < 0 || t < S.next_log) return;
    S.next_log = t + 0.2;
    cam_attitude_t ca;
    frames_camera_ned(S.att.q, S.imu_to_cam, &ca);
    char line[320];
    int n = snprintf(line, sizeof line,
                     "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.1f,%d,%s,%d,%d,%.3f,%d,%d\n",
                     t - S.mission_t0, S.kf.ax[1].x[0], S.kf.ax[0].x[0], S.kf.ax[2].x[0], S.kf.ax[1].x[1],
                     S.kf.ax[0].x[1], S.kf.ax[2].x[1], sqrt(S.kf.ax[0].P[0][0] + S.kf.ax[1].P[0][0]),
                     ca.roll_deg, ca.pitch_deg, ca.yaw_deg, S.att.yaw_valid, pos_from_gps() ? "gps+imu" : "imu",
                     S.gps.fix.sats, S.mo.stable, S.mo.angular_speed, S.mo.stopped, S.mo.landed);
    if (n > 0 && (size_t)n < sizeof line) log_append(line, (size_t)n);
}

/* ------------------------------------------------------------------ mission */
static bool valid_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= sizeof S.mission) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
              *p == '_'))
            return false;
    return true;
}

/* Averages `seconds` of IMU samples (blocking). Returns the number of good samples. */
static int average_imu(double seconds, double g_mean[3], double g_std[3], double f_mean[3])
{
    const int N = (int)(seconds * RATE_HZ);
    double gs[3] = { 0 }, gq[3] = { 0 }, as[3] = { 0 };
    int n = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (int i = 0; i < N + 20 && n < N; i++) {
        mpu6500_sample_t s;
        if (mpu6500_read(&S.imu, &s) == 0) {
            float a_cal[3];
            calib_apply_accel(&S.cal, s.accel, a_cal);
            for (int k = 0; k < 3; k++) {
                gs[k] += s.gyro[k]; gq[k] += (double)s.gyro[k] * s.gyro[k]; as[k] += a_cal[k];
            }
            n++;
        }
        next.tv_nsec += 1000000000L / RATE_HZ;
        while (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    for (int k = 0; k < 3; k++) {
        g_mean[k] = n ? gs[k] / n : 0;
        g_std[k] = n ? sqrt(fmax(0, gq[k] / n - g_mean[k] * g_mean[k])) : 0;
        f_mean[k] = n ? as[k] / n : 0;
    }
    return n;
}

static void start_mission_common(const char *id)
{
    snprintf(S.mission, sizeof S.mission, "%s", id);
    S.mission_t0 = now_s();
    S.hist_n = 0;
    S.origin.valid = false;
    S.gps_used = S.gps_rejected = S.gps_consec_rej = 0;
    S.last_gps_update = 0;
    S.max_gap_ms = S.max_gap_ms_1s = 0;
    motion_reset(&S.mo, now_s());
    log_open_for(id);
    S.last_t_ns = mono_ns();
}

/* Mission start. Never fails: a moving payload just falls back to the saved gyro bias. */
static void calib_session_end(void);

static void mission_reset(const char *id, cJSON *out)
{
    if (cs_active(&S.cs)) { calib_session_end(); S.cs.step = CS_OFF; }
    double gm[3], gsd[3], f[3];
    int n = average_imu(opt.reset_still_s, gm, gsd, f);
    double worst = fmax(gsd[0], fmax(gsd[1], gsd[2]));
    bool imu_ok = n >= (int)(opt.reset_still_s * RATE_HZ) * 9 / 10;
    bool still = imu_ok && worst * R2D < 0.5;          /* same threshold as calib_gyro */

    if (still) {
        memcpy(S.gyro_bias, gm, sizeof gm);
        S.gyro_bias_source = "still";
    } else {
        use_saved_gyro_bias();
    }
    if (imu_ok) attitude_reset_from_accel(&S.att, f);   /* mean of 2 s ~ gravity even with vibration */
    kf_reset(&S.kf, 0.1);
    start_mission_common(id);
    if (gps_fix()) {
        S.origin = (geo_origin_t){ true, S.gps.fix.lat, S.gps.fix.lon, S.gps.fix.alt };
        S.last_gps_update = now_s();
    }
    int mr = do_mag_burst(&QMC5883P_BURST_PHOTO);

    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON_AddBoolToObject(out, "imu_ok", imu_ok);
    cJSON_AddBoolToObject(out, "still", still);
    cJSON_AddNumberToObject(out, "gyro_std_dps", worst * R2D);
    cJSON_AddStringToObject(out, "gyro_bias_source", S.gyro_bias_source);
    cJSON_AddBoolToObject(out, "origin_from_gps", S.origin.valid);
    cJSON_AddBoolToObject(out, "yaw_from_mag", mr == 0);
}

/* Continue after a reboot: position from the last track line, velocity unknown. */
static void mission_resume(const char *id, double n, double e, double h, cJSON *out)
{
    double gm[3], gsd[3], f[3];
    use_saved_gyro_bias();
    int k = average_imu(0.5, gm, gsd, f);
    double worst = fmax(gsd[0], fmax(gsd[1], gsd[2]));
    if (k > 0 && worst * R2D < 0.5) attitude_reset_from_accel(&S.att, f);   /* else keep the boot leveling */
    kf_reset(&S.kf, 0.2);
    double enu[3] = { e, n, h };
    for (int i = 0; i < 3; i++) {
        S.kf.ax[i].x[0] = enu[i];
        S.kf.ax[i].P[0][0] = 25.0;                       /* 5 m: the gap is unknown */
        S.kf.ax[i].P[1][1] = 4.0;                        /* 2 m/s: may be flying */
    }
    start_mission_common(id);
    int mr = do_mag_burst(&QMC5883P_BURST_PHOTO);
    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON_AddStringToObject(out, "gyro_bias_source", S.gyro_bias_source);
    cJSON_AddBoolToObject(out, "yaw_from_mag", mr == 0);
}

/* ------------------------------------------------------------------ guided calibration */
static void calib_session_end(void)
{
    if (S.cs_mag_streaming && S.mag_ok) qmc5883p_suspend(&S.mag);
    S.cs_mag_streaming = false;
}

/* Called every loop while a session runs. Returns true when the MPU must not be read. */
static bool calib_session_step(double t)
{
    if (!cs_active(&S.cs)) return false;
    if (cs_wants_mag(&S.cs)) {
        if (!S.cs_mag_streaming) {
            if (!S.mag_ok) mag_open();
            if (S.mag_ok && qmc5883p_resume(&S.mag) == 0) S.cs_mag_streaming = true;
        }
        if (S.cs_mag_streaming) {
            qmc5883p_sample_t m;
            if (qmc5883p_read(&S.mag, &m) == 0) {
                double f[3] = { m.field[0], m.field[1], m.field[2] };
                cs_mag(&S.cs, t, f);
            }
        }
    } else if (S.cs_mag_streaming) {
        calib_session_end();                     /* figure8 over: mag back to suspend */
    }
    cs_tick(&S.cs, t);
    if (S.cs.step == CS_DONE) {                  /* just finished: save and apply */
        calib_session_end();
        calib_t cal = S.cal;
        int n = cs_apply(&S.cs, &cal);
        if (n > 0) {
            int r = calib_save(&cal, opt.calib_path);
            if (r == 0) {
                S.cal = cal;
                if (!S.mission[0]) use_saved_gyro_bias();
            }
            S.cs_saved = r == 0 ? n : r;
        }
        fprintf(stderr, "navd: calibration done: %s (saved %d)\n", S.cs.summary, S.cs_saved);
        fprintf(stderr, "navd:   gyro %s | mag %s: %d pts (%d outliers dropped, %d glitches rejected), "
                        "coverage %.0f/14, residual %.1f%% | accel %s%s: %d/6 faces, worst %.3f m/s2\n",
                S.cs.gyro_ok ? "ok" : "no", S.cs.mag_ok ? "ok" : "no", S.cs.mag_fit_pts, S.cs.mag_outliers,
                S.cs.mag_rejected_pts, S.cs.mag_coverage * 14, S.cs.mag_resid_rel * 100,
                S.cs.accel_ok ? "ok" : "no", S.cs.accel_partial ? " (partial)" : "", S.cs.nfaces,
                S.cs.accel_worst);
        S.last_t_ns = mono_ns();
        return false;
    }
    if (cs_wants_mag(&S.cs)) {
        S.last_t_ns = mono_ns() - 1000000000ull / RATE_HZ;   /* MPU paused on purpose */
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ control socket */
static int listen_fd = -1;

static int ctl_open(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));   /* abstract namespace */
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCK_NAME));
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) return -errno;
    if (bind(listen_fd, (struct sockaddr *)&a, len) < 0 || listen(listen_fd, 8) < 0) return -errno;
    return 0;
}

static void reply(int fd, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    size_t n = strlen(s), off = 0;
    while (off < n) {
        ssize_t w = write(fd, s + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    if (write(fd, "\n", 1) < 0) { /* client gone */ }
    free(s);
}

static cJSON *error_json(const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", msg);
    return o;
}

static void ctl_poll(void)
{
    for (int k = 0; k < 4; k++) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) return;
        struct timeval tv = { 0, 100000 };        /* clients send at once; cap the stall */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char buf[160] = { 0 };
        ssize_t r = read(fd, buf, sizeof buf - 1);
        if (r <= 0) { close(fd); continue; }
        buf[strcspn(buf, "\r\n")] = 0;

        char id[96];
        double n, e, h;
        cJSON *o = NULL;
        if (!strcmp(buf, "state")) {
            o = state_json();
        } else if (!strcmp(buf, "mag")) {
            int mr = do_mag_burst(&QMC5883P_BURST_PHOTO);
            o = state_json();
            cJSON_AddBoolToObject(o, "mag_burst_ok", mr == 0);
        } else if (sscanf(buf, "reset %95s", id) == 1) {
            if (!valid_id(id)) o = error_json("bad_mission_id");
            else { o = cJSON_CreateObject(); mission_reset(id, o); }
        } else if (sscanf(buf, "resume %95s %lf %lf %lf", id, &n, &e, &h) == 4) {
            if (!valid_id(id) || !isfinite(n) || !isfinite(e) || !isfinite(h)) o = error_json("bad_arguments");
            else { o = cJSON_CreateObject(); mission_resume(id, n, e, h, o); }
        } else if (!strcmp(buf, "calib_start")) {
            if (S.mission[0]) {
                o = error_json("mission_active");
            } else {
                cs_start(&S.cs, now_s(), RATE_HZ);
                S.cs_saved = 0;
                o = state_json();
                cJSON_AddBoolToObject(o, "ok", 1);
            }
        } else if (!strcmp(buf, "calib_abort")) {
            calib_session_end();
            S.cs.step = CS_OFF;
            o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
        } else if (!strcmp(buf, "stop")) {
            log_close();
            S.mission[0] = 0;
            o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
        } else if (!strcmp(buf, "quit")) {
            running = 0;
            o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", 1);
        } else {
            o = error_json("unknown_command");
        }
        reply(fd, o);
        cJSON_Delete(o);
        close(fd);
    }
}

/* ------------------------------------------------------------------ main */
static void usage(void)
{
    fprintf(stderr,
            "usage: navd [-b i2c_bus] [-g gps_dev|none] [-f calibration.json] [-L debug_log_dir]\n"
            "            [-M mag_period_s] [-D declination_deg]\n");
}

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "b:g:f:L:M:D:h")) != -1) {
        switch (c) {
        case 'b': opt.bus = optarg; break;
        case 'g': opt.gps_dev = optarg; break;
        case 'f': opt.calib_path = optarg; break;
        case 'L': opt.log_dir = optarg; break;
        case 'M': opt.mag_period_s = atof(optarg); break;
        case 'D': opt.declination_deg = atof(optarg); break;
        default: usage(); return 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Never let the 50 Hz loop be swapped out (the Pi swaps heavily on the bench). */
    S.mem_locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
    if (!S.mem_locked) perror("navd: mlockall (continuing)");

    calib_defaults(&S.cal);
    if (calib_load(&S.cal, opt.calib_path) < 0)
        fprintf(stderr, "navd: warning: could not parse %s, using no calibration\n", opt.calib_path);
    load_mounting();
    use_saved_gyro_bias();

    attitude_init(&S.att);
    S.att.declination = opt.declination_deg * D2R;
    S.kf.accel_sigma = 0.5;
    S.kf.bias_rw = 0.002;
    kf_reset(&S.kf, 0.1);
    motion_init(&S.mo, RATE_HZ);
    S.log_fd = -1;

    int r = imu_open();
    if (r < 0) { fprintf(stderr, "navd: MPU6500 on %s: %s\n", opt.bus, strerror(-r)); return 1; }
    mag_open();
    S.gps.fd = -1;
    if (strcmp(opt.gps_dev, "none")) {
        r = gps_open(&S.gps, opt.gps_dev);
        S.gps_open = r == 0;
        if (r < 0) fprintf(stderr, "navd: GPS %s: %s (continuing without)\n", opt.gps_dev, strerror(-r));
    }

    /* Small stack: with mlockall(MCL_FUTURE) a default 8 MB stack exceeds RLIMIT_MEMLOCK. */
    pthread_t writer;
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, 256 * 1024);
    if ((r = pthread_create(&writer, &pa, log_writer, NULL)) != 0) {
        fprintf(stderr, "navd: log writer thread: %s\n", strerror(r));
        return 1;
    }
    pthread_attr_destroy(&pa);
    if ((r = ctl_open()) < 0) { fprintf(stderr, "navd: control socket: %s\n", strerror(-r)); return 1; }

    /* Level from a short average so the state is sane before the first reset. */
    {
        double gm[3], gsd[3], f[3];
        if (average_imu(0.5, gm, gsd, f) > 0) attitude_reset_from_accel(&S.att, f);
    }
    S.next_mag = now_s() + 1.0;
    fprintf(stderr, "navd: running (MPU 50 Hz on %s, mag %s, GPS %s, mounting %s)\n", opt.bus,
            S.mag_ok ? "ok" : "absent", S.gps_open ? opt.gps_dev : "off",
            S.imu_to_cam_measured && S.mag_to_imu_measured ? "measured" : "assumed");

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    S.last_t_ns = mono_ns();
    double rate_t0 = now_s();
    unsigned rate_n = 0;
    double next_reopen = 0;

    while (running) {
        double t = now_s();
        mpu6500_sample_t s;
        int rr;
        if (calib_session_step(t)) {
            rr = 1;                               /* figure-8 step: MPU not read */
        } else {
            rr = mpu6500_read(&S.imu, &s);
            if (rr == 0 && cs_active(&S.cs)) {
                double g[3] = { s.gyro[0], s.gyro[1], s.gyro[2] }, a[3] = { s.accel[0], s.accel[1], s.accel[2] };
                cs_imu(&S.cs, t, g, a);         /* raw samples: the session fits them */
            }
        }
        if (rr == 0) {
            S.imu_consec_err = 0;
            double dt = (s.t_ns - S.last_t_ns) / 1e9;
            S.last_t_ns = s.t_ns;
            if (S.mission[0] && dt * 1000 > S.max_gap_ms_1s) S.max_gap_ms_1s = dt * 1000;
            if (dt <= 0 || dt > 0.5) dt = 1.0 / RATE_HZ;   /* first sample / long stall */
            ingest(&s);
            attitude_update(&S.att, S.gyro, S.accel, dt);
            motion_update_imu(&S.mo, t, S.gyro, S.accel);
            if (S.mission[0]) {
                double aw[3];
                attitude_rotate(&S.att, S.accel, aw);
                aw[2] -= G;
                kf_predict(&S.kf, aw, dt);
                if (S.mo.landed)
                    for (int i = 0; i < 3; i++) kf_update(&S.kf, i, 1, 0.0, 0.02);   /* ZUPT */
                motion_update_speed(&S.mo, t, hypot(S.kf.ax[0].x[1], S.kf.ax[1].x[1]));
            }
            rate_n++;
        } else if (rr < 0) {
            if (rr == -EBADMSG) S.imu_bad++;
            else { S.imu_errors++; S.imu_consec_err++; }
            if (S.imu_consec_err > RATE_HZ && t > next_reopen) {
                mpu6500_close(&S.imu);
                if (imu_open() == 0) S.imu_consec_err = 0;
                next_reopen = t + 1.0;
            }
        }
        if (t - rate_t0 >= 1.0) {
            S.rate_hz = rate_n / (t - rate_t0);
            rate_n = 0;
            rate_t0 = t;
            if (S.max_gap_ms_1s > S.max_gap_ms) S.max_gap_ms = S.max_gap_ms_1s;
            S.max_gap_ms_1s = 0;
        }

        gps_step();
        if (opt.mag_period_s > 0 && t >= S.next_mag && !cs_active(&S.cs)) {
            do_mag_burst(&QMC5883P_BURST_QUICK);
            S.next_mag = now_s() + opt.mag_period_s;
            clock_gettime(CLOCK_MONOTONIC, &next);       /* don't try to catch up */
        }
        log_line(t);
        ctl_poll();

        next.tv_nsec += 1000000000L / RATE_HZ;
        while (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        struct timespec nowts;
        clock_gettime(CLOCK_MONOTONIC, &nowts);
        if (nowts.tv_sec > next.tv_sec + 1) next = nowts;   /* fell far behind (e.g. reset) */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    log_close();
    pthread_mutex_lock(&LW.mu);
    LW.quit = true;
    pthread_cond_signal(&LW.cv);
    pthread_mutex_unlock(&LW.mu);
    pthread_join(writer, NULL);
    if (S.mag_ok) qmc5883p_close(&S.mag);
    mpu6500_close(&S.imu);
    gps_close(&S.gps);
    fprintf(stderr, "navd: stopped\n");
    return 0;
}
