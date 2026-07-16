# Security Policy

## Supported versions

Security fixes are evaluated for the current public source version. Older
exports, downstream forks, generated applications, and locally modified builds
may require their maintainers to backport a fix. Release notes and the
repository `VERSION` file identify the version being discussed.

## Reporting a vulnerability

Do not publish exploit details in a public issue before maintainers have had a
reasonable opportunity to investigate. Prefer the repository hosting service's
private vulnerability-reporting feature when it is available. If no private
channel is exposed, contact the project maintainers through the repository's
maintainer or organization contact and ask for a confidential reporting path.

Include:

- affected version or revision;
- affected component and configuration;
- reproduction steps or a minimal proof of concept;
- expected and observed impact;
- whether the issue is already public or under active exploitation;
- any suggested mitigation, if known.

Maintainers should acknowledge receipt, assess scope, coordinate a fix and
disclosure plan, and credit reporters who request attribution. Response and fix
timelines depend on severity, reproducibility, and maintainer availability; no
fixed remediation deadline is promised.

## Scope

Reports are especially useful for memory safety, unsafe deserialization,
authentication or authorization bypass, unintended network exposure, plugin or
package loading, path traversal, command execution, denial of service, and
dependency or build-pipeline compromise. General support requests belong under
[SUPPORT.md](SUPPORT.md).

Users remain responsible for securing deployment credentials, network
boundaries, DDS domains and QoS, filesystem permissions, loaded extensions, and
third-party dependencies.
