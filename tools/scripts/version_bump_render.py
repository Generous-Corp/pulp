#!/usr/bin/env python3
"""Version-bump gate — render cluster.

`render_report` turns the per-surface `Verdict` list into the
human-readable report and the report-mode exit code.

`version_bump_check.py` re-exports this module's public reporting helper
so external importers and the CLI entrypoint are unaffected.
"""

from __future__ import annotations

from pathlib import Path

from version_bump_surfaces import (
    LEVELS,
    Verdict,
    already_bumped,
)


# Patchable-helper indirection. See the matching comment in
# `version_bump_heuristics.py`; `render_report` resolves `already_bumped`
# through the `version_bump_check` entrypoint so entrypoint-level patches
# keep taking effect. Deferred import avoids the entrypoint↔cluster import
# cycle.
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


# ── Reporting ───────────────────────────────────────────────────────────


def render_report(
    verdicts: list[Verdict],
    mode: str,
    base: str,
    repo: Path,
    accept_intent_trailers: bool = False,
) -> tuple[str, int]:
    lines: list[str] = []
    failures = 0
    warnings = 0
    _already_bumped = _h("already_bumped")
    for v in verdicts:
        if v.final_level == "none":
            lines.append(f"[{v.surface.name}] {v.surface.label}: no bump needed")
            continue
        # Every version file in the surface must have moved, not just one.
        # Otherwise surfaces with multiple files (plugin.json + marketplace.json)
        # could pass with only one bumped, causing split-brain versions.
        per_file = [(vf, _already_bumped(base, vf, repo)) for vf in v.surface.version_files]
        all_bumped = all(bumped for _, bumped in per_file)
        any_bumped = any(bumped for _, bumped in per_file)

        # Intent-trailer mode. When the diff has an explicit
        # `Version-Bump: <surface>=<patch|minor|major>` trailer AND
        # `--accept-intent-trailers` is on, the gate treats the trailer
        # as the bump declaration and does NOT require the version files
        # to already be moved. Merge-time automation rewrites the files
        # on merge using the next-available version from main, so two PRs
        # both declaring `sdk=minor` don't race on the exact number.
        # Defaults to OFF until Shipyard / merge automation wires it up.
        intent_declared = (
            accept_intent_trailers
            and v.trailer_override in LEVELS
            and v.trailer_override != "none"
        )

        if all_bumped:
            tag = "✓ bumped"
        elif intent_declared and not any_bumped:
            tag = f"✓ intent declared (Version-Bump: {v.surface.name}={v.trailer_override})"
        elif any_bumped:
            unbumped = [vf.path for vf, b in per_file if not b]
            tag = f"✗ partial bump — not moved: {', '.join(unbumped)}"
        elif v.final_level == "patch":
            # Advisory only — not a hard fail.
            tag = "? bump suggested (patch)"
        else:
            tag = "✗ bump required"
        lines.append(
            f"[{v.surface.name}] {v.surface.label}: "
            f"heuristic={v.heuristic}"
            f"{' override=' + v.trailer_override if v.trailer_override else ''} "
            f"final={v.final_level} "
            f"current={v.current_version or '?'} "
            f"{tag}"
        )
        if not all_bumped and not (intent_declared and not any_bumped):
            # Partial-bump is always a hard fail — split-brain versions are
            # never acceptable. Patch-suggested stays advisory only when
            # nothing has been bumped at all.
            if any_bumped:
                failures += 1
            elif v.final_level == "patch":
                warnings += 1
            else:
                failures += 1

    if mode == "hint":
        return "\n".join(lines), 0

    if failures:
        lines.append("")
        lines.append("Version-bump check FAILED.")
        lines.append("Apply the required bump with:")
        lines.append("  python3 tools/scripts/version_bump_check.py --mode=apply")
        lines.append("Or record an explicit override on the tip commit:")
        lines.append('  Version-Bump: <surface>=<patch|minor|major|skip> reason="..."')
        return "\n".join(lines), 1

    return "\n".join(lines), 0
