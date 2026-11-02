# selfdev/crash_handler

Fatal-signal handler and stack capture for POSIX; replaces backward-cpp so
IBMW no longer links libdw/libelf (LGPL) or libbfd (GPL).

```cpp
#include "ibmw_selfdev/crash_handler/crash_handler.h"

namespace ch = ibmw::selfdev::crash;
ch::Options opt;                 // SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL/... by default
opt.report_callback = &OnReport; // optional: (const char*, size_t, void*)
ch::Install(opt);                // returns false on failure
```

- Runs on an alternate signal stack (`alt_stack_size`), so stack overflow is
  reported too.
- The header (signal, fault address, pc as `module+0xoffset` or
  `(not in a loaded module)`) is written first, before anything reads the
  interrupted stack, so a stack overflow or a bad sp still leaves it on
  `output_fd`.
- Only when the fault is the instruction fetch at pc itself (SIGSEGV with
  fault address == pc: a call through a null or wild function pointer, a
  jump into data or heap, inside a module or not), the handler prints
  `*** caller 0x...` (the return address at `[sp]` on x86_64, `LR` on
  aarch64, printed as is: it points after the call, `addr2line` on it may
  name the next line) and walks the stack from the caller, since the
  unwinder would fault reading instructions at pc; frame `#00` is still the
  bad pc. Every other fault (null data store, stack overflow, ...) has no
  caller line and the walk starts at pc: there `[sp]` is just a local.
- To walk from the caller the handler moves pc/sp in the interrupted
  context and puts the original values back right after the walk (also when
  the walk itself faults), before symbol lookup, callback and re-raise.
- If reading the caller, the walk, symbol lookup or callback faults anyway,
  the report ends with a marker (`*** IBMW crash: reading the caller address
  faulted`, `stack capture faulted, no stacktrace`, `symbol lookup faulted`,
  `report callback faulted`) and the process terminates with the original
  signal.
- Frames are captured with `_Unwind_Backtrace`. After the header, the report
  on `output_fd` has two parts:
  1. Header and raw frames, `#NN pc 0x...  module+0xoffset [base 0x...]`,
     from a module table that `Install()` records with `dl_iterate_phdr`.
     Async-signal-safe: no allocation, no locks, written before any symbol
     lookup. Modules loaded after the last refresh show as a bare pc (also
     in the `*** pc` line); call `RefreshModules()` after `dlopen()` (IBMW's
     ModuleBus does it once the packages are loaded, and the engine again
     before starting the subsystems). The handler never refreshes the table
     itself: `dl_iterate_phdr` takes the loader lock. The table holds 256
     modules; beyond that the report says
     `*** module table full: N modules not recorded (their frames show bare pcs)`.
     A lookup that overlaps a `RefreshModules()` on another thread is
     detected (version counter) and prints a bare pc, never a wrong module
     (the readers are excluded from ThreadSanitizer, which would otherwise
     report this intended race). `RefreshModules()` must not be called from two threads at once.
  2. With `symbolize` (default), the frames again with `dladdr` names. `dladdr`
     takes the loader lock and can block (for example while another thread
     runs a `dlopen`ed library's constructors); the watchdog bounds that and
     part 1 is already out.
- The `report_callback` stage gets both parts with demangled names
  (`__cxa_demangle` allocates) and writes the log, which is not
  async-signal-safe, the same as the backward-based code it replaces.
- After reporting, the original disposition is restored and the signal is
  re-raised (`reraise = true`), so core dumps and exit codes are unchanged
  (the interrupted context holds the faulting pc/sp again, see above).
  With `reraise = false` the process calls `_exit(128 + signo)`.
- A second fault inside the handler (callback, symbolization) ends the
  process as the *original* signal, honouring `reraise`. Synchronous fault
  signals (SEGV, BUS, FPE, ILL, TRAP) are never blocked while the handler
  runs, so such a fault re-enters the handler instead of the kernel killing
  the process with the second signal.
- `Uninstall()` restores the previous actions only for signals whose handler
  is still this one.
- The whole report is bounded by `report_timeout_sec` (default 10 s). If the
  callback hangs (for example malloc deadlocking after heap corruption), a
  SIGALRM watchdog prints `report timed out` and terminates the process with
  the original signal. `Install()` calls the unwinder and `dladdr` once so
  their lazy symbol resolution does not first happen in signal context.
- Symbols hidden by `-fvisibility=hidden` are printed as `module+offset`;
  resolve them offline with `addr2line -e <module> <offset>`.
- `CaptureStack` / `FormatStack` can also be used outside signal handlers.

Known limitations (see the header for details):

- The alternate stack is installed for the calling thread only; a stack
  overflow on another thread dies without a report.
- `Install()` / `Uninstall()` are not synchronized with a crash being
  reported on another thread; install once at start-up.
- The watchdog uses `alarm()`; SIGALRM may be handled on any thread, and a
  thread consuming SIGALRM (`sigwait` / `signalfd`) defeats it. Under TSAN,
  asynchronous signals are deferred while the reporting thread is blocked in
  the loader lock, so the watchdog cannot fire there.
- The alternate stack is never freed.
