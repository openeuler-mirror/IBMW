// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for ros2_pubsub_app.

#include "pkg/pkg_macro.h"

#include "ros2_publisher_module/ros2_publisher_module.h"
#include "ros2_subscriber_module/ros2_subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"Ros2PublisherModule", []() -> ibmw::Module* {
           return new my_project::Ros2PublisherModule();
         }},
        {"Ros2SubscriberModule", []() -> ibmw::Module* {
           return new my_project::Ros2SubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
