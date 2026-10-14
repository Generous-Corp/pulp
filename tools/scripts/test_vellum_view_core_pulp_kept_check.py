#!/usr/bin/env python3
"""Negative controls for the view-core Pulp-kept closure assertion."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("vellum_view_core_pulp_kept_check.py")
SPEC = importlib.util.spec_from_file_location("vellum_view_core_pulp_kept_check", SCRIPT)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)
ROOT = SCRIPT.parents[2]
UNITS = json.loads((ROOT / "tools/scripts/vellum_d3_units.json").read_text(encoding="utf-8"))
RULES = checker.view_core_rules(UNITS)
PLUGIN_VIEW_HOST_FILES = (
    "core/view/src/plugin_frame_renderer.cpp",
    "core/view/include/pulp/view/plugin_frame_renderer.hpp",
    "core/view/src/plugin_view_host_stub.cpp",
    "core/view/platform/mac/plugin_view_host_mac.mm",
    "core/view/platform/mac/plugin_view_host_mac_drag.mm",
    "core/view/platform/mac/plugin_view_host_mac_text_input.mm",
    "core/view/platform/mac/plugin_view_host_mac_view.h",
    "core/view/platform/mac/plugin_view_host_mac_script_keys.hpp",
    "core/view/platform/ios/plugin_view_host_ios.mm",
    "core/view/platform/win/plugin_view_host_win.cpp",
    "core/view/platform/linux/plugin_view_host_linux.cpp",
)
DEFERRED = {
    "path": "core/view/src/example_deferred.cpp",
    "owner": "example carve",
    "deferral_ruling": "example ruling",
}


def with_deferral() -> dict:
    """The checked-in rules plus one synthetic deferral, so the deferral
    mechanism stays covered after the last real deferral closed."""
    units = copy.deepcopy(UNITS)
    checker.view_core_rules(units).setdefault("deferred", []).append(dict(DEFERRED))
    return units


def clean_manifest() -> dict:
    """A closure that satisfies the checked-in rules: seam headers and
    deferred files present, kept files and kept targets absent."""
    files = {"core/view/src/view.cpp"}
    files.update(seam["path"] for seam in RULES["seam_headers"])
    files.update(entry["path"] for entry in RULES.get("deferred", []))
    return {
        "files": sorted(files),
        "targets": [
            {"name": "pulp-view-core", "link_targets": ["pulp-runtime"]},
            {"name": "pulp-view-script", "link_targets": ["pulp-view-core"]},
        ],
        "transitive_link_targets": ["pulp-runtime", "pulp-view-core"],
    }


class Tests(unittest.TestCase):
    def check(self, manifest: dict, units: dict = UNITS, root: Path = ROOT) -> list[str]:
        return checker.check(manifest, units, root)

    def test_clean_manifest_passes(self) -> None:
        self.assertEqual(self.check(clean_manifest()), [])

    def test_every_kept_file_in_the_closure_fails(self) -> None:
        for path in RULES["files"]:
            with self.subTest(path=path):
                manifest = clean_manifest()
                manifest["files"].append(path)
                errors = self.check(manifest)
                self.assertTrue(any("Pulp-kept file" in e and path in e for e in errors), errors)

    def test_extracted_target_linking_kept_target_fails(self) -> None:
        manifest = clean_manifest()
        manifest["targets"][1]["link_targets"].append("pulp-view-tooling")
        errors = self.check(manifest)
        self.assertTrue(any("pulp-view-script links Pulp-kept" in e for e in errors), errors)

    def test_transitive_kept_target_fails(self) -> None:
        manifest = clean_manifest()
        manifest["transitive_link_targets"].append("pulp-view-tooling")
        errors = self.check(manifest)
        self.assertTrue(any("reaches Pulp-kept target" in e for e in errors), errors)

    def test_non_unit_target_may_link_kept_target(self) -> None:
        manifest = clean_manifest()
        manifest["targets"].append({"name": "pulp-view", "link_targets": ["pulp-view-tooling"]})
        self.assertEqual(self.check(manifest), [])

    def test_seam_header_reaching_state_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for seam in RULES["seam_headers"]:
                target = root / seam["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / seam["path"]).read_bytes())
            host = root / "core/view/include/pulp/view/host_surfaces.hpp"
            host.write_text("#include <pulp/state/store.hpp>\n" + host.read_text(encoding="utf-8"),
                            encoding="utf-8")
            errors = self.check(clean_manifest(), root=root)
            self.assertTrue(any("includes pulp/state/store.hpp" in e for e in errors), errors)

    def test_deferred_file_that_left_the_closure_is_stale(self) -> None:
        units = with_deferral()
        errors = self.check(clean_manifest(), units)
        self.assertTrue(any("has left the extracted closure" in e for e in errors), errors)

    def test_live_deferral_passes(self) -> None:
        manifest = clean_manifest()
        manifest["files"].append(DEFERRED["path"])
        self.assertEqual(self.check(manifest, with_deferral()), [])

    def test_deferral_without_owner_fails(self) -> None:
        units = with_deferral()
        checker.view_core_rules(units)["deferred"][0]["owner"] = ""
        manifest = clean_manifest()
        manifest["files"].append(DEFERRED["path"])
        errors = self.check(manifest, units)
        self.assertTrue(any("deferred entry needs" in e for e in errors), errors)

    def test_plugin_view_host_layer_is_kept_not_deferred(self) -> None:
        # The frame pipeline and every platform editor host are asserted absent
        # from the extracted closure, and none of them is deferred.
        self.assertEqual(RULES.get("deferred", []), [])
        for path in PLUGIN_VIEW_HOST_FILES:
            with self.subTest(path=path):
                self.assertIn(path, RULES["files"])
                self.assertTrue((ROOT / path).is_file(), path)

    def test_plugin_view_host_natives_in_the_closure_fail(self) -> None:
        for path in PLUGIN_VIEW_HOST_FILES:
            with self.subTest(path=path):
                manifest = clean_manifest()
                manifest["files"].append(path)
                errors = self.check(manifest)
                self.assertTrue(any("Pulp-kept file" in e and path in e for e in errors), errors)

    def test_plugin_view_host_seam_reaching_frame_renderer_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for seam in RULES["seam_headers"]:
                target = root / seam["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / seam["path"]).read_bytes())
            host = root / "core/view/include/pulp/view/plugin_view_host.hpp"
            host.write_text("#include <pulp/view/plugin_frame_renderer.hpp>\n"
                            + host.read_text(encoding="utf-8"), encoding="utf-8")
            errors = self.check(clean_manifest(), root=root)
            self.assertTrue(
                any("includes pulp/view/plugin_frame_renderer.hpp" in e for e in errors), errors)

    def test_collaborator_also_listed_as_kept_fails(self) -> None:
        units = copy.deepcopy(UNITS)
        rules = checker.view_core_rules(units)
        rules["files"].append(rules["view_core_collaborators"][0]["path"])
        errors = self.check(clean_manifest(), units)
        self.assertTrue(any("both a view-core collaborator" in e for e in errors), errors)

    def test_missing_rules_fail(self) -> None:
        units = copy.deepcopy(UNITS)
        for unit in units["units"]:
            unit.pop("pulp_kept", None)
        errors = self.check(clean_manifest(), units)
        self.assertTrue(any("no pulp_kept rules" in e for e in errors), errors)


if __name__ == "__main__":
    unittest.main()
