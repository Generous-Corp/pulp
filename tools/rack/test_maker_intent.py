#!/usr/bin/env python3
"""Adversarial controls for maker sourcing polarity and enforcement."""
from __future__ import annotations

from dataclasses import FrozenInstanceError
import io
import os
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import maker_intent as M  # noqa: E402
import patch as P  # noqa: E402
from test_generation_eligibility import source_policy_inventory  # noqa: E402


CATALOGUE = {
    "X": {"brand": "Xylophone Labs", "arches": ["mac-arm64"]},
    "X2": {"brand": "Xylophone Labs", "arches": ["mac-arm64"]},
    "Y": {"brand": "Yellow Tree", "arches": ["mac-arm64"]},
    "CVfunk": {"brand": "CV funk", "arches": ["mac-arm64"]},
    "Slash": {"brand": "Catro/Blanco", "arches": ["mac-arm64"]},
    "Ampersand": {"brand": "Jasmine & Olive Trees",
                   "arches": ["mac-arm64"]},
    "p.s.F-X": {"brand": "p.s.F/X", "arches": ["mac-arm64"]},
    "MathematicsAndMusicLab": {
        "brand": "Mathematics and Music Lab (MML)",
        "arches": ["mac-arm64"]},
    "alto777_LFSR": {"brand": "alto777", "arches": ["mac-arm64"]},
    "PathSetOmriCohen": {
        "brand": "Path Set x Omri Cohen", "arches": ["mac-arm64"]},
    "ForgeModular": {"brand": "Forge Modular", "arches": ["mac-arm64"]},
    "AS": {"brand": "AS", "arches": ["mac-arm64"]},
    "Bogaudio": {"brand": "Bogaudio", "arches": ["mac-arm64"]},
}


class MakerIntentTest(unittest.TestCase):
    def assert_decisions(self, prompt: str, expected: dict[str, M.Decision]) -> None:
        resolution = M.resolve(prompt, CATALOGUE)
        self.assertEqual(expected,
                         {maker.brand: maker.decision
                          for maker in resolution.makers}, prompt)

    def test_minimum_clause_and_phrase_matrix(self) -> None:
        A, X, U = M.Decision.AFFIRMED, M.Decision.EXCLUDED, M.Decision.UNDECIDED
        cases = (
            ("use Xylophone Labs", {"Xylophone Labs": A}),
            ("a drone from Xylophone Labs", {"Xylophone Labs": A}),
            ("prefer modules by Xylophone Labs", {"Xylophone Labs": A}),
            ("use Xylophone Labs and Yellow Tree",
             {"Xylophone Labs": A, "Yellow Tree": A}),
            ("avoid Xylophone Labs and Yellow Tree",
             {"Xylophone Labs": X, "Yellow Tree": X}),
            ("avoid Xylophone Labs and use Yellow Tree",
             {"Xylophone Labs": X, "Yellow Tree": A}),
            ("use Xylophone Labs but not Yellow Tree",
             {"Xylophone Labs": A, "Yellow Tree": X}),
            ("do not use Xylophone Labs", {"Xylophone Labs": X}),
            ("no Xylophone Labs", {"Xylophone Labs": X}),
            ("without Xylophone Labs", {"Xylophone Labs": X}),
            ("skip Xylophone Labs", {"Xylophone Labs": X}),
            ("omit Xylophone Labs", {"Xylophone Labs": X}),
            ("leave Xylophone Labs out", {"Xylophone Labs": X}),
            ("neither Xylophone Labs nor Yellow Tree",
             {"Xylophone Labs": X, "Yellow Tree": X}),
            ("anything but Xylophone Labs", {"Xylophone Labs": X}),
            ("all modules but Xylophone Labs", {"Xylophone Labs": X}),
            ("all modules but the patch should use Xylophone Labs",
             {"Xylophone Labs": A}),
            ("all modules but patches designed to use Xylophone Labs",
             {"Xylophone Labs": X}),
            ("anything but a setup allowing you to use Xylophone Labs",
             {"Xylophone Labs": X}),
            ("nothing except Xylophone Labs", {"Xylophone Labs": A}),
            ("use Xylophone Labs; avoid Xylophone Labs",
             {"Xylophone Labs": X}),
            ("avoid Xylophone Labs; use Yellow Tree",
             {"Xylophone Labs": X, "Yellow Tree": A}),
            ("don't avoid Xylophone Labs", {"Xylophone Labs": U}),
            ("not without Xylophone Labs", {"Xylophone Labs": U}),
            ("I don't dislike Xylophone Labs", {"Xylophone Labs": U}),
            ("Xylophone Labs is popular", {"Xylophone Labs": U}),
            ("should we use Xylophone Labs?", {"Xylophone Labs": U}),
            ("@Xylophone Labs", {"Xylophone Labs": A}),
            ("avoid @Xylophone Labs", {"Xylophone Labs": X}),
            ("use (Xylophone Labs and Yellow Tree)",
             {"Xylophone Labs": A, "Yellow Tree": A}),
            ("avoid (Xylophone Labs and Yellow Tree)",
             {"Xylophone Labs": X, "Yellow Tree": X}),
            ("Xylophone Labs and Yellow Tree are forbidden",
             {"Xylophone Labs": X, "Yellow Tree": X}),
        )
        for prompt, expected in cases:
            with self.subTest(prompt=prompt):
                self.assert_decisions(prompt, expected)

    def test_qualifiers_are_orthogonal_to_polarity(self) -> None:
        cases = (
            ("only Xylophone Labs", True, False),
            ("nothing except Xylophone Labs", True, False),
            ("all modules from Xylophone Labs", False, True),
            ("not only Xylophone Labs", False, False),
        )
        for prompt, exclusive, exhaustive in cases:
            with self.subTest(prompt=prompt):
                maker = M.resolve(prompt, CATALOGUE).for_slug("X")
                self.assertEqual(M.Decision.AFFIRMED, maker.decision)
                self.assertEqual(exclusive, maker.exclusive)
                self.assertEqual(exhaustive, maker.exhaustive)

        coordinated = M.resolve(
            "only Xylophone Labs and Yellow Tree", CATALOGUE)
        self.assertTrue(all(maker.exclusive for maker in coordinated.makers))
        except_group = M.resolve(
            "nothing except Xylophone Labs and Yellow Tree", CATALOGUE)
        self.assertTrue(all(maker.exclusive for maker in except_group.makers))
        exhaustive_group = M.resolve(
            "all modules from Xylophone Labs and Yellow Tree", CATALOGUE)
        self.assertTrue(all(maker.exhaustive for maker in exhaustive_group.makers))

        for prompt in (
                "connect all cables cleanly, then use Xylophone Labs",
                "connect all cables cleanly then use Xylophone Labs",
                "connect all cables using Xylophone Labs"):
            with self.subTest(prompt=prompt):
                maker = M.resolve(prompt, CATALOGUE).for_slug("X")
                self.assertFalse(maker.exhaustive)

        for prompt in (
                "add only one oscillator with Xylophone Labs",
                "only one LFO, use Xylophone Labs",
                "use Xylophone Labs with only one LFO",
                "use Xylophone Labs, then add only one LFO"):
            with self.subTest(prompt=prompt):
                maker = M.resolve(prompt, CATALOGUE).for_slug("X")
                self.assertEqual(M.Decision.AFFIRMED, maker.decision)
                self.assertFalse(maker.exclusive)

        for prompt in (
                "use only one maker: Xylophone Labs",
                "use only one maker, Xylophone Labs",
                "use only the maker Xylophone Labs",
                "use only the maker, Xylophone Labs"):
            with self.subTest(prompt=prompt):
                maker = M.resolve(prompt, CATALOGUE).for_slug("X")
                self.assertEqual(M.Decision.AFFIRMED, maker.decision)
                self.assertTrue(maker.exclusive)

        for prompt in (
                "use Xylophone Labs and Yellow Tree modules only",
                "Xylophone Labs and Yellow Tree should be used exclusively",
                "Xylophone Labs and Yellow Tree modules must be used exclusively"):
            with self.subTest(prompt=prompt):
                resolution = M.resolve(prompt, CATALOGUE)
                self.assertEqual(
                    {"Xylophone Labs": M.Decision.AFFIRMED,
                     "Yellow Tree": M.Decision.AFFIRMED},
                    {maker.brand: maker.decision for maker in resolution.makers})
                self.assertTrue(all(maker.exclusive
                                    for maker in resolution.makers))

        for prompt in ("Xylophone Labs should be used exclusively",
                       "Xylophone Labs modules should be used exclusively"):
            with self.subTest(prompt=prompt):
                predicate = M.resolve(prompt, CATALOGUE)
                self.assertEqual(M.Decision.AFFIRMED,
                                 predicate.for_slug("X").decision)
                self.assertTrue(predicate.for_slug("X").exclusive)

        real_short = M.resolve(
            "AS and Bogaudio should be used exclusively", CATALOGUE)
        self.assertEqual(
            {"AS": M.Decision.AFFIRMED,
             "Bogaudio": M.Decision.AFFIRMED},
            {maker.brand: maker.decision for maker in real_short.makers})
        self.assertTrue(all(maker.exclusive for maker in real_short.makers))

        short = M.resolve("only AS", CATALOGUE).for_slug("AS")
        self.assertEqual(M.Decision.AFFIRMED, short.decision)
        self.assertTrue(short.exclusive)
        ordinary = M.resolve("route this as a clean signal", CATALOGUE).for_slug("AS")
        self.assertEqual(M.Decision.UNDECIDED, ordinary.decision)

    def test_exact_identity_does_not_override_polarity_or_module_channel(self) -> None:
        excluded = M.resolve("avoid @Xylophone Labs", CATALOGUE).for_slug("X")
        self.assertEqual("exact", excluded.evidence[0].channel)
        self.assertEqual(M.Decision.EXCLUDED, excluded.decision)
        self.assertEqual((), M.resolve("@CVfunk/Dunes", CATALOGUE).makers)

    def test_qualified_module_channel_never_expands_its_maker(self) -> None:
        midx = {"X": {"Osc": {"name": "Osc", "tags": ["Oscillator"]}}}
        for prompt in ("use X/Osc", "use @X/Osc"):
            with self.subTest(prompt=prompt):
                resolution = M.resolve(prompt, CATALOGUE)
                self.assertEqual((), resolution.makers)
                mentions = P.brand_mentions(prompt, CATALOGUE, resolution)
                self.assertEqual({"X": "X/Osc"}, P.module_mentions(
                    prompt, CATALOGUE, midx, {}, mentions, resolution))
                plan = P.named_fetch_plan(
                    prompt, {}, CATALOGUE, midx, mentions,
                    {"auto_download": "entitled"}, set(), resolution)
                self.assertEqual(["X"],
                                 [item["plugin"] for item in plan["fetch"]])
                self.assertNotIn("X2",
                                 [item["plugin"] for item in plan["fetch"]])

        for prompt in ("use Catro/Blanco", "use @Catro/Blanco"):
            with self.subTest(prompt=prompt):
                maker = M.resolve(prompt, CATALOGUE).for_slug("Slash")
                self.assertEqual(M.Decision.AFFIRMED, maker.decision)
        self.assertEqual(
            (), M.resolve("use Catro/Blanco/Osc", CATALOGUE).makers)

    def test_catalogue_punctuation_and_long_names_survive_tokenization(self) -> None:
        for prompt, slug in (
                ("use Catro/Blanco", "Slash"),
                ("use Jasmine & Olive Trees", "Ampersand"),
                ("use p.s.F/X", "p.s.F-X"),
                ("use @p.s.F/X", "p.s.F-X"),
                ("use Mathematics and Music Lab (MML)",
                 "MathematicsAndMusicLab"),
                ("use @Mathematics and Music Lab (MML)",
                 "MathematicsAndMusicLab")):
            with self.subTest(prompt=prompt):
                self.assertEqual(M.Decision.AFFIRMED,
                                 M.resolve(prompt, CATALOGUE).for_slug(slug).decision)

    def test_catalogue_slugs_are_independent_identity_spellings(self) -> None:
        for slug in ("alto777_LFSR", "PathSetOmriCohen",
                     "MathematicsAndMusicLab"):
            with self.subTest(slug=slug):
                maker = M.resolve(f"use @{slug}", CATALOGUE).for_slug(slug)
                self.assertEqual(M.Decision.AFFIRMED, maker.decision)

    def test_identity_never_crosses_a_hard_clause_boundary(self) -> None:
        for prompt in (
                "avoid Yellow. Tree",
                "avoid Yellow; Tree",
                "avoid Yellow: Tree",
                "avoid Yellow, Tree",
                "avoid Yellow---Tree",
                "avoid Yellow (Tree modules are noisy)"):
            with self.subTest(prompt=prompt):
                self.assertEqual((), M.resolve(prompt, CATALOGUE).makers)

        maker = M.resolve("use yElLoW   tReE", CATALOGUE).for_slug("Y")
        self.assertEqual(M.Decision.AFFIRMED, maker.decision)
        for prompt, slug in (("use CV-funk", "CVfunk"),
                             ("use CV_funk", "CVfunk"),
                             ("use Forge-Modular", "ForgeModular")):
            with self.subTest(prompt=prompt):
                self.assertEqual(
                    M.Decision.AFFIRMED,
                    M.resolve(prompt, CATALOGUE).for_slug(slug).decision)

    def test_results_and_evidence_are_immutable(self) -> None:
        maker = M.resolve("use Xylophone Labs", CATALOGUE).for_slug("X")
        with self.assertRaises(FrozenInstanceError):
            maker.decision = M.Decision.EXCLUDED
        with self.assertRaises(FrozenInstanceError):
            maker.evidence[0].rule_id = "changed"

    def test_restrictive_ambiguity_is_a_pre_provider_error(self) -> None:
        resolution = M.resolve("only Xylophone Labs is popular", CATALOGUE)
        self.assertEqual(M.Decision.UNDECIDED,
                         resolution.for_slug("X").decision)
        self.assertTrue(resolution.restrictive_ambiguities)
        with mock.patch.object(P, "catalog", return_value=CATALOGUE), \
                mock.patch.object(P, "find_claude",
                                  side_effect=AssertionError("provider resolved")):
            with self.assertRaisesRegex(SystemExit, "restrictive maker request"):
                P._generate("only Xylophone Labs is popular", {}, None)
        unrelated = M.resolve(
            "Only add one LFO. Xylophone Labs is popular", CATALOGUE)
        self.assertFalse(unrelated.restrictive_ambiguities)

    def test_restrictive_not_without_is_undecided_and_stops_pre_provider(self) -> None:
        resolution = M.resolve("only not without Xylophone Labs", CATALOGUE)
        self.assertEqual(M.Decision.UNDECIDED,
                         resolution.for_slug("X").decision)
        self.assertTrue(resolution.restrictive_ambiguities)
        with mock.patch.object(P, "catalog", return_value=CATALOGUE), \
                mock.patch.object(P, "find_claude",
                                  side_effect=AssertionError("provider resolved")), \
                mock.patch.object(P, "install_module",
                                  side_effect=AssertionError("network/install")):
            with self.assertRaisesRegex(SystemExit, "restrictive maker request"):
                P._generate("only not without Xylophone Labs", {}, None,
                            maker_resolution=resolution)

    def test_affirmed_adapter_reuses_resolution_and_filters_exclusions(self) -> None:
        resolution = P.resolve_maker_intent(
            "avoid Xylophone Labs; use Yellow Tree", CATALOGUE)
        with mock.patch.object(P, "resolve_maker_intent",
                              side_effect=AssertionError("parsed twice")):
            mentions = P.brand_mentions("ignored", CATALOGUE, resolution)
        self.assertEqual(["Yellow Tree"], list(mentions))
        self.assertIs(resolution, mentions.resolution)

    def test_excluded_maker_is_not_fetched_and_is_rejected_if_returned(self) -> None:
        prompt = "avoid Xylophone Labs"
        resolution = P.resolve_maker_intent(prompt, CATALOGUE)
        mentions = P.brand_mentions(prompt, CATALOGUE, resolution)
        plan = P.named_fetch_plan(
            prompt, {}, CATALOGUE, {"X": {"Osc": {}}}, mentions,
            {"auto_download": "entitled"}, set(), resolution)
        self.assertEqual([], plan["fetch"])
        errors = P.maker_intent_errors(
            {"modules": [{"plugin": "X", "model": "Osc"}]}, resolution)
        self.assertRegex("\n".join(errors), "excluded maker constraint")

    def test_excluded_maker_vetoes_exact_module_fetch_and_install(self) -> None:
        prompt = "avoid Xylophone Labs; use @X/Osc"
        resolution = P.resolve_maker_intent(prompt, CATALOGUE)
        mentions = P.brand_mentions(prompt, CATALOGUE, resolution)
        midx = {"X": {"Osc": {"name": "Osc", "tags": ["Oscillator"]}}}
        plan = P.named_fetch_plan(
            prompt, {}, CATALOGUE, midx, mentions,
            {"auto_download": "entitled"}, set(), resolution)
        self.assertEqual([], plan["fetch"])
        with mock.patch.object(P, "settings",
                               return_value={"auto_download": "entitled"}), \
                mock.patch.object(P, "entitlements_cached", return_value=set()), \
                mock.patch.object(P, "install_module",
                                  side_effect=AssertionError("network/install")):
            inventory, fetched = P.ensure_named_installed(
                prompt, {}, CATALOGUE, midx, mentions, resolution)
        self.assertEqual({}, inventory)
        self.assertEqual([], fetched)

    def test_comparative_second_maker_is_excluded_and_never_fetched(self) -> None:
        for prompt in (
                "use Xylophone Labs rather than Yellow Tree",
                "prefer Xylophone Labs to Yellow Tree",
                "choose Xylophone Labs over Yellow Tree",
                "use Xylophone Labs instead of Yellow Tree"):
            with self.subTest(prompt=prompt):
                resolution = P.resolve_maker_intent(prompt, CATALOGUE)
                self.assertEqual(M.Decision.AFFIRMED,
                                 resolution.for_slug("X").decision)
                self.assertEqual(M.Decision.EXCLUDED,
                                 resolution.for_slug("Y").decision)
                mentions = P.brand_mentions(prompt, CATALOGUE, resolution)
                plan = P.named_fetch_plan(
                    prompt, {}, CATALOGUE,
                    {"X": {"Osc": {}}, "Y": {"Osc": {}}},
                    mentions, {"auto_download": "entitled"}, set(), resolution)
                self.assertNotIn("Y", [item["plugin"] for item in plan["fetch"]])

    def test_absent_exact_module_conflict_is_catalogue_decidable_pre_provider(self) -> None:
        prompt = "avoid Xylophone Labs; use @X/Osc"
        resolution = P.resolve_maker_intent(prompt, CATALOGUE)
        mentions = P.brand_mentions(prompt, CATALOGUE, resolution)
        midx = {
            "X": {"Osc": {"name": "Osc", "tags": ["Oscillator"]}},
            "Y": {"Osc": {"name": "Osc", "tags": ["Oscillator"]}},
        }
        conflicts = P.excluded_named_module_conflicts(
            prompt, {}, CATALOGUE, midx, mentions, resolution)
        self.assertEqual(
            ["named module request conflicts with an excluded maker: X/Osc"],
            conflicts)

        for allowed in ("use @Y/Osc",
                        "avoid Xylophone Labs; do not use @X/Osc"):
            with self.subTest(prompt=allowed):
                allowed_resolution = P.resolve_maker_intent(allowed, CATALOGUE)
                allowed_mentions = P.brand_mentions(
                    allowed, CATALOGUE, allowed_resolution)
                self.assertEqual([], P.excluded_named_module_conflicts(
                    allowed, {}, CATALOGUE, midx, allowed_mentions,
                    allowed_resolution))

        with mock.patch.object(P, "catalog", return_value=CATALOGUE), \
                mock.patch.object(P, "module_index", return_value=midx), \
                mock.patch.object(P, "find_claude",
                                  side_effect=AssertionError("provider resolved")), \
                mock.patch.object(P, "install_module",
                                  side_effect=AssertionError("network/install")):
            with self.assertRaisesRegex(
                    SystemExit, "excluded maker: X/Osc"):
                P._generate(prompt, {}, None, maker_resolution=resolution)

    def test_capability_preflight_excludes_denied_installed_and_fetch_options(self) -> None:
        prompt = "use an oscillator; avoid Xylophone Labs"
        resolution = P.resolve_maker_intent(prompt, CATALOGUE)
        module = {"name": "Osc", "tags": ["Oscillator"]}
        inventory = {"X": {"modules": {"Osc": module}}}
        midx = {"X": {"Osc": module}}
        with mock.patch.object(P, "entitlements_cached", return_value=set()), \
                mock.patch.object(P, "install_module",
                                  side_effect=AssertionError("network/install")):
            result = P.preflight(
                prompt, inventory, midx, CATALOGUE, resolution)
        self.assertFalse(result["ok"])
        self.assertEqual([], result["missing"]["Oscillator"])

    def test_generate_exact_selection_consumes_shared_resolution_once(self) -> None:
        resolution = P.resolve_maker_intent("use @X/Osc", CATALOGUE)
        seen = []

        class Selected(Exception):
            pass

        def exact(*args, **kwargs):
            seen.append(args[-1])
            raise Selected()

        with mock.patch.object(P, "catalog", return_value=CATALOGUE), \
                mock.patch.object(P, "resolve_maker_intent",
                                  side_effect=AssertionError("parsed twice")), \
                mock.patch.object(P, "module_index", return_value={}), \
                mock.patch.object(P, "exact_named_module_selection",
                                  side_effect=exact):
            with self.assertRaises(Selected):
                P._generate("use @X/Osc", {}, None,
                            maker_resolution=resolution)
        self.assertEqual([resolution], seen)

    def test_refinement_lint_reuses_precomputed_exact_intent(self) -> None:
        named = {("X", "Osc")}
        prompt = "remove @X/Osc"
        intent = P.named_module_intent(prompt, named)
        base = {"modules": [{"id": 1, "plugin": "X", "model": "Osc",
                             "params": []}], "cables": []}
        candidate = {"modules": [{"id": 1, "plugin": "X", "model": "Osc",
                                  "params": [0.5]}], "cables": []}
        with mock.patch.object(
                P, "exact_named_module_selection",
                side_effect=AssertionError("exact intent parsed twice")):
            errors = P.refinement_errors(
                base, candidate, prompt, {}, named, intent)
        self.assertRegex("\n".join(errors), "asked to remove X/Osc")

    def test_excluded_generated_maker_is_removed_from_library_first_plan(self) -> None:
        inventory = source_policy_inventory()
        resolution = M.resolve(
            "a slowly evolving ambient drone; avoid Forge Modular",
            {"ForgeModular": {"brand": "Forge Modular"},
             "Library": {"brand": "Library"}, "Core": {"brand": "Core"}})
        plan = P.intent_module_plan(
            "a slowly evolving ambient drone; avoid Forge Modular", inventory,
            module_source="prefer_existing", maker_resolution=resolution)
        self.assertNotIn("ForgeModular/", plan)
        self.assertIn("Library/LFO", plan)


def check_maker_intent() -> tuple[int, int]:
    """Run this split suite once from the required pre-Rack test aggregate."""
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(MakerIntentTest)
    result = unittest.TextTestRunner(stream=io.StringIO()).run(suite)
    bad = len(result.failures) + len(result.errors)
    print(f"  {'ok' if not bad else 'WRONG'}    maker intent suite: "
          f"{result.testsRun - bad}/{result.testsRun}")
    return bad, result.testsRun


if __name__ == "__main__":
    unittest.main()
