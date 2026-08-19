// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "managed/play_smoke_subscriber_module.h"
#include "pkg/pkg_macro.h"

using Module =
    ibmw::examples::cpp::play_smoke_test::PlaySmokeSubscriberModule;

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PlaySmokeCdrSubscriberModule",
         []() -> ibmw::Module* {
           return new Module("PlaySmokeCdrSubscriberModule", "cdr");
         }},
        {"PlaySmokeJsonSubscriberModule",
         []() -> ibmw::Module* {
           return new Module("PlaySmokeJsonSubscriberModule", "json");
         }},
        {"PlaySmokeRos2SubscriberModule",
         []() -> ibmw::Module* {
           return new Module("PlaySmokeRos2SubscriberModule", "ros2");
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
