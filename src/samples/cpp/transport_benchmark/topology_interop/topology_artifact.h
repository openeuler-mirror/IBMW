#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>
namespace ibmw::topology {
constexpr char kSchema[] = "mw_perf.topology.v1";
constexpr char kType[] = "std_msgs/msg/UInt8MultiArray";
struct Record { uint64_t seq, origin_send_ns, relay_receive_ns, receive_ns; };
bool ReadSelfStartTicks(uint64_t* ticks);
std::string Json(const std::string& value);
bool WriteArtifact(std::ostream&, const std::vector<Record>&, const std::string& run_id,
                   const std::string& lane, const std::string& role, const std::string& sink_id,
                   uint64_t pid, uint64_t ticks, const std::string& topic, bool complete);
}
