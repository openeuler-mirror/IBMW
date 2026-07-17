// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for executor_co_loop_app.

#include "pkg/pkg_macro.h"

#include "executor_co_loop_module/executor_co_loop_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"ExecutorCoLoopModule", []() -> ibmw::Module* {
           return new executor_demo::ExecutorCoLoopModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
