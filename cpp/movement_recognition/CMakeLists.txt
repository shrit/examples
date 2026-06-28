# Cross-compilation build for the `train` binary.
#
# Training runs on the device, so this reuses the repo's embedded cross-compile
# machinery (CMake/, copied from embedded/embedded_cmake_template) to fetch
# mlpack + its dependencies and build a static binary for the Milk-V Duo.
#
# Milk-V Duo (SOPHGO CV1800B, RISC-V C906, 64 MB) with a Bootlin riscv64-lp64d
# musl toolchain:
#   cmake -DCMAKE_CROSSCOMPILE=ON -DARCH_NAME=C906 \
#         -DCMAKE_TOOLCHAIN_FILE=../CMake/crosscompile-toolchain.cmake \
#         -DTOOLCHAIN_PREFIX=/path/to/.../bin/riscv64-buildroot-linux-musl- \
#         -DCMAKE_SYSROOT=/path/to/.../riscv64-buildroot-linux-musl/sysroot ..
#   make
#
# For a native host build, prefer train/Makefile.  (collect and the driver's
# imu_test build with their own Makefiles -- no mlpack, no CMake.)

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

# Download + configure mlpack and its deps (armadillo, ensmallen, cereal,
# OpenBLAS, built single-threaded -- see ConfigureCrossCompile.cmake).
fetch_mlpack(ON)

# This example only loads CSVs, never images, so compile out mlpack's image
# (STB) support to shrink the binary.  This is mlpack's `-DDISABLE_STB=ON` build
# option; for a header-only consumer like us it is the MLPACK_DISABLE_STB define
# (see mlpack's config.hpp).  Harmless no-op on an mlpack that predates the
# switch; saves ~50-80 KB once it is present.
add_compile_definitions(MLPACK_DISABLE_STB)

add_executable(train train/train.cpp)

target_include_directories(train SYSTEM PRIVATE
  ${MLPACK_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Static link.  Link the cross-built OpenBLAS explicitly (+ its gfortran/pthread
# /m runtime): when Armadillo is fetched in "wrapper" mode, MLPACK_LIBRARIES can
# come out without BLAS, which the FFN / matrix paths need (dgemm_, ddot_, ...).
file(GLOB TRAIN_OPENBLAS "${CMAKE_BINARY_DIR}/deps/OpenBLAS-*/libopenblas.a")

target_link_libraries(train PRIVATE -static
  ${MLPACK_LIBRARIES}
  ${TRAIN_OPENBLAS}
  gfortran
  pthread
  m
)

# The inference tool talks to the IMU (so it links the self-contained driver/)
# and is always built with the ncurses TUI.  ncurses is not part of the cross
# toolchain, so it is located in one of two ways:
#
#   1. Preferred for cross builds -- point NCURSES_PREFIX at a target (RISC-V)
#      ncurses you cross-built once (see the "ncurses TUI" note in the README):
#         cmake ... -DNCURSES_PREFIX=/path/to/ncurses-riscv64
#   2. Otherwise we try to find a system/sysroot ncursesw automatically.
#
# If neither is available, `infer` is simply skipped (train still builds) with a
# hint, instead of failing the whole configure on a hard-coded path.
set(NCURSES_PREFIX "" CACHE PATH
    "target ncurses install prefix for the infer TUI (see README)")

set(INFER_NCURSES_INCLUDE "")
set(INFER_NCURSES_LIB "")
if(NCURSES_PREFIX)
  set(INFER_NCURSES_INCLUDE
      ${NCURSES_PREFIX}/include ${NCURSES_PREFIX}/include/ncursesw)
  set(INFER_NCURSES_LIB ${NCURSES_PREFIX}/lib/libncursesw.a)
else()
  find_path(NCURSESW_INCLUDE_DIR ncurses.h PATH_SUFFIXES ncursesw)
  find_library(NCURSESW_LIBRARY NAMES ncursesw)
  if(NCURSESW_INCLUDE_DIR AND NCURSESW_LIBRARY)
    set(INFER_NCURSES_INCLUDE ${NCURSESW_INCLUDE_DIR})
    set(INFER_NCURSES_LIB ${NCURSESW_LIBRARY})
  endif()
endif()

if(INFER_NCURSES_LIB)
  add_executable(infer
    infer/infer.cpp
    driver/imu.cpp
    driver/l3gd20h.cpp
    driver/lsm303d.cpp
    driver/bmp180.cpp
    driver/i2c_bus.cpp
    driver/mag_calibration.cpp
  )

  target_include_directories(infer SYSTEM PRIVATE
    ${MLPACK_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${INFER_NCURSES_INCLUDE}
  )

  target_link_libraries(infer PRIVATE -static
    ${MLPACK_LIBRARIES}
    ${TRAIN_OPENBLAS}
    ${INFER_NCURSES_LIB}
    gfortran
    pthread
    m
  )
else()
  message(STATUS
    "infer: ncurses not found -- skipping (train still builds).  Set "
    "-DNCURSES_PREFIX=/path/to/ncurses-riscv64 to enable it; see the README.")
endif()
