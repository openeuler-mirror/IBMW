// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for ros2_point_app.

#include "pkg/pkg_macro.h"

#include "point_publisher_module/point_publisher_module.h"
#include "point_subscriber_module/point_subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PointPublisherModule", []() -> ibmw::Module* {
           return new ros2_point_demo::PointPublisherModule();
         }},
        {"PointSubscriberModule", []() -> ibmw::Module* {
           return new ros2_point_demo::PointSubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
