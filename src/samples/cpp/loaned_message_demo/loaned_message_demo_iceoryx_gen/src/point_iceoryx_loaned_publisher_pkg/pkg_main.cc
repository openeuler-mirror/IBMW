// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for loaned_pub_iox_app.

#include "pkg/pkg_macro.h"

#include "point_iceoryx_loaned_publisher_module/point_iceoryx_loaned_publisher.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PointIceoryxLoanedPublisher", []() -> ibmw::Module* {
           return new ibmw::examples::loaned_message_demo_iceoryx_gen::PointIceoryxLoanedPublisher();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
