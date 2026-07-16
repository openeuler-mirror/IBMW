#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

from __future__ import annotations

import pathlib
import sys
from importlib import import_module
from typing import Protocol, cast

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))


class _MarkerPreserveModule(Protocol):
    MalformedMarkerError: type[ValueError]
    MissingMarkerError: type[ValueError]

    def extract_marker_regions(
        self,
        source_text: str,
        markers: list[str],
    ) -> dict[str, str]: ...

    def preserve_marker_regions(
        self,
        new_template: str,
        old_regions: dict[str, str],
    ) -> str: ...


marker_preserve = cast(
    _MarkerPreserveModule,
    cast(object, import_module("generators.marker_preserve")),
)
MalformedMarkerError = marker_preserve.MalformedMarkerError
MissingMarkerError = marker_preserve.MissingMarkerError
extract_marker_regions = marker_preserve.extract_marker_regions
preserve_marker_regions = marker_preserve.preserve_marker_regions


def test_marker_preserve_round_trip() -> None:
    old_source = """void RpcHandler() {
// IBMW_RPC_SERVER_HANDLER_BODY_BEGIN
  DoBusinessLogic();
  response.ok = true;
// IBMW_RPC_SERVER_HANDLER_BODY_END
}
"""
    new_template = """void RpcHandler() {
// IBMW_RPC_SERVER_HANDLER_BODY_BEGIN
  // regenerated placeholder
// IBMW_RPC_SERVER_HANDLER_BODY_END
}
"""

    regions = extract_marker_regions(
        old_source,
        ["IBMW_RPC_SERVER_HANDLER_BODY"],
    )
    preserved = preserve_marker_regions(new_template, regions)

    assert regions == {
        "IBMW_RPC_SERVER_HANDLER_BODY": "  DoBusinessLogic();\n  response.ok = true;\n",
    }
    assert preserved == old_source


def test_marker_preserve_missing_marker_raises() -> None:
    new_template = """void RpcHandler() {
// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_BEGIN
  // regenerated placeholder
// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_END
}
"""

    try:
        _ = preserve_marker_regions(
            new_template,
            {"IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD": "  request.a = 1;\n"},
        )
    except MissingMarkerError as exc:
        assert "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD" in str(exc)
    else:
        raise AssertionError("expected MissingMarkerError")


def test_marker_preserve_malformed_marker_raises() -> None:
    malformed_source = """void RpcLoop() {
// IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING_BEGIN
  HandleStatus();
}
"""

    try:
        _ = extract_marker_regions(
            malformed_source,
            ["IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING"],
        )
    except MalformedMarkerError as exc:
        assert "IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING" in str(exc)
    else:
        raise AssertionError("expected MalformedMarkerError")
