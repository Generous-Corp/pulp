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


def _class_label(event: str) -> str:
    spec = json.loads(CONTRACT.read_text())["event_class_v2"]
    event = st.DISPATCH_EVENT_ALIASES.get(event, event)
    return next(row["label"] for row in spec["classes"] if row["event"] == event)


def _class_hosts(snapshot: st.Snapshot, label: str) -> list[str]:
    return sorted({reg.host_id for reg in snapshot.registrations
                   if reg.repo == REPO and reg.class_label == label})


def _v2_gate_registrations(host: str) -> list[st.Registration]:
    """What a new host's event-class gate lane advertises, built from the
    contract's own event_class_v2 spec rather than copied from a real host."""
    spec = json.loads(CONTRACT.read_text())["event_class_v2"]
    base = next(l["expect"] for l in _raw()["lanes"] if l["variable"] == spec["variable"])
    base = [label for label in base if label not in spec["omit_labels"]]
    return [st.Registration(
        profile=f"{host}-macos-fleet", host_id=host, lane=spec["lane_id"],
        repo=spec["repo"], class_label=row["label"], labels=[*base, row["label"]],
        workflows=[row["workflow"]]) for row in spec["classes"]]


def _gate_rows(snapshot: st.Snapshot):
    contract = gate.load_contract(CONTRACT)
    return [r for r in st.evaluate(contract, snapshot, WORKFLOWS, REPO)
            if r.variable == GATE and r.source == "expect"]


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
        # The serving hosts are whatever the snapshot declares, never a list
        # written here: every host with a registration for that event's class
        # label in this repo. The contract's own profile list is narrower.
        snapshot = st.load_snapshot(SNAPSHOT)
        for row in rows:
            expected = _class_hosts(snapshot, _class_label(row.event))
            self.assertTrue(expected, row.event)
            self.assertEqual(row.served_by, expected, row.event)

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
        snapshot = st.load_snapshot(SNAPSHOT)
        for row in rows:
            self.assertEqual(row.verdict, st.REACHABLE, row.detail)
            minting = sorted({reg.host_id for reg in snapshot.registrations
                              if reg.repo == REPO and row.workflow in reg.workflows
                              and set(row.labels) <= set(reg.labels)})
            self.assertEqual(row.served_by, minting)

    def test_release_unset_fallback_is_reported_unserved_but_not_blocking(self):
        rows = _for(_rows(), RELEASE, source="unset_fallback")
        self.assertTrue(rows)
        self.assertTrue(all(r.verdict == st.UNSERVED for r in rows))
        self.assertFalse(any(r.blocking for r in rows))

    def test_release_break_glass_rollback_is_served(self):
        rows = _for(_rows(), RELEASE, source=st.ROLLBACK_SOURCE)
        self.assertEqual([r.verdict for r in rows], [st.HOSTED])
        self.assertFalse(any(r.blocking for r in rows))

    def test_release_contract_does_not_call_unsetting_a_rollback(self):
        purpose = next(l for l in _raw()["lanes"] if l["variable"] == RELEASE)["purpose"]
        self.assertIn("UNSETTING this variable is NOT a rollback", purpose)
        guide = (REPO_ROOT / "docs" / "guides" / "local-ci.md").read_text()
        self.assertNotIn("Unsetting the variable is the break-glass rollback", guide)
        self.assertIn("Unsetting the variable is not a rollback", guide)

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


class FleetScaling(unittest.TestCase):
    """Adding or removing a machine must just work: no host is named in code."""

    def setUp(self):
        self.snapshot = st.load_snapshot(SNAPSHOT)
        self.gate_hosts = sorted({h for r in _gate_rows(self.snapshot) for h in r.served_by})
        self.assertGreaterEqual(len(self.gate_hosts), 2, "scaling tests need 2+ gate hosts")

    def test_a_new_host_with_a_gate_lane_serves_the_gate(self):
        snap = copy.deepcopy(self.snapshot)
        new_host = "m7"
        self.assertNotIn(new_host, self.gate_hosts)
        snap.registrations.extend(_v2_gate_registrations(new_host))
        for row in _gate_rows(snap):
            self.assertEqual(row.verdict, st.REACHABLE, row.event)
            self.assertIn(new_host, row.served_by, row.event)
            self.assertEqual(row.served_by, sorted([*self.gate_hosts, new_host]))

    def test_removing_one_host_leaves_the_gate_reachable_by_the_rest(self):
        gone = self.gate_hosts[0]
        snap = copy.deepcopy(self.snapshot)
        snap.registrations = [r for r in snap.registrations if r.host_id != gone]
        for row in _gate_rows(snap):
            self.assertEqual(row.verdict, st.REACHABLE, row.event)
            self.assertEqual(row.served_by, self.gate_hosts[1:])

    def test_removing_every_gate_registration_fails_the_required_gate(self):
        classes = {row["label"] for row in _raw()["event_class_v2"]["classes"]}
        snap = copy.deepcopy(self.snapshot)
        snap.registrations = [r for r in snap.registrations if r.class_label not in classes]
        rows = _gate_rows(snap)
        self.assertTrue(rows)
        self.assertTrue(all(r.verdict == st.UNSERVED and r.blocking for r in rows))
        contract = gate.load_contract(CONTRACT)
        self.assertEqual(st.exit_code(st.evaluate(contract, snap, WORKFLOWS, REPO), []), 1)


class BreakGlassRollback(unittest.TestCase):
    """A rollback the contract documents must be one the fleet can serve."""

    def _release(self, raw):
        return next(l for l in raw["lanes"] if l["variable"] == RELEASE)

    def test_unserved_fallback_without_a_rollback_fails_the_required_lane(self):
        raw = _raw()
        self._release(raw).pop("break_glass_rollback")
        rows = _for(_rows(raw), RELEASE, source=st.ROLLBACK_SOURCE)
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0].blocking)
        self.assertIn("declares no break_glass_rollback", rows[0].detail)
        self.assertEqual(st.exit_code(_rows(raw), []), 1)

    def test_an_unserved_rollback_fails_the_required_lane(self):
        raw = _raw()
        self._release(raw)["break_glass_rollback"] = [
            "self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-vm"]
        rows = _for(_rows(raw), RELEASE, source=st.ROLLBACK_SOURCE)
        self.assertTrue(rows)
        self.assertTrue(all(r.verdict == st.UNSERVED and r.blocking for r in rows))
        self.assertEqual(st.exit_code(_rows(raw), []), 1)

    def test_a_served_fallback_needs_no_rollback(self):
        raw = _raw()
        lane = self._release(raw)
        lane.pop("break_glass_rollback")
        lane["unset_fallback"] = ["macos-15"]
        self.assertEqual(_for(_rows(raw), RELEASE, source=st.ROLLBACK_SOURCE), [])

    def test_advisory_lane_with_unserved_fallback_is_not_blocking(self):
        raw = _raw()
        lane = self._release(raw)
        lane.pop("break_glass_rollback")
        lane["severity"] = "advisory"
        rows = _rows(raw)
        self.assertEqual(_for(rows, RELEASE, source=st.ROLLBACK_SOURCE), [])


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


JOBS_FIXTURE = HERE / "fixtures" / "observed_supply_jobs.json"


class ObservedSupply(unittest.TestCase):
    """Declared registrations vs a real captured jobs-API response."""

    def setUp(self):
        self.snapshot = st.load_snapshot(SNAPSHOT)
        self.in_repo = [r for r in self.snapshot.registrations if r.repo == REPO]
        self.records = gate.parse_service_records(
            json.loads(JOBS_FIXTURE.read_text())["jobs"])
        self.now = max(r.completed_at for r in self.records if r.completed_at)
        self.since = self.now - gate.timedelta(hours=720)
        self.required = st.required_registrations(
            gate.load_contract(CONTRACT), self.snapshot, WORKFLOWS, REPO)
        self.gate_classes = {row["label"] for row in _raw()["event_class_v2"]["classes"]}

    def _classify(self, regs=None, all_regs=None, records=None):
        return st.classify_observed(regs if regs is not None else self.required,
                                    records if records is not None else self.records,
                                    self.since, all_regs if all_regs is not None else self.in_repo)

    def test_every_gate_registration_is_observed_in_the_real_capture(self):
        verdicts, undeclared = self._classify()
        gate_regs = [r for r in self.required if r.class_label in self.gate_classes]
        self.assertEqual(len(gate_regs), len(self.gate_classes) * len(
            {r.host_id for r in gate_regs}))
        by_handle = {v.registration: v for v in verdicts}
        for reg in gate_regs:
            self.assertEqual(by_handle[reg.handle].verdict, st.OBSERVED, reg.handle)
            self.assertIsNotNone(by_handle[reg.handle].last_seen)
        self.assertEqual(undeclared, [])

    def test_slot_suffixed_runner_names_attribute_to_their_lane(self):
        slot = next(r for r in self.records if "-slot2-" in r.runner_name
                    and "self-hosted" in r.labels)
        owners = st.attribute(slot.runner_name, slot.labels, self.in_repo)
        self.assertTrue(owners)
        self.assertTrue(all(slot.runner_name.startswith(f"{o.host_id}-{o.lane}-")
                            for o in owners))

    def test_a_class_label_selects_exactly_one_registration(self):
        job = next(r for r in self.records if r.labels & self.gate_classes)
        owners = st.attribute(job.runner_name, job.labels, self.in_repo)
        self.assertEqual(len(owners), 1)
        self.assertIn(owners[0].class_label, job.labels)

    def test_a_machine_missing_from_the_snapshot_is_undeclared_observed(self):
        gone = sorted({r.host_id for r in self.required
                       if r.class_label in self.gate_classes})[0]
        kept = [r for r in self.in_repo if r.host_id != gone]
        _v, undeclared = self._classify(
            regs=[r for r in self.required if r.host_id != gone], all_regs=kept)
        self.assertTrue(undeclared)
        self.assertTrue(all(u.verdict == st.UNDECLARED_OBSERVED for u in undeclared))
        self.assertTrue(all(u.registration.startswith(f"{gone}-") for u in undeclared))

    def test_a_declared_machine_with_demand_but_no_jobs_is_not_observed(self):
        extra = _v2_gate_registrations("m7")
        verdicts, _u = self._classify(regs=extra, all_regs=[*self.in_repo, *extra])
        self.assertTrue(verdicts)
        for v in verdicts:
            self.assertEqual(v.verdict, st.NOT_OBSERVED, v.registration)
            self.assertGreater(v.demand, 0)

    def test_foreign_pools_are_out_of_the_undeclared_scope(self):
        foreign = gate.ServiceRecord(
            runner_name="linux-ephr-2117-12",
            labels={"self-hosted", "Linux", "X64", "pulp-build-linux-x64"},
            status="completed", completed_at=self.now)
        _v, undeclared = self._classify(records=[*self.records, foreign])
        self.assertEqual(undeclared, [])

    def test_jobs_outside_the_window_do_not_count(self):
        verdicts, _u = st.classify_observed(self.required, self.records,
                                            self.now + gate.timedelta(hours=1), self.in_repo)
        self.assertTrue(all(v.verdict == st.IDLE for v in verdicts))

    def test_live_findings_are_advisory_never_errors(self):
        findings = gate.observed_supply_findings(
            gate.load_contract(CONTRACT), CONTRACT, WORKFLOWS, REPO, [], self.now)
        self.assertTrue(findings)
        self.assertFalse(any(f.level == gate.ERROR for f in findings))
        findings = gate.observed_supply_findings(
            gate.load_contract(CONTRACT), CONTRACT, WORKFLOWS, REPO, self.records, self.now)
        self.assertFalse(any(f.level == gate.ERROR for f in findings))
        self.assertTrue(any("OBSERVED, last seen" in f.detail for f in findings))


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
