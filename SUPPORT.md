# Support

IBMW is provided as an open-source project. Support is community and
maintainer-driven; no service-level agreement or guaranteed response time is
included with the source distribution.

## Before requesting help

1. Confirm the repository version in `VERSION`.
2. Read the [User Guide](docs/guides/user-guide.md),
   [Troubleshooting](docs/guides/troubleshooting.md), and
   [Compatibility Boundaries](docs/reference/compatibility.md).
3. Reproduce the problem with the smallest applicable sample or generated
   project.
4. Remove credentials, private source, customer data, and machine-specific
   secrets from logs and reproductions.

## What to include

- IBMW version or revision;
- operating system, architecture, compiler, CMake, and relevant dependency
  versions;
- build preset and non-default CMake options;
- exact command and complete error output;
- minimal configuration or source needed to reproduce the issue;
- whether the problem occurs with an included sample.

Use the repository's public issue or discussion facilities for reproducible
bugs, documentation problems, and focused usage questions. Feature requests
should explain the use case, compatibility impact, and proposed scope. Security
issues must follow [SECURITY.md](SECURITY.md), not a public support thread.

Generated projects and third-party transports may span IBMW, generator,
dependency, and deployment concerns. The project can help identify the boundary
but cannot guarantee support for every external package, operating system image,
network policy, or downstream modification.
