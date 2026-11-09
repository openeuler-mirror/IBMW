"""Functional acceptance against every network backend and native ROS peers."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import unittest

import support
from support import FunctionalCase, rows
from backends import Backend


class Network(FunctionalCase):
    start_runtime = False

    def setUp(self):
        super().setUp()
        backend_name = "ros2" if self.options.backend == "dds_ros2" else self.options.backend
        self.backend = Backend(self, backend_name)
        self.dds = backend_name == "dds"
        self.ros = backend_name == "ros2"
        self.types = self.options.dds_types if self.dds else self.options.ros_types if self.ros else self.options.types
        self.message_type = ("dds:ibmw::protocols::common::Header" if self.dds else
                             "ros2:std_msgs/msg/Int32" if self.ros else "json:diag.Message")
        self.payload = ({"time_stamp": 123, "sequence_num": 42, "frame_id": "diag"} if self.dds else
                        {"data": 42} if self.ros else {"value": 42})
        self.service_name = "dds:/diag/math/Echo" if self.dds else "/diag_add" if self.ros else "json:/diag/math/Sum"
        self.service_type = ("dds:ibmw::protocols::common::Header -> dds:ibmw::protocols::common::Error" if self.dds else
                             "example_interfaces/srv/AddTwoInts" if self.ros else "json:diag.SumRequest -> json:diag.SumResponse")
        self.server = self.launch_network()

    def launch_network(self, role="both", qos=None, label="runtime", policy="normal"):
        directory = self.root / label
        directory.mkdir()
        if self.ros:
            return self.launch_process([sys.executable, "-B", self.options.native_ros, directory, "--policy", policy], directory)
        driver = self.backend.messaging_options()
        if qos:
            driver.update({key: [{"topic": ".*", "qos": value}] for key, value in qos.items()})
        spec = {"types": self.types, "message_type": self.message_type, "payload": self.payload,
                "serialization": "cdr" if self.dds else "json", "publish": role != "subscriber",
                "subscribe": role != "publisher", "subscribe_topic": "/diag/input" if role == "both" else "/diag/data"}
        config = {"ibmw": {
            "logging": {"core_level": "OFF", "backends": []},
            "runtime_graph": {"enabled": True, "runtime_id": label, "registry_path": str(self.registry),
                              "type_support_pkgs": [self.types]},
            "extensions": {"items": [self.backend.extension()]},
            "messaging": {"drivers": [{"type": self.backend.name, "options": driver}],
                          "publishers": [{"topic": ".*", "enable_drivers": [self.backend.name]}],
                          "subscribers": [{"topic": ".*", "enable_drivers": [self.backend.name]}]}}}
        if self.options.case == "service":
            spec.update(publish=False, subscribe=False)
            config["ibmw"].pop("messaging")
        if self.options.case == "service" and self.backend.name != "iceoryx" and role != "subscriber":
            request, response = self.service_type.split(" -> ")
            spec.update(service=self.service_name, request_type=request, response_type=response)
            config["ibmw"]["service"] = {"drivers": [{"type": self.backend.name, "options": self.backend.driver}],
                                          "servers": [{"func_name": ".*", "enable_drivers": [self.backend.name]}]}
            config["ibmw"]["extensions"]["items"].append(
                {"name": "log_control_extension", "path": self.options.log,
                 "options": {"service_name": "diag/logs"}})
        config_path, spec_path = directory / "config.json", directory / "application.json"
        config_path.write_text(json.dumps(config, indent=2))
        spec_path.write_text(json.dumps(spec, indent=2))
        return self.launch_process([self.options.typed_server, config_path, spec_path, directory], directory)

    def topic_source(self):
        if self.backend.name == "mqtt":
            return ["--registry-path", str(self.registry)]
        flags = list(self.backend.discovery)
        if "--timeout" in flags:
            index = flags.index("--timeout")
            del flags[index:index + 2]
        return flags

    def wire_topic(self, logical="/diag/data"):
        return logical + "_native_" + self.message_type.replace(":", "_").replace(".", "_") if self.dds else logical

    def call(self, payload, code=0, timeout="1", missing=False):
        flags = list(self.backend.flags)
        flags[flags.index("--timeout") + 1] = timeout
        type_name = self.service_type
        if self.options.backend == "dds_ros2":
            flags[flags.index("--ros2")] = "--dds"
            flags += ["--ros2-compatible"]
            type_name = "ros2:example_interfaces/srv/AddTwoInts_Request -> ros2:example_interfaces/srv/AddTwoInts_Response"
        library = self.options.dds if self.options.backend == "dds_ros2" else self.backend.library
        type_flags = [] if self.options.backend == "ros2" else ["--type-support", self.types]
        return self.command("service", "call", self.service_name + ("Missing" if missing else ""),
                            type_name, json.dumps(payload), *flags,
                            *type_flags, "--transport-library", library, code=code, timeout=16)


class TopicNetwork(Network):
    def test_cli_publish_arrives_exactly_once_per_send(self):
        for value, spelling in [(71, "json"), (72, "yaml")]:
            payload = dict(self.payload)
            key = "sequence_num" if self.dds else "data" if self.ros else "value"
            payload[key] = value
            text = json.dumps(payload) if spelling == "json" else "{" + ", ".join(f"{k}: {v}" for k, v in payload.items()) + "}"
            self.command("topic", "pub", "/diag/input", self.message_type, text,
                         *self.backend.topic_flags(), "--transport-library", self.backend.library,
                         "--type-support", self.types, "--times", "3", "--rate", "10",
                         "--wait", "1.5", "--keep-alive", ".5", timeout=18)
            received = self.wait_for(lambda: [item for item in rows(self.server.directory / "received.jsonl")
                                              if item[key] == value] or None, "network delivery", 8)
            self.assertEqual(received, [payload] * 3)
        self.assertEqual(len(rows(self.server.directory / "received.jsonl")), 6)

    def test_rate_and_bandwidth_match_business_sender(self):
        flags = self.topic_source()
        # Observation duration is independent of the discovery timeout.
        hz = self.command("topic", "hz", self.wire_topic(), "--duration", "1.3", *flags, timeout=16).stdout
        rates = re.findall(r"average rate: ([0-9.]+)", hz)
        self.assertTrue(rates, hz)
        sent = rows(self.server.directory / "sent.jsonl")
        self.assertGreater(len(sent), 10)
        actual = (len(sent) - 1) * 1e9 / (sent[-1]["time_ns"] - sent[0]["time_ns"])
        self.assertAlmostEqual(float(rates[-1]), actual, delta=actual * .3)
        bw = self.command("topic", "bw", self.wire_topic(), "--duration", "1.3", *flags, timeout=16).stdout
        self.assertIn(f"Message size mean: {sent[0]['bytes']} B", bw)
        readings = re.findall(r"([0-9.]+) B/s from (\d+) messages", bw)
        self.assertTrue(readings, bw)
        self.assertGreaterEqual(int(readings[-1][1]), 10)
        self.assertAlmostEqual(float(readings[-1][0]), actual * sent[0]["bytes"], delta=actual * sent[0]["bytes"] * .3)

    def test_bad_payload_is_rejected_before_delivery(self):
        self.command("topic", "pub", "/diag/input", self.message_type, '{"nonexistent_field":1}',
                     *self.backend.topic_flags(), "--transport-library", self.backend.library,
                     "--type-support", self.types, "--once", code=1)
        self.assertEqual(rows(self.server.directory / "received.jsonl"), [])

    def test_discovery_observes_real_publisher(self):
        topic = self.wire_topic()
        flags = self.topic_source()
        discovered_type = self.message_type.split(":", 1)[1] if self.dds or self.ros else self.message_type
        names = self.command("topic", "list", *flags).stdout.splitlines()
        self.assertIn(topic, names)
        self.assertIn(topic, self.command("topic", "find", discovered_type, *flags).stdout.splitlines())
        self.assertEqual(self.command("topic", "type", topic, *flags).stdout.strip(), discovered_type)
        self.assertIn("Publisher count: 1", self.command("topic", "info", topic, *flags).stdout)


class ServiceNetwork(Network):
    def test_discovery_reports_live_service_name_and_type(self):
        name = self.service_name
        expected_type = self.service_type
        if self.dds:
            # DDS discovery exposes the wire service name without its type prefix.
            name = "/diag/math/Echo"
            expected_type = "ibmw::protocols::common::Header -> ibmw::protocols::common::Error"
        listing = self.command("service", "list", *self.backend.discovery).stdout.splitlines()
        self.assertIn(name, listing)
        self.assertEqual(self.command("service", "type", name, *self.backend.discovery).stdout.strip(), expected_type)
        self.assertIn(name, self.command("service", "find", expected_type, *self.backend.discovery).stdout.splitlines())

    def test_real_response_invalid_request_and_missing_service(self):
        for number in [7, 42, 1000000]:
            request = dict(self.payload, sequence_num=number) if self.dds else {"a": number, "b": -3}
            expected = {"code": number, "msg": "diag"} if self.dds else {"sum": number - 3}
            self.assertEqual(json.loads(self.call(request).stdout), expected)
        self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 3)
        self.assertFalse(self.call({"unknown": 1}, code=1).stdout)
        self.assertFalse(self.call(request, code=1, missing=True).stdout)
        self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 3)

    def test_timeout_single_send_recovery_and_server_exit(self):
        request = dict(self.payload, frame_id="delay") if self.dds else {"a": -999, "b": 2, "delay_ms": 800}
        if self.ros:
            request.pop("delay_ms")
        self.assertFalse(self.call(request, code=1, timeout=".1").stdout)
        self.wait_for(lambda: len(rows(self.server.directory / "rpc-done.jsonl")) == 1, "late RPC completion")
        self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 1)
        request = self.payload if self.dds else {"a": 19, "b": 23}
        expected = {"code": 42, "msg": "diag"} if self.dds else {"sum": 42}
        self.assertEqual(json.loads(self.call(request).stdout), expected)
        if not self.ros:
            failed = dict(self.payload, frame_id="fail") if self.dds else {"a": 1, "b": 2, "status": 1008}
            self.assertFalse(self.call(failed, code=1).stdout)
            self.assertEqual(len(rows(self.server.directory / "rpc.jsonl")), 3)
        self.server.stop()
        self.assertFalse(self.call(request, code=1, timeout=".2").stdout)


def invalid_tls_flags(case, flags):
    """Missing trust, a different CA, and absent client credentials must fail."""
    missing = list(flags)
    index = missing.index("--mqtt-truststore")
    del missing[index:index + 2]
    yield "missing trust", missing
    certificate, key = case.root / "untrusted.pem", case.root / "untrusted-key.pem"
    with (case.root / "untrusted-certificate.log").open("w") as output:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=unrelated-authority", "-out", str(certificate), "-keyout", str(key)],
                       check=True, stdout=output, stderr=subprocess.STDOUT, timeout=15)
    wrong = list(flags)
    wrong[wrong.index("--mqtt-truststore") + 1] = str(certificate)
    yield "untrusted server", wrong
    if case.options.mqtt_tls == "mutual":
        missing = list(flags)
        for option in ["--mqtt-client-cert", "--mqtt-client-key"]:
            index = missing.index(option)
            del missing[index:index + 2]
        yield "missing client certificate", missing


class TlsService(ServiceNetwork):
    def test_invalid_discovery_credentials_cannot_report_success(self):
        for reason, flags in invalid_tls_flags(self, self.backend.discovery):
            with self.subTest(reason=reason):
                for query in [("list",), ("type", self.service_name), ("find", self.service_type)]:
                    result = self.command("service", *query, *flags, code=1)
                    self.assertFalse(result.stdout)
        self.assertIn(self.service_name,
                      self.command("service", "list", *self.backend.discovery).stdout.splitlines())

    def test_invalid_tls_credentials_cannot_send_requests(self):
        original = list(self.backend.flags)
        try:
            for reason, flags in invalid_tls_flags(self, original):
                with self.subTest(reason=reason):
                    self.backend.flags = flags
                    self.assertFalse(self.call({"a": 19, "b": 23}, code=1).stdout)
                    self.assertEqual(rows(self.server.directory / "rpc.jsonl"), [])
        finally:
            self.backend.flags = original
        self.assertEqual(json.loads(self.call({"a": 19, "b": 23}).stdout), {"sum": 42})


class Qos(Network):
    def check_policy(self, policy, incompatible=True):
        self.server.stop()
        if self.ros:
            self.server = self.launch_network(label="qos", policy=policy)
            good_path, bad_path = self.server.directory / "good.jsonl", self.server.directory / "bad.jsonl"
        else:
            offered = {"reliability": "reliable", "durability": "volatile", "depth": 10}
            requested = dict(offered)
            if policy == "reliability":
                offered[policy] = "best_effort"
            elif policy == "durability":
                requested[policy] = "transient_local"
            elif policy == "depth":
                requested[policy] = 3
            self.server = self.launch_network("publisher", {"publishers": offered}, "qos-publisher")
            good = self.launch_network("subscriber", {"subscribers": offered}, "qos-compatible")
            bad = self.launch_network("subscriber", {"subscribers": requested}, "qos-requested")
            good_path, bad_path = good.directory / "received.jsonl", bad.directory / "received.jsonl"
        self.wait_for(lambda: len(rows(good_path)) >= 5, "compatible QoS actually delivers", 10)
        if not incompatible:
            self.wait_for(lambda: len(rows(bad_path)) >= 5, "different depth still delivers", 10)
        flags = [*self.backend.discovery, "--report", "--include-warnings"]
        if self.dds:
            control = self.launch_runtime()
            flags += ["--http", f"127.0.0.1:{control.ports['http']}", "--log-backend", "http", "--service", "diag/logs"]
        result = self.command("doctor", *flags, code=1 if incompatible else 0, timeout=18)
        if incompatible:
            self.assertRegex(result.stdout, r'\[WARN\] qos[^\n]*policy=' + policy)
            self.assertEqual(rows(bad_path), [], "incompatible QoS unexpectedly delivered")
        else:
            self.assertNotRegex(result.stdout, r'\[WARN\] qos')
        self.assertNotIn("[UNKNOWN]", result.stdout)
        self.assertGreaterEqual(len(rows(good_path)), 5)

    def test_reliability_mismatch_warns_and_blocks_only_incompatible_reader(self):
        self.check_policy("reliability")

    def test_durability_mismatch_warns_and_blocks_only_incompatible_reader(self):
        self.check_policy("durability")

    def test_different_history_depth_is_compatible_and_delivers(self):
        self.check_policy("depth", incompatible=False)


class RosQos(Qos):
    def test_deadline_mismatch_is_reported_and_blocks_delivery(self):
        self.check_policy("deadline")

    def test_liveliness_kind_mismatch_is_reported_and_blocks_delivery(self):
        self.check_policy("liveliness")

    def test_liveliness_lease_mismatch_is_reported_and_blocks_delivery(self):
        self.check_policy("liveliness_lease_duration")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=["topic", "service", "qos"], required=True)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--mqtt-tls", default="off", choices=["off", "server", "mutual"])
    for name in ["cli", "server", "parameter", "typed-server", "types", "dds-types", "ros-types", "native-ros",
                 "net", "dds", "ros2", "grpc", "mqtt", "zenoh", "iceoryx", "log", "mosquitto", "roudi", "artifacts"]:
        parser.add_argument("--" + name, required=True)
    support.OPTIONS = parser.parse_args()
    Path(support.OPTIONS.artifacts).mkdir(parents=True, exist_ok=True)
    support.OPTIONS.artifacts = tempfile.mkdtemp(prefix="run-", dir=support.OPTIONS.artifacts)
    qos = RosQos if support.OPTIONS.backend == "ros2" else Qos
    tls = support.OPTIONS.mqtt_tls != "off"
    suite = unittest.defaultTestLoader.loadTestsFromTestCase({"topic": TopicNetwork,
        "service": TlsService if tls else ServiceNetwork, "qos": qos}[support.OPTIONS.case])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(f"Evidence: {support.OPTIONS.artifacts}")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
