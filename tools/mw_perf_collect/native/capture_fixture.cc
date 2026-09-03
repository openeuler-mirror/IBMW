#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef IBMW_VALID_TRACE_FIXTURE
#define IBMW_VALID_TRACE_FIXTURE ""
#endif

namespace {

int TraceDirectoryFd()
{
  const char* value = getenv("MW_PERF_COMM_TRACE_DIR_FD");
  return value == nullptr ? -1 : atoi(value);
}

bool Put(const char* name, const char* value, mode_t mode = 0600)
{
  const int fd = openat(TraceDirectoryFd(), name,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
  if (fd < 0)
  {
    return false;
  }
  size_t remaining = strlen(value);
  while (remaining != 0)
  {
    const ssize_t count = write(fd, value, remaining);
    if (count < 0 && errno == EINTR)
    {
      continue;
    }
    if (count <= 0)
    {
      close(fd);
      return false;
    }
    value += count;
    remaining -= static_cast<size_t>(count);
  }
  return close(fd) == 0;
}

unsigned long long StartTicks()
{
  std::ifstream file("/proc/self/stat");
  std::string text;
  getline(file, text);
  const size_t right_parenthesis = text.rfind(')');
  std::istringstream fields(text.substr(right_parenthesis + 2));
  std::string value;
  for (int field = 3; field <= 22; ++field)
  {
    fields >> value;
    if (field == 22)
    {
      return strtoull(value.c_str(), nullptr, 10);
    }
  }
  return 0;
}

int Happy()
{
  const std::string metadata =
    "{\"producer_pid\":" + std::to_string(getpid()) +
    ",\"producer_start_time_ticks\":" + std::to_string(StartTicks()) +
    ",\"run_id\":\"run-1\"}\n";
  return Put("ibmw_dds_trace.1.a.meta.json", metadata.c_str()) &&
         Put("ibmw_dds_trace.1.a.00000000.ibct", "trace") &&
         Put("ibmw_dds_trace.1.a.seal.json", "{}\n")
       ? 0
       : 10;
}

int CaptureFd(const char* name)
{
  const char* value = getenv(name);
  return value == nullptr ? -1 : atoi(value);
}

bool V2Gate()
{
  const int control = CaptureFd("MW_PERF_CAPTURE_CONTROL_FD");
  const char marker = 'R';
  if (control < 0 || send(control, &marker, 1, 0) != 1) return false;
  pollfd descriptor{control, POLLIN, 0};
  if (poll(&descriptor, 1, 5000) != 1) return false;
  char grant = 0;
  return recv(control, &grant, 1, 0) == 1 && grant == 'G';
}

bool V2Complete()
{
  const int control = CaptureFd("MW_PERF_CAPTURE_CONTROL_FD");
  const char marker = 'C';
  if (control < 0 || send(control, &marker, 1, 0) != 1) return false;
  pollfd descriptor{control, POLLIN | POLLHUP, 0};
  if (poll(&descriptor, 1, 5000) != 1) return false;
  char evidence = 0;
  const bool granted = recv(control, &evidence, 1, 0) == 1 && evidence == 'E';
  return close(control) == 0 && granted;
}

bool V2CompleteWithImmediateZeroPacket()
{
  const int control = CaptureFd("MW_PERF_CAPTURE_CONTROL_FD");
  const char marker = 'C';
  if (control < 0 || send(control, &marker, 1, 0) != 1) return false;
  pollfd descriptor{control, POLLIN | POLLHUP, 0};
  if (poll(&descriptor, 1, 5000) != 1) return false;
  char evidence = 0;
  if (recv(control, &evidence, 1, 0) != 1 || evidence != 'E') return false;
  if (send(control, nullptr, 0, 0) != 0) return false;
  return close(control) == 0;
}

bool V2CompleteWithRetainedDescendant()
{
  const int control = CaptureFd("MW_PERF_CAPTURE_CONTROL_FD");
  const char marker = 'C';
  if (control < 0 || send(control, &marker, 1, 0) != 1) return false;
  pollfd descriptor{control, POLLIN | POLLHUP, 0};
  if (poll(&descriptor, 1, 5000) != 1) return false;
  char evidence = 0;
  if (recv(control, &evidence, 1, 0) != 1 || evidence != 'E') return false;
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0)
  {
    sleep(30);
    close(control);
    _exit(0);
  }
  return close(control) == 0;
}

bool DescendantMarker(char marker)
{
  const int control = CaptureFd("MW_PERF_CAPTURE_CONTROL_FD");
  const pid_t child = fork();
  if (control < 0 || child < 0) return false;
  if (child == 0) {
    const bool sent = send(control, &marker, 1, 0) == 1;
    close(control);
    _exit(sent ? 0 : 17);
  }
  int status = 0;
  return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool OnlyIntendedFds()
{
  DIR* directory = opendir("/proc/self/fd");
  if (directory == nullptr)
  {
    return false;
  }
  const int scan_fd = dirfd(directory);
  bool valid = true;
  while (const dirent* entry = readdir(directory))
  {
    char* end = nullptr;
    const long fd = strtol(entry->d_name, &end, 10);
    if (end == entry->d_name || *end != '\0')
    {
      continue;
    }
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO &&
        fd != TraceDirectoryFd() && fd != scan_fd)
    {
      valid = false;
    }
  }
  return closedir(directory) == 0 && valid;
}

}  // namespace

int main(int argc, char** argv)
{
  const std::string mode = argc > 1 ? argv[1] : "happy";
  if (mode == "happy")
  {
    return Happy();
  }
  if (mode == "v2" || mode == "v2-no-completion")
  {
    if (!V2Gate() || Happy() != 0) return 14;
    return mode == "v2-no-completion" || V2Complete() ? 0 : 15;
  }
  if (mode == "v2-descendant-retains-control")
  {
    if (!V2Gate() || Happy() != 0) return 22;
    return V2CompleteWithRetainedDescendant() ? 0 : 23;
  }
  if (mode == "v2-zero-packet-immediate-close")
  {
    if (!V2Gate() || Happy() != 0) return 24;
    return V2CompleteWithImmediateZeroPacket() ? 0 : 25;
  }
  if (mode == "v2-descendant-ready")
  {
    return DescendantMarker('R') ? 0 : 18;
  }
  if (mode == "v2-descendant-completion")
  {
    if (!V2Gate() || Happy() != 0) return 19;
    return DescendantMarker('C') ? 0 : 20;
  }
  if (mode == "v2-delayed-ready")
  {
    usleep(100000);
    return V2Gate() ? 0 : 21;
  }
  if (mode == "v2-valid-trace" || mode == "v2-exec-after-g")
  {
    if (!V2Gate() || setenv("MW_PERF_CAPTURE_GATE_COMPLETE", "1", 1) != 0) return 16;
    execl("/usr/bin/python3", "python3", IBMW_VALID_TRACE_FIXTURE,
          static_cast<char*>(nullptr));
    return 12;
  }
  if (mode == "valid-trace")
  {
    execl("/usr/bin/python3", "python3", IBMW_VALID_TRACE_FIXTURE,
          static_cast<char*>(nullptr));
    return 12;
  }
  if (mode == "invalid-json")
  {
    return Put("ibmw_dds_trace.1.a.meta.json", "{not json\n") &&
           Put("ibmw_dds_trace.1.a.00000000.ibct", "trace") &&
           Put("ibmw_dds_trace.1.a.seal.json", "{}\n")
         ? 0
         : 10;
  }
  if (mode == "fd-check")
  {
    return OnlyIntendedFds() ? Happy() : 11;
  }
  if (mode == "nonzero")
  {
    return 7;
  }
  if (mode == "sleep")
  {
    sleep(2);
    return 0;
  }
  if (mode == "ignore")
  {
    signal(SIGTERM, SIG_IGN);
    sleep(2);
    return 0;
  }
  if (mode == "symlink")
  {
    return symlinkat("/etc/passwd", TraceDirectoryFd(),
                     "ibmw_dds_trace.1.a.meta.json") == 0
         ? 0
         : 10;
  }
  if (mode == "hardlink")
  {
    if (!Put("ibmw_dds_trace.1.a.meta.json", "x"))
    {
      return 10;
    }
    return linkat(TraceDirectoryFd(), "ibmw_dds_trace.1.a.meta.json",
                  TraceDirectoryFd(), "ibmw_dds_trace.1.b.meta.json", 0) == 0
         ? 0
         : 10;
  }
  if (mode == "fifo")
  {
    return mkfifoat(TraceDirectoryFd(),
                    "ibmw_dds_trace.1.a.00000000.ibct", 0600) == 0
         ? 0
         : 10;
  }
  if (mode == "part")
  {
    return Put("ibmw_dds_trace.1.a.meta.json.part", "x") ? 0 : 10;
  }
  if (mode == "unknown")
  {
    return Put("bad.txt", "x") ? 0 : 10;
  }
  if (mode == "writable")
  {
    return Put("ibmw_dds_trace.1.a.meta.json", "x", 0666) ? 0 : 10;
  }
  if (mode == "oversize")
  {
    const int fd = openat(TraceDirectoryFd(),
                          "ibmw_dds_trace.1.a.00000000.ibct",
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
    {
      return 10;
    }
    const bool ok = ftruncate(fd, 129LL * 1024 * 1024) == 0;
    return close(fd) == 0 && ok ? 0 : 10;
  }
  if (mode == "mutable")
  {
    const int fd = openat(TraceDirectoryFd(),
                          "ibmw_dds_trace.1.a.00000000.ibct",
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
    {
      return 10;
    }
    const std::string block(1024 * 1024, 'a');
    const bool ok = write(fd, block.data(), block.size()) ==
                    static_cast<ssize_t>(block.size());
    if (close(fd) != 0)
    {
      return 10;
    }
    usleep(200000);
    return ok ? 0 : 10;
  }
  if (mode == "metachar")
  {
    if (argc != 3 || std::string(argv[2]) !=
        "$(touch /tmp/mw_perf_shell_was_run); x")
    {
      return 8;
    }
    return Happy();
  }
  if (mode == "stdio")
  {
    const char stdout_message[] = "child-stdout-must-not-enter-protocol\n";
    const char stderr_message[] = "child-stderr-must-not-enter-protocol\n";
    char input = 0;
    if (write(STDOUT_FILENO, stdout_message, sizeof(stdout_message) - 1) < 0 ||
        write(STDERR_FILENO, stderr_message, sizeof(stderr_message) - 1) < 0 ||
        read(STDIN_FILENO, &input, 1) != 0)
    {
      return 10;
    }
    return Happy();
  }
  if (mode == "descendant")
  {
    const pid_t child = fork();
    if (child < 0)
    {
      return 10;
    }
    if (child == 0)
    {
      sleep(2);
      _exit(0);
    }
    usleep(200000);
    return 0;
  }
  if (mode == "escaped-descendant")
  {
    const pid_t child = fork();
    if (child < 0)
    {
      return 10;
    }
    if (child == 0)
    {
      if (setsid() < 0)
      {
        _exit(13);
      }
      signal(SIGTERM, SIG_IGN);
      sleep(30);
      _exit(0);
    }
    usleep(100000);
    return 0;
  }
  return 9;
}
