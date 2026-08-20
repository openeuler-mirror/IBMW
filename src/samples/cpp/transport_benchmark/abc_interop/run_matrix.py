#!/usr/bin/env python3
import argparse,json,pathlib,subprocess,sys
p=argparse.ArgumentParser(); p.add_argument("--blocks",type=int,default=5); p.add_argument("--build",required=True,type=pathlib.Path); p.add_argument("--output",type=pathlib.Path,default=pathlib.Path("abc-results")); a=p.parse_args()
if a.blocks<1: p.error("--blocks must be >= 1")
a.output.mkdir(parents=True,exist_ok=True); results=[]; runner=pathlib.Path(__file__).with_name("run_block.py")
for block in range(1,a.blocks+1):
 for offset,lane in enumerate("ABC"):
  work=a.output/f"block-{block}"/lane; domain=80+block*3+offset; result=None
  try: rc=subprocess.run([sys.executable,str(runner),lane,"--build",str(a.build),"--work",str(work),"--domain",str(domain)],timeout=120).returncode
  except subprocess.TimeoutExpired: rc=1; result={"lane":lane,"valid":False,"validation_errors":["runner_timeout_expired"]}
  except OSError as e: rc=1; result={"lane":lane,"valid":False,"validation_errors":[f"runner_start_error: {e}"]}
  if result is None:
   try: result=json.loads((work/"result.json").read_text())
   except (OSError,json.JSONDecodeError): result={"lane":lane,"valid":False,"validation_errors":["result.json_missing_or_unparseable"]}
  result.update(block=block,returncode=rc); results.append(result)
valid_runs=sum(r.get("valid") is True and r.get("returncode")==0 for r in results)
summary={"blocks":a.blocks,"valid":bool(results) and valid_runs==len(results),"valid_runs":valid_runs,"invalid_runs":len(results)-valid_runs,"runs":results,"semantic_warning":"C is not DDS-equivalent and is reported separately"}
(a.output/"summary.json").write_text(json.dumps(summary,indent=2)+"\n"); sys.exit(0 if summary["valid"] else 1)
