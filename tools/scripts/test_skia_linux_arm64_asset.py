#!/usr/bin/env python3
"""Pulp #47 / Skia chrome/m150: guard the linux-arm64 Skia asset lane.

History: Pulp #47 added a ``linux-arm64`` entry to
``tools/deps/manifest.json`` under ``Skia → determinism →
release_assets`` after the GPU-gated CLI on Ubuntu aarch64 fell back to
the no-Skia path silently — same failure mode as pulp #1817 on
darwin-arm64, but for the linux ARM lane.

The chrome/m150 release of the danielraffel/skia-builder fork does NOT
publish a ``skia-build-linux-arm64-gpu-release.zip`` slice (m149 did).
Linux arm64 therefore stays on the m149 asset or rebuilds from source
via ``tools/build-skia.sh`` until the fork republishes that slice.

This test guards the two invariants that still hold on m150 so the
absence is deliberate and observable rather than a silent regression:

1. ``fetch_skia_for_release.py`` still maps ``linux-arm64 →
   linux-arm64`` and resolves to the canonical
   ``linux-gpu/lib/Release/libskia.a`` location FindSkia.cmake probes —
   so the day the fork republishes the slice, dropping the URL+sha back
   into the manifest is the only change needed.
2. If a ``linux-arm64`` release asset entry IS present in the manifest,
   it must be well-formed (canonical fork URL + 64-hex sha256) and
   mirrored in ``tools/harness/visual/pins.py``. On m150 the entry is
   intentionally absent, and this test asserts that fetch wiring copes
   with the absence.

Run with:

    python3 -m pytest tools/scripts/test_skia_linux_arm64_asset.py -v

or without pytest:

    python3 tools/scripts/test_skia_linux_arm64_asset.py
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import re
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tools" / "deps" / "manifest.json"


def _skia_entry() -> dict:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for dep in data.get("dependencies", []):
        if isinstance(dep, dict) and dep.get("name") == "Skia":
            return dep
    raise AssertionError("Skia entry missing from tools/deps/manifest.json")


def _release_assets() -> dict:
    return _skia_entry().get("determinism", {}).get("release_assets", {})


class LinuxArm64ManifestEntry(unittest.TestCase):
    def test_release_asset_absent_or_well_formed(self) -> None:
        assets = _release_assets()
        if "linux-arm64" not in assets:
            # chrome/m150 reality: the fork does not publish the slice.
            # Nothing to validate beyond the deliberate absence.
            return

        entry = assets["linux-arm64"]
        url = entry.get("url", "")
        self.assertTrue(
            url.startswith(
                "https://github.com/danielraffel/skia-builder/releases/"
                "download/chrome/"
            )
            and url.endswith("skia-build-linux-arm64-gpu-release.zip"),
            f"linux-arm64 URL must point at the danielraffel/skia-builder "
            f"chrome release and end in the canonical asset filename; "
            f"got {url!r}",
        )

        sha = entry.get("sha256", "")
        self.assertTrue(
            sha == "PLACEHOLDER_LINUX_ARM64_SHA256"
            or re.fullmatch(r"[0-9a-f]{64}", sha),
            f"linux-arm64 sha256 must be 64 hex chars or the documented "
            f"placeholder sentinel; got {sha!r}",
        )


class FetchScriptWiring(unittest.TestCase):
    def test_matrix_to_manifest_maps_linux_arm64(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "fetch_skia_for_release",
            REPO_ROOT / "tools" / "scripts" / "fetch_skia_for_release.py",
        )
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        # Mapping stays wired even while m150 ships no slice — so a future
        # republish only needs the manifest URL+sha re-added.
        self.assertEqual(mod.MATRIX_TO_MANIFEST.get("linux-arm64"), "linux-arm64")
        expected = mod.expected_library_path("linux-arm64")
        self.assertEqual(
            str(expected),
            "external/skia-build/build/linux-gpu/lib/Release/libskia.a",
            "linux-arm64 must resolve to the same FindSkia.cmake-probed "
            "location as linux-x64 — the post-flatten linux-gpu/ dir.",
        )


class VisualHarnessPinsInSync(unittest.TestCase):
    def test_pins_module_mirrors_manifest_for_linux_arm64(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "pulp_pins",
            REPO_ROOT / "tools" / "harness" / "visual" / "pins.py",
        )
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        manifest_sha = _release_assets().get("linux-arm64", {}).get("sha256")
        if manifest_sha is None:
            # Absent on both sides is the m150 steady state.
            self.assertNotIn(
                "linux-arm64",
                mod.RELEASE_ASSET_SHA256,
                "pins.py must not carry a stale linux-arm64 sha256 once the "
                "manifest drops the slice — the two must stay in lockstep.",
            )
            return

        self.assertEqual(
            mod.RELEASE_ASSET_SHA256.get("linux-arm64"),
            manifest_sha,
            "pins.py linux-arm64 sha256 must equal the manifest entry; "
            "this is the gate that fails CI when only one side is updated.",
        )


if __name__ == "__main__":
    unittest.main()
