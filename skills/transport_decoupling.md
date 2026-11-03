# IBMW Transport Decoupling Rules

Use these rules for public headers, generated module surfaces, package ABI,
message schemas, and deployment YAML. They express durable architecture
contracts; executable detection remains in `scripts/check_red_lines.sh`.

## Goal

Managed application logic must remain independent of the selected transport.
Changing a deployment backend should not require editing business logic. Native
backend access is an explicit, named escape hatch rather than the default path.

## R1-R12

1. **Public includes:** managed user code does not include extension or
   transport implementation headers.
2. **Public symbols:** managed code does not reference backend namespaces or
   non-public driver members.
3. **Driver ownership:** managed modules do not construct or store concrete
   transport drivers.
4. **Injection:** do not teach users to dispatch drivers by string or override a
   type-erased driver setter. Framework injection is typed and automatic.
5. **Configuration:** concrete driver selection belongs to deployment
   configuration. Module schemas describe endpoints and application types, not
   backend implementation details.
6. **Message types:** application-facing type names and declarations remain
   stable across backends. Backend typesupport is generated/internal.
7. **Package ABI:** packages export one `IbmwPkgEntryV2` and receive capabilities
   through typed slots, without public driver-name strings or dual entries.
8. **Zero copy:** loaned and ordinary send APIs are separate paths. A missing
   loan-capable backend fails clearly; no silent copy, allocation, or fallback is
   permitted.
9. **Initialization:** topic/type registration, QoS binding, and backend setup
   stay in framework-owned or generated regions, not editable business code.
10. **Escape hatches:** direct native use is named
    `direct_<transport>_*` and carries an explicit portability warning.
11. **Unified templates:** publisher/subscriber and loaned templates are not
    split into user-facing backend-specific families.
12. **Feature macros:** `IBMW_HAS_<TRANSPORT>` and related macros are
    compile-time selectors only; use preprocessor checks or `static_assert`, not
    runtime expressions.

## Review checklist

- Does generated user code include only public `cpp/...`, package, and
  application message headers?
- Is backend-specific typesupport outside user regions?
- Is selection performed once during setup/binding rather than on each send?
- Do unsupported loan combinations fail before entering normal runtime work?
- Does installed package metadata avoid internal headers and build-tree paths?
- Is every direct native sample visibly isolated from managed examples?

See [Architecture Rules](../docs/contributing/architecture-rules.md) for the
broader dependency and testing expectations.
