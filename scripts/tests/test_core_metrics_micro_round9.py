#!/usr/bin/env python3
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).parents[1] / "core_metrics_micro_round9.py"
SPEC = importlib.util.spec_from_file_location("round9", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)


class CoreMetricsMicroRound9Test(unittest.TestCase):
    def record(self, empty=1.0, null=2.0, noop=4.0, atomic=8.0):
        values = {"empty": empty, "null": null, "noop_abi": noop,
                  "atomic": atomic}
        return {"lanes": {name: {"ns_per_message": value}
                          for name, value in values.items()}}

    def test_registered_orders_are_exactly_balanced(self):
        orders = m.registered_orders()
        self.assertTrue(m.validate_orders(orders))
        self.assertEqual(len(orders), 48)
        self.assertEqual(orders, m.registered_orders())
        self.assertFalse(m.validate_orders(orders[:-1]))

    def test_paired_deltas_are_not_clamped(self):
        deltas = m.paired_deltas([self.record(empty=3, null=2)])
        self.assertEqual(deltas["null_empty"], [-1])
        self.assertEqual(deltas["noop_null"], [2])
        self.assertEqual(deltas["atomic_noop"], [4])
        self.assertEqual(deltas["atomic_null"], [6])
        self.assertEqual(deltas["atomic_empty"], [5])

    def test_projection_and_classification(self):
        projected = m.project_ci({"mean": 10, "lower": 5, "upper": 20},
                                 500_000_000)
        self.assertEqual(projected, {"mean": 5, "lower": 2.5, "upper": 10})
        self.assertEqual(m.classify(projected, 10), "PASS")
        self.assertEqual(m.classify(projected, 2), "FAIL")
        self.assertEqual(m.classify(projected, 7), "INCONCLUSIVE")

    def test_summary_uses_registered_allowances(self):
        deltas, allowances = m.summarize([self.record()] * 3, 1_000_000_000,
                                         out_cores=100, off_cores=80)
        self.assertEqual(deltas["null_empty"]["projected_cores"]["mean"], 1)
        self.assertEqual(allowances["off_out"]["budget_cores"], 2)
        self.assertEqual(allowances["counters_off"]["budget_cores"], 4)
        self.assertAlmostEqual(allowances["counters_out"]["budget_cores"], 7)

    def test_parser_accepts_valid_record_and_rejects_bad_conservation(self):
        order = m.LANES
        value = {
            "order": list(order), "iterations": 20, "warmup": 2,
            "lanes": {name: {"elapsed_ns": 10, "ns_per_message": 0.5}
                      for name in order},
            "atomic": {"expected_count": 66, "observed_count": 66},
            "conservation_valid": True,
        }
        self.assertEqual(m.parse_benchmark_output(json.dumps(value) + "\n", order),
                         value)
        value["conservation_valid"] = False
        with self.assertRaises(m.InvalidRun):
            m.parse_benchmark_output(json.dumps(value), order)

    def test_artifact_parser_requires_three_slots_and_total(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "metrics.jsonl"
            path.write_text("\n".join(json.dumps({
                "kind": "reporter_micro_control",
                "metric": f"micro_atomic_{index}", "value": 22})
                for index in range(3)) + "\n")
            self.assertEqual(sum(m.parse_atomic_artifact(path, 66).values()), 66)
            with self.assertRaises(m.InvalidRun):
                m.parse_atomic_artifact(path, 65)

    def test_core_source_can_be_number_or_round8_summary(self):
        self.assertEqual(m.load_cores("1.25", "out"), 1.25)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.json"
            path.write_text(json.dumps({"mode_means": {
                "out": {"cpu_cores": 2.5}, "off": {"cpu_cores": 3.5}}}))
            self.assertEqual(m.load_cores(str(path), "out"), 2.5)
            self.assertEqual(m.load_cores(str(path), "off"), 3.5)


if __name__ == "__main__":
    unittest.main()
