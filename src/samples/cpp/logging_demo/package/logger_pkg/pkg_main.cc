// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "pkg/pkg_macro.h"
#include "logger_bench_module/logger_bench_module.h"
#include "logger_module/logger_module.h"

using namespace ibmw::examples::cpp::logger;
static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"LoggerModule", []() -> ibmw::Module* { return new logger_module::LoggerModule(); }},
    {"LoggerBenchModule", []() -> ibmw::Module* { return new logger_bench_module::LoggerBenchModule(); }}};
IBMW_PKG_MAIN(ibmw_module_register_array)
