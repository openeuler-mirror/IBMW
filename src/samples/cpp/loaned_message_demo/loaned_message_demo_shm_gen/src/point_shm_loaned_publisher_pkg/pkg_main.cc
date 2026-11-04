// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for loaned_pub_shm_app.

#include "pkg/pkg_macro.h"

#include "point_shm_loaned_publisher_module/point_shm_loaned_publisher.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PointShmLoanedPublisher", []() -> ibmw::Module* {
           return new ibmw::examples::loaned_message_demo_shm_gen::PointShmLoanedPublisher();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
