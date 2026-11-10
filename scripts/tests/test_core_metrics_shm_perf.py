#!/usr/bin/env python3
import contextlib, importlib.util, inspect, io, json, os, signal, subprocess, sys, tempfile, time, unittest
from pathlib import Path
from unittest import mock

TESTS=Path(__file__).resolve().parent
SCRIPTS=TESTS.parent
ROOT=SCRIPTS.parent
BENCH=ROOT/"src/samples/cpp/transport_benchmark"
# Metrics of a real shm_loaned_ctx counters trial at 448000 Hz, 2+5+1 s.
FIXTURE=TESTS/"fixtures/core_metrics_shm_counters.jsonl"
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module
shm=load("cm_shm_perf",SCRIPTS/"core_metrics_shm_perf.py");g=shm.gate
iox=load("cm_iox_perf_reference",SCRIPTS/"core_metrics_perf.py")

class LaneIsolation(unittest.TestCase):
    def test_iceoryx_module_keeps_registered_identity(self):
        self.assertEqual(iox.PROTOCOL,"core-metrics-lossless-v2")
        self.assertEqual(iox.BROKER_BINARY,"iox-roudi")
        self.assertEqual(iox.DIRECT_COUNTER_KIND,"direct_iceoryx_loan")
        self.assertEqual(iox.COUNTER_TOPIC,"iox_bench_ctx_topic")
        self.assertEqual(iox.COUNTER_MODULE,"IoxLoanedCtxPublisherModule")
        self.assertEqual(iox.FROZEN_LOAD,448000)
        self.assertEqual(iox.REGISTERED_CMAKE_OPTIONS["IBMW_BUILD_ICEORYX_EXTENSION"],"ON")
        self.assertNotIn("IBMW_BUILD_SHM_EXTENSION",iox.REGISTERED_CMAKE_OPTIONS)
        self.assertIn("iox-roudi",iox.ARTIFACTS)
        self.assertIn("roudi.log",iox._seal_artifact_names("out"))
        self.assertNotIn("selfdev",iox.SOURCE_ROOTS)
        self.assertEqual(sorted(iox.DEPENDENCIES)[5],"iceoryx")
    def test_iceoryx_contract_digest_unchanged(self):
        _,digest=iox.protocol_contract()
        self.assertEqual(digest,"ab4743b13e04b62c7e539b9ab42d970a6cda495fc88a0816557dd55f4beef44c")
    def test_gates_reject_each_others_counters(self):
        with self.assertRaises(ValueError):iox.counter_conservation(FIXTURE)
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"m.jsonl";p.write_text(FIXTURE.read_text().replace("direct_shm_loan","direct_iceoryx_loan"))
            with self.assertRaises(ValueError):g.counter_conservation(p)

class Contracts(unittest.TestCase):
    def test_protocol_contract_loads(self):
        value,digest=g.protocol_contract()
        self.assertEqual(value["protocol"],"core-metrics-shm-lossless-v1")
        self.assertEqual(value["frozen_load"],g.FROZEN_LOAD)
        self.assertEqual(value["delivery_profile"],iox.LOSSLESS_PROFILE)
        self.assertEqual(value["thresholds"],iox.THRESHOLDS)
        self.assertEqual(digest,g.sha(g.PROTOCOL_CONTRACT))
    def mutated(self,change):
        value=json.loads(g.PROTOCOL_CONTRACT.read_text());change(value)
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"c.json";p.write_text(json.dumps(value))
            with self.assertRaises(ValueError):g.protocol_contract(p)
    def test_protocol_contract_rejects_semantic_changes(self):
        self.mutated(lambda v:v.update(protocol="core-metrics-lossless-v2"))
        self.mutated(lambda v:v.update(frozen_load=448001))
        self.mutated(lambda v:v["thresholds"]["off_out"].update(cpu=.5))
        self.mutated(lambda v:v["delivery_profile"].update(queue_capacity=1))
        self.mutated(lambda v:v.update(unexpected=1))
    def test_iceoryx_contract_is_not_accepted(self):
        with self.assertRaises(ValueError):g.protocol_contract(iox.PROTOCOL_CONTRACT)
    def test_duplicate_contract_key_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"c.json";p.write_text(g.PROTOCOL_CONTRACT.read_text().replace('"batch": 128,','"batch": 128,\n  "batch": 128,'))
            with self.assertRaises(ValueError):g.protocol_contract(p)
    def test_dependency_contract_matches_repository_without_iceoryx(self):
        trusted,_=g.load_dependency_contract(ROOT)
        self.assertEqual(sorted(trusted),["asio","backward","fmt","gflags","libunifex","nlohmann_json","tbb","yaml-cpp"])
    def test_dependency_contract_rejects_declaration_drift(self):
        value=json.loads(g.DEPENDENCY_CONTRACT.read_text());value["components"][0]["declaration_sha256"]="0"*64
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"c.json";p.write_text(json.dumps(value))
            with self.assertRaises(ValueError):g.load_dependency_contract(ROOT,p)
    def test_archived_contract_paths_name_the_shm_contracts(self):
        self.assertEqual(ROOT/g.PROTOCOL_CONTRACT_ARCHIVED,g.PROTOCOL_CONTRACT)
        self.assertEqual(ROOT/g.DEPENDENCY_CONTRACT_ARCHIVED,g.DEPENDENCY_CONTRACT)
    def test_plan_binds_shm_protocol(self):
        plan=g.registered_plan()
        self.assertEqual(plan["protocol"],"core-metrics-shm-lossless-v1")
        self.assertEqual(plan["capacity"]["candidates"],list(g.CAPACITY_CANDIDATES))
        self.assertIn(g.FROZEN_LOAD,g.CAPACITY_CANDIDATES)
        self.assertNotIn("lossless-v2",plan["capacity"]["candidate_source"])
        self.assertEqual(plan["schema"],"ibmw-core-metrics-shm-lossless-plan-v1")
        self.assertIn("shm-lossless-v1",plan["capacity"]["candidate_source"])

class ArtifactLabels(unittest.TestCase):
    def test_shm_labels(self):
        self.assertEqual(g.PROTOCOL_LABEL,"shm-lossless-v1")
        self.assertEqual(g.DIAGNOSTIC_SCHEMA,"ibmw-core-metrics-shm-lossless-v1-conservation-diagnostic-v1")
        summary=g.e2e_only_summary([],None)
        self.assertEqual(summary["schema"],"ibmw-core-metrics-shm-lossless-v1-e2e-summary-v1")
        self.assertEqual(summary["purpose"],"formal shm-lossless-v1 E2E gate rerun")
        self.assertEqual(summary["protocol"],"core-metrics-shm-lossless-v1")
        self.assertNotIn("lossless-v2",json.dumps(g.registered_plan()))
    def test_iceoryx_labels_unchanged(self):
        self.assertEqual(iox.PROTOCOL_LABEL,"lossless-v2")
        self.assertEqual(iox.registered_plan()["schema"],"ibmw-core-metrics-lossless-plan-v2")
        self.assertEqual(iox.registered_plan()["capacity"]["candidate_source"],"fixed before lossless-v2 from separate batch-size and pacing-window design smokes")
        summary=iox.e2e_only_summary([],None)
        self.assertEqual(summary["schema"],"ibmw-core-metrics-lossless-v2-e2e-summary-v1")
        self.assertEqual(summary["purpose"],"formal lossless-v2 E2E gate rerun")
        self.assertEqual(iox.DIAGNOSTIC_SCHEMA,"ibmw-core-metrics-lossless-v2-conservation-diagnostic-v1")
    def test_main_reads_labels_at_call_time(self):
        # Literal lane labels in _main would bypass the shm rebinding.
        source=inspect.getsource(iox._main)
        self.assertNotIn("lossless-v2",source)
        for name in ("PROTOCOL_LABEL","DIAGNOSTIC_SCHEMA"):self.assertIn(name,source)
        for function in (iox.registered_plan,iox.e2e_only_summary,iox._main):
            self.assertNotIn("lossless-v2",repr(function.__defaults__))

class OutputGuard(unittest.TestCase):
    def rejected(self,argv):
        with self.assertRaises(SystemExit) as raised:shm.check_output_outside_sources(argv)
        self.assertIn("outside snapshotted source inputs",str(raised.exception.code))
    def test_rejects_every_snapshot_root(self):
        self.assertIn("selfdev",g.SOURCE_ROOTS)
        self.rejected(["--output",str(ROOT)])
        for name in g.SOURCE_ROOTS:
            self.rejected(["--output",str(ROOT/name)])
            self.rejected(["--skip-build","--output",str(ROOT/name/"evidence")])
            self.rejected([f"--output={ROOT/name/'evidence'}"])
    def test_rejects_symlink_into_selfdev(self):
        with tempfile.TemporaryDirectory() as d:
            link=Path(d)/"link";link.symlink_to(ROOT/"selfdev")
            self.rejected(["--output",str(link/"evidence")])
    def test_accepts_outside_and_sibling_names(self):
        self.assertIsNone(shm.check_output_outside_sources(["--output","/var/tmp/evidence"]))
        self.assertIsNone(shm.check_output_outside_sources(["--output",str(ROOT/"build-core-metrics-shm")]))
        self.assertIsNone(shm.check_output_outside_sources(["--output",str(ROOT/"selfdev-evidence")]))
        self.assertIsNone(shm.check_output_outside_sources([]))
    def test_main_rejects_before_gate_runs(self):
        with mock.patch.object(g,"main") as gate_main:
            with self.assertRaises(SystemExit):shm.main(["--output",str(ROOT/"selfdev"/"evidence")])
        gate_main.assert_not_called()

class Conservation(unittest.TestCase):
    def rows(self):return [json.loads(x) for x in FIXTURE.read_text().splitlines()]
    def check(self,rows):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"m.jsonl";p.write_text("\n".join(json.dumps(x) for x in rows)+"\n")
            return g.counter_conservation(p)
    def test_real_run_conserves(self):
        result=g.counter_conservation(FIXTURE)
        self.assertTrue(result["valid"])
        self.assertEqual(result["values"]["logical_attempt"],result["values"]["publish"])
        self.assertGreater(result["values"]["logical_attempt"],0)
    def test_real_run_cardinalities(self):
        counts={}
        for row in self.rows():counts[row["kind"]]=counts.get(row["kind"],0)+1
        self.assertEqual(counts,{"logical_publish":7,"logical_loan":7,"driver_send":7,"source_driver_loan":1,"direct_shm_loan":42})
    def direct(self,rows,metric):
        return next(r for r in rows if r["kind"]=="direct_shm_loan" and r["metric"]==metric and r["type"].endswith("4KB"))
    def bump(self,rows,metric,amount):
        row=self.direct(rows,metric);row["value"]+=amount;row["cumulative"]["value"]+=amount
    def test_lost_publish_is_invalid(self):
        rows=self.rows();self.bump(rows,"publish",-1)
        self.assertFalse(self.check(rows)["valid"])
    def test_attempt_without_outcome_is_invalid(self):
        rows=self.rows();self.bump(rows,"logical_attempt",1)
        self.assertFalse(self.check(rows)["valid"])
    def test_release_balances_success(self):
        rows=self.rows();self.bump(rows,"publish",-1);self.bump(rows,"release",1)
        self.assertTrue(self.check(rows)["valid"])
    def test_retry_bound(self):
        rows=self.rows();self.bump(rows,"internal_retry",15*g.counter_conservation(FIXTURE)["values"]["logical_attempt"]+1)
        self.assertFalse(self.check(rows)["valid"])
    def test_structure_is_closed(self):
        for change in (lambda r:r.pop(),
                       lambda r:r[-1].update(snapshot_seq=1),
                       lambda r:r[0].update(unexpected=1),
                       lambda r:r[0]["cumulative"].update(module="IoxLoanedCtxPublisherModule"),
                       lambda r:self.direct(r,"publish").update(topic="iox_bench_ctx_topic"),
                       lambda r:self.direct(r,"publish").update(driver="shm")):
            rows=self.rows();change(rows)
            with self.assertRaises(ValueError):self.check(rows)

class HotValue(unittest.TestCase):
    record={"schema":"ibmw.core_metrics.v1","snapshot_seq":1,"kind":"direct_shm_loan","metric":"hot_hook","topic":"","type":"","driver":"","value":22000000,"cumulative":{"metric":"hot_hook","topic":"","type":"","driver":"","value":22000000},"complete":True}
    def parse(self,record):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"m.jsonl";p.write_text(json.dumps(record)+"\n")
            return g.reporter_hot_value(p)
    def test_parses_shm_kind(self):
        self.assertEqual(self.parse(self.record),[22000000])
    def test_rejects_iceoryx_kind(self):
        with self.assertRaises(ValueError):self.parse({**self.record,"kind":"direct_iceoryx_loan"})
    def test_rejects_cumulative_mismatch(self):
        with self.assertRaises(ValueError):self.parse({**self.record,"value":1})
    def test_rejects_extra_line(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"m.jsonl";p.write_text((json.dumps(self.record)+"\n")*2)
            with self.assertRaises(ValueError):g.reporter_hot_value(p)
    def test_hot_bench_runs_with_shm_kind(self):
        self.assertEqual(g.clean_env({})["IBMW_REPORTER_HOT_KIND"],"direct_shm_loan")

class Yaml(unittest.TestCase):
    def render(self,publisher):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"y.yaml"
            digest=g.yaml(p,publisher,g.FROZEN_LOAD,10,60,5,"/tmp/lat.csv","/tmp/st.json","/tmp/pst.json",1000000000,61000000000)
            self.assertEqual(digest,g.sha(p))
            return p.read_text()
    def test_workload(self):
        for publisher in (True,False):
            text=self.render(publisher)
            for needle in ("path: ./libibmw_shm_transport.so","payload_bytes: 4096","batch_size: 128","duration_sec: 75","warmup_sec: 10","measured_sec: 60","measure_begin_monotonic_ns: 1000000000","measure_end_monotonic_ns: 61000000000",'benchmark_profile: "core_metrics_e2e_lossless_v1"','publisher_too_slow_policy: "WAIT_FOR_CONSUMER"','subscriber_queue_full_policy: "BLOCK_PRODUCER"',"subscriber_queue_capacity: 256",'topic: "shm_bench_ctx_topic"'):
                self.assertIn(needle,text)
            self.assertNotIn("iceoryx_extension",text)
            self.assertNotIn("iox_bench_ctx_topic",text)
    def test_publisher_routing(self):
        text=self.render(True)
        self.assertIn("publishers:",text);self.assertIn("ShmLoanedCtxPublisherModule:",text)
        self.assertIn("publisher_status_path: /tmp/pst.json",text);self.assertNotIn("csv_path",text)
    def test_subscriber_routing(self):
        text=self.render(False)
        self.assertIn("subscribers:",text);self.assertIn("IoxLoanedCtxSubscriberModule:",text)
        self.assertIn("    topic: shm_bench_ctx_topic\n",text)
        self.assertIn("csv_path: /tmp/lat.csv",text);self.assertIn("status_path: /tmp/st.json",text)
    def test_options_exist_in_shm_parser(self):
        extension=(ROOT/"src/extensions/transport/shm_ext/shm_extension.cc").read_text()
        driver=(ROOT/"src/extensions/transport/shm_ext/shm_transport_driver.cc").read_text()
        for option in ("driver_name","shm_init_size","runtime_id"):
            self.assertIn(f'node["{option}"]',extension)
            self.assertIn(option,self.render(True))
        for option in ("listener_thread_name","benchmark_profile","publisher_too_slow_policy","subscriber_queue_full_policy","subscriber_queue_capacity"):
            self.assertIn(f'node["{option}"]',driver)
            self.assertIn(option,self.render(True))
    def test_module_names_match_sources(self):
        self.assertIn('"ShmLoanedCtxPublisherModule"',(BENCH/"shm_loaned_ctx/managed/publisher_module/publisher_module.h").read_text())
        self.assertIn('"IoxLoanedCtxSubscriberModule"',(BENCH/"iox_loaned_ctx/managed/subscriber_module/subscriber_module.h").read_text())

class BuildConfiguration(unittest.TestCase):
    def test_registered_options(self):
        with tempfile.TemporaryDirectory() as d:
            registration=g.registered_build_configuration(Path(d))
        self.assertEqual(set(registration["builds"]),{"out","instrumented"})
        for name,metrics in (("out","OFF"),("instrumented","ON")):
            options=registration["builds"][name]["configure_options"]
            self.assertEqual(options["IBMW_BUILD_ICEORYX_EXTENSION"],"OFF")
            self.assertEqual(options["IBMW_BUILD_SHM_EXTENSION"],"ON")
            self.assertEqual(options["IBMW_ENABLE_CORE_METRICS"],metrics)
            self.assertEqual(options["IBMW_ENABLE_REPORTER_V1"],metrics)
        self.assertEqual(registration["build_targets"],list(g.REGISTERED_BUILD_TARGETS))
    def test_targets_and_artifacts_have_no_iceoryx(self):
        for name in g.REGISTERED_BUILD_TARGETS+g.ARTIFACTS:
            self.assertNotIn("iox",name);self.assertNotIn("iceoryx",name)
        self.assertIn("libibmw_shm_transport.so",g.ARTIFACTS)
        self.assertIn("core_metrics_reporter_hot_bench",g.ARTIFACTS)
    def test_targets_are_defined(self):
        text=(BENCH/"shm_loaned_ctx/CMakeLists.txt").read_text()
        self.assertIn("add_executable(shm_loaned_ctx_publisher_app",text)
        self.assertIn("add_executable(shm_loaned_ctx_lossless_subscriber_app",text)
        self.assertIn("ibmw::core::reporter_host_abi",text)
        self.assertIn("shm_loaned_ctx_publisher_app PROPERTIES ENABLE_EXPORTS ON",text)
    def test_hot_bench_builds_for_either_transport(self):
        text=(BENCH/"CMakeLists.txt").read_text()
        self.assertIn("IBMW_BUILD_ICEORYX_EXTENSION OR IBMW_BUILD_SHM_EXTENSION",text)
        self.assertLess(text.index("add_executable(core_metrics_reporter_hot_bench"),text.index('if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")\n  add_subdirectory(iox_loaned_ctx)'))
        self.assertNotIn("add_executable(core_metrics_reporter_hot_bench",(BENCH/"iox_loaned_ctx/CMakeLists.txt").read_text())
    def test_receive_module_builds_without_iceoryx(self):
        for name in ("subscriber_module.h","subscriber_module.cc"):
            self.assertNotIn("iceoryx_ext",(BENCH/"iox_loaned_ctx/managed/subscriber_module"/name).read_text())
        self.assertNotIn("iceoryx_ext",(BENCH/"shm_loaned_ctx/standalone/lossless_subscriber_app/main.cc").read_text())
    def test_snapshot_covers_selfdev_and_skips_agent_state(self):
        self.assertIn("selfdev",g.SOURCE_ROOTS)
        self.assertTrue(g._excluded_source("src/.omc/state.json"))
        self.assertFalse(g._excluded_source("selfdev/shm/CMakeLists.txt"))
    def test_seal_names_have_no_broker_log(self):
        self.assertNotIn("roudi.log",g._seal_artifact_names("counters"))
        self.assertIn("metrics/metrics.jsonl",g._seal_artifact_names("counters"))

class TrialIsolation(unittest.TestCase):
    def test_service_dir_limit(self):
        env=shm.shm_trial_env("/tmp/ishmabcdefgh",uid=4294967294)
        self.assertEqual(env,{"IBMW_SHM_ROOT":"/tmp/ishmabcdefgh","IBMW_SHM_DOMAIN":"cmgate"})
        self.assertLessEqual(len(env["IBMW_SHM_DOMAIN"]),32)
        with self.assertRaises(ValueError):shm.shm_trial_env("/tmp/"+"x"*60)
    def test_service_dir_limit_uses_euid(self):
        # 45-byte root + "/ibmw_shm.1.cmgate" is exactly the 63-byte limit.
        root="/tmp/"+"x"*40
        with mock.patch.object(shm.os,"geteuid",return_value=1),mock.patch.object(shm.os,"getuid",return_value=4294967294):
            self.assertEqual(shm.shm_trial_env(root)["IBMW_SHM_ROOT"],root)
        with mock.patch.object(shm.os,"geteuid",return_value=4294967294),mock.patch.object(shm.os,"getuid",return_value=1):
            with self.assertRaises(ValueError):shm.shm_trial_env(root)
    def test_private_root_lifecycle(self):
        seen={}
        def fake(*args):
            env=g.clean_env({"IBMW_CORE_METRICS":"off"});seen.update(env)
            seen["existed"]=Path(env["IBMW_SHM_ROOT"]).is_dir()
            return {"ok":True}
        with mock.patch.object(shm,"_base_run_trial",fake):
            self.assertEqual(g.run_trial("out",Path("/nonexistent"),Path("/nonexistent"),1,1),{"ok":True})
        self.assertTrue(seen["existed"]);self.assertEqual(seen["IBMW_SHM_DOMAIN"],"cmgate")
        self.assertTrue(seen["IBMW_SHM_ROOT"].startswith("/tmp/ishm"))
        self.assertFalse(os.path.exists(seen["IBMW_SHM_ROOT"]))
        self.assertNotIn("IBMW_SHM_ROOT",g.clean_env({}))
    def test_root_removed_when_trial_fails(self):
        seen={}
        def fake(*args):
            seen["root"]=g.clean_env({})["IBMW_SHM_ROOT"];raise g.InvalidTrial("controlled")
        with mock.patch.object(shm,"_base_run_trial",fake):
            with self.assertRaises(g.InvalidTrial):g.run_trial("out",Path("/nonexistent"),Path("/nonexistent"),1,1)
        self.assertFalse(os.path.exists(seen["root"]))

    def failing_rmtree(self,roots):
        real=shm.shutil.rmtree
        def rmtree(path,*args,**kwargs):
            roots.append(Path(path));raise PermissionError(13,"Permission denied",str(path))
        return real,rmtree
    def test_cleanup_failure_is_reported(self):
        roots=[];real,rmtree=self.failing_rmtree(roots)
        try:
            with mock.patch.object(shm,"_base_run_trial",lambda *a:{"ok":True}),mock.patch.object(shm.shutil,"rmtree",rmtree):
                with self.assertRaises(g.InvalidTrial) as raised:g.run_trial("out",Path("/nonexistent"),Path("/nonexistent"),1,1)
            self.assertIn("private shm root not removed",str(raised.exception))
            self.assertIsInstance(raised.exception.__cause__,PermissionError)
        finally:
            for root in roots:real(root)
        self.assertEqual(len(roots),1);self.assertFalse(roots[0].exists())
    def test_cleanup_failure_keeps_trial_error(self):
        roots=[];real,rmtree=self.failing_rmtree(roots);stderr=io.StringIO()
        def fake(*args):raise g.InvalidTrial("controlled")
        try:
            with mock.patch.object(shm,"_base_run_trial",fake),mock.patch.object(shm.shutil,"rmtree",rmtree),contextlib.redirect_stderr(stderr):
                with self.assertRaises(g.InvalidTrial) as raised:g.run_trial("out",Path("/nonexistent"),Path("/nonexistent"),1,1)
            self.assertEqual(str(raised.exception),"controlled")
            self.assertTrue(any("private shm root not removed" in note for note in raised.exception.__notes__))
            self.assertIn("private shm root not removed",stderr.getvalue())
        finally:
            for root in roots:real(root)
        self.assertFalse(roots[0].exists())
    def test_sigterm_unwinds_and_removes_root(self):
        with tempfile.TemporaryDirectory() as d:
            marker=Path(d)/"root.txt";script=Path(d)/"child.py"
            script.write_text(f"""import importlib.util, signal, sys, time
from pathlib import Path
spec=importlib.util.spec_from_file_location("shm_child",{str(SCRIPTS/"core_metrics_shm_perf.py")!r})
shm=importlib.util.module_from_spec(spec);spec.loader.exec_module(shm)
def fake(*args):
    Path({str(marker)!r}).write_text(shm.gate.clean_env({{}})["IBMW_SHM_ROOT"]);time.sleep(30)
shm._base_run_trial=fake
signal.signal(signal.SIGTERM,shm._terminate_as_interrupt)
try:shm.gate.run_trial("out",Path("/nonexistent"),Path("/nonexistent"),1,1)
except KeyboardInterrupt:sys.exit(3)
""")
            child=subprocess.Popen([sys.executable,"-I",str(script)])
            try:
                deadline=time.monotonic()+20
                while not (marker.exists() and marker.read_text()) and time.monotonic()<deadline:time.sleep(.05)
                root=Path(marker.read_text());self.assertTrue(root.is_dir())
                child.send_signal(signal.SIGTERM);self.assertEqual(child.wait(20),3)
            finally:
                if child.poll() is None:child.kill();child.wait()
            self.assertFalse(root.exists())

class StatusReconciliation(unittest.TestCase):
    def test_gate_reconciles_the_publisher_window(self):
        pub=(BENCH/"shm_loaned_ctx/managed/publisher_module/publisher_module.cc").read_text()
        for key in g.PUBLISHER_STATUS_KEYS:self.assertIn(f'\\"{key}\\"',pub)
        self.assertIn("PERF_PUBLISHER_RUNNING",pub)
        self.assertIn("if (opts_.measure_begin_monotonic_ns != 0) {\n    std::cout",pub)
    def test_shutdown_stays_out_of_signal_handler(self):
        main=(BENCH/"shm_loaned_ctx/standalone/publisher_app/main.cc").read_text()
        self.assertNotIn("g_engine_ptr",main)
        self.assertIn("g_shutdown_requested = 1",main)

if __name__=="__main__":unittest.main()
