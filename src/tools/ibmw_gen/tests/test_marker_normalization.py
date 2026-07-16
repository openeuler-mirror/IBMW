#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

from __future__ import annotations

import pathlib
from dataclasses import dataclass

REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
GOLDEN_DIR = REPO_ROOT / "src/tools/ibmw_gen/tests/golden/marker_normalization"
MARKER_ABSENT = "MARKER_ABSENT"


@dataclass(frozen=True)
class Case:
    kind: str
    path: str
    fixture: str
    relative_path: pathlib.Path
    present: bool


MARKER_BASES = {
    "bare": "IBMW_PUBSUB_BARE_BODY",
    "publisher": "IBMW_PUBSUB_PUBLISHER_BODY",
    "subscriber": "IBMW_PUBSUB_SUBSCRIBER_BODY",
    "loaned_publisher": "IBMW_PUBSUB_LOANED_PUBLISHER_BODY",
    "loaned_subscriber": "IBMW_PUBSUB_LOANED_SUBSCRIBER_BODY",
}


CASE_BARE_HEADER_QUICKSTART = Case(kind="bare", path="header", fixture="quickstart", relative_path=pathlib.Path("src/samples/cpp/quickstart/quickstart_gen/src/hello_world_module/hello_world_module.h"), present=False)
CASE_BARE_CC_QUICKSTART = Case(kind="bare", path="cc", fixture="quickstart", relative_path=pathlib.Path("src/samples/cpp/quickstart/quickstart_gen/src/hello_world_module/hello_world_module.cc"), present=True)
CASE_BARE_SIDECAR_QUICKSTART = Case(kind="bare", path="sidecar", fixture="quickstart", relative_path=pathlib.Path("src/samples/cpp/quickstart/quickstart_gen/src/helloworld_app/main.cc"), present=False)
CASE_BARE_HEADER_EXECUTOR = Case(kind="bare", path="header", fixture="executor", relative_path=pathlib.Path("src/samples/cpp/executor_demo/executor_demo_gen/src/executor_module/executor_module.h"), present=False)
CASE_BARE_CC_EXECUTOR = Case(kind="bare", path="cc", fixture="executor", relative_path=pathlib.Path("src/samples/cpp/executor_demo/executor_demo_gen/src/executor_module/executor_module.cc"), present=True)
CASE_BARE_SIDECAR_EXECUTOR = Case(kind="bare", path="sidecar", fixture="executor", relative_path=pathlib.Path("src/samples/cpp/executor_demo/executor_demo_gen/src/executor_app/main.cc"), present=False)
CASE_PUBLISHER_HEADER_DDS = Case(kind="publisher", path="header", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/transport_demo/transport_demo_gen/src/publisher_module/publisher_module.h"), present=False)
CASE_PUBLISHER_CC_DDS = Case(kind="publisher", path="cc", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/transport_demo/transport_demo_gen/src/publisher_module/publisher_module.cc"), present=True)
CASE_PUBLISHER_SIDECAR_DDS = Case(kind="publisher", path="sidecar", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/transport_demo/transport_demo_gen/src/publisher_module/publisher_module_typesupport.inc.h"), present=False)
CASE_PUBLISHER_HEADER_ROS2 = Case(kind="publisher", path="header", fixture="ros2", relative_path=pathlib.Path("src/samples/cpp/ros2_chn/ros2_chn_gen/src/normal_publisher_module/normal_publisher_module.h"), present=False)
CASE_PUBLISHER_CC_ROS2 = Case(kind="publisher", path="cc", fixture="ros2", relative_path=pathlib.Path("src/samples/cpp/ros2_chn/ros2_chn_gen/src/normal_publisher_module/normal_publisher_module.cc"), present=True)
CASE_PUBLISHER_SIDECAR_ROS2 = Case(kind="publisher", path="sidecar", fixture="ros2", relative_path=pathlib.Path("src/samples/cpp/ros2_chn/ros2_chn_gen/src/normal_publisher_module/normal_publisher_module_typesupport.inc.h"), present=False)
CASE_SUBSCRIBER_HEADER_DDS = Case(kind="subscriber", path="header", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/transport_demo/transport_demo_gen/src/subscriber_module/subscriber_module.h"), present=False)
CASE_SUBSCRIBER_CC_DDS = Case(kind="subscriber", path="cc", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/transport_demo/transport_demo_gen/src/subscriber_module/subscriber_module.cc"), present=True)
CASE_SUBSCRIBER_SIDECAR_DDS = Case(kind="subscriber", path="sidecar", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/transport_demo/transport_demo_gen/src/subscriber_module/subscriber_module_typesupport.inc.h"), present=False)
CASE_SUBSCRIBER_HEADER_ROS2 = Case(kind="subscriber", path="header", fixture="ros2", relative_path=pathlib.Path("src/samples/cpp/ros2_chn/ros2_chn_gen/src/normal_subscriber_module/normal_subscriber_module.h"), present=False)
CASE_SUBSCRIBER_CC_ROS2 = Case(kind="subscriber", path="cc", fixture="ros2", relative_path=pathlib.Path("src/samples/cpp/ros2_chn/ros2_chn_gen/src/normal_subscriber_module/normal_subscriber_module.cc"), present=True)
CASE_SUBSCRIBER_SIDECAR_ROS2 = Case(kind="subscriber", path="sidecar", fixture="ros2", relative_path=pathlib.Path("src/samples/cpp/ros2_chn/ros2_chn_gen/src/normal_subscriber_module/normal_subscriber_module_typesupport.inc.h"), present=False)
CASE_LOANED_PUBLISHER_HEADER_DDS = Case(kind="loaned_publisher", path="header", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_gen/src/point_loaned_publisher_module/point_loaned_publisher.h"), present=False)
CASE_LOANED_PUBLISHER_CC_DDS = Case(kind="loaned_publisher", path="cc", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_gen/src/point_loaned_publisher_module/point_loaned_publisher.cc"), present=False)
CASE_LOANED_PUBLISHER_SIDECAR_DDS = Case(kind="loaned_publisher", path="sidecar", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_gen/src/point_loaned_publisher_module/point_loaned_publisher_typesupport.inc.h"), present=True)
CASE_LOANED_PUBLISHER_HEADER_ICEORYX = Case(kind="loaned_publisher", path="header", fixture="iceoryx", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_iceoryx_gen/src/point_iceoryx_loaned_publisher_module/point_iceoryx_loaned_publisher.h"), present=False)
CASE_LOANED_PUBLISHER_CC_ICEORYX = Case(kind="loaned_publisher", path="cc", fixture="iceoryx", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_iceoryx_gen/src/point_iceoryx_loaned_publisher_module/point_iceoryx_loaned_publisher.cc"), present=False)
CASE_LOANED_PUBLISHER_SIDECAR_ICEORYX = Case(kind="loaned_publisher", path="sidecar", fixture="iceoryx", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_iceoryx_gen/src/point_iceoryx_loaned_publisher_module/point_iceoryx_loaned_publisher_typesupport.inc.h"), present=True)
CASE_LOANED_SUBSCRIBER_HEADER_DDS = Case(kind="loaned_subscriber", path="header", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_gen/src/point_loaned_subscriber_module/point_loaned_subscriber.h"), present=False)
CASE_LOANED_SUBSCRIBER_CC_DDS = Case(kind="loaned_subscriber", path="cc", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_gen/src/point_loaned_subscriber_module/point_loaned_subscriber.cc"), present=False)
CASE_LOANED_SUBSCRIBER_SIDECAR_DDS = Case(kind="loaned_subscriber", path="sidecar", fixture="dds", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_gen/src/point_loaned_subscriber_module/point_loaned_subscriber_typesupport.inc.cc"), present=True)
CASE_LOANED_SUBSCRIBER_HEADER_ICEORYX = Case(kind="loaned_subscriber", path="header", fixture="iceoryx", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_iceoryx_gen/src/point_iceoryx_loaned_subscriber_module/point_iceoryx_loaned_subscriber.h"), present=False)
CASE_LOANED_SUBSCRIBER_CC_ICEORYX = Case(kind="loaned_subscriber", path="cc", fixture="iceoryx", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_iceoryx_gen/src/point_iceoryx_loaned_subscriber_module/point_iceoryx_loaned_subscriber.cc"), present=False)
CASE_LOANED_SUBSCRIBER_SIDECAR_ICEORYX = Case(kind="loaned_subscriber", path="sidecar", fixture="iceoryx", relative_path=pathlib.Path("src/samples/cpp/loaned_message_demo/loaned_message_demo_iceoryx_gen/src/point_iceoryx_loaned_subscriber_module/point_iceoryx_loaned_subscriber_typesupport.inc.cc"), present=True)

def _golden_path(case: Case) -> pathlib.Path:
    return GOLDEN_DIR / f"{case.kind}__{case.path}__{case.fixture}.golden"


def _extract_marker_regions(source_text: str, marker_base: str) -> str:
    begin_token = f"{marker_base}_BEGIN"
    end_token = f"{marker_base}_END"
    regions: list[str] = []
    current_region: list[str] = []
    capturing = False

    for line in source_text.splitlines(keepends=True):
        if begin_token in line:
            assert not capturing, f"nested begin marker for {marker_base}"
            capturing = True
            current_region = [line]
            continue
        if capturing:
            current_region.append(line)
            if end_token in line:
                regions.extend(current_region)
                current_region = []
                capturing = False

    assert not capturing, f"unterminated marker region for {marker_base}"
    return "".join(regions)


def _assert_case(case: Case) -> None:
    """Absent cells use the literal MARKER_ABSENT golden sentinel."""
    source_text = (REPO_ROOT / case.relative_path).read_text(encoding="utf-8")
    golden_text = _golden_path(case).read_text(encoding="utf-8")
    marker_base = MARKER_BASES[case.kind]
    begin_token = f"{marker_base}_BEGIN"
    end_token = f"{marker_base}_END"

    if case.present:
        assert golden_text != MARKER_ABSENT
        assert begin_token in source_text
        assert end_token in source_text
        assert source_text.count(begin_token) == source_text.count(end_token)
        assert _extract_marker_regions(source_text, marker_base) == golden_text
        return

    assert golden_text == MARKER_ABSENT
    assert marker_base not in source_text
    assert _extract_marker_regions(source_text, marker_base) == ""

def test_marker_bare_header_quickstart() -> None:
    _assert_case(CASE_BARE_HEADER_QUICKSTART)

def test_marker_bare_cc_quickstart() -> None:
    _assert_case(CASE_BARE_CC_QUICKSTART)

def test_marker_bare_sidecar_quickstart() -> None:
    _assert_case(CASE_BARE_SIDECAR_QUICKSTART)

def test_marker_bare_header_executor() -> None:
    _assert_case(CASE_BARE_HEADER_EXECUTOR)

def test_marker_bare_cc_executor() -> None:
    _assert_case(CASE_BARE_CC_EXECUTOR)

def test_marker_bare_sidecar_executor() -> None:
    _assert_case(CASE_BARE_SIDECAR_EXECUTOR)

def test_marker_publisher_header_dds() -> None:
    _assert_case(CASE_PUBLISHER_HEADER_DDS)

def test_marker_publisher_cc_dds() -> None:
    _assert_case(CASE_PUBLISHER_CC_DDS)

def test_marker_publisher_sidecar_dds() -> None:
    _assert_case(CASE_PUBLISHER_SIDECAR_DDS)

def test_marker_publisher_header_ros2() -> None:
    _assert_case(CASE_PUBLISHER_HEADER_ROS2)

def test_marker_publisher_cc_ros2() -> None:
    _assert_case(CASE_PUBLISHER_CC_ROS2)

def test_marker_publisher_sidecar_ros2() -> None:
    _assert_case(CASE_PUBLISHER_SIDECAR_ROS2)

def test_marker_subscriber_header_dds() -> None:
    _assert_case(CASE_SUBSCRIBER_HEADER_DDS)

def test_marker_subscriber_cc_dds() -> None:
    _assert_case(CASE_SUBSCRIBER_CC_DDS)

def test_marker_subscriber_sidecar_dds() -> None:
    _assert_case(CASE_SUBSCRIBER_SIDECAR_DDS)

def test_marker_subscriber_header_ros2() -> None:
    _assert_case(CASE_SUBSCRIBER_HEADER_ROS2)

def test_marker_subscriber_cc_ros2() -> None:
    _assert_case(CASE_SUBSCRIBER_CC_ROS2)

def test_marker_subscriber_sidecar_ros2() -> None:
    _assert_case(CASE_SUBSCRIBER_SIDECAR_ROS2)

def test_marker_loaned_publisher_header_dds() -> None:
    _assert_case(CASE_LOANED_PUBLISHER_HEADER_DDS)

def test_marker_loaned_publisher_cc_dds() -> None:
    _assert_case(CASE_LOANED_PUBLISHER_CC_DDS)

def test_marker_loaned_publisher_sidecar_dds() -> None:
    _assert_case(CASE_LOANED_PUBLISHER_SIDECAR_DDS)

def test_marker_loaned_publisher_header_iceoryx() -> None:
    _assert_case(CASE_LOANED_PUBLISHER_HEADER_ICEORYX)

def test_marker_loaned_publisher_cc_iceoryx() -> None:
    _assert_case(CASE_LOANED_PUBLISHER_CC_ICEORYX)

def test_marker_loaned_publisher_sidecar_iceoryx() -> None:
    _assert_case(CASE_LOANED_PUBLISHER_SIDECAR_ICEORYX)

def test_marker_loaned_subscriber_header_dds() -> None:
    _assert_case(CASE_LOANED_SUBSCRIBER_HEADER_DDS)

def test_marker_loaned_subscriber_cc_dds() -> None:
    _assert_case(CASE_LOANED_SUBSCRIBER_CC_DDS)

def test_marker_loaned_subscriber_sidecar_dds() -> None:
    _assert_case(CASE_LOANED_SUBSCRIBER_SIDECAR_DDS)

def test_marker_loaned_subscriber_header_iceoryx() -> None:
    _assert_case(CASE_LOANED_SUBSCRIBER_HEADER_ICEORYX)

def test_marker_loaned_subscriber_cc_iceoryx() -> None:
    _assert_case(CASE_LOANED_SUBSCRIBER_CC_ICEORYX)

def test_marker_loaned_subscriber_sidecar_iceoryx() -> None:
    _assert_case(CASE_LOANED_SUBSCRIBER_SIDECAR_ICEORYX)
