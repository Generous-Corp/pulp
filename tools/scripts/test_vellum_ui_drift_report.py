#!/usr/bin/env python3
"""Negative controls for vellum_ui_drift_report.py on a real Git repo."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("vellum_ui_drift_report.py")
WORKFLOW_PATH = pathlib.Path(__file__).parents[2] / ".github/workflows/vellum-freeze-check.yml"
SPEC = importlib.util.spec_from_file_location("vellum_ui_drift_report", MODULE_PATH)
assert SPEC and SPEC.loader
drift = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = drift
SPEC.loader.exec_module(drift)

UI_A = "core/view/a.cpp"
UI_B = "core/canvas/b.cpp"
UI_FROZEN = "core/view/frozen.cpp"
NON_UI = "core/runtime/c.cpp"
UNITS = {UI_A: "view-core", UI_B: "canvas", UI_FROZEN: "view-core", NON_UI: "foundation"}


def git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], check=True, capture_output=True, text=True, encoding="utf-8"
    ).stdout.strip()


class DriftReportTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = pathlib.Path(tmp.name)
        git(self.root, "init", "-q")
        git(self.root, "config", "user.email", "t@example.com")
        git(self.root, "config", "user.name", "t")
        for path in UNITS:
            self.write(path, f"original {path}\n")
        blobs = {p: git(self.root, "hash-object", p) for p in UNITS}
        self.write_json(
            "framework-transfer.json", {"transferred_paths": sorted(UNITS)}
        )
        self.write_json("unit-of.json", UNITS)
        self.write_json(
            "receipt.json",
            {"files": [{"pulp_path": p, "blob": b} for p, b in blobs.items()]},
        )
        self.write_json(
            "pin.json",
            {
                "receipt_path": f"{drift.RECEIPT_DIR}/receipt.json",
                "frozen_paths": [UI_FROZEN],
            },
        )
        self.commit("export")

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def write_json(self, name, data):
        self.write(f"{drift.RECEIPT_DIR}/{name}", json.dumps(data))

    def commit(self, message):
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", message)

    def report(self):
        return drift.build_report(self.root, "HEAD")

    def test_no_drift_is_empty(self):
        report = self.report()
        self.assertEqual(report["drifted"], [])
        self.assertEqual(report["ui_paths"], 3)
        self.assertEqual(report["unfrozen_ui_paths"], 2)

    def test_planted_change_reports_exactly_that_path(self):
        self.write(UI_A, "edited\n")
        self.commit("edit")
        paths = [d["path"] for d in self.report()["drifted"]]
        self.assertEqual(paths, [UI_A])

    def test_deleted_path_is_drift(self):
        (self.root / UI_B).unlink()
        self.commit("delete")
        drifted = self.report()["drifted"]
        self.assertEqual([d["path"] for d in drifted], [UI_B])
        self.assertIsNone(drifted[0]["head_blob"])

    def test_non_ui_change_is_not_reported(self):
        self.write(NON_UI, "edited\n")
        self.commit("edit")
        self.assertEqual(self.report()["drifted"], [])

    def test_frozen_change_is_not_reported(self):
        self.write(UI_FROZEN, "edited\n")
        self.commit("edit")
        self.assertEqual(self.report()["drifted"], [])

    def test_unknown_unit_is_refused(self):
        self.write_json("unit-of.json", {**UNITS, UI_A: "mystery"})
        with self.assertRaises(drift.ReportError):
            self.report()

    def run_cli(self, *extra):
        return subprocess.run(
            [sys.executable, str(MODULE_PATH), "--root", str(self.root), *extra],
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

    def test_exit_zero_with_drift_in_both_formats(self):
        self.write(UI_A, "edited\n")
        self.commit("edit")
        text = self.run_cli()
        self.assertEqual(text.returncode, 0)
        self.assertIn(UI_A, text.stdout)
        out = self.run_cli("--json")
        self.assertEqual(out.returncode, 0)
        self.assertEqual(json.loads(out.stdout)["drift_count"], 1)

    def test_missing_record_exits_two(self):
        (self.root / drift.TRANSFER_PATH).unlink()
        result = self.run_cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot read", result.stderr)

    def test_workflow_waits_for_the_later_unit_map(self):
        workflow = WORKFLOW_PATH.read_text(encoding="utf-8")
        self.assertIn(
            "if: hashFiles('.github/vellum-export-receipt/unit-of.json') != ''",
            workflow,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
