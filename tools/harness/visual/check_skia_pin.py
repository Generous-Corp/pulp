#!/usr/bin/env python3
"""Lint: the visual-harness Dockerfile must match the canonical Skia pin.

Roadmap P9-4. The visual harness builds a deterministic raster image inside
``ci/visual-harness.Dockerfile``, which downloads a pinned ``linux/amd64`` Skia
archive via ``ARG`` lines. The canonical, machine-readable Skia pin lives in
``tools/deps/manifest.json`` under the ``Skia`` dependency's ``determinism``
block (per CLAUDE.md, the manifest is the dependency inventory source of truth).

Nothing previously cross-checked the two, so the Dockerfile's baked-in Skia
release tag, archive URL, SHA-256, and skia-python version could silently drift
from the manifest. This script reads both ends and exits non-zero with a clear
diff message on any mismatch.

Usage::

    python3 tools/harness/visual/check_skia_pin.py

Exit codes:
    0 — Dockerfile Skia pin matches the canonical manifest pin.
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
REPO_ROOT = HERE.parents[2]
MANIFEST_PATH = REPO_ROOT / "tools/deps/manifest.json"
DOCKERFILE_PATH = REPO_ROOT / "ci/visual-harness.Dockerfile"

# The Dockerfile builds the Linux x64 image, so it is checked against the
# manifest's ``linux-x64`` release asset.
DOCKER_PLATFORM = "linux-x64"


class CheckError(Exception):
    """Raised when an input cannot be parsed (exit code 2)."""


def _parse_dockerfile_args(text: str) -> Dict[str, str]:
    """Return the ``ARG name=value`` defaults declared in the Dockerfile."""
    args: Dict[str, str] = {}
    arg_re = re.compile(r"^\s*ARG\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$")
    for line in text.splitlines():
        match = arg_re.match(line)
        if match:
            args[match.group(1)] = match.group(2).strip().strip('"').strip("'")
    return args


def read_dockerfile_pin(path: Path = DOCKERFILE_PATH) -> Dict[str, str]:
    """Extract the Skia pin baked into the visual-harness Dockerfile."""
    if not path.is_file():
        raise CheckError(f"Dockerfile not found: {path}")
    args = _parse_dockerfile_args(path.read_text(encoding="utf-8"))

    required = (
        "SKIA_RELEASE_TAG",
        "SKIA_LINUX_X64_SHA256",
        "SKIA_LINUX_X64_URL",
        "SKIA_PYTHON_VERSION",
    )
    missing = [name for name in required if name not in args]
    if missing:
        raise CheckError(
            f"{path} is missing required ARG line(s): {', '.join(missing)}"
        )

    return {
        "release_tag": args["SKIA_RELEASE_TAG"],
        "sha256": args["SKIA_LINUX_X64_SHA256"],
        "url": args["SKIA_LINUX_X64_URL"],
        "python_version": args["SKIA_PYTHON_VERSION"],
    }


def read_manifest_pin(path: Path = MANIFEST_PATH) -> Dict[str, str]:
    """Extract the canonical Skia pin from tools/deps/manifest.json."""
    if not path.is_file():
        raise CheckError(f"manifest not found: {path}")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:  # pragma: no cover - defensive
        raise CheckError(f"{path} is not valid JSON: {exc}") from exc

    skia = None
    for dep in manifest.get("dependencies", []):
        if dep.get("name") == "Skia":
            skia = dep
            break
    if skia is None:
        raise CheckError(f"{path} has no 'Skia' dependency entry")

    determinism = skia.get("determinism")
    if not isinstance(determinism, dict):
        raise CheckError(f"{path} Skia entry has no 'determinism' block")

    assets = determinism.get("release_assets", {})
    if not isinstance(assets, dict):
        raise CheckError(
            f"{path} Skia determinism 'release_assets' is not an object"
        )
    asset = assets.get(DOCKER_PLATFORM)
    if not isinstance(asset, dict):
        raise CheckError(
            f"{path} Skia determinism block has no "
            f"release_assets['{DOCKER_PLATFORM}']"
        )

    for key in ("skia_branch", "skia_python_smoke_version"):
        if key not in determinism:
            raise CheckError(f"{path} Skia determinism block missing '{key}'")
    for key in ("url", "sha256"):
        if key not in asset:
            raise CheckError(
                f"{path} release_assets['{DOCKER_PLATFORM}'] missing '{key}'"
            )

    return {
        "release_tag": determinism["skia_branch"],
        "sha256": asset["sha256"],
        "url": asset["url"],
        "python_version": determinism["skia_python_smoke_version"],
    }


# Human-readable label per compared field.
_FIELD_LABELS = {
    "release_tag": "Skia release tag",
    "sha256": "Linux x64 archive SHA-256",
    "url": "Linux x64 archive URL",
    "python_version": "skia-python version",
}


def compare(dockerfile: Dict[str, str], manifest: Dict[str, str]) -> list[str]:
    """Return a list of human-readable drift messages (empty == in sync)."""
    drift: list[str] = []
    for field, label in _FIELD_LABELS.items():
        if dockerfile[field] != manifest[field]:
            drift.append(
                f"  {label}:\n"
                f"    Dockerfile (ci/visual-harness.Dockerfile): {dockerfile[field]}\n"
                f"    manifest   (tools/deps/manifest.json):     {manifest[field]}"
            )
    return drift


def main(argv: list[str] | None = None) -> int:
    try:
        dockerfile = read_dockerfile_pin()
        manifest = read_manifest_pin()
    except CheckError as exc:
        print(f"check_skia_pin: ERROR: {exc}", file=sys.stderr)
        return 2

    drift = compare(dockerfile, manifest)
    if drift:
        print(
            "check_skia_pin: visual-harness Dockerfile Skia pin has drifted "
            "from the canonical manifest pin:",
            file=sys.stderr,
        )
        for entry in drift:
            print(entry, file=sys.stderr)
        print(
            "\nUpdate ci/visual-harness.Dockerfile ARG lines and "
            "tools/deps/manifest.json together so they stay in sync.",
            file=sys.stderr,
        )
        return 1

    print(
        "check_skia_pin: OK — visual-harness Dockerfile Skia pin matches "
        f"tools/deps/manifest.json (release {manifest['release_tag']})."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
