#!/usr/bin/env python3
"""The cheap-first gate's verdicts, against stub check runs and a fake clock.

Only a red cheap check blocks. A pending check waits; an absent one waits
only for the grace period; a deadline or an unreadable API starts the native
matrix. A check run from another app is ignored.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).resolve().parents[2] / "tools/ci/cheap_first_gate.py"
_spec = importlib.util.spec_from_file_location("cheap_first_gate", SCRIPT)
assert _spec and _spec.loader
gate = importlib.util.module_from_spec(_spec)
sys.modules["cheap_first_gate"] = gate
_spec.loader.exec_module(gate)

DRIFT, ENFORCE = gate.CHEAP_CHECKS


def run(name: str, status: str = "completed", conclusion: str | None = "success", *,
        run_id: int = 1, started: str = "2026-11-13T10:00:00Z",
        created: str | None = None,
        app: str = "github-actions") -> dict:
    if created is None:
        created = started
    return {"id": run_id, "name": name, "status": status, "conclusion": conclusion,
            "created_at": created, "started_at": started, "html_url": f"https://example/{run_id}",
            "app": {"slug": app}}


class Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


def scripted(timeline: dict[str, list[list[dict] | Exception]]):
    """A fetch that returns each check's next scripted response per call."""
    calls: dict[str, int] = {}

    def fetch(name: str) -> list[dict]:
        i = calls.get(name, 0)
        calls[name] = i + 1
        seq = timeline[name]
        item = seq[min(i, len(seq) - 1)]
        if isinstance(item, Exception):
            raise item
        return item

    fetch.calls = calls  # type: ignore[attr-defined]
    return fetch


def gate_run(fetch, *, deadline=1800.0, grace=600.0, interval=30.0):
    clock = Clock()
    verdict, states = gate.run_gate(fetch, deadline=deadline, absent_grace=grace,
                                    interval=interval, clock=clock, sleep=clock.sleep,
                                    log=lambda _m: None)
    return verdict, states, clock.now


class ClassifyTests(unittest.TestCase):
    def test_newest_run_of_the_name_decides(self) -> None:
        runs = [run(DRIFT, conclusion="failure", run_id=1, started="2026-11-13T10:00:00Z"),
                run(DRIFT, status="in_progress", conclusion=None, run_id=2,
                    started="2026-11-13T10:05:00Z")]
        self.assertEqual(gate.classify_run(DRIFT, runs).state, "pending")
        runs[1].update(status="completed", conclusion="success")
        self.assertEqual(gate.classify_run(DRIFT, runs).state, "green")

    def test_newer_queued_run_wins_without_started_at(self) -> None:
        runs = [run(DRIFT, conclusion="failure", run_id=1,
                    created="2026-11-13T10:00:00Z", started="2026-11-13T10:00:01Z"),
                run(DRIFT, status="queued", conclusion=None, run_id=2,
                    created="2026-11-13T10:05:00Z", started=None)]
        self.assertEqual(gate.classify_run(DRIFT, runs).state, "pending")

    def test_another_app_cannot_post_a_red_check(self) -> None:
        state = gate.classify_run(DRIFT, [run(DRIFT, conclusion="failure", app="impostor")])
        self.assertEqual(state.state, "absent")

    def test_conclusions_map_to_states(self) -> None:
        for conclusion, state in (("failure", "red"), ("timed_out", "red"),
                                  ("startup_failure", "red"), ("action_required", "red"),
                                  ("success", "green"), ("skipped", "green"),
                                  ("neutral", "green"), ("cancelled", "void"),
                                  ("stale", "void"), ("something-new", "void")):
            with self.subTest(conclusion=conclusion):
                self.assertEqual(
                    gate.classify_run(DRIFT, [run(DRIFT, conclusion=conclusion)]).state, state)


class GateTests(unittest.TestCase):
    def test_both_green_passes(self) -> None:
        verdict, _, waited = gate_run(scripted({DRIFT: [[run(DRIFT)]], ENFORCE: [[run(ENFORCE)]]}))
        self.assertEqual((verdict, waited), ("pass", 0))

    def test_waits_for_a_pending_check_then_passes(self) -> None:
        pending = [run(ENFORCE, status="in_progress", conclusion=None)]
        fetch = scripted({DRIFT: [[run(DRIFT)]], ENFORCE: [pending, pending, [run(ENFORCE)]]})
        verdict, _, waited = gate_run(fetch)
        self.assertEqual((verdict, waited), ("pass", 60))
        self.assertEqual(fetch.calls[DRIFT], 3, "terminal checks are refreshed each poll")

    def test_a_terminal_check_can_turn_red_after_first_poll(self) -> None:
        pending = [run(ENFORCE, status="in_progress", conclusion=None)]
        fetch = scripted({DRIFT: [[run(DRIFT)], [run(DRIFT, conclusion="failure", run_id=2,
                                                       created="2026-11-13T10:01:00Z")]],
                          ENFORCE: [pending]})
        self.assertEqual(gate_run(fetch, deadline=60, grace=0)[0], "block")

    def test_a_red_check_blocks_without_waiting_for_the_other(self) -> None:
        pending = [run(ENFORCE, status="queued", conclusion=None)]
        fetch = scripted({DRIFT: [[run(DRIFT, conclusion="failure")]], ENFORCE: [pending]})
        verdict, states, waited = gate_run(fetch)
        self.assertEqual((verdict, waited), ("block", 0))
        self.assertEqual([s.state for s in states], ["red", "pending"])

    def test_a_red_check_that_turns_up_later_blocks(self) -> None:
        pending = [run(DRIFT, status="in_progress", conclusion=None)]
        fetch = scripted({DRIFT: [pending, [run(DRIFT, conclusion="failure")]],
                          ENFORCE: [[run(ENFORCE)]]})
        self.assertEqual(gate_run(fetch)[0], "block")

    def test_skipped_or_cancelled_checks_never_hold_the_gate(self) -> None:
        fetch = scripted({DRIFT: [[run(DRIFT, conclusion="skipped")]],
                          ENFORCE: [[run(ENFORCE, conclusion="cancelled")]]})
        self.assertEqual(gate_run(fetch)[:2][0], "pass")

    def test_an_absent_check_waits_only_for_the_grace_period(self) -> None:
        fetch = scripted({DRIFT: [[run(DRIFT)]], ENFORCE: [[]]})
        verdict, states, waited = gate_run(fetch, grace=120, interval=30)
        self.assertEqual(verdict, "pass")
        self.assertEqual(waited, 120)
        self.assertEqual(states[1].state, "absent")

    def test_the_deadline_starts_the_matrix(self) -> None:
        pending = [run(ENFORCE, status="in_progress", conclusion=None)]
        fetch = scripted({DRIFT: [[run(DRIFT)]], ENFORCE: [pending]})
        verdict, _, waited = gate_run(fetch, deadline=300, interval=30)
        self.assertEqual(verdict, "unknown")
        self.assertLessEqual(waited, 300)

    def test_an_unreadable_api_never_blocks(self) -> None:
        err = urllib.error.URLError("rate limited")
        fetch = scripted({DRIFT: [err], ENFORCE: [err]})
        verdict, _, _ = gate_run(fetch, deadline=120, grace=0)
        self.assertEqual(verdict, "unknown")

    def test_a_read_error_alone_does_not_pass(self) -> None:
        # An error on one poll is not a green; the next good read decides.
        err = urllib.error.URLError("blip")
        fetch = scripted({DRIFT: [err, [run(DRIFT, conclusion="failure")]],
                          ENFORCE: [[run(ENFORCE)]]})
        self.assertEqual(gate_run(fetch, grace=0)[0], "block")


class ApplicabilityTests(unittest.TestCase):
    def test_events(self) -> None:
        self.assertTrue(gate.applies("pull_request", "main", "")[0])
        self.assertTrue(gate.applies("merge_group", "", "")[0])
        self.assertTrue(gate.applies("workflow_dispatch", "", "123")[0])
        self.assertFalse(gate.applies("workflow_dispatch", "", "")[0])
        self.assertFalse(gate.applies("push", "", "")[0])
        self.assertFalse(gate.applies("pull_request", "develop/x", "")[0])


class MainTests(unittest.TestCase):
    def _main(self, *args: str) -> tuple[int, dict[str, str]]:
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "out"
            env = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
            env.update(GITHUB_OUTPUT=str(out), GITHUB_STEP_SUMMARY=str(Path(td) / "sum"),
                       GITHUB_API_URL="http://127.0.0.1:9")
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--repo", "o/r",
                 *args], env=env, capture_output=True, text=True, encoding="utf-8", timeout=60)
            outputs = dict(line.split("=", 1) for line in out.read_text(encoding="utf-8").splitlines()
                           if "=" in line) if out.exists() else {}
            return proc.returncode, outputs

    def test_off_passes_at_once(self) -> None:
        self.assertEqual(self._main("--event", "pull_request", "--base-ref", "main",
                                    "--sha", "a" * 40, "--mode", "off"),
                         (0, {"verdict": "pass", "reason": "PULP_CHEAP_FIRST_MODE=off"}))

    def test_push_passes_at_once(self) -> None:
        rc, out = self._main("--event", "push", "--sha", "a" * 40)
        self.assertEqual((rc, out["verdict"]), (0, "pass"))

    def test_unreachable_api_fails_open_with_exit_zero(self) -> None:
        rc, out = self._main("--event", "merge_group", "--sha", "a" * 40,
                             "--deadline", "1", "--interval", "1", "--absent-grace", "0")
        self.assertEqual((rc, out["verdict"]), (0, "unknown"))

    def test_missing_sha_fails_open(self) -> None:
        rc, out = self._main("--event", "merge_group", "--sha", "")
        self.assertEqual((rc, out["verdict"]), (0, "unknown"))


class RequiredContextTests(unittest.TestCase):
    def test_every_cheap_check_is_a_required_context_on_main(self) -> None:
        ruleset = json.loads((ROOT / ".github/rulesets/main-protection.json").read_text(encoding="utf-8"))
        contexts = {
            c["context"]
            for rule in ruleset.get("rules", [])
            for c in (rule.get("parameters") or {}).get("required_status_checks", [])
        }
        self.assertIn("macos", contexts, "control: the ruleset parse must find `macos`")
        for name in gate.CHEAP_CHECKS:
            self.assertIn(name, contexts)


if __name__ == "__main__":
    unittest.main()
