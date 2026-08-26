// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>

// Dispatch a top-level CLI command through the providers listed in the
// deployment CLI configuration.  Returns -1 when no provider handles the
// requested command.
int RunCliCommandFromProviders(
    int argc, char** argv,
    const std::filesystem::path& config_path = {});

inline constexpr int kCliCommandNotFound = -1;
