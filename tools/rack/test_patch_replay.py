#!/usr/bin/env python3
"""Zero-provider replay contract for generated Rack patches."""

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import patch  # noqa: E402


class PatchResponseReplay(unittest.TestCase):
    @staticmethod
    def _response(document):
        return ("```json patch\n" + json.dumps(document) + "\n```\n"
                "```json why\n{}\n```\n")

    def test_unusable_evidence_root_fails_before_provider_resolution(self):
        with tempfile.TemporaryDirectory() as root:
            blocked = pathlib.Path(root) / "not-a-directory"
            blocked.write_text("occupied")
            with mock.patch.dict(
                    os.environ, {"FORGE_ATTEMPT_DIR": str(blocked)}), \
                    mock.patch.object(
                        patch, "find_claude",
                        side_effect=AssertionError("provider resolution is forbidden")):
                with self.assertRaisesRegex(
                        RuntimeError, "cannot reserve a generation evidence"):
                    patch.generate("a simple utility patch", {}, None, retries=0)

    def test_saved_response_runs_normal_gates_without_provider_resolution(self):
        document = {"version": "2.6.6", "modules": [], "cables": []}
        response_text = (
            "```json patch\n" + json.dumps(document) + "\n```\n"
            "```json why\n{}\n```\n")
        with tempfile.TemporaryDirectory() as root:
            response = pathlib.Path(root) / "response.txt"
            response.write_text(response_text)
            with mock.patch.dict(os.environ, {"FORGE_ATTEMPT_DIR": root}), \
                    mock.patch.object(
                        patch, "find_claude",
                        side_effect=AssertionError("provider resolution is forbidden")), \
                    mock.patch.object(
                        patch, "prepare_and_lint",
                        side_effect=lambda value, _inventory, **_: (value, [])), \
                    mock.patch.object(patch, "configure_audio", return_value=None), \
                    mock.patch.object(
                        patch, "audibility", return_value=(patch.AUDIBLE, "audible")):
                got, why, shortfall = patch.generate(
                    "a simple utility patch", {}, None, retries=9,
                    response_file=str(response))

            self.assertEqual(document, got)
            self.assertEqual({}, why)
            self.assertIsNone(shortfall)
            retained = pathlib.Path(root) / "attempt01-model-response.txt"
            self.assertEqual(response_text, retained.read_text())

    def test_saved_response_refines_the_immutable_base_in_one_call(self):
        base = {"version": "2.6.6", "modules": [
            {"id": 1, "plugin": "Maker", "model": "Clock",
             "params": [{"id": 0, "value": 1.0}]},
            {"id": 2, "plugin": "Maker", "model": "Voice",
             "params": [{"id": 0, "value": 0.5}]},
            {"id": 3, "plugin": "Core", "model": "AudioInterface2"},
        ], "cables": []}
        candidate = json.loads(json.dumps(base))
        candidate["modules"][1]["params"][0]["value"] = 0.25
        inventory = {
            "Maker": {"name": "Maker", "modules": {
                "Clock": {"name": "Clock"}, "Voice": {"name": "Voice"}}},
            "Core": {"name": "Core", "modules": {
                "AudioInterface2": {"name": "Audio 2"}}},
        }
        with tempfile.TemporaryDirectory() as root:
            response = pathlib.Path(root) / "response.txt"
            response.write_text(self._response(candidate))
            with mock.patch.dict(os.environ, {"FORGE_ATTEMPT_DIR": root}), \
                    mock.patch.object(
                        patch, "find_claude",
                        side_effect=AssertionError("provider resolution is forbidden")), \
                    mock.patch.object(
                        patch, "prepare_and_lint",
                        side_effect=lambda value, _inventory, **_: (value, [])), \
                    mock.patch.object(patch, "configure_audio", return_value=None), \
                    mock.patch.object(
                        patch, "audibility",
                        return_value=(patch.AUDIBLE, "audible")):
                got, _, shortfall = patch.generate(
                    "make the voice darker", inventory, None, retries=0,
                    response_file=str(response), base_patch=base)
        self.assertEqual(candidate, got)
        self.assertEqual(0.5, base["modules"][1]["params"][0]["value"])
        self.assertIsNone(shortfall)

    def test_saved_response_cannot_fake_refinement_with_no_change(self):
        base = {"version": "2.6.6", "modules": [
            {"id": 1, "plugin": "Maker", "model": "Clock"},
            {"id": 2, "plugin": "Maker", "model": "Voice"},
            {"id": 3, "plugin": "Core", "model": "AudioInterface2"},
        ], "cables": []}
        inventory = {
            "Maker": {"name": "Maker", "modules": {
                "Clock": {"name": "Clock"}, "Voice": {"name": "Voice"}}},
            "Core": {"name": "Core", "modules": {
                "AudioInterface2": {"name": "Audio 2"}}},
        }
        with tempfile.TemporaryDirectory() as root:
            response = pathlib.Path(root) / "response.txt"
            response.write_text(self._response(base))
            with mock.patch.dict(os.environ, {"FORGE_ATTEMPT_DIR": root}), \
                    mock.patch.object(
                        patch, "prepare_and_lint",
                        side_effect=lambda value, _inventory, **_: (value, [])):
                with self.assertRaisesRegex(SystemExit, "gave up after 1 attempts"):
                    patch.generate(
                        "make the voice darker", inventory, None, retries=0,
                        response_file=str(response), base_patch=base)


if __name__ == "__main__":
    unittest.main()
