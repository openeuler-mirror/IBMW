// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>

namespace ibmw::context {
class Context;
class OpPub;
class OpSub;
class OpCli;
class OpSrv;
}  // namespace ibmw::context

namespace ibmw::context::res::details {

/**
 * @brief Resource identifier base class, stores name and owning Context info.
 */
class Base {
 public:
  Base() = default;

  explicit Base(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] bool IsValid() const {
    return idx_ != static_cast<std::size_t>(-1) && context_id_ != -1;
  }

  [[nodiscard]] const std::string& GetName() const { return name_; }

 protected:
  static void SetName(Base& obj, std::string name) { obj.name_ = std::move(name); }

 private:
  friend class ibmw::context::Context;
  friend class ibmw::context::OpPub;
  friend class ibmw::context::OpSub;
  friend class ibmw::context::OpCli;
  friend class ibmw::context::OpSrv;

  std::string name_;
  std::size_t idx_ = static_cast<std::size_t>(-1);
  int context_id_ = -1;
};

}  // namespace ibmw::context::res::details
