# OHOS remote-managed capture candidate v1

This candidate is built only when `IBMW_BUILD_OHOS_CAPTURE_AGENT=ON`; the
option defaults to OFF on every platform, including OHOS. The native agent does
not require Python and does not alter the local managed
collector. The host creates a private directory (owned by the agent uid and not
group/world accessible) and invokes:

```text
mw_perf_ohos_capture_agent \
  --run-root /absolute/canonical/existing/private/root \
  --request-challenge 64_LOWER_HEX \
  --run-id RUN --capability-id CAP --path-id PATH \
  --topology-generation GEN --topology-sha256 64_LOWER_HEX \
  --trace-mode off|sampled|full --exact-required true|false \
  --timeout-ms MILLISECONDS [--env NAME ...] -- /absolute/sut [arg ...]
```

`--env NAME` copies only that variable from the agent environment. Names with
the `MW_PERF_` or `IBMW_` prefixes are forbidden because the agent supplies the
authoritative trace controls. A named variable must exist; its resolved value
is included in the canonical request hash. Exact-required capture requires
`--trace-mode full`. The run root and SUT are opened by component-wise `openat`
walking with `O_NOFOLLOW`. The SUT is executed through its validated fd with
`fexecve`; a platform without `fexecve`, the subreaper prctl, or the
parent-death-signal prctl fails at compile/configure time. There is no PATH
lookup or shell evaluation. In protocol v1, the child's stdin, stdout, and
stderr are all redirected to `/dev/null` before execution; child output is not
retained. The child otherwise receives only standard fds and
`MW_PERF_COMM_TRACE_DIR_FD`.

The agent creates an exclusive `capture-<capture-id>` directory with mode 0700 and
publishes `capture-<request-challenge>.tar` beside it. The host challenge is mandatory,
is included in the canonical request hash, and is repeated in the result and manifest.
The archive basename is exactly bound to that challenge; the capture id is
agent-generated randomness only. The uncompressed USTAR is normalized
to mode 0600, uid/gid/mtime zero, and bytewise filename order. Its manifest's
`archive_tree_sha256` hashes the canonical sequence of accepted producer
`name`, `size`, and content SHA-256 values; the manifest itself is excluded to
avoid a circular hash. The single stdout line is bounded JSON containing
`snapshot_valid`, archive path/hash/size, child identity, and reasons.
`snapshot_valid` means only that transport, lifecycle, stable-file scans, and
archive publication completed under this contract. It does not mean producer
JSON or trace events are semantically valid. The manifest always records
`semantic_validation=false`. A host importer must parse every producer JSON
file and check producer pid/start ticks, run id, and meta/seal coherence before
using the trace as evidence. Invalid fixture JSON can still be a valid
transport snapshot.

After copying the archive and the agent's single-line result JSON to a trusted
host, save the exact request object used to launch the agent and run the
installed importer (this command never launches a remote process or shell):

```text
mw-perf-collect import-ohos-capture \
  --archive /absolute/host/capture-64_LOWER_HEX.tar \
  --result /absolute/host/agent-result.json \
  --request /absolute/host/request.json \
  [--output /absolute/host/import-summary.json]
```

Without `--output`, exactly one bounded JSON document is written to stdout.
With `--output`, canonical JSON is published mode 0600 without replacing an
existing path. Result and request inputs must be regular, non-symlink JSON files
with unique object keys. The summary reports semantic status, exact/stage
counts, hashes, and provenance only. It always reports `formal_p0a=false` and
never creates `directory_finalization.json` or claims metric/comparison validity.

`descendant_policy=forbid` is enforced by acting as a subreaper, rejecting any
observed descendant, terminating the leader's group only while its PID/start-tick
identity remains live, reaping adopted children, and proving every recorded
identity absent before scanning. The threat model is a
cooperative SUT contained in that process group. A malicious same-UID process
that deliberately escapes the group and reparents outside the agent's
subreaper ancestry is outside this candidate's threat model; this is not a uid,
namespace, cgroup, or sandbox boundary.
