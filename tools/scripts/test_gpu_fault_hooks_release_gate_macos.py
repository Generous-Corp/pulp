#!/usr/bin/env python3
"""macOS-only self-test for gpu_fault_hooks_release_gate.py: installer
packages (.pkg, via pkgutil) and disk images (.dmg, mounted read-only and
hidden) are opened and scanned. Kept apart from the portable self-test,
which the build-free Linux lane runs, so nothing here can skip and pass."""
from __future__ import annotations

import shutil
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_gpu_fault_hooks_release_gate as base  # noqa: E402


class MacArchives(base.ReleaseGate):
    def setUp(self):
        if sys.platform != "darwin":
            self.fail("this self-test runs only on the macOS gate")
        super().setUp()

    def test_a_pkg_is_expanded_and_scanned(self):
        for planted in (False, True):
            with self.subTest(planted=planted):
                stage = self._staged(planted)
                pkg = self.build / "Installer.pkg"
                subprocess.run(["pkgbuild", "--root", str(stage), "--identifier",
                                "com.example.gate-test", "--version", "1", str(pkg)],
                               check=True, capture_output=True)
                self.assertEqual(self.run_gate(), 1 if planted else 0)
                pkg.unlink()

    def test_a_dmg_is_mounted_and_scanned(self):
        for planted in (False, True):
            with self.subTest(planted=planted):
                stage = self._staged(planted)
                dmg = self.build / "Installer.dmg"
                subprocess.run(["hdiutil", "create", "-quiet", "-srcfolder", str(stage),
                                "-format", "UDZO", "-ov", str(dmg)],
                               check=True, capture_output=True)
                self.assertEqual(self.run_gate(), 1 if planted else 0)
                dmg.unlink()



if __name__ == "__main__":
    unittest.main(defaultTest="MacArchives")
