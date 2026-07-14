# This file adds the necessary configurations to cross compile
# mlpack for embedded systems. You need to set the following variables
# from the command line: CMAKE_SYSROOT and TOOLCHAIN_PREFIX.
# This file will compile OpenBLAS if it is downloaded and it is not
# available on your system in order to find the BLAS library.  If OpenBLAS will
# be compiled, the OPENBLAS_TARGET variable must be set.  This can be done
# by, e.g., setting ARCH_NAME (which will set OPENBLAS_TARGET in
# `flags-config.cmake`).

# Apply a list of patch files to an OpenBLAS source tree before it is built.
# `srcDir` is the unpacked OpenBLAS directory; `patches` is a ;-list of patch
# files (each `patch -p1`-compatible).  Applying is idempotent: a patch that is
# already applied (detected via a reverse dry-run) is skipped, so re-running
# CMake against an existing build tree is safe.  A patch that neither applies
# cleanly nor is already applied is a hard error -- unlike an in-place `sed`, it
# never silently does nothing when the upstream source has changed.
function(apply_openblas_patches srcDir patches)
  find_program(PATCH_EXECUTABLE patch)
  if(NOT PATCH_EXECUTABLE)
    message(FATAL_ERROR "The 'patch' tool is required to apply OPENBLAS_PATCHES "
                        "but was not found on PATH.")
  endif()

  foreach(patchFile IN LISTS patches)
    if(NOT IS_ABSOLUTE "${patchFile}")
      set(patchFile "${CMAKE_CURRENT_LIST_DIR}/${patchFile}")
    endif()
    if(NOT EXISTS "${patchFile}")
      message(FATAL_ERROR "OpenBLAS patch not found: ${patchFile}")
    endif()

    get_filename_component(patchName "${patchFile}" NAME)

    # Already applied?  `patch -R --dry-run` succeeds only if the reverse patch
    # would apply, i.e. the forward patch is already in place.
    execute_process(
        COMMAND ${PATCH_EXECUTABLE} -p1 -R --dry-run --force
                --input=${patchFile}
        WORKING_DIRECTORY ${srcDir}
        RESULT_VARIABLE alreadyApplied
        OUTPUT_QUIET ERROR_QUIET)
    if(alreadyApplied EQUAL 0)
      message(STATUS "OpenBLAS patch already applied, skipping: ${patchName}")
      continue()
    endif()

    execute_process(
        COMMAND ${PATCH_EXECUTABLE} -p1 --forward --input=${patchFile}
        WORKING_DIRECTORY ${srcDir}
        RESULT_VARIABLE patchResult
        OUTPUT_VARIABLE patchOutput ERROR_VARIABLE patchOutput)
    if(NOT patchResult EQUAL 0)
      message(FATAL_ERROR
          "Failed to apply OpenBLAS patch ${patchName}:\n${patchOutput}")
    endif()
    message(STATUS "Applied OpenBLAS patch: ${patchName}")
  endforeach()
endfunction()

if (CMAKE_CROSSCOMPILING)
  include(CMake/crosscompile-arch-config.cmake)
  if (NOT CMAKE_SYSROOT AND (NOT TOOLCHAIN_PREFIX))
    message(FATAL_ERROR "Neither CMAKE_SYSROOT nor TOOLCHAIN_PREFIX are set; please set both of them and try again.")
  elseif(NOT CMAKE_SYSROOT)
    message(FATAL_ERROR "Cannot configure: CMAKE_SYSROOT must be set when performing cross-compiling!")
  elseif(NOT TOOLCHAIN_PREFIX)
    message(FATAL_ERROR "Cannot configure: TOOLCHAIN_PREFIX must be set when performing cross-compiling!")
  endif()

  # Now make sure that we can still compile a simple test program.
  # (This ensures we didn't add any bad CXXFLAGS.)
  # Note that OUTPUT_VARIABLE is only available in newer versions of CMake!
  # CMake 3.23 (silently) introduced the variable.
  include(CheckCXXSourceCompiles)
  if (CMAKE_VERSION VERSION_LESS "3.22.0")
    check_cxx_source_compiles("int main() { return 0; }" COMPILE_SUCCESS)
    if (NOT COMPILE_SUCCESS)
      message(FATAL_ERROR "The C++ cross-compiler at ${CMAKE_CXX_COMPILER} is "
        "not able to compile a trivial test program.  Check the CXXFLAGS!")
    endif ()
  else ()
    check_cxx_source_compiles("int main() { return 0; }" COMPILE_SUCCESS
        OUTPUT_VARIABLE COMPILE_OUTPUT)
    if (NOT COMPILE_SUCCESS)
      message(FATAL_ERROR "The C++ cross-compiler at ${CMAKE_CXX_COMPILER} is "
        "not able to compile a trivial test program.  Compiler output:\n\n"
        "${COMPILE_OUTPUT}")
    endif ()
  endif ()
endif()

macro(search_openblas version)
  set(BLA_STATIC ON)
  find_package(BLAS)
  if (NOT BLAS_FOUND OR (NOT BLAS_LIBRARIES))
    if(NOT OPENBLAS_TARGET)
      message(FATAL_ERROR "Cannot compile OpenBLAS: OPENBLAS_TARGET is not set.  Either set that variable, or set BOARD_NAME correctly!")
    endif()
    get_deps(https://github.com/xianyi/OpenBLAS/releases/download/v${version}/OpenBLAS-${version}.tar.gz OpenBLAS OpenBLAS-${version}.tar.gz)
    if (NOT MSVC)
      if (NOT EXISTS "${CMAKE_BINARY_DIR}/deps/OpenBLAS-${version}/libopenblas.a")
        set(ENV{COMMON_OPT} "${CMAKE_OPENBLAS_FLAGS}") # Pass our flags to OpenBLAS

        # NN-on-device memory fit (riscv64).  OpenBLAS lazily allocates a per-GEMM
        # scratch buffer (BUFFER_SIZE -- 32 MB on riscv64) sized for its default
        # N-block (SGEMM_DEFAULT_R = 12288).  That single 32 MB allocation does
        # not fit on a ~28 MB device, so the first f32 matrix-multiply -- e.g. the
        # neural network's dense layers -- is OOM-killed at startup.  (Random
        # forest and KNN avoid that big GEMM path, which is why only the NN
        # failed.)  The fix is a patch that shrinks the N-block to 2048 and the
        # buffer to 8 MB; see CMake/patches/openblas-riscv64-low-memory.patch and
        # README.md / BINARY_SIZE.md for the full investigation.
        #
        # For the generic riscv64 target we apply that patch by default; a caller
        # can override or extend the list with -DOPENBLAS_PATCHES="a.patch;b.patch"
        # (absolute paths, or relative to this CMake/ directory).
        if(NOT DEFINED OPENBLAS_PATCHES AND OPENBLAS_TARGET STREQUAL "RISCV64_GENERIC")
          set(OPENBLAS_PATCHES
              "${CMAKE_CURRENT_LIST_DIR}/patches/openblas-riscv64-low-memory.patch")
        endif()
        apply_openblas_patches("${CMAKE_BINARY_DIR}/deps/OpenBLAS-${version}"
                               "${OPENBLAS_PATCHES}")
        # USE_THREAD=0 / NUM_THREADS=1 / USE_OPENMP=0: build a single-threaded
        # OpenBLAS.  On a single-core, 64 MB target (Milk-V Duo) the threaded
        # build spawns worker threads that busy-wait (spin) at startup, which
        # starves the main thread on one core and hangs the program before it
        # even runs.  Single-threaded also removes the per-thread GEMM buffers.
        execute_process(COMMAND make TARGET=${OPENBLAS_TARGET} BINARY=${OPENBLAS_BINARY} HOSTCC=gcc CC=${CMAKE_C_COMPILER} FC=${CMAKE_FORTRAN_COMPILER} NO_SHARED=1 USE_THREAD=0 NUM_THREADS=1 USE_OPENMP=0
                        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/deps/OpenBLAS-${version})
      endif()
      file(GLOB OPENBLAS_LIBRARIES "${CMAKE_BINARY_DIR}/deps/OpenBLAS-${version}/libopenblas.a")
      set(BLAS_openblas_LIBRARY ${OPENBLAS_LIBRARIES})
      set(LAPACK_openblas_LIBRARY ${OPENBLAS_LIBRARIES}) 
      set(BLA_VENDOR OpenBLAS)
      set(BLAS_FOUND ON)
    endif()
  endif()
  find_library(GFORTRAN NAMES libgfortran.a)
  find_library(PTHREAD NAMES libpthread.a)
  set(CROSS_COMPILE_SUPPORT_LIBRARIES ${GFORTRAN} ${PTHREAD})
endmacro()
