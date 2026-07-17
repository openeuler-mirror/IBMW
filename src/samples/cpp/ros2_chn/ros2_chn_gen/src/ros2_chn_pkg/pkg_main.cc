// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for ros2_chn_app.

#include "pkg/pkg_macro.h"

#include "normal_publisher_module/normal_publisher_module.h"
#include "normal_subscriber_module/normal_subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"NormalPublisherModule", []() -> ibmw::Module* {
           return new ros2_chn_demo::NormalPublisherModule();
         }},
        {"NormalSubscriberModule", []() -> ibmw::Module* {
           return new ros2_chn_demo::NormalSubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
