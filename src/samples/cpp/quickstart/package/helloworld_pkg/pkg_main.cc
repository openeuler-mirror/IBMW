// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "pkg/pkg_macro.h"
#include "helloworld_module/helloworld_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"HelloWorldModule", []() -> ibmw::Module* {
       return new ibmw::examples::cpp::helloworld::helloworld_module::HelloWorldModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
