#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# TOOL-025: dds_idl_gen_rpc.py only writes inside --output_path, whatever
# directories the YAML idl_file names.
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "dds_idl_gen_rpc" / "dds_idl_gen_rpc.py"

SERVICE = """module: demo
idl_file: {idl_file}
services:
  - name: AddTwoIntsService
    methods:
      - name: AddTwoInts
        request: AddTwoInts_Request
        response: AddTwoInts_Response
"""


def _run(tmp_path, idl_file):
    service = tmp_path / "svc.yaml"
    service.write_text(SERVICE.format(idl_file=idl_file))
    out = tmp_path / "work" / "out"
    result = subprocess.run(
        [sys.executable, str(SCRIPT), f"--service_file={service}", f"--output_path={out}"],
        capture_output=True, text=True, timeout=60)
    written = sorted(p.relative_to(tmp_path).as_posix() for p in tmp_path.rglob("*.ibmw_rpc.dds.*"))
    return result, written


@pytest.mark.parametrize("idl_file", ["../../Escape.idl", "sub/dir/Escape.idl"])
def test_outputs_stay_in_output_path(tmp_path, idl_file):
    result, written = _run(tmp_path, idl_file)
    assert result.returncode == 0, result.stderr
    assert written == ["work/out/Escape.ibmw_rpc.dds.cc", "work/out/Escape.ibmw_rpc.dds.h"]


@pytest.mark.parametrize("idl_file", ["'..'", "'bad name.idl'"])
def test_invalid_stem_is_rejected(tmp_path, idl_file):
    result, written = _run(tmp_path, idl_file)
    assert result.returncode != 0
    assert "invalid idl_file" in result.stderr
    assert written == []
