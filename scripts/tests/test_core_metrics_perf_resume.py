import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

P=Path(__file__).parents[1]/"core_metrics_perf.py"
spec=importlib.util.spec_from_file_location("cmp_resume",P)
assert spec is not None and spec.loader is not None
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ResumeTest(unittest.TestCase):
    def e2e(self,mode="out",**extra):
        value={"mode":mode,"hz":10,"measured_sec":60,"measure_begin_monotonic_ns":1,"measure_end_monotonic_ns":60_000_000_001,"valid":True,"correctness":{"valid":True},"reconciliation":{"valid":True},"publisher_correctness":{"valid":True},"quality_valid":True,"throughput":1.0,"p50_us":1.0,"p99_us":1.0,"cpu_cores":1.0,"pss_median_kib":1.0,"bottleneck_utilization":.75}
        value.update(extra);return value

    def micro(self,mode="out",**extra):
        value={"mode":mode,"reporter_final_valid":True,"ns_per_op":1.0};value.update(extra);return value

    def seal_e2e(self,trial,mode="out",valid=True):
        trial.mkdir(parents=True);(trial/"metrics").mkdir();begin=1;end=60_000_000_001;unique=60
        status={"received_measured":unique,"measure_begin_monotonic_ns":begin,"measure_end_monotonic_ns":end,"unique_measured":unique,"duplicate":0,"out_of_order":0,"missing":0,"span_missing":0,"first_seq":1,"last_seq":60,"invalid":0,"valid":valid}
        publisher={"measure_begin_monotonic_ns":begin,"measure_end_monotonic_ns":end,"attempts":unique,"success":unique,"fail":0,"first_attempt_seq":1,"last_attempt_seq":60,"first_success_seq":1,"last_success_seq":60,"completed_send_window":True,"valid":valid}
        m.write_json(trial/"status.json",status);m.write_json(trial/"publisher-status.json",publisher)
        (trial/"latency.csv").write_text("label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\nv2,60,1,1,0,1,1,1,1,1\n")
        for name in ("publisher.yaml","subscriber.yaml","publisher.log","subscriber.log","roudi.log"):(trial/name).write_text(name+"\n")
        base={"mode":mode,"hz":10,"measured_sec":60,"cpu_cores":1.0,"cpu_by_process":{"publisher":.5,"subscriber":.5,"roudi":0},"cpu_sample_elapsed_sec":60.0,"bottleneck_utilization":.75,"pss_median_kib":1.0,"pss_peak_kib":1,"pss_valid_samples":1,"pss_invalid_samples":0,"thread_affinity":{},"delivery_profile":m.LOSSLESS_PROFILE,"measure_begin_monotonic_ns":begin,"measure_end_monotonic_ns":end,"subscriber_ready_ns":0,"publisher_ready_ns":0,"cpu_sample_begin_ns":begin,"cpu_sample_end_ns":end}
        m.write_json(trial/"observations.json",{"base":base,"quality_required":True});result=m.recompute_e2e_result(trial);m.write_json(trial/"result.json",result);m._seal_artifacts(trial,mode);return result

    def seal_micro(self,trial,mode="out"):
        trial.mkdir(parents=True);(trial/"metrics").mkdir();m.write_json(trial/"bench-output.json",{"iterations":20000000,"elapsed_ns":20000000,"ns_per_op":1.0,"reporter_bound":False})
        result=m.recompute_micro_result(trial,mode);m.write_json(trial/"result.json",result);m._seal_artifacts(trial,mode,True);return result

    def test_resume_requires_skip_build(self):
        with mock.patch.object(m.sys,"argv",[str(P),"--output","/tmp/evidence","--resume"]):
            with self.assertRaises(SystemExit) as error:m._main()
        self.assertEqual(error.exception.code,2)

    def test_resume_preserves_identical_duplicate_and_runs_only_missing(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);(out/"trials").mkdir();(out/"micro").mkdir()
            orders=[("out","counters")];micro_orders=[("out",)]
            (out/"calibration.json").write_text('{"frozen_load": 10}')
            trial=out/"trials"/"block-00-0-out";existing=self.seal_e2e(trial);existing.update(block=0,position=0)
            (out/"raw-e2e.jsonl").write_text(json.dumps(existing)+"\n"+json.dumps(existing)+"\n")
            micro_dir=out/"micro"/"block-00-0-out";micro_result=self.seal_micro(micro_dir);micro_result.update(block=0,position=0)
            (out/"raw-micro.jsonl").write_text(json.dumps(micro_result)+"\n")
            with mock.patch.object(m,"run_trial",return_value=self.e2e("counters")) as trial_mock,mock.patch.object(m,"run_micro",return_value=self.micro("counters")) as micro_mock:
                e2e,micro_rows=m._resume_trials(out,{"out":Path("out"),"instrumented":Path("instrumented")},orders,micro_orders)
            self.assertEqual(trial_mock.call_count,1);self.assertEqual(micro_mock.call_count,0)
            self.assertEqual(len(e2e),2);self.assertEqual(len(micro_rows),1);self.assertEqual(m._RESUME_DUPLICATE_COUNT,0)
            self.assertEqual(len(m._read_jsonl(out/"raw-e2e.jsonl")),2)

    def test_e2e_only_resume_does_not_touch_micro(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);(out/"trials").mkdir();(out/"calibration.json").write_text('{"frozen_load": 10}')
            with mock.patch.object(m,"run_trial",return_value=self.e2e("out")),mock.patch.object(m,"run_micro") as micro_mock:
                e2e,micro=m._resume_trials(out,{"out":Path("out"),"instrumented":Path("instrumented")},[("out",)],[("out",)],True)
            self.assertEqual(len(e2e),1);self.assertEqual(micro,[]);micro_mock.assert_not_called()
            self.assertFalse((out/"raw-micro.jsonl").exists())

    def test_execution_scope_is_bound_and_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);m.execution_scope(out,"e2e-only",True)
            self.assertEqual(m.execution_scope(out,"e2e-only")["scope"],"e2e-only")
            with self.assertRaisesRegex(ValueError,"scope mismatch"):m.execution_scope(out,"full")

    def test_e2e_only_summary_uses_only_e2e_verdict(self):
        modes=[x for _ in range(24) for x in m.MODES]
        e2e=[self.e2e(mode,block=i//3,position=i%3) for i,mode in enumerate(modes)]
        with mock.patch.object(m,"comparisons",return_value=(list(range(24)),{},"PASS")):
            summary=m.e2e_only_summary(e2e,448000)
        self.assertEqual(summary["verdict"],"PASS");self.assertTrue(summary["complete_e2e"])
        self.assertEqual(summary["micro_verdict"],"NOT_APPLICABLE");self.assertEqual(summary["micro_trial_count"],0)
        self.assertEqual(m.summary_label(summary),"# Core Metrics lossless v2 E2E: PASS")

    def test_registered_e2e_rejects_wrong_load_duration_and_boundaries(self):
        value=self.e2e()
        m._validate_registered_e2e(value,10)
        for change in ({"hz":11},{"measured_sec":5},{"measure_end_monotonic_ns":59_000_000_001}):
            with self.assertRaises(ValueError):m._validate_registered_e2e({**value,**change},10)

    def test_quality_cannot_contradict_correctness(self):
        value=self.e2e(valid=False)
        with self.assertRaisesRegex(ValueError,"contradicts"):m._validate_trial_payload(value,"out",False)

    def test_corrupt_result_fails_closed(self):
        with tempfile.TemporaryDirectory() as d:
            trial=Path(d)/"trial";self.seal_e2e(trial);path=trial/"result.json";value=json.loads(path.read_text());del value["throughput"];m.write_json(path,value)
            with self.assertRaisesRegex(ValueError,"sealed artifact mismatch"):m._load_trial_result(path,0,0,"out")

    def test_invalid_trial_is_preserved_as_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            trial=Path(d)/"trial";self.seal_e2e(trial,valid=False);path=trial/"result.json"
            self.assertFalse(m._load_trial_result(path,0,0,"out")["valid"])

    def test_mode_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as d:
            trial=Path(d)/"trial";self.seal_e2e(trial,"off");path=trial/"result.json"
            with self.assertRaises(ValueError):m._load_trial_result(path,0,0,"out")

    def test_coherently_rewritten_raw_and_result_cannot_override_seal(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);(out/"micro").mkdir();trial=out/"trials"/"block-00-0-out"
            (out/"calibration.json").write_text('{"frozen_load": 10}')
            result=self.seal_e2e(trial);conflict=dict(result);conflict["throughput"]=2;m.write_json(trial/"result.json",conflict)
            (out/"raw-e2e.jsonl").write_text(json.dumps({**conflict,"block":0,"position":0})+"\n")
            with self.assertRaisesRegex(ValueError,"sealed artifact mismatch"):m._resume_trials(out,{"out":Path("out")},[("out",)],[])

    def test_raw_jsonl_is_reconstructed_from_seal(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);(out/"micro").mkdir();(out/"calibration.json").write_text('{"frozen_load": 10}')
            sealed=self.seal_e2e(out/"trials"/"block-00-0-out")
            (out/"raw-e2e.jsonl").write_text(json.dumps({"block":0,"position":0,"mode":"out","throughput":999999})+"\n")
            rows,_=m._resume_trials(out,{"out":Path("unused")},[("out",)],[],True)
            self.assertEqual(rows[0]["throughput"],sealed["throughput"]);self.assertEqual(m._read_jsonl(out/"raw-e2e.jsonl")[0]["throughput"],sealed["throughput"])

    def test_missing_seal_is_not_resume_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);trial=out/"trials"/"block-00-0-out";trial.mkdir(parents=True);m.write_json(trial/"result.json",self.e2e())
            (out/"calibration.json").write_text('{"frozen_load": 10}')
            with mock.patch.object(m,"run_trial",return_value=self.e2e()) as run:
                m._resume_trials(out,{"out":Path("out")},[("out",)],[],True)
            run.assert_called_once()

    def test_sealed_status_tamper_fails(self):
        with tempfile.TemporaryDirectory() as d:
            trial=Path(d)/"trial";self.seal_e2e(trial);status=json.loads((trial/"status.json").read_text());status["unique_measured"]=59;m.write_json(trial/"status.json",status)
            with self.assertRaisesRegex(ValueError,"sealed artifact mismatch"):m._load_sealed_result(trial,"out")

    def test_coherently_rewritten_seal_cannot_omit_subscriber_log(self):
        with tempfile.TemporaryDirectory() as d:
            trial=Path(d)/"trial";self.seal_e2e(trial);path=trial/"seal.json";seal=json.loads(path.read_text())
            seal["artifacts"]=[item for item in seal["artifacts"] if item["path"]!="subscriber.log"]
            (trial/"subscriber.log").unlink();path.chmod(0o644);m.write_json(path,seal)
            with self.assertRaisesRegex(ValueError,"noncanonical trial seal artifacts"):m._load_sealed_result(trial,"out")

    def test_lossless_v2_summary_counts_complete_and_incomplete_runs(self):
        modes=[x for _ in range(24) for x in m.MODES]
        e2e=[self.e2e(mode,block=i//3,position=i%3) for i,mode in enumerate(modes)]
        micro=[self.micro(mode,block=i//3,position=i%3) for i,mode in enumerate([x for _ in range(30) for x in m.MODES])]
        def fake(rows,*args):return (list(range(24 if len(rows)==72 else 30)),{},"PASS")
        with mock.patch.object(m,"comparisons",side_effect=fake):
            complete=m.lossless_v2_summary(e2e,micro,123);incomplete=m.lossless_v2_summary(e2e[:-1],micro[:-1],123)
        self.assertEqual((complete["verdict"],complete["e2e_count"],complete["micro_count"]),("PASS",72,30))
        self.assertTrue(complete["complete_e2e"]);self.assertTrue(complete["complete_micro"])
        self.assertEqual(incomplete["verdict"],"INCONCLUSIVE");self.assertFalse(incomplete["complete_e2e"]);self.assertFalse(incomplete["complete_micro"])

    def test_summary_label_and_writer(self):
        with tempfile.TemporaryDirectory() as d:
            summary={"verdict":"INCONCLUSIVE"};m.write_summary(Path(d),summary)
            self.assertEqual(m.summary_label(summary),"# Core Metrics lossless v2: INCONCLUSIVE")
            self.assertTrue((Path(d)/"summary.md").read_text().startswith(m.summary_label(summary)))

    def test_resume_verification_uses_archived_snapshot_not_current_tree(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);prov={"schema":"ibmw-core-metrics-provenance-v3","protocol":m.PROTOCOL,"plan_sha256":"plan","source_manifest_sha256":"manifest","source_snapshot_sha256":"snapshot","source_state_sha256":"state","dependency_manifest_sha256":"dm","dependency_aggregate_sha256":"da","execution_binding_sha256":"binding","cmake_source":m.SOURCE_DIRECTORY,"git_context":{"records":[{}]},"host_records":[{}],"registered_build":{},"builds":{}}
            (out/"provenance.json").write_text(json.dumps(prov));builds={"out":out/"build-out","instrumented":out/"build-instrumented"}
            with mock.patch.object(m,"_verify_plan",return_value="plan") as plan,mock.patch.object(m,"verify_source_snapshot",return_value={"source_state_sha256":"state"}) as snapshot,mock.patch.object(m,"verify_dependency_snapshot",return_value={"manifest_sha256":"dm","aggregate_sha256":"da"}),mock.patch.object(m,"execution_binding",return_value={"sha256":"binding"}),mock.patch.object(m,"execution_scope",return_value={"legacy_unbound":True}),mock.patch.object(m,"verify_registered_builds"),mock.patch.object(m,"collect_build_artifacts",return_value={}):
                self.assertEqual(m.verify_resume_evidence(out),builds)
            plan.assert_called_once_with(out);snapshot.assert_called_once_with(out,"manifest","snapshot")

if __name__=="__main__":unittest.main()
