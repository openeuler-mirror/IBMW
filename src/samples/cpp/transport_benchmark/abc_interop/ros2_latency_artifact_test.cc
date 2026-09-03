#include "ros2_latency_artifact.h"
#include <cassert>
#include <sstream>
int main() {
  uint64_t ticks=0; std::string stat="42 (worker name (aux)) S";
  for(int i=0;i<18;++i) stat += " 0";
  stat += " 98765";
  assert(ibmw::abc::ParseProcStartTimeTicks(stat,&ticks) && ticks==98765);
  assert(!ibmw::abc::ParseProcStartTimeTicks("broken",&ticks));
  std::ostringstream out; std::vector<ibmw::abc::LatencyRecord> records{{7,10,25,4096}};
  assert(ibmw::abc::WriteLatencyArtifact(out,records,"run\n\"id",42,98765,"/topic",true));
  const std::string expected =
    "{\"schema\":\"mw_perf.latency.v1\",\"seq\":7,\"send_ns\":10,\"receive_ns\":25,"
    "\"latency_ns\":15,\"topic\":\"/topic\",\"type\":\"std_msgs/msg/UInt8MultiArray\","
    "\"payload_bytes\":4096,\"valid\":true}\n"
    "{\"schema\":\"mw_perf.latency.v1\",\"kind\":\"completion\",\"run_id\":\"run\\n\\\"id\","
    "\"producer_pid\":42,\"producer_start_time_ticks\":98765,\"record_count\":1,"
    "\"topic\":\"/topic\",\"type\":\"std_msgs/msg/UInt8MultiArray\",\"complete\":true,"
    "\"clock_domain\":\"producer_monotonic\"}\n";
  assert(out.str() == expected);
}
