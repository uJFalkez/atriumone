#include "i2c.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

int i2c_open(i2c_dev_t *dev, const char *bus, uint8_t addr)
{
    dev->fd = open(bus, O_RDWR | O_CLOEXEC);
    if (dev->fd < 0)
        return -errno;
    dev->addr = addr;
    return 0;
}

void i2c_close(i2c_dev_t *dev)
{
    if (dev->fd >= 0)
        close(dev->fd);
    dev->fd = -1;
}

#define I2C_ATTEMPTS 3

/* Transient bus errors (NACK, timeout) are retried; all callers' transfers are idempotent. */
static int transfer(const i2c_dev_t *dev, struct i2c_msg *msgs, unsigned n)
{
    struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = n };
    int err = 0;

    for (int attempt = 0; attempt < I2C_ATTEMPTS; attempt++) {
        if (ioctl(dev->fd, I2C_RDWR, &xfer) >= 0)
            return 0;
        err = errno;
        if (err != ENXIO && err != EIO && err != EREMOTEIO && err != ETIMEDOUT && err != EAGAIN)
            break;
        sleep_ms(1);
    }
    return -err;
}

int i2c_write_reg(const i2c_dev_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    struct i2c_msg msg = { .addr = dev->addr, .flags = 0, .len = 2, .buf = buf };

    return transfer(dev, &msg, 1);
}

int i2c_read_regs(const i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    struct i2c_msg msgs[2] = {
        { .addr = dev->addr, .flags = 0,        .len = 1,              .buf = &reg },
        { .addr = dev->addr, .flags = I2C_M_RD, .len = (uint16_t)len,  .buf = buf  },
    };

    return transfer(dev, msgs, 2);
}

int i2c_read_reg(const i2c_dev_t *dev, uint8_t reg, uint8_t *val)
{
    return i2c_read_regs(dev, reg, val, 1);
}
