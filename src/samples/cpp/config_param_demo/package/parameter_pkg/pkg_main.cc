// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "pkg/pkg_macro.h"
#include "parameter_module/parameter_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"ParameterModule", []() -> ibmw::Module* {
       return new ibmw::examples::cpp::parameter::parameter_module::ParameterModule();
     }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
