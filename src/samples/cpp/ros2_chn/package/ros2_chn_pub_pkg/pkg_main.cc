// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstring>

#include "pkg/pkg_macro.h"
#include "benchmark_publisher_module/benchmark_publisher_module.h"
#include "normal_publisher_module/normal_publisher_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"NormalPublisherModule", []() -> ibmw::Module* {
       return new ibmw::examples::cpp::ros2_chn::normal_publisher_module::NormalPublisherModule();
     }},
    {"BenchmarkPublisherModule", []() -> ibmw::Module* {
       return new ibmw::examples::cpp::ros2_chn::benchmark_publisher_module::BenchmarkPublisherModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
