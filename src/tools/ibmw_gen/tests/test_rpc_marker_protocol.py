#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

from __future__ import annotations

import pathlib
import sys
from importlib import import_module
from typing import Protocol, cast

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))

CANONICAL_RPC_SERVER_MARKERS = [
    "IBMW_RPC_SERVER_HANDLER_BODY",
]

CANONICAL_RPC_CLIENT_MARKERS = [
    "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD",
    "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME",
    "IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING",
]

CANONICAL_RPC_ANCHORS = [
    "// IBMW_RPC_SERVER_HANDLER_BODY_BEGIN",
    "// IBMW_RPC_SERVER_HANDLER_BODY_END",
    "// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_BEGIN",
    "// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_END",
    "// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_BEGIN",
    "// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_END",
    "// IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING_BEGIN",
    "// IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING_END",
]


class _TransportSpecProtocol(Protocol):
    rpc_server_markers: list[str]
    rpc_client_markers: list[str]
    marker_anchor_format: str


class _TransportSpecFactory(Protocol):
    __dataclass_fields__: dict[str, object]

    def __call__(self, *, name: str) -> _TransportSpecProtocol: ...


class _TransportsModule(Protocol):
    TransportSpec: _TransportSpecFactory


def test_rpc_marker_protocol_grammar() -> None:
    transports = cast(_TransportsModule, cast(object, import_module("transports")))
    transport_spec_fields = set(transports.TransportSpec.__dataclass_fields__)

    assert "rpc_server_markers" in transport_spec_fields
    assert "rpc_client_markers" in transport_spec_fields
    assert "marker_anchor_format" in transport_spec_fields

    spec = transports.TransportSpec(name="step_a_scaffold")
    assert spec.rpc_server_markers == []
    assert spec.rpc_client_markers == []
    assert spec.marker_anchor_format == "// {marker}_{edge}"

    header_path = REPO_ROOT / "src/api/cpp/rpc_marker.h"
    assert header_path.is_file(), "rpc_marker.h must exist for Step A"

    header_text = header_path.read_text(encoding="utf-8")
    for marker in CANONICAL_RPC_SERVER_MARKERS:
        assert marker in header_text
    for marker in CANONICAL_RPC_CLIENT_MARKERS:
        assert marker in header_text
    for anchor in CANONICAL_RPC_ANCHORS:
        assert anchor in header_text
