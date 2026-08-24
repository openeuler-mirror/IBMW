#!/usr/bin/env python3
"""Run one isolated steady-state A, B, or C benchmark lane."""
import argparse, csv, json, math, os, pathlib, re, shutil, signal, subprocess, sys, time

CSV_SCHEMA = ("label", "count", "min_us", "avg_us", "stddev_us", "p50_us", "p90_us", "p99_us", "p999_us", "max_us")
LANE_LABELS = {"A": "abc_ros2_4KB", "B": "abc_dds_4KB", "C": "iox_send_4KB"}
ERROR_FIELDS = ("missing", "duplicate", "out_of_order", "invalid_payload", "invalid_timestamp")

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
    errors=[]; fresh,fe=freshness_ok(artifacts,run_started_ns); errors.extend(fe); required=("publisher","subscriber") if lane in "AB" else ("publisher","subscriber","roudi")
    for name in required:
        code=process_exit_codes.get(name); allowed=code==0 or (lane=="C" and code==-signal.SIGINT)
        if not allowed: errors.append(f"process_exit_code: {name}={code!r}")
    if timed_out: errors.append(f"timeout_or_kill: {timed_out}")
    attempted=None; schema_ok,se=validate_status_schema(status); errors.extend(se); status_data=status if isinstance(status,dict) else {}
    if schema_ok: attempted=status_data.get("attempted_measured")
    checks={"status_valid":schema_ok and status_data.get("valid") is True,"attempted_measured":attempted==3000,"received_measured":status_data.get("received_measured")==3000,"unique_measured":status_unique_measured(status_data)==3000,"status_errors_zero":schema_ok and all(status_data[k]==0 for k in ERROR_FIELDS),"strict_csv":csv_result.get("valid") is True,"fresh_artifacts":fresh}
    if lane=="C":
        publisher_schema_ok,publisher_errors=validate_publisher_status_schema(publisher_status); errors.extend(publisher_errors)
        publisher_data=publisher_status if isinstance(publisher_status,dict) else {}
        checks.update({"publisher_status_valid":publisher_schema_ok and publisher_data.get("valid") is True,"publisher_completion":publisher_schema_ok and publisher_data.get("completion") is True,"publisher_attempted_total":publisher_data.get("attempted_total")==3050,"publisher_attempted_measured":publisher_data.get("attempted_measured")==3000})
    errors.extend(k for k,v in checks.items() if not v); return not errors,errors

def write_result(work,result):
    (work/"result.json").write_text(json.dumps(result,indent=2)+"\n"); print(json.dumps(result))

def main():
    p=argparse.ArgumentParser(); p.add_argument("lane",choices="ABC"); p.add_argument("--build",required=True,type=pathlib.Path); p.add_argument("--work",required=True,type=pathlib.Path); p.add_argument("--domain",type=int,default=45); a=p.parse_args(); a.build=a.build.resolve(); a.work=a.work.resolve(); a.work.mkdir(parents=True,exist_ok=True)
    for x in a.work.iterdir(): shutil.rmtree(x) if x.is_dir() else x.unlink()
    started=time.time_ns(); csv_path=a.work/"latency.csv"; status_path=a.work/"subscriber.status.json"; env=clean_env(); procs={}; logs=[]; warnings=[]; timed_out=None; shutdown_signal=None
    try:
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
            template=pathlib.Path(__file__).resolve().parents[1]/"iox_send/config/linux/bin/cfg/iox_send"; sub=a.work/"subscriber.yaml"; pub=a.work/"publisher.yaml"; publisher_status_path=a.work/"publisher.status.json"; common={"topic":"/mw_perf/payload_4kb","payload_bytes":4096,"hz":100,"warmup_messages":50,"message_count":3000,"start_delay_ms":2000}; global_config={"path":str((a.build/"libibmw_iceoryx_transport.so").resolve()),"topic":"/mw_perf/payload_4kb"}; patch_iox_config(template/"subscriber.yaml",sub,global_config,{**common,"label":"iox_send_4KB","csv_path":csv_path,"status_path":status_path}); patch_iox_config(template/"publisher.yaml",pub,global_config,{**common,"label":"iox_send_4KB","publisher_status_path":publisher_status_path})
            for name,exe,args in (("roudi",a.build/"iox-roudi",[]),("subscriber",a.build/"iox_send_subscriber_app",[str(sub)]),("publisher",a.build/"iox_send_publisher_app",[str(pub)])):
                f=(a.work/f"{name}.log").open("w"); logs.append(f); procs[name]=subprocess.Popen([str(exe),*args],cwd=a.build,env=env,stdout=f,stderr=subprocess.STDOUT); time.sleep(1 if name!="subscriber" else 2)
            deadline=time.monotonic()+45
            while time.monotonic()<deadline:
                try:
                    pp=a.work/"publisher.status.json"; ps=json.loads(pp.read_text()); ok,_=validate_publisher_status_schema(ps); fresh,_=freshness_ok((pp,),started)
                    if ok and fresh and ps.get("valid") is True and ps.get("completion") is True and ps.get("attempted_measured")==3000: break
                except (OSError,json.JSONDecodeError): pass
                time.sleep(.25)
            else: timed_out="publisher"
            for proc in procs.values():
                if proc.poll() is None: proc.send_signal(signal.SIGINT); shutdown_signal="SIGINT"
            for name,proc in procs.items():
                try: proc.wait(timeout=30)
                except subprocess.TimeoutExpired: timed_out=timed_out or name; proc.kill(); proc.wait()
        try: status=json.loads(status_path.read_text()) if status_path.exists() else None
        except (OSError,json.JSONDecodeError): status=None
        csv_result=validate_latency_csv(csv_path,a.lane); exits={n:p.returncode for n,p in procs.items()}; artifacts=[csv_path,status_path]; pp=a.work/"publisher.status.json"; publisher_status=None
        if a.lane=="C":
            artifacts.append(pp)
            try: publisher_status=json.loads(pp.read_text())
            except (OSError,json.JSONDecodeError): pass
        valid,errors=validate_result_gate(a.lane,status,csv_result,exits,started,artifacts,timed_out,publisher_status); result={"lane":a.lane,"domain":a.domain,"run_started_ns":started,"valid":valid,"status":status,"csv_validation":csv_result,"process_exit_codes":exits,"validation_errors":errors,"validation_warnings":warnings};
        if a.lane=="C": result["publisher_status"]=publisher_status
        if shutdown_signal: result["shutdown_signal"]=shutdown_signal
        write_result(a.work,result); return 0 if valid else 1
    except KeyboardInterrupt:
        write_result(a.work,{"lane":a.lane,"run_started_ns":started,"valid":False,"validation_errors":["runner_keyboard_interrupt"],"validation_warnings":warnings}); return 130
    except (OSError,subprocess.SubprocessError,TypeError,ValueError) as e:
        write_result(a.work,{"lane":a.lane,"run_started_ns":started,"valid":False,"validation_errors":[f"runner_error: {e}"],"validation_warnings":warnings}); return 1
    finally:
        for proc in procs.values():
            if proc.poll() is None: proc.terminate()
        for stream in logs: stream.close()

if __name__=="__main__": sys.exit(main())
