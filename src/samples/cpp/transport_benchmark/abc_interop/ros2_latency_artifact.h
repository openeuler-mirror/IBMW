// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>
namespace ibmw::abc {
constexpr char kLatencySchema[] = "mw_perf.latency.v1";
constexpr char kLatencyClockDomain[] = "producer_monotonic";
constexpr char kRos2PayloadType[] = "std_msgs/msg/UInt8MultiArray";
struct LatencyRecord { uint64_t seq, send_ns, receive_ns, payload_bytes; };
bool ParseProcStartTimeTicks(const std::string& stat, uint64_t* ticks);
bool ReadSelfStartTimeTicks(uint64_t* ticks);
std::string JsonString(const std::string& value);
bool WriteLatencyArtifact(std::ostream& out, const std::vector<LatencyRecord>& records,
    const std::string& run_id, uint64_t producer_pid, uint64_t producer_start_time_ticks,
    const std::string& topic, bool complete);
}  // namespace ibmw::abc
