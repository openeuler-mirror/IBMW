import json, os, stat, subprocess, sys, tempfile, textwrap, time, unittest
from unittest.mock import patch
from pathlib import Path
from run_block import CSV_SCHEMA, clean_env, freshness_ok, patch_iox_config, resolve_roudi, validate_latency_csv, validate_publisher_status_schema, validate_result_gate, validate_status_schema, write_result
import run_matrix

def csv_text(label="abc_ros2_4KB", count="3000", value="1.0"):
    return ",".join(CSV_SCHEMA) + "\n" + ",".join([label, count] + [value] * 8) + "\n"

class RunBlockValidationTest(unittest.TestCase):
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
