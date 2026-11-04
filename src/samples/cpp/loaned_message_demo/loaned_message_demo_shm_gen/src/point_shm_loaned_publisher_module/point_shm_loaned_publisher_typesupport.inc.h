// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

// Public TypeSupport binding for a loaned publisher. No concrete transport
// implementation type is visible to the generated application module.

#include <chrono>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "cpp/transport/transport_factory.h"
#include "type_support/ros2/ros2_transport.h"
#include <geometry_msgs/msg/point.hpp>

#define IBMW_TS_LOANED_PUB_MSG_TYPE geometry_msgs::msg::Point

#define IBMW_TS_LOANED_PUB_STATIC_ASSERT                                      \
  static_assert(std::is_trivially_copyable_v<IBMW_TS_LOANED_PUB_MSG_TYPE>,    \
                "loaned_publisher requires a trivially_copyable "             \
                "message type (geometry_msgs::msg::Point)")

#define IBMW_TS_LOANED_PUB_MEMBER                                             \
  std::optional<ibmw::transport::SenderAccess<                                \
      IBMW_TS_LOANED_PUB_MSG_TYPE>> publisher_

#define IBMW_TS_LOANED_PUB_SETUP(core_expr)                                   \
  do {                                                                        \
    publisher_.emplace(                                                       \
        (core_expr).GetTransport().GetPublisher("point_topic"));                  \
    if (!publisher_->RegisterSendType()) {                                    \
      MW_ERROR("PointShmLoanedPublisher: RegisterSendType failed for topic 'point_topic'");        \
      return false;                                                           \
    }                                                                         \
    if (!publisher_->RegisterLoanedPodTopic(                                  \
            sizeof(IBMW_TS_LOANED_PUB_MSG_TYPE),                              \
            alignof(IBMW_TS_LOANED_PUB_MSG_TYPE))) {                         \
      MW_ERROR("PointShmLoanedPublisher: configured transport rejected POD loan setup for "    \
               "topic 'point_topic'");                                            \
      return false;                                                           \
    }                                                                         \
    MW_INFO("PointShmLoanedPublisher: public loaned publisher ready on 'point_topic' ({} bytes)", \
            sizeof(IBMW_TS_LOANED_PUB_MSG_TYPE));                             \
  } while (0)

#define IBMW_TS_LOANED_PUB_RUN_INIT(core_expr)                                \
  do {                                                                        \
    (void)(core_expr);                                                        \
    if (!publisher_) {                                                        \
      MW_ERROR("PointShmLoanedPublisher: loaned publisher was not initialized");              \
      return false;                                                           \
    }                                                                         \
  } while (0)

// Whole PublishLoop while-body: Borrow + strict mode counter +
// populate user-code marker pair + Publish + sleep.
#define IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_var)                            \
  do {                                                                        \
    auto loop_var = publisher_->Loan();                                       \
    if (!loop_var.is_valid()) {                                               \
      /* Strict mode: failed Borrow IS the failure mode (no copy */           \
      /* fallback). Log and back off briefly. */                              \
      ++fallback_count_;                                                      \
      MW_ERROR("PointShmLoanedPublisher: LoanedSample::Borrow failed (topic='point_topic')");      \
      std::this_thread::sleep_for(std::chrono::milliseconds(100));            \
      break;                                                                  \
    }                                                                         \
    ++zero_copy_count_;                                                       \
    /* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_BEGIN */                             \
    /* IBMW: user code begin (populate_loaned_message) */                     \
    /* TODO: populate `*loop_var` (or `loop_var->field`) here. This */        \
    /* writes directly into the shm chunk -- true zero-copy. */              \
    /* The subscriber sees these bytes verbatim. */                           \
    /* IBMW: user code end (populate_loaned_message) */                       \
    /* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_END */                               \
    if ((publish_count_ % 50) == 0) {                                         \
      MW_INFO("[SEND] {} #{} zero_copy=true (shm)",                          \
              "point_topic", publish_count_);                                     \
    }                                                                         \
    bool _ibmw_pub_ok = publisher_->Publish(std::move(loop_var));             \
    if (_ibmw_pub_ok) {                                                       \
      ++publish_count_;                                                       \
    } else {                                                                  \
      MW_ERROR("PointShmLoanedPublisher: LoanedSample::Publish failed");                       \
    }                                                                         \
    std::this_thread::sleep_for(std::chrono::milliseconds(100));              \
  } while (0)

// PrintStatistics body: 4 lines + strict-mode comment line + separator.
#define IBMW_TS_LOANED_PUB_PRINT_STATS                                        \
  do {                                                                        \
    uint64_t _ibmw_total = zero_copy_count_ + fallback_count_;                \
    double _ibmw_ratio = _ibmw_total > 0                                      \
        ? static_cast<double>(zero_copy_count_) / _ibmw_total * 100.0 : 0.0;  \
    MW_INFO("============ Loaned Publisher (shm) Statistics ============"); \
    MW_INFO("Total published : {}", publish_count_);                          \
    MW_INFO("Zero-copy       : {} ({:.1f}%)", zero_copy_count_, _ibmw_ratio); \
    MW_INFO("Borrow failures : {} (strict mode -- no copy fallback)",         \
            fallback_count_);                                                 \
    MW_INFO("==========================================================="); \
  } while (0)
