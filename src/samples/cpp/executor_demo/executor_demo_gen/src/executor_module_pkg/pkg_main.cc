// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for executor_app.

#include "pkg/pkg_macro.h"

#include "executor_module/executor_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"ExecutorModule", []() -> ibmw::Module* {
           return new executor_demo::ExecutorModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
