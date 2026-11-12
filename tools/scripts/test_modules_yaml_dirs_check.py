#!/usr/bin/env python3
"""Tests for modules_yaml_dirs_check.py."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modules_yaml_dirs_check as check  # noqa: E402

MODULES = """modules:
  - name: runtime
    status: stable
    dependencies: []

  - name: pulp-native
    path: integrations/pulp-native
    status: experimental
    dependencies: [runtime]
"""


class ModulesYamlDirsCheck(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "core" / "runtime").mkdir(parents=True)
        (self.root / "integrations" / "pulp-native").mkdir(parents=True)
        self.modules = self.root / "modules.yaml"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_check(self, text: str) -> tuple[int, list[str]]:
        self.modules.write_text(text, encoding="utf-8")
        entries = check.read_entries(text)
        return check.main(["--root", str(self.root), "--modules", str(self.modules)]), check.problems(
            self.root, entries
        )

    def test_core_and_integrations_entries_pass(self) -> None:
        rc, found = self.run_check(MODULES)
        self.assertEqual(found, [])
        self.assertEqual(rc, 0)

    def test_path_is_read_from_its_own_entry(self) -> None:
        self.assertEqual(
            check.read_entries(MODULES), [("runtime", None), ("pulp-native", "integrations/pulp-native")]
        )

    def test_missing_integrations_package_fails(self) -> None:
        rc, found = self.run_check(MODULES.replace("integrations/pulp-native", "integrations/rmlui-native"))
        self.assertEqual(rc, 1)
        self.assertEqual(found, ["modules.yaml lists 'pulp-native' but integrations/rmlui-native/ does not exist"])

    def test_entry_without_path_still_needs_core(self) -> None:
        # Control: dropping the path sends the package back to core/<name>,
        # which does not exist, so the integrations directory alone does not pass.
        rc, found = self.run_check(MODULES.replace("    path: integrations/pulp-native\n", ""))
        self.assertEqual(rc, 1)
        self.assertEqual(found, ["modules.yaml lists 'pulp-native' but core/pulp-native/ does not exist"])

    def test_path_outside_the_repository_is_refused(self) -> None:
        for bad in ("../elsewhere", "/abs/path", "docs/status"):
            with self.subTest(path=bad):
                rc, found = self.run_check(MODULES.replace("integrations/pulp-native", bad))
                self.assertEqual(rc, 1)
                self.assertEqual(len(found), 1)
                self.assertIn(f"path '{bad}'", found[0])

    def test_no_entries_is_an_error(self) -> None:
        self.modules.write_text("modules: []\n", encoding="utf-8")
        self.assertEqual(check.main(["--root", str(self.root), "--modules", str(self.modules)]), 1)


if __name__ == "__main__":
    unittest.main()
