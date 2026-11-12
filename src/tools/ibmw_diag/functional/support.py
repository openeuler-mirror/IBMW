"""Process isolation and evidence capture; never substitute CLI/RPC implementations."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

OPTIONS = None


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def rows(path):
    if not path.exists():
        return []
    # A writer may be appending its last record. Only consume complete lines.
    content = path.read_text()
    return [json.loads(line) for line in content.splitlines(keepends=True)
            if line.endswith("\n") and line.strip()]


class Process:
    def __init__(self, command, directory, env):
        self.directory = directory
        self.path = directory / "server.log"
        self.log = self.path.open("w")
        try:
            self.child = subprocess.Popen(command, cwd=directory, env=env,
                                          stdout=self.log, stderr=subprocess.STDOUT,
                                          start_new_session=True)
        except BaseException:
            self.log.close()
            raise
        self.stopped = False

    def stop(self):
        if self.stopped:
            return
        self.stopped = True
        forced = False
        try:
            if self.child.poll() is None:
                os.killpg(self.child.pid, signal.SIGTERM)
            try:
                self.child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                forced = True
                os.killpg(self.child.pid, signal.SIGKILL)
                self.child.wait(timeout=3)
        finally:
            self.log.close()
        if forced or self.child.returncode != 0:
            raise AssertionError(f"Runtime did not shut down cleanly: {self.child.returncode}\n"
                                 + self.path.read_text()[-12000:])


class FunctionalCase(unittest.TestCase):
    start_runtime = True

    def setUp(self):
        self.options = OPTIONS
        self.root = Path(tempfile.mkdtemp(prefix="ibmw-diag-"))
        self.registry = self.root / "registry"
        self.processes = []
        self.server_number = 0
        xdg = self.root / "xdg"
        xdg.mkdir(mode=0o700)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(xdg))
        # Exercise this build's deployed provider manifest, not a user's override.
        self.env.pop("IBMW_CLI_CONFIG", None)
        self.addCleanup(self.cleanup)
        if self.start_runtime:
            self.server = self.launch_runtime()

    def wait_for(self, condition, description, seconds=5):
        deadline = time.monotonic() + seconds
        while True:
            result = condition()
            if result:
                return result
            if time.monotonic() >= deadline:
                self.fail(f"Timed out: {description}; evidence: {self.root}")
            time.sleep(0.02)

    def launch_process(self, command, directory):
        process = Process([str(item) for item in command], directory, self.env)
        self.processes.append(process)

        def ready():
            if process.child.poll() is not None:
                self.fail("Runtime exited before READY:\n" + process.path.read_text())
            return "READY\n" in process.path.read_text()

        self.wait_for(ready, "runtime readiness", seconds=15)
        return process

    def launch_runtime(self, orphan=False):
        self.server_number += 1
        directory = self.root / f"runtime-{self.server_number}"
        directory.mkdir()
        ports = {"http": free_port(), "tcp": free_port(), "udp": free_port(socket.SOCK_DGRAM)}
        identity = f"runtime-{self.server_number}"
        config = {"ibmw": {
            "logging": {"core_level": "OFF", "default_module_level": "INFO",
                        "backends": [{"type": "console", "options": {"color": False, "pattern": "%v"}}]},
            "runtime_graph": {"enabled": True, "runtime_id": identity,
                              "registry_path": str(self.registry),
                              "type_support_pkgs": [self.options.types]},
            "extensions": {"items": [
                {"name": "net_extension", "path": self.options.net,
                 "options": {"thread_num": 2, **{
                     f"{name}_options": {"listen_ip": "127.0.0.1", "listen_port": port}
                     for name, port in ports.items()}}},
                {"name": "parameter_extension", "path": self.options.parameter,
                 "options": {"service_name": "diag/parameters"}},
                {"name": "log_control_extension", "path": self.options.log,
                 "options": {"service_name": "diag/logs"}}]},
            "messaging": {
                "drivers": [{"type": name} for name in ["local", "http", "tcp", "udp"]],
                "publishers": [{"topic": ".*", "enable_drivers": ["local"]}],
                "subscribers": [{"topic": "/diag/input", "enable_drivers": ["http", "tcp", "udp"]},
                                {"topic": ".*", "enable_drivers": ["local"]}]},
            "service": {"drivers": [{"type": "http"}],
                        "servers": [{"func_name": ".*", "enable_drivers": ["http"]}]}}}
        backend_name = getattr(self.options, "rpc_backend", "http")
        if backend_name != "http":
            from backends import Backend
            if not hasattr(self, "rpc_backend"):
                self.rpc_backend = Backend(self, backend_name)
            backend = self.rpc_backend
            config["ibmw"]["extensions"]["items"].append(backend.extension())
            config["ibmw"]["service"]["drivers"].append(
                {"type": backend_name, "options": backend.driver})
            config["ibmw"]["service"]["servers"] = [
                {"func_name": "json:/diag/math/.*", "enable_drivers": ["http"]},
                {"func_name": ".*", "enable_drivers": [backend_name]}]
        path = directory / "config.json"
        path.write_text(json.dumps(config, indent=2))
        process = self.launch_process([self.options.server, path, directory,
                                       "orphan" if orphan else "normal"], directory)
        process.ports = ports
        process.identity = identity
        process.config = path
        self.wait_for(lambda: (directory / "observed.json").exists(), "business observation")
        return process

    def command(self, *arguments, code=0, timeout=12):
        command = [self.options.cli, *map(str, arguments)]
        start = time.monotonic()
        try:
            result = subprocess.run(command, cwd=self.root, env=self.env,
                                    capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            with (self.root / "commands.jsonl").open("a") as log:
                log.write(json.dumps({"args": command, "timeout": timeout,
                                      "stdout": str(error.stdout), "stderr": str(error.stderr)}) + "\n")
            raise
        with (self.root / "commands.jsonl").open("a") as log:
            log.write(json.dumps({"args": command, "code": result.returncode,
                                  "stdout": result.stdout, "stderr": result.stderr,
                                  "elapsed": time.monotonic() - start}, ensure_ascii=False) + "\n")
        if code is not None:
            self.assertEqual(result.returncode, code,
                             f"{command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        return result

    def registry_command(self, *arguments, **kwargs):
        return self.command(*arguments, "--registry-path", self.registry, **kwargs)

    def connection(self, server=None):
        if hasattr(self, "rpc_backend"):
            return [*self.rpc_backend.flags, "--transport-library", self.rpc_backend.library]
        server = server or self.server
        return ["--http", f"127.0.0.1:{server.ports['http']}",
                "--timeout", "1", "--discovery-timeout", "2"]

    def param(self, verb, *args, code=0):
        return self.command("param", verb, *self.connection(), "--service", "diag/parameters", *args, code=code)

    def log(self, verb, *args, code=0):
        return self.command("log", verb, *self.connection(), "--service", "diag/logs", *args, code=code)

    def observed(self):
        return json.loads((self.server.directory / "observed.json").read_text())

    def probe_logs(self, token):
        pending = self.server.directory / "probe.pending"
        pending.write_text(token + "\n")
        pending.replace(self.server.directory / "probe.txt")
        self.wait_for(lambda: f"{token}:END" in self.server.path.read_text().splitlines(), "log probe completion")
        return [line for line in self.server.path.read_text().splitlines()
                if line.startswith(token + ":") and line != token + ":END"]

    def cleanup(self):
        # Retain live registration evidence before clean shutdown removes it.
        registrations = self.root / "registrations"
        for path in self.registry.glob("v2/*.json"):
            registrations.mkdir(exist_ok=True)
            shutil.copy2(path, registrations / path.name)
        errors = []
        for process in reversed(self.processes):
            try:
                process.stop()
            except Exception as error:
                errors.append(str(error))
        destination = Path(self.options.artifacts) / self.id().split(".", 1)[-1]
        destination.mkdir(parents=True, exist_ok=True)
        for path in self.root.rglob("*"):
            if path.is_file():
                target = destination / path.relative_to(self.root)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
        shutil.rmtree(self.root)
        if errors:
            self.fail("\n".join(errors))
