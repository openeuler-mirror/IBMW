#!/usr/bin/env python3
"""Core Metrics lossless-v2 E2E and Reporter hot-hook evidence gate."""
import argparse, csv, hashlib, io, itertools, json, math, os, random, shutil, signal
import re, stat, statistics, subprocess, sys, tarfile, time
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
FROZEN_LOAD = 448000
LOSSLESS_PROFILE = {"name":"core_metrics_e2e_lossless_v1","publisher_too_slow_policy":"WAIT_FOR_CONSUMER","subscriber_queue_full_policy":"BLOCK_PRODUCER","queue_capacity":256}
CAPACITY_CANDIDATES = (448000,435200,460800,422400,409600,473600)
REGISTERED_CMAKE_OPTIONS = {
 "CMAKE_BUILD_TYPE":"Release","IBMW_BUILD_TESTS":"OFF","IBMW_BUILD_EXAMPLES":"ON",
 "IBMW_BUILD_BENCHMARKS":"ON","IBMW_BUILD_ICEORYX_EXTENSION":"ON",
 "IBMW_BUILD_DDS_EXTENSION":"OFF","IBMW_BUILD_WITH_ROS2":"OFF","IBMW_BUILD_ROS2_TYPES":"OFF",
 "fastcdr_DIR":"/opt/ros/humble/lib/cmake/fastcdr",
 "fastrtps_DIR":"/opt/ros/humble/share/fastrtps/cmake"}
REGISTERED_BUILD_TARGETS = ("iox_loaned_ctx_publisher_app","iox_loaned_ctx_subscriber_app",
                            "core_metrics_reporter_hot_bench","iox-roudi","ibmw_extensions_iceoryx_ext")
VERDICT_EXIT_CODES = {"PASS":0,"FAIL":1,"INCONCLUSIVE":2}
PROTOCOL="core-metrics-lossless-v2"
# Lane label and schema ids written into plan, summary and calibration
# artifacts; read at call time so a lane wrapper can rebind them.
PROTOCOL_LABEL="lossless-v2"
PLAN_SCHEMA="ibmw-core-metrics-lossless-plan-v2"
E2E_SUMMARY_SCHEMA="ibmw-core-metrics-lossless-v2-e2e-summary-v1"
DIAGNOSTIC_SCHEMA="ibmw-core-metrics-lossless-v2-conservation-diagnostic-v1"
CONTRACT_DIRECTORY=Path(__file__).resolve().parent/"contracts"
PROTOCOL_CONTRACT=CONTRACT_DIRECTORY/"core-metrics-lossless-v2.json"
DEPENDENCY_CONTRACT=CONTRACT_DIRECTORY/"core-metrics-dependencies-v1.json"
PROTOCOL_CONTRACT_ARCHIVED="scripts/contracts/core-metrics-lossless-v2.json"
DEPENDENCY_CONTRACT_ARCHIVED="scripts/contracts/core-metrics-dependencies-v1.json"
BWRAP=Path("/usr/bin/bwrap")
# Lane identity. core_metrics_shm_perf.py rebinds these on its own module
# instance; the values below are the registered iceoryx lane.
BROKER_BINARY="iox-roudi"
E2E_PUBLISHER_APP="iox_loaned_ctx_publisher_app"
E2E_SUBSCRIBER_APP="iox_loaned_ctx_subscriber_app"
DIRECT_COUNTER_KIND="direct_iceoryx_loan"
COUNTER_MODULE="IoxLoanedCtxPublisherModule"
COUNTER_TOPIC="iox_bench_ctx_topic"
COUNTER_DRIVER="iceoryx"
# Lane hook into run_trial; None here, so this lane's trials never call it.
TRIAL_OBSERVER=None

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
def canonical_json(value):return json.dumps(value,sort_keys=True,separators=(",",":"),ensure_ascii=True).encode()
def strict_json_bytes(data,label="JSON"):
    def pairs(items):
        value={}
        for key,item in items:
            if key in value:raise ValueError(f"duplicate {label} key: {key}")
            value[key]=item
        return value
    return json.loads(data,object_pairs_hook=pairs,parse_constant=lambda x:(_ for _ in ()).throw(ValueError(f"non-finite {label} number")))
def _load_closed_contract(path,schema,keys):
    path=Path(path)
    if not path.is_file() or path.is_symlink():raise FileNotFoundError(f"missing trusted contract: {path}")
    value=strict_json_bytes(path.read_bytes(),"contract")
    if not isinstance(value,dict) or set(value)!=set(keys) or value.get("schema")!=schema:raise ValueError(f"invalid trusted contract: {path.name}")
    return value,sha(path)
def protocol_contract(path=None):
    value,digest=_load_closed_contract(path or PROTOCOL_CONTRACT,"ibmw-core-metrics-protocol-contract-v1",("schema","protocol","payload_bytes","batch","frozen_load","warmup_sec","measured_sec","cooldown_sec","blocks","bootstrap","thresholds","delivery_profile"))
    expected={"schema":"ibmw-core-metrics-protocol-contract-v1","protocol":PROTOCOL,"payload_bytes":4096,"batch":BATCH_SIZE,"frozen_load":FROZEN_LOAD,"warmup_sec":10,"measured_sec":60,"cooldown_sec":5,"blocks":24,"bootstrap":BOOTSTRAPS,"thresholds":THRESHOLDS,"delivery_profile":LOSSLESS_PROFILE}
    if value!=expected:raise ValueError("trusted protocol contract semantics mismatch")
    return value,digest
def verdict_exit_code(verdict):
    try:return VERDICT_EXIT_CODES[verdict]
    except KeyError:raise ValueError(f"unknown verdict: {verdict}")

SOURCE_ROOTS=("CMakeLists.txt","VERSION","cmake","src","scripts","tools")
SOURCE_MANIFEST="source-manifest.json"
SOURCE_ARCHIVE="source-snapshot.tar"
SOURCE_DIRECTORY="source-snapshot"
DEPENDENCY_DIRECTORY="dependency-snapshot"
DEPENDENCY_MANIFEST="dependency-manifest.json"
DEPENDENCY_CONTRACT_COPY="dependency-contract.json"
DEPENDENCY_MAX_ENTRIES=100000
DEPENDENCY_MAX_BYTES=2*1024*1024*1024
DEPENDENCIES={
 "asio":("GetAsio.cmake","https://github.com/chriskohlhoff/asio/archive/asio-1-30-2.tar.gz",("asio/include/asio/version.hpp","#define ASIO_VERSION 103002")),
 "backward":("GetBackward.cmake","https://github.com/bombela/backward-cpp/archive/refs/tags/v1.6.tar.gz",("CMakeLists.txt","project(backward CXX)")),
 "cpptoml":("GetCppToml.cmake","https://github.com/skystrife/cpptoml/archive/refs/tags/v0.1.0.tar.gz",("CMakeLists.txt","set(cpptoml_VERSION 0.4.0)")),
 "fmt":("GetFmt.cmake","https://github.com/fmtlib/fmt/archive/10.2.1.tar.gz",("include/fmt/core.h","FMT_VERSION 100201")),
 "gflags":("GetGFlags.cmake","https://github.com/gflags/gflags/archive/v2.2.2.tar.gz",("CMakeLists.txt",'set (PACKAGE_VERSION     "2.2.2")')),
 "iceoryx":("GetIceoryx.cmake","https://github.com/eclipse-iceoryx/iceoryx/archive/refs/tags/v2.95.4.tar.gz",("iceoryx_posh/CMakeLists.txt",'set(IOX_VERSION_STRING "2.95.4")')),
 "libunifex":("GetLibUnifex.cmake","https://github.com/facebookexperimental/libunifex/archive/591ec09e7d51858ad05be979d4034574215f5971.tar.gz",("CMakeLists.txt","project(libunifex")),
 "nlohmann_json":("GetNlohmannJson.cmake","https://github.com/nlohmann/json/archive/v3.11.3.tar.gz",("CMakeLists.txt","project(nlohmann_json VERSION 3.11.3")),
 "tbb":("GetTBB.cmake","https://github.com/oneapi-src/oneTBB/archive/v2021.13.0.tar.gz",("include/oneapi/tbb/version.h","#define TBB_VERSION_MINOR 13")),
 "yaml-cpp":("GetYamlCpp.cmake","https://github.com/jbeder/yaml-cpp/archive/0.8.0.tar.gz",("CMakeLists.txt","project(YAML_CPP VERSION 0.8.0")),
}
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
def _dependency_mode(mode,kind):
    if kind=="directory":return "040755"
    return "100755" if mode&0o111 else "100644"
def _dependency_lock(root,name,spec):
    declaration=Path(root)/"cmake"/spec[0]
    if not declaration.is_file() or declaration.is_symlink():raise FileNotFoundError(f"missing trusted dependency declaration: {name}")
    text=declaration.read_text()
    urls=re.findall(r'https://[^"\s)]+',text)
    if urls!=[spec[1]]:raise ValueError(f"wrong source lock for dependency: {name}")
    return {"declaration":f"cmake/{spec[0]}","url":spec[1],"identity":spec[1].rsplit("/",1)[-1]}
def _dependency_entries(source,name,max_entries,max_bytes):
    source=Path(source);entries=[];payloads={};total=0;case_paths={}
    if not source.is_dir() or source.is_symlink():raise FileNotFoundError(f"missing dependency source component: {name}")
    def visit(relative):
        nonlocal total
        path=source/relative;before=path.lstat();mode=before.st_mode;key=relative.as_posix().casefold()
        if key in case_paths and case_paths[key]!=relative.as_posix():raise ValueError(f"case-colliding dependency path: {name}/{relative}")
        case_paths[key]=relative.as_posix()
        if stat.S_ISLNK(mode):raise ValueError(f"dependency symlink rejected: {name}/{relative}")
        if stat.S_ISDIR(mode):
            entry={"path":relative.as_posix(),"kind":"directory","mode":_dependency_mode(mode,"directory"),"bytes":0,"sha256":hashlib.sha256(b"").hexdigest()}
            entries.append(entry)
            for child in sorted(path.iterdir(),key=lambda x:os.fsencode(x.name)):visit(relative/child.name)
        elif stat.S_ISREG(mode):
            data=path.read_bytes();total+=len(data);payloads[relative.as_posix()]=data
            entries.append({"path":relative.as_posix(),"kind":"file","mode":_dependency_mode(mode,"file"),"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest()})
        else:raise ValueError(f"unsupported dependency entry: {name}/{relative}")
        if len(entries)>max_entries:raise ValueError(f"dependency entry limit exceeded: {name}")
        if total>max_bytes:raise ValueError(f"dependency byte limit exceeded: {name}")
        after=path.lstat()
        if (before.st_mode,before.st_size,before.st_mtime_ns)!=(after.st_mode,after.st_size,after.st_mtime_ns):raise RuntimeError(f"dependency changed while snapshotting: {name}/{relative}")
    for child in sorted(source.iterdir(),key=lambda x:os.fsencode(x.name)):visit(Path(child.name))
    entries.sort(key=lambda x:x["path"])
    return entries,payloads,total
def _dependency_state(components):
    canonical={"components":components}
    return hashlib.sha256(json.dumps(canonical,sort_keys=True,separators=(",",":")).encode()).hexdigest()
def _dependency_tree_digest(entries):return hashlib.sha256(canonical_json(entries)).hexdigest()
def load_dependency_contract(root,path=None):
    contract,digest=_load_closed_contract(path or DEPENDENCY_CONTRACT,"ibmw-core-metrics-dependency-contract-v1",("schema","components"))
    components=contract["components"]
    required={"name","declaration","declaration_sha256","url","tree_sha256","entry_count","byte_count"}
    if not isinstance(components,list) or [x.get("name") for x in components]!=sorted(DEPENDENCIES) or any(set(x)!=required for x in components):raise ValueError("invalid dependency contract components")
    for item in components:
        name=item["name"]
        if root is not None:
            lock=_dependency_lock(root,name,DEPENDENCIES[name]);declaration=Path(root)/lock["declaration"]
            if item["declaration"]!=lock["declaration"] or item["url"]!=lock["url"] or item["declaration_sha256"]!=sha(declaration):raise ValueError(f"dependency declaration contract mismatch: {name}")
        for key in ("declaration_sha256","tree_sha256"):
            if not isinstance(item[key],str) or not re.fullmatch(r"[0-9a-f]{64}",item[key]):raise ValueError(f"invalid dependency contract digest: {name}")
        if not all(isinstance(item[x],int) and item[x]>=0 for x in ("entry_count","byte_count")):raise ValueError(f"invalid dependency contract totals: {name}")
    return {x["name"]:x for x in components},digest
def create_dependency_snapshot(root,pool,out,max_entries=DEPENDENCY_MAX_ENTRIES,max_bytes=DEPENDENCY_MAX_BYTES,contract_path=None):
    root=Path(root);pool=Path(pool);out=Path(out);directory=out/DEPENDENCY_DIRECTORY
    if directory.exists():raise FileExistsError(directory)
    trusted_path=Path(contract_path or DEPENDENCY_CONTRACT);trusted,contract_sha=load_dependency_contract(root,trusted_path);verified=[];remaining_entries=max_entries;remaining_bytes=max_bytes
    for name,spec in sorted(DEPENDENCIES.items()):
        lock=_dependency_lock(root,name,spec);source=pool/f"{name}-src"
        entries,payloads,total=_dependency_entries(source,name,remaining_entries,remaining_bytes)
        expected=trusted[name]
        if (_dependency_tree_digest(entries)!=expected["tree_sha256"] or len(entries)!=expected["entry_count"] or total!=expected["byte_count"]):raise ValueError(f"trusted source tree mismatch for dependency: {name}")
        verified.append((name,lock,entries,payloads,total));remaining_entries-=len(entries);remaining_bytes-=total
    # Nothing is copied until every source pool component matches trusted data.
    directory.mkdir();components=[]
    for name,lock,entries,payloads,total in verified:
        target=directory/name;target.mkdir()
        for entry in entries:
            path=target/entry["path"]
            if entry["kind"]=="directory":path.mkdir()
            else:path.write_bytes(payloads[entry["path"]])
            path.chmod(int(entry["mode"][-3:],8))
        components.append({"name":name,"source_component":f"{name}-src","locked_identity":lock,"trusted_tree_sha256":trusted[name]["tree_sha256"],"entry_count":len(entries),"byte_count":total,"entries":entries})
    aggregate=_dependency_state(components)
    manifest={"schema":"ibmw-dependency-snapshot-v2","contract_sha256":contract_sha,"aggregate_sha256":aggregate,"entry_limit":max_entries,"byte_limit":max_bytes,"components":components}
    (out/DEPENDENCY_CONTRACT_COPY).write_bytes(trusted_path.read_bytes())
    write_json(out/DEPENDENCY_MANIFEST,manifest)
    return verify_dependency_snapshot(out)
def verify_dependency_snapshot(out,expected_manifest_sha=None,expected_aggregate=None):
    out=Path(out);manifest_path=out/DEPENDENCY_MANIFEST;directory=out/DEPENDENCY_DIRECTORY
    if not manifest_path.is_file() or manifest_path.is_symlink() or not directory.is_dir() or directory.is_symlink():raise FileNotFoundError("incomplete dependency snapshot evidence")
    manifest_sha=sha(manifest_path)
    if expected_manifest_sha and manifest_sha!=expected_manifest_sha:raise ValueError("dependency manifest raw digest mismatch")
    manifest=json.loads(manifest_path.read_text())
    if manifest.get("schema")!="ibmw-dependency-snapshot-v2":raise ValueError("unsupported dependency manifest")
    archived_root=out/SOURCE_DIRECTORY;contract_path=archived_root/DEPENDENCY_CONTRACT_ARCHIVED if archived_root.is_dir() else out/DEPENDENCY_CONTRACT_COPY
    trusted,contract_sha=load_dependency_contract(archived_root if archived_root.is_dir() else None,contract_path)
    if manifest.get("contract_sha256")!=contract_sha:raise ValueError("dependency contract binding mismatch")
    components=manifest.get("components")
    if not isinstance(components,list) or [x.get("name") for x in components]!=sorted(DEPENDENCIES):raise ValueError("dependency component list mismatch")
    if _dependency_state(components)!=manifest.get("aggregate_sha256"):raise ValueError("dependency aggregate digest mismatch")
    if expected_aggregate and manifest["aggregate_sha256"]!=expected_aggregate:raise ValueError("dependency aggregate binding mismatch")
    total_entries=total_bytes=0
    for component in components:
        name=component["name"];expected_lock=_dependency_lock(out/SOURCE_DIRECTORY,name,DEPENDENCIES[name]) if (out/SOURCE_DIRECTORY).is_dir() else None
        if expected_lock is not None and component.get("locked_identity")!=expected_lock:raise ValueError(f"dependency source lock mismatch: {name}")
        entries=component.get("entries")
        if not isinstance(entries,list) or [x.get("path") for x in entries]!=sorted(x.get("path") for x in entries):raise ValueError(f"invalid dependency entries: {name}")
        if len({x.get("path") for x in entries})!=len(entries) or len({str(x.get("path")).casefold() for x in entries})!=len(entries):raise ValueError(f"duplicate dependency path: {name}")
        actual,_,byte_count=_dependency_entries(directory/name,name,DEPENDENCY_MAX_ENTRIES,DEPENDENCY_MAX_BYTES)
        if actual!=entries:raise ValueError(f"dependency snapshot content mismatch: {name}")
        if _dependency_tree_digest(actual)!=trusted[name]["tree_sha256"] or component.get("trusted_tree_sha256")!=trusted[name]["tree_sha256"]:raise ValueError(f"dependency trusted tree mismatch: {name}")
        if component.get("entry_count")!=len(entries) or component.get("byte_count")!=byte_count:raise ValueError(f"dependency component totals mismatch: {name}")
        total_entries+=len(entries);total_bytes+=byte_count
    if total_entries>manifest.get("entry_limit",-1) or total_bytes>manifest.get("byte_limit",-1):raise ValueError("dependency snapshot bounds exceeded")
    return {"manifest_sha256":manifest_sha,"aggregate_sha256":manifest["aggregate_sha256"]}
COUNTER_METRICS=("logical_attempt","logical_success","logical_fail","internal_retry","publish","release")
COUNTER_TYPES=tuple(f"dds:ibmw::samples::transport_benchmark::BenchPlainPayload{x}" for x in ("1KB","4KB","16KB","64KB","256KB","1MB","4MB"))
def _nonnegative_integer(value,label):
    if not isinstance(value,int) or isinstance(value,bool) or value<0:raise ValueError(f"invalid nonnegative counter: {label}")
    return value
def counter_conservation(path):
    lines=Path(path).read_bytes().splitlines()
    if len(lines)!=64 or any(not line.strip() for line in lines):raise ValueError("metrics JSONL cardinality mismatch")
    values={metric:0 for metric in COUNTER_METRICS};seen=set();structural_seen=set();seq=[];kind_counts={}
    allowed={"logical_publish","logical_loan","driver_send","source_driver_loan",DIRECT_COUNTER_KIND}
    for raw in lines:
        item=strict_json_bytes(raw,"metrics")
        if not isinstance(item,dict) or item.get("schema")!="ibmw.core_metrics.v1" or item.get("complete") is not True:raise ValueError("invalid metrics envelope")
        sequence=_nonnegative_integer(item.get("snapshot_seq"),"snapshot_seq");seq.append(sequence)
        kind=item.get("kind")
        if kind not in allowed:raise ValueError(f"unknown metrics kind: {kind}")
        kind_counts[kind]=kind_counts.get(kind,0)+1
        cumulative=item.get("cumulative")
        if not isinstance(cumulative,dict):raise ValueError("invalid cumulative metrics object")
        if kind==DIRECT_COUNTER_KIND:
            if set(item)!={"schema","snapshot_seq","kind","metric","topic","type","driver","value","cumulative","complete"}:raise ValueError("unknown direct counter field")
            if set(cumulative)!={"metric","topic","type","driver","value"}:raise ValueError("unknown cumulative direct counter field")
            metric=item["metric"];dimension=(metric,item["topic"],item["type"],item["driver"])
            if metric not in COUNTER_METRICS or item["topic"]!=COUNTER_TOPIC or item["type"] not in COUNTER_TYPES or item["driver"]!=COUNTER_DRIVER:raise ValueError("unknown direct counter dimension")
            if dimension in seen:raise ValueError("duplicate direct counter dimension")
            seen.add(dimension);value=_nonnegative_integer(item["value"],metric)
            if cumulative!={"metric":metric,"topic":item["topic"],"type":item["type"],"driver":COUNTER_DRIVER,"value":value}:raise ValueError("direct cumulative counter mismatch")
            values[metric]+=value
        else:
            if set(item)!={"schema","snapshot_seq","kind","cumulative","complete"}:raise ValueError(f"unknown {kind} counter field")
            fields={"logical_publish":("pkg","module","topic","type","attempted","rejected"),"logical_loan":("pkg","module","topic","type","loan_attempted","loan_succeeded","loan_failed"),"driver_send":("pkg","module","topic","type","driver","attempted","returned"),"source_driver_loan":("driver","loan_publish_called","loan_release_called")}[kind]
            if set(cumulative)!=set(fields):raise ValueError(f"invalid {kind} cumulative schema")
            numeric={"logical_publish":("attempted","rejected"),"logical_loan":("loan_attempted","loan_succeeded","loan_failed"),"driver_send":("attempted","returned"),"source_driver_loan":("loan_publish_called","loan_release_called")}[kind]
            for field in numeric:_nonnegative_integer(cumulative[field],f"{kind}.{field}")
            if kind=="source_driver_loan":dimension=(kind,cumulative["driver"])
            else:
                if cumulative["pkg"]!="core" or cumulative["module"]!=COUNTER_MODULE or cumulative["topic"]!=COUNTER_TOPIC or cumulative["type"] not in COUNTER_TYPES:raise ValueError(f"unknown {kind} dimension")
                if kind=="driver_send" and cumulative["driver"]!=COUNTER_DRIVER:raise ValueError("unknown driver_send driver")
                dimension=(kind,cumulative["type"])
            if dimension in structural_seen:raise ValueError(f"duplicate {kind} dimension")
            structural_seen.add(dimension)
    if seq!=list(range(1,65)) or kind_counts!={"logical_publish":7,"logical_loan":7,"driver_send":7,"source_driver_loan":1,DIRECT_COUNTER_KIND:42}:raise ValueError("metrics sequence/kind cardinality mismatch")
    expected={(metric,COUNTER_TOPIC,type_name,COUNTER_DRIVER) for type_name in COUNTER_TYPES for metric in COUNTER_METRICS}
    expected_structural={(kind,type_name) for kind in ("logical_publish","logical_loan","driver_send") for type_name in COUNTER_TYPES}|{("source_driver_loan",COUNTER_DRIVER)}
    if seen!=expected or structural_seen!=expected_structural:raise ValueError("counter dimensions incomplete")
    a=values["logical_attempt"];s=values["logical_success"]
    valid=(a>0 and a==s+values["logical_fail"]
           and values["publish"]+values["release"]==s
           and values["internal_retry"]<=15*a)
    return {"window":"engine_lifecycle_total","values":values,"valid":valid}
def clean_env(extra):
    keep=("HOME","USER","LOGNAME","PATH","LD_LIBRARY_PATH","LANG","LC_ALL","TZ")
    env={k:v for k,v in os.environ.items() if k in keep}; env.update(extra); return env
def terminate(ps,fail_closed=False):
    for p in ps:
        if p.poll() is not None:continue
        p.send_signal(signal.SIGINT)
        try:p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill();p.wait()
            if fail_closed:raise InvalidTrial("bounded process shutdown timed out")
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
        options: {{shm_init_size: 4096, runtime_id: "metrics_v2_{rid}"}}
  logging: {{core_level: WARN, backends: [{{type: console}}]}}
  scheduling:
    executors: [{{name: work_thread_pool, type: thread_pool, options: {{thread_num: 2}}}}]
  messaging:
    drivers: [{{type: iceoryx, options: {{listener_thread_name: "perf_{rid}", benchmark_profile: "{LOSSLESS_PROFILE['name']}", publisher_too_slow_policy: "{LOSSLESS_PROFILE['publisher_too_slow_policy']}", subscriber_queue_full_policy: "{LOSSLESS_PROFILE['subscriber_queue_full_policy']}", subscriber_queue_capacity: {LOSSLESS_PROFILE['queue_capacity']}}}}}]
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
    label: core_metrics_lossless_v2_4KiB
{tail}''')
    return sha(path)

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
def _dependency_build_options(out):
    out=Path(out)
    if not (out/DEPENDENCY_MANIFEST).is_file():return {}
    verify_dependency_snapshot(out);base=(out/DEPENDENCY_DIRECTORY).resolve()
    options={"FETCHCONTENT_FULLY_DISCONNECTED":"ON"}
    for component in sorted(DEPENDENCIES):
        path=str((base/component).resolve())
        options[f"{component}_LOCAL_SOURCE"]=path
        options[f"FETCHCONTENT_SOURCE_DIR_{component.upper()}"]=path
    return options
def registered_build_configuration(out,runner_python=None):
    out=Path(out);python=str(Path(runner_python or sys.executable).resolve());source=str((out/SOURCE_DIRECTORY).resolve());dependency_options=_dependency_build_options(out)
    builds={}
    for name in ("out","instrumented"):
        options=dict(REGISTERED_CMAKE_OPTIONS)
        options.update(dependency_options)
        options.update({"Python3_EXECUTABLE":python,"PYTHON_EXECUTABLE":python,
                        "IBMW_ENABLE_CORE_METRICS":"OFF" if name=="out" else "ON",
                        "IBMW_ENABLE_REPORTER_V1":"OFF" if name=="out" else "ON"})
        builds[name]={"directory":f"build-{name}","configure_options":options}
    schema="ibmw-core-metrics-build-registration-v2" if dependency_options else "ibmw-core-metrics-build-registration-v1"
    return {"schema":schema,"runner_python":python,
            "cmake_source":source,"build_targets":list(REGISTERED_BUILD_TARGETS),"builds":builds}
def _parse_cmake_cache(path):
    values={}
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith(("#","//")) or "=" not in line or ":" not in line.split("=",1)[0]:continue
        typed,value=line.split("=",1);key,_=typed.split(":",1);values[key]=value
    return values
def verify_registered_builds(out,registration,require_current=True):
    out=Path(out)
    if require_current and registration!=registered_build_configuration(out):raise ValueError("build registration mismatch")
    expected_source=Path(registration["cmake_source"]).resolve();dependency_root=(out/DEPENDENCY_DIRECTORY).resolve()
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
        if registration.get("schema")=="ibmw-core-metrics-build-registration-v2":
            if cache.get("FETCHCONTENT_FULLY_DISCONNECTED")!="ON":raise ValueError(f"{name} network isolation mismatch")
            for component in DEPENDENCIES:
                for key in (f"{component}_LOCAL_SOURCE",f"FETCHCONTENT_SOURCE_DIR_{component.upper()}"):
                    actual=cache.get(key);resolved=Path(actual).resolve() if actual else None
                    if resolved is None or resolved!=dependency_root/component or dependency_root not in resolved.parents:
                        raise ValueError(f"{name} dependency source path mismatch: {key}")
    return registration
def sandbox_command(build_dir,command):
    build_dir=Path(build_dir).resolve()
    if not BWRAP.is_file() or not os.access(BWRAP,os.X_OK):raise RuntimeError("required /usr/bin/bwrap sandbox unavailable")
    if build_dir==Path("/tmp") or Path("/tmp") in build_dir.parents:raise RuntimeError("sandbox build directory cannot be below isolated /tmp")
    return [str(BWRAP),"--unshare-net","--die-with-parent","--new-session","--ro-bind","/","/","--dev","/dev","--bind",str(build_dir),str(build_dir),"--tmpfs","/tmp","--chdir",str(build_dir),"--"]+list(command)
def verify_network_sandbox(build_dir):
    probe=[sys.executable,"-c","import socket,sys; s=socket.socket();\ntry:s.connect((\"1.1.1.1\",53))\nexcept OSError:sys.exit(0)\nsys.exit(9)"]
    try:result=subprocess.run(sandbox_command(build_dir,probe),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
    except (OSError,subprocess.TimeoutExpired) as error:raise RuntimeError(f"network sandbox unavailable: {error}")
    if result.returncode!=0:raise RuntimeError(f"network sandbox probe failed closed ({result.returncode}): {result.stderr.decode(errors='replace')}")
def build(source,out,jobs):
    verify_dependency_snapshot(out);registration=registered_build_configuration(out);source=Path(source).resolve()
    if source!=Path(registration["cmake_source"]):raise ValueError("build source differs from registered source")
    dirs={"out":out/"build-out","instrumented":out/"build-instrumented"}
    sandbox_env=clean_env({"CCACHE_DISABLE":"1"})
    for name,d in dirs.items():
        d.mkdir(parents=True,exist_ok=False)
        verify_network_sandbox(d)
        verify_dependency_snapshot(out)
        options=registration["builds"][name]["configure_options"]
        configure=["cmake","-S",str(source),"-B",str(d)]+[f"-D{k}={v}" for k,v in options.items()]
        subprocess.run(sandbox_command(d,configure),check=True,env=sandbox_env)
        verify_dependency_snapshot(out)
        compile_command=["cmake","--build",str(d),"--target"]+registration["build_targets"]+["-j",str(jobs)]
        subprocess.run(sandbox_command(d,compile_command),check=True,env=sandbox_env)
        verify_dependency_snapshot(out)
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
E2E_STATUS_KEYS={"received_measured","measure_begin_monotonic_ns","measure_end_monotonic_ns","unique_measured","duplicate","out_of_order","missing","span_missing","first_seq","last_seq","invalid","valid"}
PUBLISHER_STATUS_KEYS={"measure_begin_monotonic_ns","measure_end_monotonic_ns","attempts","success","fail","first_attempt_seq","last_attempt_seq","first_success_seq","last_success_seq","completed_send_window","valid"}
LATENCY_FIELDS=("label","count","min_us","avg_us","stddev_us","p50_us","p90_us","p99_us","p999_us","max_us")
def _strict_status(path,keys):
    value=strict_json_bytes(Path(path).read_bytes(),"status")
    if not isinstance(value,dict) or set(value)!=keys or not isinstance(value["valid"],bool):raise ValueError(f"invalid status schema: {path}")
    for key,item in value.items():
        if key not in ("valid","completed_send_window"):_nonnegative_integer(item,key)
    if "completed_send_window" in value and not isinstance(value["completed_send_window"],bool):raise ValueError("invalid completed_send_window")
    return value
def recompute_e2e_result(trial):
    trial=Path(trial);observed=strict_json_bytes((trial/"observations.json").read_bytes(),"observations")
    if not isinstance(observed,dict) or set(observed)!={"base","quality_required"}:raise ValueError("invalid observations schema")
    result=dict(observed["base"]);mode=result.get("mode")
    st=_strict_status(trial/"status.json",E2E_STATUS_KEYS);pst=_strict_status(trial/"publisher-status.json",PUBLISHER_STATUS_KEYS)
    with (trial/"latency.csv").open(newline="") as stream:
        reader=csv.DictReader(stream)
        if tuple(reader.fieldnames or ())!=LATENCY_FIELDS:raise ValueError("latency CSV schema mismatch")
        rows=list(reader)
    if len(rows)!=1 or set(rows[0])!=set(LATENCY_FIELDS):raise ValueError("latency CSV cardinality mismatch")
    row=rows[0];count=_nonnegative_integer(int(row["count"]),"latency count")
    if count!=st["unique_measured"]:raise ValueError("latency/status count mismatch")
    begin=result["measure_begin_monotonic_ns"];end=result["measure_end_monotonic_ns"]
    reconciliation=reconcile_status(pst,st,begin,end);artifact=trial/"metrics"/"metrics.jsonl"
    if mode=="counters":
        if not artifact.is_file():raise ValueError("missing counter artifact")
        conservation=counter_conservation(artifact)
    else:
        if artifact.exists():raise ValueError(f"{mode} emitted metrics")
        conservation=None
    result.update(unique=st["unique_measured"],throughput=st["unique_measured"]/result["measured_sec"],p50_us=float(row["p50_us"]),p99_us=float(row["p99_us"]),correctness=st,publisher_correctness=pst,reconciliation=reconciliation,counter_conservation=conservation,publisher_yaml_sha256=sha(trial/"publisher.yaml"),subscriber_yaml_sha256=sha(trial/"subscriber.yaml"),valid=reconciliation["valid"])
    result["quality_valid"]=reconciliation["valid"] and (not observed["quality_required"] or .65<=result["bottleneck_utilization"]<=.90)
    result["counter_windows"]={"lifecycle_total":"metrics.jsonl is cumulative from Engine metrics Start through final shutdown snapshot","measured_delivery":"status.json and publisher-status.json use the shared [measure_begin_monotonic_ns, measure_end_monotonic_ns) window","equal_windows":False}
    return result
def _seal_artifact_names(mode,micro=False):
    names=["result.json"]
    if micro:
        names+=["bench-output.json"]
        if mode=="counters":names.append("metrics/metrics.jsonl")
    else:
        names += ["observations.json","status.json","publisher-status.json","latency.csv","publisher.yaml","subscriber.yaml","publisher.log","subscriber.log"]
        if BROKER_BINARY:names.append("roudi.log")
        if mode=="counters":names.append("metrics/metrics.jsonl")
    return sorted(names)
def _seal_artifacts(trial,mode,micro=False):
    trial=Path(trial)
    artifacts=[]
    for name in _seal_artifact_names(mode,micro):
        path=trial/name
        if not path.is_file() or path.is_symlink():raise FileNotFoundError(f"missing seal artifact: {name}")
        artifacts.append({"path":name,"bytes":path.stat().st_size,"sha256":sha(path)})
    evidence_root=next((parent for parent in trial.parents if (parent/"plan.json").is_file()),None)
    context={name:sha(evidence_root/name) if evidence_root is not None and (evidence_root/name).is_file() else None for name in ("plan.json","execution-binding.json","provenance.json")}
    seal={"schema":"ibmw-core-metrics-trial-seal-v1","protocol":PROTOCOL,"mode":mode,"kind":"micro" if micro else "e2e","context_sha256":context,"artifacts":artifacts,"result_sha256":sha(trial/"result.json")}
    payload=json.dumps(seal,indent=2,sort_keys=True)+"\n";path=trial/"seal.json"
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o444)
    try:os.write(fd,payload.encode());os.fsync(fd)
    finally:os.close(fd)
def _load_sealed_result(trial,mode,micro=False):
    trial=Path(trial);seal_path=trial/"seal.json"
    if not seal_path.is_file() or seal_path.is_symlink():return None
    seal=strict_json_bytes(seal_path.read_bytes(),"seal")
    if set(seal)!={"schema","protocol","mode","kind","context_sha256","artifacts","result_sha256"} or seal["schema"]!="ibmw-core-metrics-trial-seal-v1" or seal["protocol"]!=PROTOCOL or seal["mode"]!=mode or seal["kind"]!=("micro" if micro else "e2e"):raise ValueError("trial seal binding mismatch")
    if not isinstance(seal["context_sha256"],dict) or set(seal["context_sha256"])!={"plan.json","execution-binding.json","provenance.json"}:raise ValueError("invalid trial seal context")
    evidence_root=next((parent for parent in trial.parents if (parent/"plan.json").is_file()),None)
    expected_context={name:sha(evidence_root/name) if evidence_root is not None and (evidence_root/name).is_file() else None for name in seal["context_sha256"]}
    if seal["context_sha256"]!=expected_context:raise ValueError("trial seal context mismatch")
    paths=[x.get("path") for x in seal["artifacts"]]
    if paths!=_seal_artifact_names(mode,micro):raise ValueError("noncanonical trial seal artifacts")
    for item in seal["artifacts"]:
        if set(item)!={"path","bytes","sha256"} or not _safe_relative(item["path"]):raise ValueError("invalid trial seal artifact")
        path=trial/item["path"]
        if not path.is_file() or path.is_symlink() or path.stat().st_size!=item["bytes"] or sha(path)!=item["sha256"]:raise ValueError(f"sealed artifact mismatch: {item['path']}")
    recomputed=recompute_micro_result(trial,mode) if micro else recompute_e2e_result(trial)
    stored=strict_json_bytes((trial/"result.json").read_bytes(),"result")
    if stored!=recomputed or sha(trial/"result.json")!=seal["result_sha256"]:raise ValueError("sealed result recomputation mismatch")
    return stored
def run_trial(mode,build_dir,trial,hz,measured,quality=True,warmup=10,cooldown=5,
              subscriber_build_dir=None):
    if TRIAL_OBSERVER:TRIAL_OBSERVER.before_trial(trial)
    subscriber_build_dir=Path(subscriber_build_dir or build_dir)
    if trial.exists():shutil.rmtree(trial)
    trial.mkdir(parents=True);metrics=trial/"metrics";metrics.mkdir();lat=trial/"latency.csv";status=trial/"status.json";pub_status=trial/"publisher-status.json"
    begin_ns=time.monotonic_ns()+STARTUP_GUARD_SEC*1_000_000_000;end_ns=begin_ns+measured*1_000_000_000
    publisher_yaml_sha256=yaml(trial/"publisher.yaml",True,hz,warmup,measured,cooldown,publisher_status_path=pub_status,measure_begin_ns=begin_ns,measure_end_ns=end_ns)
    subscriber_yaml_sha256=yaml(trial/"subscriber.yaml",False,hz,warmup,measured,cooldown,lat,status,measure_begin_ns=begin_ns,measure_end_ns=end_ns)
    logs={n:open(trial/f"{n}.log","w") for n in (("roudi","subscriber","publisher") if BROKER_BINARY else ("subscriber","publisher"))}; prefix=lambda cpu:["taskset","-c",str(cpu)]
    base=clean_env({"IBMW_CORE_METRICS":"off"}); ps=[]
    try:
        r=None
        if BROKER_BINARY:r=subprocess.Popen(prefix(CPUS[0])+[str(build_dir/BROKER_BINARY)],cwd=build_dir,env=base,stdout=logs["roudi"],stderr=subprocess.STDOUT);ps=[r];time.sleep(1)
        s=subprocess.Popen(prefix(CPUS[1])+[str(subscriber_build_dir/E2E_SUBSCRIBER_APP),str(trial/"subscriber.yaml")],cwd=subscriber_build_dir,env=base,stdout=logs["subscriber"],stderr=subprocess.STDOUT);ps.insert(0,s)
        if not wait_ready(trial/"subscriber.log","PERF_SUBSCRIBER_RUNNING",s):raise InvalidTrial("subscriber not running")
        subscriber_ready_ns=time.monotonic_ns()
        env=clean_env({"IBMW_CORE_METRICS":"counters" if mode=="counters" else "off","IBMW_OBSERVABILITY_OUTPUT_DIR":str(metrics)})
        p=subprocess.Popen(prefix(CPUS[2])+[str(build_dir/E2E_PUBLISHER_APP),str(trial/"publisher.yaml")],cwd=build_dir,env=env,stdout=logs["publisher"],stderr=subprocess.STDOUT);ps.insert(0,p)
        if not wait_ready(trial/"publisher.log","PERF_PUBLISHER_RUNNING",p):raise InvalidTrial("publisher not running")
        publisher_ready_ns=time.monotonic_ns();send_begin_ns=begin_ns-warmup*1_000_000_000
        if max(subscriber_ready_ns,publisher_ready_ns)>=send_begin_ns:raise InvalidTrial("readiness missed fixed publish start")
        procs={"publisher":p,"subscriber":s,"roudi":r} if r else {"publisher":p,"subscriber":s}; affin={k:thread_affinity(v.pid,CPUS[i]) for i,(k,v) in enumerate((("roudi",r),("subscriber",s),("publisher",p))) if v}
        if not all(x[0] for x in affin.values()):raise InvalidTrial(f"critical thread affinity escaped: {affin}")
        time.sleep(max(0,(begin_ns-time.monotonic_ns())/1_000_000_000));start_sample_ns=time.monotonic_ns();start={k:proc_ticks(v.pid) for k,v in procs.items()}
        if any(v is None for v in start.values()):raise InvalidTrial("initial proc ticks unreadable")
        if TRIAL_OBSERVER:TRIAL_OBSERVER.window_begin({k:v.pid for k,v in procs.items()})
        totals=[];invalid_pss=0
        while time.monotonic_ns()<end_ns and all(v.poll() is None for v in procs.values()):
            stamp=[]
            for k,v in procs.items():
                x=pss_kib(v.pid);stamp.append(x)
            if all(x is not None for x in stamp):totals.append(sum(stamp))
            else:invalid_pss+=1
            if TRIAL_OBSERVER:TRIAL_OBSERVER.window_sample()
            time.sleep(min(1,max(0,(end_ns-time.monotonic_ns())/1_000_000_000)))
        # Freeze the measured wall-clock window with the sampled counters;
        # cooldown and natural shutdown are deliberately outside denominators.
        end_sample_ns=time.monotonic_ns();measured_end={k:proc_ticks(v.pid) for k,v in procs.items()}
        if any(v is None for v in measured_end.values()):raise InvalidTrial("endpoint proc ticks unreadable")
        if TRIAL_OBSERVER:TRIAL_OBSERVER.window_end()
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
        terminate(ps,True);ps=[]
        if not totals:raise InvalidTrial(f"no valid simultaneous {'three' if BROKER_BINARY else 'two'}-process PSS samples")
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
        row=rows[-1]; result={"mode":mode,"hz":hz,"measured_sec":elapsed,"unique":st["unique_measured"],"throughput":st["unique_measured"]/elapsed,"p50_us":float(row["p50_us"]),"p99_us":float(row["p99_us"]),"cpu_cores":sum(cpu_by.values()),"cpu_by_process":cpu_by,"cpu_sample_elapsed_sec":cpu_elapsed,"bottleneck_utilization":max(cpu_by.values()),"pss_median_kib":statistics.median(totals),"pss_peak_kib":max(totals),"pss_valid_samples":len(totals),"pss_invalid_samples":invalid_pss,"correctness":st,"publisher_correctness":pst,"reconciliation":reconciliation,"counter_conservation":conservation,"thread_affinity":affin,"delivery_profile":LOSSLESS_PROFILE,"publisher_yaml_sha256":publisher_yaml_sha256,"subscriber_yaml_sha256":subscriber_yaml_sha256,"measure_begin_monotonic_ns":begin_ns,"measure_end_monotonic_ns":end_ns,"subscriber_ready_ns":subscriber_ready_ns,"publisher_ready_ns":publisher_ready_ns,"cpu_sample_begin_ns":start_sample_ns,"cpu_sample_end_ns":end_sample_ns}
        if TRIAL_OBSERVER:result["interference"]=TRIAL_OBSERVER.evidence()
        result["valid"]=reconciliation["valid"]
        result["quality_valid"]=reconciliation["valid"] and (not quality or .65<=result["bottleneck_utilization"]<=.90)
        base={key:value for key,value in result.items() if key not in ("unique","throughput","p50_us","p99_us","correctness","publisher_correctness","reconciliation","counter_conservation","publisher_yaml_sha256","subscriber_yaml_sha256","valid","quality_valid")}
        write_json(trial/"observations.json",{"base":base,"quality_required":quality})
        result=recompute_e2e_result(trial);write_json(trial/"result.json",result)
        for stream in logs.values():stream.flush();os.fsync(stream.fileno())
        _seal_artifacts(trial,mode);return result
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
    lines=Path(path).read_bytes().splitlines()
    if len(lines)!=1:raise ValueError("Reporter metrics cardinality mismatch")
    item=strict_json_bytes(lines[0],"Reporter metrics");keys={"schema","snapshot_seq","kind","metric","topic","type","driver","value","cumulative","complete"}
    if not isinstance(item,dict) or set(item)!=keys or item.get("schema")!="ibmw.core_metrics.v1" or item.get("snapshot_seq")!=1 or item.get("kind")!=DIRECT_COUNTER_KIND or item.get("metric")!="hot_hook" or item.get("topic")!="" or item.get("type")!="" or item.get("driver")!="" or item.get("complete") is not True:raise ValueError("invalid Reporter metrics record")
    value=_nonnegative_integer(item["value"],"hot_hook");expected={"metric":"hot_hook","topic":"","type":"","driver":"","value":value}
    if item.get("cumulative")!=expected:raise ValueError("Reporter cumulative counter mismatch")
    return [value]
def recompute_micro_result(trial,mode):
    trial=Path(trial);result=strict_json_bytes((trial/"bench-output.json").read_bytes(),"micro output")
    if not isinstance(result,dict) or set(result)!={"iterations","elapsed_ns","ns_per_op","reporter_bound"} or result["iterations"]!=20000000 or not _number(result["ns_per_op"]) or not isinstance(result["reporter_bound"],bool):raise ValueError("invalid micro benchmark output")
    artifact=trial/"metrics"/"metrics.jsonl";expected=22000000
    if mode=="counters":
        values=reporter_hot_value(artifact) if artifact.exists() else []
        if values!=[expected]:raise InvalidTrial(f"Reporter hot-hook conservation failed: {values}")
        result.update(expected_reporter_final=expected,observed_reporter_final=values[0],reporter_final_valid=True,metrics_sha256=sha(artifact))
    else:
        if artifact.exists():raise InvalidTrial(f"{mode} emitted Reporter artifact")
        result.update(expected_reporter_final=0,observed_reporter_final=0,reporter_final_valid=True,metrics_sha256=None)
    if result["reporter_bound"]!=(mode=="counters"):raise InvalidTrial(f"{mode} Reporter binding mismatch")
    result["mode"]=mode
    return result
def run_micro(mode,build_dir,trial):
    if trial.exists():shutil.rmtree(trial)
    trial.mkdir(parents=True);metrics=trial/"metrics";metrics.mkdir()
    env=clean_env({"IBMW_CORE_METRICS":"counters" if mode=="counters" else "off"})
    env["IBMW_OBSERVABILITY_OUTPUT_DIR"]=str(metrics)
    if TRIAL_OBSERVER:TRIAL_OBSERVER.micro_begin()
    cp=subprocess.run(["taskset","-c",str(CPUS[2]),str(build_dir/"core_metrics_reporter_hot_bench")],cwd=build_dir,env=env,text=True,capture_output=True,check=True)
    if TRIAL_OBSERVER:TRIAL_OBSERVER.micro_end()
    raw=strict_json_bytes(cp.stdout.splitlines()[-1].encode(),"micro output");write_json(trial/"bench-output.json",raw)
    result=recompute_micro_result(trial,mode);write_json(trial/"result.json",result);_seal_artifacts(trial,mode,True)
    return result
def run_micro_lane(out,builds,micro_orders):
    micro=[]
    with (out/"raw-micro.jsonl").open("w") as f:
        for block,order in enumerate(micro_orders):
            for pos,mode in enumerate(order):
                r=run_micro(mode,builds["out" if mode=="out" else "instrumented"],out/"micro"/f"block-{block:02d}-{pos}-{mode}");r.update(block=block,position=pos,mode=mode);micro.append(r);f.write(json.dumps(r)+"\n");f.flush()
    pairs,cmp,verdict=comparisons(micro,(("ns_per_op","micro"),),False)
    return micro,pairs,cmp,verdict if len(pairs)==30 else "INCONCLUSIVE"
def _trial_key(record):
    try:return (int(record["block"]),int(record["position"]),record["mode"])
    except (KeyError,TypeError,ValueError):raise ValueError("invalid trial record key")
def _read_jsonl(path):
    if not path.is_file():return []
    records=[]
    for line in path.read_text().splitlines():
        if not line.strip():continue
        value=json.loads(line)
        if not isinstance(value,dict):raise ValueError(f"invalid JSONL record: {path}")
        records.append(value)
    return records
def _number(value):
    return isinstance(value,(int,float)) and not isinstance(value,bool) and math.isfinite(value)
def _validate_trial_payload(value,mode,micro):
    if not isinstance(value,dict) or value.get("mode")!=mode:
        raise ValueError(f"trial result mismatch: expected mode {mode}")
    required=("reporter_final_valid","ns_per_op") if micro else ("valid","correctness","reconciliation","publisher_correctness","quality_valid","throughput","p50_us","p99_us","cpu_cores","pss_median_kib","bottleneck_utilization")
    missing=[key for key in required if key not in value]
    if missing:raise ValueError(f"trial result missing fields: {','.join(missing)}")
    if micro:
        if not isinstance(value.get("reporter_final_valid"),bool) or not _number(value["ns_per_op"]):raise ValueError("invalid micro trial result")
    else:
        if not isinstance(value.get("valid"),bool):raise ValueError("invalid trial validity field")
        for key in ("correctness","reconciliation","publisher_correctness"):
            nested=value.get(key)
            if not isinstance(nested,dict) or not isinstance(nested.get("valid"),bool):raise ValueError(f"invalid {key}")
        if not isinstance(value["quality_valid"],bool):raise ValueError("invalid quality_valid")
        validity=(value["valid"],value["correctness"]["valid"],value["reconciliation"]["valid"],value["publisher_correctness"]["valid"])
        if value["quality_valid"] and not all(validity):raise ValueError("quality_valid contradicts correctness validity")
        for key in ("throughput","p50_us","p99_us","cpu_cores","pss_median_kib","bottleneck_utilization"):
            if not _number(value[key]):raise ValueError(f"invalid numeric field: {key}")
        evidence_keys=("delivery_profile","publisher_yaml_sha256","subscriber_yaml_sha256")
        present=[key in value for key in evidence_keys]
        if any(present) and not all(present):raise ValueError("partial trial delivery evidence")
        if all(present):
            if value["delivery_profile"]!=LOSSLESS_PROFILE:raise ValueError("trial delivery profile mismatch")
            for key in evidence_keys[1:]:
                if not isinstance(value[key],str) or len(value[key])!=64:raise ValueError(f"invalid YAML digest: {key}")
    return value
def _validate_registered_e2e(value,load,measured=60):
    if value.get("hz")!=load:raise ValueError("trial load does not match frozen calibration")
    if value.get("measured_sec")!=measured:raise ValueError("trial duration does not match registered window")
    begin=value.get("measure_begin_monotonic_ns");end=value.get("measure_end_monotonic_ns")
    if not isinstance(begin,int) or not isinstance(end,int) or end-begin!=measured*1_000_000_000:
        raise ValueError("trial boundaries do not match registered window")
    return value
def _load_trial_result(path,block,position,mode):
    path=Path(path);micro="micro" in path.parts
    value=_load_sealed_result(path.parent,mode,micro)
    if value is None:return None
    _validate_trial_payload(value,mode,"micro" in path.parts)
    value.update(block=block,position=position)
    value["mode"]=mode
    return value
def _check_raw_records(records,path,expected,micro):
    seen={};duplicates=0
    for record in records:
        key=_trial_key(record)
        if key not in expected:raise ValueError(f"{path.name} record does not match preregistered plan")
        _validate_trial_payload(record,key[2],micro)
        if key in seen:
            if record!=seen[key]:raise ValueError(f"conflicting duplicate {path.name} record: {key}")
            duplicates+=1
        else:seen[key]=record
    return seen,duplicates
def _compare_raw_result(raw,result,path,key):
    if raw is not None and raw!=result:raise ValueError(f"{path.name} does not match trial result: {key}")
def _resume_trials(out,builds,orders,micro_orders,e2e_only=False):
    """Resume only sealed trials and reconstruct JSONL from sealed results."""
    e2e=[]; e2e_path=out/"raw-e2e.jsonl";load=json.loads((out/"calibration.json").read_text())["frozen_load"]
    e2e_file=e2e_path.open("w")
    try:
        for block,order in enumerate(orders):
            for position,mode in enumerate(order):
                key=(block,position,mode)
                result=_load_trial_result(out/"trials"/f"block-{block:02d}-{position}-{mode}"/"result.json",block,position,mode)
                if result is None:
                    result=run_trial(mode,builds["out" if mode=="out" else "instrumented"],out/"trials"/f"block-{block:02d}-{position}-{mode}",load,60)
                result.update(block=block,position=position,mode=mode)
                _validate_trial_payload(result,mode,False);_validate_registered_e2e(result,load)
                e2e.append(result)
                e2e_file.write(json.dumps(result,sort_keys=True,separators=(",",":"))+"\n");e2e_file.flush()
    finally:e2e_file.close()
    if e2e_only:
        global _RESUME_DUPLICATE_COUNT
        _RESUME_DUPLICATE_COUNT=0
        return e2e,[]
    expected_micro={(block,position,mode) for block,order in enumerate(micro_orders) for position,mode in enumerate(order)}
    micro=[];micro_path=out/"raw-micro.jsonl";micro_file=micro_path.open("w")
    try:
        for block,order in enumerate(micro_orders):
            for position,mode in enumerate(order):
                key=(block,position,mode)
                result_path=out/"micro"/f"block-{block:02d}-{position}-{mode}"/"result.json"
                result=_load_trial_result(result_path,block,position,mode)
                if result is None:
                    result=run_micro(mode,builds["out" if mode=="out" else "instrumented"],out/"micro"/f"block-{block:02d}-{position}-{mode}")
                result.update(block=block,position=position,mode=mode);micro.append(result)
                _validate_trial_payload(result,mode,True)
                micro_file.write(json.dumps(result,sort_keys=True,separators=(",",":"))+"\n");micro_file.flush()
    finally:micro_file.close()
    _RESUME_DUPLICATE_COUNT=0
    return e2e,micro
def e2e_only_summary(e2e,load):
    pairs,cmp,verdict=comparisons(e2e,(('cpu_cores','cpu'),('throughput','throughput'),('p50_us','p50'),('p99_us','p99')),True) if e2e else ([],{},"INCONCLUSIVE")
    complete=len(e2e)==72 and len(pairs)==24
    quality=complete and all(r.get("quality_valid") is True for r in e2e)
    if not quality:verdict="INCONCLUSIVE"
    means={}
    if complete:
        means={mode:{key:statistics.mean(r[key] for r in e2e if r["mode"]==mode) for key in ("throughput","p50_us","p99_us","cpu_cores","pss_median_kib","bottleneck_utilization")} for mode in MODES}
    return {"schema":E2E_SUMMARY_SCHEMA,"scope":"e2e-only","protocol":PROTOCOL,"purpose":f"formal {PROTOCOL_LABEL} E2E gate rerun","verdict":verdict,"e2e_verdict":verdict,"micro_verdict":"NOT_APPLICABLE","quality_valid":quality,"complete_e2e":complete,"complete_micro":False,"e2e_count":len(e2e),"micro_count":0,"micro_trial_count":0,"duplicate_count":globals().get("_RESUME_DUPLICATE_COUNT",0),"frozen_load":load,"yaml_sha256":sorted({r[k] for r in e2e for k in ("publisher_yaml_sha256","subscriber_yaml_sha256") if k in r}),"e2e_comparisons":cmp,"micro_comparisons":{},"mode_means":means}
def lossless_v2_summary(e2e,micro,load):
    epairs,ecmp,everdict=comparisons(e2e,(('cpu_cores','cpu'),('throughput','throughput'),('p50_us','p50'),('p99_us','p99')),True) if e2e else ([],{},"INCONCLUSIVE")
    mpairs,mcmp,mverdict=comparisons(micro,(('ns_per_op','micro'),),False) if micro else ([],{},"INCONCLUSIVE")
    complete_e2e=len(e2e)==72 and len(epairs)==24
    # Each of the 30 preregistered micro blocks has one trial per mode.
    complete_micro=len(micro)==90 and len(mpairs)==30
    quality=complete_e2e and all(r.get("quality_valid") is True for r in e2e)
    if not complete_e2e or not quality:everdict="INCONCLUSIVE"
    if not complete_micro or not all(r.get("reporter_final_valid") is True for r in micro):mverdict="INCONCLUSIVE"
    final=aggregate_overall(everdict,mverdict)
    means={}
    if complete_e2e:
        means={mode:{key:statistics.mean(r[key] for r in e2e if r["mode"]==mode) for key in ("throughput","p50_us","p99_us","cpu_cores","pss_median_kib","bottleneck_utilization")} for mode in MODES}
    summary={"verdict":final,"e2e_verdict":everdict,"micro_verdict":mverdict,"quality_valid":quality,"complete_e2e":complete_e2e,"complete_micro":complete_micro,"e2e_count":len(e2e),"micro_count":len(mpairs),"micro_trial_count":len(micro),"duplicate_count":globals().get("_RESUME_DUPLICATE_COUNT",0),"frozen_load":load,"yaml_sha256":sorted({r[k] for r in e2e for k in ("publisher_yaml_sha256","subscriber_yaml_sha256") if k in r}),"e2e_comparisons":ecmp,"micro_comparisons":mcmp,"mode_means":means}
    return summary
def summary_label(summary):
    if summary.get("scope")=="e2e-only":return "# Core Metrics lossless v2 E2E: "+summary["verdict"]
    return "# Core Metrics lossless v2: "+summary["verdict"]
def write_summary(out,summary):
    out=Path(out);summary=dict(summary);_,contract_sha=protocol_contract(out/SOURCE_DIRECTORY/PROTOCOL_CONTRACT_ARCHIVED if (out/SOURCE_DIRECTORY).is_dir() else None);summary.update(delivery_profile=LOSSLESS_PROFILE,protocol=PROTOCOL,protocol_contract_sha256=contract_sha,frozen_load=FROZEN_LOAD,batch=BATCH_SIZE)
    if (out/"plan.json").is_file():summary["plan_sha256"]=sha(out/"plan.json")
    write_json(out/"summary.json",summary)
    (out/"summary.md").write_text(summary_label(summary)+"\n\n```json\n"+json.dumps(summary,indent=2)+"\n```\n")
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
def registered_plan(contract_sha=None):
    if contract_sha is None:_,contract_sha=protocol_contract()
    orders=list(itertools.permutations(MODES))*4;random.Random(SEED).shuffle(orders)
    micro_orders=list(itertools.permutations(MODES))*5;random.Random(SEED).shuffle(micro_orders)
    return {"schema":PLAN_SCHEMA,"protocol":PROTOCOL,"protocol_contract_sha256":contract_sha,"seed":SEED,"cpus":list(CPUS),"startup_guard_sec":STARTUP_GUARD_SEC,"window_clock":"CLOCK_MONOTONIC shared [begin,end)","orders":[list(x) for x in orders],"micro":{"scope":"actual Reporter query/stable binding/null branch/atomic ABI indirect add; live LoanSerializedSample IPC remains E2E","warmup_ops":2000000,"measured_ops":20000000,"blocks":30,"orders":[list(x) for x in micro_orders]},"capacity":{"probes":3,"probe_sec":15,"confirmation_sec":20,"confirmation_range":[.70,.85],"formal_range":[.65,.90],"candidates":list(CAPACITY_CANDIDATES),"candidate_source":f"fixed before {PROTOCOL_LABEL} from separate batch-size and pacing-window design smokes","pacing_policy":"absolute batch schedule; batch 128; skip missed slots; no schedule rebase; no catch-up burst","unpaced_policy":"saturation_diagnostic_only"}}
def execution_scope(out,expected=None,create=False,dependency_binding=None):
    path=Path(out)/"execution-scope.json"
    if create:
        value={"schema":"ibmw-core-metrics-execution-scope-v1","scope":expected}
        if dependency_binding:value={"schema":"ibmw-core-metrics-execution-scope-v2","scope":expected,"dependency_snapshot":{key:dependency_binding[key] for key in ("manifest_sha256","aggregate_sha256")}}
        write_json(path,value)
    elif not path.is_file():
        if expected=="full":return {"scope":"full","legacy_unbound":True}
        raise ValueError("missing execution scope evidence")
    value=json.loads(path.read_text())
    wanted={"schema":"ibmw-core-metrics-execution-scope-v1","scope":expected}
    if dependency_binding:wanted={"schema":"ibmw-core-metrics-execution-scope-v2","scope":expected,"dependency_snapshot":{key:dependency_binding[key] for key in ("manifest_sha256","aggregate_sha256")}}
    if value!=wanted:
        raise ValueError("execution scope mismatch")
    return {**value,"sha256":sha(path)}
def execution_binding(out,dependency_binding=None,create=False):
    out=Path(out);path=out/"execution-binding.json";_,contract_sha=protocol_contract(out/SOURCE_DIRECTORY/PROTOCOL_CONTRACT_ARCHIVED if (out/SOURCE_DIRECTORY).is_dir() else None)
    expected={"schema":"ibmw-core-metrics-execution-binding-v1","protocol":PROTOCOL,"protocol_contract_sha256":contract_sha,"dependency_snapshot":{key:dependency_binding[key] for key in ("manifest_sha256","aggregate_sha256")} if dependency_binding else None,"sandbox":{"program":"/usr/bin/bwrap","network":"unshare-net","root":"read-only","writable":"build-directory-only","tmp":"tmpfs","die_with_parent":True,"new_session":True}}
    if create:write_json(path,expected)
    if not path.is_file() or strict_json_bytes(path.read_bytes(),"execution binding")!=expected:raise ValueError("execution binding mismatch")
    return {**expected,"sha256":sha(path)}
def provenance(root,out,builds,topo,source_binding,dependency_binding,git,scope="full"):
    commands=(("lscpu",),("uname","-a"),("bash","-lc","for f in /sys/devices/system/cpu/cpu{25,26,27}/cpufreq/scaling_governor /sys/devices/system/cpu/intel_pstate/no_turbo /sys/devices/system/cpu/cpufreq/boost; do [ -r \"$f\" ] && printf '%s=%s\\n' \"$f\" \"$(cat \"$f\")\"; done"))
    records=[]
    for cmd in commands:
        p=subprocess.run(cmd,text=True,capture_output=True);records.append({"command":list(cmd),"returncode":p.returncode,"stdout":p.stdout,"stderr":p.stderr})
    binding_record=execution_binding(out,dependency_binding)
    prov={"schema":"ibmw-core-metrics-provenance-v3","protocol":PROTOCOL,"plan_sha256":sha(out/"plan.json"),
          "source_manifest_sha256":source_binding["manifest_sha256"],
          "source_state_sha256":source_binding["source_state_sha256"],
          "source_snapshot_sha256":source_binding["snapshot_sha256"],
          "dependency_manifest_sha256":dependency_binding["manifest_sha256"],
          "dependency_aggregate_sha256":dependency_binding["aggregate_sha256"],
          "cmake_source":SOURCE_DIRECTORY,"git_context":git,"topology":topo,
          "host_records":records,"registered_build":registered_build_configuration(out),
          "execution_binding_sha256":binding_record["sha256"],"builds":collect_build_artifacts(out,builds)}
    scope_record=execution_scope(out,scope,dependency_binding=dependency_binding)
    if not scope_record.get("legacy_unbound"):prov["execution_scope_sha256"]=scope_record["sha256"]
    write_json(out/"provenance.json",prov)
    return prov
def _verify_plan(out):
    plan=out/"plan.json";sidecar=out/"plan.sha256"
    if not plan.is_file() or not sidecar.is_file():raise FileNotFoundError("missing preregistered plan evidence")
    digest=sha(plan)
    if sidecar.read_text()!=digest+"  plan.json\n":raise ValueError("plan digest sidecar mismatch")
    value=strict_json_bytes(plan.read_bytes(),"plan");archived=out/SOURCE_DIRECTORY/PROTOCOL_CONTRACT_ARCHIVED;_,contract_sha=protocol_contract(archived if archived.is_file() else None)
    if value!=registered_plan(contract_sha):raise ValueError("preregistered plan semantics mismatch")
    return digest
def verify_provenance(out,topo=None,scope="full"):
    out=Path(out);path=out/"provenance.json"
    if not path.is_file():raise FileNotFoundError("missing provenance.json")
    prov=json.loads(path.read_text())
    if prov.get("schema")!="ibmw-core-metrics-provenance-v3" or prov.get("protocol")!=PROTOCOL:raise ValueError("unsupported provenance")
    if prov.get("plan_sha256")!=_verify_plan(out):raise ValueError("provenance plan binding mismatch")
    binding=verify_source_snapshot(out,prov.get("source_manifest_sha256"),prov.get("source_snapshot_sha256"))
    if binding["source_state_sha256"]!=prov.get("source_state_sha256"):raise ValueError("provenance source state mismatch")
    dependency_binding=verify_dependency_snapshot(out,prov.get("dependency_manifest_sha256"),prov.get("dependency_aggregate_sha256"))
    if prov.get("execution_binding_sha256")!=execution_binding(out,dependency_binding)["sha256"]:raise ValueError("provenance execution binding mismatch")
    if prov.get("cmake_source")!=SOURCE_DIRECTORY:raise ValueError("provenance CMake source mismatch")
    scope_record=execution_scope(out,scope,dependency_binding=dependency_binding)
    if not scope_record.get("legacy_unbound") and prov.get("execution_scope_sha256")!=scope_record["sha256"]:raise ValueError("provenance execution scope mismatch")
    if scope_record.get("legacy_unbound") and prov.get("execution_scope_sha256") is not None:raise ValueError("missing bound execution scope artifact")
    if topo is not None and prov.get("topology")!=topo:raise ValueError("provenance topology mismatch")
    if not prov.get("git_context",{}).get("records") or not prov.get("host_records"):raise ValueError("incomplete provenance context")
    builds={"out":out/"build-out","instrumented":out/"build-instrumented"}
    verify_registered_builds(out,prov.get("registered_build"))
    expected=collect_build_artifacts(out,builds)
    if prov.get("builds")!=expected:raise ValueError("build artifact provenance mismatch")
    return builds
def verify_resume_evidence(out,topo=None,scope="full"):
    """Verify archived resume evidence without hashing the current source tree."""
    out=Path(out);path=out/"provenance.json"
    if not path.is_file():raise FileNotFoundError("missing provenance.json")
    prov=json.loads(path.read_text())
    if prov.get("schema")!="ibmw-core-metrics-provenance-v3" or prov.get("protocol")!=PROTOCOL:raise ValueError("unsupported provenance")
    if prov.get("plan_sha256")!=_verify_plan(out):raise ValueError("provenance plan binding mismatch")
    binding=verify_source_snapshot(out,prov.get("source_manifest_sha256"),prov.get("source_snapshot_sha256"))
    if binding["source_state_sha256"]!=prov.get("source_state_sha256"):raise ValueError("provenance source state mismatch")
    dependency_binding=verify_dependency_snapshot(out,prov.get("dependency_manifest_sha256"),prov.get("dependency_aggregate_sha256"))
    if prov.get("execution_binding_sha256")!=execution_binding(out,dependency_binding)["sha256"]:raise ValueError("provenance execution binding mismatch")
    if prov.get("cmake_source")!=SOURCE_DIRECTORY:raise ValueError("provenance CMake source mismatch")
    scope_record=execution_scope(out,scope,dependency_binding=dependency_binding)
    if not scope_record.get("legacy_unbound") and prov.get("execution_scope_sha256")!=scope_record["sha256"]:raise ValueError("provenance execution scope mismatch")
    if scope_record.get("legacy_unbound") and prov.get("execution_scope_sha256") is not None:raise ValueError("missing bound execution scope artifact")
    if topo is not None and prov.get("topology")!=topo:raise ValueError("provenance topology mismatch")
    if not prov.get("git_context",{}).get("records") or not prov.get("host_records"):raise ValueError("incomplete provenance context")
    builds={"out":out/"build-out","instrumented":out/"build-instrumented"}
    verify_registered_builds(out,prov.get("registered_build"),require_current=False)
    if prov.get("builds")!=collect_build_artifacts(out,builds):raise ValueError("build artifact provenance mismatch")
    return builds
def _main():
    ap=argparse.ArgumentParser();ap.add_argument("--output",type=Path,required=True);ap.add_argument("--jobs",type=int,default=max(1,(os.cpu_count() or 2)//2));ap.add_argument("--dependency-source-pool",type=Path);ap.add_argument("--skip-build",action="store_true");ap.add_argument("--resume",action="store_true");ap.add_argument("--e2e-only",action="store_true");ap.add_argument("--diagnostic-conservation",action="store_true",help="run three short conservation trials; never produces an acceptance verdict");a=ap.parse_args()
    if a.resume and not a.skip_build:ap.error("--resume requires --skip-build")
    if a.diagnostic_conservation and (a.resume or a.e2e_only):ap.error("diagnostic conservation is a separate execution mode")
    root=Path(__file__).resolve().parents[1];out=a.output.resolve();scope="diagnostic-conservation" if a.diagnostic_conservation else ("e2e-only" if a.e2e_only else "full")
    if out==root or any(out==root/name or root/name in out.parents for name in ("cmake","src","scripts")):
        raise SystemExit("evidence output must be outside snapshotted source inputs")
    if a.skip_build:
        if not out.is_dir():raise SystemExit("--skip-build requires pre-existing evidence")
        topo=topology()
        try:builds=verify_resume_evidence(out,topo,scope) if a.resume else verify_provenance(out,topo,scope)
        except (OSError,ValueError,json.JSONDecodeError,tarfile.TarError,KeyError,TypeError) as e:raise SystemExit(f"--skip-build evidence verification failed: {e}")
        plan=json.loads((out/"plan.json").read_text());orders=[tuple(x) for x in plan["orders"]];micro_orders=[tuple(x) for x in plan["micro"]["orders"]]
    else:
        if out.exists() and any(out.iterdir()):raise SystemExit("refusing to modify non-empty evidence directory")
        git=git_context(root);out.mkdir(parents=True,exist_ok=True)
        pool=(a.dependency_source_pool or root/"build-core-metrics"/"_deps").resolve()
        dependency_binding=create_dependency_snapshot(root,pool,out)
        plan=registered_plan();orders=[tuple(x) for x in plan["orders"]];micro_orders=[tuple(x) for x in plan["micro"]["orders"]]
        write_json(out/"plan.json",plan);(out/"plan.sha256").write_text(sha(out/"plan.json")+"  plan.json\n")
        source_binding=create_source_snapshot(root,out)
        dependency_binding=verify_dependency_snapshot(out,dependency_binding["manifest_sha256"],dependency_binding["aggregate_sha256"])
        execution_scope(out,scope,True,dependency_binding)
        execution_binding(out,dependency_binding,True)
        builds=build(out/SOURCE_DIRECTORY,out,a.jobs)
        source_binding=verify_source_snapshot(out,source_binding["manifest_sha256"],source_binding["snapshot_sha256"])
        topo=topology();provenance(root,out,builds,topo,source_binding,dependency_binding,git,scope);builds=verify_provenance(out,topo,scope)
    if not topo["valid"]:
        summary={"verdict":"INCONCLUSIVE","reason":"fixed affinity validation failed","topology":topo};write_json(out/"summary.json",summary);print(json.dumps(summary,indent=2));return verdict_exit_code(summary["verdict"])
    if a.diagnostic_conservation:
        results=[]
        with (out/"raw-diagnostic-conservation.jsonl").open("w") as raw:
            for position,mode in enumerate(MODES):
                result=run_trial(mode,builds["out" if mode=="out" else "instrumented"],out/"diagnostic"/f"{position}-{mode}",FROZEN_LOAD,5,False,2,1)
                result.update(position=position,diagnostic_only=True,acceptance_eligible=False);results.append(result);raw.write(json.dumps(result)+"\n");raw.flush()
        summary={"schema":DIAGNOSTIC_SCHEMA,"protocol":PROTOCOL,"protocol_contract_sha256":plan["protocol_contract_sha256"],"verdict":"DIAGNOSTIC_ONLY","diagnostic_only":True,"acceptance_eligible":False,"trial_count":len(results),"all_conserved":all(r["reconciliation"]["valid"] for r in results),"yaml_sha256":sorted({r[k] for r in results for k in ("publisher_yaml_sha256","subscriber_yaml_sha256")})}
        write_summary(out,summary);print(json.dumps(summary,indent=2));return 0 if summary["all_conserved"] else 1
    if a.resume:
        try:
            calibration=json.loads((out/"calibration.json").read_text())
            if calibration.get("protocol")!=PROTOCOL or calibration.get("protocol_contract_sha256")!=plan["protocol_contract_sha256"]:raise ValueError("resume calibration protocol binding mismatch")
            load=calibration.get("frozen_load")
            if load is not None and load!=FROZEN_LOAD:raise ValueError(f"resume frozen load differs from {FROZEN_LOAD}")
            if load is None and a.e2e_only:
                summary=e2e_only_summary([],load);summary.update(reason=f"{PROTOCOL_LABEL} paced confirmation failed",calibration=calibration)
                write_summary(out,summary);print(json.dumps(summary,indent=2));return verdict_exit_code(summary["verdict"])
            if load is None:raise ValueError("resume requires a valid frozen calibration")
            for mode in MODES:
                priming=_load_trial_result(out/"priming"/mode/"result.json",0,0,mode)
                if priming is None:raise ValueError(f"missing priming evidence: {mode}")
                _validate_registered_e2e(priming,load,5)
            e2e,micro=_resume_trials(out,builds,orders,micro_orders,a.e2e_only)
            summary=e2e_only_summary(e2e,load) if a.e2e_only else lossless_v2_summary(e2e,micro,load)
            write_summary(out,summary);print(json.dumps(summary,indent=2));return verdict_exit_code(summary["verdict"])
        except (OSError,ValueError,json.JSONDecodeError,KeyError,TypeError) as e:
            raise SystemExit(f"--resume evidence verification failed: {e}")
    probes=[]
    for i in range(3):
        try: probe=run_trial("out",builds["out"],out/"calibration"/f"capacity-{i}",0,15,False,2,1)
        except InvalidTrial as e: probe={"trial":f"capacity-{i}","valid":False,"error":str(e)}
        probes.append(classify_unpaced_probe(probe))
    confirmations=[];load=None
    for candidate in (FROZEN_LOAD,):
        try:r=run_trial("out",builds["out"],out/"calibration"/f"confirm-{len(confirmations)}",candidate,20,False,3,1)
        except InvalidTrial as e:r={"valid":False,"error":str(e)}
        r["candidate"]=candidate;confirmations.append(r)
        if confirmation_is_strictly_valid(r):load=candidate;break
    calibration={"protocol":PROTOCOL,"protocol_contract_sha256":plan["protocol_contract_sha256"],"unpaced_diagnostics":probes,"candidate_confirmations":confirmations,"frozen_load":load,"frozen_load_source":f"{PROTOCOL_LABEL} trusted contract and paced confirmation" if load is not None else None};write_json(out/"calibration.json",calibration)
    if load is None:
        if a.e2e_only:
            summary=e2e_only_summary([],load);summary.update(reason=f"{PROTOCOL_LABEL} paced confirmation failed",calibration=calibration)
            write_summary(out,summary);print(json.dumps(summary,indent=2));return verdict_exit_code(summary["verdict"])
        micro,mpairs,mcmp,mverdict=run_micro_lane(out,builds,micro_orders)
        e2e_verdict="INCONCLUSIVE";final,exit_code=no_load_outcome(mverdict)
        summary={"verdict":final,"e2e_verdict":e2e_verdict,"e2e_reason":f"{PROTOCOL_LABEL} paced confirmation failed","micro_verdict":mverdict,"reason":f"{PROTOCOL_LABEL} paced confirmation failed","calibration":calibration,"e2e_comparisons":{},"micro_comparisons":mcmp,"complete_micro":len(mpairs)==30}
        write_summary(out,summary);print(json.dumps(summary,indent=2));return exit_code
    for mode in MODES:run_trial(mode,builds["out" if mode=="out" else "instrumented"],out/"priming"/mode,load,5,False,2,1)
    e2e=[]
    with (out/"raw-e2e.jsonl").open("w") as f:
        for block,order in enumerate(orders):
            for pos,mode in enumerate(order):
                r=run_trial(mode,builds["out" if mode=="out" else "instrumented"],out/"trials"/f"block-{block:02d}-{pos}-{mode}",load,60);r.update(block=block,position=pos);e2e.append(r);f.write(json.dumps(r)+"\n");f.flush()
    if a.e2e_only:
        summary=e2e_only_summary(e2e,load);write_summary(out,summary);print(json.dumps(summary,indent=2));return verdict_exit_code(summary["verdict"])
    micro,mpairs,mcmp,mverdict=run_micro_lane(out,builds,micro_orders)
    epairs,ecmp,everdict=comparisons(e2e,(("cpu_cores","cpu"),("throughput","throughput"),("p50_us","p50"),("p99_us","p99")),True)
    quality=len(epairs)==24 and all(r["quality_valid"] for r in e2e); complete_micro=len(mpairs)==30
    if not quality:everdict="INCONCLUSIVE"
    if not complete_micro:mverdict="INCONCLUSIVE"
    final=aggregate_overall(everdict,mverdict)
    summary={"verdict":final,"e2e_verdict":everdict,"micro_verdict":mverdict,"quality_valid":quality,"complete_micro":complete_micro,"frozen_load":load,"yaml_sha256":sorted({r[k] for r in e2e for k in ("publisher_yaml_sha256","subscriber_yaml_sha256")}),"e2e_comparisons":ecmp,"micro_comparisons":mcmp,"mode_means":{m:{k:statistics.mean(r[k] for r in e2e if r["mode"]==m) for k in ("throughput","p50_us","p99_us","cpu_cores","pss_median_kib","bottleneck_utilization")} for m in MODES}}
    write_summary(out,summary);print(json.dumps(summary,indent=2));return verdict_exit_code(final)
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
