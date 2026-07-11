#!/usr/bin/env python3
"""Screenshot-sync gate.

Keeps a plugin repo's screenshots in lockstep with its UX. A repo opts in by
carrying a `.pulp/screenshots.toml` manifest at its root (presence == opt-in,
the same pattern `~/.config/pulp/daw-smoke.toml` and `.shipyard.local/config.toml`
use). No manifest → this gate is a silent no-op for that repo.

Given a diff range (base..head), the gate:

  1. Reads the manifest's ``[trigger].paths`` globs (repo-relative, gitignore
     style — the same glob dialect the skill-sync / version-bump gates use).
  2. Diffs the change range against them. If a UX-path file changed, every
     ``[[target]]`` in the manifest is considered a candidate for a re-shoot
     (a design-token change reskins every widget, so a trigger hit invalidates
     all targets — unless a target narrows its own trigger via a per-target
     ``trigger`` list).
  3. For each triggered target, checks that at least one of the PNGs it
     ``consumes`` (kinds ``readme`` / ``gallery`` / ``og``) was ALSO updated in
     the same diff — or that a bypass trailer authorizes skipping it.

It mirrors ``skill_sync_check.py`` exactly (same three-layer hint→report→CI
shape, same ``gate_common`` substrate) so it is familiar and shares the bypass
grammar. It does NOT capture screenshots — capturing is heavy and
non-deterministic across machines; the gate only DETECTS staleness. The re-shoot
is an explicit step the developer/agent runs (see the ``screenshot-sync`` skill),
then commits the PNGs. This is the same detect-here / apply-elsewhere split the
version-bump gate uses.

Bypass (tip commit trailer, git audit trail — same convention as the other
gates):

    Screenshot-Sync: skip target=<id|all> reason="..."

Uses stdlib ``tomllib`` (Python 3.11+). On an older interpreter that lacks it,
the gate degrades to a clean no-op with a note — CI runs 3.12 and is the
authoritative layer.
"""

from __future__ import annotations

import argparse
import subprocess  # noqa: F401  — kept so tests can patch this module's subprocess; gate_common shares the singleton.
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    tomllib = None  # type: ignore[assignment]

# Shared gate helpers — single-file invocation puts this dir on sys.path.
from gate_common import (
    repo_root,
    git_diff_names,
    git_range_trailers,
    matches_any as _matches_any,
)

MANIFEST_REL = Path(".pulp/screenshots.toml")
TRAILER_KEY = "Screenshot-Sync"

# consume kinds that name a committed PNG whose freshness we can verify.
PNG_KINDS = ("readme", "gallery", "og")


# ── Types ───────────────────────────────────────────────────────────────


@dataclass
class Target:
    id: str
    route: str
    trigger: list[str]          # per-target override; empty → use global trigger
    png_paths: list[str]        # repo-relative PNGs this target consumes


@dataclass
class Manifest:
    trigger_paths: list[str]
    targets: list[Target]


@dataclass
class Finding:
    target: str
    triggered_by: list[str]
    png_paths: list[str]
    png_refreshed: bool
    bypass_reason: str | None


# ── Manifest loading ──────────────────────────────────────────────────────


def load_manifest(path: Path) -> Manifest:
    with path.open("rb") as fh:
        data = tomllib.load(fh)

    trig = data.get("trigger", {}) or {}
    trigger_paths = list(trig.get("paths", []) or [])

    targets: list[Target] = []
    for i, raw in enumerate(data.get("target", []) or []):
        tid = raw.get("id")
        if not tid:
            raise ValueError(f"[[target]] #{i} is missing a required `id`")
        route = raw.get("route", "")
        if route not in ("native", "web"):
            raise ValueError(
                f"target '{tid}': route must be 'native' or 'web' (got {route!r})"
            )
        png_paths: list[str] = []
        for c in raw.get("consumes", []) or []:
            if c.get("kind") in PNG_KINDS and c.get("path"):
                png_paths.append(c["path"])
        targets.append(
            Target(
                id=tid,
                route=route,
                trigger=list(raw.get("trigger", []) or []),
                png_paths=png_paths,
            )
        )
    return Manifest(trigger_paths=trigger_paths, targets=targets)


# ── Bypass trailer ────────────────────────────────────────────────────────


def parse_bypasses(trailers: dict[str, list[str]]) -> dict[str, str]:
    """Return {target_id_or_'all': reason} from Screenshot-Sync: skip trailers."""
    import re

    out: dict[str, str] = {}
    for v in trailers.get(TRAILER_KEY.lower(), []):
        if not v.lower().startswith("skip"):
            continue
        m = re.search(r"target\s*=\s*([A-Za-z0-9_.*-]+)", v)
        if not m:
            continue
        target = m.group(1)
        rm = (
            re.search(r'reason\s*=\s*"([^"]*)"', v)
            or re.search(r"reason\s*=\s*(\S+)", v)
        )
        out[target] = rm.group(1) if rm else "(no reason given)"
    return out


# ── Core check ────────────────────────────────────────────────────────────


def compute_findings(
    changed: list[str],
    manifest: Manifest,
    bypasses: dict[str, str],
) -> list[Finding]:
    changed_set = set(changed)
    findings: list[Finding] = []
    for tgt in manifest.targets:
        patterns = tgt.trigger or manifest.trigger_paths
        triggered_by = [p for p in changed if _matches_any(p, patterns)]
        if not triggered_by:
            continue
        png_refreshed = any(p in changed_set for p in tgt.png_paths)
        reason = bypasses.get(tgt.id, bypasses.get("all"))
        findings.append(
            Finding(
                target=tgt.id,
                triggered_by=sorted(triggered_by),
                png_paths=list(tgt.png_paths),
                png_refreshed=png_refreshed,
                bypass_reason=reason,
            )
        )
    return findings


# ── Reporting ─────────────────────────────────────────────────────────────


def render_report(findings: list[Finding], mode: str) -> tuple[str, int]:
    lines: list[str] = []
    hard: list[Finding] = []

    for f in findings:
        if f.png_refreshed:
            status = "✓ screenshot refreshed"
        elif f.bypass_reason is not None:
            status = f"✓ bypassed ({f.bypass_reason})"
        else:
            status = "✗ screenshot NOT refreshed"
            hard.append(f)
        lines.append(f"[{f.target}] {status}")
        if mode != "report" or not f.png_refreshed:
            for tp in f.triggered_by[:6]:
                lines.append(f"    triggered by: {tp}")
            if len(f.triggered_by) > 6:
                lines.append(f"    … {len(f.triggered_by) - 6} more")
            for pp in f.png_paths:
                lines.append(f"    expects: {pp}")

    if hard and mode in ("report", "apply"):
        lines.append("")
        lines.append("Screenshot-sync check FAILED.")
        lines.append("For each target above marked ✗, either:")
        lines.append("  1. Re-capture and commit its screenshot(s) in this branch")
        lines.append("     (see the `screenshot-sync` skill), OR")
        lines.append("  2. Add a commit trailer on the tip commit with the exact form:")
        for f in hard:
            lines.append(
                f'     {TRAILER_KEY}: skip target={f.target} reason="..."'
            )
        return "\n".join(lines), 1

    if not findings:
        lines.append(
            "screenshot-sync: no UX-path (trigger) files touched — nothing to verify."
        )
    return "\n".join(lines), 0


# ── Main ──────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Screenshot-sync gate")
    parser.add_argument("--base", default="origin/main", help="Diff base (default: origin/main)")
    parser.add_argument("--head", default="HEAD", help="Diff head (default: HEAD)")
    parser.add_argument(
        "--manifest",
        default=None,
        help="Path to .pulp/screenshots.toml (default: <repo-root>/.pulp/screenshots.toml)",
    )
    parser.add_argument(
        "--mode",
        choices=("report", "hint", "apply"),
        default="report",
        help="report: fail on stale shots (CI); hint: advisory text only; "
             "apply: same as report (screenshots cannot be auto-generated here)",
    )
    parser.add_argument("--repo-root", default=None, help="Override repo root")
    args = parser.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else repo_root()
    manifest_path = Path(args.manifest) if args.manifest else root / MANIFEST_REL

    if not manifest_path.exists():
        # Not opted in — silent no-op (this is the common case in pulp core and
        # any library/tool repo).
        if args.mode == "hint":
            return 0
        print("screenshot-sync: no .pulp/screenshots.toml — repo not opted in; skipping.")
        return 0

    if tomllib is None:
        msg = (
            "screenshot-sync: tomllib unavailable (Python < 3.11) — skipping. "
            "CI (Python 3.12) is the authoritative layer."
        )
        print(msg, file=sys.stderr)
        return 0

    try:
        manifest = load_manifest(manifest_path)
    except (ValueError, KeyError) as exc:
        print(f"screenshot-sync: invalid manifest {manifest_path}: {exc}", file=sys.stderr)
        return 2

    changed = git_diff_names(args.base, args.head)
    trailers = git_range_trailers(args.base, args.head)
    bypasses = parse_bypasses(trailers)

    findings = compute_findings(changed, manifest, bypasses)
    text, code = render_report(findings, args.mode)
    if text:
        print(text)

    if args.mode == "hint":
        return 0
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
