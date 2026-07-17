// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Shared DDS type registrations for the context example.
// Include this header in any module that uses ExampleEventMsg with the Context API.

#pragma once

// IBMW_R1_EXEMPT_BEGIN
#include "dds_types/ibmw_dds_types.h"
// IBMW_R1_EXEMPT_END

#include "ExampleEvent.hpp"
#include "ExampleEventCdrAux.ipp"

IBMW_DDS_TYPE_NAME(ibmw::protocols::example::ExampleEventMsg,
                    "ibmw::protocols::example::ExampleEventMsg")
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::protocols::example::ExampleEventMsg)
