// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Pkg-mode entry point for Demo 8 (bring-your-own IDL pub/sub).

#include "pkg/pkg_macro.h"

#include "my_msg_publisher_module/my_msg_publisher_module.h"
#include "my_msg_subscriber_module/my_msg_subscriber_module.h"

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"MyMsgPublisherModule", []() -> ibmw::Module* {
           return new my_project::MyMsgPublisherModule();
         }},
        {"MyMsgSubscriberModule", []() -> ibmw::Module* {
           return new my_project::MyMsgSubscriberModule();
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
