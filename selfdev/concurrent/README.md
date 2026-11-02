# selfdev/concurrent

Header-only concurrent containers; replace the two oneTBB types IBMW uses.

| Type | Replaces | Notes |
|---|---|---|
| `BoundedBlockingQueue<T>` | `tbb::concurrent_bounded_queue<T>` | ticketed queue: one atomic ticket per item, items spread over 8 lanes of 16-cell blocks, so producers (and consumers, when `T` moves without throwing) take no lock; a thread that must wait (empty, full, or an item still being written) spins briefly and then sleeps on a condition variable, and wakers only signal registered sleepers; `Push`/`Emplace` block when full, `Pop` blocks when empty; `Abort()` wakes all waiters and makes later blocking calls fail (sticky until `Reset()`) |
| `ConcurrentHashMap<K, V>` | `tbb::concurrent_hash_map<K, V>` | sharded `unordered_map` (power-of-two shards, 64 by default, cache-line aligned); value-returning API (`Emplace`, `InsertOrAssign`, `Take`, `Get`, `Erase`) plus `Visit` / `ForEach` under the shard lock |

```cpp
#include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"

ibmw::selfdev::concurrent::BoundedBlockingQueue<Task> q(1024);
q.Emplace(std::move(task));     // false once aborted
Task t;
if (q.Pop(t)) t();              // false once aborted
```

Callbacks given to `Visit` / `ForEach` run under a shard lock and must not
call back into the same map. Values are destroyed outside it (`Erase`,
the old value of `InsertOrAssign`, the moved-from value of `Take`), so a
destructor may use the map. `InsertOrAssign(key, v)` builds the new value
before taking the lock and swaps it in, so `V` must be constructible from
`v`, move constructible and swappable (checked by `static_assert`); if
building it throws, the map is unchanged.

`BoundedBlockingQueue` notes:

- `TryPop` returns false when the queue is empty. If the oldest item is
  still being written by its producer: when `T` moves without throwing,
  `TryPop` takes that item's ticket and waits for it (brief spin, then
  sleep; usually nanoseconds, longer only if the producer was preempted);
  otherwise it returns false and the items behind stay queued in FIFO
  order. `PopFor` honours its timeout in every wait and treats huge
  timeouts as "no limit".
- A push whose `T` constructor (or block allocation) throws keeps its
  capacity slot until a consumer steps over it; blocked consumers and
  producers are woken either way.
- `Abort()` / `Reset()` keep a generation: a call blocked when `Abort()`
  came returns false even if `Reset()` follows before it wakes.
- `T`'s constructors, move operations and destructor must not use the same
  queue (they run while the thread holds its place in a lane).
- Fixed memory per queue: 8 lanes x 16 cells x `sizeof(T)` (plus one state
  byte per cell) allocated up front; each lane keeps at most one drained
  block for reuse.
- On Linux the first queue registers the process for `membarrier(2)`
  (private expedited); a thread about to sleep while waiting for another
  thread at a lane issues one `membarrier` call, so that the hot paths
  need no fence. Without kernel support (or with
  `IBMW_SELFDEV_NO_MEMBARRIER` defined) both sides use seq_cst fences.
- Sandboxes: a seccomp policy may forbid `membarrier` (container runtime
  profiles, app sandboxes; check the target, e.g. OHOS). If it fails the
  call at registration, the queue uses fences. If it kills the process
  instead, the first queue does that. Opt out there:
  - at run time: set `IBMW_SELFDEV_NO_MEMBARRIER=1` in the environment
    before the first queue is made; it is read once, before any
    `membarrier` call;
  - at build time: CMake option `IBMW_SELFDEV_CONCURRENT_NO_MEMBARRIER=ON`
    (the default when building for OHOS, until `membarrier` is confirmed
    on the board), which defines `IBMW_SELFDEV_NO_MEMBARRIER` for every
    user of the target. The OHOS default applies to a fresh build
    directory only: the option is cached, so a directory configured
    before keeps its value until it is set with `-D` or the cache is
    recreated. Defining the macro by hand must be done for all
    translation units (globally): units that disagree can lose wakeups.
    The queue lives in an inline namespace named after the variant,
    which makes some mixes fail to link, but not all (a queue held as a
    member of a type shared across units still links).
- If `membarrier` fails after registration (a filter installed later
  returns an error, or `ENOMEM` on every one of a few tries), the queue
  keeps working: a thread about to sleep at a lane then re-checks every
  millisecond instead of relying on a wakeup, and a message naming the
  variable is printed once to stderr. An error that will not go away
  (`EPERM`, `ENOSYS`, ...) is remembered and later sleeps skip the call;
  after transient ones (`ENOMEM`, `EINTR`, `EAGAIN`) the next sleep tries
  again.
- `PopFor` that times out, a call ended by `Abort()`, or a `Pop` whose
  move assignment throws, with items still queued, passes on the wakeup
  it may have taken, so other blocked `Pop` calls are not left asleep.
