#!/usr/bin/env python3
"""Negative controls for required-check execution coverage."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
import sys
from pathlib import Path


SCRIPT = Path(__file__).with_name("required_gate_liveness.py")
spec = importlib.util.spec_from_file_location("required_gate_liveness", SCRIPT)
assert spec and spec.loader
rgl = importlib.util.module_from_spec(spec)
sys.modules["required_gate_liveness"] = rgl
spec.loader.exec_module(rgl)

REQUIRED = ("macos", "Enforce version & skill sync")


def run(
    name: str,
    status: str = "completed",
    conclusion: str | None = "success",
    check_id: int = 1,
) -> dict:
    return {"id": check_id, "name": name, "status": status, "conclusion": conclusion}


class RequiredGateLivenessTests(unittest.TestCase):
    def test_every_required_check_on_exact_sha_passes(self) -> None:
        self.assertEqual(rgl.evaluate(REQUIRED, [run(name) for name in REQUIRED]), [])

    def test_negative_control_missing_path_filtered_gate_fires(self) -> None:
        findings = rgl.evaluate(REQUIRED, [run("macos")])
        self.assertEqual([finding.context for finding in findings], ["Enforce version & skill sync"])
        self.assertIn("missing", findings[0].reason)

    def test_negative_control_check_from_another_sha_does_not_count(self) -> None:
        # fetch_check_runs is scoped to commits/<target-sha>; a check attached to
        # another SHA is therefore absent from this snapshot.
        findings = rgl.evaluate(REQUIRED, [])
        self.assertEqual({finding.context for finding in findings}, set(REQUIRED))

    def test_negative_control_pending_gate_fires(self) -> None:
        findings = rgl.evaluate(REQUIRED, [run("macos", "in_progress", None), run(REQUIRED[1])])
        self.assertEqual(len(findings), 1)
        self.assertIn("in_progress", findings[0].reason)

    def test_negative_control_failed_gate_fires(self) -> None:
        findings = rgl.evaluate(REQUIRED, [run("macos", conclusion="failure"), run(REQUIRED[1])])
        self.assertEqual(len(findings), 1)
        self.assertIn("failure", findings[0].reason)

    def test_negative_control_only_skipped_gate_fires(self) -> None:
        findings = rgl.evaluate(REQUIRED, [run("macos", conclusion="skipped"), run(REQUIRED[1])])
        self.assertEqual(len(findings), 1)
        self.assertIn("skipped", findings[0].reason)

    def test_newer_successful_rerun_satisfies_context(self) -> None:
        checks = [
            run("macos", conclusion="failure", check_id=1),
            run("macos", check_id=2),
            run(REQUIRED[1]),
        ]
        self.assertEqual(rgl.evaluate(REQUIRED, checks), [])

    def test_negative_control_newer_failure_masks_old_success(self) -> None:
        checks = [
            run("macos", check_id=1),
            run("macos", conclusion="failure", check_id=2),
            run(REQUIRED[1]),
        ]
        findings = rgl.evaluate(REQUIRED, checks)
        self.assertEqual(len(findings), 1)
        self.assertIn("failure", findings[0].reason)

    def test_later_skipped_duplicate_does_not_mask_executed_success(self) -> None:
        checks = [
            run("macos", check_id=1),
            run("macos", conclusion="skipped", check_id=2),
            run(REQUIRED[1]),
        ]
        self.assertEqual(rgl.evaluate(REQUIRED, checks), [])

    def test_ruleset_parser_is_non_vacuous_and_authoritative(self) -> None:
        doc = {
            "rules": [
                {
                    "type": "required_status_checks",
                    "parameters": {
                        "required_status_checks": [{"context": name} for name in REQUIRED]
                    },
                }
            ]
        }
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "ruleset.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            self.assertEqual(rgl.required_contexts(path), REQUIRED)

            path.write_text('{"rules": []}', encoding="utf-8")
            with self.assertRaisesRegex(rgl.LivenessError, "contract is empty"):
                rgl.required_contexts(path)


if __name__ == "__main__":
    unittest.main()
