#!/usr/bin/env python3
"""Single-source-of-truth check for the Shipyard pin.

`tools/shipyard.toml` is the canonical pin used by
`tools/install-shipyard.sh` and `shipyard pr`. The post-tag changelog
sync workflow also bakes the version into a `SHIPYARD_VERSION` env
value. Prior drift between those locations caused local and tag-time
Shipyard behavior to diverge.

This script makes the drift mechanically impossible to ship by
validating that every workflow's `SHIPYARD_VERSION` matches the pin in
`tools/shipyard.toml`. Wire it into a workflow lint test or run it
locally before pushing release-pipeline changes.

Exits:
    0 — every required workflow declares a matching `SHIPYARD_VERSION`.
    1 — drift detected (mismatch OR a required workflow stopped
        declaring `SHIPYARD_VERSION` entirely — vacuous-pass case).
    2 — configuration error (file missing, unparseable, etc).

The vacuous-pass case is gated by `REQUIRED_PIN_WORKFLOWS` — the
explicit set of workflow filenames that MUST carry the pin. Regression
coverage ensures a silent removal of the env entry would otherwise let
the script exit 0 even though the pin was no longer enforced anywhere. If
a workflow replaces the inline env with a step that reads
`tools/shipyard.toml` at runtime, update `REQUIRED_PIN_WORKFLOWS` in the
same diff with a comment explaining why.

Pure stdlib; no third-party deps.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PIN_FILE = REPO_ROOT / "tools" / "shipyard.toml"
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

# Workflows that MUST carry a `SHIPYARD_VERSION` env entry. The check
# fails if any of these workflows exists but no longer declares the pin.
# A legitimate switch to a runtime-read shape should update this tuple in
# the same diff and document why.
REQUIRED_PIN_WORKFLOWS: tuple[str, ...] = (
    "post-tag-sync.yml",
)

SHIPYARD_CONFIG = REPO_ROOT / ".shipyard" / "config.toml"

# Minimum Shipyard version required by a config key we actually USE.
# Equality between shipyard.toml and the workflows is not sufficient: both
# can agree on a version that is too old for a feature the config enables,
# and the failure is silent. `push_mode = "pr"` is honoured from 0.78.0, but
# until 0.79.0 the changelog commit was still stamped `[skip ci]` — which
# makes Actions skip the required checks, which makes the changelog PR
# permanently unmergeable. Nine stacked up and stalled the release pipeline.
#
# Each entry: (regex matching the enabling config, floor, why it matters).
FEATURE_FLOORS: tuple[tuple[str, str, str], ...] = (
    (
        r'^\s*push_mode\s*=\s*"pr"\s*$',
        "0.79.0",
        'post_tag_hook push_mode = "pr" — below 0.79.0 the changelog commit '
        "is stamped [skip ci], so its required checks never run and the PR "
        "can never merge",
    ),
    (
        r'^\s*ssh_signing_setup_script\s*=\s*"[^"]+"\s*$',
        "0.81.4",
        "post_tag_hook ssh_signing_setup_script — older Shipyard releases "
        "ignore the key and regenerate a workflow without signed bot setup",
    ),
)


def _as_tuple(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.split(".") if part.isdigit())


def check_feature_floors(pinned: str) -> list[str]:
    """Violations where the pin is below a floor the config demands."""
    if not SHIPYARD_CONFIG.exists():
        return []
    text = SHIPYARD_CONFIG.read_text(errors="replace")
    problems: list[str] = []
    for pattern, floor, why in FEATURE_FLOORS:
        if not re.search(pattern, text, re.MULTILINE):
            continue
        if _as_tuple(pinned) < _as_tuple(floor):
            problems.append(
                f"pin {pinned} is below the {floor} floor required by "
                f".shipyard/config.toml: {why}"
            )
    return problems

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

    # Required workflows that exist but no longer declare
    # `SHIPYARD_VERSION` would otherwise let the gate pass while the pin
    # stopped being enforced.
    declared = {wf.name for wf, _ in workflow_versions}
    missing_required: list[str] = []
    for name in REQUIRED_PIN_WORKFLOWS:
        wf_path = WORKFLOWS_DIR / name
        if wf_path.exists() and name not in declared:
            missing_required.append(name)

    drift: list[tuple[Path, str]] = []
    for wf, v in workflow_versions:
        if v != pinned:
            drift.append((wf, v))

    if missing_required or drift:
        print(
            f"Shipyard pin drift detected — pinned={pinned} "
            f"in tools/shipyard.toml",
            file=sys.stderr,
        )
        for name in missing_required:
            print(
                f"  ✗ .github/workflows/{name}: no SHIPYARD_VERSION declared "
                f"(REQUIRED — see REQUIRED_PIN_WORKFLOWS in this script)",
                file=sys.stderr,
            )
        for wf, v in drift:
            rel = wf.relative_to(REPO_ROOT)
            print(f"  ✗ {rel}: SHIPYARD_VERSION={v}", file=sys.stderr)
        print(
            "\nFix: align every workflow's `SHIPYARD_VERSION` value to the\n"
            "pin in tools/shipyard.toml. If a workflow legitimately no\n"
            "longer needs an inline pin (e.g. it now reads tools/shipyard.toml\n"
            "at runtime), remove it from REQUIRED_PIN_WORKFLOWS in this\n"
            "script in the same diff with a comment explaining why.",
            file=sys.stderr,
        )
        return 1

    if not workflow_versions:
        # Pin file exists but the whole repo has zero pin declarations
        # AND REQUIRED_PIN_WORKFLOWS is empty (or none of those files
        # exist). This is a configured state — emit but pass.
        print(
            "No workflows declare SHIPYARD_VERSION — pin file present but "
            "unused.",
            file=sys.stderr,
        )
        return 0

    floor_problems = check_feature_floors(pinned)
    if floor_problems:
        print(
            "Shipyard pin is version-consistent but TOO OLD for the config:",
            file=sys.stderr,
        )
        for problem in floor_problems:
            print(f"  ✗ {problem}", file=sys.stderr)
        print(
            "\nFix: raise the pin in tools/shipyard.toml (and every workflow's\n"
            "SHIPYARD_VERSION, which this check also enforces), or stop using the\n"
            "config key that demands the floor.",
            file=sys.stderr,
        )
        return 1

    print(
        f"Shipyard pin OK: {len(workflow_versions)} workflow(s) match "
        f"tools/shipyard.toml version {pinned} "
        f"(required: {', '.join(REQUIRED_PIN_WORKFLOWS)})."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
