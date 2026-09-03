#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import mw_perf_collect.ohos_capture as ohos_capture
from mw_perf_collect.ohos_capture import CaptureSemanticError, validate_capture_semantics

AGENT = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 2 else ""
FIXTURE = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else ""
CANDIDATE = str(Path(sys.argv[3]).resolve()) if len(sys.argv) > 3 else ""
EXTENSION = str(Path(sys.argv[4]).resolve()) if len(sys.argv) > 4 else ""
TOPOLOGY = "a" * 64
CHALLENGE = "b" * 64
V8_CONTRACT = "mw_perf.p0a_contract.v8"
V8_CONTRACT_SHA = "c8f76997156ecd6e608f0d5d4a0d756d3e5748e8b21c67895936ee004a614575"
_RUN_NUMBER = 0
_ROOT_NUMBER = 0
_LAST_RUN_DIAGNOSTIC = {}
_RUN_LOCK = threading.Lock()
_USED_RUN_ROOTS = set()
_USED_CHALLENGES = set()
_CAPTURE_DIRECTORIES = set()


def new_run_root(parent, label):
    global _ROOT_NUMBER
    with _RUN_LOCK:
        number = _ROOT_NUMBER
        _ROOT_NUMBER += 1
    root = parent / f"run-{number:03d}-{label}"
    root.mkdir(mode=0o700)
    return root


def root_process_holders(root):
    prefix = str(root.resolve()) + "/"
    holders = []
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        if int(process.name) == os.getpid():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            targets = []
            for fd in (process / "fd").iterdir():
                try:
                    target = os.readlink(fd)
                except OSError:
                    continue
                if target == str(root) or target.startswith(prefix):
                    targets.append(f"fd:{fd.name}={target}")
            if targets:
                command_line = (process / "cmdline").read_bytes().replace(b"\0", b" ")
                holders.append({"pid": int(process.name),
                                "command": command_line.decode("ascii", "backslashreplace"),
                                "targets": targets})
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return holders


def root_inventory(root):
    try:
        return sorted(str(path.relative_to(root)) for path in root.rglob("*"))
    except FileNotFoundError:
        return ["<run root disappeared>"]


def command(root, executable=FIXTURE, arguments=("happy",), timeout=5000, exact=False,
            challenge=CHALLENGE):
    capability = ("dds.ordinary.nonplain.single_driver.sync_callback"
                  if exact else "cap.one")
    return [
        AGENT, "--run-root", str(root), "--run-id", "run-1",
        "--capability-id", capability, "--path-id", "path-1",
        "--request-challenge", challenge,
        "--topology-generation", "gen-1", "--topology-sha256", TOPOLOGY,
        "--trace-mode", "full", "--exact-required", "true" if exact else "false",
        "--timeout-ms", str(timeout), "--", executable, *arguments,
    ]


def v2_command(root, mode="v2", *, role="exact_anchor", capability=None,
               workload=None, contract=V8_CONTRACT, contract_sha=V8_CONTRACT_SHA,
               image_path=None, image_sha=None, extra_images=(), executable=None,
               arguments=None, extension_path=None,
               timeout=5000,
               env_literals=("CAPTURE_LITERAL=literal-value",)):
    executable = str(Path(executable or FIXTURE).resolve())
    challenge = sha(str(root).encode())
    with _RUN_LOCK:
        if challenge not in _USED_CHALLENGES:
            _USED_CHALLENGES.add(challenge)
    capability = capability or "dds.ordinary.nonplain.single_driver.sync_callback"
    workload = workload or capability
    image_path = image_path or executable
    image_sha = image_sha or sha(Path(image_path).read_bytes())
    invocation = command(root, executable=executable,
                         arguments=arguments if arguments is not None else (mode,), timeout=timeout,
                         exact=True, challenge=challenge)
    declaration = [
        "--protocol", "v2", "--role", role, "--campaign-id", "campaign-1",
        "--fault", "none", "--contract-id", contract,
        "--contract-sha256", contract_sha, "--workload-id", workload,
        "--ready-timeout-ms", "2000",
        "--completion-timeout-ms", "2000", "--runtime-image",
        f"sut|executable|exe|{image_path}|{image_sha}",
        "--runtime-image", (extra_images[0] if extra_images else
                            f"extension|runtime-map|maps|{extension_path or executable}|"
                            f"{sha(Path(extension_path or executable).read_bytes())}"),
    ]
    for image in extra_images[1:]:
        declaration.extend(("--runtime-image", image))
    for literal in env_literals:
        declaration.extend(("--env", literal))
    separator = invocation.index("--")
    return invocation[:separator] + declaration + invocation[separator:]


def invoke_one_line(invocation, env=None):
    completed = subprocess.run(invocation, text=True, capture_output=True,
                               env=env, check=False, timeout=10)
    assert len(completed.stdout.splitlines()) == 1, completed
    return completed, json.loads(completed.stdout)


def assert_v2_fail_closed(completed, result, root, started, acceptable_reasons,
                          cleanup_limit=4.0):
    assert completed.returncode != 0, result
    assert result["snapshot_valid"] is False, result
    assert result["archive_path"] == "" and result["archive_size"] == 0, result
    assert result["archive_sha256"] == "", result
    assert result["reasons"] and set(result["reasons"]) <= set(acceptable_reasons), result
    assert time.monotonic() - started < cleanup_limit, result
    assert not list(root.glob("*.tar")), root_inventory(root)
    assert not root_process_holders(root), root_process_holders(root)


def v2_acceptance_cases(run_roots):
    root = new_run_root(run_roots, "v2-success")
    completed, result = invoke_one_line(v2_command(root))
    assert completed.returncode == 0 and result["snapshot_valid"] is True, result
    assert result["protocol"] == "mw_perf.ohos_remote_capture.result.v2"
    assert set(result) == ohos_capture._RESULT_KEYS
    _, entries = inspect_ustar(Path(result["archive_path"]))
    manifest = json.loads(entries[0][1])
    assert manifest["protocol"] == "mw_perf.ohos_remote_capture.v2"
    assert manifest["capture_role"] == "exact_anchor" and manifest["campaign_id"] == "campaign-1"
    assert manifest["registration_contract"] == {"id": V8_CONTRACT, "sha256": V8_CONTRACT_SHA}
    payload = dict(entries[1:])
    completion = json.loads(payload["device_completion.json"])
    assert completion["complete"] is True
    attestation = json.loads(payload["device_runtime_attestation.json"])
    assert attestation["request_challenge"] == result["request_challenge"]
    assert attestation["child_pid"] == result["child_pid"]
    assert attestation["child_start_time_ticks"] == result["child_start_time_ticks"]
    assert attestation["boot_id"] == manifest["boot_id"]
    assert attestation["live_images"][0]["matched"] is True

    request = {
        "protocol": "mw_perf.ohos_remote_capture.request.v2", "capture_role": "exact_anchor",
        "campaign_id": "campaign-1", "fault": "none",
        "workload_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "registration_contract": {"id": V8_CONTRACT, "sha256": V8_CONTRACT_SHA},
        "runtime_images": manifest["runtime_images"], "ready_timeout_ms": 2000,
        "completion_timeout_ms": 2000, "argv": [str(Path(FIXTURE).resolve()), "v2"],
        "capability_id": "dds.ordinary.nonplain.single_driver.sync_callback",
        "env": ["CAPTURE_LITERAL=literal-value"],
        "exact_required": True, "path_id": "path-1", "run_id": "run-1",
        "run_root": str(root), "timeout_ms": 5000, "topology_generation": "gen-1",
        "topology_sha256": TOPOLOGY, "trace_mode": "full",
        "request_challenge": sha(str(root).encode()),
    }
    assert manifest["request_sha256"] == sha(json.dumps(
        request, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii"))

    root = new_run_root(run_roots, "v2-exec-after-g")
    completed, result = invoke_one_line(v2_command(root, "v2-exec-after-g"))
    assert completed.returncode != 0 and result["snapshot_valid"] is False, result
    if "completion_missing_or_malformed" not in result["reasons"]:
        assert "runtime_executable_mismatch" in result["reasons"], result

    root = new_run_root(run_roots, "v2-missing-completion")
    completed, result = invoke_one_line(v2_command(root, "v2-no-completion"))
    assert completed.returncode != 0 and "completion_missing_or_malformed" in result["reasons"]

    root = new_run_root(run_roots, "v2-descendant-retains-control")
    started = time.monotonic()
    completed, result = invoke_one_line(v2_command(root, "v2-descendant-retains-control"))
    assert_v2_fail_closed(completed, result, root, started, {
        "handshake_missing", "completion_missing_or_malformed", "timeout", "sigkill_escalation",
        "child_not_zero_exit", "descendant_detected", "device_attestation_write_failed",
    })

    root = new_run_root(run_roots, "v2-zero-packet-immediate-close")
    started = time.monotonic()
    completed, result = invoke_one_line(v2_command(root, "v2-zero-packet-immediate-close"))
    assert_v2_fail_closed(completed, result, root, started, {
        "completion_missing_or_malformed", "device_attestation_write_failed",
    })
    assert result["reasons"] == [
        "completion_missing_or_malformed", "device_attestation_write_failed"], result

    for mode, reason in (("v2-descendant-ready", "handshake_sender_identity_mismatch"),
                         ("v2-descendant-completion", "completion_sender_identity_mismatch")):
        root = new_run_root(run_roots, mode)
        completed, result = invoke_one_line(v2_command(root, mode))
        assert completed.returncode != 0 and reason in result["reasons"], result

    root = new_run_root(run_roots, "v2-tiny-overall-timeout")
    started = time.monotonic()
    completed, result = invoke_one_line(v2_command(root, "v2-delayed-ready", timeout=5))
    assert completed.returncode != 0, result
    assert time.monotonic() - started < 1.0, result

    root = new_run_root(run_roots, "v2-runtime-hash-deadline")
    injected = os.environ.copy()
    injected["IBMW_CAPTURE_INJECT_RUNTIME_HASH_DELAY_MS"] = "1000"
    started = time.monotonic()
    completed, result = invoke_one_line(v2_command(root, timeout=500), env=injected)
    assert_v2_fail_closed(completed, result, root, started, {
        "runtime_executable_mismatch", "runtime_maps_mismatch", "timeout",
        "child_not_zero_exit", "sigkill_escalation", "device_attestation_write_failed",
        "agent_hash_failed",
    }, cleanup_limit=2.0)
    assert "runtime_executable_mismatch" in result["reasons"], result
    assert "timeout" in result["reasons"], result

    root = new_run_root(run_roots, "v2-hash-mismatch")
    completed, result = invoke_one_line(v2_command(root, image_sha="0" * 64))
    assert completed.returncode != 0 and "prelaunch_image_hash_mismatch" in result["reasons"]

    root = new_run_root(run_roots, "v2-path-mismatch")
    completed, result = invoke_one_line(v2_command(
        root, image_path="/bin/true", image_sha=sha(Path("/bin/true").read_bytes())))
    assert completed.returncode == 2 and result["reasons"] == ["runtime_executable_path_mismatch"]

    root = new_run_root(run_roots, "v2-argv-image-mismatch")
    other = Path("/bin/true").resolve()
    completed, result = invoke_one_line(v2_command(
        root, executable=FIXTURE, image_path=str(other), image_sha=sha(other.read_bytes())))
    assert completed.returncode == 2 and result["reasons"] == ["runtime_executable_path_mismatch"]

    root = new_run_root(run_roots, "v2-role-name-collision")
    executable = str(Path(FIXTURE).resolve())
    collision = f"sut|executable|exe|{executable}|{sha(Path(executable).read_bytes())}"
    completed, result = invoke_one_line(v2_command(root, extra_images=(collision,)))
    assert completed.returncode == 2 and result["reasons"] == ["runtime_image_collision"]

    root = new_run_root(run_roots, "v2-map-missing")
    unrelated = Path("/bin/true").resolve()
    missing_map = f"extension|missing|maps|{unrelated}|{sha(unrelated.read_bytes())}"
    completed, result = invoke_one_line(v2_command(root, extra_images=(missing_map,)))
    assert completed.returncode != 0 and "runtime_maps_mismatch" in result["reasons"]

    malformed = (
        ("role", "fault_runtime_matrix", "unsupported_role"),
        ("contract", "mw_perf.p0a_contract.v9", "v2_contract_binding_mismatch"),
        ("workload", "wrong.workload", "v2_contract_binding_mismatch"),
    )
    for label, value, reason in malformed:
        root = new_run_root(run_roots, f"v2-{label}")
        changes = {label: value}
        completed, result = invoke_one_line(v2_command(root, **changes))  # type: ignore[arg-type]
        assert completed.returncode != 0 and reason in result["reasons"], result

    root = new_run_root(run_roots, "v2-image-collision")
    executable = str(Path(FIXTURE).resolve())
    duplicate = f"sut|executable|exe|{executable}|{sha(Path(executable).read_bytes())}"
    completed, result = invoke_one_line(v2_command(root, extra_images=(duplicate,)))
    assert completed.returncode != 0 and "runtime_image_collision" in result["reasons"]

    root = new_run_root(run_roots, "v2-archive-collision")
    archive = root / f"capture-{sha(str(root).encode())}.tar"
    sentinel = b"v2 collision sentinel\n"
    archive.write_bytes(sentinel)
    archive.chmod(0o600)
    completed, result = invoke_one_line(v2_command(root))
    assert completed.returncode != 0 and "archive_publish_failed" in result["reasons"]
    assert archive.read_bytes() == sentinel

    v2_only = (
        ("--role", "exact_anchor"), ("--campaign-id", "campaign-1"),
        ("--fault", "none"), ("--contract-id", V8_CONTRACT),
        ("--contract-sha256", V8_CONTRACT_SHA),
        ("--workload-id", "dds.ordinary.nonplain.single_driver.sync_callback"),
        ("--ready-timeout-ms", "1000"), ("--completion-timeout-ms", "1000"),
        ("--runtime-image", f"sut|fixture|exe|{FIXTURE}|{sha(Path(FIXTURE).read_bytes())}"),
    )
    parse_root = run_roots
    for explicit_protocol in ((), ("--protocol", "v1")):
        for option in v2_only:
            invocation = command(parse_root)
            separator = invocation.index("--")
            invocation[separator:separator] = [*explicit_protocol, *option]
            completed, result = invoke_one_line(invocation)
            assert completed.returncode == 2 and result["reasons"] == ["unknown_option"], result
        invocation = command(parse_root)
        separator = invocation.index("--")
        invocation[separator:separator] = [*explicit_protocol, "--ready-timeout-ms", "invalid"]
        completed, result = invoke_one_line(invocation)
        assert completed.returncode == 2 and result["reasons"] == ["unknown_option"], result

    if CANDIDATE and EXTENSION:
        root = new_run_root(run_roots, "v2-genuine-candidate")
        completed, result = invoke_one_line(v2_command(
            root, executable=CANDIDATE, arguments=("--exit-after-complete",),
            extension_path=EXTENSION, env_literals=(), timeout=30000))
        assert completed.returncode == 0 and result["snapshot_valid"] is True, result
        _, entries = inspect_ustar(Path(result["archive_path"]))
        manifest = json.loads(entries[0][1])
        assert manifest["completion_attested"] is True
        assert manifest["evidence_released"] is True
        assert manifest["completion_images"]


def run(root, mode="happy", timeout=5000, env=None, arguments=None, exact=False,
        challenge=CHALLENGE, executable=FIXTURE):
    global _RUN_NUMBER
    resolved_root = str(root.resolve())
    with _RUN_LOCK:
        assert resolved_root not in _USED_RUN_ROOTS, ("reused run root", resolved_root)
        _USED_RUN_ROOTS.add(resolved_root)
        if challenge == CHALLENGE and _RUN_NUMBER:
            challenge = f"{_RUN_NUMBER:064x}"
        _RUN_NUMBER += 1
        assert challenge not in _USED_CHALLENGES, ("reused challenge", challenge)
        _USED_CHALLENGES.add(challenge)
    invocation = command(root, executable=executable,
                         arguments=arguments if arguments is not None else (mode,),
                         timeout=timeout, exact=exact, challenge=challenge)
    started = time.monotonic()
    try:
        completed = subprocess.run(invocation, text=True, capture_output=True,
                                   env=env, check=False, timeout=10)
    except subprocess.TimeoutExpired as error:
        diagnostic = {"command": invocation, "elapsed_seconds": time.monotonic() - started,
                      "run_root": str(root), "inventory": root_inventory(root),
                      "process_holders": root_process_holders(root),
                      "stdout": error.stdout, "stderr": error.stderr}
        raise AssertionError(("capture agent subprocess timeout", diagnostic)) from error
    diagnostic = {"command": invocation, "elapsed_seconds": time.monotonic() - started,
                  "returncode": completed.returncode, "run_root": str(root),
                  "inventory": root_inventory(root),
                  "process_holders": root_process_holders(root),
                  "stdout": completed.stdout, "stderr": completed.stderr}
    _LAST_RUN_DIAGNOSTIC[str(root)] = diagnostic
    lines = completed.stdout.splitlines()
    assert len(lines) == 1, diagnostic
    try:
        result = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise AssertionError(("invalid capture agent protocol JSON", diagnostic)) from error
    diagnostic["result"] = result
    capture_directories = [path for path in root.iterdir()
                           if path.name.startswith("capture-") and path.is_dir()]
    assert len(capture_directories) == 1, diagnostic
    capture_identity = (str(capture_directories[0].resolve()),
                        capture_directories[0].stat().st_dev,
                        capture_directories[0].stat().st_ino)
    with _RUN_LOCK:
        assert capture_identity not in _CAPTURE_DIRECTORIES, diagnostic
        _CAPTURE_DIRECTORIES.add(capture_identity)
    assert not diagnostic["process_holders"], diagnostic
    return completed.returncode, result, completed.stderr, completed.stdout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def octal(field):
    return int(field.rstrip(b"\0 ") or b"0", 8)


def inspect_ustar(path):
    data = path.read_bytes()
    assert len(data) % 512 == 0
    assert data[-1024:] == b"\0" * 1024
    entries = []
    offset = 0
    while data[offset:offset + 512] != b"\0" * 512:
        header = data[offset:offset + 512]
        stored = octal(header[148:156])
        checksum_header = bytearray(header)
        checksum_header[148:156] = b" " * 8
        assert stored == sum(checksum_header)
        assert header[257:263] == b"ustar\0"
        assert header[263:265] == b"00"
        assert octal(header[100:108]) == 0o600
        assert octal(header[108:116]) == 0
        assert octal(header[116:124]) == 0
        assert octal(header[136:148]) == 0
        assert header[156:157] == b"0"
        name = header[:100].split(b"\0", 1)[0].decode("ascii")
        size = octal(header[124:136])
        start = offset + 512
        content = data[start:start + size]
        padding_end = start + ((size + 511) // 512) * 512
        assert data[start + size:padding_end] == b"\0" * (padding_end - start - size)
        entries.append((name, content))
        offset = padding_end
    assert data[offset:] == b"\0" * (len(data) - offset)
    return data, entries


def make_ustar(entries, *, typeflags=None):
    output = bytearray()
    typeflags = typeflags or {}
    for name, content in entries:
        header = bytearray(512)
        encoded = name.encode("ascii")
        header[:len(encoded)] = encoded
        for offset, width, value in ((100, 8, 0o600), (108, 8, 0), (116, 8, 0),
                                     (124, 12, len(content)), (136, 12, 0)):
            header[offset:offset + width] = f"{value:0{width - 1}o}\0".encode()
        header[148:156] = b" " * 8
        header[156] = ord(typeflags.get(name, "0"))
        header[257:265] = b"ustar\0" + b"00"
        checksum = sum(header)
        header[148:156] = f"{checksum:06o}\0 ".encode()
        output.extend(header)
        output.extend(content)
        output.extend(b"\0" * ((512 - len(content) % 512) % 512))
    output.extend(b"\0" * 1024)
    return bytes(output)


def write_attack_archive(root, result, name, data):
    path = root / name
    path.write_bytes(data)
    path.chmod(0o600)
    return path, dict(result, archive_path=str(path), archive_size=len(data),
                      archive_sha256=sha(data))


def expected_request(root, arguments=("valid-trace",), timeout=5000, exact=False,
                     challenge=CHALLENGE, executable=FIXTURE):
    capability = ("dds.ordinary.nonplain.single_driver.sync_callback"
                  if exact else "cap.one")
    return {
        "argv": [executable, *arguments], "capability_id": capability, "env": [],
        "exact_required": exact, "path_id": "path-1", "run_id": "run-1",
        "request_challenge": challenge,
        "run_root": str(root), "timeout_ms": timeout,
        "topology_generation": "gen-1", "topology_sha256": TOPOLOGY,
        "trace_mode": "full",
    }


def canonical_request(root):
    return json.dumps(expected_request(root), separators=(",", ":"),
                      sort_keys=True).encode()


def verify_happy(root, result):
    assert result["protocol"] == "mw_perf.ohos_remote_capture.result.v1"
    assert result["snapshot_valid"] is True
    assert result["semantic_validation"] is False
    assert result["child_pid"] > 1
    assert result["child_start_time_ticks"] > 0
    archive = Path(result["archive_path"])
    assert archive.parent == root
    assert stat.S_IMODE(archive.stat().st_mode) == 0o600
    data, entries = inspect_ustar(archive)
    assert len(data) <= 64 * 1024 * 1024
    assert sha(data) == result["archive_sha256"]
    assert len(data) == result["archive_size"]
    assert entries[0][0] == "device_capture_manifest.json"
    assert [name for name, _ in entries[1:]] == sorted(name for name, _ in entries[1:])
    manifest = json.loads(entries[0][1])
    assert manifest["snapshot_valid"] is True
    assert manifest["semantic_validation"] is False
    assert manifest["descendant_policy"] == "forbid"
    assert manifest["descendant_detected"] is False
    assert manifest["child_pid"] == result["child_pid"]
    assert manifest["child_start_time_ticks"] == result["child_start_time_ticks"]
    assert result["request_challenge"] == CHALLENGE
    assert manifest["request_challenge"] == CHALLENGE
    assert archive.name == f"capture-{CHALLENGE}.tar"
    assert manifest["request_sha256"] == sha(canonical_request(root))
    assert manifest["agent_executable_sha256"] == sha(Path(AGENT).read_bytes())
    payload = dict(entries[1:])
    source = manifest["source_files"]
    assert len(source) == len(payload) == 3
    for item in source:
        assert item["size"] == len(payload[item["name"]])
        assert item["sha256"] == sha(payload[item["name"]])
    tree = b"".join(
        item["name"].encode() + b"\0" + str(item["size"]).encode() + b"\0" +
        item["sha256"].encode() + b"\n" for item in source)
    assert manifest["archive_tree_sha256"] == sha(tree)
    metadata = json.loads(payload["ibmw_dds_trace.1.a.meta.json"])
    assert metadata["producer_pid"] == result["child_pid"]
    assert metadata["producer_start_time_ticks"] == result["child_start_time_ticks"]
    assert metadata["run_id"] == "run-1"
    receive_root = root.parent / "host-receive"
    receive_root.mkdir(mode=0o700)
    received_archive = receive_root / archive.name
    shutil.copyfile(archive, received_archive)
    received_archive.chmod(0o600)
    assert received_archive.parent != Path(result["archive_path"]).parent
    imported = validate_capture_semantics(received_archive, result, expected_request(root))
    assert imported["semantic_validation"] is True
    assert imported["formal_p0a"] is False
    assert imported["imported_semantics"]["record_count"] == 1
    return received_archive


def installed_cli_cases(temporary, root, archive, result):
    repository = Path(__file__).resolve().parents[3]
    project = Path(temporary) / "install-project"
    project.mkdir()
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(ibmw LANGUAGES NONE)\n"
        "include(GNUInstallDirs)\n"
        "set(IBMW_INSTALL ON)\n"
        f'add_subdirectory("{repository / "tools/mw_perf_collect"}" collector)\n',
        encoding="ascii")
    build = Path(temporary) / "install-build"
    prefix = Path(temporary) / "install-prefix"
    subprocess.run(["cmake", "-S", str(project), "-B", str(build)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix),
                    "--component", "mw_perf_collect"], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    executable = prefix / "bin/mw-perf-collect"
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("PYTHONPATH", "PYTHONHOME")}
    help_result = subprocess.run([str(executable), "import-ohos-capture", "--help"],
                                 env=environment, capture_output=True, text=True, check=False)
    assert help_result.returncode == 0 and "--archive" in help_result.stdout

    request_path = Path(temporary) / "request.json"
    result_path = Path(temporary) / "result.json"
    request_path.write_text(json.dumps(expected_request(root)), encoding="ascii")
    result_path.write_text(json.dumps(result), encoding="ascii")

    def invoke(result_file=result_path, request_file=request_path, archive_file=archive,
               output=None):
        arguments = [str(executable), "import-ohos-capture", "--archive", str(archive_file),
                     "--result", str(result_file), "--request", str(request_file)]
        if output is not None:
            arguments.extend(("--output", str(output)))
        completed = subprocess.run(arguments, env=environment, capture_output=True,
                                   text=True, check=False)
        lines = completed.stdout.splitlines()
        assert len(lines) <= 1, completed.stdout
        return completed, json.loads(lines[0]) if lines else None

    completed, document = invoke()
    assert completed.returncode == 0, (completed.stdout, completed.stderr)
    assert document is not None
    assert document["semantic_validation"] is True and document["formal_p0a"] is False
    assert document["counts"]["stage_records"] == 1
    encoded = json.dumps(document)
    assert "events" not in encoded and "directory_finalization" not in encoded
    assert "valid_for_metric" not in encoded and "valid_for_comparison" not in encoded

    invalid_root = Path(temporary) / "invalid-archive"
    invalid_root.mkdir()
    invalid_archive = invalid_root / archive.name
    invalid_archive.write_bytes(archive.read_bytes()[:-1] + b"x")
    invalid_archive.chmod(0o600)
    completed, failure = invoke(archive_file=invalid_archive)
    assert failure is not None
    assert completed.returncode != 0 and failure["error_code"] == "OHOS_CAPTURE_IMPORT_INVALID"

    mismatch = dict(expected_request(root), run_id="wrong-run")
    mismatch_path = Path(temporary) / "mismatch-request.json"
    mismatch_path.write_text(json.dumps(mismatch), encoding="ascii")
    completed, failure = invoke(request_file=mismatch_path)
    assert failure is not None
    assert completed.returncode != 0 and failure["error_code"] == "OHOS_CAPTURE_IMPORT_INVALID"

    duplicate_path = Path(temporary) / "duplicate-result.json"
    duplicate_path.write_text('{"protocol":"one","protocol":"two"}', encoding="ascii")
    completed, failure = invoke(result_file=duplicate_path)
    assert failure is not None
    assert completed.returncode != 0 and failure["error_code"] == "OHOS_CAPTURE_IMPORT_INVALID"

    symlink_path = Path(temporary) / "result-link.json"
    symlink_path.symlink_to(result_path)
    completed, failure = invoke(result_file=symlink_path)
    assert failure is not None
    assert completed.returncode != 0 and failure["error_code"] == "OHOS_CAPTURE_IMPORT_INVALID"

    output = Path(temporary) / "import-summary.json"
    completed, document = invoke(output=output)
    assert completed.returncode == 0 and document is None
    assert stat.S_IMODE(output.stat().st_mode) == 0o600
    assert json.loads(output.read_text(encoding="ascii"))["semantic_validation"] is True
    completed, failure = invoke(output=output)
    assert failure is not None
    assert completed.returncode != 0 and failure["error_code"] == "OHOS_CAPTURE_OUTPUT_EXISTS"


def assert_failure(root, mode, reason, timeout=1000):
    code, result, _, _ = run(root, mode, timeout)
    assert code != 0
    assert result["snapshot_valid"] is False
    assert reason in result["reasons"], result


def archive_collision_case(run_roots):
    root = new_run_root(run_roots, "archive-collision")
    challenge = "8" * 64
    with _RUN_LOCK:
        assert challenge not in _USED_CHALLENGES, ("reused challenge", challenge)
        _USED_CHALLENGES.add(challenge)
    archive = root / f"capture-{challenge}.tar"
    sentinel = b"pre-existing archive must survive\n"
    archive.write_bytes(sentinel)
    archive.chmod(0o600)
    completed = subprocess.run(
        command(root, challenge=challenge), text=True, capture_output=True,
        check=False, timeout=10)
    result = json.loads(completed.stdout)
    assert completed.returncode != 0 and "archive_publish_failed" in result["reasons"], (
        completed.stdout, completed.stderr)
    assert archive.read_bytes() == sentinel
    assert not root_process_holders(root), root_process_holders(root)
    capture_directories = [path for path in root.iterdir()
                           if path.name.startswith("capture-") and path.is_dir()]
    assert len(capture_directories) == 1, root_inventory(root)


def mutation_case(root):
    holder = {}
    def invoke():
        holder["result"] = run(root, "mutable")
    thread = threading.Thread(target=invoke)
    thread.start()
    deadline = time.monotonic() + 5
    touched = False
    while time.monotonic() < deadline and thread.is_alive():
        for path in root.glob("capture-*/ibmw_dds_trace.1.a.00000000.ibct"):
            with path.open("r+b", buffering=0) as file:
                file.write(b"b")
                touched = True
        if touched:
            time.sleep(0.002)
        else:
            time.sleep(0.001)
    thread.join()
    code, result, _, _ = holder["result"]
    assert touched and code != 0
    assert {"entry_mutated", "entries_changed_between_scans"} & set(result["reasons"])


def rejected(action):
    try:
        action()
    except CaptureSemanticError:
        return
    raise AssertionError("malformed remote capture passed host import validation")


def memory_error_cases(root, result):
    archive = Path(result["archive_path"])
    request = expected_request(root)
    for target in ("_read_bound_archive", "_materialize_and_validate"):
        original = getattr(ohos_capture, target)
        def out_of_memory(*_args, **_kwargs):
            raise MemoryError(f"injected at {target}")
        setattr(ohos_capture, target, out_of_memory)
        try:
            try:
                validate_capture_semantics(archive, result, request)
            except CaptureSemanticError as error:
                assert str(error) == "imported trace exceeds memory budget"
                assert isinstance(error.__cause__, MemoryError)
            else:
                raise AssertionError(f"MemoryError escaped semantic boundary at {target}")
        finally:
            setattr(ohos_capture, target, original)


def path_binding_cases(root, local_archive, result):
    request = expected_request(root)
    basename = local_archive.name

    wrong_root = dict(result, archive_path=f"/wrong/device/root/{basename}")
    rejected(lambda: validate_capture_semantics(local_archive, wrong_root, request))

    wrong_device_basename = dict(result, archive_path=f"{root}/wrong-name.tar")
    rejected(lambda: validate_capture_semantics(
        local_archive, wrong_device_basename, request))

    traversing_result = dict(result, archive_path=f"{root}/device-child/../{basename}")
    rejected(lambda: validate_capture_semantics(local_archive, traversing_result, request))

    traversing_request = dict(request, run_root=f"{root}/device-child/..")
    rejected(lambda: validate_capture_semantics(local_archive, result, traversing_request))

    nested_result = dict(result, archive_path=f"{root}/nested/{basename}")
    rejected(lambda: validate_capture_semantics(local_archive, nested_result, request))

    wrong_basename = local_archive.with_name("wrong-name.tar")
    shutil.copyfile(local_archive, wrong_basename)
    wrong_basename.chmod(0o600)
    rejected(lambda: validate_capture_semantics(wrong_basename, result, request))

    local_child = local_archive.parent / "local-child"
    local_child.mkdir(mode=0o700)
    traversing_local = f"{local_child}/../{basename}"
    rejected(lambda: validate_capture_semantics(traversing_local, result, request))

    symlink_root = local_archive.parent / "symlink-receive"
    symlink_root.mkdir(mode=0o700)
    symlink_archive = symlink_root / basename
    symlink_archive.symlink_to(local_archive)
    rejected(lambda: validate_capture_semantics(symlink_archive, result, request))


def host_import_attack_cases(root, result):
    archive = Path(result["archive_path"])
    request = expected_request(root)
    bad_result = dict(result)
    bad_result["archive_size"] += 1
    rejected(lambda: validate_capture_semantics(archive, bad_result, request))
    bad_result = dict(result)
    bad_result["archive_sha256"] = "0" * 64
    rejected(lambda: validate_capture_semantics(archive, bad_result, request))

    original_read = ohos_capture.os.read
    replacement = archive.with_suffix(".replacement")
    replacement.write_bytes(archive.read_bytes())
    replacement.chmod(0o600)
    replaced = False
    def replacing_read(fd, size):
        nonlocal replaced
        value = original_read(fd, size)
        if value and not replaced:
            os.replace(replacement, archive)
            replaced = True
        return value
    ohos_capture.os.read = replacing_read
    try:
        rejected(lambda: validate_capture_semantics(archive, result, request))
    finally:
        ohos_capture.os.read = original_read

    data, entries = inspect_ustar(archive)
    for tar_format in (tarfile.GNU_FORMAT, tarfile.PAX_FORMAT):
        malformed = root / f"malformed-{tar_format}.tar"
        with tarfile.open(malformed, "w", format=tar_format) as output:
            info = tarfile.TarInfo("device_capture_manifest.json")
            info.size = 2
            with tempfile.TemporaryFile() as source:
                source.write(b"{}")
                source.seek(0)
                output.addfile(info, source)
        malformed.chmod(0o600)
        malformed_result = dict(result, archive_path=str(malformed),
                                archive_size=malformed.stat().st_size,
                                archive_sha256=sha(malformed.read_bytes()))
        rejected(lambda r=malformed_result, p=malformed:
                 validate_capture_semantics(p, r, request))

    duplicate = bytearray(data)
    first_size = octal(duplicate[124:136])
    second_offset = 512 + ((first_size + 511) // 512) * 512
    duplicate[second_offset:second_offset + 100] = duplicate[:100]
    checksum_header = bytearray(duplicate[second_offset:second_offset + 512])
    checksum_header[148:156] = b" " * 8
    checksum = sum(checksum_header)
    duplicate[second_offset + 148:second_offset + 156] = f"{checksum:06o}\0 ".encode()
    duplicate_path = root / "duplicate.tar"
    duplicate_path.write_bytes(duplicate)
    duplicate_path.chmod(0o600)
    duplicate_result = dict(result, archive_path=str(duplicate_path),
                            archive_size=len(duplicate), archive_sha256=sha(duplicate))
    rejected(lambda: validate_capture_semantics(duplicate_path, duplicate_result, request))

    sparse = make_ustar(entries, typeflags={entries[1][0]: "S"})
    sparse_path, sparse_result = write_attack_archive(root, result, "sparse.tar", sparse)
    rejected(lambda: validate_capture_semantics(sparse_path, sparse_result, request))

    manifest = json.loads(entries[0][1])
    for label, change in (
            ("manifest-protocol", lambda value: value.update(protocol="wrong")),
            ("tree", lambda value: value.update(archive_tree_sha256="0" * 64)),
            ("request", lambda value: value.update(request_sha256="0" * 64)),
            ("topology", lambda value: value.update(topology_generation="wrong")),
            ("lifecycle", lambda value: value.update(graceful=False))):
        changed = json.loads(json.dumps(manifest))
        change(changed)
        changed_entries = [(entries[0][0], json.dumps(changed, separators=(",", ":")).encode()),
                           *entries[1:]]
        attack = make_ustar(changed_entries)
        attack_path, attack_result = write_attack_archive(
            root, result, f"{label}.tar", attack)
        rejected(lambda p=attack_path, r=attack_result:
                 validate_capture_semantics(p, r, request))

    changed = json.loads(json.dumps(manifest))
    changed["source_files"][0]["name"] = changed["source_files"][1]["name"]
    inventory_data = make_ustar([
        (entries[0][0], json.dumps(changed, separators=(",", ":")).encode()), *entries[1:]])
    inventory_path, inventory_result = write_attack_archive(
        root, result, "inventory.tar", inventory_data)
    rejected(lambda: validate_capture_semantics(inventory_path, inventory_result, request))

    protocol_result = dict(result, protocol="wrong")
    rejected(lambda: validate_capture_semantics(archive, protocol_result, request))

    aggregate_result = dict(result, archive_size=ohos_capture.MAX_ARCHIVE_BYTES + 1)
    rejected(lambda: validate_capture_semantics(archive, aggregate_result, request))
    rejected(lambda: validate_capture_semantics(archive, dict(result, request_challenge="c" * 64), request))
    renamed = root / "renamed.tar"
    renamed.write_bytes(archive.read_bytes())
    renamed.chmod(0o600)
    rejected(lambda: validate_capture_semantics(renamed, dict(result, archive_path=str(renamed)), request))


def candidate_lifecycle_cases(run_roots):
    waiting_challenge = "e" * 64
    root = new_run_root(run_roots, "candidate-timeout")
    code, result, stderr, stdout = run(
        root, timeout=3000, exact=True, challenge=waiting_challenge,
        arguments=(), executable=CANDIDATE)
    assert code != 0 and "timeout" in result["reasons"]
    assert "sigkill_escalation" not in result["reasons"]
    assert "child_not_zero_exit" not in result["reasons"]
    assert stdout.count("\n") == 1 and stderr == "", (stdout, stderr, result)

    completion_challenge = "f" * 64
    root = new_run_root(run_roots, "candidate-completion")
    code, result, stderr, stdout = run(
        root, timeout=30000, exact=True, challenge=completion_challenge,
        arguments=("--exit-after-complete",), executable=CANDIDATE)
    assert code == 0 and result["snapshot_valid"] is True, result
    assert result["reasons"] == [] and stderr == "" and stdout.count("\n") == 1
    _, entries = inspect_ustar(Path(result["archive_path"]))
    manifest = json.loads(entries[0][1])
    names = {name for name, _ in entries[1:]}
    assert manifest["graceful"] is True and manifest["no_escalation"] is True
    assert any(name.endswith(".seal.json") for name in names)
    assert any(name.endswith(".exact.seal.json") for name in names)
    imported = validate_capture_semantics(
        result["archive_path"], result,
        expected_request(root, arguments=("--exit-after-complete",),
                         timeout=30000, exact=True,
                         challenge=completion_challenge, executable=CANDIDATE))
    assert imported["semantic_validation"] is True
    assert imported["imported_semantics"]["measurement_kind"] == "exact_correlated"


def main():
    if not AGENT or not FIXTURE:
        raise SystemExit("usage: capture_acceptance_test.py AGENT FIXTURE")
    assert sha(b"abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    with tempfile.TemporaryDirectory(prefix="mw-perf-native-") as temporary:
        run_roots = Path(temporary) / "runs"
        run_roots.mkdir(mode=0o700)

        root = new_run_root(run_roots, "valid-trace")
        code, result, _, _ = run(root, "valid-trace", timeout=5000)
        assert code == 0, result
        received_archive = verify_happy(root, result)
        installed_cli_cases(temporary, root, received_archive, result)
        path_binding_cases(root, received_archive, result)
        memory_error_cases(root, result)
        host_import_attack_cases(root, result)

        exact_challenge = "c" * 64
        root = new_run_root(run_roots, "exact-trace")
        code, exact_result, _, _ = run(
            root, "valid-trace", timeout=5000, exact=True, challenge=exact_challenge)
        assert code == 0, exact_result
        imported = validate_capture_semantics(
            exact_result["archive_path"], exact_result,
            expected_request(root, timeout=5000, exact=True, challenge=exact_challenge))
        assert imported["semantic_validation"] is True
        assert imported["imported_semantics"]["measurement_kind"] == "exact_correlated"

        bad = Path(temporary) / "bad-executable"
        bad.write_text("not an executable format\n", encoding="ascii")
        bad.chmod(0o700)
        root = new_run_root(run_roots, "exec-failed")
        with _RUN_LOCK:
            assert "d" * 64 not in _USED_CHALLENGES
            _USED_CHALLENGES.add("d" * 64)
        completed = subprocess.run(command(root, str(bad), (), challenge="d" * 64), text=True,
                                   capture_output=True, check=False)
        parsed = json.loads(completed.stdout)
        assert completed.returncode != 0 and "exec_failed" in parsed["reasons"]
        assert not root_process_holders(root), root_process_holders(root)
        archive_collision_case(run_roots)
        v2_acceptance_cases(run_roots)

        injected = os.environ.copy()
        injected["IBMW_CAPTURE_INJECT_FORK_FAILURE"] = "1"
        root = new_run_root(run_roots, "fork-failed")
        code, result, _, _ = run(root, env=injected)
        assert code != 0 and "fork_failed" in result["reasons"]

        root = new_run_root(run_roots, "nonzero")
        assert_failure(root, "nonzero", "child_not_zero_exit")
        root = new_run_root(run_roots, "timeout")
        assert_failure(root, "sleep", "timeout", 50)
        root = new_run_root(run_roots, "sigkill")
        assert_failure(root, "ignore", "sigkill_escalation", 250)
        root = new_run_root(run_roots, "descendant")
        assert_failure(root, "descendant", "descendant_detected")
        started = time.monotonic()
        root = new_run_root(run_roots, "escaped-descendant")
        assert_failure(root, "escaped-descendant", "descendant_detected")
        assert time.monotonic() - started < 3
        for mode, reason in (("symlink", "symlink"), ("hardlink", "hardlink"),
                             ("fifo", "special_file"), ("part", "part_file"),
                             ("unknown", "invalid_filename"),
                             ("writable", "writable_by_group_or_world"),
                             ("oversize", "source_size_limit")):
            root = new_run_root(run_roots, mode)
            assert_failure(root, mode, reason)
        root = new_run_root(run_roots, "mutation")
        mutation_case(root)

        marker = Path("/tmp/mw_perf_shell_was_run")
        marker.unlink(missing_ok=True)
        value = "$(touch /tmp/mw_perf_shell_was_run); x"
        root = new_run_root(run_roots, "metachar")
        code, result, _, _ = run(root, arguments=("metachar", value))
        assert code == 0 and result["snapshot_valid"] and not marker.exists()
        root = new_run_root(run_roots, "stdio")
        code, result, stderr, stdout = run(root, "stdio")
        assert code == 0 and result["snapshot_valid"], _LAST_RUN_DIAGNOSTIC[str(root)]
        assert stdout.endswith("\n") and stdout.count("\n") == 1
        assert "child-stdout-must-not-enter-protocol" not in stdout
        assert "child-stderr-must-not-enter-protocol" not in stdout
        assert "child-stdout-must-not-enter-protocol" not in stderr
        assert "child-stderr-must-not-enter-protocol" not in stderr
        root = new_run_root(run_roots, "fd-check")
        code, result, _, _ = run(root, "fd-check")
        assert code == 0 and result["snapshot_valid"]
        root = new_run_root(run_roots, "invalid-json")
        code, result, _, _ = run(root, "invalid-json")
        assert code == 0 and result["snapshot_valid"]
        rejected(lambda: validate_capture_semantics(
            result["archive_path"], result, expected_request(root, ("invalid-json",))))
        root = new_run_root(run_roots, "happy-invalid-semantics")
        code, result, _, _ = run(root, "happy")
        assert code == 0 and result["snapshot_valid"]
        rejected(lambda: validate_capture_semantics(
            result["archive_path"], result, expected_request(root, ("happy",))))

        if CANDIDATE:
            candidate_lifecycle_cases(run_roots)

        symlink_root = Path(temporary) / "root-link"
        symlink_root.symlink_to(root)
        with _RUN_LOCK:
            assert "9" * 64 not in _USED_CHALLENGES
            _USED_CHALLENGES.add("9" * 64)
        completed = subprocess.run(command(symlink_root, challenge="9" * 64), text=True,
                                   capture_output=True, check=False)
        parsed = json.loads(completed.stdout)
        assert completed.returncode != 0 and "invalid_run_root" in parsed["reasons"]
        assert len(_CAPTURE_DIRECTORIES) == len(_USED_RUN_ROOTS)
        generated_roots = [path for path in run_roots.iterdir()
                           if path.name.startswith("run-")]
        assert len(generated_roots) == _ROOT_NUMBER
        assert len(_USED_CHALLENGES) == _ROOT_NUMBER + 1
        capture_directories = []
        for run_root in generated_roots:
            assert stat.S_IMODE(run_root.stat().st_mode) == 0o700
            case_directories = [entry for entry in run_root.iterdir()
                                if entry.name.startswith("capture-") and entry.is_dir()]
            assert len(case_directories) <= 1, root_inventory(run_root)
            if case_directories:
                capture_directories.append(case_directories[0].resolve())
            for entry in run_root.iterdir():
                expected = 0o700 if entry.is_dir() else 0o600
                assert stat.S_IMODE(entry.lstat().st_mode) == expected
        assert len(capture_directories) == len(set(capture_directories))
    print("native capture acceptance passed")


if __name__ == "__main__":
    main()
