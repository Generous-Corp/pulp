#!/usr/bin/env python3
"""Version-bump gate — apply cluster.

Version-bump arithmetic and the `apply_bumps` writer used by
`--mode=apply` (and by `pulp pr`) to rewrite version files in place.

`version_bump_check.py` re-exports every public symbol from this module
so external importers and the CLI entrypoint are unaffected.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

from version_bump_surfaces import (
    Verdict,
    already_bumped,
    version_at_base,
    write_version,
)


# Patchable-helper indirection. See the matching comment in
# `version_bump_heuristics.py`; `apply_bumps` resolves `already_bumped` /
# `version_at_base` / `write_version` / `subprocess` through the
# `version_bump_check` entrypoint so entrypoint-level patches keep taking
# effect. Deferred import avoids the entrypoint↔cluster import cycle.
def _vbc():
    try:
        import version_bump_check as _v
        return _v
    except ImportError:  # pragma: no cover - isolated-module fallback
        return None


def _h(name: str):
    v = _vbc()
    if v is not None and hasattr(v, name):
        return getattr(v, name)
    return globals()[name]


# ── Version bumping arithmetic ──────────────────────────────────────────


def bump_version(current: str, level: str) -> str:
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", current)
    if not m:
        return current
    major, minor, patch = map(int, m.groups())
    if level == "major":
        return f"{major + 1}.0.0"
    if level == "minor":
        return f"{major}.{minor + 1}.0"
    if level == "patch":
        return f"{major}.{minor}.{patch + 1}"
    return current


def apply_bumps(
    verdicts: list[Verdict],
    base: str,
    repo: Path,
) -> list[str]:
    """Write new versions for surfaces that need a bump and aren't already bumped."""
    edited: list[str] = []
    for v in verdicts:
        # Only "none" means no release is owed. patch / minor / major all
        # need the version file written. Excluding "patch" here stranded
        # every `fix:` PR (which classifies as patch): apply mode "suggested"
        # the bump but never wrote it, so `--require-bump-for-fix-feat` then
        # hard-failed with no `chore: bump versions` marker. See
        # danielraffel/Shipyard#358. The "already at target" short-circuit
        # below keeps a repeat apply idempotent.
        if v.final_level == "none":
            continue
        # Skip if ALL version files are already at the target; otherwise
        # apply to every file (keeps plugin.json and marketplace.json in
        # lockstep after a partial-bump from a prior run).
        _already_bumped = _h("already_bumped")
        _version_at_base = _h("version_at_base")
        _write_version = _h("write_version")
        all_bumped = all(_already_bumped(base, vf, repo) for vf in v.surface.version_files)
        if all_bumped or not v.current_version:
            continue
        # Compute the target from the BASE version, not v.current_version.
        # v.current_version reflects the first readable file at HEAD, which
        # may already have been bumped by a prior partial run — bumping it
        # again would double-bump (e.g. 0.1.0 -> 0.2.0 -> 0.3.0). Reading
        # the base keeps a partial-apply idempotent.
        base_ver: str | None = None
        for vf in v.surface.version_files:
            base_ver = _version_at_base(base, vf)
            if base_ver:
                break
        source_ver = base_ver or v.current_version
        new_ver = bump_version(source_ver, v.final_level)
        for vf in v.surface.version_files:
            if _write_version(repo, vf, new_ver):
                edited.append(vf.path)
        # CHANGELOG.md is intentionally NOT written here. Ownership lives
        # with Shipyard post-tag sync via
        # `.github/workflows/post-tag-sync.yml` and the
        # `shipyard changelog regenerate` command. PR-side stub insertion
        # creates repeated multi-PR-train rebases: PR A and PR B both
        # insert `## [0.105.0]` headers, the first one merges, the second
        # one conflicts on the same line. Letting Shipyard own the full
        # regen at tag time eliminates the conflict class entirely.
        # `versioning.json` still carries each surface's `changelog` field;
        # Shipyard reads it.
    # Stage for commit so callers see them in `git status`.
    if edited:
        _subprocess = _h("subprocess")
        _subprocess.run(["git", "-C", str(repo), "add", "--"] + edited, check=False)
    return edited
