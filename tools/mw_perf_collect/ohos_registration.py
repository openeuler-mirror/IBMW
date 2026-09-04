"""Host-owned pre-capture trust registrations for OHOS remote P0a."""

from __future__ import annotations

from datetime import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Any

from . import core
from .json_strict import loads as strict_json_loads

SCHEMA = "mw_perf.ohos_p0a_registration.v1"
APPROVAL_SCHEMA = "mw_perf.ohos_p0a_operator_approval.v1"
CAPABILITY = "dds.ordinary.nonplain.single_driver.sync_callback"
THREAT_MODEL = "cooperative_contained_sut_no_active_same_uid"
BUILD_FORMULA = "sha256(canonical_json(source,toolchain,capture_agent,runtime_images))"
RUNTIME_LAYOUT = (("sut", "exe"), ("extension", "maps"), ("fastdds", "maps"),
                  ("fastcdr", "maps"), ("cpp_runtime", "maps"))
TRUST_CLASSIFICATION = {
    "preregistered_declarations": [
        "device.serial", "device.os_name", "device.os_version", "device.build_id",
        "device.abi", "device.os_image_sha256", "toolchain", "source",
        "approval_artifact",
    ],
    "runtime_attested_facts": [
        "boot_id", "agent_executable_sha256", "five_live_image_identities",
    ],
    "self_contained_authority": "retained_consistency_only",
    "external_authority": "requires_separately_supplied_expected_roots",
}
MAX_BYTES = 1024 * 1024


class OhosRegistrationError(ValueError):
    pass


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("ascii") + b"\n"


def build_identity_sha256(registration: dict[str, Any]) -> str:
    material = {key: registration[key] for key in
                ("source", "toolchain", "capture_agent", "runtime_images")}
    return hashlib.sha256(canonical_bytes(material)[:-1]).hexdigest()


def preregistration_declaration_sha256(registration: dict[str, Any]) -> str:
    fields = ("schema", "registration_role", "registration_id", "capability_id", "campaign_id", "device",
               "exact_boot_id", "toolchain", "source", "capture_agent", "runtime_images",
              "threat_model", "production_anchor")
    fields = tuple(key for key in fields if key in registration)
    return hashlib.sha256(canonical_bytes({key: registration[key] for key in fields})[:-1]).hexdigest()


def validate_operator_approval(value: Any, *, raw: bytes | None = None,
                               expected_sha256: str | None = None) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise OhosRegistrationError("operator approval JSON object required")
    schema = strict_json_loads(
        (Path(__file__).parent / "schemas/ohos-p0a-operator-approval.schema.v1.json").read_bytes())
    errors = core._schema_errors(value, schema)
    if errors:
        raise OhosRegistrationError(f"operator approval schema mismatch: {errors[0]}")
    if raw is not None and raw != canonical_bytes(value):
        raise OhosRegistrationError("operator approval must use canonical JSON encoding")
    digest = hashlib.sha256(canonical_bytes(value)).hexdigest()
    if expected_sha256 is not None and (not re.fullmatch(r"[0-9a-f]{64}", expected_sha256)
                                        or digest != expected_sha256):
        raise OhosRegistrationError("operator approval SHA256 mismatch")
    try:
        timestamp = datetime.fromisoformat(value["approved_at_utc"].replace("Z", "+00:00"))
    except ValueError as error:
        raise OhosRegistrationError("operator approval timestamp is invalid") from error
    utc_offset = timestamp.utcoffset()
    if (not value["approved_at_utc"].endswith("Z") or utc_offset is None
            or utc_offset.total_seconds() != 0):
        raise OhosRegistrationError("operator approval timestamp must be UTC with Z suffix")
    return value


def validate_registration(value: Any, *, raw: bytes | None = None,
                          expected_sha256: str | None = None,
                          approval: dict[str, Any] | None = None,
                          approval_raw: bytes | None = None,
                          expected_approval_sha256: str | None = None) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise OhosRegistrationError("registration JSON object required")
    schema_raw = (Path(__file__).parent / "schemas/ohos-p0a-registration.schema.v1.json").read_bytes()
    schema = strict_json_loads(schema_raw)
    errors = core._schema_errors(value, schema)
    if errors:
        raise OhosRegistrationError(f"registration schema mismatch: {errors[0]}")
    if raw is not None and raw != canonical_bytes(value):
        raise OhosRegistrationError("registration must use canonical JSON encoding")
    digest = hashlib.sha256(canonical_bytes(value)).hexdigest()
    if expected_sha256 is not None and (not re.fullmatch(r"[0-9a-f]{64}", expected_sha256)
                                        or digest != expected_sha256):
        raise OhosRegistrationError("registration SHA256 mismatch")
    role = value["registration_role"]
    if ((role == "fault_runtime_subject") != ("production_anchor" in value)):
        raise OhosRegistrationError("registration role/production anchor relationship is invalid")
    if value["build_identity"] != {"formula": BUILD_FORMULA,
                                     "sha256": build_identity_sha256(value)}:
        raise OhosRegistrationError("registration build identity mismatch")
    images = value["runtime_images"]
    if [(item["role"], item["source"]) for item in images] != list(RUNTIME_LAYOUT):
        raise OhosRegistrationError("registration production runtime layout mismatch")
    if len({(item["role"], item["name"]) for item in images}) != len(images):
        raise OhosRegistrationError("registration runtime image identity collision")
    if any(item["sha256"] == "0" * 64 for item in images):
        raise OhosRegistrationError("registration runtime image hashes must be nonzero")
    if value["trust_classification"] != TRUST_CLASSIFICATION:
        raise OhosRegistrationError("registration trust classification mismatch")
    if approval is None or approval_raw is None or expected_approval_sha256 is None:
        raise OhosRegistrationError("external operator approval and expected SHA256 are required")
    validate_operator_approval(approval, raw=approval_raw,
                               expected_sha256=expected_approval_sha256)
    approval_digest = hashlib.sha256(approval_raw).hexdigest()
    if (value["approval_artifact"] != {"schema": APPROVAL_SCHEMA,
                                       "sha256": approval_digest}
            or approval["campaign_id"] != value["campaign_id"]
            or approval["capability_id"] != value["capability_id"]
            or approval["device_serial"] != value["device"]["serial"]
            or approval["exact_boot_id"] != value["exact_boot_id"]
            or approval["threat_model"] != value["threat_model"]
            or approval["preregistration_declaration_sha256"] !=
               preregistration_declaration_sha256(value)):
        raise OhosRegistrationError("operator approval does not bind registration")
    for item in [value["capture_agent"], *images]:
        path = PurePosixPath(item["path"])
        if str(path) != item["path"] or ".." in path.parts or "." in path.parts:
            raise OhosRegistrationError("registration contains a noncanonical device path")
    return value


def validate_fault_registration(value: Any, **kwargs: Any) -> dict[str, Any]:
    """Validate a fault registration while retaining the production anchor binding."""
    if not isinstance(value, dict) or value.get("registration_role") != "fault_runtime_subject":
        raise OhosRegistrationError("fault runtime registration role is required")
    return validate_registration(value, **kwargs)


def _read_bound_json(path_value: str | Path, label: str) -> tuple[dict[str, Any], bytes]:
    path = Path(path_value)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as error:
        raise OhosRegistrationError(f"{label} open failed") from error
    try:
        before = os.fstat(fd)
        named = os.stat(path, follow_symlinks=False)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
                or before.st_uid != os.getuid() or stat.S_IMODE(before.st_mode) & 0o022
                or before.st_size < 1 or before.st_size > MAX_BYTES
                or (before.st_dev, before.st_ino) != (named.st_dev, named.st_ino)):
            raise OhosRegistrationError(f"{label} must be a bounded regular file")
        raw = b""
        while len(raw) < before.st_size:
            chunk = os.read(fd, before.st_size - len(raw))
            if not chunk:
                raise OhosRegistrationError(f"{label} was truncated")
            raw += chunk
        after = os.fstat(fd)
        if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
            raise OhosRegistrationError(f"{label} changed while reading")
    finally:
        os.close(fd)
    try:
        value = strict_json_loads(raw)
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise OhosRegistrationError(f"{label} JSON is invalid") from error
    if not isinstance(value, dict):
        raise OhosRegistrationError(f"{label} JSON object required")
    return value, raw


def read_operator_approval(path_value: str | Path,
                           expected_sha256: str) -> tuple[dict[str, Any], bytes]:
    value, raw = _read_bound_json(path_value, "operator approval")
    return validate_operator_approval(value, raw=raw,
                                      expected_sha256=expected_sha256), raw


def read_registration(path_value: str | Path, expected_sha256: str, *,
                      approval_path: str | Path,
                      expected_approval_sha256: str) -> tuple[dict[str, Any], bytes,
                                                              dict[str, Any], bytes]:
    value, raw = _read_bound_json(path_value, "registration")
    approval, approval_raw = read_operator_approval(
        approval_path, expected_approval_sha256)
    validate_registration(value, raw=raw, expected_sha256=expected_sha256,
                          approval=approval, approval_raw=approval_raw,
                          expected_approval_sha256=expected_approval_sha256)
    return value, raw, approval, approval_raw
