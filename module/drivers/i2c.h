/* Minimal Linux I2C helper (i2c-dev). */
#ifndef I2C_H
#define I2C_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int     fd;
    uint8_t addr;
} i2c_dev_t;

/* All functions return 0 on success or a negative errno. */
int  i2c_open(i2c_dev_t *dev, const char *bus, uint8_t addr);
void i2c_close(i2c_dev_t *dev);
int  i2c_write_reg(const i2c_dev_t *dev, uint8_t reg, uint8_t val);
/* Burst read starting at reg (repeated-start write+read in one transaction). */
int  i2c_read_regs(const i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len);
int  i2c_read_reg(const i2c_dev_t *dev, uint8_t reg, uint8_t *val);

#endif
