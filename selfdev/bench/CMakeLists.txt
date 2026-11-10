# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Performance comparison of the selfdev components against the third-party
# originals they replace. Standalone selfdev builds only; never part of IBMW.
#   cmake -S selfdev -B <dir> -DCMAKE_BUILD_TYPE=Release -DIBMW_BUILD_TESTS=OFF \
#         -DIBMW_SELFDEV_BUILD_BENCH=ON \
#         -DIBMW_SELFDEV_BENCH_TBB_SOURCE=<oneTBB src> \
#         -DIBMW_SELFDEV_BENCH_UNIFEX_SOURCE=<libunifex src> \
#         -DIBMW_SELFDEV_BENCH_BACKWARD_SOURCE=<backward-cpp src>

set(IBMW_SELFDEV_BENCH_TBB_SOURCE "" CACHE PATH "oneTBB source tree for the queue/hash map benchmarks.")
set(IBMW_SELFDEV_BENCH_UNIFEX_SOURCE "" CACHE PATH "libunifex source tree for the coro benchmark.")
set(IBMW_SELFDEV_BENCH_BACKWARD_SOURCE "" CACHE PATH "backward-cpp source tree for the stack benchmark.")

# Third-party targets: their own tests off, no -Werror, headers as SYSTEM so
# the selfdev warning set does not fire inside them.
function(ibmw_selfdev_bench_system target)
  if(TARGET ${target} AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
    set_property(TARGET ${target} PROPERTY SYSTEM ON)
  endif()
endfunction()

function(ibmw_selfdev_add_bench name)
  cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  ibmw_selfdev_warnings(${name})
endfunction()

if(IBMW_SELFDEV_CONCURRENT)
  if(IBMW_SELFDEV_BENCH_TBB_SOURCE)
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT OFF CACHE BOOL "" FORCE)
    set(TBB_INSTALL OFF CACHE BOOL "" FORCE)
    # IBMW links oneTBB statically (top-level BUILD_SHARED_LIBS OFF); do the
    # same so call overhead and allocator behaviour match.
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(${IBMW_SELFDEV_BENCH_TBB_SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/tbb EXCLUDE_FROM_ALL)
    ibmw_selfdev_bench_system(tbb)
    ibmw_selfdev_add_bench(ibmw_selfdev_queue_bench SRCS queue_bench.cc
                           LIBS ibmw::selfdev::concurrent TBB::tbb)
    ibmw_selfdev_add_bench(ibmw_selfdev_hash_map_bench SRCS hash_map_bench.cc
                           LIBS ibmw::selfdev::concurrent TBB::tbb)
  else()
    message(STATUS "selfdev bench: IBMW_SELFDEV_BENCH_TBB_SOURCE not set, skipping queue/hash map benchmarks")
  endif()
endif()

if(IBMW_EXECUTOR_USE_SELFDEV_CORO)
  if(IBMW_SELFDEV_BENCH_UNIFEX_SOURCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(UNIFEX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${IBMW_SELFDEV_BENCH_UNIFEX_SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/unifex EXCLUDE_FROM_ALL)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      # Same relaxation as IBMW's cmake/GetLibUnifex.cmake.
      target_compile_options(unifex PRIVATE -Wno-unused-but-set-variable)
    endif()
    ibmw_selfdev_bench_system(unifex)
    ibmw_selfdev_add_bench(ibmw_selfdev_coro_bench SRCS coro_bench.cc
                           LIBS ibmw::selfdev::coro unifex)
  else()
    message(STATUS "selfdev bench: IBMW_SELFDEV_BENCH_UNIFEX_SOURCE not set, skipping coro benchmark")
  endif()
endif()

if(IBMW_SELFDEV_CRASH_HANDLER)
  if(IBMW_SELFDEV_BENCH_BACKWARD_SOURCE)
    # backward-cpp is header only. Two variants: the resolver available
    # without extra libraries (backtrace_symbols / dladdr), and libdw, which
    # IBMW's backward build picks up when elfutils is installed.
    foreach(variant symbols dw)
      if(variant STREQUAL "dw")
        find_library(IBMW_SELFDEV_BENCH_LIBDW dw)
        find_path(IBMW_SELFDEV_BENCH_LIBDW_INCLUDE elfutils/libdw.h)
        if(NOT IBMW_SELFDEV_BENCH_LIBDW OR NOT IBMW_SELFDEV_BENCH_LIBDW_INCLUDE)
          message(STATUS "selfdev bench: libdw not found, skipping ibmw_selfdev_stack_bench_dw")
          continue()
        endif()
        set(_defs BACKWARD_HAS_DW=1 BACKWARD_HAS_BACKTRACE_SYMBOL=0)
        set(_libs ${IBMW_SELFDEV_BENCH_LIBDW})
        set(_name ibmw_selfdev_stack_bench_dw)
      else()
        set(_defs BACKWARD_HAS_DW=0 BACKWARD_HAS_BFD=0 BACKWARD_HAS_DWARF=0 BACKWARD_HAS_BACKTRACE_SYMBOL=1)
        set(_libs "")
        set(_name ibmw_selfdev_stack_bench)
      endif()
      ibmw_selfdev_add_bench(${_name} SRCS stack_bench.cc
                             LIBS ibmw::selfdev::crash_handler ${_libs})
      target_compile_definitions(${_name} PRIVATE BACKWARD_HAS_UNWIND=1 ${_defs})
      target_include_directories(${_name} SYSTEM PRIVATE ${IBMW_SELFDEV_BENCH_BACKWARD_SOURCE})
      # Real frames and dynamic symbols for dladdr / backtrace_symbols.
      set_target_properties(${_name} PROPERTIES ENABLE_EXPORTS ON)
      target_compile_options(${_name} PRIVATE -fno-omit-frame-pointer -fvisibility=default)
    endforeach()
  else()
    message(STATUS "selfdev bench: IBMW_SELFDEV_BENCH_BACKWARD_SOURCE not set, skipping stack benchmark")
  endif()
endif()
