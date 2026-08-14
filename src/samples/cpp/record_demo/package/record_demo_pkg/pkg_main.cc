// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "pkg/pkg_macro.h"
#include "cdr_publisher_module/cdr_publisher_module.h"
#include "ros2_publisher_module/ros2_publisher_module.h"

using CdrPublisherModule =
    ibmw::examples::cpp::record_demo::cdr_publisher_module::CdrPublisherModule;
using Ros2PublisherModule =
    ibmw::examples::cpp::record_demo::ros2_publisher_module::Ros2PublisherModule;

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"CdrPublisherModule", []() -> ibmw::Module* {
           return new CdrPublisherModule();
         }},
        {"Ros2PublisherModule", []() -> ibmw::Module* {
           return new Ros2PublisherModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
