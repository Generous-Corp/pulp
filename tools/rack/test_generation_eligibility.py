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
import idiom_check as I  # noqa: E402


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


def source_policy_inventory() -> dict:
    """Two equally capable module families with unequal metadata richness."""
    def module(tags, inputs=(), outputs=(), roles_in=(), roles_out=(),
               exact_params=0):
        return {
            "tags": list(tags), "inputs": list(inputs),
            "outputs": list(outputs), "roles_in": list(roles_in),
            "roles_out": list(roles_out),
            "params": [
                {"name": f"P{index}", "min": 0.0, "max": 1.0,
                 "default": 0.5}
                for index in range(exact_params)
            ],
        }

    external = {
        "LFO": module(["Low-frequency oscillator"], outputs=["CV"],
                      roles_out=["Cv"]),
        "VCO": module(["Oscillator"], inputs=["FM"], outputs=["Audio"],
                      roles_in=["Cv"], roles_out=["Audio"]),
        "VCF": module(["Filter"], inputs=["Audio", "CV"], outputs=["Audio"],
                      roles_in=["Audio", "Cv"], roles_out=["Audio"]),
    }
    generated = {
        name: {**value, "params": [
            {"name": f"P{index}", "min": 0.0, "max": 1.0,
             "default": 0.5} for index in range(8)
        ]}
        for name, value in external.items()
    }
    return {
        "Library": {"version": "1.0", "modules": external},
        "ForgeModular": {"version": "2.0", "modules": generated},
        "Core": {"modules": {"AudioInterface2": {
            "tags": ["External"], "inputs": ["L", "R"],
            "roles_in": ["Audio", "Audio"]}}},
    }


def dub_inventory() -> dict:
    """Two mixers differ only in whether they can form a two-input sum."""
    def module(tags, inputs=(), outputs=(), roles_in=(), roles_out=()):
        return {"tags": list(tags), "inputs": list(inputs),
                "outputs": list(outputs), "roles_in": list(roles_in),
                "roles_out": list(roles_out)}

    return {"Library": {"version": "1.0", "modules": {
        "Source": module(["Oscillator"], outputs=["Audio"],
                         roles_out=["Audio"]),
        "Delay": module(["Delay"], inputs=["Audio"], outputs=["Audio"],
                        roles_in=["Audio"], roles_out=["Audio"]),
        "Filter": module(["Filter"], inputs=["Audio"], outputs=["Audio"],
                         roles_in=["Audio"], roles_out=["Audio"]),
        "OneInputMixer": module(["Mixer"], inputs=["Audio"],
                                outputs=["Audio"], roles_in=["Audio"],
                                roles_out=["Audio"]),
        "TwoInputMixer": module(["Mixer"], inputs=["Audio 1", "Audio 2"],
                                outputs=["Audio"],
                                roles_in=["Audio", "Audio"],
                                roles_out=["Audio"]),
    }}, "Core": {"modules": {"AudioInterface2": module(
        ["External"], inputs=["L", "R"], roles_in=["Audio", "Audio"])}}}


class FreshGenerationEligibilityTest(unittest.TestCase):
    def test_complete_authoring_surface_requires_known_ports_and_params(self) -> None:
        self.assertTrue(P.complete_authoring_surface({
            "inputs": ["Audio"], "outputs": ["Audio"], "params": []}))
        self.assertFalse(P.complete_authoring_surface({
            "inputs": None, "outputs": None, "params": None}))
        self.assertFalse(P.complete_authoring_surface({
            "inputs": [], "outputs": [], "params": []}))

    def test_required_tags_augment_only_an_existing_shortlist(self) -> None:
        required = {("Library", "Dynamics")}
        unrestricted: set[tuple[str, str]] = set()
        P.augment_required_tag_shortlist(unrestricted, required)
        self.assertEqual(set(), unrestricted)

        structural = {("Library", "Clock")}
        P.augment_required_tag_shortlist(structural, required)
        self.assertEqual({("Library", "Clock"),
                          ("Library", "Dynamics")}, structural)

    def test_metadata_completeness_is_strict_only_for_narrowed_tags(self) -> None:
        inv = {"Library": {"version": "1", "modules": {
            "Unknown": {"tags": ["Dynamics"]},
            "Known": {"tags": ["Dynamics"], "inputs": ["Audio"],
                      "outputs": ["Audio"], "params": []},
        }}}
        self.assertEqual(
            {("Library", "Unknown"), ("Library", "Known")},
            P.required_tag_candidate_allowlist(inv, narrowed=False))
        self.assertEqual(
            {("Library", "Known")},
            P.required_tag_candidate_allowlist(inv, narrowed=True))

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

    def test_library_first_is_enforced_before_the_model_call(self) -> None:
        selected: set[tuple[str, str]] = set()
        plan = P.intent_module_plan(
            "a slowly evolving ambient drone", source_policy_inventory(),
            selected=selected, module_source="prefer_existing")
        self.assertIn("Library/LFO", plan)
        self.assertIn("Library/VCO", plan)
        self.assertIn("Library/VCF", plan)
        self.assertNotIn("ForgeModular/", plan)
        self.assertFalse(any(plugin == "ForgeModular"
                             for plugin, _ in selected))

    def test_library_first_uses_generated_module_for_a_real_gap(self) -> None:
        inv = source_policy_inventory()
        inv["Library"]["modules"]["VCF"]["inputs"] = ["Audio"]
        inv["Library"]["modules"]["VCF"]["roles_in"] = ["Audio"]
        selected: set[tuple[str, str]] = set()
        plan = P.intent_module_plan(
            "a slowly evolving ambient drone", inv, selected=selected,
            module_source="prefer_existing")
        self.assertIn("ForgeModular/VCF", plan)
        self.assertIn(("ForgeModular", "VCF"), selected)

    def test_prefer_generated_prioritizes_the_generated_pack(self) -> None:
        plan = P.intent_module_plan(
            "a slowly evolving ambient drone", source_policy_inventory(),
            module_source="prefer_generated")
        role_lines = [line for line in plan.splitlines()
                      if line.startswith(("- audio_oscillator ", "- filter ",
                                          "- lfo "))]
        self.assertTrue(role_lines)
        self.assertTrue(all(line.index("ForgeModular/") < line.index("Library/")
                            for line in role_lines))

    def test_library_first_keeps_an_explicit_generated_request(self) -> None:
        plan = P.intent_module_plan(
            "a slowly evolving ambient drone using Forge Modular",
            source_policy_inventory(), module_source="prefer_existing")
        self.assertIn("ForgeModular/LFO", plan)
        self.assertIn("ForgeModular/VCO", plan)
        self.assertIn("ForgeModular/VCF", plan)

    def test_library_first_does_not_invert_a_negated_generated_request(self) -> None:
        plan = P.intent_module_plan(
            "a slowly evolving ambient drone but do not use Forge Modular",
            source_policy_inventory(), module_source="prefer_existing")
        self.assertNotIn("ForgeModular/", plan)
        self.assertIn("Library/LFO", plan)

    def test_unrelated_negation_does_not_hide_an_explicit_generated_request(self) -> None:
        for prompt in (
                "a slowly evolving ambient drone, no delay, use ForgeModular",
                "I don't want delay but use Forge Modular",
                "I don't want to use delay but use Forge Modular",
                "I do not want delay and prefer Forge Modular",
                "I do not want the Library and prefer Forge Modular",
                "do not prefer Library but choose Forge Modular",
                "do not use all modules but use Forge Modular",
                "I do not want all modules but use Forge Modular",
                "do not include all options but prefer Forge Modular",
                "do not use all modules but please use Forge Modular",
                "do not use all modules but definitely use Forge Modular",
                "do not use all modules but actually use Forge Modular",
                "do not use all modules but I prefer Forge Modular",
                "I do not want all modules but I want to use Forge Modular",
                "do not include all options but then choose Forge Modular",
                "do not use all modules but the patch should use Forge Modular",
                "do not use all modules but it should use Forge Modular",
                "do not include all options but the result must include Forge Modular",
                "do not use all modules but my patch should prefer Forge Modular",
                "do not use all modules but the generated patch should use Forge Modular",
                "do not use all modules but the updated result must include Forge Modular",
                "do not include all options but my preferred patch should choose Forge Modular",
                "do not use all modules but the existing patch can use Forge Modular"):
            with self.subTest(prompt=prompt):
                self.assertTrue(P._affirmatively_names_maker(
                    prompt, "ForgeModular"))
                plan = P.intent_module_plan(
                    prompt + " for a slowly evolving ambient drone",
                    source_policy_inventory(), module_source="prefer_existing")
                self.assertIn("ForgeModular/LFO", plan)
                self.assertIn("ForgeModular/VCO", plan)
                self.assertIn("ForgeModular/VCF", plan)

    def test_postpositive_exclusion_does_not_become_an_explicit_request(self) -> None:
        for prompt in ("Forge Modular is not allowed",
                       "Forge Modular should not be used",
                       "Forge Modular cannot be used",
                       "Forge Modular must be excluded",
                       "Forge Modular should be avoided",
                       "Forge Modular? No."):
            with self.subTest(prompt=prompt):
                self.assertFalse(P._affirmatively_names_maker(
                    prompt, "ForgeModular"))

    def test_contrastive_exclusion_does_not_become_an_explicit_request(self) -> None:
        for prompt in ("use Library, not Forge Modular",
                       "instead of Forge Modular",
                       "with no modules from Forge Modular",
                       "not from Forge Modular",
                       "I do not want Forge Modular",
                       "prefer Library over Forge Modular",
                       "Forge Modular-free",
                       "anything except Forge Modular",
                       "anything but Forge Modular",
                       "all but Forge Modular",
                       "use all available modules but Forge Modular",
                       "anything but a setup where you use Forge Modular",
                       "everything but modules that include Forge Modular",
                       "all modules but patches that include Forge Modular",
                       "all available modules but ones that use Forge Modular",
                       "anything but a setup allowing you to use Forge Modular",
                       "everything but modules intended to include Forge Modular",
                       "all modules but patches designed to use Forge Modular",
                       "anything but modules designed so they can use Forge Modular",
                       "all modules but patches built so they can include Forge Modular",
                       "everything but a setup intended so users could choose Forge Modular",
                       "anything other than Forge Modular",
                       "never use Forge Modular",
                       "I do not want to use Forge Modular",
                       "I would rather not use Forge Modular",
                       "but not Forge Modular"):
            with self.subTest(prompt=prompt):
                self.assertFalse(P._affirmatively_names_maker(
                    prompt, "ForgeModular"))
                plan = P.intent_module_plan(
                    f"a slowly evolving ambient drone {prompt}",
                    source_policy_inventory(), module_source="prefer_existing")
                self.assertNotIn("ForgeModular/", plan)
                self.assertIn("Library/LFO", plan)

    def test_postpositive_affirmation_remains_an_explicit_request(self) -> None:
        for prompt in ("Forge Modular should be used",
                       "Forge Modular is allowed",
                       "Forge Modular must be included"):
            with self.subTest(prompt=prompt):
                self.assertTrue(P._affirmatively_names_maker(
                    prompt, "ForgeModular"))

    def test_dub_sum_requires_two_physical_mixer_inputs(self) -> None:
        plan = P.intent_module_plan(
            "a dub-inspired feedback texture", dub_inventory(),
            module_source="prefer_existing")
        self.assertNotIn("Library/OneInputMixer", plan)
        self.assertIn("Library/TwoInputMixer", plan)

    def test_dub_contract_allows_an_unrelated_second_mixer(self) -> None:
        roles = I.load_roles()
        idiom = I.load_idioms()["dub-feedback-texture"]
        inv = I._fixture_inventory(roles)
        patch = I.synthesize(idiom, inv, roles)
        self.assertIsNotNone(patch)
        patch["modules"].append({"id": 999, "plugin": "Fixture",
                                 "model": "mixer"})
        self.assertEqual([], I.check(patch, inv, idiom, roles))

    def test_dub_identity_binding_preserves_transparent_source_routing(self) -> None:
        roles = I.load_roles()
        idiom = I.load_idioms()["dub-feedback-texture"]
        inv = I._fixture_inventory(roles)
        patch = I.synthesize(idiom, inv, roles)
        self.assertIsNotNone(patch)
        source = next(m for m in patch["modules"]
                      if m["model"] == "audio_oscillator")
        mixer = next(m for m in patch["modules"] if m["model"] == "mixer")
        direct = next(c for c in patch["cables"]
                      if c["outputModuleId"] == source["id"] and
                      c["inputModuleId"] == mixer["id"])
        patch["cables"].remove(direct)
        relay_id = 999
        patch["modules"].append({"id": relay_id, "plugin": "Fixture",
                                 "model": "multiple"})
        patch["cables"].extend((
            {"id": 998, "outputModuleId": source["id"], "outputId": 0,
             "inputModuleId": relay_id, "inputId": 0},
            {"id": 999, "outputModuleId": relay_id, "outputId": 0,
             "inputModuleId": mixer["id"], "inputId": direct["inputId"]},
        ))
        self.assertEqual([], I.check(patch, inv, idiom, roles))

    def test_dub_identity_binding_rejects_each_split_role_instance(self) -> None:
        roles = I.load_roles()
        idiom = I.load_idioms()["dub-feedback-texture"]
        inv = I._fixture_inventory(roles)
        patch = I.synthesize(idiom, inv, roles)
        self.assertIsNotNone(patch)
        groups = {group["role"]: group["describe"]
                  for group in idiom["same_role_instance_groups"]}
        mistakes = {mistake["module"]: mistake
                    for mistake in idiom["common_mistakes"]
                    if mistake.get("do") == "split_role_instance"}
        self.assertEqual(set(groups), set(mistakes))
        for role, description in groups.items():
            with self.subTest(role=role):
                mistake = dict(mistakes[role], _topology=idiom["topology"])
                broken = I.apply_mistake(patch, mistake, inv, roles)
                self.assertIsNotNone(broken)
                self.assertIn(description, I.check(broken, inv, idiom, roles))

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
