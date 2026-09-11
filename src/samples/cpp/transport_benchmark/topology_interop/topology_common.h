#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include "base/benchmark_util.h"
#include "topology_artifact.h"
#include <std_msgs/msg/u_int8_multi_array.hpp>
namespace ibmw::topology {
constexpr size_t kBytes=4096; constexpr uint8_t kFill=0xA5; constexpr uint64_t kMagic=0x544F504F4C4F4759ULL;
inline void Make(std_msgs::msg::UInt8MultiArray& m,uint64_t seq,uint64_t send,uint64_t relay=0){m.data.assign(kBytes,kFill);std::memcpy(m.data.data(),&seq,8);std::memcpy(m.data.data()+8,&send,8);std::memcpy(m.data.data()+16,&relay,8);std::memcpy(m.data.data()+24,&kMagic,8);}
inline bool Decode(const std_msgs::msg::UInt8MultiArray& m,uint64_t*seq,uint64_t*send,uint64_t*relay){if(m.data.size()!=kBytes)return false;for(size_t i=32;i<kBytes;i++)if(m.data[i]!=kFill)return false;uint64_t magic=0;std::memcpy(seq,m.data.data(),8);std::memcpy(send,m.data.data()+8,8);std::memcpy(relay,m.data.data()+16,8);std::memcpy(&magic,m.data.data()+24,8);return magic==kMagic;}
struct Options {std::string role, sink_id, config, csv, status, jsonl; uint32_t hz=100,warmup=50,count=3000;};
inline bool Uint(const char*s,uint32_t*v,bool allow_zero=false){try{size_t n=0;auto x=std::stoul(s,&n);if(!*s||s[n]||(!allow_zero&&!x)||x>UINT32_MAX)return false;*v=x;return true;}catch(...){return false;}}
inline bool Parse(int ac,char**av,Options&o,bool dds){for(int i=1;i<ac;i++){std::string a=av[i];if(a=="--help"||i+1>=ac)return false;auto v=av[++i];if(a=="--role")o.role=v;else if(a=="--sink-id")o.sink_id=v;else if(a=="--config")o.config=v;else if(a=="--csv")o.csv=v;else if(a=="--status")o.status=v;else if(a=="--jsonl")o.jsonl=v;else if(a=="--hz"){if(!Uint(v,&o.hz))return false;}else if(a=="--warmup-messages"){if(!Uint(v,&o.warmup,true))return false;}else if(a=="--message-count"){if(!Uint(v,&o.count))return false;}else return false;}return(o.role=="source"||o.role=="relay"||(o.role=="sink"&&(o.sink_id=="A"||o.sink_id=="B")))&&(!dds||!o.config.empty());}
inline bool Valid(const std_msgs::msg::UInt8MultiArray&m,uint64_t now,uint64_t*seq,uint64_t*send,uint64_t*relay){return Decode(m,seq,send,relay)&&*send&&*relay&&*send<=*relay&&*relay<=now;}
}
