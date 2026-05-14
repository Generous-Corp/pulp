#!/usr/bin/env python3
"""Per-tier coverage gate for Pulp's PR diff — #566 Phase 2.

Consumes a Cobertura XML + `ci/coverage-targets.yaml` + the set of
lines the PR added/modified, and asserts that each tier's diff
coverage meets its configured floor.

Design: runs ALONGSIDE the existing `diff-cover` gate (which
enforces a global 75% floor via `.github/workflows/coverage.yml`).
This script adds per-tier floors on top — audio-critical code has
to hit 80%, user-facing 70%, infrastructure 50%. Files that don't
match any tier fall back to the global gate silently.

Inputs:
    --cobertura <path>      Cobertura XML from gcovr
    --targets <path>        ci/coverage-targets.yaml
    --compare-branch <ref>  origin/main (for the diff range)
    --markdown-report <p>   write a markdown summary for the PR comment

Exit codes:
    0  — every touched tier met its floor (or no touched files)
    1  — at least one tier fell below its floor
    2  — config or invocation error

The script is unit-testable via `render()` and `classify_file()`;
`main()` is the shell-out entry point used by the workflow.
"""

from __future__ import annotations

import argparse
import fnmatch
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Optional


# ── Config loading ─────────────────────────────────────────────────────────


@dataclass(frozen=True)
class Tier:
    """A single tier definition from ci/coverage-targets.yaml."""

    name: str
    line_target: int
    paths: tuple[str, ...]


def load_targets(path: pathlib.Path) -> list[Tier]:
    """Parse ci/coverage-targets.yaml; tolerate missing PyYAML by hand-parsing.

    We avoid a hard PyYAML dependency because the workflow otherwise
    doesn't need one — a single `pip install pyyaml` step added just
    for this script is noisy. The YAML we care about is a fixed
    shape, so the hand parser is small.
    """
    import yaml  # type: ignore[import-not-found]

    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or "tiers" not in data:
        raise ValueError(f"{path}: expected top-level 'tiers' list")
    version = int(data.get("version", 0))
    if version != 1:
        raise ValueError(f"{path}: unsupported version {version}")

    tiers: list[Tier] = []
    for entry in data["tiers"]:
        tiers.append(
            Tier(
                name=str(entry["name"]),
                line_target=int(entry["line_target"]),
                paths=tuple(str(p) for p in entry["paths"]),
            )
        )
    return tiers


# ── Classification ─────────────────────────────────────────────────────────


def classify_file(relpath: str, tiers: list[Tier]) -> Optional[Tier]:
    """Return the first tier whose path globs match ``relpath``.

    fnmatch semantics: `core/audio/**` matches `core/audio/src/foo.cpp`.
    First match wins, so narrower tiers in the YAML should come first.
    """
    for tier in tiers:
        for pat in tier.paths:
            # fnmatch doesn't treat `**` specially, so strip it for a
            # prefix-style match. `core/audio/**` → `core/audio/*`
            # matched against the full relpath via `startswith`.
            prefix = pat.rstrip("*").rstrip("/")
            if prefix and relpath.startswith(prefix + "/"):
                return tier
            if fnmatch.fnmatch(relpath, pat):
                return tier
    return None


# ── Diff discovery ─────────────────────────────────────────────────────────


def diff_files(compare_branch: str) -> list[str]:
    """Return repo-relative paths of files changed vs ``compare_branch``.

    Uses ``git diff --name-only <compare_branch>...HEAD``. The triple-dot
    form diffs against the merge base, matching diff-cover's semantics.
    """
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", f"{compare_branch}...HEAD"],
            text=True,
        )
    except subprocess.CalledProcessError as exc:
        raise ValueError(f"git diff failed: {exc}") from exc
    return [line.strip() for line in out.splitlines() if line.strip()]


def diff_lines(compare_branch: str, relpath: str) -> set[int]:
    """Return the set of line numbers the PR added/modified in ``relpath``.

    Uses ``git diff --unified=0`` and parses the @@ hunks. We only care
    about the NEW line range (the "+" side).
    """
    try:
        out = subprocess.check_output(
            [
                "git", "diff", "--unified=0",
                f"{compare_branch}...HEAD", "--", relpath,
            ],
            text=True,
        )
    except subprocess.CalledProcessError:
        return set()

    changed: set[int] = set()
    for line in out.splitlines():
        if not line.startswith("@@"):
            continue
        # @@ -12,3 +45,7 @@ ...  →  new start 45, count 7
        try:
            new_part = line.split("+", 1)[1].split(" ", 1)[0]
            if "," in new_part:
                start, count = (int(x) for x in new_part.split(","))
            else:
                start, count = int(new_part), 1
        except (IndexError, ValueError):
            continue
        for i in range(start, start + count):
            changed.add(i)
    return changed


# ── Cobertura parsing ──────────────────────────────────────────────────────


@dataclass
class FileCoverage:
    """Cobertura per-file line hits."""

    path: str
    hits: dict[int, int] = field(default_factory=dict)  # line -> hit-count


def parse_cobertura(xml_path: pathlib.Path) -> dict[str, FileCoverage]:
    """Return {relpath: FileCoverage} from a Cobertura XML.

    Cobertura stores a <sources><source>REPO_ROOT</source></sources> element
    and <class filename="core/audio/src/foo.cpp"> children. We ignore the
    source root and use the filename as-is (already repo-relative).
    """
    root = ET.parse(xml_path).getroot()
    out: dict[str, FileCoverage] = {}
    for cls in root.iter("class"):
        filename = cls.attrib.get("filename", "")
        if not filename:
            continue
        fc = FileCoverage(path=filename)
        for line in cls.iter("line"):
            try:
                n = int(line.attrib["number"])
                hits = int(line.attrib.get("hits", "0"))
            except (KeyError, ValueError):
                continue
            fc.hits[n] = hits
        out[filename] = fc
    return out


# ── Source filtering ───────────────────────────────────────────────────────


# Extensions whose coverage data shows up in a Cobertura XML the gate
# can read. Two sources feed this set:
#
#   1. The Clang source-based coverage pipeline (scripts/run_coverage.sh)
#      emits Cobertura rows for C / C++ / Obj-C / Obj-C++ sources.
#   2. The vitest v8 coverage pipeline in `packages/pulp-react/**` emits
#      Cobertura rows for `.ts` / `.tsx` / `.js` / `.jsx` — wired by
#      pulp #1886 Phase 1 (measure-only). Phase 2 layers enforcement.
#
# Scripts, CMake modules, YAML, shell, Python etc. in the tier map
# (ship/**, tools/**) can't produce Cobertura entries, so the old
# "missing file = fully uncovered" path would fail the infrastructure
# tier for any PR that only touched those files. Codex #612 P1.
#
# Codex P2 (pulp #1886): JS/TS coverage is NOT global. Vitest only emits
# Cobertura rows for `packages/pulp-react/**`. A `.ts` file outside that
# surface (e.g. `tools/foo.ts`, `inspect/src/dev.tsx`) has no Cobertura
# row by design, so the "missing file = uncovered" path would falsely
# fail the tier. The `_REPORTED_SURFACES` table below scopes each
# extension to the path prefixes its coverage lane actually reports for.
# Files with the right extension but outside the reporting surface are
# treated as not-instrumented (mirroring how a `.sh` or `.cmake` file
# is handled — present in the diff, but unable to contribute to the
# Cobertura aggregate either way).
_INSTRUMENTED_EXTS = frozenset({
    # Clang source-based coverage (scripts/run_coverage.sh):
    ".c", ".cc", ".cpp", ".cxx", ".c++",
    ".h", ".hh", ".hpp", ".hxx", ".h++",
    ".m", ".mm",
    # Vitest v8 coverage (packages/pulp-react/**) — pulp #1886 Phase 1.
    # `.mts` / `.cts` deliberately omitted for now; @pulp/react is pure
    # `.ts` and there's no measured surface for the other variants yet.
    # Add them here when a real source file lands.
    ".ts", ".tsx", ".js", ".jsx",
})


# Path prefixes (repo-relative, "/" suffix) under which a given extension
# is actually instrumented. `None` means "no scope restriction — the
# extension is instrumented anywhere it appears" (the Clang pipeline's
# behavior). A tuple of prefixes restricts the extension to those
# subtrees; anything outside is treated as not-instrumented.
#
# When you add a new JS/TS coverage lane (or move @pulp/react to a
# different folder), update this table in lock-step with the lane.
_REPORTED_SURFACES: dict[str, Optional[tuple[str, ...]]] = {
    # Clang C-family: any path the build compiles.
    ".c": None,    ".cc": None,   ".cpp": None,  ".cxx": None,  ".c++": None,
    ".h": None,    ".hh": None,   ".hpp": None,  ".hxx": None,  ".h++": None,
    ".m": None,    ".mm": None,
    # Vitest v8: only `packages/pulp-react/**` is wired into the
    # Cobertura output today.
    ".ts":  ("packages/pulp-react/",),
    ".tsx": ("packages/pulp-react/",),
    ".js":  ("packages/pulp-react/",),
    ".jsx": ("packages/pulp-react/",),
}


def is_instrumented_source(relpath: str) -> bool:
    """True iff this path has an extension AND lives under a reported surface.

    A file with an extension in `_INSTRUMENTED_EXTS` but outside the
    reporting surface for that extension (per `_REPORTED_SURFACES`) is
    NOT counted as instrumented — its diff lines won't be aggregated as
    "uncovered" against any tier. This matches how non-source files
    (shell, CMake, Python, YAML) are handled. Codex P2 on pulp #1886.
    """
    dot = relpath.rfind(".")
    if dot < 0:
        return False
    ext = relpath[dot:].lower()
    if ext not in _INSTRUMENTED_EXTS:
        return False
    surfaces = _REPORTED_SURFACES.get(ext)
    if surfaces is None:
        return True
    return any(relpath.startswith(prefix) for prefix in surfaces)


# ── Per-tier aggregation ───────────────────────────────────────────────────


@dataclass
class TierResult:
    """Aggregate diff coverage for one tier."""

    tier: Tier
    touched_lines: int = 0
    covered_lines: int = 0
    files: list[str] = field(default_factory=list)

    @property
    def percent(self) -> float:
        if self.touched_lines == 0:
            return 100.0
        return 100.0 * self.covered_lines / self.touched_lines

    @property
    def passed(self) -> bool:
        if self.touched_lines == 0:
            return True
        return self.percent >= self.tier.line_target


def aggregate(
    tiers: list[Tier],
    changed_files: list[str],
    coverage: dict[str, FileCoverage],
    lines_getter,
) -> list[TierResult]:
    """Compute per-tier diff coverage.

    ``lines_getter`` is injected for testability — real usage passes a
    closure that calls ``diff_lines(compare_branch, relpath)``.
    """
    results: dict[str, TierResult] = {t.name: TierResult(tier=t) for t in tiers}
    for relpath in changed_files:
        tier = classify_file(relpath, tiers)
        if tier is None:
            continue
        # Non-instrumented files (CMake, shell, Python, YAML, …) can't
        # produce coverage rows — don't count them as "uncovered" and
        # tank the tier. Codex #612 P1.
        if not is_instrumented_source(relpath):
            continue
        fc = coverage.get(relpath)
        if fc is None:
            # File not in coverage report (new source, no tests) — count
            # all changed lines as untested.
            changed = lines_getter(relpath)
            tr = results[tier.name]
            tr.touched_lines += len(changed)
            tr.covered_lines += 0
            if changed:
                tr.files.append(relpath)
            continue
        changed = lines_getter(relpath)
        tr = results[tier.name]
        tier_hit = 0
        for ln in changed:
            if fc.hits.get(ln, 0) > 0:
                tier_hit += 1
        tr.touched_lines += len(changed)
        tr.covered_lines += tier_hit
        if changed:
            tr.files.append(relpath)
    return [results[t.name] for t in tiers]


# ── Rendering ──────────────────────────────────────────────────────────────


def render(results: list[TierResult]) -> str:
    """Build a markdown summary suitable for a PR comment section."""
    lines: list[str] = []
    lines.append("## Per-tier diff coverage (#566 Phase 2)")
    lines.append("")
    lines.append("| Tier | Target | Diff coverage | Touched lines | Result |")
    lines.append("|------|--------|---------------|---------------|--------|")
    for r in results:
        mark = "✓" if r.passed else "✗"
        if r.touched_lines == 0:
            pct = "— (no touched lines)"
        else:
            pct = f"{r.percent:.1f}%"
        lines.append(
            f"| {r.tier.name} | {r.tier.line_target}% | {pct} | "
            f"{r.touched_lines} | {mark} |"
        )
    lines.append("")
    failed = [r for r in results if not r.passed]
    if failed:
        names = ", ".join(r.tier.name for r in failed)
        lines.append(
            f"**Per-tier gate failed:** {names} below tier floor(s). "
            "Add tests for the untested diff or split the untested "
            "portion into its own PR."
        )
    else:
        lines.append("All touched tiers meet their per-tier floors.")
    return "\n".join(lines).rstrip() + "\n"


# ── Main ───────────────────────────────────────────────────────────────────


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cobertura", required=True, type=pathlib.Path)
    parser.add_argument("--targets", required=True, type=pathlib.Path)
    parser.add_argument("--compare-branch", required=True)
    parser.add_argument("--markdown-report", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)

    if not args.cobertura.exists():
        # Missing Cobertura XML means the upstream coverage job didn't
        # produce one. The existing diff-cover gate handles this same
        # case gracefully; do the same here so we don't double-fail.
        args.markdown_report.write_text(
            "## Per-tier diff coverage (#566 Phase 2)\n\n"
            "_Cobertura XML missing from upstream coverage job; "
            "per-tier gate skipped._\n",
            encoding="utf-8",
        )
        return 0

    tiers = load_targets(args.targets)
    coverage = parse_cobertura(args.cobertura)
    changed = diff_files(args.compare_branch)

    def lines_getter(relpath: str) -> set[int]:
        return diff_lines(args.compare_branch, relpath)

    results = aggregate(tiers, changed, coverage, lines_getter)
    report = render(results)
    args.markdown_report.write_text(report, encoding="utf-8")
    print(report)

    return 0 if all(r.passed for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
