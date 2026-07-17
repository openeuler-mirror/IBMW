// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Shared DDS type registration for ExampleEventMsg.
// Include this header in any module that uses ExampleEventMsg.

#pragma once

#include "internal/typesupport/dds/dds_transport.h"

#include "ExampleEvent.hpp"
#include "ExampleEventCdrAux.ipp"

IBMW_DDS_TYPE_NAME(ibmw::protocols::example::ExampleEventMsg,
                    "my_project::ExampleEventMsg")
