#!/usr/bin/env python3
"""Self-test for swift_bridge_split_guard.py: each rule must be able to fail."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

GUARD = Path(__file__).resolve().parent / "swift_bridge_split_guard.py"
MOTION = '#include "PulpBridge.h"\n#include <vellum/view/motion.hpp>\n'


def run(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(GUARD), "--root", str(root)],
                          capture_output=True, text=True, encoding="utf-8")


class GuardTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        src = self.root / "apple" / "Sources" / "PulpSwift"
        src.mkdir(parents=True)
        (src / "PulpBridge.cpp").write_text('#include "PulpBridge.h"\n', encoding="utf-8")
        (src / "PulpBridge.h").write_text("void pulp_motion_tracing_enabled(void);\n", encoding="utf-8")
        (src / "PulpMotion.swift").write_text("let x = pulp_motion_tracing_enabled()\n", encoding="utf-8")
        vellum = self.root / "integrations" / "pulp-vellum" / "src"
        vellum.mkdir(parents=True)
        (vellum / "swift_motion_bridge.cpp").write_text(MOTION, encoding="utf-8")
        self.src = src

    def tearDown(self):
        self._tmp.cleanup()

    def test_clean_tree_passes(self):
        self.assertEqual(run(self.root).returncode, 0, run(self.root).stdout)

    def test_view_include_in_core_cpp_fails(self):
        (self.src / "PulpBridge.cpp").write_text('#include <pulp/view/motion.hpp>\n', encoding="utf-8")
        self.assertEqual(run(self.root).returncode, 1)

    def test_view_include_in_other_bridge_sources_fails(self):
        for name in ("Extra.mm", "Extra.h", "Extra.swift"):
            path = self.src / name
            path.write_text('#include <pulp/view/motion.hpp>\n', encoding="utf-8")
            self.assertEqual(run(self.root).returncode, 1, name)
            path.unlink()

    def test_motion_shim_in_core_implementation_fails(self):
        (self.src / "PulpBridge.cpp").write_text(
            'void f() { pulp_motion_publish_value(); }\n', encoding="utf-8")
        self.assertEqual(run(self.root).returncode, 1)

    def test_motion_bridge_must_use_vellum_include(self):
        vellum = self.root / "integrations" / "pulp-vellum" / "src"
        (vellum / "swift_motion_bridge.cpp").write_text('#include "PulpBridge.h"\n', encoding="utf-8")
        self.assertEqual(run(self.root).returncode, 1)


if __name__ == "__main__":
    unittest.main()
