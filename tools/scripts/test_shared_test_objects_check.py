#!/usr/bin/env python3
"""Self-test for shared_test_objects_check.py (paths shortened from a real build.ninja)."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import shared_test_objects_check as check  # noqa: E402

SHARED = {"test/harness/rt_allocation_probe.cpp": "pulp-test-rt-allocation-probe"}
PROBE_OBJ = "test/CMakeFiles/pulp-test-rt-allocation-probe.dir/harness/rt_allocation_probe.cpp.o"

FOLDED = f"""\
build {PROBE_OBJ}: CXX_COMPILER__pulp-test-rt-allocation-probe_unscanned_Release /src/test/harness/rt_allocation_probe.cpp
  FLAGS = -O3
build test/CMakeFiles/pulp-test-host.dir/test_host.cpp.o: CXX_COMPILER__pulp-test-host_unscanned_Release /src/test/test_host.cpp
build test/pulp-test-host: CXX_EXECUTABLE_LINKER__pulp-test-host_Release test/CMakeFiles/pulp-test-host.dir/test_host.cpp.o {PROBE_OBJ} | lib.a || test/CMakeFiles/pulp-test-rt-allocation-probe
build examples/Sampler/CMakeFiles/sampler-test.dir/__/__/test/harness/rt_allocation_probe.cpp.o: CXX_COMPILER__sampler-test_unscanned_Release /src/test/harness/rt_allocation_probe.cpp
"""

PER_TARGET = """\
build test/CMakeFiles/pulp-test-host.dir/harness/rt_allocation_probe.cpp.o: CXX_COMPILER__pulp-test-host_unscanned_Release /src/test/harness/rt_allocation_probe.cpp
build test/CMakeFiles/pulp-test-state.dir/harness/rt_allocation_probe.cpp.o: CXX_COMPILER__pulp-test-state_unscanned_Release /src/test/harness/rt_allocation_probe.cpp
"""


class SharedTestObjectsCheckTest(unittest.TestCase):
    def test_folded_build_passes_and_ignores_examples(self) -> None:
        self.assertEqual(check.check(FOLDED, SHARED), [])

    def test_per_target_copies_fail(self) -> None:
        problems = "\n".join(check.check(PER_TARGET, SHARED))
        self.assertIn("found 0", problems)
        self.assertIn("2 test target(s) still compile their own copy", problems)

    def test_one_missed_consumer_fails(self) -> None:
        text = FOLDED + PER_TARGET.splitlines()[0] + "\n"
        problems = check.check(text, SHARED)
        self.assertEqual(len(problems), 1)
        self.assertIn("1 test target(s)", problems[0])

    def test_unlinked_shared_object_fails(self) -> None:
        text = FOLDED.replace(f" {PROBE_OBJ} | lib.a", " | lib.a")
        problems = check.check(text, SHARED)
        self.assertEqual(len(problems), 1)
        self.assertIn("no link step consumes", problems[0])

    def test_main_skips_without_build_ninja(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            args = ["--build-dir", tmp, "--shared",
                    "test/harness/rt_allocation_probe.cpp=pulp-test-rt-allocation-probe"]
            self.assertEqual(check.main(args), check.SKIP)
            (Path(tmp) / "build.ninja").write_text(PER_TARGET)
            self.assertEqual(check.main(args), 1)
            (Path(tmp) / "build.ninja").write_text(FOLDED)
            self.assertEqual(check.main(args), 0)


if __name__ == "__main__":
    unittest.main()
