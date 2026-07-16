// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for ros2_sub_only_app.

#include "pkg/pkg_macro.h"

#include "subscriber_module/subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"SubscriberModule", []() -> ibmw::Module* {
           return new ros2_interop_demo::SubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
