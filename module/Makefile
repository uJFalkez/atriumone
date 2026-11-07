CC      ?= gcc
CFLAGS  ?= -O2 -pipe
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Idrivers -Icalibration -Inav
LDLIBS  += -lcjson -lm -lpthread

BUILD   := build
LIB_SRC := drivers/i2c.c drivers/mpu6500.c drivers/qmc5883p.c drivers/calib.c calibration/linalg.c \
           nav/attitude.c nav/kf.c nav/gps.c nav/frames.c nav/motion.c nav/calsession.c
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
LIB     := $(BUILD)/libsensors.a

TOOLS   := $(BUILD)/sensor_monitor $(BUILD)/calib_gyro $(BUILD)/calib_accel \
           $(BUILD)/calib_mag $(BUILD)/test_fit $(BUILD)/test_nav $(BUILD)/navd $(BUILD)/camgrab

all: $(LIB) $(TOOLS)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/sensor_monitor: $(BUILD)/tools/sensor_monitor.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/calib_%: $(BUILD)/calibration/calib_%.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_fit: $(BUILD)/calibration/test_fit.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/camgrab: $(BUILD)/cam/camgrab.o
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/navd: $(BUILD)/nav/navd.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_nav: $(BUILD)/nav/test_nav.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

test: $(BUILD)/test_fit $(BUILD)/test_nav
	$(BUILD)/test_fit
	$(BUILD)/test_nav

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

.PHONY: all clean test
.SECONDARY:
