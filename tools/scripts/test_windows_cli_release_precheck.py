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


def run(run_id: int, sha: str, created: str, conclusion: str = "success") -> dict:
    return {
        "id": run_id,
        "head_sha": sha,
        "created_at": created,
        "status": "completed",
        "conclusion": conclusion,
        "html_url": f"https://example.invalid/runs/{run_id}",
    }


def jobs(compile_step: str | None, job_conclusion: str | None = None) -> list[dict]:
    """A run's jobs; compile_step None means the compile job was skipped."""
    scope = {"name": "Windows CLI compile scope", "conclusion": "success", "steps": []}
    if compile_step is None:
        return [scope, {"name": pre.COMPILE_JOB, "conclusion": "skipped", "steps": []}]
    return [
        scope,
        {
            "name": pre.COMPILE_JOB,
            "conclusion": job_conclusion
            or ("failure" if compile_step in ("failure", "skipped") else "success"),
            "steps": [
                {"name": "Bootstrap dependencies", "conclusion": "success"},
                {"name": pre.COMPILE_STEP, "conclusion": compile_step},
            ],
        },
    ]


def everything_is_ancestor(_: str) -> bool:
    return True


class Decide(unittest.TestCase):
    def test_latest_compiled_failure_blocks(self) -> None:
        runs = [run(1, "a" * 40, "2026-10-03T10:00:00Z", "failure")]
        verdict = pre.decide(
            runs, is_ancestor=everything_is_ancestor, jobs_for=lambda _: jobs("failure")
        )
        self.assertEqual(verdict.action, pre.BLOCK)
        self.assertIn("aaaaaaaaaaaa", verdict.reason)
        self.assertTrue(verdict.run_url.endswith("/1"))

    def test_later_clean_compile_clears_an_older_failure(self) -> None:
        runs = [
            run(1, "a" * 40, "2026-10-03T10:00:00Z", "failure"),
            run(2, "b" * 40, "2026-10-03T11:00:00Z"),
        ]
        by_id = {1: jobs("failure"), 2: jobs("success")}
        verdict = pre.decide(
            runs, is_ancestor=everything_is_ancestor, jobs_for=by_id.__getitem__
        )
        self.assertEqual(verdict.action, pre.ALLOW)

    def test_skipped_compiles_are_looked_through(self) -> None:
        # Most push runs skip compiling; they must not mask an older failure.
        runs = [
            run(1, "a" * 40, "2026-10-03T10:00:00Z", "failure"),
            run(2, "b" * 40, "2026-10-03T11:00:00Z"),
            run(3, "c" * 40, "2026-10-03T12:00:00Z"),
        ]
        by_id = {1: jobs("failure"), 2: jobs(None), 3: jobs(None)}
        verdict = pre.decide(
            runs, is_ancestor=everything_is_ancestor, jobs_for=by_id.__getitem__
        )
        self.assertEqual(verdict.action, pre.BLOCK)

    def test_failure_on_a_commit_the_tag_does_not_contain_is_ignored(self) -> None:
        runs = [
            run(1, "a" * 40, "2026-10-03T10:00:00Z"),
            run(2, "b" * 40, "2026-10-03T11:00:00Z", "failure"),
        ]
        by_id = {1: jobs("success"), 2: jobs("failure")}
        verdict = pre.decide(
            runs, is_ancestor=lambda sha: sha.startswith("a"), jobs_for=by_id.__getitem__
        )
        self.assertEqual(verdict.action, pre.ALLOW)

    def test_infrastructure_failure_does_not_block(self) -> None:
        # The job failed before its compile step ran: nothing is known about
        # the code, so the release must not be withheld on it.
        runs = [run(1, "a" * 40, "2026-10-03T10:00:00Z", "failure")]
        verdict = pre.decide(
            runs,
            is_ancestor=everything_is_ancestor,
            jobs_for=lambda _: jobs("skipped", job_conclusion="failure"),
        )
        self.assertEqual(verdict.action, pre.ALLOW)
        self.assertIn("before compiling", verdict.reason)

    def test_cancelled_runs_are_not_evidence(self) -> None:
        runs = [
            run(1, "a" * 40, "2026-10-03T10:00:00Z", "failure"),
            run(2, "b" * 40, "2026-10-03T11:00:00Z", "cancelled"),
        ]
        calls: list[int] = []

        def jobs_for(run_id: int) -> list[dict]:
            calls.append(run_id)
            return jobs("failure")

        verdict = pre.decide(runs, is_ancestor=everything_is_ancestor, jobs_for=jobs_for)
        self.assertEqual(verdict.action, pre.BLOCK)
        self.assertEqual(calls, [1])

    def test_no_runs_means_no_hold(self) -> None:
        verdict = pre.decide(
            [], is_ancestor=everything_is_ancestor, jobs_for=lambda _: []
        )
        self.assertEqual(verdict.action, pre.ALLOW)

    def test_inspection_is_bounded(self) -> None:
        runs = [
            run(i, f"{i:040d}", f"2026-10-03T{i:02d}:00:00Z") for i in range(1, 6)
        ]
        calls: list[int] = []

        def jobs_for(run_id: int) -> list[dict]:
            calls.append(run_id)
            return jobs(None)

        verdict = pre.decide(
            runs, is_ancestor=everything_is_ancestor, jobs_for=jobs_for, max_runs=2
        )
        self.assertEqual(verdict.action, pre.ALLOW)
        self.assertEqual(len(calls), 2)


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
            cwd=Path(__file__).resolve().parent,
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
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=True
        ).stdout.strip()
        listing = {"workflow_runs": [run(7, head, "2026-10-03T10:00:00Z", "failure")]}
        payload = {"jobs": jobs("failure")}
        self.stub_gh(
            'case "$2" in\n'
            f"  *jobs*) cat <<'J'\n{json.dumps(payload)}\nJ\n;;\n"
            f"  *) cat <<'L'\n{json.dumps(listing)}\nL\n;;\n"
            "esac\n"
        )
        verdict = self.invoke({"HEAD_SHA": head})
        self.assertEqual(verdict["action"], pre.BLOCK)
        self.assertIn("block=1", self.output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
