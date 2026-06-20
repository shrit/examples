# Makefile for the data-collection tool (size-optimized, dependency-light).
#
# `collect` builds into a small static binary suitable for a 64 MB
# device.  The flags below minimize size:
#   -Os                      optimize for size
#   -fno-exceptions -fno-rtti drop exception tables and RTTI
#   -ffunction-sections -fdata-sections + --gc-sections  drop unused sections
#   -flto                    cross-module size optimization
#   -s                       strip symbols
#
# Optional interactive ncurses TUI: build with `make NCURSES=1` if the toolchain
# provides ncurses (the Milk-V Duo's RISC-V toolchain does).  Without it, collect
# is command-line only and links nothing extra.
#
# Host build:   make                 (CLI only)
#               make NCURSES=1        (with the --tui front-end)
# Cross-build:  make CXX=/path/to/riscv64-buildroot-linux-musl-g++ STATIC=-static
# Clean:        make clean

TARGET := collect

SRC := \
	collect_main.cpp \
	../driver/imu.cpp \
	../driver/l3gd20h.cpp \
	../driver/lsm303d.cpp \
	../driver/bmp180.cpp \
	../driver/i2c_bus.cpp \
	../driver/mag_calibration.cpp

CXX := g++
CXXFLAGS += -std=c++17 -Wall -Wextra -Os \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -flto
# STATIC=-static (recommended for the device); empty for a host build.
STATIC ?=
LDFLAGS += $(STATIC) -flto -s -Wl,--gc-sections -pthread
LIBS +=

# Enable the ncurses TUI when requested (make NCURSES=1).
# For a host build the system ncurses is found automatically.  For a cross build
# point at a target ncurses via NCURSES_CFLAGS / NCURSES_LIBS, e.g.
#   make NCURSES=1 \
#        NCURSES_CFLAGS="-isystem /path/to/ncurses-riscv64/include" \
#        NCURSES_LIBS="-L/path/to/ncurses-riscv64/lib -lncurses"
NCURSES ?=
NCURSES_CFLAGS ?=
NCURSES_LIBS ?= -lncurses
ifeq ($(NCURSES),1)
  CXXFLAGS += -DUSE_NCURSES $(NCURSES_CFLAGS)
  LIBS += $(NCURSES_LIBS)
endif

CLEAN_LIST := $(TARGET)

default: all

.PHONY: all
all: $(TARGET)

# Single-invocation build: every source is compiled together with the size
# flags above (and LTO across all of them), and no intermediate .o files are
# left in the shared driver/common directories.
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS) $(LIBS)

.PHONY: clean
clean:
	@echo CLEAN $(CLEAN_LIST)
	@rm -f $(CLEAN_LIST)
