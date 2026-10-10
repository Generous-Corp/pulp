#!/usr/bin/env python3
"""Pure-stdlib checks for Pulp's optional mise developer contract."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MISE = ROOT / ".mise.toml"
REQUIREMENTS_LOCK = ROOT / "tools/motion/visual/requirements.lock"
NOTICE = ROOT / "NOTICE.md"


class MiseContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = MISE.read_text(encoding="utf-8")

    def test_profile_does_not_enable_automatic_install_or_update(self) -> None:
        self.assertIn("auto_install = false", self.text)
        self.assertNotIn("auto_update", self.text)

    def test_local_tasks_use_authoritative_visual_lock(self) -> None:
        self.assertIn("tools/ci/install_visual_python_deps.sh", self.text)
        installer = (ROOT / "tools/ci/install_visual_python_deps.sh").read_text(encoding="utf-8")
        self.assertIn("--require-hashes", installer)
        self.assertIn("tools/motion/visual/requirements.lock", installer)
        self.assertTrue(REQUIREMENTS_LOCK.is_file())

    def test_notice_records_optional_mise_dependency(self) -> None:
        notice = NOTICE.read_text(encoding="utf-8")
        self.assertIn("mise", notice.lower())


if __name__ == "__main__":
    unittest.main()
