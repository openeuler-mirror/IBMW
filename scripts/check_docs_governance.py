#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
"""Validate IBMW public documentation links, authorities, and repository policy."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


REQUIRED_DOCUMENTS = (
    "README.md",
    "README.zh.md",
    "CONTRIBUTING.md",
    "CHANGELOG.md",
    "SECURITY.md",
    "SUPPORT.md",
    "CODE_OF_CONDUCT.md",
    "LICENSE",
    "NOTICE",
    "docs/README.md",
    "docs/guides/user-guide.md",
    "docs/reference/api-reference.md",
    "docs/reference/compatibility.md",
    "docs/reference/support-matrix.md",
    "docs/contributing/testing.md",
    "docs/contributing/architecture-rules.md",
    "docs/ecosystem/README.md",
    "docs/releases/0.1.0.md",
)

PROTECTED_DOCUMENTS = REQUIRED_DOCUMENTS


def _absolute_path(*parts: str) -> str:
    return "/" + "/".join(parts)


PRIVATE_HOME_PATH = _absolute_path("home", "oee")
PRIVATE_GO_ROOT = _absolute_path("go") + "/"
PRIVATE_TEMP_PATH = _absolute_path("tmp", "opencode")


STALE_PRIVATE_REFERENCES = (
    ("private home path", re.compile(re.escape(PRIVATE_HOME_PATH) + r"(?:/|\b)")),
    ("private Go root path", re.compile(re.escape(PRIVATE_GO_ROOT))),
    ("private temporary path", re.compile(re.escape(PRIVATE_TEMP_PATH) + r"(?:/|\b)")),
    ("private .sisyphus path", re.compile(r"\.sisyphus(?:[/\\]|\b)")),
    ("verification notes", re.compile(r"\bIBMW_VERIFICATION_NOTES(?:_V\d+)?(?:\.md)?\b", re.I)),
    ("project status", re.compile(r"\bPROJECT_STATUS(?:\.md)?\b", re.I)),
    ("internal refactor plan", re.compile(r"\bREFACTOR_PLAN_Y9(?:\.md)?\b", re.I)),
    ("private documentation data", re.compile(r"(?:^|[(/`'\"\s])docs/data/", re.I)),
)

# Put this marker on a fixture line that intentionally exercises a forbidden
# value. It is deliberately narrow: prose claiming that a value is an example
# does not exempt a public document from the check.
PRIVATE_REFERENCE_FIXTURE_MARKER = "<!-- docs-governance: allow-private-reference -->"

INLINE_LINK = re.compile(r"!?\[[^\]\n]*\]\((<[^>]+>|[^\s)]+)")
REFERENCE_LINK = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(\S+)")
FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
INLINE_CODE = re.compile(r"(`+)(.*?)\1")


def git(repo: Path, args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def markdown_files(repo: Path) -> tuple[list[Path], list[str]]:
    result = git(
        repo,
        ["ls-files", "--cached", "--others", "--exclude-standard", "--", "*.md"],
    )
    if result.returncode != 0:
        return [], [f"git ls-files failed: {result.stderr.strip()}"]
    paths = sorted({repo / line for line in result.stdout.splitlines() if line})
    return [path for path in paths if path.is_file()], []


def link_destination(raw: str) -> str:
    value = raw.strip()
    if value.startswith("<") and ">" in value:
        return value[1 : value.index(">")]
    return value.split(maxsplit=1)[0] if value else ""


def local_link_target(source: Path, destination: str) -> Path | None:
    destination = destination.strip()
    if not destination or destination.startswith("#"):
        return None
    parsed = urlsplit(destination)
    if parsed.scheme.lower() in {"http", "https", "mailto"} or parsed.netloc:
        return None
    path_text = unquote(parsed.path)
    if not path_text or path_text.startswith("/"):
        return None
    return source.parent / path_text


def links_in(path: Path) -> list[tuple[int, str]]:
    links: list[tuple[int, str]] = []
    fence_marker = ""
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        fence = FENCE.match(line)
        if fence:
            marker = fence.group(1)[0]
            if not fence_marker:
                fence_marker = marker
            elif marker == fence_marker:
                fence_marker = ""
            continue
        if fence_marker:
            continue
        link_line = INLINE_CODE.sub("", line)
        for match in INLINE_LINK.finditer(link_line):
            links.append((line_number, link_destination(match.group(1))))
        reference = REFERENCE_LINK.match(line)
        if reference:
            links.append((line_number, link_destination(reference.group(1))))
    return links


def check_required(repo: Path) -> list[str]:
    return [
        f"required document is missing: {name}"
        for name in REQUIRED_DOCUMENTS
        if not (repo / name).is_file()
    ]


def check_links(repo: Path) -> list[str]:
    files, errors = markdown_files(repo)
    for source in files:
        try:
            links = links_in(source)
        except (OSError, UnicodeError) as exc:
            errors.append(f"cannot read {source.relative_to(repo)}: {exc}")
            continue
        for line_number, destination in links:
            target = local_link_target(source, destination)
            if target is not None and not target.exists():
                errors.append(
                    f"broken local link: {source.relative_to(repo)}:{line_number}: {destination}"
                )
    return errors


def check_stale_private_references(repo: Path) -> list[str]:
    files, errors = markdown_files(repo)
    for path in files:
        relative = path.relative_to(repo)
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as exc:
            errors.append(f"cannot read {relative}: {exc}")
            continue
        for line_number, line in enumerate(lines, 1):
            if PRIVATE_REFERENCE_FIXTURE_MARKER in line:
                continue
            for label, pattern in STALE_PRIVATE_REFERENCES:
                if pattern.search(line):
                    errors.append(
                        f"stale private reference ({label}): {relative}:{line_number}"
                    )
    return errors


def check_all(repo: Path) -> list[str]:
    return check_required(repo) + check_links(repo) + check_stale_private_references(repo)


def check_deletions(repo: Path, base: str) -> list[str]:
    result = git(repo, ["diff", "--name-status", "--find-renames", base, "--"])
    if result.returncode != 0:
        return [f"git diff failed for base {base!r}: {result.stderr.strip()}"]

    protected = set(PROTECTED_DOCUMENTS)
    errors: list[str] = []
    for line in result.stdout.splitlines():
        fields = line.split("\t")
        status = fields[0] if fields else ""
        removed = fields[1] if status.startswith("R") and len(fields) >= 3 else (
            fields[1] if status == "D" and len(fields) >= 2 else ""
        )
        if removed in protected:
            errors.append(f"protected document deleted or renamed: {removed}")
    return errors


def parser() -> argparse.ArgumentParser:
    cli = argparse.ArgumentParser(description=__doc__)
    mode = cli.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="run all documentation checks")
    mode.add_argument(
        "--check-deletions",
        nargs="?",
        const="HEAD",
        metavar="BASE",
        help="reject protected-document deletion or rename since BASE (default: HEAD)",
    )
    cli.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (default: parent of scripts/)",
    )
    return cli


def main() -> int:
    args = parser().parse_args()
    repo = args.repo_root.resolve()
    if not (repo / ".git").exists():
        print(f"ERROR: not a Git repository: {repo}", file=sys.stderr)
        return 2

    errors = check_all(repo) if args.check else check_deletions(repo, args.check_deletions)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print(f"docs governance: FAIL ({len(errors)} issue(s))", file=sys.stderr)
        return 1
    print("docs governance: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
