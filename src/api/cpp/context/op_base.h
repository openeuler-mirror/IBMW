// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/source_location.h"

namespace ibmw::context {

class Context;

class OpBase {
 protected:
  OpBase(Context& ctx, ibmw::common::util::SourceLocation loc) noexcept
      : ctx_(ctx), loc_(loc) {}

  Context& ctx_;
  ibmw::common::util::SourceLocation loc_;
};

}  // namespace ibmw::context
