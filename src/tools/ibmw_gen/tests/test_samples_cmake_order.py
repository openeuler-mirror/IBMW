#!/usr/bin/env python3

from pathlib import Path
import re
import unittest


REPO_ROOT = Path(__file__).resolve().parents[4]
SAMPLES_CMAKE = REPO_ROOT / "src" / "samples" / "cpp" / "CMakeLists.txt"


class SamplesCMakeOrderTest(unittest.TestCase):
    def test_r8_validation_follows_its_benchmark_dependency(self):
        contents = SAMPLES_CMAKE.read_text(encoding="utf-8")
        benchmark_block = re.search(
            r"if\(IBMW_BUILD_BENCHMARKS\)(.*?)endif\(\)", contents, re.DOTALL
        )
        self.assertIsNotNone(benchmark_block)
        block = benchmark_block.group(1)
        benchmark = "add_subdirectory(transport_benchmark)"
        validation = "add_subdirectory(r8_validation)"
        self.assertIn(benchmark, block)
        self.assertIn(validation, block)
        self.assertLess(block.index(benchmark), block.index(validation))
        self.assertEqual(contents.count(validation), 1)


if __name__ == "__main__":
    unittest.main()
