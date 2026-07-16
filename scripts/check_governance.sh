#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

python3 "$SCRIPT_DIR/check_docs_governance.py" --check --repo-root "$REPO_ROOT"
python3 "$SCRIPT_DIR/check_release_hygiene.py" --repo-root "$REPO_ROOT"
bash "$SCRIPT_DIR/tests/test_docs_governance.sh"
bash "$SCRIPT_DIR/tests/test_release_hygiene.sh"
bash "$SCRIPT_DIR/tests/test_commit_msg_hook.sh"

echo "PASS: governance checks and fixtures"
