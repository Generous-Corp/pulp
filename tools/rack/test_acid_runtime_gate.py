#!/usr/bin/env python3
"""No-Rack orchestration tests for the acid runtime build gate."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import acid_runtime_gate as G                              # noqa: E402
import acid_taps as T                                      # noqa: E402
import idiom_check as I                                    # noqa: E402
import patch as P                                          # noqa: E402


def ready_plan() -> T.Plan:
    taps = [(index + 1, 0, name) for index, name in enumerate(T.ORDER)]
    return T.Plan(T.READY, taps, {}, [], [])


class Run:
    def __init__(self, *, why=""):
        self.engine = {"modules": [], "cables": []}
        self.signal = {"sampleRate": 48000, "frames": 4, "channels": 8}
        self.frames = [[float(index)] * 4 for index in range(8)]
        self.log = "Loading patch\n"
        self.why = why


class AcidRuntimeGateTests(unittest.TestCase):
    def test_cli_runs_real_entrypoint_contract_and_maps_pass_to_zero(self):
        with tempfile.TemporaryDirectory() as root:
            patch_path = os.path.join(root, "source.vcv")
            artifact_dir = os.path.join(root, "proof")
            with open(patch_path, "w") as out:
                json.dump({"modules": [], "cables": []}, out)
            with mock.patch.object(P, "inventory", return_value={}), \
                    mock.patch.object(G, "evaluate", return_value={
                        "verdict": T.PASS, "reasons": [],
                        "artifacts": {"verdict": "proof/acid-verdict.json"},
                    }) as evaluate:
                rc = G.main([patch_path, "--artifact-dir", artifact_dir,
                             "--seconds", "1.5"])
            self.assertEqual(0, rc)
            evaluate.assert_called_once_with(
                {"modules": [], "cables": []}, {}, artifact_dir, seconds=1.5)

    def test_pass_runs_one_synchronized_capture_and_retains_inputs(self):
        calls = []
        plan = ready_plan()
        with tempfile.TemporaryDirectory() as root:
            result = G.evaluate(
                {"modules": [], "cables": []}, {}, root,
                planner=lambda *_: plan,
                rack_finder=lambda: "/Rack",
                probe_builder=lambda stage: calls.append("probe") or "/probe",
                instrumenter=lambda patch, taps: (
                    calls.append(("instrument", taps)) or patch, list(taps)),
                runner=lambda *args: calls.append(("run", args[-1])) or Run(),
                structural_checker=lambda *_: [],
                behavior_evaluator=lambda got_plan, series: {
                    "verdict": T.PASS, "scope": "acid-behavior", "reasons": [],
                    "structural_failures": [], "measurement_failures": [],
                    "behavior_failures": [],
                    "observations": {"channels": sorted(series)},
                })
            self.assertEqual(T.PASS, result["verdict"])
            self.assertEqual(["probe", ("instrument", plan.taps), ("run", 8.0)],
                             calls)
            for name in ("acid-plan.json", "acid-source.vcv",
                         "acid-instrumented.vcv",
                         "acid-engine.json", "acid-signal.json",
                         "acid-capture.f32", "acid-capture.json",
                         "acid-rack.log", "acid-verdict.json"):
                self.assertTrue(os.path.isfile(os.path.join(root, name)), name)
            with open(os.path.join(root, "acid-verdict.json")) as source:
                retained = json.load(source)
            self.assertEqual(T.PASS, retained["verdict"])

    def test_missing_rack_is_unmeasured_and_never_runs(self):
        with tempfile.TemporaryDirectory() as root:
            result = G.evaluate(
                {}, {}, root, planner=lambda *_: ready_plan(),
                rack_finder=lambda: None,
                runner=lambda *_: self.fail("runner must not be called"))
            self.assertEqual(T.UNMEASURED, result["verdict"])
            self.assertIn("no Rack", " ".join(result["reasons"]))
            self.assertTrue(os.path.isfile(os.path.join(root,
                                                       "acid-verdict.json")))

    def test_planner_failure_is_retained_as_unmeasured(self):
        def broken(*_):
            raise ValueError("bad inventory")

        with tempfile.TemporaryDirectory() as root:
            result = G.evaluate({}, {}, root, planner=broken,
                                rack_finder=lambda: self.fail("must not run"))
            self.assertEqual(T.UNMEASURED, result["verdict"])
            self.assertIn("bad inventory", " ".join(result["reasons"]))
            self.assertTrue(os.path.isfile(os.path.join(root,
                                                       "acid-source.vcv")))

    def test_loaded_state_difference_is_a_failure_not_a_pass(self):
        with tempfile.TemporaryDirectory() as root:
            result = G.evaluate(
                {}, {}, root, planner=lambda *_: ready_plan(),
                rack_finder=lambda: "/Rack", probe_builder=lambda _: "/probe",
                instrumenter=lambda patch, taps: (patch, list(taps)),
                runner=lambda *_: Run(),
                structural_checker=lambda *_: ["module state changed"],
                behavior_evaluator=lambda *_: self.fail(
                    "behavior must not be judged from structurally different state"))
            self.assertEqual(T.FAIL, result["verdict"])
            self.assertEqual(["module state changed"],
                             result["structural_failures"])

    def test_capture_failure_is_unmeasured_not_retried_here(self):
        runs = []
        with tempfile.TemporaryDirectory() as root:
            result = G.evaluate(
                {}, {}, root, planner=lambda *_: ready_plan(),
                rack_finder=lambda: "/Rack", probe_builder=lambda _: "/probe",
                instrumenter=lambda patch, taps: (patch, list(taps)),
                runner=lambda *_: runs.append(1) or Run(why="probe timed out"))
            self.assertEqual(T.UNMEASURED, result["verdict"])
            self.assertEqual([1], runs)
            self.assertIn("probe timed out", " ".join(result["reasons"]))

    def _generate_with(self, runtime_result):
        idioms = I.load_idioms()
        roles = I.load_roles()
        inv = I._fixture_inventory(roles)
        built = I.synthesize(idioms["acid-voice"], inv, roles)
        prompt = "a melodic acid line with accent and slide from a single sequencer"
        calls = []

        def model(*_):
            calls.append(1)
            return (0, "```json patch\n" + json.dumps(built) +
                    "\n```\n```json why\n{}\n```", "")

        old_attempts = P._ATTEMPTS_DIR
        with tempfile.TemporaryDirectory() as root:
            P._ATTEMPTS_DIR = root
            try:
                with mock.patch.object(P, "find_claude", return_value="/model"), \
                        mock.patch.object(P, "ask_model", side_effect=model), \
                        mock.patch.object(P, "library_brief", return_value=""), \
                        mock.patch.object(P, "render_inventory", return_value=""), \
                        mock.patch.object(P, "catalog", return_value={}), \
                        mock.patch.object(P, "configure_audio", return_value=None), \
                        mock.patch.object(P, "audibility",
                                          return_value=(P.AUDIBLE, "audible")), \
                        mock.patch.object(P, "prepare_and_lint",
                                          side_effect=lambda patch, _, **__: (patch, [])), \
                        mock.patch.object(G, "evaluate",
                                          return_value=runtime_result):
                    answer = P.generate(prompt, inv, None, retries=4)
            finally:
                P._ATTEMPTS_DIR = old_attempts
        return answer, calls

    def test_runtime_pass_is_required_for_finished_acid(self):
        (patch, _why, shortfall), calls = self._generate_with({
            "verdict": T.PASS, "reasons": [], "artifacts": {}})
        self.assertIsInstance(patch, dict)
        self.assertIsNone(shortfall)
        self.assertEqual([1], calls)

    def test_runtime_unmeasured_is_unfinished_and_never_retries_model(self):
        (_patch, _why, shortfall), calls = self._generate_with({
            "verdict": T.UNMEASURED,
            "reasons": ["capture unavailable"], "artifacts": {}})
        self.assertIsNotNone(shortfall)
        self.assertEqual(1, shortfall.tried)
        self.assertIn("UNMEASURED", " ".join(shortfall.detail))
        self.assertEqual([1], calls)

    def test_runtime_fail_is_the_acid_build_failure(self):
        (_patch, _why, shortfall), calls = self._generate_with({
            "verdict": T.FAIL,
            "reasons": ["selected transition did not glide"], "artifacts": {}})
        self.assertIsNotNone(shortfall)
        self.assertIn("acid behavior FAIL", " ".join(shortfall.detail))
        self.assertEqual([1], calls)

    def test_saved_patch_response_runs_normal_runtime_gate_without_provider(self):
        idioms = I.load_idioms()
        roles = I.load_roles()
        inv = I._fixture_inventory(roles)
        built = I.synthesize(idioms["acid-voice"], inv, roles)
        prompt = "a melodic acid line with accent and slide from a single sequencer"
        response = "```json patch\n" + json.dumps(built) + \
            "\n```\n```json why\n{}\n```"
        old_attempts = P._ATTEMPTS_DIR
        with tempfile.TemporaryDirectory() as root:
            saved = os.path.join(root, "saved-response.txt")
            with open(saved, "w") as out:
                out.write(response)
            P._ATTEMPTS_DIR = root
            try:
                with mock.patch.object(
                        P, "find_claude",
                        side_effect=AssertionError("provider must not be resolved")), \
                        mock.patch.object(
                            P, "ask_model",
                            side_effect=AssertionError("provider must not run")), \
                        mock.patch.object(P, "library_brief", return_value=""), \
                        mock.patch.object(P, "render_inventory", return_value=""), \
                        mock.patch.object(P, "catalog", return_value={}), \
                        mock.patch.object(P, "configure_audio", return_value=None), \
                        mock.patch.object(P, "audibility",
                                          return_value=(P.AUDIBLE, "audible")), \
                        mock.patch.object(P, "prepare_and_lint",
                                          side_effect=lambda patch, _, **__: (patch, [])), \
                        mock.patch.object(G, "evaluate", return_value={
                            "verdict": T.PASS, "reasons": [], "artifacts": {}}):
                    patch, _why, shortfall = P.generate(
                        prompt, inv, None, retries=4, response_file=saved)
            finally:
                P._ATTEMPTS_DIR = old_attempts
        self.assertIsInstance(patch, dict)
        self.assertIsNone(shortfall)


if __name__ == "__main__":
    unittest.main()
