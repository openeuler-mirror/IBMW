#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
"""Reject private release artifacts and internal references in the work tree."""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


FORBIDDEN_DIRECTORY_NAMES = {".pytest_cache", "__pycache__", ".sisyphus"}
FORBIDDEN_FILE_SUFFIXES = {".pyc", ".pyo", ".swn", ".swo", ".swp"}
TEXT_SUFFIXES = {
    ".adoc",
    ".c",
    ".cc",
    ".cfg",
    ".cmake",
    ".conf",
    ".cpp",
    ".css",
    ".csv",
    ".cxx",
    ".go",
    ".h",
    ".hpp",
    ".html",
    ".hxx",
    ".in",
    ".ini",
    ".java",
    ".js",
    ".json",
    ".kt",
    ".md",
    ".properties",
    ".py",
    ".rs",
    ".rst",
    ".sh",
    ".toml",
    ".ts",
    ".tsx",
    ".txt",
    ".xml",
    ".yaml",
    ".yml",
}
LICENSE_HEADER_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
LICENSE_HEADER_LINES = 5
LICENSE_HEADER_ACCEPTED = re.compile(r"copyright|license|generated", re.I)
TEXT_FILENAMES = {
    ".gitignore",
    ".mailmap",
    "CODEOWNERS",
    "LICENSE",
    "NOTICE",
    "VERSION",
}


def _absolute_path(*parts: str) -> str:
    return "/" + "/".join(parts)


HOST_HOME_ROOT = _absolute_path("home")
GENERIC_PLACEHOLDER_ROOT = _absolute_path("path", "to")
PRIVATE_TEMP_PATH = _absolute_path("tmp", "opencode")
PRIVATE_GO_WORKSPACE_PATH = _absolute_path("go", "ibmw")


PRIVATE_PATH_CONTENT = (
    ("host home path", re.compile(re.escape(HOST_HOME_ROOT) + r"(?:/|\b)")),
    (
        "generic absolute path placeholder",
        re.compile(re.escape(GENERIC_PLACEHOLDER_ROOT) + r"(?:/|\b)"),
    ),
    ("private temporary path", re.compile(re.escape(PRIVATE_TEMP_PATH) + r"(?:/|\b)")),
    (
        "private Go workspace path",
        re.compile(re.escape(PRIVATE_GO_WORKSPACE_PATH) + r"(?:/|\b)", re.I),
    ),
)

FORBIDDEN_CONTENT = PRIVATE_PATH_CONTENT + (
    ("verification notes", re.compile(r"\bIBMW_VERIFICATION_NOTES\b", re.I)),
    ("project status", re.compile(r"\bPROJECT_STATUS\b", re.I)),
    ("internal refactor plan", re.compile(r"\bREFACTOR_PLAN_Y9\b", re.I)),
    (
        "private documentation data",
        re.compile(r"(?<![A-Za-z0-9_.-])docs/data/", re.I),
    ),
    ("internal HR milestone", re.compile(r"(?<![A-Za-z0-9_])HR-[A-Za-z0-9][A-Za-z0-9._-]*", re.I)),
    ("internal QW milestone", re.compile(r"(?<![A-Za-z0-9_])QW-[A-Za-z0-9][A-Za-z0-9._-]*", re.I)),
    ("internal section milestone", re.compile(r"§Y[A-Za-z0-9._-]*", re.I)),
    ("internal V2 notes marker", re.compile(r"\bV2\s+NOTES\b", re.I)),
    ("internal commit marker", re.compile(r"\bTHIS\s+COMMIT\b", re.I)),
    (
        "legacy project owner order",
        re.compile(re.escape("openEuler Embedded SIG & IB-Robot Group")),
    ),
    (
        "legacy project authors",
        re.compile(re.escape("The Ibmw Authors")),
    ),
    (
        "legacy XML maintainer",
        re.compile(re.escape("openEuler Embedded SIG &amp; IB-Robot Group")),
    ),
)


# Checker definitions and negative fixtures necessarily contain other governed
# markers. They remain subject to every private-path rule.
NON_PATH_DEFINITION_SOURCES = {
    Path("scripts/check_docs_governance.py"),
    Path("scripts/check_release_hygiene.py"),
    Path("scripts/tests/test_docs_governance.sh"),
    Path("scripts/tests/test_release_hygiene.sh"),
}


def display(path: Path) -> str:
    return path.as_posix() or "."


def is_text_candidate(path: Path) -> bool:
    return path.name in TEXT_FILENAMES or path.suffix.lower() in TEXT_SUFFIXES


def check_tree(repo: Path) -> list[str]:
    errors: list[str] = []
    for current, directory_names, file_names in os.walk(repo, topdown=True):
        current_path = Path(current)
        relative_current = current_path.relative_to(repo)

        kept_directories: list[str] = []
        for name in directory_names:
            if name == ".git":
                continue
            relative = relative_current / name
            if name in FORBIDDEN_DIRECTORY_NAMES:
                errors.append(f"forbidden cache/internal path: {display(relative)}")
                continue
            kept_directories.append(name)
        directory_names[:] = kept_directories

        for name in file_names:
            relative = relative_current / name
            path = current_path / name
            if name in FORBIDDEN_DIRECTORY_NAMES:
                errors.append(f"forbidden cache/internal path: {display(relative)}")
                continue
            if path.suffix.lower() in FORBIDDEN_FILE_SUFFIXES:
                errors.append(f"forbidden release file: {display(relative)}")
                continue
            if not is_text_candidate(path):
                continue
            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except (OSError, UnicodeError) as exc:
                errors.append(f"cannot read text candidate {display(relative)}: {exc}")
                continue
            patterns = (
                PRIVATE_PATH_CONTENT
                if relative in NON_PATH_DEFINITION_SOURCES
                else FORBIDDEN_CONTENT
            )
            if path.suffix.lower() in LICENSE_HEADER_SUFFIXES and not any(
                LICENSE_HEADER_ACCEPTED.search(line)
                for line in lines[:LICENSE_HEADER_LINES]
            ):
                errors.append(f"missing license header: {display(relative)}")
            for line_number, line in enumerate(lines, 1):
                for label, pattern in patterns:
                    if pattern.search(line):
                        errors.append(
                            f"forbidden release content ({label}): "
                            f"{display(relative)}:{line_number}"
                        )
    return errors


def parser() -> argparse.ArgumentParser:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="work-tree root (default: parent of scripts/)",
    )
    return cli


def main() -> int:
    repo = parser().parse_args().repo_root.resolve()
    if not repo.is_dir():
        print(f"ERROR: not a directory: {repo}", file=sys.stderr)
        return 2
    errors = check_tree(repo)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print(f"release hygiene: FAIL ({len(errors)} issue(s))", file=sys.stderr)
        return 1
    print("release hygiene: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
