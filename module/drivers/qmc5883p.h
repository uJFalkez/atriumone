/* QST QMC5883P 3-axis magnetometer driver (I2C). Outputs are UNCALIBRATED.
 * Note: many "HMC5883L"/GY-271 boards actually ship with this chip. */
#ifndef QMC5883P_H
#define QMC5883P_H

#include <stdint.h>
#include "i2c.h"

#define QMC5883P_ADDR_DEFAULT 0x2C
#define QMC5883P_ADDR_ALT     0x3C  /* observed after a bus glitch on our unit; persists across power cycles */
#define QMC5883P_CHIP_ID_VAL  0x80

typedef enum { QMC_RANGE_30G = 0, QMC_RANGE_12G, QMC_RANGE_8G, QMC_RANGE_2G } qmc_range_t;
typedef enum { QMC_ODR_10HZ = 0, QMC_ODR_50HZ, QMC_ODR_100HZ, QMC_ODR_200HZ } qmc_odr_t;
/* Oversampling ratios: OSR1 {8,4,2,1}, OSR2 {1,2,4,8} as register codes 0..3. */

typedef struct {
    qmc_range_t range;
    qmc_odr_t   odr;
    uint8_t     osr1;  /* 0..3 */
    uint8_t     osr2;  /* 0..3 */
} qmc5883p_config_t;

/* ±8 G (Earth field is ~0.25 G), 100 Hz, OSR1=8, OSR2=4 for low noise. */
#define QMC5883P_CONFIG_DEFAULT \
    ((qmc5883p_config_t){ QMC_RANGE_8G, QMC_ODR_100HZ, 0, 2 })

typedef struct {
    i2c_dev_t         i2c;
    qmc5883p_config_t cfg;
    float             lsb_per_gauss;
    uint8_t           ctrl1, ctrl2;   /* expected register values */
    uint64_t          last_data_ns;   /* last fresh sample */
    uint64_t          last_check_ns;  /* last config verification */
    uint64_t          stall_ns;       /* no-data timeout before recovery */
    unsigned          recoveries;     /* watchdog re-inits since init (total) */
    unsigned          rec_config;     /*   ...because CTRL1/CTRL2 changed */
    unsigned          rec_stall;      /*   ...because no data within stall_ns */
} qmc5883p_t;

typedef struct {
    uint64_t t_ns;
    int16_t  raw[3];
    float    field[3];  /* µT */
    int      overflow;
} qmc5883p_sample_t;

int  qmc5883p_init(qmc5883p_t *dev, const char *bus, uint8_t addr, const qmc5883p_config_t *cfg);
/* Returns 0 with fresh data, -EAGAIN if no new sample is ready yet.
 * Watchdog: re-initializes the chip (counted in dev->recoveries) if CTRL1/CTRL2
 * no longer match the configuration or no data arrives within dev->stall_ns. */
int  qmc5883p_read(qmc5883p_t *dev, qmc5883p_sample_t *s);
void qmc5883p_close(qmc5883p_t *dev);

/* ---- On-demand use (mag shares the bus with the MPU; see docs/module/CLAUDE.md) ----
 * After qmc5883p_init(), call qmc5883p_suspend() and only wake the chip for a burst while
 * nothing else talks on the bus. Tested: drop 3, average 27 -> ~0.3 µT repeatability, ~300 ms. */
typedef struct {
    unsigned n_drop;       /* samples discarded after wake-up */
    unsigned n_avg;        /* samples averaged */
    unsigned timeout_ms;   /* whole burst */
    float    outlier_ut;   /* samples farther than this from the median vector are rejected */
} qmc5883p_burst_opts_t;

#define QMC5883P_BURST_PHOTO ((qmc5883p_burst_opts_t){ 3, 27, 1000, 10.0f })
#define QMC5883P_BURST_QUICK ((qmc5883p_burst_opts_t){ 3, 8, 400, 10.0f })

typedef struct {
    uint64_t t_ns;         /* midpoint of the averaged samples */
    float    field[3];     /* µT, mean of inliers (UNCALIBRATED) */
    float    std[3];       /* µT, per-axis std of inliers */
    unsigned n_used, n_rejected;
    int      overflow;     /* any sample flagged OVFL */
    unsigned duration_ms;
} qmc5883p_burst_t;

int qmc5883p_suspend(qmc5883p_t *dev);
/* Back to continuous (normal) mode after qmc5883p_suspend(), e.g. for a calibration that
 * streams the mag while the MPU is paused. Resets the watchdog timers. */
int qmc5883p_resume(qmc5883p_t *dev);
/* Wakes the chip, collects a burst, suspends it again. Re-configures first if CTRL2 or the
 * non-mode bits of CTRL1 changed (counted in rec_config). Returns 0, a negative errno, or
 * -EBADMSG if fewer than 60 % of n_avg samples were usable. */
int qmc5883p_burst(qmc5883p_t *dev, const qmc5883p_burst_opts_t *o, qmc5883p_burst_t *out);

#endif
