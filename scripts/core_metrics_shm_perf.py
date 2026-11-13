#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
"""Core Metrics lossless E2E and Reporter hot-hook gate for the shm transport.

Runs the protocol machinery of core_metrics_perf.py (snapshots, sandboxed build,
preregistered plan, seals, bootstrap verdicts) on the shm_loaned_ctx lane: no
RouDi, libibmw_shm_transport.so, direct_shm_loan counters. The iceoryx module is
loaded as a private instance whose lane identity is rebound below, so importing
this file never changes what core_metrics_perf.py does when it runs itself.

CPU and PSS totals sum two processes here and three on the iceoryx gate, so the
gates' absolute totals are not comparable; verdicts compare modes within a gate.

The host is shared, so every trial also accounts for foreign load on the bound
CPUs (see "Interference accounting" below): a disturbed trial is not
quality-valid and its lane (E2E or micro) is INCONCLUSIVE. Lanes combine as in
core_metrics_perf.py, so a FAIL on clean data in the other lane stays a FAIL.
"""
import argparse, contextlib, importlib.util, json, os, resource, shutil, signal, statistics, sys, tempfile, time
from pathlib import Path

_spec=importlib.util.spec_from_file_location("core_metrics_perf_shm_lane",Path(__file__).resolve().with_name("core_metrics_perf.py"))
gate=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(gate)

PROTOCOL="core-metrics-shm-lossless-v1"
PROTOCOL_LABEL="shm-lossless-v1"
PLAN_SCHEMA="ibmw-core-metrics-shm-lossless-plan-v1"
E2E_SUMMARY_SCHEMA="ibmw-core-metrics-shm-lossless-v1-e2e-summary-v1"
DIAGNOSTIC_SCHEMA="ibmw-core-metrics-shm-lossless-v1-conservation-diagnostic-v1"
ROOT=Path(__file__).resolve().parents[1]
TOPIC="shm_bench_ctx_topic"
PUBLISHER_MODULE="ShmLoanedCtxPublisherModule"
SUBSCRIBER_MODULE="IoxLoanedCtxSubscriberModule"
# The lane registers the shm driver under the name its apps look up, exactly as
# cfg/shm_loaned_ctx does; it is a label, the plugin is libibmw_shm_transport.so.
DRIVER_NAME="iceoryx"
DIRECT_KIND="direct_shm_loan"
# The iceoryx load (448000) leaves this lane's bottleneck near 0.46, outside the
# registered quality band; 736000 holds it at 0.76-0.80 in all three modes with
# the same band, batch and pacing policy (paced scans, 30 s windows).
FROZEN_LOAD=736000
CAPACITY_CANDIDATES=(736000,704000,768000,672000,640000,800000)
SHM_DOMAIN="cmgate"
# struct sockaddr_un limit minus the socket name shm_ext appends.
SHM_SERVICE_DIR_MAX=63
_base_clean_env=gate.clean_env
_base_run_trial=gate.run_trial
_base_excluded_source=gate._excluded_source
_base_registered_plan=gate.registered_plan
_base_recompute_e2e_result=gate.recompute_e2e_result
_base_recompute_micro_result=gate.recompute_micro_result
_base_load_trial_result=gate._load_trial_result
_base_comparisons=gate.comparisons
_base_run_micro=gate.run_micro
_base_write_summary=gate.write_summary
_trial_env={}

# Interference accounting. A CPU is judged only if one of ours runs on it: the
# subscriber's (CPUS[1]) and the publisher's (CPUS[2]) for E2E, the publisher's
# for the micro benchmark. CPUS[0] hosts nothing of the shm lane; it and the CPUs
# sharing an L2 with the bound set are reported as context. Per judged CPU,
# foreign = run time (/proc/schedstat) - our process on it (utime+stime), raised
# to the per-task scan where that is larger. /proc/stat cannot be the minuend:
# its jiffies are 250 Hz tick samples, and this lane's 5750 batches/s is 23 per
# tick, so ticks land on one phase of the pacing cycle; with nothing else running
# the bound CPUs read 0.06 to 2.04 busy cores against 1.39 own. It stays as context
# (tick_busy_cores). schedstat books a task's run time when it leaves the CPU, so
# a lone task that never yields reads late (0 to 2.1 cores per second for one
# pinned burner); the scan reads such a task exactly but misses tasks that come
# and go between scans, hence the larger of the two.
INTERFERENCE_SCHEMA="ibmw-core-metrics-interference-v2"
MICRO_INTERFERENCE_SCHEMA="ibmw-core-metrics-micro-interference-v1"
# Limits, fixed from noise data taken before the acceptance run (selfdev/shm/
# docs/测试文档.md 6.19). Per judged CPU and 1 s sample, own-running quiet trials
# read at most 0.039 core; the smallest burst seen (other tenants' short-lived
# processes) read 0.12 and came with 0.5-1 s of run delay on both CPUs.
# Our booked time never exceeded the CPU's run time by more than 0.013 core
# per sample (USER_HZ rounding) and the window total was never below zero.
FOREIGN_MEAN_MAX_CORES=.02
FOREIGN_PEAK_MAX_CORES=.10
DEFICIT_MEAN_CORES=.02
DEFICIT_SAMPLE_CORES=.05
MAX_SAMPLE_GAP_NS=1_500_000_000
# 90 quiet benchmark runs: at most 0.69% of the run foreign (one run; the
# next 0.06%); with a burner on CPUS[2] at least 203%.
MICRO_FOREIGN_MAX_FRACTION=.01
# The benchmark's rusage also holds the fork and taskset exec it spent before
# moving to CPUS[2] (0.33 to 0.55 ms in 90 quiet runs), so its CPU time exceeds
# what CPUS[2] ran by that much; a larger shortfall means it ran elsewhere.
MICRO_LAUNCH_ALLOWANCE_NS=1_000_000
QUIET_WINDOW_SEC=2
QUIET_MAX_WAIT_SEC=600
# The window is the gate's CPU sampling window (warmup and cooldown excluded).
# A sample shorter than this, in practice only the tail, holds too few ticks to
# rate (one 10 ms jiffy is 0.02 core at .5 s); it still counts toward the totals.
MIN_SAMPLE_NS=500_000_000
# Our window bounds against the gate's own CPU sampling bounds.
WINDOW_SKEW_NS=250_000_000
TOP_FOREIGN_COUNT=5
EVIDENCE_KEYS={"schema","cpus","own_cpus","clk_tck","begin_ns","end_ns","samples","wait_ns","tick_busy_cores","context","host_busy_cores","top_foreign","quiet","sampler","threshold",
               "busy_cores","own_cores","foreign_cores","foreign_by_cpu","foreign_peak_cores","clean","reasons"}
SAMPLE_KEYS={"ns","run_ns","own_ticks","scan_ticks"}
MICRO_EVIDENCE_KEYS={"schema","cpu","begin_ns","end_ns","run_ns","wait_ns","bench_cpu_us","context_run_ns","quiet","sampler","threshold","foreign_ns","foreign_fraction","clean","reasons"}

def e2e_own_cpus():
    return {gate.CPUS[1]:"subscriber",gate.CPUS[2]:"publisher"}
def micro_cpu():
    return gate.CPUS[2]
def context_cpus():
    return [gate.CPUS[0]]+[c for c in l2_neighbors(gate.CPUS) if c!=gate.CPUS[0]]
def thresholds():
    return {"mean_cores":FOREIGN_MEAN_MAX_CORES,"peak_cores":FOREIGN_PEAK_MAX_CORES,"deficit_mean_cores":DEFICIT_MEAN_CORES,
            "deficit_sample_cores":DEFICIT_SAMPLE_CORES,"max_sample_gap_ns":MAX_SAMPLE_GAP_NS,"min_sample_ns":MIN_SAMPLE_NS}
def micro_thresholds():
    return {"foreign_fraction":MICRO_FOREIGN_MAX_FRACTION,"launch_allowance_ns":MICRO_LAUNCH_ALLOWANCE_NS}

def parse_proc_stat(text):
    """Busy jiffies per CPU: user+nice+system+irq+softirq+steal (guest is inside user)."""
    busy={}
    for line in text.splitlines():
        name,*fields=line.split()
        if name.startswith("cpu") and name!="cpu":
            values=[int(x) for x in fields]
            busy[int(name[3:])]=sum(values[i] for i in (0,1,2,5,6,7))
    return busy
def read_cpu_busy():
    return parse_proc_stat(Path("/proc/stat").read_text())
def parse_schedstat(text):
    """Per CPU (run ns, wait ns) of all non-idle tasks, exact at each context switch."""
    lines=text.splitlines()
    if not lines or lines[0]!="version 15":raise gate.InvalidTrial(f"unsupported /proc/schedstat: {lines[:1]}")
    times={}
    for line in lines:
        name,*fields=line.split()
        if name.startswith("cpu"):times[int(name[3:])]=(int(fields[6]),int(fields[7]))
    return times
def read_cpu_times():
    return parse_schedstat(Path("/proc/schedstat").read_text())
def parse_task_stat(text):
    """(comm, processor, ticks, starttime) of a /proc stat line; comm may hold spaces and parens."""
    head,tail=text.rsplit(")",1);fields=tail.split()
    return head.split("(",1)[1],int(fields[36]),int(fields[11])+int(fields[12]),int(fields[19])
def parse_cpu_list(text):
    cpus=set()
    for part in text.strip().split(","):
        first,_,last=part.partition("-");cpus.update(range(int(first),int(last or first)+1))
    return cpus
def l2_neighbors(cpus):
    """CPUs sharing an L2 with a bound CPU: unbound, but they contend for that cache."""
    shared=set()
    for cpu in cpus:
        try:shared|=parse_cpu_list(Path(f"/sys/devices/system/cpu/cpu{cpu}/cache/index2/shared_cpu_list").read_text())
        except (OSError,ValueError):pass
    return sorted(shared-set(cpus))
def _boot_ticks(clk_tck):
    return time.clock_gettime(time.CLOCK_BOOTTIME)*clk_tck
class TaskScan:
    """Attributes task CPU time to the judged CPU each task sits on at scan time.

    A task unseen by the previous scan counts whole only if it started after that
    scan began; an older one missed by it (a racing read) is unknown and skipped.
    Field 39 is where the task is now, so this names culprits and bounds the
    foreign time from below (see ForeignWindow).
    """
    def __init__(self,cpus,skip_pids=(),clk_tck=None):
        self.cpus=set(cpus);self.skip={str(x) for x in skip_pids};self.last=None;self.last_boot=None;self.ticks={}
        self.clk_tck=clk_tck or os.sysconf("SC_CLK_TCK");self.count=0;self.cpu_ns=0;self.max_wall_ns=0;self.unknown=0
    def _read(self):
        seen={}
        for pid in os.listdir("/proc"):
            if not pid.isdigit() or pid in self.skip:continue
            try:tids=os.listdir(f"/proc/{pid}/task")
            except OSError:continue
            for tid in tids:
                try:
                    with open(f"/proc/{pid}/task/{tid}/stat",errors="replace") as stream:comm,cpu,ticks,start=parse_task_stat(stream.read())
                except (OSError,ValueError,IndexError):continue
                seen[(int(pid),int(tid),start)]=(comm,cpu,ticks)
        return seen
    def scan(self):
        """Returns the ticks this scan attributed to each judged CPU."""
        wall=time.monotonic_ns();cpu=time.thread_time_ns();boot=_boot_ticks(self.clk_tck);seen=self._read();per_cpu={c:0 for c in self.cpus}
        if self.last is not None:
            for key,(comm,processor,ticks) in seen.items():
                if processor not in self.cpus:continue
                previous=self.last.get(key)
                if previous is None:
                    # starttime is in ticks since boot; one tick of slack for rounding.
                    if key[2]+1<self.last_boot:self.unknown+=1;continue
                    delta=ticks
                else:delta=ticks-previous[2]
                if delta>0:
                    name=(key[0],comm);self.ticks[name]=self.ticks.get(name,0)+delta;per_cpu[processor]+=delta
        self.last=seen;self.last_boot=boot;self.count+=1;self.cpu_ns+=time.thread_time_ns()-cpu
        self.max_wall_ns=max(self.max_wall_ns,time.monotonic_ns()-wall);return per_cpu
    def top(self,elapsed_sec):
        ranked=sorted(self.ticks.items(),key=lambda x:(-x[1],x[0]))[:TOP_FOREIGN_COUNT]
        return [{"pid":pid,"comm":comm,"cores":ticks/self.clk_tck/elapsed_sec} for (pid,comm),ticks in ranked]
def _own_ticks(pids):
    ticks={name:gate.proc_ticks(pid) for name,pid in pids.items()}
    if any(x is None for x in ticks.values()):raise gate.InvalidTrial("own process ticks unreadable")
    return ticks
class ForeignWindow:
    """Raw per-CPU samples of the judged CPUs, about one a second, tiling the window."""
    def __init__(self,pids=None,own_cpus=None):
        self.pids=dict(pids or {});self.own_cpus=dict(e2e_own_cpus() if own_cpus is None else own_cpus);self.cpus=sorted(self.own_cpus)
        if any(name is not None and name not in self.pids for name in self.own_cpus.values()):raise gate.InvalidTrial("judged CPU without its own process")
        self.clk_tck=os.sysconf("SC_CLK_TCK");self.context=context_cpus();self.samples=[]
        self.begin=self.mark=self._now();self.end=None
        self.scan=TaskScan(self.cpus,self.pids.values(),self.clk_tck);self.scan.scan()
    def _now(self):
        times=read_cpu_times();own=_own_ticks(self.pids);ns=time.monotonic_ns();ticks=read_cpu_busy()
        needed=set(self.cpus)|set(self.context)
        if not needed<=set(times)&set(ticks):raise gate.InvalidTrial(f"bound CPU missing: {sorted(needed-(set(times)&set(ticks)))}")
        return {"ns":ns,"times":times,"own":own,"ticks":ticks}
    def _record(self,first,second,scan):
        own=lambda c:second["own"][self.own_cpus[c]]-first["own"][self.own_cpus[c]] if self.own_cpus[c] else 0
        return {"ns":second["ns"]-first["ns"],"run_ns":{str(c):second["times"][c][0]-first["times"][c][0] for c in self.cpus},
                "own_ticks":{str(c):own(c) for c in self.cpus},"scan_ticks":{str(c):scan[c] for c in self.cpus}}
    def sample(self):
        now=self._now()
        if now["ns"]-self.mark["ns"]>=MIN_SAMPLE_NS:
            self.samples.append(self._record(self.mark,now,self.scan.scan()));self.mark=now
    def close(self):
        self.end=self._now();self.samples.append(self._record(self.mark,self.end,self.scan.scan()))
    def evidence(self,quiet):
        begin,end=self.begin,self.end;seconds=(end["ns"]-begin["ns"])/1e9
        ticks=lambda cpus:sum(end["ticks"][c]-begin["ticks"][c] for c in cpus)/self.clk_tck/seconds
        host=[c for c in end["ticks"] if c in begin["ticks"] and c not in gate.CPUS]
        value={"schema":INTERFERENCE_SCHEMA,"cpus":self.cpus,"own_cpus":{str(c):name for c,name in self.own_cpus.items()},"clk_tck":self.clk_tck,
               "begin_ns":begin["ns"],"end_ns":end["ns"],"samples":self.samples,
               "wait_ns":{str(c):end["times"][c][1]-begin["times"][c][1] for c in self.cpus},"tick_busy_cores":ticks(self.cpus),
               "context":{str(c):{"run_cores":(end["times"][c][0]-begin["times"][c][0])/1e9/seconds,"tick_busy_cores":ticks([c])} for c in self.context},
               "host_busy_cores":ticks(host),"top_foreign":self.scan.top(seconds),"quiet":quiet,
               "sampler":{"scans":self.scan.count,"cpu_sec":self.scan.cpu_ns/1e9,"max_scan_ms":self.scan.max_wall_ns/1e6,"unknown_tasks":self.scan.unknown,"affinity":sorted(os.sched_getaffinity(0))},
               "threshold":thresholds()}
        value.update(derive_interference(value));return value
def _sample_foreign(sample,cpu,clk):
    seconds=sample["ns"]/1e9
    return max(sample["run_ns"][cpu]/1e9/seconds-sample["own_ticks"][cpu]/clk/seconds,sample["scan_ticks"][cpu]/clk/seconds)
def derive_interference(value):
    """Every verdict-bearing field, from the raw samples alone."""
    samples=value["samples"];cpus=[str(c) for c in value["cpus"]];clk=value["clk_tck"];limit=value["threshold"]
    elapsed=sum(x["ns"] for x in samples);seconds=elapsed/1e9;total=lambda key,c:sum(x[key][c] for x in samples)
    busy={c:total("run_ns",c)/1e9/seconds for c in cpus};own={c:total("own_ticks",c)/clk/seconds for c in cpus}
    by_cpu={c:max(busy[c]-own[c],total("scan_ticks",c)/clk/seconds) for c in cpus}
    full=[x for x in samples if x["ns"]>=limit["min_sample_ns"]]
    peak=max((_sample_foreign(x,c,clk) for x in full for c in cpus),default=max(by_cpu.values()))
    reasons=[]
    if sum(by_cpu.values())>limit["mean_cores"]:reasons.append("foreign_mean")
    if peak>limit["peak_cores"]:reasons.append("foreign_peak")
    if sum(busy.values())-sum(own.values())<-limit["deficit_mean_cores"] or any((x["run_ns"][c]/1e9-x["own_ticks"][c]/clk)/(x["ns"]/1e9)<-limit["deficit_sample_cores"] for x in full for c in cpus):
        reasons.append("accounting_deficit")
    if any(x["ns"]>limit["max_sample_gap_ns"] for x in samples) or len(samples)<elapsed/limit["max_sample_gap_ns"]:reasons.append("sampler_gap")
    if any(x[key][c]<0 for x in samples for key in ("run_ns","own_ticks","scan_ticks") for c in cpus):reasons.append("counter_regression")
    return {"busy_cores":sum(busy.values()),"own_cores":sum(own.values()),"foreign_cores":sum(by_cpu.values()),"foreign_by_cpu":by_cpu,
            "foreign_peak_cores":peak,"clean":not reasons,"reasons":reasons}
def verify_interference(value,own_cpus=None):
    """Reject evidence that does not follow from its own samples and the registered limits."""
    if not isinstance(value,dict) or set(value)!=EVIDENCE_KEYS or value["schema"]!=INTERFERENCE_SCHEMA:raise ValueError("invalid interference evidence schema")
    expected=e2e_own_cpus() if own_cpus is None else own_cpus
    if value["cpus"]!=sorted(expected) or value["own_cpus"]!={str(c):name for c,name in expected.items()}:raise ValueError("interference evidence CPU set mismatch")
    if value["threshold"]!=thresholds():raise ValueError("interference threshold differs from registered limits")
    samples=value["samples"];cpus={str(c) for c in value["cpus"]}
    if not isinstance(samples,list) or not samples or any(not isinstance(x,dict) or set(x)!=SAMPLE_KEYS or not x["ns"]>0 or any(set(x[k])!=cpus for k in SAMPLE_KEYS-{"ns"}) for x in samples):
        raise ValueError("invalid interference samples")
    if not value["clk_tck"]>0 or sum(x["ns"] for x in samples)!=value["end_ns"]-value["begin_ns"]:raise ValueError("interference samples do not tile the window")
    if set(value["sampler"]["affinity"])&set(gate.CPUS):raise ValueError("interference sampler ran on a bound CPU")
    derived=derive_interference(value)
    if any(value[key]!=item for key,item in derived.items()):raise ValueError("interference evidence does not match its samples")
    return value
def derive_micro_interference(value):
    # Foreign = what CPUS[2] ran beyond the benchmark's CPU time, plus the time
    # tasks on it sat runnable: a task that took the CPU from the benchmark shows
    # in wait_ns even where the launch time hides its run time.
    elapsed=value["end_ns"]-value["begin_ns"];own=value["run_ns"]-value["bench_cpu_us"]*1000
    foreign=max(own,0)+value["wait_ns"];limit=value["threshold"];reasons=[]
    if foreign>limit["foreign_fraction"]*elapsed:reasons.append("foreign_fraction")
    if own<-limit["launch_allowance_ns"]:reasons.append("accounting_deficit")
    if min(value["run_ns"],value["wait_ns"],value["bench_cpu_us"])<0:reasons.append("counter_regression")
    return {"foreign_ns":foreign,"foreign_fraction":foreign/elapsed,"clean":not reasons,"reasons":reasons}
def verify_micro_interference(value):
    if not isinstance(value,dict) or set(value)!=MICRO_EVIDENCE_KEYS or value["schema"]!=MICRO_INTERFERENCE_SCHEMA:raise ValueError("invalid micro interference evidence schema")
    if value["cpu"]!=micro_cpu():raise ValueError("micro interference CPU mismatch")
    if value["threshold"]!=micro_thresholds():raise ValueError("micro interference threshold differs from registered limits")
    if not value["end_ns"]>value["begin_ns"]:raise ValueError("invalid micro interference interval")
    if set(value["sampler"]["affinity"])&set(gate.CPUS):raise ValueError("interference sampler ran on a bound CPU")
    derived=derive_micro_interference(value)
    if any(value[key]!=item for key,item in derived.items()):raise ValueError("micro interference evidence does not match its counters")
    return value
def wait_for_quiet(log=None):
    """Bounded wait for a quiet window on the judged CPUs; returns the wait record."""
    log=log or (lambda text:print(f"core_metrics_shm_perf: {text}",file=sys.stderr,flush=True))
    begin=time.monotonic();checks=0;own_cpus={c:None for c in e2e_own_cpus()}
    while True:
        window=ForeignWindow({},own_cpus)
        for _ in range(QUIET_WINDOW_SEC):
            time.sleep(1);window.sample()
        window.close();value=window.evidence(None);foreign=value["foreign_cores"];checks+=1;waited=time.monotonic()-begin;top=value["top_foreign"]
        record={"checks":checks,"waited_sec":waited,"timed_out":False,"last_foreign_cores":foreign,"top_foreign":top}
        if foreign<FOREIGN_MEAN_MAX_CORES:return record
        if waited>=QUIET_MAX_WAIT_SEC:
            log(f"quiet wait gave up after {waited:.0f} s: foreign {foreign:.3f} cores, top {top}")
            return {**record,"timed_out":True}
        if checks%15==1:log(f"waiting for quiet CPUs: foreign {foreign:.3f} cores, top {top}")
def _children_cpu_us():
    usage=resource.getrusage(resource.RUSAGE_CHILDREN)
    return round((usage.ru_utime+usage.ru_stime)*1_000_000)
class InterferenceObserver:
    """The lane's TRIAL_OBSERVER: quiet check, then the measured window.

    For the micro benchmark the window is exactly the subprocess.run of the
    benchmark on CPUS[2]: it lasts 10 to 100 ms, far below a jiffy-resolved
    window, so it is judged on schedstat ns and rusage us of that one CPU.
    """
    def __init__(self):self.quiet=None;self.window=None;self.micro_quiet=None;self.micro_begin_state=None;self.micro_pending=None
    def before_trial(self,trial):
        self.quiet=wait_for_quiet();self.window=None
    def _check_affinity(self):
        if set(os.sched_getaffinity(0))&set(gate.CPUS):raise gate.InvalidTrial("interference sampler may run on a bound CPU")
    def window_begin(self,pids):
        self._check_affinity();self.window=ForeignWindow(pids)
    def window_sample(self):self.window.sample()
    def window_end(self):self.window.close()
    def evidence(self):return self.window.evidence(self.quiet)
    def _micro_state(self):
        return {"cpu_us":_children_cpu_us(),"times":read_cpu_times(),"ns":time.monotonic_ns()}
    def micro_begin(self):
        self._check_affinity();self.micro_pending=None;self.micro_begin_state=self._micro_state()
    def micro_end(self):
        end=self._micro_state();begin=self.micro_begin_state;cpu=micro_cpu()
        value={"schema":MICRO_INTERFERENCE_SCHEMA,"cpu":cpu,"begin_ns":begin["ns"],"end_ns":end["ns"],
               "run_ns":end["times"][cpu][0]-begin["times"][cpu][0],"wait_ns":end["times"][cpu][1]-begin["times"][cpu][1],"bench_cpu_us":end["cpu_us"]-begin["cpu_us"],
               "context_run_ns":{str(c):end["times"][c][0]-begin["times"][c][0] for c in sorted(set(gate.CPUS)-{cpu})+[c for c in context_cpus() if c not in gate.CPUS] if c in end["times"]},
               "quiet":self.micro_quiet,"sampler":{"affinity":sorted(os.sched_getaffinity(0))},"threshold":micro_thresholds()}
        value.update(derive_micro_interference(value));self.micro_pending=value;self.micro_begin_state=None
@contextlib.contextmanager
def off_bound_cpus():
    """Keep the calling thread, so the sampler and child launchers, off the bound CPUs and, where possible, their L2 neighbours."""
    allowed=os.sched_getaffinity(0);rest=allowed-set(gate.CPUS)
    if not rest:raise gate.InvalidTrial("no CPU left for the gate outside the bound set")
    rest=(rest-set(l2_neighbors(gate.CPUS))) or rest
    os.sched_setaffinity(0,rest)
    try:yield
    finally:os.sched_setaffinity(0,allowed)
def trial_is_clean(row):
    evidence=row.get("interference")
    return isinstance(evidence,dict) and evidence.get("clean") is True
def recompute_e2e_result(trial):
    # A sealed result must equal this recomputation, so a trial sealed without
    # evidence stays as it was here; _load_trial_result refuses it as clean.
    result=_base_recompute_e2e_result(trial)
    if "interference" in result:
        evidence=verify_interference(result["interference"])
        for edge in ("begin","end"):
            skew=evidence[f"{edge}_ns"]-result[f"cpu_sample_{edge}_ns"]
            if not 0<=skew<=WINDOW_SKEW_NS:raise ValueError(f"interference window {edge} is {skew} ns from the CPU sampling window")
        if not evidence["clean"]:result.update(quality_valid=False,quality_reason="interference")
    return result
def recompute_micro_result(trial,mode):
    # interference.json is bound by the seal through result.json, which embeds it.
    trial=Path(trial);path=trial/"interference.json";observer=gate.TRIAL_OBSERVER
    if isinstance(observer,InterferenceObserver) and observer.micro_pending is not None:
        gate.write_json(path,observer.micro_pending);observer.micro_pending=None
    result=_base_recompute_micro_result(trial,mode)
    if path.is_file():result["interference"]=verify_micro_interference(gate.strict_json_bytes(path.read_bytes(),"interference"))
    return result
def run_micro(mode,build_dir,trial):
    with off_bound_cpus():
        observer=gate.TRIAL_OBSERVER
        observer.micro_quiet=wait_for_quiet()
        try:return _base_run_micro(mode,build_dir,trial)
        finally:observer.micro_quiet=None;observer.micro_pending=None;observer.micro_begin_state=None
def _load_trial_result(path,block,position,mode):
    value=_base_load_trial_result(path,block,position,mode)
    if value is not None and "interference" not in value:
        value["interference"]={"clean":False,"reasons":["missing_evidence"]}
        if "quality_valid" in value:value.update(quality_valid=False,quality_reason="interference")
    return value
def comparisons(rows,key_metrics,pss=False):
    # The one path every E2E and micro verdict takes, fresh or resumed: a lane
    # with a disturbed trial is INCONCLUSIVE. Lanes then combine as before
    # (FAIL over INCONCLUSIVE over PASS), so a FAIL on clean data stays a FAIL.
    pairs,out,verdict=_base_comparisons(rows,key_metrics,pss)
    return pairs,out,verdict if all(trial_is_clean(row) for row in rows) else "INCONCLUSIVE"
def _disturbed_entry(row):
    evidence=row.get("interference") or {"reasons":["missing_evidence"]}
    return {"block":row.get("block"),"position":row.get("position"),"mode":row.get("mode"),"reasons":evidence["reasons"],
            **{key:evidence.get(key) for key in ("foreign_cores","foreign_by_cpu","foreign_peak_cores","foreign_fraction","top_foreign")}}
def interference_summary(rows,micro):
    """Evidence for a verdict: who disturbed what, and means over the clean trials."""
    disturbed=[_disturbed_entry(row) for row in rows if not trial_is_clean(row)]
    disturbed_micro=[_disturbed_entry(row) for row in micro if not trial_is_clean(row)]
    evidence=[row["interference"] for row in rows if "foreign_cores" in row.get("interference",{})]
    micro_evidence=[row["interference"] for row in micro if "foreign_fraction" in row.get("interference",{})]
    waits=[x["quiet"] for x in evidence]+[x["quiet"] for x in micro_evidence]
    clean={}
    for mode in gate.MODES:
        picked=[row for row in rows if row.get("mode")==mode and trial_is_clean(row)]
        if picked:clean[mode]={"trials":len(picked),**{key:statistics.mean(row[key] for row in picked) for key in ("throughput","p50_us","p99_us","cpu_cores","pss_median_kib","bottleneck_utilization")}}
    reasons={}
    for entry in disturbed+disturbed_micro:
        for reason in entry["reasons"]:reasons[reason]=reasons.get(reason,0)+1
    return {"threshold":thresholds(),"micro_threshold":micro_thresholds(),"e2e_trials":len(rows),"e2e_disturbed":len(disturbed),"micro_trials":len(micro),"micro_disturbed":len(disturbed_micro),
            "reason_counts":reasons,"disturbed":disturbed,"disturbed_micro":disturbed_micro,
            "foreign_cores_max":max((x["foreign_cores"] for x in evidence),default=None),"foreign_peak_cores_max":max((x["foreign_peak_cores"] for x in evidence),default=None),
            "micro_foreign_fraction_max":max((x["foreign_fraction"] for x in micro_evidence),default=None),
            "host_busy_cores_mean":statistics.mean(x["host_busy_cores"] for x in evidence) if evidence else None,
            "quiet_waited_trials":sum(x["checks"]>1 for x in waits),"quiet_waited_sec":sum(x["waited_sec"] for x in waits if x["checks"]>1),"quiet_timeouts":sum(x["timed_out"] for x in waits),"clean_mode_means":clean}
def write_summary(out,summary):
    out=Path(out);summary=dict(summary)
    diagnostic=summary.get("schema")==gate.DIAGNOSTIC_SCHEMA
    rows=gate._read_jsonl(out/("raw-diagnostic-conservation.jsonl" if diagnostic else "raw-e2e.jsonl"));micro=[] if diagnostic else gate._read_jsonl(out/"raw-micro.jsonl")
    report=summary["interference"]=interference_summary(rows,micro)
    if report["disturbed"]:summary["e2e_reason"]="interference"
    if report["disturbed_micro"]:summary["micro_reason"]="interference"
    # Only an INCONCLUSIVE overall verdict can be owed to interference.
    if summary.get("verdict")=="INCONCLUSIVE" and (report["disturbed"] or report["disturbed_micro"]):summary.setdefault("reason","interference")
    _base_write_summary(out,summary)
def noise_floor(seconds,path=None):
    """Sample the judged CPUs with nothing of ours on them; prints the distribution."""
    own_cpus={c:None for c in e2e_own_cpus()}
    with off_bound_cpus():
        window=ForeignWindow({},own_cpus);end=time.monotonic()+seconds
        while time.monotonic()<end:
            time.sleep(1);window.sample()
        window.close();value=window.evidence(None)
    clk=value["clk_tck"];full=[x for x in value["samples"] if x["ns"]>=MIN_SAMPLE_NS]
    per_cpu={c:[_sample_foreign(x,c,clk) for x in full] for c in value["samples"][0]["run_ns"]}
    summed=[sum(_sample_foreign(x,c,clk) for c in per_cpu) for x in full]
    def means(values,width):return [statistics.mean(values[i:i+width]) for i in range(0,len(values)-width+1,width)]
    def spread(values):return {"count":len(values),"p50":gate.percentile(values,.5),"p95":gate.percentile(values,.95),"p99":gate.percentile(values,.99),"max":max(values)} if values else None
    report={"seconds":seconds,"mean_cores":value["foreign_cores"],"foreign_by_cpu":value["foreign_by_cpu"],"sample_1s_per_cpu":spread([v for vs in per_cpu.values() for v in vs]),
            "sample_1s_summed":spread(summed),"mean_2s_summed":spread(means(summed,2)),"mean_60s_summed":spread(means(summed,60)),
            "context":value["context"],"host_busy_cores":value["host_busy_cores"],"top_foreign":value["top_foreign"],"sampler":value["sampler"],"samples":value["samples"]}
    text=json.dumps(report,indent=2)
    if path:Path(path).write_text(text+"\n")
    print(json.dumps({k:v for k,v in report.items() if k!="samples"},indent=2))
    return 0
def shm_trial_env(root,uid=None):
    # The runtime names the directory after its euid; the lane sets no group
    # (no yaml group option, clean_env drops IBMW_SHM_GROUP), so g<gid> cannot occur.
    uid=os.geteuid() if uid is None else uid
    service=f"{root}/ibmw_shm.{uid}.{SHM_DOMAIN}"
    if len(service)>SHM_SERVICE_DIR_MAX:raise ValueError(f"shm service directory too long: {service}")
    return {"IBMW_SHM_ROOT":str(root),"IBMW_SHM_DOMAIN":SHM_DOMAIN}
def clean_env(extra):
    # Every lane process sees the trial's private root; the hot bench the kind.
    return _base_clean_env({"IBMW_REPORTER_HOT_KIND":DIRECT_KIND,**_trial_env,**extra})
def remove_trial_root(root):
    try:shutil.rmtree(root)
    except OSError as error:raise gate.InvalidTrial(f"private shm root not removed: {root}: {error}") from error
def run_trial(mode,build_dir,trial,hz,measured,quality=True,warmup=10,cooldown=5,subscriber_build_dir=None):
    with off_bound_cpus():return _run_trial_in_private_root(mode,build_dir,trial,hz,measured,quality,warmup,cooldown,subscriber_build_dir)
def _run_trial_in_private_root(mode,build_dir,trial,hz,measured,quality,warmup,cooldown,subscriber_build_dir):
    root=Path(tempfile.mkdtemp(prefix="ishm",dir="/tmp"))
    try:
        _trial_env.update(shm_trial_env(root))
        result=_base_run_trial(mode,build_dir,trial,hz,measured,quality,warmup,cooldown,subscriber_build_dir)
    except BaseException as error:
        # A leftover root is reported without replacing the trial's own error.
        _trial_env.clear()
        try:remove_trial_root(root)
        except gate.InvalidTrial as cleanup:
            print(f"core_metrics_shm_perf: {cleanup}",file=sys.stderr)
            if hasattr(error,"add_note"):error.add_note(str(cleanup))  # Python >= 3.11
        raise
    _trial_env.clear();remove_trial_root(root)
    return result
def yaml(path,publisher,hz,warmup,measured,cooldown,csv_path="",status_path="",
         publisher_status_path="",measure_begin_ns=0,measure_end_ns=0):
    profile=gate.LOSSLESS_PROFILE;route="publishers" if publisher else "subscribers";rid="pub" if publisher else "sub"
    module=PUBLISHER_MODULE if publisher else SUBSCRIBER_MODULE
    tail=(f"    publisher_status_path: {publisher_status_path}\n" if publisher else
          f"    topic: {TOPIC}\n    csv_path: {csv_path}\n    status_path: {status_path}\n")
    path.write_text(f'''ibmw:
  extensions:
    items:
      - name: shm_extension
        path: ./libibmw_shm_transport.so
        options: {{driver_name: {DRIVER_NAME}, shm_init_size: 4096, runtime_id: "metrics_shm_{rid}"}}
  logging: {{core_level: WARN, backends: [{{type: console}}]}}
  scheduling:
    executors: [{{name: work_thread_pool, type: thread_pool, options: {{thread_num: 2}}}}]
  messaging:
    drivers: [{{type: {DRIVER_NAME}, options: {{listener_thread_name: "perf_{rid}", benchmark_profile: "{profile['name']}", publisher_too_slow_policy: "{profile['publisher_too_slow_policy']}", subscriber_queue_full_policy: "{profile['subscriber_queue_full_policy']}", subscriber_queue_capacity: {profile['queue_capacity']}}}}}]
    {route}: [{{topic: "{TOPIC}", enable_drivers: [{DRIVER_NAME}]}}]
  modules:
    modules: [{{name: {module}, log_level: WARN}}]
{module}:
  bench_options:
    payload_bytes: 4096
    hz: {hz}
    batch_size: {gate.BATCH_SIZE}
    duration_sec: {warmup+measured+cooldown}
    warmup_sec: {warmup}
    measured_sec: {measured}
    measure_begin_monotonic_ns: {measure_begin_ns}
    measure_end_monotonic_ns: {measure_end_ns}
    warmup_messages: 0
    label: core_metrics_shm_lossless_v1_4KiB
{tail}''')
    return gate.sha(path)
def _excluded_source(path):
    # selfdev is a snapshot root here; agent state dirs must not enter it.
    return _base_excluded_source(path) or ".omc" in Path(path).parts
def registered_plan(contract_sha=None):
    plan=_base_registered_plan(contract_sha)
    plan["capacity"]["candidate_source"]=f"fixed before {PROTOCOL_LABEL} from paced load scans of the shm_loaned_ctx lane"
    # Registered with the plan: evidence from before these limits cannot resume.
    plan["interference"]={"schema":INTERFERENCE_SCHEMA,"micro_schema":MICRO_INTERFERENCE_SCHEMA,"own_cpus":{str(c):name for c,name in e2e_own_cpus().items()},"micro_cpu":micro_cpu(),
                          "threshold":thresholds(),"micro_threshold":micro_thresholds(),"window_skew_ns":WINDOW_SKEW_NS,"quiet_window_sec":QUIET_WINDOW_SEC,"quiet_max_wait_sec":QUIET_MAX_WAIT_SEC}
    return plan
def summary_label(summary):
    if summary.get("scope")=="e2e-only":return "# Core Metrics shm lossless v1 E2E: "+summary["verdict"]
    lanes=[f"{name} {summary[key]}" for name,key in (("E2E","e2e_verdict"),("micro","micro_verdict")) if key in summary]
    return "# Core Metrics shm lossless v1: "+summary["verdict"]+(f" ({', '.join(lanes)})" if lanes else "")

for _name,_value in {
    "PROTOCOL":PROTOCOL,
    "PROTOCOL_LABEL":PROTOCOL_LABEL,
    "PLAN_SCHEMA":PLAN_SCHEMA,
    "E2E_SUMMARY_SCHEMA":E2E_SUMMARY_SCHEMA,
    "DIAGNOSTIC_SCHEMA":DIAGNOSTIC_SCHEMA,
    "PROTOCOL_CONTRACT":gate.CONTRACT_DIRECTORY/"core-metrics-shm-lossless-v1.json",
    "DEPENDENCY_CONTRACT":gate.CONTRACT_DIRECTORY/"core-metrics-shm-dependencies-v1.json",
    "PROTOCOL_CONTRACT_ARCHIVED":"scripts/contracts/core-metrics-shm-lossless-v1.json",
    "DEPENDENCY_CONTRACT_ARCHIVED":"scripts/contracts/core-metrics-shm-dependencies-v1.json",
    "REGISTERED_CMAKE_OPTIONS":{**gate.REGISTERED_CMAKE_OPTIONS,"IBMW_BUILD_ICEORYX_EXTENSION":"OFF","IBMW_BUILD_SHM_EXTENSION":"ON"},
    "REGISTERED_BUILD_TARGETS":("shm_loaned_ctx_publisher_app","shm_loaned_ctx_lossless_subscriber_app","core_metrics_reporter_hot_bench","ibmw_extensions_shm_ext"),
    "ARTIFACTS":("shm_loaned_ctx_publisher_app","shm_loaned_ctx_lossless_subscriber_app","core_metrics_reporter_hot_bench","libibmw_shm_transport.so"),
    "DEPENDENCIES":{k:v for k,v in gate.DEPENDENCIES.items() if k not in ("iceoryx","cpptoml")},
    "FROZEN_LOAD":FROZEN_LOAD,
    "CAPACITY_CANDIDATES":CAPACITY_CANDIDATES,
    "SOURCE_ROOTS":gate.SOURCE_ROOTS+("selfdev",),
    "BROKER_BINARY":None,
    "E2E_PUBLISHER_APP":"shm_loaned_ctx_publisher_app",
    "E2E_SUBSCRIBER_APP":"shm_loaned_ctx_lossless_subscriber_app",
    "DIRECT_COUNTER_KIND":DIRECT_KIND,
    "COUNTER_MODULE":PUBLISHER_MODULE,
    "COUNTER_TOPIC":TOPIC,
    "COUNTER_DRIVER":DRIVER_NAME,
    "clean_env":clean_env,"run_trial":run_trial,"yaml":yaml,"_excluded_source":_excluded_source,
    "registered_plan":registered_plan,"summary_label":summary_label,
    "TRIAL_OBSERVER":InterferenceObserver(),"recompute_e2e_result":recompute_e2e_result,
    "recompute_micro_result":recompute_micro_result,"run_micro":run_micro,
    "_load_trial_result":_load_trial_result,"comparisons":comparisons,"write_summary":write_summary,
}.items():setattr(gate,_name,_value)

def check_output_outside_sources(argv,root=ROOT):
    """Reject --output inside any snapshotted root, selfdev included."""
    ap=argparse.ArgumentParser(add_help=False);ap.add_argument("--output",type=Path)
    output=ap.parse_known_args(argv)[0].output
    if output is None:return
    out=output.resolve();root=Path(root).resolve()
    if out==root or any(out==root/name or root/name in out.parents for name in gate.SOURCE_ROOTS):
        raise SystemExit(f"evidence output must be outside snapshotted source inputs ({', '.join(gate.SOURCE_ROOTS)}): {out}")
def _terminate_as_interrupt(signum,frame):
    # Unwinds through run_trial's finally blocks: children are stopped and the
    # private shm root is removed before the process exits.
    raise KeyboardInterrupt(f"signal {signum}")
def main(argv=None):
    args=sys.argv[1:] if argv is None else argv
    ap=argparse.ArgumentParser(add_help=False);ap.add_argument("--noise-floor",type=int);ap.add_argument("--noise-output",type=Path)
    probe=ap.parse_known_args(args)[0]
    if probe.noise_floor:return noise_floor(probe.noise_floor,probe.noise_output)
    check_output_outside_sources(args)
    signal.signal(signal.SIGTERM,_terminate_as_interrupt)
    return gate.main()

if __name__=="__main__":sys.exit(main())
