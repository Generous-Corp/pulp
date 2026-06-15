#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
import unittest

from module_test_utils import load_local_ci_module



def load_evidence_cli_module():
    return load_local_ci_module("evidence_cli.py")


class EvidenceCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_evidence_cli_module()
        self.printed: list[str] = []
        self.summary_calls: list[dict] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def args(self, *, branch=None, sha=None, limit=3):
        return Namespace(branch=branch, sha=sha, limit=limit)

    def run_cmd(self, args, *, found, current_branch="feature/current"):
        def summary(**kwargs):
            self.summary_calls.append(kwargs)
            return found

        return self.mod.cmd_evidence(
            args,
            current_branch_fn=lambda: current_branch,
            evidence_scope_header_line_fn=self.header_line,
            print_evidence_summary_fn=summary,
            evidence_empty_line_fn=lambda *, has_header: "  (none)" if has_header else "No local CI evidence recorded.",
            print_fn=self.print_line,
        )

    @staticmethod
    def header_line(branch, sha):
        if branch:
            return f"Evidence for branch `{branch}`:"
        if sha:
            return f"Evidence for sha `{sha[:12]}`:"
        return None

    def test_cmd_evidence_prints_branch_header_and_returns_success(self):
        result = self.run_cmd(self.args(branch="feature/evidence", limit=5), found=True)

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["Evidence for branch `feature/evidence`:"])
        self.assertEqual(self.summary_calls, [{"branch": "feature/evidence", "sha": None, "limit": 5}])

    def test_cmd_evidence_uses_current_branch_fallback(self):
        result = self.run_cmd(self.args(limit=2), found=True, current_branch="feature/current")

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["Evidence for branch `feature/current`:"])
        self.assertEqual(self.summary_calls, [{"branch": "feature/current", "sha": None, "limit": 2}])

    def test_cmd_evidence_prints_sha_empty_result_with_header_indent(self):
        result = self.run_cmd(self.args(sha="f" * 40, limit=1), found=False, current_branch="")

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Evidence for sha `ffffffffffff`:", "  (none)"])
        self.assertEqual(self.summary_calls, [{"branch": "", "sha": "f" * 40, "limit": 1}])

    def test_cmd_evidence_prints_unscoped_empty_result(self):
        result = self.run_cmd(self.args(limit=1), found=False, current_branch="")

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["No local CI evidence recorded."])
        self.assertEqual(self.summary_calls, [{"branch": "", "sha": None, "limit": 1}])


if __name__ == "__main__":
    unittest.main()
