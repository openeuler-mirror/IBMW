#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Behaviour of the ibmw_cli project generator that does not need a full build.
import os
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "ibmw_cli"))

from ibmw_cli.generator import GeneratorBase  # noqa: E402
from ibmw_cli.generator.project_generator import ProjectGenerator  # noqa: E402
from jinja2 import Template  # noqa: E402


def _write(generator, name="out.txt", content="x"):
    generator.output({"output_path": "sub", "output_file": name, "template_name": "t"}, "n", content)


# TOOL-015: only a leading "~" means the home directory.
def test_tilde_inside_the_output_dir_is_kept(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.chdir(tmp_path)
    (tmp_path / "home").mkdir()
    generator = GeneratorBase()
    generator.output_dir_ = str(tmp_path / "a~b")
    _write(generator)
    assert (tmp_path / "a~b" / "sub" / "out.txt").read_text() == "x"


def test_leading_tilde_is_the_home_directory(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path))
    generator = GeneratorBase()
    generator.output_dir_ = "~/proj"
    _write(generator)
    assert (tmp_path / "proj" / "sub" / "out.txt").read_text() == "x"


# TOOL-013: a missing or empty section is reported by name, before any use.
@pytest.mark.parametrize("section", ["deploy_modes", "modules", "pkgs"])
@pytest.mark.parametrize("missing", [True, False])
def test_missing_section_is_a_readable_error(tmp_path, monkeypatch, section, missing):
    cfg = {"base_info": {"project_name": "p"}, "deploy_modes": [], "modules": [], "pkgs": []}
    if missing:
        del cfg[section]
    else:
        cfg[section] = None
    (tmp_path / "cfg.yaml").write_text(yaml.safe_dump(cfg))
    monkeypatch.chdir(tmp_path)
    generator = ProjectGenerator(cfg_path="cfg.yaml", output_dir=str(tmp_path / "out"))
    with pytest.raises(Exception, match=section):
        generator.generate()
