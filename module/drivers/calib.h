/* Sensor calibration store (JSON) shared by the whole stack.
 *
 * File layout (all sections optional; unknown top-level keys are preserved on save):
 * {
 *   "version": 1,
 *   "gyro":  { "bias_rad_s": [x,y,z], "std_rad_s": [..], "temp_c": t, "time": "ISO-8601" },
 *   "accel": { "offset_m_s2": [..], "scale": [..], "residual_m_s2": r, "time": ".." },
 *   "mag":   { "offset_ut": [..], "matrix": [[..],[..],[..]], "field_ut": f,
 *              "residual_ut": r, "coverage": c, "time": ".." }
 * }
 * Corrected values:
 *   gyro  = raw - bias
 *   accel = (raw - offset) * scale        (per axis)
 *   mag   = matrix * (raw - offset)
 */
#ifndef CALIB_H
#define CALIB_H

#include <stdbool.h>

#define CALIB_PATH_DEFAULT "data/calibration/calibration.json"  /* docs/PIPELINE.md layout */
#define CALIB_VERSION      1

typedef struct {
    bool   gyro_valid;
    double gyro_bias[3];
    double gyro_std[3];
    double gyro_temp_c;
    char   gyro_time[32];

    bool   accel_valid;
    double accel_offset[3];
    double accel_scale[3];
    double accel_residual;
    char   accel_time[32];

    bool   mag_valid;
    double mag_offset[3];
    double mag_matrix[3][3];
    double mag_field;
    double mag_residual;
    double mag_coverage;
    char   mag_time[32];
} calib_t;

/* Identity calibration, all sections invalid. */
void calib_defaults(calib_t *c);
/* Loads sections present in the file. A missing file is not an error (returns 0,
 * defaults kept). Returns <0 on unreadable or malformed JSON. */
int  calib_load(calib_t *c, const char *path);
/* Writes valid sections atomically (temp file + rename), keeping other keys. */
int  calib_save(const calib_t *c, const char *path);
/* Current UTC time as ISO-8601 (for the *_time fields). */
void calib_timestamp(char out[32]);

void calib_apply_gyro(const calib_t *c, const float in[3], float out[3]);
void calib_apply_accel(const calib_t *c, const float in[3], float out[3]);
void calib_apply_mag(const calib_t *c, const float in[3], float out[3]);

#endif
