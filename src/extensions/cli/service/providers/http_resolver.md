# HTTP discovery name resolution

`libibmw_service_discovery_http.so` resolves numeric IPv4/IPv6 addresses directly.
For hostnames it starts `ibmw_service_dns_helper` with `posix_spawn`, locating the
executable beside the provider DSO, including after installation or relocation.
The `discovery` install component contains both files; deployments must retain
both. A missing or non-executable helper is a discovery error, without a PATH or
build-tree fallback.

The helper calls the system resolver (including hosts/NSS configuration) and
returns at most 128 numeric addresses / 16 KiB over a private pipe. It is not a
public command or a persistent service. Hostname queries pay one process launch;
numeric-address queries do not. The original hostname remains the HTTP Host
header.

DNS, connect, write and read use one absolute steady-clock deadline. At expiry,
the provider sends SIGKILL to its owned helper and reaps it before returning and
unloading. It never waits for the resolver call itself to complete and leaves
no detached thread, child or callback referencing an unloaded DSO. Process
termination/reaping still depends on OS scheduling; this is not a hard real-time
guarantee against an uninterruptible kernel task or a stalled scheduler.
