# Architecture Rules

These rules apply to public headers, package ABI, generated application code,
module templates, and deployment configuration.

## Dependency direction

- Application modules depend on public APIs, never transport implementations.
- `src/api/` and `src/core/` must not include or statically link extension
  implementations.
- Extensions implement public extension interfaces and register capabilities at
  runtime; core does not select an implementation by including its headers.

## Application surface

- Managed user code must not include transport-specific headers, construct
  transport drivers, call non-public driver members, or store concrete driver
  pointers.
- Generated user regions contain business logic and calls to stable adapters.
  Type registration, QoS binding, and transport initialization stay in generated
  or framework-owned regions.
- Publisher/subscriber and loaned templates remain unified. Transport-specific
  generation is internal typesupport, not a second user-facing template family.
- Direct native integration is allowed only through clearly named
  `direct_<transport>_*` escape hatches with an explicit portability warning.

## ABI and feature selection

- Loadable packages expose `IbmwPkgEntry` and typed-slot injection. Do not add a
  second package entry or driver-name string dispatch to the public ABI.
- Feature macros such as `IBMW_HAS_<TRANSPORT>` are compile-time selectors only.
  Use preprocessor checks or `static_assert`, never runtime branching on a macro.
- Preserve public/internal header separation. Installed consumers must not need
  build-tree paths or internal typesupport headers.

## Messaging and zero copy

- Ordinary send and loaned send are separate contracts. Loaned paths preserve
  ownership and zero-copy behavior; no silent allocation, copy, or fallback is
  allowed.
- Bind backend dispatch during connection/setup. Avoid backend-selection branches
  in the hot path.
- Message declarations use stable application-facing names. Backend-specific
  generated representations must not leak into editable business code.

## Configuration

- Select concrete drivers in deployment configuration, in one authoritative
  place. Do not scatter backend choices through module definitions.
- Keep generated YAML and documented examples consistent with the current schema.
- Cross-compilation configuration must distinguish host tools from target
  packages and reject missing or wrong-architecture target dependencies early.

Run the repository red-line checker for changes to these surfaces and add focused
tests for every new rule or exception. See [Testing](testing.md).
