import contextlib, io, json, os, signal, stat, subprocess, sys, tempfile, textwrap, time, unittest
from unittest.mock import patch
from pathlib import Path
from run_block import CSV_SCHEMA, LANE_LABELS, clean_env, configure_shm_run, freshness_ok, patch_iox_config, patch_send_lane_configs, required_processes, resolve_roudi, send_lane_commands, validate_latency_csv, validate_publisher_status_schema, validate_result_gate, validate_status_schema, write_result
import run_block, run_matrix

def csv_text(label="abc_ros2_4KB", count="3000", value="1.0"):
    return ",".join(CSV_SCHEMA) + "\n" + ",".join([label, count] + [value] * 8) + "\n"

FAKE_SHM_APP = """#!/bin/sh
cfg=$1
value() { sed -n "s/^ *$1: *//p" "$cfg"; }
[ -d "$IBMW_SHM_ROOT" ] || exit 3
printf '%s %s\\n' "$IBMW_SHM_ROOT" "$IBMW_SHM_DOMAIN" >>"$(dirname "$cfg")/shm-env.txt"
printf '%s\\n' "$$" >"$(dirname "$cfg")/$(basename "$0").pid"
finish() {
  printf '%s\\n' '{"valid":true,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0}' >"$(value status_path)"
  printf '%s\\n%s\\n' 'label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us' 'shm_send_4KB,3000,1,1,1,1,1,1,1,1' >"$(value csv_path)"
  exit 0
}
if [ "$(basename "$0")" = shm_send_subscriber_app ]; then
  trap finish INT
else
  [ -n "$FAKE_HOLD_PUBLISHER_STATUS" ] || printf '%s\\n' '{"completion":true,"attempted_total":3050,"attempted_measured":3000,"valid":true}' >"$(value publisher_status_path)"
  trap 'exit 0' INT
fi
while :; do sleep 0.05; done
"""

FAKE_ABC_ENDPOINT = """#!/bin/sh
role=; csv=; status=
while [ $# -gt 0 ]; do
  case "$1" in --role) role=$2; shift;; --csv) csv=$2; shift;; --status) status=$2; shift;; esac
  shift
done
case "$(basename "$0")" in ibmw_abc_ros2_endpoint) label=abc_ros2_4KB;; *) label=abc_dds_4KB;; esac
if [ "$role" = sub ]; then
  sleep 1
  printf '%s\\n%s\\n' 'label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us' "$label,3000,1,1,1,1,1,1,1,1" >"$csv"
  printf '%s\\n' '{"valid":true,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0}' >"$status"
fi
exit 0
"""

REPOSITORY = Path(__file__).resolve().parents[5]

class RunBlockValidationTest(unittest.TestCase):
    def setUp(self):
        self.addCleanup(signal.signal, signal.SIGTERM, signal.getsignal(signal.SIGTERM))

    def _fake_shm_build(self, root):
        build = root / "build"; build.mkdir()
        for name in ("shm_send_subscriber_app", "shm_send_publisher_app"):
            (build / name).write_text(FAKE_SHM_APP); (build / name).chmod(0o755)
        return build

    def _run_directory(self):
        """A directory that outlives _cleanup_run, which reads the work dir."""
        directory = tempfile.TemporaryDirectory(); self.addCleanup(directory.cleanup)
        return contextlib.nullcontext(directory.name)

    def _cleanup_run(self, process, work, group=False):
        """Kill the runner (and its session) and remove any private root it created."""
        def cleanup():
            try:
                if group: os.killpg(process.pid, signal.SIGKILL)
                else: process.kill()
            except ProcessLookupError: pass
            process.wait()
            for name in ("shm_send_subscriber_app", "shm_send_publisher_app"):
                try: os.kill(int((work / f"{name}.pid").read_text()), signal.SIGKILL)
                except (OSError, ValueError): pass
            roots = set()
            for name in ("shm-root.txt", "shm-env.txt"):
                try: roots |= {line.split()[0] for line in (work / name).read_text().splitlines() if line}
                except OSError: pass
            for shm_root in roots:
                if shm_root.startswith("/tmp/ibS."): __import__("shutil").rmtree(shm_root, ignore_errors=True)
        self.addCleanup(cleanup)

    def _assert_stopped(self, work):
        for name in ("shm_send_subscriber_app", "shm_send_publisher_app"):
            pid = int((work / f"{name}.pid").read_text())
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try: os.kill(pid, 0)
                except ProcessLookupError: break
                time.sleep(.05)
            else: self.fail(f"{name} is still running")
        root = (work / "shm-env.txt").read_text().split()[0]
        self.assertTrue(root.startswith("/tmp/ibS."))
        self.assertFalse(Path(root).exists())

    def _run_matrix(self, root, declarations=()):
        output = root / "runs"
        plan = root / "comparison-plan.json"

        def completed(command, timeout):
            work = Path(command[command.index("--work") + 1])
            work.mkdir(parents=True, exist_ok=True)
            lane = command[2]
            (work / "result.json").write_text(json.dumps({"lane": lane, "valid": True}))
            return subprocess.CompletedProcess(command, 0)

        argv = ["--blocks", "1", "--build", str(root / "build"),
                "--output", str(output), "--comparison-plan", str(plan),
                "--order-policy", "seeded-fixed", "--order-seed", "17", *declarations]
        with patch.object(run_matrix.subprocess, "run", side_effect=completed):
            self.assertEqual(run_matrix.main(argv), 0)
        return output, json.loads(plan.read_text())

    def test_matrix_emits_formal_plan_and_lane_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, plan = self._run_matrix(root, (
                "--contract", '{"id":"abc-v1"}', "--profile", "process-tree",
                "--workload", "4k-3000", "--resource-scope", "sut-descendants"))
            self.assertEqual(plan["contract"], {"id": "abc-v1"})
            self.assertEqual(plan["order"], ["A", "B", "C"])
            self.assertEqual(plan["order_policy"], "seeded-fixed")
            self.assertEqual(plan["order_seed"], 17)
            for lane in "ABC":
                run = output / "block-1" / lane
                manifest = json.loads((run / "manifest.json").read_text())
                validation = json.loads((run / "validation.json").read_text())
                self.assertEqual(manifest["comparison_block_id"], "abc-block-1")
                self.assertEqual(manifest["comparison_lane"], lane)
                self.assertEqual(manifest["contract"], plan["contract"])
                self.assertEqual(manifest["profile"], plan["profile"])
                self.assertEqual(manifest["workload"], plan["workload"])
                self.assertEqual(manifest["resource_scope"], plan["resource_scope"])
                self.assertEqual(manifest["order_seed"], 17)
                self.assertEqual(manifest["metric_availability"], "unsupported")
                self.assertFalse(validation["valid_for_comparison"])

    def test_matrix_missing_declarations_are_unsupported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, plan = self._run_matrix(root)
            for field in ("contract", "profile", "workload", "resource_scope"):
                self.assertEqual(plan[field]["status"], "unsupported")
            manifest = json.loads((output / "block-1/A/manifest.json").read_text())
            self.assertEqual(manifest["contract"]["status"], "unsupported")
            self.assertIn("RESOURCE_PROVENANCE_UNSUPPORTED", json.loads(
                (output / "block-1/A/validation.json").read_text())["failure_codes"])

            repository = Path(__file__).resolve().parents[5]
            env = os.environ.copy()
            env["PYTHONPATH"] = str(repository / "tools")
            comparison = root / "comparison"
            completed = subprocess.run([
                os.fspath(Path(sys.executable)), "-m", "mw_perf_collect", "compare",
                "--plan", str(root / "comparison-plan.json"),
                "--output", str(comparison)], env=env, capture_output=True, text=True)
            self.assertNotEqual(completed.returncode, 0)
            result = json.loads((comparison / "comparison.json").read_text())
            self.assertFalse(result["valid_for_comparison"])
            self.assertFalse(any(code.startswith("PLAN_SCHEMA:")
                                 for code in result["failure_codes"]))
            self.assertTrue(any(code.startswith("RUN_NOT_VALID_FOR_COMPARISON:")
                                for code in result["failure_codes"]))

    def test_matrix_collector_managed_uses_current_python_and_canonical_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); commands=[]
            def completed(command, timeout, env=None):
                commands.append(command)
                if command[2:4] == ["mw_perf_collect", "run"]:
                    config_path=Path(command[command.index("--config")+1]); config=json.loads(config_path.read_text())
                    source=Path(config["artifacts"]["latency"]["result_path"]); source.parent.mkdir(parents=True,exist_ok=True)
                    source.write_text(json.dumps({"lane":config["comparison"]["lane"],"valid":True}))
                    output=Path(command[command.index("--output")+1]); run=output/("canonical-"+config["comparison"]["lane"]); run.mkdir(parents=True)
                    (run/"validation.json").write_text(json.dumps({"valid_for_comparison":True}))
                return subprocess.CompletedProcess(command,0)
            argv=["--collector-managed","--blocks","1","--build",str(root/"build"),"--output",str(root/"runs"),
                  "--contract","abc-v1","--profile","tree","--workload","4k","--resource-scope","descendants"]
            with patch.object(run_matrix.subprocess,"run",side_effect=completed): self.assertEqual(run_matrix.main(argv),0)
            self.assertEqual(sum(command[:3]==[sys.executable,"-m","mw_perf_collect"] and command[3]=="run" for command in commands),3)
            self.assertEqual(sum(command[:3]==[sys.executable,"-m","mw_perf_collect"] and command[3]=="validate" for command in commands),3)
            plan=json.loads((root/"runs/comparison-plan.json").read_text())
            self.assertTrue(all("canonical-" in item["path"] for item in plan["blocks"][0]["runs"]))
            config=json.loads((root/"runs/block-1/A/collector-config.json").read_text())
            self.assertTrue(config["collectors"]["process"]["include_descendants"])
            self.assertEqual(config["collectors"]["network"]["interfaces"],["lo"])
            self.assertIn("--collector-managed",config["sut"]["command"])

    def test_matrix_collector_managed_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            argv=["--collector-managed","--blocks","1","--build",str(root/"build"),"--output",str(root/"runs"),
                  "--contract","abc-v1","--profile","tree","--workload","4k","--resource-scope","descendants"]
            with patch.object(run_matrix.subprocess,"run",return_value=subprocess.CompletedProcess([],20)):
                self.assertEqual(run_matrix.main(argv),1)
            self.assertFalse(json.loads((root/"runs/summary.json").read_text())["valid"])

    def test_a_lane_managed_config_is_repository_root_example(self):
        source_dir = Path(__file__).resolve().parent
        config = json.loads((source_dir / "a_lane_managed.json").read_text())
        command = config["sut"]["command"]
        env = config["sut"]["env"]
        artifact = config["adapters"]["ros2"]["latency_artifact"]

        self.assertEqual(command[1], "src/samples/cpp/transport_benchmark/abc_interop/run_a_managed.sh")
        self.assertEqual(config["sut"]["working_directory"], ".")
        self.assertEqual(env["IBMW_ABC_ROS2_ENDPOINT"], "build-benchmark-smoke/ibmw_abc_ros2_endpoint")
        self.assertEqual(env["IBMW_ABC_LATENCY_JSONL"], "/tmp/ibmw_abc_a_lane_latency.jsonl")
        self.assertEqual(artifact["path"], env["IBMW_ABC_LATENCY_JSONL"])
        self.assertNotIn("/home/", json.dumps(config))

    def test_resolve_roudi_override_environment_metadata_and_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            paths = [build / name for name in ("override", "environment", "package-roudi", "iox-roudi")]
            for path in paths:
                path.write_text("#!/bin/sh\n")
                path.chmod(path.stat().st_mode | stat.S_IXUSR)
            (build / "ibmw_abc_roudi_path.txt").write_text(str(paths[2]) + "\n")
            self.assertEqual(resolve_roudi(build, paths[0], {"IBMW_ROUDI_BIN": str(paths[1])}), paths[0].resolve())
            self.assertEqual(resolve_roudi(build, environ={"IBMW_ROUDI_BIN": str(paths[1])}), paths[1].resolve())
            self.assertEqual(resolve_roudi(build, environ={}), paths[2].resolve())
            (build / "ibmw_abc_roudi_path.txt").unlink()
            self.assertEqual(resolve_roudi(build, environ={}), paths[3].resolve())

    def test_cmake_roudi_helper_supports_source_and_package_targets(self):
        module_dir = Path(__file__).resolve().parents[5] / "cmake"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "main.c").write_text("int main(void) { return 0; }\n")
            (source / "CMakeLists.txt").write_text(textwrap.dedent(f"""
                cmake_minimum_required(VERSION 3.24)
                project(roudi_source C)
                list(APPEND CMAKE_MODULE_PATH "{module_dir}")
                include(IbmwIceoryxRouDi)
                add_executable(iox-roudi EXCLUDE_FROM_ALL main.c)
                add_executable(consumer main.c)
                ibmw_add_iceoryx_roudi_dependency(consumer)
                ibmw_generate_iceoryx_roudi_path("${{CMAKE_BINARY_DIR}}/roudi.txt")
            """))
            source_build = root / "source-build"
            subprocess.run(["cmake", "-S", str(source), "-B", str(source_build)], check=True, capture_output=True, text=True)
            subprocess.run(["cmake", "--build", str(source_build), "--target", "consumer"], check=True, capture_output=True, text=True)
            self.assertTrue((source_build / "iox-roudi").is_file())

            package = root / "package"
            package.mkdir()
            (package / "CMakeLists.txt").write_text(textwrap.dedent(f"""
                cmake_minimum_required(VERSION 3.24)
                project(roudi_package NONE)
                list(APPEND CMAKE_MODULE_PATH "{module_dir}")
                include(IbmwIceoryxRouDi)
                add_executable(iceoryx_posh::iox-roudi IMPORTED GLOBAL)
                set_target_properties(iceoryx_posh::iox-roudi PROPERTIES IMPORTED_LOCATION "/bin/true")
                add_custom_target(consumer)
                ibmw_add_iceoryx_roudi_dependency(consumer)
                ibmw_generate_iceoryx_roudi_path("${{CMAKE_BINARY_DIR}}/roudi.txt")
            """))
            package_build = root / "package-build"
            subprocess.run(["cmake", "-S", str(package), "-B", str(package_build)], check=True, capture_output=True, text=True)
            self.assertEqual((package_build / "roudi.txt").read_text().strip(), "/bin/true")

    def test_c_lane_patches_temporary_historical_configs(self):
        template_dir = Path(__file__).resolve().parents[1] / "iox_send/config/linux/bin/cfg/iox_send"
        for name, artifact_key in (("publisher.yaml", "publisher_status_path"),
                                   ("subscriber.yaml", "status_path")):
            source = template_dir / name
            historical = source.read_text()
            self.assertIn('topic: "iox_bench_send_topic"', historical)
            self.assertIn("payload_bytes: 1024", historical)
            with tempfile.TemporaryDirectory() as directory:
                target = Path(directory) / name
                patch_iox_config(
                    source, target,
                    {"path": "/tmp/libibmw_iceoryx_transport.so",
                     "topic": '"/mw_perf/payload_4kb"'},
                    {"topic": '"/mw_perf/payload_4kb"', "payload_bytes": 4096,
                     "message_count": 3000, "start_delay_ms": 2000,
                     "label": "iox_send_4KB", artifact_key: "/tmp/status.json"})
                patched = target.read_text()
                self.assertIn('    topic: "/mw_perf/payload_4kb"', patched)
                self.assertIn("    payload_bytes: 4096", patched)
                self.assertIn("    message_count: 3000", patched)
                self.assertIn("    start_delay_ms: 2000", patched)
                self.assertIn(f"    {artifact_key}: /tmp/status.json", patched)
            self.assertEqual(source.read_text(), historical)

    def test_s_lane_patches_temporary_shm_configs(self):
        template_dir = Path(__file__).resolve().parents[1] / "shm_send/config/linux/bin/cfg/shm_send"
        historical = {name: (template_dir / name).read_text() for name in ("publisher.yaml", "subscriber.yaml")}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); sub = root / "subscriber.yaml"; pub = root / "publisher.yaml"
            patch_send_lane_configs("S", root, sub, pub, root / "latency.csv", root / "status.json", root / "publisher.status.json")
            for path, artifact in ((sub, f"    status_path: {root}/status.json"),
                                   (pub, f"    publisher_status_path: {root}/publisher.status.json")):
                patched = path.read_text()
                self.assertIn("- name: shm_extension", patched)
                self.assertIn(f"path: {root.resolve()}/libibmw_shm_transport.so", patched)
                self.assertIn("driver_name: iceoryx", patched)
                self.assertNotIn("libibmw_iceoryx_transport.so", patched)
                self.assertIn('- topic: /mw_perf/payload_4kb', patched)
                for line in ("topic: /mw_perf/payload_4kb", "payload_bytes: 4096", "hz: 100", "warmup_messages: 50",
                             "message_count: 3000", "start_delay_ms: 2000", "label: shm_send_4KB"):
                    self.assertIn("    " + line + "\n", patched)
                self.assertIn(artifact, patched)
            self.assertIn(f"    csv_path: {root}/latency.csv", sub.read_text())
        self.assertEqual({name: (template_dir / name).read_text() for name in historical}, historical)

    def test_s_lane_commands_do_not_resolve_roudi(self):
        build = Path("/build")
        with patch.object(run_block, "resolve_roudi", side_effect=AssertionError("RouDi resolved")):
            commands = send_lane_commands("S", build, "sub.yaml", "pub.yaml", Path("/ignored/iox-roudi"))
        self.assertEqual(commands, [("subscriber", build / "shm_send_subscriber_app", ["sub.yaml"]),
                                    ("publisher", build / "shm_send_publisher_app", ["pub.yaml"])])

    def test_c_lane_commands_start_resolved_roudi_first(self):
        build = Path("/build"); roudi = Path("/opt/iox-roudi")
        with patch.object(run_block, "resolve_roudi", return_value=roudi) as resolve:
            commands = send_lane_commands("C", build, "sub.yaml", "pub.yaml", roudi)
        resolve.assert_called_once_with(build, roudi)
        self.assertEqual(commands, [("roudi", roudi, []),
                                    ("subscriber", build / "iox_send_subscriber_app", ["sub.yaml"]),
                                    ("publisher", build / "iox_send_publisher_app", ["pub.yaml"])])

    def test_lane_labels_and_required_processes(self):
        self.assertEqual(LANE_LABELS, {"A": "abc_ros2_4KB", "B": "abc_dds_4KB", "C": "iox_send_4KB", "S": "shm_send_4KB"})
        for lane in "ABS":
            self.assertEqual(required_processes(lane), ("publisher", "subscriber"))
        self.assertEqual(required_processes("C"), ("publisher", "subscriber", "roudi"))

    def test_shm_run_uses_private_root_and_unique_domain(self):
        env = {"IBMW_SHM_GROUP": "1000", "IBMW_SHM_ROOT": "/shared", "IBMW_SHM_DOMAIN": "shared"}
        domain = configure_shm_run(env, "/tmp/ibS.abcd1234", 45)
        self.assertEqual(domain, "abcS45.abcd1234")
        self.assertEqual(env, {"IBMW_SHM_ROOT": "/tmp/ibS.abcd1234", "IBMW_SHM_DOMAIN": domain})
        self.assertNotEqual(domain, configure_shm_run({}, "/tmp/ibS.wxyz5678", 45))
        self.assertLessEqual(len(f"/tmp/ibS.abcd1234/ibmw_shm.{2**32 - 1}.abcS65535.abcd1234"), 63)

    def test_shm_run_rejects_paths_beyond_transport_limits(self):
        with self.assertRaisesRegex(ValueError, "service directory"):
            configure_shm_run({}, "/tmp/" + "r" * 40 + "/ibS.abcd1234", 45)
        with self.assertRaisesRegex(ValueError, "domain"):
            configure_shm_run({}, "/tmp/ibS.abcd1234", 10**24)

    def test_s_lane_runs_without_roudi_and_removes_private_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = self._fake_shm_build(root); work = root / "work"
            argv = ["run_block.py", "S", "--build", str(build), "--work", str(work)]
            with patch.object(sys, "argv", argv), patch.dict(os.environ, {"IBMW_SHM_GROUP": "1000"}), \
                 patch.object(run_block, "resolve_roudi", side_effect=AssertionError("RouDi resolved")):
                self.assertEqual(run_block.main(), 0)
            result = json.loads((work / "result.json").read_text())
            self.assertTrue(result["valid"], result["validation_errors"])
            self.assertEqual(sorted(result["process_exit_codes"]), ["publisher", "subscriber"])
            self.assertEqual(result["publisher_status"]["attempted_total"], 3050)
            self.assertEqual(result["validation_warnings"], [])
            observed = set((work / "shm-env.txt").read_text().splitlines())
            self.assertEqual(len(observed), 1)
            shm_root, shm_domain = observed.pop().split()
            self.assertTrue(shm_root.startswith("/tmp/ibS."))
            self.assertEqual(shm_domain, result["shm_domain"])
            self.assertEqual((work / "shm-root.txt").read_text(), shm_root + "\n")
            self.assertFalse(Path(shm_root).exists())

    def test_s_lane_interrupt_or_error_stops_live_endpoints_before_result(self):
        for failure, expected in ((KeyboardInterrupt(), "runner_keyboard_interrupt"), (ValueError("boom"), "runner_error: boom")):
            with self.subTest(expected), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); build = self._fake_shm_build(root); work = root / "work"
                real_write = run_block.write_result
                def write_after_cleanup(target, result, managed=False):
                    self.assertFalse(Path((work / "shm-env.txt").read_text().split()[0]).exists())
                    real_write(target, result, managed)
                argv = ["run_block.py", "S", "--build", str(build), "--work", str(work)]
                with patch.object(sys, "argv", argv), patch.object(run_block, "write_result", side_effect=write_after_cleanup), \
                     patch.object(run_block, "validate_publisher_status_schema", side_effect=failure):
                    self.assertEqual(run_block.main(), 130 if isinstance(failure, KeyboardInterrupt) else 1)
                result = json.loads((work / "result.json").read_text())
                self.assertEqual(result["validation_errors"], [expected])
                self.assertEqual(result["validation_warnings"], [])
                self.assertEqual(result["shm_domain"], (work / "shm-env.txt").read_text().split()[1])
                self._assert_stopped(work)

    def test_s_lane_sigterm_stops_endpoints_and_removes_private_root(self):
        with self._run_directory() as directory:
            root = Path(directory); build = self._fake_shm_build(root); work = root / "work"
            runner = Path(__file__).with_name("run_block.py")
            process = subprocess.Popen([sys.executable, str(runner), "S", "--build", str(build), "--work", str(work)],
                                       env={**os.environ, "FAKE_HOLD_PUBLISHER_STATUS": "1"}, stdout=subprocess.DEVNULL)
            self._cleanup_run(process, work)
            deadline = time.monotonic() + 15
            while not (work / "shm_send_publisher_app.pid").is_file() and time.monotonic() < deadline: time.sleep(.05)
            time.sleep(.2)
            process.send_signal(signal.SIGTERM)
            self.assertEqual(process.wait(timeout=20), 130)
            self._assert_stopped(work)
            result = json.loads((work / "result.json").read_text())
            self.assertEqual(result["validation_errors"], ["runner_keyboard_interrupt"])

    def test_stop_shm_run_removes_root_when_wait_is_interrupted(self):
        class Interrupted:
            def poll(self): return None
            def terminate(self): pass
            def wait(self, timeout=None): raise KeyboardInterrupt
        root = Path(tempfile.mkdtemp(prefix="ibS.", dir="/tmp")); self.addCleanup(lambda: __import__("shutil").rmtree(root, ignore_errors=True))
        with self.assertRaises(KeyboardInterrupt):
            run_block.stop_shm_run({"publisher": Interrupted()}, root, [])
        self.assertFalse(root.exists())

    def test_stop_shm_run_reports_failed_removal_and_is_repeatable(self):
        errors = []
        with patch.object(run_block.shutil, "rmtree", side_effect=PermissionError("denied")):
            run_block.stop_shm_run({}, "/tmp/ibS.none", errors)
        self.assertEqual(errors, ["shm_root_cleanup_failed: denied"])
        errors = []
        run_block.stop_shm_run({}, "/tmp/ibS.does-not-exist", errors)
        self.assertEqual(errors, [])

    def test_stop_shm_run_shares_one_deadline_across_endpoints(self):
        clock = [100.0]; timeouts = []
        class Stuck:
            def __init__(self): self.killed = False
            def poll(self): return None
            def terminate(self): pass
            def kill(self): self.killed = True
            def wait(self, timeout=None):
                if timeout is None: return 0
                timeouts.append(timeout); clock[0] += timeout
                raise subprocess.TimeoutExpired("endpoint", timeout)
        procs = {"subscriber": Stuck(), "publisher": Stuck()}
        with patch.object(run_block.time, "monotonic", side_effect=lambda: clock[0]), \
             patch.object(run_block.shutil, "rmtree"):
            run_block.stop_shm_run(procs, "/tmp/ibS.none", [])
        self.assertEqual(timeouts, [5, 0])
        self.assertTrue(all(proc.killed for proc in procs.values()))

    def test_s_lane_failed_root_removal_invalidates_the_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = self._fake_shm_build(root); work = root / "work"
            real_rmtree = run_block.shutil.rmtree
            def refuse(path, *args, **kwargs):
                if str(path).startswith("/tmp/ibS."): raise PermissionError("denied")
                return real_rmtree(path, *args, **kwargs)
            argv = ["run_block.py", "S", "--build", str(build), "--work", str(work)]
            with patch.object(sys, "argv", argv), patch.object(run_block.shutil, "rmtree", side_effect=refuse):
                try: self.assertEqual(run_block.main(), 1)
                finally: real_rmtree((work / "shm-env.txt").read_text().split()[0], ignore_errors=True)
            result = json.loads((work / "result.json").read_text())
            self.assertFalse(result["valid"])
            self.assertEqual(result["validation_errors"], ["shm_root_cleanup_failed: denied"])
            self.assertEqual(result["validation_warnings"], [])

    def test_s_lane_term_before_the_run_starts_aborts_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = self._fake_shm_build(root); work = root / "work"
            real_clean_env = run_block.clean_env
            def term_first():
                os.kill(os.getpid(), signal.SIGTERM); time.sleep(.05)
                return real_clean_env()
            argv = ["run_block.py", "S", "--build", str(build), "--work", str(work), "--collector-managed"]
            with patch.object(sys, "argv", argv), patch.dict(os.environ, {"MW_PERF_RUN_ID": "run"}), \
                 patch.object(run_block, "clean_env", side_effect=term_first), \
                 patch.object(run_block.tempfile, "mkdtemp", side_effect=AssertionError("root created")):
                self.assertEqual(run_block.main(), 130)
            result = json.loads((work / "result.json").read_text())
            self.assertEqual(result["validation_errors"], ["runner_keyboard_interrupt"])
            self.assertFalse((work / "shm-root.txt").exists())

    def test_s_lane_term_while_creating_root_still_removes_it(self):
        # SIGINT keeps its default KeyboardInterrupt, so only the signal mask
        # stops it from landing between mkdir and recording the root.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = self._fake_shm_build(root); work = root / "work"; created = []
            real_mkdtemp = tempfile.mkdtemp
            def mkdtemp_then_term(**kwargs):
                created.append(real_mkdtemp(**kwargs)); self.addCleanup(__import__("shutil").rmtree, created[-1], True)
                os.kill(os.getpid(), signal.SIGTERM); os.kill(os.getpid(), signal.SIGINT); time.sleep(.05)
                return created[-1]
            argv = ["run_block.py", "S", "--build", str(build), "--work", str(work), "--collector-managed"]
            with patch.object(sys, "argv", argv), patch.dict(os.environ, {"MW_PERF_RUN_ID": "run"}), \
                 patch.object(run_block.tempfile, "mkdtemp", side_effect=mkdtemp_then_term):
                self.assertEqual(run_block.main(), 130)
            self.assertEqual(len(created), 1)
            self.assertFalse(Path(created[0]).exists())
            self.assertEqual((work / "shm-root.txt").read_text(), created[0] + "\n")
            result = json.loads((work / "result.json").read_text())
            self.assertEqual(result["validation_errors"], ["runner_keyboard_interrupt"])
            self.assertFalse((work / "shm-env.txt").exists())

    def test_s_lane_term_inside_endpoint_start_still_stops_that_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = self._fake_shm_build(root); work = root / "work"; started = []
            real_popen = subprocess.Popen
            def popen_then_term(*args, **kwargs):
                started.append(real_popen(*args, **kwargs)); self.addCleanup(started[-1].kill)
                # Delivered after the fork, before Popen returns to the runner.
                os.kill(os.getpid(), signal.SIGTERM); time.sleep(.05)
                return started[-1]
            argv = ["run_block.py", "S", "--build", str(build), "--work", str(work)]
            with patch.object(sys, "argv", argv), patch.object(run_block.subprocess, "Popen", side_effect=popen_then_term):
                self.assertEqual(run_block.main(), 130)
            self.assertEqual(len(started), 1, "the publisher must not start after a TERM")
            self.assertIsNotNone(started[0].poll(), "the subscriber started under the TERM is still running")
            self.assertFalse(Path((work / "shm-root.txt").read_text().strip()).exists())
            result = json.loads((work / "result.json").read_text())
            self.assertEqual(result["validation_errors"], ["runner_keyboard_interrupt"])

    def test_s_lane_term_after_completion_keeps_the_valid_result(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = self._fake_shm_build(root); work = root / "work"
            real_write = run_block.write_result
            def term_then_write(target, result, managed=False):
                os.kill(os.getpid(), signal.SIGTERM); time.sleep(.05)
                real_write(target, result, managed)
                os.kill(os.getpid(), signal.SIGTERM); time.sleep(.05)
            argv = ["run_block.py", "S", "--build", str(build), "--work", str(work)]
            with patch.object(sys, "argv", argv), patch.object(run_block, "write_result", side_effect=term_then_write), \
                 patch.object(sys, "stdout", io.StringIO()):
                self.assertEqual(run_block.main(), 0)
            result = json.loads((work / "result.json").read_text())
            self.assertTrue(result["valid"], result["validation_errors"])
            self._assert_stopped(work)

    def test_s_lane_term_during_error_handling_keeps_the_error_result(self):
        for failure, code, expected in ((ValueError("boom"), 1, "runner_error: boom"),
                                        (KeyboardInterrupt(), 130, "runner_keyboard_interrupt")):
            with self.subTest(expected), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); build = self._fake_shm_build(root); work = root / "work"
                real_stop = run_block.stop_shm_run; calls = []
                def term_then_stop(procs, shm_root, errors):
                    if not calls: os.kill(os.getpid(), signal.SIGTERM); time.sleep(.05)
                    calls.append(shm_root); real_stop(procs, shm_root, errors)
                argv = ["run_block.py", "S", "--build", str(build), "--work", str(work)]
                with patch.object(sys, "argv", argv), patch.object(run_block, "stop_shm_run", side_effect=term_then_stop), \
                     patch.object(run_block, "validate_publisher_status_schema", side_effect=failure):
                    self.assertEqual(run_block.main(), code)
                result = json.loads((work / "result.json").read_text())
                self.assertEqual(result["validation_errors"], [expected])
                self._assert_stopped(work)

    def _start_managed_s(self, root, env=None):
        build = self._fake_shm_build(root); work = root / "work"
        runner = Path(__file__).with_name("run_block.py")
        process = subprocess.Popen([sys.executable, str(runner), "S", "--build", str(build), "--work", str(work),
                                    "--collector-managed"], env={**os.environ, "MW_PERF_RUN_ID": "run", **(env or {})},
                                   stdout=subprocess.DEVNULL, start_new_session=True)
        self._cleanup_run(process, work, group=True)
        return process, work

    def _wait_for(self, path, timeout=30):
        deadline = time.monotonic() + timeout
        while not path.is_file():
            if time.monotonic() > deadline: self.fail(f"{path.name} did not appear")
            time.sleep(.05)

    def _collector_stop(self, process):
        """Signal like the collector: the group first, then the leader again."""
        os.killpg(process.pid, signal.SIGTERM); process.send_signal(signal.SIGTERM)

    def test_s_lane_collector_managed_keeps_valid_result_after_collector_term(self):
        with self._run_directory() as directory:
            process, work = self._start_managed_s(Path(directory))
            self._wait_for(work / "result.json")
            time.sleep(.5)
            self.assertIsNone(process.poll(), "leader must wait for the collector")
            self.assertFalse(Path((work / "shm-env.txt").read_text().split()[0]).exists())
            before = (work / "result.json").read_bytes()
            self._collector_stop(process)
            self.assertEqual(process.wait(timeout=10), 0)
            self.assertEqual((work / "result.json").read_bytes(), before)
            result = json.loads(before)
            self.assertTrue(result["valid"], result["validation_errors"])
            identity = result["collector_managed"]
            self.assertEqual((identity["run_id"], identity["lane"], identity["producer_pid"]), ("run", "S", process.pid))
            self._assert_stopped(work)

    def test_s_lane_collector_term_during_run_cleans_up_and_exits(self):
        with self._run_directory() as directory:
            process, work = self._start_managed_s(Path(directory), {"FAKE_HOLD_PUBLISHER_STATUS": "1"})
            self._wait_for(work / "shm_send_publisher_app.pid")
            time.sleep(.2)
            self._collector_stop(process)
            self.assertEqual(process.wait(timeout=20), 130)
            self._assert_stopped(work)
            result = json.loads((work / "result.json").read_text())
            self.assertFalse(result["valid"])
            self.assertEqual(result["validation_errors"], ["runner_keyboard_interrupt"])
            self.assertEqual(result["collector_managed"]["lane"], "S")

    def test_s_lane_collector_managed_survives_a_term_burst_after_the_result(self):
        # Before _StopFlag a nested TERM inside threading.Event.set() deadlocked
        # the leader until the collector's SIGKILL.
        # Signals sent back to back coalesce while the leader is still asleep,
        # so the burst is spread out to land inside the handlers it wakes.
        burst = textwrap.dedent("""
            import os, signal, sys, time
            pid, gap = int(sys.argv[1]), 2e-6
            for _ in range(25):
                for send in (lambda: os.killpg(pid, signal.SIGTERM), lambda: os.kill(pid, signal.SIGTERM)):
                    try: send()
                    except ProcessLookupError: sys.exit(0)
                    until = time.perf_counter() + gap
                    while time.perf_counter() < until: pass
        """)
        with self._run_directory() as directory:
            process, work = self._start_managed_s(Path(directory))
            self._wait_for(work / "result.json")
            subprocess.run([sys.executable, "-c", burst, str(process.pid)], check=True, timeout=10)
            self.assertEqual(process.wait(timeout=10), 0)
            result = json.loads((work / "result.json").read_text())
            self.assertTrue(result["valid"], result["validation_errors"])
            self.assertFalse(Path((work / "shm-root.txt").read_text().strip()).exists())
            self._assert_stopped(work)

    def test_s_lane_leader_exits_zero_under_real_collector_stop(self):
        sys.path.insert(0, str(REPOSITORY / "tools")); self.addCleanup(sys.path.remove, str(REPOSITORY / "tools"))
        from mw_perf_collect import core
        for attempt in range(4):
            with self.subTest(attempt), self._run_directory() as directory:
                process, work = self._start_managed_s(Path(directory))
                self._wait_for(work / "result.json")
                known = {process.pid: core.proc_stat(process.pid)["start_time_ticks"]}
                stopped = core._stop_managed(process, 10, known)
                self.assertIsNone(stopped.error)
                self.assertFalse(stopped.sigkill_escalated)
                self.assertEqual(process.returncode, 0)
                self.assertTrue(json.loads((work / "result.json").read_text())["valid"])
                self._assert_stopped(work)

    def test_matrix_abs_runs_through_real_collector_and_compare(self):
        """run_matrix -> run_block (fake endpoints) -> real collector run, validate and compare."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = self._fake_shm_build(root)
            for name in ("ibmw_abc_ros2_endpoint", "ibmw_abc_dds_endpoint"):
                (build / name).write_text(FAKE_ABC_ENDPOINT); (build / name).chmod(0o755)
            real_config = run_matrix._managed_config
            def short_config(*args):
                config = real_config(*args); config["run"]["sample_seconds"] = 6; return config
            output = root / "runs"
            argv = ["--blocks", "1", "--build", str(build), "--output", str(output), "--lanes", "S,A,B",
                    "--collector-managed", "--contract", "abc-4kb-v1", "--profile", "tree",
                    "--workload", "payload-4k-3000", "--resource-scope", "descendants"]
            with patch.object(run_matrix, "_managed_config", side_effect=short_config), \
                 patch.object(sys, "stdout", io.StringIO()):
                code = run_matrix.main(argv)
            self.assertEqual(code, 0, (output / "summary.json").read_text())
            plan = json.loads((output / "comparison-plan.json").read_text())
            self.assertEqual((plan["schema"], plan["lanes"], plan["order"]),
                             ("mw_perf.comparison-plan.v2", ["A", "B", "S"], ["S", "A", "B"]))
            for run in plan["blocks"][0]["runs"]:
                manifest = json.loads((Path(run["path"]) / "manifest.json").read_text())
                self.assertEqual(manifest["sut_returncode"], 0, run["lane"])
                self.assertEqual(manifest["comparison_schema"], "mw_perf.comparison-metadata.v2")
            compare = subprocess.run([sys.executable, "-m", "mw_perf_collect", "compare", "--plan",
                                      str(output / "comparison-plan.json"), "--output", str(root / "compare")],
                                     env=run_matrix._collector_environment(), capture_output=True, text=True)
            comparison = json.loads((root / "compare" / "comparison.json").read_text())
            self.assertEqual(compare.returncode, 0, comparison["failure_codes"])
            self.assertEqual(comparison["valid_blocks"], ["abc-block-1"])
            self.assertEqual({lane: value["label"] for lane, value in comparison["blocks"][0]["lanes"].items()},
                             {"A": "abc_ros2_4KB", "B": "abc_dds_4KB", "S": "shm_send_4KB"})
            roots = (output / "block-1" / "S" / "lane-output" / "shm-env.txt").read_text().split()
            self.assertFalse(Path(roots[0]).exists())

    def test_stop_flag_takes_no_lock_in_signal_handlers(self):
        # A nested TERM inside Event.set() deadlocks on its non-reentrant lock.
        self.assertFalse(hasattr(run_block, "threading"))
        flag = run_block._StopFlag()
        self.assertFalse(flag.wait(0))
        flag.set()
        self.assertTrue(flag.is_set())
        self.assertTrue(flag.wait(0))
        # A TERM ends a long wait promptly instead of after the whole timeout.
        flag = run_block._StopFlag(); signal.signal(signal.SIGTERM, lambda *_: flag.set())
        sender = subprocess.Popen(["sh", "-c", f"sleep 0.2; kill -TERM {os.getpid()}"]); self.addCleanup(sender.wait)
        started = time.monotonic()
        self.assertTrue(flag.wait(10))
        self.assertLess(time.monotonic() - started, 5)

    def test_s_lane_collector_managed_requires_run_id_before_touching_work(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory) / "work"
            env = {key: value for key, value in os.environ.items() if key != "MW_PERF_RUN_ID"}
            completed = subprocess.run([sys.executable, str(Path(__file__).with_name("run_block.py")), "S", "--build", directory,
                                        "--work", str(work), "--collector-managed"], env=env, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("--collector-managed requires MW_PERF_RUN_ID", completed.stderr)
            self.assertEqual(list(work.iterdir()), [])

    def test_runner_exits_with_a_defined_code_on_unexpected_errors(self):
        runner = str(Path(__file__).with_name("run_block.py"))
        with tempfile.TemporaryDirectory() as directory:
            blocker = Path(directory) / "file"; blocker.write_text("")
            # The work directory cannot be created: the error escapes main().
            completed = subprocess.run([sys.executable, runner, "A", "--build", directory, "--work", str(blocker / "work")],
                                       capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 1)
            self.assertIn("Traceback", completed.stderr)
            # stdout is a closed pipe: printing the result fails, the exit is still 1.
            read_end, write_end = os.pipe(); os.close(read_end)
            try:
                completed = subprocess.run([sys.executable, runner, "S", "--build", str(Path(directory) / "missing"),
                                            "--work", str(Path(directory) / "work")], stdout=write_end, stderr=subprocess.PIPE, timeout=30)
            finally: os.close(write_end)
            self.assertEqual(completed.returncode, 1)
            self.assertFalse(Path((Path(directory) / "work" / "shm-root.txt").read_text().strip()).exists())

    def test_s_lane_failed_start_removes_private_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); work = root / "work"; created = []
            real_mkdtemp = tempfile.mkdtemp
            def mkdtemp(**kwargs):
                created.append(real_mkdtemp(**kwargs)); return created[-1]
            argv = ["run_block.py", "S", "--build", str(root / "missing"), "--work", str(work)]
            with patch.object(sys, "argv", argv), patch.object(run_block.tempfile, "mkdtemp", side_effect=mkdtemp):
                self.assertEqual(run_block.main(), 1)
            self.assertFalse(json.loads((work / "result.json").read_text())["valid"])
            self.assertEqual(len(created), 1)
            self.assertFalse(Path(created[0]).exists())

    def test_matrix_default_order_follows_shm_only_build(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            self.assertEqual(run_matrix._default_order(build), ["A", "B", "C"])
            (build / "shm_send_publisher_app").touch()
            self.assertEqual(run_matrix._default_order(build), ["A", "B", "S"])
            (build / "iox_send_publisher_app").touch()
            self.assertEqual(run_matrix._default_order(build), ["A", "B", "C"])

    def test_matrix_default_order_prefers_cmake_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory); (build / "iox_send_publisher_app").touch()
            def cache(iceoryx, shm):
                (build / "CMakeCache.txt").write_text(f"IBMW_BUILD_ICEORYX_EXTENSION:BOOL={iceoryx}\nIBMW_BUILD_SHM_EXTENSION:BOOL={shm}\n")
                return run_matrix._default_order(build)
            self.assertEqual(cache("OFF", "ON"), ["A", "B", "S"])
            for shm in ("on", "True", "1", "yes", "Y"):
                self.assertEqual(cache("0", shm), ["A", "B", "S"])
            for iceoryx in ("ON", "on", "TRUE", "1", "YES", "y"):
                self.assertEqual(cache(iceoryx, "ON"), ["A", "B", "C"])
            self.assertEqual(cache("OFF", "OFF"), ["A", "B", "C"])
            (build / "iox_send_publisher_app").unlink(); (build / "shm_send_publisher_app").touch()
            self.assertEqual(cache("ON", "ON"), ["A", "B", "C"])
            (build / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Ninja\n")
            self.assertEqual(run_matrix._default_order(build), ["A", "B", "S"])

    def _run_lanes(self, root, *extra):
        commands = []
        def completed(command, timeout):
            commands.append(command)
            work = Path(command[command.index("--work") + 1]); work.mkdir(parents=True, exist_ok=True)
            (work / "result.json").write_text(json.dumps({"lane": command[2], "valid": True}))
            return subprocess.CompletedProcess(command, 0)
        argv = ["--build", str(root / "build"), "--output", str(root / "runs"), *extra]
        with patch.object(run_matrix.subprocess, "run", side_effect=completed):
            self.assertEqual(run_matrix.main(argv), 0)
        return commands, json.loads((root / "runs/summary.json").read_text())

    def test_matrix_abc_keeps_domains_and_semantic_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            commands, summary = self._run_lanes(Path(directory), "--blocks", "2")
        self.assertEqual([(c[2], c[c.index("--domain") + 1]) for c in commands],
                         [("A", "83"), ("B", "84"), ("C", "85"), ("A", "86"), ("B", "87"), ("C", "88")])
        self.assertEqual(summary["semantic_warning"], "C is not DDS-equivalent and is reported separately")

    def test_matrix_explicit_s_lane(self):
        with tempfile.TemporaryDirectory() as directory:
            commands, summary = self._run_lanes(Path(directory), "--lanes", "S")
            manifest = json.loads((Path(directory) / "runs/block-1/S/manifest.json").read_text())
        self.assertEqual([c[2] for c in commands], ["S"] * 5)
        self.assertEqual(manifest["comparison_lane"], "S")
        self.assertEqual(summary["semantic_warning"], "S is not DDS-equivalent and is reported separately")

    def test_matrix_four_lane_domains_stay_unique(self):
        with tempfile.TemporaryDirectory() as directory:
            commands, summary = self._run_lanes(Path(directory), "--lanes", "A,B,C,S", "--blocks", "3")
        domains = [c[c.index("--domain") + 1] for c in commands]
        self.assertEqual(len(domains), 12)
        self.assertEqual(len(set(domains)), 12)
        self.assertEqual(summary["semantic_warning"],
                         "C is not DDS-equivalent and is reported separately; S is not DDS-equivalent and is reported separately")

    def test_matrix_rejects_invalid_lanes(self):
        with tempfile.TemporaryDirectory() as directory, patch("sys.stderr"):
            for lanes in ("A,X", "A,A", "", "ABC"):
                with self.assertRaises(SystemExit):
                    run_matrix.main(["--build", directory, "--output", directory, "--lanes", lanes])

    def test_matrix_comparison_outputs_reject_other_lane_sets(self):
        for lanes, message in (("A,B,C,S", "cannot mix lanes C and S"), ("A,C,S", "cannot mix lanes C and S"),
                               ("C,S", "cannot mix lanes C and S"), ("S", "exactly lanes A, B, C or A, B, S"),
                               ("A,S", "exactly lanes A, B, C or A, B, S"), ("A,B", "exactly lanes A, B, C or A, B, S")):
            with self.subTest(lanes), tempfile.TemporaryDirectory() as directory, \
                 patch.object(run_matrix.subprocess, "run", side_effect=AssertionError("lane started")):
                root = Path(directory)
                base = ["--build", str(root), "--output", str(root / "runs"), "--lanes", lanes]
                for extra in (["--comparison-plan", str(root / "plan.json")],
                              ["--collector-managed", "--contract", "c", "--profile", "p",
                               "--workload", "w", "--resource-scope", "r"]):
                    stderr = io.StringIO()
                    with patch("sys.stderr", stderr), self.assertRaises(SystemExit) as raised:
                        run_matrix.main(base + extra)
                    self.assertEqual(raised.exception.code, 2)
                    self.assertIn(message, stderr.getvalue())
                self.assertFalse((root / "runs").exists())

    def test_matrix_abc_keeps_emitting_plan_v1_exactly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / "build").mkdir()
            _, plan = self._run_matrix(root)
            self.assertEqual(sorted(plan), ["blocks", "contract", "order", "order_policy", "order_seed", "profile",
                                            "resource_scope", "schema", "schema_version", "workload"])
            self.assertEqual((plan["schema"], plan["schema_version"]), ("mw_perf.comparison-plan.v1", 1))

    def test_matrix_s_set_emits_plan_v2_with_canonical_lanes_and_order_kept(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / "build").mkdir()
            output, plan = self._run_matrix(root, ("--lanes", "S,A,B"))
            self.assertEqual((plan["schema"], plan["schema_version"]), ("mw_perf.comparison-plan.v2", 2))
            self.assertEqual(plan["lanes"], ["A", "B", "S"])
            self.assertEqual(plan["order"], ["S", "A", "B"])
            self.assertEqual([run["lane"] for run in plan["blocks"][0]["runs"]], ["S", "A", "B"])
            self.assertEqual(json.loads((output / "block-1/S/manifest.json").read_text())["comparison_lane"], "S")

    def test_matrix_s_plan_validates_against_collector_schema(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "tools"))
        self.addCleanup(sys.path.pop, 0)
        from mw_perf_collect.core import _schema_errors
        schema = json.loads((Path(__file__).resolve().parents[5] /
                             "tools/mw_perf_collect/schemas/comparison-plan.schema.v2.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / "build").mkdir()
            _, plan = self._run_matrix(root, ("--lanes", "A,B,S", "--contract", "c", "--profile", "p",
                                              "--workload", "w", "--resource-scope", "r"))
            self.assertEqual(_schema_errors(plan, schema), [])

    def test_matrix_collector_managed_s_lane_declares_metadata_v2(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); commands = []
            def completed(command, timeout, env=None):
                commands.append(command)
                if command[2:4] == ["mw_perf_collect", "run"]:
                    config = json.loads(Path(command[command.index("--config") + 1]).read_text())
                    lane = config["comparison"]["lane"]
                    source = Path(config["artifacts"]["latency"]["result_path"]); source.parent.mkdir(parents=True, exist_ok=True)
                    source.write_text(json.dumps({"lane": lane, "valid": True}))
                    run = Path(command[command.index("--output") + 1]) / ("canonical-" + lane); run.mkdir(parents=True)
                    (run / "validation.json").write_text(json.dumps({"valid_for_comparison": True}))
                return subprocess.CompletedProcess(command, 0)
            argv = ["--collector-managed", "--blocks", "1", "--build", str(root / "build"), "--output", str(root / "runs"),
                    "--lanes", "A,B,S", "--contract", "abc-v1", "--profile", "tree", "--workload", "4k",
                    "--resource-scope", "descendants"]
            with patch.object(run_matrix.subprocess, "run", side_effect=completed):
                self.assertEqual(run_matrix.main(argv), 0)
            config = json.loads((root / "runs/block-1/S/collector-config.json").read_text())
            self.assertEqual(config["comparison"]["schema"], "mw_perf.comparison-metadata.v2")
            self.assertEqual((config["comparison"]["lane"], config["comparison"]["order"]), ("S", ["A", "B", "S"]))
            self.assertEqual((config["artifacts"]["latency"]["lane"], config["artifacts"]["latency"]["expected_label"]),
                             ("S", "shm_send_4KB"))
            self.assertEqual(config["sut"]["command"][2], "S")
            self.assertIn("--collector-managed", config["sut"]["command"])
            lane_a = json.loads((root / "runs/block-1/A/collector-config.json").read_text())
            self.assertEqual(lane_a["comparison"]["schema"], "mw_perf.comparison-metadata.v2")
            plan = json.loads((root / "runs/comparison-plan.json").read_text())
            self.assertEqual(plan["schema"], "mw_perf.comparison-plan.v2")
            self.assertTrue(all("canonical-" in item["path"] for item in plan["blocks"][0]["runs"]))

    def test_matrix_collector_managed_abc_keeps_metadata_v1(self):
        config = run_matrix._managed_config(Path("run_block.py"), Path("/build"), Path("/work"), "C", 81, None,
            {"contract": "c", "profile": "p", "workload": "w", "resource_scope": "r"}, "abc-block-1",
            ["A", "B", "C"], "fixed", 0, 2)
        self.assertEqual(config["comparison"]["schema"], "mw_perf.comparison-metadata.v1")
        self.assertEqual(config["artifacts"]["latency"]["expected_label"], "iox_send_4KB")

    def test_clean_env_prefers_system_libstdcpp(self):
        env = clean_env()
        paths = env["LD_LIBRARY_PATH"].split(":")
        self.assertEqual(paths[0], "/usr/lib/x86_64-linux-gnu")
        self.assertFalse(any("conda" in path.lower() for path in paths))

    def test_managed_result_binds_run_pid_ticks_lane_and_csv_hash(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ,{"MW_PERF_RUN_ID":"collector-run"}):
            work=Path(directory); (work/"latency.csv").write_text(csv_text())
            write_result(work,{"lane":"A","valid":True},True)
            identity=json.loads((work/"result.json").read_text())["collector_managed"]
            self.assertEqual(identity["run_id"],"collector-run"); self.assertEqual(identity["producer_pid"],os.getpid())
            self.assertGreater(identity["producer_start_time_ticks"],0); self.assertTrue(identity["complete"])

    def test_status_schema_rejects_missing_null_and_arrays(self):
        self.assertFalse(validate_status_schema(None)[0])
        self.assertFalse(validate_status_schema([])[0])
        self.assertFalse(validate_status_schema({"valid": True})[0])
        self.assertFalse(validate_status_schema({"valid": True, "attempted_measured": 3000, "received_measured": 3000, "unique_measured": 3000, **{k: None for k in ("missing", "duplicate", "out_of_order", "invalid_payload", "invalid_timestamp")}})[0])

    def test_status_schema_uses_unique_measured(self):
        status = {"valid": True, "attempted_measured": 3000, "received_measured": 3000,
                  "unique_measured": 3000, "missing": 0, "duplicate": 0,
                  "out_of_order": 0, "invalid_payload": 0, "invalid_timestamp": 0}
        self.assertTrue(validate_status_schema(status)[0])
        self.assertFalse(validate_status_schema({**status, "unique_measured": None})[0])
        legacy=dict(status); legacy["unique"]=legacy.pop("unique_measured")
        self.assertFalse(validate_status_schema(legacy)[0])
        legacy=dict(status); legacy["sent_measured"]=legacy.pop("attempted_measured")
        self.assertFalse(validate_status_schema(legacy)[0])

    def test_result_gate_rejects_null_status_without_exception(self):
        valid, validation_errors = validate_result_gate(
            "A", None, {"valid": False}, {"publisher": 0, "subscriber": 0},
            0, (),
        )
        self.assertFalse(valid)
        self.assertIn("status_not_object", validation_errors)

    def test_c_gate_and_signal_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); csv=root/"latency.csv"; status=root/"status.json"; pub=root/"publisher.status.json"
            csv.write_text(csv_text("iox_send_4KB")); payload={"valid":True,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0}; publisher={"completion":True,"attempted_total":3050,"attempted_measured":3000,"valid":True}; status.write_text(json.dumps(payload)); pub.write_text(json.dumps(publisher));
            self.assertTrue(validate_result_gate("C",payload,validate_latency_csv(csv,"C"),{"publisher":-2,"subscriber":0,"roudi":0},0,(csv,status,pub),publisher_status=publisher)[0])
            self.assertFalse(validate_result_gate("C",payload,validate_latency_csv(csv,"C"),{"publisher":-9,"subscriber":0,"roudi":0},0,(csv,status,pub),publisher_status=publisher)[0])

    def test_s_gate_needs_no_roudi_and_keeps_publisher_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); csv=root/"latency.csv"; status=root/"status.json"; pub=root/"publisher.status.json"
            csv.write_text(csv_text("shm_send_4KB")); payload={"valid":True,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0}; publisher={"completion":True,"attempted_total":3050,"attempted_measured":3000,"valid":True}; status.write_text(json.dumps(payload)); pub.write_text(json.dumps(publisher))
            def gate(exits=None,publisher_status=publisher,csv_result=None):
                return validate_result_gate("S",payload,csv_result or validate_latency_csv(csv,"S"),exits or {"publisher":0,"subscriber":0},0,(csv,status,pub),publisher_status=publisher_status)
            self.assertEqual(gate(),(True,[]))
            self.assertTrue(gate({"publisher":-2,"subscriber":-2})[0])
            self.assertFalse(gate({"publisher":-9,"subscriber":0})[0])
            self.assertFalse(gate({"publisher":0})[0])
            self.assertFalse(gate(publisher_status=None)[0])
            self.assertFalse(gate(publisher_status={**publisher,"attempted_total":3049})[0])
            csv.write_text(csv_text("iox_send_4KB"))
            self.assertFalse(gate()[0])

    def test_publisher_status_schema_is_independent(self):
        publisher={"completion":True,"attempted_total":3050,"attempted_measured":3000,"valid":True}
        self.assertTrue(validate_publisher_status_schema(publisher)[0])
        self.assertFalse(validate_publisher_status_schema({**publisher,"completion":"true"})[0])

    def test_c_rejects_missing_publisher_artifact(self):
        self.assertFalse(self.c_gate(None, include_publisher_artifact=False)[0])

    def test_c_rejects_malformed_publisher_status(self):
        self.assertFalse(self.c_gate({"completion":True})[0])

    def test_c_rejects_incomplete_publisher_status(self):
        self.assertFalse(self.c_gate({"completion":False,"attempted_total":100,"attempted_measured":50,"valid":False})[0])

    def test_c_rejects_wrong_publisher_count(self):
        self.assertFalse(self.c_gate({"completion":True,"attempted_total":3049,"attempted_measured":2999,"valid":True})[0])
        self.assertFalse(self.c_gate({"completion":True,"attempted_total":3049,"attempted_measured":3000,"valid":True})[0])

    def c_gate(self, publisher_status, include_publisher_artifact=True):
        root=Path(self._testMethodName + "-artifacts"); self.addCleanup(lambda: __import__("shutil").rmtree(root,ignore_errors=True)); root.mkdir(exist_ok=True)
        csv=root/"latency.csv"; status=root/"subscriber.status.json"; pub=root/"publisher.status.json"; csv.write_text(csv_text("iox_send_4KB")); subscriber={"valid":True,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0}; status.write_text(json.dumps(subscriber)); artifacts=[csv,status]
        if include_publisher_artifact: pub.write_text(json.dumps(publisher_status)); artifacts.append(pub)
        return validate_result_gate("C",subscriber,validate_latency_csv(csv,"C"),{"publisher":0,"subscriber":0,"roudi":0},0,artifacts,publisher_status=publisher_status)
    def write(self, text):
        f=tempfile.NamedTemporaryFile(mode="w",suffix=".csv",delete=False); f.write(text); f.close(); self.addCleanup(lambda: Path(f.name).unlink(missing_ok=True)); return Path(f.name)
    def test_strict_csv_cases(self):
        self.assertTrue(validate_latency_csv(self.write(csv_text()),"A")["valid"])
        for text in ("bad\n1\n",csv_text("wrong"),csv_text(value="nan"),csv_text()+csv_text(),csv_text().splitlines()[0]+"\n"):
            self.assertFalse(validate_latency_csv(self.write(text),"A")["valid"])
    def test_result_gate_exit_codes_and_freshness(self):
        with tempfile.TemporaryDirectory() as directory:
            path,status=Path(directory)/"latency.csv",Path(directory)/"status.json"; path.write_text(csv_text()); status.write_text(json.dumps({"valid":True,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0})); started=0
            self.assertTrue(validate_result_gate("A",json.loads(status.read_text()),validate_latency_csv(path,"A"),{"publisher":0,"subscriber":0},started,(path,status))[0])
            self.assertFalse(validate_result_gate("A",json.loads(status.read_text()),validate_latency_csv(path,"A"),{"publisher":1,"subscriber":0},started,(path,status))[0]); self.assertFalse(freshness_ok((path,),time.time_ns()+1)[0])
if __name__ == "__main__": unittest.main()
