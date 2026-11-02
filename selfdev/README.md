# selfdev — self-developed replacements for third-party components

Each component lives in its own directory, has no dependency on IBMW
sources, and is selected by a CMake switch. With a switch OFF, IBMW uses the
original third-party library unchanged.

| Component | Replaces | Switch (default) | Target |
|---|---|---|---|
| [`crash_handler/`](crash_handler/README.md) | backward-cpp (+ libdw/libelf/libbfd) | `IBMW_SELFDEV_CRASH_HANDLER` (ON) | `ibmw::selfdev::crash_handler` (static) |
| [`concurrent/`](concurrent/README.md) | oneTBB (`concurrent_bounded_queue`, `concurrent_hash_map`) | `IBMW_SELFDEV_CONCURRENT` (ON) | `ibmw::selfdev::concurrent` (header-only) |
| [`coro/`](coro/README.md) | libunifex / stdexec behind `ibmw::co::*` | `IBMW_EXECUTOR_USE_SELFDEV_CORO` (ON unless `IBMW_EXECUTOR_USE_STDEXEC=ON`; the two are exclusive) | `ibmw::selfdev::coro` (headers + shared library `libibmw_selfdev_coro`) |

Layout of every component:

```
<component>/
  CMakeLists.txt
  include/ibmw_selfdev/<component>/*.h   public headers
  src/                                   (only if not header-only)
  test/*_test.cc                         gtest; case IDs match the test plan
```

## Standalone build

```bash
cmake -S selfdev -B build-selfdev -DIBMW_BUILD_TESTS=ON \
      -Dgoogletest_LOCAL_SOURCE=<path-to-googletest>
cmake --build build-selfdev -j
ctest --test-dir build-selfdev
```

Standalone builds use `-Wall -Wextra -Wshadow -Werror`. Sanitizers are
enabled by passing e.g. `-DCMAKE_CXX_FLAGS=-fsanitize=thread`.

Design documents, test plans and quality records are kept outside the
source tree (`/oee_work/flx/st/selfdev_process/`).
