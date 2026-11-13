#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
# Checks that the files listed in scripts/clang_format_files.txt are
# clang-format clean (HYG-028). Other versions format differently, so only
# clang-format 18 is accepted.
# Usage: scripts/check_clang_format.sh [--repo-root DIR]
# Environment: CLANG_FORMAT (default: clang-format on PATH).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
if [[ "${1:-}" == "--repo-root" ]]; then
  REPO_ROOT="$(cd "$2" && pwd)"
fi
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if ! version="$("$CLANG_FORMAT" --version 2>/dev/null)"; then
  echo "check_clang_format: $CLANG_FORMAT not found; install clang-format 18 or set CLANG_FORMAT" >&2
  exit 2
fi
if [[ ! "$version" =~ version\ 18\. ]]; then
  echo "check_clang_format: need clang-format 18, got: $version" >&2
  exit 2
fi

failed=0
count=0
while read -r file; do
  [[ -z "$file" || "$file" == \#* ]] && continue
  count=$((count + 1))
  if [[ ! -f "$REPO_ROOT/$file" ]]; then
    echo "check_clang_format: listed file is missing: $file" >&2
    failed=1
  elif ! (cd "$REPO_ROOT" && "$CLANG_FORMAT" --dry-run --Werror "$file"); then
    echo "check_clang_format: not formatted: $file (run: $CLANG_FORMAT -i $file)" >&2
    failed=1
  fi
done < "$REPO_ROOT/scripts/clang_format_files.txt"

if [[ "$failed" != 0 ]]; then
  exit 1
fi
echo "clang-format: $count files clean"
