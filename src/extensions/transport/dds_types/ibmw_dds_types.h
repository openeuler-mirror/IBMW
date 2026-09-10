// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Umbrella header for ibmw DDS interface.
// Including this header provides:
// - DDS type support (CDR serialization via FastCDR)
// - DDS transport helpers (RegisterSendType, Publish, Subscribe)
// - Context API integration (MessageTypeSupportTraits, MessagePublisherTraits)
//
// After including this header, use IBMW_DDS_REGISTER_CONTEXT_TYPE(Type) for
// each DDS type that should work with the Context API's CreatePublisher<T>,
// CreateSubscriber<T>, etc.

#pragma once

#include "type_support/dds/dds_transport.h"
#include "dds_types/util/dds_type_support.h"

/**
 * @brief Register a DDS type for use with the ibmw Context API.
 *
 * This macro specializes ibmw::MessageTypeSupportTraits<T> and
 * ibmw::transport::MessagePublisherTraits<T> so that Context::CreatePublisher<T>,
 * Context::CreateSubscriber<T>, etc. work correctly with DDS types.
 *
 * Prerequisites:
 * - The type must have FastCDR serialize/deserialize specializations
 *   (either hand-written or from fastddsgen CdrAux files)
 * - The type must have a DdsTypeName<T> specialization
 *   (via IBMW_DDS_TYPE_NAME macro or explicit specialization)
 *
 * Usage:
 *   #include "dds_types/ibmw_dds_types.h"
 *   #include "ExampleEvent.hpp"
 *   #include "ExampleEventCdrAux.hpp"
 *   IBMW_DDS_TYPE_NAME(ibmw::protocols::example::ExampleEventMsg,
 *                       "ibmw::protocols::example::ExampleEventMsg")
 *   IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::protocols::example::ExampleEventMsg)
 */
#define IBMW_DDS_REGISTER_CONTEXT_TYPE(DdsType)                                \
  namespace ibmw {                                                             \
  template <>                                                                  \
  struct MessageTypeSupportTraits<DdsType> {                                   \
    static const ibmw_type_support_base_t* Get() {                             \
      return ibmw::dds::GetDdsMessageTypeSupport<DdsType>();                   \
    }                                                                          \
  };                                                                           \
  }                                                                            \
  namespace ibmw::transport {                                                    \
  template <>                                                                  \
  struct MessagePublisherTraits<DdsType> {                                     \
    static void PublishMsg(SenderRef publisher, ContextRef ctx_ref,          \
                           const DdsType& msg) {                               \
      ibmw::transport::Send<DdsType>(publisher, ctx_ref, msg);               \
    }                                                                          \
  };                                                                           \
  }
