// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "pkg/pkg_macro.h"
#include "managed/record_smoke_module.h"

using RecordSmokeModule =
    ibmw::examples::cpp::record_smoke_test::RecordSmokeModule;

static std::tuple<std::string_view, std::function<ibmw::Module*()>>
    ibmw_module_register_array[]{
        {"RecordSmokeCdrModule", []() -> ibmw::Module* {
           return new RecordSmokeModule("RecordSmokeCdrModule", "cdr");
         }},
        {"RecordSmokeRos2Module", []() -> ibmw::Module* {
           return new RecordSmokeModule("RecordSmokeRos2Module", "ros2");
         }},
        {"RecordSmokeJsonModule", []() -> ibmw::Module* {
           return new RecordSmokeModule("RecordSmokeJsonModule", "json");
         }}};

IBMW_PKG_MAIN(ibmw_module_register_array)
