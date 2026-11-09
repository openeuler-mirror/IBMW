"""Compare doctor QoS judgments with native DDS endpoint delivery."""
import argparse
from pathlib import Path
import sys
import tempfile
import unittest

import support
from support import FunctionalCase, free_port, rows


class NativeQos(FunctionalCase):
    def check(self, policy, incompatible=True):
        directory = self.root / policy
        directory.mkdir()
        domain = 40 + free_port() % 130
        self.launch_process([self.options.qos_peer, domain, policy, directory], directory)
        self.wait_for(lambda: len(rows(directory / "good.jsonl")) >= 5, "native compatible DDS delivery", 10)
        if not incompatible:
            self.wait_for(lambda: len(rows(directory / "requested.jsonl")) >= 5, "native compatible requested DDS delivery", 10)
        result = self.command("doctor", "--dds", "--domain", domain, "--timeout", "2",
                              "--http", f"127.0.0.1:{self.server.ports['http']}", "--log-backend", "http",
                              "--service", "diag/logs", "--report", "--include-warnings",
                              code=1 if incompatible else 0, timeout=18)
        if incompatible:
            self.assertRegex(result.stdout, r'\[WARN\] qos[^\n]*policy=' + policy)
            self.assertEqual(rows(directory / "requested.jsonl"), [])
        else:
            self.assertNotRegex(result.stdout, r'\[WARN\] qos')
        self.assertNotIn("[UNKNOWN]", result.stdout)
        transmitted = {item["sequence"] for item in rows(directory / "sent.jsonl")}
        self.assertTrue({item["sequence"] for item in rows(directory / "good.jsonl")} <= transmitted)

    def test_deadline(self): self.check("deadline")
    def test_liveliness(self): self.check("liveliness")
    def test_liveliness_lease_duration(self): self.check("liveliness_lease_duration")
    def test_ownership(self): self.check("ownership")
    def test_partition(self): self.check("partition")
    def test_partition_wildcard(self): self.check("partition_wildcard", False)
    def test_history_depth(self): self.check("depth", False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ["cli", "server", "types", "net", "parameter", "log", "qos-peer", "artifacts"]:
        parser.add_argument("--" + option, required=True)
    support.OPTIONS = parser.parse_args()
    Path(support.OPTIONS.artifacts).mkdir(parents=True, exist_ok=True)
    support.OPTIONS.artifacts = tempfile.mkdtemp(prefix="run-", dir=support.OPTIONS.artifacts)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(NativeQos))
    print(f"Evidence: {support.OPTIONS.artifacts}")
    sys.exit(0 if result.wasSuccessful() else 1)
