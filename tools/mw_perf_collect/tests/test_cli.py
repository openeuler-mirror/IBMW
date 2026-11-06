import copy, hashlib, json, os, subprocess, sys, tempfile, unittest
from unittest.mock import patch
from pathlib import Path
import mw_perf_collect.core as core
from mw_perf_collect.cli import main

class CliTest(unittest.TestCase):
    def test_ohos_import_cli_emits_bounded_summary_and_exclusive_output(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            result = root / "result.json"
            request = root / "request.json"
            archive = root / "capture.tar"
            archive.write_bytes(b"archive")
            result_value = {"archive_sha256": "a" * 64, "archive_size": 7,
                            "child_pid": 12, "child_start_time_ticks": 34}
            request_value = {"request_challenge": "b" * 64, "run_id": "run",
                             "capability_id": "cap", "path_id": "path",
                             "topology_generation": "gen", "topology_sha256": "c" * 64}
            result.write_text(json.dumps(result_value))
            request.write_text(json.dumps(request_value))
            imported = {"semantic_validation": True, "formal_p0a": False,
                        "imported_semantics": {"status": "supported", "record_count": 3,
                                               "stream_count": 1, "segment_count": 1,
                                               "raw_stage_events": [{"large": "payload"}]}}
            output = root / "summary.json"
            with patch("mw_perf_collect.cli.validate_capture_semantics",
                       return_value=imported):
                self.assertEqual(main(["import-ohos-capture", "--archive", str(archive),
                                       "--result", str(result), "--request", str(request),
                                       "--output", str(output)]), 0)
                self.assertEqual(main(["import-ohos-capture", "--archive", str(archive),
                                       "--result", str(result), "--request", str(request),
                                       "--output", str(output)]), core.EXIT_INVALID)
            document = json.loads(output.read_text())
            self.assertTrue(document["semantic_validation"])
            self.assertFalse(document["formal_p0a"])
            self.assertEqual(document["counts"]["stage_records"], 3)
            self.assertNotIn("raw_stage_events", output.read_text())
            self.assertNotIn("directory_finalization", document)
            self.assertNotIn("valid_for_metric", document)
            self.assertNotIn("valid_for_comparison", document)

    def test_ohos_import_cli_rejects_duplicate_json_and_symlink(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); archive = root / "capture.tar"; archive.write_bytes(b"x")
            request = root / "request.json"; request.write_text("{}")
            duplicate = root / "duplicate.json"
            duplicate.write_text('{"protocol":"one","protocol":"two"}')
            with patch("mw_perf_collect.cli.validate_capture_semantics") as validator:
                self.assertEqual(main(["import-ohos-capture", "--archive", str(archive),
                                       "--result", str(duplicate), "--request", str(request)]),
                                 core.EXIT_INVALID)
                validator.assert_not_called()
            target = root / "result.json"; target.write_text("{}")
            link = root / "result-link.json"; link.symlink_to(target)
            self.assertEqual(main(["import-ohos-capture", "--archive", str(archive),
                                   "--result", str(link), "--request", str(request)]),
                             core.EXIT_INVALID)

    def _formal_run(self, root, lane, block="b1"):
        path = root / lane
        path.mkdir(parents=True)
        manifest = {
            "run_id": lane, "comparison_lane": lane,
            "scenario_contract_hash": "contract", "profile": "profile",
            "workload": "payload-4k-3000", "resource_scope": "sut-only",
            "order": ["A", "B", "C"], "comparison_block_id": block,
        }
        (path / "manifest.json").write_text(json.dumps(manifest))
        (path / "validation.json").write_text(json.dumps({"valid_for_comparison": True}))
        return path

    def _real_formal_run(self, root, lane, block="b1", lanes="ABC", label=None):
        path = root / lane
        path.mkdir(parents=True)
        raw = path / "raw"
        raw.mkdir()
        process = {"timestamp": 1.0, "processes": [{"pid": 1, "cpu_seconds": 0.1,
            "rss_bytes": 10, "pss_bytes": None, "threads": 1, "start_time_ticks": 2}]}
        network = {"timestamp": 1.0, "interfaces": {"eth0": {"rx_bytes": 1,
            "tx_bytes": 2, "rx_packets": 3, "tx_packets": 4}}}
        (raw / "process.jsonl").write_text(json.dumps(process) + "\n")
        (raw / "network.jsonl").write_text(json.dumps(network) + "\n")
        label = label or {"A": "abc_ros2_4KB", "B": "abc_dds_4KB", "C": "iox_send_4KB",
                          "S": "shm_send_4KB"}[lane]
        csv_data = ("label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\n"
                    + f"{label},3000,1,2,1,1,2,3,4,5\n")
        (raw / "latency.csv").write_text(csv_data)
        csv_sha = hashlib.sha256((raw / "latency.csv").read_bytes()).hexdigest()
        lane_result = {"lane": lane, "valid": True, "collector_managed": {
            "schema": "ibmw.abc.collector-managed.v1", "run_id": lane,
            "producer_pid": 1, "producer_start_time_ticks": 2, "lane": lane,
            "complete": True, "latency_csv_sha256": csv_sha}}
        (raw / "lane_result.json").write_text(json.dumps(lane_result))
        result_sha = hashlib.sha256((raw / "lane_result.json").read_bytes()).hexdigest()
        final = {name: {"exists": True, "path": f"/source/{name}", "mtime_ns": 2_000_000_000,
            "size": (raw / ("latency.csv" if name == "csv" else "lane_result.json")).stat().st_size,
            "inode": 1, "sha256": csv_sha if name == "csv" else result_sha}
            for name in ("csv", "result")}
        manifest = {"schema_version": 1, "tool_version": "test", "run_id": lane,
            "adapter": "generic", "mode": "managed", "run_status": "completed",
            "scenario_contract_hash": "0" * 64, "metric_availability": "supported",
            "measurement_kind": "latency_summary_csv", "start_time": 1.0, "end_time": 2.0,
            "pid": 1, "pid_start_time_ticks": 2, "states": ["SAMPLE"],
            "sample_count": 1, "metrics_valid": True, "adapter_readiness": True,
            "sample_artifacts": core._sample_artifact_evidence(path),
            "comparison_schema": "mw_perf.comparison-metadata.v2" if "S" in lanes
                else "mw_perf.comparison-metadata.v1", "sequence_index": lanes.index(lane),
            "comparison_lane": lane, "comparison_block_id": block, "contract": "0" * 64,
            "profile": "profile", "workload": "workload", "resource_scope": "scope",
            "order": list(lanes), "resource_collection": {
                "include_descendants": True, "network_interfaces": ["lo"]},
            "artifacts": {"latency": {"schema": "mw_perf.latency.csv.v1", "required": True,
                "status": "supported", "label": label, "record_count": 3000,
                "content_sha256": csv_sha, "provenance": {"run_id": lane, "mode": "managed",
                    "sut_pid": 1, "sut_start_time_ticks": 2, "lane": lane,
                    "source_sha256": csv_sha, "archive_sha256": csv_sha,
                    "lane_result_sha256": result_sha, "final": final,
                    "graceful_finalization": True, "lane_result_identity_verified": True}}}}
        (path / "manifest.json").write_text(json.dumps(manifest))
        validation = core.validate_dir(path)
        (path / "validation.json").write_text(json.dumps(validation, indent=2))
        return path

    def _plan(self, root, blocks):
        plan = root / "plan.json"
        plan.write_text(json.dumps({"schema": "mw_perf.comparison-plan.v1", "schema_version": 1,
            "contract": "0" * 64, "profile": "profile", "workload": "workload",
            "resource_scope": "scope", "order": ["A", "B", "C"], "blocks": blocks}))
        return plan

    def test_formal_compare_uses_real_validate_dir_and_valid_blocks(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = [self._real_formal_run(root / "b1", lane) for lane in "ABC"]
            plan = self._plan(root, [{"block_id": "b1", "runs": [
                {"lane": lane, "path": str(path)} for lane, path in zip("ABC", runs)]}])
            self.assertEqual(main(["compare", "--plan", str(plan), "--valid-blocks", "1",
                                   "--output", str(root / "out")]), 0)
            result = json.loads((root / "out/comparison.json").read_text())
            self.assertEqual(result["valid_blocks"], ["b1"])
            self.assertEqual(main(["compare", "--plan", str(plan), "--valid-blocks", "2",
                                   "--output", str(root / "out2")]), core.EXIT_INVALID)
            result = json.loads((root / "out2/comparison.json").read_text())
            self.assertIn("INSUFFICIENT_VALID_BLOCKS", result["failure_codes"])

    def test_formal_compare_checks_optional_order_policy_and_seed(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = [self._formal_run(root / "b1", lane) for lane in "ABC"]
            for path in runs:
                manifest = json.loads((path / "manifest.json").read_text())
                manifest.update(order_policy="seeded-fixed", order_seed=17)
                (path / "manifest.json").write_text(json.dumps(manifest))
            plan = self._plan(root, [{"block_id": "b1", "runs": [
                {"lane": lane, "path": str(path)} for lane, path in zip("ABC", runs)]}])
            value = json.loads(plan.read_text())
            value.update(order_policy="seeded-fixed", order_seed=18)
            plan.write_text(json.dumps(value))
            with patch("mw_perf_collect.cli.validate_dir",
                       return_value={"valid_for_comparison": True}):
                self.assertEqual(main(["compare", "--plan", str(plan),
                                       "--output", str(root / "out")]), core.EXIT_INVALID)
            reasons = json.loads((root / "out/comparison.json").read_text())["failure_codes"]
            self.assertIn("ORDER_SEED_MISMATCH:A", reasons)

    def test_formal_compare_rejects_block_id_and_path_aliases(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            paths = [self._real_formal_run(root / lane, lane) for lane in "ABC"]
            manifest = json.loads((paths[0] / "manifest.json").read_text())
            manifest["comparison_block_id"] = "b2"
            (paths[0] / "manifest.json").write_text(json.dumps(manifest))
            alias = root / "alias"
            alias.symlink_to(paths[0], target_is_directory=True)
            plan = self._plan(root, [{"block_id": "b1", "runs": [
                {"lane": "A", "path": str(alias)}, {"lane": "B", "path": str(paths[1])},
                {"lane": "C", "path": str(paths[2])}]}])
            self.assertEqual(main(["compare", "--plan", str(plan), "--output", str(root / "out")]),
                             core.EXIT_INVALID)
            reasons = json.loads((root / "out/comparison.json").read_text())[
                "invalid_blocks"][0]["reasons"]
            self.assertIn("PATH_SYMLINK_OR_ALIAS:A", reasons)
            self.assertIn("BLOCK_ID_MISMATCH:A", reasons)

    def test_formal_compare_rejects_global_path_identity_and_run_id_reuse(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            paths = [self._formal_run(root / "runs", lane) for lane in "ABC"]
            blocks = [{"block_id": block, "runs": [
                {"lane": lane, "path": str(path)} for lane, path in zip("ABC", paths)]}
                for block in ("b1", "b2")]
            plan = self._plan(root, blocks)
            with patch("mw_perf_collect.cli.validate_dir",
                       return_value={"valid_for_comparison": True}):
                self.assertEqual(main(["compare", "--plan", str(plan),
                                       "--output", str(root / "out")]), core.EXIT_INVALID)
            result = json.loads((root / "out/comparison.json").read_text())
            self.assertTrue(any(code.startswith("PATH_DUPLICATE:")
                                for code in result["failure_codes"]))
            self.assertTrue(any(code.startswith("DEVICE_INODE_DUPLICATE:")
                                for code in result["failure_codes"]))
            self.assertTrue(any(code.startswith("RUN_ID_DUPLICATE:")
                                for code in result["failure_codes"]))

    def test_comparison_schema_rejects_empty_contract_fields_and_long_order(self):
        schema = json.loads((Path(core.__file__).parent /
                             "schemas/comparison-plan.schema.v1.json").read_text())
        plan = {"schema": "mw_perf.comparison-plan.v1", "schema_version": 1,
            "contract": "contract", "profile": "profile", "workload": "workload",
            "resource_scope": "scope", "order": ["A", "B", "C"], "blocks": [{
                "block_id": "b1", "runs": [{"lane": lane, "path": "/run/" + lane}
                    for lane in "ABC"]}]}
        for field in ("contract", "profile", "workload", "resource_scope"):
            invalid = copy.deepcopy(plan)
            invalid[field] = {}
            self.assertTrue(core._schema_errors(invalid, schema), field)
        invalid = copy.deepcopy(plan)
        invalid["order"].append("A")
        self.assertIn("$.order:maxItems", core._schema_errors(invalid, schema))

    def test_compare_malformed_or_non_object_plan_returns_tool_error(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for content in ("{broken", "[]"):
                plan = root / "plan.json"
                plan.write_text(content)
                output = root / ("out" + str(len(content)))
                self.assertEqual(main(["compare", "--plan", str(plan), "--output", str(output)]),
                                 core.EXIT_INVALID)
                result = json.loads((output / "comparison.json").read_text())
                self.assertFalse(result["valid_for_comparison"])

    def test_both_compare_modes_explicitly_reject_metric_qualification(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); qualification = root / "qualification"
            qualification.mkdir()
            for name in ("declaration.json", "latency_samples.jsonl",
                         "qualification_seal.json"):
                (qualification / name).write_text("{}\n")
            (qualification / "metric_qualification.json").write_text(json.dumps({
                "schema": "mw_perf.metric_qualification.v1"}) + "\n")
            legacy = root / "legacy.json"
            legacy.write_text(json.dumps({"runs": [str(qualification)]}))
            self.assertEqual(main(["compare", "--plan", str(legacy), "--output",
                                   str(root / "legacy-out")]), core.EXIT_INVALID)
            legacy_result = json.loads((root / "legacy-out/comparison.json").read_text())
            self.assertIn("METRIC_QUALIFICATION_NOT_COMPARABLE",
                          legacy_result["failure_codes"])

            plan = self._plan(root, [{"block_id": "b1", "runs": [
                {"lane": lane, "path": str(qualification)} for lane in "ABC"]}])
            self.assertEqual(main(["compare", "--plan", str(plan), "--output",
                                   str(root / "formal-out")]), core.EXIT_INVALID)
            reasons = json.loads((root / "formal-out/comparison.json").read_text())["failure_codes"]
            self.assertTrue(any(reason.startswith("METRIC_QUALIFICATION_NOT_COMPARABLE:")
                                for reason in reasons))

    def test_formal_compare_requires_exact_abc_and_reports_block_reason(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            a = self._formal_run(root, "A")
            plan = root / "plan.json"
            plan.write_text(json.dumps({
                "schema": "mw_perf.comparison-plan.v1", "schema_version": 1,
                "contract": "contract", "profile": "profile",
                "workload": "payload-4k-3000", "resource_scope": "sut-only",
                "order": ["A", "B", "C"],
                "blocks": [{"block_id": "b1", "runs": [{"lane": "A", "path": str(a)}]}],
            }))
            with patch("mw_perf_collect.cli.validate_dir", return_value={"valid_for_comparison": True}):
                self.assertEqual(main(["compare", "--plan", str(plan), "--output", str(root / "out")]), core.EXIT_INVALID)
            result = json.loads((root / "out/comparison.json").read_text())
            self.assertEqual(result["invalid_blocks"][0]["reasons"],
                             ["LANES_MISSING:B,C", "LANES_NOT_EXACTLY_ABC", "ORDER_SEQUENCE_MISMATCH"])
            self.assertFalse(result["valid_for_comparison"])

    def test_formal_compare_rejects_mismatched_scope_and_duplicate_lane(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            a = self._formal_run(root, "A")
            b = self._formal_run(root, "B")
            c = self._formal_run(root, "C")
            manifest = json.loads((c / "manifest.json").read_text())
            manifest["resource_scope"] = "host"
            (c / "manifest.json").write_text(json.dumps(manifest))
            plan = root / "plan.json"
            plan.write_text(json.dumps({
                "schema": "mw_perf.comparison-plan.v1", "schema_version": 1,
                "contract": "contract", "profile": "profile", "workload": "payload-4k-3000",
                "resource_scope": "sut-only", "order": ["A", "B", "C"],
                "blocks": [{"block_id": "b1", "runs": [
                    {"lane": "A", "path": str(a)}, {"lane": "A", "path": str(b)}, {"lane": "C", "path": str(c)}]}],
            }))
            with patch("mw_perf_collect.cli.validate_dir", return_value={"valid_for_comparison": True}):
                self.assertEqual(main(["compare", "--plan", str(plan), "--output", str(root / "out")]), core.EXIT_INVALID)
            reasons = json.loads((root / "out/comparison.json").read_text())["invalid_blocks"][0]["reasons"]
            self.assertIn("LANE_DUPLICATE:A", reasons)
            self.assertIn("LANES_MISSING:B", reasons)
            self.assertIn("RESOURCE_SCOPE_MISMATCH:C", reasons)

    def test_validate_profile_p0a_fails_closed_independently_of_manifest_fields(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)
            self.assertEqual(main(["validate", "--profile", "standard", str(path)]), core.EXIT_INVALID)
            result = json.loads((path / "validation.json").read_text())
            self.assertEqual(result["validation_profile"], "standard")
            self.assertEqual(main(["validate", "--profile", "p0a", str(path)]), core.EXIT_INVALID)
            result = json.loads((path / "validation.json").read_text())
            self.assertEqual(result["validation_profile"], "p0a")
            self.assertEqual(result["failure_codes"], ["MANIFEST_MISSING"])

    def test_ros2_sut_exit_before_pid_capture_preserves_failure_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cfg = root / "run.json"
            cfg.write_text(json.dumps({
                "sut": {"mode": "managed", "command": [str(root / "missing-sut")]},
                "adapters": {"ros2": {"latency_artifact": {
                    "path": str(root / "missing.jsonl"), "required": True}}},
            }))
            self.assertNotEqual(main(["run", "--adapter", "ros2", "--config", str(cfg),
                                      "--output", str(root / "runs")]), 0)
            run = next((root / "runs").iterdir())
            manifest = json.loads((run / "manifest.json").read_text())
            self.assertIn("FileNotFoundError", manifest["error"])
            self.assertNotIn("KeyError", manifest["error"])
            self.assertTrue((run / "validation.json").exists())

    def test_run_command_separator_is_not_passed_to_sut(self):
        with tempfile.TemporaryDirectory() as td:
            cfg=Path(td)/"run.json"; cfg.write_text(json.dumps({"run":{"sample_seconds":.01},"sut":{"mode":"managed"}}))
            self.assertEqual(main(["run","--config",str(cfg),"--output",td,"--",sys.executable,"-c","import time; time.sleep(.1)"]),0)

    def test_fake_sut_archive_timeout_and_attached_not_stopped(self):
        with tempfile.TemporaryDirectory() as td:
            cfg=Path(td)/"run.json"; cfg.write_text(json.dumps({"run":{"sample_seconds":.1,"stop_timeout_seconds":.1},"sut":{"mode":"managed","command":[sys.executable,"-c","import time; time.sleep(10)"]}}))
            self.assertEqual(main(["run","--config",str(cfg),"--output",td]),0)
            run=next(p for p in Path(td).iterdir() if p.is_dir() and p.name != "__pycache__")
            self.assertTrue((run/"manifest.json").exists()); self.assertTrue((run/"raw/process.jsonl").read_text()); self.assertTrue((run/"report.md").exists())
            proc=subprocess.Popen([sys.executable,"-c","import time; time.sleep(2)"])
            try:
                cfg.write_text(json.dumps({"run":{"sample_seconds":.05},"sut":{"mode":"attached","pid":proc.pid}}))
                self.assertEqual(main(["run","--adapter","ibmw","--config",str(cfg),"--output",td]),0); self.assertIsNone(proc.poll())
                attached=max((p for p in Path(td).iterdir() if p.is_dir()),key=lambda p:p.stat().st_mtime)
                self.assertTrue((attached/"environment/ibmw.json").exists())
                self.assertEqual(json.loads((attached/"capability/ibmw.json").read_text())["core_metrics"]["status"],"unsupported")
            finally: proc.terminate(); proc.wait()

    def test_compare_rejects_forged_validation(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); run=root/"run"; (run/"raw").mkdir(parents=True)
            manifest={"schema_version":1,"tool_version":"x","run_id":"run","adapter":"generic","mode":"managed","run_status":"completed","scenario_contract_hash":"a"*64,"metric_availability":"supported","measurement_kind":"metadata","start_time":1,"end_time":2,"pid":1,"pid_start_time_ticks":1,"states":[],"sample_count":1,"metrics_valid":True,"adapter_readiness":True,"profile":"p","block":"b","order_index":1}
            (run/"manifest.json").write_text(json.dumps(manifest)); (run/"raw/process.jsonl").write_text("{}\n"); (run/"raw/network.jsonl").write_text("{}\n")
            (run/"validation.json").write_text(json.dumps({"valid_for_comparison":True}))
            plan=root/"plan.json"; plan.write_text(json.dumps({"runs":[str(run)]}))
            self.assertNotEqual(main(["compare","--plan",str(plan),"--output",str(root/"comparison")]),0)
            result=json.loads((root/"comparison/comparison.json").read_text())
            self.assertIn("VALIDATION_STALE_OR_TAMPERED",result["failure_codes"])

    def test_managed_ibmw_archives_metrics_after_finalization(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); metrics=root/"core"; metrics.mkdir()
            record={"schema":"ibmw.core_metrics.v1","snapshot_seq":1,"kind":"fake","cumulative":{},"complete":True}
            script="import json,signal,time,sys\np=sys.argv[1]\ndef stop(*_):\n open(p,'w').write(json.dumps(" + repr(record) + ")+\"\\n\");sys.exit(0)\nsignal.signal(signal.SIGTERM,stop)\nwhile True: time.sleep(.01)"
            cfg=root/"run.json"; cfg.write_text(json.dumps({"run":{"sample_seconds":.05},"sut":{"mode":"managed","command":[sys.executable,"-c",script,str(metrics/"metrics.jsonl")]},"adapters":{"ibmw":{"root":str(root),"core_metrics":{"directory":str(metrics),"required":True}}}}))
            self.assertEqual(main(["run","--adapter","ibmw","--config",str(cfg),"--output",str(root/"runs")]),0)
            run=next((root/"runs").iterdir()); manifest=json.loads((run/"manifest.json").read_text())
            self.assertEqual(manifest["core_metrics"]["status"],"supported")
            self.assertEqual(manifest["core_metrics"]["record_count"],1)
            self.assertTrue((run/"raw/core_metrics.jsonl").exists())

    def test_managed_ros2_fake_producer_archives_latency_after_finalization(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); artifact = root / "latency.jsonl"
            record = {"schema": "mw_perf.latency.v1", "seq": 0, "send_ns": 10,
                      "receive_ns": 20, "latency_ns": 10, "topic": "/chatter",
                      "type": "std_msgs/msg/String", "payload_bytes": 4, "valid": True}
            script = "import json,signal,time,sys,os\n"
            script += "p=sys.argv[1]\n"
            script += "def ticks(): return int(open('/proc/self/stat').read().split()[21])\n"
            script += "def stop(*_):\n"
            script += " r=" + repr(record) + "\n"
            script += " c={'schema':os.environ['MW_PERF_ARTIFACT_SCHEMA_VERSION'],'kind':'completion','run_id':os.environ['MW_PERF_RUN_ID'],'producer_pid':os.getpid(),'producer_start_time_ticks':ticks(),'record_count':1,'topic':r['topic'],'type':r['type'],'complete':True,'clock_domain':'producer_monotonic'}\n"
            script += " open(p,'w').write(json.dumps(r)+'\\n'+json.dumps(c)+'\\n');sys.exit(0)\n"
            script += "signal.signal(signal.SIGTERM,stop)\n"
            script += "while True: time.sleep(.01)"
            cfg = root / "run.json"
            cfg.write_text(json.dumps({"run": {"sample_seconds": .05},
                "sut": {"mode": "managed", "command": [sys.executable, "-c", script, str(artifact)]},
                "adapters": {"ros2": {"latency_artifact": {"path": str(artifact), "required": True}}}}))
            self.assertEqual(main(["run", "--adapter", "ros2", "--config", str(cfg), "--output", str(root / "runs")]), 0)
            run = next((root / "runs").iterdir())
            manifest = json.loads((run / "manifest.json").read_text())
            self.assertEqual(manifest["latency_artifact"]["status"], "supported")
            self.assertTrue((run / "raw/latency.jsonl").exists())

    def test_optional_supported_latency_is_revalidated_after_collection(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); artifact = root / "latency.jsonl"
            record = {"schema": "mw_perf.latency.v1", "seq": 0, "send_ns": 10,
                      "receive_ns": 20, "latency_ns": 10, "topic": "/chatter",
                      "type": "std_msgs/msg/String", "payload_bytes": 4, "valid": True}
            script = "import json,signal,time,sys,os\np=sys.argv[1]\n"
            script += "def ticks(): return int(open('/proc/self/stat').read().split()[21])\n"
            script += "def stop(*_):\n r=" + repr(record) + "\n"
            script += " c={'schema':os.environ['MW_PERF_ARTIFACT_SCHEMA_VERSION'],'kind':'completion','run_id':os.environ['MW_PERF_RUN_ID'],'producer_pid':os.getpid(),'producer_start_time_ticks':ticks(),'record_count':1,'topic':r['topic'],'type':r['type'],'complete':True,'clock_domain':'producer_monotonic'}\n"
            script += " open(p,'w').write(json.dumps(r)+'\\n'+json.dumps(c)+'\\n');sys.exit(0)\n"
            script += "signal.signal(signal.SIGTERM,stop)\nwhile True: time.sleep(.01)"
            cfg = root / "run.json"
            cfg.write_text(json.dumps({"run": {"sample_seconds": .05},
                "sut": {"mode": "managed", "command": [sys.executable, "-c", script, str(artifact)]},
                "adapters": {"ros2": {"latency_artifact": {"path": str(artifact)}}}}))
            self.assertEqual(main(["run", "--adapter", "ros2", "--config", str(cfg),
                                   "--output", str(root / "runs")]), 0)
            run = next((root / "runs").iterdir()); manifest_path = run / "manifest.json"
            archive = run / "raw/latency.jsonl"; original_raw = archive.read_bytes()
            original_manifest = json.loads(manifest_path.read_text())
            self.assertFalse(original_manifest["latency_artifact"]["required"])
            self.assertTrue(core.validate_dir(run)["valid_for_comparison"])

            for mutation in ("raw", "hash", "provenance", "count"):
                archive.write_bytes(original_raw)
                manifest = copy.deepcopy(original_manifest)
                if mutation == "raw":
                    archive.write_bytes(original_raw + b"{}\n")
                elif mutation == "hash":
                    manifest["latency_artifact"]["content_sha256"] = "0" * 64
                elif mutation == "provenance":
                    manifest["latency_artifact"]["provenance"]["run_id"] = "forged"
                else:
                    manifest["latency_artifact"]["record_count"] += 1
                manifest_path.write_text(json.dumps(manifest))
                self.assertFalse(core.validate_dir(run)["valid_for_comparison"], mutation)

    def test_optional_absent_does_not_turn_metadata_into_latency_measurement(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); cfg = root / "run.json"
            cfg.write_text(json.dumps({"run": {"sample_seconds": .01},
                "sut": {"mode": "managed", "command": [sys.executable, "-c", "import time; time.sleep(.1)"]}}))
            self.assertEqual(main(["run", "--config", str(cfg), "--output", str(root / "runs")]), 0)
            run = next((root / "runs").iterdir())
            manifest = json.loads((run / "manifest.json").read_text())
            self.assertEqual(manifest["measurement_kind"], "metadata")
            self.assertEqual(manifest["sample_artifacts"]["process"]["record_count"], manifest["sample_count"])
            self.assertEqual(manifest["sample_artifacts"]["network"]["record_count"], manifest["sample_count"])
            self.assertEqual(manifest["sample_artifacts"]["process"]["relative_path"], "raw/process.jsonl")
            self.assertEqual(manifest["sample_artifacts"]["network"]["relative_path"], "raw/network.jsonl")
            self.assertTrue(core.validate_dir(run)["valid_for_comparison"])

    def test_installed_cli_smokes_both_adapter_doctors(self):
        """The installed entry point must load both adapter module layouts."""
        repository = Path(__file__).parents[3]
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cmake_source = root / "install-project"
            cmake_source.mkdir()
            (cmake_source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.24)\n"
                "project(ibmw LANGUAGES NONE)\n"
                "include(GNUInstallDirs)\n"
                "set(IBMW_INSTALL ON)\n"
                f"add_subdirectory(\"{repository / 'tools/mw_perf_collect'}\" collector)\n")
            build = root / "cmake-build"
            prefix = root / "prefix"
            subprocess.run(["cmake", "-S", str(cmake_source), "-B", str(build)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix),
                            "--component", "mw_perf_collect"], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

            executable = prefix / "bin/mw-perf-collect"
            config = root / "doctor.json"
            config.write_text(json.dumps({
                "sut": {"root": str(root / "empty-ibmw")},
                "adapters": {"ros2": {
                    "ros2_executable": str(root / "missing-ros2")},
                    "ibmw": {"root": str(root / "empty-ibmw")}},
            }))
            (root / "empty-ibmw").mkdir()
            for adapter in ("ros2", "ibmw"):
                result = subprocess.run(
                    [str(executable), "doctor", "--adapter", adapter,
                     "--config", str(config)],
                    check=False, capture_output=True, text=True,
                    env={key: value for key, value in os.environ.items()
                         if key not in ("PYTHONPATH", "PYTHONHOME")},
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                document = json.loads(result.stdout)
                self.assertEqual(document["adapter"], adapter)
                self.assertIn(document["status"], {"ready", "degraded", "unsupported"})

    def test_installed_cli_matrix_loads_fixtures_without_python_environment(self):
        """Installed validate/report/compare must work without repository imports."""
        repository = Path(__file__).parents[3]
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cmake_source = root / "install-project"
            cmake_source.mkdir()
            (cmake_source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.24)\n"
                "project(ibmw LANGUAGES NONE)\n"
                "include(GNUInstallDirs)\n"
                "set(IBMW_INSTALL ON)\n"
                f"add_subdirectory(\"{repository / 'tools/mw_perf_collect'}\" collector)\n")
            build = root / "cmake-build"
            prefix = root / "prefix"
            subprocess.run(["cmake", "-S", str(cmake_source), "-B", str(build)],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix),
                            "--component", "mw_perf_collect"], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

            executable = prefix / "bin/mw-perf-collect"
            env = {key: value for key, value in os.environ.items()
                   if key not in ("PYTHONPATH", "PYTHONHOME")}
            runs = [self._real_formal_run(root / "runs", lane) for lane in "ABC"]
            plan = self._plan(root, [{"block_id": "b1", "runs": [
                {"lane": lane, "path": str(path)} for lane, path in zip("ABC", runs)]}])

            for run_path in runs:
                checked = subprocess.run(
                    [str(executable), "validate", str(run_path)], env=env,
                    check=False, capture_output=True, text=True)
                self.assertEqual(checked.returncode, 0, checked.stderr)
                validation = json.loads(checked.stdout)
                self.assertTrue(validation["valid_for_metric"])
                self.assertIsInstance(validation.get("schema_audit"), dict)

                reported = subprocess.run(
                    [str(executable), "report", str(run_path)], env=env,
                    check=False, capture_output=True, text=True)
                self.assertEqual(reported.returncode, 0, reported.stderr)
                self.assertTrue((run_path / "report.md").read_text(encoding="utf-8"))
                json.loads((run_path / "validation.json").read_text(encoding="utf-8"))

            compared = subprocess.run(
                [str(executable), "compare", "--plan", str(plan),
                 "--output", str(root / "comparison")], env=env,
                check=False, capture_output=True, text=True)
            self.assertEqual(compared.returncode, 0, compared.stderr)
            comparison = json.loads(
                (root / "comparison/comparison.json").read_text(encoding="utf-8"))
            self.assertTrue(comparison["valid_for_comparison"])
            self.assertEqual(comparison["valid_blocks"], ["b1"])
            self.assertIsInstance(comparison.get("schema_audit"), dict)

            # Exercise the documented business-state contract rather than check=True:
            # one valid block is not enough when two are explicitly requested.
            insufficient = subprocess.run(
                [str(executable), "compare", "--plan", str(plan),
                 "--valid-blocks", "2", "--output", str(root / "comparison-short")],
                env=env, check=False, capture_output=True, text=True)
            self.assertEqual(insufficient.returncode, core.EXIT_INVALID)
            insufficient_result = json.loads(
                (root / "comparison-short/comparison.json").read_text(encoding="utf-8"))
            self.assertFalse(insufficient_result["valid_for_comparison"])
            self.assertIn("INSUFFICIENT_VALID_BLOCKS",
                          insufficient_result["failure_codes"])

            legacy = root / "legacy-fixture"
            legacy.mkdir()
            invalid = subprocess.run(
                [str(executable), "validate", str(legacy)], env=env,
                check=False, capture_output=True, text=True)
            self.assertEqual(invalid.returncode, core.EXIT_INVALID)
            invalid_result = json.loads(invalid.stdout)
            self.assertEqual(invalid_result["failure_codes"], ["MANIFEST_MISSING"])

    def test_installed_cli_packages_resources_and_fails_closed_on_resource_tamper(self):
        repository = Path(__file__).parents[3]
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); source = root / "project"; source.mkdir()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.24)\nproject(ibmw LANGUAGES NONE)\n"
                "include(GNUInstallDirs)\nset(IBMW_INSTALL ON)\n"
                f"add_subdirectory(\"{repository / 'tools/mw_perf_collect'}\" collector)\n")
            build, prefix = root / "build", root / "prefix"
            subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run(["cmake", "--install", str(build), "--prefix", str(prefix),
                            "--component", "mw_perf_collect"], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            installed = prefix / "libexec/ibmw/mw_perf_collect"
            self.assertTrue((installed / "ohos_capture.py").is_file())
            for relative in core._RESOURCE_SHA256:
                self.assertTrue((installed / relative).is_file(), relative)
            run_path = self._real_formal_run(root, "A")
            executable = prefix / "bin/mw-perf-collect"
            target = installed / "schemas/result.schema.v1.json"
            original = target.read_bytes()
            for mutation in ("tampered", "missing"):
                target.write_bytes(original + b"\n" if mutation == "tampered" else b"")
                if mutation == "missing":
                    target.unlink()
                checked = subprocess.run([str(executable), "validate", str(run_path)],
                                         check=False, capture_output=True, text=True)
                self.assertNotEqual(checked.returncode, 0, mutation)
                validation = json.loads(checked.stdout)
                self.assertFalse(validation["valid_for_metric"], mutation)
                self.assertFalse(validation["valid_for_comparison"], mutation)
                self.assertTrue(any(code.startswith(("RESOURCE_TAMPERED:", "RESOURCE_MISSING:"))
                                    for code in validation["failure_codes"]), mutation)
                target.write_bytes(original)

    def _plan_v2(self, root, blocks, lanes=("A", "B", "S"), order=None):
        plan = root / "plan-v2.json"
        plan.write_text(json.dumps({"schema": "mw_perf.comparison-plan.v2", "schema_version": 2,
            "lanes": list(lanes), "contract": "0" * 64, "profile": "profile",
            "workload": "workload", "resource_scope": "scope",
            "order": list(order or lanes), "blocks": blocks}))
        return plan

    def _block(self, runs, block_id="b1"):
        return {"block_id": block_id, "runs": [{"lane": lane, "path": str(path)}
                                               for lane, path in runs]}

    def _compare(self, root, plan):
        code = main(["compare", "--plan", str(plan), "--output", str(root / "out")])
        return code, json.loads((root / "out/comparison.json").read_text())

    def _abs_runs(self, root, lanes="ABS"):
        return [(lane, self._real_formal_run(root / "b1", lane, lanes=lanes)) for lane in lanes]

    def test_compare_v2_accepts_abs_block_with_real_validation_and_labels(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)]))
            self.assertEqual(code, 0, result["failure_codes"])
            self.assertTrue(result["valid_for_comparison"])
            self.assertEqual(result["schema"], "mw_perf.comparison-result.v2")
            self.assertEqual(result["schema_version"], 2)
            self.assertEqual(result["lanes"], ["A", "B", "S"])
            self.assertEqual(result["order"], ["A", "B", "S"])
            self.assertEqual(result["valid_blocks"], ["b1"])
            lanes = result["blocks"][0]["lanes"]
            self.assertEqual(sorted(lanes), ["A", "B", "S"])
            self.assertEqual(lanes["S"]["label"], "shm_send_4KB")
            self.assertEqual(lanes["S"]["run_id"], "S")
            self.assertEqual(result["schema_audit"]["schemas"]["comparison_plan"]["schema_file"],
                             "schemas/comparison-plan.schema.v2.json")

    def test_compare_v2_order_is_a_permutation_of_declared_lanes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root, "SAB")
            plan = self._plan_v2(root, [self._block(runs)], order="SAB")
            code, result = self._compare(root, plan)
            self.assertEqual(code, 0, result["failure_codes"])
            self.assertEqual(result["lanes"], ["A", "B", "S"])
            self.assertEqual(result["order"], ["S", "A", "B"])

    def test_compare_v2_accepts_declared_abc_set_with_v1_runs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = [(lane, self._real_formal_run(root / "b1", lane)) for lane in "ABC"]
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)], "ABC"))
            self.assertEqual(code, 0, result["failure_codes"])
            self.assertEqual(result["lanes"], ["A", "B", "C"])

    def test_compare_v2_rejects_c_lane_in_s_plan(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            runs[2] = ("C", self._real_formal_run(root / "b1", "C"))
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)]))
            self.assertEqual(code, core.EXIT_INVALID)
            reasons = result["invalid_blocks"][0]["reasons"]
            for reason in ("LANE_INVALID", "LANES_MISSING:S", "LANES_NOT_EXACTLY_ABS",
                           "ORDER_SEQUENCE_MISMATCH"):
                self.assertIn(reason, reasons)
            self.assertIn("PLAN_SCHEMA:$.blocks[0].runs[2].lane:enum", result["failure_codes"])

    def test_compare_v2_rejects_s_lane_in_declared_abc_plan(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            code, result = self._compare(root, self._plan_v2(
                root, [self._block(runs)], "ABC", order="ABS"))
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertIn("PLAN_SCHEMA:$.order[2]:enum", result["failure_codes"])
            reasons = result["invalid_blocks"][0]["reasons"]
            self.assertIn("LANE_INVALID", reasons)
            self.assertIn("LANES_MISSING:C", reasons)

    def test_compare_v2_rejects_missing_s(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)[:2]
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)]))
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertEqual(result["invalid_blocks"][0]["reasons"],
                             ["LANES_MISSING:S", "LANES_NOT_EXACTLY_ABS", "ORDER_SEQUENCE_MISMATCH"])
            self.assertEqual(result["valid_blocks"], [])

    def test_compare_v2_rejects_duplicate_lane(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            runs[2] = ("A", self._real_formal_run(root / "b2", "A", lanes="ABS"))
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)]))
            self.assertEqual(code, core.EXIT_INVALID)
            reasons = result["invalid_blocks"][0]["reasons"]
            self.assertIn("LANE_DUPLICATE:A", reasons)
            self.assertIn("LANES_MISSING:S", reasons)
            self.assertIn("LANES_NOT_EXACTLY_ABS", reasons)

    def test_compare_v2_rejects_block_sequence_and_manifest_order_mismatch(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs[::-1])]))
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertEqual(result["invalid_blocks"][0]["reasons"], ["ORDER_SEQUENCE_MISMATCH"])
            plan = self._plan_v2(root, [self._block(runs[::-1])], order="SBA")
            code, result = self._compare(root, plan)
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertEqual(result["invalid_blocks"][0]["reasons"],
                             [f"ORDER_MISMATCH:{lane}" for lane in "ABS"])

    def test_compare_v2_rejects_abc_run_reused_in_s_block(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            runs[0] = ("A", self._real_formal_run(root / "abc", "A"))
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)]))
            self.assertEqual(code, core.EXIT_INVALID)
            reasons = result["invalid_blocks"][0]["reasons"]
            self.assertIn("ORDER_MISMATCH:A", reasons)
            self.assertIn("ORDER_INCONSISTENT", reasons)

    def test_compare_v2_keeps_integrity_checks(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            manifest_path = runs[2][1] / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["comparison_lane"] = "A"
            manifest_path.write_text(json.dumps(manifest))
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)]))
            self.assertEqual(code, core.EXIT_INVALID)
            reasons = result["invalid_blocks"][0]["reasons"]
            self.assertIn("LANE_MISMATCH:S", reasons)
            self.assertIn("VALIDATION_STALE_OR_TAMPERED:S", reasons)
            self.assertIn("RUN_NOT_VALID_FOR_COMPARISON:S", reasons)

    def test_compare_v2_fails_closed_on_undeclared_lane_set(self):
        for lanes in (["A", "B"], ["A", "C", "S"], ["B", "A", "S"], ["A", "B", "C", "S"], "ABS", None):
            with self.subTest(lanes), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                runs = self._abs_runs(root)
                plan = self._plan_v2(root, [self._block(runs)], order="ABS")
                value = json.loads(plan.read_text())
                value["lanes"] = lanes
                plan.write_text(json.dumps(value))
                code, result = self._compare(root, plan)
                self.assertEqual(code, core.EXIT_INVALID)
                self.assertIn("PLAN_LANES_INVALID", result["failure_codes"])
                self.assertEqual(result["valid_blocks"], [])

    def test_compare_v2_empty_block_with_undeclared_lane_set_is_not_valid(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            # order=[] matches the empty run list, so only the block-level
            # PLAN_LANES_INVALID keeps this block out of valid_blocks.
            plan = self._plan_v2(root, [{"block_id": "b1", "runs": []}], lanes=["A"], order=[])
            value = json.loads(plan.read_text()); value["order"] = []
            plan.write_text(json.dumps(value))
            code, result = self._compare(root, plan)
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertEqual(result["valid_blocks"], [])
            self.assertEqual(result["invalid_blocks"][0]["reasons"], ["PLAN_LANES_INVALID"])

    def test_compare_v1_still_rejects_s(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            plan = self._plan(root, [self._block(runs)])
            code, result = self._compare(root, plan)
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertEqual(result["schema"], "mw_perf.comparison-result.v1")
            self.assertNotIn("lanes", result)
            reasons = result["invalid_blocks"][0]["reasons"]
            self.assertIn("LANE_INVALID", reasons)
            self.assertIn("LANES_MISSING:C", reasons)
            self.assertIn("LANES_NOT_EXACTLY_ABC", reasons)
            self.assertIn("PLAN_SCHEMA:$.blocks[0].runs[2].lane:enum", result["failure_codes"])
            value = json.loads(plan.read_text())
            value["order"] = ["A", "B", "S"]
            plan.write_text(json.dumps(value))
            _, result = self._compare(root, plan)
            self.assertIn("PLAN_SCHEMA:$.order[2]:enum", result["failure_codes"])

    def test_compare_v1_and_legacy_output_bytes_are_unchanged(self):
        # SHA-256 of comparison.json written by the code before plan v2 (HEAD
        # 9db410c) for the same fixtures, with the temporary root as <ROOT>.
        golden = {"formal": "afa785e5c0bdb32059464cd762903371cb7628418cf4039ec7b07b7b6f1ad102",
                  "legacy": "95f090879face59e3a76b2bb0b97c83fb59b799b11df3c6d956c1557ea12049f"}
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = [(lane, self._real_formal_run(root / "b1", lane)) for lane in "ABC"]
            plan = root / "plan.json"
            plan.write_text(json.dumps({"schema": "mw_perf.comparison-plan.v1", "schema_version": 1,
                "contract": "0" * 64, "profile": "profile", "workload": "workload",
                "resource_scope": "scope", "order": ["A", "B", "C"],
                "blocks": [{"block_id": "b1", "runs": [{"lane": lane, "path": str(path)}
                                                       for lane, path in runs]}]}))
            legacy = root / "legacy.json"
            legacy.write_text(json.dumps({"runs": [str(path) for _, path in runs]}))
            for name, source in (("formal", plan), ("legacy", legacy)):
                output = root / f"{name}-out"
                self.assertEqual(main(["compare", "--plan", str(source), "--output", str(output)]), 0)
                data = (output / "comparison.json").read_bytes().replace(str(root).encode(), b"<ROOT>")
                self.assertEqual(hashlib.sha256(data).hexdigest(), golden[name], data.decode())

    def test_legacy_compare_rejects_runs_without_v1_metadata(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = [self._real_formal_run(root / "abc", lane) for lane in "ABC"]
            runs.append(self._real_formal_run(root / "abs", "S", lanes="ABS"))
            legacy = root / "legacy.json"
            legacy.write_text(json.dumps({"runs": [str(path) for path in runs]}))
            code, result = self._compare(root, legacy)
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertFalse(result["valid_for_comparison"])
            self.assertEqual(result["failure_codes"], ["COMPARISON_METADATA_VERSION_UNSUPPORTED"])
            self.assertEqual([row["run_id"] for row in result["runs"]], ["A", "B", "C"])

    def test_compare_fails_closed_on_unknown_formal_plan_schema(self):
        for schema in ("mw_perf.comparison-plan.v3", "mw_perf.comparison-plan.v2x"):
            with self.subTest(schema), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                runs = [(lane, self._real_formal_run(root / "b1", lane)) for lane in "ABC"]
                plan = root / "plan.json"
                plan.write_text(json.dumps({"schema": schema, "lanes": ["A", "B", "C"],
                    "runs": [str(path) for _, path in runs],
                    "blocks": [self._block(runs)]}))
                code, result = self._compare(root, plan)
                self.assertEqual(code, core.EXIT_INVALID)
                self.assertFalse(result["valid_for_comparison"])
                self.assertEqual(result["failure_codes"], ["PLAN_SCHEMA_UNSUPPORTED"])

    def test_compare_v2_binds_lane_to_label(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runs = self._abs_runs(root)
            runs[2] = ("S", self._real_formal_run(root / "b2", "S", lanes="ABS",
                                                  label="iox_send_4KB"))
            self.assertTrue(core.validate_dir(runs[2][1])["valid_for_comparison"])
            code, result = self._compare(root, self._plan_v2(root, [self._block(runs)]))
            self.assertEqual(code, core.EXIT_INVALID)
            self.assertEqual(result["invalid_blocks"][0]["reasons"], ["LABEL_MISMATCH:S"])
            # v1 output never carried labels and keeps its old verdict.
            runs = [(lane, self._real_formal_run(root / "v1", lane,
                                                 label="abc_dds_4KB" if lane == "A" else None))
                    for lane in "ABC"]
            code, result = self._compare(root, self._plan(root, [self._block(runs)]))
            self.assertEqual(code, 0, result["failure_codes"])

    def test_compare_v2_audit_follows_lane_set(self):
        for lanes, result_file in (("ABS", "result.schema.v2.json"), ("ABC", "result.schema.v1.json")):
            with self.subTest(lanes), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                runs = self._abs_runs(root, lanes)
                code, result = self._compare(root, self._plan_v2(root, [self._block(runs)], lanes))
                self.assertEqual(code, 0, result["failure_codes"])
                schemas = result["schema_audit"]["schemas"]
                self.assertEqual(schemas["comparison_result"]["schema_file"], "schemas/" + result_file)
                self.assertEqual(schemas["comparison_plan"]["schema_file"],
                                 "schemas/comparison-plan.schema.v2.json")

    def test_v1_evidence_schemas_stay_frozen_for_archived_validations(self):
        # validation.json embeds these digests; changing one makes every
        # archived A/B/C run VALIDATION_STALE_OR_TAMPERED.
        frozen = {"schemas/result.schema.v1.json":
                      "4895699463484942340973dc6bf370b9c6a1c03dda3848b72fdfb4cc0156d16f",
                  "schemas/validation.schema.v1.json":
                      "2a859e48c5a6e89b26d62b9ff2019fcab6ec120450cdb0801b8380823099e95d",
                  "schemas/comparison-plan.schema.v1.json":
                      "b3c3fe804c3c7ef9897837abc4f756d9c79301eaf293ef00598799ffa7ffeb69"}
        for relative, digest in frozen.items():
            self.assertEqual(core._RESOURCE_SHA256[relative], digest)
        with tempfile.TemporaryDirectory() as td:
            run = self._real_formal_run(Path(td), "A")
            audit = core.validate_dir(run)["schema_audit"]["schemas"]["result"]
            self.assertEqual(audit["schema_file"], "schemas/result.schema.v1.json")
            self.assertEqual(audit["schema_sha256"], frozen["schemas/result.schema.v1.json"])

    def test_s_run_audit_names_its_own_result_schema(self):
        with tempfile.TemporaryDirectory() as td:
            run = self._real_formal_run(Path(td), "S", lanes="ABS")
            validation = core.validate_dir(run)
            self.assertTrue(validation["valid_for_comparison"], validation["failure_codes"])
            self.assertEqual(validation["schema_audit"]["schemas"]["result"]["schema_file"],
                             "schemas/result.schema.v2.json")

    def test_metadata_version_binds_lane_and_order(self):
        cases = (("mw_perf.comparison-metadata.v1", "S", ["A", "B", "S"]),
                 ("mw_perf.comparison-metadata.v1", "S", ["A", "B", "C"]),
                 ("mw_perf.comparison-metadata.v1", "A", ["A", "B", "S"]),
                 ("mw_perf.comparison-metadata.v2", "C", ["A", "B", "C"]),
                 ("mw_perf.comparison-metadata.v2", "S", ["A", "B", "C"]),
                 ("mw_perf.comparison-metadata.v2", "C", ["A", "B", "S"]),
                 ("mw_perf.comparison-metadata.v2", "S", ["A", "C", "S"]),
                 ("mw_perf.comparison-metadata.v2", "S", ["A", "B", "B"]),
                 ("mw_perf.comparison-metadata.v2", "S", ["A", "B"]),
                 ("mw_perf.comparison-metadata.v2", ["S"], ["A", "B", "S"]),
                 (["mw_perf.comparison-metadata.v2"], "S", ["A", "B", "S"]),
                 ("mw_perf.comparison-metadata.v3", "S", ["A", "B", "S"]))
        for schema, lane, order in cases:
            with self.subTest(schema=schema, lane=lane, order=order), \
                 tempfile.TemporaryDirectory() as td:
                run = self._real_formal_run(Path(td), "S", lanes="ABS")
                manifest = json.loads((run / "manifest.json").read_text())
                manifest.update(comparison_schema=schema, comparison_lane=lane, order=order)
                (run / "manifest.json").write_text(json.dumps(manifest))
                validation = core.validate_dir(run)
                self.assertFalse(validation["valid_for_comparison"])
                self.assertIn("COMPARISON_METADATA", validation["failure_codes"])

    def test_comparison_plan_v2_schema(self):
        root = Path(core.__file__).parent / "schemas"
        schema = json.loads((root / "comparison-plan.schema.v2.json").read_text())
        plan = {"schema": "mw_perf.comparison-plan.v2", "schema_version": 2,
            "lanes": ["A", "B", "S"], "contract": "contract", "profile": "profile",
            "workload": "workload", "resource_scope": "scope", "order": ["S", "A", "B"],
            "blocks": [{"block_id": "b1", "runs": [{"lane": lane, "path": "/run/" + lane}
                                                   for lane in "SAB"]}]}
        self.assertEqual(core._schema_errors(plan, schema), [])
        for field, value in (("lanes", ["A", "B", "C", "S"]), ("lanes", ["S", "A", "B"]),
                             ("order", ["A", "B", "C"]), ("order", ["A", "B", "B"]),
                             ("schema_version", 1), ("schema", "mw_perf.comparison-plan.v1")):
            self.assertTrue(core._schema_errors({**plan, field: value}, schema), (field, value))
        invalid = {name: value for name, value in plan.items() if name != "lanes"}
        self.assertIn("$.lanes:required", core._schema_errors(invalid, schema))
        v1 = json.loads((root / "comparison-plan.schema.v1.json").read_text())
        self.assertTrue(core._schema_errors({**plan, "schema": "mw_perf.comparison-plan.v1",
                                             "schema_version": 1}, v1))

    def test_result_schema_versions_split_lane_sets(self):
        root = Path(core.__file__).parent / "schemas"
        v1 = json.loads((root / "result.schema.v1.json").read_text())
        v2 = json.loads((root / "result.schema.v2.json").read_text())
        with tempfile.TemporaryDirectory() as td:
            s_run = self._real_formal_run(Path(td) / "s", "S", lanes="ABS")
            c_run = self._real_formal_run(Path(td) / "c", "C")
            s_manifest = json.loads((s_run / "manifest.json").read_text())
            c_manifest = json.loads((c_run / "manifest.json").read_text())
        fields = ("comparison_schema", "order", "comparison_lane")
        def comparison(value):
            return {name: value[name] for name in fields}
        probe = {"properties": {name: v2["properties"][name] for name in fields}}
        self.assertEqual(core._schema_errors(comparison(s_manifest), probe), [])
        self.assertTrue(core._schema_errors(comparison(c_manifest), probe))
        probe = {"properties": {name: v1["properties"][name] for name in fields}}
        self.assertEqual(core._schema_errors(comparison(c_manifest), probe), [])
        self.assertTrue(core._schema_errors(comparison(s_manifest), probe))

    def test_resource_registry_matches_installed_json_resources(self):
        root = Path(core.__file__).parent
        actual = {str(path.relative_to(root)) for directory in ("schemas", "contracts")
                  for path in (root / directory).glob("*.json")}
        self.assertEqual(actual, set(core._RESOURCE_SHA256))
