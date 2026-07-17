#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOKS_DIR="$REPO_ROOT/githooks"

if [[ ! -d "$REPO_ROOT/.git" ]]; then
  echo "ERROR: not a Git repository: $REPO_ROOT" >&2
  exit 1
fi

for hook in pre-commit commit-msg; do
  if [[ ! -x "$HOOKS_DIR/$hook" ]]; then
    echo "ERROR: hook is missing or not executable: $HOOKS_DIR/$hook" >&2
    exit 1
  fi
done

git -C "$REPO_ROOT" config --local core.hooksPath githooks
configured="$(git -C "$REPO_ROOT" config --local --get core.hooksPath)"
if [[ "$configured" != "githooks" ]]; then
  echo "ERROR: failed to configure local core.hooksPath" >&2
  exit 1
fi

echo "Configured local core.hooksPath=$configured"
echo "Executable hook: githooks/pre-commit"
echo "Executable hook: githooks/commit-msg"
