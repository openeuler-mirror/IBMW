// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for JSON pub/sub demo.

#include "pkg/pkg_macro.h"

#include "json_publisher_module/json_publisher_module.h"
#include "json_subscriber_module/json_subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"JsonPublisherModule", []() -> ibmw::Module* {
           return new my_project::JsonPublisherModule();
         }},
        {"JsonSubscriberModule", []() -> ibmw::Module* {
           return new my_project::JsonSubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
