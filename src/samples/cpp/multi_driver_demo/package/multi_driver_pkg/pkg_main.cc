// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "pkg/pkg_macro.h"
#include "multi_driver_module/multi_driver_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"MultiDriverModule", []() -> ibmw::Module* {
       return new ibmw::examples::cpp::multi_driver_demo::multi_driver_module::MultiDriverModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
