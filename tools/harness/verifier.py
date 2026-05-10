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
import re
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
from tools.harness.visual import runner as visual_runner  # noqa: E402

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
# Evidence check (#1657 control #1) — requires runtime tests for supported claims
# ─────────────────────────────────────────────────────────────────────────────


def _get_grace_until(compat: dict) -> Optional[str]:
    audit = compat.get("_audit") or {}
    return audit.get("_evidence_grace_until")  # e.g. "2026-05-22"


def _grace_active(grace_until: Optional[str]) -> bool:
    if not grace_until:
        return False
    from datetime import date

    try:
        deadline = date.fromisoformat(grace_until)
        return date.today() <= deadline
    except (ValueError, TypeError):
        return False


# ── pulp #1737 — test-tag validation cache ─────────────────────────────────
# Reading + scanning a test file is cheap, but we do it once per file across
# all catalog entries that reference it. Cache the extracted TEST_CASE tag
# sets + name sets so the verifier doesn't re-parse the same file 50× during
# a full run.
_TEST_CASE_INDEX_CACHE: dict[Path, tuple[set[str], set[str]]] = {}

# Matches `TEST_CASE("test name", "[tag1][tag2]")`. Tolerates Catch2's
# multi-line literal-concat form (`"part1 " "part2"` inside the first arg)
# by matching the SECOND quoted string in the call as the tag block — the
# fallback regex below handles concatenated names by capturing the full
# arg list and reparsing.
_TEST_CASE_FULL_PATTERN = re.compile(
    r'TEST_CASE\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\s*,\s*"([^"]*)"',
    re.MULTILINE | re.DOTALL,
)
# Catch2 also allows the single-arg `TEST_CASE("name")` form (no tags).
_TEST_CASE_NAME_ONLY_PATTERN = re.compile(
    r'TEST_CASE\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\s*\)',
    re.MULTILINE | re.DOTALL,
)


def _join_concat_string_literals(raw: str) -> str:
    """Catch2's TEST_CASE name argument is often broken across multiple
    "literal " "literal" string concatenations. Re-join into one string."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', raw)
    return "".join(parts)


def _extract_test_case_index(file_path: Path) -> tuple[set[str], set[str]]:
    """Return (tags, names) — the union of all ``[tag]`` substrings AND the
    set of TEST_CASE names declared in the file. Empty sets if the file
    doesn't exist or doesn't define any TEST_CASEs."""
    if file_path in _TEST_CASE_INDEX_CACHE:
        return _TEST_CASE_INDEX_CACHE[file_path]
    tags: set[str] = set()
    names: set[str] = set()
    if file_path.exists():
        try:
            text = file_path.read_text(errors="replace")
        except OSError:
            text = ""
        for name_raw, tag_string in _TEST_CASE_FULL_PATTERN.findall(text):
            names.add(_join_concat_string_literals(name_raw))
            for tok in re.findall(r"\[([^\]]+)\]", tag_string):
                tags.add(tok.strip())
        # Single-arg TEST_CASE(name) — names only.
        for name_raw in _TEST_CASE_NAME_ONLY_PATTERN.findall(text):
            joined = _join_concat_string_literals(name_raw)
            if joined:
                names.add(joined)
    _TEST_CASE_INDEX_CACHE[file_path] = (tags, names)
    return tags, names


def _extract_test_case_tags(file_path: Path) -> set[str]:
    """Backward-compat wrapper — returns just the tag set."""
    return _extract_test_case_index(file_path)[0]


# Typed test-reference prefixes per the compat.json convention (see the
# top-level _comment field). These routes name validation in other
# subsystems (visual baselines, semantic fixtures, design fixtures) and
# don't have a TEST_CASE-tag contract, so we accept them on file-existence
# alone (or always, for `cannot-validate:`).
_TYPED_REF_PREFIXES = (
    "unit:", "semantic:", "visual:", "dom:", "behavior:", "cannot-validate:",
)


def _validate_test_ref(repo_root: Path, ref: str) -> tuple[bool, str]:
    """Parse a catalog test reference (e.g. `test/foo.cpp [issue-1737][tag2]`)
    and verify (a) the file exists, AND (b) every listed [tag] appears in
    some TEST_CASE in that file. Returns (ok, reason). When ok is True,
    reason is empty; when False, reason explains the gap.

    Typed prefixes (`unit:` / `semantic:` / `visual:` / `dom:` / `behavior:`
    / `cannot-validate:`) are accepted as-is — they name fixtures in other
    subsystems with their own validation contracts; we don't grep TEST_CASE
    tags out of them. `cannot-validate:` is always accepted (it documents
    why the entry is intentionally unverifiable).
    """
    stripped = ref.strip()
    # Typed prefixes — pulp #1737 followup (Codex P2 on #1768): the
    # part-after-prefix is validated when it looks like a file path
    # (contains `/`) so typo'd / stale typed refs don't auto-pass.
    # `cannot-validate:` always passes — it documents intentional
    # un-verifiability. The other prefixes try a file-existence check;
    # if the body doesn't look like a path, fall back to accept-on-prefix
    # (e.g. `semantic:yoga/aspect-ratio` is a fixture id, not a path).
    for prefix in _TYPED_REF_PREFIXES:
        if stripped.startswith(prefix):
            if prefix == "cannot-validate:":
                return True, ""
            body = stripped[len(prefix):].strip()
            # Heuristic: bodies with a `/` AND a recognised test-source
            # extension are paths (test/foo.cpp, packages/x/foo.test.ts).
            # Anything else is a fixture id we can't validate from here.
            looks_like_path = "/" in body and any(
                body.endswith(ext) for ext in (
                    ".cpp", ".hpp", ".h", ".cc", ".mm",
                    ".test.ts", ".test.tsx", ".spec.ts", ".spec.tsx",
                    ".test.js", ".spec.js",
                )
            )
            if looks_like_path:
                if not (repo_root / body).exists():
                    return False, (
                        f"typed ref `{prefix}{body}` points at `{body}` "
                        "which does not exist in the repo"
                    )
            return True, ""
    # Prefer the bracket-tag form when `[` appears before `::`. This
    # avoids tripping on `::` that appears inside free-form description
    # text after the bracket tags (e.g. `path [tag] (View::method...)`).
    bracket_first = stripped.find("[")
    name_split = stripped.find('::')
    if name_split > 0 and (bracket_first < 0 or name_split < bracket_first):
        path_part = stripped[:name_split].strip()
        name_part = stripped[name_split + 2:].strip()
        # Strip optional surrounding quotes.
        if (name_part.startswith('"') and name_part.endswith('"')
                and len(name_part) >= 2):
            name_part = name_part[1:-1]
        # Non-.cpp files (e.g. .ts jest tests) — file-existence check
        # only; we don't grep for TEST_CASE in .ts files.
        if not path_part.endswith('.cpp'):
            if not (repo_root / path_part).exists():
                return False, f"file `{path_part}` not found"
            return True, ""
        if not (repo_root / path_part).exists():
            return False, f"file `{path_part}` not found"
        _, file_names = _extract_test_case_index(repo_root / path_part)
        if name_part not in file_names:
            return False, (
                f"TEST_CASE name {name_part!r} not found in `{path_part}` — "
                "either the test was renamed / deleted or the catalog "
                "reference is stale"
            )
        return True, ""
    # `path [tag1][tag2]` form — match against TEST_CASE second-arg tags.
    bracket = stripped.find("[")
    if bracket < 0:
        # No tags — file-existence check is the contract.
        if not stripped:
            return False, "empty test reference"
        if not (repo_root / stripped).exists():
            return False, f"file `{stripped}` not found"
        return True, ""
    path_part = stripped[:bracket].strip()
    tag_part = stripped[bracket:]
    if not path_part or not (repo_root / path_part).exists():
        return False, f"file `{path_part}` not found"
    listed_tags = re.findall(r"\[([^\]]+)\]", tag_part)
    if not listed_tags:
        return True, ""
    file_tags, _ = _extract_test_case_index(repo_root / path_part)
    if not file_tags:
        return False, (
            f"file `{path_part}` exists but no TEST_CASE was found — "
            "either the file is empty or the tag is wrong"
        )
    missing = [t for t in listed_tags if t.strip() not in file_tags]
    if missing:
        return False, (
            f"tag(s) {missing} not found in any TEST_CASE in `{path_part}` — "
            "either the test was renamed/deleted or the catalog reference "
            "is stale"
        )
    return True, ""


def check_evidence(repo_root: Path, results: list[Result], compat: dict) -> list[Result]:
    """Post-process PASS results: demote to SUPPORTED_NO_EVIDENCE when the
    catalog entry claims ``supported`` but has no ``tests`` paths that exist in
    the repo. During a grace period the result stays PASS with a warning;
    after the grace period it becomes SUPPORTED_NO_EVIDENCE.

    pulp #1737 — also verifies that any ``[tag]`` substrings inside each test
    reference actually appear in some ``TEST_CASE`` declaration in the named
    file. Without this, a renamed / deleted TEST_CASE leaves a dangling
    reference in the catalog (the em-dash glitch fixed in PR #1752 was an
    instance of exactly this — file existed, tag didn't match because the
    em-dash made the path string unparseable). Tag-validation gaps are
    treated the same as file-existence gaps for grace-period demotion.
    """
    grace_until = _get_grace_until(compat)
    in_grace = _grace_active(grace_until)
    updated: list[Result] = []
    for r in results:
        if r.status is not Status.PASS:
            updated.append(r)
            continue
        if r.entry.status != "supported":
            updated.append(r)
            continue
        # Check for evidence: at least one test reference must (a) point at a
        # file that exists AND (b) name [tag]s that appear as TEST_CASE
        # declarations in that file. pulp #1737 — the second check is new;
        # without it a renamed/deleted TEST_CASE leaves a dangling catalog
        # reference that the verifier didn't notice (PR #1752 hand-fixed
        # 6 such cases that the file-existence-only check let through).
        valid_tests: list[str] = []
        invalid_reasons: list[str] = []
        for t in r.entry.tests:
            ok, reason = _validate_test_ref(repo_root, t)
            if ok:
                valid_tests.append(t)
            else:
                invalid_reasons.append(f"  - {t!r}: {reason}")
        if valid_tests:
            # Even when SOME tests are valid, surface dangling refs as
            # warnings so contributors can clean up the catalog without
            # the entry losing its PASS status.
            for reason in invalid_reasons:
                logger.warning(
                    "evidence: `%s` has a dangling test reference:\n%s",
                    r.entry.name,
                    reason,
                )
            updated.append(r)
            continue
        # No evidence (or all references were dangling).
        if in_grace:
            grace_msg = (
                "evidence: `%s` claims supported but has no validatable "
                "tests — grace period until %s, will fail after"
            )
            if invalid_reasons:
                grace_msg += "\n  reasons:\n" + "\n".join(invalid_reasons)
            logger.warning(grace_msg, r.entry.name, grace_until)
            updated.append(r)
        else:
            detail = "catalog claims supported but evidence.tests is empty"
            if invalid_reasons:
                detail = (
                    "catalog claims supported but every test reference is "
                    "dangling: " + "; ".join(
                        line.lstrip("  - ").rstrip() for line in invalid_reasons
                    )
                )
            updated.append(
                Result(
                    entry=r.entry,
                    status=Status.SUPPORTED_NO_EVIDENCE,
                    detail=detail,
                )
            )
    return updated


# ─────────────────────────────────────────────────────────────────────────────
# Run
# ─────────────────────────────────────────────────────────────────────────────


def run_surface(repo_root: Path, surface: str) -> list[Result]:
    if surface not in ADAPTERS:
        raise ValueError(f"surface {surface!r} has no adapter wired yet")
    compat = load_compat(repo_root)
    entries = collect_entries(compat, surface)
    adapter = ADAPTERS[surface](repo_root)
    results = [adapter.run(e) for e in entries]
    return check_evidence(repo_root, results, compat)


def run_all(repo_root: Path) -> dict[str, list[Result]]:
    out: dict[str, list[Result]] = {}
    compat = load_compat(repo_root)
    for surface in KNOWN_SURFACES:
        if surface not in ADAPTERS:
            continue
        entries = collect_entries(compat, surface)
        adapter = ADAPTERS[surface](repo_root)
        results = [adapter.run(e) for e in entries]
        out[surface] = check_evidence(repo_root, results, compat)
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


def render_markdown(
    results_by_surface: dict[str, list[Result]],
    sha: str,
    visual_counts: Optional[dict[str, dict]] = None,
    validation_counts: Optional[dict[str, dict]] = None,
) -> str:
    visual_counts = visual_counts or {}
    validation_counts = validation_counts or {}
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
    lines.append("| Surface | Total | PASS | NO-EV | DIVERGE | NO-OP | NOT-IMPL | OOS | PASS % | Progress % | Drift | Validation route | Visual covered |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for surface, results in results_by_surface.items():
        counts = StatusCounts.from_results([r.status for r in results])
        drifts = sum(1 for r in results if r.drifts)
        visual = visual_counts.get(surface, {"label": "0/0"})
        validation = validation_counts.get(surface, {"label": "0/0"})
        lines.append(
            "| `{}/` | {} | {} | {} | {} | {} | {} | {} | {:.1f}% | {:.1f}% | {} | {} | {} |".format(
                surface,
                counts.total,
                counts.pass_,
                counts.supported_no_evidence,
                counts.diverge,
                counts.no_op,
                counts.not_impl,
                counts.oos,
                counts.pass_pct,
                counts.progress_pct,
                drifts,
                validation["label"],
                visual["label"],
            )
        )
    if len(results_by_surface) > 1:
        all_drifts = sum(
            1
            for results in results_by_surface.values()
            for r in results
            if r.drifts
        )
        visual_pass = sum(int(v.get("pass", 0)) for v in visual_counts.values())
        visual_total = sum(int(v.get("total", 0)) for v in visual_counts.values())
        validation_pass = sum(int(v.get("pass", 0)) for v in validation_counts.values())
        validation_total = sum(int(v.get("total", 0)) for v in validation_counts.values())
        lines.append(
            "| **TOTAL** | {} | {} | {} | {} | {} | {} | {} | {:.1f}% | {:.1f}% | {} | {}/{} | {}/{} |".format(
                total_counts.total,
                total_counts.pass_,
                total_counts.supported_no_evidence,
                total_counts.diverge,
                total_counts.no_op,
                total_counts.not_impl,
                total_counts.oos,
                total_counts.pass_pct,
                total_counts.progress_pct,
                all_drifts,
                validation_pass,
                validation_total,
                visual_pass,
                visual_total,
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


def render_json(
    results_by_surface: dict[str, list[Result]],
    sha: str,
    visual_counts: Optional[dict[str, dict]] = None,
    validation_counts: Optional[dict[str, dict]] = None,
) -> dict:
    visual_counts = visual_counts or {}
    validation_counts = validation_counts or {}
    out_surfaces = {}
    for surface, results in results_by_surface.items():
        counts = StatusCounts.from_results([r.status for r in results])
        visual_coverage = visual_counts.get(surface, {"pass": 0, "total": 0, "label": "0/0"})
        validation_routes = validation_counts.get(surface, {"pass": 0, "total": 0, "label": "0/0"})
        out_surfaces[surface] = {
            "total": counts.total,
            "pass": counts.pass_,
            "supported_no_evidence": counts.supported_no_evidence,
            "diverge": counts.diverge,
            "no_op": counts.no_op,
            "not_impl": counts.not_impl,
            "oos": counts.oos,
            "pass_pct": round(counts.pass_pct, 2),
            "progress_pct": round(counts.progress_pct, 2),
            "drift_count": sum(1 for r in results if r.drifts),
            "validation_routes": validation_routes,
            "visual_coverage": visual_coverage,
            "visual_pass": visual_coverage,
            "results": [r.to_dict() for r in results],
        }
    total_counts = StatusCounts.from_results(
        [r.status for results in results_by_surface.values() for r in results]
    )
    visual_pass = sum(int(v.get("pass", 0)) for v in visual_counts.values())
    visual_total = sum(int(v.get("total", 0)) for v in visual_counts.values())
    validation_pass = sum(int(v.get("pass", 0)) for v in validation_counts.values())
    validation_total = sum(int(v.get("total", 0)) for v in validation_counts.values())
    validation_routes = {
        "pass": validation_pass,
        "total": validation_total,
        "label": f"{validation_pass}/{validation_total}" if validation_total else f"{validation_pass}/0",
    }
    visual_coverage = {
        "pass": visual_pass,
        "total": visual_total,
        "label": f"{visual_pass}/{visual_total}" if visual_total else f"{visual_pass}/0",
    }
    return {
        "schema_version": "0.2",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "sha": sha,
        "totals": {
            "total": total_counts.total,
            "pass": total_counts.pass_,
            "supported_no_evidence": total_counts.supported_no_evidence,
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
            "validation_routes": validation_routes,
            "visual_coverage": visual_coverage,
            "visual_pass": visual_coverage,
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

    visual_counts = visual_runner.visual_pass_counts(repo_root, results_by_surface.keys())
    validation_counts = visual_runner.validation_route_counts(repo_root, results_by_surface.keys())
    json_payload = render_json(results_by_surface, sha, visual_counts, validation_counts)
    md_payload = render_markdown(results_by_surface, sha, visual_counts, validation_counts)

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
    if argv and argv[0] == "visual":
        return visual_runner.main(argv)
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
    visual_counts = visual_runner.visual_pass_counts(repo_root, results_by_surface.keys())
    validation_counts = visual_runner.validation_route_counts(repo_root, results_by_surface.keys())

    if args.json:
        print(json.dumps(render_json(results_by_surface, sha, visual_counts, validation_counts), indent=2))
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
    print(f"{'surface':<10} {'total':>6} {'PASS':>6} {'NO-EV':>6} {'DIVERGE':>8} {'NO-OP':>6} "
          f"{'NOT-IMPL':>9} {'OOS':>5} {'PASS%':>7} {'drift':>6} {'valid':>8} {'visual':>8}")
    for surface, results in results_by_surface.items():
        c = StatusCounts.from_results([r.status for r in results])
        drifts = sum(1 for r in results if r.drifts)
        visual = visual_counts.get(surface, {"label": "0/0"})
        validation = validation_counts.get(surface, {"label": "0/0"})
        print(
            f"{surface:<10} {c.total:>6} {c.pass_:>6} {c.supported_no_evidence:>6} {c.diverge:>8} "
            f"{c.no_op:>6} {c.not_impl:>9} {c.oos:>5} {c.pass_pct:>6.1f}% "
            f"{drifts:>6} {validation['label']:>8} {visual['label']:>8}"
        )
    if len(results_by_surface) > 1:
        c = total_counts
        all_drifts = sum(
            1
            for results in results_by_surface.values()
            for r in results
            if r.drifts
        )
        visual_pass = sum(int(v.get("pass", 0)) for v in visual_counts.values())
        visual_total = sum(int(v.get("total", 0)) for v in visual_counts.values())
        validation_pass = sum(int(v.get("pass", 0)) for v in validation_counts.values())
        validation_total = sum(int(v.get("total", 0)) for v in validation_counts.values())
        print(
            f"{'TOTAL':<10} {c.total:>6} {c.pass_:>6} {c.supported_no_evidence:>6} {c.diverge:>8} "
            f"{c.no_op:>6} {c.not_impl:>9} {c.oos:>5} {c.pass_pct:>6.1f}% "
            f"{all_drifts:>6} {f'{validation_pass}/{validation_total}':>8} "
            f"{f'{visual_pass}/{visual_total}':>8}"
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
