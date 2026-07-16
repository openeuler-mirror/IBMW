// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for executor_real_time_app.

#include "pkg/pkg_macro.h"

#include "real_time_module/real_time_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"RealTimeModule", []() -> ibmw::Module* {
           return new executor_demo::RealTimeModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
