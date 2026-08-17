#!/usr/bin/env python3
"""Pre-registered Core Metrics round-7 E2E and Reporter hot-hook gate."""
import argparse, csv, hashlib, io, itertools, json, math, os, random, shutil, signal
import stat, statistics, subprocess, sys, tarfile, time
from pathlib import Path

SEED = 1229081943
CPUS = (25, 26, 27)
MODES = ("out", "off", "counters")
BOOTSTRAPS = 100000
STARTUP_GUARD_SEC = 15
THRESHOLDS = {
 "off_out":{"cpu":.02,"pss_pct":.02,"pss_abs_kib":2048,"throughput":.02,"p50":.03,"p99":.05,"micro":.02},
 "counters_off":{"cpu":.05,"pss_pct":.05,"pss_abs_kib":4096,"throughput":.05,"p50":.05,"p99":.10,"micro":.05},
 "counters_out":{"cpu":.07,"throughput":.07,"p99":.15,"micro":.07}}
SPECS = (("off_out","off","out"),("counters_off","counters","off"),
         ("counters_out","counters","out"))

BATCH_SIZE = 128
CAPACITY_CANDIDATES = (448000,435200,460800,422400,409600,473600)
REGISTERED_CMAKE_OPTIONS = {
 "CMAKE_BUILD_TYPE":"Release","IBMW_BUILD_TESTS":"OFF","IBMW_BUILD_EXAMPLES":"ON",
 "IBMW_BUILD_BENCHMARKS":"ON","IBMW_BUILD_ICEORYX_EXTENSION":"ON",
 "IBMW_BUILD_DDS_EXTENSION":"OFF","IBMW_BUILD_WITH_ROS2":"OFF","IBMW_BUILD_ROS2_TYPES":"ON"}
REGISTERED_BUILD_TARGETS = ("iox_loaned_ctx_publisher_app","iox_loaned_ctx_subscriber_app",
                            "core_metrics_reporter_hot_bench","iox-roudi","ibmw_extensions_iceoryx_ext")
VERDICT_EXIT_CODES = {"PASS":0,"FAIL":1,"INCONCLUSIVE":2}

class InvalidTrial(RuntimeError): pass

def percentile(xs, q):
    s=sorted(xs); return s[min(len(s)-1,max(0,math.ceil(q*len(s))-1))]
def proc_ticks(pid):
    try:
        fields=Path(f"/proc/{pid}/stat").read_text().split()
        return int(fields[13])+int(fields[14])
    except (OSError,ValueError,IndexError): return None
def pss_kib(pid):
    try:
        lines=Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
        values=[int(x.split()[1]) for x in lines if x.startswith("Pss:")]
        return sum(values) if values else None
    except (OSError,ValueError,IndexError): return None
def sha(path):
    h=hashlib.sha256(); h.update(Path(path).read_bytes()); return h.hexdigest()
def write_json(path, value): path.write_text(json.dumps(value,indent=2,sort_keys=True)+"\n")
def verdict_exit_code(verdict):
    try:return VERDICT_EXIT_CODES[verdict]
    except KeyError:raise ValueError(f"unknown verdict: {verdict}")

SOURCE_ROOTS=("CMakeLists.txt","VERSION","cmake","src","scripts")
SOURCE_MANIFEST="source-manifest.json"
SOURCE_ARCHIVE="source-snapshot.tar"
SOURCE_DIRECTORY="source-snapshot"
ARTIFACTS=("iox_loaned_ctx_publisher_app","iox_loaned_ctx_subscriber_app",
           "core_metrics_reporter_hot_bench","iox-roudi","libibmw_iceoryx_transport.so")

def _safe_relative(path):
    p=Path(path)
    return not p.is_absolute() and bool(p.parts) and all(x not in ("",".","..") for x in p.parts)
def _safe_symlink(path,target):
    if os.path.isabs(target):return False
    normalized=Path(os.path.normpath((Path(path).parent/target).as_posix()))
    return _safe_relative(normalized) and normalized.parts[0] in SOURCE_ROOTS and not _excluded_source(normalized)
def _excluded_source(path):
    parts=Path(path).parts
    return any(x in ("__pycache__",".opencode","build","evidence") for x in parts) or Path(path).suffix==".pyc"
def _symlink_bytes(path): return os.fsencode(os.readlink(path))
def _entry(root,relative):
    path=root/relative
    before=path.lstat();mode=before.st_mode
    if stat.S_ISLNK(mode):
        data=_symlink_bytes(path);target=os.fsdecode(data)
        if not _safe_symlink(relative,target):
            raise ValueError(f"unsafe symlink: {relative} -> {target}")
        kind="symlink";normalized="120000"
    elif stat.S_ISREG(mode):
        data=path.read_bytes();kind="file";normalized="100755" if mode&0o111 else "100644"
    else: raise ValueError(f"unsupported source entry: {relative}")
    after=path.lstat()
    if (before.st_mode,before.st_size,before.st_mtime_ns)!=(after.st_mode,after.st_size,after.st_mtime_ns):
        raise RuntimeError(f"source changed while snapshotting: {relative}")
    return {"path":relative.as_posix(),"kind":kind,"mode":normalized,
            "size":len(data),"sha256":hashlib.sha256(data).hexdigest()},data
def collect_source_state(root):
    root=Path(root);found=[]
    def visit(relative):
        if _excluded_source(relative):return
        path=root/relative
        mode=path.lstat().st_mode
        if stat.S_ISDIR(mode):
            for child in sorted(path.iterdir(),key=lambda x:os.fsencode(x.name)):
                visit(relative/child.name)
        else: found.append(_entry(root,relative))
    for name in SOURCE_ROOTS:
        path=root/name
        if os.path.lexists(path):visit(Path(name))
        elif name=="CMakeLists.txt":raise FileNotFoundError(path)
    found.sort(key=lambda x:x[0]["path"]);entries=[x[0] for x in found]
    state=hashlib.sha256(json.dumps(entries,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    return entries,{x[0]["path"]:x[1] for x in found},state
def _write_source_archive(path,entries,data):
    with tarfile.open(path,"w",format=tarfile.GNU_FORMAT) as archive:
        for entry in entries:
            info=tarfile.TarInfo(entry["path"]);info.uid=info.gid=0;info.uname=info.gname="";info.mtime=0
            info.mode=int(entry["mode"][-3:],8)
            payload=data[entry["path"]]
            if entry["kind"]=="symlink":
                info.type=tarfile.SYMTYPE;info.linkname=os.fsdecode(payload);info.size=0;archive.addfile(info)
            else:
                info.type=tarfile.REGTYPE;info.size=len(payload);archive.addfile(info,io.BytesIO(payload))
def _validate_manifest(manifest):
    if manifest.get("schema")!="ibmw-source-snapshot-v1":raise ValueError("unsupported source manifest")
    entries=manifest.get("entries")
    if not isinstance(entries,list) or not all(isinstance(x,dict) for x in entries):raise ValueError("invalid source manifest entries")
    if not all(isinstance(x.get("path"),str) for x in entries):raise ValueError("invalid source manifest path")
    if [x.get("path") for x in entries]!=sorted(x.get("path") for x in entries):
        raise ValueError("source manifest entries are not sorted")
    if len({x.get("path") for x in entries})!=len(entries):raise ValueError("duplicate source manifest path")
    for entry in entries:
        if not _safe_relative(entry.get("path","")) or _excluded_source(entry["path"]):raise ValueError("unsafe source manifest path")
        if entry.get("kind") not in ("file","symlink") or entry.get("mode") not in ("100644","100755","120000"):
            raise ValueError(f"invalid source manifest entry: {entry.get('path')}")
        if not isinstance(entry.get("size"),int) or entry["size"]<0 or not isinstance(entry.get("sha256"),str) or len(entry["sha256"])!=64:
            raise ValueError(f"invalid source manifest metadata: {entry.get('path')}")
        if entry["kind"]=="symlink" and entry["mode"]!="120000":raise ValueError("invalid symlink mode")
        if entry["kind"]=="file" and entry["mode"]=="120000":raise ValueError("invalid file mode")
    state=hashlib.sha256(json.dumps(entries,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    if manifest.get("source_state_sha256")!=state:raise ValueError("source state digest mismatch")
    return entries
def _archive_payloads(path,entries):
    expected={x["path"]:x for x in entries};payloads={}
    with tarfile.open(path,"r:") as archive:
        members=archive.getmembers()
        if [x.name for x in members]!=[x["path"] for x in entries]:raise ValueError("archive member list mismatch")
        for member in members:
            entry=expected[member.name]
            if member.uid or member.gid or member.mtime or member.uname or member.gname:
                raise ValueError(f"non-deterministic archive metadata: {member.name}")
            if entry["kind"]=="symlink":
                if not member.issym():raise ValueError(f"archive kind mismatch: {member.name}")
                data=os.fsencode(member.linkname)
                if not _safe_symlink(member.name,member.linkname):
                    raise ValueError(f"unsafe archive symlink: {member.name}")
            else:
                if not member.isreg():raise ValueError(f"unsafe archive member: {member.name}")
                stream=archive.extractfile(member)
                if stream is None:raise ValueError(f"unreadable archive member: {member.name}")
                data=stream.read()
            mode="120000" if member.issym() else ("100755" if member.mode&0o111 else "100644")
            if mode!=entry["mode"] or len(data)!=entry["size"] or hashlib.sha256(data).hexdigest()!=entry["sha256"]:
                raise ValueError(f"archive content mismatch: {member.name}")
            payloads[member.name]=data
    return payloads
def _verify_extracted(directory,entries):
    actual=[]
    def visit(path):
        mode=path.lstat().st_mode
        if stat.S_ISDIR(mode):
            for child in sorted(path.iterdir(),key=lambda x:os.fsencode(x.name)):visit(child)
        else:actual.append(path.relative_to(directory).as_posix())
    visit(directory)
    actual.sort()
    if actual!=[x["path"] for x in entries]:raise ValueError("extracted source member list mismatch")
    for expected in entries:
        got,_=_entry(directory,Path(expected["path"]))
        if got!=expected:raise ValueError(f"extracted source mismatch: {expected['path']}")
def verify_source_snapshot(out,expected_manifest_sha=None,expected_snapshot_sha=None):
    out=Path(out);manifest_path=out/SOURCE_MANIFEST;archive_path=out/SOURCE_ARCHIVE;directory=out/SOURCE_DIRECTORY
    if not manifest_path.is_file() or not archive_path.is_file() or not directory.is_dir():raise FileNotFoundError("incomplete source snapshot evidence")
    manifest_sha=sha(manifest_path);snapshot_sha=sha(archive_path)
    if expected_manifest_sha and manifest_sha!=expected_manifest_sha:raise ValueError("source manifest raw digest mismatch")
    if expected_snapshot_sha and snapshot_sha!=expected_snapshot_sha:raise ValueError("source snapshot digest mismatch")
    manifest=json.loads(manifest_path.read_text());entries=_validate_manifest(manifest);_archive_payloads(archive_path,entries);_verify_extracted(directory,entries)
    return {"manifest_sha256":manifest_sha,"source_state_sha256":manifest["source_state_sha256"],"snapshot_sha256":snapshot_sha}
def create_source_snapshot(root,out):
    out=Path(out);entries,data,state=collect_source_state(root)
    manifest={"schema":"ibmw-source-snapshot-v1","source_state_sha256":state,"entries":entries}
    write_json(out/SOURCE_MANIFEST,manifest);_write_source_archive(out/SOURCE_ARCHIVE,entries,data)
    directory=out/SOURCE_DIRECTORY;directory.mkdir()
    payloads=_archive_payloads(out/SOURCE_ARCHIVE,entries)
    for entry in entries:
        path=directory/entry["path"];path.parent.mkdir(parents=True,exist_ok=True)
        if entry["kind"]=="symlink":os.symlink(os.fsdecode(payloads[entry["path"]]),path)
        else:path.write_bytes(payloads[entry["path"]]);path.chmod(int(entry["mode"][-3:],8))
    return verify_source_snapshot(out)
def counter_conservation(path):
    values={}
    for line in path.read_text().splitlines():
        try: item=json.loads(line)
        except json.JSONDecodeError: continue
        if item.get("kind")=="direct_iceoryx_loan" and item.get("metric"):
            values[item["metric"]]=values.get(item["metric"],0)+int(item.get("value",0))
    a=values.get("logical_attempt",0); s=values.get("logical_success",0)
    valid=a>0 and a==s+values.get("logical_fail",0) and values.get("publish",0)+values.get("release",0)==s
    return {"values":values,"valid":valid}
def clean_env(extra):
    keep=("HOME","USER","LOGNAME","PATH","LD_LIBRARY_PATH","LANG","LC_ALL","TZ")
    env={k:v for k,v in os.environ.items() if k in keep}; env.update(extra); return env
def terminate(ps):
    for p in ps:
        if p.poll() is None: p.send_signal(signal.SIGINT)
    end=time.monotonic()+5
    for p in ps:
        try: p.wait(max(0,end-time.monotonic()))
        except subprocess.TimeoutExpired: p.kill(); p.wait()
def wait_ready(path,marker,proc,timeout=20):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if proc.poll() is not None:return False
        try:
            if marker in path.read_text(errors="replace"):return True
        except OSError:pass
        time.sleep(.05)
    return False
def yaml(path,publisher,hz,warmup,measured,cooldown,csv_path="",status_path="",
         publisher_status_path="",measure_begin_ns=0,measure_end_ns=0):
    role="Publisher" if publisher else "Subscriber"; route="publishers" if publisher else "subscribers"; rid="pub" if publisher else "sub"
    tail=(f"    publisher_status_path: {publisher_status_path}\n" if publisher else
          f"    csv_path: {csv_path}\n    status_path: {status_path}\n")
    path.write_text(f'''ibmw:
  extensions:
    items:
      - name: iceoryx_extension
        path: ./libibmw_iceoryx_transport.so
        options: {{shm_init_size: 4096, runtime_id: "metrics_r7_{rid}"}}
  logging: {{core_level: WARN, backends: [{{type: console}}]}}
  scheduling:
    executors: [{{name: work_thread_pool, type: thread_pool, options: {{thread_num: 2}}}}]
  messaging:
    drivers: [{{type: iceoryx, options: {{listener_thread_name: "perf_{rid}"}}}}]
    {route}: [{{topic: "iox_bench_ctx_topic", enable_drivers: [iceoryx]}}]
  modules:
    modules: [{{name: IoxLoanedCtx{role}Module, log_level: WARN}}]
IoxLoanedCtx{role}Module:
  bench_options:
    payload_bytes: 4096
    hz: {hz}
    batch_size: {BATCH_SIZE}
    duration_sec: {warmup+measured+cooldown}
    warmup_sec: {warmup}
    measured_sec: {measured}
    measure_begin_monotonic_ns: {measure_begin_ns}
    measure_end_monotonic_ns: {measure_end_ns}
    warmup_messages: 0
    label: core_metrics_round7_4KiB
{tail}''')

def reconcile_status(publisher,subscriber,begin_ns,end_ns):
    reasons=[]
    for name,value in (("publisher",publisher),("subscriber",subscriber)):
        if value.get("measure_begin_monotonic_ns")!=begin_ns or value.get("measure_end_monotonic_ns")!=end_ns:
            reasons.append(f"{name} measurement boundaries differ")
        if not value.get("valid",False): reasons.append(f"{name} status invalid")
    if publisher.get("fail")!=0: reasons.append("measured publish failure")
    if publisher.get("attempts")!=publisher.get("success"): reasons.append("publisher attempts/success differ")
    if subscriber.get("unique_measured")!=publisher.get("success"): reasons.append("publisher/subscriber count differs")
    if subscriber.get("first_seq")!=publisher.get("first_success_seq"): reasons.append("first sequence differs")
    if subscriber.get("last_seq")!=publisher.get("last_success_seq"): reasons.append("last sequence differs")
    for key in ("duplicate","out_of_order","missing","span_missing","invalid"):
        if subscriber.get(key)!=0: reasons.append(f"subscriber {key} nonzero")
    return {"valid":not reasons,"reasons":reasons}
def registered_build_configuration(out,runner_python=None):
    out=Path(out);python=str(Path(runner_python or sys.executable).resolve());source=str((out/SOURCE_DIRECTORY).resolve())
    builds={}
    for name in ("out","instrumented"):
        options=dict(REGISTERED_CMAKE_OPTIONS)
        options.update({"Python3_EXECUTABLE":python,"PYTHON_EXECUTABLE":python,
                        "IBMW_ENABLE_CORE_METRICS":"OFF" if name=="out" else "ON",
                        "IBMW_ENABLE_REPORTER_V1":"OFF" if name=="out" else "ON"})
        builds[name]={"directory":f"build-{name}","configure_options":options}
    return {"schema":"ibmw-core-metrics-build-registration-v1","runner_python":python,
            "cmake_source":source,"build_targets":list(REGISTERED_BUILD_TARGETS),"builds":builds}
def _parse_cmake_cache(path):
    values={}
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith(("#","//")) or "=" not in line or ":" not in line.split("=",1)[0]:continue
        typed,value=line.split("=",1);key,_=typed.split(":",1);values[key]=value
    return values
def verify_registered_builds(out,registration):
    out=Path(out)
    if registration!=registered_build_configuration(out):raise ValueError("build registration mismatch")
    expected_source=Path(registration["cmake_source"]).resolve()
    for name,build_registration in registration["builds"].items():
        cache_path=out/build_registration["directory"]/"CMakeCache.txt"
        if not cache_path.is_file():raise FileNotFoundError(f"missing CMake cache: {cache_path}")
        cache=_parse_cmake_cache(cache_path)
        home=cache.get("CMAKE_HOME_DIRECTORY")
        if home is None or Path(home).resolve()!=expected_source:
            raise ValueError(f"{name} CMAKE_HOME_DIRECTORY mismatch")
        for key,value in build_registration["configure_options"].items():
            actual=cache.get(key)
            if key in ("Python3_EXECUTABLE","PYTHON_EXECUTABLE"):
                matches=actual is not None and Path(actual).resolve()==Path(value).resolve()
            else:matches=actual==value
            if not matches:raise ValueError(f"{name} required CMake option mismatch: {key}")
    return registration
def build(source,out,jobs):
    registration=registered_build_configuration(out);source=Path(source).resolve()
    if source!=Path(registration["cmake_source"]):raise ValueError("build source differs from registered source")
    dirs={"out":out/"build-out","instrumented":out/"build-instrumented"}
    for name,d in dirs.items():
        options=registration["builds"][name]["configure_options"]
        subprocess.run(["cmake","-S",str(source),"-B",str(d)]+[f"-D{k}={v}" for k,v in options.items()],check=True)
        subprocess.run(["cmake","--build",str(d),"--target"]+registration["build_targets"]+["-j",str(jobs)],check=True)
    verify_registered_builds(out,registration)
    return dirs
def _owned_tree(path,uid):
    try:
        if path.is_symlink() or path.lstat().st_uid!=uid:return False
        if path.is_dir():return all(_owned_tree(child,uid) for child in path.iterdir())
        return True
    except OSError:return False
def cleanup_owned_resources(paths,uid=None):
    """Remove only paths explicitly registered as resources owned by this run."""
    uid=os.getuid() if uid is None else uid
    allowed=(Path("/dev/shm"),Path("/tmp"))
    for raw in paths:
        path=Path(raw)
        try:
            if path.parent not in allowed or not _owned_tree(path,uid):continue
            path.unlink() if not path.is_dir() else shutil.rmtree(path)
        except OSError:pass
def cpu_cores_for_interval(begin_ns,end_ns,begin_ticks,end_ticks,hz):
    if end_ns<=begin_ns or end_ticks<begin_ticks or hz<=0:
        raise InvalidTrial("invalid CPU sampling interval")
    return (end_ticks-begin_ticks)/hz/((end_ns-begin_ns)/1_000_000_000)
def topology():
    allowed=set(os.sched_getaffinity(0)); online=[]; entries=[]; sibling_sets=[]
    for c in CPUS:
        online_path=Path(f"/sys/devices/system/cpu/cpu{c}/online")
        is_online=not online_path.exists() or online_path.read_text().strip()=="1"; online.append(is_online)
        base=Path(f"/sys/devices/system/cpu/cpu{c}/topology")
        def val(name): return (base/name).read_text().strip()
        sib=set(int(x) for part in val("thread_siblings_list").split(",") for x in (range(*([int(y) for y in part.split("-")]+[int(part.split("-")[0])+1])) if "-" in part else [int(part)]))
        sibling_sets.append(sib)
        entries.append({"cpu":c,"package":val("physical_package_id"),"die":val("die_id") if (base/"die_id").exists() else "0","core":val("core_id"),"siblings":sorted(sib)})
    domains={(x["package"],x["die"],x["core"]) for x in entries}
    disjoint=all(not sibling_sets[i]&sibling_sets[j] for i in range(3) for j in range(i+1,3))
    return {"cpus":list(CPUS),"allowed":all(c in allowed for c in CPUS),"online":all(online),"entries":entries,"distinct_core_domains":len(domains)==3,"siblings_disjoint":disjoint,"valid":all(c in allowed for c in CPUS) and all(online) and len(domains)==3 and disjoint}
def thread_affinity(pid,cpu):
    bad=[]
    try: tids=list(Path(f"/proc/{pid}/task").iterdir())
    except OSError:return False,["process_missing"]
    for tid in tids:
        try: got=set(os.sched_getaffinity(int(tid.name)))
        except OSError: bad.append(f"{tid.name}:unreadable"); continue
        if got!={cpu}:bad.append(f"{tid.name}:{sorted(got)}")
    return not bad,bad
def run_trial(mode,build_dir,trial,hz,measured,quality=True,warmup=10,cooldown=5):
    if trial.exists():shutil.rmtree(trial)
    trial.mkdir(parents=True);metrics=trial/"metrics";metrics.mkdir();lat=trial/"latency.csv";status=trial/"status.json";pub_status=trial/"publisher-status.json"
    begin_ns=time.monotonic_ns()+STARTUP_GUARD_SEC*1_000_000_000;end_ns=begin_ns+measured*1_000_000_000
    yaml(trial/"publisher.yaml",True,hz,warmup,measured,cooldown,publisher_status_path=pub_status,measure_begin_ns=begin_ns,measure_end_ns=end_ns)
    yaml(trial/"subscriber.yaml",False,hz,warmup,measured,cooldown,lat,status,measure_begin_ns=begin_ns,measure_end_ns=end_ns)
    logs={n:open(trial/f"{n}.log","w") for n in ("roudi","subscriber","publisher")}; prefix=lambda cpu:["taskset","-c",str(cpu)]
    base=clean_env({"IBMW_CORE_METRICS":"off"}); ps=[]
    try:
        r=subprocess.Popen(prefix(CPUS[0])+[str(build_dir/"iox-roudi")],cwd=build_dir,env=base,stdout=logs["roudi"],stderr=subprocess.STDOUT);ps=[r];time.sleep(1)
        s=subprocess.Popen(prefix(CPUS[1])+[str(build_dir/"iox_loaned_ctx_subscriber_app"),str(trial/"subscriber.yaml")],cwd=build_dir,env=base,stdout=logs["subscriber"],stderr=subprocess.STDOUT);ps.insert(0,s)
        if not wait_ready(trial/"subscriber.log","PERF_SUBSCRIBER_RUNNING",s):raise InvalidTrial("subscriber not running")
        subscriber_ready_ns=time.monotonic_ns()
        env=clean_env({"IBMW_CORE_METRICS":"counters" if mode=="counters" else "off","IBMW_OBSERVABILITY_OUTPUT_DIR":str(metrics)})
        p=subprocess.Popen(prefix(CPUS[2])+[str(build_dir/"iox_loaned_ctx_publisher_app"),str(trial/"publisher.yaml")],cwd=build_dir,env=env,stdout=logs["publisher"],stderr=subprocess.STDOUT);ps.insert(0,p)
        if not wait_ready(trial/"publisher.log","PERF_PUBLISHER_RUNNING",p):raise InvalidTrial("publisher not running")
        publisher_ready_ns=time.monotonic_ns();send_begin_ns=begin_ns-warmup*1_000_000_000
        if max(subscriber_ready_ns,publisher_ready_ns)>=send_begin_ns:raise InvalidTrial("readiness missed fixed publish start")
        procs={"publisher":p,"subscriber":s,"roudi":r}; affin={k:thread_affinity(v.pid,CPUS[i]) for i,(k,v) in enumerate((("roudi",r),("subscriber",s),("publisher",p)))}
        if not all(x[0] for x in affin.values()):raise InvalidTrial(f"critical thread affinity escaped: {affin}")
        time.sleep(max(0,(begin_ns-time.monotonic_ns())/1_000_000_000));start_sample_ns=time.monotonic_ns();start={k:proc_ticks(v.pid) for k,v in procs.items()}
        if any(v is None for v in start.values()):raise InvalidTrial("initial proc ticks unreadable")
        totals=[];invalid_pss=0
        while time.monotonic_ns()<end_ns and all(v.poll() is None for v in procs.values()):
            stamp=[]
            for k,v in procs.items():
                x=pss_kib(v.pid);stamp.append(x)
            if all(x is not None for x in stamp):totals.append(sum(stamp))
            else:invalid_pss+=1
            time.sleep(min(1,max(0,(end_ns-time.monotonic_ns())/1_000_000_000)))
        # Freeze the measured wall-clock window with the sampled counters;
        # cooldown and natural shutdown are deliberately outside denominators.
        end_sample_ns=time.monotonic_ns();measured_end={k:proc_ticks(v.pid) for k,v in procs.items()}
        if any(v is None for v in measured_end.values()):raise InvalidTrial("endpoint proc ticks unreadable")
        # Let the application execute its registered cooldown and natural
        # module Stop(), which writes correctness artifacts. CPU accounting
        # remains frozen at measured_end above.
        natural_deadline=time.monotonic()+cooldown+3
        send_end_ns=end_ns+cooldown*1_000_000_000
        while p.poll() is None and time.monotonic_ns()<send_end_ns:time.sleep(.05)
        if p.poll() is None:p.send_signal(signal.SIGINT)
        try:p.wait(max(0,natural_deadline-time.monotonic()))
        except subprocess.TimeoutExpired:raise InvalidTrial("publisher did not complete controlled shutdown")
        if p.returncode!=0:raise InvalidTrial(f"publisher exited {p.returncode}")
        terminate(ps);ps=[]
        if not totals:raise InvalidTrial("no valid simultaneous three-process PSS samples")
        if not status.exists() or not pub_status.exists() or not lat.exists():raise InvalidTrial("missing correctness/latency artifact")
        st=json.loads(status.read_text());pst=json.loads(pub_status.read_text());reconciliation=reconcile_status(pst,st,begin_ns,end_ns);rows=list(csv.DictReader(lat.open()))
        if not rows:raise InvalidTrial("empty latency artifact")
        artifact=metrics/"metrics.jsonl"; artifact_ok=artifact.exists() and artifact.stat().st_size>0
        if mode!="counters" and artifact.exists():raise InvalidTrial(f"{mode} emitted metrics")
        conservation=counter_conservation(artifact) if artifact_ok else None
        if mode=="counters" and (not artifact_ok or conservation is None or not conservation["valid"]):raise InvalidTrial("counter conservation failed")
        elapsed=measured
        cpu_elapsed=(end_sample_ns-start_sample_ns)/1_000_000_000
        hzclock=os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        cpu_by={}
        for k in procs:
            begin_tick=start[k]; end_tick=measured_end[k]
            if begin_tick is None or end_tick is None:raise InvalidTrial("proc ticks lost")
            cpu_by[k]=cpu_cores_for_interval(start_sample_ns,end_sample_ns,
                                              begin_tick,end_tick,hzclock)
        row=rows[-1]; result={"mode":mode,"hz":hz,"measured_sec":elapsed,"unique":st["unique_measured"],"throughput":st["unique_measured"]/elapsed,"p50_us":float(row["p50_us"]),"p99_us":float(row["p99_us"]),"cpu_cores":sum(cpu_by.values()),"cpu_by_process":cpu_by,"cpu_sample_elapsed_sec":cpu_elapsed,"bottleneck_utilization":max(cpu_by.values()),"pss_median_kib":statistics.median(totals),"pss_peak_kib":max(totals),"pss_valid_samples":len(totals),"pss_invalid_samples":invalid_pss,"correctness":st,"publisher_correctness":pst,"reconciliation":reconciliation,"counter_conservation":conservation,"thread_affinity":affin,"measure_begin_monotonic_ns":begin_ns,"measure_end_monotonic_ns":end_ns,"subscriber_ready_ns":subscriber_ready_ns,"publisher_ready_ns":publisher_ready_ns,"cpu_sample_begin_ns":start_sample_ns,"cpu_sample_end_ns":end_sample_ns}
        result["valid"]=reconciliation["valid"]
        result["quality_valid"]=reconciliation["valid"] and (not quality or .65<=result["bottleneck_utilization"]<=.90)
        write_json(trial/"result.json",result);return result
    finally:
        if ps:terminate(ps)
        for f in logs.values():f.close()
def bootstrap_values(values,rng,n=BOOTSTRAPS):
    draws=[statistics.mean(rng.choice(values) for _ in values) for _ in range(n)]
    return statistics.mean(values),percentile(draws,.025),percentile(draws,.975)
def bootstrap_ratio(pairs,numerator,denominator,key,rng,n=BOOTSTRAPS):
    point,lo,hi=bootstrap_values([p[numerator][key]/p[denominator][key] for p in pairs],rng,n)
    return {"ratio":point,"lower":lo,"upper":hi}
def bootstrap_delta(pairs,numerator,denominator,key,rng,n=BOOTSTRAPS):
    point,lo,hi=bootstrap_values([p[numerator][key]-p[denominator][key] for p in pairs],rng,n)
    return {"delta":point,"lower":lo,"upper":hi}
def classify_ratio(ci,budget,lower_better=False):
    if lower_better:
        boundary=1-budget
        return "PASS" if ci["lower"]>=boundary else ("FAIL" if ci["upper"]<boundary else "INCONCLUSIVE")
    boundary=1+budget
    return "PASS" if ci["upper"]<=boundary else ("FAIL" if ci["lower"]>boundary else "INCONCLUSIVE")
def combine(verdicts):
    return "FAIL" if "FAIL" in verdicts else ("PASS" if verdicts and all(x=="PASS" for x in verdicts) else "INCONCLUSIVE")
def aggregate_overall(e2e,micro): return combine((e2e,micro))
def no_load_outcome(micro):
    overall=aggregate_overall("INCONCLUSIVE",micro)
    return overall,verdict_exit_code(overall)
def classify_unpaced_probe(probe):
    reconciliation=probe.get("reconciliation",{})
    lossless=bool(probe.get("valid",False) and reconciliation.get("valid",False))
    # Unpaced runs are saturation diagnostics only. Even a lossless sample is
    # never an eligible source for the paced formal operating point.
    return {"diagnostic_class":"lossless" if lossless else "lossy","eligible":False,
            "throughput_capacity_estimate":None,"probe":probe}
def confirmation_is_strictly_valid(result):
    return bool(result.get("valid",False) and result.get("reconciliation",{}).get("valid",False)
                and result.get("correctness",{}).get("valid",False)
                and result.get("publisher_correctness",{}).get("valid",False)
                and .70<=result.get("bottleneck_utilization",-1)<=.85)
def comparisons(rows,key_metrics,pss=False):
    byblock={};
    for r in rows:byblock.setdefault(r["block"],{})[r["mode"]]=r
    pairs=[v for _,v in sorted(byblock.items()) if len(v)==3];out={};rng=random.Random(SEED)
    for name,num,den in SPECS:
        out[name]={};metric_verdicts=[]
        for metric,threshold_key in key_metrics:
            if threshold_key not in THRESHOLDS[name]:continue
            ci=bootstrap_ratio(pairs,num,den,metric,rng);ci["verdict"]=classify_ratio(ci,THRESHOLDS[name][threshold_key],metric=="throughput");out[name][metric]=ci;metric_verdicts.append(ci["verdict"])
        if pss and "pss_pct" in THRESHOLDS[name]:
            ratio=bootstrap_ratio(pairs,num,den,"pss_median_kib",rng);delta=bootstrap_delta(pairs,num,den,"pss_median_kib",rng)
            ratio["verdict"]=classify_ratio(ratio,THRESHOLDS[name]["pss_pct"]); b=THRESHOLDS[name]["pss_abs_kib"]
            delta["verdict"]="PASS" if delta["upper"]<=b else ("FAIL" if delta["lower"]>b else "INCONCLUSIVE")
            # Either registered PSS allowance may establish budget compliance;
            # both must be definitively exceeded to establish failure.
            pv="PASS" if "PASS" in (ratio["verdict"],delta["verdict"]) else ("FAIL" if ratio["verdict"]==delta["verdict"]=="FAIL" else "INCONCLUSIVE")
            out[name]["pss_ratio"]=ratio;out[name]["pss_absolute_delta_kib"]=delta;out[name]["pss_verdict"]=pv;metric_verdicts.append(pv)
        out[name]["verdict"]=combine(metric_verdicts)
    return pairs,out,combine([v["verdict"] for v in out.values()])
def reporter_hot_value(path):
    found=[]
    for line in path.read_text().splitlines():
        item=json.loads(line)
        if item.get("kind")=="direct_iceoryx_loan" and item.get("metric")=="hot_hook":found.append(int(item.get("value",-1)))
    return found
def run_micro(mode,build_dir,trial):
    if trial.exists():shutil.rmtree(trial)
    trial.mkdir(parents=True);metrics=trial/"metrics";metrics.mkdir()
    env=clean_env({"IBMW_CORE_METRICS":"counters" if mode=="counters" else "off"})
    env["IBMW_OBSERVABILITY_OUTPUT_DIR"]=str(metrics)
    cp=subprocess.run(["taskset","-c",str(CPUS[2]),str(build_dir/"core_metrics_reporter_hot_bench")],cwd=build_dir,env=env,text=True,capture_output=True,check=True)
    result=json.loads(cp.stdout.splitlines()[-1]);artifact=metrics/"metrics.jsonl";expected=22000000
    if mode=="counters":
        values=reporter_hot_value(artifact) if artifact.exists() else []
        if values!=[expected]:raise InvalidTrial(f"Reporter hot-hook conservation failed: {values}")
        result.update(expected_reporter_final=expected,observed_reporter_final=values[0],reporter_final_valid=True,metrics_sha256=sha(artifact))
    else:
        if artifact.exists():raise InvalidTrial(f"{mode} emitted Reporter artifact")
        result.update(expected_reporter_final=0,observed_reporter_final=0,reporter_final_valid=True,metrics_sha256=None)
    expected_bound=mode=="counters"
    if result.get("reporter_bound")!=expected_bound:raise InvalidTrial(f"{mode} Reporter binding mismatch")
    return result
def run_micro_lane(out,builds,micro_orders):
    micro=[]
    with (out/"raw-micro.jsonl").open("w") as f:
        for block,order in enumerate(micro_orders):
            for pos,mode in enumerate(order):
                r=run_micro(mode,builds["out" if mode=="out" else "instrumented"],out/"micro"/f"block-{block:02d}-{pos}-{mode}");r.update(block=block,position=pos,mode=mode);micro.append(r);f.write(json.dumps(r)+"\n");f.flush()
    pairs,cmp,verdict=comparisons(micro,(("ns_per_op","micro"),),False)
    return micro,pairs,cmp,verdict if len(pairs)==30 else "INCONCLUSIVE"
def git_context(root):
    def run(*command):
        p=subprocess.run(command,cwd=root,text=True,capture_output=True)
        return {"command":list(command),"returncode":p.returncode,"stdout":p.stdout,"stderr":p.stderr}
    head=run("git","rev-parse","HEAD");branch=run("git","branch","--show-current")
    status=run("git","status","--short","--branch","--untracked-files=all")
    return {"head":head["stdout"].strip(),"branch":branch["stdout"].strip(),"records":[head,branch,status]}
def _artifact_record(out,path):
    path=Path(path)
    if not path.is_file() or not stat.S_ISREG(path.lstat().st_mode):raise FileNotFoundError(f"missing regular build artifact: {path}")
    relative=path.relative_to(out)
    return {"path":relative.as_posix(),"size":path.stat().st_size,"sha256":sha(path)}
def collect_build_artifacts(out,builds):
    return {kind:{name:_artifact_record(out,d/name) for name in ARTIFACTS} for kind,d in builds.items()}
def registered_plan():
    orders=list(itertools.permutations(MODES))*4;random.Random(SEED).shuffle(orders)
    micro_orders=list(itertools.permutations(MODES))*5;random.Random(SEED).shuffle(micro_orders)
    return {"schema":"core-metrics-round7-preregistered-v1","seed":SEED,"cpus":list(CPUS),"payload_bytes":4096,"batch":BATCH_SIZE,"startup_guard_sec":STARTUP_GUARD_SEC,"window_clock":"CLOCK_MONOTONIC shared [begin,end)","warmup_sec":10,"measured_sec":60,"cooldown_sec":5,"blocks":24,"orders":[list(x) for x in orders],"micro":{"scope":"actual Reporter query/stable binding/null branch/atomic ABI indirect add; live LoanSerializedSample IPC remains E2E","warmup_ops":2000000,"measured_ops":20000000,"blocks":30,"orders":[list(x) for x in micro_orders]},"capacity":{"probes":3,"probe_sec":15,"confirmation_sec":20,"confirmation_range":[.70,.85],"formal_range":[.65,.90],"candidates":list(CAPACITY_CANDIDATES),"candidate_source":"fixed before round7 from separate batch-size and pacing-window design smokes under /tmp/opencode/round7-*-design-20260813","pacing_policy":"absolute batch schedule; batch 128; skip missed slots; no schedule rebase; no catch-up burst","unpaced_policy":"saturation_diagnostic_only"},"bootstrap":BOOTSTRAPS,"thresholds":json.loads(json.dumps(THRESHOLDS))}
def provenance(root,out,builds,topo,source_binding,git):
    commands=(("lscpu",),("uname","-a"),("bash","-lc","for f in /sys/devices/system/cpu/cpu{25,26,27}/cpufreq/scaling_governor /sys/devices/system/cpu/intel_pstate/no_turbo /sys/devices/system/cpu/cpufreq/boost; do [ -r \"$f\" ] && printf '%s=%s\\n' \"$f\" \"$(cat \"$f\")\"; done"))
    records=[]
    for cmd in commands:
        p=subprocess.run(cmd,text=True,capture_output=True);records.append({"command":list(cmd),"returncode":p.returncode,"stdout":p.stdout,"stderr":p.stderr})
    prov={"schema":"ibmw-core-metrics-provenance-v1","plan_sha256":sha(out/"plan.json"),
          "source_manifest_sha256":source_binding["manifest_sha256"],
          "source_state_sha256":source_binding["source_state_sha256"],
          "source_snapshot_sha256":source_binding["snapshot_sha256"],
          "cmake_source":SOURCE_DIRECTORY,"git_context":git,"topology":topo,
          "host_records":records,"registered_build":registered_build_configuration(out),
          "builds":collect_build_artifacts(out,builds)}
    write_json(out/"provenance.json",prov)
    return prov
def _verify_plan(out):
    plan=out/"plan.json";sidecar=out/"plan.sha256"
    if not plan.is_file() or not sidecar.is_file():raise FileNotFoundError("missing preregistered plan evidence")
    digest=sha(plan)
    if sidecar.read_text()!=digest+"  plan.json\n":raise ValueError("plan digest sidecar mismatch")
    if json.loads(plan.read_text())!=registered_plan():raise ValueError("preregistered plan semantics mismatch")
    return digest
def verify_provenance(out,topo=None):
    out=Path(out);path=out/"provenance.json"
    if not path.is_file():raise FileNotFoundError("missing provenance.json")
    prov=json.loads(path.read_text())
    if prov.get("schema")!="ibmw-core-metrics-provenance-v1":raise ValueError("unsupported provenance")
    if prov.get("plan_sha256")!=_verify_plan(out):raise ValueError("provenance plan binding mismatch")
    binding=verify_source_snapshot(out,prov.get("source_manifest_sha256"),prov.get("source_snapshot_sha256"))
    if binding["source_state_sha256"]!=prov.get("source_state_sha256"):raise ValueError("provenance source state mismatch")
    if prov.get("cmake_source")!=SOURCE_DIRECTORY:raise ValueError("provenance CMake source mismatch")
    if topo is not None and prov.get("topology")!=topo:raise ValueError("provenance topology mismatch")
    if not prov.get("git_context",{}).get("records") or not prov.get("host_records"):raise ValueError("incomplete provenance context")
    builds={"out":out/"build-out","instrumented":out/"build-instrumented"}
    verify_registered_builds(out,prov.get("registered_build"))
    expected=collect_build_artifacts(out,builds)
    if prov.get("builds")!=expected:raise ValueError("build artifact provenance mismatch")
    return builds
def _main():
    ap=argparse.ArgumentParser();ap.add_argument("--output",type=Path,required=True);ap.add_argument("--jobs",type=int,default=max(1,(os.cpu_count() or 2)//2));ap.add_argument("--skip-build",action="store_true");a=ap.parse_args()
    root=Path(__file__).resolve().parents[1];out=a.output.resolve()
    if out==root or any(out==root/name or root/name in out.parents for name in ("cmake","src","scripts")):
        raise SystemExit("evidence output must be outside snapshotted source inputs")
    if a.skip_build:
        if not out.is_dir():raise SystemExit("--skip-build requires pre-existing evidence")
        topo=topology()
        try:builds=verify_provenance(out,topo)
        except (OSError,ValueError,json.JSONDecodeError,tarfile.TarError,KeyError,TypeError) as e:raise SystemExit(f"--skip-build evidence verification failed: {e}")
        plan=json.loads((out/"plan.json").read_text());orders=[tuple(x) for x in plan["orders"]];micro_orders=[tuple(x) for x in plan["micro"]["orders"]]
    else:
        if out.exists() and any(out.iterdir()):raise SystemExit("refusing to modify non-empty evidence directory")
        git=git_context(root);out.mkdir(parents=True,exist_ok=True)
        plan=registered_plan();orders=[tuple(x) for x in plan["orders"]];micro_orders=[tuple(x) for x in plan["micro"]["orders"]]
        write_json(out/"plan.json",plan);(out/"plan.sha256").write_text(sha(out/"plan.json")+"  plan.json\n")
        source_binding=create_source_snapshot(root,out)
        builds=build(out/SOURCE_DIRECTORY,out,a.jobs)
        source_binding=verify_source_snapshot(out,source_binding["manifest_sha256"],source_binding["snapshot_sha256"])
        topo=topology();provenance(root,out,builds,topo,source_binding,git);builds=verify_provenance(out,topo)
    if not topo["valid"]:
        summary={"verdict":"INCONCLUSIVE","reason":"fixed affinity validation failed","topology":topo};write_json(out/"summary.json",summary);print(json.dumps(summary,indent=2));return verdict_exit_code(summary["verdict"])
    probes=[]
    for i in range(3):
        try: probe=run_trial("out",builds["out"],out/"calibration"/f"capacity-{i}",0,15,False,2,1)
        except InvalidTrial as e: probe={"trial":f"capacity-{i}","valid":False,"error":str(e)}
        probes.append(classify_unpaced_probe(probe))
    confirmations=[];load=None
    for candidate in plan["capacity"]["candidates"]:
        try:r=run_trial("out",builds["out"],out/"calibration"/f"confirm-{len(confirmations)}",candidate,20,False,3,1)
        except InvalidTrial as e:r={"valid":False,"error":str(e)}
        r["candidate"]=candidate;confirmations.append(r)
        if confirmation_is_strictly_valid(r):load=candidate;break
    calibration={"unpaced_diagnostics":probes,"candidate_confirmations":confirmations,"frozen_load":load,"frozen_load_source":"round7 paced confirmation" if load is not None else None};write_json(out/"calibration.json",calibration)
    if load is None:
        micro,mpairs,mcmp,mverdict=run_micro_lane(out,builds,micro_orders)
        e2e_verdict="INCONCLUSIVE";final,exit_code=no_load_outcome(mverdict)
        summary={"verdict":final,"e2e_verdict":e2e_verdict,"e2e_reason":"all round7 paced confirmations failed","micro_verdict":mverdict,"reason":"all round7 paced confirmations failed","calibration":calibration,"e2e_comparisons":{},"micro_comparisons":mcmp,"complete_micro":len(mpairs)==30}
        write_json(out/"summary.json",summary);(out/"summary.md").write_text("# Core Metrics round 7: "+final+"\n\n"+json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,indent=2));return exit_code
    for mode in MODES:run_trial(mode,builds["out" if mode=="out" else "instrumented"],out/"priming"/mode,load,5,False,2,1)
    e2e=[]
    with (out/"raw-e2e.jsonl").open("w") as f:
        for block,order in enumerate(orders):
            for pos,mode in enumerate(order):
                r=run_trial(mode,builds["out" if mode=="out" else "instrumented"],out/"trials"/f"block-{block:02d}-{pos}-{mode}",load,60);r.update(block=block,position=pos);e2e.append(r);f.write(json.dumps(r)+"\n");f.flush()
    micro,mpairs,mcmp,mverdict=run_micro_lane(out,builds,micro_orders)
    epairs,ecmp,everdict=comparisons(e2e,(("cpu_cores","cpu"),("throughput","throughput"),("p50_us","p50"),("p99_us","p99")),True)
    quality=len(epairs)==24 and all(r["quality_valid"] for r in e2e); complete_micro=len(mpairs)==30
    if not quality:everdict="INCONCLUSIVE"
    if not complete_micro:mverdict="INCONCLUSIVE"
    final=aggregate_overall(everdict,mverdict)
    summary={"verdict":final,"e2e_verdict":everdict,"micro_verdict":mverdict,"quality_valid":quality,"complete_micro":complete_micro,"frozen_load":load,"e2e_comparisons":ecmp,"micro_comparisons":mcmp,"mode_means":{m:{k:statistics.mean(r[k] for r in e2e if r["mode"]==m) for k in ("throughput","p50_us","p99_us","cpu_cores","pss_median_kib","bottleneck_utilization")} for m in MODES}}
    write_json(out/"summary.json",summary);(out/"summary.md").write_text("# Core Metrics round 7: "+final+"\n\n```json\n"+json.dumps(summary,indent=2)+"\n```\n");print(json.dumps(summary,indent=2));return verdict_exit_code(final)
def main():
    try:return _main()
    except InvalidTrial as error:
        try:
            index=sys.argv.index("--output")
            out=Path(sys.argv[index+1]).resolve()
        except (ValueError,IndexError):
            print(f"INCONCLUSIVE: invalid trial: {error}",file=sys.stderr)
            return verdict_exit_code("INCONCLUSIVE")
        out.mkdir(parents=True,exist_ok=True)
        summary={"verdict":"INCONCLUSIVE","reason":f"invalid trial: {error}"}
        write_json(out/"summary.json",summary)
        print(json.dumps(summary,indent=2))
        return verdict_exit_code(summary["verdict"])
if __name__=="__main__":sys.exit(main())
