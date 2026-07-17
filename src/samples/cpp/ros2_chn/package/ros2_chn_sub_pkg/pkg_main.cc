// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstring>

#include "pkg/pkg_macro.h"
#include "benchmark_subscriber_module/benchmark_subscriber_module.h"
#include "normal_subscriber_module/normal_subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"NormalSubscriberModule", []() -> ibmw::Module* {
       return new ibmw::examples::cpp::ros2_chn::normal_subscriber_module::NormalSubscriberModule();
     }},
    {"BenchmarkSubscriberModule", []() -> ibmw::Module* {
       return new ibmw::examples::cpp::ros2_chn::benchmark_subscriber_module::BenchmarkSubscriberModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
