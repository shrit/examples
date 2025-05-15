cmake_minimum_required(VERSION 3.13...3.27)

include(pico_sdk_import.cmake)

project(RandomForest CXX C ASM)

include(CMake/ConfigureCrossCompile.cmake)
include(CMake/mlpack.cmake)

set(PICO_CXX_ENABLE_EXCEPTIONS ON)
set(PICO_CXX_ENABLE_RTTI ON)
set(PICO_CXX_ENABLE_CXA_ATEXIT ON)

pico_sdk_init()

fetch_mlpack(OFF)

add_definitions(-DMLPACK_NO_STD_MUTEX)

add_executable(main
    main.cpp
)

target_include_directories(main PRIVATE
  ${MLPACK_INCLUDE_DIRS}
)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(main
  pico_stdlib
  /meta/mlpack_examples/embedded/rpi_pico_2040/OpenBLAS-0.3.28/build/lib/libopenblas.a
)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(main)
