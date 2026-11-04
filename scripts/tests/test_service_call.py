#!/usr/bin/env python3
"""Exercise service call against independent server processes and real transports."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@contextlib.contextmanager
def process(command, directory, label, ready=None, env=None):
    path = directory / (label + ".log")
    with path.open("w") as log:
        child = subprocess.Popen(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True, env=env)
        try:
            if ready:
                deadline = time.monotonic() + 20
                while ready not in path.read_text():
                    if child.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError(f"{label} did not become ready:\n{path.read_text()}")
                    time.sleep(0.02)
            yield child
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--types", required=True)
    parser.add_argument("--dds-types")
    parser.add_argument("--backend", required=True, choices=["http", "grpc", "dds", "mqtt", "zenoh", "ros2", "dds_ros2"])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ibmw-service-call-") as tmp, contextlib.ExitStack() as stack:
        directory = Path(tmp)
        backend = args.backend
        library_dir = Path(args.cli).parent
        domain = 30 + os.getpid() % 150
        endpoint = port()
        counter = directory / "requests.jsonl"
        if backend == "mqtt":
            broker_cfg = directory / "mosquitto.conf"
            broker_cfg.write_text(f"listener {endpoint} 127.0.0.1\nallow_anonymous true\npersistence false\n")
            stack.enter_context(process(["mosquitto", "-c", str(broker_cfg)], directory, "broker"))
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", endpoint), timeout=.1):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise AssertionError("MQTT broker did not start")
                    time.sleep(.02)

        dds_types = backend in ("dds", "grpc")
        req_type = "dds:ibmw::protocols::common::Header" if dds_types else "json:service_call.Request"
        rsp_type = "dds:ibmw::protocols::common::Error" if dds_types else "json:service_call.Response"
        types = args.dds_types if dds_types else args.types
        name = "dds:/service_call/Echo" if dds_types else "json:/service_call/Add"
        request = {"time_stamp": 12, "sequence_num": 42, "frame_id": "call-test"} if dds_types else {"a": 19, "b": 23}
        expected = {"code": 42, "msg": "call-test"} if dds_types else {"sum": 42}
        flags = ["--timeout", "2", "--discovery-timeout", "5"]
        if backend in ("http", "grpc"):
            flags += ["--" + backend, f"127.0.0.1:{endpoint}"]
        elif backend == "mqtt":
            flags += ["--mqtt", f"tcp://127.0.0.1:{endpoint}", "--mqtt-server-id", "service-call-server"]
        elif backend == "zenoh":
            flags += ["--zenoh", "--zenoh-domain", f"call-{os.getpid()}"]
        else:
            flags += ["--" + ("dds" if backend == "dds_ros2" else backend), "--domain", str(domain)]

        if backend in ("ros2", "dds_ros2"):
            native_server = directory / "native_server.py"
            native_server.write_text('''import rclpy
from example_interfaces.srv import AddTwoInts
rclpy.init()
node = rclpy.create_node("ibmw_call_native_server")
def handle(req, rsp):
    rsp.sum = req.a + req.b
    return rsp
service = node.create_service(AddTwoInts, "/service_call_add", handle)
print("READY", flush=True)
try:
    rclpy.spin(node)
except KeyboardInterrupt:
    pass
''')
            env = dict(os.environ, ROS_DOMAIN_ID=str(domain), ROS_LOG_DIR=str(directory / "ros_logs"))
            stack.enter_context(process([sys.executable, str(native_server)], directory, "server", "READY", env))
            name = "/service_call_add"
            type_arg = "example_interfaces/srv/AddTwoInts"
            if backend == "dds_ros2":
                type_arg = "ros2:example_interfaces/srv/AddTwoInts_Request -> ros2:example_interfaces/srv/AddTwoInts_Response"
                flags += ["--ros2-compatible", "--type-support", args.dds_types]
        else:
            extension_name = "net" if backend == "http" else backend
            settings = {"thread_num": 2} if backend in ("http", "grpc") else {}
            if backend == "http": settings["http_options"] = {"listen_ip": "127.0.0.1", "listen_port": endpoint}
            if backend == "grpc": settings.update(listen_ip="127.0.0.1", listen_port=endpoint)
            if backend == "dds": settings["domain_id"] = domain
            if backend == "mqtt": settings.update(broker_addr=f"tcp://127.0.0.1:{endpoint}", client_id="service-call-server")
            if backend == "zenoh": settings["limit_domain"] = f"call-{os.getpid()}"
            driver_options = {"ros2_compatible": False} if backend == "dds" else {}
            if backend == "mqtt": driver_options["servers"] = [{"func_name": ".*"}]
            if backend == "zenoh": driver_options["servers"] = [{"func_name": ".*", "shm_enabled": False}]
            config = {"ibmw": {
                "logging": {"core_level": "OFF", "backends": []},
                "runtime_graph": {"enabled": False},
                "extensions": {"items": [{"name": extension_name + "_extension", "path": str(library_dir / f"libibmw_{extension_name}_transport.so"), "options": settings}]},
                "service": {"drivers": [{"type": backend, "options": driver_options}], "servers": [{"func_name": ".*", "enable_drivers": [backend]}]},
            }}
            cfg = directory / "server.json"
            cfg.write_text(json.dumps(config))
            stack.enter_context(process([args.server, str(cfg), types, req_type, rsp_type, name, str(counter), ""], directory, "server", "READY"))
            type_arg = req_type + " -> " + rsp_type
            flags += ["--type-support", types]

        def command(payload=request, service=name, extra=()):
            return [args.cli, "service", "call", service, type_arg, json.dumps(payload), *flags, *extra]

        def run(command_line):
            return subprocess.run(command_line, cwd=directory, capture_output=True, text=True, timeout=12)

        for _ in range(2):
            result = run(command())
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            assert json.loads(result.stdout) == expected, result.stdout
        if backend not in ("ros2", "dds_ros2"):
            assert len(counter.read_text().splitlines()) == 2, "Unexpected request duplication"

        bad = run(command({"unknown_field": 1}))
        assert bad.returncode != 0 and not bad.stdout and "Calling " not in bad.stderr, bad
        if backend not in ("ros2", "dds_ros2"):
            assert len(counter.read_text().splitlines()) == 2

        absent = run(command(service=name + "Missing"))
        assert absent.returncode != 0 and not absent.stdout, absent

        if backend in ("dds", "ros2", "dds_ros2", "zenoh"):
            for stop_signal in (signal.SIGINT, signal.SIGTERM):
                err = directory / "readiness-interrupt.err"
                with err.open("w") as stderr:
                    child = subprocess.Popen(command(service=name + "Missing"), cwd=directory,
                                             stdout=subprocess.PIPE, stderr=stderr, text=True)
                    try:
                        deadline = time.monotonic() + 6
                        while "Waiting for client readiness" not in err.read_text():
                            if child.poll() is not None or time.monotonic() > deadline:
                                raise AssertionError("Client did not enter readiness wait: " + err.read_text())
                            time.sleep(.02)
                        child.send_signal(stop_signal)
                        stdout, _ = child.communicate(timeout=5)
                        assert child.returncode == 128 + stop_signal and not stdout, (child.returncode, stdout, err.read_text())
                        assert "no request was sent" in err.read_text() and "Calling " not in err.read_text(), err.read_text()
                    finally:
                        if child.poll() is None: child.kill(); child.wait()
            if backend in ("dds", "zenoh"):
                assert len(counter.read_text().splitlines()) == 2

        if backend in ("http", "mqtt", "zenoh"):
            failed = run(command({"a": 1, "b": 2, "status": 1008}))
            assert failed.returncode == 1 and not failed.stdout, failed
            # HTTP's existing driver maps non-200 responses to a transport
            # status rather than carrying the original application status.
            if backend != "http": assert "1008" in failed.stderr, failed.stderr
            assert len(counter.read_text().splitlines()) == 3, "Failed request was retried"
            slow = command({"a": 1, "b": 2, "delay_ms": 1000})
            slow[slow.index("--timeout") + 1] = "0.1"
            result = run(slow)
            assert result.returncode != 0 and not result.stdout, result
            time.sleep(1.1)
            assert len(counter.read_text().splitlines()) == 4, "Timed-out request was retried"
            # After a late response, a new client process must still work.
            result = run(command())
            assert result.returncode == 0 and json.loads(result.stdout) == expected, result
            err = directory / "interrupt.err"
            with err.open("w") as stderr:
                child = subprocess.Popen(command({"a": 1, "b": 2, "delay_ms": 1200}), cwd=directory,
                                         stdout=subprocess.PIPE, stderr=stderr, text=True)
                try:
                    deadline = time.monotonic() + 6
                    while len(counter.read_text().splitlines()) < 6:
                        if child.poll() is not None or time.monotonic() > deadline:
                            raise AssertionError("Interrupted request never reached server: " + err.read_text())
                        time.sleep(.02)
                    child.send_signal(signal.SIGINT)
                    stdout, _ = child.communicate(timeout=5)
                    assert child.returncode == 130 and not stdout, (child.returncode, stdout, err.read_text())
                finally:
                    if child.poll() is None: child.kill(); child.wait()
        print(f"PASS {backend}: response, restart, invalid input, absent service" +
              (", cancel before send" if backend in ("dds", "ros2", "dds_ros2", "zenoh") else "") +
              (", failure status, timeout, late response, SIGINT, single send" if backend in ("http", "mqtt", "zenoh") else ""))


if __name__ == "__main__":
    main()
