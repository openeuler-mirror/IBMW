// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for ros2_pub_only_app.

#include "pkg/pkg_macro.h"

#include "publisher_module/publisher_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PublisherModule", []() -> ibmw::Module* {
           return new ros2_interop_demo::PublisherModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
