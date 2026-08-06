#!/usr/bin/env python3
"""Unit tests for runner_topology_check.py.

Fixtures mirror the real fleet inventory: three Mac Studios carrying
`pulp-build`/`pulp-build-studio`, an M5 preamble box, an ephemeral Linux
runner, and an ephemeral macOS sanitizer VM. The black-hole cases reproduce
the live routing state in which the macOS overflow lane pointed at
`pulp-build-vm`, a label no runner carried and no job had ever been served on.

No network: every test drives the pure check() over injected state.
"""

from __future__ import annotations

import importlib.util
import json
import os
import plistlib
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "runner_topology_check", HERE / "runner_topology_check.py")
gate = importlib.util.module_from_spec(_spec)
sys.modules["runner_topology_check"] = gate
_spec.loader.exec_module(gate)

_lspec = importlib.util.spec_from_file_location(
    "runner_labels", HERE / "runner_labels.py")
labels_mod = importlib.util.module_from_spec(_lspec)
sys.modules["runner_labels"] = labels_mod
_lspec.loader.exec_module(labels_mod)

CI = HERE.parent / "ci"
LAUNCHD = HERE.parent / "launchd"


# ── Fixtures drawn from the real fleet ──────────────────────────────────

STUDIO_LABELS = ["self-hosted", "macOS", "ARM64",
                 "pulp-build", "pulp-build-studio", "pulp-preamble"]

LIVE_RUNNERS = [
    {"name": "pulp-studio-01", "status": "online", "labels": STUDIO_LABELS},
    {"name": "pulp-studio-02", "status": "online", "labels": STUDIO_LABELS},
    {"name": "pulp-studio-03", "status": "online", "labels": STUDIO_LABELS},
    {"name": "pulp-preamble-m5", "status": "online",
     "labels": ["self-hosted", "macOS", "ARM64", "pulp-preamble"]},
    {"name": "linux-ephr-2117-12", "status": "online",
     "labels": ["self-hosted", "ARM64", "Linux",
                "pulp-build-linux", "pulp-host-macstudio"]},
]

STUDIO_LANE = ["self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-studio"]
VM_LANE = ["self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-vm"]


def runners(specs=None):
    return gate.parse_runners(list(specs if specs is not None else LIVE_RUNNERS))


def lane(**kw):
    base = dict(variable="PULP_TEST_RUNS_ON_JSON", purpose="test lane",
                expect=STUDIO_LANE, provisioning="persistent",
                severity="required", hosts=[], unset_fallback=None,
                require_explicit_value=False)
    base.update(kw)
    return gate.Lane(**base)


def contract(lanes, unset=(), hosted=("macos-15",), sentinels=("local-only",)):
    return gate.Contract(
        lanes=list(lanes),
        github_hosted_labels=set(hosted),
        sentinels=set(sentinels),
        must_remain_unset=list(unset),
        must_remain_unset_why="paid overflow, off for cost",
        routing_controls={},
        lookback_hours=720,
        runs_per_workflow=20,
    )


class TestRoutingControls(unittest.TestCase):
    def test_exact_control_value_is_enforced(self):
        c = contract([])
        c.routing_controls = {
            "PULP_LOCAL_MAC_RUNNER_LABEL":
                gate.RoutingControl(expect="pulp-gate-fast",
                                    unset_fallback="pulp-gate-fast")
        }
        findings = gate.check(
            c, runners(), {"PULP_LOCAL_MAC_RUNNER_LABEL": "pulp-build-vm"}, [])
        self.assertEqual(kinds(findings), ["control-drift"])

    def test_exact_control_value_passes(self):
        c = contract([])
        c.routing_controls = {
            "PULP_LOCAL_MAC_RUNNER_LABEL":
                gate.RoutingControl(expect="pulp-gate-fast",
                                    unset_fallback="pulp-gate-fast")
        }
        findings = gate.check(
            c, runners(), {"PULP_LOCAL_MAC_RUNNER_LABEL": "pulp-gate-fast"}, [])
        self.assertEqual(findings, [])

    def test_unset_control_uses_the_workflow_fallback(self):
        c = contract([])
        c.routing_controls = {
            "PULP_LOCAL_MAC_RUNNER_LABEL":
                gate.RoutingControl(expect="pulp-gate-fast",
                                    unset_fallback="pulp-gate-fast")
        }
        self.assertEqual(gate.check(c, runners(), {}, []), [])


class TestExplicitLaneValues(unittest.TestCase):
    def test_unset_off_switch_fails_even_with_a_workflow_fallback(self):
        guarded = lane(
            variable="PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON",
            expect="local-only",
            unset_fallback=["macos-15"],
            require_explicit_value=True,
        )
        findings = gate.check(contract([guarded]), runners(), {}, [])
        self.assertEqual(kinds(findings), ["unset"])

    def test_json_quoted_off_switch_is_rejected(self):
        guarded = lane(
            variable="PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON",
            expect="local-only",
            unset_fallback=["macos-15"],
            require_explicit_value=True,
        )
        findings = gate.check(
            contract([guarded]),
            runners(),
            {"PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON": '"local-only"'},
            [],
        )
        self.assertEqual(kinds(findings), ["sentinel-encoding"])

    def test_reenabled_selector_is_still_checked_for_black_holes(self):
        guarded = lane(
            variable="PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON",
            expect="local-only",
            unset_fallback=["macos-15"],
            require_explicit_value=True,
        )
        typo = ["self-hosted", "macOS", "ARM64", "pulp-gate-fasr"]
        findings = gate.check(
            contract([guarded]),
            runners(),
            {"PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON": json.dumps(typo)},
            [],
        )
        self.assertEqual(kinds(findings, gate.ERROR), ["black-hole", "drift"])


def kinds(findings, level=None):
    return sorted(f.kind for f in findings
                  if level is None or f.level == level)


# ── Label matching — GitHub's rule is ALL labels, not any ───────────────


class TestLabelMatching(unittest.TestCase):
    def test_runner_must_carry_every_requested_label(self):
        # pulp-preamble-m5 carries `pulp-preamble` but NOT `pulp-build`, so it
        # cannot serve the build lane even though labels overlap.
        matches = gate.matching_runners(STUDIO_LANE, runners())
        self.assertEqual(
            sorted(r.name for r in matches),
            ["pulp-studio-01", "pulp-studio-02", "pulp-studio-03"])

    def test_extra_runner_labels_do_not_block_a_match(self):
        # Studios carry pulp-preamble on top of the requested set; a superset
        # runner still satisfies a subset request.
        self.assertTrue(gate.matching_runners(
            ["self-hosted", "pulp-build"], runners()))

    def test_one_unowned_label_makes_the_whole_set_unsatisfiable(self):
        self.assertEqual(gate.matching_runners(VM_LANE, runners()), [])


# ── The live bug: a lane routed at a label nothing carries ──────────────


class TestBlackHole(unittest.TestCase):
    def test_persistent_lane_with_no_matching_runner_is_an_error(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=VM_LANE)])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["black-hole"])

    def test_reconciled_lane_passes(self):
        # The green half: same check, same fleet, lane pointed at a label the
        # Studios actually carry.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=STUDIO_LANE)])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(STUDIO_LANE)}, [])
        self.assertEqual(f, [])

    def test_advisory_lane_black_hole_warns_but_does_not_fail(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=VM_LANE,
                           severity="advisory")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)}, [])
        self.assertEqual(kinds(f, gate.WARN), ["black-hole"])
        self.assertEqual(kinds(f, gate.ERROR), [])


# ── Three states: online, offline, ephemeral-idle ───────────────────────


class TestRunnerStates(unittest.TestCase):
    def test_offline_only_runner_is_degraded_not_a_black_hole(self):
        # An asleep host is a different failure from a label nobody owns:
        # the runner exists and will serve again when it wakes.
        asleep = [{"name": "pulp-m1", "status": "offline", "labels": STUDIO_LABELS}]
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=STUDIO_LANE)])
        f = gate.check(c, runners(asleep),
                       {"PULP_X_RUNS_ON_JSON": json.dumps(STUDIO_LANE)}, [])
        self.assertEqual(kinds(f, gate.WARN), ["degraded"])
        self.assertEqual(kinds(f, gate.ERROR), [])

    def test_ephemeral_lane_idle_with_service_evidence_is_ok(self):
        # Tart runners register JIT and vanish when idle, so an empty registry
        # proves nothing. Recent service proves the provisioner is alive.
        # This is the release lanes: no runner registered, yet not broken.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=VM_LANE,
                           provisioning="ephemeral")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)},
                       [set(VM_LANE)])
        self.assertEqual(kinds(f, gate.ERROR), [])
        self.assertEqual(kinds(f, gate.OK), ["ephemeral-idle"])

    def test_ephemeral_lane_without_service_evidence_is_a_black_hole(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=VM_LANE,
                           provisioning="ephemeral")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)},
                       [set(STUDIO_LANE)])
        self.assertEqual(kinds(f, gate.ERROR), ["black-hole"])

    def test_service_evidence_requires_the_exact_label_set(self):
        # A job served on a DIFFERENT lane whose labels happen to be a superset
        # says nothing about whether this lane has a provisioner.
        superset = set(VM_LANE) | {"extra-label"}
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=VM_LANE,
                           provisioning="ephemeral")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)},
                       [superset])
        self.assertEqual(kinds(f, gate.ERROR), ["black-hole"])


# ── Service evidence is scoped and lazy ─────────────────────────────────


class TestServiceEvidence(unittest.TestCase):
    def test_dispatch_only_fetch_filters_for_manual_runs(self):
        with mock.patch.object(gate, "_api", return_value={"workflow_runs": []}) as api:
            gate.fetch_served_label_sets(
                "owner/repo", 720, ["build.yml"], 20, manual_only=True)
        self.assertIn("event=workflow_dispatch", api.call_args.args[0][0])

    def test_automatic_fetch_does_not_filter_event(self):
        with mock.patch.object(gate, "_api", return_value={"workflow_runs": []}) as api:
            gate.fetch_served_label_sets("owner/repo", 720, ["build.yml"], 20)
        self.assertNotIn("event=", api.call_args.args[0][0])

    def test_evidence_is_not_fetched_when_a_live_runner_exists(self):
        # The scan costs API calls. A healthy fleet must not pay for them on
        # every sweep, so the provider is only consulted when a lane has no
        # live runner to point at.
        calls = []

        def provider(lane):
            calls.append(lane.variable)
            return []

        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=STUDIO_LANE,
                           provisioning="ephemeral")])
        gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(STUDIO_LANE)},
                   provider)
        self.assertEqual(calls, [])

    def test_evidence_is_fetched_only_for_the_lane_that_needs_it(self):
        calls = []

        def provider(lane):
            calls.append(lane.variable)
            return [set(VM_LANE)]

        c = contract([
            lane(variable="PULP_LIVE_RUNS_ON_JSON", expect=STUDIO_LANE,
                 provisioning="ephemeral"),
            lane(variable="PULP_IDLE_RUNS_ON_JSON", expect=VM_LANE,
                 provisioning="ephemeral"),
        ])
        gate.check(c, runners(), {
            "PULP_LIVE_RUNS_ON_JSON": json.dumps(STUDIO_LANE),
            "PULP_IDLE_RUNS_ON_JSON": json.dumps(VM_LANE),
        }, provider)
        self.assertEqual(calls, ["PULP_IDLE_RUNS_ON_JSON"])

    def test_persistent_lane_never_consults_service_history(self):
        # A persistent lane is adjudicated on the registry: if the label is
        # unowned it is a black hole regardless of what once ran.
        def provider(lane):
            raise AssertionError("persistent lanes must not fetch evidence")

        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=VM_LANE,
                           provisioning="persistent")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)},
                       provider)
        self.assertEqual(kinds(f, gate.ERROR), ["black-hole"])


class TestConsumingWorkflows(unittest.TestCase):
    def test_finds_the_workflow_that_references_the_variable(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            (td / "release-cli.yml").write_text(
                "runs-on: ${{ fromJSON(vars.PULP_RELEASE_MACOS_RUNS_ON_JSON) }}")
            (td / "unrelated.yml").write_text("runs-on: ubuntu-latest")
            self.assertEqual(
                gate.find_consuming_workflows(
                    "PULP_RELEASE_MACOS_RUNS_ON_JSON", td),
                ["release-cli.yml"])

    def test_a_variable_no_workflow_consumes_has_no_consumers(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(
                gate.find_consuming_workflows("PULP_UNUSED_RUNS_ON_JSON", Path(td)),
                [])

    def test_missing_workflow_dir_is_not_a_crash(self):
        self.assertEqual(
            gate.find_consuming_workflows("PULP_X", Path("/nonexistent/xyz")), [])

    def test_real_release_lane_resolves_to_a_real_workflow(self):
        # Guards the derivation against a rename: if the release lane stops
        # resolving to a consuming workflow, its evidence scan silently returns
        # nothing and the lane gets condemned as a black hole.
        wf = HERE.parent.parent / ".github" / "workflows"
        self.assertTrue(
            gate.find_consuming_workflows("PULP_RELEASE_MACOS_RUNS_ON_JSON", wf))


# ── Drift: the variable must match the reviewed contract ────────────────


class TestDrift(unittest.TestCase):
    def test_variable_edited_away_from_contract_is_drift(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=STUDIO_LANE)])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)}, [])
        self.assertIn("drift", kinds(f, gate.ERROR))

    def test_drift_is_adjudicated_against_the_live_value(self):
        # Contract says a good lane, someone edited the var to a dead label.
        # Both must surface: the drift AND the black hole the drift created --
        # reporting only the drift would understate a live outage.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=STUDIO_LANE)])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": json.dumps(VM_LANE)}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["black-hole", "drift"])

    def test_malformed_json_is_caught_before_dispatch(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": "[not json"}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["malformed"])

    def test_a_declared_sentinel_is_off_not_malformed(self):
        # A sentinel is a bare word by design, so it cannot parse as JSON. The
        # live `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON=local-only` — the
        # documented off switch — was therefore reported as an ERROR claiming
        # `fromJSON()` would fail at dispatch. A standing error for an intended
        # state is worse than no check: it teaches readers to skim the report,
        # which is where the real drift hides.
        c = contract([
            lane(variable="PULP_X_RUNS_ON_JSON", expect="local-only")
        ])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": "local-only"}, [])
        self.assertEqual(kinds(f, gate.ERROR), [])

    def test_sentinel_must_match_the_lane_contract(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": "local-only"}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["drift"])

    def test_an_undeclared_bare_word_is_still_malformed(self):
        # The escape must be the contract's declared vocabulary, not "any
        # unparseable string" — otherwise a typo'd label set becomes silent.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON")], sentinels=())
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": "local-only"}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["malformed"])


# ── GitHub-hosted lanes are allowlisted, not guessed ────────────────────


class TestGithubHosted(unittest.TestCase):
    def test_known_hosted_image_passes(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect="macos-15",
                           provisioning="github-hosted", severity="advisory")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": '"macos-15"'}, [])
        self.assertEqual(f, [])

    def test_typo_in_a_hosted_image_is_not_waved_through(self):
        # The whole point of the allowlist: "macos-15x" has no self-hosted
        # label, so a heuristic would call it hosted and let it queue forever.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect="macos-15x",
                           provisioning="github-hosted", severity="advisory")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": '"macos-15x"'}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["hosted-unknown"])

    def test_hosted_lane_is_not_measured_against_self_hosted_runners(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect="macos-15",
                           provisioning="github-hosted", severity="advisory")])
        f = gate.check(c, runners([]), {"PULP_X_RUNS_ON_JSON": '"macos-15"'}, [])
        self.assertEqual(f, [])

    def test_single_element_hosted_array_is_legal(self):
        # `runs-on: [macos-15]` is as valid as `runs-on: macos-15`. Treating
        # only the scalar form as hosted would fail a working lane.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=["macos-15"],
                           provisioning="github-hosted", severity="required")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": '["macos-15"]'}, [])
        self.assertEqual(f, [])

    def test_typo_inside_a_hosted_array_is_caught(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect=["macos-15x"],
                           provisioning="github-hosted", severity="required")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": '["macos-15x"]'}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["hosted-unknown"])


# ── Unset is not automatically broken ───────────────────────────────────


class TestUnsetFallback(unittest.TestCase):
    def test_unset_lane_with_a_workflow_fallback_routes_to_the_fallback(self):
        # GitHub treats unset and empty identically, so a workflow's
        # `${{ vars.X || '["macos-15"]' }}` still routes. Unset is a working
        # state, not an outage.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON",
                           unset_fallback=["macos-15"],
                           provisioning="github-hosted")])
        self.assertEqual(gate.check(c, runners(), {}, []), [])

    def test_a_fallback_pointing_at_a_dead_label_is_still_a_black_hole(self):
        # The fallback is what actually runs jobs when the variable is unset,
        # so it gets adjudicated like any other target.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON",
                           unset_fallback=VM_LANE)])
        f = gate.check(c, runners(), {}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["black-hole"])

    def test_unset_with_no_fallback_is_an_error(self):
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON")])
        f = gate.check(c, runners(), {}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["unset"])

    def test_off_switch_sentinel_is_not_a_routing_target(self):
        # `local-only` disables overflow; it is not a label to match runners on.
        c = contract([lane(variable="PULP_X_RUNS_ON_JSON", expect="local-only")])
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": "local-only"}, [])
        self.assertEqual(f, [])


# ── Guards around the contract itself ───────────────────────────────────


class TestContractGuards(unittest.TestCase):
    def test_undeclared_routing_variable_is_an_error(self):
        c = contract([])
        f = gate.check(c, runners(), {"PULP_NEW_RUNS_ON_JSON": '["self-hosted"]'}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["undeclared"])

    def test_non_routing_variables_are_ignored(self):
        c = contract([])
        f = gate.check(c, runners(), {"PULP_LOCAL_MAC_OVERFLOW_THRESHOLD": "3"}, [])
        self.assertEqual(f, [])

    def test_paid_overflow_variable_must_stay_unset(self):
        c = contract([], unset=["PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON"])
        f = gate.check(c, runners(),
                       {"PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON": '"ns-macos"'}, [])
        self.assertEqual(kinds(f, gate.ERROR), ["must-unset"])

    def test_unset_paid_overflow_variable_is_silent(self):
        c = contract([], unset=["PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON"])
        self.assertEqual(gate.check(c, runners(), {}, []), [])


# ── The shipped contract must itself be valid ───────────────────────────


class TestShippedContract(unittest.TestCase):
    def setUp(self):
        self.c = gate.load_contract(HERE / "runner_topology.json")

    def test_shipped_contract_parses(self):
        self.assertTrue(self.c.lanes)

    def test_every_lane_declares_a_known_provisioning_kind(self):
        for ln in self.c.lanes:
            self.assertIn(ln.provisioning,
                          {"persistent", "ephemeral", "github-hosted"},
                          f"{ln.variable} has an unknown provisioning kind")

    def test_every_lane_declares_a_known_severity(self):
        for ln in self.c.lanes:
            self.assertIn(ln.severity, {"required", "advisory"},
                          f"{ln.variable} has an unknown severity")

    def test_dispatch_only_lane_without_recent_service_is_unverified(self):
        lane = gate.Lane(
            variable="TEST_DISPATCH_ONLY",
            purpose="manual test lane",
            expect=["self-hosted", "Linux", "X64", "manual"],
            provisioning="ephemeral",
            severity="advisory",
            dispatch_only=True,
        )
        findings = gate.check(
            contract([lane]),
            [],
            {"TEST_DISPATCH_ONLY": json.dumps(lane.expect)},
            [],
        )
        self.assertEqual([f.kind for f in findings], ["dispatch-only-unverified"])
        self.assertEqual([f.level for f in findings], [gate.WARN])

    def test_dispatch_only_lane_accepts_matching_manual_service(self):
        lane = gate.Lane(
            variable="TEST_DISPATCH_ONLY",
            purpose="manual test lane",
            expect=["self-hosted", "Linux", "X64", "manual"],
            provisioning="ephemeral",
            severity="advisory",
            dispatch_only=True,
        )
        findings = gate.check(
            contract([lane]),
            [],
            {"TEST_DISPATCH_ONLY": json.dumps(lane.expect)},
            [set(lane.expect)],
        )
        self.assertEqual([f.kind for f in findings], ["dispatch-only-idle"])
        self.assertEqual([f.level for f in findings], [gate.OK])

    def test_self_hosted_lanes_are_not_declared_github_hosted(self):
        for ln in self.c.lanes:
            if ln.is_self_hosted:
                self.assertNotEqual(ln.provisioning, "github-hosted",
                                    f"{ln.variable} carries the self-hosted label")

    def test_lane_variables_are_unique(self):
        names = [ln.variable for ln in self.c.lanes]
        self.assertEqual(len(names), len(set(names)))

    def test_required_macos_gate_is_declared_required(self):
        # Regression guard: every merge depends on this lane resolving, so it
        # must never be quietly demoted to advisory.
        gate_lane = next(ln for ln in self.c.lanes
                         if ln.variable == "PULP_LOCAL_MACOS_RUNS_ON_JSON")
        self.assertEqual(gate_lane.severity, "required")
        # Ephemeral, not persistent. The gate runs on clean-per-job tartci VMs
        # precisely so it cannot inherit a warm build dir from another branch —
        # the reuse behind the 2026-06-07 SEGFAULT cluster, which `clean: false`
        # on self-hosted makes the default for a persistent runner. Routing this
        # lane back to the persistent Studios would reopen that class, so the
        # assertion is on the property that protects the gate, not on whatever
        # it happened to be provisioned by when this test was written.
        self.assertEqual(gate_lane.provisioning, "ephemeral")
        self.assertIn("pulp-build-vm", gate_lane.expect)
        self.assertIn("pulp-gate-fast", gate_lane.expect)
        self.assertNotIn("pulp-build-studio", gate_lane.expect)

    def test_namespace_paid_overflow_is_contracted_unset(self):
        self.assertIn("PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON",
                      self.c.must_remain_unset)


# ── Supervisors must register a label set something can select ──────────
#
# The lane checker above reconciles the ROUTING side: a repo variable pointed at
# labels nothing carries. This half covers the PROVISIONING side, which fails
# the same way and is even quieter. A supervisor that registers a label set no
# lane requests produces a runner that is online, idle, and unselectable — the
# operator reads "3 runners free" while jobs queue against a lane none of them
# can serve, and no runner log, job, or API response says otherwise.


def _supervisor_default_labels(script: Path) -> list[str]:
    """The label set the script registers when nothing overrides it."""
    m = re.search(r'^LABELS="\$\{PULP_RUNNER_LABELS:-([^}"]+)\}"',
                  script.read_text(), re.M)
    assert m, f"no LABELS default found in {script}"
    return [x.strip() for x in m.group(1).split(",") if x.strip()]


def _plist_invocation(template: Path) -> list[str]:
    """ProgramArguments from a launchd template (placeholders left intact)."""
    return plistlib.loads(template.read_bytes())["ProgramArguments"]


def _flag(argv: list[str], name: str) -> str | None:
    return argv[argv.index(name) + 1] if name in argv else None


SUPERVISORS = {
    "linux": (CI / "tart-runner-linux.sh",
              LAUNCHD / "pulp-tart-runner-linux.plist.template"),
    "windows": (CI / "qemu-runner-windows.sh",
                LAUNCHD / "pulp-qemu-runner-windows.plist.template"),
}

# Supervisors that are not launchd-managed Tart/QEMU on an Apple-Silicon Mac.
# These register their own label sets, so they need the same "can anything
# actually pick this runner?" guard the launchd pair gets above — just reached
# differently, since there is no plist to read the invocation out of.
OTHER_SUPERVISORS = {
    "proxmox-systemd": CI / "proxmox-ephemeral-runner-linux.sh",
    "native-intel-launchd": CI / "native-intel-runner.sh",
}


def _fixed_supervisor_labels(script: Path) -> list[str]:
    """The default label set a non-Tart supervisor registers."""
    text = script.read_text()
    m = re.search(r'^LABELS="([^"$]+)"', text, re.M)
    if m is None:
        m = re.search(r'^LABELS="\$\{[^:}]+:-([^}"]+)\}"', text, re.M)
    assert m, f"no LABELS assignment found in {script}"
    return [x.strip() for x in m.group(1).split(",") if x.strip()]


class TestNonLaunchdSupervisorsCanRouteTheirHosts(unittest.TestCase):
    """A host served by something other than Tart/QEMU still has to be routable.

    Exempting such a lane from the launchd supervisors' derive-path check (they
    register ARM64 because they run on Apple Silicon; this host is x86_64) would
    otherwise leave it with no guard at all — which is the failure mode being
    guarded against: a runner that registers labels no lane selects looks
    healthy, is never picked, and GitHub reports no error.
    """

    def setUp(self):
        self.lanes = labels_mod.load_lanes(HERE / "runner_topology.json")

    def _declared(self):
        return [(labels_mod.lane_supervisor(ln), ln) for ln in self.lanes
                if labels_mod.lane_supervisor(ln) != labels_mod.DEFAULT_SUPERVISOR]

    def test_every_named_supervisor_ships_a_provisioner(self):
        for name, lane in self._declared():
            with self.subTest(variable=lane["variable"], supervisor=name):
                self.assertIn(
                    name, OTHER_SUPERVISORS,
                    f"{lane['variable']} names supervisor {name!r}, which maps "
                    f"to no provisioning script in this repo")
                self.assertTrue(
                    OTHER_SUPERVISORS[name].is_file(),
                    f"{OTHER_SUPERVISORS[name]} is missing — the fleet would be "
                    f"provisioned by a script nobody reviews")

    def test_supervisor_registers_labels_its_lane_selects(self):
        # THE REGRESSION this mirrors: labels that drift from the lane. The
        # script is what actually runs `config.sh --labels`, so if its set stops
        # satisfying the lane, jobs queue against a runner that can never win.
        for name, lane in self._declared():
            script = OTHER_SUPERVISORS.get(name)
            if script is None or not script.is_file():
                continue  # reported by the test above
            with self.subTest(variable=lane["variable"], supervisor=name):
                labels = _fixed_supervisor_labels(script)
                self.assertTrue(
                    labels_mod.selecting_lanes(labels, self.lanes),
                    f"{script.name} registers {labels}, which no lane selects")
                self.assertEqual(
                    [x.casefold() for x in labels],
                    [x.casefold() for x in lane["expect"]],
                    f"{script.name} and {lane['variable']} disagree on labels")


class TestSupervisorLabelsAreSelectable(unittest.TestCase):
    """Drives the real shipped artifacts: the supervisor scripts, their launchd
    templates, and the routing contract. No network, no host dependency — the
    Shipyard tag probe is stubbed, because whether THIS machine happens to
    answer must not change the verdict."""

    def setUp(self):
        self.lanes = labels_mod.load_lanes(HERE / "runner_topology.json")

    def _platform_lanes(self, platform):
        return labels_mod.lanes_for(platform, self.lanes)

    def _tags(self, platform, supervisor=labels_mod.DEFAULT_SUPERVISOR):
        # Only the hosts THIS supervisor provisions. Both supervisors below are
        # launchd-managed and run on Apple Silicon, so they register ARM64
        # runners; a lane served by something else (the x86_64 Proxmox host) is
        # not theirs to route and asserting otherwise tests a claim nobody made.
        known = labels_mod.known_host_labels(
            self._platform_lanes(platform), supervisor)
        return sorted(h[len(labels_mod.HOST_PREFIX):] for h in known)

    def _resolve(self, platform, labels, tag, shipyard=None):
        lanes = self._platform_lanes(platform)
        resolved, note = labels_mod.resolve(
            platform, labels, tag, lanes, probe_shipyard=lambda: shipyard)
        return labels_mod.selecting_lanes(resolved, lanes), resolved, note

    def test_every_declared_host_tag_yields_a_selectable_runner(self):
        # The derive path: naming the machine is enough to make the shipped
        # default routable, on every machine the contract declares.
        for platform, (script, _) in SUPERVISORS.items():
            defaults = _supervisor_default_labels(script)
            for tag in self._tags(platform):
                with self.subTest(platform=platform, tag=tag):
                    sel, resolved, note = self._resolve(platform, defaults, tag)
                    self.assertTrue(sel, f"{resolved} selectable by nothing ({note})")

    def test_launchd_invocation_is_selectable_on_every_declared_host(self):
        # THE REGRESSION. The launchd template is what actually runs on the
        # fleet, so a template that hardcodes a host-label-less `--labels` (or
        # forgets to declare the host at all) is the live bug regardless of what
        # the script's own default says.
        for platform, (script, template) in SUPERVISORS.items():
            argv = _plist_invocation(template)
            labels = _flag(argv, "--labels")
            labels = ([x.strip() for x in labels.split(",") if x.strip()]
                      if labels else _supervisor_default_labels(script))
            declared = _flag(argv, "--host-tag")
            self.assertIsNotNone(
                declared,
                f"{template.name} never declares --host-tag, so the runner it "
                f"registers carries no pulp-host-* label and no lane can pick it")
            for tag in self._tags(platform):
                # The template ships a placeholder the operator substitutes;
                # every declared machine must be a working substitution.
                tag = tag if declared.startswith("$") else declared
                with self.subTest(platform=platform, tag=tag):
                    sel, resolved, note = self._resolve(platform, labels, tag)
                    self.assertTrue(sel, f"{resolved} selectable by nothing ({note})")

    def test_undetermined_host_is_refused_rather_than_registered(self):
        # Fail closed. A runner that looks healthy and can never be picked is
        # worse than one that refused to start and said why.
        for platform, (script, _) in SUPERVISORS.items():
            with self.subTest(platform=platform):
                sel, _, _ = self._resolve(
                    platform, _supervisor_default_labels(script), None)
                self.assertFalse(sel)

    def test_shipyard_tag_is_used_only_on_an_exact_lane_match(self):
        # `shipyard runner tag` answers `studio` on the Mac Studio while the
        # routing label is `pulp-host-macstudio`. Two vocabularies that agree on
        # m1/m5 and disagree here, so an exact match is the only safe rule: a
        # studio -> macstudio mapping is nowhere stated in this repo.
        defaults = _supervisor_default_labels(SUPERVISORS["linux"][0])
        sel, _, note = self._resolve("linux", defaults, None, shipyard="studio")
        self.assertFalse(sel)
        self.assertIn("pulp-host-studio", note)

        sel, resolved, _ = self._resolve("linux", defaults, None, shipyard="m5")
        self.assertTrue(sel)
        self.assertIn("pulp-host-m5", resolved)

    def test_an_undeclared_host_tag_is_rejected(self):
        defaults = _supervisor_default_labels(SUPERVISORS["linux"][0])
        sel, resolved, note = self._resolve("linux", defaults, "nosuchbox")
        self.assertFalse(sel)
        self.assertNotIn("pulp-host-nosuchbox", resolved)
        self.assertIn("no lane", note)

    def test_an_explicit_complete_label_set_needs_no_tag(self):
        # The escape hatch: --labels carrying its own host label is selectable
        # without any derivation, so ad-hoc routing still works.
        explicit = ["self-hosted", "Linux", "ARM64", "pulp-build-linux",
                    "pulp-host-m5"]
        sel, _, _ = self._resolve("linux", explicit, None)
        self.assertTrue(sel)


class TestSupervisorsRefuseToRegister(unittest.TestCase):
    """The scripts must ACT on an unresolvable label set, not just be able to
    compute one. Drives the real bash with stub `tart`/`gh`/`qemu` on PATH and
    asserts it dies before minting a JIT config — registration is the
    irreversible step, since a JIT runner appears in the fleet the moment it is
    minted and an operator then reads it as capacity."""

    @classmethod
    def setUpClass(cls):
        if os.name != "posix":
            raise unittest.SkipTest("supervisors are POSIX shell only")
        cls.tmp = tempfile.mkdtemp(prefix="runner-labels-")
        cls.stub_dir = Path(cls.tmp) / "bin"
        cls.stub_dir.mkdir()
        cls.minted = Path(cls.tmp) / "minted"
        for tool in ("tart", "qemu-system-aarch64"):
            (cls.stub_dir / tool).write_text("#!/bin/sh\nexit 0\n")
            (cls.stub_dir / tool).chmod(0o755)
        # A `gh` that records being called: reaching it at all means the
        # supervisor got as far as minting, which is the failure this asserts.
        gh = cls.stub_dir / "gh"
        gh.write_text(f'#!/bin/sh\ntouch "{cls.minted}"\nexit 1\n')
        gh.chmod(0o755)

    def _run(self, script, *args):
        self.minted.unlink(missing_ok=True)
        env = {
            **os.environ,
            "PATH": f"{self.stub_dir}:/usr/bin:/bin",
            "TART_HOME": self.tmp,          # satisfy the store precondition
            "PULP_RUNNER_HOST_TAG": "",     # no ambient declaration
        }
        return subprocess.run(["bash", str(script), "--once", *args],
                              capture_output=True, text=True, env=env, timeout=120)

    def test_the_bare_default_stops_before_minting(self):
        # THE REGRESSION, end to end: launched with nothing but its own
        # defaults — no flag, no env, no Shipyard on PATH — the supervisor must
        # refuse rather than mint a runner every Linux/Windows lane will ignore.
        # This is exactly how the LaunchAgent ran while 3 idle Linux runners sat
        # unselectable and 8 jobs queued.
        for platform, (script, _) in SUPERVISORS.items():
            with self.subTest(platform=platform):
                r = self._run(script)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("refusing to register", r.stderr)
                self.assertFalse(self.minted.exists(),
                                 "minted a JIT config for a runner nothing can select")

    def test_an_undeclared_host_tag_stops_before_minting(self):
        for platform, (script, _) in SUPERVISORS.items():
            with self.subTest(platform=platform):
                r = self._run(script, "--host-tag", "nosuchbox")
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("refusing to register", r.stderr)
                self.assertFalse(self.minted.exists(),
                                 "minted a JIT config for a runner nothing can select")

    def test_a_declared_host_tag_gets_past_label_resolution(self):
        # The other half of the control: same stubs, valid tag — the label step
        # must NOT be what stops it, or the refusal above would prove nothing.
        for platform, (script, _) in SUPERVISORS.items():
            with self.subTest(platform=platform):
                r = self._run(script, "--host-tag", "m5")
                self.assertNotIn("refusing to register", r.stderr)


class TestSelectionRule(unittest.TestCase):
    """Selection uses GitHub's rule, not a looser one."""

    def test_every_label_must_match_not_merely_some(self):
        lanes = [{"variable": "V", "expect": ["self-hosted", "Linux", "a", "b"]}]
        self.assertFalse(labels_mod.selecting_lanes(
            ["self-hosted", "Linux", "a"], lanes))
        self.assertTrue(labels_mod.selecting_lanes(
            ["self-hosted", "Linux", "a", "b"], lanes))

    def test_a_superset_still_selects(self):
        lanes = [{"variable": "V", "expect": ["self-hosted", "Linux", "a"]}]
        self.assertTrue(labels_mod.selecting_lanes(
            ["self-hosted", "Linux", "a", "extra"], lanes))

    def test_matching_is_case_insensitive_like_github(self):
        # The repo spells the OS label `macos` in a supervisor default and
        # `macOS` in a lane; GitHub does not care and neither may this.
        lanes = [{"variable": "V", "expect": ["self-hosted", "macOS", "ARM64"]}]
        self.assertTrue(labels_mod.selecting_lanes(
            ["self-hosted", "macos", "arm64"], lanes))


# ── CLI surface ─────────────────────────────────────────────────────────


class TestCli(unittest.TestCase):
    def _run(self, runners_spec, variables, jobs, mode):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            (td / "r.json").write_text(json.dumps(runners_spec))
            (td / "v.json").write_text(json.dumps(
                [{"name": k, "value": v} for k, v in variables.items()]))
            (td / "j.json").write_text(json.dumps(jobs))
            return gate.main([
                "--mode", mode,
                "--contract", str(HERE / "runner_topology.json"),
                "--runners-json", str(td / "r.json"),
                "--variables-json", str(td / "v.json"),
                "--jobs-json", str(td / "j.json"),
            ])

    def test_report_mode_fails_on_the_live_black_hole_state(self):
        # Exactly the state observed on the fleet: the overflow lane routed at
        # pulp-build-vm, which no runner carries and no job has been served on.
        variables = {
            "PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON": json.dumps(VM_LANE),
        }
        rc = self._run(LIVE_RUNNERS, variables, [], "report")
        self.assertEqual(rc, 1)

    def test_hint_mode_never_fails(self):
        variables = {
            "PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON": json.dumps(VM_LANE),
        }
        rc = self._run(LIVE_RUNNERS, variables, [], "hint")
        self.assertEqual(rc, 0)


class TestApiFailureHandling(unittest.TestCase):
    def test_api_failure_does_not_report_a_false_green(self):
        # A check that says OK because the API was unreachable is worse than
        # no check: exit 2 is distinct from both pass (0) and violation (1).
        with mock.patch.object(gate, "fetch_runners",
                               side_effect=subprocess.CalledProcessError(
                                   1, "ghapp", stderr="boom")):
            self.assertEqual(gate.main(["--mode", "report"]), 2)

    def test_missing_ghapp_is_advisory_in_hint_mode(self):
        with mock.patch.object(gate, "fetch_runners", side_effect=FileNotFoundError):
            self.assertEqual(gate.main(["--mode", "hint"]), 0)


if __name__ == "__main__":
    unittest.main()
