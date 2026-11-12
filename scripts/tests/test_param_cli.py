#!/usr/bin/env python3
"""Permanent regression coverage for param CLI against real service transports."""
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
    with tempfile.TemporaryDirectory(prefix="ibmw-param-regression-") as tmp, contextlib.ExitStack() as stack:
        directory = Path(tmp)
        libraries = Path(args.cli).parent
        backend = args.backend
        endpoint = port()
        domain = 30 + os.getpid() % 150
        service_name = "regression/parameters"
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
            flags += ["--zenoh", "--zenoh-domain", f"param-{os.getpid()}"]
            settings["limit_domain"] = f"param-{os.getpid()}"
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
            flags += ["--mqtt", broker, "--mqtt-server-id", "parameter-server"]
            settings.update(broker_addr=broker, client_id="parameter-server")
            driver_options["servers"] = [{"func_name": ".*"}]

        extension = "net" if backend == "http" else backend
        config = {"ibmw": {
            "logging": {"core_level": "OFF", "backends": []},
            "runtime_graph": {"enabled": False},
            "extensions": {"items": [
                {"name": extension + "_extension", "path": str(libraries / f"libibmw_{extension}_transport.so"), "options": settings},
                {"name": "parameter_extension", "path": str(libraries / "libibmw_parameter_service.so"),
                 "options": {"service_name": service_name}}]},
            "service": {"drivers": [{"type": backend, "options": driver_options}],
                        "servers": [{"func_name": ".*", "enable_drivers": [backend]}]},
        }}
        cfg = directory / "server.json"
        cfg.write_text(json.dumps(config))
        original_config = cfg.read_bytes()
        observed = directory / "module-observed.txt"

        def run(verb, *arguments, extra=(), value=None):
            command = [args.cli, "param", verb, *arguments, *flags, *extra]
            if value is not None:
                command += ["--", value]
            result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=15)
            return result

        def success(verb, *arguments, **kwargs):
            result = run(verb, *arguments, **kwargs)
            if result.returncode != 0:
                logs = {p.name: p.read_text()[-8000:] for p in directory.glob("*.log")}
                raise AssertionError((result.args, result.returncode, result.stdout, result.stderr, logs))
            return result.stdout

        with process([args.server, str(cfg), str(observed)], directory, "server", "READY"):
            assert success("list", "CarModule").splitlines() == ["controller.gain", "speed"]
            assert success("get", "CarModule", "speed") == "String value is: 0.2\n"
            assert success("get", "OtherModule", "speed", extra=["--hide-type"]) == "7\n"
            grouped = success("list")
            assert "CarModule:\n  controller.gain\n  speed\n" in grouped, grouped
            assert "OtherModule:\n  speed\n" in grouped, grouped
            assert success("list", "CarModule", extra=["--filter", "^speed$", "--param-type"]) == "speed (string)\n"
            assert success("list", "CarModule", extra=["--param-prefixes", "controller"]) == "controller.gain\n"
            # Exercise variable-size request/reply payloads beyond DDS's initial allocation.
            for value in ["0.5", "", "false", "[1, 2]", "中文 with spaces", "-1.5", "--help", "long-value-" * 1024]:
                assert success("set", "CarModule", "speed", value=value) == "Set parameter successful\n"
                assert success("get", "CarModule", "speed", extra=["--hide-type"]) == value + "\n"
                deadline = time.monotonic() + 2
                while not observed.exists() or observed.read_text() != value:
                    assert time.monotonic() < deadline, "Module did not read the updated parameter"
                    time.sleep(.02)
            assert success("get", "OtherModule", "speed", extra=["--hide-type"]) == "7\n"
            success("set", "CarModule", "new_parameter", value="created")
            assert "new_parameter" in success("list", "CarModule").splitlines()
            for verb, arguments, reason in [
                ("get", ["CarModule", "missing"], "PARAMETER_NOT_FOUND"),
                ("get", ["MissingModule", "speed"], "INVALID_MODULE_NAME"),
                ("set", ["MissingModule", "speed", "2"], "INVALID_MODULE_NAME"),
                ("list", ["MissingModule"], "INVALID_MODULE_NAME"),
            ]:
                result = run(verb, *arguments)
                assert result.returncode != 0 and not result.stdout and reason in result.stderr, result
            assert cfg.read_bytes() == original_config, "set modified the startup configuration"

        # Runtime changes are intentionally not persisted across a server restart.
        with process([args.server, str(cfg), str(observed)], directory, "restarted-server", "READY"):
            assert success("get", "CarModule", "speed", extra=["--hide-type"]) == "0.2\n"
            missing = run("get", "CarModule", "new_parameter")
            assert missing.returncode != 0 and "PARAMETER_NOT_FOUND" in missing.stderr, missing
        unavailable = run("get", "CarModule", "speed")
        assert unavailable.returncode != 0 and not unavailable.stdout, unavailable
        print(f"param {backend}: list/get/set, module isolation, literal strings, errors, live reads, restart and unavailable server passed")


if __name__ == "__main__":
    main()
