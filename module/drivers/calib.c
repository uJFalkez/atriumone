#include "calib.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void calib_defaults(calib_t *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 3; i++) {
        c->accel_scale[i] = 1.0;
        c->mag_matrix[i][i] = 1.0;
    }
}

void calib_timestamp(char out[32])
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* ---- JSON helpers ---- */

static bool get_num(const cJSON *o, const char *k, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

static bool get_vec3(const cJSON *o, const char *k, double out[3])
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 3) return false;
    for (int i = 0; i < 3; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(v)) return false;
        out[i] = v->valuedouble;
    }
    return true;
}

static bool get_mat3(const cJSON *o, const char *k, double out[3][3])
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 3) return false;
    for (int i = 0; i < 3; i++) {
        const cJSON *row = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 3) return false;
        for (int j = 0; j < 3; j++) {
            const cJSON *v = cJSON_GetArrayItem(row, j);
            if (!cJSON_IsNumber(v)) return false;
            out[i][j] = v->valuedouble;
        }
    }
    return true;
}

static void get_str(const cJSON *o, const char *k, char out[32])
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(out, v->valuestring, 31);
        out[31] = '\0';
    }
}

static char *read_file(const char *path, int *err)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *err = -errno; return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = (len >= 0 && len < (1 << 20)) ? malloc((size_t)len + 1) : NULL;
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        *err = -EIO;
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* ---- load ---- */

int calib_load(calib_t *c, const char *path)
{
    int err = 0;
    calib_defaults(c);

    char *text = read_file(path, &err);
    if (!text)
        return err == -ENOENT ? 0 : err;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -EINVAL;
    }

    const cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "gyro");
    if (cJSON_IsObject(g) && get_vec3(g, "bias_rad_s", c->gyro_bias)) {
        c->gyro_valid = true;
        get_vec3(g, "std_rad_s", c->gyro_std);
        get_num(g, "temp_c", &c->gyro_temp_c);
        get_str(g, "time", c->gyro_time);
    }

    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "accel");
    if (cJSON_IsObject(a) && get_vec3(a, "offset_m_s2", c->accel_offset) &&
        get_vec3(a, "scale", c->accel_scale)) {
        c->accel_valid = true;
        get_num(a, "residual_m_s2", &c->accel_residual);
        get_str(a, "time", c->accel_time);
    } else {
        for (int i = 0; i < 3; i++) { c->accel_offset[i] = 0; c->accel_scale[i] = 1; }
    }

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "mag");
    if (cJSON_IsObject(m) && get_vec3(m, "offset_ut", c->mag_offset) &&
        get_mat3(m, "matrix", c->mag_matrix)) {
        c->mag_valid = true;
        get_num(m, "field_ut", &c->mag_field);
        get_num(m, "residual_ut", &c->mag_residual);
        get_num(m, "coverage", &c->mag_coverage);
        get_str(m, "time", c->mag_time);
    } else {
        memset(c->mag_offset, 0, sizeof c->mag_offset);
        memset(c->mag_matrix, 0, sizeof c->mag_matrix);
        for (int i = 0; i < 3; i++) c->mag_matrix[i][i] = 1;
    }

    cJSON_Delete(root);
    return 0;
}

/* ---- save ---- */

static void put(cJSON *root, const char *key, cJSON *item)
{
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddItemToObject(root, key, item);
}

int calib_save(const calib_t *c, const char *path)
{
    int err = 0;
    cJSON *root = NULL;
    char *text = read_file(path, &err);
    if (text) {
        root = cJSON_Parse(text);  /* keep keys written by other parts of the stack */
        free(text);
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    put(root, "version", cJSON_CreateNumber(CALIB_VERSION));

    if (c->gyro_valid) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddItemToObject(g, "bias_rad_s", cJSON_CreateDoubleArray(c->gyro_bias, 3));
        cJSON_AddItemToObject(g, "std_rad_s", cJSON_CreateDoubleArray(c->gyro_std, 3));
        cJSON_AddNumberToObject(g, "temp_c", c->gyro_temp_c);
        cJSON_AddStringToObject(g, "time", c->gyro_time);
        put(root, "gyro", g);
    }
    if (c->accel_valid) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddItemToObject(a, "offset_m_s2", cJSON_CreateDoubleArray(c->accel_offset, 3));
        cJSON_AddItemToObject(a, "scale", cJSON_CreateDoubleArray(c->accel_scale, 3));
        cJSON_AddNumberToObject(a, "residual_m_s2", c->accel_residual);
        cJSON_AddStringToObject(a, "time", c->accel_time);
        put(root, "accel", a);
    }
    if (c->mag_valid) {
        cJSON *m = cJSON_CreateObject(), *mat = cJSON_CreateArray();
        for (int i = 0; i < 3; i++)
            cJSON_AddItemToArray(mat, cJSON_CreateDoubleArray(c->mag_matrix[i], 3));
        cJSON_AddItemToObject(m, "offset_ut", cJSON_CreateDoubleArray(c->mag_offset, 3));
        cJSON_AddItemToObject(m, "matrix", mat);
        cJSON_AddNumberToObject(m, "field_ut", c->mag_field);
        cJSON_AddNumberToObject(m, "residual_ut", c->mag_residual);
        cJSON_AddNumberToObject(m, "coverage", c->mag_coverage);
        cJSON_AddStringToObject(m, "time", c->mag_time);
        put(root, "mag", m);
    }

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out)
        return -ENOMEM;

    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        cJSON_free(out);
        return -ENAMETOOLONG;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        err = -errno;
        cJSON_free(out);
        return err;
    }
    size_t len = strlen(out);
    ssize_t w = write(fd, out, len);
    if (w == (ssize_t)len) w = write(fd, "\n", 1) == 1 ? (ssize_t)len : -1;
    err = (w == (ssize_t)len && fsync(fd) == 0) ? 0 : -EIO;
    close(fd);
    cJSON_free(out);
    if (err == 0 && rename(tmp, path) != 0)
        err = -errno;
    if (err != 0)
        unlink(tmp);
    return err;
}

/* ---- apply ---- */

void calib_apply_gyro(const calib_t *c, const float in[3], float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = in[i] - (float)c->gyro_bias[i];
}

void calib_apply_accel(const calib_t *c, const float in[3], float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = (in[i] - (float)c->accel_offset[i]) * (float)c->accel_scale[i];
}

void calib_apply_mag(const calib_t *c, const float in[3], float out[3])
{
    double d[3];
    for (int i = 0; i < 3; i++) d[i] = in[i] - c->mag_offset[i];
    for (int i = 0; i < 3; i++)
        out[i] = (float)(c->mag_matrix[i][0] * d[0] + c->mag_matrix[i][1] * d[1] +
                         c->mag_matrix[i][2] * d[2]);
}
