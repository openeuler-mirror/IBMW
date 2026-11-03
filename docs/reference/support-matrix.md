# Support Matrix

This matrix summarizes public capability boundaries. “Supported” means the
repository provides a maintained public path; it does not guarantee every OS,
distribution, backend combination, or application payload.

| Capability | Status | Boundary |
|---|---|---|
| C++ module/context APIs | Supported | Public installed `cpp/...` headers and documented lifecycle/messaging/RPC contracts. |
| Loadable package ABI | Supported | Single `IbmwPkgEntryV2` with typed-slot driver injection. |
| Local messaging and RPC | Supported | Suitable for in-process examples and application validation. |
| DDS messaging and RPC | Supported | Requires compatible FastDDS/FastCDR dependencies and application-specific type/payload validation. |
| DDS-only ROS2 pub/sub interoperability | Supported, scoped | FastDDS wire interoperability without rcl/rclcpp in the IBMW runtime; validate the ROS2 distribution and interface set used. |
| ROS2 interface import | Supported, scoped | Message/service artifact generation; generated IBMW runtime remains rcl-free. Action inventory is allowed, but Action semantics are not automatically migrated. |
| rcl-based ROS2 extension | Deprecated | Compatibility surface only; prefer DDS-only interoperability. |
| Shared-memory loaned messaging | Supported, backend-dependent | Requires an enabled loan-capable backend and compatible plain types. No silent copy fallback. |
| JSON/local application generation | Supported, scoped | Local pub/sub and RPC application workflows; not evidence for a network or target backend. |
| AArch64 Linux cross compilation | Supported, scoped | Requires a target toolchain/sysroot and explicit target dependency prefix. Board runtime is a separate validation step. |
| OHOS ARM64 cross compilation | Supported, scoped | Compile and artifact-audit workflow with an OHOS native SDK and target ROS2 prefix where applicable. Board deployment/runtime must be demonstrated separately. |
| DDS-only ROS2 Action adapter | Experimental | Documented sample/adapter scope only; no general generator, cross-RMW, or cross-distribution guarantee. |
| Transport benchmarks | Methodology supported | Results are environment-specific. Reproduce with recorded hardware, software, affinity, payload, duration, and repetitions. |
| SDK install-prefix consumption | Supported, scoped | Host and cross consumers use installed package metadata; release packaging and target dependencies remain profile-specific. |

## Dependency lanes

- Native FastDDS/FastCDR generated code must match the dependency major-version
  lane used to compile it.
- OHOS ROS2 target prefixes may carry an older FastRTPS/FastCDR ABI than native
  builds. Do not mix generated code or libraries across those lanes.
- Host ROS2 libraries are host tools/dependencies only and must not satisfy a
  target link during cross compilation.

## Claim discipline

An application must validate its largest representative serialized payload,
runtime peers, target architecture, and deployment environment. Compile success
does not prove board runtime, and a local JSON smoke does not prove a DDS, ROS2,
or OHOS deployment.

See [Compatibility Boundaries](compatibility.md) and
[Cross-compilation](../guides/cross-compilation.md).
