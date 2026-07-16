// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for loaned_sub_iox_app.

#include "pkg/pkg_macro.h"

#include "point_iceoryx_loaned_subscriber_module/point_iceoryx_loaned_subscriber.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PointIceoryxLoanedSubscriber", []() -> ibmw::Module* {
           return new ibmw::examples::loaned_message_demo_iceoryx_gen::PointIceoryxLoanedSubscriber();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
