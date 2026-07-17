// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstring>

#include "pkg/pkg_macro.h"
#include "benchmark_publisher_module/benchmark_publisher_module.h"
#include "benchmark_subscriber_module/benchmark_subscriber_module.h"
#include "normal_publisher_module/normal_publisher_module.h"
#include "normal_subscriber_module/normal_subscriber_module.h"

using namespace ibmw::examples::cpp::ros2_chn;

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"NormalPublisherModule", []() -> ibmw::Module* {
       return new normal_publisher_module::NormalPublisherModule();
     }},
    {"NormalSubscriberModule", []() -> ibmw::Module* {
       return new normal_subscriber_module::NormalSubscriberModule();
     }},
    {"BenchmarkPublisherModule", []() -> ibmw::Module* {
       return new benchmark_publisher_module::BenchmarkPublisherModule();
     }},
    {"BenchmarkSubscriberModule", []() -> ibmw::Module* {
       return new benchmark_subscriber_module::BenchmarkSubscriberModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
