// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "pkg/pkg_macro.h"
#include "executor_co_loop_module/executor_co_loop_module.h"
#include "executor_co_module/executor_co_module.h"
#include "executor_module/executor_module.h"
#include "real_time_module/real_time_module.h"
#include "timer_module/timer_module.h"

using namespace ibmw::examples::cpp::executor;
static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"ExecutorModule", []() -> ibmw::Module* { return new executor_module::ExecutorModule(); }},
    {"ExecutorCoModule", []() -> ibmw::Module* { return new executor_co_module::ExecutorCoModule(); }},
    {"ExecutorCoLoopModule", []() -> ibmw::Module* { return new executor_co_loop_module::ExecutorCoLoopModule(); }},
    {"RealTimeModule", []() -> ibmw::Module* { return new real_time_module::RealTimeModule(); }},
    {"TimerModule", []() -> ibmw::Module* { return new timer_module::TimerModule(); }}};
IBMW_PKG_MAIN(ibmw_module_register_array)
