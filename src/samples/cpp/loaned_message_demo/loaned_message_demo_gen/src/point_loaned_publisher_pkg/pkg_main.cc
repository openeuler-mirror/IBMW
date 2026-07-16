// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for loaned_pub_app.

#include "pkg/pkg_macro.h"

#include "point_loaned_publisher_module/point_loaned_publisher.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PointLoanedPublisher", []() -> ibmw::Module* {
           return new ibmw::examples::loaned_message_demo_gen::PointLoanedPublisher();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
