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
  assert(out.str().find("\"latency_ns\":15") != std::string::npos);
  assert(out.str().find("\"run_id\":\"run\\n\\\"id\"") != std::string::npos);
  assert(out.str().find("\"record_count\":1") != std::string::npos);
  assert(out.str().find("\"kind\":\"completion\"")==out.str().rfind("\"kind\":\"completion\""));
}
