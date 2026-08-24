import json, tempfile, time, unittest
from pathlib import Path
from run_block import CSV_SCHEMA, clean_env, freshness_ok, patch_iox_config, validate_latency_csv, validate_publisher_status_schema, validate_result_gate, validate_status_schema

def csv_text(label="abc_ros2_4KB", count="3000", value="1.0"):
    return ",".join(CSV_SCHEMA) + "\n" + ",".join([label, count] + [value] * 8) + "\n"

class RunBlockValidationTest(unittest.TestCase):
    def test_c_lane_patches_temporary_historical_configs(self):
        template_dir = Path(__file__).resolve().parents[1] / "iox_send/config/linux/bin/cfg/iox_send"
        for name, artifact_key in (("publisher.yaml", "publisher_status_path"),
                                   ("subscriber.yaml", "status_path")):
            source = template_dir / name
            historical = source.read_text()
            self.assertIn('topic: "iox_bench_send_topic"', historical)
            self.assertIn("payload_bytes: 1024", historical)
            with tempfile.TemporaryDirectory() as directory:
                target = Path(directory) / name
                patch_iox_config(
                    source, target,
                    {"path": "/tmp/libibmw_iceoryx_transport.so",
                     "topic": '"/mw_perf/payload_4kb"'},
                    {"topic": '"/mw_perf/payload_4kb"', "payload_bytes": 4096,
                     "message_count": 3000, "start_delay_ms": 2000,
                     "label": "iox_send_4KB", artifact_key: "/tmp/status.json"})
                patched = target.read_text()
                self.assertIn('    topic: "/mw_perf/payload_4kb"', patched)
                self.assertIn("    payload_bytes: 4096", patched)
                self.assertIn("    message_count: 3000", patched)
                self.assertIn("    start_delay_ms: 2000", patched)
                self.assertIn(f"    {artifact_key}: /tmp/status.json", patched)
            self.assertEqual(source.read_text(), historical)

    def test_clean_env_prefers_system_libstdcpp(self):
        env = clean_env()
        paths = env["LD_LIBRARY_PATH"].split(":")
        self.assertEqual(paths[0], "/usr/lib/x86_64-linux-gnu")
        self.assertFalse(any("conda" in path.lower() for path in paths))

    def test_status_schema_rejects_missing_null_and_arrays(self):
        self.assertFalse(validate_status_schema(None)[0])
        self.assertFalse(validate_status_schema([])[0])
        self.assertFalse(validate_status_schema({"valid": True})[0])
        self.assertFalse(validate_status_schema({"valid": True, "attempted_measured": 3000, "received_measured": 3000, "unique_measured": 3000, **{k: None for k in ("missing", "duplicate", "out_of_order", "invalid_payload", "invalid_timestamp")}})[0])

    def test_status_schema_uses_unique_measured(self):
        status = {"valid": True, "attempted_measured": 3000, "received_measured": 3000,
                  "unique_measured": 3000, "missing": 0, "duplicate": 0,
                  "out_of_order": 0, "invalid_payload": 0, "invalid_timestamp": 0}
        self.assertTrue(validate_status_schema(status)[0])
        self.assertFalse(validate_status_schema({**status, "unique_measured": None})[0])
        legacy=dict(status); legacy["unique"]=legacy.pop("unique_measured")
        self.assertFalse(validate_status_schema(legacy)[0])
        legacy=dict(status); legacy["sent_measured"]=legacy.pop("attempted_measured")
        self.assertFalse(validate_status_schema(legacy)[0])

    def test_result_gate_rejects_null_status_without_exception(self):
        valid, validation_errors = validate_result_gate(
            "A", None, {"valid": False}, {"publisher": 0, "subscriber": 0},
            0, (),
        )
        self.assertFalse(valid)
        self.assertIn("status_not_object", validation_errors)

    def test_c_gate_and_signal_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); csv=root/"latency.csv"; status=root/"status.json"; pub=root/"publisher.status.json"
            csv.write_text(csv_text("iox_send_4KB")); payload={"valid":True,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0}; publisher={"completion":True,"attempted_total":3050,"attempted_measured":3000,"valid":True}; status.write_text(json.dumps(payload)); pub.write_text(json.dumps(publisher));
            self.assertTrue(validate_result_gate("C",payload,validate_latency_csv(csv,"C"),{"publisher":-2,"subscriber":0,"roudi":0},0,(csv,status,pub),publisher_status=publisher)[0])
            self.assertFalse(validate_result_gate("C",payload,validate_latency_csv(csv,"C"),{"publisher":-9,"subscriber":0,"roudi":0},0,(csv,status,pub),publisher_status=publisher)[0])

    def test_publisher_status_schema_is_independent(self):
        publisher={"completion":True,"attempted_total":3050,"attempted_measured":3000,"valid":True}
        self.assertTrue(validate_publisher_status_schema(publisher)[0])
        self.assertFalse(validate_publisher_status_schema({**publisher,"completion":"true"})[0])

    def test_c_rejects_missing_publisher_artifact(self):
        self.assertFalse(self.c_gate(None, include_publisher_artifact=False)[0])

    def test_c_rejects_malformed_publisher_status(self):
        self.assertFalse(self.c_gate({"completion":True})[0])

    def test_c_rejects_incomplete_publisher_status(self):
        self.assertFalse(self.c_gate({"completion":False,"attempted_total":100,"attempted_measured":50,"valid":False})[0])

    def test_c_rejects_wrong_publisher_count(self):
        self.assertFalse(self.c_gate({"completion":True,"attempted_total":3049,"attempted_measured":2999,"valid":True})[0])
        self.assertFalse(self.c_gate({"completion":True,"attempted_total":3049,"attempted_measured":3000,"valid":True})[0])

    def c_gate(self, publisher_status, include_publisher_artifact=True):
        root=Path(self._testMethodName + "-artifacts"); self.addCleanup(lambda: __import__("shutil").rmtree(root,ignore_errors=True)); root.mkdir(exist_ok=True)
        csv=root/"latency.csv"; status=root/"subscriber.status.json"; pub=root/"publisher.status.json"; csv.write_text(csv_text("iox_send_4KB")); subscriber={"valid":True,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0}; status.write_text(json.dumps(subscriber)); artifacts=[csv,status]
        if include_publisher_artifact: pub.write_text(json.dumps(publisher_status)); artifacts.append(pub)
        return validate_result_gate("C",subscriber,validate_latency_csv(csv,"C"),{"publisher":0,"subscriber":0,"roudi":0},0,artifacts,publisher_status=publisher_status)
    def write(self, text):
        f=tempfile.NamedTemporaryFile(mode="w",suffix=".csv",delete=False); f.write(text); f.close(); self.addCleanup(lambda: Path(f.name).unlink(missing_ok=True)); return Path(f.name)
    def test_strict_csv_cases(self):
        self.assertTrue(validate_latency_csv(self.write(csv_text()),"A")["valid"])
        for text in ("bad\n1\n",csv_text("wrong"),csv_text(value="nan"),csv_text()+csv_text(),csv_text().splitlines()[0]+"\n"):
            self.assertFalse(validate_latency_csv(self.write(text),"A")["valid"])
    def test_result_gate_exit_codes_and_freshness(self):
        with tempfile.TemporaryDirectory() as directory:
            path,status=Path(directory)/"latency.csv",Path(directory)/"status.json"; path.write_text(csv_text()); status.write_text(json.dumps({"valid":True,"attempted_measured":3000,"received_measured":3000,"unique_measured":3000,"missing":0,"duplicate":0,"out_of_order":0,"invalid_payload":0,"invalid_timestamp":0})); started=0
            self.assertTrue(validate_result_gate("A",json.loads(status.read_text()),validate_latency_csv(path,"A"),{"publisher":0,"subscriber":0},started,(path,status))[0])
            self.assertFalse(validate_result_gate("A",json.loads(status.read_text()),validate_latency_csv(path,"A"),{"publisher":1,"subscriber":0},started,(path,status))[0]); self.assertFalse(freshness_ok((path,),time.time_ns()+1)[0])
if __name__ == "__main__": unittest.main()
