// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for logging_app.

#include "pkg/pkg_macro.h"

#include "logger_module/logger_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"LoggerModule", []() -> ibmw::Module* {
           return new logging_demo::LoggerModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
