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
                severity="required", hosts=[], unset_fallback=None)
    base.update(kw)
    return gate.Lane(**base)


def contract(lanes, unset=(), hosted=("macos-15",), sentinels=("local-only",)):
    return gate.Contract(
        lanes=list(lanes),
        github_hosted_labels=set(hosted),
        sentinels=set(sentinels),
        must_remain_unset=list(unset),
        must_remain_unset_why="paid overflow, off for cost",
        lookback_hours=720,
        runs_per_workflow=20,
    )


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
        f = gate.check(c, runners(), {"PULP_X_RUNS_ON_JSON": '"local-only"'}, [])
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
        self.assertEqual(gate_lane.provisioning, "persistent")

    def test_namespace_paid_overflow_is_contracted_unset(self):
        self.assertIn("PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON",
                      self.c.must_remain_unset)


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
