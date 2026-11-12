#!/usr/bin/env python3
"""Permanent regression coverage for log CLI against real service transports."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from test_service_call import port, process


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--backend", required=True, choices=["http", "dds", "grpc", "mqtt", "zenoh"])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ibmw-log-regression-") as tmp, contextlib.ExitStack() as stack:
        directory = Path(tmp)
        libraries = Path(args.cli).parent
        backend = args.backend
        endpoint = port()
        domain = 30 + os.getpid() % 150
        service_name = "regression/logs"
        flags = ["--service", service_name, "--timeout", "2", "--discovery-timeout", "5"]
        settings = {}
        driver_options = {}
        if backend in ("http", "grpc"):
            flags += ["--" + backend, f"127.0.0.1:{endpoint}"]
            settings["thread_num"] = 2
            if backend == "http":
                settings["http_options"] = {"listen_ip": "127.0.0.1", "listen_port": endpoint}
            else:
                settings.update(listen_ip="127.0.0.1", listen_port=endpoint)
        elif backend == "dds":
            flags += ["--dds", "--domain", str(domain)]
            settings["domain_id"] = domain
            driver_options["ros2_compatible"] = False
        elif backend == "zenoh":
            flags += ["--zenoh", "--zenoh-domain", f"log-{os.getpid()}"]
            settings["limit_domain"] = f"log-{os.getpid()}"
            driver_options["servers"] = [{"func_name": ".*", "shm_enabled": False}]
        else:
            cfg = directory / "mosquitto.conf"
            cfg.write_text(f"listener {endpoint} 127.0.0.1\nallow_anonymous true\npersistence false\n")
            stack.enter_context(process(["mosquitto", "-c", str(cfg)], directory, "broker"))
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", endpoint), timeout=.1):
                        break
                except OSError:
                    assert time.monotonic() < deadline, "MQTT broker did not start"
                    time.sleep(.02)
            broker = f"tcp://127.0.0.1:{endpoint}"
            flags += ["--mqtt", broker, "--mqtt-server-id", "log-server"]
            settings.update(broker_addr=broker, client_id="log-server")
            driver_options["servers"] = [{"func_name": ".*"}]

        extension = "net" if backend == "http" else backend
        config = {"ibmw": {
            "logging": {"core_level": "OFF", "default_module_level": "INFO",
                        "backends": [{"type": "console", "options": {"color": False, "pattern": "%v"}}]},
            "runtime_graph": {"enabled": False},
            "extensions": {"items": [
                {"name": extension + "_extension", "path": str(libraries / f"libibmw_{extension}_transport.so"), "options": settings},
                {"name": "log_control_extension", "path": str(libraries / "libibmw_log_control.so"),
                 "options": {"service_name": service_name}}]},
            "service": {"drivers": [{"type": backend, "options": driver_options}],
                        "servers": [{"func_name": ".*", "enable_drivers": [backend]}]},
        }}
        cfg = directory / "server.json"
        cfg.write_text(json.dumps(config))
        original_config = cfg.read_bytes()
        probe = directory / "probe.txt"

        def run(verb, *arguments):
            return subprocess.run([args.cli, "log", verb, *arguments, *flags], cwd=directory,
                                  capture_output=True, text=True, timeout=15)

        def success(verb, *arguments):
            result = run(verb, *arguments)
            if result.returncode != 0:
                logs = {p.name: p.read_text()[-8000:] for p in directory.glob("*.log")}
                raise AssertionError((result.args, result.returncode, result.stdout, result.stderr, logs))
            return result.stdout

        def check_filter(token, threshold):
            pending = directory / "probe.pending"
            pending.write_text(token + "\n")
            pending.replace(probe)
            deadline = time.monotonic() + 3
            while True:
                lines = (directory / "server.log").read_text().splitlines()
                if token + ":END" in lines:
                    break
                assert time.monotonic() < deadline, "Logger did not finish the probe"
                time.sleep(.02)
            actual = [line for line in lines if line.startswith(token + ":") and line != token + ":END"]
            assert actual == [f"{token}:{level}" for level in range(threshold, 6)], actual

        with process([args.server, str(cfg), str(probe)], directory, "server", "READY"):
            assert success("list", "CarModule") == "CarModule INFO\n"
            assert success("list", "OtherModule") == "OtherModule INFO\n"
            lines = success("list").splitlines()
            assert lines == sorted(lines), lines
            for line in ["CarModule INFO", "OtherModule INFO", "core OFF", "中文模块 OFF", "x" * 2048 + " OFF"]:
                assert line in lines, line
            check_filter("initial", 2)
            for threshold, level in enumerate(["trace", "DeBuG", "INFO", "warn", "error", "Fatal", "OFF"]):
                assert success("set", "CarModule", level) == "Set log level successful\n"
                assert success("list", "CarModule") == f"CarModule {level.upper()}\n"
                check_filter(f"level-{threshold}", threshold)
            assert success("list", "OtherModule") == "OtherModule INFO\n"
            assert success("list", "core") == "core OFF\n"
            success("set", "core", "WARN")
            assert success("list", "core") == "core WARN\n"
            for name in ["中文模块", "x" * 2048]:
                success("set", name, "DEBUG")
                assert success("list", name) == name + " DEBUG\n"
            for verb, arguments, reason in [
                ("set", ["MissingModule", "INFO"], "INVALID_MODULE_NAME"),
                ("list", ["MissingModule"], "INVALID_MODULE_NAME"),
                ("set", ["CarModule", "DEBIG"], "Level must be"),
            ]:
                result = run(verb, *arguments)
                assert result.returncode != 0 and not result.stdout and reason in result.stderr, result
            assert success("list", "CarModule") == "CarModule OFF\n"
            assert cfg.read_bytes() == original_config, "set modified the startup configuration"

        # Runtime changes must disappear after restart, and the default service name works too.
        config["ibmw"]["extensions"]["items"][1]["options"] = {}
        cfg.write_text(json.dumps(config))
        flags = flags[2:]  # Remove the custom --service pair.
        with process([args.server, str(cfg), str(probe)], directory, "restarted-server", "READY"):
            assert success("list", "CarModule") == "CarModule INFO\n"
            assert success("list", "core") == "core OFF\n"
        unavailable = run("list")
        assert unavailable.returncode != 0 and not unavailable.stdout, unavailable
        print(f"log {backend}: list/set, isolation, all levels, actual filtering, Unicode/large names, errors, restart and unavailable server passed")


if __name__ == "__main__":
    main()
