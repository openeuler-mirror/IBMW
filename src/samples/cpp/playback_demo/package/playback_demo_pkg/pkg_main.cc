// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "pkg/pkg_macro.h"
#include "subscriber_module/subscriber_module.h"

using PlaybackSubscriberModule =
    ibmw::examples::cpp::playback_demo::subscriber_module::SubscriberModule;

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"PlaybackSubscriberModule", []() -> ibmw::Module* {
           return new PlaybackSubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
