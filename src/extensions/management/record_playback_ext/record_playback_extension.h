// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "extension_api/ibmw_extension_base.h"

namespace ibmw::extensions::record_playback_extension {

// The implementation is private to the provider DSO. The public boundary is
// the versioned EngineExtensionBase entry point; Core engine, transport, and
// TypeSupport implementation types do not cross this header.
class RecordPlaybackExtension;

}  // namespace ibmw::extensions::record_playback_extension
