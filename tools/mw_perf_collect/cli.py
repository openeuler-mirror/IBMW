from __future__ import annotations
import argparse, hashlib, json, os, secrets, stat
from pathlib import Path
from .core import VERSION, EXIT_FAILED, EXIT_INVALID, run, report, validate_dir, load_config, adapter_doctor, _schema_errors, _schema_audit, _validation_matches_current, _resource_integrity_errors
from .json_strict import loads as strict_json_loads
from .metric_qualification import build_metric_qualification, validate_metric_qualification
from .ohos_capture import CaptureSemanticError, validate_capture_semantics
from .ohos_remote_p0a import OhosP0aMaterializationError, materialize_ohos_p0a_candidate

_METRIC_QUALIFICATION_FILES = frozenset({
    "declaration.json", "latency_samples.jsonl", "metric_qualification.json",
    "qualification_seal.json"})
_OHOS_IMPORT_ERROR = "OHOS_CAPTURE_IMPORT_INVALID"
_OHOS_OUTPUT_ERROR = "OHOS_CAPTURE_OUTPUT_EXISTS"
_OHOS_JSON_LIMIT = 1024 * 1024


def _read_strict_regular_json(path_value: str, label: str):
    path = Path(path_value)
    try:
        named = os.stat(path, follow_symlinks=False)
        if not stat.S_ISREG(named.st_mode) or named.st_nlink != 1:
            raise CaptureSemanticError(f"{label} JSON must be a bounded regular file")
        flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                 | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
        fd = os.open(path, flags)
    except OSError as error:
        raise CaptureSemanticError(f"{label} JSON open failed") from error
    try:
        before = os.fstat(fd)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
                or (before.st_dev, before.st_ino) != (named.st_dev, named.st_ino)
                or before.st_size > _OHOS_JSON_LIMIT):
            raise CaptureSemanticError(f"{label} JSON must be a bounded regular file")
        data = bytearray()
        while len(data) <= _OHOS_JSON_LIMIT:
            chunk = os.read(fd, min(65536, _OHOS_JSON_LIMIT + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
        after = os.fstat(fd)
        if (len(data) > _OHOS_JSON_LIMIT
                or (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns,
                    before.st_ctime_ns) !=
                   (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns,
                    after.st_ctime_ns)):
            raise CaptureSemanticError(f"{label} JSON changed while reading")
        value = strict_json_loads(data)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        if isinstance(error, CaptureSemanticError):
            raise
        raise CaptureSemanticError(f"{label} JSON is invalid") from error
    finally:
        os.close(fd)
    if not isinstance(value, dict):
        raise CaptureSemanticError(f"{label} JSON object required")
    return value, hashlib.sha256(data).hexdigest()


def _canonical_json_line(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("ascii") + b"\n"


def _publish_no_replace(path_value: str, data: bytes):
    path = Path(path_value)
    parent = path.parent
    parent_stat = os.stat(parent, follow_symlinks=False)
    if not stat.S_ISDIR(parent_stat.st_mode):
        raise OSError("output parent is not a directory")
    parent_fd = os.open(parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                        | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0))
    temporary_name = None
    try:
        for _ in range(128):
            temporary_name = ".mw-perf-ohos-import-" + secrets.token_hex(16)
            try:
                fd = os.open(temporary_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                             | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
                             0o600, dir_fd=parent_fd)
                break
            except FileExistsError:
                continue
        else:
            raise OSError("unable to allocate temporary output")
        try:
            view = memoryview(data)
            while view:
                written = os.write(fd, view)
                if written <= 0:
                    raise OSError("output write failed")
                view = view[written:]
            os.fsync(fd)
        finally:
            os.close(fd)
        os.link(temporary_name, path.name, src_dir_fd=parent_fd,
                dst_dir_fd=parent_fd, follow_symlinks=False)
        os.fsync(parent_fd)
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name, dir_fd=parent_fd)
            except FileNotFoundError:
                pass
        os.close(parent_fd)


def _ohos_import_document(archive, result, request, result_sha256, request_sha256):
    imported = validate_capture_semantics(archive, result, request)
    semantics = imported["imported_semantics"]
    stage_count = semantics.get("stage_record_count", semantics.get("record_count"))
    exact_correlated = semantics.get("measurement_kind") == "exact_correlated"
    exact_count = semantics.get("record_count") if exact_correlated else 0
    return {
        "schema": "mw_perf.ohos_capture_import.v1",
        "semantic_validation": imported["semantic_validation"],
        "formal_p0a": False,
        "status": semantics.get("status"),
        "stage_status": "supported",
        "exact_status": semantics.get("status") if exact_correlated else "not_requested",
        "measurement_kind": semantics.get("measurement_kind", "stage_only"),
        "counts": {
            "stage_records": stage_count,
            "exact_records": exact_count,
            "streams": semantics.get("stream_count"),
            "segments": semantics.get("segment_count"),
        },
        "hashes": {
            "archive_sha256": result["archive_sha256"],
            "result_json_sha256": result_sha256,
            "request_json_sha256": request_sha256,
        },
        "provenance": {
            "request_challenge": request["request_challenge"],
            "run_id": request["run_id"],
            "capability_id": request["capability_id"],
            "path_id": request["path_id"],
            "topology_generation": request["topology_generation"],
            "topology_sha256": request["topology_sha256"],
            "child_pid": result["child_pid"],
            "child_start_time_ticks": result["child_start_time_ticks"],
            "archive_size": result["archive_size"],
        },
    }


def _run_ohos_import(args):
    try:
        result, result_sha256 = _read_strict_regular_json(args.result, "result")
        request, request_sha256 = _read_strict_regular_json(args.request, "request")
        document = _ohos_import_document(
            args.archive, result, request, result_sha256, request_sha256)
        data = _canonical_json_line(document)
        if args.output:
            try:
                _publish_no_replace(args.output, data)
            except FileExistsError:
                failure = {"schema": "mw_perf.ohos_capture_import.v1",
                           "semantic_validation": False, "formal_p0a": False,
                           "error_code": _OHOS_OUTPUT_ERROR}
                print(_canonical_json_line(failure).decode("ascii"), end="")
                return EXIT_INVALID
        else:
            print(data.decode("ascii"), end="")
        return 0
    except (CaptureSemanticError, OSError, TypeError, KeyError, ValueError) as error:
        failure = {"schema": "mw_perf.ohos_capture_import.v1",
                   "semantic_validation": False, "formal_p0a": False,
                   "error_code": _OHOS_IMPORT_ERROR,
                   "error": str(error)[:256] or "capture import validation failed"}
        print(_canonical_json_line(failure).decode("ascii"), end="")
        return EXIT_INVALID


def _run_ohos_p0a_materialize(args):
    try:
        result, result_sha256 = _read_strict_regular_json(args.result, "result")
        request, request_sha256 = _read_strict_regular_json(args.request, "request")
        validation = materialize_ohos_p0a_candidate(
            args.archive, result, request, args.destination,
            result_sha256=result_sha256, request_sha256=request_sha256)
        print(_canonical_json_line({
            "schema": "mw_perf.ohos_remote_p0a_materialization.v1",
            "materialized": True, "formal_p0a": False,
            "destination": args.destination, "validation": validation,
        }).decode("ascii"), end="")
        return 0
    except (CaptureSemanticError, OhosP0aMaterializationError, OSError, TypeError,
            KeyError, ValueError) as error:
        print(_canonical_json_line({
            "schema": "mw_perf.ohos_remote_p0a_materialization.v1",
            "materialized": False, "formal_p0a": False,
            "error_code": ("OHOS_P0A_OUTPUT_EXISTS" if isinstance(error, FileExistsError)
                           else "OHOS_P0A_MATERIALIZATION_INVALID"),
            "error": str(error)[:256] or "candidate materialization failed",
        }).decode("ascii"), end="")
        return EXIT_INVALID


def _is_metric_qualification(path: Path) -> bool:
    """Detect qualification artifacts before treating a path as a comparison run."""
    try:
        names = {member.name for member in path.iterdir()}
        if names == _METRIC_QUALIFICATION_FILES:
            return True
        document = strict_json_loads((path / "metric_qualification.json").read_bytes())
        return isinstance(document, dict) and document.get("schema") == (
            "mw_perf.metric_qualification.v1")
    except (OSError, ValueError, json.JSONDecodeError):
        return False

def _comparison_audit():
    return _schema_audit({"comparison_result": ("result.schema.v1.json", "comparison_result"),
                          "comparison_plan": ("comparison-plan.schema.v1.json", "comparison_plan")},
                         validation_mode="comparison")

def main(argv=None):
    p=argparse.ArgumentParser(prog="mw-perf-collect"); s=p.add_subparsers(dest="action", required=True)
    d=s.add_parser("doctor"); d.add_argument("--adapter", default="generic"); d.add_argument("--config")
    r=s.add_parser("run"); r.add_argument("--adapter", default="generic"); r.add_argument("--config"); r.add_argument("--output", default="runs"); r.add_argument("--override-command", action="store_true"); r.add_argument("command", nargs=argparse.REMAINDER)
    report_parser=s.add_parser("report"); report_parser.add_argument("path"); report_parser.add_argument("--profile", choices=("standard", "p0a"), default="standard")
    validate_parser=s.add_parser("validate"); validate_parser.add_argument("path"); validate_parser.add_argument("--profile", choices=("standard", "p0a"), default="standard")
    c=s.add_parser("compare"); c.add_argument("--plan", required=True); c.add_argument("--output", default="comparison"); c.add_argument("--valid-blocks", type=int); c.add_argument("--order")
    qb=s.add_parser("metric-qualification-build")
    qv=s.add_parser("metric-qualification-validate")
    oi=s.add_parser("import-ohos-capture")
    oi.add_argument("--archive", required=True)
    oi.add_argument("--result", required=True)
    oi.add_argument("--request", required=True)
    oi.add_argument("--output")
    op=s.add_parser("materialize-ohos-p0a-candidate")
    op.add_argument("--archive", required=True)
    op.add_argument("--result", required=True)
    op.add_argument("--request", required=True)
    op.add_argument("--destination", required=True)
    for parser in (qb, qv):
        parser.add_argument("--campaign", required=True)
        parser.add_argument("--object-store", required=True)
        parser.add_argument("--registry", required=True)
        parser.add_argument("--registry-sha256", required=True)
        parser.add_argument("--provider", required=True)
        parser.add_argument("--campaign-id", required=True)
    qb.add_argument("--destination", required=True)
    qv.add_argument("--qualification", required=True)
    a=p.parse_args(argv)
    if a.action == "import-ohos-capture":
        return _run_ohos_import(a)
    if a.action == "materialize-ohos-p0a-candidate":
        return _run_ohos_p0a_materialize(a)
    if a.action=="doctor":
        try:
            result=adapter_doctor(a.adapter,load_config(a.config)); result["tool_version"]=VERSION
            result["common"]={"proc":Path('/proc').exists(),"sysfs_network":Path('/sys/class/net').exists(),"yaml":_yaml()}
            print(json.dumps(result,indent=2)); return 0
        except Exception as e: print(e); return EXIT_FAILED
    if a.action=="run": return run(a)
    if a.action=="report":
        try:
            report(Path(a.path), profile=a.profile)
            return 0
        except Exception as exc:
            print(f"RESOURCE_OR_REPORT_ERROR: {exc}")
            return EXIT_FAILED
    if a.action=="validate":
        v=validate_dir(Path(a.path), profile=a.profile); Path(a.path).mkdir(parents=True,exist_ok=True); (Path(a.path)/"validation.json").write_text(json.dumps(v,indent=2)+"\n"); print(json.dumps(v,indent=2)); return 0 if v["valid_for_metric"] else EXIT_INVALID
    if a.action == "metric-qualification-build":
        try:
            result = build_metric_qualification(
                Path(a.campaign), object_store=Path(a.object_store),
                registry_path=Path(a.registry), expected_registry_sha256=a.registry_sha256,
                provider=a.provider, campaign_id=a.campaign_id,
                destination=Path(a.destination))
        except (OSError, TypeError, ValueError) as exc:
            print(json.dumps({"valid": False, "failure_codes": [
                "METRIC_QUALIFICATION_INVALID"], "error": str(exc)}))
            return EXIT_INVALID
        print(json.dumps(result, indent=2)); return 0
    if a.action == "metric-qualification-validate":
        result = validate_metric_qualification(
            Path(a.qualification), campaign_root=Path(a.campaign),
            object_store=Path(a.object_store), registry_path=Path(a.registry),
            expected_registry_sha256=a.registry_sha256, provider=a.provider,
            campaign_id=a.campaign_id)
        print(json.dumps(result, indent=2)); return 0 if result["valid"] else EXIT_INVALID
    try:
        plan=load_config(a.plan)
        if not isinstance(plan, dict):
            raise ValueError("PLAN_MUST_BE_OBJECT")
    except Exception as exc:
        output = Path(a.output)
        output.mkdir(parents=True, exist_ok=True)
        failure = str(exc) or "PLAN_INVALID"
        (output / "comparison.json").write_text(json.dumps({
            "schema": "mw_perf.comparison-result.v1", "schema_version": 1,
            "valid_for_comparison": False, "valid_blocks": [],
            "invalid_blocks": [], "failure_codes": [failure], "schema_audit": _comparison_audit(),
        }, indent=2) + "\n")
        return EXIT_INVALID
    if plan.get("schema") == "mw_perf.comparison-plan.v1":
        return _compare_plan_v1(plan, a.output, a.valid_blocks)
    rows=[]; failures=list(_resource_integrity_errors()); expected_contract=None; expected_profile=None; blocks={}
    plan_validation_profile=plan.get("validation_profile", "standard")
    if plan_validation_profile not in ("standard", "p0a"): failures.append("VALIDATION_PROFILE_INVALID")
    for item in plan.get("runs",[]):
        path = Path(item if isinstance(item, (str, Path)) else item.get("path", ""))
        try:
            if _is_metric_qualification(path):
                raise ValueError("METRIC_QUALIFICATION_NOT_COMPARABLE")
            m=strict_json_loads((path/"manifest.json").read_bytes())
            validation_path = path/"validation.json"
            if not validation_path.exists(): raise ValueError("VALIDATION_MISSING")
            v=strict_json_loads(validation_path.read_bytes())
            validation_profile=(item.get("validation_profile", plan_validation_profile)
                                if isinstance(item, dict) else plan_validation_profile)
            if validation_profile not in ("standard", "p0a"): raise ValueError("VALIDATION_PROFILE_INVALID")
            if v.get("validation_profile", "standard") != validation_profile: raise ValueError("VALIDATION_PROFILE_MISMATCH")
            current=validate_dir(path, profile=validation_profile)
            if not _validation_matches_current(v, current): raise ValueError("VALIDATION_STALE_OR_TAMPERED")
            v=current
            if v.get("valid_for_comparison") is not True: raise ValueError("RUN_NOT_VALID_FOR_COMPARISON")
            contract=m.get("scenario_contract_hash"); profile=m.get("resource_accounting_profile", m.get("profile", m.get("scenario_profile"))); block=m.get("comparison_block_id", m.get("block", m.get("block_id"))); order=m.get("sequence_index", m.get("order_index"))
            if any(value is None for value in (contract, profile, block, order)): raise ValueError("COMPARISON_METADATA_MISSING")
            expected_contract = contract if expected_contract is None else expected_contract
            expected_profile = profile if expected_profile is None else expected_profile
            if contract != expected_contract: raise ValueError("CONTRACT_MISMATCH")
            if profile != expected_profile: raise ValueError("PROFILE_MISMATCH")
            order_policy=m.get("order", m.get("order_policy", m.get("randomization_order")))
            if a.order is not None and order_policy != a.order: raise ValueError("ORDER_MISMATCH")
            if order in blocks.setdefault(str(block), set()): raise ValueError("ORDER_DUPLICATE")
            blocks[str(block)].add(order)
            rows.append({"run_id":m["run_id"],"status":m["run_status"],"adapter":m.get("adapter")})
        except (OSError, ValueError, json.JSONDecodeError, KeyError) as exc:
            failures.append(str(exc))
    if a.valid_blocks is not None and len(blocks) < a.valid_blocks: failures.append("INSUFFICIENT_VALID_BLOCKS")
    Path(a.output).mkdir(parents=True,exist_ok=True); (Path(a.output)/"comparison.json").write_text(json.dumps({"schema_version":1,"runs":rows,"valid_for_comparison":bool(rows and not failures),"failure_codes":failures,"note":"MVP comparison prototype","schema_audit":_comparison_audit()},indent=2)+"\n"); return 0 if rows and not failures else EXIT_INVALID


_COMPARISON_FIELDS = ("contract", "profile", "workload", "resource_scope", "order")


def _comparison_value(manifest, field):
    aliases = {
        "contract": ("contract", "scenario_contract_hash", "scenario_contract"),
        "profile": ("resource_accounting_profile", "profile", "scenario_profile"),
        "workload": ("workload", "workload_id"),
        "resource_scope": ("resource_scope", "resource_accounting_scope"),
        "order": ("order", "order_policy", "randomization_order"),
    }
    for name in aliases[field]:
        if name in manifest:
            return manifest[name]
    return None


def _compare_plan_v1(plan, output, valid_blocks=None):
    """Validate formal A/B/C block membership and comparability metadata only."""
    reasons = list(_resource_integrity_errors())
    try:
        schema = strict_json_loads((Path(__file__).parent / "schemas/comparison-plan.schema.v1.json").read_bytes())
        reasons.extend(f"PLAN_SCHEMA:{error}" for error in _schema_errors(plan, schema))
    except (OSError, ValueError, json.JSONDecodeError):
        reasons.append("PLAN_SCHEMA_UNAVAILABLE")
    if not isinstance(plan.get("blocks"), list) or not plan["blocks"]:
        reasons.append("PLAN_BLOCKS_MISSING")
    validation_profile = plan.get("validation_profile", "standard")
    if validation_profile not in ("standard", "p0a"):
        reasons.append("VALIDATION_PROFILE_INVALID")
    expected = {field: plan.get(field) for field in _COMPARISON_FIELDS}
    reasons.extend(f"PLAN_{field.upper()}_MISSING" for field, value in expected.items()
                   if value is None)
    block_results = []
    # These fields were added for the A/B/C runner bridge.  They remain
    # optional for older plans, but are mandatory on every lane when present.
    ordering_metadata = {field: plan[field] for field in ("order_policy", "order_seed")
                         if field in plan}
    seen_paths = {}
    seen_identities = {}
    seen_run_ids = {}
    for index, block in enumerate(plan.get("blocks", []) if isinstance(plan.get("blocks"), list) else []):
        block_id = block.get("block_id", str(index + 1)) if isinstance(block, dict) else str(index + 1)
        block_reasons = []
        if not isinstance(block, dict) or not isinstance(block.get("runs"), list):
            block_results.append({"block_id": block_id, "valid": False,
                                  "reasons": ["BLOCK_RUNS_MISSING"]})
            continue
        by_lane = {}
        for entry in block["runs"]:
            if not isinstance(entry, dict) or entry.get("lane") not in ("A", "B", "C"):
                block_reasons.append("LANE_INVALID")
                continue
            lane = entry["lane"]
            if lane in by_lane:
                block_reasons.append(f"LANE_DUPLICATE:{lane}")
            else:
                by_lane[lane] = entry
        missing = sorted(set("ABC") - set(by_lane))
        if missing:
            block_reasons.append("LANES_MISSING:" + ",".join(missing))
        if len(by_lane) != 3 or len(block["runs"]) != 3:
            block_reasons.append("LANES_NOT_EXACTLY_ABC")
        if isinstance(expected["order"], list) and [entry.get("lane") for entry in block["runs"]
                if isinstance(entry, dict)] != expected["order"]:
            block_reasons.append("ORDER_SEQUENCE_MISMATCH")
        manifests = {}
        for lane, entry in by_lane.items():
            declared_path = entry.get("path")
            path = Path(declared_path) if isinstance(declared_path, str) else Path("")
            try:
                if not isinstance(declared_path, str) or not declared_path:
                    raise ValueError("RUN_PATH_MISSING")
                if _is_metric_qualification(path):
                    block_reasons.append(
                        f"METRIC_QUALIFICATION_NOT_COMPARABLE:{lane}")
                    continue
                canonical_path = path.resolve(strict=True)
                if path.absolute() != canonical_path:
                    block_reasons.append(f"PATH_SYMLINK_OR_ALIAS:{lane}")
                path_key = str(canonical_path)
                if path_key in seen_paths:
                    block_reasons.append(f"PATH_DUPLICATE:{lane}")
                    seen_paths[path_key][0].append(f"PATH_DUPLICATE:{seen_paths[path_key][1]}")
                else:
                    seen_paths[path_key] = (block_reasons, lane)
                directory_stat = canonical_path.stat()
                identity = (directory_stat.st_dev, directory_stat.st_ino)
                if identity in seen_identities:
                    block_reasons.append(f"DEVICE_INODE_DUPLICATE:{lane}")
                    seen_identities[identity][0].append(
                        f"DEVICE_INODE_DUPLICATE:{seen_identities[identity][1]}")
                else:
                    seen_identities[identity] = (block_reasons, lane)
                manifest = strict_json_loads((path / "manifest.json").read_bytes())
                if not isinstance(manifest, dict):
                    raise ValueError("manifest must be an object")
                recorded = strict_json_loads((path / "validation.json").read_bytes())
                validation = validate_dir(path, profile=validation_profile)
                if not _validation_matches_current(recorded, validation):
                    block_reasons.append(f"VALIDATION_STALE_OR_TAMPERED:{lane}")
                if validation.get("valid_for_comparison") is not True:
                    block_reasons.append(f"RUN_NOT_VALID_FOR_COMPARISON:{lane}")
                manifest_lane = manifest.get("comparison_lane", manifest.get("lane"))
                if manifest_lane != lane:
                    block_reasons.append(f"LANE_MISMATCH:{lane}")
                if manifest.get("comparison_block_id") != block_id:
                    block_reasons.append(f"BLOCK_ID_MISMATCH:{lane}")
                for field, expected_value in ordering_metadata.items():
                    if manifest.get(field) != expected_value:
                        block_reasons.append(f"{field.upper()}_MISMATCH:{lane}")
                run_id = manifest.get("run_id")
                if not isinstance(run_id, str) or not run_id:
                    block_reasons.append(f"RUN_ID_MISSING:{lane}")
                elif run_id in seen_run_ids:
                    block_reasons.append(f"RUN_ID_DUPLICATE:{lane}")
                    seen_run_ids[run_id][0].append(
                        f"RUN_ID_DUPLICATE:{seen_run_ids[run_id][1]}")
                else:
                    seen_run_ids[run_id] = (block_reasons, lane)
                manifests[lane] = manifest
            except (OSError, ValueError, json.JSONDecodeError, TypeError):
                block_reasons.append(f"RUN_INVALID:{lane}")
        values = {}
        for lane, manifest in manifests.items():
            values[lane] = {field: _comparison_value(manifest, field) for field in _COMPARISON_FIELDS}
            for field, value in values[lane].items():
                if value is None:
                    block_reasons.append(f"{field.upper()}_MISSING:{lane}")
                elif value != expected[field]:
                    block_reasons.append(f"{field.upper()}_MISMATCH:{lane}")
        for field in _COMPARISON_FIELDS:
            present = {json.dumps(values[lane][field], sort_keys=True, allow_nan=False)
                       for lane in values if values[lane][field] is not None}
            if len(present) > 1:
                block_reasons.append(f"{field.upper()}_INCONSISTENT")
        block_results.append({"block_id": block_id, "valid": not block_reasons,
                              "reasons": block_reasons,
                              "lanes": {lane: {"path": by_lane[lane].get("path"),
                                               "run_id": manifests[lane].get("run_id")}
                                        for lane in by_lane if lane in manifests}})
    for block in block_results:
        block["reasons"] = sorted(set(block["reasons"]))
        block["valid"] = not block["reasons"]
    all_reasons = sorted(set(reasons + [reason for block in block_results
                                         for reason in block["reasons"]]))
    valid_block_ids = [b["block_id"] for b in block_results if b["valid"]]
    if valid_blocks is not None and (valid_blocks < 0 or len(valid_block_ids) < valid_blocks):
        all_reasons = sorted(set(all_reasons + ["INSUFFICIENT_VALID_BLOCKS"]))
    result = {"schema": "mw_perf.comparison-result.v1", "schema_version": 1,
              "valid_for_comparison": bool(block_results) and not all_reasons
              and all(block["valid"] for block in block_results),
              "valid_blocks": valid_block_ids,
              "invalid_blocks": [b for b in block_results if not b["valid"]],
              "failure_codes": all_reasons,
              "note": "P0 gate only; no aggregate statistics or winner conclusion",
              "blocks": block_results, "schema_audit": _comparison_audit()}
    Path(output).mkdir(parents=True, exist_ok=True)
    (Path(output) / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["valid_for_comparison"] else EXIT_INVALID

def _yaml():
    try: import yaml  # type: ignore
    except ImportError: return False
    return True

if __name__ == "__main__": raise SystemExit(main())
