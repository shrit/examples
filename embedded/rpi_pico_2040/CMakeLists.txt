cmake_minimum_required(VERSION 3.13...3.27)

include(pico_sdk_import.cmake)

project(RandomForest CXX C ASM)

set(PICO_CXX_ENABLE_EXCEPTIONS ON)
set(PICO_CXX_ENABLE_RTTI ON)
set(PICO_CXX_ENABLE_CXA_ATEXIT ON)

pico_sdk_init()

#include(CMake/Autodownload.cmake)
#include(CMake/ConfigureCrossCompile.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(OPENBLAS_TARGET arm)
#set(OPENBLAS_BINARY 64)
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--hash-style=gnu -Wl,--build-id=none")
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,norelro")
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nosys.specs")

# initialize the Raspberry Pi Pico SDK

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os -s -frtti -fdata-sections -ffunction-sections")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fomit-frame-pointer -fno-unwind-tables")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-asynchronous-unwind-tables -fvisibility=hidden")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fshort-enums -finline-small-functions")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -findirect-inlining -fno-common")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fmerge-all-constants -fno-ident")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-unroll-loops -fno-math-errno")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-protector -fexceptions")

##search_openblas(0.3.26)
#get_deps(https://github.com/xianyi/OpenBLAS/releases/download/v0.3.26/OpenBLAS-0.3.26.tar.gz OpenBLAS OpenBLAS-0.3.26.tar.gz)
    #if (NOT MSVC)
      #if (NOT EXISTS "${CMAKE_BINARY_DIR}/deps/OpenBLAS-${version}/libopenblas.a")
        #set(ENV{COMMON_OPT} "${CMAKE_OPENBLAS_FLAGS}") # Pass our flags to OpenBLAS
        #execute_process(COMMAND make TARGET=${OPENBLAS_TARGET} BINARY=${OPENBLAS_BINARY} HOSTCC=gcc CC=${CMAKE_C_COMPILER} FC=${CMAKE_FORTRAN_COMPILER} NO_SHARED=1
                        #WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/deps/OpenBLAS-${version})
      #endif()
      #file(GLOB OPENBLAS_LIBRARIES "${CMAKE_BINARY_DIR}/deps/OpenBLAS-${version}/libopenblas.a")
      #set(BLAS_openblas_LIBRARY ${OPENBLAS_LIBRARIES})
      #set(LAPACK_openblas_LIBRARY ${OPENBLAS_LIBRARIES}) 
      #set(BLA_VENDOR OpenBLAS)
      #set(BLAS_FOUND ON)
      #    endif()

#get_deps(https://files.mlpack.org/armadillo-12.6.5.tar.gz armadillo armadillo-12.6.5.tar.gz)
#set(ARMADILLO_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
##find_package(Armadillo REQUIRED)
## Include directories for the previous dependencies.
#set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} ${ARMADILLO_INCLUDE_DIRS})
#set(MLPACK_LIBRARIES ${MLPACK_LIBRARIES} ${ARMADILLO_LIBRARIES})

## Find stb_image.h and stb_image_write.h.
#get_deps(https://mlpack.org/files/stb.tar.gz stb stb.tar.gz)
#set(STB_IMAGE_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
#set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} "${STB_IMAGE_INCLUDE_DIR}")

## Find ensmallen.
#get_deps(https://www.ensmallen.org/files/ensmallen-latest.tar.gz ensmallen ensmallen-latest.tar.gz)
#set(ENSMALLEN_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
#set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} "${ENSMALLEN_INCLUDE_DIR}")

## Find cereal.
#get_deps(https://github.com/USCiLab/cereal/archive/refs/tags/v1.3.0.tar.gz cereal cereal-1.3.0.tar.gz)
#set(CEREAL_INCLUDE_DIR ${GENERIC_INCLUDE_DIR})
#set(MLPACK_INCLUDE_DIRS ${MLPACK_INCLUDE_DIRS} ${CEREAL_INCLUDE_DIR})


include_directories(BEFORE ${MLPACK_INCLUDE_DIRS})
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/src/)

# Finally, add any cross-compilation support libraries (they may need to come
# last).  If we are not cross-compiling, no changes will happen here.
set(MLPACK_LIBRARIES ${MLPACK_LIBRARIES} ${CROSS_COMPILE_SUPPORT_LIBRARIES})

add_definitions(-DMLPACK_NO_STD_MUTEX)

add_executable(main
    main.cpp
)

target_include_directories(main PRIVATE
  ${MLPACK_INCLUDE_DIRS}
  /meta/mlpack/src/
  /meta/ensmallen/include/
  deps/armadillo-12.6.5/include/
  deps/cereal-1.3.0/include/ 
)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(main
  pico_stdlib
  /meta/mlpack_examples/embedded/rpi_pico_2040/OpenBLAS-0.3.28/build/lib/libopenblas.a
)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(main)
