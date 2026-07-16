// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for dds_image_app.

#include "pkg/pkg_macro.h"

#include "image_publisher_module/image_publisher.h"
#include "image_subscriber_module/image_subscriber.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"ImagePublisher", []() -> ibmw::Module* {
           return new dds_image_demo::ImagePublisher();
         }},
        {"ImageSubscriber", []() -> ibmw::Module* {
           return new dds_image_demo::ImageSubscriber();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
