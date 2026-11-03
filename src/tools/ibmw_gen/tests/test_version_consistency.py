#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
"""HYG-020: every hard-coded release version matches the repository VERSION file.

ibmw_gen keeps a literal __version__ because it also runs packaged (without
the repository around it); this test catches a forgotten bump instead.
"""

import ast
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[4]
VERSION = (REPO_ROOT / "VERSION").read_text(encoding="utf-8").strip()


def _ibmw_gen_version() -> str:
    tree = ast.parse((REPO_ROOT / "src/tools/ibmw_gen/ibmw_gen.py").read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "__version__" for target in node.targets
        ):
            return ast.literal_eval(node.value)
    raise AssertionError("ibmw_gen.py has no __version__")


def test_ibmw_gen_version_matches_version_file() -> None:
    assert _ibmw_gen_version() == VERSION


def test_package_xml_versions_match_version_file() -> None:
    files = sorted(REPO_ROOT.glob("src/schema/**/package.xml"))
    files.append(REPO_ROOT / "src/tools/ibmw_cli/ibmw_cli/templates/protocols/ros2_package.xml.jinja2")
    assert len(files) >= 4
    for path in files:
        versions = re.findall(r"<version>([^<]*)</version>", path.read_text(encoding="utf-8"))
        assert versions == [VERSION], f"{path.relative_to(REPO_ROOT)}: {versions} != {VERSION}"
