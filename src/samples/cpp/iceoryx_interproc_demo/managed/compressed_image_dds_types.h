// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Shared DDS type registration for iceoryx_interproc_demo.
// Reuses sensor::CompressedImage IDL so we can craft 1 MB+ payloads to
// exercise the Iceoryx shared-memory auto-grow path across two processes
// connected via the iox-roudi daemon.
//
// IMPORTANT: the registered type name MUST match between publisher and
// subscriber processes; the iceoryx URL is derived from
// (topic_name, dds_type_name) so a mismatch yields different
// ServiceDescriptions and the subscriber would silently never receive.

#pragma once

#include "internal/typesupport/dds/dds_transport.h"

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "CompressedImage.hpp"
#include "CompressedImageCdrAux.ipp"

IBMW_DDS_TYPE_NAME(ibmw::protocols::sensor::CompressedImage,
                    "iceoryx_demo::CompressedImage")
