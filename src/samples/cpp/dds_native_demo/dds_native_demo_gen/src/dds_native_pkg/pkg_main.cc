// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for dds_native_app.

#include "pkg/pkg_macro.h"

#include "publisher_module/publisher_module.h"
#include "subscriber_module/subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PublisherModule", []() -> ibmw::Module* {
           return new dds_native_demo::PublisherModule();
         }},
        {"SubscriberModule", []() -> ibmw::Module* {
           return new dds_native_demo::SubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
