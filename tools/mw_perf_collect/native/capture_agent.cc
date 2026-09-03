#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#if !__has_include(<sys/prctl.h>)
#error "descendant_policy=forbid requires sys/prctl.h"
#endif
#include <sys/prctl.h>
#if !defined(PR_SET_CHILD_SUBREAPER) || !defined(PR_SET_PDEATHSIG)
#error "descendant_policy=forbid requires subreaper and parent-death prctl constants"
#endif
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
constexpr size_t kMaxResult = 32768;
constexpr uint64_t kMaxMember = 32ULL * 1024 * 1024;
constexpr uint64_t kMaxArchive = 64ULL * 1024 * 1024;
constexpr size_t kMaxEntries = 4096;
volatile sig_atomic_t g_forwarded_signal = 0;

void ForwardSignal(int signal_number)
{
  g_forwarded_signal = signal_number;
}

struct Sha256
{
  uint32_t h[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                0x5be0cd19};
  uint64_t bits = 0;
  std::array<unsigned char, 64> buf{};
  size_t used = 0;
  static uint32_t R(uint32_t x, unsigned n)
  {
    return (x >> n) | (x << (32 - n));
  }
  void Block(const unsigned char* p)
  {
    static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
      0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc,
      0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351,
      0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
      0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585,
      0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
      w[i] =
        (uint32_t(p[i * 4]) <<
          24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i)
    {
      uint32_t a = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3),
               b = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + a + w[i - 7] + b;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], q = h[7];
    for (int i = 0; i < 64; ++i)
    {
      uint32_t s1 = R(e, 6) ^ R(e, 11) ^ R(e, 25), ch = (e & f) ^ (~e & g),
               t1 = q + s1 + ch + k[i] + w[i], s0 = R(a, 2) ^ R(a, 13) ^ R(a, 22),
               maj = (a & b) ^ (a & c) ^ (b & c), t2 = s0 + maj;
      q = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += q;
  }
  void Add(const void* data, size_t n)
  {
    auto*p = static_cast<const unsigned char*>(data);
    bits += uint64_t(n) * 8;
    while (n)
    {
      size_t z = std::min(n, 64 - used);
      memcpy(buf.data() + used, p, z);
      used += z;
      p += z;
      n -= z;
      if (used == 64)
      {
        Block(buf.data());
        used = 0;
      }
    }
  }
  std::string Final()
  {
    uint64_t nbits = bits;
    unsigned char one = 0x80;
    Add(&one, 1);
    unsigned char zero = 0;
    while (used != 56)
    {
      Add(&zero, 1);
    }
    unsigned char len[8];
    for (int i = 0; i < 8; ++i)
    {
      len[7 - i] = static_cast<unsigned char>(nbits >> (i * 8));
    }
    Add(len, 8);
    char out[65];
    for (int i = 0; i < 8; ++i)
    {
      snprintf(out + i * 8, 9, "%08x", h[i]);
    }
    return std::string(out, 64);
  }
};

std::string Hash(const std::string& s)
{
  Sha256 h;
  h.Add(s.data(), s.size());
  return h.Final();
}
bool WriteAll(int fd, const void* data, size_t n)
{
  auto*p = static_cast<const char*>(data);
  while (n)
  {
    ssize_t z = write(fd, p, n);
    if (z < 0 && errno == EINTR)
    {
      continue;
    }
    if (z <= 0)
    {
      return false;
    }
    p += z;
    n -= size_t(z);
  }
  return true;
}
std::string HashFd(int fd, bool* ok, std::string* bytes = nullptr)
{
  Sha256 h;
  char b[65536];
  if (lseek(fd, 0, SEEK_SET) < 0)
  {
    *ok = false;
    return{};
  }
  for (;;)
  {
    ssize_t n = read(fd, b, sizeof b);
    if (n < 0 && errno == EINTR)
    {
      continue;
    }
    if (n < 0)
    {
      *ok = false;
      return{};
    }
    if (!n)
    {
      break;
    }
    h.Add(b, size_t(n));
    if (bytes)
    {
      bytes->append(b, size_t(n));
    }
  }
  *ok = true;
  return h.Final();
}
std::string J(std::string_view s)
{
  std::string o = "\"";
  for (unsigned char c:s)
  {
    if (c == '\"' || c == '\\')
    {
      o += '\\';
      o += char(c);
    }
    else if (c == '\n')
    {
      o += "\\n";
    }
    else if (c == '\r')
    {
      o += "\\r";
    }
    else if (c == '\t')
    {
      o += "\\t";
    }
    else if (c < 0x20)
    {
      char b[7];
      snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    }
    else
    {
      o += char(c);
    }
  }
  return o + '\"';
}
bool Ascii(const std::string& s, size_t maximum)
{
  return !s.empty() && s.size() <= maximum && std::all_of(s.begin(), s.end(), [](unsigned char c){
      return c >= 0x20 && c <= 0x7e;
    });
}
bool SafeId(const std::string& s)
{
  if (!Ascii(s, 128))
  {
    return false;
  }
  for (unsigned char c:s)
  {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '_' || c == ':' || c == '-'))
    {
      return false;
    }
  }
  return true;
}
bool Hex64(const std::string&s)
{
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c){
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool EnvName(const std::string&s)
{
  if (!Ascii(s, 128) || (!isalpha((unsigned char)s[0]) && s[0] != '_'))
  {
    return false;
  }
  return std::all_of(s.begin() + 1, s.end(), [](unsigned char c){
      return isalnum(c) || c == '_';
    });
}
bool ParseU64(const std::string&s, uint64_t*out)
{
  if (s.empty() || s[0] == '-')
  {
    return false;
  }
  char*e = nullptr;
  errno = 0;
  auto v = strtoull(s.c_str(), &e, 10);
  if (errno || *e)
  {
    return false;
  }
  *out = v;
  return true;
}
void Reason(std::vector<std::string>*r, const std::string&s)
{
  if (std::find(r->begin(), r->end(), s) == r->end())
  {
    r->push_back(s);
  }
}

struct Request {std::string root, run, cap, path, gen, topo, mode, challenge;
                bool exact = false;
                uint64_t timeout = 0;
                std::vector<std::string> env, argv;
};
bool Parse(int argc, char**argv, Request*r, std::string*why)
{
  std::set<std::string> seen;
  bool command = false;
  size_t bytes = 0;
  for (int i = 1; i < argc; ++i)
  {
    std::string a = argv[i];
    if (!Ascii(a, 4096))
    {
      *why = "invalid_contract_string";
      return false;
    }
    if (command)
    {
      bytes += a.size() + 1;
      if (r->argv.size() >= 4096 || bytes > 1024 * 1024)
      {
        *why = "invalid_command_size";
        return false;
      }
      r->argv.push_back(a);
      continue;
    }
    if (a == "--")
    {
      command = true;
      continue;
    }
    if (a == "--env")
    {
      if (++i == argc || !Ascii(argv[i], 128))
      {
        *why = "missing_env_name";
        return false;
      }
      r->env.push_back(argv[i]);
      continue;
    }
    if (a.rfind("--", 0) != 0 || ++i == argc)
    {
      *why = "invalid_arguments";
      return false;
    }
    if (!seen.insert(a).second)
    {
      *why = "duplicate_option";
      return false;
    }
    std::string v = argv[i];
    if (!Ascii(v, 4096))
    {
      *why = "invalid_contract_string";
      return false;
    }
    if (a == "--run-root")
    {
      r->root = v;
    }
    else if (a == "--run-id")
    {
      r->run = v;
    }
    else if (a == "--capability-id")
    {
      r->cap = v;
    }
    else if (a == "--path-id")
    {
      r->path = v;
    }
    else if (a == "--request-challenge")
    {
      r->challenge = v;
    }
    else if (a == "--topology-generation")
    {
      r->gen = v;
    }
    else if (a == "--topology-sha256")
    {
      r->topo = v;
    }
    else if (a == "--trace-mode")
    {
      r->mode = v;
    }
    else if (a == "--exact-required")
    {
      if (v == "true")
      {
        r->exact = true;
      }
      else if (v == "false")
      {
        r->exact = false;
      }
      else
      {
        *why = "invalid_exact_required";
        return false;
      }
    }
    else if (a == "--timeout-ms")
    {
      if (!ParseU64(v, &r->timeout) || r->timeout < 1 || r->timeout > 86400000)
      {
        *why = "invalid_timeout";
        return false;
      }
    }
    else
    {
      *why = "unknown_option";
      return false;
    }
  }
  if (!command || r->argv.empty())
  {
    *why = "missing_command";
    return false;
  }
  if (!SafeId(r->run) || !SafeId(r->cap) || !SafeId(r->path) || !SafeId(r->gen))
  {
    *why = "invalid_identifier";
    return false;
  }
  if (!Hex64(r->topo))
  {
    *why = "invalid_topology_hash";
    return false;
  }
  if (!Hex64(r->challenge))
  {
    *why = "invalid_request_challenge";
    return false;
  }
  if (r->mode != "off" && r->mode != "sampled" && r->mode != "full")
  {
    *why = "invalid_trace_mode";
    return false;
  }
  if (r->timeout == 0)
  {
    *why = "missing_timeout";
    return false;
  }
  if (r->exact && r->mode != "full")
  {
    *why = "exact_requires_full_trace";
    return false;
  }
  if (r->env.size() > 128)
  {
    *why = "invalid_env_allowlist";
    return false;
  }
  std::set<std::string> names;
  size_t env_bytes = 0;
  for (const auto&n:r->env)
  {
    const char*v = getenv(n.c_str());
    if (!EnvName(n) ||
        n.rfind("MW_PERF_",
                0) == 0 ||
        n.rfind("IBMW_",
                0) == 0 || !names.insert(n).second || v == nullptr ||
        !Ascii(v, 4096) || (env_bytes += n.size() + strlen(v) + 1) > 65536)
    {
      *why = "invalid_env_allowlist";
      return false;
    }
  }
  return true;
}
std::string CanonicalRequest(const Request&r)
{
  std::ostringstream o;
  o << "{\"argv\":[";
  for (size_t i = 0; i < r.argv.size(); ++i)
  {
    if (i)
    {
      o << ',';
    }
    o << J(r.argv[i]);
  }
  o << "],\"capability_id\":" << J(r.cap) << ",\"env\":[";
  for (size_t i = 0; i < r.env.size(); ++i)
  {
    if (i)
    {
      o << ',';
    }
    o << J(r.env[i] + "=" + getenv(r.env[i].c_str()));
  }
  o << "],\"exact_required\":" << (r.exact?"true":"false") << ",\"path_id\":" << J(r.path) <<
    ",\"request_challenge\":" << J(r.challenge) << ",\"run_id\":" << J(r.run) <<
    ",\"run_root\":" << J(r.root) << ",\"timeout_ms\":" <<
    r.timeout << ",\"topology_generation\":" << J(r.gen) << ",\"topology_sha256\":" << J(r.topo) <<
    ",\"trace_mode\":" << J(r.mode) << '}';
  return o.str();
}
bool ProcStart(pid_t pid, uint64_t*out)
{
  std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
  std::string s;
  getline(f, s);
  auto p = s.rfind(')');
  if (!f || p == std::string::npos)
  {
    return false;
  }
  std::istringstream in(s.substr(p + 2));
  std::string x;
  for (int field = 3; field <= 22; ++field)
  {
    if (!(in >> x))
    {
      return false;
    }
    if (field == 22)
    {
      return ParseU64(x, out);
    }
  }
  return false;
}
struct ProcessIdentity
{
  pid_t pid = -1;
  pid_t ppid = -1;
  uint64_t start = 0;
};
bool ProcIdentity(pid_t pid, ProcessIdentity*out)
{
  std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
  std::string s;
  getline(f, s);
  auto p = s.rfind(')');
  if (!f || p == std::string::npos)
  {
    return false;
  }
  std::istringstream in(s.substr(p + 2));
  std::string value;
  pid_t parent = -1;
  uint64_t start = 0;
  for (int field = 3; field <= 22; ++field)
  {
    if (!(in >> value))
    {
      return false;
    }
    if (field == 4)
    {
      char*end = nullptr;
      const long parsed = strtol(value.c_str(), &end, 10);
      if (*end || parsed < 0 || parsed > INT_MAX)
      {
        return false;
      }
      parent = static_cast<pid_t>(parsed);
    }
    if (field == 22 && !ParseU64(value, &start))
    {
      return false;
    }
  }
  *out = {pid, parent, start};
  return start != 0;
}
bool DiscoverDescendants(pid_t leader, std::map<pid_t, uint64_t>*known)
{
  DIR*d = opendir("/proc");
  if (!d)
  {
    return false;
  }
  std::vector<ProcessIdentity> processes;
  while (auto*e = readdir(d))
  {
    if (!isdigit((unsigned char)e->d_name[0]))
    {
      continue;
    }
    char*end = nullptr;
    const long parsed = strtol(e->d_name, &end, 10);
    ProcessIdentity identity;
    if (*end == '\0' && parsed > 1 && parsed <= INT_MAX &&
        ProcIdentity(static_cast<pid_t>(parsed), &identity))
    {
      processes.push_back(identity);
    }
  }
  (void)closedir(d);
  bool changed = true;
  bool descendant = false;
  while (changed)
  {
    changed = false;
    for (const auto&process:processes)
    {
      if (process.pid == leader || known->count(process.pid))
      {
        continue;
      }
      if (process.ppid == leader || process.ppid == getpid() || known->count(process.ppid))
      {
        (*known)[process.pid] = process.start;
        changed = true;
        descendant = true;
      }
    }
  }
  return descendant;
}
bool IdentityAlive(pid_t pid, uint64_t start)
{
  uint64_t actual = 0;
  return pid > 1 && ProcStart(pid, &actual) && actual == start;
}
void SignalIdentities(const std::map<pid_t, uint64_t>&known, int signal,
#if defined(IBMW_CAPTURE_TEST_HOOKS)
                      pid_t leader,
#endif
                      std::vector<std::string>*reasons)
{
  for (const auto&[pid, start]:known)
  {
    if (!IdentityAlive(pid, start))
    {
      continue;
    }
#if defined(IBMW_CAPTURE_TEST_HOOKS)
    if (signal == SIGKILL && pid != leader &&
        getenv("IBMW_CAPTURE_INJECT_DESCENDANT_SIGKILL_SURVIVOR") != nullptr)
    {
      continue;
    }
#endif
    if (kill(pid, signal) != 0 && errno != ESRCH)
    {
      Reason(reasons, "kill_failed");
    }
  }
}
#if defined(IBMW_CAPTURE_TEST_HOOKS)
#define IBMW_SIGNAL_IDENTITIES(known, signal, leader, reasons) \
  SignalIdentities(known, signal, leader, reasons)
#else
#define IBMW_SIGNAL_IDENTITIES(known, signal, leader, reasons) \
  SignalIdentities(known, signal, reasons)
#endif
bool ReapNonblocking(pid_t leader, int*leader_status, bool*leader_reaped,
                     bool*adopted_reaped, std::vector<std::string>*reasons)
{
  bool had_child = false;
  for (;;)
  {
    int status = 0;
    pid_t child = waitpid(-1, &status, WNOHANG);
    if (child > 0)
    {
      had_child = true;
      if (child == leader)
      {
        *leader_status = status;
        *leader_reaped = true;
      }
      else
      {
        *adopted_reaped = true;
      }
      continue;
    }
    if (child == 0 || (child < 0 && errno == ECHILD))
    {
      return had_child;
    }
    if (errno == EINTR)
    {
      continue;
    }
    Reason(reasons, "waitpid_failed");
    return had_child;
  }
}
bool SignalLeaderGroup(pid_t pid, uint64_t start, int signal, std::vector<std::string>*reasons)
{
#if defined(IBMW_CAPTURE_TEST_HOOKS)
  if (getenv("IBMW_CAPTURE_INJECT_LEADER_IDENTITY_MISMATCH") != nullptr)
  {
    return false;
  }
#endif
  if (!IdentityAlive(pid, start)) return false;
  if (kill(-pid, signal) == 0 || errno == ESRCH)
  {
    return true;
  }
  Reason(reasons, "kill_failed");
  return false;
}
bool CloseOtherFds(int keep1, int keep2, int keep3)
{
  long maximum = sysconf(_SC_OPEN_MAX);
  if (maximum < 0)
  {
    maximum = 65536;
  }
  maximum = std::min<long>(maximum, 1048576);
  for (int fd = 3; fd < maximum; ++fd)
  {
    if (fd == keep1 || fd == keep2 || fd == keep3)
    {
      continue;
    }
    if (close(fd) != 0 && errno != EBADF)
    {
      return false;
    }
  }
  return true;
}
std::string BootId()
{
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  std::string s;
  getline(f, s);
  return s;
}
std::string Nonce()
{
  unsigned char b[16];
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    return{};
  }
  size_t offset = 0;
  while (offset < sizeof(b))
  {
    const ssize_t count = read(fd, b + offset, sizeof(b) - offset);
    if (count < 0 && errno == EINTR)
    {
      continue;
    }
    if (count <= 0)
    {
      (void)close(fd);
      return{};
    }
    offset += static_cast<size_t>(count);
  }
  if (close(fd) != 0)
  {
    return{};
  }
  char x[33];
  for (int i = 0; i < 16; ++i)
  {
    snprintf(x + i * 2, 3, "%02x", b[i]);
  }
  return std::string(x, 32);
}
int OpenAbsolute(const std::string&p, int final_flags)
{
  if (p.size() < 2 || p[0] != '/' || p.back() == '/')
  {
    return -1;
  }
  int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
  {
    return -1;
  }
  size_t begin = 1;
  while (begin < p.size())
  {
    size_t end = p.find('/', begin);
    bool final = end == std::string::npos;
    std::string component = p.substr(begin, final?end:end - begin);
    if (component.empty() || component == "." || component == ".." || component.size() > NAME_MAX)
    {
      close(fd);
      errno = EINVAL;
      return -1;
    }
    int next = openat(fd,
                      component.c_str(),
                      (final?final_flags:O_RDONLY | O_DIRECTORY | O_CLOEXEC) | O_NOFOLLOW);
    int saved = errno;
    if (close(fd) != 0 && next >= 0)
    {
      close(next);
      return -1;
    }
    if (next < 0)
    {
      errno = saved;
      return -1;
    }
    fd = next;
    if (final)
    {
      return fd;
    }
    begin = end + 1;
  }
  close(fd);
  return -1;
}

struct File {std::string name, sha;
             off_t size = 0;
             dev_t dev = 0;
             ino_t ino = 0;
             timespec mt{};
};
bool Digits(std::string_view s)
{
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c){
      return isdigit(c);
    });
}
bool Token(std::string_view s)
{
  return !s.empty() && s.size() <= 32 && std::all_of(s.begin(), s.end(), [](unsigned char c){
      return isalnum(c) || c == '_' || c == '-';
    });
}
bool AllowedName(const std::string&n)
{
  constexpr std::string_view stage_prefix = "ibmw_dds_trace.";
  constexpr std::string_view exact_prefix = "ibmw_dds_exact.";
  const bool stage = n.rfind(stage_prefix, 0) == 0;
  const std::string_view prefix = stage?stage_prefix:exact_prefix;
  if (n.size() > 99 || (!stage && n.rfind(exact_prefix, 0) != 0))
  {
    return false;
  }
  size_t a = n.find('.', prefix.size()), b = a == std::string::npos?a:n.find('.', a + 1);
  if (a == std::string::npos || b == std::string::npos ||
      !Digits(std::string_view(n).substr(prefix.size(),
                                         a - prefix.size())) ||
      !Token(std::string_view(n).substr(a + 1, b - a - 1)))
  {
    return false;
  }
  std::string_view suffix(n.data() + b + 1, n.size() - b - 1);
  if (stage && (suffix == "meta.json" || suffix == "seal.json"))
  {
    return true;
  }
  if (!stage)
  {
    return suffix == "exact.meta.json" || suffix == "exact.seal.json" || suffix == "ibce";
  }
  size_t dot = suffix.find('.');
  return dot == 8 &&
         Digits(suffix.substr(0,
                              dot)) &&
         suffix.substr(dot + 1) == "ibct";
}
bool SameStat(const struct stat&a, const struct stat&b)
{
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
         a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
}
std::vector<File> Scan(int dfd, std::vector<std::string>*reasons)
{
  std::vector<File>out;
  uint64_t total = 0;
  size_t entries = 0;
  int scanfd = openat(dfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  DIR*d = scanfd < 0?nullptr:fdopendir(scanfd);
  if (!d)
  {
    if (scanfd >= 0)
    {
      close(scanfd);
    }
    Reason(reasons, "scan_failed");
    return out;
  }
  while (auto*e = readdir(d))
  {
    std::string n = e->d_name;
    if (n == "." || n == "..")
    {
      continue;
    }
    if (++entries > kMaxEntries)
    {
      Reason(reasons, "too_many_entries");
      break;
    }
    struct stat a {}, b{}, c{};
    if (fstatat(dfd, n.c_str(), &a, AT_SYMLINK_NOFOLLOW))
    {
      Reason(reasons, "entry_stat_failed");
      continue;
    }
    if (!AllowedName(n))
    {
      Reason(reasons, n.ends_with(".part")?"part_file":"invalid_filename");
      continue;
    }
    if (!S_ISREG(a.st_mode))
    {
      Reason(reasons, S_ISLNK(a.st_mode)?"symlink":"special_file");
      continue;
    }
    if (a.st_nlink != 1)
    {
      Reason(reasons, "hardlink");
      continue;
    }
    if (a.st_uid != getuid())
    {
      Reason(reasons, "wrong_owner");
      continue;
    }
    if (a.st_mode & 022)
    {
      Reason(reasons, "writable_by_group_or_world");
      continue;
    }
    if (a.st_size < 0 || uint64_t(a.st_size) > kMaxMember ||
        total > kMaxArchive - uint64_t(a.st_size))
    {
      Reason(reasons, "source_size_limit");
      continue;
    }
    int fd = openat(dfd, n.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
    {
      Reason(reasons, "entry_open_failed");
      continue;
    }
    bool ok = false;
    std::string sha = HashFd(fd, &ok);
    bool stable = fstat(fd, &b) == 0 && SameStat(a, b) && ok &&
                  fstat(fd, &c) == 0 && SameStat(b, c);
    if (close(fd) != 0)
    {
      stable = false;
    }
    if (!stable)
    {
      Reason(reasons, "entry_mutated");
      continue;
    }
    total += uint64_t(b.st_size);
    out.push_back({n, sha, b.st_size, b.st_dev, b.st_ino, b.st_mtim});
  }
  if (closedir(d) != 0)
  {
    Reason(reasons, "scan_close_failed");
  }
  std::sort(out.begin(), out.end(), [](const File&a, const File&b){
      return a.name < b.name;
    });
  return out;
}
bool SameScans(const std::vector<File>&a, const std::vector<File>&b)
{
  if (a.size() != b.size())
  {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (a[i].name != b[i].name || a[i].size != b[i].size || a[i].dev != b[i].dev ||
        a[i].ino != b[i].ino || a[i].mt.tv_sec != b[i].mt.tv_sec ||
        a[i].mt.tv_nsec != b[i].mt.tv_nsec || a[i].sha != b[i].sha)
    {
      return false;
    }
  }
  return true;
}
std::string TreeHash(const std::vector<File>&fs)
{
  Sha256 h;
  for (const auto&f:fs)
  {
    h.Add(f.name.data(), f.name.size());
    char z = 0;
    h.Add(&z, 1);
    auto size = std::to_string(f.size);
    h.Add(size.data(), size.size());
    h.Add(&z, 1);
    h.Add(f.sha.data(), f.sha.size());
    char nl = '\n';
    h.Add(&nl, 1);
  }
  return h.Final();
}
void Octal(char*p, size_t n, uint64_t v)
{
  snprintf(p, n, "%0*llo", int(n - 1), (unsigned long long)v);
}
bool TarHeader(int fd, const std::string&name, uint64_t size)
{
  std::array<char, 512>h{};
  memcpy(h.data(), name.data(), name.size());
  Octal(h.data() + 100, 8, 0600);
  Octal(h.data() + 108, 8, 0);
  Octal(h.data() + 116, 8, 0);
  Octal(h.data() + 124, 12, size);
  Octal(h.data() + 136, 12, 0);
  memset(h.data() + 148, ' ', 8);
  h[156] = '0';
  memcpy(h.data() + 257, "ustar", 5);
  h[262] = 0;
  memcpy(h.data() + 263, "00", 2);
  unsigned sum = 0;
  for (unsigned char c:h)
  {
    sum += c;
  }
  snprintf(h.data() + 148, 8, "%06o", sum);
  h[154] = 0;
  h[155] = ' ';
  return WriteAll(fd, h.data(), h.size());
}
bool TarEntry(int fd, const std::string&name, const std::string&data)
{
  if (!TarHeader(fd, name, data.size()) || !WriteAll(fd, data.data(), data.size()))
  {
    return false;
  }
  std::array<char, 512>z{};
  size_t pad = (512 - data.size() % 512) % 512;
  return !pad || WriteAll(fd, z.data(), pad);
}
bool TarFile(int out, int dfd, const File&file)
{
  int in = openat(dfd, file.name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (in < 0)
  {
    return false;
  }
  struct stat before {}, after{};
  bool ok =
    fstat(in,
          &before) == 0 && before.st_nlink == 1 && before.st_uid == getuid() &&
    !(before.st_mode & 022) &&
    before.st_dev == file.dev && before.st_ino == file.ino && before.st_size == file.size &&
    TarHeader(out, file.name, file.size);
  Sha256 hash;
  std::array<char, 65536>buffer{};
  uint64_t copied = 0;
  while (ok && copied < uint64_t(file.size))
  {
    size_t want = std::min<uint64_t>(buffer.size(), uint64_t(file.size) - copied);
    ssize_t n = read(in, buffer.data(), want);
    if (n < 0 && errno == EINTR)
    {
      continue;
    }
    if (n <= 0)
    {
      ok = false;
      break;
    }
    hash.Add(buffer.data(), size_t(n));
    ok = WriteAll(out, buffer.data(), size_t(n));
    copied += size_t(n);
  }
  char extra;
  ssize_t extra_count;
  do
  {
    extra_count = read(in, &extra, 1);
  }
  while(extra_count < 0 && errno == EINTR);
  ok = ok && extra_count == 0 && fstat(in, &after) == 0 &&
       SameStat(before, after) && hash.Final() == file.sha;
  if (close(in) != 0)
  {
    ok = false;
  }
  std::array<char, 512>zero{};
  size_t pad = (512 - uint64_t(file.size) % 512) % 512;
  return ok && (!pad || WriteAll(out, zero.data(), pad));
}
bool RenameNoReplace(int rootfd, const std::string&tmp, const std::string&final)
{
#if defined(SYS_renameat2)
  return syscall(SYS_renameat2, rootfd, tmp.c_str(), rootfd, final.c_str(), 1U) == 0;
#else
  if (linkat(rootfd, tmp.c_str(), rootfd, final.c_str(), 0) != 0)
  {
    return false;
  }
  if (unlinkat(rootfd, tmp.c_str(), 0) == 0)
  {
    return true;
  }
  unlinkat(rootfd, final.c_str(), 0);
  return false;
#endif
}
bool PublishTar(int rootfd, int dirfd, const std::string&tmp, const std::string&final,
                const std::string&manifest, const std::vector<File>&files, std::string*sha,
                uint64_t*size)
{
  int fd = openat(rootfd, tmp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
  {
    return false;
  }
  bool ok = fchmod(fd, 0600) == 0 && TarEntry(fd, "device_capture_manifest.json", manifest);
  for (const auto&f:files)
  {
    ok = ok && TarFile(fd, dirfd, f);
  }
  std::array<char, 1024>z{};
  ok = ok && WriteAll(fd, z.data(), z.size()) && fdatasync(fd) == 0;
  struct stat retained {}, published{};
  ok = ok && fstat(fd, &retained) == 0 && uint64_t(retained.st_size) <= kMaxArchive;
  if (!ok)
  {
    unlinkat(rootfd, tmp.c_str(), 0);
    close(fd);
    return false;
  }
  if (!RenameNoReplace(rootfd, tmp, final))
  {
    unlinkat(rootfd, tmp.c_str(), 0);
    close(fd);
    return false;
  }
  if (fsync(rootfd) != 0 ||
      fstatat(rootfd, final.c_str(), &published,
              AT_SYMLINK_NOFOLLOW) != 0 || retained.st_dev != published.st_dev ||
      retained.st_ino != published.st_ino)
  {
    unlinkat(rootfd, final.c_str(), 0);
    close(fd);
    return false;
  }
  *sha = HashFd(fd, &ok);
  *size = uint64_t(retained.st_size);
  if (close(fd) != 0)
  {
    ok = false;
  }
  if (!ok)
  {
    unlinkat(rootfd, final.c_str(), 0);
    sha->clear();
    *size = 0;
  }
  return ok;
}
std::string ReasonsJson(const std::vector<std::string>&r)
{
  std::string s = "[";
  for (size_t i = 0; i < r.size(); ++i)
  {
    if (i)
    {
      s += ',';
    }
    s += J(r[i]);
  }
  return s + "]";
}
void Result(bool valid, const std::string&challenge, const std::string&archive,
            const std::string&sha, uint64_t size, pid_t pid, uint64_t ticks,
            const std::vector<std::string>&reasons)
{
  std::ostringstream o;
  o << "{\"protocol\":\"mw_perf.ohos_remote_capture.result.v1\",\"snapshot_valid\":" <<
    (valid?"true":"false") << ",\"semantic_validation\":false,\"archive_path\":" << J(archive) <<
    ",\"archive_sha256\":" << J(sha) << ",\"archive_size\":" << size << ",\"child_pid\":" << pid <<
    ",\"child_start_time_ticks\":" << ticks << ",\"request_challenge\":" << J(challenge) <<
    ",\"reasons\":" << ReasonsJson(reasons) << '}';
  std::string s = o.str();
  if (s.size() > kMaxResult)
  {
    s =
      "{\"protocol\":\"mw_perf.ohos_remote_capture.result.v1\",\"snapshot_valid\":false,\"semantic_validation\":false,\"reasons\":[\"result_too_large\"]}";
  }
  std::cout << s << '\n';
}
}

int main(int argc, char**argv)
{
  Request r;
  std::string why;
  if (!Parse(argc, argv, &r, &why))
  {
    Result(false, r.challenge, "", "", 0, 0, 0, {why});
    return 2;
  }
  int rootfd = OpenAbsolute(r.root, O_RDONLY | O_DIRECTORY | O_CLOEXEC), exefd = OpenAbsolute(
    r.argv[0], O_RDONLY | O_CLOEXEC);
  struct stat rootst {}, exest{};
  if (rootfd < 0 ||
      fstat(rootfd,
            &rootst) != 0 || !S_ISDIR(rootst.st_mode) || rootst.st_uid != getuid() ||
      (rootst.st_mode & 077))
  {
    if (rootfd >= 0)
    {
      close(rootfd);
    }
    if (exefd >= 0)
    {
      close(exefd);
    }
    Result(false, r.challenge, "", "", 0, 0, 0, {"invalid_run_root"});
    return 2;
  }
  if (exefd < 0 || fstat(exefd, &exest) != 0 || !S_ISREG(exest.st_mode) || !(exest.st_mode & 0111))
  {
    if (exefd >= 0)
    {
      close(exefd);
    }
    close(rootfd);
    Result(false, r.challenge, "", "", 0, 0, 0, {"invalid_sut_executable"});
    return 2;
  }
  std::string nonce = Nonce(), dirname = "capture-" + nonce;
  if (nonce.empty() || mkdirat(rootfd, dirname.c_str(), 0700) != 0)
  {
    close(exefd);
    close(rootfd);
    Result(false, r.challenge, "", "", 0, 0, 0, {"capture_directory_create_failed"});
    return 2;
  }
  int dirfd = openat(rootfd, dirname.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct stat dirst {}, pathst{};
  if (dirfd < 0 ||
      fstat(dirfd,
            &dirst) != 0 ||
      fstatat(rootfd, dirname.c_str(), &pathst,
              AT_SYMLINK_NOFOLLOW) != 0 || dirst.st_dev != pathst.st_dev ||
      dirst.st_ino != pathst.st_ino ||
      dirst.st_uid != getuid() || fchmod(dirfd, 0700) != 0)
  {
    if (dirfd >= 0)
    {
      close(dirfd);
    }
    close(exefd);
    close(rootfd);
    Result(false, r.challenge, "", "", 0, 0, 0, {"capture_directory_open_failed"});
    return 2;
  }
  int flags = fcntl(dirfd, F_GETFD);
  struct sigaction action {};
  action.sa_handler = ForwardSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (flags < 0 ||
      fcntl(dirfd, F_SETFD,
            flags & ~FD_CLOEXEC) != 0 ||
      prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0,
            0) != 0 ||
      sigaction(SIGTERM, &action,
                nullptr) != 0 ||
      sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGHUP, &action, nullptr) != 0)
  {
    close(dirfd);
    close(exefd);
    close(rootfd);
    Result(false, r.challenge, "", "", 0, 0, 0, {"containment_setup_failed"});
    return 2;
  }
  std::vector<std::string>envs;
  for (const auto&n:r.env)
  {
    envs.push_back(n + "=" + getenv(n.c_str()));
  }
  auto add = [&](const std::string&n, const std::string&v){
               envs.push_back(n + "=" + v);
             };
  add("MW_PERF_RUN_ID", r.run);
  add("MW_PERF_CAPABILITY_ID", r.cap);
  add("MW_PERF_PATH_ID", r.path);
  add("MW_PERF_DEPLOYMENT_MODE", "managed");
  add("MW_PERF_COMM_TRACE_DIR_FD", std::to_string(dirfd));
  add("MW_PERF_TOPOLOGY_GENERATION", r.gen);
  add("MW_PERF_TOPOLOGY_SHA256", r.topo);
  add("IBMW_COMM_TRACE", r.mode);
  add("IBMW_COMM_TRACE_SINK", "managed");
  if (r.exact)
  {
    add("MW_PERF_EXACT_REQUIRED", "1");
  }
  std::vector<char*>av, ev;
  for (auto&s:r.argv)
  {
    av.push_back(s.data());
  }
  av.push_back(nullptr);
  for (auto&s:envs)
  {
    ev.push_back(s.data());
  }
  ev.push_back(nullptr);
  int pipefd[2];
  int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (nullfd < 0 || pipe2(pipefd, O_CLOEXEC) != 0)
  {
    if (nullfd >= 0)
    {
      close(nullfd);
    }
    close(dirfd);
    close(exefd);
    close(rootfd);
    Result(false, r.challenge, "", "", 0, 0, 0, {"status_pipe_failed"});
    return 2;
  }
  const pid_t expected_parent = getpid();
  pid_t pid = -1;
#if defined(IBMW_CAPTURE_TEST_HOOKS)
  if (getenv("IBMW_CAPTURE_INJECT_FORK_FAILURE") == nullptr)
  {
    pid = fork();
  }
  else
  {
    errno = EAGAIN;
  }
#else
  pid = fork();
#endif
  if (pid < 0)
  {
    close(pipefd[0]);
    close(pipefd[1]);
    close(nullfd);
    close(dirfd);
    close(exefd);
    close(rootfd);
    Result(false, r.challenge, "", "", 0, 0, 0, {"fork_failed"});
    return 2;
  }
  if (pid == 0)
  {
    int e = 0;
    if (close(pipefd[0]) != 0 ||
        setpgid(0,
                0) != 0 ||
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0,
              0) != 0 || getppid() != expected_parent ||
        dup2(nullfd, STDIN_FILENO) < 0 ||
        dup2(nullfd, STDOUT_FILENO) < 0 ||
        dup2(nullfd, STDERR_FILENO) < 0 ||
        !CloseOtherFds(dirfd, pipefd[1], exefd))
    {
      e = errno?errno:EIO;
      WriteAll(pipefd[1], &e, sizeof e);
      _exit(126);
    }
    fexecve(exefd, av.data(), ev.data());
    e = errno;
    WriteAll(pipefd[1], &e, sizeof e);
    _exit(127);
  }
  std::vector<std::string>reasons;
  if (close(nullfd) != 0)
  {
    Reason(&reasons, "close_failed");
  }
  if (close(pipefd[1]) != 0)
  {
    Reason(&reasons, "close_failed");
  }
  if (setpgid(pid, pid) != 0 && errno != EACCES)
  {
    Reason(&reasons, "setpgid_failed");
  }
  if (close(exefd) != 0)
  {
    Reason(&reasons, "close_failed");
  }
  uint64_t ticks = 0;
  for (int i = 0; i < 100 && ticks == 0; ++i)
  {
    ProcStart(pid, &ticks);
    if (!ticks)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (!ticks)
  {
    Reason(&reasons, "child_start_ticks_unavailable");
  }
  int execerr = 0;
  ssize_t en;
  do en = read(pipefd[0], &execerr, sizeof execerr);
  while(en < 0 && errno == EINTR);
  if (close(pipefd[0]) != 0)
  {
    Reason(&reasons, "close_failed");
  }
  if (en != 0)
  {
    Reason(&reasons, en == sizeof execerr?"exec_failed":"exec_status_failed");
  }
  int status = 0;
  bool reaped = false, timedout = false, desc = false, adopted_reaped = false;
  std::map<pid_t, uint64_t> known;
  if (ticks)
  {
    known[pid] = ticks;
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(r.timeout);
  while (!reaped)
  {
    if (DiscoverDescendants(pid, &known))
    {
      desc = true;
      SignalLeaderGroup(pid, ticks, SIGTERM, &reasons);
      IBMW_SIGNAL_IDENTITIES(known, SIGTERM, pid, &reasons);
    }
    ReapNonblocking(pid, &status, &reaped, &adopted_reaped, &reasons);
    desc = desc || adopted_reaped || known.size() > 1;
    if (reaped)
    {
      break;
    }
    if (g_forwarded_signal != 0)
    {
      const int forwarded = static_cast<int>(g_forwarded_signal);
      g_forwarded_signal = 0;
      SignalLeaderGroup(pid, ticks, forwarded, &reasons);
      IBMW_SIGNAL_IDENTITIES(known, forwarded, pid, &reasons);
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      timedout = true;
      SignalLeaderGroup(pid, ticks, SIGTERM, &reasons);
      IBMW_SIGNAL_IDENTITIES(known, SIGTERM, pid, &reasons);
      auto grace = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
      while (std::chrono::steady_clock::now() < grace)
      {
        if (DiscoverDescendants(pid, &known))
        {
          desc = true;
        }
        IBMW_SIGNAL_IDENTITIES(known, SIGTERM, pid, &reasons);
        ReapNonblocking(pid, &status, &reaped, &adopted_reaped, &reasons);
        desc = desc || adopted_reaped || known.size() > 1;
        if (reaped) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!reaped)
      {
        Reason(&reasons, "sigkill_escalation");
        SignalLeaderGroup(pid, ticks, SIGKILL, &reasons);
        IBMW_SIGNAL_IDENTITIES(known, SIGKILL, pid, &reasons);
        auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < kill_deadline && !reaped)
        {
          DiscoverDescendants(pid, &known);
          IBMW_SIGNAL_IDENTITIES(known, SIGKILL, pid, &reasons);
          ReapNonblocking(pid, &status, &reaped, &adopted_reaped, &reasons);
          if (!reaped) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!reaped) Reason(&reasons, "reap_deadline_exceeded");
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  DiscoverDescendants(pid, &known);
  IBMW_SIGNAL_IDENTITIES(known, SIGTERM, pid, &reasons);
  auto adopted_grace = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < adopted_grace)
  {
    if (DiscoverDescendants(pid, &known)) desc = true;
    IBMW_SIGNAL_IDENTITIES(known, SIGTERM, pid, &reasons);
    ReapNonblocking(pid, &status, &reaped, &adopted_reaped, &reasons);
    desc = desc || adopted_reaped || known.size() > 1;
    if (std::none_of(known.begin(), known.end(), [](const auto& item){
          return IdentityAlive(item.first, item.second);
        })) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  IBMW_SIGNAL_IDENTITIES(known, SIGKILL, pid, &reasons);
  auto adopted_kill = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < adopted_kill)
  {
    DiscoverDescendants(pid, &known);
    IBMW_SIGNAL_IDENTITIES(known, SIGKILL, pid, &reasons);
    ReapNonblocking(pid, &status, &reaped, &adopted_reaped, &reasons);
    if (std::none_of(known.begin(), known.end(), [](const auto& item){
          return IdentityAlive(item.first, item.second);
        })) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  DiscoverDescendants(pid, &known);
  ReapNonblocking(pid, &status, &reaped, &adopted_reaped, &reasons);
  bool identity_survived = false;
  for (const auto&[recorded_pid, recorded_start]:known)
  {
    if (IdentityAlive(recorded_pid, recorded_start))
    {
      identity_survived = true;
      Reason(&reasons, recorded_pid == pid?"leader_identity_survived":"descendant_identity_survived");
    }
  }
  if (identity_survived)
  {
    if (close(dirfd) != 0 || close(rootfd) != 0)
    {
      Reason(&reasons, "close_failed");
    }
    Result(false, r.challenge, "", "", 0, pid, ticks, reasons);
    return 1;
  }
  if (desc)
  {
    Reason(&reasons, "descendant_detected");
  }
  if (timedout)
  {
    Reason(&reasons, "timeout");
  }
  if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
  {
    Reason(&reasons, "child_not_zero_exit");
  }
  auto first = Scan(dirfd, &reasons);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto second = Scan(dirfd, &reasons);
  if (!SameScans(first, second))
  {
    Reason(&reasons, "entries_changed_between_scans");
  }
  std::string tree = TreeHash(second), selfsha;
  int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  bool hashok = false;
  if (self >= 0)
  {
    selfsha = HashFd(self, &hashok);
  }
  if (self < 0 || !hashok || close(self) != 0)
  {
    Reason(&reasons, "agent_hash_failed");
  }
  std::string boot = BootId();
  if (boot.empty())
  {
    Reason(&reasons, "boot_id_unavailable");
  }
  bool valid = reasons.empty();
  std::ostringstream m;
  m << "{\"protocol\":\"mw_perf.ohos_remote_capture.v1\",\"nonce\":" << J(nonce) <<
    ",\"request_challenge\":" << J(r.challenge) << ",\"run_id\":" << J(r.run) <<
    ",\"capability_id\":" << J(r.cap) << ",\"path_id\":" <<
  J(r.path) <<
    ",\"topology_generation\":" << J(r.gen) << ",\"topology_sha256\":" << J(r.topo) <<
    ",\"trace_mode\":" << J(r.mode) << ",\"exact_required\":" << (r.exact?"true":"false") <<
    ",\"boot_id\":" << J(boot) << ",\"agent_executable_sha256\":" << J(selfsha) <<
    ",\"request_sha256\":" << J(Hash(CanonicalRequest(r))) << ",\"child_pid\":" << pid <<
    ",\"child_start_time_ticks\":" << ticks << ",\"wait_status\":" << status << ",\"graceful\":" <<
    ((reaped && WIFEXITED(status) &&
      !timedout)?"true":"false") << ",\"no_escalation\":" <<
    (std::find(reasons.begin(), reasons.end(),
               "sigkill_escalation") ==
     reasons.end()?"true":"false") <<
  ",\"descendant_policy\":\"forbid\",\"descendant_detected\":" <<
    (desc?"true":"false") << ",\"semantic_validation\":false,\"source_files\":[";
  for (size_t i = 0; i < second.size(); ++i)
  {
    if (i)
    {
      m << ',';
    }
    m << "{\"name\":" << J(second[i].name) << ",\"size\":" << second[i].size << ",\"sha256\":" << J(
      second[i].sha) << '}';
  }
  m << "],\"archive_tree_sha256\":" << J(tree) << ",\"snapshot_valid\":" <<
    (valid?"true":"false") << ",\"reasons\":" << ReasonsJson(reasons) << '}';
  std::string final = "capture-" + r.challenge + ".tar";
  std::string tmp = "." + final + ".tmp-" + nonce, archive_sha;
  uint64_t archive_size = 0;
  if (!PublishTar(rootfd, dirfd, tmp, final, m.str(), second, &archive_sha, &archive_size))
  {
    Reason(&reasons, "archive_publish_failed");
    valid = false;
  }
  if (close(dirfd) != 0 || close(rootfd) != 0)
  {
    Reason(&reasons, "close_failed");
    valid = false;
  }
  Result(valid, r.challenge, valid?r.root + "/" + final:"", valid?archive_sha:"",
         valid?archive_size:0, pid, ticks, reasons);
  return valid?0:1;
}
