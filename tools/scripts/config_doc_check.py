#!/usr/bin/env python3
"""Config→doc drift gate.

Given a diff range (base..head), check that every map entry whose config
`paths` were touched also had at least one of its `docs` touched in the same
range — or has a `Config-Doc: skip reason="..."` trailer on any commit in the
range.

The motivation mirrors skill_sync_check.py: a handful of config surfaces
(Shipyard validation config, the Shipyard binary pin/installer, the build +
release + gate workflows) each have a human-facing guide whose description
silently goes stale when the config changes without a doc edit. This gate makes
that drift a fast, local, pre-push failure instead of an after-the-fact
discovery.

Declarative map: tools/scripts/config_doc_map.json.
Shares git/glob helpers with the other gates via gate_common.

Uses JSON (not YAML) for zero-dependency execution on PEP-668 Python.

    python3 tools/scripts/config_doc_check.py --mode=report   # CI / pre-push (exit 1 on drift)
    python3 tools/scripts/config_doc_check.py --mode=hint      # advisory (always exit 0)
    python3 tools/scripts/config_doc_check.py --base main      # custom diff base
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess  # noqa: F401  — kept so tests can patch config_doc_check.subprocess and reach gate_common's calls (modules are singletons in sys.modules).
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Shared gate helpers. Single-file script invocation puts this directory on
# sys.path, so a plain import works from CI, hooks, and ad-hoc runs.
from gate_common import (
    repo_root,
    git_diff_names,
    git_range_trailers,
    matches_any as _matches_any,
    strip_meta as _strip_meta,
)


# ── Types ───────────────────────────────────────────────────────────────


@dataclass
class Entry:
    paths: list[str]
    docs: list[str]
    why: str


@dataclass
class ConfigDocMap:
    entries: list[Entry] = field(default_factory=list)
    trailer_config_doc: str = "Config-Doc"


@dataclass
class Finding:
    touched_paths: list[str]
    docs: list[str]
    doc_modified: bool
    why: str


# ── Map loading ─────────────────────────────────────────────────────────


def load_map(path: Path) -> ConfigDocMap:
    data = _strip_meta(json.loads(path.read_text()))
    trailers = data.get("trailers", {}) or {}
    raw_entries = data.get("entries", []) or []
    entries: list[Entry] = []
    for e in raw_entries:
        entries.append(
            Entry(
                paths=list(e.get("paths", []) or []),
                docs=list(e.get("docs", []) or []),
                why=str(e.get("why", "")).strip(),
            )
        )
    return ConfigDocMap(
        entries=entries,
        trailer_config_doc=trailers.get("config_doc", "Config-Doc"),
    )


def parse_config_doc_trailer(trailers: dict[str, list[str]], key: str) -> str | None:
    """Return a bypass reason if any `Config-Doc: skip ...` trailer is present.

    The gate is a single global skip (no per-entry key), so the first `skip`
    trailer disables the whole check for this range — same spirit as the
    Planning-Bump / Release skip trailers.
    """
    for v in trailers.get(key.lower(), []):
        if not v.lower().startswith("skip"):
            continue
        reason_m = (
            re.search(r'reason\s*=\s*"([^"]*)"', v)
            or re.search(r"reason\s*=\s*(\S+)", v)
        )
        return reason_m.group(1) if reason_m else "(no reason given)"
    return None


# ── Core check ──────────────────────────────────────────────────────────


def compute_findings(changed: list[str], cfg_map: ConfigDocMap) -> list[Finding]:
    findings: list[Finding] = []
    for entry in cfg_map.entries:
        touched = [p for p in changed if _matches_any(p, entry.paths)]
        if not touched:
            continue
        doc_modified = any(_matches_any(p, entry.docs) for p in changed)
        findings.append(
            Finding(
                touched_paths=sorted(touched),
                docs=list(entry.docs),
                doc_modified=doc_modified,
                why=entry.why,
            )
        )
    return findings


# ── Reporting ───────────────────────────────────────────────────────────


def render_report(
    findings: list[Finding],
    trailer_key: str,
    mode: str,
    bypass_reason: str | None,
) -> tuple[str, int]:
    lines: list[str] = []
    hard_failures: list[Finding] = []

    for f in findings:
        if f.doc_modified:
            status = "✓ doc updated"
        elif bypass_reason is not None:
            status = f"✓ bypassed ({bypass_reason})"
        else:
            status = "✗ doc NOT updated"
            hard_failures.append(f)
        lines.append(f"[config-doc] {status}")
        if mode != "report" or not f.doc_modified:
            for tp in f.touched_paths[:8]:
                lines.append(f"    changed: {tp}")
            if len(f.touched_paths) > 8:
                lines.append(f"    … {len(f.touched_paths) - 8} more")
            lines.append(f"    expected a change in one of: {', '.join(f.docs)}")
            if f.why:
                lines.append(f"    why: {f.why}")

    if hard_failures and mode in ("report", "apply"):
        lines.append("")
        lines.append("Config-doc check FAILED.")
        lines.append("For each entry above marked ✗, either:")
        lines.append("  1. Update the relevant guide doc in this branch so it stays in sync, OR")
        lines.append("  2. Add a commit trailer on any commit in this branch with the form:")
        lines.append(f'     {trailer_key}: skip reason="..."')
        return "\n".join(lines), 1

    if not findings:
        lines.append("config-doc: no mapped config paths touched — nothing to verify.")
    return "\n".join(lines), 0


# ── Main ────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Config→doc drift gate")
    parser.add_argument("--base", default="origin/main",
                        help="Diff base (default: origin/main)")
    parser.add_argument("--head", default="HEAD",
                        help="Diff head (default: HEAD)")
    parser.add_argument(
        "--map",
        default=None,
        help="Path to config_doc_map.json (default: tools/scripts/config_doc_map.json at repo root)",
    )
    parser.add_argument(
        "--mode",
        choices=("report", "hint", "apply"),
        default="report",
        help="report: fail on drift (CI); hint: advisory text only; apply: same as report (docs cannot be auto-generated)",
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="Override repo root (default: git rev-parse --show-toplevel)",
    )
    args = parser.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else repo_root()
    map_path = (
        Path(args.map) if args.map
        else root / "tools" / "scripts" / "config_doc_map.json"
    )
    if not map_path.exists():
        sys.stderr.write(f"config_doc_check: map not found: {map_path}\n")
        return 2

    cfg_map = load_map(map_path)

    changed = git_diff_names(args.base, args.head)

    trailers = git_range_trailers(args.base, args.head)
    bypass_reason = parse_config_doc_trailer(trailers, cfg_map.trailer_config_doc)

    findings = compute_findings(changed, cfg_map)

    text, code = render_report(findings, cfg_map.trailer_config_doc, args.mode, bypass_reason)
    if text:
        print(text)

    if args.mode == "hint":
        return 0
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
