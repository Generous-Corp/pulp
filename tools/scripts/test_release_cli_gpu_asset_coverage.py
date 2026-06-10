#!/usr/bin/env python3
"""Guard: every release-cli platform that REQUIRES GPU must have a Skia asset.

Root-cause backstop for the chrome/m150 linux-arm64 incident. The failure was
a *contradiction* between two files that no existing test cross-checked:

  * ``.github/workflows/release-cli.yml`` set
    ``PULP_REQUIRE_GPU_FOR_SDK=ON`` for ``linux-arm64`` (in both the CLI
    configure and the Linux SDK-build configure ``case`` blocks), which makes
    a missing Skia a FATAL configure error.
  * ``tools/deps/manifest.json`` had no ``linux-arm64`` entry under
    ``Skia → determinism → release_assets``, so
    ``fetch_skia_for_release.py`` correctly *skipped* the fetch.

Result: the linux-arm64 release leg FATALed with "Skia not found", every SDK
release stayed a draft, and ``test_skia_linux_arm64_asset.py`` stayed green
because it deliberately *tolerates* the absent asset. Nothing tied "I require
GPU here" to "I provide a Skia asset here".

This guard encodes that invariant:

    For every platform that release-cli.yml marks PULP_REQUIRE_GPU_FOR_SDK=ON,
    MATRIX_TO_MANIFEST[platform] must resolve to a release_assets entry in
    manifest.json.

It fails loudly the moment a platform is flipped GPU-ON without its Skia asset
being published + pinned (or vice-versa — an asset removed while still
required).

Run with:

    python3 -m pytest tools/scripts/test_release_cli_gpu_asset_coverage.py -v

or without pytest:

    python3 tools/scripts/test_release_cli_gpu_asset_coverage.py
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import re
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RELEASE_CLI = REPO_ROOT / ".github" / "workflows" / "release-cli.yml"
MANIFEST = REPO_ROOT / "tools" / "deps" / "manifest.json"
FETCH_SCRIPT = REPO_ROOT / "tools" / "scripts" / "fetch_skia_for_release.py"

# A case label line such as "  darwin-arm64|linux-x64)" — capture the tokens
# before the closing paren.
_CASE_LABEL = re.compile(r"^\s*([A-Za-z0-9_.\-|]+)\)\s*$")
_REQUIRE_ON = re.compile(r"PULP_REQUIRE_GPU_FOR_SDK=ON")


def gpu_on_platforms() -> set[str]:
    """Platforms whose case label is followed by a REQUIRE_GPU=ON assignment."""
    lines = RELEASE_CLI.read_text(encoding="utf-8").splitlines()
    found: set[str] = set()
    for i, line in enumerate(lines):
        m = _CASE_LABEL.match(line)
        if not m:
            continue
        # The REQUIRE_GPU="...=ON" assignment is the next line or two.
        window = "\n".join(lines[i + 1 : i + 3])
        if _REQUIRE_ON.search(window):
            for token in m.group(1).split("|"):
                token = token.strip()
                if token:
                    found.add(token)
    return found


def _load_fetch_module():
    spec = importlib.util.spec_from_file_location("fetch_skia_for_release", FETCH_SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _skia_release_assets() -> dict:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for dep in data.get("dependencies", []):
        if isinstance(dep, dict) and dep.get("name") == "Skia":
            return dep.get("determinism", {}).get("release_assets", {})
    raise AssertionError("Skia entry missing from tools/deps/manifest.json")


class GpuOnRequiresAsset(unittest.TestCase):
    def test_parser_finds_the_known_gpu_on_platforms(self) -> None:
        # Sanity: the regex must actually be matching the workflow. If this
        # set ever empties out, the parser silently stopped working and the
        # real assertion below would pass vacuously.
        platforms = gpu_on_platforms()
        self.assertIn(
            "linux-x64",
            platforms,
            "parser failed to find any GPU-ON platform in release-cli.yml; "
            "the case-block format likely changed — update _CASE_LABEL.",
        )

    def test_every_gpu_on_platform_has_a_skia_asset(self) -> None:
        fetch = _load_fetch_module()
        mapping = fetch.MATRIX_TO_MANIFEST
        assets = _skia_release_assets()

        missing = []
        for platform in sorted(gpu_on_platforms()):
            self.assertIn(
                platform,
                mapping,
                f"release-cli.yml requires GPU for {platform!r} but it has no "
                f"MATRIX_TO_MANIFEST mapping in fetch_skia_for_release.py",
            )
            manifest_key = mapping[platform]
            if manifest_key not in assets:
                missing.append((platform, manifest_key))

        self.assertEqual(
            missing,
            [],
            "release-cli.yml sets PULP_REQUIRE_GPU_FOR_SDK=ON for platform(s) "
            "with no Skia release asset in manifest.json — the configure step "
            "will FATAL with 'Skia not found' and the release will stay a "
            "draft. Either publish + pin the asset, or drop the platform from "
            "the REQUIRE_GPU=ON case block. Offenders (platform -> manifest "
            f"key): {missing}",
        )


if __name__ == "__main__":
    unittest.main()
