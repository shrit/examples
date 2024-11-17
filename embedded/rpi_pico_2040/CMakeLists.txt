cmake_minimum_required(VERSION 3.13...3.27)
project(RandomForest)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)
include(CMake/Autodownload.cmake)
include(CMake/ConfigureCrossCompile.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

search_openblas(0.3.26)

get_deps(https://files.mlpack.org/armadillo-12.6.5.tar.gz armadillo armadillo-12.6.5.tar.gz)
set(ARMADILLO_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
find_package(Armadillo REQUIRED)
# Include directories for the previous dependencies.
set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} ${ARMADILLO_INCLUDE_DIRS})
set(MLPACK_LIBRARIES ${MLPACK_LIBRARIES} ${ARMADILLO_LIBRARIES})

# Find stb_image.h and stb_image_write.h.
get_deps(https://mlpack.org/files/stb.tar.gz stb stb.tar.gz)
set(STB_IMAGE_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} "${STB_IMAGE_INCLUDE_DIR}")

# Find ensmallen.
get_deps(https://www.ensmallen.org/files/ensmallen-latest.tar.gz ensmallen ensmallen-latest.tar.gz)
set(ENSMALLEN_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} "${ENSMALLEN_INCLUDE_DIR}")

# Find cereal.
get_deps(https://github.com/USCiLab/cereal/archive/refs/tags/v1.3.0.tar.gz cereal cereal-1.3.0.tar.gz)
set(CEREAL_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} ${CEREAL_INCLUDE_DIR})


include_directories(BEFORE ${MLPACK_INCLUDE_DIRS})
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/src/)

# Finally, add any cross-compilation support libraries (they may need to come
# last).  If we are not cross-compiling, no changes will happen here.
set(MLPACK_LIBRARIES ${MLPACK_LIBRARIES} ${CROSS_COMPILE_SUPPORT_LIBRARIES})

add_executable(main
    main.cpp
)

target_include_directories(main PRIVATE
  ${MLPACK_INCLUDE_DIRS}
  /meta/mlpack/src/
)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(main

  pico_stdlib

)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(main)
