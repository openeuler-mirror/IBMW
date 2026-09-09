#include "topology_artifact.h"
#include <fstream>
#include <sstream>
namespace ibmw::topology {
bool Parse(const std::string& s, uint64_t* out) { auto p=s.rfind(')'); if(p==std::string::npos)return false; std::istringstream in(s.substr(p+1)); std::string x; for(int i=3;i<=22;i++)if(!(in>>x))return false; try{*out=std::stoull(x);return *out!=0;}catch(...){return false;} }
bool ReadSelfStartTicks(uint64_t* t) { std::ifstream f("/proc/self/stat"); std::string s; return std::getline(f,s)&&Parse(s,t); }
std::string Json(const std::string& v) { std::string r="\""; for(unsigned char c:v){if(c=='"'||c=='\\'){r+='\\';r+=c;}else if(c=='\n')r+="\\n";else if(c=='\r')r+="\\r";else if(c=='\t')r+="\\t";else r+=char(c);} return r+'"'; }
bool WriteArtifact(std::ostream& out,const std::vector<Record>& rs,const std::string& run,const std::string& lane,const std::string& role,const std::string& sink,uint64_t pid,uint64_t ticks,const std::string& topic,bool complete) {
 if(!out||run.empty()||lane.empty()||role.empty()||pid==0||ticks==0)return false;
 for(auto&r:rs){if(!r.origin_send_ns||r.receive_ns<r.origin_send_ns)return false; out<<"{\"schema\":\""<<kSchema<<"\",\"kind\":\"record\",\"run_id\":"<<Json(run)<<",\"lane\":"<<Json(lane)<<",\"role\":"<<Json(role)<<",\"sink_id\":"<<Json(sink)<<",\"pid\":"<<pid<<",\"start_ticks\":"<<ticks<<",\"seq\":"<<r.seq<<",\"origin_send_ns\":"<<r.origin_send_ns<<",\"relay_receive_ns\":"<<r.relay_receive_ns<<",\"receive_ns\":"<<r.receive_ns<<",\"latency_ns\":"<<r.receive_ns-r.origin_send_ns<<",\"topic\":"<<Json(topic)<<",\"type\":\""<<kType<<"\",\"payload_bytes\":4096,\"valid\":true}\n";}
 out<<"{\"schema\":\""<<kSchema<<"\",\"kind\":\"completion\",\"run_id\":"<<Json(run)<<",\"lane\":"<<Json(lane)<<",\"role\":"<<Json(role)<<",\"sink_id\":"<<Json(sink)<<",\"pid\":"<<pid<<",\"start_ticks\":"<<ticks<<",\"record_count\":"<<rs.size()<<",\"topic\":"<<Json(topic)<<",\"type\":\""<<kType<<"\",\"payload_bytes\":4096,\"complete\":"<<(complete?"true":"false")<<"}\n"; out.flush(); return bool(out);
}
}
