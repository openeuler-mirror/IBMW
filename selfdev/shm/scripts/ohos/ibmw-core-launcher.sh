#!/bin/sh
# Compiler launcher for the OHOS IBMW build: silences two clang-only diagnostics
# in IBMW core headers (runtime_graph_subsystem.h -Winconsistent-missing-override;
# ibmw_discovery_provider.h -Wreturn-type-c-linkage) that -Werror turns fatal.
exec "$@" -Wno-inconsistent-missing-override -Wno-return-type-c-linkage
