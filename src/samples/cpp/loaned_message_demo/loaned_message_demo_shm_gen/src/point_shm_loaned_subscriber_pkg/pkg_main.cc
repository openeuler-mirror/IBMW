// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for loaned_sub_shm_app.

#include "pkg/pkg_macro.h"

#include "point_shm_loaned_subscriber_module/point_shm_loaned_subscriber.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PointShmLoanedSubscriber", []() -> ibmw::Module* {
           return new ibmw::examples::loaned_message_demo_shm_gen::PointShmLoanedSubscriber();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
