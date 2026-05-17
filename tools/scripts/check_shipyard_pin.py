#!/usr/bin/env python3
"""Single-source-of-truth check for the Shipyard pin (D1).

`tools/shipyard.toml` is the canonical pin used by
`tools/install-shipyard.sh` and `shipyard pr`. Two release workflows
(`.github/workflows/release-cli.yml` and `.github/workflows/post-tag-sync.yml`)
also bake the version into a `SHIPYARD_VERSION` env value. Prior drift
between those locations broke release-note generation (Codex P1 on
PR #719), and the fix at the time was "remember to update both".

This script makes the drift mechanically impossible to ship by
validating that every workflow's `SHIPYARD_VERSION` matches the pin in
`tools/shipyard.toml`. Wire it into a workflow lint test or run it
locally before pushing release-pipeline changes.

Exits:
    0 — every workflow `SHIPYARD_VERSION` value matches the pin.
    1 — at least one workflow declares a different version.
    2 — configuration error (file missing, unparseable, etc).

Pure stdlib; no third-party deps.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PIN_FILE = REPO_ROOT / "tools" / "shipyard.toml"
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

# `version = "v0.56.2"` in shipyard.toml. The leading `v` is part of the
# release tag; workflows historically store the unprefixed semver.
PIN_VERSION_RE = re.compile(r'^\s*version\s*=\s*"v?([\d.]+)"\s*$', re.MULTILINE)

# `SHIPYARD_VERSION: "0.56.2"` (with or without quotes; allow leading
# `v` for flexibility, normalize at compare time).
WORKFLOW_VERSION_RE = re.compile(
    r'^\s*SHIPYARD_VERSION:\s*"?v?([\d.]+)"?\s*$',
    re.MULTILINE,
)


def read_pinned_version() -> str:
    if not PIN_FILE.exists():
        print(f"ERROR: pin file {PIN_FILE} does not exist", file=sys.stderr)
        sys.exit(2)
    text = PIN_FILE.read_text(encoding="utf-8")
    m = PIN_VERSION_RE.search(text)
    if not m:
        print(
            f"ERROR: could not find `version = \"...\"` in {PIN_FILE}",
            file=sys.stderr,
        )
        sys.exit(2)
    return m.group(1)


def find_workflow_versions() -> list[tuple[Path, str]]:
    """Return (workflow_path, declared_version) pairs for every
    workflow that carries a `SHIPYARD_VERSION` env entry."""
    if not WORKFLOWS_DIR.exists():
        return []
    out: list[tuple[Path, str]] = []
    for wf in sorted(WORKFLOWS_DIR.glob("*.yml")):
        text = wf.read_text(encoding="utf-8")
        for m in WORKFLOW_VERSION_RE.finditer(text):
            out.append((wf, m.group(1)))
    return out


def main() -> int:
    pinned = read_pinned_version()
    workflow_versions = find_workflow_versions()

    drift: list[tuple[Path, str]] = []
    for wf, v in workflow_versions:
        if v != pinned:
            drift.append((wf, v))

    if drift:
        print(
            f"Shipyard pin drift detected — pinned={pinned} "
            f"in tools/shipyard.toml",
            file=sys.stderr,
        )
        for wf, v in drift:
            rel = wf.relative_to(REPO_ROOT)
            print(f"  ✗ {rel}: SHIPYARD_VERSION={v}", file=sys.stderr)
        print(
            "\nFix: align every workflow's `SHIPYARD_VERSION` value to the\n"
            "pin in tools/shipyard.toml, or — better — replace the inline\n"
            "value with a step that reads from tools/shipyard.toml at runtime.",
            file=sys.stderr,
        )
        return 1

    if not workflow_versions:
        print(
            "No workflows declare SHIPYARD_VERSION — pin file present but "
            "unused.",
            file=sys.stderr,
        )
        return 0

    print(
        f"Shipyard pin OK: {len(workflow_versions)} workflow(s) match "
        f"tools/shipyard.toml version {pinned}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
