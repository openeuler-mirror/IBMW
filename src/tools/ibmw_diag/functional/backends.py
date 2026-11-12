"""Private real transport environments shared by the functional scenarios."""
import json
import os
from pathlib import Path
import socket
import subprocess

from support import Process, free_port


class Backend:
    def __init__(self, case, name):
        self.case, self.name = case, name
        self.directory = case.root / ("backend-" + name)
        self.directory.mkdir(exist_ok=True)
        self.port = free_port()
        self.domain = 40 + self.port % 130
        self.identity = "diag-" + case.root.name
        self.flags = ["--timeout", "2", "--discovery-timeout", "3"]
        self.discovery = ["--timeout", "1.2"]
        self.settings, self.driver = {}, {}
        extension = "net" if name in ("http", "tcp", "udp") else name
        self.library = getattr(case.options, extension)
        self.extension_name = extension + "_extension"
        if name in ("http", "grpc"):
            self.settings = {"thread_num": 2}
            address = {"listen_ip": "127.0.0.1", "listen_port": self.port}
            self.settings.update({"http_options": address} if name == "http" else address)
            self.flags += ["--" + name, f"127.0.0.1:{self.port}"]
            self.discovery += ["--" + name, f"127.0.0.1:{self.port}"]
        elif name == "dds":
            self.settings = {"domain_id": self.domain}
            self.driver = {"ros2_compatible": False}
            self.flags += ["--dds", "--domain", str(self.domain)]
            self.discovery += ["--dds", "--domain", str(self.domain)]
        elif name == "ros2":
            self.settings = {"node_name": self.identity.replace("-", "_"), "domain_id": self.domain}
            self.flags += ["--ros2", "--domain", str(self.domain)]
            self.discovery += ["--ros2", "--domain", str(self.domain)]
            case.env.update(ROS_DOMAIN_ID=str(self.domain), ROS_LOCALHOST_ONLY="0",
                            ROS_LOG_DIR=str(self.directory / "ros-logs"))
        elif name == "zenoh":
            server_config = self.directory / "server.json5"
            client_config = self.directory / "client.json5"
            common = {"mode": "peer", "scouting": {"multicast": {"enabled": False}}}
            endpoint = f"tcp/127.0.0.1:{self.port}"
            server_config.write_text(json.dumps(dict(common, listen={"endpoints": [endpoint]})))
            client_config.write_text(json.dumps(dict(common, connect={"endpoints": [endpoint]})))
            self.settings = {"limit_domain": self.identity, "native_cfg_path": str(server_config)}
            self.driver = {"servers": [{"func_name": ".*", "shm_enabled": False}]}
            self.flags += ["--zenoh", "--zenoh-domain", self.identity, "--zenoh-config", str(client_config)]
            self.discovery += self.flags[4:]
        elif name == "mqtt":
            config = self.directory / "mosquitto.conf"
            tls = getattr(case.options, "mqtt_tls", "off")
            broker_config = f"listener {self.port} 127.0.0.1\nallow_anonymous true\npersistence false\nlog_type all\n"
            credentials = []
            if tls != "off":
                certificate, key = self.directory / "certificate.pem", self.directory / "key.pem"
                with (self.directory / "certificates.log").open("w") as output:
                    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                                    "-keyout", str(key), "-out", str(certificate), "-days", "1",
                                    "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"],
                                   check=True, stdout=output, stderr=subprocess.STDOUT, timeout=15)
                broker_config += f"cafile {certificate}\ncertfile {certificate}\nkeyfile {key}\n"
                credentials = ["--mqtt-truststore", str(certificate)]
                self.settings["truststore"] = str(certificate)
                if tls == "mutual":
                    broker_config += "require_certificate true\n"
                    credentials += ["--mqtt-client-cert", str(certificate), "--mqtt-client-key", str(key)]
                    self.settings.update(client_cert=str(certificate), client_key=str(key))
            config.write_text(broker_config)
            self.broker_config = config
            self.start_mqtt_broker(self.directory)
            broker = f"{'tcp' if tls == 'off' else 'ssl'}://127.0.0.1:{self.port}"
            self.settings.update(broker_addr=broker, client_id=self.identity)
            self.driver = {"servers": [{"func_name": ".*"}]}
            self.flags += ["--mqtt", broker, "--mqtt-server-id", self.identity, *credentials]
            self.discovery += ["--mqtt", broker, *credentials]
            self.credentials = credentials
        elif name == "iceoryx":
            # Never attach to, stop, or clean a pre-existing user RouDi.
            existing = subprocess.run(["pgrep", "-x", "iox-roudi"], capture_output=True)
            if existing.returncode == 0:
                raise AssertionError("A RouDi is already running; require an isolated test host")
            config = self.directory / "roudi.toml"
            config.write_text('[general]\nversion = 1\n[[segment]]\n[[segment.mempool]]\nsize = 4096\ncount = 512\n')
            process = Process([case.options.roudi, "-c", str(config)], self.directory, case.env)
            case.processes.append(process)
            case.wait_for(lambda: "RouDi is ready" in process.path.read_text(), "private RouDi readiness", 15)
            self.settings = {"runtime_id": self.identity, "shm_init_size": 2048}
            self.flags += ["--iceoryx", "--iceoryx-runtime-id", self.identity + "-cli"]
            self.discovery = ["--registry-path", str(case.registry)]
        else:
            raise ValueError(name)

    def start_mqtt_broker(self, directory):
        process = Process([self.case.options.mosquitto, "-c", str(self.broker_config)], directory, self.case.env)
        self.case.processes.append(process)

        def listening():
            if process.child.poll() is not None:
                raise AssertionError(process.path.read_text())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return True
            except OSError:
                return False

        self.case.wait_for(listening, "private MQTT broker readiness")

    def extension(self):
        return {"name": self.extension_name, "path": self.library, "options": self.settings}

    def topic_flags(self):
        if self.name == "mqtt":
            return ["--mqtt", "--broker", self.settings["broker_addr"], "--client-id", self.identity + "-cli", *self.credentials]
        if self.name in ("dds", "ros2", "zenoh", "iceoryx"):
            return self.flags[4:]
        raise ValueError(self.name)

    def messaging_options(self):
        if self.name == "dds":
            return {"ros2_compatible": False}
        return {}
