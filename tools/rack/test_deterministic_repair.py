#!/usr/bin/env python3
"""Dynamic deterministic repair contracts and refusal controls."""

from __future__ import annotations

import copy
import io
import json
import pathlib
import random
import unittest
from unittest import mock

import deterministic_repair as D
import patch as P


FIXTURE = pathlib.Path(__file__).parent / "test_fixtures/silent-live-output"


def silent_live_output_inventory() -> dict:
    return {
        "AudibleInstruments": {"modules": {"Tides2": {
            "outputs": ["Channel 1", "Channel 2", "Channel 3", "Channel 4"],
            "roles_out": ["Audio", "Audio", "Audio", "Audio"],
        }}},
        "Core": {"modules": {"AudioInterface2": {
            "inputs": ["Left", "Right"], "roles_in": ["Audio", "Audio"],
        }}},
    }


def inventory() -> dict:
    ports = lambda names: [{"index": i, "name": name}
                           for i, name in enumerate(names)]
    return {
        "CVfunk": {"version": "2.0.48", "modules": {
            "Aulos": {"inputs": [None] * 17, "outputs": ["Audio L", "Audio R"]},
            "Glass": {"inputs": ["V/Oct (polyphonic)", "Gate (polyphonic)"] +
                                  [None] * 15,
                      "outputs": ["Audio L", "Audio R"]}}},
        "Fundamental": {"version": "2.6.4", "modules": {
            "LFO": {"inputs": [], "outputs": ports(
                        ["Sine", "Triangle", "Sawtooth", "Square"]),
                    "params": [{"index": 2, "name": "Frequency"},
                               {"index": 0, "name": "Offset"}]},
            "VCA-1": {"inputs": ports(["CV", "Channel"]),
                      "outputs": ports(["Channel"]),
                      "params": [{"index": 0, "name": "Level"}]}}},
        "Core": {"version": "2.6.6", "modules": {
            "AudioInterface2": {"inputs": ["L", "R"], "outputs": []}}},
    }


def activation_fixture(layers: int = 6, seed: int = 7240) -> dict:
    rng = random.Random(seed)
    ids = rng.sample(range(100, 10000), layers * 2 + 3)
    source_a, source_b, audio = ids[-3:]
    modules, cables = [
        {"id": source_a, "plugin": "Fundamental", "model": "LFO",
         "pos": [0, 1], "params": []},
        {"id": source_b, "plugin": "Fundamental", "model": "LFO",
         "pos": [50, 1], "params": []},
        {"id": audio, "plugin": "Core", "model": "AudioInterface2",
         "pos": [layers * 40, 0], "params": []},
    ], []
    cable_id = 50000
    aulos, glass = [], []
    for index in range(layers):
        aid, gid = ids[index * 2:index * 2 + 2]
        aulos.append(aid)
        glass.append(gid)
        x = index * 40
        modules += [
            {"id": aid, "plugin": "CVfunk", "model": "Aulos",
             "pos": [x, 0], "params": [{"id": 22, "value": 1.0}]},
            {"id": gid, "plugin": "CVfunk", "model": "Glass",
             "pos": [x + 20, 0], "params": []},
        ]
        source = source_a if index < (layers + 1) // 2 else source_b
        cables.append({"id": cable_id, "outputModuleId": source,
                       "outputId": 3, "inputModuleId": gid, "inputId": 1})
        cable_id += 1
    for chain in (aulos, glass):
        for left, right in zip(chain, chain[1:]):
            cables.append({"id": cable_id, "outputModuleId": left,
                           "outputId": 0, "inputModuleId": right,
                           "inputId": 13 if chain is aulos else 12})
            cable_id += 1
    cables += [
        {"id": cable_id, "outputModuleId": aulos[-1], "outputId": 0,
         "inputModuleId": audio, "inputId": 0},
        {"id": cable_id + 1, "outputModuleId": glass[-1], "outputId": 1,
         "inputModuleId": audio, "inputId": 1},
    ]
    rng.shuffle(modules)
    rng.shuffle(cables)
    return {"modules": modules, "cables": cables}


class DeterministicRepairTests(unittest.TestCase):
    def test_retained_m5_patch_offers_only_live_exact_role_siblings(self) -> None:
        patch = json.loads((FIXTURE / "unfinished.vcv").read_text())
        report = (FIXTURE / "gate-report.txt").read_text()
        finding = P.silence_cause(
            report, patch, silent_live_output_inventory())
        self.assertEqual([6], finding["causal_cable_ids"])

        repairs, refusal = D.alternate_output_repairs(
            patch, silent_live_output_inventory(), finding)

        self.assertEqual([], refusal)
        self.assertEqual([1, 2, 3], [
            next(cable for cable in repair.patch["cables"]
                 if cable["id"] == 6)["outputId"]
            for repair in repairs])
        self.assertEqual(0, next(cable for cable in patch["cables"]
                                 if cable["id"] == 6)["outputId"])

    def test_alternate_output_does_not_rewrite_noncausal_fanout(self) -> None:
        patch = json.loads((FIXTURE / "unfinished.vcv").read_text())
        report = (FIXTURE / "gate-report.txt").read_text()
        finding = P.silence_cause(
            report, patch, silent_live_output_inventory())
        patch["cables"].append({
            "id": 7, "outputModuleId": 5, "outputId": 0,
            "inputModuleId": 3, "inputId": 1,
        })

        repairs, refusal = D.alternate_output_repairs(
            patch, silent_live_output_inventory(), finding)

        self.assertEqual([], refusal)
        self.assertEqual(1, next(cable for cable in repairs[0].patch["cables"]
                                 if cable["id"] == 6)["outputId"])
        self.assertEqual(0, next(cable for cable in repairs[0].patch["cables"]
                                 if cable["id"] == 7)["outputId"])

    def test_alternate_output_never_crosses_or_invents_a_role(self) -> None:
        patch = json.loads((FIXTURE / "unfinished.vcv").read_text())
        report = (FIXTURE / "gate-report.txt").read_text()
        inventory = silent_live_output_inventory()
        finding = P.silence_cause(report, patch, inventory)
        inventory["AudibleInstruments"]["modules"]["Tides2"]["roles_out"] = [
            "Audio", "Cv", "Gate", "Trigger"]

        repairs, refusal = D.alternate_output_repairs(
            patch, inventory, finding)

        self.assertEqual([], repairs)
        self.assertTrue(any("exact same semantic role" in item
                            for item in refusal))

    def test_alternate_output_refuses_duplicate_endpoint(self) -> None:
        patch = json.loads((FIXTURE / "unfinished.vcv").read_text())
        report = (FIXTURE / "gate-report.txt").read_text()
        inventory = silent_live_output_inventory()
        finding = P.silence_cause(report, patch, inventory)
        patch["cables"].extend({
            "id": 7 + output_id, "outputModuleId": 5,
            "outputId": output_id, "inputModuleId": 6, "inputId": 0,
        } for output_id in (1, 2, 3))

        repairs, refusal = D.alternate_output_repairs(
            patch, inventory, finding)

        self.assertEqual([], repairs)
        self.assertTrue(any("duplicate an existing cable" in item
                            for item in refusal))

    def test_alternate_output_requires_fresh_audibility_measurement(self) -> None:
        patch = json.loads((FIXTURE / "unfinished.vcv").read_text())
        report = (FIXTURE / "gate-report.txt").read_text()
        calls = []

        def gate(candidate, checkpoints=None):
            calls.append((candidate, checkpoints))
            return P.AUDIBLE, "patch gate passed"

        with mock.patch.object(P, "audibility", side_effect=gate), \
                mock.patch.object(P, "keep_deterministic_repair"):
            repaired, verdict, measured = P.audition_silent_output_repair(
                patch, silent_live_output_inventory(), P.SILENT, report,
                checkpoints=[0.0, 15.0, 30.0, 54.0])

        self.assertEqual(P.AUDIBLE, verdict)
        self.assertEqual("patch gate passed", measured)
        self.assertEqual(1, len(calls))
        self.assertEqual([0.0, 15.0, 30.0, 54.0], calls[0][1])
        self.assertEqual(1, next(cable for cable in repaired["cables"]
                                 if cable["id"] == 6)["outputId"])

    def test_alternate_output_moves_endpoint_keyed_rationale(self) -> None:
        patch = json.loads((FIXTURE / "unfinished.vcv").read_text())
        report = (FIXTURE / "gate-report.txt").read_text()
        why = {"5:0>6:0": "the measured voice reaches the listener"}

        with mock.patch.object(P, "audibility",
                               return_value=(P.AUDIBLE, "patch gate passed")), \
                mock.patch.object(P, "keep_deterministic_repair"):
            repaired, verdict, _ = P.audition_silent_output_repair(
                patch, silent_live_output_inventory(), P.SILENT, report,
                why=why)

        self.assertEqual(P.AUDIBLE, verdict)
        self.assertNotIn("5:0>6:0", why)
        self.assertEqual("the measured voice reaches the listener",
                         why["5:1>6:0"])
        self.assertEqual([], P.lint_why(
            repaired, silent_live_output_inventory(), why))

    def test_alternate_output_is_not_kept_when_every_audition_is_silent(self) -> None:
        patch = json.loads((FIXTURE / "unfinished.vcv").read_text())
        report = (FIXTURE / "gate-report.txt").read_text()
        calls = []

        def gate(candidate, checkpoints=None):
            calls.append(candidate)
            return P.SILENT, "still silent"

        with mock.patch.object(P, "audibility", side_effect=gate), \
                mock.patch.object(P, "keep_deterministic_repair"):
            retained, verdict, retained_report = \
                P.audition_silent_output_repair(
                    patch, silent_live_output_inventory(), P.SILENT, report)

        self.assertEqual(3, len(calls))
        self.assertEqual(patch, retained)
        self.assertEqual(P.SILENT, verdict)
        self.assertEqual(report, retained_report)

    def test_exact_drone_class_repairs_with_random_ids_and_order(self) -> None:
        patch = activation_fixture()
        findings = P.module_activation_contract_errors(patch, inventory())
        self.assertEqual(12, len(findings))
        repaired = D.repair_activation(patch, inventory(), findings)
        self.assertIsNotNone(repaired.patch)
        self.assertEqual([], repaired.refusal)
        self.assertEqual([], P.module_activation_contract_errors(
            repaired.patch, inventory()))
        self.assertEqual(12, len(repaired.actions))

    def test_distinct_small_topology_repairs_without_known_ids(self) -> None:
        patch = activation_fixture(layers=1, seed=88)
        findings = P.module_activation_contract_errors(patch, inventory())
        repaired = D.repair_activation(patch, inventory(), findings)
        self.assertIsNotNone(repaired.patch)
        self.assertEqual([], P.module_activation_contract_errors(
            repaired.patch, inventory()))

    def test_equidistant_gate_sources_refuse_instead_of_guessing(self) -> None:
        patch = activation_fixture(layers=2, seed=19)
        aulos = next(module for module in patch["modules"]
                     if module["model"] == "Aulos")
        glasses = [module for module in patch["modules"]
                   if module["model"] == "Glass"]
        aulos["pos"] = [50, 0]
        glasses[0]["pos"], glasses[1]["pos"] = [40, 0], [60, 0]
        findings = P.module_activation_contract_errors(patch, inventory())
        chosen = aulos["id"]
        findings = [finding for finding in findings
                    if not finding.startswith("CVfunk/Aulos module ") or
                    finding.startswith(f"CVfunk/Aulos module {chosen} ")]
        repaired = D.repair_activation(patch, inventory(), findings)
        self.assertIsNone(repaired.patch)
        self.assertTrue(any("no unique nearest" in item
                            for item in repaired.refusal))

    def test_missing_600_second_marker_remains_unmeasured(self) -> None:
        errors = P.long_horizon_evolution_errors("patch gate passed")
        self.assertTrue(errors)
        self.assertIn("UNMEASURED", errors[0])

    def test_repair_module_has_no_provider_or_prompt_dependency(self) -> None:
        source = pathlib.Path(D.__file__).read_text()
        self.assertNotIn("ask_model", source)
        self.assertNotIn("prompt", source)


def check_deterministic_repairs() -> tuple[int, int]:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        DeterministicRepairTests)
    result = unittest.TextTestRunner(stream=io.StringIO()).run(suite)
    bad = len(result.failures) + len(result.errors)
    print(f"  {'ok' if not bad else 'WRONG'}    deterministic repair suite: "
          f"{result.testsRun - bad}/{result.testsRun}")
    return bad, result.testsRun


if __name__ == "__main__":
    unittest.main()
