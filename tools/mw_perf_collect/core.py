"""Common standard-library collection lifecycle for the MVP CLI."""
from __future__ import annotations

import argparse, fnmatch, hashlib, json, math, os, re, signal, stat, subprocess, sys, time, uuid
from pathlib import Path
from typing import Any

from .json_strict import loads as strict_json_loads

VERSION = "0.1.0-mvp"
EXIT_OK, EXIT_PARTIAL, EXIT_INVALID, EXIT_FAILED, EXIT_CLEANUP = 0, 10, 20, 30, 40

# These are package data, not user supplied evidence.  Keep the registry in the
# importer so an installed validator cannot silently accept a changed or
# incomplete schema/contract directory.
_RESOURCE_SHA256 = {
    "contracts/dds-exact-callback-contract.v1.json": "7eda97eae22e24b1c3e392e7a4ae4f02120742da9dd9a82802e41bd13b8b2b71",
    "contracts/metric-qualification-contract.v1.json": "05e1442ffac03795ad19cdff59ac5f14d321aea1052df3a1d3e4f15783e1658d",
    "contracts/p0a-evidence-contract.v1.json": "9bc17677e794d4341656b16af66ff121211e3bd3d4dba8d162a889c41a3e28eb",
    "contracts/p0a-evidence-contract.v2.json": "dc2da2f476bbffe4e0728455279e8841b00ae01b3bf57612f3e84aa7dedcddd5",
    "contracts/p0a-evidence-contract.v3.json": "e122d6b8dd774c5c2a458f5557624cc5fa49c9d259e9b2a604344310020c2c9d",
    "contracts/p0a-evidence-contract.v4.json": "99ba12db651bec747d9d03b46ae48a84db3015bd4e485852448c49d82b164cf2",
    "contracts/p0a-evidence-contract.v5.json": "d564dbaf47ac11101c691c66168d5708025640b563d96378c0a94e43fb8d7004",
    "contracts/p0a-evidence-contract.v6.json": "8bf1394a3744c9023365b679c6c2cf1063fbb9d143e5e0e6c1fd65176af3413e",
    "contracts/p0a-evidence-contract.v7.json": "eb3bc93735bfe6adb6bfbffa67bfd4b6d3cf51658c7bc76a1d0b64fc41e9cc29",
    "contracts/p0a-evidence-contract.v8.json": "c8f76997156ecd6e608f0d5d4a0d756d3e5748e8b21c67895936ee004a614575",
    "contracts/p0a-evidence-contract.v9.json": "4ecef870afdc652853f40bc3d2716d66a6a3304849741a7cb03eef7033fbce11",
    "contracts/p0a-ohos-remote-contract.v1.json": "618c7e8e17473f19c41871a7e45d58894ab76d9ff28c1df0a7543d1cb78eb427",
    "schemas/comparison-plan.schema.v1.json": "b3c3fe804c3c7ef9897837abc4f756d9c79301eaf293ef00598799ffa7ffeb69",
    "schemas/config.schema.v1.json": "ec6299a21ea2a402d10ae2b2249997a8960d27b1511a07fb5312c1a685ad3bcc",
    "schemas/evidence-campaign-seal.schema.v1.json": "ae0e9c184c79dad0e3d7d931da94f5ef56a131ca1844d45776cbd814040daba4",
    "schemas/evidence-campaign-seal.v2.schema.json": "a3d2b74aa563e9441558f4131fc018e740b5c4774aea7a2812670aadd76dee60",
    "schemas/evidence-campaign-validation.schema.v1.json": "555cdb0213ccf2cf8665ddabc4725479410df447b01d88340282b7d26cbf4d1b",
    "schemas/evidence-campaign-validation.v2.schema.json": "157193442d47a53550ca52175079e913f0d3c98116eb184c3fbfd7aacef24134",
    "schemas/evidence-campaign.schema.v1.json": "6072ce40f9bd6a96df4e2218bfb0f949a85472c48ff9b7b02d3e8393b24e51a4",
    "schemas/evidence-campaign.v2.schema.json": "acfce9b8243861c40fdb65bd00f75bb457742ad31086ec3c0f6b5dfb57091e8d",
    "schemas/evidence-companion-bundle.schema.v1.json": "dd609385fff8e70380e548a670ba92d345c5e20652cb811000e5cd112d5c063e",
    "schemas/fault-evidence-tamper-result.schema.v1.json": "97e63bdb12019a434dd920ea21a03083b170a48cedf9fc601d196d0c90269116",
    "schemas/fault-injection-formal.schema.v2.json": "13f204c058040d1fc2f825d7cf0f3abc82446ef8fe74582521544d575ceb32b5",
    "schemas/fault-runtime-matrix-result.schema.v1.json": "78b3d6acddb215e394097c46932805d9eaffa1ac7ccd6fe3f961533e047b8547",
    "schemas/formal-anchor.schema.v1.json": "0cdde24f1cd3e091ac891b352985d636eb7dcccdc3f9e3e0d3290fc484f9ad22",
    "schemas/metric-qualification-seal.schema.v1.json": "cf6a9c6cd4b4cf0b693dd19a708c0411a3bbb04d4ec5b22fb807b2d98d924678",
    "schemas/metric-qualification.schema.v1.json": "f0309ec5aa2a5495516459f4aab14de5618573805778eda9523864a40f77480a",
    "schemas/p0a-evidence-manifest.schema.v1.json": "66cd0f6aba140056cad8717c9b02268dddf56f4433c16140dd231cd22b2317d6",
    "schemas/result.schema.v1.json": "4895699463484942340973dc6bf370b9c6a1c03dda3848b72fdfb4cc0156d16f",
    "schemas/rollback-build-plan.schema.v1.json": "feb47456d8513d385912b077f9285c4582bf7f4d1e2fc1c23883e8b999558e35",
    "schemas/rollback-evidence-manifest.schema.v1.json": "0bc71a2ef9f222f5a224ed3f60d73f64b02f077eb09e996242654d77fd6a4c3d",
    "schemas/rollback-formal.schema.v2.json": "df576ed0d139de424747a9fa6c821d195b6a4d46d83359aeee109eabbd59ef24",
    "schemas/rollback-link-closure.schema.v1.json": "21c262fa24dd368e063c85c923e881c560c474b6ec0c4c4630e417295b23086a",
    "schemas/rollback-runtime-resolution.schema.v1.json": "60eb8404625997c2c83e0f06a10b913b20e1fd17ba050c480c80b42d515b2644",
    "schemas/rollback-trace-off-result.schema.v1.json": "5f2eb0f6e214d54ad64f9ccdef0498fd4c6051f803e73b2bd38e045c42c4ffd0",
    "schemas/rollback-trace-off-result.schema.v2.json": "5c067cebb391df7e664c37c7ed745004a57c1242cf752b172e1d4bdaf3bb13c3",
    "schemas/ros2-preflight.schema.v1.json": "acd086154fa06ffe549245ae1f619eed4f3a9d28a72733534ad396ae60108167",
    "schemas/runtime-matrix-evidence-manifest.schema.v1.json": "fe089ace1f388de75dd891edb3ba8f6004b9e86c2f5bddac6deb5c6c835dead1",
    "schemas/runtime-matrix-manifest.schema.v1.json": "bf9caff1194b2405e5da54a481fa5f57a40188f66b24b80652c2708eeeb8e7b4",
    "schemas/trace-quality-formal.schema.v2.json": "be32ccaf29ee017c2bdb75885ffef936bd0e21d743171c8180c226b07fe50f8a",
    "schemas/trace-quality.schema.v1.json": "741835b8644e715eabb187ddfcaf4517f2e13df9468daa4d008da1fb493641ed",
    "schemas/trusted-external-roots.schema.v1.json": "ebd7711c0c8fcb537a62e90e1d53bd84db10b0cfc3faeeb7bac7dfc1017067a1",
    "schemas/validation.schema.v1.json": "2a859e48c5a6e89b26d62b9ff2019fcab6ec120450cdb0801b8380823099e95d",
    "schemas/ohos-capture-request.schema.v2.json": "42ea56e132e0e1027d7d6c6bb8cc4fe9b9c3cfad3843b53e0999cc15d9875850",
    "schemas/ohos-device-completion.schema.v2.json": "38ad4f8eb4e60bfdab9364f1ef9f2e1aaa7ca428ad3a5eba870e7b17edb79dfc",
    "schemas/ohos-runtime-attestation.schema.v2.json": "9a325494bb823a557afb48a89571913613e8b61d26c65b2f2b58fad379dadfc6",
    "schemas/ohos-capture-result.schema.v2.json": "763cc10fb8155732caa8c1f0fc75e281dc327d83a3163450562f5c119ae4143a",
    "schemas/ohos-capture-manifest.schema.v2.json": "568bc6f3fcc163cfc730b8fd3f5175ca2743ac46d0117e39bd9e7cb9cd08b91d",
    "schemas/ohos-remote-capture-envelope.schema.v1.json": "7acb2c26dda75e254f5cb7c12f5b4e6e6e3488e1ad678a8b8c585c896c5911f8",
    "schemas/ohos-p0a-operator-approval.schema.v1.json": "553390734c6b3a6748f620e018ab79c7c8c0fc3cb47e5df3dce16034d6a9751d",
    "schemas/ohos-p0a-registration.schema.v1.json": "e5ab174c16075a1346e29197bd3d5a5cc910fc4e9ce2edb1ba4ce45e6c7d8c0e",
    "schemas/p0a-ohos-remote-resolution.schema.v1.json": "5ee8f187e0dcc4ad56dc1cdb22fa5ae90b0f14890fc588056ff04f53b7e5bb60",
    "schemas/p0a-ohos-remote-resolution-declarations.schema.v1.json": "a4d4040bc7c04b01bda14945e8fea5f09b7499c0e4e647063bfb0cc986513f75",
    "schemas/trace-quality-ohos-remote.schema.v1.json": "f0f06bb2d53da119113a1a52ee91d724899151641393ad82f7f7ecf33c231c1a",
}

def _resource_integrity_errors() -> list[str]:
    root = Path(__file__).parent
    errors = []
    for relative, expected in _RESOURCE_SHA256.items():
        try:
            actual = hashlib.sha256((root / relative).read_bytes()).hexdigest()
        except OSError:
            errors.append(f"RESOURCE_MISSING:{relative}")
        else:
            if actual != expected:
                errors.append(f"RESOURCE_TAMPERED:{relative}")
    return errors


def _schema_descriptor(filename: str, role: str, validation_mode: str) -> dict[str, Any]:
    """Describe the installed schema without depending on its parsed encoding."""
    relative = f"schemas/{filename}"
    descriptor: dict[str, Any] = {
        "role": role, "schema_file": relative, "validation_mode": validation_mode,
    }
    match = re.search(r"\.v(\d+)(?:\.schema)?\.json$", filename)
    if match:
        descriptor["schema_version"] = int(match.group(1))
    try:
        raw = (Path(__file__).parent / "schemas" / filename).read_bytes()
        document = strict_json_loads(raw)
        if not isinstance(document, dict) or not isinstance(document.get("$id"), str):
            raise ValueError("schema $id is missing")
        descriptor.update({"schema_id": document["$id"],
                           "schema_sha256": hashlib.sha256(raw).hexdigest()})
    except (OSError, UnicodeError, ValueError, TypeError, json.JSONDecodeError) as exc:
        descriptor.update({"schema_id": None, "schema_sha256": None,
                           "failure_code": f"SCHEMA_AUDIT_{type(exc).__name__.upper()}"})
    return descriptor


def _schema_audit(documents: dict[str, tuple[str, str]] | None = None,
                  validation_mode: str = "standard") -> dict[str, Any]:
    """Return stable provenance for schemas used to produce an artifact."""
    documents = documents or {
        "validation": ("validation.schema.v1.json", "validation"),
        "result": ("result.schema.v1.json", "result"),
        "comparison_plan": ("comparison-plan.schema.v1.json", "comparison_plan"),
    }
    return {
        "audit_version": 1,
        "validator": {"implementation": "mw_perf_collect.core", "version": VERSION},
        "resource_integrity_errors": _resource_integrity_errors(),
        "schemas": {role: _schema_descriptor(filename, descriptor_role, validation_mode)
                    for role, (filename, descriptor_role) in documents.items()},
    }


def _validation_matches_current(recorded: Any, current: Any) -> bool:
    """Compare archived validation with current semantics across audit rollout.

    Older, already-sealed campaigns predate ``schema_audit`` and the
    ``resource_integrity`` rule.  These are validator metadata/guard rollout
    additions, not a change to the result represented by the archived
    validation.  Accept their absence only for legacy records; current
    validation still evaluates resource integrity and remains fail-closed.
    """
    if isinstance(recorded, dict) and "schema_audit" not in recorded:
        expected = dict(current) if isinstance(current, dict) else current
        if isinstance(expected, dict):
            expected.pop("schema_audit", None)
            recorded_rules = recorded.get("rules")
            if (isinstance(recorded_rules, list)
                    and not any(isinstance(item, dict)
                                and item.get("name") == "resource_integrity"
                                for item in recorded_rules)):
                expected["rules"] = [item for item in expected.get("rules", [])
                                      if not (isinstance(item, dict)
                                              and item.get("name") == "resource_integrity")]
        return recorded == expected
    return recorded == current
def load_config(path: str | None) -> dict[str, Any]:
    if not path: return {}
    p = Path(path); text = p.read_text(encoding="utf-8")
    if p.suffix.lower() == ".json":
        return strict_json_loads(text)
    try:
        import yaml  # type: ignore
    except ImportError as exc:
        raise RuntimeError("YAML config requires optional PyYAML; use JSON or install PyYAML") from exc
    return yaml.safe_load(text) or {}


def effective_validation_profile(config: dict[str, Any]) -> str:
    """Resolve the validation profile once, rejecting conflicting declarations."""
    if not isinstance(config, dict):
        raise TypeError("config must be an object")
    ros2 = _adapter_config(config, "ros2")
    ibmw = _adapter_config(config, "ibmw")
    declarations: list[tuple[str, Any]] = []
    if "p0a_evidence" in config:
        declarations.append(("p0a_evidence", config["p0a_evidence"]))
    if "p0a_evidence" in ibmw:
        declarations.append(("adapters.ibmw.p0a_evidence", ibmw["p0a_evidence"]))
    for location, value in declarations:
        if not isinstance(value, dict):
            raise ValueError(f"{location} must be an object")
    if len(declarations) == 2 and declarations[0][1] != declarations[1][1]:
        raise ValueError("conflicting p0a_evidence declarations")
    instrumented = ros2.get("validation_instrumented")
    if instrumented is not None and not isinstance(instrumented, bool):
        raise ValueError("adapters.ros2.validation_instrumented must be boolean")
    deployment_mode = ros2.get("deployment_mode")
    if deployment_mode is not None and not isinstance(deployment_mode, str):
        raise ValueError("adapters.ros2.deployment_mode must be a string")
    run_types = [value.get("run_type") for _, value in declarations if "run_type" in value]
    if any(not isinstance(value, str) or not value for value in run_types):
        raise ValueError("p0a_evidence.run_type must be a non-empty string")
    explicit = config.get("validation_profile")
    if explicit is not None and explicit not in ("standard", "p0a"):
        raise ValueError("validation_profile must be standard or p0a")
    p0a_signal = (bool(declarations) or instrumented is True
        or deployment_mode == "validation_instrumented" or any(
        value in ("p0a_proof", "exact_run") for value in run_types)
    )
    resolved = "p0a" if p0a_signal else "standard"
    if explicit is not None and explicit != resolved:
        raise ValueError("validation_profile conflicts with effective profile signals")
    return resolved


def validate_ros2_preflight_schema(value: Any) -> list[str]:
    """Runtime validation for the security-relevant preflight schema subset."""
    errors: list[str] = []
    if not isinstance(value, dict) or value.get("schema") != "mw_perf.ros2.preflight.v1":
        return ["root"]
    try:
        schema = strict_json_loads((Path(__file__).parent / "schemas/ros2-preflight.schema.v1.json").read_bytes())
        errors.extend(f"json_schema:{item}" for item in _schema_errors(value, schema))
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
        return [f"schema_unavailable:{type(exc).__name__}"]
    required = ("pid", "ros2_command", "environment", "ros2_pkg_prefix", "process_identity", "process",
                "ros2_tracing", "middleware_libraries", "required_runtime_libraries", "claims")
    errors.extend(f"missing:{key}" for key in required if key not in value)
    identity, tracing = value.get("process_identity"), value.get("ros2_tracing")
    libraries, required_libraries = value.get("middleware_libraries"), value.get("required_runtime_libraries")
    if not isinstance(identity, dict) or not isinstance(identity.get("valid"), bool):
        errors.append("process_identity")
    if not isinstance(tracing, dict):
        errors.append("ros2_tracing")
    else:
        flags = ("command_available", "session_command_usable", "tracepoints_enumerated",
                 "required_tracepoints_present", "available")
        if any(not isinstance(tracing.get(flag), bool) for flag in flags):
            errors.append("ros2_tracing.flags")
        required_tracepoints = tracing.get("required_tracepoints")
        if not isinstance(required_tracepoints, list) or any(
                not isinstance(item, str) or not item for item in required_tracepoints):
            errors.append("ros2_tracing.required_tracepoints")
        if tracing.get("available") is True and (not required_tracepoints or any(
                tracing.get(flag) is not True for flag in flags[:-1])):
            errors.append("ros2_tracing.available_condition")
    if not isinstance(libraries, list):
        errors.append("middleware_libraries")
    else:
        for index, library in enumerate(libraries):
            if not isinstance(library, dict) or library.get("status") not in (
                    "candidate", "identity_verified", "unverifiable_deleted"):
                errors.append(f"middleware_libraries:{index}")
                continue
            if library.get("capability_verified") is True and not (
                    library.get("verification") == "identity_verified"
                    and library.get("expected_identity_match") is True):
                errors.append(f"middleware_libraries:{index}:capability")
    if not isinstance(required_libraries, dict) or not isinstance(
            required_libraries.get("all_capability_verified"), bool):
        errors.append("required_runtime_libraries")
    return errors


def _ros2_preflight_gate(result: Any, options: dict[str, Any], required: bool) -> dict[str, Any]:
    if not required:
        return {"required": False, "ok": True, "failure_reasons": []}
    observations = result.get("observations", []) if isinstance(result, dict) else []
    record = next((item for item in observations if isinstance(item, dict)
                   and item.get("name") == "ros2_preflight"), None)
    value = record.get("value") if isinstance(record, dict) else None
    schema_errors = validate_ros2_preflight_schema(value)
    reasons = [f"schema:{error}" for error in schema_errors]
    if not schema_errors:
        assert isinstance(value, dict)
        if value["ros2_command"].get("trusted") is not True:
            reasons.append("trusted_ros2_command_unavailable")
        if value["process_identity"].get("valid") is not True:
            reasons.append("pid_identity_unstable")
        libraries = value["middleware_libraries"]
        if any(item.get("required") is True and item.get("mapping", {}).get("deleted") is True
               for item in libraries if isinstance(item, dict)):
            reasons.append("required_library_deleted_mapping")
        if value["required_runtime_libraries"].get("all_capability_verified") is not True:
            reasons.append("required_runtime_libraries_unverified")
        tracing_config = options.get("tracing", {})
        tracing_required = (options.get("tracing_required") is True or
                            isinstance(tracing_config, dict) and tracing_config.get("required") is True)
        if tracing_required and value["ros2_tracing"].get("available") is not True:
            reasons.append("required_tracing_capability_unavailable")
    return {"required": True, "ok": not reasons, "failure_reasons": reasons,
            "schema_valid": not schema_errors}

def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, default=str, allow_nan=False) + "\n", encoding="utf-8")

def proc_stat(pid: int) -> dict[str, Any] | None:
    try:
        raw_stat = Path(f"/proc/{pid}/stat").read_text()
        # comm is parenthesized and may itself contain spaces or parentheses.  The
        # final ')' is the only reliable boundary before the state field.
        close = raw_stat.rfind(")")
        if close >= 0:
            stat = raw_stat[close + 1:].split()
            utime, stime, starttime, rss = stat[11], stat[12], stat[19], stat[21]
        else:  # compatibility with synthetic/legacy fixtures
            stat = raw_stat.split()
            utime, stime, starttime, rss = stat[13], stat[14], stat[21], stat[23]
        status = {x.split(":", 1)[0]: x.split(":", 1)[1].strip() for x in Path(f"/proc/{pid}/status").read_text().splitlines() if ":" in x}
        try:
            rollup = {x.split(":", 1)[0]: x.split(":", 1)[1].strip() for x in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines() if ":" in x}
            pss_bytes = int(rollup["Pss"].split()[0])*1024
        except (OSError, ValueError, KeyError, IndexError):
            pss_bytes = None
        return {"pid": pid, "cpu_seconds": (int(utime)+int(stime))/os.sysconf("SC_CLK_TCK"),
                "rss_bytes": int(rss)*os.sysconf("SC_PAGE_SIZE"), "pss_bytes": pss_bytes,
                "threads": int(status.get("Threads", 0)), "start_time_ticks": int(starttime)}
    except (OSError, ValueError, IndexError): return None

def descendants(pid: int) -> list[int]:
    children: dict[int, list[int]] = {}
    for p in Path("/proc").iterdir():
        if p.name.isdigit():
            try:
                raw = (p/"stat").read_text(); close = raw.rfind(")")
                fields = raw[close + 1:].split() if close >= 0 else raw.split()[2:]
                children.setdefault(int(fields[1]), []).append(int(p.name))
            except (OSError, ValueError, IndexError): pass
    out, todo = [], [pid]
    while todo:
        parent = todo.pop()
        for child in children.get(parent, []): out.append(child); todo.append(child)
    return out

def network_snapshot(interfaces: list[str] | None = None) -> dict[str, Any]:
    root = Path("/sys/class/net"); names = interfaces or ([p.name for p in root.iterdir()] if root.exists() else [])
    out = {}
    for name in names:
        base = root/name/"statistics"
        try: out[name] = {k: int((base/k).read_text()) for k in ("rx_bytes", "tx_bytes", "rx_packets", "tx_packets")}
        except OSError: pass
    return out

def _hash_contract(config: dict[str, Any]) -> str:
    contract = config.get("scenario_contract")
    if not isinstance(contract, dict) or not contract:
        return ""
    return hashlib.sha256(json.dumps(contract, sort_keys=True, separators=(",", ":"),
                                ensure_ascii=False).encode()).hexdigest()


def _valid_ibmw_provenance(value: Any) -> bool:
    if not isinstance(value, dict) or value.get("required") is not True:
        return False
    binaries, source, build = value.get("binaries"), value.get("source"), value.get("build")
    if not isinstance(binaries, list) or not binaries or not isinstance(source, dict) or not isinstance(build, dict):
        return False
    try:
        from .adapters.ibmw import binary_identity, source_identity
        for recorded in binaries:
            if not isinstance(recorded, dict) or binary_identity(recorded.get("declared_path", "")) != recorded:
                return False
        if source_identity(source.get("root", "")) != source:
            return False
        cache = Path(build.get("build_dir", "")) / "CMakeCache.txt"
        if not build.get("configured") or hashlib.sha256(cache.read_bytes()).hexdigest() != build.get("cache_sha256"):
            return False
    except (OSError, ValueError, TypeError, subprocess.SubprocessError):
        return False
    return True

def _file_identity(path: Path) -> dict[str, Any]:
    resolved = path.resolve(strict=True); info = resolved.stat()
    return {"resolved_path": str(resolved), "device": info.st_dev, "inode": info.st_ino,
            "size": info.st_size, "sha256": hashlib.sha256(resolved.read_bytes()).hexdigest()}

def _fd_identity(fd: int, resolved_path: str) -> dict[str, Any]:
    before = os.fstat(fd); digest = hashlib.sha256(); os.lseek(fd, 0, os.SEEK_SET)
    while chunk := os.read(fd, 1024 * 1024): digest.update(chunk)
    after = os.fstat(fd)
    if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
            != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
        raise OSError("runtime image changed while hashing")
    return {"resolved_path": resolved_path, "device": before.st_dev, "inode": before.st_ino,
            "size": before.st_size, "sha256": digest.hexdigest()}

def _observation_id(observation: dict[str, Any]) -> str:
    value = {key: item for key, item in observation.items() if key != "observation_id"}
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()

class _IdentityMismatch(OSError):
    pass


def _open_identity(proc_path: str, resolved_path: str,
                   expected: tuple[int, int] | None = None) -> dict[str, Any]:
    fd = os.open(proc_path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0))
    try:
        info = os.fstat(fd)
        if expected is not None and (info.st_dev, info.st_ino) != expected:
            raise _IdentityMismatch("runtime image does not match maps identity")
        return _fd_identity(fd, resolved_path)
    finally: os.close(fd)

def _maps_identity(pid: int, fields: list[str]) -> dict[str, Any] | None:
    address, permissions, _offset, mapped_device, mapped_inode, mapped_path = fields
    start, end = address.split("-", 1); deleted = mapped_path.endswith(" (deleted)")
    resolved_path = os.path.normpath(mapped_path.removesuffix(" (deleted)"))
    try:
        major, minor = (int(part, 16) for part in mapped_device.split(":", 1))
        expected_device, expected_inode = os.makedev(major, minor), int(mapped_inode)
    except (ValueError, OSError): return None
    expected = (expected_device, expected_inode)
    try:
        identity = _open_identity(f"/proc/{pid}/map_files/{start}-{end}", resolved_path, expected)
        identity_source = "proc_map_file"
    except _IdentityMismatch:
        return None
    except OSError:
        if deleted:
            return None
        try:
            identity = _open_identity(f"/proc/{pid}/root{resolved_path}", resolved_path, expected)
            identity_source = "proc_root_path"
        except OSError:
            return None
    if identity["device"] != expected_device or identity["inode"] != expected_inode: return None
    return {**identity, "identity_source": identity_source,
            "mapping": {"start_address": start, "end_address": end,
                        "permissions": permissions, "device": mapped_device.lower(),
                        "inode": expected_inode, "deleted": deleted}}

def _runtime_observations(pid: int, run_id: str) -> list[dict[str, Any]]:
    observations: list[dict[str, Any]] = []
    for current in [pid] + descendants(pid):
        before = proc_stat(current)
        if before is None: continue
        candidates: list[tuple[str, dict[str, Any]]] = []
        exe_path = f"/proc/{current}/exe"
        try:
            target = os.readlink(exe_path).removesuffix(" (deleted)")
            candidates.append(("exe", {**_open_identity(exe_path, os.path.normpath(target)),
                                       "identity_source": "proc_exe"}))
        except OSError: pass
        try:
            for line in Path(f"/proc/{current}/maps").read_text(errors="replace").splitlines():
                fields = line.split(None, 5)
                if len(fields) == 6 and fields[5].startswith("/"):
                    identity = _maps_identity(current, fields)
                    if identity is not None and "x" in identity["mapping"]["permissions"]:
                        candidates.append(("maps", identity))
        except OSError: pass
        after = proc_stat(current)
        if after is None or after["start_time_ticks"] != before["start_time_ticks"]: continue
        seen: set[tuple[Any, ...]] = set()
        for source, identity in candidates:
            key = (source, identity["resolved_path"], identity["device"], identity["inode"])
            if key in seen: continue
            seen.add(key)
            observation = {"run_id": run_id, "pid": current,
                "start_time_ticks": before["start_time_ticks"], "source": source, **identity}
            observation["observation_id"] = _observation_id(observation)
            observations.append(observation)
    return observations

def _canonical_json_bytes(value: Any, *, newline: bool = False) -> bytes:
    data = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False).encode("utf-8")
    return data + (b"\n" if newline else b"")


def _normalize_resolution_declarations(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not value:
        raise ValueError("p0a_evidence.resolution.runtime_images must be a non-empty list")
    declarations: list[dict[str, Any]] = []
    identities: set[tuple[str, str]] = set()
    targets: set[tuple[str, str]] = set()
    required = {"role", "name", "source", "expected_path", "sha256"}
    for item in value:
        if not isinstance(item, dict) or set(item) != required:
            raise ValueError("resolution runtime image declarations have unknown or missing fields")
        role, name, source = item["role"], item["name"], item["source"]
        expected_path, digest = item["expected_path"], item["sha256"]
        if (not isinstance(role, str) or not role or not isinstance(name, str) or not name
                or source not in ("exe", "maps") or not isinstance(expected_path, str)
                or not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None):
            raise ValueError("invalid resolution runtime image declaration")
        try:
            if (not os.path.isabs(expected_path)
                    or os.path.realpath(expected_path) != expected_path):
                raise ValueError("resolution expected_path must be a canonical absolute file")
            fd = os.open(expected_path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                         | getattr(os, "O_NOFOLLOW", 0))
            try:
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode):
                    raise ValueError("resolution expected_path must be a regular file")
                prelaunch = _fd_identity(fd, expected_path)
            finally:
                os.close(fd)
        except OSError as exc:
            raise ValueError("resolution expected_path must resolve to an existing file") from exc
        if prelaunch["sha256"] != digest:
            raise ValueError("resolution expected_path SHA-256 does not match declaration")
        identity, target = (role, name), (source, expected_path)
        if identity in identities or target in targets:
            raise ValueError("ambiguous resolution runtime image declaration")
        identities.add(identity); targets.add(target)
        declarations.append({"role": role, "name": name, "source": source,
            "expected_path": expected_path, "sha256": digest,
            "expected_device": prelaunch["device"], "expected_inode": prelaunch["inode"],
            "expected_size": prelaunch["size"]})
    return sorted(declarations, key=lambda item: tuple(
        item[field].encode("utf-8") for field in ("role", "name", "source", "expected_path", "sha256")))


def _validate_v5_resolution_roles(declarations: list[dict[str, Any]]) -> None:
    bindings = {(item.get("role"), item.get("source")) for item in declarations}
    if not {("sut", "exe"), ("extension", "maps")}.issubset(bindings):
        raise ValueError("P0a contract v5 requires sut/exe and extension/maps resolution images")


def _resolution_matches(declarations: list[dict[str, Any]],
                        observations: list[dict[str, Any]]) -> list[dict[str, Any]]:
    matches: list[dict[str, Any]] = []
    for declaration in declarations:
        observed = sorted(item["observation_id"] for item in observations
            if item.get("source") == declaration.get("source")
            and item.get("resolved_path") == declaration.get("expected_path")
            and item.get("sha256") == declaration.get("sha256")
            and item.get("device") == declaration.get("expected_device")
            and item.get("inode") == declaration.get("expected_inode")
            and item.get("size") == declaration.get("expected_size")
            and (declaration.get("source") != "maps"
                 or isinstance(item.get("mapping"), dict)
                 and "x" in item["mapping"].get("permissions", "")))
        matches.append({"role": declaration.get("role"), "name": declaration.get("name"),
            "source": declaration.get("source"), "expected_path": declaration.get("expected_path"),
            "expected_sha256": declaration.get("sha256"),
            "expected_device": declaration.get("expected_device"),
            "expected_inode": declaration.get("expected_inode"),
            "expected_size": declaration.get("expected_size"), "observed": observed})
    return matches


def _build_resolution_proof(manifest: dict[str, Any], p0a_config: dict[str, Any],
                            declarations: list[dict[str, Any]],
                            observations: list[dict[str, Any]]) -> tuple[dict[str, Any], bytes]:
    matches = _resolution_matches(declarations, observations)
    if not matches or any(not item["observed"] for item in matches):
        raise ValueError("not all resolution declarations were observed exactly")
    matched_ids = {identifier for item in matches for identifier in item["observed"]}
    matched = sorted((item for item in observations if item.get("observation_id") in matched_ids),
                     key=lambda item: item["observation_id"])
    proof = {
        "schema": "mw_perf.p0a.resolution.v2", "run_id": manifest["run_id"],
        "capability_id": str(p0a_config.get("capability_id")),
        "path_id": str(p0a_config.get("path_id", "default")),
        "deployment_mode": "managed", "finalized": True, "resolved": True,
        "conclusion": "pass", "producer": "mw_perf_collect",
        "declaration_sha256": hashlib.sha256(_canonical_json_bytes(declarations)).hexdigest(),
        "declarations": declarations, "observations": matched,
        "matches": _resolution_matches(declarations, matched),
    }
    return proof, _canonical_json_bytes(proof, newline=True)

def _runtime_binary_identity(binaries: Any, resolved_path: str) -> dict[str, Any] | None:
    if not isinstance(binaries, list):
        return None
    candidates = [item for item in binaries
                  if isinstance(item, dict) and item.get("resolved_path") == resolved_path]
    if not candidates:
        return None
    identity = candidates[0]
    if (not isinstance(identity.get("sha256"), str) or len(identity["sha256"]) != 64
            or any(char not in "0123456789abcdef" for char in identity["sha256"])
            or not isinstance(identity.get("size"), int) or isinstance(identity.get("size"), bool)
            or identity["size"] < 0
            or any(not isinstance(identity.get(field), int) or isinstance(identity.get(field), bool)
                   for field in ("device", "inode"))):
        return None
    for candidate in candidates[1:]:
        if candidate.get("sha256") != identity["sha256"] or candidate.get("size") != identity["size"]:
            return None
        for field in ("device", "inode"):
            if candidate.get(field) != identity[field]:
                return None
    return identity


def _runtime_image_result(required: Any, observations: list[dict[str, Any]], run_id: str,
                          binaries: Any = None) -> dict[str, Any]:
    declarations = required if isinstance(required, list) else []
    normalized_observations: list[dict[str, Any]] = []
    for item in observations:
        if isinstance(item, dict):
            observed = dict(item); observed["observation_id"] = _observation_id(observed)
            normalized_observations.append(observed)
    matches: list[dict[str, Any]] = []
    artifacts_valid = True
    for declaration in declarations:
        if not isinstance(declaration, dict):
            return {"schema_version": 1, "run_id": run_id, "required": declarations,
                    "observations": normalized_observations, "matches": matches, "valid": False}
        expected_path = declaration.get("expected_path")
        try:
            expected_resolved = str(Path(expected_path).expanduser().resolve(strict=True)) if isinstance(expected_path, str) and expected_path else None
        except (OSError, ValueError):
            expected_resolved = None
        declared_sha = declaration.get("sha256")
        binary = (_runtime_binary_identity(binaries, expected_resolved)
                  if expected_resolved is not None and binaries is not None else None)
        if binaries is not None and (binary is None
                or declared_sha is not None and declared_sha != binary.get("sha256")):
            artifacts_valid = False
        expected_sha = binary.get("sha256") if binary is not None else declared_sha
        source = declaration.get("source")
        found = [x["observation_id"] for x in normalized_observations
                 if x.get("source") == source
                 and expected_resolved is not None and x.get("resolved_path") == expected_resolved
                 and (source != "maps" or isinstance(x.get("mapping"), dict)
                      and isinstance(x["mapping"].get("permissions"), str)
                      and "x" in x["mapping"]["permissions"])
                 and (expected_sha is None or x.get("sha256") == expected_sha)]
        matches.append({"role": declaration.get("role"), "name": declaration.get("name"),
                        "expected_path": expected_path, "expected_sha256": expected_sha,
                        "source": source, "observed": found})
    return {"schema_version": 1, "run_id": run_id, "required": declarations,
            "observations": normalized_observations, "matches": matches,
            "valid": bool(declarations) and artifacts_valid
            and all(x["observed"] for x in matches)}

def _sampled_process_identities(run_dir: Path) -> set[tuple[int, int]] | None:
    identities: set[tuple[int, int]] = set()
    try:
        with (run_dir/"raw"/"process.jsonl").open(encoding="utf-8") as stream:
            for line in stream:
                record = strict_json_loads(line)
                if not isinstance(record, dict) or not isinstance(record.get("processes"), list): return None
                for process in record["processes"]:
                    if (not isinstance(process, dict)
                            or not isinstance(process.get("pid"), int) or isinstance(process.get("pid"), bool)
                            or not isinstance(process.get("start_time_ticks"), int)
                            or isinstance(process.get("start_time_ticks"), bool)):
                        return None
                    identities.add((process["pid"], process["start_time_ticks"]))
    except (OSError, ValueError, json.JSONDecodeError): return None
    return identities or None

def _valid_runtime_images(value: Any, manifest: dict[str, Any], run_dir: Path) -> bool:
    if not isinstance(value, dict): return False
    evidence_path = run_dir/"raw"/"runtime_images.json"
    try:
        data = evidence_path.read_bytes(); evidence = strict_json_loads(data)
        published = strict_json_loads((Path(__file__).parent/"schemas/result.schema.v1.json").read_bytes())
    except (OSError, ValueError, json.JSONDecodeError): return False
    if not isinstance(evidence, dict):
        return False
    if (_schema_errors(value, published["properties"]["runtime_images"], published)
            or _schema_errors(evidence, published["$defs"]["runtimeImageEvidence"], published)):
        return False
    if hashlib.sha256(data).hexdigest() != value.get("content_sha256") or evidence != value.get("evidence"):
        return False
    if evidence.get("run_id") != manifest.get("run_id") or evidence.get("valid") is not True:
        return False
    declarations = evidence.get("required")
    observations = evidence.get("observations")
    matches = evidence.get("matches")
    sampled = _sampled_process_identities(run_dir)
    manifest_process = (manifest.get("pid"), manifest.get("pid_start_time_ticks"))
    if (not isinstance(declarations, list) or not declarations
            or not isinstance(observations, list) or not observations or sampled is None
            or not isinstance(manifest_process[0], int) or isinstance(manifest_process[0], bool)
            or not isinstance(manifest_process[1], int) or isinstance(manifest_process[1], bool)
            or manifest_process not in sampled
            or not isinstance(matches, list) or len(matches) != len(declarations)):
        return False
    by_id: dict[str, dict[str, Any]] = {}
    for item in observations:
        if (not isinstance(item, dict) or item.get("run_id") != manifest.get("run_id")
                or not isinstance(item.get("pid"), int) or isinstance(item.get("pid"), bool)
                or not isinstance(item.get("start_time_ticks"), int) or isinstance(item.get("start_time_ticks"), bool)
                or not isinstance(item.get("observation_id"), str)
                or item["observation_id"] != _observation_id(item)
                or item["observation_id"] in by_id
                or item.get("source") not in ("exe", "maps")
                or not isinstance(item.get("resolved_path"), str) or not item["resolved_path"]
                or not isinstance(item.get("device"), int) or isinstance(item.get("device"), bool)
                or not isinstance(item.get("inode"), int) or isinstance(item.get("inode"), bool)
                or not isinstance(item.get("size"), int) or isinstance(item.get("size"), bool)
                or not isinstance(item.get("sha256"), str) or len(item["sha256"]) != 64
                or any(char not in "0123456789abcdef" for char in item["sha256"])):
            return False
        identity_source = item.get("identity_source")
        if ((item["source"] == "exe" and identity_source != "proc_exe")
                or (item["source"] == "maps"
                    and identity_source not in ("proc_map_file", "proc_root_path"))):
            return False
        if item["source"] == "maps":
            mapping = item.get("mapping")
            if (not isinstance(mapping, dict) or not isinstance(mapping.get("device"), str)
                    or not isinstance(mapping.get("permissions"), str)
                    or len(mapping["permissions"]) != 4
                    or mapping["permissions"][0] not in "r-"
                    or mapping["permissions"][1] not in "w-"
                    or mapping["permissions"][2] not in "x-"
                    or mapping["permissions"][3] not in "ps"
                    or "x" not in mapping["permissions"]
                    or not isinstance(mapping.get("inode"), int) or isinstance(mapping.get("inode"), bool)
                    or mapping.get("inode") != item.get("inode")
                    or not isinstance(mapping.get("deleted"), bool)
                    or (mapping.get("deleted") and identity_source == "proc_root_path")):
                return False
            try:
                major, minor = (int(part, 16) for part in mapping["device"].split(":", 1))
                if os.makedev(major, minor) != item["device"]: return False
            except (ValueError, OSError): return False
        by_id[item["observation_id"]] = item
    provenance = manifest.get("provenance")
    binaries = provenance.get("binaries") if isinstance(provenance, dict) else None
    for declaration, match in zip(declarations, matches):
        if not isinstance(declaration, dict) or not isinstance(match, dict):
            return False
        expected_path = declaration.get("expected_path")
        source = declaration.get("source")
        expected_sha = declaration.get("sha256")
        if not isinstance(expected_path, str) or not expected_path or source not in ("exe", "maps"):
            return False
        try:
            expected_resolved = str(Path(expected_path).expanduser().resolve(strict=True))
        except (OSError, ValueError):
            return False
        if expected_sha is not None and (not isinstance(expected_sha, str)
                or len(expected_sha) != 64
                or any(char not in "0123456789abcdef" for char in expected_sha)):
            return False
        binary = _runtime_binary_identity(binaries, expected_resolved)
        if binary is None or (expected_sha is not None and expected_sha != binary["sha256"]):
            return False
        effective_sha = binary["sha256"]
        expected = {
            "role": declaration.get("role"),
            "name": declaration.get("name"),
            "expected_path": expected_path,
            "expected_sha256": expected_sha,
            "source": source,
        }
        if any(match.get(key) != item for key, item in expected.items()
               if key != "expected_sha256"):
            return False
        match_sha = match.get("expected_sha256")
        if match_sha != effective_sha and not (expected_sha is None and match_sha is None):
            return False
        if not isinstance(match.get("observed"), list) or not match["observed"]: return False
        for observation_id in match["observed"]:
            if not isinstance(observation_id, str) or observation_id not in by_id: return False
            item = by_id[observation_id]
            if (not isinstance(item, dict) or item.get("run_id") != manifest.get("run_id")
                    or not isinstance(item.get("pid"), int) or isinstance(item.get("pid"), bool)
                    or not isinstance(item.get("start_time_ticks"), int) or isinstance(item.get("start_time_ticks"), bool)
                    or (item["pid"], item["start_time_ticks"]) not in sampled
                    or item.get("source") != source
                    or item.get("resolved_path") != expected_resolved
                    or item.get("sha256") != effective_sha
                    or item.get("size") != binary["size"]):
                return False
            for field in ("device", "inode"):
                if item.get(field) != binary[field]: return False
            identity_source = item.get("identity_source")
            if (source == "exe" and identity_source != "proc_exe") or (source == "maps" and identity_source not in ("proc_map_file", "proc_root_path")):
                return False
            if source == "maps":
                mapping = item.get("mapping")
                if (not isinstance(mapping, dict) or not isinstance(mapping.get("device"), str)
                        or not isinstance(mapping.get("permissions"), str)
                        or len(mapping["permissions"]) != 4
                        or mapping["permissions"][0] not in "r-"
                        or mapping["permissions"][1] not in "w-"
                        or mapping["permissions"][2] not in "x-"
                        or mapping["permissions"][3] not in "ps"
                        or "x" not in mapping["permissions"]
                        or not isinstance(mapping.get("inode"), int) or isinstance(mapping.get("inode"), bool)
                        or mapping.get("inode") != item.get("inode") or not isinstance(mapping.get("deleted"), bool)
                        or (mapping.get("deleted") and identity_source == "proc_root_path")):
                    return False
                try:
                    major, minor = (int(part, 16) for part in mapping["device"].split(":", 1))
                    if os.makedev(major, minor) != item["device"]: return False
                except (ValueError, OSError): return False
            if item.get("sha256") != effective_sha: return False
    return True

_REQUIRED_MANIFEST_FIELDS = ("schema_version", "tool_version", "run_id", "adapter", "mode", "run_status", "scenario_contract_hash", "metric_availability", "measurement_kind", "pid", "pid_start_time_ticks", "states", "sample_count", "sample_artifacts", "metrics_valid", "adapter_readiness")
_SAMPLE_ARTIFACT_PATHS = {
    "process": "raw/process.jsonl",
    "network": "raw/network.jsonl",
}


def _strict_jsonl_record_count(path: Path) -> int | None:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        return None
    for line in lines:
        if not line.strip():
            return None
        try:
            if not isinstance(strict_json_loads(line), dict):
                return None
        except (ValueError, json.JSONDecodeError):
            return None
    return len(lines)


def _sample_artifact_evidence(run_dir: Path) -> dict[str, Any]:
    evidence: dict[str, Any] = {}
    for name, relative_path in _SAMPLE_ARTIFACT_PATHS.items():
        artifact = run_dir/relative_path
        count = _strict_jsonl_record_count(artifact)
        if count is None:
            raise ValueError(f"invalid sample artifact: {relative_path}")
        evidence[name] = {
            "relative_path": relative_path,
            "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
            "record_count": count,
        }
    return evidence


def _valid_sample_artifact_evidence(value: Any, run_dir: Path,
                                    sample_count: Any) -> bool:
    if (not isinstance(value, dict) or set(value) != set(_SAMPLE_ARTIFACT_PATHS)
            or not isinstance(sample_count, int) or isinstance(sample_count, bool)
            or sample_count < 0):
        return False
    for name, relative_path in _SAMPLE_ARTIFACT_PATHS.items():
        recorded = value.get(name)
        if not isinstance(recorded, dict) or set(recorded) != {"relative_path", "sha256", "record_count"}:
            return False
        digest, count = recorded.get("sha256"), recorded.get("record_count")
        if (recorded.get("relative_path") != relative_path
                or not isinstance(digest, str) or len(digest) != 64
                or any(char not in "0123456789abcdef" for char in digest)
                or not isinstance(count, int) or isinstance(count, bool)
                or count != sample_count):
            return False
        artifact = run_dir/relative_path
        actual_count = _strict_jsonl_record_count(artifact)
        try:
            actual_digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        except OSError:
            return False
        if actual_count != count or actual_digest != digest:
            return False
    return True


def _valid_remote_not_collected(value: Any, manifest: dict[str, Any]) -> bool:
    expected = {
        "collection": "not_collected",
        "process": {"status": "not_collected"},
        "network": {"status": "not_collected"},
        "reason": "ohos_remote_capture_v2_did_not_collect_resource_samples",
    }
    return (manifest.get("artifact_kind") == "ohos_remote_p0a_candidate"
            and manifest.get("dds_comm_trace", {}).get("registration_contract_version") ==
                _P0A_REMOTE_CONTRACT
            and manifest.get("sample_count") == 0 and value == expected)


def _remote_manifest_declaration(manifest: dict[str, Any], run_dir: Path) -> bool:
    trace = manifest.get("dds_comm_trace")
    declared = (manifest.get("artifact_kind") == "ohos_remote_p0a_candidate"
                or isinstance(trace, dict)
                and trace.get("registration_contract_version") == _P0A_REMOTE_CONTRACT)
    try:
        evidence = _strict_json(_secure_read(
            run_dir, "capability/p0a_evidence_manifest.json")[0])
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError):
        return declared
    return declared or (isinstance(evidence, dict)
                        and evidence.get("registration_contract_version") ==
                        _P0A_REMOTE_CONTRACT)


def _manifest_schema_errors(value: dict[str, Any]) -> list[str]:
    def integer(name: str, minimum: int) -> bool:
        item = value.get(name)
        return isinstance(item, int) and not isinstance(item, bool) and item >= minimum

    def number(name: str) -> bool:
        item = value.get(name)
        return isinstance(item, (int, float)) and not isinstance(item, bool) and math.isfinite(item)

    remote = value.get("artifact_kind") == "ohos_remote_p0a_candidate"
    timing_valid = (value.get("remote_timing") == {
        "schema": "mw_perf.ohos_remote_timing.v1",
        "start_time": "unavailable", "end_time": "unavailable",
        "reason": "ohos_remote_capture_v2_did_not_export_wall_clock_boundaries",
    } and "start_time" not in value and "end_time" not in value) if remote else (
        number("start_time") and number("end_time") and "remote_timing" not in value)
    checks = {
        "schema_version": value.get("schema_version") == 1 and not isinstance(value.get("schema_version"), bool),
        "tool_version": isinstance(value.get("tool_version"), str),
        "run_id": isinstance(value.get("run_id"), str),
        "adapter": value.get("adapter") in ("generic", "ros2", "ibmw"),
        "mode": value.get("mode") in ("managed", "attached"),
        "run_status": value.get("run_status") in ("completed", "partial", "invalid", "failed", "cancelled", "cleanup_failed"),
        "scenario_contract_hash": isinstance(value.get("scenario_contract_hash"), str)
            and len(value["scenario_contract_hash"]) == 64
            and all(char in "0123456789abcdef" for char in value["scenario_contract_hash"]),
        "metric_availability": value.get("metric_availability") in ("supported", "unsupported", "estimated", "partial"),
        "measurement_kind": value.get("measurement_kind") in ("observation", "measurement", "derived", "metadata", "producer_latency", "latency_summary_csv", "stage_only", "exact_correlated", "observer_latency"),
        "timing": timing_valid,
        "pid": integer("pid", 1),
        "pid_start_time_ticks": integer("pid_start_time_ticks", 0),
        "states": isinstance(value.get("states"), list) and all(isinstance(item, str) for item in value.get("states", [])),
        "sample_count": integer("sample_count", 0),
        "sample_artifacts": isinstance(value.get("sample_artifacts"), dict),
        "metrics_valid": isinstance(value.get("metrics_valid"), bool),
        "adapter_readiness": isinstance(value.get("adapter_readiness"), bool),
    }
    return [name for name, valid in checks.items()
            if (name == "timing" or name in value) and not valid]

def _valid_jsonl(path: Path, kind: str) -> bool:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        return False
    if not lines:
        return False
    required_process_fields = {"pid", "cpu_seconds", "rss_bytes", "pss_bytes", "threads", "start_time_ticks"}
    required_network_fields = {"rx_bytes", "tx_bytes", "rx_packets", "tx_packets"}
    for line in lines:
        if not line.strip():
            return False
        try:
            record = strict_json_loads(line)
        except (ValueError, json.JSONDecodeError):
            return False
        if (not isinstance(record, dict) or not isinstance(record.get("timestamp"), (int, float))
                or isinstance(record.get("timestamp"), bool) or not math.isfinite(record["timestamp"])):
            return False
        if kind == "process":
            processes = record.get("processes")
            if not isinstance(processes, list) or not processes:
                return False
            if any(not isinstance(item, dict) or not required_process_fields.issubset(item) for item in processes):
                return False
            for item in processes:
                if not isinstance(item["pid"], int) or isinstance(item["pid"], bool) or item["pid"] <= 0:
                    return False
                if any(not isinstance(item[name], (int, float)) or isinstance(item[name], bool)
                       or not math.isfinite(item[name]) for name in ("cpu_seconds", "rss_bytes", "threads", "start_time_ticks")):
                    return False
                if item["pss_bytes"] is not None and (not isinstance(item["pss_bytes"], (int, float))
                        or isinstance(item["pss_bytes"], bool) or not math.isfinite(item["pss_bytes"])):
                    return False
        else:
            interfaces = record.get("interfaces")
            if not isinstance(interfaces, dict):
                return False
            for name, counters in interfaces.items():
                if not isinstance(name, str) or not name or not isinstance(counters, dict) or not required_network_fields.issubset(counters):
                    return False
                if any(not isinstance(counters[field], int) or isinstance(counters[field], bool) or counters[field] < 0 for field in required_network_fields):
                    return False
    return True

def validate_dir(path: Path, profile: str | None = None, config: dict[str, Any] | None = None) -> dict[str, Any]:
    if profile is None:
        profile = effective_validation_profile(config or {})
    if profile not in ("standard", "p0a"):
        raise ValueError("validation profile must be standard or p0a")
    try:
        m = strict_json_loads((path/"manifest.json").read_bytes())
        if not isinstance(m, dict): raise ValueError
    except FileNotFoundError:
        resources = _resource_integrity_errors()
        return {"validation_profile": profile, "valid_for_collection": False, "valid_for_metric": False, "valid_for_comparison": False, "rules": [{"name": "resource_integrity", "passed": not resources}], "failure_codes": resources + ["MANIFEST_MISSING"], "schema_audit": _schema_audit(validation_mode=profile)}
    except (OSError, ValueError, json.JSONDecodeError):
        resources = _resource_integrity_errors()
        return {"validation_profile": profile, "valid_for_collection": False, "valid_for_metric": False, "valid_for_comparison": False, "rules": [{"name": "resource_integrity", "passed": not resources}], "failure_codes": resources + ["MANIFEST_INVALID"], "schema_audit": _schema_audit(validation_mode=profile)}
    resource_errors = _resource_integrity_errors()
    remote_declared = _remote_manifest_declaration(m, path)
    required_fields = _REQUIRED_MANIFEST_FIELDS + (() if remote_declared else (
        "start_time", "end_time"))
    missing = [name for name in required_fields if name not in m]
    schema_errors = _manifest_schema_errors(m)
    remote_resource_claim = _valid_remote_not_collected(m.get("sample_artifacts"), m)
    sampled_artifacts_valid = (
        _valid_jsonl(path/"raw"/"process.jsonl", "process")
        and _valid_jsonl(path/"raw"/"network.jsonl", "network")
        and _valid_sample_artifact_evidence(m.get("sample_artifacts"), path,
                                            m.get("sample_count")))
    core_metrics = m.get("core_metrics", {})
    core_required = isinstance(core_metrics, dict) and core_metrics.get("required") is True
    provenance = m.get("provenance")
    provenance_required = isinstance(provenance, dict) and provenance.get("required") is True
    scenario = m.get("scenario_contract")
    scenario_ok = (not provenance_required or
        isinstance(scenario, dict) and bool(scenario) and _hash_contract({"scenario_contract": scenario}) == m.get("scenario_contract_hash"))
    provenance_ok = not provenance_required or _valid_ibmw_provenance(provenance)
    runtime_required = m.get("adapter") == "ibmw" and provenance_required
    runtime_ok = not runtime_required or _valid_runtime_images(m.get("runtime_images"), m, path)
    core_validation: dict[str, Any] | None = None
    core_ok = not core_required
    core_path = path/"raw"/"core_metrics.jsonl"
    if core_required or core_path.exists():
        from .adapters.ibmw import validate_core_metrics
        core_validation = validate_core_metrics(core_path)
    if core_required and core_validation is not None:
        manifest_count = core_metrics.get("record_count")
        manifest_kinds = core_metrics.get("kinds")
        try:
            archived_hash = hashlib.sha256(core_path.read_bytes()).hexdigest()
        except OSError:
            archived_hash = None
        core_ok = (core_metrics.get("status") == "supported" and core_validation["valid"]
                   and isinstance(manifest_count, int) and not isinstance(manifest_count, bool)
                   and manifest_count == core_validation["record_count"]
                   and isinstance(manifest_kinds, list) and manifest_kinds == core_validation["kinds"])
        content_sha256 = core_metrics.get("content_sha256")
        core_ok = (core_ok and isinstance(content_sha256, str)
                   and len(content_sha256) == 64
                   and content_sha256 == archived_hash)
        core_ok = core_ok and _valid_core_provenance(core_metrics.get("provenance"), m, archived_hash)
    latency = m.get("latency_artifact", {})
    latency_required = isinstance(latency, dict) and latency.get("required") is True
    latency_claimed = ((isinstance(latency, dict) and latency.get("status") == "supported")
                       or m.get("measurement_kind") == "producer_latency")
    latency_must_validate = latency_required or latency_claimed
    latency_validation: dict[str, Any] | None = None
    latency_ok = not latency_must_validate
    latency_path = path/"raw"/"latency.jsonl"
    if latency_must_validate or latency_path.exists():
        from .adapters.ros2 import validate_latency_artifact
        latency_validation = validate_latency_artifact(latency_path,
            expected_run_id=m.get("run_id"), expected_producer_pid=m.get("pid"),
            expected_producer_start_time_ticks=m.get("pid_start_time_ticks"))
    if latency_must_validate and latency_validation is not None and isinstance(latency, dict):
        try: latency_hash = hashlib.sha256(latency_path.read_bytes()).hexdigest()
        except OSError: latency_hash = None
        latency_ok = (latency.get("status") == "supported" and latency_validation["valid"]
            and isinstance(latency.get("record_count"), int) and not isinstance(latency.get("record_count"), bool)
            and latency.get("record_count") == latency_validation["record_count"]
            and isinstance(latency.get("topic"), str) and bool(latency.get("topic"))
            and latency.get("topic") == latency_validation["topic"]
            and isinstance(latency.get("type"), str) and bool(latency.get("type"))
            and latency.get("type") == latency_validation["type"]
            and isinstance(latency.get("content_sha256"), str)
            and latency.get("content_sha256") == latency_hash
            and latency.get("clock_domain") == latency_validation.get("clock_domain") == "producer_monotonic"
            and isinstance(latency.get("provenance"), dict)
            and latency["provenance"].get("clock_domain") == latency_validation.get("clock_domain")
            and not latency_validation.get("legacy", False)
            and _valid_latency_provenance(latency.get("provenance"), m, latency_hash))
    # Any archived ROS 2 latency artifact participates in the clock contract,
    # even when it is optional.  In particular, an optional archive whose
    # metadata was downgraded to ``invalid`` must not be allowed to disappear
    # behind the ordinary metadata-run path.  A missing optional artifact is
    # intentionally not covered by this gate.
    archived_latency = latency_path.exists() and isinstance(latency, dict)
    archived_latency_contract_blocked = (
        archived_latency
        and (latency.get("status") in ("partial", "legacy")
             or (m.get("adapter") == "ros2"
                 and (latency.get("status") != "supported"
                      or latency.get("clock_domain") != "producer_monotonic"
                      or latency_validation is None
                      or not latency_validation.get("valid")
                      or latency_validation.get("clock_domain") != "producer_monotonic"))))
    csv_latency = m.get("artifacts", {}).get("latency") if isinstance(m.get("artifacts"), dict) else None
    formal_comparison = any(name in m for name in ("comparison_lane", "comparison_block_id", "contract"))
    def declared(item: Any) -> bool:
        return isinstance(item, str) and bool(item) or isinstance(item, dict) and bool(item)
    block = m.get("comparison_block_id")
    sequence = m.get("sequence_index")
    comparison_metadata_ok = (not formal_comparison or
        m.get("comparison_schema") == "mw_perf.comparison-metadata.v1"
        and all(declared(m.get(name)) for name in ("contract", "profile", "workload", "resource_scope"))
        and m.get("order") in (["A", "B", "C"], ["A", "C", "B"], ["B", "A", "C"],
                               ["B", "C", "A"], ["C", "A", "B"], ["C", "B", "A"])
        and m.get("comparison_lane") in ("A", "B", "C")
        and (isinstance(block, str) and bool(block) or isinstance(block, int) and not isinstance(block, bool))
        and isinstance(sequence, int) and not isinstance(sequence, bool) and sequence >= 0)
    resources = m.get("resource_collection")
    comparison_resources_ok = (not formal_comparison or isinstance(resources, dict)
        and resources.get("include_descendants") is True and resources.get("network_interfaces") == ["lo"])
    csv_latency_ok = not formal_comparison
    if formal_comparison:
        from .adapters.csv_latency import revalidate
        csv_latency_ok = revalidate(csv_latency, path, m)
    managed_comparison_ok = not formal_comparison or m.get("mode") == "managed"
    p0a_path = path / "capability/p0a_evidence_manifest.json"
    p0a_required = profile == "p0a" or remote_declared
    p0a_marker_ok = not p0a_required or m.get("validation_profile") == "p0a"
    trace_controls = m.get("dds_comm_trace")
    exact_top_level = m.get("run_type") == "exact_run"
    expected_exact_metadata = None
    if exact_top_level:
        expected_exact_metadata = {
            "run_id": m.get("run_id"),
            **{field: trace_controls.get(field) if isinstance(trace_controls, dict) else None
               for field in ("registration_contract_version", "run_type", "observer",
                             "capability_id", "path_id", "deployment_mode")},
            "measurement_kind": m.get("measurement_kind"),
        }
    p0a_validation_kwargs = {
        "producer_pid": m.get("pid"),
        "producer_start_time_ticks": m.get("pid_start_time_ticks"),
        "topology_generation": (trace_controls.get("topology_generation")
                                if isinstance(trace_controls, dict) else None),
        "topology_sha256": (trace_controls.get("topology_sha256")
                            if isinstance(trace_controls, dict) else None),
        "expected_exact_metadata": expected_exact_metadata,
    }
    if exact_top_level:
        p0a_validation_kwargs["enumerate_after_binding_failure"] = True
    p0a_validation = (validate_p0a_evidence_manifest(path, **p0a_validation_kwargs)
                      if p0a_required else {"valid": True, "failure_codes": []})
    remote_invariants = (not remote_declared or (
        m.get("artifact_kind") == "ohos_remote_p0a_candidate"
        and isinstance(trace_controls, dict)
        and trace_controls.get("registration_contract_version") == _P0A_REMOTE_CONTRACT
        and m.get("candidate_only") is True
        and m.get("formal_p0a") is False
        and m.get("metric_availability") == "unsupported"
        and m.get("valid_for_metric") is False
        and m.get("valid_for_comparison") is False))
    remote_evidence_valid = (remote_declared and remote_invariants
                             and p0a_validation.get("valid") is True
                             and p0a_validation.get("failure_codes") == [])
    remote_resources = remote_resource_claim and remote_evidence_valid
    artifacts_valid = sampled_artifacts_valid or remote_resources
    trace_summary_ok = True
    if p0a_required and exact_top_level:
        fresh_trace = p0a_validation.get("dds_comm_trace")
        def nonnegative_trace_int(field: str) -> bool:
            value = trace_controls.get(field) if isinstance(trace_controls, dict) else None
            return isinstance(value, int) and not isinstance(value, bool) and value >= 0
        exact_summary_fields = (
            "required", "status", "runtime_changed", "business_unchanged", "measurement_kind",
            "record_count", "exact_record_count", "stream_count", "segment_count", "dropped", "invalid_identity",
            "missing_registration", "unsupported_async", "unsupported_multi_handler",
            "registration_contract_version", "run_type", "observer", "capability_id", "path_id",
            "deployment_mode", "topology_generation", "topology_sha256")
        trace_summary_ok = (
            isinstance(trace_controls, dict) and isinstance(fresh_trace, dict)
            and trace_controls.get("required") is True
            and trace_controls.get("status") == "exact_correlated"
            and trace_controls.get("measurement_kind") == "exact_correlated"
            and all(trace_controls.get(field) == fresh_trace.get(field) for field in exact_summary_fields)
            and all(nonnegative_trace_int(field)
                    for field in ("record_count", "exact_record_count", "stream_count", "segment_count"))
            and all(trace_controls.get(field) == 0 for field in (
                "dropped", "invalid_identity", "missing_registration", "unsupported_async",
                "unsupported_multi_handler")))
    rules = [{"name": "resource_integrity", "passed": not resource_errors}, {"name": "required_fields", "passed": not missing}, {"name": "manifest_schema", "passed": not schema_errors}, {"name": "sample_artifacts", "passed": artifacts_valid}, {"name": "remote_invariants", "passed": remote_invariants}, {"name": "candidate_only", "passed": m.get("candidate_only") is not True}, {"name": "run_completed", "passed": m.get("run_status") == "completed"}, {"name": "samples_present", "passed": remote_resources or isinstance(m.get("sample_count"), int) and not isinstance(m.get("sample_count"), bool) and m.get("sample_count", 0) > 0}, {"name": "metrics_valid", "passed": m.get("metrics_valid") is True}, {"name": "adapter_ready", "passed": m.get("adapter_readiness") is True}, {"name": "metric_available", "passed": m.get("metric_availability") in ("supported", "estimated")}, {"name": "scenario_contract", "passed": scenario_ok}, {"name": "ibmw_provenance", "passed": provenance_ok}, {"name": "runtime_images", "passed": runtime_ok}, {"name": "core_metrics_required", "passed": core_ok}, {"name": "latency_artifact_required", "passed": latency_ok}, {"name": "collector_managed_comparison", "passed": managed_comparison_ok}, {"name": "comparison_metadata", "passed": comparison_metadata_ok}, {"name": "comparison_resources", "passed": comparison_resources_ok}, {"name": "comparison_latency_csv", "passed": csv_latency_ok}, {"name": "p0a_profile_marker", "passed": p0a_marker_ok}, {"name": "p0a_evidence", "passed": p0a_validation["valid"]}, {"name": "dds_comm_trace_summary", "passed": trace_summary_ok}]
    collection = all(rule["passed"] for rule in rules[:4]) and (
        not remote_declared or remote_invariants and p0a_validation["valid"])
    passed = collection and all(r["passed"] for r in rules[4:]) and not archived_latency_contract_blocked
    failures = (resource_errors
                + [f"REQUIRED_FIELD_MISSING:{x}" for x in missing]
                + [f"MANIFEST_SCHEMA:{x}" for x in schema_errors]
                + [r["name"].upper() for r in rules[1:] if not r["passed"]])
    result = {"validation_profile": profile, "valid_for_collection": collection, "valid_for_metric": passed, "valid_for_comparison": passed,
              "rules": rules, "failure_codes": failures,
              "schema_audit": _schema_audit({
                  "validation": ("validation.schema.v1.json", "validation"),
                  "result": ("result.schema.v1.json", "manifest/result"),
              }, validation_mode=profile)}
    if archived_latency_contract_blocked:
        result["failure_codes"].append("LATENCY_ARTIFACT_DIAGNOSTIC_ONLY")
    if core_validation is not None:
        result["core_metrics"] = core_validation
    if latency_validation is not None:
        result["latency_artifact"] = latency_validation
    if csv_latency is not None:
        result.setdefault("artifacts", {})["latency"] = csv_latency
    if p0a_required:
        result["p0a_evidence"] = p0a_validation
    return result

def _valid_latency_provenance(value: Any, manifest: dict[str, Any], archived_hash: str | None) -> bool:
    if not isinstance(value, dict): return False
    def digest(item: Any) -> bool:
        return isinstance(item, str) and len(item) == 64 and all(c in "0123456789abcdef" for c in item)
    def metadata(item: Any, final: bool = False) -> bool:
        if not isinstance(item, dict) or set(item) != {"exists", "path", "mtime_ns", "size", "inode"}: return False
        exists = item.get("exists")
        if not isinstance(exists, bool) or final and not exists or item.get("path") != value.get("source_path"): return False
        values = [item.get(k) for k in ("mtime_ns", "size", "inode")]
        return (all(isinstance(x, int) and not isinstance(x, bool) and x >= 0 for x in values)
                if exists else all(x is None for x in values))
    return (value.get("run_id") == manifest.get("run_id") and value.get("adapter") == manifest.get("adapter") == "ros2"
        and value.get("clock_domain") == manifest.get("latency_artifact", {}).get("clock_domain") == "producer_monotonic"
        and value.get("mode") == manifest.get("mode") == "managed" and value.get("sut_pid") == manifest.get("pid")
        and value.get("sut_start_time_ticks") == manifest.get("pid_start_time_ticks")
        and isinstance(value.get("sut_pid"), int) and not isinstance(value.get("sut_pid"), bool)
        and isinstance(value.get("sut_start_time_ticks"), int) and not isinstance(value.get("sut_start_time_ticks"), bool)
        and value.get("source_path") == manifest.get("latency_artifact", {}).get("source_path")
        and isinstance(value.get("source_path"), str) and bool(value.get("source_path"))
        and value.get("graceful_finalization") is True and value.get("producer_completion_verified") is True
        and digest(value.get("source_sha256")) and digest(value.get("archive_sha256"))
        and value.get("source_sha256") == value.get("archive_sha256") == archived_hash
        and metadata(value.get("baseline_metadata")) and metadata(value.get("final_metadata"), True))

def _valid_core_provenance(value: Any, manifest: dict[str, Any], archived_hash: str | None) -> bool:
    if not isinstance(value, dict): return False
    def digest(item: Any) -> bool:
        return isinstance(item, str) and len(item) == 64 and all(c in "0123456789abcdef" for c in item)
    baseline, final = value.get("baseline_metadata"), value.get("final_metadata")
    def metadata(item: Any, *, final_snapshot: bool = False) -> bool:
        if not isinstance(item, dict) or set(item) != {"exists", "path", "mtime_ns", "size", "inode"}:
            return False
        exists = item.get("exists")
        if not isinstance(exists, bool) or final_snapshot and not exists:
            return False
        if item.get("path") != value.get("source_path") or not isinstance(item.get("path"), str):
            return False
        numeric = (item.get(key) for key in ("mtime_ns", "size", "inode"))
        return (all(isinstance(x, int) and not isinstance(x, bool) and x >= 0 for x in numeric)
                if exists else all(x is None for x in numeric))
    return (value.get("run_id") == manifest.get("run_id")
            and value.get("adapter") == manifest.get("adapter") == "ibmw"
            and value.get("mode") == manifest.get("mode") == "managed"
            and value.get("sut_pid") == manifest.get("pid")
            and value.get("sut_start_time_ticks") == manifest.get("pid_start_time_ticks")
            and isinstance(value.get("sut_pid"), int) and not isinstance(value.get("sut_pid"), bool)
            and isinstance(value.get("sut_start_time_ticks"), int) and not isinstance(value.get("sut_start_time_ticks"), bool)
            and value.get("root") == manifest.get("root") and isinstance(value.get("root"), str) and bool(value["root"])
            and value.get("build") == manifest.get("build") and isinstance(value.get("build"), dict)
            and value.get("source_path") == manifest.get("core_metrics", {}).get("source_path")
            and value.get("graceful_finalization") is True
            and value.get("final_snapshot") is True
            and digest(value.get("source_sha256")) and digest(value.get("archive_sha256"))
            and value.get("source_sha256") == value.get("archive_sha256") == archived_hash
            and metadata(baseline) and metadata(final, final_snapshot=True))


# P0a is a control-plane feature and trusts only the collector installation.
_P0A_STATUSES = {"required", "optional", "not_applicable"}
_P0A_MANIFEST_REF = "registration/contract.json"
_P0A_CONTRACT_FILES = {
    "mw_perf.p0a_contract.v1": "p0a-evidence-contract.v1.json",
    "mw_perf.p0a_contract.v2": "p0a-evidence-contract.v2.json",
    "mw_perf.p0a_contract.v3": "p0a-evidence-contract.v3.json",
    "mw_perf.p0a_contract.v4": "p0a-evidence-contract.v4.json",
    "mw_perf.p0a_contract.v5": "p0a-evidence-contract.v5.json",
    "mw_perf.p0a_contract.v6": "p0a-evidence-contract.v6.json",
    "mw_perf.p0a_contract.v7": "p0a-evidence-contract.v7.json",
    "mw_perf.p0a_contract.v8": "p0a-evidence-contract.v8.json",
    "mw_perf.p0a_contract.v9": "p0a-evidence-contract.v9.json",
    "mw_perf.p0a_ohos_remote_contract.v1": "p0a-ohos-remote-contract.v1.json",
}
_P0A_REMOTE_CONTRACT = "mw_perf.p0a_ohos_remote_contract.v1"
_P0A_EXACT_PIPELINE_CONTRACTS = {
    "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
    "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8", _P0A_REMOTE_CONTRACT,
}

def _strict_json(data: bytes) -> Any:
    return strict_json_loads(data)

def _schema_errors(value: Any, schema: dict[str, Any], root: dict[str, Any] | None = None,
                   location: str = "$") -> list[str]:
    if schema is True:
        return []
    if schema is False:
        return [f"{location}:falseSchema"]
    root = root or schema; errors: list[str] = []
    if "$ref" in schema:
        target: Any = root
        for part in schema["$ref"].removeprefix("#/").split("/"):
            target = target[part.replace("~1", "/").replace("~0", "~")]
        return _schema_errors(value, target, root, location)
    for item in schema.get("allOf", []): errors += _schema_errors(value, item, root, location)
    if "if" in schema and not _schema_errors(value, schema["if"], root, location):
        errors += _schema_errors(value, schema.get("then", {}), root, location)
    if "oneOf" in schema and sum(not _schema_errors(value, item, root, location) for item in schema["oneOf"]) != 1:
        errors.append(f"{location}:oneOf")
    if "anyOf" in schema and not any(not _schema_errors(value, item, root, location)
                                      for item in schema["anyOf"]):
        errors.append(f"{location}:anyOf")
    expected = schema.get("type")
    types = expected if isinstance(expected, list) else [expected] if expected else []
    checks = {"object": lambda x: isinstance(x, dict), "array": lambda x: isinstance(x, list),
              "string": lambda x: isinstance(x, str), "boolean": lambda x: isinstance(x, bool),
              "null": lambda x: x is None,
              "integer": lambda x: isinstance(x, int) and not isinstance(x, bool),
              "number": lambda x: isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)}
    if types and not any(checks.get(kind, lambda _x: False)(value) for kind in types):
        return errors + [f"{location}:type"]
    if "const" in schema and value != schema["const"]: errors.append(f"{location}:const")
    if "enum" in schema and value not in schema["enum"]: errors.append(f"{location}:enum")
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0): errors.append(f"{location}:minLength")
        if "pattern" in schema and re.search(schema["pattern"], value) is None: errors.append(f"{location}:pattern")
    if isinstance(value, (int, float)) and not isinstance(value, bool) and "minimum" in schema and value < schema["minimum"]:
        errors.append(f"{location}:minimum")
    if isinstance(value, (int, float)) and not isinstance(value, bool) and "maximum" in schema and value > schema["maximum"]:
        errors.append(f"{location}:maximum")
    if isinstance(value, dict):
        if len(value) < schema.get("minProperties", 0):
            errors.append(f"{location}:minProperties")
        properties = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in value: errors.append(f"{location}.{name}:required")
        for name, item in value.items():
            if name in properties: errors += _schema_errors(item, properties[name], root, f"{location}.{name}")
            elif isinstance(schema.get("additionalProperties"), dict):
                errors += _schema_errors(item, schema["additionalProperties"], root, f"{location}.{name}")
            elif schema.get("additionalProperties") is False: errors.append(f"{location}.{name}:additional")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0): errors.append(f"{location}:minItems")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append(f"{location}:maxItems")
        if schema.get("uniqueItems") and len({json.dumps(x, sort_keys=True, allow_nan=False) for x in value}) != len(value):
            errors.append(f"{location}:uniqueItems")
        prefix = schema.get("prefixItems", [])
        for index, item_schema in enumerate(prefix):
            if index < len(value):
                errors += _schema_errors(value[index], item_schema, root, f"{location}[{index}]")
        if "items" in schema:
            start = len(prefix)
            for index, item in enumerate(value[start:], start=start):
                errors += _schema_errors(item, schema["items"], root, f"{location}[{index}]")
    return errors

def _trusted_p0a_contract(version: Any) -> tuple[dict[str, Any], bytes]:
    if not isinstance(version, str) or version not in _P0A_CONTRACT_FILES: raise ValueError("unknown P0a contract version")
    raw = (Path(__file__).parent / "contracts" / _P0A_CONTRACT_FILES[version]).read_bytes()
    stored = _strict_json(raw)
    if not isinstance(stored, dict) or stored.get("registration_contract_version") != version:
        raise ValueError("invalid trusted P0a contract")
    contract = stored
    if version == _P0A_REMOTE_CONTRACT:
        pipeline, pipeline_raw = _trusted_p0a_contract("mw_perf.p0a_contract.v8")
        if (stored.get("shared_trace_pipeline") != "mw_perf.p0a_contract.v8"
                or stored.get("shared_trace_pipeline_sha256") !=
                    hashlib.sha256(pipeline_raw).hexdigest()):
            raise ValueError("remote P0a shared pipeline binding is invalid")
        contract = json.loads(json.dumps(pipeline))
        contract["registration_contract_version"] = version
        contract["contract_family"] = stored.get("contract_family")
        contract["shared_trace_pipeline"] = stored.get("shared_trace_pipeline")
        contract["shared_trace_pipeline_sha256"] = stored.get(
            "shared_trace_pipeline_sha256")
        contract["artifact_schemas"].update(stored.get("artifact_schemas", {}))
        overrides = stored.get("artifact_overrides")
        if not isinstance(overrides, list): raise ValueError("invalid remote P0a artifact overrides")
        by_path = {item.get("relative_path"): item for item in overrides if isinstance(item, dict)}
        contract["artifacts"] = [by_path.pop(item.get("relative_path"), item)
                                 for item in contract["artifacts"]]
        contract["artifacts"].extend(by_path.values())
    base_version = stored.get("base_contract_version")
    if base_version is not None:
        base, _ = _trusted_p0a_contract(base_version)
        contract = json.loads(json.dumps(base))
        contract["registration_contract_version"] = version
        contract["run_types"].update(stored.get("run_types", {}))
        contract["artifact_schemas"].update(stored.get("artifact_schemas", {}))
        overrides = stored.get("artifact_overrides")
        if not isinstance(overrides, list): raise ValueError("invalid P0a artifact overrides")
        by_path = {item.get("relative_path"): item for item in overrides if isinstance(item, dict)}
        contract["artifacts"] = [by_path.pop(item.get("relative_path"), item)
                                 for item in contract["artifacts"]]
        contract["artifacts"].extend(by_path.values())
    if version == "mw_perf.p0a_contract.v8":
        schema_raw = (Path(__file__).parent / "schemas/trace-quality.schema.v1.json").read_bytes()
        schema = _strict_json(schema_raw)
        if not isinstance(schema, dict) or schema.get("$id") != "mw_perf.trace_quality.v1":
            raise ValueError("invalid trusted trace-quality schema")
        contract["artifact_schemas"]["mw_perf.trace_quality.v1"] = schema
    if version == "mw_perf.p0a_contract.v9":
        schema_dir = Path(__file__).parent / "schemas"
        for filename, schema_id in (
            ("formal-anchor.schema.v1.json", "mw_perf.formal_anchor.v1"),
            ("trace-quality-formal.schema.v2.json", "mw_perf.trace_quality_formal.v2"),
            ("fault-injection-formal.schema.v2.json", "mw_perf.fault_injection_formal.v2"),
            ("rollback-formal.schema.v2.json", "mw_perf.rollback_formal.v2"),
        ):
            schema = _strict_json((schema_dir / filename).read_bytes())
            if not isinstance(schema, dict) or schema.get("$id") != schema_id:
                raise ValueError("invalid trusted formal P0a schema")
            contract["artifact_schemas"][schema_id] = schema
    if version == _P0A_REMOTE_CONTRACT:
        schema_dir = Path(__file__).parent / "schemas"
        for filename, schema_id in (
            ("ohos-remote-capture-envelope.schema.v1.json",
             "mw_perf.p0a.ohos_remote_capture_envelope.v1"),
            ("p0a-ohos-remote-resolution.schema.v1.json",
             "mw_perf.p0a.remote_resolution.v1"),
            ("p0a-ohos-remote-resolution-declarations.schema.v1.json",
             "mw_perf.p0a.remote_resolution_declarations.v1"),
            ("trace-quality-ohos-remote.schema.v1.json",
             "mw_perf.trace_quality_ohos_remote.v1"),
        ):
            schema = _strict_json((schema_dir / filename).read_bytes())
            if not isinstance(schema, dict) or schema.get("$id") != schema_id:
                raise ValueError("invalid trusted remote P0a schema")
            contract["artifact_schemas"][schema_id] = schema
    schemas, artifacts = contract.get("artifact_schemas"), contract.get("artifacts")
    if not isinstance(schemas, dict) or not schemas or not isinstance(artifacts, list) or not artifacts:
        raise ValueError("incomplete trusted P0a contract")
    return contract, raw

def _p0a_contract(config: Any) -> tuple[dict[str, Any] | None, bytes | None, str | None]:
    if not isinstance(config, dict): return None, None, None
    version = config.get("registration_contract_version", "mw_perf.p0a_contract.v1")
    try: contract, raw = _trusted_p0a_contract(version)
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError): return None, None, str(version)
    supplied = config.get("registration_contract")
    if supplied is not None:
        try:
            supplied_raw = Path(supplied).read_bytes() if isinstance(supplied, (str, Path)) else json.dumps(supplied, indent=2, ensure_ascii=False).encode() + b"\n"
            if supplied_raw != raw: return None, None, str(version)
        except (OSError, TypeError, ValueError): return None, None, str(version)
    return contract, raw, str(version)

def _p0a_declarations(contract: dict[str, Any], run_type: str, observer: bool,
                      measurement_kind: str, deployment_mode: str) -> list[dict[str, Any]]:
    run_rule = contract.get("run_types", {}).get(run_type)
    if not isinstance(run_rule, dict) or deployment_mode not in contract.get("deployment_modes", []): raise ValueError("invalid P0a metadata")
    if "observer" in run_rule and observer is not run_rule["observer"]: raise ValueError("invalid observer mode")
    if "measurement_kind" in run_rule and measurement_kind != run_rule["measurement_kind"]: raise ValueError("invalid measurement kind")
    universes = run_rule.get("universes")
    if not isinstance(universes, list): raise ValueError("invalid P0a universe")
    declarations = [dict(item) for item in contract["artifacts"] if item.get("universe") in universes]
    for item in declarations:
        if item.get("always_not_applicable"):
            item["status"] = "not_applicable"
            item["reason"] = item.get("not_applicable_reason")
        elif item.get("observer_conditional"):
            item["status"] = "required" if observer else "not_applicable"
            if not observer: item["reason"] = item.get("not_applicable_reason")
        else: item["status"] = "required"
    return declarations

def _canonical_relative(relative: Any, *, directory: bool = False) -> str:
    if not isinstance(relative, str) or not relative or "\\" in relative or relative.startswith("/"): raise ValueError("invalid P0a path")
    trailing = relative.endswith("/"); parts = relative.rstrip("/").split("/")
    if any(part in ("", ".", "..") for part in parts): raise ValueError("invalid P0a path")
    canonical = "/".join(parts) + ("/" if trailing else "")
    if canonical != relative or trailing != directory: raise ValueError("non-canonical P0a path")
    return canonical

def _secure_read(run_dir: Path, relative: str) -> tuple[bytes, tuple[int, int, int, int]]:
    _canonical_relative(relative)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    root_fd = os.open(run_dir, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_CLOEXEC", 0))
    parent_fd = root_fd
    try:
        parts = relative.split("/")
        for part in parts[:-1]:
            next_fd = os.open(part, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0), dir_fd=parent_fd)
            if parent_fd != root_fd: os.close(parent_fd)
            parent_fd = next_fd
        fd = os.open(parts[-1], flags, dir_fd=parent_fd)
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1: raise OSError("P0a artifact is not a private regular file")
            chunks: list[bytes] = []
            while chunk := os.read(fd, 1024 * 1024): chunks.append(chunk)
            after = os.fstat(fd)
            identity = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            if identity != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns): raise OSError("P0a artifact changed while reading")
            return b"".join(chunks), identity
        finally: os.close(fd)
    finally:
        if parent_fd != root_fd: os.close(parent_fd)
        os.close(root_fd)

def _secure_write_new(run_dir: Path, relative: str, data: bytes) -> None:
    _canonical_relative(relative)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    root_fd = os.open(run_dir, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_CLOEXEC", 0))
    parent_fd = root_fd
    try:
        parts = relative.split("/")
        for component in parts[:-1]:
            try: os.mkdir(component, 0o700, dir_fd=parent_fd)
            except FileExistsError: pass
            next_fd = os.open(component, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0), dir_fd=parent_fd)
            if parent_fd != root_fd: os.close(parent_fd)
            parent_fd = next_fd
        fd = os.open(parts[-1], flags, 0o600, dir_fd=parent_fd)
        try:
            view = memoryview(data)
            while view: view = view[os.write(fd, view):]
            os.fsync(fd)
        finally: os.close(fd)
        os.fsync(parent_fd)
    finally:
        if parent_fd != root_fd: os.close(parent_fd)
        os.close(root_fd)


def _publish_resolution(run_dir: Path, manifest: dict[str, Any],
                        proof: dict[str, Any], proof_bytes: bytes) -> None:
    relative = "capability/resolution.json"
    _secure_write_new(run_dir, relative, proof_bytes)
    manifest["p0a_resolution"] = {
        "collector_written": True,
        "relative_path": relative,
        "content_sha256": hashlib.sha256(proof_bytes).hexdigest(),
        "declaration_sha256": proof["declaration_sha256"],
    }


def _publish_identity_trace(run_dir: Path, manifest: dict[str, Any],
                            derivation: dict[str, Any]) -> None:
    relative = "normalized/identity_trace.jsonl"
    _secure_write_new(run_dir, relative, derivation["bytes"])
    manifest["p0a_identity_trace"] = {
        "collector_written": True, "relative_path": relative,
        "content_sha256": derivation["content_sha256"],
        "source_set_sha256": derivation["source_set_sha256"],
        "record_count": derivation["record_count"],
    }


def _publish_hook_callgraph(run_dir: Path, manifest: dict[str, Any],
                            derivation: dict[str, Any]) -> None:
    relative = "capability/hook_callgraph.json"
    _secure_write_new(run_dir, relative, derivation["bytes"])
    manifest["p0a_hook_callgraph"] = {
        "collector_written": True, "relative_path": relative,
        "content_sha256": derivation["content_sha256"],
        "boundary_catalog_sha256": derivation["boundary_catalog_sha256"],
        "delivery_set_sha256": derivation["delivery_set_sha256"],
        "record_count": derivation["record_count"],
    }


def _publish_normalized_result(run_dir: Path, manifest: dict[str, Any], *,
                               name: str, relative: str,
                               derivation: dict[str, Any]) -> None:
    _secure_write_new(run_dir, relative, derivation["bytes"])
    manifest[name] = {
        "collector_written": True, "relative_path": relative,
        "content_sha256": derivation["content_sha256"],
        "source_identity_sha256": derivation["source_identity_sha256"],
        "source_identity_record_count": derivation["source_identity_record_count"],
        "record_count": derivation["record_count"],
    }


def _publish_communication_latency(run_dir: Path, manifest: dict[str, Any],
                                   derivation: dict[str, Any]) -> None:
    relative = "capability/communication_latency.json"
    _secure_write_new(run_dir, relative, derivation["bytes"])
    manifest["p0a_communication_latency"] = {
        "collector_written": True, "relative_path": relative,
        "content_sha256": derivation["content_sha256"],
        "latency_samples_content_sha256": derivation["latency_samples_content_sha256"],
        "latency_samples_record_count": derivation["latency_samples_record_count"],
        "sample_set_sha256": derivation["sample_set_sha256"],
        "sample_set_count": derivation["sample_set_count"],
    }


def _publish_communication_delivery(run_dir: Path, manifest: dict[str, Any],
                                    derivation: dict[str, Any]) -> None:
    relative = "validation/communication_delivery.json"
    _secure_write_new(run_dir, relative, derivation["bytes"])
    manifest["p0a_communication_delivery"] = {
        "collector_written": True, "relative_path": relative,
        "content_sha256": derivation["content_sha256"],
        "expected_publish_count": derivation["expected_publish_count"],
        "expected_readers_per_sample": derivation["expected_readers_per_sample"],
        "observed_publish_count": derivation["observed_publish_count"],
        "observed_receive_count": derivation["observed_receive_count"],
        "observed_dispatch_count": derivation["observed_dispatch_count"],
        "observed_callback_count": derivation["observed_callback_count"],
        "sample_set_sha256": derivation["sample_set_sha256"],
        "delivery_set_sha256": derivation["delivery_set_sha256"],
        "record_count": derivation["record_count"],
    }


def _publish_trace_quality(run_dir: Path, manifest: dict[str, Any],
                           derivation: dict[str, Any]) -> None:
    relative = "validation/trace_quality.json"
    _secure_write_new(run_dir, relative, derivation["bytes"])
    manifest["p0a_trace_quality"] = {
        "collector_written": True, "relative_path": relative,
        "content_sha256": derivation["content_sha256"],
        "raw_source_set_sha256": derivation["raw_source_set_sha256"],
        "stage_record_count": derivation["stage_record_count"],
        "exact_record_count": derivation["exact_record_count"],
        "sample_set_sha256": derivation["sample_set_sha256"],
        "delivery_set_sha256": derivation["delivery_set_sha256"],
        "record_count": derivation["record_count"],
    }


def _secure_stat(run_dir: Path, relative: str) -> tuple[int, int, int, int]:
    _canonical_relative(relative)
    root_fd = os.open(run_dir, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_CLOEXEC", 0))
    parent_fd = root_fd
    try:
        parts = relative.split("/")
        for component in parts[:-1]:
            next_fd = os.open(component, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0), dir_fd=parent_fd)
            if parent_fd != root_fd: os.close(parent_fd)
            parent_fd = next_fd
        current = os.stat(parts[-1], dir_fd=parent_fd, follow_symlinks=False)
        if not stat.S_ISREG(current.st_mode): raise OSError("P0a path no longer names a regular file")
        return current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns
    finally:
        if parent_fd != root_fd: os.close(parent_fd)
        os.close(root_fd)

def _p0a_decode(data: bytes, declared: dict[str, Any]) -> tuple[int, list[Any]]:
    artifact_format, rule = declared.get("format"), declared.get("record_count_rule")
    if artifact_format == "jsonl" and rule == "physical_lines":
        if not data: return 0, []
        text = data.decode("utf-8")
        lines = text.splitlines()
        if any(not line.strip() for line in lines): raise ValueError("empty JSONL record")
        return len(lines), [_strict_json(line.encode()) for line in lines]
    if artifact_format == "json":
        value = _strict_json(data)
        if rule in ("document", "single"): return 1, [value]
        if rule == "array":
            if not isinstance(value, list): raise ValueError("JSON array required")
            return len(value), value
        if isinstance(rule, dict) and rule.get("type") == "field":
            target = value
            for component in str(rule.get("field", "")).split("."):
                if not isinstance(target, dict) or component not in target: raise ValueError("record field missing")
                target = target[component]
            if not isinstance(target, list): raise ValueError("record field is not an array")
            return len(target), [value]
    if artifact_format == "binary" and rule == "zero": return 0, []
    if artifact_format == "binary" and rule == "single_blob":
        if not data: raise ValueError("empty binary artifact")
        return 1, []
    if artifact_format == "binary" and isinstance(rule, dict) and rule.get("type") == "fixed_size":
        size = rule.get("record_size")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0 or len(data) % size: raise ValueError("invalid binary records")
        return len(data) // size, []
    if artifact_format == "binary" and rule == "header_plus_fixed_size":
        from .dds_trace import binary_record_count
        return binary_record_count(data), []
    raise ValueError("unsupported trusted count rule")

def _p0a_file_entry(run_dir: Path, relative: str, declared: dict[str, Any], status: str) -> dict[str, Any]:
    schema_version = declared.get("schema_version")
    if not isinstance(schema_version, str) or not schema_version: raise ValueError("schema_version required")
    try: data, _identity = _secure_read(run_dir, relative)
    except OSError:
        return {"relative_path": relative, "sha256": None, "record_count": 0, "schema_version": schema_version,
                "finalization": "incomplete", "status": status}
    count, _ = _p0a_decode(data, declared)
    return {"relative_path": relative, "sha256": hashlib.sha256(data).hexdigest(), "record_count": count,
            "schema_version": schema_version, "finalization": declared["finalization"], "status": status}

def _trace_files(run_dir: Path) -> list[str]:
    root = run_dir / "raw" / "comm_trace"
    if not root.is_dir() or root.is_symlink(): return []
    files: list[str] = []
    for child in root.rglob("*"):
        if child.is_symlink(): raise ValueError("comm_trace symlink")
        if child.is_file():
            rel = child.relative_to(run_dir).as_posix(); _canonical_relative(rel); files.append(rel)
    return sorted(files, key=lambda item: item.encode("utf-8"))


def _collector_trace_summary(trace_result: dict[str, Any]) -> dict[str, Any]:
    summary = {key: value for key, value in trace_result.items()
               if key not in ("events", "joined_records", "stream_id",
                              "raw_stage_events", "raw_callback_events")}
    if trace_result.get("status") == "exact_correlated":
        summary["exact_record_count"] = trace_result.get("record_count")
        summary["record_count"] = trace_result.get("stage_record_count")
    return summary

def _directory_file_declaration(directory: dict[str, Any], relative: str) -> dict[str, Any]:
    patterns = directory.get("patterns")
    if not isinstance(patterns, list):
        schema = directory.get("file_schema_version")
        if not isinstance(schema, str): raise ValueError("directory file schema is missing")
        return {**directory, "relative_path": relative, "schema_version": schema}
    name = relative.removeprefix(directory["relative_path"])
    matches = [item for item in patterns if isinstance(item, dict)
               and isinstance(item.get("pattern"), str) and fnmatch.fnmatchcase(name, item["pattern"])]
    if name.endswith(".exact.meta.json"):
        matches = [item for item in matches if item.get("pattern") == "*.exact.meta.json"]
    elif name.endswith(".exact.seal.json"):
        matches = [item for item in matches if item.get("pattern") == "*.exact.seal.json"]
    if len(matches) != 1 or "/" in name: raise ValueError("unknown or ambiguous directory artifact")
    return {**directory, **matches[0], "relative_path": relative,
            "finalization": directory["finalization"]}

def build_p0a_evidence_manifest(run_dir: Path, config: dict[str, Any], *, run_id: str,
                                capability_id: str, path_id: str, deployment_mode: str,
                                run_type: str, observer: bool, measurement_kind: str) -> dict[str, Any] | None:
    contract, raw, _ = _p0a_contract(config)
    if contract is None or raw is None: return None
    declarations = _p0a_declarations(contract, run_type, observer, measurement_kind, deployment_mode)
    _secure_write_new(run_dir, _P0A_MANIFEST_REF, raw)
    entries: list[dict[str, Any]] = []; directory_decl: dict[str, Any] | None = None
    for declared in declarations:
        relative = declared["relative_path"]
        if relative.endswith("/"):
            _canonical_relative(relative, directory=True); directory_decl = declared; continue
        _canonical_relative(relative)
        if declared["status"] == "not_applicable":
            entries.append({"relative_path": relative, "sha256": None, "record_count": 0,
                "schema_version": declared["schema_version"], "finalization": "not_applicable",
                "status": "not_applicable", "reason": declared["reason"]})
        else: entries.append(_p0a_file_entry(run_dir, relative, declared, "required"))
    if directory_decl is not None:
        declared_paths = {entry["relative_path"] for entry in entries}
        for relative in _trace_files(run_dir):
            if relative not in declared_paths:
                expanded = _directory_file_declaration(directory_decl, relative)
                entries.append(_p0a_file_entry(run_dir, relative, expanded, "required"))
    entries.sort(key=lambda item: item["relative_path"].encode("utf-8"))
    result = {"schema": "mw_perf.p0a_evidence_manifest.v1", "validation_profile": "p0a", "run_id": run_id,
        "capability_id": capability_id, "path_id": path_id, "deployment_mode": deployment_mode,
        "registration_contract_sha256": hashlib.sha256(raw).hexdigest(),
        "registration_contract_version": contract["registration_contract_version"],
        "registration_contract_ref": _P0A_MANIFEST_REF, "run_type": run_type, "observer": observer,
        "measurement_kind": measurement_kind, "entries": entries}
    if contract["registration_contract_version"] in ("mw_perf.p0a_contract.v2", "mw_perf.p0a_contract.v3", "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8", "mw_perf.p0a_contract.v9", _P0A_REMOTE_CONTRACT) and directory_decl is not None:
        result["runtime_changed"] = True
        result["business_unchanged"] = True
    result["required"] = [item for item in entries if item["status"] == "required"]
    result["conditional"] = [item for item in entries if item["status"] != "required"]
    return result

def _p0a_schema_valid(value: Any) -> bool:
    try:
        schema = _strict_json((Path(__file__).parent / "schemas/p0a-evidence-manifest.schema.v1.json").read_bytes())
        return isinstance(schema, dict) and not _schema_errors(value, schema)
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError, KeyError): return False


def _valid_resolution_v2(run_dir: Path, p0a_manifest: dict[str, Any],
                         entries: list[Any]) -> bool:
    relative = "capability/resolution.json"
    try:
        root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
        data = _secure_read(run_dir, relative)[0]
        resolution = _strict_json(data)
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError):
        return False
    if not isinstance(root, dict) or not isinstance(resolution, dict): return False
    publication = root.get("p0a_resolution")
    digest = hashlib.sha256(data).hexdigest()
    entry = next((item for item in entries if isinstance(item, dict)
                  and item.get("relative_path") == relative), None)
    if (root.get("mode") != "managed" or root.get("run_type") != "exact_run"
            or root.get("run_status") != "completed" or "cleanup_error" in root
            or root.get("stop_escalated_sigkill") is True
            or not isinstance(publication, dict)
            or set(publication) != {"collector_written", "relative_path", "content_sha256",
                                    "declaration_sha256"}
            or publication.get("collector_written") is not True
            or publication.get("relative_path") != relative
            or publication.get("content_sha256") != digest
            or publication.get("declaration_sha256") != resolution.get("declaration_sha256")
            or not isinstance(entry, dict) or entry.get("sha256") != digest):
        return False
    identity_fields = ("run_id", "capability_id", "path_id", "deployment_mode")
    if (any(resolution.get(field) != p0a_manifest.get(field) for field in identity_fields)
            or p0a_manifest.get("registration_contract_version") not in (
                "mw_perf.p0a_contract.v3", "mw_perf.p0a_contract.v4",
                "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
                "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")
            or p0a_manifest.get("run_type") != "exact_run"
            or resolution.get("schema") != "mw_perf.p0a.resolution.v2"
            or resolution.get("producer") != "mw_perf_collect"
            or resolution.get("finalized") is not True or resolution.get("resolved") is not True
            or resolution.get("conclusion") != "pass"):
        return False
    declarations, observations, matches = (resolution.get("declarations"),
        resolution.get("observations"), resolution.get("matches"))
    if (not isinstance(declarations, list) or not declarations
            or not isinstance(observations, list) or not observations
            or not isinstance(matches, list)
            or resolution.get("declaration_sha256") != hashlib.sha256(
                _canonical_json_bytes(declarations)).hexdigest()):
        return False
    if any(not isinstance(item, dict) for item in declarations): return False
    declaration_identities: set[tuple[Any, Any]] = set()
    declaration_targets: set[tuple[Any, Any]] = set()
    for item in declarations:
        identity = (item.get("role"), item.get("name"))
        target = (item.get("source"), item.get("expected_path"))
        expected_path = item.get("expected_path")
        if (set(item) != {"role", "name", "source", "expected_path", "sha256",
                          "expected_device", "expected_inode", "expected_size"}
                or not all(isinstance(item.get(field), str) and item[field]
                           for field in ("role", "name"))
                or item.get("source") not in ("exe", "maps")
                or identity in declaration_identities or target in declaration_targets
                or not isinstance(expected_path, str) or not expected_path.startswith("/")
                or os.path.normpath(expected_path) != expected_path
                or re.fullmatch(r"[0-9a-f]{64}", str(item.get("sha256", ""))) is None
                or any(not isinstance(item.get(field), int) or isinstance(item.get(field), bool)
                       or item[field] < 0
                       for field in ("expected_device", "expected_inode", "expected_size"))):
            return False
        declaration_identities.add(identity); declaration_targets.add(target)
    declaration_order = sorted(declarations, key=lambda item: tuple(
        str(item.get(field, "")).encode("utf-8")
        for field in ("role", "name", "source", "expected_path", "sha256")))
    if declarations != declaration_order: return False
    sampled = _sampled_process_identities(run_dir)
    root_identity = (root.get("pid"), root.get("pid_start_time_ticks"))
    if sampled is None or root_identity not in sampled: return False
    by_id: dict[str, dict[str, Any]] = {}
    for item in observations:
        if (not isinstance(item, dict) or item.get("run_id") != p0a_manifest.get("run_id")
                or (item.get("pid"), item.get("start_time_ticks")) != root_identity
                or not isinstance(item.get("observation_id"), str)
                or item["observation_id"] != _observation_id(item)
                or item["observation_id"] in by_id
                or item.get("source") not in ("exe", "maps")
                or not isinstance(item.get("resolved_path"), str)
                or not item["resolved_path"].startswith("/")
                or any(not isinstance(item.get(field), int) or isinstance(item.get(field), bool)
                       or item[field] < 0 for field in ("device", "inode", "size"))
                or not isinstance(item.get("sha256"), str)
                or re.fullmatch(r"[0-9a-f]{64}", item["sha256"]) is None):
            return False
        if item["source"] == "exe":
            if item.get("identity_source") != "proc_exe" or "mapping" in item: return False
        else:
            mapping = item.get("mapping")
            if (item.get("identity_source") not in ("proc_map_file", "proc_root_path")
                    or not isinstance(mapping, dict) or set(mapping) != {
                        "start_address", "end_address", "permissions", "device", "inode", "deleted"}
                    or re.fullmatch(r"[r-][w-]x[ps]", str(mapping.get("permissions", ""))) is None
                    or mapping.get("inode") != item.get("inode")
                    or not isinstance(mapping.get("deleted"), bool)
                    or mapping.get("deleted") and item.get("identity_source") == "proc_root_path"):
                return False
            try:
                major, minor = (int(part, 16) for part in mapping["device"].split(":", 1))
                if os.makedev(major, minor) != item["device"]: return False
            except (AttributeError, ValueError, OSError): return False
        by_id[item["observation_id"]] = item
    if observations != sorted(observations, key=lambda item: item["observation_id"]): return False
    expected_matches = _resolution_matches(declarations, observations)
    if matches != expected_matches or any(not item["observed"] for item in expected_matches):
        return False
    return {identifier for item in matches for identifier in item["observed"]} == set(by_id)


def _remote_entry_digest(entries: list[Any], relative: str, data: bytes) -> bool:
    entry = next((item for item in entries if isinstance(item, dict)
                  and item.get("relative_path") == relative), None)
    return isinstance(entry, dict) and entry.get("sha256") == hashlib.sha256(data).hexdigest()


def _valid_remote_capture(run_dir: Path, p0a_manifest: dict[str, Any],
                          entries: list[Any], trusted_raw: bytes) -> bool:
    """Revalidate retained v2 wire bytes and their remote-only resolution proof."""
    try:
        from . import ohos_capture, ohos_registration
        root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
        request_raw = _secure_read(run_dir, "raw/remote_capture/request.json")[0]
        result_raw = _secure_read(run_dir, "raw/remote_capture/result.json")[0]
        archive_raw = _secure_read(run_dir, "raw/remote_capture/archive.tar")[0]
        envelope_raw = _secure_read(run_dir, "validation/remote_capture_envelope.json")[0]
        declarations_raw = _secure_read(run_dir, "registration/resolution_declarations.json")[0]
        registration_raw = _secure_read(run_dir, "registration/ohos_capture_registration.json")[0]
        approval_raw = _secure_read(run_dir, "registration/operator_approval.json")[0]
        resolution_raw = _secure_read(run_dir, "capability/resolution.json")[0]
        request, result = _strict_json(request_raw), _strict_json(result_raw)
        envelope, declarations_doc = _strict_json(envelope_raw), _strict_json(declarations_raw)
        resolution = _strict_json(resolution_raw)
        registration = ohos_capture._strict_json(
            registration_raw, "registration/ohos_capture_registration.json")
        approval = ohos_capture._strict_json(
            approval_raw, "registration/operator_approval.json")
        registration_digest = hashlib.sha256(registration_raw).hexdigest()
        approval_digest = hashlib.sha256(approval_raw).hexdigest()
        ohos_registration.validate_registration(
            registration, raw=registration_raw, expected_sha256=registration_digest,
            approval=approval, approval_raw=approval_raw,
            expected_approval_sha256=approval_digest)
        if not all(isinstance(item, dict) for item in (
                root, request, result, envelope, declarations_doc, resolution)):
            return False
        for relative, data in (
            ("raw/remote_capture/request.json", request_raw),
            ("raw/remote_capture/result.json", result_raw),
            ("raw/remote_capture/archive.tar", archive_raw),
            ("validation/remote_capture_envelope.json", envelope_raw),
            ("registration/resolution_declarations.json", declarations_raw),
            ("registration/ohos_capture_registration.json", registration_raw),
            ("registration/operator_approval.json", approval_raw),
            ("capability/resolution.json", resolution_raw),
        ):
            if not _remote_entry_digest(entries, relative, data): return False
        imported = ohos_capture.validate_capture_bytes(
            archive_raw, f"capture-{request['request_challenge']}.tar", result, request)
        if imported.get("semantic_validation") is not True: return False
        archive_entries = ohos_capture._parse_ustar(archive_raw)
        archive_manifest_raw = archive_entries[0][1]
        archive_manifest = ohos_capture._strict_json(
            archive_manifest_raw, "device_capture_manifest.json")
        members = dict(archive_entries[1:])
        attestation_raw = members[ohos_capture._V2_ATTESTATION]
        completion_raw = members[ohos_capture._V2_COMPLETION]
        attestation = ohos_capture._strict_json(attestation_raw, ohos_capture._V2_ATTESTATION)
        completion = ohos_capture._strict_json(completion_raw, ohos_capture._V2_COMPLETION)
        contract_hash = hashlib.sha256(trusted_raw).hexdigest()
        binding = {"id": registration["registration_id"], "sha256": registration_digest}
        if (request.get("trust_registration") != binding
                or result.get("trust_registration") != binding
                or archive_manifest.get("trust_registration") != binding
                or request.get("campaign_id") != registration["campaign_id"]
                or request.get("capability_id") != registration["capability_id"]
                or request.get("runtime_images") != registration["runtime_images"]
                or archive_manifest.get("agent_executable_sha256")
                   != registration["capture_agent"]["sha256"]
                or archive_manifest.get("boot_id") != registration["exact_boot_id"]
                or root.get("formal_p0a") is not False):
            return False
        expected_raw = {
            "request": ("raw/remote_capture/request.json", request_raw),
            "result": ("raw/remote_capture/result.json", result_raw),
            "archive": ("raw/remote_capture/archive.tar", archive_raw),
        }
        if (envelope.get("schema") != "mw_perf.p0a.ohos_remote_capture_envelope.v1"
                or envelope.get("finalized") is not True
                or envelope.get("trust_classification")
                   != ohos_registration.TRUST_CLASSIFICATION
                or envelope.get("registration_contract") != {
                    "version": _P0A_REMOTE_CONTRACT, "sha256": contract_hash}
                or envelope.get("trust_registration") != {
                    "relative_path": "registration/ohos_capture_registration.json",
                    "id": registration["registration_id"], "size": len(registration_raw),
                    "sha256": registration_digest}
                or envelope.get("operator_approval") != {
                    "relative_path": "registration/operator_approval.json",
                    "schema": ohos_registration.APPROVAL_SCHEMA,
                    "size": len(approval_raw), "sha256": approval_digest}
                or any(envelope.get(field) != p0a_manifest.get(field)
                       for field in ("run_id", "capability_id", "path_id", "deployment_mode"))):
            return False
        for key, (relative, raw) in expected_raw.items():
            if envelope.get(key) != {"relative_path": relative, "size": len(raw),
                                     "sha256": hashlib.sha256(raw).hexdigest()}:
                return False
        for key, name, raw in (
            ("device_capture_manifest", "device_capture_manifest.json", archive_manifest_raw),
            ("runtime_attestation", ohos_capture._V2_ATTESTATION, attestation_raw),
            ("device_completion", ohos_capture._V2_COMPLETION, completion_raw),
        ):
            if envelope.get(key) != {"archive_member": name, "size": len(raw),
                                     "sha256": hashlib.sha256(raw).hexdigest()}:
                return False
        declarations = [{"role": item["role"], "name": item["name"],
                         "source": item["source"], "path": item["path"],
                         "sha256": item["sha256"]} for item in request["runtime_images"]]
        declaration_hash = hashlib.sha256(_canonical_json_bytes(declarations)).hexdigest()
        if declarations_doc != {"schema": "mw_perf.p0a.remote_resolution_declarations.v1",
                "registration_contract_version": _P0A_REMOTE_CONTRACT,
                "run_id": request["run_id"], "capability_id": request["capability_id"],
                "path_id": request["path_id"], "deployment_mode": "managed",
                "declaration_sha256": declaration_hash,
                "declarations": declarations,
                "trust_classification": ohos_registration.TRUST_CLASSIFICATION}:
            return False
        def observations(images: list[dict[str, Any]], phase: str) -> list[dict[str, Any]]:
            values = []
            for item in images:
                value = {"phase": phase, "role": item["role"], "name": item["name"],
                    "source": item["source"], "path": item["expected_path"],
                    "sha256": item["observed_sha256"],
                    "device_major": item["observed_device_major"],
                    "device_minor": item["observed_device_minor"],
                    "inode": item["observed_inode"]}
                value["observation_id"] = _observation_id(value)
                values.append(value)
            return values
        ready = observations(attestation["live_images"], "ready")
        completed = observations(completion["completion_images"], "completion")
        ready_by_key = {(item["role"], item["name"]): item for item in ready}
        completed_by_key = {(item["role"], item["name"]): item for item in completed}
        matches = [{**item,
            "ready_observation": ready_by_key[(item["role"], item["name"])]["observation_id"],
            "completion_observation": completed_by_key[(item["role"], item["name"])]["observation_id"]}
            for item in declarations]
        expected_resolution = {"schema": "mw_perf.p0a.remote_resolution.v1",
            "run_id": request["run_id"], "capability_id": request["capability_id"],
            "path_id": request["path_id"], "deployment_mode": "managed",
            "producer": "ohos_remote_capture_v2", "finalized": True,
            "declaration_sha256": declaration_hash, "declarations": declarations,
            "ready_observations": ready, "completion_observations": completed,
            "matches": matches, "resolved": True, "conclusion": "pass"}
        publication = root.get("p0a_resolution")
        return (resolution == expected_resolution
            and publication == {"collector_written": True,
                "relative_path": "capability/resolution.json",
                "content_sha256": hashlib.sha256(resolution_raw).hexdigest(),
                "declaration_sha256": declaration_hash})
    except (KeyError, OSError, TypeError, ValueError, UnicodeError,
            json.JSONDecodeError):
        return False

def validate_p0a_evidence_manifest(run_dir: Path, manifest: dict[str, Any] | None = None,
                                   contract: dict[str, Any] | None = None, *,
                                   producer_pid: int | None = None,
                                   producer_start_time_ticks: int | None = None,
                                   topology_generation: str | None = None,
                                   topology_sha256: str | None = None,
                                   expected_exact_metadata: dict[str, Any] | None = None,
                                   enumerate_after_binding_failure: bool = False) -> dict[str, Any]:
    errors: list[str] = []
    try:
        m = manifest if manifest is not None else _strict_json(_secure_read(run_dir, "capability/p0a_evidence_manifest.json")[0])
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError):
        return {"valid": False, "failure_codes": ["P0A_MANIFEST_MISSING"]}
    if not _p0a_schema_valid(m): errors.append("P0A_SCHEMA")
    if not isinstance(m, dict): return {"valid": False, "failure_codes": errors}
    if expected_exact_metadata is not None:
        binding_fields = ("run_id", "registration_contract_version", "run_type", "observer",
                          "capability_id", "path_id", "deployment_mode", "measurement_kind")
        binding_valid = (expected_exact_metadata.get("run_type") == "exact_run"
            and expected_exact_metadata.get("registration_contract_version") in (
                "mw_perf.p0a_contract.v2", "mw_perf.p0a_contract.v3",
                "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5",
                "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
                "mw_perf.p0a_contract.v8", _P0A_REMOTE_CONTRACT)
            and expected_exact_metadata.get("measurement_kind") == "exact_correlated"
            and all(m.get(field) == expected_exact_metadata.get(field) for field in binding_fields))
        if not binding_valid:
            errors.append("P0A_TOP_LEVEL_BINDING")
            if not enumerate_after_binding_failure:
                return {"valid": False, "failure_codes": sorted(set(errors))}
    try: trusted, trusted_raw = _trusted_p0a_contract(m.get("registration_contract_version"))
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError):
        return {"valid": False, "failure_codes": sorted(set(errors + ["P0A_CONTRACT_VERSION"]))}
    try: archived_raw, archived_identity = _secure_read(run_dir, _P0A_MANIFEST_REF)
    except OSError:
        archived_raw, archived_identity = None, None; errors.append("P0A_CONTRACT_HASH")
    if archived_raw != trusted_raw or hashlib.sha256(trusted_raw).hexdigest() != m.get("registration_contract_sha256"):
        errors.append("P0A_CONTRACT_HASH")
    if contract is not None and contract != trusted: errors.append("P0A_CONTRACT_HASH")
    try:
        run_type, observer = m.get("run_type"), m.get("observer")
        measurement_kind, deployment_mode = m.get("measurement_kind"), m.get("deployment_mode")
        if not isinstance(run_type, str) or not isinstance(observer, bool) or not isinstance(measurement_kind, str) or not isinstance(deployment_mode, str):
            raise ValueError("invalid P0a metadata types")
        declarations = _p0a_declarations(trusted, run_type, observer, measurement_kind, deployment_mode)
    except ValueError:
        declarations = []; errors.append("P0A_CONTRACT_METADATA")
    expected_map: dict[str, dict[str, Any]] = {}; directory_decl = None
    for item in declarations:
        relative = item["relative_path"]
        if relative.endswith("/"): directory_decl = item
        else: expected_map[relative] = item
    try: trace_files = _trace_files(run_dir) if directory_decl is not None else []
    except ValueError: trace_files = []; errors.append("P0A_PATH")
    if directory_decl is not None:
        for relative in trace_files:
            if relative not in expected_map:
                try: expected_map[relative] = _directory_file_declaration(directory_decl, relative)
                except ValueError: errors.append("P0A_PATH")
        if "raw/comm_trace/directory_finalization.json" not in expected_map: errors.append("P0A_DIRECTORY_FINALIZATION")
    entries = m.get("entries")
    entry_list = entries if isinstance(entries, list) else []
    paths = [item.get("relative_path") for item in entry_list if isinstance(item, dict)]
    if len(paths) != len(entry_list) or len(set(paths)) != len(paths) or set(paths) != set(expected_map): errors.append("P0A_ENTRY_SET")
    if paths != sorted(paths, key=lambda item: item.encode("utf-8") if isinstance(item, str) else b""): errors.append("P0A_ENTRY_ORDER")
    required = [item for item in entry_list if isinstance(item, dict) and item.get("status") == "required"]
    conditional = [item for item in entry_list if isinstance(item, dict) and item.get("status") != "required"]
    if m.get("required") != required or m.get("conditional") != conditional: errors.append("P0A_ENTRY_GROUP")
    schemas = trusted["artifact_schemas"]; identities: set[tuple[int, int]] = set(); snapshots: dict[str, tuple[int, int, int, int]] = {}
    if archived_identity is not None: snapshots[_P0A_MANIFEST_REF] = archived_identity
    decoded: dict[str, list[Any]] = {}
    trace_decode_failed = False
    for entry in entry_list:
        if not isinstance(entry, dict): continue
        relative, status = entry.get("relative_path"), entry.get("status")
        declared = expected_map.get(relative) if isinstance(relative, str) else None
        if declared is None: continue
        expected_status = declared["status"]
        if status != expected_status: errors.append("P0A_STATUS")
        if entry.get("schema_version") != declared.get("schema_version"): errors.append("P0A_SCHEMA_VERSION")
        if status == "not_applicable":
            if (entry.get("sha256") is not None or entry.get("record_count") != 0
                    or entry.get("finalization") != "not_applicable" or entry.get("reason") != declared.get("reason")):
                errors.append("P0A_NOT_APPLICABLE")
            continue
        try:
            if not isinstance(relative, str): raise ValueError("invalid artifact path")
            data, identity = _secure_read(run_dir, relative)
            file_identity = identity[:2]
            if file_identity in identities: raise ValueError("duplicate file identity")
            identities.add(file_identity); snapshots[relative] = identity
            count, records = _p0a_decode(data, declared)
            minimum = declared.get("min_record_count")
            if not isinstance(minimum, int) or isinstance(minimum, bool) or minimum < 0: raise ValueError("invalid minimum")
            if entry.get("sha256") != hashlib.sha256(data).hexdigest() or entry.get("record_count") != count or count < minimum:
                raise ValueError("artifact digest/count")
            if entry.get("finalization") != declared.get("finalization"): raise ValueError("finalization")
            schema = schemas.get(declared.get("schema_version"))
            if records and (not isinstance(schema, dict) or not schema
                            or any(_schema_errors(record, schema) for record in records)):
                raise ValueError("artifact schema")
            for record in records:
                if not isinstance(record, dict) or any(record.get(field) != m.get(field) for field in trusted["identity_fields"]):
                    raise ValueError("artifact identity")
            decoded[relative] = records
        except (OSError, ValueError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_ARTIFACT")
            if isinstance(relative, str) and relative.startswith("raw/comm_trace/"):
                trace_decode_failed = True
    finalization_path = "raw/comm_trace/directory_finalization.json"
    if directory_decl is not None and trace_decode_failed:
        errors.append("P0A_DDS_COMM_TRACE")
    if finalization_path in decoded:
        record = decoded[finalization_path][0] if decoded[finalization_path] else None
        actual = [item for item in trace_files if item != finalization_path]
        if not isinstance(record, dict) or record.get("files") != actual or not actual:
            errors.append("P0A_DIRECTORY_FINALIZATION")
    trace_summary: dict[str, Any] | None = None
    if (directory_decl is not None and directory_decl.get("format") == "pattern_directory"
            and not trace_decode_failed):
        try:
            from .dds_trace import validate_directory_files
            if (trusted.get("registration_contract_version") in (
                    "mw_perf.p0a_contract.v2", "mw_perf.p0a_contract.v3",
                    "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5",
                    "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
                    "mw_perf.p0a_contract.v8", _P0A_REMOTE_CONTRACT)
                    and m.get("run_type") == "exact_run"):
                valid_exact_controls = (
                    isinstance(producer_pid, int) and not isinstance(producer_pid, bool)
                    and producer_pid > 0
                    and isinstance(producer_start_time_ticks, int)
                    and not isinstance(producer_start_time_ticks, bool)
                    and producer_start_time_ticks >= 0
                    and isinstance(topology_generation, str) and bool(topology_generation)
                    and isinstance(topology_sha256, str)
                    and re.fullmatch(r"[0-9a-f]{64}", topology_sha256) is not None)
                if not valid_exact_controls:
                    raise ValueError("exact collector controls are missing or malformed")
            producer_files = {relative.removeprefix("raw/comm_trace/"): _secure_read(run_dir, relative)[0]
                              for relative in trace_files if relative != finalization_path}
            trace_result = validate_directory_files(producer_files, schemas, _schema_errors, {
                field: str(m.get(field)) for field in trusted["identity_fields"]},
                run_type=str(m.get("run_type")), measurement_kind=str(m.get("measurement_kind")),
                producer_pid=producer_pid,
                producer_start_time_ticks=producer_start_time_ticks,
                topology_generation=topology_generation,
                topology_sha256=topology_sha256,
                registration_contract_version=trusted.get("registration_contract_version"))
            trace_summary = {"required": True, **_collector_trace_summary(trace_result)}
            if expected_exact_metadata is not None:
                trace_summary.update(expected_exact_metadata)
                trace_summary.update({"topology_generation": topology_generation,
                                      "topology_sha256": topology_sha256})
        except (OSError, ValueError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_DDS_COMM_TRACE")
    proof_paths = ["capability/resolution.json", "capability/hook_callgraph.json",
                   "capability/communication_latency.json",
                   "validation/communication_delivery.json", "validation/trace_quality.json",
                   "normalized/identity_trace.jsonl",
                   "validation/fault_injection.json", "validation/rollback.json"]
    conclusions = {record.get("conclusion") for path in proof_paths for record in decoded.get(path, []) if isinstance(record, dict)}
    if any(path in expected_map for path in proof_paths) and conclusions != {"pass"}: errors.append("P0A_CROSS_ARTIFACT")
    if (trusted.get("registration_contract_version") in (
            "mw_perf.p0a_contract.v3", "mw_perf.p0a_contract.v4",
            "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
            "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")
            and not _valid_resolution_v2(run_dir, m, entry_list)):
        errors.append("P0A_RESOLUTION")
    if (trusted.get("registration_contract_version") == _P0A_REMOTE_CONTRACT
            and not _valid_remote_capture(run_dir, m, entry_list, trusted_raw)):
        errors.append("P0A_REMOTE_CAPTURE")
    offline_identity_derivation: dict[str, Any] | None = None
    if trusted.get("registration_contract_version") in (
            "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5",
            "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
            "mw_perf.p0a_contract.v8", _P0A_REMOTE_CONTRACT):
        try:
            from .dds_trace import derive_identity_trace
            if (not isinstance(producer_pid, int) or isinstance(producer_pid, bool)
                    or not isinstance(producer_start_time_ticks, int)
                    or isinstance(producer_start_time_ticks, bool)
                    or not isinstance(topology_generation, str)
                    or not isinstance(topology_sha256, str)):
                raise ValueError("identity trace collector controls are malformed")
            root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
            if not isinstance(root, dict): raise ValueError("root manifest is not an object")
            producer_files = {relative.removeprefix("raw/comm_trace/"): _secure_read(run_dir, relative)[0]
                              for relative in trace_files if relative != finalization_path}
            derived = derive_identity_trace(producer_files, schemas, _schema_errors, {
                field: str(m.get(field)) for field in trusted["identity_fields"]},
                producer_pid=producer_pid, producer_start_time_ticks=producer_start_time_ticks,
                topology_generation=topology_generation, topology_sha256=topology_sha256)
            offline_identity_derivation = derived
            relative = "normalized/identity_trace.jsonl"
            artifact = _secure_read(run_dir, relative)[0]
            publication = root.get("p0a_identity_trace")
            entry = next((item for item in entry_list if isinstance(item, dict)
                          and item.get("relative_path") == relative), None)
            expected_publication = {
                "collector_written": True, "relative_path": relative,
                "content_sha256": derived["content_sha256"],
                "source_set_sha256": derived["source_set_sha256"],
                "record_count": derived["record_count"],
            }
            if (artifact != derived["bytes"] or publication != expected_publication
                    or not isinstance(entry, dict)
                    or entry.get("sha256") != derived["content_sha256"]
                    or entry.get("record_count") != derived["record_count"]
                    or entry.get("schema_version") != "mw_perf.p0a.identity_trace.v2"):
                raise ValueError("identity trace derivation mismatch")
        except (OSError, ValueError, TypeError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_IDENTITY_TRACE")
    offline_hook_derivation: dict[str, Any] | None = None
    if trusted.get("registration_contract_version") in (
            "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
            "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8",
            _P0A_REMOTE_CONTRACT):
        try:
            from .dds_trace import derive_hook_callgraph
            if offline_identity_derivation is None:
                raise ValueError("identity trace could not be regenerated")
            root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
            resolution_bytes = _secure_read(run_dir, "capability/resolution.json")[0]
            resolution = _strict_json(resolution_bytes)
            identity_bytes = _secure_read(run_dir, "normalized/identity_trace.jsonl")[0]
            if not isinstance(root, dict) or not isinstance(resolution, dict):
                raise ValueError("hook callgraph inputs are malformed")
            if identity_bytes != offline_identity_derivation["bytes"]:
                raise ValueError("identity trace regeneration mismatch")
            derived_hook = derive_hook_callgraph(
                offline_identity_derivation, resolution, resolution_bytes,
                registration_contract_sha256=hashlib.sha256(trusted_raw).hexdigest())
            offline_hook_derivation = derived_hook
            relative = "capability/hook_callgraph.json"
            artifact = _secure_read(run_dir, relative)[0]
            publication = root.get("p0a_hook_callgraph")
            entry = next((item for item in entry_list if isinstance(item, dict)
                          and item.get("relative_path") == relative), None)
            expected_publication = {
                "collector_written": True, "relative_path": relative,
                "content_sha256": derived_hook["content_sha256"],
                "boundary_catalog_sha256": derived_hook["boundary_catalog_sha256"],
                "delivery_set_sha256": derived_hook["delivery_set_sha256"],
                "record_count": 1,
            }
            if (artifact != derived_hook["bytes"] or publication != expected_publication
                    or not isinstance(entry, dict)
                    or entry.get("sha256") != derived_hook["content_sha256"]
                    or entry.get("record_count") != 1
                    or entry.get("schema_version") != "mw_perf.p0a.hook_callgraph.v2"):
                raise ValueError("hook callgraph derivation mismatch")
        except (OSError, ValueError, TypeError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_HOOK_CALLGRAPH")
    offline_normalized: dict[str, Any] | None = None
    if trusted.get("registration_contract_version") in (
            "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
            "mw_perf.p0a_contract.v8", _P0A_REMOTE_CONTRACT):
        try:
            from .dds_trace import derive_normalized_results
            if offline_identity_derivation is None or offline_hook_derivation is None:
                raise ValueError("normalized result inputs could not be regenerated")
            root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
            resolution_bytes = _secure_read(run_dir, "capability/resolution.json")[0]
            resolution = _strict_json(resolution_bytes)
            hook_bytes = _secure_read(run_dir, "capability/hook_callgraph.json")[0]
            if not isinstance(root, dict) or not isinstance(resolution, dict):
                raise ValueError("normalized result inputs are malformed")
            normalized = derive_normalized_results(
                offline_identity_derivation, resolution, resolution_bytes,
                offline_hook_derivation, hook_bytes)
            offline_normalized = normalized
            for key, metadata_name, relative, schema_version in (
                    ("comm_events", "p0a_comm_events", "normalized/comm_events.jsonl",
                     "mw_perf.comm_event.v2"),
                    ("latency_samples", "p0a_latency_samples",
                     "normalized/latency_samples.jsonl", "mw_perf.latency_sample.v2")):
                derived = normalized[key]
                artifact = _secure_read(run_dir, relative)[0]
                publication = root.get(metadata_name)
                entry = next((item for item in entry_list if isinstance(item, dict)
                              and item.get("relative_path") == relative), None)
                expected_publication = {
                    "collector_written": True, "relative_path": relative,
                    "content_sha256": derived["content_sha256"],
                    "source_identity_sha256": derived["source_identity_sha256"],
                    "source_identity_record_count": derived["source_identity_record_count"],
                    "record_count": derived["record_count"],
                }
                if (artifact != derived["bytes"] or publication != expected_publication
                        or not isinstance(entry, dict)
                        or entry.get("sha256") != derived["content_sha256"]
                        or entry.get("record_count") != derived["record_count"]
                        or entry.get("schema_version") != schema_version):
                    raise ValueError("normalized result derivation mismatch")
        except (OSError, ValueError, TypeError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_NORMALIZED_RESULTS")
    offline_communication_latency: dict[str, Any] | None = None
    if trusted.get("registration_contract_version") in (
            "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8",
            _P0A_REMOTE_CONTRACT):
        try:
            from .dds_trace import derive_communication_latency
            if (offline_identity_derivation is None or offline_hook_derivation is None
                    or offline_normalized is None):
                raise ValueError("communication latency inputs could not be regenerated")
            root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
            resolution_bytes = _secure_read(run_dir, "capability/resolution.json")[0]
            resolution = _strict_json(resolution_bytes)
            hook_bytes = _secure_read(run_dir, "capability/hook_callgraph.json")[0]
            if not isinstance(root, dict) or not isinstance(resolution, dict):
                raise ValueError("communication latency inputs are malformed")
            derived = derive_communication_latency(
                offline_identity_derivation, resolution, resolution_bytes,
                offline_hook_derivation, hook_bytes, offline_normalized,
                registration_contract_sha256=hashlib.sha256(trusted_raw).hexdigest())
            offline_communication_latency = derived
            relative = "capability/communication_latency.json"
            artifact = _secure_read(run_dir, relative)[0]
            publication = root.get("p0a_communication_latency")
            entry = next((item for item in entry_list if isinstance(item, dict)
                          and item.get("relative_path") == relative), None)
            expected_publication = {
                "collector_written": True, "relative_path": relative,
                "content_sha256": derived["content_sha256"],
                "latency_samples_content_sha256": derived["latency_samples_content_sha256"],
                "latency_samples_record_count": derived["latency_samples_record_count"],
                "sample_set_sha256": derived["sample_set_sha256"],
                "sample_set_count": derived["sample_set_count"],
            }
            if (artifact != derived["bytes"] or publication != expected_publication
                    or not isinstance(entry, dict)
                    or entry.get("sha256") != derived["content_sha256"]
                    or entry.get("record_count") != 1
                    or entry.get("schema_version") != "mw_perf.communication_latency.v2"):
                raise ValueError("communication latency derivation mismatch")
        except (OSError, ValueError, TypeError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_COMMUNICATION_LATENCY")
    offline_communication_delivery: dict[str, Any] | None = None
    if trusted.get("registration_contract_version") in (
            "mw_perf.p0a_contract.v8", _P0A_REMOTE_CONTRACT):
        try:
            from .dds_trace import derive_communication_delivery
            if (offline_identity_derivation is None or offline_hook_derivation is None
                    or offline_normalized is None or offline_communication_latency is None):
                raise ValueError("communication delivery inputs could not be regenerated")
            root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
            resolution_bytes = _secure_read(run_dir, "capability/resolution.json")[0]
            resolution = _strict_json(resolution_bytes)
            hook_bytes = _secure_read(run_dir, "capability/hook_callgraph.json")[0]
            if not isinstance(root, dict) or not isinstance(resolution, dict):
                raise ValueError("communication delivery inputs are malformed")
            derived = derive_communication_delivery(
                offline_identity_derivation, resolution, resolution_bytes,
                offline_hook_derivation, hook_bytes, offline_normalized,
                offline_communication_latency,
                registration_contract_sha256=hashlib.sha256(trusted_raw).hexdigest(),
                expectations={"publish_count": 1000, "readers_per_sample": 1,
                              "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records"})
            offline_communication_delivery = derived
            relative = "validation/communication_delivery.json"
            artifact = _secure_read(run_dir, relative)[0]
            publication = root.get("p0a_communication_delivery")
            entry = next((item for item in entry_list if isinstance(item, dict)
                          and item.get("relative_path") == relative), None)
            expected_publication = {
                "collector_written": True, "relative_path": relative,
                "content_sha256": derived["content_sha256"],
                "expected_publish_count": derived["expected_publish_count"],
                "expected_readers_per_sample": derived["expected_readers_per_sample"],
                "observed_publish_count": derived["observed_publish_count"],
                "observed_receive_count": derived["observed_receive_count"],
                "observed_dispatch_count": derived["observed_dispatch_count"],
                "observed_callback_count": derived["observed_callback_count"],
                "sample_set_sha256": derived["sample_set_sha256"],
                "delivery_set_sha256": derived["delivery_set_sha256"],
                "record_count": 1,
            }
            if (artifact != derived["bytes"] or publication != expected_publication
                    or not isinstance(entry, dict)
                    or entry.get("sha256") != derived["content_sha256"]
                    or entry.get("record_count") != 1
                    or entry.get("schema_version") != "mw_perf.communication_delivery.v2"):
                raise ValueError("communication delivery derivation mismatch")
        except (OSError, ValueError, TypeError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_COMMUNICATION_DELIVERY")
    if trusted.get("registration_contract_version") in (
            "mw_perf.p0a_contract.v8", _P0A_REMOTE_CONTRACT):
        try:
            from .dds_trace import derive_trace_quality
            if (offline_identity_derivation is None
                    or offline_communication_delivery is None):
                raise ValueError("trace quality inputs could not be regenerated")
            root = _strict_json(_secure_read(run_dir, "manifest.json")[0])
            if not isinstance(root, dict):
                raise ValueError("trace quality root manifest is malformed")
            derived = derive_trace_quality(
                offline_identity_derivation, offline_communication_delivery,
                registration_contract_version=trusted["registration_contract_version"],
                registration_contract_sha256=hashlib.sha256(trusted_raw).hexdigest())
            relative = "validation/trace_quality.json"
            artifact = _secure_read(run_dir, relative)[0]
            publication = root.get("p0a_trace_quality")
            entry = next((item for item in entry_list if isinstance(item, dict)
                          and item.get("relative_path") == relative), None)
            expected_publication = {
                "collector_written": True, "relative_path": relative,
                "content_sha256": derived["content_sha256"],
                "raw_source_set_sha256": derived["raw_source_set_sha256"],
                "stage_record_count": derived["stage_record_count"],
                "exact_record_count": derived["exact_record_count"],
                "sample_set_sha256": derived["sample_set_sha256"],
                "delivery_set_sha256": derived["delivery_set_sha256"],
                "record_count": 1,
            }
            if (artifact != derived["bytes"] or publication != expected_publication
                    or not isinstance(entry, dict)
                    or entry.get("sha256") != derived["content_sha256"]
                    or entry.get("record_count") != 1
                    or entry.get("schema_version") != (
                        "mw_perf.trace_quality_ohos_remote.v1"
                        if trusted.get("registration_contract_version") == _P0A_REMOTE_CONTRACT
                        else "mw_perf.trace_quality.v1")):
                raise ValueError("trace quality derivation mismatch")
        except (OSError, ValueError, TypeError, UnicodeError, json.JSONDecodeError, KeyError):
            errors.append("P0A_TRACE_QUALITY")
    # Recheck only metadata and verify each path still names the same snapshot.
    for relative, identity in snapshots.items():
        try:
            if identity != _secure_stat(run_dir, relative): raise OSError("snapshot changed")
        except OSError: errors.append("P0A_SNAPSHOT_CHANGED")
    if directory_decl is not None:
        try:
            if _trace_files(run_dir) != trace_files: errors.append("P0A_SNAPSHOT_CHANGED")
        except ValueError: errors.append("P0A_SNAPSHOT_CHANGED")
    result = {"valid": not errors, "failure_codes": sorted(set(errors))}
    if trace_summary is not None:
        result["dds_comm_trace"] = trace_summary
    return result

def report(path: Path, profile: str = "standard") -> None:
    m = strict_json_loads((path/"manifest.json").read_bytes()); v = validate_dir(path, profile=profile)
    if _resource_integrity_errors():
        raise RuntimeError("installed schemas/contracts failed integrity validation")
    write_json(path/"validation.json", v)
    core = m.get("core_metrics", {})
    checked_core = v.get("core_metrics", core)
    provenance = core.get("provenance", {})
    latency = m.get("latency_artifact", {}); checked_latency = v.get("latency_artifact", latency); lp = latency.get("provenance", {})
    trace = m.get("dds_comm_trace", {})
    audit = v.get("schema_audit", {})
    validation_schema = audit.get("schemas", {}).get("validation", {})
    report_text = f"# mw-perf-collect\n\n- run_id: `{m['run_id']}`\n- status: `{m['run_status']}`\n- candidate only: `{m.get('candidate_only', False)}`\n- samples: `{m.get('sample_count', 0)}`\n- Core Metrics status: `{core.get('status', 'unsupported')}`\n- Core Metrics records: `{checked_core.get('record_count', core.get('record_count', 0))}`\n- Core Metrics kinds: `{', '.join(checked_core.get('kinds', core.get('kinds', [])))}`\n- Core Metrics source SHA-256: `{provenance.get('source_sha256', 'n/a')}`\n- Core Metrics archive SHA-256: `{provenance.get('archive_sha256', 'n/a')}`\n- Core Metrics graceful finalization: `{provenance.get('graceful_finalization', 'n/a')}`\n- Core Metrics source root: `{provenance.get('root', 'n/a')}`\n- Latency artifact status: `{latency.get('status', 'unsupported')}`\n- Latency records: `{checked_latency.get('record_count', 0)}`\n- Latency topic/type: `{checked_latency.get('topic', 'n/a')}` / `{checked_latency.get('type', 'n/a')}`\n- Latency source SHA-256: `{lp.get('source_sha256', 'n/a')}`\n- Latency archive SHA-256: `{lp.get('archive_sha256', 'n/a')}`\n- Latency graceful finalization: `{lp.get('graceful_finalization', 'n/a')}`\n- DDS comm trace status: `{trace.get('status', 'not_applicable')}`\n- DDS comm trace records: `{trace.get('record_count', 0)}`\n- Runtime changed: `{m.get('runtime_changed', False)}`\n- Business unchanged: `{m.get('business_unchanged', False)}`\n- valid_for_comparison: `{v['valid_for_comparison']}`\n- Validation schema ID: `{validation_schema.get('schema_id', 'n/a')}`\n- Validation schema SHA-256: `{validation_schema.get('schema_sha256', 'n/a')}`\n"
    (path/"report.md").write_text(report_text, encoding="utf-8")

def _same_process(pid: int, start_time_ticks: int) -> bool:
    stat = proc_stat(pid)
    return stat is not None and stat["start_time_ticks"] == start_time_ticks

def _remember_processes(root_pid: int, known: dict[int, int]) -> None:
    for pid in [root_pid] + descendants(root_pid):
        stat = proc_stat(pid)
        if stat is not None: known.setdefault(pid, stat["start_time_ticks"])

class _StopResult:
    def __init__(self, error: str | None, sigkill_escalated: bool):
        self.error = error
        self.sigkill_escalated = sigkill_escalated

    def __eq__(self, other: object) -> bool:
        if isinstance(other, str) or other is None: return self.error == other
        return super().__eq__(other)


def _stop_managed(proc: subprocess.Popen[Any], timeout: float, known: dict[int, int]) -> _StopResult:
    errors = []
    sigkill_escalated = False
    try: os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    except OSError as exc: errors.append(f"group SIGTERM: {exc}")
    def alive() -> list[tuple[int, int]]:
        proc.poll()  # reap the leader promptly so a zombie is not treated as live
        return [(p, s) for p, s in known.items() if not (p == proc.pid and proc.returncode is not None) and _same_process(p, s)]
    for sig in (signal.SIGTERM, signal.SIGKILL):
        targets = alive()
        if sig == signal.SIGKILL and targets: sigkill_escalated = True
        for pid, start in known.items():
            if (pid, start) in targets:
                try: os.kill(pid, sig)
                except ProcessLookupError: pass
                except OSError as exc: errors.append(f"pid {pid} {sig.name}: {exc}")
        # Escalation gets a fresh window: time spent waiting for TERM must not
        # consume the time needed for SIGKILL delivery and delayed kernel reap.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and alive(): time.sleep(.02)
        if not alive(): break
    try: proc.wait(timeout=max(0, timeout))
    except (subprocess.TimeoutExpired, OSError) as exc: errors.append(f"reap: {exc}")
    remaining = [str(p) for p, _ in alive()]
    if remaining: errors.append("processes remain: " + ",".join(remaining))
    return _StopResult("; ".join(errors) or None, sigkill_escalated)


def _run_admission(adapter: str, p0a: dict[str, Any]) -> dict[str, Any] | None:
    """Validate P0a control-plane semantics before creating run resources."""
    run_type = p0a.get("run_type")
    if run_type not in ("observer_diagnostic", "exact_run", "stage_run"):
        return None
    observer = p0a.get("observer")
    measurement_kind = p0a.get("measurement_kind")
    if not isinstance(observer, bool):
        raise ValueError(f"{run_type} requires explicit boolean p0a_evidence.observer")
    if run_type == "observer_diagnostic":
        if adapter != "ros2" or observer is not True or measurement_kind != "observer_latency":
            raise ValueError(
                "observer_diagnostic requires adapter ros2, observer=true, "
                "and measurement_kind=observer_latency")
        code = "ROS2_OBSERVER_NOT_IMPLEMENTED"
        return {
            "status": "refused", "failure_code": code, "failure_codes": [code],
            "required_stop": True, "metric_availability": "unsupported",
            "valid_for_metric": False, "valid_for_comparison": False,
        }
    if run_type == "exact_run":
        if observer is not False or measurement_kind != "exact_correlated":
            raise ValueError(
                "exact_run requires observer=false and measurement_kind=exact_correlated")
    elif observer is not False or measurement_kind != "stage_only":
        raise ValueError(
            "stage_run requires observer=false and measurement_kind=stage_only")
    return None


def run(args: argparse.Namespace) -> int:
    run_dir: Path | None = None; manifest: dict[str, Any] | None = None
    proc: subprocess.Popen[Any] | None = None; failure: BaseException | None = None; cleanup_error: str | None = None
    stop_result = _StopResult(None, False)
    config: dict[str, Any] = {}; managed_identities: dict[int, int] = {}; core_metrics_value: Any = None
    p0a_config: dict[str, Any] = {}; validation_profile = "standard"
    provenance_config: Any = {}; runtime_seen: dict[tuple[Any, ...], dict[str, Any]] = {}
    resolution_declarations: list[dict[str, Any]] = []
    resolution_seen: dict[str, dict[str, Any]] = {}
    identity_derivation: dict[str, Any] | None = None
    resolution_proof: dict[str, Any] | None = None
    resolution_proof_bytes: bytes | None = None
    trusted_contract_raw: bytes | None = None
    core_metrics_baseline: dict[str, Any] = {"exists": False, "path": None}
    latency_value: Any = None; latency_baseline: dict[str, Any] = {"exists": False, "path": None}
    csv_latency_value: Any = None; csv_latency_baseline: dict[str, Any] = {}
    comm_trace_dirfd: int | None = None; comm_trace_controls: dict[str, str] = {}
    try:
        config = load_config(args.config); validation_profile = effective_validation_profile(config)
        sut = config.get("sut", {}); mode = sut.get("mode", "managed")
        ibmw_config = _adapter_config(config, "ibmw") if args.adapter == "ibmw" else {}
        p0a_value = config.get("p0a_evidence")
        p0a_config = dict(p0a_value) if isinstance(p0a_value, dict) else dict(ibmw_config.get("p0a_evidence", {})) if isinstance(ibmw_config.get("p0a_evidence"), dict) else {}
        if ("candidate_only" in p0a_config
                and not isinstance(p0a_config["candidate_only"], bool)):
            raise ValueError("p0a_evidence.candidate_only must be boolean")
        refusal = _run_admission(args.adapter, p0a_config)
        if refusal is not None:
            print(json.dumps(refusal, sort_keys=True))
            return EXIT_INVALID
        resolution_config = p0a_config.get("resolution")
        if (p0a_config.get("registration_contract_version") == "mw_perf.p0a_contract.v4"
                and (mode != "managed" or p0a_config.get("run_type") != "exact_run"
                     or resolution_config is None)):
            raise ValueError("P0a contract v4 requires managed exact_run with resolution")
        if (p0a_config.get("registration_contract_version") in (
                "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
                "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")
                and (mode != "managed" or p0a_config.get("run_type") != "exact_run"
                     or resolution_config is None)):
            raise ValueError("P0a contract v5/v6/v7 requires managed exact_run with resolution")
        if (p0a_config.get("registration_contract_version") in (
                "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
                "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")
                and (p0a_config.get("trace_mode") != "full"
                     or p0a_config.get("capability_id") !=
                        "dds.ordinary.nonplain.single_driver.sync_callback"
                     or p0a_config.get("measurement_kind") != "exact_correlated")):
            raise ValueError("P0a contract v5/v6/v7 requires the eligible full exact DDS capability")
        if (p0a_config.get("registration_contract_version") in (
                "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")
                and p0a_config.get("candidate_only") is not True):
            raise ValueError("P0a contract v7/v8 requires candidate_only=true")
        if p0a_config.get("registration_contract_version") == "mw_perf.p0a_contract.v8":
            frozen_delivery_expectations = {
                "publish_count": 1000,
                "readers_per_sample": 1,
                "ordering_policy": "dds_identity_monotonic_stage_and_callback_completion_records",
            }
            if p0a_config.get("delivery_expectations") != frozen_delivery_expectations:
                raise ValueError("P0a contract v8 requires exact delivery_expectations")
        if resolution_config is not None:
            if (mode != "managed" or p0a_config.get("run_type") != "exact_run"
                    or p0a_config.get("registration_contract_version") not in (
                        "mw_perf.p0a_contract.v3", "mw_perf.p0a_contract.v4",
                        "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
                        "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")):
                raise ValueError("p0a_evidence.resolution supports managed exact_run contract v3-v8 only")
            if not isinstance(resolution_config, dict) or set(resolution_config) != {"runtime_images"}:
                raise ValueError("p0a_evidence.resolution must contain only runtime_images")
            resolution_declarations = _normalize_resolution_declarations(
                resolution_config.get("runtime_images"))
            if p0a_config.get("registration_contract_version") in (
                    "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
                    "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8"):
                _validate_v5_resolution_roles(resolution_declarations)
        provenance_config = ibmw_config.get("provenance", {})
        provenance_required = (provenance_config is True or
                               isinstance(provenance_config, dict) and provenance_config.get("required") is True)
        if provenance_required and (not isinstance(config.get("scenario_contract"), dict) or not config["scenario_contract"]):
            raise RuntimeError("required provenance needs a non-empty scenario_contract")
        runtime_declarations = (provenance_config.get("runtime_images")
                                if isinstance(provenance_config, dict) else None)
        if provenance_required and (not isinstance(runtime_declarations, list) or not runtime_declarations):
            raise RuntimeError("required IBMW provenance needs non-empty runtime_images declarations")
        if args.command and sut.get("command") and not args.override_command: raise RuntimeError("config and CLI command both set; add --override-command")
        cli_command = list(args.command or [])
        if cli_command[:1] == ["--"]: cli_command = cli_command[1:]
        command = cli_command or sut.get("command")
        if mode == "managed" and not command: raise RuntimeError("managed mode requires sut.command")
        if mode not in ("managed", "attached"): raise RuntimeError("sut.mode must be managed or attached")
        run_dir = Path(args.output)/(time.strftime("%Y%m%d-%H%M%S")+"-"+uuid.uuid4().hex[:8])
        for d in ("environment", "topology", "capability", "raw", "normalized", "logs", "registration", "validation"): (run_dir/d).mkdir(parents=True)
        for relative_path in _SAMPLE_ARTIFACT_PATHS.values():
            (run_dir/relative_path).write_bytes(b"")
        contract = config.get("scenario_contract") if isinstance(config.get("scenario_contract"), dict) else None
        manifest = {"schema_version": 1, "tool_version": VERSION, "run_id": run_dir.name, "adapter": args.adapter, "mode": mode, "run_status": "failed", "metric_availability": "supported", "measurement_kind": "metadata", "start_time": time.time(), "command": command, "scenario_contract_hash": _hash_contract(config) or hashlib.sha256(b"{}").hexdigest(), "states": ["PREFLIGHT", "PREPARE"], "metrics_valid": False, "adapter_readiness": False}
        if validation_profile == "p0a":
            p0a_config.setdefault("registration_contract_version", "mw_perf.p0a_contract.v2")
            p0a_config.setdefault("capability_id", args.adapter)
            p0a_config.setdefault("path_id", "default")
            manifest["validation_profile"] = "p0a"
            manifest["p0a_evidence_required"] = True
            manifest["run_type"] = str(p0a_config.get("run_type", "p0a_proof"))
            if p0a_config.get("candidate_only") is True:
                manifest["candidate_only"] = True
        if (p0a_config.get("registration_contract_version") == "mw_perf.p0a_contract.v8"
                and resolution_declarations):
            declaration_record = {
                "schema": "mw_perf.p0a.resolution_declarations.v1",
                "registration_contract_version": "mw_perf.p0a_contract.v8",
                "run_id": manifest["run_id"],
                "declaration_sha256": hashlib.sha256(
                    _canonical_json_bytes(resolution_declarations)).hexdigest(),
                "declarations": resolution_declarations,
            }
            _secure_write_new(run_dir, "registration/resolution_declarations.json",
                              _canonical_json_bytes(declaration_record, newline=True))
        if contract: manifest["scenario_contract"] = json.loads(json.dumps(contract))
        comparison = config.get("comparison")
        if comparison is not None:
            if not isinstance(comparison, dict):
                raise ValueError("comparison must be an object")
            mapping = {"schema": "comparison_schema", "lane": "comparison_lane", "block": "comparison_block_id"}
            for name in ("schema", "contract", "profile", "workload", "resource_scope", "order",
                         "order_policy", "order_seed", "lane", "block", "sequence_index"):
                if name in comparison:
                    manifest[mapping.get(name, name)] = json.loads(json.dumps(comparison[name]))
            manifest["resource_collection"] = {
                "include_descendants": config.get("collectors", {}).get("process", {}).get("include_descendants", True),
                "network_interfaces": config.get("collectors", {}).get("network", {}).get("interfaces"),
            }
        csv_latency_value = config.get("artifacts", {}).get("latency") if isinstance(config.get("artifacts"), dict) else None
        if csv_latency_value is not None:
            from .adapters.csv_latency import snapshot
            csv_latency_baseline = snapshot(csv_latency_value)
        if args.adapter == "ibmw":
            from .adapters.ibmw import core_metrics_snapshot
            core_metrics_value = _adapter_config(config, "ibmw").get("core_metrics")
            core_metrics_baseline = core_metrics_snapshot(core_metrics_value)
        elif args.adapter == "ros2":
            from .adapters.ros2 import latency_artifact_snapshot
            ros2_options = _adapter_config(config, "ros2")
            p0a_required = validation_profile == "p0a"
            if p0a_required:
                ros2_options["preflight"] = True
                config.setdefault("adapters", {})["ros2"] = ros2_options
            latency_value = ros2_options.get("latency_artifact")
            latency_baseline = latency_artifact_snapshot(latency_value)
        write_json(run_dir/"environment"/"host.json", {"platform": sys.platform, "python": sys.version, "env": {k: os.environ.get(k) for k in ("ROS_DISTRO", "RMW_IMPLEMENTATION", "ROS_DOMAIN_ID", "IBMW_OBSERVABILITY_MODE")}})
        pid = sut.get("pid")
        collection_trace = validation_profile == "p0a" and manifest.get("run_type") in (
            "stage_run", "exact_run", "observer_diagnostic")
        if collection_trace and mode == "attached":
            manifest.update({"run_status": "invalid", "required_stop": True,
                             "metric_availability": "unsupported", "adapter_readiness": False,
                             "dds_comm_trace": {"required": True, "status": "unsupported",
                                                "reason": "attached P0a binary trace is unsupported"}})
            raise RuntimeError("attached P0a binary trace is unsupported")
        if collection_trace:
            trace_mode = p0a_config.get("trace_mode")
            if trace_mode not in ("sampled", "full"):
                raise RuntimeError("p0a_evidence.trace_mode must be sampled or full")
            capability_id = str(p0a_config.get("capability_id", args.adapter))
            exact_run = manifest.get("run_type") == "exact_run"
            measurement_kind = str(p0a_config.get("measurement_kind", "stage_only"))
            if exact_run and (trace_mode != "full"
                    or capability_id != "dds.ordinary.nonplain.single_driver.sync_callback"
                    or measurement_kind != "exact_correlated"):
                manifest.update({"run_status": "invalid", "required_stop": True,
                    "metric_availability": "unsupported", "adapter_readiness": False,
                    "dds_comm_trace": {"required": True, "status": "unsupported",
                        "reason": "exact_run capability contract is not eligible"}})
                raise RuntimeError("exact_run requires full trace and the exact DDS capability contract")
            path_id = str(p0a_config.get("path_id", "default"))
            topology_value = config.get("scenario_contract", config.get("topology", {}))
            topology_sha = hashlib.sha256(json.dumps(topology_value, sort_keys=True,
                separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest()
            topology_generation = str(p0a_config.get("topology_generation", topology_sha[:16]))
            trace_dir = run_dir / "raw" / "comm_trace"
            trace_dir.mkdir(mode=0o700)
            os.chmod(trace_dir, 0o700)
            comm_trace_dirfd = os.open(trace_dir, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                                       | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
            comm_trace_controls = {
                "MW_PERF_RUN_ID": manifest["run_id"], "MW_PERF_CAPABILITY_ID": capability_id,
                "MW_PERF_PATH_ID": path_id, "MW_PERF_DEPLOYMENT_MODE": "managed",
                "MW_PERF_COMM_TRACE_DIR_FD": str(comm_trace_dirfd),
                "MW_PERF_TOPOLOGY_GENERATION": topology_generation,
                "MW_PERF_TOPOLOGY_SHA256": topology_sha,
                "IBMW_COMM_TRACE": trace_mode, "IBMW_COMM_TRACE_SINK": "managed",
            }
            if exact_run:
                comm_trace_controls["MW_PERF_EXACT_REQUIRED"] = "1"
        if mode == "managed":
            cmd = command if isinstance(command, list) else ["/bin/sh", "-c", str(command)]
            producer_env = {"MW_PERF_RUN_ID": manifest["run_id"],
                            "MW_PERF_ARTIFACT_SCHEMA_VERSION": "mw_perf.latency.v1"}
            try:
                proc = subprocess.Popen(cmd, cwd=sut.get("working_directory"),
                    env={**os.environ, **sut.get("env", {}), **producer_env, **comm_trace_controls},
                    pass_fds=((comm_trace_dirfd,) if comm_trace_dirfd is not None else ()),
                    start_new_session=True, stdout=(run_dir/"logs"/"sut.stdout.log").open("wb"),
                    stderr=(run_dir/"logs"/"sut.stderr.log").open("wb")); pid = proc.pid
            finally:
                if comm_trace_dirfd is not None:
                    os.close(comm_trace_dirfd); comm_trace_dirfd = None
        initial_stat = proc_stat(int(pid)) if pid else None
        if not pid or initial_stat is None: raise RuntimeError("SUT process is unavailable")
        start_time_ticks = initial_stat["start_time_ticks"]
        manifest["pid"] = int(pid); manifest["pid_start_time_ticks"] = start_time_ticks
        manifest["states"] += ["START_COLLECTORS", "START_SUT" if proc else "SKIPPED_START_SUT", "WARMUP"]
        if proc: _remember_processes(int(pid), managed_identities)
        if resolution_declarations:
            for image in _runtime_observations(int(pid), manifest["run_id"]):
                if image.get("pid") == int(pid): resolution_seen[image["observation_id"]] = image
        samples = 0; valid_samples = True
        warmup_end = time.time()+float(config.get("run", {}).get("warmup_seconds", 0))
        while time.time() < warmup_end:
            if not _same_process(int(pid), start_time_ticks): raise RuntimeError("SUT identity changed or exited during warmup")
            if proc: _remember_processes(int(pid), managed_identities)
            if args.adapter == "ibmw":
                for image in _runtime_observations(int(pid), manifest["run_id"]):
                    runtime_seen[(image["observation_id"],)] = image
            time.sleep(min(.1, max(0, warmup_end-time.time())))
        if not _same_process(int(pid), start_time_ticks): raise RuntimeError("SUT identity changed or exited after warmup")
        if proc: _remember_processes(int(pid), managed_identities)
        if resolution_declarations:
            for image in _runtime_observations(int(pid), manifest["run_id"]):
                if image.get("pid") == int(pid): resolution_seen[image["observation_id"]] = image
        adapter_result = _collect_adapter(args.adapter, config, run_dir, int(pid))
        readiness = adapter_result.get("readiness") if adapter_result else None
        manifest["adapter_readiness"] = readiness.get("ok") if isinstance(readiness, dict) else True
        if adapter_result and adapter_result.get("status") in ("degraded", "unsupported", "failed"):
            manifest["metric_availability"] = "unsupported"
        preflight_gate = _ros2_preflight_gate(adapter_result, _adapter_config(config, "ros2"),
                                               bool(args.adapter == "ros2" and locals().get("p0a_required", False)))
        if preflight_gate["required"]:
            manifest["preflight_readiness"] = preflight_gate
        if not preflight_gate["ok"]:
            manifest["run_status"] = "invalid"
            manifest["required_stop"] = True
            manifest["adapter_readiness"] = False
            valid_samples = False
        else:
            manifest["states"].append("SAMPLE")
            end = time.time()+float(config.get("run", {}).get("sample_seconds", 1)); interval = float(config.get("collectors", {}).get("process", {}).get("interval_ms", 1000))/1000
            with (run_dir/"raw"/"process.jsonl").open("w") as pf, (run_dir/"raw"/"network.jsonl").open("w") as nf:
              while time.time() < end:
                if not _same_process(int(pid), start_time_ticks): manifest["run_status"]="invalid"; valid_samples=False; break
                if proc: _remember_processes(int(pid), managed_identities)
                if args.adapter == "ibmw":
                    for image in _runtime_observations(int(pid), manifest["run_id"]):
                        runtime_seen[(image["observation_id"],)] = image
                if resolution_declarations:
                    for image in _runtime_observations(int(pid), manifest["run_id"]):
                        if image.get("pid") == int(pid): resolution_seen[image["observation_id"]] = image
                pids = [int(pid)] + (descendants(int(pid)) if config.get("collectors", {}).get("process", {}).get("include_descendants", True) else [])
                processes = [x for p in pids if (x:=proc_stat(p))]
                valid_samples = valid_samples and bool(processes)
                pf.write(json.dumps({"timestamp": time.time(), "processes": processes})+"\n"); nf.write(json.dumps({"timestamp": time.time(), "interfaces": network_snapshot(config.get("collectors", {}).get("network", {}).get("interfaces"))})+"\n"); pf.flush(); nf.flush(); samples += 1; time.sleep(interval)
        if not proc: manifest["warnings"] = ["attached process was not stopped"]
        manifest.update({"sample_count": samples, "metrics_valid": valid_samples and samples > 0, "run_status": manifest["run_status"] if manifest["run_status"]=="invalid" else ("completed" if samples else "partial"), "end_time": time.time()})
    except BaseException as exc:
        failure = exc
    finally:
        if comm_trace_dirfd is not None:
            try: os.close(comm_trace_dirfd)
            except OSError: pass
            comm_trace_dirfd = None
        if proc is not None:
            timeout = float(config.get("run", {}).get("stop_timeout_seconds", 5)) if "config" in locals() else 5
            _remember_processes(proc.pid, managed_identities)
            stop_result = _stop_managed(proc, timeout, managed_identities)
            cleanup_error = stop_result.error
        if manifest is not None and run_dir is not None:
            if proc is not None:
                manifest["sut_returncode"] = proc.returncode
            if failure is not None:
                manifest.update({"error": f"{type(failure).__name__}: {failure}", "end_time": time.time()})
                if manifest.get("run_status") != "invalid": manifest["run_status"] = "partial"
            if cleanup_error:
                manifest["cleanup_error"] = cleanup_error; manifest["run_status"] = "cleanup_failed"
            elif stop_result.sigkill_escalated:
                manifest["run_status"] = "partial"
                manifest["stop_escalated_sigkill"] = True
            if comm_trace_controls:
                manifest["dds_comm_trace"] = {"required": True, "status": "invalid",
                                               "runtime_changed": True,
                                               "business_unchanged": True}
                graceful = (failure is None and cleanup_error is None and proc is not None
                            and proc.returncode == 0 and not stop_result.sigkill_escalated
                            and manifest.get("run_status") == "completed")
                if graceful:
                    trace_fd: int | None = None
                    try:
                        from .dds_trace import (confirm_directory_unchanged, derive_identity_trace,
                                                scan_directory_fd, validate_directory_files,
                                                write_directory_finalization)
                        trace_fd = os.open(run_dir/"raw"/"comm_trace", os.O_RDONLY
                            | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
                            | getattr(os, "O_CLOEXEC", 0))
                        producer_files = scan_directory_fd(trace_fd)
                        trusted, trusted_contract_raw = _trusted_p0a_contract(
                            p0a_config["registration_contract_version"])
                        identity = {"run_id": manifest["run_id"],
                            "capability_id": comm_trace_controls["MW_PERF_CAPABILITY_ID"],
                            "path_id": comm_trace_controls["MW_PERF_PATH_ID"],
                            "deployment_mode": comm_trace_controls["MW_PERF_DEPLOYMENT_MODE"]}
                        if p0a_config["registration_contract_version"] in (
                                "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5",
                                "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
                                "mw_perf.p0a_contract.v8"):
                            identity_derivation = derive_identity_trace(producer_files,
                                trusted["artifact_schemas"], _schema_errors, identity,
                                producer_pid=int(manifest["pid"]),
                                producer_start_time_ticks=int(manifest["pid_start_time_ticks"]),
                                topology_generation=comm_trace_controls["MW_PERF_TOPOLOGY_GENERATION"],
                                topology_sha256=comm_trace_controls["MW_PERF_TOPOLOGY_SHA256"])
                            trace_result = identity_derivation["trace_result"]
                        else:
                            trace_result = validate_directory_files(producer_files,
                                trusted["artifact_schemas"], _schema_errors, identity,
                                run_type=str(manifest.get("run_type")),
                                measurement_kind=str(p0a_config.get("measurement_kind", "stage_only")),
                                producer_pid=manifest.get("pid"),
                                producer_start_time_ticks=manifest.get("pid_start_time_ticks"),
                                topology_generation=comm_trace_controls["MW_PERF_TOPOLOGY_GENERATION"],
                                topology_sha256=comm_trace_controls["MW_PERF_TOPOLOGY_SHA256"],
                                registration_contract_version=p0a_config["registration_contract_version"])
                        confirm_directory_unchanged(trace_fd, producer_files)
                        write_directory_finalization(trace_fd, identity,
                            [f"raw/comm_trace/{name}" for name in producer_files])
                        trace_metadata = {
                            "registration_contract_version": p0a_config["registration_contract_version"],
                            "run_type": str(manifest.get("run_type")),
                            "observer": bool(p0a_config.get("observer", False)),
                            "capability_id": identity["capability_id"],
                            "path_id": identity["path_id"],
                            "deployment_mode": identity["deployment_mode"],
                            "measurement_kind": str(trace_result.get("measurement_kind",
                                                                     p0a_config.get("measurement_kind"))),
                        }
                        manifest["dds_comm_trace"] = {"required": True,
                            **_collector_trace_summary(trace_result),
                            **trace_metadata,
                            "topology_generation": comm_trace_controls["MW_PERF_TOPOLOGY_GENERATION"],
                            "topology_sha256": comm_trace_controls["MW_PERF_TOPOLOGY_SHA256"]}
                        if manifest.get("run_type") == "exact_run":
                            manifest["measurement_kind"] = trace_metadata["measurement_kind"]
                        manifest["runtime_changed"] = True
                        manifest["business_unchanged"] = True
                    except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
                        manifest["run_status"] = "invalid"
                        manifest["dds_comm_trace"]["error"] = f"{type(exc).__name__}: {exc}"
                    finally:
                        if trace_fd is not None: os.close(trace_fd)
            if resolution_declarations:
                try:
                    if (proc is None or proc.returncode != 0 or cleanup_error is not None
                            or stop_result.sigkill_escalated):
                        raise ValueError("resolution finalization requires graceful managed process success")
                    proof, proof_bytes = _build_resolution_proof(
                        manifest, p0a_config, resolution_declarations,
                        list(resolution_seen.values()))
                    _publish_resolution(run_dir, manifest, proof, proof_bytes)
                    resolution_proof, resolution_proof_bytes = proof, proof_bytes
                except (OSError, ValueError, TypeError, KeyError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_resolution_error"] = f"{type(exc).__name__}: {exc}"
            if (p0a_config.get("registration_contract_version") in (
                    "mw_perf.p0a_contract.v4", "mw_perf.p0a_contract.v5",
                    "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
                    "mw_perf.p0a_contract.v8")
                    and manifest.get("run_status") == "completed"
                    and manifest.get("p0a_resolution", {}).get("collector_written") is True):
                try:
                    if identity_derivation is None:
                        raise ValueError("identity trace derivation is unavailable")
                    _publish_identity_trace(run_dir, manifest, identity_derivation)
                except (OSError, ValueError, TypeError, KeyError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_identity_trace_error"] = f"{type(exc).__name__}: {exc}"
            hook_derivation: dict[str, Any] | None = None
            if (p0a_config.get("registration_contract_version") in (
                    "mw_perf.p0a_contract.v5", "mw_perf.p0a_contract.v6",
                    "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")
                    and manifest.get("run_status") == "completed"
                    and manifest.get("p0a_identity_trace", {}).get("collector_written") is True):
                try:
                    from .dds_trace import derive_hook_callgraph
                    if (identity_derivation is None or resolution_proof is None
                            or resolution_proof_bytes is None or trusted_contract_raw is None):
                        raise ValueError("hook callgraph derivation inputs are unavailable")
                    hook_derivation = derive_hook_callgraph(
                        identity_derivation, resolution_proof, resolution_proof_bytes,
                        registration_contract_sha256=hashlib.sha256(
                            trusted_contract_raw).hexdigest())
                    _publish_hook_callgraph(run_dir, manifest, hook_derivation)
                except (OSError, ValueError, TypeError, KeyError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_hook_callgraph_error"] = f"{type(exc).__name__}: {exc}"
            normalized: dict[str, Any] | None = None
            if (p0a_config.get("registration_contract_version") in (
                    "mw_perf.p0a_contract.v6", "mw_perf.p0a_contract.v7",
                    "mw_perf.p0a_contract.v8")
                    and manifest.get("run_status") == "completed"
                    and manifest.get("p0a_hook_callgraph", {}).get("collector_written") is True):
                try:
                    from .dds_trace import derive_normalized_results
                    if (identity_derivation is None or resolution_proof is None
                            or resolution_proof_bytes is None or hook_derivation is None):
                        raise ValueError("normalized result derivation inputs are unavailable")
                    normalized = derive_normalized_results(
                        identity_derivation, resolution_proof, resolution_proof_bytes,
                        hook_derivation, hook_derivation["bytes"])
                    _publish_normalized_result(run_dir, manifest, name="p0a_comm_events",
                        relative="normalized/comm_events.jsonl",
                        derivation=normalized["comm_events"])
                    _publish_normalized_result(run_dir, manifest, name="p0a_latency_samples",
                        relative="normalized/latency_samples.jsonl",
                        derivation=normalized["latency_samples"])
                except (OSError, ValueError, TypeError, KeyError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_normalized_results_error"] = f"{type(exc).__name__}: {exc}"
            communication_latency: dict[str, Any] | None = None
            if (p0a_config.get("registration_contract_version") in (
                    "mw_perf.p0a_contract.v7", "mw_perf.p0a_contract.v8")
                    and manifest.get("run_status") == "completed"
                    and manifest.get("p0a_comm_events", {}).get("collector_written") is True
                    and manifest.get("p0a_latency_samples", {}).get("collector_written") is True):
                try:
                    from .dds_trace import derive_communication_latency
                    if (identity_derivation is None or resolution_proof is None
                            or resolution_proof_bytes is None or hook_derivation is None
                            or normalized is None or trusted_contract_raw is None):
                        raise ValueError("communication latency derivation inputs are unavailable")
                    communication_latency = derive_communication_latency(
                        identity_derivation, resolution_proof, resolution_proof_bytes,
                        hook_derivation, hook_derivation["bytes"], normalized,
                        registration_contract_sha256=hashlib.sha256(
                            trusted_contract_raw).hexdigest())
                    _publish_communication_latency(run_dir, manifest, communication_latency)
                except (OSError, ValueError, TypeError, KeyError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_communication_latency_error"] = \
                        f"{type(exc).__name__}: {exc}"
            communication_delivery: dict[str, Any] | None = None
            if (p0a_config.get("registration_contract_version") == "mw_perf.p0a_contract.v8"
                    and manifest.get("run_status") == "completed"
                    and manifest.get("p0a_communication_latency", {}).get(
                        "collector_written") is True):
                try:
                    from .dds_trace import derive_communication_delivery
                    if (identity_derivation is None or resolution_proof is None
                            or resolution_proof_bytes is None or hook_derivation is None
                            or normalized is None or communication_latency is None
                            or trusted_contract_raw is None):
                        raise ValueError("communication delivery derivation inputs are unavailable")
                    communication_delivery = derive_communication_delivery(
                        identity_derivation, resolution_proof, resolution_proof_bytes,
                        hook_derivation, hook_derivation["bytes"], normalized,
                        communication_latency,
                        registration_contract_sha256=hashlib.sha256(
                            trusted_contract_raw).hexdigest(),
                        expectations=p0a_config["delivery_expectations"])
                    _publish_communication_delivery(
                        run_dir, manifest, communication_delivery)
                except (OSError, ValueError, TypeError, KeyError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_communication_delivery_error"] = \
                        f"{type(exc).__name__}: {exc}"
            if (p0a_config.get("registration_contract_version") == "mw_perf.p0a_contract.v8"
                    and manifest.get("run_status") == "completed"
                    and manifest.get("p0a_communication_delivery", {}).get(
                        "collector_written") is True):
                try:
                    from .dds_trace import derive_trace_quality
                    if (identity_derivation is None or communication_delivery is None
                            or trusted_contract_raw is None):
                        raise ValueError("trace quality derivation inputs are unavailable")
                    trace_quality = derive_trace_quality(
                        identity_derivation, communication_delivery,
                        registration_contract_version="mw_perf.p0a_contract.v8",
                        registration_contract_sha256=hashlib.sha256(
                            trusted_contract_raw).hexdigest())
                    _publish_trace_quality(run_dir, manifest, trace_quality)
                except (OSError, ValueError, TypeError, KeyError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_trace_quality_error"] = f"{type(exc).__name__}: {exc}"
            if args.adapter == "ibmw":
                from .adapters.ibmw import archive_core_metrics
                ibmw_options = _adapter_config(config, "ibmw")
                ibmw_root = str(Path(ibmw_options.get("root", config.get("sut", {}).get("root", "."))).expanduser().resolve())
                try:
                    ibmw_environment = strict_json_loads((run_dir/"environment"/"ibmw.json").read_bytes())
                    ibmw_build = ibmw_environment.get("build", {})
                    manifest["provenance"] = ibmw_environment.get("provenance")
                except (OSError, ValueError, json.JSONDecodeError):
                    ibmw_build = {}
                declarations = provenance_config.get("runtime_images", []) if isinstance(provenance_config, dict) else []
                if declarations:
                    binaries = (manifest.get("provenance", {}).get("binaries")
                                if isinstance(manifest.get("provenance"), dict) else None)
                    runtime_result = _runtime_image_result(
                        declarations, list(runtime_seen.values()), manifest["run_id"], binaries)
                    runtime_path = run_dir/"raw"/"runtime_images.json"; write_json(runtime_path, runtime_result)
                    manifest["runtime_images"] = {"content_sha256": hashlib.sha256(runtime_path.read_bytes()).hexdigest(), "evidence": runtime_result}
                    if isinstance(manifest.get("provenance"), dict): manifest["provenance"]["runtime_images_required"] = True
                core_result = archive_core_metrics(core_metrics_value, core_metrics_baseline,
                                                   run_dir/"raw"/"core_metrics.jsonl", managed=proc is not None,
                                                   graceful_finalization=proc is not None and not stop_result.sigkill_escalated,
                                                   run_id=manifest["run_id"], adapter=args.adapter,
                                                   build=ibmw_build, root=ibmw_root, mode=manifest["mode"],
                                                   sut_pid=manifest.get("pid"),
                                                   sut_start_time_ticks=manifest.get("pid_start_time_ticks"))
                manifest["root"] = ibmw_root
                manifest["build"] = ibmw_build
                manifest["core_metrics"] = core_result
                capability_path = run_dir/"capability"/"ibmw.json"
                try:
                    capability = strict_json_loads(capability_path.read_bytes())
                except (OSError, ValueError, json.JSONDecodeError):
                    capability = {}
                capability["core_metrics"] = core_result
                degradations = [x for x in capability.get("degradations", []) if not str(x).startswith("core_metrics:")]
                if core_result["status"] != "supported":
                    degradations.append(f"core_metrics: {core_result['status']}; external P0 collection continues")
                capability["degradations"] = degradations
                write_json(capability_path, capability)
            elif args.adapter == "ros2":
                from .adapters.ros2 import archive_latency_artifact
                latency_result = archive_latency_artifact(latency_value, latency_baseline, run_dir/"raw"/"latency.jsonl",
                    managed=proc is not None, graceful_finalization=proc is not None and cleanup_error is None and not stop_result.sigkill_escalated,
                    run_id=manifest["run_id"], mode=manifest["mode"], sut_pid=int(manifest.get("pid", 0)),
                    sut_start_time_ticks=int(manifest.get("pid_start_time_ticks", 0)))
                manifest["latency_artifact"] = latency_result
                if latency_result.get("status") == "supported":
                    manifest["metric_availability"] = "supported"
                    manifest["measurement_kind"] = "producer_latency"
                capability_path = run_dir/"capability"/"ros2.json"
                try: capability = strict_json_loads(capability_path.read_bytes())
                except (OSError, ValueError, json.JSONDecodeError): capability = {}
                capability["latency_artifact"] = latency_result
                write_json(capability_path, capability)
            if csv_latency_value is not None:
                from .adapters.csv_latency import archive
                csv_result = archive(csv_latency_value, csv_latency_baseline,
                    run_dir/"raw"/"latency.csv", run_dir/"raw"/"lane_result.json",
                    run_id=manifest["run_id"], mode=manifest["mode"],
                    pid=int(manifest.get("pid", 0)), start_ticks=int(manifest.get("pid_start_time_ticks", 0)),
                    graceful_finalization=proc is not None and cleanup_error is None and not stop_result.sigkill_escalated)
                manifest.setdefault("artifacts", {})["latency"] = csv_result
                if csv_result.get("status") == "supported":
                    manifest["metric_availability"] = "supported"
                    manifest["measurement_kind"] = "latency_summary_csv"
                elif csv_result.get("required"):
                    manifest["run_status"] = "invalid"
            if validation_profile == "p0a":
                try:
                    p0a = build_p0a_evidence_manifest(
                        run_dir, p0a_config, run_id=manifest["run_id"],
                        capability_id=str(p0a_config.get("capability_id", args.adapter)),
                        path_id=str(p0a_config.get("path_id", "default")),
                        deployment_mode=str(p0a_config.get("deployment_mode", manifest["mode"])),
                        run_type=str(p0a_config.get("run_type", "p0a_proof")),
                        observer=bool(p0a_config.get("observer", False)),
                        measurement_kind=str(p0a_config.get("measurement_kind", "exact_correlated")))
                    if p0a is None: raise ValueError("P0a registration contract is missing or invalid")
                    write_json(run_dir / "capability/p0a_evidence_manifest.json", p0a)
                except (OSError, ValueError, TypeError, UnicodeError, json.JSONDecodeError) as exc:
                    manifest["run_status"] = "invalid"
                    manifest["p0a_evidence_error"] = f"{type(exc).__name__}: {exc}"
            if manifest.get("candidate_only") is True:
                manifest["metric_availability"] = "unsupported"
            try:
                manifest["sample_artifacts"] = _sample_artifact_evidence(run_dir)
                manifest["states"] += ["STOP_SUT" if proc else "SKIPPED_STOP_SUT", "VALIDATE", "REPORT", "PACKAGE"]
                write_json(run_dir/"manifest.json", manifest); write_json(run_dir/"validation.json", validate_dir(run_dir, profile=validation_profile)); report(run_dir, profile=validation_profile)
            except Exception as exc:
                if failure is None: failure = exc
    if failure is not None: print(f"mw-perf-collect: {failure}", file=sys.stderr)
    if cleanup_error: print(f"mw-perf-collect cleanup: {cleanup_error}", file=sys.stderr); return EXIT_CLEANUP
    if failure is not None: return EXIT_FAILED
    return EXIT_OK if manifest and run_dir and manifest["run_status"]=="completed" and validate_dir(run_dir, profile=validation_profile)["valid_for_comparison"] else EXIT_INVALID

def _adapter_config(config: dict[str, Any], name: str) -> dict[str, Any]:
    value = config.get("adapters", {}).get(name, config.get(name, {}))
    return dict(value) if isinstance(value, dict) else {}

def adapter_doctor(name: str, config: dict[str, Any]) -> dict[str, Any]:
    if name == "ros2":
        from .adapters import Ros2Adapter
        options = _adapter_config(config, "ros2")
        adapter_env = _ros2_collector_environment(config)
        return Ros2Adapter(environ=adapter_env,
                           ros2_executable=options.get("ros2_executable"),
                           ros2_prefix=options.get("ros2_prefix")).doctor()
    if name == "ibmw":
        from .adapters import collect_ibmw
        options = _adapter_config(config, "ibmw")
        root = options.pop("root", config.get("sut", {}).get("root", "."))
        result = collect_ibmw(root, **options)
        result["adapter"] = "ibmw"
        result["status"] = ("ready" if result["capability"]["core_metrics"]["available"]
                             else "degraded")
        return result
    if name == "generic":
        return {"adapter": "generic", "status": "ready"}
    raise RuntimeError(f"unknown adapter: {name}")

def _collect_adapter(name: str, config: dict[str, Any], run_dir: Path, pid: int) -> dict[str, Any]:
    if name == "generic": return {"status": "ok", "readiness": {"ok": True}}
    if name == "ros2":
        from .adapters import Ros2Adapter
        options = _adapter_config(config, "ros2"); options.setdefault("pid", pid)
        adapter_env = _ros2_collector_environment(config)
        result = Ros2Adapter(environ=adapter_env,
                             ros2_executable=options.get("ros2_executable"),
                             ros2_prefix=options.get("ros2_prefix")).run(options)
        observations = result.get("observations", [])
        preflight = next((x for x in observations if x.get("name") == "ros2_preflight"), None)
        write_json(run_dir/"environment"/"ros2.json", {"adapter": "ros2", "observations": [x for x in observations if x.get("name") in ("ros_environment", "ros2_cli")], **({"preflight": preflight} if preflight else {})})
        write_json(run_dir/"topology"/"ros2.json", next((x for x in observations if x.get("name") == "ros_graph_snapshot"), {}))
        write_json(run_dir/"capability"/"ros2.json", {"adapter": "ros2", "status": result.get("status"), "readiness": result.get("readiness"), **({"preflight": preflight} if preflight else {})})
        return result
    if name == "ibmw":
        from .adapters import collect_ibmw
        options = _adapter_config(config, "ibmw"); root = options.pop("root", config.get("sut", {}).get("root", "."))
        result = collect_ibmw(root, **options)
        write_json(run_dir/"environment"/"ibmw.json", result["manifest"])
        write_json(run_dir/"topology"/"ibmw.json", result["topology"])
        write_json(run_dir/"capability"/"ibmw.json", result["capability"])
        return result
    raise RuntimeError(f"unknown adapter: {name}")


def _ros2_collector_environment(config: dict[str, Any]) -> dict[str, str]:
    from .adapters.ros2 import Ros2Adapter
    environment = Ros2Adapter.sanitized_environment(os.environ, include_ros=True)
    environment.setdefault("PATH", os.defpath)
    sut_env = config.get("sut", {}).get("env", {})
    if isinstance(sut_env, dict):
        for key in Ros2Adapter.SUT_ENV_ALLOWLIST:
            value = sut_env.get(key)
            if isinstance(value, str) and Ros2Adapter.valid_environment_value(key, value):
                environment[key] = value
    return environment
