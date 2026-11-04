#!/usr/bin/env python3
"""Core Metrics lossless E2E and Reporter hot-hook gate for the shm transport.

Runs the protocol machinery of core_metrics_perf.py (snapshots, sandboxed build,
preregistered plan, seals, bootstrap verdicts) on the shm_loaned_ctx lane: no
RouDi, libibmw_shm_transport.so, direct_shm_loan counters. The iceoryx module is
loaded as a private instance whose lane identity is rebound below, so importing
this file never changes what core_metrics_perf.py does when it runs itself.

CPU and PSS totals sum two processes here and three on the iceoryx gate, so the
gates' absolute totals are not comparable; verdicts compare modes within a gate.
"""
import argparse, importlib.util, os, shutil, signal, sys, tempfile
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
_trial_env={}

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
    return plan
def summary_label(summary):
    if summary.get("scope")=="e2e-only":return "# Core Metrics shm lossless v1 E2E: "+summary["verdict"]
    return "# Core Metrics shm lossless v1: "+summary["verdict"]

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
    check_output_outside_sources(sys.argv[1:] if argv is None else argv)
    signal.signal(signal.SIGTERM,_terminate_as_interrupt)
    return gate.main()

if __name__=="__main__":sys.exit(main())
