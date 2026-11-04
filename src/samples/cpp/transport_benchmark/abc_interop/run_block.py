#!/usr/bin/env python3
"""Run one isolated steady-state A, B, C, or S benchmark lane."""
import argparse, csv, hashlib, json, math, os, pathlib, re, shutil, signal, subprocess, sys, tempfile, time, traceback

CSV_SCHEMA = ("label", "count", "min_us", "avg_us", "stddev_us", "p50_us", "p90_us", "p99_us", "p999_us", "max_us")
LANE_LABELS = {"A": "abc_ros2_4KB", "B": "abc_dds_4KB", "C": "iox_send_4KB", "S": "shm_send_4KB"}
# Ordinary Send() lanes run the same iox_send sources: C on iceoryx with RouDi,
# S on the daemonless shm_ext. Both share the publisher status and SIGINT rules.
SEND_LANES = {"C": {"sample": "iox_send", "plugin": "libibmw_iceoryx_transport.so"},
              "S": {"sample": "shm_send", "plugin": "libibmw_shm_transport.so"}}
SHM_MAX_DOMAIN_LENGTH = 32
SHM_MAX_SERVICE_DIR_LENGTH = 63
ERROR_FIELDS = ("missing", "duplicate", "out_of_order", "invalid_payload", "invalid_timestamp")

def resolve_roudi(build, override=None, environ=None):
    """Resolve RouDi from CLI, environment, CMake metadata, or source build layout."""
    env = os.environ if environ is None else environ
    candidates = []
    if override: candidates.append(pathlib.Path(override))
    elif env.get("IBMW_ROUDI_BIN"): candidates.append(pathlib.Path(env["IBMW_ROUDI_BIN"]))
    else:
        metadata = pathlib.Path(build) / "ibmw_abc_roudi_path.txt"
        try:
            configured = metadata.read_text().splitlines()[0].strip()
            if configured: candidates.append(pathlib.Path(configured))
        except (OSError, IndexError):
            pass
        candidates.append(pathlib.Path(build) / "iox-roudi")
    for candidate in candidates:
        candidate = candidate.expanduser().resolve()
        if candidate.is_file() and os.access(candidate, os.X_OK): return candidate
    attempted = ", ".join(str(path) for path in candidates) or "no candidate"
    raise ValueError(f"iox-roudi executable not found ({attempted}); use --roudi or IBMW_ROUDI_BIN")

def required_processes(lane):
    """Processes that must exit cleanly; only C owns a RouDi."""
    return ("publisher","subscriber","roudi") if lane=="C" else ("publisher","subscriber")

def send_lane_commands(lane, build, subscriber_config, publisher_config, roudi_override=None):
    """Return the (name, executable, args) start order of a Send() lane."""
    sample=SEND_LANES[lane]["sample"]
    commands=[("subscriber",build/f"{sample}_subscriber_app",[str(subscriber_config)]),("publisher",build/f"{sample}_publisher_app",[str(publisher_config)])]
    return [("roudi",resolve_roudi(build,roudi_override),[])]+commands if lane=="C" else commands

def patch_send_lane_configs(lane, build, subscriber_config, publisher_config, csv_path, status_path, publisher_status_path):
    """Write temporary Send() lane configs from the lane's historical templates."""
    sample=SEND_LANES[lane]["sample"]; label=LANE_LABELS[lane]
    template=pathlib.Path(__file__).resolve().parents[1]/f"{sample}/config/linux/bin/cfg/{sample}"
    common={"topic":"/mw_perf/payload_4kb","payload_bytes":4096,"hz":100,"warmup_messages":50,"message_count":3000,"start_delay_ms":2000}
    global_config={"path":str((build/SEND_LANES[lane]["plugin"]).resolve()),"topic":"/mw_perf/payload_4kb"}
    patch_iox_config(template/"subscriber.yaml",subscriber_config,global_config,{**common,"label":label,"csv_path":csv_path,"status_path":status_path})
    patch_iox_config(template/"publisher.yaml",publisher_config,global_config,{**common,"label":label,"publisher_status_path":publisher_status_path})

def configure_shm_run(env, root, lane_domain):
    """Point env at a private shm root and a domain unique to this run."""
    domain=f"abcS{lane_domain}.{pathlib.Path(root).name.rsplit('.',1)[-1]}"
    service_dir=f"{root}/ibmw_shm.{os.geteuid()}.{domain}"
    if len(domain)>SHM_MAX_DOMAIN_LENGTH: raise ValueError(f"shm domain too long: {domain}")
    if len(service_dir)>SHM_MAX_SERVICE_DIR_LENGTH: raise ValueError(f"shm service directory too long: {service_dir}")
    env.pop("IBMW_SHM_GROUP",None); env["IBMW_SHM_ROOT"]=str(root); env["IBMW_SHM_DOMAIN"]=domain
    return domain

def stop_shm_run(procs, root, errors):
    """Stop live S endpoints, then remove the private root even if interrupted.

    All endpoints share one 5 s TERM budget so the whole stop fits well inside
    the collector's 10 s stop timeout.  A root that cannot be removed is an
    error: the run left state behind.
    """
    try:
        for proc in procs.values():
            if proc.poll() is None: proc.terminate()
        deadline=time.monotonic()+5
        for proc in procs.values():
            try: proc.wait(timeout=max(0,deadline-time.monotonic()))
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    finally:
        try: shutil.rmtree(root)
        except FileNotFoundError: pass
        except OSError as e: errors.append(f"shm_root_cleanup_failed: {e}")

class _StopFlag:
    """Stop request set from signal handlers.

    threading.Event must not be used here: its lock is not reentrant, and the
    collector's second TERM can re-enter the handler inside Event.set(), which
    deadlocks the main thread until the collector escalates to SIGKILL.
    """
    def __init__(self): self.requested=False
    def set(self): self.requested=True
    def is_set(self): return self.requested
    def wait(self,timeout):
        """Sleep up to timeout, returning early once a stop is requested."""
        deadline=time.monotonic()+timeout
        while not self.requested and time.monotonic()<deadline: time.sleep(min(.05,max(0,deadline-time.monotonic())))
        return self.requested

def validate_status_schema(status):
    errors = []
    if not isinstance(status, dict): return False, ["status_not_object"]
    attempted_key = "attempted_measured" if "attempted_measured" in status else None
    if attempted_key is None: errors.append("status_missing_attempted_measured_or_sent_measured")
    unique_key = "unique_measured" if "unique_measured" in status else None
    if unique_key is None: errors.append("status_missing_unique_measured")
    for key in ("valid", "received_measured", *ERROR_FIELDS):
        if key not in status: errors.append(f"status_missing_{key}")
    if "valid" in status and type(status["valid"]) is not bool: errors.append("status_valid_not_boolean")
    for key in ((attempted_key,) if attempted_key else ()) + ("received_measured",) + ((unique_key,) if unique_key else ()) + ERROR_FIELDS:
        if key in status and (type(status[key]) is not int or status[key] < 0): errors.append(f"status_{key}_not_nonnegative_integer")
    return not errors, errors

def validate_publisher_status_schema(status):
    """Validate the publisher-only completion artifact (no subscriber fields)."""
    errors = []
    if not isinstance(status, dict): return False, ["publisher_status_not_object"]
    for key in ("completion", "attempted_total", "attempted_measured", "valid"):
        if key not in status: errors.append(f"publisher_status_missing_{key}")
    for key in ("completion", "valid"):
        if key in status and type(status[key]) is not bool: errors.append(f"publisher_status_{key}_not_boolean")
    for key in ("attempted_total", "attempted_measured"):
        if key in status and (type(status[key]) is not int or status[key] < 0): errors.append(f"publisher_status_{key}_not_nonnegative_integer")
    return not errors, errors

def status_unique_measured(status):
    """Return the strict canonical unique count."""
    return status.get("unique_measured")

def clean_env():
    env = os.environ.copy(); env.pop("CONDA_PREFIX", None)
    inherited = [p for p in env.get("LD_LIBRARY_PATH", "").split(":")
                 if p and "conda" not in p.lower()]
    preferred = ["/usr/lib/x86_64-linux-gnu", "/opt/ros/humble/lib/x86_64-linux-gnu",
                 "/opt/ros/humble/lib"]
    env["LD_LIBRARY_PATH"] = ":".join(dict.fromkeys(preferred + inherited))
    return env

def patch_yaml(source, target, replacements):
    text = pathlib.Path(source).read_text()
    for key, value in replacements.items(): text = re.sub(rf"(?m)^(\s*{re.escape(key)}:\s*).*$", rf"\g<1>{value}", text)
    pathlib.Path(target).write_text(text)

def patch_iox_config(source, target, global_replacements, bench_replacements):
    """Patch a temporary C-lane config while leaving historical templates intact."""
    text = pathlib.Path(source).read_text()
    for key, value in global_replacements.items():
        text, count = re.subn(
            rf"(?m)^(\s*(?:-\s*)?{re.escape(key)}:\s*).*$",
            rf"\g<1>{value}", text)
        if count == 0: raise ValueError(f"missing global config key: {key}")
    marker = "  bench_options:\n"
    if text.count(marker) != 1: raise ValueError("expected exactly one bench_options mapping")
    prefix, bench = text.split(marker)
    for key, value in bench_replacements.items():
        pattern = rf"(?m)^(    {re.escape(key)}:\s*).*$"
        if re.search(pattern, bench):
            bench = re.sub(pattern, rf"\g<1>{value}", bench)
        else:
            bench += f"    {key}: {value}\n"
    pathlib.Path(target).write_text(prefix + marker + bench)

def validate_latency_csv(path, lane):
    errors=[]; result={"valid":False,"count":None,"label":None,"errors":errors}
    try:
        with pathlib.Path(path).open(newline="") as stream: rows=list(csv.reader(stream))
    except (OSError,csv.Error) as e: errors.append(f"csv_read_error: {e}"); return result
    if len(rows)!=2: errors.append(f"csv_must_have_exactly_header_and_one_row: got {len(rows)} rows"); return result
    if tuple(rows[0])!=CSV_SCHEMA: errors.append("csv_schema_mismatch")
    row=rows[1]
    if len(row)!=len(CSV_SCHEMA): errors.append("csv_data_column_count_mismatch"); return result
    result["label"]=row[0]
    if row[0]!=LANE_LABELS[lane]: errors.append("csv_label_mismatch")
    try: result["count"]=int(row[1]); errors += [] if result["count"]==3000 else ["csv_count_mismatch"]
    except ValueError: errors.append("csv_count_not_integer")
    for name,value in zip(CSV_SCHEMA[2:],row[2:]):
        try:
            if not math.isfinite(float(value)): raise ValueError
        except ValueError: errors.append(f"csv_{name}_not_finite_numeric")
    result["valid"]=not errors; return result

def freshness_ok(paths, run_started_ns):
    errors=[]
    for path in paths:
        try:
            if pathlib.Path(path).stat().st_mtime_ns < run_started_ns: errors.append(f"stale_artifact: {pathlib.Path(path).name}")
        except OSError: errors.append(f"missing_artifact: {pathlib.Path(path).name}")
    return not errors, errors

def validate_result_gate(lane,status,csv_result,process_exit_codes,run_started_ns,artifacts,timed_out=None,publisher_status=None):
    errors=[]; fresh,fe=freshness_ok(artifacts,run_started_ns); errors.extend(fe)
    for name in required_processes(lane):
        code=process_exit_codes.get(name); allowed=code==0 or (lane in SEND_LANES and code==-signal.SIGINT)
        if not allowed: errors.append(f"process_exit_code: {name}={code!r}")
    if timed_out: errors.append(f"timeout_or_kill: {timed_out}")
    attempted=None; schema_ok,se=validate_status_schema(status); errors.extend(se); status_data=status if isinstance(status,dict) else {}
    if schema_ok: attempted=status_data.get("attempted_measured")
    checks={"status_valid":schema_ok and status_data.get("valid") is True,"attempted_measured":attempted==3000,"received_measured":status_data.get("received_measured")==3000,"unique_measured":status_unique_measured(status_data)==3000,"status_errors_zero":schema_ok and all(status_data[k]==0 for k in ERROR_FIELDS),"strict_csv":csv_result.get("valid") is True,"fresh_artifacts":fresh}
    if lane in SEND_LANES:
        publisher_schema_ok,publisher_errors=validate_publisher_status_schema(publisher_status); errors.extend(publisher_errors)
        publisher_data=publisher_status if isinstance(publisher_status,dict) else {}
        checks.update({"publisher_status_valid":publisher_schema_ok and publisher_data.get("valid") is True,"publisher_completion":publisher_schema_ok and publisher_data.get("completion") is True,"publisher_attempted_total":publisher_data.get("attempted_total")==3050,"publisher_attempted_measured":publisher_data.get("attempted_measured")==3000})
    errors.extend(k for k,v in checks.items() if not v); return not errors,errors

def _self_start_ticks():
    text=pathlib.Path("/proc/self/stat").read_text(); return int(text[text.rfind(")")+2:].split()[19])

def write_result(work,result,collector_managed=False):
    if collector_managed:
        csv_path=work/"latency.csv"
        digest=hashlib.sha256(csv_path.read_bytes()).hexdigest() if csv_path.is_file() else None
        result["collector_managed"]={"schema":"ibmw.abc.collector-managed.v1",
            "run_id":os.environ.get("MW_PERF_RUN_ID"),"producer_pid":os.getpid(),
            "producer_start_time_ticks":_self_start_ticks(),"lane":result.get("lane"),
            "complete":True,"latency_csv_sha256":digest}
    target=work/"result.json"; temporary=work/"result.json.tmp"
    temporary.write_text(json.dumps(result,indent=2)+"\n"); temporary.replace(target); print(json.dumps(result))

def _finish(code, collector_managed, stop_event):
    if not collector_managed: return code
    while not stop_event.wait(.25): pass
    return code

def main():
    p=argparse.ArgumentParser(); p.add_argument("lane",choices=tuple(LANE_LABELS)); p.add_argument("--build",required=True,type=pathlib.Path); p.add_argument("--work",required=True,type=pathlib.Path); p.add_argument("--domain",type=int,default=45); p.add_argument("--roudi",type=pathlib.Path,help="RouDi executable for lane C (overrides IBMW_ROUDI_BIN and CMake metadata)"); p.add_argument("--collector-managed",action="store_true"); a=p.parse_args()
    a.build=a.build.resolve(); a.work=a.work.resolve(); a.work.mkdir(parents=True,exist_ok=True)
    stop_event=_StopFlag()
    if a.collector_managed and not os.environ.get("MW_PERF_RUN_ID"): p.error("--collector-managed requires MW_PERF_RUN_ID")
    # TERM only records the request; it never raises.  S aborts at the safe
    # points below, so a TERM cannot land inside Popen (orphaning an endpoint)
    # or inside the result write of a finished run.
    if a.collector_managed or a.lane=="S": signal.signal(signal.SIGTERM,lambda *_: stop_event.set())
    for x in a.work.iterdir(): shutil.rmtree(x) if x.is_dir() else x.unlink()
    started=time.time_ns(); csv_path=a.work/"latency.csv"; status_path=a.work/"subscriber.status.json"; env=clean_env(); procs={}; logs=[]; warnings=[]; cleanup_errors=[]; timed_out=None; shutdown_signal=None; shm_root=None; shm_domain=None
    def stop_point():
        """S safe point: a recorded TERM aborts a run that has not completed."""
        if a.lane=="S" and stop_event.is_set(): raise KeyboardInterrupt
    def pause(seconds):
        if a.lane=="S": stop_event.wait(seconds); stop_point()
        else: time.sleep(seconds)
    def wait_endpoint(proc,timeout):
        if a.lane!="S": return proc.wait(timeout=timeout)
        deadline=time.monotonic()+timeout
        while proc.poll() is None:
            if time.monotonic()>=deadline: raise subprocess.TimeoutExpired(proc.args,timeout)
            pause(.05)
    try:
        stop_point()
        if a.lane in "AB":
            exe=a.build/("ibmw_abc_ros2_endpoint" if a.lane=="A" else "ibmw_abc_dds_endpoint"); base=[str(exe),"--hz","100","--warmup-messages","50","--message-count","3000"]
            if a.lane=="A": env["ROS_DOMAIN_ID"]=str(a.domain); subcfg=pubcfg=[]
            else:
                src=pathlib.Path(__file__).parent/"config"; sub=a.work/"subscriber.yaml"; pub=a.work/"publisher.yaml"; patch_yaml(src/"subscriber.yaml",sub,{"domain_id":a.domain}); patch_yaml(src/"publisher.yaml",pub,{"domain_id":a.domain}); subcfg=["--config",str(sub)]; pubcfg=["--config",str(pub)]
            sf=(a.work/"subscriber.log").open("w"); pf=(a.work/"publisher.log").open("w"); logs=[sf,pf]; procs["subscriber"]=subprocess.Popen(base+["--role","sub","--csv",str(csv_path),"--status",str(status_path)]+subcfg,cwd=a.build,env=env,stdout=sf,stderr=subprocess.STDOUT); time.sleep(2); procs["publisher"]=subprocess.Popen(base+["--role","pub","--status",str(a.work/"publisher.status.json")]+pubcfg,cwd=a.build,env=env,stdout=pf,stderr=subprocess.STDOUT)
            for name,timeout in (("publisher",45),("subscriber",10)):
                try: procs[name].wait(timeout=timeout)
                except subprocess.TimeoutExpired: timed_out=name; procs[name].kill(); procs[name].wait()
        else:
            sub=a.work/"subscriber.yaml"; pub=a.work/"publisher.yaml"; publisher_status_path=a.work/"publisher.status.json"; commands=send_lane_commands(a.lane,a.build,sub,pub,a.roudi); patch_send_lane_configs(a.lane,a.build,sub,pub,csv_path,status_path,publisher_status_path)
            if a.lane=="S":
                # Hold TERM/INT until the root is recorded: SIGINT's default
                # KeyboardInterrupt pending here is raised on unblock, where every
                # cleanup path can see the root.  shm-root.txt records it for
                # anyone cleaning up after a SIGKILL.
                mask=signal.pthread_sigmask(signal.SIG_BLOCK,())
                try:
                    signal.pthread_sigmask(signal.SIG_BLOCK,{signal.SIGTERM,signal.SIGINT})
                    shm_root=tempfile.mkdtemp(prefix="ibS.",dir="/tmp")
                    (a.work/"shm-root.txt").write_text(shm_root+"\n")
                finally: signal.pthread_sigmask(signal.SIG_SETMASK,mask)
                stop_point(); shm_domain=configure_shm_run(env,shm_root,a.domain)
            for name,exe,args in commands:
                f=(a.work/f"{name}.log").open("w"); logs.append(f); procs[name]=subprocess.Popen([str(exe),*args],cwd=a.build,env=env,stdout=f,stderr=subprocess.STDOUT); stop_point(); pause(1 if name!="subscriber" else 2)
            deadline=time.monotonic()+45
            while time.monotonic()<deadline:
                try:
                    pp=a.work/"publisher.status.json"; ps=json.loads(pp.read_text()); ok,_=validate_publisher_status_schema(ps); fresh,_=freshness_ok((pp,),started)
                    if ok and fresh and ps.get("valid") is True and ps.get("completion") is True and ps.get("attempted_measured")==3000: break
                except (OSError,json.JSONDecodeError): pass
                pause(.25)
            else: timed_out="publisher"
            for proc in procs.values():
                if proc.poll() is None: proc.send_signal(signal.SIGINT); shutdown_signal="SIGINT"
            for name,proc in procs.items():
                try: wait_endpoint(proc,30)
                except subprocess.TimeoutExpired: timed_out=timed_out or name; proc.kill(); proc.wait()
            # Every endpoint has exited: the run is complete, and a TERM from here
            # on is not checked again, so it leaves the finished result alone.
            if shm_root: stop_shm_run(procs,shm_root,cleanup_errors)
        try: status=json.loads(status_path.read_text()) if status_path.exists() else None
        except (OSError,json.JSONDecodeError): status=None
        csv_result=validate_latency_csv(csv_path,a.lane); exits={n:p.returncode for n,p in procs.items()}; artifacts=[csv_path,status_path]; pp=a.work/"publisher.status.json"; publisher_status=None
        if a.lane in SEND_LANES:
            artifacts.append(pp)
            try: publisher_status=json.loads(pp.read_text())
            except (OSError,json.JSONDecodeError): pass
        valid,errors=validate_result_gate(a.lane,status,csv_result,exits,started,artifacts,timed_out,publisher_status); errors+=cleanup_errors; valid=valid and not cleanup_errors; result={"lane":a.lane,"domain":a.domain,"run_started_ns":started,"valid":valid,"status":status,"csv_validation":csv_result,"process_exit_codes":exits,"validation_errors":errors,"validation_warnings":warnings};
        if a.lane in SEND_LANES: result["publisher_status"]=publisher_status
        if shm_domain: result["shm_domain"]=shm_domain
        if shutdown_signal: result["shutdown_signal"]=shutdown_signal
        write_result(a.work,result,a.collector_managed); return _finish(0 if valid else 1,a.collector_managed,stop_event)
    except KeyboardInterrupt:
        if shm_root: stop_shm_run(procs,shm_root,cleanup_errors)
        write_result(a.work,{"lane":a.lane,"run_started_ns":started,"valid":False,"validation_errors":["runner_keyboard_interrupt",*cleanup_errors],"validation_warnings":warnings,**({"shm_domain":shm_domain} if shm_domain else {})},a.collector_managed); return _finish(130,a.collector_managed,stop_event)
    except (OSError,subprocess.SubprocessError,TypeError,ValueError) as e:
        if shm_root: stop_shm_run(procs,shm_root,cleanup_errors)
        write_result(a.work,{"lane":a.lane,"run_started_ns":started,"valid":False,"validation_errors":[f"runner_error: {e}",*cleanup_errors],"validation_warnings":warnings,**({"shm_domain":shm_domain} if shm_domain else {})},a.collector_managed); return _finish(1,a.collector_managed,stop_event)
    finally:
        try:
            if shm_root: stop_shm_run(procs,shm_root,cleanup_errors)
        finally:
            for proc in procs.values():
                if proc.poll() is None: proc.terminate()
            for stream in logs: stream.close()

if __name__=="__main__":
    # Always leave through os._exit, skipping interpreter teardown: finalisation
    # restores SIGTERM's default action, so a late collector TERM would turn a
    # clean exit into -15.  Unexpected exceptions and broken pipes exit 1.
    code=1
    try: code=main()
    except SystemExit as e:
        code=e.code if isinstance(e.code,int) else 0 if e.code is None else 1
        if isinstance(e.code,str):
            try: print(e.code,file=sys.stderr)
            except (OSError,ValueError): pass
    except KeyboardInterrupt: code=130
    except BaseException:
        try: traceback.print_exc()
        except (OSError,ValueError): pass
    finally:
        for stream in (sys.stdout,sys.stderr):
            try: stream.flush()
            except (OSError,ValueError): pass
        os._exit(code)
