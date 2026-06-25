#!/usr/bin/env python3
"""Pulp #47 / Skia chrome/m150: guard the linux-arm64 Skia asset lane.

History: Pulp #47 added a ``linux-arm64`` entry to
``tools/deps/manifest.json`` under ``Skia → determinism →
release_assets`` after the GPU-gated CLI on Ubuntu aarch64 fell back to
the no-Skia path silently — same failure mode as pulp #1817 on
darwin-arm64, but for the linux ARM lane.

The chrome/m150 release of the danielraffel/skia-builder fork initially
shipped no ``skia-build-linux-arm64-gpu-release.zip`` slice: the fork's
linux-arm64 build lane (skia-builder 634672f) was added ~20h after m150
was cut, and that commit wired the build matrix but not the
create-release upload list. Both are fixed now — the fork uploads
linux-arm64 on every release, and the m150 release was backfilled with
the slice — so the manifest carries a ``linux-arm64`` entry again.

This test guards the invariants that keep the lane honest:

1. ``fetch_skia_for_release.py`` maps ``linux-arm64 → linux-arm64`` and
   resolves to the canonical ``linux-gpu/lib/Release/libskia.a`` location
   FindSkia.cmake probes (shared with linux-x64 after the post-unpack
   flatten).
2. If a ``linux-arm64`` release asset entry is present in the manifest
   (the m150 steady state), it must be well-formed (canonical fork URL +
   64-hex sha256) and mirrored in ``tools/harness/visual/pins.py``. The
   test still tolerates a temporary absence so a future Skia bump that
   lands before the slice is rebaked doesn't hard-fail; the harder
   "GPU-ON requires an asset" invariant lives in
   ``test_release_cli_gpu_asset_coverage.py``.

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
            # Temporary absence is allowed while a future Skia bump is being
            # rebaked; nothing else can be validated without an asset entry.
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

        # Mapping stays wired even when the manifest temporarily drops the
        # asset entry, so republishing only needs the URL+sha re-added.
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
