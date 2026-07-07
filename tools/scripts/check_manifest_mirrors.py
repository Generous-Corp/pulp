#!/usr/bin/env python3
"""Lint: tooling-consumed pin mirrors must match the canonical manifest.

``tools/deps/manifest.json`` is the dependency-inventory source of truth
(per CLAUDE.md). A handful of files hand-mirror the Skia/Dawn/V8 pin data,
and two of those mirrors are *tooling-consumed*, so a hand-sync typo is a
silent behavioural bug, not a cosmetic doc lag:

1. ``external/skia-build/VERSION.md`` — its "Release Asset Digests" table is
   read by ``tools/scripts/fetch_skia_for_release.py`` as a cache-skip oracle
   (a matching digest lets the fetcher skip the download). A stale digest here
   silently trusts a wrong cache.
2. ``DEPENDENCIES.md`` — the Skia/Dawn/V8 ``version`` cells are the license
   inventory of record; the release/NOTICE audit trail depends on them.

``check_skia_pin.py`` already guards the Dockerfile mirror, and
``pins.py`` is guarded by ``test_skia_determinism.py``. This lint closes the
two remaining tooling-consumed gaps for BOTH Skia and V8, so the next pin
bump is enforced instead of tribal. Prose mirrors (SKILL.md, READMEs) are
deliberately NOT guarded — a lagging patch digit there is low-harm.

Usage::

    python3 tools/scripts/check_manifest_mirrors.py

Exit codes:
    0 — every tooling-consumed mirror matches the manifest.
    1 — drift detected (mismatch printed to stderr).
    2 — could not parse one of the inputs (treated as a hard failure).
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Dict

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
MANIFEST_PATH = REPO_ROOT / "tools/deps/manifest.json"
SKIA_VERSION_MD = REPO_ROOT / "external/skia-build/VERSION.md"
DEPENDENCIES_MD = REPO_ROOT / "DEPENDENCIES.md"

# VERSION.md digest rows: | `skia-build-<platform>-gpu-release.zip` | `<sha>` |
_VERSION_ROW_RE = re.compile(
    r"\|\s*`skia-build-(?P<platform>[a-z0-9_\-]+)-gpu-release\.zip`\s*\|"
    r"\s*`(?P<sha>[0-9a-f]{64})`\s*\|"
)
# DEPENDENCIES.md table rows: | <name> | <version> | ...
_DEPS_ROW_RE = re.compile(r"^\|\s*(?P<name>[^|]+?)\s*\|\s*(?P<version>[^|]+?)\s*\|")


class CheckError(Exception):
    """Raised when an input cannot be parsed (exit code 2)."""


def _load_manifest(path: Path = MANIFEST_PATH) -> dict:
    if not path.is_file():
        raise CheckError(f"manifest not found: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:  # pragma: no cover - defensive
        raise CheckError(f"{path} is not valid JSON: {exc}") from exc


def _dep(manifest: dict, name: str) -> dict:
    for dep in manifest.get("dependencies", []):
        if dep.get("name") == name:
            return dep
    raise CheckError(f"manifest has no '{name}' dependency entry")


def manifest_skia_asset_shas(manifest: dict) -> Dict[str, str]:
    """Return {platform: sha256} for the Skia release assets in the manifest."""
    det = _dep(manifest, "Skia").get("determinism")
    if not isinstance(det, dict):
        raise CheckError("manifest Skia entry has no 'determinism' block")
    assets = det.get("release_assets")
    if not isinstance(assets, dict):
        raise CheckError("manifest Skia 'release_assets' is not an object")
    out: Dict[str, str] = {}
    for platform, info in assets.items():
        if not isinstance(info, dict) or "sha256" not in info:
            raise CheckError(f"manifest Skia release_assets['{platform}'] missing sha256")
        out[platform] = info["sha256"]
    return out


def version_md_asset_shas(path: Path = SKIA_VERSION_MD) -> Dict[str, str]:
    """Parse the {platform: sha256} digest table from external skia VERSION.md."""
    if not path.is_file():
        raise CheckError(f"VERSION.md not found: {path}")
    out: Dict[str, str] = {}
    for m in _VERSION_ROW_RE.finditer(path.read_text(encoding="utf-8")):
        out[m.group("platform")] = m.group("sha")
    if not out:
        raise CheckError(f"{path} has no parseable 'Release Asset Digests' table")
    return out


def dependencies_md_versions(path: Path = DEPENDENCIES_MD) -> Dict[str, str]:
    """Return {dep-name: version-cell} from the DEPENDENCIES.md inventory table."""
    if not path.is_file():
        raise CheckError(f"DEPENDENCIES.md not found: {path}")
    out: Dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _DEPS_ROW_RE.match(line)
        if m:
            out[m.group("name")] = m.group("version")
    if not out:
        raise CheckError(f"{path} has no parseable dependency table")
    return out


def compare(manifest: dict) -> list[str]:
    """Return human-readable drift messages (empty == every mirror in sync)."""
    drift: list[str] = []

    # 1. VERSION.md digest table == manifest Skia release_assets shas.
    man_shas = manifest_skia_asset_shas(manifest)
    md_shas = version_md_asset_shas()
    for platform, man_sha in man_shas.items():
        md_sha = md_shas.get(platform)
        if md_sha is None:
            drift.append(
                f"  external/skia-build/VERSION.md is missing a digest row for "
                f"Skia asset '{platform}' (present in manifest)."
            )
        elif md_sha != man_sha:
            drift.append(
                f"  Skia asset '{platform}' SHA-256 drift (VERSION.md is a "
                f"fetch cache-skip oracle):\n"
                f"    VERSION.md: {md_sha}\n"
                f"    manifest:   {man_sha}"
            )
    for platform in md_shas.keys() - man_shas.keys():
        drift.append(
            f"  external/skia-build/VERSION.md lists Skia asset '{platform}' "
            f"that is absent from the manifest release_assets."
        )

    # 2. DEPENDENCIES.md version cells == manifest version fields (Skia/Dawn/V8).
    dep_versions = dependencies_md_versions()
    for name in ("Skia", "Dawn", "V8"):
        man_version = _dep(manifest, name).get("version")
        md_version = dep_versions.get(name)
        if md_version is None:
            drift.append(f"  DEPENDENCIES.md has no '{name}' inventory row.")
        elif md_version != man_version:
            drift.append(
                f"  {name} version drift (DEPENDENCIES.md is the license "
                f"inventory of record):\n"
                f"    DEPENDENCIES.md: {md_version}\n"
                f"    manifest:        {man_version}"
            )
    return drift


def main(argv: list[str] | None = None) -> int:
    try:
        manifest = _load_manifest()
        drift = compare(manifest)
    except CheckError as exc:
        print(f"check_manifest_mirrors: ERROR: {exc}", file=sys.stderr)
        return 2

    if drift:
        print(
            "check_manifest_mirrors: tooling-consumed pin mirrors have drifted "
            "from tools/deps/manifest.json:",
            file=sys.stderr,
        )
        for entry in drift:
            print(entry, file=sys.stderr)
        print(
            "\nUpdate the mirror(s) and tools/deps/manifest.json together so "
            "they stay in sync (VERSION.md digests gate fetch caching; "
            "DEPENDENCIES.md versions are the license inventory).",
            file=sys.stderr,
        )
        return 1

    print(
        "check_manifest_mirrors: OK — external/skia-build/VERSION.md digests "
        "and DEPENDENCIES.md Skia/Dawn/V8 versions match tools/deps/manifest.json."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
