/* MPU-6500 accelerometer + gyroscope driver (I2C). Outputs are UNCALIBRATED. */
#ifndef MPU6500_H
#define MPU6500_H

#include <stdint.h>
#include "i2c.h"

#define MPU6500_ADDR_DEFAULT 0x68
#define MPU6500_WHO_AM_I_VAL 0x70

typedef enum { MPU_ACCEL_2G = 0, MPU_ACCEL_4G, MPU_ACCEL_8G, MPU_ACCEL_16G } mpu_accel_range_t;
typedef enum { MPU_GYRO_250 = 0, MPU_GYRO_500, MPU_GYRO_1000, MPU_GYRO_2000 } mpu_gyro_range_t;

typedef struct {
    mpu_accel_range_t accel_range;
    mpu_gyro_range_t  gyro_range;
    uint8_t dlpf;        /* 0..6, gyro DLPF_CFG (3 = 41 Hz) */
    uint8_t accel_dlpf;  /* 0..7, A_DLPF_CFG   (3 = 45 Hz) */
    uint8_t smplrt_div;  /* output rate = 1 kHz / (1 + div) */
} mpu6500_config_t;

/* ±4 g, ±500 °/s, ~41 Hz filters, 200 Hz output (calibration tools). */
#define MPU6500_CONFIG_DEFAULT \
    ((mpu6500_config_t){ MPU_ACCEL_4G, MPU_GYRO_500, 3, 3, 4 })

/* Mission rate: ±4 g, ±500 °/s, ~20 Hz filters (anti-alias), 50 Hz output. */
#define MPU6500_CONFIG_50HZ \
    ((mpu6500_config_t){ MPU_ACCEL_4G, MPU_GYRO_500, 4, 4, 19 })

typedef struct {
    i2c_dev_t i2c;
    float     accel_lsb_per_g;
    float     gyro_lsb_per_dps;
} mpu6500_t;

typedef struct {
    uint64_t t_ns;      /* CLOCK_MONOTONIC at read time */
    int16_t  raw_accel[3];
    int16_t  raw_gyro[3];
    int16_t  raw_temp;
    float    accel[3];  /* m/s^2 */
    float    gyro[3];   /* rad/s */
    float    temp_c;
} mpu6500_sample_t;

int  mpu6500_init(mpu6500_t *dev, const char *bus, uint8_t addr, const mpu6500_config_t *cfg);
/* Returns 0, a negative errno on I/O error, or -EBADMSG for a frame that matches a known
 * bus-corruption signature (all 0x00 / all 0xFF, impossible temperature). On -EBADMSG the
 * sample is still filled in but must not be used. */
int  mpu6500_read(mpu6500_t *dev, mpu6500_sample_t *s);
void mpu6500_close(mpu6500_t *dev);

#endif
