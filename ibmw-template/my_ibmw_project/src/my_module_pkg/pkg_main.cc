// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for my_app.

#include "pkg/pkg_macro.h"

#include "my_module/my_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"MyModule", []() -> ibmw::Module* {
           return new my_project::MyModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
