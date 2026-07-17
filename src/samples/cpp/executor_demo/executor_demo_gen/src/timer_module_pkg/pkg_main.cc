// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for executor_timer_app.

#include "pkg/pkg_macro.h"

#include "timer_module/timer_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"TimerModule", []() -> ibmw::Module* {
           return new executor_demo::TimerModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
