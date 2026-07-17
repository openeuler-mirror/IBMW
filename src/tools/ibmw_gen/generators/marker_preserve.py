# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

"""Utilities for preserving RPC marker-delimited user regions."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class _MarkerSpan:
    """Line-index span for one marker-delimited user region."""

    begin_line: int
    end_line: int


class MissingMarkerError(ValueError):
    """Raised when a requested marker region is absent from a template."""


class MalformedMarkerError(ValueError):
    """Raised when marker anchors are incomplete, duplicated, or inverted."""


def _anchor_lines(marker: str) -> tuple[str, str]:
    """Return canonical BEGIN/END anchor lines for a marker stem."""

    return (f"// {marker}_BEGIN", f"// {marker}_END")


def _find_marker_spans(source_text: str, markers: list[str]) -> dict[str, _MarkerSpan]:
    """Locate marker-delimited line spans within source text."""

    lines = source_text.splitlines(keepends=True)
    spans: dict[str, _MarkerSpan] = {}

    for marker in markers:
        begin_anchor, end_anchor = _anchor_lines(marker)
        begin_indices = [idx for idx, line in enumerate(lines) if line.rstrip("\n") == begin_anchor]
        end_indices = [idx for idx, line in enumerate(lines) if line.rstrip("\n") == end_anchor]

        if not begin_indices and not end_indices:
            raise MissingMarkerError(f"marker '{marker}' not found")
        if len(begin_indices) != 1 or len(end_indices) != 1:
            raise MalformedMarkerError(
                f"marker '{marker}' must have exactly one BEGIN and one END anchor"
            )

        begin_idx = begin_indices[0]
        end_idx = end_indices[0]
        if end_idx <= begin_idx:
            raise MalformedMarkerError(
                f"marker '{marker}' END anchor must appear after BEGIN anchor"
            )

        spans[marker] = _MarkerSpan(begin_line=begin_idx, end_line=end_idx)

    return spans


def extract_marker_regions(source_text: str, markers: list[str]) -> dict[str, str]:
    """Extract user content between canonical BEGIN/END marker anchors.

    Args:
        source_text: Generated source text containing marker-delimited regions.
        markers: Canonical marker stems such as
            ``IBMW_RPC_SERVER_HANDLER_BODY``.

    Returns:
        Mapping from marker stem to the exact text between its BEGIN/END lines.

    Raises:
        MissingMarkerError: If any requested marker is absent.
        MalformedMarkerError: If any marker pair is incomplete, duplicated, or
            ordered incorrectly.
    """

    lines = source_text.splitlines(keepends=True)
    spans = _find_marker_spans(source_text, markers)
    return {
        marker: ''.join(lines[span.begin_line + 1:span.end_line])
        for marker, span in spans.items()
    }


def preserve_marker_regions(new_template: str, old_regions: dict[str, str]) -> str:
    """Splice preserved user regions into a freshly generated template.

    Args:
        new_template: Fresh generator output containing canonical marker anchors.
        old_regions: Mapping of marker stem to previously preserved user text.

    Returns:
        New template text with each marker-delimited region replaced by the
        preserved user text.

    Raises:
        MissingMarkerError: If any requested marker is absent from the template.
        MalformedMarkerError: If any marker pair is incomplete, duplicated, or
            ordered incorrectly.
    """

    if not old_regions:
        return new_template

    lines = new_template.splitlines(keepends=True)
    spans = _find_marker_spans(new_template, list(old_regions))
    output: list[str] = []
    cursor = 0

    for marker, span in sorted(spans.items(), key=lambda item: item[1].begin_line):
        output.extend(lines[cursor:span.begin_line + 1])
        output.append(old_regions[marker])
        output.extend(lines[span.end_line:span.end_line + 1])
        cursor = span.end_line + 1

    output.extend(lines[cursor:])
    return ''.join(output)
