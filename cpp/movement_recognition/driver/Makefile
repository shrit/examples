# Makefile for the standalone, self-contained GY-89 driver test (imu_test).
#
# This directory depends on nothing outside it: the sensor drivers, the I2C
# wrapper, the magnetometer calibration and the test program are all here, so
# the IMU can be built and checked on its own.  Built small (no exceptions/RTTI,
# size-optimized, dead-section stripped) for the device.
#
# Host build:   make
# Cross-build:  make CXX=/path/to/riscv64-buildroot-linux-musl-g++ STATIC=-static
# Clean:        make clean

TARGET := imu_test

SRC := \
	imu_test.cpp \
	imu.cpp \
	l3gd20h.cpp \
	lsm303d.cpp \
	bmp180.cpp \
	i2c_bus.cpp \
	mag_calibration.cpp

CXX := g++
CXXFLAGS += -std=c++17 -Wall -Wextra -Os \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -flto
# STATIC=-static (recommended for the device); empty for a host build.
STATIC ?=
# pthread is pulled in by std::this_thread::sleep_until / sleep_for.
LDFLAGS += $(STATIC) -flto -s -Wl,--gc-sections -pthread

CLEAN_LIST := $(TARGET)

default: all

.PHONY: all
all: $(TARGET)

# Single-invocation build: every source compiled together with the size flags
# (and LTO), no intermediate .o files left behind.
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

.PHONY: clean
clean:
	@echo CLEAN $(CLEAN_LIST)
	@rm -f $(CLEAN_LIST)
