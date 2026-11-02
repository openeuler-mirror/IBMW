# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# A user package built against an installed IBMW the way ibmw-template does
# (find_package(ibmw) + ibmw_add_pkg), whose module awaits selfdev coro
# Tasks, so it needs libibmw_selfdev_coro. Used by
# scripts/tests/test_installed_coro_package.sh.
cmake_minimum_required(VERSION 3.24)
project(ibmw_installed_coro_package LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(ibmw REQUIRED)

ibmw_add_pkg(coro_fixture_pkg SRCS coro_fixture_pkg.cc)
install(TARGETS coro_fixture_pkg LIBRARY DESTINATION lib)
