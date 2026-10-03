#!/usr/bin/env python3
"""Tests for tools/scripts/windows_cli_release_precheck.py.

The check may withhold a release, so the cases that matter most are the ones
where it must NOT: missing evidence, infrastructure failures, and verdicts for
commits the tag does not contain.

Run:  python3 tools/scripts/test_windows_cli_release_precheck.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import windows_cli_release_precheck as pre  # noqa: E402


def check(conclusion: str, completed: str = "2026-10-03T10:00:00Z",
          name: str | None = None, run_id: int = 1) -> dict:
    return {
        "name": name or pre.VERDICT_CHECK,
        "status": "completed",
        "conclusion": conclusion,
        "completed_at": completed,
        "html_url": f"https://example.invalid/runs/{run_id}",
    }


def checks(table: dict[str, list[dict]]):
    calls: list[str] = []

    def lookup(sha: str) -> list[dict]:
        calls.append(sha)
        return table.get(sha, [])

    return lookup, calls


class Decide(unittest.TestCase):
    def test_newest_failed_verdict_blocks(self) -> None:
        lookup, _ = checks({"a" * 40: [check("failure", run_id=1)]})
        verdict = pre.decide(["a" * 40], checks_for=lookup)
        self.assertEqual(verdict.action, pre.BLOCK)
        self.assertIn("aaaaaaaaaaaa", verdict.reason)
        self.assertTrue(verdict.run_url.endswith("/1"))

    def test_newer_clean_verdict_clears_an_older_failure(self) -> None:
        lookup, _ = checks({"b" * 40: [check("success")], "a" * 40: [check("failure")]})
        verdict = pre.decide(["b" * 40, "a" * 40], checks_for=lookup)
        self.assertEqual(verdict.action, pre.ALLOW)

    def test_commits_without_a_verdict_are_looked_through(self) -> None:
        # Most commits never compile (their range could not reach the CLI);
        # they must not mask an older failure.
        lookup, calls = checks({"a" * 40: [check("failure")]})
        verdict = pre.decide(["c" * 40, "b" * 40, "a" * 40], checks_for=lookup)
        self.assertEqual(verdict.action, pre.BLOCK)
        self.assertEqual(len(calls), 3)

    def test_skipped_verdict_is_not_evidence(self) -> None:
        # The verdict job is skipped when the runner broke before compiling.
        lookup, _ = checks(
            {"b" * 40: [check("skipped")], "a" * 40: [check("failure")]}
        )
        verdict = pre.decide(["b" * 40, "a" * 40], checks_for=lookup)
        self.assertEqual(verdict.action, pre.BLOCK)

    def test_other_checks_on_the_commit_are_ignored(self) -> None:
        lookup, _ = checks(
            {"a" * 40: [check("failure", name="Windows CLI compile (MSVC)")]}
        )
        verdict = pre.decide(["a" * 40], checks_for=lookup)
        self.assertEqual(verdict.action, pre.ALLOW, "only the verdict check counts")

    def test_rerun_on_the_same_commit_takes_the_newest(self) -> None:
        lookup, _ = checks(
            {"a" * 40: [check("failure", "2026-10-03T10:00:00Z"),
                        check("success", "2026-10-03T11:00:00Z")]}
        )
        verdict = pre.decide(["a" * 40], checks_for=lookup)
        self.assertEqual(verdict.action, pre.ALLOW)

    def test_no_verdict_means_no_hold(self) -> None:
        lookup, _ = checks({})
        self.assertEqual(pre.decide([], checks_for=lookup).action, pre.ALLOW)
        self.assertEqual(pre.decide(["a" * 40], checks_for=lookup).action, pre.ALLOW)

    def test_inspection_is_bounded(self) -> None:
        lookup, calls = checks({})
        verdict = pre.decide([f"{i:040d}" for i in range(10)], checks_for=lookup,
                             max_commits=3)
        self.assertEqual(verdict.action, pre.ALLOW)
        self.assertEqual(len(calls), 3)


class SelectCandidates(unittest.TestCase):
    def test_a_batch_head_above_a_touching_merge_is_a_candidate(self) -> None:
        # The run reports on the push's head; a 5-PR queue batch can put four
        # unrelated merges above the one that touched tools/cli.
        history = ["h0", "h1", "h2", "h3", "t4", "u5", "u6", "u7", "u8", "u9", "u10"]
        picked = pre.select_candidates(history, {"t4"})
        self.assertEqual(picked, ["h0", "h1", "h2", "h3", "t4"])

    def test_nothing_touching_means_nothing_to_ask(self) -> None:
        self.assertEqual(pre.select_candidates(["a", "b", "c"], set()), [])

    def test_order_is_newest_first(self) -> None:
        picked = pre.select_candidates(["n", "m", "t1", "x", "y", "z", "q", "t2"],
                                       {"t1", "t2"})
        self.assertEqual(picked[0], "n")
        self.assertEqual(picked, sorted(picked, key=["n", "m", "t1", "x", "y", "z",
                                                     "q", "t2"].index))


class MainShell(unittest.TestCase):
    """Run main() with a stub `gh` on PATH."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.bin = Path(self.tmp.name) / "bin"
        self.bin.mkdir()
        self.output = Path(self.tmp.name) / "gh_output"
        self.output.write_text("", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def stub_gh(self, body: str) -> None:
        gh = self.bin / "gh"
        gh.write_text("#!/bin/sh\n" + body, encoding="utf-8")
        gh.chmod(0o755)

    def invoke(self, extra_env: dict[str, str] | None = None) -> dict:
        repo = Path(__file__).resolve().parent.parent.parent
        env = {
            **os.environ,
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "REPO": "example/repo",
            "HEAD_SHA": "HEAD",
            "GITHUB_OUTPUT": str(self.output),
            **(extra_env or {}),
        }
        result = subprocess.run(
            [sys.executable, str(Path(pre.__file__).resolve())],
            capture_output=True,
            text=True,
            env=env,
            cwd=repo,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout.strip().splitlines()[-1])

    def test_unreadable_api_allows_and_says_why(self) -> None:
        self.stub_gh('echo "HTTP 404: Not Found" >&2\nexit 1\n')
        verdict = self.invoke()
        self.assertEqual(verdict["action"], pre.ALLOW)
        self.assertIn("could not read", verdict["reason"])
        self.assertIn("block=0", self.output.read_text(encoding="utf-8"))

    def test_kill_switch_skips_the_api(self) -> None:
        self.stub_gh("echo should-not-run >&2\nexit 1\n")
        verdict = self.invoke({"PULP_RELEASE_WINDOWS_PRECHECK": "off"})
        self.assertEqual(verdict["action"], pre.ALLOW)
        self.assertIn("disabled", verdict["reason"])

    def test_blocking_verdict_reaches_github_output(self) -> None:
        # A real commit that touches tools/cli, so candidate selection (which
        # runs real git) offers it; the stub reports a failed verdict on it.
        repo = Path(__file__).resolve().parent.parent.parent
        sha = subprocess.run(
            ["git", "rev-list", "--first-parent", "-n", "1", "HEAD", "--", "tools/cli"],
            capture_output=True, text=True, check=True, cwd=repo,
        ).stdout.strip()
        self.assertTrue(sha, "control: this checkout must have a tools/cli commit")
        failed = {"check_runs": [check("failure", run_id=9)]}
        self.stub_gh(
            f'case "$2" in\n  *{sha}*) cat <<\'J\'\n{json.dumps(failed)}\nJ\n;;\n'
            '  *) echo \'{"check_runs": []}\';;\nesac\n'
        )
        verdict = self.invoke({"HEAD_SHA": sha})
        self.assertEqual(verdict["action"], pre.BLOCK)
        self.assertIn("block=1", self.output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
