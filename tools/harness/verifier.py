#!/usr/bin/env python3
"""Catalog verifier core — pulp #1391.

Walks `compat.json`, dispatches each entry to its surface adapter, and
aggregates the results into a coverage matrix.

Outputs:
* `build/harness-coverage-<sha>.json`     machine-readable
* `build/harness-coverage.md`             human-readable, latest-only
* `docs/reports/harness-coverage.md`      committed mirror, with drift list

Usage::

    python3 tools/harness/verifier.py --surface=yoga
    python3 tools/harness/verifier.py --all
    python3 tools/harness/verifier.py --surface=yoga --json   # raw json on stdout

This script is wired through the CLI as `pulp harness coverage`.
"""

from __future__ import annotations

import argparse
import importlib
import json
import logging
import os
import pkgutil
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

# Allow `python3 tools/harness/verifier.py` from anywhere by inserting the
# parent package on sys.path.
HERE = Path(__file__).resolve().parent
REPO_ROOT_GUESS = HERE.parent.parent
if str(REPO_ROOT_GUESS) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT_GUESS))

from tools.harness.adapters import base as adapters_base  # noqa: E402
from tools.harness.adapters.base import CatalogEntry, Result  # noqa: E402
from tools.harness.status import STATUS_ORDER, Status, StatusCounts  # noqa: E402

logger = logging.getLogger("pulp.harness.verifier")

# Surfaces tracked in compat.json that we know about. Used so we can call out
# "not yet wired" cleanly when the user asks for `--all`.
KNOWN_SURFACES = ["yoga", "css", "rn", "html", "canvas2d", "imports"]


# ─────────────────────────────────────────────────────────────────────────────
# Adapter auto-discovery (pulp #1401)
#
# Each adapter module under ``tools/harness/adapters/`` is imported in
# alphabetical order. The :func:`tools.harness.adapters.base.register_adapter`
# decorator side-effect populates :data:`adapters_base.ADAPTERS`. We expose
# that dict as the module-level :data:`ADAPTERS` for backward compatibility
# with anything that imported the symbol from the old hand-rolled registry.
#
# A broken adapter module (raises on import) is logged and skipped; it does
# not crash the verifier or other surfaces. This is the property covered by
# the issue's "verifier still works when one adapter throws on import" test.
# ─────────────────────────────────────────────────────────────────────────────


def _discover_adapters(reload: bool = False) -> dict[str, type]:
    """Import every sibling module of :mod:`tools.harness.adapters` so the
    ``@register_adapter`` decorators populate :data:`adapters_base.ADAPTERS`.

    Returns the live registry dict (the same object as
    :data:`adapters_base.ADAPTERS`) for callers that want to inspect it.

    Modules whose name starts with ``_`` or that equal ``"base"`` are
    skipped; everything else is imported. Import failures are logged at
    WARNING and the offending surface is left out of the registry — they
    do not propagate to the caller.
    """
    import tools.harness.adapters as adapters_pkg

    for _, mod_name, _ in pkgutil.iter_modules(adapters_pkg.__path__):
        if mod_name.startswith("_") or mod_name == "base":
            continue
        full_name = f"{adapters_pkg.__name__}.{mod_name}"
        try:
            if reload and full_name in sys.modules:
                importlib.reload(sys.modules[full_name])
            else:
                importlib.import_module(full_name)
        except Exception as e:  # noqa: BLE001 — we genuinely want to swallow
            logger.warning(
                "harness: adapter %r failed to load and will be skipped: %s",
                mod_name,
                e,
            )
    return adapters_base.ADAPTERS


_discover_adapters()

# Surface -> Adapter class. Populated at import time by
# ``_discover_adapters``. Re-export so callers that did
# ``from verifier import ADAPTERS`` keep working.
ADAPTERS = adapters_base.ADAPTERS


# ─────────────────────────────────────────────────────────────────────────────
# Discovery
# ─────────────────────────────────────────────────────────────────────────────


def find_repo_root(start: Path) -> Path:
    """Walk up looking for the marker files we expect at the pulp root."""
    cur = start.resolve()
    for _ in range(10):
        if (cur / "compat.json").exists() and (cur / "CMakeLists.txt").exists():
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return start.resolve()


def load_compat(repo_root: Path) -> dict:
    with open(repo_root / "compat.json") as f:
        return json.load(f)


def collect_entries(compat: dict, surface: str) -> list[CatalogEntry]:
    bucket = compat.get(surface) or {}
    entries: list[CatalogEntry] = []
    for name, payload in bucket.items():
        if not isinstance(payload, dict):
            continue
        entries.append(CatalogEntry.from_compat_json(surface, name, payload))
    # stable order
    entries.sort(key=lambda e: e.name)
    return entries


# ─────────────────────────────────────────────────────────────────────────────
# Run
# ─────────────────────────────────────────────────────────────────────────────


def run_surface(repo_root: Path, surface: str) -> list[Result]:
    if surface not in ADAPTERS:
        raise ValueError(f"surface {surface!r} has no adapter wired yet")
    compat = load_compat(repo_root)
    entries = collect_entries(compat, surface)
    adapter = ADAPTERS[surface](repo_root)
    return [adapter.run(e) for e in entries]


def run_all(repo_root: Path) -> dict[str, list[Result]]:
    out: dict[str, list[Result]] = {}
    for surface in KNOWN_SURFACES:
        if surface not in ADAPTERS:
            continue
        out[surface] = run_surface(repo_root, surface)
    return out


# ─────────────────────────────────────────────────────────────────────────────
# Reporting
# ─────────────────────────────────────────────────────────────────────────────


def get_short_sha(repo_root: Path) -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=repo_root,
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip()
    except Exception:
        return "unknown"


def render_markdown(results_by_surface: dict[str, list[Result]], sha: str) -> str:
    lines: list[str] = []
    lines.append("# Harness coverage")
    lines.append("")
    lines.append(
        f"_Auto-generated by `pulp harness coverage` — sha `{sha}` — "
        f"{datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}_"
    )
    lines.append("")
    lines.append("> See `tools/harness/README.md` for status taxonomy.")
    lines.append("> Drift = harness verdict disagrees with hand-edited `compat.json` status.")
    lines.append("")

    # ── Summary across all surfaces ──────────────────────────────────
    total_counts = StatusCounts.from_results(
        [r.status for results in results_by_surface.values() for r in results]
    )
    lines.append("## Summary")
    lines.append("")
    lines.append("| Surface | Total | PASS | DIVERGE | NO-OP | NOT-IMPL | OOS | PASS % | Progress % | Drift |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for surface, results in results_by_surface.items():
        counts = StatusCounts.from_results([r.status for r in results])
        drifts = sum(1 for r in results if r.drifts)
        lines.append(
            "| `{}/` | {} | {} | {} | {} | {} | {} | {:.1f}% | {:.1f}% | {} |".format(
                surface,
                counts.total,
                counts.pass_,
                counts.diverge,
                counts.no_op,
                counts.not_impl,
                counts.oos,
                counts.pass_pct,
                counts.progress_pct,
                drifts,
            )
        )
    if len(results_by_surface) > 1:
        all_drifts = sum(
            1
            for results in results_by_surface.values()
            for r in results
            if r.drifts
        )
        lines.append(
            "| **TOTAL** | {} | {} | {} | {} | {} | {} | {:.1f}% | {:.1f}% | {} |".format(
                total_counts.total,
                total_counts.pass_,
                total_counts.diverge,
                total_counts.no_op,
                total_counts.not_impl,
                total_counts.oos,
                total_counts.pass_pct,
                total_counts.progress_pct,
                all_drifts,
            )
        )
    lines.append("")

    # ── Per-surface details ─────────────────────────────────────────
    for surface, results in results_by_surface.items():
        lines.append(f"## `{surface}/` — {len(results)} entries")
        lines.append("")
        # Bucket by status for readability.
        for st in STATUS_ORDER:
            bucket = [r for r in results if r.status is st]
            if not bucket:
                continue
            lines.append(f"### {st.value} ({len(bucket)})")
            lines.append("")
            lines.append("| Entry | Catalog | Drift | Detail |")
            lines.append("|---|---|---|---|")
            for r in bucket:
                drift_marker = "⚠" if r.drifts else ""
                detail = (r.detail or "").replace("|", "\\|")
                if len(detail) > 140:
                    detail = detail[:137] + "..."
                lines.append(
                    f"| `{r.entry.name}` | {r.entry.status or '—'} | {drift_marker} | {detail} |"
                )
            lines.append("")

        # Drift list per surface — entries where catalog disagrees.
        drifts = [r for r in results if r.drifts]
        if drifts:
            lines.append(f"### Drift list — `{surface}/` ({len(drifts)} entries)")
            lines.append("")
            lines.append("| Entry | Catalog says | Harness says | Why |")
            lines.append("|---|---|---|---|")
            for r in drifts:
                detail = (r.detail or "").replace("|", "\\|")
                if len(detail) > 120:
                    detail = detail[:117] + "..."
                lines.append(
                    f"| `{r.entry.name}` | {r.entry.status or '—'} ({r.entry.expected_status.value}) "
                    f"| {r.status.value} | {detail} |"
                )
            lines.append("")

    return "\n".join(lines)


def render_json(results_by_surface: dict[str, list[Result]], sha: str) -> dict:
    out_surfaces = {}
    for surface, results in results_by_surface.items():
        counts = StatusCounts.from_results([r.status for r in results])
        out_surfaces[surface] = {
            "total": counts.total,
            "pass": counts.pass_,
            "diverge": counts.diverge,
            "no_op": counts.no_op,
            "not_impl": counts.not_impl,
            "oos": counts.oos,
            "pass_pct": round(counts.pass_pct, 2),
            "progress_pct": round(counts.progress_pct, 2),
            "drift_count": sum(1 for r in results if r.drifts),
            "results": [r.to_dict() for r in results],
        }
    total_counts = StatusCounts.from_results(
        [r.status for results in results_by_surface.values() for r in results]
    )
    return {
        "schema_version": "0.1",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "sha": sha,
        "totals": {
            "total": total_counts.total,
            "pass": total_counts.pass_,
            "diverge": total_counts.diverge,
            "no_op": total_counts.no_op,
            "not_impl": total_counts.not_impl,
            "oos": total_counts.oos,
            "pass_pct": round(total_counts.pass_pct, 2),
            "progress_pct": round(total_counts.progress_pct, 2),
            "drift_count": sum(
                1
                for results in results_by_surface.values()
                for r in results
                if r.drifts
            ),
        },
        "surfaces": out_surfaces,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Output orchestration
# ─────────────────────────────────────────────────────────────────────────────


def write_outputs(
    repo_root: Path,
    results_by_surface: dict[str, list[Result]],
    sha: str,
    write_docs: bool,
) -> dict[str, Path]:
    build_dir = repo_root / "build"
    build_dir.mkdir(exist_ok=True)

    json_path = build_dir / f"harness-coverage-{sha}.json"
    md_path = build_dir / "harness-coverage.md"

    json_payload = render_json(results_by_surface, sha)
    md_payload = render_markdown(results_by_surface, sha)

    json_path.write_text(json.dumps(json_payload, indent=2))
    md_path.write_text(md_payload)

    written = {"json": json_path, "md": md_path}

    if write_docs:
        docs_dir = repo_root / "docs" / "reports"
        docs_dir.mkdir(parents=True, exist_ok=True)
        docs_path = docs_dir / "harness-coverage.md"
        docs_path.write_text(md_payload)
        written["docs"] = docs_path

    return written


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="pulp harness coverage",
        description="Run the catalog harness and produce a coverage report.",
    )
    p.add_argument(
        "--surface",
        action="append",
        default=None,
        help="Run a single surface (e.g. yoga). May be repeated.",
    )
    p.add_argument(
        "--all",
        action="store_true",
        help="Run every wired surface adapter.",
    )
    p.add_argument(
        "--json",
        action="store_true",
        help="Print the JSON report to stdout instead of writing files.",
    )
    p.add_argument(
        "--no-docs",
        action="store_true",
        help="Skip writing the docs/reports/harness-coverage.md mirror.",
    )
    p.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="Override the repo root (default: walks up from cwd looking for compat.json).",
    )
    return p.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> int:
    # Trim a leading "coverage" subcommand if invoked via `pulp harness coverage`.
    if argv is None:
        argv = sys.argv[1:]
    if argv and argv[0] == "coverage":
        argv = argv[1:]

    args = parse_args(argv)

    repo_root = args.repo_root or find_repo_root(Path.cwd())
    if not (repo_root / "compat.json").exists():
        print(f"error: compat.json not found under {repo_root}", file=sys.stderr)
        return 2

    if args.all and args.surface:
        print("error: pass --all OR --surface, not both", file=sys.stderr)
        return 2

    if args.all:
        surfaces = [s for s in KNOWN_SURFACES if s in ADAPTERS]
    elif args.surface:
        surfaces = list(args.surface)
    else:
        # Default: yoga only this week — same as `--surface=yoga`.
        surfaces = ["yoga"]

    for s in surfaces:
        if s not in ADAPTERS:
            print(
                f"error: surface {s!r} has no adapter wired yet (known: {sorted(ADAPTERS)})",
                file=sys.stderr,
            )
            return 2

    results_by_surface: dict[str, list[Result]] = {}
    for s in surfaces:
        results_by_surface[s] = run_surface(repo_root, s)

    sha = get_short_sha(repo_root)

    if args.json:
        print(json.dumps(render_json(results_by_surface, sha), indent=2))
        return 0

    written = write_outputs(
        repo_root,
        results_by_surface,
        sha,
        write_docs=not args.no_docs,
    )

    # Print a tight stdout summary so CI logs are useful without scrolling.
    total_counts = StatusCounts.from_results(
        [r.status for results in results_by_surface.values() for r in results]
    )
    print(f"# pulp harness coverage @ {sha}")
    print(f"")
    print(f"{'surface':<10} {'total':>6} {'PASS':>6} {'DIVERGE':>8} {'NO-OP':>6} "
          f"{'NOT-IMPL':>9} {'OOS':>5} {'PASS%':>7} {'drift':>6}")
    for surface, results in results_by_surface.items():
        c = StatusCounts.from_results([r.status for r in results])
        drifts = sum(1 for r in results if r.drifts)
        print(
            f"{surface:<10} {c.total:>6} {c.pass_:>6} {c.diverge:>8} "
            f"{c.no_op:>6} {c.not_impl:>9} {c.oos:>5} {c.pass_pct:>6.1f}% {drifts:>6}"
        )
    if len(results_by_surface) > 1:
        c = total_counts
        all_drifts = sum(
            1
            for results in results_by_surface.values()
            for r in results
            if r.drifts
        )
        print(
            f"{'TOTAL':<10} {c.total:>6} {c.pass_:>6} {c.diverge:>8} "
            f"{c.no_op:>6} {c.not_impl:>9} {c.oos:>5} {c.pass_pct:>6.1f}% {all_drifts:>6}"
        )
    print()
    for label, path in written.items():
        try:
            rel = path.relative_to(repo_root)
        except ValueError:
            rel = path
        print(f"wrote {label}: {rel}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
