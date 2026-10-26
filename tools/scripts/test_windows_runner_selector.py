#!/usr/bin/env python3
"""Tests for windows_runner_selector.py: the dispatch-input guard that keeps a
Windows job off the macOS gate pool and the Linux pool, and the --route mode
that defaults Windows x64 jobs to the self-hosted lane.

Run:  python3 tools/scripts/test_windows_runner_selector.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "windows_runner_selector.py"
sys.path.insert(0, str(HERE))

import windows_runner_selector as selector  # noqa: E402

MACPRO_WINDOWS = ["self-hosted", "Windows", "X64", "pulp-build-windows-x64", "pulp-host-macpro"]


class ResolveTests(unittest.TestCase):
    def test_empty_selector_keeps_the_hosted_fallback(self) -> None:
        self.assertEqual(selector.resolve(""), '"windows-latest"')
        self.assertEqual(selector.resolve("   "), '"windows-latest"')

    def test_macpro_windows_slot_is_accepted_verbatim(self) -> None:
        self.assertEqual(json.loads(selector.resolve(json.dumps(MACPRO_WINDOWS))), MACPRO_WINDOWS)

    def test_hosted_windows_image_is_accepted(self) -> None:
        self.assertEqual(selector.resolve('"windows-2022"'), '"windows-2022"')

    def test_macos_gate_selectors_are_refused(self) -> None:
        for raw in (
            '["self-hosted","macOS","ARM64","pulp-build-vm","pulp-gate-fast"]',
            '["self-hosted","Windows","pulp-gate-fast"]',
            '["self-hosted","Windows","pulp-build-vm"]',
            '"macos-15"',
            '["self-hosted","Windows","OSX"]',
            '["self-hosted","Windows","darwin"]',
        ):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)

    def test_linux_selectors_are_refused(self) -> None:
        for raw in (
            '["self-hosted","Linux","X64","pulp-build-linux-x64","pulp-host-macpro"]',
            '["self-hosted","Windows","X64","pulp-build-linux-x64"]',
            '["self-hosted","Linux","X64","pulp-preamble-macpro"]',
            '"ubuntu-latest"',
        ):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)

    def test_a_list_must_name_windows_and_self_hosted(self) -> None:
        for raw in ('["self-hosted","X64"]', '["Windows","X64"]', "[]"):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)

    def test_malformed_input_is_refused(self) -> None:
        for raw in ("not json", '{"labels":["Windows"]}', '["self-hosted","Windows","a b"]',
                    '["self-hosted","Windows","Windows"]', '"self-hosted"', "[1]"):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)


LANE = ["self-hosted", "Windows", "X64", "pulp-build-windows-x64"]


class RouteTests(unittest.TestCase):
    def route(self, event: str, **kwargs):
        runs_on, chosen, reason = selector.route(event_name=event, **kwargs)
        return json.loads(runs_on), chosen, reason

    def test_an_empty_selector_takes_the_self_hosted_lane(self) -> None:
        for event in ("schedule", "workflow_dispatch", "pull_request", "push"):
            with self.subTest(event=event):
                self.assertEqual(self.route(event, repository="o/r", head_repo="o/r"),
                                 (LANE, "self-hosted", "self-hosted-default"))

    def test_the_configured_lane_selector_replaces_the_default(self) -> None:
        self.assertEqual(self.route("schedule", default_selector=json.dumps(MACPRO_WINDOWS))[0],
                         MACPRO_WINDOWS)

    def test_a_configured_lane_selector_must_be_windows(self) -> None:
        for raw in ('["self-hosted","Linux","X64"]', '"ubuntu-latest"', "not json"):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.route(event_name="schedule", default_selector=raw)

    def test_a_hosted_lane_selector_switches_the_lane_back_to_each_jobs_image(self) -> None:
        # The lane-scoped rollback: the job keeps its own hosted image.
        for configured in ('"windows-latest"', '"windows-2022"'):
            with self.subTest(configured=configured):
                self.assertEqual(
                    self.route("schedule", default_selector=configured, hosted_label="windows-2022"),
                    ("windows-2022", "hosted", "hosted-opt-in"))

    def test_hosted_needs_the_opt_in(self) -> None:
        for value in ("1", "true", "TRUE"):
            with self.subTest(value=value):
                self.assertEqual(self.route("schedule", allow_hosted=value),
                                 ("windows-latest", "hosted", "hosted-opt-in"))
        self.assertEqual(self.route("schedule", allow_hosted="1", hosted_label="windows-2022")[0],
                         "windows-2022")
        for value in ("", "0", "false", "yes"):
            with self.subTest(value=value):
                self.assertEqual(self.route("schedule", allow_hosted=value)[1], "self-hosted")

    def test_a_fork_pull_request_never_reaches_the_lane(self) -> None:
        for event in ("pull_request", "pull_request_target"):
            with self.subTest(event=event):
                self.assertEqual(
                    self.route(event, head_repo="someone/pulp", repository="Generous-Corp/pulp"),
                    ("windows-latest", "hosted", "fork-hosted"))

    def test_an_explicit_selector_wins_and_is_still_policed(self) -> None:
        self.assertEqual(self.route("workflow_dispatch", selector=json.dumps(MACPRO_WINDOWS)),
                         (MACPRO_WINDOWS, "self-hosted", "explicit-selector"))
        self.assertEqual(self.route("workflow_dispatch", selector='"windows-2025"'),
                         ("windows-2025", "hosted", "explicit-selector"))
        with self.assertRaises(selector.SelectorError):
            selector.route(event_name="workflow_dispatch",
                           selector='["self-hosted","macOS","pulp-gate-fast"]')

    def test_a_busy_lane_skips_automatic_events_only(self) -> None:
        busy = lambda: 1  # noqa: E731
        self.assertEqual(self.route("schedule", max_backlog=1, backlog=busy),
                         (LANE, "skip", "capacity-skip"))
        self.assertEqual(self.route("pull_request", max_backlog=1, backlog=busy)[1], "skip")
        # A dispatch always queues: someone asked for it.
        self.assertEqual(self.route("workflow_dispatch", max_backlog=1, backlog=busy)[1],
                         "self-hosted")
        # Below the limit, and with no limit at all, the job queues.
        self.assertEqual(self.route("schedule", max_backlog=2, backlog=busy)[1], "self-hosted")
        self.assertEqual(self.route("schedule", backlog=busy)[1], "self-hosted")

    def test_the_backlog_is_only_probed_when_it_can_matter(self) -> None:
        calls = []

        def probe() -> int:
            calls.append(1)
            return 0

        selector.route(event_name="schedule", allow_hosted="1", max_backlog=1, backlog=probe)
        selector.route(event_name="workflow_dispatch", max_backlog=1, backlog=probe)
        self.assertEqual(calls, [])
        selector.route(event_name="schedule", max_backlog=1, backlog=probe)
        self.assertEqual(calls, [1])

    def test_every_reason_is_declared(self) -> None:
        reasons = {
            self.route("schedule")[2],
            self.route("schedule", allow_hosted="1")[2],
            self.route("pull_request", head_repo="a/b", repository="c/d")[2],
            self.route("schedule", max_backlog=1, backlog=lambda: 5)[2],
            self.route("workflow_dispatch", selector='"windows-2022"')[2],
        }
        self.assertEqual(reasons, set(selector.ROUTE_REASONS))


class BacklogProbeTests(unittest.TestCase):
    def fake_gh(self, tmp: Path, body: str) -> dict[str, str]:
        gh = tmp / "gh"
        gh.write_text("#!/bin/sh\n" + body, encoding="utf-8")
        gh.chmod(0o755)
        return {**os.environ, "PATH": f"{tmp}{os.pathsep}{os.environ.get('PATH', '')}"}

    def test_counts_lane_jobs_across_queued_and_running_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            # Two runs listed under both statuses are counted once each.
            env = self.fake_gh(Path(tmp), (
                'case "$2" in\n'
                '  *"runs?status="*) printf "11\\n12\\n" ;;\n'
                '  *"/runs/11/jobs"*) echo 1 ;;\n'
                '  *"/runs/12/jobs"*) echo 2 ;;\n'
                'esac\n'))
            result = subprocess.run(
                [sys.executable, "-c",
                 "import sys; sys.path.insert(0, sys.argv[1]); import windows_runner_selector as s; "
                 "print(s.count_lane_backlog('o/r', '99'))", str(HERE)],
                capture_output=True, text=True, encoding="utf-8", env=env, check=True)
            self.assertEqual(result.stdout.strip(), "3")

    def test_a_failed_probe_counts_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            env = self.fake_gh(Path(tmp), "exit 1\n")
            result = subprocess.run(
                [sys.executable, "-c",
                 "import sys; sys.path.insert(0, sys.argv[1]); import windows_runner_selector as s; "
                 "print(s.count_lane_backlog('o/r'))", str(HERE)],
                capture_output=True, text=True, encoding="utf-8", env=env, check=True)
            self.assertEqual(result.stdout.strip(), "0")
            self.assertIn("backlog probe failed", result.stderr)


class CliTests(unittest.TestCase):
    def run_cli(self, *args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            capture_output=True, text=True, encoding="utf-8",
            env={**os.environ, **(env or {})},
        )

    def test_github_output_receives_the_resolved_value(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            result = self.run_cli("--selector", json.dumps(MACPRO_WINDOWS), "--github-output",
                                  env={"GITHUB_OUTPUT": str(out)})
            self.assertEqual(result.returncode, 0, result.stderr)
            line = out.read_text(encoding="utf-8").strip()
            self.assertEqual(json.loads(line.split("=", 1)[1]), MACPRO_WINDOWS)

    def test_route_writes_runs_on_route_and_reason(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            result = self.run_cli("--route", "--event-name", "schedule", "--github-output",
                                  env={"GITHUB_OUTPUT": str(out)})
            self.assertEqual(result.returncode, 0, result.stderr)
            written = dict(line.split("=", 1) for line in out.read_text(encoding="utf-8").splitlines())
            self.assertEqual(json.loads(written["runs_on"]), LANE)
            self.assertEqual(written["route"], "self-hosted")
            self.assertEqual(written["route_reason"], "self-hosted-default")

    def test_without_route_an_empty_selector_stays_hosted(self) -> None:
        result = self.run_cli("--selector", "")
        self.assertEqual(result.stdout.strip(), 'runs_on="windows-latest"')

    def test_refusal_exits_2_and_names_the_label(self) -> None:
        result = self.run_cli("--selector", '["self-hosted","macOS","pulp-gate-fast"]')
        self.assertEqual(result.returncode, 2)
        self.assertIn("refused", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
