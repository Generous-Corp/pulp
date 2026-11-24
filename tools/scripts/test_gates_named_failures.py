#!/usr/bin/env python3
"""Self-test the named failure summary in gates.sh."""

from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GATES = ROOT / "tools/scripts/gates.sh"


class GatesNamedFailuresTest(unittest.TestCase):
    def test_every_failure_uses_gate_fail(self) -> None:
        source = GATES.read_text(encoding="utf-8")
        assignments = [
            line
            for line in source.splitlines()
            if re.search(r"\bfail=1\b", line)
        ]
        self.assertEqual(
            assignments,
            ['gate_fail() { fail=1; failed_gates+=("$1"); }'],
            "gates.sh contains a bare fail=1 outside gate_fail",
        )

    def test_summary_names_two_injected_failures(self) -> None:
        source = GATES.read_text(encoding="utf-8")
        helper = re.search(r"(?m)^gate_fail\(\) \{[^\n]*\}$", source)
        self.assertIsNotNone(helper, "gate_fail helper is missing")
        summary = re.search(
            r"(?ms)^    failed_gates_joined=.*?^    echo \"gates: failed:.*?$",
            source,
        )
        self.assertIsNotNone(summary, "named failure summary is missing")
        script = "\n".join(
            [
                "fail=0",
                "failed_gates=()",
                helper.group(0),
                'gate_fail "injected alpha"',
                'gate_fail "injected beta"',
                summary.group(0).replace("    ", ""),
            ]
        )
        result = subprocess.run(
            ["bash", "-c", script],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "gates: failed: injected alpha, injected beta",
            result.stdout + result.stderr,
        )


if __name__ == "__main__":
    unittest.main()
