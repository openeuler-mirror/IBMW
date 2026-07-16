# Architecture Overview

IBMW separates application logic from runtime services and transport plugins.
Applications are built as modules, packaged either as loadable packages or
standalone executables, and connected to executors, messaging, RPC, logging,
configuration, and parameters through public APIs.

## Layers

1. **Application modules** contain lifecycle methods and business logic. Managed
   modules use transport-neutral C++ interfaces and generated type bindings.
2. **Public API and package ABI** expose module, context, messaging, RPC, and
   package-loading contracts. Loadable packages export the single
   `IbmwPkgEntry` entry and receive drivers through typed slots.
3. **Core runtime** owns lifecycle, scheduling, configuration, package loading,
   and routing. Core code depends on extension interfaces, not extension
   implementations.
4. **Extensions** provide transports, RPC backends, observability, management,
   and utility services. They are dynamically loaded and configured at
   deployment time.
5. **Schemas and generated code** adapt DDS IDL, ROS2 interfaces, and JSON data
   to the public messaging and RPC APIs. Transport-specific typesupport remains
   generated/internal rather than editable application logic.

## Runtime flow

The engine reads deployment YAML, loads extensions and packages, creates module
instances, injects typed runtime capabilities, and calls module lifecycle
methods. Messaging and RPC endpoints are bound during setup so normal data paths
do not repeatedly choose a backend. Shutdown reverses ownership in a controlled
order.

## Public contracts

- Managed application code uses public `cpp/...` headers and does not include
  extension implementation headers.
- Transport selection belongs to deployment configuration. A deliberate
  `direct_<transport>_*` module is an explicit interoperability escape hatch,
  not the default application model.
- Loaned/shared-memory APIs are distinct from ordinary send APIs. Unsupported
  loan combinations fail clearly; they do not silently copy or fall back.
- Package loading uses one public typed-slot ABI entry. Internal typesupport
  headers and extension implementation symbols are not public compatibility
  surfaces.
- Cross builds resolve target libraries from an explicit sysroot or target
  prefix; host packages must not satisfy target dependencies.

See [Architecture Rules](../contributing/architecture-rules.md), the
[API Reference](../reference/api-reference.md), and the
[Support Matrix](../reference/support-matrix.md) for enforceable boundaries.
