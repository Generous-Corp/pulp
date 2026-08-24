#!/usr/bin/env python3
"""Fresh patch generation admits only exact module versions it can author."""

from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P  # noqa: E402


def inventory(pathset_version: str = "2.1.0") -> dict:
    return {
        "PathSet-Infinity": {
            "name": "Path Set Infinity",
            "version": pathset_version,
            "modules": {
                "WarpDrive": {
                    "name": "WarpDrive",
                    "description": "A self-contained stereo voice",
                    "tags": ["Oscillator"],
                    "outputs": ["Audio L", "Audio R"],
                    "roles_out": ["Audio", "Audio"],
                }
            },
        },
        "CVfunkModulations": {
            "name": "CV funk Modulations",
            "version": "2.4.2",
            "modules": {
                "Syzygy": {
                    "name": "Syzygy",
                    "description": "An oscillator that mixes pink and white noise",
                    "tags": ["Oscillator", "Noise"],
                    "outputs": ["Main L", "Main R"],
                    "roles_out": ["Audio", "Audio"],
                    "params": [
                        {"id": 0, "name": "Noise", "min": 0.0,
                         "max": 1.0, "default": 0.5}
                    ],
                }
            },
        },
        "Core": {
            "name": "Core",
            "version": "2.6.6",
            "modules": {
                "AudioInterface2": {
                    "name": "Audio 2",
                    "tags": ["External"],
                    "inputs": ["To device output 1", "To device output 2"],
                    "roles_in": ["Audio", "Audio"],
                    "outputs": ["From device input 1", "From device input 2"],
                    "roles_out": ["Audio", "Audio"],
                }
            },
        },
    }


class FreshGenerationEligibilityTest(unittest.TestCase):
    def test_exact_unsupported_version_is_hidden_from_generation(self) -> None:
        inv = inventory()
        rendered = P.render_inventory(inv)
        self.assertNotIn("PathSet-Infinity", rendered)
        self.assertNotIn("WarpDrive", rendered)
        self.assertIn("CVfunkModulations", rendered)
        self.assertIn("Syzygy", rendered)

    def test_version_scope_does_not_inherit(self) -> None:
        inv = inventory("2.1.1")
        self.assertIsNone(
            P.fresh_generation_refusal("PathSet-Infinity", "WarpDrive", inv))
        self.assertIn("WarpDrive", P.render_inventory(inv))

    def test_existing_patch_refinement_can_describe_unsupported_module(self) -> None:
        self.assertIn(
            "WarpDrive",
            P.render_inventory(inventory(), fresh_generation=False))

    def test_generic_drone_shortlist_keeps_audible_entropy_candidate(self) -> None:
        inv = inventory()
        selected: set[tuple[str, str]] = set()
        plan = P.intent_module_plan(
            "an ambient generative drone that never repeats", inv,
            selected=selected)
        self.assertNotIn("PathSet-Infinity/WarpDrive", plan)
        self.assertNotIn(("PathSet-Infinity", "WarpDrive"), selected)
        self.assertIn("CVfunkModulations/Syzygy", plan)
        self.assertIn(("CVfunkModulations", "Syzygy"), selected)

    def test_explicit_fresh_request_refuses_before_provider_resolution(self) -> None:
        inv = inventory()
        with mock.patch.object(
                P, "find_claude",
                side_effect=AssertionError("provider resolution must not run")), \
                mock.patch.object(
                    P, "catalog",
                    side_effect=AssertionError("catalogue lookup must not run")):
            with self.assertRaises(SystemExit) as raised:
                P._generate(
                    "build a drone with PathSet-Infinity/WarpDrive", inv, None)
        message = str(raised.exception)
        self.assertIn("cannot freshly generate PathSet-Infinity/WarpDrive", message)
        self.assertIn("opaque persisted coil, LFO, and envelope sequences", message)
        self.assertIn("Nothing was sent to the model", message)


if __name__ == "__main__":
    unittest.main()
