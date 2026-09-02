"""Strict JSON decoding shared by trusted contracts and collected artifacts."""
from __future__ import annotations

import json
from typing import Any


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON object key: {key}")
        value[key] = item
    return value


def loads(data: str | bytes | bytearray) -> Any:
    if isinstance(data, (bytes, bytearray)):
        data = bytes(data).decode("utf-8")
    return json.loads(
        data,
        object_pairs_hook=_unique_object,
        parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
    )
