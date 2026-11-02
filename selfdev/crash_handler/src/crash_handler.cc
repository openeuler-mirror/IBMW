// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/crash_handler/crash_handler.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cxxabi.h>
  #include <dlfcn.h>
  #if !defined(__APPLE__)
    #include <link.h>
  #endif
  #include <signal.h>
  #include <sys/syscall.h>
  #include <ucontext.h>
  #include <unistd.h>
  #include <unwind.h>
#endif

namespace ibmw::selfdev::crash {

namespace {

// The report reads the module table while a RefreshModules() may rewrite it
// (seqlock, see g_modules): such reads are discarded when g_module_seq
// changed, so ThreadSanitizer is told not to report them.
#if defined(__GNUC__) || defined(__clang__)
  #define IBMW_CRASH_SEQLOCK_READER __attribute__((no_sanitize("thread")))
#else
  #define IBMW_CRASH_SEQLOCK_READER
#endif

// Bounded, allocation-free string builder. Keeps buf NUL terminated.
class Writer {
 public:
  Writer(char* buf, size_t cap) noexcept : buf_(buf), cap_(cap) {
    if (cap_ > 0) buf_[0] = '\0';
  }

  void Put(const char* s) noexcept {
    if (s == nullptr) return;
    while (*s != '\0') PutChar(*s++);
  }

  void PutChar(char c) noexcept {
    if (len_ + 1 >= cap_) return;
    buf_[len_++] = c;
    buf_[len_] = '\0';
  }

  void PutHex(uintptr_t v, int min_width) noexcept {
    char tmp[2 * sizeof(uintptr_t)];
    int n = 0;
    do {
      tmp[n++] = "0123456789abcdef"[v & 0xf];
      v >>= 4;
    } while (v != 0 && n < static_cast<int>(sizeof(tmp)));
    for (int i = n; i < min_width; ++i) PutChar('0');
    while (n > 0) PutChar(tmp[--n]);
  }

  void PutDec(long long v, int min_width = 0) noexcept {
    unsigned long long u = v < 0 ? 0ULL - static_cast<unsigned long long>(v)
                                 : static_cast<unsigned long long>(v);
    if (v < 0) PutChar('-');
    char tmp[24];
    int n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + u % 10);
      u /= 10;
    } while (u != 0);
    for (int i = n; i < min_width; ++i) PutChar('0');
    while (n > 0) PutChar(tmp[--n]);
  }

  size_t Len() const noexcept { return len_; }

 private:
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
};

// Copies the tail of src (the file name matters most) into dst.
void CopyTail(char* dst, size_t cap, const char* src) noexcept {
  size_t len = std::strlen(src);
  size_t from = 0;
  if (len + 1 > cap) {
    from = len - (cap - 4);
    std::memcpy(dst, "...", 3);
    std::memcpy(dst + 3, src + from, cap - 4);
    dst[cap - 1] = '\0';
    return;
  }
  std::memcpy(dst, src, len + 1);
}

void PutSymbolized(Writer& w, const void* pc, bool demangle) noexcept {
#if defined(_WIN32)
  (void)w;
  (void)pc;
  (void)demangle;
#else
  Dl_info info;
  std::memset(&info, 0, sizeof(info));
  if (dladdr(pc, &info) == 0 || info.dli_fname == nullptr) return;

  const auto addr = reinterpret_cast<uintptr_t>(pc);
  w.Put("  ");
  w.Put(info.dli_fname);
  w.Put("+0x");
  w.PutHex(addr - reinterpret_cast<uintptr_t>(info.dli_fbase), 1);

  if (info.dli_sname == nullptr) return;
  w.Put(" (");
  char* demangled = nullptr;
  if (demangle) {
    int status = 0;
    demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
  }
  w.Put(demangled != nullptr ? demangled : info.dli_sname);
  std::free(demangled);
  w.Put("+0x");
  w.PutHex(addr - reinterpret_cast<uintptr_t>(info.dli_saddr), 1);
  w.PutChar(')');
#endif
}

void PutFrame(Writer& w, size_t index, const void* pc, bool symbolize, bool demangle) noexcept {
  w.PutChar('#');
  w.PutDec(static_cast<long long>(index), 2);
  w.Put(" pc 0x");
  w.PutHex(reinterpret_cast<uintptr_t>(pc), 2 * static_cast<int>(sizeof(uintptr_t)));
  if (symbolize) PutSymbolized(w, pc, demangle);
  w.PutChar('\n');
}

}  // namespace

const char* SignalName(int signo) noexcept {
  switch (signo) {
#if !defined(_WIN32)
    case SIGBUS: return "SIGBUS";
    case SIGQUIT: return "SIGQUIT";
    case SIGSYS: return "SIGSYS";
    case SIGTRAP: return "SIGTRAP";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
#endif
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGSEGV: return "SIGSEGV";
    default: return "UNKNOWN";
  }
}

size_t FormatFrame(char* buf, size_t buf_size, size_t index, const void* pc,
                   bool symbolize, bool demangle) noexcept {
  if (buf == nullptr || buf_size == 0) return 0;
  Writer w(buf, buf_size);
  PutFrame(w, index, pc, symbolize, demangle);
  return w.Len();
}

size_t FormatStack(char* buf, size_t buf_size, const void* const* frames, size_t count,
                   bool symbolize, bool demangle) noexcept {
  if (buf == nullptr || buf_size == 0) return 0;
  Writer w(buf, buf_size);
  for (size_t i = 0; i < count; ++i) PutFrame(w, i, frames[i], symbolize, demangle);
  return w.Len();
}

#if defined(_WIN32)

std::vector<int> DefaultSignals() { return {}; }

size_t CaptureStack(void** frames, size_t max_frames, size_t skip) noexcept {
  if (frames == nullptr || max_frames == 0) return 0;
  // +1 skips CaptureStack itself.
  return RtlCaptureStackBackTrace(static_cast<DWORD>(skip + 1), static_cast<DWORD>(max_frames),
                                  frames, nullptr);
}

bool Install(const Options&) { return false; }
void Uninstall() noexcept {}
bool IsInstalled() noexcept { return false; }
void RefreshModules() noexcept {}

#else

namespace {

struct UnwindState {
  void** frames;
  size_t max_frames;
  size_t skip;
  size_t count;
};

_Unwind_Reason_Code UnwindCallback(_Unwind_Context* ctx, void* arg) {
  auto* st = static_cast<UnwindState*>(arg);
  int ip_before_insn = 0;
  uintptr_t pc = _Unwind_GetIPInfo(ctx, &ip_before_insn);
  if (pc == 0) return _URC_END_OF_STACK;
  if (st->skip > 0) {
    --st->skip;
    return _URC_NO_REASON;
  }
  if (st->count >= st->max_frames) return _URC_END_OF_STACK;
  // Return addresses point after the call; step back into the call
  // instruction so symbol and line lookups name the caller correctly.
  if (ip_before_insn == 0) --pc;
  st->frames[st->count++] = reinterpret_cast<void*>(pc);
  return _URC_NO_REASON;
}

struct Config {
  int output_fd = 2;
  size_t max_frames = 64;
  bool symbolize = true;
  ReportCallback report_callback = nullptr;
  void* report_user_data = nullptr;
  bool reraise = true;
  unsigned report_timeout_sec = 10;
};

struct SavedAction {
  int signo;
  struct sigaction action;
};

constexpr size_t kMaxSignals = 64;
constexpr size_t kReportBufSize = 64 * 1024;

// Loaded modules, recorded by Install() / RefreshModules() with
// dl_iterate_phdr so the raw part of a report can print module+offset without
// dladdr (which takes the loader lock, held e.g. while a dlopen'ed library's
// constructors run). Bounded, static; modules loaded after the last refresh
// are not in it and show as bare pcs in the raw part (the symbolized part
// still names them). Never refreshed from the handler: dl_iterate_phdr takes
// the loader lock.
constexpr size_t kMaxModules = 256;
constexpr size_t kModuleNameSize = 256;

struct ModuleEntry {
  uintptr_t begin;  // lowest PT_LOAD address
  uintptr_t end;    // one past the highest PT_LOAD byte
  uintptr_t base;   // load bias (dlpi_addr): offsets are relative to it
  char name[kModuleNameSize];
};

// Two tables: Install() fills the inactive one and then publishes it, so a
// crash during a re-Install() reads a complete table. A second refresh while
// a report is being written would overwrite the table the report reads;
// g_module_seq (incremented before and after every refresh) lets the report
// detect that and print a bare pc instead of a torn entry (see CopyModule).
ModuleEntry g_modules[2][kMaxModules];
size_t g_module_count[2] = {0, 0};
// Modules that did not fit into the table (more than kMaxModules loaded).
size_t g_module_dropped[2] = {0, 0};
std::atomic<int> g_module_active{-1};
std::atomic<unsigned> g_module_seq{0};

std::mutex g_install_mu;
Config g_cfg;
SavedAction g_saved[kMaxSignals];
size_t g_saved_count = 0;
std::atomic<bool> g_installed{false};
std::atomic<long> g_handling_tid{0};
volatile sig_atomic_t g_handling_signo = 0;

// What the reporting thread is doing, so a fault inside the handler can say
// where the report stopped (see HandleSignal).
enum Stage : int { kStageNone, kStageCaller, kStageCapture, kStageSymbolize, kStageCallback };
volatile sig_atomic_t g_stage = kStageNone;

// Signal-context storage; never freed (the kernel may still reference the
// alternate stack after Uninstall()).
char g_report[kReportBufSize];
void* g_frames[kMaxFrames + 1];
char* g_alt_stack = nullptr;
size_t g_alt_stack_size = 0;

long CurrentTid() noexcept {
  #if defined(SYS_gettid)
  return static_cast<long>(syscall(SYS_gettid));
  #else
  return static_cast<long>(getpid());
  #endif
}

void* PcFromContext(void* raw_ctx) noexcept {
  if (raw_ctx == nullptr) return nullptr;
  auto* uctx = static_cast<ucontext_t*>(raw_ctx);
  (void)uctx;
  #if defined(REG_RIP)  // x86_64
  return reinterpret_cast<void*>(uctx->uc_mcontext.gregs[REG_RIP]);
  #elif defined(REG_EIP)  // x86_32
  return reinterpret_cast<void*>(uctx->uc_mcontext.gregs[REG_EIP]);
  #elif defined(__arm__)
  return reinterpret_cast<void*>(uctx->uc_mcontext.arm_pc);
  #elif defined(__aarch64__) && defined(__APPLE__)
  return reinterpret_cast<void*>(uctx->uc_mcontext->__ss.__pc);
  #elif defined(__aarch64__)
  return reinterpret_cast<void*>(uctx->uc_mcontext.pc);
  #elif defined(__riscv)
  return reinterpret_cast<void*>(uctx->uc_mcontext.__gregs[REG_PC]);
  #elif defined(__APPLE__) && defined(__x86_64__)
  return reinterpret_cast<void*>(uctx->uc_mcontext->__ss.__rip);
  #else
  return nullptr;
  #endif
}

// Return address of the frame that made the call to `pc`, when `pc` was
// reached by a call: the word at [sp] on x86_64, LR on AArch64. Reads the
// interrupted stack; a bad sp faults here (handled as a nested fault). Only
// used for an instruction-fetch fault: elsewhere [sp] is whatever the
// interrupted function keeps there (and after a stack overflow, sp points
// into the guard page).
void* CallerFromContext(void* raw_ctx) noexcept {
  if (raw_ctx == nullptr) return nullptr;
  auto* uctx = static_cast<ucontext_t*>(raw_ctx);
  (void)uctx;
  #if defined(__x86_64__) && defined(REG_RSP)
  return *reinterpret_cast<void* const*>(uctx->uc_mcontext.gregs[REG_RSP]);
  #elif defined(__aarch64__) && !defined(__APPLE__)
  return reinterpret_cast<void*>(uctx->uc_mcontext.regs[30]);
  #else
  return nullptr;
  #endif
}

// Registers ResumeAtCaller() changed, so RestoreInterruptedContext() can put
// them back. Written and read by the reporting thread only.
struct MovedContext {
  ucontext_t* uctx;
  uintptr_t pc;
  uintptr_t sp;
};
MovedContext g_moved{nullptr, 0, 0};

// Makes the interrupted context look as if the bad call had returned to
// `caller`, for the stack walk only. The unwinder reads the interrupted
// registers from this context; left at a pc that is not code, libgcc's
// fallback for frames without unwind info reads the instructions at pc and
// faults. The original registers are saved and put back by
// RestoreInterruptedContext() right after the walk, so a core dump or a
// debugger still sees the faulting pc and sp.
bool ResumeAtCaller(void* raw_ctx, void* caller) noexcept {
  if (raw_ctx == nullptr || caller == nullptr) return false;
  auto* uctx = static_cast<ucontext_t*>(raw_ctx);
  (void)uctx;
  #if defined(__x86_64__) && defined(REG_RSP) && defined(REG_RIP)
  g_moved = {uctx, static_cast<uintptr_t>(uctx->uc_mcontext.gregs[REG_RIP]),
             static_cast<uintptr_t>(uctx->uc_mcontext.gregs[REG_RSP])};
  uctx->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(reinterpret_cast<uintptr_t>(caller));
  uctx->uc_mcontext.gregs[REG_RSP] += static_cast<greg_t>(sizeof(void*));
  return true;
  #elif defined(__aarch64__) && !defined(__APPLE__)
  g_moved = {uctx, static_cast<uintptr_t>(uctx->uc_mcontext.pc),
             static_cast<uintptr_t>(uctx->uc_mcontext.sp)};
  uctx->uc_mcontext.pc = reinterpret_cast<uintptr_t>(caller);
  return true;
  #else
  return false;
  #endif
}

// Undoes ResumeAtCaller(); a no-op when the context was not moved.
void RestoreInterruptedContext() noexcept {
  ucontext_t* const uctx = g_moved.uctx;
  if (uctx == nullptr) return;
  #if defined(__x86_64__) && defined(REG_RSP) && defined(REG_RIP)
  uctx->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(g_moved.pc);
  uctx->uc_mcontext.gregs[REG_RSP] = static_cast<greg_t>(g_moved.sp);
  #elif defined(__aarch64__) && !defined(__APPLE__)
  uctx->uc_mcontext.pc = g_moved.pc;
  uctx->uc_mcontext.sp = g_moved.sp;
  #endif
  g_moved.uctx = nullptr;
}

void WriteAll(int fd, const char* data, size_t len) noexcept {
  while (len > 0) {
    ssize_t n = write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
}

IBMW_CRASH_SEQLOCK_READER const ModuleEntry* FindModule(uintptr_t pc) noexcept {
  const int active = g_module_active.load(std::memory_order_acquire);
  if (active < 0) return nullptr;
  const ModuleEntry* table = g_modules[active];
  const size_t n = g_module_count[active];
  for (size_t i = 0; i < n; ++i) {
    if (pc >= table[i].begin && pc < table[i].end) return &table[i];
  }
  return nullptr;
}

// Copies the module containing pc into *out. False when no module contains
// it, or when a RefreshModules() ran meanwhile (the entry may be torn): the
// caller then prints a bare pc.
IBMW_CRASH_SEQLOCK_READER bool CopyModule(uintptr_t pc, ModuleEntry* out) noexcept {
  const unsigned seq = g_module_seq.load(std::memory_order_acquire);
  const ModuleEntry* m = FindModule(pc);
  if (m == nullptr) return false;
  // Byte loop instead of memcpy: the sanitizers intercept memcpy whatever
  // the function attributes, and volatile keeps it from becoming one.
  const volatile unsigned char* src = reinterpret_cast<const volatile unsigned char*>(m);
  unsigned char* dst = reinterpret_cast<unsigned char*>(out);
  for (size_t i = 0; i < sizeof(*out); ++i) dst[i] = src[i];
  std::atomic_thread_fence(std::memory_order_acquire);
  if (g_module_seq.load(std::memory_order_relaxed) != seq) return false;
  out->name[sizeof(out->name) - 1] = '\0';
  return true;
}

// "  /path/to/module+0x1b3c4 [base 0x000055d0c1a10000]" from the module table
// only (async-signal-safe, no loader lock), or `missing` when no module
// contains addr.
void PutModuleOf(Writer& w, uintptr_t addr, const char* missing) noexcept {
  ModuleEntry m;
  if (CopyModule(addr, &m)) {
    w.Put("  ");
    w.Put(m.name);
    w.Put("+0x");
    w.PutHex(addr - m.base, 1);
    w.Put(" [base 0x");
    w.PutHex(m.base, 2 * static_cast<int>(sizeof(uintptr_t)));
    w.PutChar(']');
  } else {
    w.Put(missing);
  }
}

// "*** module table full: N modules not recorded (their frames show bare pcs)"
// when more than kMaxModules modules were loaded at the last refresh. The
// count is read under the same version check as CopyModule(): during a
// concurrent RefreshModules() the marker is left out rather than wrong.
IBMW_CRASH_SEQLOCK_READER void PutModuleTableFull(Writer& w) noexcept {
  const unsigned seq = g_module_seq.load(std::memory_order_acquire);
  const int active = g_module_active.load(std::memory_order_acquire);
  if (active < 0) return;
  const size_t dropped = *static_cast<const volatile size_t*>(&g_module_dropped[active]);
  std::atomic_thread_fence(std::memory_order_acquire);
  if (g_module_seq.load(std::memory_order_relaxed) != seq) return;
  if (dropped == 0) return;
  w.Put("*** module table full: ");
  w.PutDec(static_cast<long long>(dropped));
  w.Put(" modules not recorded (their frames show bare pcs)\n");
}

// "#03 pc 0x000055d0c1a2b3c4  /path/to/module+0x1b3c4 [base 0x000055d0c1a10000]"
void PutRawFrame(Writer& w, size_t index, const void* pc) noexcept {
  w.PutChar('#');
  w.PutDec(static_cast<long long>(index), 2);
  w.Put(" pc 0x");
  const auto addr = reinterpret_cast<uintptr_t>(pc);
  w.PutHex(addr, 2 * static_cast<int>(sizeof(uintptr_t)));
  PutModuleOf(w, addr, "");
  w.PutChar('\n');
}

// Header, async-signal-safe, written before anything touches the interrupted
// stack:
//   *** IBMW crash: signal 11 (SIGSEGV), code 1, fault addr 0x..., pc 0x..., tid N
//   *** pc 0x...  module+0x... [base 0x...]       (or "(not in a loaded module)")
void PutHeader(Writer& w, int signo, const siginfo_t* info, const void* pc, long tid) noexcept {
  const int width = 2 * static_cast<int>(sizeof(uintptr_t));
  w.Put("*** IBMW crash: signal ");
  w.PutDec(signo);
  w.Put(" (");
  w.Put(SignalName(signo));
  w.Put("), code ");
  w.PutDec(info != nullptr ? info->si_code : 0);
  w.Put(", fault addr 0x");
  w.PutHex(reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr : nullptr), width);
  w.Put(", pc 0x");
  w.PutHex(reinterpret_cast<uintptr_t>(pc), width);
  w.Put(", tid ");
  w.PutDec(tid);
  w.Put("\n*** pc 0x");
  w.PutHex(reinterpret_cast<uintptr_t>(pc), width);
  PutModuleOf(w, reinterpret_cast<uintptr_t>(pc), "  (not in a loaded module)");
  w.PutChar('\n');
}

// Only for an instruction-fetch fault (see HandleSignal):
//   *** caller 0x...  module+0x... [base 0x...]  (return address of the call to pc)
void PutCaller(Writer& w, const void* caller) noexcept {
  if (caller == nullptr) return;
  w.Put("*** caller 0x");
  w.PutHex(reinterpret_cast<uintptr_t>(caller), 2 * static_cast<int>(sizeof(uintptr_t)));
  PutModuleOf(w, reinterpret_cast<uintptr_t>(caller), "  (not in a loaded module)");
  w.Put("  (return address of the call to pc)\n");
}

// Part 1, async-signal-safe: raw frames (module+offset from the table
// recorded at Install()). Without symbolization it ends the report.
size_t FormatRawFrames(Writer& w, const void* const* frames, size_t count, bool symbolize) noexcept {
  if (symbolize) w.Put("*** raw stacktrace (no symbol lookup):\n");
  PutModuleTableFull(w);
  for (size_t i = 0; i < count; ++i) PutRawFrame(w, i, frames[i]);
  if (!symbolize) {
    w.Put("*** end of stacktrace (");
    w.PutDec(static_cast<long long>(count));
    w.Put(" frames)\n");
  }
  return w.Len();
}

// Part 2, not async-signal-safe: dladdr may take the loader lock (bounded by
// the watchdog); demangling allocates.
size_t FormatSymbolizedReport(Writer& w, const void* const* frames, size_t count, bool demangle) noexcept {
  w.Put("*** symbolized stacktrace:\n");
  for (size_t i = 0; i < count; ++i) PutFrame(w, i, frames[i], true, demangle);
  w.Put("*** end of stacktrace (");
  w.PutDec(static_cast<long long>(count));
  w.Put(" frames)\n");
  return w.Len();
}

[[noreturn]] void Terminate(int signo) noexcept {
  if (g_cfg.reraise) {
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(signo, &dfl, nullptr);
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, signo);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    raise(signo);
  }
  _exit(128 + signo);
}

// SIGALRM handler armed by the reporting thread: a report that hangs (for
// example malloc deadlocking in the callback after heap corruption, or a
// lock held by a thread parked in HandleSignal) still ends the process with
// the original signal.
void HandleWatchdog(int) {
  if (g_cfg.output_fd >= 0) {
    static const char kMsg[] = "*** IBMW crash: report timed out\n";
    WriteAll(g_cfg.output_fd, kMsg, sizeof(kMsg) - 1);
  }
  Terminate(g_handling_signo);
}

void ArmWatchdog(unsigned seconds) noexcept {
  if (seconds == 0) return;
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = &HandleWatchdog;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_ONSTACK;
  if (sigaction(SIGALRM, &action, nullptr) != 0) return;
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, SIGALRM);
  pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
  alarm(seconds);
}

void HandleSignal(int signo, siginfo_t* info, void* raw_ctx) {
  // A fault inside the handler must re-enter it (see below). Install() keeps
  // the synchronous signals out of sa_mask and sets SA_NODEFER, but a layer
  // in between may not (TSAN's signal emulation runs this handler with the
  // signal blocked): a blocked synchronous signal kills the process with the
  // second signal. pthread_sigmask is async-signal-safe.
  {
    sigset_t sync_signals;
    sigemptyset(&sync_signals);
    for (int sync_sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP}) sigaddset(&sync_signals, sync_sig);
    pthread_sigmask(SIG_UNBLOCK, &sync_signals, nullptr);
  }
  const long tid = CurrentTid();
  long expected = 0;
  if (!g_handling_tid.compare_exchange_strong(expected, tid)) {
    if (expected == tid) {
      // Crashed again inside the handler: say where the report stopped (the
      // header is already out), then end the process as the original signal
      // would have, honouring reraise (false: _exit(128 + original signal)).
      const char* msg = nullptr;
      switch (g_stage) {
        case kStageCaller: msg = "*** IBMW crash: reading the caller address faulted\n"; break;
        case kStageCapture: msg = "*** IBMW crash: stack capture faulted, no stacktrace\n"; break;
        case kStageSymbolize: msg = "*** IBMW crash: symbol lookup faulted\n"; break;
        case kStageCallback: msg = "*** IBMW crash: report callback faulted\n"; break;
        default: break;
      }
      if (msg != nullptr && g_cfg.output_fd >= 0) WriteAll(g_cfg.output_fd, msg, std::strlen(msg));
      // The fault may have hit while the context was moved to the caller.
      RestoreInterruptedContext();
      Terminate(g_handling_signo != 0 ? static_cast<int>(g_handling_signo) : signo);
    }
    // Another thread is reporting and will terminate the process.
    for (;;) pause();
  }

  g_handling_signo = signo;
  ArmWatchdog(g_cfg.report_timeout_sec);

  void* const pc = PcFromContext(raw_ctx);

  // Header first, before anything reads the interrupted stack: if reading
  // it, the unwinder or the symbol lookup faults or hangs, signal, fault
  // address and pc are already on output_fd.
  if (g_cfg.output_fd >= 0) {
    Writer w(g_report, sizeof(g_report));
    PutHeader(w, signo, info, pc, tid);
    WriteAll(g_cfg.output_fd, g_report, w.Len());
  }

  // The fault is the instruction fetch at pc itself (a call through a null
  // or wild function pointer, a jump into data): pc is not code, so the
  // caller names the culprit and the unwinder must start from there (it
  // cannot walk from a pc that is not code). Decided from the fault only,
  // not from the module table: a module loaded after the last refresh is
  // missing from it, and data inside a module is in it.
  void* caller = nullptr;
  bool unwind_from_caller = false;
  if (signo == SIGSEGV && info != nullptr && info->si_addr == pc) {
    g_stage = kStageCaller;
    caller = CallerFromContext(raw_ctx);
    if (g_cfg.output_fd >= 0) {
      Writer w(g_report, sizeof(g_report));
      PutCaller(w, caller);
      WriteAll(g_cfg.output_fd, g_report, w.Len());
    }
    unwind_from_caller = ResumeAtCaller(raw_ctx, caller);
  }

  g_stage = kStageCapture;
  size_t count = CaptureStack(g_frames, g_cfg.max_frames, 0);
  RestoreInterruptedContext();

  // Drop the handler's own frames: start at the interrupted pc (the caller
  // when the context was moved there). If the unwinder could not cross the
  // signal frame, the pc is prepended instead; a moved context gets the
  // original pc prepended as frame #00.
  void** frames = g_frames;
  void* const walk_pc = unwind_from_caller ? caller : pc;
  if (walk_pc != nullptr) {
    size_t start = count;
    for (size_t i = 0; i < count; ++i) {
      if (g_frames[i] == walk_pc) {
        start = i;
        break;
      }
    }
    if (start < count) {
      frames += start;
      count -= start;
    } else {
      if (count == g_cfg.max_frames && count > 0) --count;
      std::memmove(g_frames + 1, g_frames, count * sizeof(void*));
      g_frames[0] = walk_pc;
      ++count;
    }
    if (unwind_from_caller) {
      if (frames == g_frames) {
        if (count == g_cfg.max_frames && count > 0) --count;
        std::memmove(g_frames + 1, g_frames, count * sizeof(void*));
      } else {
        --frames;
      }
      frames[0] = pc;
      if (count < g_cfg.max_frames) ++count;
    }
  }

  g_stage = kStageSymbolize;
  if (g_cfg.output_fd >= 0) {
    // Raw part next, written before any symbol lookup: if dladdr blocks
    // (loader lock held by a thread inside dlopen), the watchdog still finds
    // the header and the raw frames on output_fd.
    {
      Writer w(g_report, sizeof(g_report));
      WriteAll(g_cfg.output_fd, g_report, FormatRawFrames(w, frames, count, g_cfg.symbolize));
    }
    if (g_cfg.symbolize) {
      Writer w(g_report, sizeof(g_report));
      WriteAll(g_cfg.output_fd, g_report, FormatSymbolizedReport(w, frames, count, false));
    }
  }

  if (g_cfg.report_callback != nullptr) {
    g_stage = kStageCallback;
    Writer w(g_report, sizeof(g_report));
    PutHeader(w, signo, info, pc, tid);
    PutCaller(w, caller);
    FormatRawFrames(w, frames, count, g_cfg.symbolize);
    if (g_cfg.symbolize) FormatSymbolizedReport(w, frames, count, true);
    g_cfg.report_callback(g_report, w.Len(), g_cfg.report_user_data);
  }

  Terminate(signo);
}

bool SetupAltStack(size_t size) {
  if (size == 0) return true;
  if (g_alt_stack == nullptr || g_alt_stack_size < size) {
    // A previous, smaller stack is intentionally leaked; see g_alt_stack.
    char* mem = static_cast<char*>(std::malloc(size));
    if (mem == nullptr) return false;
    g_alt_stack = mem;
    g_alt_stack_size = size;
  }
  stack_t ss;
  ss.ss_sp = g_alt_stack;
  ss.ss_size = g_alt_stack_size;
  ss.ss_flags = 0;
  return sigaltstack(&ss, nullptr) == 0;
}

#if !defined(__APPLE__)
struct ModuleScan {
  ModuleEntry* table;
  size_t count;
  size_t dropped;
  const char* exe_name;
};

int AddModule(struct dl_phdr_info* info, size_t, void* arg) {
  auto* scan = static_cast<ModuleScan*>(arg);
  uintptr_t lo = UINTPTR_MAX;
  uintptr_t hi = 0;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const auto& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    const uintptr_t b = static_cast<uintptr_t>(info->dlpi_addr + ph.p_vaddr);
    const uintptr_t e = b + static_cast<uintptr_t>(ph.p_memsz);
    if (b < lo) lo = b;
    if (e > hi) hi = e;
  }
  if (lo >= hi) return 0;
  if (scan->count >= kMaxModules) {
    ++scan->dropped;  // keep counting for the "module table full" marker
    return 0;
  }
  ModuleEntry& m = scan->table[scan->count++];
  m.begin = lo;
  m.end = hi;
  m.base = static_cast<uintptr_t>(info->dlpi_addr);
  const char* name = info->dlpi_name;
  // The main program has an empty name.
  if (name == nullptr || name[0] == '\0') name = scan->exe_name;
  CopyTail(m.name, sizeof(m.name), name);
  return 0;
}
#endif

// Records the loaded modules (see g_modules). Called by Install() and
// RefreshModules(), outside signal context.
void RefreshModuleTable() noexcept {
#if !defined(__APPLE__)
  const int next = g_module_active.load(std::memory_order_relaxed) == 0 ? 1 : 0;
  char exe[kModuleNameSize] = "<main>";
  char path[4096];
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n > 0) {
    path[n] = '\0';
    CopyTail(exe, sizeof(exe), path);
  }
  // Odd while a table is being rewritten; a report that saw another value
  // before reading its entry discards it (CopyModule).
  g_module_seq.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  ModuleScan scan{g_modules[next], 0, 0, exe};
  dl_iterate_phdr(&AddModule, &scan);
  g_module_count[next] = scan.count;
  g_module_dropped[next] = scan.dropped;
  g_module_active.store(next, std::memory_order_release);
  g_module_seq.fetch_add(1, std::memory_order_release);
#endif
}

void UninstallLocked() noexcept {
  // Reverse order, so the oldest saved action wins if a signal ever appears
  // twice. A handler that someone else installed after us is left alone
  // (restoring ours-before-it would silently remove theirs).
  for (size_t i = g_saved_count; i-- > 0;) {
    struct sigaction current;
    std::memset(&current, 0, sizeof(current));
    const bool ours = sigaction(g_saved[i].signo, nullptr, &current) == 0 &&
                      (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == &HandleSignal;
    if (ours) sigaction(g_saved[i].signo, &g_saved[i].action, nullptr);
  }
  g_saved_count = 0;
  g_installed.store(false);
}

}  // namespace

std::vector<int> DefaultSignals() {
  return {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGQUIT, SIGSEGV, SIGSYS, SIGTRAP, SIGXCPU, SIGXFSZ};
}

size_t CaptureStack(void** frames, size_t max_frames, size_t skip) noexcept {
  if (frames == nullptr || max_frames == 0) return 0;
  // +1 skips CaptureStack itself.
  UnwindState st{frames, max_frames, skip + 1, 0};
  _Unwind_Backtrace(&UnwindCallback, &st);
  return st.count;
}

bool Install(const Options& options) {
  std::lock_guard<std::mutex> lock(g_install_mu);
  if (g_installed.load()) UninstallLocked();

  g_cfg.output_fd = options.output_fd;
  g_cfg.max_frames = options.max_frames < kMaxFrames ? options.max_frames : kMaxFrames;
  g_cfg.symbolize = options.symbolize;
  g_cfg.report_callback = options.report_callback;
  g_cfg.report_user_data = options.report_user_data;
  g_cfg.reraise = options.reraise;
  g_cfg.report_timeout_sec = options.report_timeout_sec;
  g_handling_tid.store(0);
  g_stage = kStageNone;
  g_moved.uctx = nullptr;

  // Resolve the PLT slots and unwinder caches of _Unwind_Backtrace and dladdr
  // now, so their first use does not happen in signal context.
  void* warm[4];
  (void)CaptureStack(warm, 4);
  if (g_cfg.symbolize) {
    Dl_info info;
    (void)dladdr(reinterpret_cast<void*>(&HandleSignal), &info);
  }

  RefreshModuleTable();

  bool ok = SetupAltStack(options.alt_stack_size);

  for (int sig : options.signals) {
    bool duplicate = false;
    for (size_t i = 0; i < g_saved_count; ++i) duplicate = duplicate || g_saved[i].signo == sig;
    if (duplicate) continue;
    if (g_saved_count >= kMaxSignals) {
      ok = false;
      break;
    }
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    // No SA_RESETHAND: a second thread faulting while the first one reports
    // must reach HandleSignal (and wait) instead of the default action.
    // Terminate() restores SIG_DFL itself.
    action.sa_flags = static_cast<int>(SA_SIGINFO | SA_ONSTACK | SA_NODEFER);
    // Block asynchronous signals while reporting, but never the synchronous
    // ones: a fault raised while it is blocked (e.g. SIGFPE inside the
    // callback) makes the kernel kill the process with that signal, losing
    // the original one. Unblocked, it re-enters HandleSignal, which ends the
    // process as the original signal.
    sigfillset(&action.sa_mask);
    sigdelset(&action.sa_mask, sig);
    for (int sync_sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP}) sigdelset(&action.sa_mask, sync_sig);
    action.sa_sigaction = &HandleSignal;

    SavedAction saved{};
    saved.signo = sig;
    if (sigaction(sig, &action, &saved.action) < 0) {
      ok = false;
      continue;
    }
    g_saved[g_saved_count++] = saved;
  }

  g_installed.store(g_saved_count > 0);
  return ok;
}

void Uninstall() noexcept {
  std::lock_guard<std::mutex> lock(g_install_mu);
  UninstallLocked();
}

bool IsInstalled() noexcept { return g_installed.load(); }

void RefreshModules() noexcept {
  std::lock_guard<std::mutex> lock(g_install_mu);
  RefreshModuleTable();
}

#endif

}  // namespace ibmw::selfdev::crash
