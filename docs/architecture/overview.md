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

## Boundary model

- **Public API**: installed `cpp/...` headers, generated application bindings,
  lifecycle/context/messaging/RPC contracts, and the single package ABI. This
  is the compatibility surface for managed applications.
- **Extension SPI**: interfaces used by core to discover, configure, and inject
  transport and service implementations. It is a plugin implementation
  contract, not a managed application's transport API.
- **Internal/direct**: internal typesupport, backend-specific headers, and
  direct benchmark/interoperability endpoints. These are implementation details
  or explicitly non-portable escape hatches.

Managed applications own business behavior and use the public API. The runtime
owns transport selection, endpoint setup, capability injection, and orderly
shutdown. A direct endpoint owns native backend setup and teardown and accepts
the resulting portability and ABI risk.

The normal deployment is a dynamically loaded extension selected by
configuration. A `direct_<transport>_*` endpoint may instead link the backend
directly when a benchmark or interoperability test needs implementation-level
access; it is not an example of managed application architecture.

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
