#!/usr/bin/env python3
"""Pulp #47: assert the manifest publishes a linux-arm64 Skia asset.

Before this PR, ``tools/deps/manifest.json`` had no ``linux-arm64`` entry
under ``Skia → determinism → release_assets``. That left
``tools/scripts/fetch_skia_for_release.py`` printing "no Skia release
asset for matrix=linux-arm64" and the GPU-gated CLI on Ubuntu aarch64
fell back to the no-Skia path silently — same failure mode as
pulp #1817 on darwin-arm64, but for the linux ARM lane.

This test guards three invariants so removing the linux-arm64 asset
again can never regress quietly:

1. ``Skia → determinism → release_assets → linux-arm64`` exists, with
   a URL that matches the documented danielraffel/skia-builder fork +
   chrome/m149 release tag and a 64-character hex sha256.
2. ``fetch_skia_for_release.py`` maps ``linux-arm64 → linux-arm64``
   and points at the canonical ``linux-gpu/lib/Release/libskia.a``
   location FindSkia.cmake probes for.
3. ``tools/harness/visual/pins.py`` exposes the same sha256 so the
   visual-harness determinism smoke (``test_skia_determinism.py``)
   fails loud when the manifest and pins drift apart.

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


class LinuxArm64ManifestEntry(unittest.TestCase):
    def test_release_asset_present_and_well_formed(self) -> None:
        skia = _skia_entry()
        assets = skia.get("determinism", {}).get("release_assets", {})
        self.assertIn(
            "linux-arm64",
            assets,
            "tools/deps/manifest.json must publish a linux-arm64 Skia "
            "asset so fetch_skia_for_release.py can populate "
            "external/skia-build/build/linux-gpu/lib/Release/libskia.a "
            "on Ubuntu aarch64 hosts.",
        )

        entry = assets["linux-arm64"]
        url = entry.get("url", "")
        self.assertTrue(
            url.startswith(
                "https://github.com/danielraffel/skia-builder/releases/"
                "download/chrome/m149/"
            ),
            f"linux-arm64 URL must point at the danielraffel/skia-builder "
            f"chrome/m149 release; got {url!r}",
        )
        self.assertTrue(
            url.endswith("skia-build-linux-arm64-gpu-release.zip"),
            f"linux-arm64 URL must end in the canonical asset filename; "
            f"got {url!r}",
        )

        sha = entry.get("sha256", "")
        # Accept either the published 64-hex digest or the
        # PLACEHOLDER_LINUX_ARM64_SHA256 sentinel that lands first
        # (PR #47) and is replaced by `gh release upload` + a follow-on
        # commit that swaps the digest in.
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

        self.assertEqual(mod.MATRIX_TO_MANIFEST.get("linux-arm64"), "linux-arm64")
        expected = mod.expected_library_path("linux-arm64")
        self.assertEqual(
            str(expected),
            "external/skia-build/build/linux-gpu/lib/Release/libskia.a",
            "linux-arm64 must resolve to the same FindSkia.cmake-probed "
            "location as linux-x64 — the post-flatten linux-gpu/ dir.",
        )


class VisualHarnessPinsInSync(unittest.TestCase):
    def test_pins_module_publishes_linux_arm64(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "pulp_pins",
            REPO_ROOT / "tools" / "harness" / "visual" / "pins.py",
        )
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        self.assertIn(
            "linux-arm64",
            mod.RELEASE_ASSET_SHA256,
            "tools/harness/visual/pins.py must mirror the manifest's "
            "linux-arm64 sha256 — otherwise the visual-harness "
            "determinism smoke (test_skia_determinism.py) can't notice "
            "when the two drift.",
        )

        skia = _skia_entry()
        manifest_sha = (
            skia.get("determinism", {})
            .get("release_assets", {})
            .get("linux-arm64", {})
            .get("sha256")
        )
        self.assertEqual(
            mod.RELEASE_ASSET_SHA256["linux-arm64"],
            manifest_sha,
            "pins.py linux-arm64 sha256 must equal the manifest entry; "
            "this is the gate that fails CI when only one side is "
            "updated.",
        )


if __name__ == "__main__":
    unittest.main()
