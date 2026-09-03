#include "ros2_latency_artifact.h"
#include <fstream>
#include <sstream>
namespace ibmw::abc {
bool ParseProcStartTimeTicks(const std::string& stat, uint64_t* ticks) {
  if (!ticks) return false;
  const auto close = stat.rfind(')');
  if (close == std::string::npos) return false;
  std::istringstream fields(stat.substr(close + 1));
  std::string field;
  for (int index = 3; index <= 22; ++index) if (!(fields >> field)) return false;
  try { size_t used = 0; auto value = std::stoull(field, &used);
    if (used != field.size() || value == 0) return false;
    *ticks = value;
    return true;
  } catch (...) { return false; }
}
bool ReadSelfStartTimeTicks(uint64_t* ticks) {
  std::ifstream in("/proc/self/stat"); std::string stat;
  return static_cast<bool>(std::getline(in, stat)) && ParseProcStartTimeTicks(stat, ticks);
}
std::string JsonString(const std::string& value) {
  static constexpr char hex[] = "0123456789abcdef"; std::string result; result.reserve(value.size()+2); result += '"';
  for (unsigned char c : value) { switch (c) {
    case '"': result += "\\\""; break; case '\\': result += "\\\\"; break;
    case '\b': result += "\\b"; break; case '\f': result += "\\f"; break;
    case '\n': result += "\\n"; break; case '\r': result += "\\r"; break; case '\t': result += "\\t"; break;
    default: if (c < 0x20) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
      else result += static_cast<char>(c);
  }} result += '"'; return result;
}
bool WriteLatencyArtifact(std::ostream& out, const std::vector<LatencyRecord>& records,
    const std::string& run_id, uint64_t pid, uint64_t ticks, const std::string& topic, bool complete) {
  if (!out || run_id.empty() || topic.empty() || pid == 0 || ticks == 0) return false;
  for (const auto& r : records) { if (!r.send_ns || r.receive_ns < r.send_ns) return false;
    out << "{\"schema\":\"" << kLatencySchema << "\",\"seq\":" << r.seq << ",\"send_ns\":" << r.send_ns
      << ",\"receive_ns\":" << r.receive_ns << ",\"latency_ns\":" << r.receive_ns-r.send_ns
      << ",\"topic\":" << JsonString(topic) << ",\"type\":\"" << kRos2PayloadType
      << "\",\"payload_bytes\":" << r.payload_bytes << ",\"valid\":true}\n"; }
  out << "{\"schema\":\"" << kLatencySchema << "\",\"kind\":\"completion\",\"run_id\":" << JsonString(run_id)
    << ",\"producer_pid\":" << pid << ",\"producer_start_time_ticks\":" << ticks
    << ",\"record_count\":" << records.size() << ",\"topic\":" << JsonString(topic)
    << ",\"type\":\"" << kRos2PayloadType << "\",\"complete\":" << (complete?"true":"false")
    << ",\"clock_domain\":\"" << kLatencyClockDomain << "\"}\n";
  out.flush(); return static_cast<bool>(out);
}
}  // namespace ibmw::abc
