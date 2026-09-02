// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#if __has_include(<fastdds/config.hpp>)
#include <fastdds/config.hpp>
#elif __has_include(<fastrtps/config.h>)
#include <fastrtps/config.h>
#else
#error "Fast DDS/Fast RTPS config header not found"
#endif

#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>

#if __has_include(<fastdds/rtps/common/WriteParams.hpp>)
#include <fastdds/rtps/common/WriteParams.hpp>
#else
#include <fastdds/rtps/common/WriteParams.h>
#endif

#if defined(FASTDDS_VERSION_MAJOR) && FASTDDS_VERSION_MAJOR >= 3
#define IBMW_FASTDDS_V3_PLUS 1
#define IBMW_FASTDDS_WRITER_INITIAL_HEARTBEAT_DELAY initial_heartbeat_delay
#define IBMW_FASTDDS_WRITER_HEARTBEAT_PERIOD heartbeat_period
#define IBMW_FASTDDS_WRITER_NACK_RESPONSE_DELAY nack_response_delay
#define IBMW_FASTDDS_READER_INITIAL_ACKNACK_DELAY initial_acknack_delay
#define IBMW_FASTDDS_READER_HEARTBEAT_RESPONSE_DELAY heartbeat_response_delay
#else
#define IBMW_FASTDDS_V3_PLUS 0
#define IBMW_FASTDDS_WRITER_INITIAL_HEARTBEAT_DELAY initialHeartbeatDelay
#define IBMW_FASTDDS_WRITER_HEARTBEAT_PERIOD heartbeatPeriod
#define IBMW_FASTDDS_WRITER_NACK_RESPONSE_DELAY nackResponseDelay
#define IBMW_FASTDDS_READER_INITIAL_ACKNACK_DELAY initialAcknackDelay
#define IBMW_FASTDDS_READER_HEARTBEAT_RESPONSE_DELAY heartbeatResponseDelay
#endif

namespace ibmw::extensions::dds_extension::fastdds_compat {

#if IBMW_FASTDDS_V3_PLUS
namespace rtps = eprosima::fastdds::rtps;
namespace return_code = eprosima::fastdds::dds;
using RequestedDeadlineMissedStatus = eprosima::fastdds::dds::RequestedDeadlineMissedStatus;
using SampleRejectedStatus = eprosima::fastdds::dds::SampleRejectedStatus;
inline constexpr auto kReturnCodeOk = eprosima::fastdds::dds::RETCODE_OK;
inline constexpr auto kReturnCodeNoData = eprosima::fastdds::dds::RETCODE_NO_DATA;
inline constexpr auto kReturnCodePreconditionNotMet = eprosima::fastdds::dds::RETCODE_PRECONDITION_NOT_MET;
#else
namespace rtps = eprosima::fastrtps::rtps;
namespace return_code = eprosima::fastrtps::types;
using RequestedDeadlineMissedStatus = eprosima::fastrtps::RequestedDeadlineMissedStatus;
using SampleRejectedStatus = eprosima::fastrtps::SampleRejectedStatus;
inline constexpr auto kReturnCodeOk = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
inline constexpr auto kReturnCodeNoData = eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA;
inline constexpr auto kReturnCodePreconditionNotMet = eprosima::fastrtps::types::ReturnCode_t::RETCODE_PRECONDITION_NOT_MET;
#endif

using SampleIdentity = rtps::SampleIdentity;
using WriteParams = rtps::WriteParams;

inline bool WriteWithParams(eprosima::fastdds::dds::DataWriter* writer,
                            void* sample, WriteParams& params) {
#if IBMW_FASTDDS_V3_PLUS
  return writer->write(sample, params) == kReturnCodeOk;
#else
  return writer->write(sample, params);
#endif
}

inline const SampleIdentity& WrittenSampleIdentity(
    const WriteParams& params) noexcept {
  return params.sample_identity();
}

inline const SampleIdentity& ReceivedSampleIdentity(
    const eprosima::fastdds::dds::SampleInfo& info) noexcept {
  return info.sample_identity;
}

}  // namespace ibmw::extensions::dds_extension::fastdds_compat
