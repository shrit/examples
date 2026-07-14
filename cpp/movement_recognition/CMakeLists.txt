# Build for the movement-recognition example: one CMake project, four programs.
#
#   imu_test  -- sensor check + magnetometer calibration (no mlpack)
#   collect   -- record sensor data to CSV               (no mlpack)
#   train     -- train a neural network from the CSVs    (mlpack)
#   infer     -- live inference from the IMU             (mlpack)
#
# `imu_test` and `collect` only need the Linux I2C headers; they are compiled
# exception-free and size-optimized into small static binaries.  `train` and
# `infer` link mlpack, so this project fetches mlpack + its dependencies and
# cross-compiles OpenBLAS via the CMake/ machinery (see ConfigureCrossCompile).
#
# Host build (your computer):
#   cmake .. && make
#
# Cross-compile for the Milk-V Duo (SOPHGO CV1800B, RISC-V C906, 64 MB) with a
# Bootlin riscv64-lp64d musl toolchain:
#   cmake -DCMAKE_CROSSCOMPILING=ON -DARCH_NAME=RV64GCV \
#         -DCMAKE_TOOLCHAIN_FILE=../CMake/crosscompile-toolchain.cmake \
#         -DTOOLCHAIN_PREFIX=/path/to/.../bin/riscv64-buildroot-linux-musl- \
#         -DCMAKE_SYSROOT=/path/to/.../riscv64-buildroot-linux-musl/sysroot ..
#   make
#
# Build a single program with, e.g., `make collect` or `make train`.

cmake_minimum_required(VERSION 3.11)
project(movement_recognition CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CMake/ConfigureCrossCompile.cmake)
include(CMake/mlpack.cmake)

# The Duo is effectively single-core for this workload; OpenMP buys nothing and
# its runtime spawns spinning worker threads that starve the program at startup.
# Disable it AFTER include(mlpack.cmake) (which resets the flag) and BEFORE
# fetch_mlpack() (which runs find_package(OpenMP)).
set(MLPACK_DISABLE_OPENMP ON)

# This example only loads CSVs, never images, so compile out mlpack's image
# (STB) support to shrink the binary (~50-80 KB).  MLPACK_DISABLE_STB is one of
# the footprint options mlpack.cmake exposes; setting it here (or on the command
# line, -DMLPACK_DISABLE_STB=ON) makes fetch_mlpack() apply the compile
# definition for us.  Harmless no-op on an mlpack that predates the switch.
set(MLPACK_DISABLE_STB ON)

# Download + configure mlpack and its deps (armadillo, ensmallen, cereal,
# OpenBLAS, built single-threaded -- see ConfigureCrossCompile.cmake).
fetch_mlpack(ON)

# Link statically for the device (so nothing needs to be installed on the Duo);
# a host build links normally, which needs no static system libraries.
if(CMAKE_CROSSCOMPILING)
  set(LINK_STATIC -static)
endif()

# The GY-89 driver is self-contained: the I2C wrapper and the three chip drivers.
# The magnetometer calibration and the SensorBoard that composes everything into
# one object are small enough to be header-only (see their .hpp files), so they
# need no entry here.  Every program that talks to the sensor compiles these in.
set(DRIVER_SOURCES
  driver/imu.cpp
  driver/l3gd20h.cpp
  driver/lsm303d.cpp
  driver/bmp180.cpp
  driver/i2c_bus.cpp
)

# --- imu_test and collect: tiny, mlpack-free, exception-free ----------------
# Built for size (no exceptions/RTTI, dead-section stripped) so they stay in the
# 150-170 KB range on the device.  They do not depend on mlpack/Armadillo.
add_executable(imu_test driver/imu_test.cpp ${DRIVER_SOURCES})
add_executable(collect  collect/collect_main.cpp ${DRIVER_SOURCES})

foreach(tool imu_test collect)
  target_compile_options(${tool} PRIVATE
    -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
  target_link_libraries(${tool} PRIVATE ${LINK_STATIC}
    -Wl,--gc-sections
    pthread)   # std::this_thread::sleep_* pulls in pthread
endforeach()

# --- train and infer: link mlpack -------------------------------------------
# When cross-compiling, link the cross-built OpenBLAS explicitly, plus its
# gfortran/m runtime: Armadillo is fetched in "wrapper" mode, so MLPACK_LIBRARIES
# can come out without BLAS, which the FFN / matrix paths need (dgemm_, ddot_,
# ...).  On a host build MLPACK_LIBRARIES already carries the system BLAS.
if(CMAKE_CROSSCOMPILING)
  file(GLOB OPENBLAS_LIB "${CMAKE_BINARY_DIR}/deps/OpenBLAS-*/libopenblas.a")
  set(MLPACK_EXTRA_LIBS ${OPENBLAS_LIB} gfortran m)
endif()

add_executable(train train/train.cpp)
target_include_directories(train SYSTEM PRIVATE ${MLPACK_INCLUDE_DIRS})
target_link_libraries(train PRIVATE ${LINK_STATIC}
  ${MLPACK_LIBRARIES}
  ${MLPACK_EXTRA_LIBS}
  pthread
)

# The inference tool talks to the IMU, so it links the driver alongside mlpack;
# it prints predictions to stdout.
add_executable(infer infer/infer.cpp ${DRIVER_SOURCES})
target_include_directories(infer SYSTEM PRIVATE ${MLPACK_INCLUDE_DIRS})
target_link_libraries(infer PRIVATE ${LINK_STATIC}
  ${MLPACK_LIBRARIES}
  ${MLPACK_EXTRA_LIBS}
  pthread
)
