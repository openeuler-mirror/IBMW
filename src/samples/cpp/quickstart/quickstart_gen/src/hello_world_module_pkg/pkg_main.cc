// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for helloworld_app.

#include "pkg/pkg_macro.h"

#include "hello_world_module/hello_world_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"HelloWorldModule", []() -> ibmw::Module* {
           return new ibmw::examples::cpp::helloworld::HelloWorldModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
