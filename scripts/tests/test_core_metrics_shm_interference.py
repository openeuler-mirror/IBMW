#!/usr/bin/env python3
"""Interference accounting of the shm Core Metrics gate (core_metrics_shm_perf.py)."""
import ast, contextlib, importlib.util, io, json, os, shutil, subprocess, sys, tempfile, time, unittest
from pathlib import Path
from unittest import mock

TESTS=Path(__file__).resolve().parent
SCRIPTS=TESTS.parent
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module
shm=load("cm_shm_interference",SCRIPTS/"core_metrics_shm_perf.py");g=shm.gate
iox=load("cm_iox_interference_reference",SCRIPTS/"core_metrics_perf.py")

def _host_problem():
    """Why this host cannot run the tests that read or load the bound CPUs; None if it can."""
    cpus=set(g.CPUS)
    try:
        allowed=os.sched_getaffinity(0)
        if not cpus<=allowed:return f"bound CPUs {sorted(cpus-allowed)} are outside this process's affinity"
        if not allowed-cpus:return "no CPU is left outside the bound set"
        if not cpus<=set(shm.read_cpu_times()) or not cpus<=set(shm.read_cpu_busy()):return "bound CPUs are missing from /proc/schedstat or /proc/stat"
    except (OSError,ValueError,IndexError,g.InvalidTrial) as error:return f"/proc/schedstat or /proc/stat unusable: {error}"
    if shutil.which("taskset") is None:return "taskset is not installed"
    return None
HOST_PROBLEM=_host_problem();HOST_OK=HOST_PROBLEM is None
REQUIRE_HOST=os.environ.get("IBMW_REQUIRE_HOST_TESTS")=="1"
def host_only(cls):
    """Skip on a host without the gate's CPUs; IBMW_REQUIRE_HOST_TESTS=1 makes that a failure."""
    if HOST_OK:return cls
    if REQUIRE_HOST:
        def setUp(self):self.fail(f"IBMW_REQUIRE_HOST_TESTS=1 but {HOST_PROBLEM}")
        cls.setUp=setUp;return cls
    return unittest.skip(f"host cannot run gate CPU tests: {HOST_PROBLEM}")(cls)
GATE_SCRIPTS=("core_metrics_perf.py","core_metrics_shm_perf.py")
def running_gates():
    """pids of Core Metrics gates running on this host."""
    found=[]
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid)==os.getpid():continue
        try:argv=Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")[:3]
        except OSError:continue
        if any(Path(x.decode(errors="replace")).name in GATE_SCRIPTS for x in argv):found.append(int(pid))
    return found

E2E_CPUS=("26","27")
QUIET={"checks":1,"waited_sec":2.0,"timed_out":False,"last_foreign_cores":0.0,"top_foreign":[]}
BASE_KEYS={"mode","hz","measured_sec","cpu_cores","cpu_by_process","cpu_sample_elapsed_sec","bottleneck_utilization","pss_median_kib","pss_peak_kib","pss_valid_samples","pss_invalid_samples","thread_affinity","delivery_profile","measure_begin_monotonic_ns","measure_end_monotonic_ns","subscriber_ready_ns","publisher_ready_ns","cpu_sample_begin_ns","cpu_sample_end_ns"}
STAT="""cpu  900 0 900 9000 0 0 0 0 0 0
cpu25 100 5 20 9000 7 3 2 1 40 0
cpu26 10 0 0 500 0 0 0 0 0 0
intr 1 2 3
ctxt 9
"""
SCHEDSTAT="""version 15
timestamp 4361229973
cpu25 0 0 0 0 0 0 13061792732177 3243503458314 88995415
domain0 00000000,0f000000 1 2 3
cpu26 0 0 0 0 0 0 7 8 9
"""
FAKE_APP='''#!%s -I
import json, re, sys, time
from pathlib import Path
text=Path(sys.argv[1]).read_text()
find=lambda key:re.search(key+r": (\\S+)",text).group(1)
begin=int(find("measure_begin_monotonic_ns"));end=int(find("measure_end_monotonic_ns"))
window={"measure_begin_monotonic_ns":begin,"measure_end_monotonic_ns":end,"valid":True}
wait=lambda ns:time.sleep(max(0,(ns-time.monotonic_ns())/1e9))
if "publisher_status_path" in text:
    print("PERF_PUBLISHER_RUNNING",flush=True);wait(end)
    Path(find("publisher_status_path")).write_text(json.dumps({**window,"attempts":60,"success":60,"fail":0,"first_attempt_seq":1,"last_attempt_seq":60,"first_success_seq":1,"last_success_seq":60,"completed_send_window":True}))
else:
    print("PERF_SUBSCRIBER_RUNNING",flush=True);wait(end)
    Path(find("status_path")).write_text(json.dumps({**window,"received_measured":60,"unique_measured":60,"duplicate":0,"out_of_order":0,"missing":0,"span_missing":0,"first_seq":1,"last_seq":60,"invalid":0}))
    Path(find("csv_path")).write_text("label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\\nfake,60,1,1,0,1,1,1,1,1\\n")
    time.sleep(30)
'''%sys.executable
FAKE_BROKER=f'#!{sys.executable} -I\nimport time\ntime.sleep(60)\n'
FAKE_BENCH=f'''#!{sys.executable} -I
print('{{"iterations":20000000,"elapsed_ns":20000000,"ns_per_op":1.0,"reporter_bound":false}}')
'''
def executable(path,text):
    path.write_text(text);path.chmod(0o755)
def sample(ns=1_000_000_000,run=(.7,.7),own=(70,70),scan=(0,0)):
    """One raw sample: run as cores over the sample, own and scan as ticks."""
    return {"ns":ns,"run_ns":{c:round(r*ns) for c,r in zip(E2E_CPUS,run)},"own_ticks":dict(zip(E2E_CPUS,own)),"scan_ticks":dict(zip(E2E_CPUS,scan))}
def evidence(samples=None,begin=1,clk=100,quiet=QUIET,**extra):
    samples=[sample() for _ in range(60)] if samples is None else samples
    value={"schema":shm.INTERFERENCE_SCHEMA,"cpus":[26,27],"own_cpus":{"26":"subscriber","27":"publisher"},"clk_tck":clk,
           "begin_ns":begin,"end_ns":begin+sum(x["ns"] for x in samples),"samples":samples,"wait_ns":{"26":0,"27":0},"tick_busy_cores":1.4,
           "context":{"25":{"run_cores":0.0,"tick_busy_cores":0.0},"24":{"run_cores":0.0,"tick_busy_cores":0.0}},"host_busy_cores":1.0,"top_foreign":[],
           "quiet":quiet,"sampler":{"scans":61,"cpu_sec":1.0,"max_scan_ms":30.0,"unknown_tasks":0,"affinity":[0,1]},"threshold":shm.thresholds()}
    value.update(extra);value.update(shm.derive_interference(value));return value
def with_sample(index,changed,count=60):
    return [changed if i==index else sample() for i in range(count)]
def disturbed():
    # One second with 0.4 foreign cores on the publisher's CPU: the mean stays low.
    return evidence(with_sample(10,sample(run=(.7,1.1))),top_foreign=[{"pid":7,"comm":"cc1plus","cores":.0067}])
def micro_evidence(own_ns=-450_000,wait_ns=0,bench_cpu_us=17_000,elapsed=17_600_000,quiet=QUIET):
    value={"schema":shm.MICRO_INTERFERENCE_SCHEMA,"cpu":27,"begin_ns":1,"end_ns":1+elapsed,"run_ns":bench_cpu_us*1000+own_ns,"wait_ns":wait_ns,
           "bench_cpu_us":bench_cpu_us,"context_run_ns":{"25":0,"26":0,"24":0},"quiet":quiet,"sampler":{"affinity":[0,1]},"threshold":shm.micro_thresholds()}
    value.update(shm.derive_micro_interference(value));return value
def seal_e2e(trial,mode="out",interference=None):
    trial.mkdir(parents=True);(trial/"metrics").mkdir();begin=1;end=60_000_000_001;unique=60
    window={"measure_begin_monotonic_ns":begin,"measure_end_monotonic_ns":end,"valid":True}
    g.write_json(trial/"status.json",{**window,"received_measured":unique,"unique_measured":unique,"duplicate":0,"out_of_order":0,"missing":0,"span_missing":0,"first_seq":1,"last_seq":60,"invalid":0})
    g.write_json(trial/"publisher-status.json",{**window,"attempts":unique,"success":unique,"fail":0,"first_attempt_seq":1,"last_attempt_seq":60,"first_success_seq":1,"last_success_seq":60,"completed_send_window":True})
    (trial/"latency.csv").write_text("label,count,min_us,avg_us,stddev_us,p50_us,p90_us,p99_us,p999_us,max_us\nv1,60,1,1,0,1,1,1,1,1\n")
    for name in ("publisher.yaml","subscriber.yaml","publisher.log","subscriber.log"):(trial/name).write_text(name+"\n")
    base={"mode":mode,"hz":10,"measured_sec":60,"cpu_cores":1.0,"cpu_by_process":{"publisher":.5,"subscriber":.5},"cpu_sample_elapsed_sec":60.0,"bottleneck_utilization":.75,"pss_median_kib":1.0,"pss_peak_kib":1,"pss_valid_samples":1,"pss_invalid_samples":0,"thread_affinity":{},"delivery_profile":g.LOSSLESS_PROFILE,"measure_begin_monotonic_ns":begin,"measure_end_monotonic_ns":end,"subscriber_ready_ns":0,"publisher_ready_ns":0,"cpu_sample_begin_ns":begin,"cpu_sample_end_ns":end}
    if interference is not None:base["interference"]=interference
    g.write_json(trial/"observations.json",{"base":base,"quality_required":True})
    result=g.recompute_e2e_result(trial);g.write_json(trial/"result.json",result);g._seal_artifacts(trial,mode);return result
def row(mode="out",clean=True,block=0,position=0):
    return {"mode":mode,"block":block,"position":position,"quality_valid":clean,"interference":evidence() if clean else disturbed(),"throughput":1.0,"p50_us":1.0,"p99_us":2.0,"cpu_cores":1.0,"pss_median_kib":1.0,"bottleneck_utilization":.75,"reporter_final_valid":True,"publisher_yaml_sha256":"a"*64,"subscriber_yaml_sha256":"b"*64}
def micro_row(mode="out",clean=True,block=0,position=0):
    return {"mode":mode,"block":block,"position":position,"ns_per_op":1.0,"reporter_final_valid":True,"interference":micro_evidence() if clean else micro_evidence(wait_ns=2_000_000)}
def full_e2e(disturbed_index=None):
    return [row(mode,i!=disturbed_index,i//3,i%3) for i,mode in enumerate(m for _ in range(24) for m in g.MODES)]
def full_micro(disturbed_index=None):
    return [micro_row(mode,i!=disturbed_index,i//3,i%3) for i,mode in enumerate(m for _ in range(30) for m in g.MODES)]

class Parsing(unittest.TestCase):
    def test_proc_stat_counts_busy_fields_only(self):
        # user+nice+system+irq+softirq+steal; idle, iowait and guest stay out.
        self.assertEqual(shm.parse_proc_stat(STAT),{25:100+5+20+3+2+1,26:10})
    def test_schedstat_reads_run_and_wait_time(self):
        self.assertEqual(shm.parse_schedstat(SCHEDSTAT),{25:(13061792732177,3243503458314),26:(7,8)})
    def test_unknown_schedstat_version_fails_closed(self):
        with self.assertRaisesRegex(g.InvalidTrial,"unsupported"):shm.parse_schedstat(SCHEDSTAT.replace("version 15","version 99"))
    def test_task_stat_with_hostile_comm(self):
        fields=["S"]+["0"]*50;fields[11]="7";fields[12]="5";fields[19]="1234";fields[36]="26"
        self.assertEqual(shm.parse_task_stat("42 (a) (b c) "+" ".join(fields)),("a) (b c",26,12,1234))
    def test_task_stat_matches_this_process(self):
        comm,cpu,ticks,start=shm.parse_task_stat(Path("/proc/self/stat").read_text())
        self.assertEqual(comm,Path("/proc/self/comm").read_text().strip())
        self.assertIn(cpu,shm.read_cpu_busy());self.assertGreaterEqual(ticks,0);self.assertGreater(start,0)
    def test_cpu_list(self):
        self.assertEqual(shm.parse_cpu_list("24-27\n"),{24,25,26,27})
        self.assertEqual(shm.parse_cpu_list("1,3-4"),{1,3,4})
    def test_l2_neighbors_exclude_bound_cpus(self):
        self.assertFalse(set(shm.l2_neighbors(g.CPUS))&set(g.CPUS))
    def test_judged_cpus_are_the_ones_hosting_our_processes(self):
        self.assertEqual(shm.e2e_own_cpus(),{26:"subscriber",27:"publisher"});self.assertEqual(shm.micro_cpu(),27)
        self.assertEqual(shm.context_cpus()[0],25);self.assertFalse(set(shm.context_cpus())&{26,27})

@host_only
class HostReads(unittest.TestCase):
    def test_real_proc_stat_lists_bound_cpus(self):
        self.assertLessEqual(set(g.CPUS),set(shm.read_cpu_busy()))
    def test_real_schedstat_lists_bound_cpus_and_counts(self):
        times=shm.read_cpu_times();self.assertLessEqual(set(g.CPUS),set(times));self.assertTrue(all(times[c][0]>0 for c in g.CPUS))

class HostGuard(unittest.TestCase):
    def test_required_host_tests_fail_instead_of_skipping(self):
        class Probe(unittest.TestCase):
            def test_x(self):pass
        with mock.patch.multiple(sys.modules[__name__],HOST_OK=False,HOST_PROBLEM="no CPU 27",REQUIRE_HOST=True):
            result=unittest.TestResult();host_only(Probe)("test_x").run(result)
        self.assertEqual((len(result.failures),len(result.skipped)),(1,0));self.assertIn("no CPU 27",result.failures[0][1])
    def test_missing_host_skips_with_the_reason(self):
        class Probe(unittest.TestCase):
            def test_x(self):pass
        with mock.patch.multiple(sys.modules[__name__],HOST_OK=False,HOST_PROBLEM="no CPU 27",REQUIRE_HOST=False):
            result=unittest.TestResult();host_only(Probe)("test_x").run(result)
        self.assertEqual(len(result.skipped),1);self.assertIn("no CPU 27",result.skipped[0][1])
    def test_probe_names_a_restricted_affinity(self):
        with mock.patch.object(os,"sched_getaffinity",return_value=set(range(24))):self.assertIn("affinity",_host_problem())
    def test_probe_names_an_unusable_schedstat(self):
        with mock.patch.object(shm,"read_cpu_times",side_effect=g.InvalidTrial("unsupported /proc/schedstat")),mock.patch.object(os,"sched_getaffinity",return_value=set(range(28))):
            self.assertIn("unsupported",_host_problem())
    def test_running_gate_is_found_by_its_script_name(self):
        with tempfile.TemporaryDirectory() as d:
            script=Path(d)/"core_metrics_shm_perf.py";script.write_text("import time\ntime.sleep(30)\n")
            child=subprocess.Popen([sys.executable,"-I",str(script)])
            try:
                deadline=time.monotonic()+5
                while child.pid not in running_gates() and time.monotonic()<deadline:time.sleep(.01)
                self.assertIn(child.pid,running_gates())
            finally:child.kill();child.wait()
        self.assertNotIn(child.pid,running_gates())

class Attribution(unittest.TestCase):
    def scan(self,*snapshots,boots=None):
        scan=shm.TaskScan({25,26},clk_tck=100);boots=iter(boots or [100*(i+1) for i in range(len(snapshots))])
        with mock.patch.object(scan,"_read",side_effect=snapshots),mock.patch.object(shm,"_boot_ticks",lambda clk:next(boots)):
            per_cpu=[scan.scan() for _ in snapshots]
        return scan,per_cpu
    def test_delta_on_judged_cpu(self):
        scan,per_cpu=self.scan({(1,1,9):("cc1plus",25,100)},{(1,1,9):("cc1plus",25,130)})
        self.assertEqual(scan.top(1),[{"pid":1,"comm":"cc1plus","cores":.3}]);self.assertEqual(per_cpu[1],{25:30,26:0})
    def test_baseline_is_not_attributed(self):
        self.assertEqual(self.scan({(1,1,9):("old",25,5000)})[0].top(1),[])
    def test_other_cpu_is_ignored(self):
        self.assertEqual(self.scan({(1,1,9):("x",3,1)},{(1,1,9):("x",3,90)})[0].top(1),[])
    def test_task_born_between_scans_counts_whole(self):
        scan,per_cpu=self.scan({},{(2,2,150):("ld",26,40)})
        self.assertEqual(scan.top(1),[{"pid":2,"comm":"ld","cores":.4}]);self.assertEqual(per_cpu[1][26],40);self.assertEqual(scan.unknown,0)
    def test_old_task_missed_by_one_scan_is_unknown_not_new(self):
        # Present, absent from one scan (a racing read), present again: its 5000
        # lifetime ticks are not this window's.
        key=(3,3,9)
        scan,per_cpu=self.scan({key:("a",25,5000)},{},{key:("a",25,5030)})
        self.assertEqual(scan.top(1),[]);self.assertEqual(scan.unknown,1);self.assertEqual(per_cpu[2],{25:0,26:0})
    def test_reused_tid_is_a_new_task(self):
        scan,_=self.scan({(3,3,1):("a",25,900)},{(3,3,150):("b",25,10)})
        self.assertEqual(scan.top(1),[{"pid":3,"comm":"b","cores":.1}])
    def test_threads_of_a_process_add_up_and_rank(self):
        scan,_=self.scan({},{(4,4,150):("t",25,10),(4,5,150):("t",26,20),(6,6,150):("u",25,5)})
        self.assertEqual([x["pid"] for x in scan.top(1)],[4,6]);self.assertEqual(scan.top(1)[0]["cores"],.3)
    def test_skipped_pids_are_not_read(self):
        scan=shm.TaskScan({25},[os.getpid()]);self.assertNotIn(os.getpid(),{key[0] for key in scan._read()})

class ForeignMath(unittest.TestCase):
    def test_foreign_is_run_minus_own_per_judged_cpu(self):
        value=evidence([sample(run=(.705,.71)) for _ in range(60)])
        self.assertAlmostEqual(value["busy_cores"],1.415);self.assertAlmostEqual(value["own_cores"],1.4)
        self.assertAlmostEqual(value["foreign_by_cpu"]["26"],.005);self.assertAlmostEqual(value["foreign_by_cpu"]["27"],.01)
        self.assertAlmostEqual(value["foreign_cores"],.015);self.assertAlmostEqual(value["foreign_peak_cores"],.01);self.assertTrue(value["clean"])
    def test_clock_tick_rate_is_honoured(self):
        # 140 ticks on a 200 Hz clock are the same 0.7 core.
        self.assertAlmostEqual(evidence([sample(own=(140,140)) for _ in range(60)],clk=200)["foreign_cores"],0)
    def test_mean_limit_sums_the_judged_cpus(self):
        over=shm.FOREIGN_MEAN_MAX_CORES+.005
        self.assertEqual(evidence([sample(run=(.7,.7+over)) for _ in range(60)])["reasons"],["foreign_mean"])
        self.assertEqual(evidence([sample(run=(.7+over/2,.7+over/2)) for _ in range(60)])["reasons"],["foreign_mean"])
        self.assertTrue(evidence([sample(run=(.7,.7+over-.01)) for _ in range(60)])["clean"])
    def test_peak_is_per_cpu_and_catches_a_burst_the_mean_hides(self):
        peak=shm.FOREIGN_PEAK_MAX_CORES
        value=evidence(with_sample(5,sample(run=(.7,.7+peak+.01))))
        self.assertLess(value["foreign_cores"],shm.FOREIGN_MEAN_MAX_CORES);self.assertEqual(value["reasons"],["foreign_peak"])
        self.assertAlmostEqual(value["foreign_peak_cores"],peak+.01)
        # The same total split over both CPUs stays under the per-CPU peak.
        self.assertTrue(evidence(with_sample(5,sample(run=(.7+peak/2+.005,.7+peak/2+.005))))["clean"])
    def test_short_sample_is_totalled_but_not_rated(self):
        value=evidence([sample() for _ in range(60)]+[sample(ns=200_000_000,run=(.7,1.2),own=(14,14))])
        self.assertTrue(value["clean"]);self.assertAlmostEqual(value["foreign_peak_cores"],0)
        self.assertAlmostEqual(value["foreign_cores"],.5*.2/60.2)
    def test_own_tick_rounding_below_zero_is_clean(self):
        value=evidence([sample(run=(.7,.69)) for _ in range(60)])
        self.assertEqual(value["foreign_cores"],0);self.assertTrue(value["clean"])
    def test_window_accounting_deficit(self):
        # Our processes booked more than the CPUs ran: the minuend is broken.
        value=evidence([sample(run=(.7,.7-shm.DEFICIT_MEAN_CORES-.005)) for _ in range(60)])
        self.assertEqual(value["reasons"],["accounting_deficit"])
    def test_sample_accounting_deficit(self):
        value=evidence(with_sample(7,sample(run=(.7,.7-shm.DEFICIT_SAMPLE_CORES-.01))))
        self.assertEqual(value["reasons"],["accounting_deficit"])
        self.assertTrue(evidence(with_sample(7,sample(run=(.7,.7-shm.DEFICIT_SAMPLE_CORES+.01))))["clean"])
    def test_sampler_gap(self):
        self.assertEqual(evidence(with_sample(3,sample(ns=1_600_000_000,run=(.7,.7),own=(112,112))))["reasons"],["sampler_gap"])
        self.assertTrue(evidence(with_sample(3,sample(ns=1_500_000_000,run=(.7,.7),own=(105,105))))["clean"])
    def test_counter_regression_is_not_clean(self):
        self.assertIn("counter_regression",evidence(with_sample(2,sample(run=(-.01,.7),own=(0,70))))["reasons"])
    def test_scan_bounds_a_lone_task_schedstat_has_not_booked(self):
        # A pinned burner that never yields: schedstat booked none of its time yet.
        value=evidence([sample(scan=(50,0)) for _ in range(60)])
        self.assertAlmostEqual(value["foreign_cores"],.5);self.assertEqual(value["reasons"],["foreign_mean","foreign_peak"])
    def test_tick_aliasing_does_not_read_as_foreign(self):
        # Measured on this lane: ticks saw 1.99 busy cores for 1.39 own with no other load.
        value=evidence(tick_busy_cores=1.99);self.assertAlmostEqual(value["foreign_cores"],0);self.assertTrue(value["clean"])
    def test_window_arithmetic(self):
        cpus=(24,25,26,27)
        times=iter(({c:(0,0) for c in cpus},{24:(100_000_000,0),25:(200_000_000,0),26:(800_000_000,6),27:(600_000_000,7)}))
        stat=iter(({24:0,25:0,26:0,27:0,3:0},{24:5,25:10,26:80,27:60,3:7}))
        ticks=iter((10,20,80,70));clock=iter((1_000,1_000_001_000));scans=iter(({26:0,27:0},{26:5,27:0}))
        with mock.patch.object(shm,"read_cpu_times",lambda:next(times)),mock.patch.object(shm,"read_cpu_busy",lambda:next(stat)),mock.patch.object(g,"proc_ticks",lambda pid:next(ticks)),\
             mock.patch.object(shm.TaskScan,"scan",lambda self:next(scans)),mock.patch.object(shm.os,"sysconf",return_value=100),mock.patch.object(shm,"context_cpus",return_value=[25,24]),\
             mock.patch.object(shm.time,"monotonic_ns",lambda:next(clock)):
            window=shm.ForeignWindow({"subscriber":1,"publisher":2});window.close()
        with mock.patch.object(shm.os,"sched_getaffinity",return_value={0,1}):value=window.evidence(QUIET)
        self.assertEqual(value["samples"],[{"ns":1_000_000_000,"run_ns":{"26":800_000_000,"27":600_000_000},"own_ticks":{"26":70,"27":50},"scan_ticks":{"26":5,"27":0}}])
        self.assertEqual((value["begin_ns"],value["end_ns"]),(1_000,1_000_001_000));self.assertEqual(value["wait_ns"],{"26":6,"27":7})
        self.assertAlmostEqual(value["foreign_by_cpu"]["26"],.1);self.assertAlmostEqual(value["foreign_by_cpu"]["27"],.1);self.assertAlmostEqual(value["foreign_cores"],.2)
        self.assertAlmostEqual(value["tick_busy_cores"],1.4);self.assertAlmostEqual(value["context"]["25"]["run_cores"],.2);self.assertAlmostEqual(value["context"]["24"]["tick_busy_cores"],.05)
        self.assertAlmostEqual(value["host_busy_cores"],.12);self.assertEqual(value["own_cpus"],{"26":"subscriber","27":"publisher"})
        self.assertIs(shm.verify_interference(value),value)
    def test_samples_tile_the_window(self):
        clock=iter(range(0,10_000_000_000,400_000_000))
        with mock.patch.object(shm,"read_cpu_times",return_value={c:(0,0) for c in (24,25,26,27)}),mock.patch.object(shm,"read_cpu_busy",return_value={c:0 for c in (24,25,26,27)}),\
             mock.patch.object(shm.TaskScan,"scan",return_value={26:0,27:0}),mock.patch.object(shm.time,"monotonic_ns",lambda:next(clock)),mock.patch.object(shm,"context_cpus",return_value=[25]):
            window=shm.ForeignWindow({},{26:None,27:None})
            for _ in range(4):window.sample()
            window.close()
        # 0.4 s ticks: a sample is cut at the first mark 0.5 s or more after the last.
        self.assertEqual([x["ns"] for x in window.samples],[800_000_000,800_000_000,400_000_000])
        self.assertEqual(sum(x["ns"] for x in window.samples),window.end["ns"]-window.begin["ns"])
    def test_judged_cpu_needs_its_process(self):
        with self.assertRaisesRegex(g.InvalidTrial,"without its own process"):shm.ForeignWindow({"publisher":1})
    def test_offline_bound_cpu_fails_closed(self):
        with mock.patch.object(shm,"read_cpu_times",return_value={24:(0,0),25:(0,0),26:(0,0)}),mock.patch.object(shm,"context_cpus",return_value=[25]):
            with self.assertRaisesRegex(g.InvalidTrial,"27"):shm.ForeignWindow({},{26:None,27:None})

class EvidenceIntegrity(unittest.TestCase):
    def rejected(self,change,message,value=None):
        value=evidence() if value is None else value;change(value)
        with self.assertRaisesRegex(ValueError,message):shm.verify_interference(value)
    def test_valid_evidence_passes(self):
        self.assertTrue(shm.verify_interference(evidence())["clean"])
        self.assertFalse(shm.verify_interference(disturbed())["clean"])
    def test_forged_verdict_fields_are_rejected(self):
        self.rejected(lambda v:v.update(clean=True,reasons=[]),"does not match its samples",disturbed())
        self.rejected(lambda v:v.update(foreign_peak_cores=.01),"does not match",disturbed())
        self.rejected(lambda v:v.update(foreign_cores=0.001),"does not match")
        self.rejected(lambda v:v["foreign_by_cpu"].update({"27":0.0}),"does not match",disturbed())
    def test_forged_samples_are_rejected(self):
        # Smoothing the burst away, without recomputing, leaves derived fields that disagree.
        self.rejected(lambda v:v["samples"][10]["run_ns"].update({"27":700_000_000}),"does not match its samples",disturbed())
        self.rejected(lambda v:v["samples"][10]["own_ticks"].update({"27":110}),"does not match its samples",disturbed())
        self.rejected(lambda v:v["samples"].pop(10),"tile",disturbed())
        self.rejected(lambda v:v["samples"][10].update(ns=900_000_000),"tile",disturbed())
        self.rejected(lambda v:v["samples"][3].update(extra=1),"samples")
        self.rejected(lambda v:v["samples"][3]["run_ns"].pop("26"),"samples")
        self.rejected(lambda v:v.update(samples=[]),"samples")
        self.rejected(lambda v:v["samples"][3].update(ns=0),"samples")
    def test_limits_cannot_be_widened(self):
        self.rejected(lambda v:v["threshold"].update(mean_cores=9),"registered limits")
        self.rejected(lambda v:v["threshold"].update(max_sample_gap_ns=10**12),"registered limits")
    def test_schema_is_closed(self):
        self.rejected(lambda v:v.update(extra=1),"schema");self.rejected(lambda v:v.pop("quiet"),"schema")
        self.rejected(lambda v:v.update(schema="x"),"schema")
    def test_cpu_set_is_bound(self):
        self.rejected(lambda v:v.update(cpus=[25,26,27]),"CPU set")
        self.rejected(lambda v:v.update(own_cpus={"26":"publisher","27":"subscriber"}),"CPU set")
    def test_sampler_on_bound_cpu_is_rejected(self):
        self.rejected(lambda v:v["sampler"].update(affinity=[0,26]),"bound CPU")

class MicroMath(unittest.TestCase):
    def test_launch_time_is_not_a_deficit(self):
        # Measured: the benchmark's rusage exceeds CPU 27's run time by 0.33-0.55 ms.
        value=micro_evidence(own_ns=-555_000);self.assertTrue(value["clean"]);self.assertEqual(value["foreign_ns"],0)
    def test_shortfall_beyond_the_launch_allowance(self):
        self.assertEqual(micro_evidence(own_ns=-shm.MICRO_LAUNCH_ALLOWANCE_NS-1)["reasons"],["accounting_deficit"])
        self.assertTrue(micro_evidence(own_ns=-shm.MICRO_LAUNCH_ALLOWANCE_NS)["clean"])
    def test_runnable_wait_is_foreign(self):
        limit=shm.MICRO_FOREIGN_MAX_FRACTION*17_600_000
        value=micro_evidence(wait_ns=int(limit)+1);self.assertEqual(value["reasons"],["foreign_fraction"]);self.assertEqual(value["foreign_ns"],int(limit)+1)
        self.assertTrue(micro_evidence(wait_ns=int(limit))["clean"])
    def test_run_beyond_the_benchmark_is_foreign(self):
        value=micro_evidence(own_ns=200_000,wait_ns=10_000)
        self.assertEqual(value["foreign_ns"],210_000);self.assertAlmostEqual(value["foreign_fraction"],210_000/17_600_000);self.assertEqual(value["reasons"],["foreign_fraction"])
    def test_counter_regression(self):
        self.assertIn("counter_regression",micro_evidence(wait_ns=-1)["reasons"])
    def rejected(self,change,message,value=None):
        value=micro_evidence() if value is None else value;change(value)
        with self.assertRaisesRegex(ValueError,message):shm.verify_micro_interference(value)
    def test_verification(self):
        self.assertIs(shm.verify_micro_interference(micro_evidence())["clean"],True)
        self.rejected(lambda v:v.update(clean=True,reasons=[]),"does not match",micro_evidence(wait_ns=2_000_000))
        self.rejected(lambda v:v.update(wait_ns=0),"does not match",micro_evidence(wait_ns=2_000_000))
        self.rejected(lambda v:v["threshold"].update(foreign_fraction=1),"registered limits")
        self.rejected(lambda v:v.update(cpu=26),"CPU");self.rejected(lambda v:v.update(extra=1),"schema")
        self.rejected(lambda v:v.update(end_ns=1),"interval");self.rejected(lambda v:v["sampler"].update(affinity=[27]),"bound CPU")
    def test_observer_window_is_the_benchmark_run(self):
        observer=shm.InterferenceObserver();observer.micro_quiet=QUIET
        times=iter(({24:(0,0),25:(0,0),26:(5,0),27:(1_000,10)},{24:(7,0),25:(8,0),26:(9,0),27:(17_001_000,20)}))
        with mock.patch.object(shm,"read_cpu_times",lambda:next(times)),mock.patch.object(shm,"_children_cpu_us",side_effect=(100,17_500)),\
             mock.patch.object(shm.time,"monotonic_ns",side_effect=(5,17_600_005)),mock.patch.object(shm.os,"sched_getaffinity",return_value={0,1}),mock.patch.object(shm,"context_cpus",return_value=[25,24]):
            observer.micro_begin();observer.micro_end()
        value=observer.micro_pending
        self.assertEqual((value["begin_ns"],value["end_ns"],value["run_ns"],value["wait_ns"],value["bench_cpu_us"]),(5,17_600_005,17_000_000,10,17_400))
        self.assertEqual(value["context_run_ns"],{"25":8,"26":4,"24":7});self.assertEqual(value["quiet"],QUIET)
        self.assertEqual(value["foreign_ns"],10);self.assertTrue(value["clean"]);self.assertIs(shm.verify_micro_interference(value),value)
    def test_observer_refuses_to_time_from_a_bound_cpu(self):
        with mock.patch.object(shm.os,"sched_getaffinity",return_value={0,27}):
            with self.assertRaisesRegex(g.InvalidTrial,"bound CPU"):shm.InterferenceObserver().micro_begin()

class TrialQuality(unittest.TestCase):
    def test_clean_trial_stays_quality_valid(self):
        with tempfile.TemporaryDirectory() as d:
            result=seal_e2e(Path(d)/"t",interference=evidence())
            self.assertIs(result["quality_valid"],True);self.assertNotIn("quality_reason",result)
    def test_disturbed_trial_is_not_quality_valid_and_says_why(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"t";result=seal_e2e(path,interference=disturbed())
            self.assertIs(result["valid"],True);self.assertIs(result["quality_valid"],False);self.assertEqual(result["quality_reason"],"interference")
            self.assertEqual(g._load_trial_result(path/"result.json",0,0,"out")["quality_valid"],False)
    def test_window_must_sit_on_the_cpu_sampling_window(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIs(seal_e2e(Path(d)/"ok",interference=evidence(begin=1+shm.WINDOW_SKEW_NS))["quality_valid"],True)
            with self.assertRaisesRegex(ValueError,"begin is .* from the CPU sampling window"):seal_e2e(Path(d)/"late",interference=evidence(begin=2+shm.WINDOW_SKEW_NS))
            with self.assertRaisesRegex(ValueError,"begin is -1 ns"):seal_e2e(Path(d)/"early",interference=evidence(begin=0))
            short=[sample() for _ in range(59)]+[sample(ns=500_000_000,own=(35,35))]
            with self.assertRaisesRegex(ValueError,"end is -500000000 ns"):seal_e2e(Path(d)/"short",interference=evidence(short))
    def test_observations_schema_is_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"t";seal_e2e(path,interference=evidence())
            self.assertEqual(set(json.loads((path/"observations.json").read_text())),{"base","quality_required"})
    def test_sealed_evidence_tamper_fails(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"t";seal_e2e(path,interference=disturbed())
            observed=json.loads((path/"observations.json").read_text());observed["base"]["interference"]=evidence();g.write_json(path/"observations.json",observed)
            with self.assertRaisesRegex(ValueError,"sealed artifact mismatch"):g._load_sealed_result(path,"out")
    def test_forged_evidence_in_a_coherent_seal_fails(self):
        with tempfile.TemporaryDirectory() as d:
            forged=disturbed();forged.update(clean=True,reasons=[])
            with self.assertRaisesRegex(ValueError,"does not match"):seal_e2e(Path(d)/"t",interference=forged)

class Verdicts(unittest.TestCase):
    def setUp(self):
        patcher=mock.patch.object(shm,"_base_comparisons",side_effect=lambda rows,*a:(list(range(24 if len(rows)==72 else 30)),{},"PASS"))
        patcher.start();self.addCleanup(patcher.stop)
    def test_clean_run_keeps_its_verdict(self):
        self.assertEqual(g.lossless_v2_summary(full_e2e(),full_micro(),736000)["verdict"],"PASS")
        self.assertEqual(g.e2e_only_summary(full_e2e(),736000)["verdict"],"PASS")
    def test_one_disturbed_trial_makes_the_run_inconclusive(self):
        self.assertEqual(g.lossless_v2_summary(full_e2e(40),full_micro(),736000)["verdict"],"INCONCLUSIVE")
        self.assertEqual(g.e2e_only_summary(full_e2e(40),736000)["verdict"],"INCONCLUSIVE")
    def test_fail_from_a_disturbed_lane_is_inconclusive(self):
        with mock.patch.object(shm,"_base_comparisons",return_value=(list(range(24)),{},"FAIL")):
            self.assertEqual(g.comparisons(full_e2e(3),())[2],"INCONCLUSIVE")
            self.assertEqual(g.comparisons(full_e2e(),())[2],"FAIL")
    def test_disturbed_micro_alone_is_inconclusive(self):
        summary=g.lossless_v2_summary(full_e2e(),full_micro(4),736000)
        self.assertEqual((summary["e2e_verdict"],summary["micro_verdict"],summary["verdict"]),("PASS","INCONCLUSIVE","INCONCLUSIVE"))
    def test_fail_on_a_clean_lane_wins_over_an_inconclusive_lane(self):
        # The registered decision: lanes combine FAIL > INCONCLUSIVE > PASS.
        def base(rows,*a):return list(range(24 if len(rows)==72 else 30)),{},"FAIL" if len(rows)==90 else "PASS"
        with mock.patch.object(shm,"_base_comparisons",side_effect=base):
            summary=g.lossless_v2_summary(full_e2e(5),full_micro(),736000)
        self.assertEqual((summary["e2e_verdict"],summary["micro_verdict"],summary["verdict"]),("INCONCLUSIVE","FAIL","FAIL"))
    def test_row_without_evidence_cannot_pass_comparisons(self):
        rows=full_e2e();del rows[5]["interference"]
        self.assertEqual(g.comparisons(rows,())[2],"INCONCLUSIVE")
        rows=full_e2e();rows[5]["interference"]={"clean":"true"}
        self.assertEqual(g.comparisons(rows,())[2],"INCONCLUSIVE")
    def test_summary_names_trials_causes_and_clean_means(self):
        rows=full_e2e(1);rows[0].update(throughput=3.0);micro=full_micro(2)
        report=shm.interference_summary(rows,micro)
        self.assertEqual((report["e2e_trials"],report["e2e_disturbed"],len(report["disturbed"])),(72,1,1))
        self.assertEqual((report["micro_trials"],report["micro_disturbed"]),(90,1));self.assertEqual(report["disturbed_micro"][0]["reasons"],["foreign_fraction"])
        self.assertEqual(report["disturbed"][0]["top_foreign"][0]["comm"],"cc1plus");self.assertEqual(report["disturbed"][0]["reasons"],["foreign_peak"])
        self.assertEqual(report["reason_counts"],{"foreign_peak":1,"foreign_fraction":1})
        self.assertEqual(report["clean_mode_means"][rows[1]["mode"]]["trials"],23);self.assertEqual(report["clean_mode_means"][rows[0]["mode"]]["trials"],24)
        self.assertAlmostEqual(report["clean_mode_means"][rows[0]["mode"]]["throughput"],(23+3)/24)
    def test_summary_reports_quiet_waits(self):
        rows=full_e2e();rows[0]["interference"]["quiet"]={**QUIET,"checks":4,"waited_sec":8.0}
        rows[1]["interference"]["quiet"]={**QUIET,"checks":300,"waited_sec":600.0,"timed_out":True}
        micro=full_micro();micro[0]["interference"]["quiet"]={**QUIET,"checks":2,"waited_sec":4.0}
        report=shm.interference_summary(rows,micro)
        self.assertEqual((report["quiet_waited_trials"],report["quiet_waited_sec"],report["quiet_timeouts"]),(3,612.0,1))
    def summary(self,verdict,e2e,micro=None,schema=None):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);name="raw-diagnostic-conservation.jsonl" if schema else "raw-e2e.jsonl"
            (out/name).write_text("".join(json.dumps(x)+"\n" for x in e2e))
            # A diagnostic summary must not read the micro lane: this file would not parse.
            (out/"raw-micro.jsonl").write_text("not json\n" if schema else "".join(json.dumps(x)+"\n" for x in micro or []))
            g.write_summary(out,{"verdict":verdict,**({"schema":schema} if schema else {})})
            return json.loads((out/"summary.json").read_text()),(out/"summary.md").read_text().splitlines()[0]
    def test_inconclusive_summary_names_interference(self):
        value,_=self.summary("INCONCLUSIVE",full_e2e(2),full_micro())
        self.assertEqual((value["reason"],value["e2e_reason"]),("interference","interference"));self.assertNotIn("micro_reason",value)
        self.assertEqual(value["interference"]["e2e_disturbed"],1)
    def test_fail_summary_does_not_blame_interference(self):
        value,label=self.summary("FAIL",full_e2e(2),full_micro())
        self.assertNotIn("reason",value);self.assertEqual(value["e2e_reason"],"interference");self.assertNotIn("micro_reason",value)
        value,_=self.summary("FAIL",full_e2e(),full_micro(1))
        self.assertNotIn("reason",value);self.assertEqual(value["micro_reason"],"interference")
        value,_=self.summary("PASS",full_e2e(),full_micro())
        self.assertFalse({"reason","e2e_reason","micro_reason"}&set(value))
    def test_diagnostic_summary_reads_diagnostic_rows_only(self):
        value,_=self.summary("DIAGNOSTIC_ONLY",full_e2e(2)[:6],schema=g.DIAGNOSTIC_SCHEMA)
        self.assertEqual((value["interference"]["e2e_trials"],value["interference"]["micro_trials"],value["e2e_reason"]),(6,0,"interference"))
        self.assertNotIn("reason",value)
    def test_label_shows_the_lane_verdicts(self):
        self.assertEqual(g.summary_label({"verdict":"FAIL","e2e_verdict":"INCONCLUSIVE","micro_verdict":"FAIL"}),"# Core Metrics shm lossless v1: FAIL (E2E INCONCLUSIVE, micro FAIL)")
        self.assertEqual(g.summary_label({"verdict":"PASS"}),"# Core Metrics shm lossless v1: PASS")
        self.assertEqual(g.summary_label({"verdict":"INCONCLUSIVE","scope":"e2e-only"}),"# Core Metrics shm lossless v1 E2E: INCONCLUSIVE")

class Resume(unittest.TestCase):
    builds={"out":Path("out"),"instrumented":Path("instrumented")}
    def directory(self,d):
        out=Path(d);(out/"calibration.json").write_text('{"frozen_load": 10}');return out
    def fresh(self,mode="out"):
        return {"mode":mode,"hz":10,"measured_sec":60,"measure_begin_monotonic_ns":1,"measure_end_monotonic_ns":60_000_000_001,"valid":True,"correctness":{"valid":True},"reconciliation":{"valid":True},"publisher_correctness":{"valid":True},"quality_valid":True,"throughput":1.0,"p50_us":1.0,"p99_us":1.0,"cpu_cores":1.0,"pss_median_kib":1.0,"bottleneck_utilization":.75,"interference":evidence()}
    def test_sealed_trial_without_evidence_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            out=self.directory(d);sealed=seal_e2e(out/"trials"/"block-00-0-out")
            self.assertIs(sealed["quality_valid"],True)
            with mock.patch.object(g,"run_trial") as run:rows,_=g._resume_trials(out,self.builds,[("out",)],[],True)
            run.assert_not_called()
            self.assertIs(rows[0]["quality_valid"],False);self.assertEqual(rows[0]["quality_reason"],"interference")
            self.assertEqual(rows[0]["interference"]["reasons"],["missing_evidence"])
            self.assertEqual(g._read_jsonl(out/"raw-e2e.jsonl")[0]["interference"]["reasons"],["missing_evidence"])
            self.assertFalse(shm.trial_is_clean(rows[0]))
            self.assertEqual(shm.interference_summary(rows,[])["disturbed"][0]["reasons"],["missing_evidence"])
    def test_legacy_verdict_cannot_be_decisive(self):
        with tempfile.TemporaryDirectory() as d:
            out=self.directory(d);seal_e2e(out/"trials"/"block-00-0-out")
            rows,_=g._resume_trials(out,self.builds,[("out",)],[],True)
        many=full_e2e();many[0]=rows[0]|{k:many[0][k] for k in ("block","position")}
        with mock.patch.object(shm,"_base_comparisons",return_value=(list(range(24)),{},"PASS")):
            self.assertEqual(g.e2e_only_summary(many,736000)["verdict"],"INCONCLUSIVE")
    def test_disturbed_sealed_trial_stays_and_is_not_rerun(self):
        with tempfile.TemporaryDirectory() as d:
            out=self.directory(d);seal_e2e(out/"trials"/"block-00-0-out",interference=disturbed())
            with mock.patch.object(g,"run_trial") as run:rows,_=g._resume_trials(out,self.builds,[("out",)],[],True)
            run.assert_not_called();self.assertIs(rows[0]["quality_valid"],False);self.assertEqual(rows[0]["interference"]["reasons"],["foreign_peak"])
    def test_clean_sealed_trial_resumes_and_missing_runs_with_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            out=self.directory(d);seal_e2e(out/"trials"/"block-00-0-out",interference=evidence())
            with mock.patch.object(g,"run_trial",return_value=self.fresh("off")) as run:
                rows,_=g._resume_trials(out,self.builds,[("out","off")],[],True)
            self.assertEqual(run.call_count,1);self.assertTrue(all(shm.trial_is_clean(x) for x in rows))
    def test_plan_binds_limits_so_older_evidence_cannot_resume(self):
        plan=g.registered_plan()
        self.assertEqual(plan["interference"],{"schema":shm.INTERFERENCE_SCHEMA,"micro_schema":shm.MICRO_INTERFERENCE_SCHEMA,"own_cpus":{"26":"subscriber","27":"publisher"},"micro_cpu":27,
                                               "threshold":shm.thresholds(),"micro_threshold":shm.micro_thresholds(),"window_skew_ns":shm.WINDOW_SKEW_NS,
                                               "quiet_window_sec":shm.QUIET_WINDOW_SEC,"quiet_max_wait_sec":shm.QUIET_MAX_WAIT_SEC})
        with tempfile.TemporaryDirectory() as d:
            out=Path(d);older=json.loads(json.dumps(plan));older["interference"]["threshold"]["peak_cores"]=1.0;g.write_json(out/"plan.json",older);(out/"plan.sha256").write_text(g.sha(out/"plan.json")+"  plan.json\n")
            with self.assertRaisesRegex(ValueError,"plan semantics mismatch"):g._verify_plan(out)
            g.write_json(out/"plan.json",plan);(out/"plan.sha256").write_text(g.sha(out/"plan.json")+"  plan.json\n")
            self.assertEqual(g._verify_plan(out),g.sha(out/"plan.json"))

class QuietCheck(unittest.TestCase):
    def fake_windows(self,values):
        windows=[]
        def build(pids,own_cpus):
            value=values[len(windows)];window=mock.Mock()
            window.evidence.return_value={"foreign_cores":value,"top_foreign":[{"pid":9,"comm":"make","cores":value}]}
            window.own_cpus=own_cpus;windows.append(window);return window
        return build,windows
    def test_quiet_host_passes_first_check(self):
        build,windows=self.fake_windows([0.0]);log=[]
        with mock.patch.object(shm,"ForeignWindow",build),mock.patch.object(shm.time,"sleep"):
            record=shm.wait_for_quiet(log.append)
        self.assertEqual((record["checks"],record["timed_out"]),(1,False));self.assertEqual(log,[])
        self.assertEqual(windows[0].own_cpus,{26:None,27:None});self.assertEqual(windows[0].sample.call_count,shm.QUIET_WINDOW_SEC);windows[0].close.assert_called_once()
    def test_busy_host_waits_logs_and_records(self):
        build,_=self.fake_windows([.9,.8,0.0]);log=[]
        with mock.patch.object(shm,"ForeignWindow",build),mock.patch.object(shm.time,"sleep"):
            record=shm.wait_for_quiet(log.append)
        self.assertEqual(record["checks"],3);self.assertFalse(record["timed_out"]);self.assertEqual(record["last_foreign_cores"],0.0)
        self.assertIn("make",log[0])
    def test_wait_is_bounded_then_trial_runs_on_record(self):
        build,_=self.fake_windows([.9]*10);log=[]
        with mock.patch.object(shm,"ForeignWindow",build),mock.patch.object(shm.time,"sleep"),mock.patch.object(shm,"QUIET_MAX_WAIT_SEC",0):
            record=shm.wait_for_quiet(log.append)
        self.assertTrue(record["timed_out"]);self.assertEqual(record["checks"],1);self.assertEqual(record["top_foreign"][0]["comm"],"make");self.assertIn("gave up",log[-1])
    def test_limit_is_exclusive(self):
        build,_=self.fake_windows([shm.FOREIGN_MEAN_MAX_CORES,0.0])
        with mock.patch.object(shm,"ForeignWindow",build),mock.patch.object(shm.time,"sleep"):
            self.assertEqual(shm.wait_for_quiet(lambda text:None)["checks"],2)
    def test_micro_waits_for_quiet_before_its_window(self):
        calls=[]
        observer=mock.Mock(spec=shm.InterferenceObserver)
        with mock.patch.object(g,"TRIAL_OBSERVER",observer),mock.patch.object(shm,"wait_for_quiet",lambda:calls.append("quiet") or QUIET),\
             mock.patch.object(shm,"_base_run_micro",lambda *a:calls.append(("run",observer.micro_quiet)) or {"ok":1}),mock.patch.object(shm,"off_bound_cpus",contextlib.nullcontext):
            self.assertEqual(shm.run_micro("out",Path("b"),Path("t")),{"ok":1})
        self.assertEqual(calls,["quiet",("run",QUIET)]);self.assertIsNone(observer.micro_quiet);self.assertIsNone(observer.micro_pending)

@host_only
class NoiseFloor(unittest.TestCase):
    def test_noise_floor_reports_a_distribution_from_off_the_bound_cpus(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"floor.json"
            with contextlib.redirect_stdout(io.StringIO()):self.assertEqual(shm.noise_floor(2,path),0)
            report=json.loads(path.read_text())
        self.assertEqual(report["sample_1s_per_cpu"]["count"],2*report["sample_1s_summed"]["count"]);self.assertGreaterEqual(len(report["samples"]),1)
        self.assertFalse(set(report["sampler"]["affinity"])&set(g.CPUS));self.assertIsNone(report["mean_60s_summed"])
        self.assertEqual(set(report["foreign_by_cpu"]),{"26","27"})

class SamplerPlacement(unittest.TestCase):
    def placed(self,allowed,neighbors=(24,)):
        with mock.patch.object(shm.os,"sched_getaffinity",return_value=set(allowed)),mock.patch.object(shm.os,"sched_setaffinity") as setter,mock.patch.object(shm,"l2_neighbors",return_value=list(neighbors)):
            with shm.off_bound_cpus():pass
        return setter.call_args_list[0].args[1]
    def test_l2_neighbours_are_left_when_another_cpu_remains(self):
        self.assertEqual(self.placed({0,1,24,25,26,27}),{0,1})
        self.assertEqual(self.placed({24,25,26,27}),{24})
    def test_no_cpu_outside_the_bound_set_fails(self):
        with mock.patch.object(shm.os,"sched_getaffinity",return_value={25,26,27}):
            with self.assertRaisesRegex(g.InvalidTrial,"no CPU left"):
                with shm.off_bound_cpus():pass
    def test_observer_refuses_to_sample_from_a_bound_cpu(self):
        observer=shm.InterferenceObserver()
        with mock.patch.object(shm.os,"sched_getaffinity",return_value={0,26}):
            with self.assertRaisesRegex(g.InvalidTrial,"bound CPU"):observer.window_begin({})

@host_only
class HostPlacement(unittest.TestCase):
    def test_trial_thread_leaves_bound_cpus_and_returns(self):
        before=os.sched_getaffinity(0)
        with shm.off_bound_cpus():
            self.assertFalse(os.sched_getaffinity(0)&set(g.CPUS))
            child=subprocess.run([sys.executable,"-I","-c","import os;print(sorted(os.sched_getaffinity(0)))"],capture_output=True,text=True)
            self.assertFalse(set(json.loads(child.stdout))&set(g.CPUS))
        self.assertEqual(os.sched_getaffinity(0),before)
    def test_affinity_returns_after_error(self):
        before=os.sched_getaffinity(0)
        with self.assertRaises(KeyError):
            with shm.off_bound_cpus():raise KeyError("x")
        self.assertEqual(os.sched_getaffinity(0),before)
    def test_topology_still_sees_bound_cpus(self):
        # The gate process keeps them: only the trial thread moves, and only in a trial.
        self.assertTrue(set(g.CPUS)<=os.sched_getaffinity(0));self.assertTrue(g.topology()["allowed"])

class IceoryxLane(unittest.TestCase):
    def test_observer_is_absent(self):
        self.assertIsNone(iox.TRIAL_OBSERVER)
        self.assertIsInstance(g.TRIAL_OBSERVER,shm.InterferenceObserver)
    def test_functions_are_not_rebound(self):
        for name in ("recompute_e2e_result","recompute_micro_result","comparisons","write_summary","run_micro","_load_trial_result","registered_plan","summary_label"):
            self.assertEqual(getattr(iox,name).__module__,iox.__name__,name)
        self.assertNotIn("interference",iox.registered_plan())
    def test_every_hook_is_guarded(self):
        # Reading the observer outside `if TRIAL_OBSERVER:` would reach this lane.
        tree=ast.parse((SCRIPTS/"core_metrics_perf.py").read_text());guarded=set();hooks=[]
        for node in ast.walk(tree):
            if isinstance(node,ast.If) and isinstance(node.test,ast.Name) and node.test.id=="TRIAL_OBSERVER":
                self.assertEqual(node.orelse,[]);self.assertEqual(len(node.body),1)
                guarded.update(id(x) for x in ast.walk(node))
                hooks+=[x.func.attr for x in ast.walk(node) if isinstance(x,ast.Call) and isinstance(x.func,ast.Attribute) and isinstance(x.func.value,ast.Name) and x.func.value.id=="TRIAL_OBSERVER"]
        uses=[x for x in ast.walk(tree) if isinstance(x,ast.Name) and x.id=="TRIAL_OBSERVER" and isinstance(x.ctx,ast.Load)]
        self.assertEqual(sorted(hooks),["before_trial","evidence","micro_begin","micro_end","window_begin","window_end","window_sample"])
        self.assertEqual(len(uses),14);self.assertTrue(all(id(x) in guarded for x in uses))

@host_only
class RealTrials(unittest.TestCase):
    """Whole run_trial and run_micro paths against stand-in apps; only processes started here are killed."""
    def setUp(self):
        # These load the bound CPUs; a gate measuring on them now would be disturbed.
        pids=running_gates()
        if pids:self.skipTest(f"a Core Metrics gate is running (pid {pids}); these trials would load its bound CPUs")
        self.dir=Path(tempfile.mkdtemp(prefix="cm-interference-"));self.addCleanup(shutil.rmtree,self.dir)
        self.build=self.dir/"build";self.build.mkdir()
        for name in (g.E2E_PUBLISHER_APP,g.E2E_SUBSCRIBER_APP,iox.E2E_PUBLISHER_APP,iox.E2E_SUBSCRIBER_APP):executable(self.build/name,FAKE_APP)
        executable(self.build/iox.BROKER_BINARY,FAKE_BROKER);executable(self.build/"core_metrics_reporter_hot_bench",FAKE_BENCH)
        for patcher in (mock.patch.object(g,"STARTUP_GUARD_SEC",2),mock.patch.object(iox,"STARTUP_GUARD_SEC",3),mock.patch.object(shm,"QUIET_MAX_WAIT_SEC",0)):
            patcher.start();self.addCleanup(patcher.stop)
    def burner(self,cpu):
        process=subprocess.Popen(["taskset","-c",str(cpu),sys.executable,"-I","-c","while True:pass"])
        def stop():
            process.kill();process.wait()
        self.addCleanup(stop);time.sleep(.2);return process
    def window(self,pids,own_cpus,seconds=3):
        with shm.off_bound_cpus():
            window=shm.ForeignWindow(pids,own_cpus)
            for _ in range(seconds):
                time.sleep(1);window.sample()
            window.close()
        return window.evidence(QUIET)
    def test_window_detects_a_foreign_burner(self):
        burner=self.burner(26);value=self.window({},{26:None,27:None})
        self.assertGreater(value["foreign_by_cpu"]["26"],.8);self.assertGreater(value["foreign_peak_cores"],.8)
        self.assertEqual(value["own_cores"],0);self.assertFalse(value["clean"]);self.assertIn("foreign_mean",value["reasons"])
        self.assertIn(burner.pid,[x["pid"] for x in value["top_foreign"]]);self.assertIsNone(burner.poll())
    def test_unjudged_cpu_is_context_only(self):
        self.burner(25);value=self.window({},{26:None,27:None})
        self.assertNotIn("25",value["foreign_by_cpu"]);self.assertIn("25",value["context"])
    def test_own_process_is_not_foreign(self):
        burner=self.burner(27);value=self.window({"publisher":burner.pid},{27:"publisher"},4)
        self.assertGreater(value["own_cores"],.8);self.assertNotIn(burner.pid,[x["pid"] for x in value["top_foreign"]])
        # Counted as foreign, the burner alone would read about one core; the
        # bound is loose because other host load may land on CPU 27 meanwhile.
        self.assertLess(value["foreign_cores"],.5)
    def test_disturbed_trial_end_to_end(self):
        burner=self.burner(g.CPUS[1])
        result=g.run_trial("out",self.build,self.dir/"trial",1000,3,False,1,1)
        evidence=result["interference"]
        self.assertTrue(result["valid"]);self.assertFalse(result["quality_valid"]);self.assertEqual(result["quality_reason"],"interference")
        self.assertGreater(evidence["foreign_by_cpu"]["26"],.8);self.assertIn(burner.pid,[x["pid"] for x in evidence["top_foreign"]])
        self.assertTrue(evidence["quiet"]["timed_out"]);self.assertEqual(evidence["own_cpus"],{"26":"subscriber","27":"publisher"})
        self.assertFalse(set(evidence["sampler"]["affinity"])&set(g.CPUS));self.assertGreaterEqual(evidence["sampler"]["scans"],3)
        self.assertEqual(sum(x["ns"] for x in evidence["samples"]),evidence["end_ns"]-evidence["begin_ns"])
        self.assertEqual(set(json.loads((self.dir/"trial"/"observations.json").read_text())),{"base","quality_required"})
        loaded=g._load_trial_result(self.dir/"trial"/"result.json",0,0,"out")
        self.assertEqual(loaded["interference"],evidence);self.assertFalse(shm.trial_is_clean({"interference":{}}))
    def test_iceoryx_trial_is_unchanged(self):
        result=iox.run_trial("out",self.build,self.dir/"iox",1000,1,False,1,1)
        observed=json.loads((self.dir/"iox"/"observations.json").read_text())
        self.assertEqual(set(observed),{"base","quality_required"});self.assertEqual(set(observed["base"]),BASE_KEYS)
        self.assertNotIn("interference",result);self.assertNotIn("quality_reason",result);self.assertTrue(result["quality_valid"])
        self.assertEqual(sorted(x["path"] for x in json.loads((self.dir/"iox"/"seal.json").read_text())["artifacts"]),iox._seal_artifact_names("out"))
    def test_micro_trial_carries_sealed_evidence(self):
        trial=self.dir/"micro"/"block-00-0-out"
        result=g.run_micro("out",self.build,trial)
        value=result["interference"];self.assertEqual(value["cpu"],27);self.assertGreater(value["bench_cpu_us"],0);self.assertGreater(value["run_ns"],0)
        self.assertLess(value["end_ns"]-value["begin_ns"],5_000_000_000);self.assertIn("checks",value["quiet"])
        self.assertEqual(g._load_trial_result(trial/"result.json",0,0,"out")["interference"],value)
        self.assertIsNone(g.TRIAL_OBSERVER.micro_pending);self.assertIsNone(g.TRIAL_OBSERVER.micro_quiet)
    def test_micro_evidence_tamper_fails(self):
        trial=self.dir/"micro"/"block-00-0-out";g.run_micro("out",self.build,trial)
        path=trial/"interference.json";value=json.loads(path.read_text());value["wait_ns"]=0;value["context_run_ns"]={};g.write_json(path,value)
        with self.assertRaisesRegex(ValueError,"recomputation mismatch"):g._load_trial_result(trial/"result.json",0,0,"out")
        path.unlink()
        with self.assertRaisesRegex(ValueError,"recomputation mismatch"):g._load_trial_result(trial/"result.json",0,0,"out")
    def test_micro_disturbed_by_a_burner_is_not_clean(self):
        self.burner(27)
        result=g.run_micro("out",self.build,self.dir/"micro"/"block-00-0-out")
        self.assertFalse(shm.trial_is_clean(result));self.assertIn("foreign_fraction",result["interference"]["reasons"]);self.assertTrue(result["interference"]["quiet"]["timed_out"])
    def test_micro_burner_on_another_cpu_is_not_judged(self):
        self.burner(26)
        result=g.run_micro("out",self.build,self.dir/"micro"/"block-00-0-out")
        self.assertNotIn("foreign_fraction",result["interference"]["reasons"])
    def test_sealed_micro_without_evidence_is_not_accepted(self):
        trial=self.dir/"micro"/"block-00-0-out";trial.mkdir(parents=True);(trial/"metrics").mkdir()
        g.write_json(trial/"bench-output.json",{"iterations":20000000,"elapsed_ns":20000000,"ns_per_op":1.0,"reporter_bound":False})
        g.write_json(trial/"result.json",g.recompute_micro_result(trial,"out"));g._seal_artifacts(trial,"out",True)
        loaded=g._load_trial_result(trial/"result.json",0,0,"out")
        self.assertEqual(loaded["interference"]["reasons"],["missing_evidence"]);self.assertFalse(shm.trial_is_clean(loaded))

if __name__=="__main__":unittest.main()
