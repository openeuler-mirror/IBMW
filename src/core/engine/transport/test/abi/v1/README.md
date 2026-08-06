# TransportDriver ABI v1 fixture

This directory contains a frozen `TransportDriver` declaration and a test-only
shared-library implementation for the old-driver/new-core compatibility test.

- ABI version: `v1`
- Baseline commit: `9eb9c8d5a2c5815a3da0465981ec135dc8a63294`
- Direction covered: driver built against v1, loaded and called by the current core

Do not update `transport_driver.h` to match the current production header. A
new incompatible baseline must be added as a new versioned fixture directory.
These files are test inputs only and must not be installed as public headers.
