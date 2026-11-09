"""Acceptance tests through separate CLI processes and real runtime transports."""
import argparse
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

import support
from support import FunctionalCase, rows

MESSAGE_TYPE = "json:diag.Message"
SERVICE = "json:/diag/math/Sum"
SERVICE_TYPE = "json:diag.SumRequest -> json:diag.SumResponse"


class Topic(FunctionalCase):
    def test_discovery_reflects_real_registered_interfaces(self):
        result = self.registry_command("topic", "list")
        self.assertEqual(set(result.stdout.splitlines()), {"/diag/data", "/diag/input"})
        self.assertEqual(self.registry_command("topic", "type", "/diag/data").stdout.strip(), MESSAGE_TYPE)
        self.assertEqual(set(self.registry_command("topic", "find", MESSAGE_TYPE).stdout.splitlines()),
                         {"/diag/data", "/diag/input"})
        info = self.registry_command("topic", "info", "/diag/data", "--verbose").stdout
        self.assertIn("Publisher count: 1", info)
        self.assertIn("Subscription count: 1", info)
        self.assertIn("Producer", info)
        self.assertIn("Peer", info)
        self.wait_for(lambda: rows(self.server.directory / "local-received.jsonl"), "real local delivery")

    def test_echo_missing_topic_fails(self):
        missing = self.registry_command("topic", "echo", "/diag/missing", "--once", "--timeout", "0.2", code=1)
        self.assertFalse(missing.stdout)
        self.assertIn("Timed out", missing.stderr)

    def test_hz_and_bw_agree_with_independent_sender_journal(self):
        hz = self.registry_command("topic", "hz", "/diag/data", "--duration", "1.2", "--timeout", "2").stdout
        matches = re.findall(r"average rate: ([0-9.]+)", hz)
        self.assertTrue(matches, hz)
        emitted = rows(self.server.directory / "sent.jsonl")
        self.assertGreaterEqual(len(emitted), 10)
        actual_rate = (len(emitted) - 1) * 1e9 / (emitted[-1]["time_ns"] - emitted[0]["time_ns"])
        # This checks units and the measured stream, not scheduler performance.
        self.assertAlmostEqual(float(matches[-1]), actual_rate, delta=actual_rate * 0.3)
        bw = self.registry_command("topic", "bw", "/diag/data", "--duration", "1.2", "--timeout", "2").stdout
        values = re.findall(r"([0-9.]+) B/s from (\d+) messages", bw)
        self.assertTrue(values, bw)
        self.assertGreaterEqual(int(values[-1][1]), 10)
        self.assertIn(f"Message size mean: {emitted[0]['bytes']} B", bw)
        self.assertAlmostEqual(float(values[-1][0]), actual_rate * emitted[0]["bytes"],
                               delta=actual_rate * emitted[0]["bytes"] * 0.3)

    def test_pub_delivers_exact_payload_and_count_to_another_process(self):
        for index, backend in enumerate(["udp", "tcp", "http"]):
            with self.subTest(backend=backend):
                port = self.server.ports[backend]
                target = f"127.0.0.1:{port}"
                if backend == "http":
                    target = "http://" + target
                value = 70 + index
                self.command("topic", "pub", "/diag/input", MESSAGE_TYPE, json.dumps({"value": value}),
                             "--" + backend, "--target", target, "--transport-library", self.options.net,
                             "--type-support", self.options.types, "--times", "3", "--rate", "20",
                             "--wait", "0.1", "--keep-alive", "0.3")
                received = self.wait_for(
                    lambda: [r for r in rows(self.server.directory / "received.jsonl") if r["value"] == value]
                    if len([r for r in rows(self.server.directory / "received.jsonl") if r["value"] == value]) >= 3 else None,
                    f"{backend} receiver observed three publications")
                self.assertEqual(received, [{"value": value}] * 3)
        self.assertEqual(len(rows(self.server.directory / "received.jsonl")), 9)


class Service(FunctionalCase):
    def call(self, payload, service=SERVICE, timeout="1", code=0):
        return self.command("service", "call", service, SERVICE_TYPE, json.dumps(payload),
                            "--http", f"127.0.0.1:{self.server.ports['http']}",
                            "--timeout", timeout, "--type-support", self.options.types, code=code)

    def test_registry_and_http_discovery_find_the_serving_process(self):
        for flags in [["--registry-path", self.registry],
                      ["--http", f"127.0.0.1:{self.server.ports['http']}"]]:
            with self.subTest(source=flags[0]):
                self.assertIn(SERVICE, self.command("service", "list", *flags).stdout.splitlines())
                self.assertEqual(self.command("service", "type", SERVICE, *flags).stdout.strip(), SERVICE_TYPE)
                self.assertEqual(self.command("service", "find", SERVICE_TYPE, *flags).stdout.strip(), SERVICE)

    def test_calls_compute_results_and_reject_bad_input_without_sending(self):
        for a, b in [(19, 23), (-50, 7), (1000000, 2)]:
            self.assertEqual(json.loads(self.call({"a": a, "b": b}).stdout), {"sum": a + b})
        self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 3)
        bad = self.call({"a": "invalid", "b": 1}, code=1)
        self.assertFalse(bad.stdout)
        self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 3)
        self.assertFalse(self.call({"a": 1, "b": 2}, service=SERVICE + "Missing", code=1).stdout)
        self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 3)

    def test_failure_timeout_and_server_loss_do_not_look_successful(self):
        self.assertFalse(self.call({"a": 1, "b": 2, "status": 1008}, code=1).stdout)
        self.assertFalse(self.call({"a": 1, "b": 2, "delay_ms": 600}, timeout="0.1", code=1).stdout)
        self.wait_for(lambda: len(rows(self.server.directory / "rpc-done.jsonl")) == 2, "late response completion")
        self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 2, "failed calls must not be retried")
        self.assertEqual(json.loads(self.call({"a": 2, "b": 5}).stdout), {"sum": 7})
        self.server.stop()
        self.assertFalse(self.call({"a": 1, "b": 2}, timeout="0.2", code=1).stdout)


class Param(FunctionalCase):
    def test_list_filters_and_module_isolation(self):
        self.assertEqual(self.param("list", "Producer").stdout.splitlines(), ["controller.gain", "speed"])
        self.assertEqual(self.param("list", "Producer", "--param-prefixes", "controller").stdout, "controller.gain\n")
        self.assertEqual(self.param("list", "Producer", "--filter", "^speed$").stdout, "speed\n")
        grouped = self.param("list").stdout
        self.assertIn("Producer:\n", grouped)
        self.assertIn("Peer:\n", grouped)
        self.assertEqual(self.param("get", "Peer", "speed", "--hide-type").stdout, "7\n")

    def test_set_changes_the_value_read_by_business_module(self):
        for value in ["1.5", "", "false", "中文 value", "--help", "-2.5"]:
            with self.subTest(value=value):
                self.param("set", "Producer", "speed", "--", value)
                self.assertEqual(self.param("get", "Producer", "speed", "--hide-type").stdout, value + "\n")
                self.wait_for(lambda: self.observed()["producer_speed"] == value, "module reads updated parameter")
                self.assertEqual(self.observed()["peer_speed"], "7")

    def test_errors_do_not_mutate_state_and_restart_restores_defaults(self):
        original = self.server.config.read_bytes()
        for verb, arguments in [("get", ["Producer", "absent"]), ("set", ["Missing", "speed", "2"])]:
            self.assertFalse(self.param(verb, *arguments, code=1).stdout)
        self.assertEqual(self.observed()["producer_speed"], "0.2")
        self.param("set", "Producer", "speed", "9")
        self.wait_for(lambda: self.observed()["producer_speed"] == "9", "changed parameter before restart")
        self.assertEqual(self.server.config.read_bytes(), original)
        self.server.stop()
        self.assertFalse(self.param("get", "Producer", "speed", code=1).stdout)
        self.server = self.launch_runtime()
        self.assertEqual(self.observed()["producer_speed"], "0.2")


class Log(FunctionalCase):
    def test_levels_change_actual_emitted_logs_and_preserve_other_module(self):
        self.assertEqual(self.log("list", "Producer").stdout, "Producer INFO\n")
        for threshold, level in enumerate(["TRACE", "debug", "INFO", "warn", "ERROR", "FATAL", "OFF"]):
            with self.subTest(level=level):
                self.log("set", "Producer", level)
                self.assertEqual(self.log("list", "Producer").stdout, f"Producer {level.upper()}\n")
                token = f"probe-{threshold}"
                self.assertEqual(self.probe_logs(token), [f"{token}:{i}" for i in range(threshold, 6)])
                self.assertEqual(self.log("list", "Peer").stdout, "Peer INFO\n")

    def test_invalid_requests_and_restart_preserve_expected_levels(self):
        original = self.server.config.read_bytes()
        self.assertFalse(self.log("set", "Producer", "INVALID", code=1).stdout)
        self.assertFalse(self.log("set", "Missing", "DEBUG", code=1).stdout)
        self.assertEqual(self.probe_logs("unchanged"), [f"unchanged:{i}" for i in range(2, 6)])
        self.log("set", "Producer", "OFF")
        self.assertEqual(self.server.config.read_bytes(), original)
        self.server.stop()
        self.assertFalse(self.log("list", code=1).stdout)
        self.server = self.launch_runtime()
        self.assertEqual(self.log("list", "Producer").stdout, "Producer INFO\n")
        self.assertEqual(self.probe_logs("restarted"), [f"restarted:{i}" for i in range(2, 6)])


class Module(FunctionalCase):
    def test_list_and_info_match_the_running_modules(self):
        self.assertEqual(self.registry_command("module", "list", "-c").stdout, "2\n")
        listing = self.registry_command("module", "list").stdout
        self.assertIn(f"{self.server.identity}/Producer [pid: {self.server.child.pid}]", listing)
        self.assertIn(f"{self.server.identity}/Peer [pid: {self.server.child.pid}]", listing)
        info = self.registry_command("module", "info", "Producer").stdout
        self.assertIn("Publishers:\n  /diag/data [json:diag.Message]", info)
        self.assertIn("Subscribers:\n  /diag/input [json:diag.Message]", info)

    def test_duplicate_names_require_runtime_selection(self):
        second = self.launch_runtime()
        self.assertEqual(self.registry_command("module", "list", "-c").stdout, "4\n")
        self.assertFalse(self.registry_command("module", "info", "Producer", code=1).stdout)
        selected = self.registry_command("module", "info", "Producer", "--runtime", second.identity).stdout
        self.assertIn(f"PID: {second.child.pid}", selected)
        self.assertEqual(self.registry_command("module", "list", "--runtime", second.identity, "-c").stdout, "2\n")

    def test_lifecycle_queries_are_read_only_and_exited_runtime_disappears(self):
        before = rows(self.server.directory / "lifecycle.jsonl")
        for _ in range(3):
            result = self.registry_command("doctor", "module", "--report")
            self.assertIn("Module lifecycle checks: 4 OK, 0 WARN", result.stdout)
            self.assertIn('module="Producer"', result.stdout)
        self.assertEqual(rows(self.server.directory / "lifecycle.jsonl"), before)
        self.server.stop()
        self.assertEqual(self.registry_command("module", "list", "-c").stdout, "0\n")
        result = self.registry_command("doctor", "module", "--runtime", self.server.identity, code=2)
        self.assertIn("not found", result.stderr)


class Doctor(FunctionalCase):
    def test_root_report_matches_healthy_runtime_and_explicit_log_baseline(self):
        result = self.registry_command("doctor", "--registry", "--report", "--include-warnings",
                                       *self.connection(), "--service", "diag/logs", "--expect-level", "Producer=INFO")
        self.assertIn('[OK] topic "/diag/data"', result.stdout)
        self.assertIn("Module lifecycle checks: 4 OK, 0 WARN", result.stdout)
        self.assertIn('[OK] log_level', result.stdout)
        self.assertIn("Doctor checks: 0 WARN, 0 UNKNOWN", result.stdout)

    def test_missing_subscriber_is_reported_and_strict_mode_fails(self):
        self.server.stop()
        self.server = self.launch_runtime(orphan=True)
        ordinary = self.registry_command("doctor", "--registry", "--report")
        self.assertIn('[WARN] topic "/diag/orphan"', ordinary.stdout)
        strict = self.registry_command("doctor", "--registry", "--include-warnings", code=1)
        self.assertIn('[WARN] topic "/diag/orphan"', strict.stdout)

    def test_log_deviation_is_reported_without_changing_runtime(self):
        flags = [*self.connection(), "--service", "diag/logs", "--expect-level", "Producer=DEBUG"]
        ordinary = self.command("doctor", "log", *flags)
        self.assertIn("[WARN] log_level", ordinary.stdout)
        self.command("doctor", "log", *flags, "--include-warnings", code=1)
        self.assertEqual(self.log("list", "Producer").stdout, "Producer INFO\n")

    def test_unavailable_evidence_is_an_error_not_a_healthy_result(self):
        self.server.stop()
        result = self.command("doctor", "log", *self.connection(), "--service", "diag/logs", code=2)
        self.assertIn("[UNKNOWN]", result.stdout)
        self.assertNotIn("[OK]", result.stdout)


class Lifecycle(FunctionalCase):
    start_runtime = False

    def check_failure(self, phase):
        directory = self.root / "failure"
        directory.mkdir()
        journal = directory / "callbacks.txt"
        self.launch_process([self.options.lifecycle_server, self.registry, phase, journal], directory)
        expected = "setup\n" if phase == "initialize" else "setup\nrun\n"
        self.assertEqual(journal.read_text(), expected)
        for strict in [False, True]:
            args = ["doctor", "module", "--report"]
            if strict:
                args.append("--include-warnings")
            result = self.registry_command(*args, code=1 if strict else 0)
            self.assertIn('[WARN] module', result.stdout)
            self.assertIn(f"phase={phase}", result.stdout)
            self.assertIn(f"device {'initialization' if phase == 'initialize' else 'start'} failed", result.stdout)
            self.assertEqual(journal.read_text(), expected, "queries must not rerun lifecycle callbacks")

    def test_initialization_failure_is_reported_from_recorded_callback_result(self):
        self.check_failure("initialize")

    def test_start_failure_is_reported_from_recorded_callback_result(self):
        self.check_failure("start")

    def test_unsupported_runtime_query_is_not_reported_as_success(self):
        directory = self.root / "unsupported"
        directory.mkdir()
        self.launch_process([self.options.lifecycle_server, self.registry, "unsupported", directory / "callbacks.txt"], directory)
        result = self.registry_command("doctor", "module", "--report", code=2)
        self.assertIn("query errors: 1", result.stdout)
        self.assertNotIn("[OK]", result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=["topic", "service", "param", "log", "module", "doctor", "lifecycle"], required=True)
    for option in ["cli", "server", "lifecycle-server", "types", "net", "parameter", "log", "artifacts"]:
        parser.add_argument("--" + option, required=True)
    parser.add_argument("--rpc-backend", default="http", choices=["http", "dds", "grpc", "mqtt", "zenoh"])
    parser.add_argument("--mqtt-tls", default="off", choices=["off", "server", "mutual"])
    for option in ["dds", "grpc", "mqtt", "zenoh", "mosquitto"]:
        parser.add_argument("--" + option)
    support.OPTIONS = parser.parse_args()
    Path(support.OPTIONS.artifacts).mkdir(parents=True, exist_ok=True)
    support.OPTIONS.artifacts = tempfile.mkdtemp(prefix="run-", dir=support.OPTIONS.artifacts)
    cases = {"topic": Topic, "service": Service, "param": Param, "log": Log,
             "module": Module, "doctor": Doctor, "lifecycle": Lifecycle}
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(cases[support.OPTIONS.case])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(f"Evidence: {support.OPTIONS.artifacts}")
    sys.exit(0 if result.wasSuccessful() else 1)
