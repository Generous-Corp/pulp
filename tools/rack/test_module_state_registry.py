#!/usr/bin/env python3
"""The module-state registry stays exact, and stays connected to generation.

A registry entry is only worth writing if something downstream reads it. Two
independent failures are therefore checked separately here:

  * the entry is WRONG -- its pinned version no longer matches the installed
    plugin, so the rule silently stops applying while still looking present;
  * the entry is IGNORED -- the candidate allowlist, the model-facing
    inventory page, or the pre-flight refusal stops consulting the registry
    at all, so every rule silently stops applying at once.

Each check below is paired with a control that must move in the opposite
direction, because a check that reads clean whether or not the mechanism
works is not evidence.
"""

from __future__ import annotations

import json
import os
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P  # noqa: E402

# One refused module and one ordinary module from the same installed pack
# family, used as a matched pair throughout: the refusal must remove the
# first and must not touch the second.
REFUSED_PLUGIN, REFUSED_MODEL, REFUSED_VERSION = "VCV-Host", "Host-XL", "2.0.4"
ALLOWED_PLUGIN, ALLOWED_MODEL, ALLOWED_VERSION = "Fundamental", "VCO", "2.6.4"


def _module(name: str, **over) -> dict:
    module = {"name": name, "tags": ["Oscillator"], "inputs": ["Audio"],
              "outputs": ["Audio"], "params": []}
    module.update(over)
    return module


def paired_inventory(refused_version: str = REFUSED_VERSION) -> dict:
    """A refused module and an unrefused one, otherwise identical."""
    return {
        REFUSED_PLUGIN: {
            "name": "Host", "version": refused_version,
            "modules": {REFUSED_MODEL: _module(REFUSED_MODEL)},
        },
        ALLOWED_PLUGIN: {
            "name": "Fundamental", "version": ALLOWED_VERSION,
            "modules": {ALLOWED_MODEL: _module(ALLOWED_MODEL)},
        },
    }


class RegistryIsConsultedTest(unittest.TestCase):
    """Negative controls: these fail if the registry stops being read."""

    def test_allowlist_drops_a_refused_module_and_keeps_its_control(self) -> None:
        inv = paired_inventory()
        allowed = P.required_tag_candidate_allowlist(inv, narrowed=False)
        # Fails if the registry is not consulted, or the rule's version pin
        # no longer matches the installed version this fixture states.
        self.assertNotIn((REFUSED_PLUGIN, REFUSED_MODEL), allowed)
        # Control: fails if the allowlist collapsed for an unrelated reason,
        # which would make the assertion above pass for free.
        self.assertIn((ALLOWED_PLUGIN, ALLOWED_MODEL), allowed)

    def test_allowlist_control_also_holds_when_narrowed(self) -> None:
        inv = paired_inventory()
        allowed = P.required_tag_candidate_allowlist(inv, narrowed=True)
        self.assertNotIn((REFUSED_PLUGIN, REFUSED_MODEL), allowed)
        self.assertIn((ALLOWED_PLUGIN, ALLOWED_MODEL), allowed)

    def test_refused_module_is_absent_from_the_page_the_model_reads(self) -> None:
        rendered = P.render_inventory(paired_inventory())
        self.assertNotIn(REFUSED_MODEL, rendered)
        self.assertNotIn(REFUSED_PLUGIN, rendered)
        # Control: the page was really rendered.
        self.assertIn(ALLOWED_MODEL, rendered)
        self.assertIn(ALLOWED_PLUGIN, rendered)

    def test_refinement_still_describes_a_refused_module(self) -> None:
        """Refusal scopes to fresh authoring, not to reading an open patch."""
        rendered = P.render_inventory(
            paired_inventory(), fresh_generation=False)
        self.assertIn(REFUSED_MODEL, rendered)

    def test_version_pin_does_not_leak_to_a_neighbouring_version(self) -> None:
        inv = paired_inventory(refused_version="2.0.5")
        self.assertIsNone(P.fresh_generation_refusal(
            REFUSED_PLUGIN, REFUSED_MODEL, inv))
        self.assertIn((REFUSED_PLUGIN, REFUSED_MODEL),
                      P.required_tag_candidate_allowlist(inv, narrowed=False))

    def test_explicit_request_refuses_before_any_provider_call(self) -> None:
        inv = {"LomasModules": {
            "name": "Lomas Modules", "version": "2.0.0",
            "modules": {"AdvancedSampler": _module("Advanced Sampler")}}}
        with mock.patch.object(
                P, "find_claude",
                side_effect=AssertionError("provider resolution must not run")), \
                mock.patch.object(
                    P, "catalog",
                    side_effect=AssertionError("catalogue lookup must not run")):
            with self.assertRaises(SystemExit) as raised:
                P._generate(
                    "a beat built on LomasModules/AdvancedSampler", inv, None)
        message = str(raised.exception)
        self.assertIn("cannot freshly generate LomasModules/AdvancedSampler",
                      message)
        # The refusal reaches the reader with its mechanics intact, not just
        # as a bare status.
        self.assertIn("no sample to trigger", message)
        self.assertIn("Nothing was sent to the model", message)


class RegistryShapeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.rules = P.module_state_rules()

    def test_every_refusal_carries_an_actionable_reason(self) -> None:
        refusals = {key: rule for key, rule in self.rules.items()
                    if (rule.get("fresh_generation") or {}).get("status")
                    == "unsupported"}
        # Control: an empty registry would satisfy every assertion below.
        self.assertGreater(len(refusals), 1)
        for key, rule in refusals.items():
            with self.subTest(module=key):
                reason = rule["fresh_generation"]["reason"]
                self.assertIsInstance(reason, str)
                # A one-word reason is a label, not something a reader can
                # act on. The refusal is surfaced verbatim to the user.
                self.assertGreater(len(reason.split()), 8)
                self.assertEqual(reason, reason.strip())

    def test_a_blank_reason_is_refused_rather_than_silently_ignored(self) -> None:
        inv = paired_inventory()
        for blank in ("", "   ", None):
            with self.subTest(reason=blank):
                broken = {f"{REFUSED_PLUGIN}/{REFUSED_MODEL}": {
                    "plugin_version": REFUSED_VERSION,
                    "fresh_generation": {"status": "unsupported",
                                         "reason": blank}}}
                with self.assertRaises(RuntimeError):
                    P.fresh_generation_refusal(
                        REFUSED_PLUGIN, REFUSED_MODEL, inv, broken)

    def test_keys_are_plugin_slash_model_and_sorted(self) -> None:
        keys = list(self.rules)
        self.assertEqual(sorted(keys), keys)
        for key in keys:
            with self.subTest(module=key):
                self.assertEqual(1, key.count("/"))
                self.assertTrue(all(key.split("/")))


class PersistentAuthoringRefusalTest(unittest.TestCase):
    """Opaque musical state is never mistaken for ordinary button params."""

    def test_hexaquark_is_excluded_but_an_existing_patch_is_describable(
            self) -> None:
        inv = {
            "Geodesics-Vultiverse": {
                "name": "Geodesics Vultiverse", "version": "2.0.4",
                "modules": {
                    "Hexaquark": _module(
                        "Hexaquark", tags=["Sequencer"],
                        params=[{"id": 35, "name": "Scene 1"},
                                {"id": 51, "name": "Run"}]),
                    "Ions": _module("Ions", tags=["Utility"]),
                },
            },
        }
        fresh = P.render_inventory(inv)
        refinement = P.render_inventory(inv, fresh_generation=False)
        self.assertNotIn("Hexaquark", fresh)
        self.assertIn("Ions", fresh)
        self.assertIn("Hexaquark", refinement)
        reason = P.fresh_generation_refusal(
            "Geodesics-Vultiverse", "Hexaquark", inv)
        self.assertIn("opaque module-owned state", reason)
        self.assertIn("external clock", reason)


class InstalledVersionPinTest(unittest.TestCase):
    """Every pin still names the exact version installed on this machine.

    A drifted pin does not fail loudly: the rule stops applying and the module
    quietly returns to the generator's vocabulary. Only a comparison against
    the real inventory can see that, so this test needs Rack's plugin
    directory and says so rather than passing vacuously without it.
    """

    def test_pins_match_the_installed_inventory(self) -> None:
        inv = P.inventory()
        # Control: without a populated inventory every pin check below is
        # vacuous, so refuse to report a pass at all.
        if not (inv.get("Core") or {}).get("modules"):
            self.skipTest("no installed Rack plugin inventory to check pins "
                          "against; this proves nothing")
        installed = {slug for slug, pkg in inv.items()
                     if (pkg.get("modules") or {})}
        self.assertGreater(len(installed), 1)
        drift = []
        for key, rule in P.module_state_rules().items():
            plugin, model = key.split("/", 1)
            package = inv.get(plugin)
            if package is None:
                drift.append(f"{key}: plugin is not installed")
                continue
            if model not in (package.get("modules") or {}):
                drift.append(f"{key}: model is absent from the installed pack")
            pinned, actual = rule.get("plugin_version"), package.get("version")
            if pinned != actual:
                drift.append(
                    f"{key}: pinned {pinned!r}, installed {actual!r}")
        self.assertEqual([], drift)

    def test_each_refusal_reaches_the_real_inventory(self) -> None:
        inv = P.inventory()
        if not (inv.get("Core") or {}).get("modules"):
            self.skipTest("no installed Rack plugin inventory; this proves "
                          "nothing")
        rules = P.module_state_rules()
        refused = {tuple(key.split("/", 1)) for key, rule in rules.items()
                   if (rule.get("fresh_generation") or {}).get("status")
                   == "unsupported"}
        allowed = P.required_tag_candidate_allowlist(inv, narrowed=False)
        self.assertGreater(len(allowed), len(refused))
        for pair in sorted(refused):
            with self.subTest(module="/".join(pair)):
                self.assertNotIn(pair, allowed)


class CensusRefusalTest(unittest.TestCase):
    """The refusals added from the offline audibility census still bite.

    These entries were written from a measurement rather than from reading a
    panel, so the thing most likely to rot is the link between the two: a pin
    that drifts off the installed version, or an expander refused while the
    module it expands quietly becomes generatable again. Neither fails loudly
    on its own -- the rule just stops applying -- so both are checked here
    against the real inventory, with controls that must move the other way.
    """

    # (plugin, model, the module this one expands or None if it stands alone).
    MEASURED = (
        ("voxglitch", "GrainEngineMK2Expander", "GrainEngineMK2"),
        ("voxglitch", "GrooveBoxExpander", "groovebox"),
        ("SickoCV", "Parking", None),
    )
    # One module per pack that the census found emitting signal and that the
    # registry deliberately leaves alone. If a pack-wide refusal ever lands by
    # accident these are what notices.
    CONTROLS = (("voxglitch", "vector_rotation"), ("SickoCV", "Adder8"))

    def setUp(self) -> None:
        self.inv = P.inventory()
        if not (self.inv.get("Core") or {}).get("modules"):
            self.skipTest("no installed Rack plugin inventory to measure these "
                          "refusals against; this proves nothing")
        self.rules = P.module_state_rules()

    def test_each_measured_refusal_removes_its_module(self) -> None:
        allowed = P.required_tag_candidate_allowlist(self.inv, narrowed=False)
        for plugin, model, _ in self.MEASURED:
            with self.subTest(module=f"{plugin}/{model}"):
                # Fails if the pin drifts off the installed version, because
                # the refusal then stops applying while still looking present.
                self.assertIsNotNone(
                    P.fresh_generation_refusal(plugin, model, self.inv))
                self.assertNotIn((plugin, model), allowed)
        for plugin, model in self.CONTROLS:
            with self.subTest(control=f"{plugin}/{model}"):
                # Control: fails if the allowlist collapsed, or if the refusal
                # spread to the whole pack, either of which would make the
                # assertions above pass for free.
                self.assertIsNone(
                    P.fresh_generation_refusal(plugin, model, self.inv))
                self.assertIn((plugin, model), allowed)

    def test_an_expander_is_not_refused_while_its_base_still_is_not(self) -> None:
        """Refusing the expander but not the module it expands is incoherent.

        The expander reasons rest on the base being unusable too. If the base
        is ever made generatable, the expander's stated mechanic is no longer
        true and its entry has to be rewritten rather than left standing.
        """
        bases = [(plugin, base) for plugin, _, base in self.MEASURED if base]
        # Control: an empty pair list would satisfy the loop vacuously.
        self.assertEqual(2, len(bases))
        for plugin, base in bases:
            with self.subTest(base=f"{plugin}/{base}"):
                # The base is a real installed model, so the check is not
                # passing merely because the name is wrong.
                self.assertIn(base,
                              (self.inv[plugin].get("modules") or {}))
                self.assertIsNotNone(
                    P.fresh_generation_refusal(plugin, base, self.inv))

    def test_measured_refusals_state_the_measurement_not_just_a_mechanic(
            self) -> None:
        for plugin, model, _ in self.MEASURED:
            with self.subTest(module=f"{plugin}/{model}"):
                reason = self.rules[f"{plugin}/{model}"]["fresh_generation"][
                    "reason"]
                self.assertIn("Measured offline", reason)
                # The census drove eleven parameter and input conditions; a
                # reason that cites the measurement has to say what it covered
                # or a reader cannot tell how weak the claim is.
                self.assertIn("eleven", reason)


class RegistryFileTest(unittest.TestCase):
    def test_the_file_on_disk_is_the_object_the_loader_returns(self) -> None:
        with open(P.MODULE_STATE_OVERRIDES, encoding="utf-8") as handle:
            raw = json.load(handle)
        self.assertEqual(raw, P.module_state_rules())


if __name__ == "__main__":
    unittest.main()
