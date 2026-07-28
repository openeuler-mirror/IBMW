// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Shared DDS type registration for pubsub_demo.
// Include this header in any module that uses ExampleEventMsg.

#pragma once

#include "type_support/dds/dds_transport.h"

#include "ExampleEvent.hpp"
#include "ExampleEventCdrAux.ipp"

IBMW_DDS_TYPE_NAME(ibmw::protocols::example::ExampleEventMsg,
                    "pubsub_demo::ExampleEventMsg")
