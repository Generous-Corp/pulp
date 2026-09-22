#!/usr/bin/env python3
"""Tests for the offline routing-reachability audit and routing overrides.

The load-bearing cases run the REAL contract, the REAL workflow tree, and the
REAL advertised-labels snapshot: the required macOS gate must come out
REACHABLE, and it only does because the dispatched label set is projected
through the event-class rewrite. The raw repo variable is asserted UNSERVED
beside it, so a regression that stops projecting cannot pass quietly.

The clock is always pinned. A test that read the wall clock against a checked-in
expiry would turn every unrelated PR red on the expiry date; the live hourly
sweep enforces expiry against the real clock instead.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from datetime import date
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"
CONTRACT = HERE / "runner_topology.json"
SNAPSHOT = HERE / "fleet_advertised_labels.json"
CHECKER = HERE / "runner_topology_check.py"

sys.path.insert(0, str(HERE))
_spec = importlib.util.spec_from_file_location(
    "runner_topology_check", HERE / "runner_topology_check.py")
gate = importlib.util.module_from_spec(_spec)
sys.modules["runner_topology_check"] = gate
_spec.loader.exec_module(gate)
import runner_topology_static as st  # noqa: E402

REPO = "Generous-Corp/pulp"
GATE = "PULP_LOCAL_MACOS_RUNS_ON_JSON"
RELEASE = "PULP_RELEASE_MACOS_RUNS_ON_JSON"
OVERFLOW = "PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON"


def _raw() -> dict:
    return json.loads(CONTRACT.read_text())


def _pinned_today(raw: dict) -> date:
    # The newest `since` is always inside every override's live window, so the
    # real-config assertions survive a renewal without editing this file.
    dates = [date.fromisoformat(o["since"]) for o in raw.get("overrides", [])]
    return max(dates) if dates else date(2026, 9, 22)


def _load(raw: dict) -> "gate.Contract":
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
        json.dump(raw, fh)
    return gate.load_contract(Path(fh.name))


def _rows(raw: dict | None = None, snapshot: st.Snapshot | None = None):
    contract = gate.load_contract(CONTRACT) if raw is None else _load(raw)
    snap = snapshot or st.load_snapshot(SNAPSHOT)
    return st.evaluate(contract, snap, WORKFLOWS, REPO)


def _for(rows, variable, source="expect"):
    return [r for r in rows if r.variable == variable and r.source == source]


def _snapshot(*regs: dict) -> st.Snapshot:
    return st.Snapshot([st.Registration(
        profile="p", host_id=r.get("host", "h"), lane=r.get("lane", "l"),
        repo=r.get("repo", REPO), class_label=r.get("class_label"),
        labels=r["labels"], workflows=r["workflows"]) for r in regs])


class RealConfig(unittest.TestCase):
    def test_required_gate_is_reachable_on_every_dispatched_event(self):
        rows = _for(_rows(), GATE)
        events = {r.event for r in rows}
        self.assertEqual(events, {"pull_request", "merge_group", "workflow_dispatch"})
        for row in rows:
            self.assertEqual(row.verdict, st.REACHABLE, (row.event, row.detail))
            self.assertEqual(row.workflow, "Build and Test")
            self.assertNotIn("pulp-gate-fast", row.labels)
        # m1 and m5 serve the gate as well as m3; the contract's own profile
        # list names only m3, so this reads supply from the snapshot.
        hosts = {h.split(":")[0] for r in rows for h in r.detail[3:].split(", ")}
        self.assertEqual(hosts, {"m1", "studio", "m5"})

    def test_raw_gate_variable_is_unserved_without_the_event_projection(self):
        # Negative control for the projection: the pre-dispatch selector still
        # carries the legacy shared label, which no event-class registration
        # advertises. If this ever reads REACHABLE the snapshot changed shape
        # and the projection test above proves nothing.
        contract = gate.load_contract(CONTRACT)
        lane = next(l for l in contract.lanes if l.variable == GATE)
        verdict, detail = st.reach(lane.expect, "Build and Test", REPO,
                                   st.load_snapshot(SNAPSHOT))
        self.assertEqual(verdict, st.UNSERVED)
        self.assertIn("pulp-gate-fast", detail)

    def test_release_lane_is_reachable_on_declared_supply(self):
        rows = _for(_rows(), RELEASE)
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(row.verdict, st.REACHABLE, row.detail)
            self.assertIn("m5:pulp-release", row.detail)

    def test_release_rollback_fallback_is_reported_unserved_but_not_blocking(self):
        rows = _for(_rows(), RELEASE, source="unset_fallback")
        self.assertTrue(rows)
        self.assertTrue(all(r.verdict == st.UNSERVED for r in rows))
        self.assertFalse(any(r.blocking for r in rows))

    def test_overflow_lane_is_a_sentinel_backed_by_an_override(self):
        rows = _for(_rows(), OVERFLOW)
        self.assertEqual([r.verdict for r in rows], [st.SENTINEL])
        raw = _raw()
        contract = gate.load_contract(CONTRACT)
        statuses, problems = st.check_overrides(raw, contract, _pinned_today(raw))
        self.assertEqual(problems, [])
        self.assertIn(OVERFLOW, {s.subject for s in statuses})

    def test_uncovered_supervisors_are_unknown_not_unserved(self):
        rows = _for(_rows(), "PULP_LOCAL_LINUX_RUNS_ON_JSON")
        self.assertEqual([r.verdict for r in rows], [st.UNKNOWN])

    def test_undeclared_selector_variables_are_listed(self):
        undeclared = {r.variable for r in _rows() if r.verdict == st.UNDECLARED}
        self.assertIn("PULP_RELEASE_DARWIN_ARM64_RUNS_ON_JSON", undeclared)
        self.assertNotIn(GATE, undeclared)
        self.assertNotIn("PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON", undeclared)

    def test_cli_static_mode_is_green_on_the_real_config(self):
        today = _pinned_today(_raw()).isoformat()
        proc = subprocess.run(
            [sys.executable, str(CHECKER), "--mode=static", "--today", today],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("DECLARED supply", proc.stdout)
        self.assertIn("REACHABLE", proc.stdout)


class Reachability(unittest.TestCase):
    def test_a_typo_label_is_unserved_and_fails_the_required_lane(self):
        raw = _raw()
        lane = next(l for l in raw["lanes"] if l["variable"] == RELEASE)
        lane["expect"] = ["self-hosted", "macOS", "ARM64", "pulp-build-vm-release-typo"]
        rows = _for(_rows(raw), RELEASE)
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(row.verdict, st.UNSERVED)
            self.assertIn("pulp-build-vm-release-typo", row.detail)
            self.assertTrue(row.blocking)
        self.assertEqual(st.exit_code(_rows(raw), []), 1)

    def test_cli_exits_nonzero_on_the_typo_fixture(self):
        raw = _raw()
        next(l for l in raw["lanes"] if l["variable"] == RELEASE)["expect"] = [
            "self-hosted", "macOS", "ARM64", "pulp-build-vm-release-typo"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "runner_topology.json"
            path.write_text(json.dumps(raw))
            proc = subprocess.run(
                [sys.executable, str(CHECKER), "--mode=static", "--contract", str(path),
                 "--snapshot", str(SNAPSHOT), "--workflows-dir", str(WORKFLOWS),
                 "--today", _pinned_today(raw).isoformat()],
                capture_output=True, text=True)
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("UNSERVED", proc.stdout)

    def test_workflow_mismatch_is_unserved(self):
        snap = _snapshot({"labels": ["self-hosted", "macOS", "x"], "workflows": ["Other"]})
        verdict, detail = st.reach(["self-hosted", "x"], "Build and Test", REPO, snap)
        self.assertEqual(verdict, st.UNSERVED)
        self.assertIn("not one they mint for", detail)

    def test_repo_mismatch_is_unserved(self):
        snap = _snapshot({"labels": ["self-hosted", "x"], "workflows": ["W"],
                          "repo": "someone/else"})
        verdict, _ = st.reach(["self-hosted", "x"], "W", REPO, snap)
        self.assertEqual(verdict, st.UNSERVED)

    def test_labels_match_case_insensitively(self):
        snap = _snapshot({"labels": ["self-hosted", "macOS", "ARM64"], "workflows": ["W"]})
        verdict, _ = st.reach(["Self-Hosted", "macos", "arm64"], "W", REPO, snap)
        self.assertEqual(verdict, st.REACHABLE)

    def test_a_registration_carrying_only_some_labels_does_not_serve(self):
        snap = _snapshot({"labels": ["self-hosted", "a"], "workflows": ["W"]},
                         {"labels": ["self-hosted", "b"], "workflows": ["W"]})
        verdict, detail = st.reach(["self-hosted", "a", "b"], "W", REPO, snap)
        self.assertEqual(verdict, st.UNSERVED)
        self.assertIn("no single registration", detail)

    def test_unknown_workflow_name_is_unknown_not_reachable(self):
        snap = _snapshot({"labels": ["self-hosted", "a"], "workflows": ["W"]})
        verdict, _ = st.reach(["self-hosted", "a"], None, REPO, snap)
        self.assertEqual(verdict, st.UNKNOWN)

    def test_reusable_workflow_name_is_not_guessed(self):
        with tempfile.TemporaryDirectory() as tmp:
            wf = Path(tmp) / "reuse.yml"
            wf.write_text("name: Reuse\non:\n  workflow_call:\njobs:\n"
                          "  a:\n    runs-on: ${{ fromJSON(vars.PULP_X_RUNS_ON_JSON) }}\n")
            self.assertEqual(st.consuming_workflows("PULP_X_RUNS_ON_JSON", Path(tmp)),
                             [("reuse.yml", None)])


class Overrides(unittest.TestCase):
    def setUp(self):
        self.raw = _raw()
        self.contract = gate.load_contract(CONTRACT)
        self.override = next(o for o in self.raw["overrides"]
                             if o["subject"] == OVERFLOW)

    def _problems(self, raw: dict, today: date):
        return st.check_overrides(raw, _load(raw), today)[1]

    def test_expired_override_is_a_finding(self):
        expires = date.fromisoformat(self.override["expires"])
        after = date.fromordinal(expires.toordinal() + 1)
        kinds = [k for k, _, _ in self._problems(self.raw, after)]
        self.assertIn("override-expired", kinds)
        self.assertEqual(self._problems(self.raw, expires), [])

    def test_expired_override_fails_the_cli(self):
        expires = date.fromisoformat(self.override["expires"])
        after = date.fromordinal(expires.toordinal() + 1).isoformat()
        proc = subprocess.run(
            [sys.executable, str(CHECKER), "--mode=static", "--today", after],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("override-expired", proc.stdout)

    def test_live_mode_findings_include_expiry(self):
        expires = date.fromisoformat(self.override["expires"])
        after = date.fromordinal(expires.toordinal() + 1)
        findings = gate.override_findings(self.contract, after)
        self.assertTrue(any(f.level == gate.ERROR and f.kind == "override-expired"
                            for f in findings))
        ok = gate.override_findings(self.contract, _pinned_today(self.raw))
        self.assertFalse(any(f.level == gate.ERROR for f in ok))
        self.assertTrue(any(f.kind == "override" for f in ok))

    def test_unknown_subject_is_a_finding(self):
        raw = copy.deepcopy(self.raw)
        raw["overrides"].append(dict(self.override, id="ghost",
                                     subject="PULP_NOT_A_LANE_RUNS_ON_JSON"))
        kinds = {k for k, s, _ in self._problems(raw, _pinned_today(raw)) if s == "ghost"}
        self.assertEqual(kinds, {"override-unknown-subject"})

    def test_lane_citing_a_missing_override_is_a_finding(self):
        raw = copy.deepcopy(self.raw)
        raw["overrides"] = []
        kinds = [k for k, _, _ in self._problems(raw, _pinned_today(self.raw))]
        self.assertEqual(kinds, ["override-missing"])

    def test_override_whose_value_no_longer_matches_the_lane_is_stale(self):
        raw = copy.deepcopy(self.raw)
        next(l for l in raw["lanes"] if l["variable"] == OVERFLOW)["expect"] = ["macos-15"]
        kinds = [k for k, _, _ in self._problems(raw, _pinned_today(raw))]
        self.assertIn("override-stale", kinds)

    def test_missing_fields_and_bad_dates_are_findings(self):
        raw = copy.deepcopy(self.raw)
        raw["overrides"][0] = dict(self.override, owner="", expires="soon")
        kinds = [k for k, _, _ in self._problems(raw, _pinned_today(self.raw))]
        self.assertEqual(kinds.count("override-invalid"), 2)


if __name__ == "__main__":
    unittest.main()
