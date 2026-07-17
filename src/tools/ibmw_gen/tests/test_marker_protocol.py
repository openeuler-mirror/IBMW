#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

from __future__ import annotations

import pathlib
import sys
from importlib import import_module
from typing import Callable, TypedDict, cast

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))


class _TypesupportInfo(TypedDict):
    msg_type_cpp: str
    inc_h_content: str
    inc_cc_content: str


def _extract_marker_regions(source_text: str, markers: list[str]) -> dict[str, str]:
    module = import_module("generators.marker_preserve")
    extract = cast(
        Callable[[str, list[str]], dict[str, str]],
        getattr(module, "extract_marker_regions"),
    )
    return extract(source_text, markers)


def _preserve_marker_regions(new_template: str, old_regions: dict[str, str]) -> str:
    module = import_module("generators.marker_preserve")
    preserve = cast(
        Callable[[str, dict[str, str]], str],
        getattr(module, "preserve_marker_regions"),
    )
    return preserve(new_template, old_regions)


def _resolve_rpc_markerized_typesupport(
    mod: dict[str, object],
    ns: str,
    role: str,
) -> _TypesupportInfo:
    module = import_module("generators.module_gen")
    resolve = cast(
        Callable[[dict[str, object], str, str], _TypesupportInfo],
        getattr(module, "_resolve_rpc_markerized_typesupport"),
    )
    return resolve(mod, ns, role)


def _dds_rpc_server_mod() -> dict[str, object]:
    return {
        "name": "demo_rpc_server",
        "type": "rpc_server",
        "transport": "dds",
        "_dds_namespace_cpp": "ibmw::protocols::demo",
        "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
        "_dds_service_info": {
            "name": "CalculatorService",
            "methods": [{
                "name": "Add",
                "request": "Add_Request",
                "response": "Add_Response",
            }],
        },
    }


def test_marker_protocol_round_trip_preserves_user_region_during_regen() -> None:
    mod = _dds_rpc_server_mod()
    ts_info = _resolve_rpc_markerized_typesupport(mod, "ibmw::demo", "server")
    fresh_output = ts_info["inc_cc_content"]
    marker = "IBMW_RPC_SERVER_HANDLER_BODY_ADD"
    begin_anchor = f"  // {marker}_BEGIN"
    end_anchor = f"  // {marker}_END"
    user_customization = "  // USER CUSTOMIZATION\n  rsp.sum = req.a + req.b;\n"

    begin_count = fresh_output.count(begin_anchor)
    end_count = fresh_output.count(end_anchor)
    assert begin_count == 1
    assert end_count == 1

    old_output = fresh_output.replace(
        f"{begin_anchor}\n",
        f"{begin_anchor}\n{user_customization}",
        1,
    )
    canonical_old_output = old_output.replace(begin_anchor, begin_anchor[2:]).replace(
        end_anchor,
        end_anchor[2:],
    )
    canonical_fresh_output = fresh_output.replace(begin_anchor, begin_anchor[2:]).replace(
        end_anchor,
        end_anchor[2:],
    )

    preserved_regions = _extract_marker_regions(canonical_old_output, [marker])
    merged_output = _preserve_marker_regions(canonical_fresh_output, preserved_regions)

    expected_region = user_customization + "  MW_INFO(\"Add called\");\n  // TODO: implement handler logic\n"

    assert user_customization in old_output
    assert preserved_regions == {marker: expected_region}
    assert begin_anchor[2:] in merged_output
    assert end_anchor[2:] in merged_output
    assert user_customization in merged_output
    assert "MW_INFO(\"Add called\");" in merged_output
    assert "// TODO: implement handler logic" in merged_output
    assert merged_output != fresh_output
