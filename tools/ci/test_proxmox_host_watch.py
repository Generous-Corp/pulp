#!/usr/bin/env python3
"""Contract for the off-host reader of the macpro Proxmox CI host's health signals."""

from __future__ import annotations

import copy
import datetime as dt
import io
import json
import pathlib
import subprocess
import sys
import unittest
from contextlib import redirect_stdout
from unittest import mock

CI = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(CI))

import proxmox_host_watch as w  # noqa: E402

# Recorded on 2026-10-15 from the host, addresses redacted: the preamble slot's
# unit failed after 96 restarts, health.json absent, the reaper skipping VM 203
# (guest agent dead) and VM 200 (a live runner's lease) on every pass.
FIXTURE = CI / "fixtures" / "proxmox_host_watch_20261015.json"
LIVE_200 = [{"name": "pulp-ci-ephemeral-200-80bcc2b1-835e-4eb8-a3b1-93b229fd128e",
             "status": "online"}]


def obs() -> dict:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def keys(findings) -> list[str]:
    return sorted(f["key"] for f in findings)


class AnalyzeTests(unittest.TestCase):
    def test_recorded_outage_raises_exactly_the_three_real_problems(self) -> None:
        findings = w.analyze(obs(), LIVE_200)
        self.assertEqual(keys(findings), [
            "health:unknown", "skip:203", "unit:pulp-ephemeral-pool@preamble.service"])
        unit = next(f for f in findings if f["key"].startswith("unit:"))
        self.assertIn("no free clone id in 203..203", unit["body"])
        self.assertIn("203 (pulp-ci-ephemeral-203, running)", unit["body"])
        self.assertIn("systemctl reset-failed pulp-ephemeral-pool@preamble.service", unit["body"])
        skip = next(f for f in findings if f["key"] == "skip:203")
        self.assertIn("qm stop 203", skip["body"])

    def test_a_skipped_vm_with_an_online_runner_is_quiet(self) -> None:
        # Negative control: VM 200 is skipped every pass because a live job
        # holds its lease; its runner is online, so it is not an alarm.
        self.assertNotIn("skip:200", keys(w.analyze(obs(), LIVE_200)))
        self.assertIn("skip:200", keys(w.analyze(obs(), [])))

    def test_without_a_runner_census_no_skip_is_claimed(self) -> None:
        self.assertFalse([k for k in keys(w.analyze(obs(), None)) if k.startswith("skip:")])

    def test_skip_needs_four_passes_and_the_span(self) -> None:
        def skips(passes, step):
            o = obs()
            o["skips"] = [{"ts": o["now"] - 7200 + i * step, "pid": str(i), "vmid": "203",
                           "reason": "guest state cannot be proved"} for i in range(passes)]
            return keys(w.analyze(o, LIVE_200))
        self.assertNotIn("skip:203", skips(3, 1800))
        self.assertIn("skip:203", skips(4, 900))
        self.assertNotIn("skip:203", skips(4, 60))

    def test_a_freshly_failed_unit_waits_out_the_grace(self) -> None:
        o = obs()
        for unit in o["failed_units"]:
            unit["since"] = str(o["now"] - 300)
        self.assertFalse([k for k in keys(w.analyze(o, LIVE_200)) if k.startswith("unit:")])

    def test_health_file_states(self) -> None:
        o = obs()
        now = dt.datetime.fromtimestamp(o["now"], dt.timezone.utc)

        def health(**fields):
            o["health"] = {"present": True, "raw": json.dumps(fields)}
            return keys(w.analyze(o, LIVE_200))

        fresh = (now - dt.timedelta(minutes=5)).isoformat().replace("+00:00", "Z")
        stale = (now - dt.timedelta(minutes=45)).isoformat().replace("+00:00", "Z")
        self.assertNotIn("health:unknown", health(state="healthy", checked_at=fresh))
        self.assertNotIn("health:state", health(state="healthy", checked_at=fresh))
        self.assertIn("health:unknown", health(state="healthy", checked_at=stale))
        self.assertIn("health:state", health(state="unhealthy", checked_at=fresh,
                                             reinstall_command="x --install-staged"))
        self.assertIn("health:state", health(state="healthy", checked_at=fresh,
                                             drift={"age_seconds": 7200}))
        o["health"] = {"present": True, "raw": "{not json"}
        self.assertIn("health:unknown", keys(w.analyze(o, LIVE_200)))

    def test_addresses_are_redacted_from_issue_bodies(self) -> None:
        o = obs()
        o["failed_units"][0]["last_error"] = "ERROR: cannot reach 192.168.1.20 for clone"
        body = "".join(f["body"] for f in w.analyze(o, LIVE_200))
        self.assertNotIn("192.168.1.20", body)
        self.assertIn("<addr>", body)


class FakeGh(w.GitHub):
    def __init__(self, issues):
        super().__init__("gh", runner=self._run)
        self.issues = issues
        self.calls: list[list[str]] = []

    def _run(self, argv, stdin=None):
        self.calls.append(argv[2:])
        out = {"items": self.issues} if "/search/issues" in argv else {}
        return subprocess.CompletedProcess(argv, 0, json.dumps(out), "")


def issue(number, key, state="open"):
    return {"number": number, "state": state, "title": f"{w.TITLE_PREFIX} x",
            "body": f"b\n<!-- {w.KEY_MARKER}: {key} -->\n"}


class ReconcileTests(unittest.TestCase):
    def test_creates_updates_and_closes_by_key(self) -> None:
        gh = FakeGh([issue(5, "skip:203"), issue(6, "unit:old.service"),
                     issue(7, "health:unknown", state="closed")])
        findings = w.analyze(obs(), LIVE_200)
        actions = w.reconcile(gh, "o/r", findings, dry_run=False)
        self.assertIn("update #5 skip:203", actions)
        self.assertIn("update #7 health:unknown", actions)  # reopened in place
        self.assertIn("create unit:pulp-ephemeral-pool@preamble.service", actions)
        self.assertIn("close #6 unit:old.service", actions)
        self.assertNotIn("close #7 health:unknown", actions)

    def test_an_unreadable_runner_census_never_closes_a_skip_issue(self) -> None:
        o = obs()
        o["unreadable"] = []
        gh = FakeGh([issue(5, "skip:203")])
        actions = w.reconcile(gh, "o/r", w.analyze(o, None), dry_run=False,
                              unreadable=w.unreadable_kinds(o, None))
        self.assertNotIn("close #5 skip:203", actions)
        self.assertIn("keep #5 skip:203 (evidence unreadable)", actions)

    def test_a_failed_probe_command_never_closes_its_kind(self) -> None:
        # systemctl failed on the host: the empty unit list is not "all healthy".
        o = obs()
        o["failed_units"] = []
        o["unreadable"] = ["unit"]
        gh = FakeGh([issue(6, "unit:pulp-ephemeral-pool@preamble.service")])
        actions = w.reconcile(gh, "o/r", w.analyze(o, LIVE_200), dry_run=False,
                              unreadable=w.unreadable_kinds(o, LIVE_200))
        self.assertIn("keep #6 unit:pulp-ephemeral-pool@preamble.service (evidence unreadable)",
                      actions)
        # Negative control: with the probe readable, the cleared unit closes.
        o["unreadable"] = []
        actions = w.reconcile(FakeGh([issue(6, "unit:pulp-ephemeral-pool@preamble.service")]),
                              "o/r", w.analyze(o, LIVE_200), dry_run=False,
                              unreadable=w.unreadable_kinds(o, LIVE_200))
        self.assertIn("close #6 unit:pulp-ephemeral-pool@preamble.service", actions)

    @unittest.skipUnless(sys.platform != "win32", "the stubbed tools are POSIX shell scripts")
    def test_the_probe_marks_a_failing_command_unreadable(self) -> None:
        # Run the real probe with systemctl, journalctl and qm all failing.
        import os, tempfile
        with tempfile.TemporaryDirectory() as td:
            for tool in ("systemctl", "journalctl", "qm"):
                path = pathlib.Path(td) / tool
                path.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
                path.chmod(0o755)
            env = dict(os.environ, PATH=f"{td}:/usr/bin:/bin")
            out = subprocess.run([sys.executable, "-"], input=w.REMOTE_PROBE, env=env,
                                 capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(sorted(json.loads(out.stdout)["unreadable"]), ["skip", "unit"])

    def test_dry_run_changes_nothing(self) -> None:
        gh = FakeGh([issue(6, "unit:old.service")])
        w.reconcile(gh, "o/r", w.analyze(obs(), LIVE_200), dry_run=True)
        self.assertEqual([c for c in gh.calls if "/search/issues" not in c], [])

    def test_an_unreadable_host_never_closes_anything(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out), mock.patch.object(
                w, "observe", side_effect=RuntimeError("ssh macpro failed")), \
                mock.patch.object(w, "reconcile") as reconcile:
            rc = w.main([])
        self.assertEqual(rc, 2)
        reconcile.assert_not_called()


if __name__ == "__main__":
    unittest.main()
