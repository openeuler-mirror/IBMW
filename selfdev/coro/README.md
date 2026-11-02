# selfdev/coro

C++20 coroutine primitives behind `ibmw::co::*`; replaces libunifex /
stdexec. Headers plus one small shared library, `libibmw_selfdev_coro`
(target `ibmw::selfdev::coro`), which holds the per-thread slot of Task's
synchronous-completion handoff and the process-wide `StartDetached` scope
(see "One slot per process" below). Everything is built on plain awaitables, so any type
with `await_ready/await_suspend/await_resume` (for example
`rpc_handle.h`'s `CallAwaitable`) can be `co_await`ed.

| API | Meaning |
|---|---|
| `Task<T>` | lazy, move-only coroutine; exceptions propagate to the awaiter; not scheduler-affine (continues on the thread that completed it, as the unifex branch does) |
| `Schedule(s)` / `ScheduleAfter(s, d)` / `ScheduleAt(s, tp)` | awaitables from a scheduler's `schedule()` / `schedule_after()` / `schedule_at()` |
| `InlineScheduler` | completes immediately on the current thread |
| `On(s, a)` | switch to `s`, then await `a` |
| `Then(a, f)` / `a \| Then(f)` | await `a`, return `f(result)` |
| `AsyncScope` | `spawn(a)` starts work eagerly; `co_await complete()` (or `cleanup()`) closes the scope and waits for all work |
| `SyncWait(a)` | block the calling thread; returns `std::optional<R>` (`Unit` for void) or rethrows |
| `StartDetached(a)` | spawn on a process-wide scope drained at static destruction; returns `false` (and destroys `a` unrun) once that scope is closed |

```cpp
#include "ibmw_selfdev/coro/coro.h"
using namespace ibmw::selfdev::coro;

Task<int> Work(MyScheduler s) {
  co_await Schedule(s);
  co_return 42;
}

AsyncScope scope;
scope.spawn(On(s, Work(s)) | Then([](int v) { Use(v); }));
SyncWait(scope.complete());
```

A synchronous `co_await` does not grow the stack at any optimization level:
the awaiter resumes the child directly and a per-thread record on the
stack of `Start()` tells the child's final awaiter that it completed
synchronously, instead of relying on symmetric-transfer tail calls (GCC does
not emit them at `-O0`).

Concepts `Awaitable`, `Scheduler` and `TimedScheduler` (in `concepts.h`)
describe what the algorithms accept.

## Rules for awaiters (library and user code)

1. **No aggregate temporaries as `co_await` operands.** GCC 11/12 destroy the
   members of `co_await Aw{...}` (and of C++20 parenthesized aggregate init
   `co_await Aw(...)`) twice. Give the awaiter a user-declared constructor,
   or name it (`Aw a(...); co_await a;`), or return it from a function /
   `operator co_await`. This applies to the operand itself: an aggregate
   wrapper with an `operator co_await`, written `co_await Wrap{...}`, is hit
   the same way. Every awaiter type this library defines, final awaiters
   included, is a non-aggregate (checked by `static_assert` in
   `test/awaiter_rules_test.cc`).
2. **Nothing after publishing the handle.** Once `await_suspend` has handed
   the coroutine handle to another thread (executor queue, callback), the
   coroutine may already be resumed and its frame destroyed. Clang before 19
   may keep the awaiter and the locals of an inlined `await_suspend` in that
   frame. Copy what is needed into locals first, touch only those afterwards,
   and mark such an `await_suspend` `IBMW_SELFDEV_CORO_NOINLINE`
   (`export.h`). Publishing under a mutex that the resuming side also takes
   (`AsyncScope::CompleteAwaiter`, `SyncWait`'s final awaiter) counts as
   publishing: the handle may run as soon as the mutex is released.

## One slot per process

`Task` relies on one thread_local slot per thread. It is defined once, in
`libibmw_selfdev_coro` (`src/task.cc`), together with the out-of-line
`TaskPromiseBase::Start()` / `Finish()`; an inline function-local
thread_local used to be duplicated per DSO under `RTLD_DEEPBIND`,
`-Bsymbolic` or version scripts (CO-17, `test/dso/`). Consequences:

- Never build `src/task.cc` into a static library or into more than one DSO.
- Only the `IBMW_SELFDEV_CORO_API` functions are exported (version script
  `src/libibmw_selfdev_coro.map`); the `ibmw_selfdev_coro_exports` test
  compares `nm -D --defined-only` of the library with `test/exports.txt`.
- Installed with `ibmw` (`lib/`, Windows DLL in `bin/`); executables built by
  the IBMW project get `$ORIGIN/../lib` in their install RPATH. Downstream
  projects get the library through `find_package(ibmw)` and
  `ibmw::selfdev::coro`.
- `IBMW_SELFDEV_CORO_TLS_INITIAL_EXEC` (default ON on Linux with GCC/Clang,
  OFF on OHOS and elsewhere): initial-exec TLS for the slot, a plain
  `%fs`-relative access. The library is then flagged `STATIC_TLS`; glibc's
  static TLS reserve covers it when it is first loaded by `dlopen()`. Turn it
  off for C libraries without such a reserve.

## Limitations

- No fibers / user-space stack switching (ucontext, boost.context): fibers
  sharing an OS thread would share the slot.
- `STB_GNU_UNIQUE`: the library exports no unique symbol (its copy of
  libstdc++'s noop coroutine frame is local), but GCC-built modules that use
  coroutines still get libstdc++'s
  `coroutine_handle<noop_coroutine_promise>::_S_fr` and `std::variant`
  vtable symbols as `STB_GNU_UNIQUE`, so `dlclose()` does not unload such a
  module. Build the module with `-fno-gnu-unique` if it must be unloadable.
- `StartDetached` shutdown: the process-wide scope is created by the first
  call and drained by an exit-time destructor, in reverse order of static
  construction. Statics that detached work uses must be constructed before
  the first `StartDetached`; calling it after the drain is undefined;
  `quick_exit` / `_exit` skip the drain.
- Unloading a module while its detached work is in flight: the scope is
  process-wide and is only drained at exit, so `dlclose()` of a module does
  not wait for coroutines it started with `StartDetached` (in IBMW, the
  generated RPC handler coroutines of a package). If one is still running,
  its remaining code runs from the unloaded module. Unload a package only
  after its RPC calls and other detached work have finished, or do not
  unload it.
- Thread-local state read inside a coroutine (for example IBMW's
  `g_thread_ctx`) is re-read after each resume by GCC 11/12 and Clang 14 at
  -O1..-O3 (checked, including shared-library builds); do not keep a
  reference or pointer to a thread_local across a `co_await`.
