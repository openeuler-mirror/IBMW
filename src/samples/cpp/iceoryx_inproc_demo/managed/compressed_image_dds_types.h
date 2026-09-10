// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Shared DDS type registration for iceoryx_inproc_demo.
// Reuses sensor::CompressedImage IDL (header + format + sequence<octet> data)
// so we can craft 1MB+ payloads to exercise the Iceoryx shared-memory
// auto-grow path (LoanShm doubling).

#pragma once

#include "type_support/dds/dds_transport.h"

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "CompressedImage.hpp"
#include "CompressedImageCdrAux.ipp"

IBMW_DDS_TYPE_NAME(ibmw::protocols::sensor::CompressedImage,
                    "iceoryx_demo::CompressedImage")
