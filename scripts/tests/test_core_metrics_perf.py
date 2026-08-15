#!/usr/bin/env python3
import importlib.util, json, os, random, tempfile, unittest
from pathlib import Path
from unittest import mock

P=Path(__file__).parents[1]/"core_metrics_perf.py"
ROOT=P.parents[1]
spec=importlib.util.spec_from_file_location("cmp",P)
assert spec is not None and spec.loader is not None
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class PerfRunnerTest(unittest.TestCase):
 def make_source(self,root):
  (root/"cmake").mkdir();(root/"src").mkdir();(root/"scripts").mkdir()
  (root/"CMakeLists.txt").write_text("project(snapshot)\n");(root/"VERSION").write_text("1.0\n")
  (root/"cmake"/"flags.cmake").write_text("set(X ON)\n");(root/"src"/"tracked.cc").write_text("tracked\n")
  (root/"scripts"/"ignored.txt").write_text("ignored but included\n");(root/"scripts"/"untracked.py").write_text("print('included')\n")
 def snapshot(self,root,out):
  out.mkdir();return m.create_source_snapshot(root,out)
 def write_registered_caches(self,out,registration):
  for build in registration["builds"].values():
   directory=out/build["directory"];directory.mkdir()
   lines=[f"CMAKE_HOME_DIRECTORY:INTERNAL={registration['cmake_source']}"]
   lines.extend(f"{key}:STRING={value}" for key,value in build["configure_options"].items())
   (directory/"CMakeCache.txt").write_text("\n".join(lines)+"\n")
 def test_bootstrap_is_deterministic_and_paired(self):
  pairs=[{"a":{"x":2.0},"b":{"x":1.0}},{"a":{"x":3.0},"b":{"x":2.0}}]
  x=m.bootstrap_ratio(pairs,"a","b","x",random.Random(7),10000)
  y=m.bootstrap_ratio(pairs,"a","b","x",random.Random(7),10000)
  self.assertEqual(x,y); self.assertEqual(x["ratio"],1.75)
 def test_yaml_fixes_registered_workload(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/"p.yaml"; m.yaml(p,True,4000,10,60,5,publisher_status_path="/tmp/pub.json",measure_begin_ns=1000000000,measure_end_ns=61000000000); text=p.read_text()
   self.assertIn("payload_bytes: 4096",text); self.assertIn("batch_size: 128",text)
   self.assertIn("warmup_sec: 10",text); self.assertIn("measured_sec: 60",text)
   self.assertIn("measure_begin_monotonic_ns: 1000000000",text)
   self.assertIn("publisher_status_path: /tmp/pub.json",text)
   self.assertIn('runtime_id: "metrics_r7_pub"',text)
   self.assertIn("batch_size: 128",text)
   self.assertIn("label: core_metrics_round7_4KiB",text)
 def test_thresholds_and_registration(self):
  self.assertEqual(m.THRESHOLDS["off_out"]["p99"],.05)
  self.assertEqual(m.THRESHOLDS["counters_out"]["throughput"],.07)
  self.assertEqual(m.SEED,1229081943); self.assertEqual(m.CPUS,(25,26,27))
 def test_registered_build_configuration_pins_options_and_runner_python(self):
  with tempfile.TemporaryDirectory() as d:
   registration=m.registered_build_configuration(Path(d))
   self.assertEqual(registration["runner_python"],str(Path(m.sys.executable).resolve()))
   for name,build in registration["builds"].items():
    options=build["configure_options"]
    self.assertEqual(options["CMAKE_BUILD_TYPE"],"Release")
    self.assertEqual(options["IBMW_BUILD_TESTS"],"OFF")
    self.assertEqual(options["IBMW_BUILD_DDS_EXTENSION"],"OFF")
    self.assertEqual(options["IBMW_BUILD_ROS2_TYPES"],"ON")
    self.assertEqual(options["Python3_EXECUTABLE"],registration["runner_python"])
    expected="OFF" if name=="out" else "ON"
    self.assertEqual(options["IBMW_ENABLE_CORE_METRICS"],expected)
    self.assertEqual(options["IBMW_ENABLE_REPORTER_V1"],expected)
 def test_registered_build_cache_rejects_foreign_source_and_changed_option(self):
  with tempfile.TemporaryDirectory() as d:
   out=Path(d);registration=m.registered_build_configuration(out);self.write_registered_caches(out,registration)
   self.assertEqual(m.verify_registered_builds(out,registration),registration)
   cache=out/"build-out"/"CMakeCache.txt";original=cache.read_text()
   cache.write_text(original.replace(registration["cmake_source"],str(out/"foreign-source"),1))
   with self.assertRaisesRegex(ValueError,"CMAKE_HOME_DIRECTORY mismatch"):
    m.verify_registered_builds(out,registration)
   cache.write_text(original.replace("IBMW_BUILD_TESTS:STRING=OFF","IBMW_BUILD_TESTS:STRING=ON"))
   with self.assertRaisesRegex(ValueError,"required CMake option mismatch: IBMW_BUILD_TESTS"):
    m.verify_registered_builds(out,registration)
 def test_rehashed_semantically_altered_plan_is_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   out=Path(d);plan=m.registered_plan();m.write_json(out/"plan.json",plan)
   (out/"plan.sha256").write_text(m.sha(out/"plan.json")+"  plan.json\n")
   self.assertEqual(m._verify_plan(out),m.sha(out/"plan.json"))
   plan["capacity"]["candidates"][0]+=16;m.write_json(out/"plan.json",plan)
   (out/"plan.sha256").write_text(m.sha(out/"plan.json")+"  plan.json\n")
   with self.assertRaisesRegex(ValueError,"plan semantics mismatch"):m._verify_plan(out)
 def test_source_snapshot_is_deterministic_and_captures_file_state(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/"root";root.mkdir();self.make_source(root)
   executable=root/"scripts"/"untracked.py";executable.chmod(0o755)
   os.symlink("untracked.py",root/"scripts"/"tool-link")
   first=self.snapshot(root,Path(d)/"one");second=self.snapshot(root,Path(d)/"two")
   self.assertEqual(first,second)
   self.assertEqual((Path(d)/"one"/m.SOURCE_ARCHIVE).read_bytes(),(Path(d)/"two"/m.SOURCE_ARCHIVE).read_bytes())
   entries={x["path"]:x for x in json.loads((Path(d)/"one"/m.SOURCE_MANIFEST).read_text())["entries"]}
   self.assertEqual(entries["scripts/untracked.py"]["mode"],"100755");self.assertEqual(entries["scripts/tool-link"]["kind"],"symlink")
   executable.write_text("print('modified')\n");third=self.snapshot(root,Path(d)/"three")
   self.assertNotEqual(first["source_state_sha256"],third["source_state_sha256"])
   executable.write_text("print('included')\n");executable.chmod(0o644);fourth=self.snapshot(root,Path(d)/"four")
   self.assertNotEqual(first["source_state_sha256"],fourth["source_state_sha256"])
   (root/"scripts"/"tool-link").unlink();os.symlink("ignored.txt",root/"scripts"/"tool-link");fifth=self.snapshot(root,Path(d)/"five")
   self.assertNotEqual(fourth["source_state_sha256"],fifth["source_state_sha256"])
 def test_snapshot_exclusions_are_structural(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/"root";root.mkdir();self.make_source(root)
   for directory in (root/"scripts"/"__pycache__",root/"scripts"/"build",root/"scripts"/"evidence",root/"src"/".opencode"):
    directory.mkdir(parents=True);(directory/"excluded.txt").write_text("no\n")
   (root/"src"/"cache.pyc").write_bytes(b"no");out=Path(d)/"out";self.snapshot(root,out)
   paths=[x["path"] for x in json.loads((out/m.SOURCE_MANIFEST).read_text())["entries"]]
   self.assertFalse(any("excluded.txt" in x or x.endswith(".pyc") for x in paths))
 def test_extracted_verification_uses_manifest_global_path_order(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/"root";root.mkdir();self.make_source(root)
   (root/"cmake"/"z").mkdir();(root/"cmake"/"z"/"nested.cmake").write_text("set(Z ON)\n")
   (root/"src"/"a.cc").write_text("a\n")
   self.snapshot(root,Path(d)/"out")
 def test_snapshot_rejects_unsafe_special_and_tampered_inputs(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)/"root";root.mkdir();self.make_source(root);os.symlink("../../outside",root/"scripts"/"escape")
   out=Path(d)/"unsafe";out.mkdir()
   with self.assertRaisesRegex(ValueError,"unsafe symlink"):m.create_source_snapshot(root,out)
   (root/"scripts"/"escape").unlink();os.mkfifo(root/"src"/"special")
   with self.assertRaisesRegex(ValueError,"unsupported source entry"):m.collect_source_state(root)
   (root/"src"/"special").unlink();good=Path(d)/"good";binding=self.snapshot(root,good)
   (good/m.SOURCE_ARCHIVE).write_bytes((good/m.SOURCE_ARCHIVE).read_bytes()+b"tamper")
   with self.assertRaisesRegex(ValueError,"snapshot digest"):m.verify_source_snapshot(good,binding["manifest_sha256"],binding["snapshot_sha256"])
 def test_build_artifacts_bind_plugin_size_path_and_hash(self):
  with tempfile.TemporaryDirectory() as d:
   out=Path(d);builds={"out":out/"build-out","instrumented":out/"build-instrumented"}
   for kind,directory in builds.items():
    directory.mkdir()
    for name in m.ARTIFACTS:(directory/name).write_bytes((kind+name).encode())
   records=m.collect_build_artifacts(out,builds);plugin=records["instrumented"]["libibmw_iceoryx_transport.so"]
   self.assertEqual(set(plugin),{"path","size","sha256"});self.assertEqual(plugin["path"],"build-instrumented/libibmw_iceoryx_transport.so")
   self.assertEqual(plugin["size"],len(b"instrumentedlibibmw_iceoryx_transport.so"))
 def test_skip_build_missing_evidence_is_rejected_without_provenance(self):
  with tempfile.TemporaryDirectory() as d:
   out=Path(d)
   with mock.patch.object(m,"topology",return_value={"valid":True}),mock.patch.object(m.sys,"argv",[str(P),"--output",str(out),"--skip-build"]):
    with self.assertRaisesRegex(SystemExit,"evidence verification failed"):m.main()
   self.assertFalse((out/"provenance.json").exists())
 def test_overall_verdict_truth_table_and_no_load_failure(self):
  expected={("PASS","PASS"):"PASS",("PASS","INCONCLUSIVE"):"INCONCLUSIVE",("INCONCLUSIVE","PASS"):"INCONCLUSIVE",("INCONCLUSIVE","INCONCLUSIVE"):"INCONCLUSIVE",("FAIL","PASS"):"FAIL",("PASS","FAIL"):"FAIL",("FAIL","INCONCLUSIVE"):"FAIL",("INCONCLUSIVE","FAIL"):"FAIL",("FAIL","FAIL"):"FAIL"}
  for values,verdict in expected.items():self.assertEqual(m.aggregate_overall(*values),verdict)
  self.assertEqual(m.no_load_outcome("FAIL"),("FAIL",1));self.assertEqual(m.no_load_outcome("PASS"),("INCONCLUSIVE",2))
  self.assertEqual({v:m.verdict_exit_code(v) for v in ("PASS","FAIL","INCONCLUSIVE")},{"PASS":0,"FAIL":1,"INCONCLUSIVE":2})
  with self.assertRaises(ValueError):m.verdict_exit_code("UNKNOWN")
 def test_counter_conservation(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/"metrics.jsonl"
   p.write_text("\n".join('{"kind":"direct_iceoryx_loan","metric":"%s","value":%d}' % x for x in (("logical_attempt",2),("logical_success",2),("logical_fail",0),("publish",2),("release",0))))
   self.assertTrue(m.counter_conservation(p)["valid"])
 def test_proc_read_failure_is_invalid_not_zero(self):
  self.assertIsNone(m.proc_ticks(999999999)); self.assertIsNone(m.pss_kib(999999999))
 def test_cleanup_removes_only_explicit_owned_resources(self):
  with tempfile.TemporaryDirectory(dir="/tmp") as d:
   root=Path(d);owned=root/"owned";unrelated=root/"iceoryx-unrelated"
   owned.write_text("owned");unrelated.write_text("keep")
   # Nested test paths are deliberately rejected: callers must register an
   # exact top-level /tmp or /dev/shm resource, never a broad parent tree.
   m.cleanup_owned_resources((owned,unrelated))
   self.assertTrue(owned.exists());self.assertTrue(unrelated.exists())
  token=Path("/tmp")/f"ibmw-owned-{os.getpid()}-{random.randrange(1<<30)}"
  unrelated=Path("/tmp")/f"iceoryx-unrelated-{os.getpid()}-{random.randrange(1<<30)}"
  try:
   token.write_text("owned");unrelated.write_text("keep")
   m.cleanup_owned_resources((token,))
   self.assertFalse(token.exists());self.assertTrue(unrelated.exists())
  finally:
   token.unlink(missing_ok=True);unrelated.unlink(missing_ok=True)
 def test_cleanup_rejects_unowned_and_symlink_paths(self):
  target=Path("/tmp")/f"ibmw-target-{os.getpid()}-{random.randrange(1<<30)}"
  link=Path("/tmp")/f"ibmw-link-{os.getpid()}-{random.randrange(1<<30)}"
  try:
   target.write_text("keep");link.symlink_to(target)
   m.cleanup_owned_resources((target,),uid=os.getuid()+1)
   m.cleanup_owned_resources((link,))
   self.assertTrue(target.exists());self.assertTrue(link.is_symlink())
  finally:
   link.unlink(missing_ok=True);target.unlink(missing_ok=True)
 def test_cpu_ticks_use_actual_sample_interval(self):
  self.assertEqual(m.cpu_cores_for_interval(1_000_000_000,3_500_000_000,10,260,100),1.0)
  with self.assertRaises(m.InvalidTrial):m.cpu_cores_for_interval(2,2,0,1,100)
 def test_three_state_and_throughput_direction(self):
  self.assertEqual(m.classify_ratio({"lower":.99,"upper":1.01},.02),"PASS")
  self.assertEqual(m.classify_ratio({"lower":1.03,"upper":1.04},.02),"FAIL")
  self.assertEqual(m.classify_ratio({"lower":1.01,"upper":1.03},.02),"INCONCLUSIVE")
  self.assertEqual(m.classify_ratio({"lower":.99,"upper":1.01},.02,True),"PASS")
  self.assertEqual(m.classify_ratio({"lower":.95,"upper":.97},.02,True),"FAIL")
 def test_absolute_delta_is_paired(self):
  pairs=[{"a":{"x":15},"b":{"x":10}},{"a":{"x":30},"b":{"x":20}}]
  self.assertEqual(m.bootstrap_delta(pairs,"a","b","x",random.Random(2),1000)["delta"],7.5)
 def test_measured_window_end_precedes_cooldown(self):
  source=P.read_text()
  end=source.index("end_sample_ns=time.monotonic_ns()")
  cooldown=source.index("natural_deadline=time.monotonic()+cooldown+3")
  elapsed=source.index("cpu_elapsed=(end_sample_ns-start_sample_ns)")
  self.assertLess(end,cooldown); self.assertLess(cooldown,elapsed)
  self.assertIn("measured_end={k:proc_ticks(v.pid)",source)
 def test_formal_invalid_trial_writes_inconclusive_summary(self):
  with tempfile.TemporaryDirectory() as d:
   out=Path(d)/"evidence"
   with mock.patch.object(m,"_main",side_effect=m.InvalidTrial("controlled failure")),mock.patch.object(m.sys,"argv",[str(P),"--output",str(out)]):
    self.assertEqual(m.main(),2)
   summary=json.loads((out/"summary.json").read_text())
   self.assertEqual(summary["verdict"],"INCONCLUSIVE")
   self.assertIn("controlled failure",summary["reason"])
 def test_subscriber_window_baseline_and_csv_are_independent_of_validity(self):
  source=(ROOT/"src/samples/cpp/transport_benchmark/iox_loaned_ctx/managed/subscriber_module/subscriber_module.cc").read_text()
  self.assertIn("!have_last_sequence_ && opts_.measured_sec == 0",source)
  self.assertIn("if (!opts_.csv_path.empty())",source)
  self.assertNotIn("if (valid && !opts_.csv_path.empty())",source)
 def test_reporter_benchmark_owns_metrics_lifecycle_outside_timing(self):
  source=(ROOT/"src/samples/cpp/transport_benchmark/iox_loaned_ctx/standalone/reporter_hot_bench/main.cc").read_text()
  initialize=source.index("metrics.Initialize(&kMetricsOwner)")
  query=source.index("ibmw_reporter_query_v1()")
  start=source.index("metrics.Start(&kMetricsOwner)")
  begin=source.index("const auto begin =")
  end=source.index("const auto end =")
  finalize=source.index("if (!metrics.FinalizeSnapshot(&kMetricsOwner))")
  self.assertLess(initialize,query); self.assertLess(query,start)
  self.assertLess(start,begin); self.assertLess(end,finalize)
  self.assertIn("#ifdef IBMW_ENABLE_CORE_METRICS",source)
  self.assertIn("const ibmw_reporter_v1* observed = Opaque(api)",source)
  self.assertNotIn("sink += count",source)
 def test_reconciliation_rejects_boundary_loss(self):
  pub={"measure_begin_monotonic_ns":10,"measure_end_monotonic_ns":20,"attempts":3,"success":3,"fail":0,"first_success_seq":7,"last_success_seq":9,"valid":True}
  sub={"measure_begin_monotonic_ns":10,"measure_end_monotonic_ns":20,"unique_measured":3,"first_seq":7,"last_seq":9,"duplicate":0,"out_of_order":0,"missing":0,"span_missing":0,"invalid":0,"valid":True}
  self.assertTrue(m.reconcile_status(pub,sub,10,20)["valid"])
  for changed in ({"first_seq":8},{"last_seq":8},{"unique_measured":2},{"span_missing":1}):
   bad=dict(sub);bad.update(changed)
   self.assertFalse(m.reconcile_status(pub,bad,10,20)["valid"])
 def test_reporter_hot_value_requires_exact_metric(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/"metrics.jsonl";p.write_text('{"kind":"direct_iceoryx_loan","metric":"hot_hook","value":22000000}\n')
   self.assertEqual(m.reporter_hot_value(p),[22000000])
 def test_gate_uses_running_markers_and_reconciled_confirmation(self):
  source=P.read_text()
  self.assertIn('"PERF_SUBSCRIBER_RUNNING"',source)
  self.assertIn('"PERF_PUBLISHER_RUNNING"',source)
  self.assertIn('publisher did not complete controlled shutdown',source)
  self.assertNotIn('add_argument("--resume"',source)
 def test_unpaced_probe_is_diagnostic_only_even_when_lossless(self):
  lossy=m.classify_unpaced_probe({"valid":False,"throughput":500000,"reconciliation":{"valid":False}})
  lossless=m.classify_unpaced_probe({"valid":True,"throughput":400000,"reconciliation":{"valid":True}})
  self.assertEqual(lossy["diagnostic_class"],"lossy")
  self.assertEqual(lossless["diagnostic_class"],"lossless")
  for result in (lossy,lossless):
   self.assertFalse(result["eligible"])
   self.assertIsNone(result["throughput_capacity_estimate"])
 def test_round7_candidates_are_preregistered_and_batch_aligned(self):
  self.assertEqual(m.BATCH_SIZE,128)
  self.assertEqual(m.CAPACITY_CANDIDATES,(448000,435200,460800,422400,409600,473600))
  self.assertTrue(all(value%m.BATCH_SIZE==0 for value in m.CAPACITY_CANDIDATES))
 def test_confirmation_requires_all_strict_gates(self):
  valid={"valid":True,"reconciliation":{"valid":True},"correctness":{"valid":True},"publisher_correctness":{"valid":True},"bottleneck_utilization":.775}
  self.assertTrue(m.confirmation_is_strictly_valid(valid))
  for patch in ({"valid":False},{"bottleneck_utilization":.69},{"bottleneck_utilization":.86},{"reconciliation":{"valid":False}},{"correctness":{"valid":False}},{"publisher_correctness":{"valid":False}}):
   candidate=dict(valid);candidate.update(patch)
   self.assertFalse(m.confirmation_is_strictly_valid(candidate))

if __name__=="__main__":unittest.main()
