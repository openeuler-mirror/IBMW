// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <vector>

namespace ibmw::selfdev::crash {

inline constexpr size_t kMaxFrames = 128;

/// Signals caught by default, same set as backward-cpp make_default_signals().
/// Empty on Windows.
std::vector<int> DefaultSignals();

/// Called after the report has been written to output_fd. `report` is NUL
/// terminated and holds the same parts (header, raw frames, then symbolized
/// frames with demangled names). Runs in signal context but may do
/// non-signal-safe work such as logging, at the caller's risk.
using ReportCallback = void (*)(const char* report, size_t len, void* user_data);

struct Options {
  std::vector<int> signals = DefaultSignals();
  /// Size of the sigaltstack installed for the calling thread. 0 installs
  /// none and disables the one a previous Install() set up on this thread
  /// (an alternate stack installed by someone else is left alone).
  size_t alt_stack_size = 1024 * 1024;
  /// fd receiving the report; -1 disables it. The header (signal, code,
  /// fault address, pc, tid, then "*** pc 0x...  module+0xoffset" or
  /// "(not in a loaded module)"; the fault address only for a fault the
  /// kernel raised, "sent by pid N uid N" for kill() / raise() / abort() /
  /// sigqueue(), "call addr 0x..." for a seccomp SIGSYS) is written first,
  /// before anything reads the interrupted stack. When the fault is the
  /// instruction fetch at pc itself
  /// (SIGSEGV with fault addr == pc: a call through a null or wild function
  /// pointer, a jump into data), a "*** caller 0x..." line follows with the
  /// return address of that call ([sp] on x86_64, LR on aarch64, printed as
  /// is, not minus one) and the stack walk starts from the caller; frame #00
  /// stays the bad pc. For any other fault (including a stack overflow) no
  /// caller line is printed and the walk starts at pc. To walk from the
  /// caller the handler moves pc/sp in the interrupted context; it puts them
  /// back right after the walk (also when the walk faults), so a core dump
  /// or a debugger sees the faulting registers. A fault while reporting
  /// prints a marker naming the stage ("reading the caller address faulted",
  /// "stack capture faulted, no stacktrace", "writing the raw stacktrace
  /// faulted", "symbol lookup faulted" (dladdr or demangling, also while
  /// building the callback's copy), "report callback faulted") and ends the
  /// process with the original signal. The rest is written in two parts.
  /// First the raw frames as "#NN pc 0x...  module+0xoffset [base 0x...]",
  /// using the module table recorded by Install() / RefreshModules()
  /// (modules loaded later show as a bare pc); formatting and writing them
  /// is async-signal-safe, the stack walk before them is as safe as the
  /// unwinder (see the README: older libgcc / glibc and LLVM libunwind take
  /// locks in the FDE lookup). Then, when `symbolize` is set, the frames
  /// again with dladdr symbols (not async-signal-safe: dladdr takes the
  /// loader lock and can block, e.g. while another thread runs a dlopen'ed
  /// library's constructors; the watchdog bounds that, and the first part
  /// is already out). Each part, and the callback's copy, is at most 64 KiB;
  /// a longer one is cut and ends with "*** IBMW crash: report truncated".
  /// The last part of the report (the symbolized part, or the raw part
  /// without symbolization; the whole callback copy) then still ends with
  /// the "*** end of stacktrace" line.
  int output_fd = 2;
  /// Clamped to kMaxFrames.
  size_t max_frames = 64;
  /// Append the symbolized part (module and dynamic symbol names via dladdr).
  bool symbolize = true;
  ReportCallback report_callback = nullptr;
  void* report_user_data = nullptr;
  /// Restore the default action and re-raise the signal after reporting, so
  /// the process terminates with the original signal (core dump, exit status).
  /// When false the process calls _exit(128 + signo). Applies to every way
  /// out: normal end of the report, watchdog, and a second fault inside the
  /// handler (e.g. in the callback), which always uses the original signal.
  bool reraise = true;
  /// Upper bound for the whole report (stack walk, symbolization, callback).
  /// When it expires a SIGALRM watchdog terminates the process as above, so a
  /// callback that deadlocks (e.g. malloc after heap corruption) cannot keep
  /// the process alive; at most one second more if the reporting thread
  /// blocks SIGALRM (see Install()). 0 disables the watchdog. The watchdog uses alarm():
  /// a thread consuming SIGALRM via sigwait()/signalfd() defeats it.
  unsigned report_timeout_sec = 10;
};

/// Installs process-wide handlers for options.signals (duplicates are
/// ignored). Re-installing replaces the previous configuration and refreshes
/// the module table used by the raw report part (see RefreshModules()).
/// Returns true when every handler and the alternate stack were set up.
/// Returns false when a sigaction() failed (the other signals are still
/// installed), when the alternate stack could not be allocated, installed or
/// disabled (the handlers are still installed), or when more than 64
/// distinct signals were given (the rest are not installed). IsInstalled()
/// is true when at least one handler was installed. Not supported on
/// Windows (returns false).
///
/// Known limitations:
///  - The alternate signal stack is installed for the calling thread only;
///    a stack overflow on another thread dies without a report (unless that
///    thread has its own sigaltstack).
///  - Install()/Uninstall() are not synchronized with a crash being handled
///    concurrently on another thread (the configuration may be half updated
///    in that report); install once at start-up.
///  - The watchdog uses alarm(): SIGALRM may be delivered to any thread that
///    does not block it; that thread hands it over to the reporting thread,
///    which writes "report timed out" and terminates the process. If that
///    has not happened one second later (the reporting thread blocks
///    SIGALRM, e.g. in the callback, or its write blocks), the next alarm
///    terminates the process on whichever thread it lands, without the
///    message (a socket output_fd gets one non-blocking "ended by the
///    watchdog's last resort" note); the core dump then shows that thread
///    as the one that died.
///    A thread that consumes SIGALRM (sigwait/signalfd) defeats it. Under
///    TSAN, which defers asynchronous signals until the next intercepted
///    call, it cannot fire while the reporting thread is blocked inside
///    dladdr.
///  - Memory of the alternate stack is never freed (the kernel may still
///    reference it); a larger alt_stack_size on re-install leaks the old one.
bool Install(const Options& options);

/// Restores the signal actions that were active before Install(), for the
/// signals whose handler is still this one: a handler installed by someone
/// else after Install() is left in place.
void Uninstall() noexcept;

bool IsInstalled() noexcept;

/// Re-records the table of loaded modules that the header ("*** pc" and
/// "*** caller" lines) and the raw report part use for module+offset. The
/// handler cannot refresh it itself: dl_iterate_phdr takes the loader lock,
/// which is not async-signal-safe. Call it after dlopen() of libraries whose
/// crashes should be reported with module names (IBMW calls it once the
/// packages are loaded); Install() does the same. Works whether or not the
/// handler is installed. Not async-signal-safe: never call it from a signal
/// handler. Calls from several threads are serialized (same mutex as
/// Install()). May run while a crash is being reported on another thread: the
/// table is double buffered and versioned, and a report that sees it change
/// while reading an entry prints that address as a bare pc instead of a
/// possibly torn module name. At most 256 modules are recorded; when more
/// are loaded the raw part starts with "*** module table full: N modules not
/// recorded (their frames show bare pcs)". No-op on Windows.
void RefreshModules() noexcept;

/// Captures the calling thread's stack, skipping the first `skip` frames
/// (frame 0 is the caller of CaptureStack). Each entry points inside the call
/// instruction (return address - 1), except signal-interrupted frames which
/// hold the exact interrupted pc. Does not allocate.
size_t CaptureStack(void** frames, size_t max_frames, size_t skip = 0) noexcept;

/// Formats one frame as
///   "#03 pc 0x000055d0c1a2b3c4  /system/lib64/libfoo.so+0x1b3c4 (symbol+0x24)\n".
/// Output is always NUL terminated and truncated to fit. Returns the number
/// of bytes written, excluding the NUL. Allocation free unless demangle is set.
size_t FormatFrame(char* buf, size_t buf_size, size_t index, const void* pc,
                   bool symbolize, bool demangle) noexcept;

/// Formats `count` frames, one line each. Same guarantees as FormatFrame.
/// A null `frames` formats nothing.
size_t FormatStack(char* buf, size_t buf_size, const void* const* frames, size_t count,
                   bool symbolize, bool demangle) noexcept;

/// "SIGSEGV", "SIGABRT", ... or "UNKNOWN". Async-signal-safe.
const char* SignalName(int signo) noexcept;

}  // namespace ibmw::selfdev::crash
