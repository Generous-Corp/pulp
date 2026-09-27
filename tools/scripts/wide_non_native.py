#!/usr/bin/env python3
"""Decide whether a tooling-only change can leave the native macOS gate.

``classify_changes.py`` treats every ``tools/scripts/**`` file as a native
build input, because some of them are: ctest runs them, CMake reads them, or
``build.yml`` executes them inside the gate. Most are not. This module proves,
per changed file, that no test the native gate still runs can observe it. It is
consulted only when the ``PULP_CLASSIFY_WIDE_NON_NATIVE`` repository variable
is ``1``; with the variable unset the classifier never imports it.

A candidate file is admitted when every file that names it (a word-boundary
``git grep`` for its stem, the way a Python import, a shell call or a CMake
registration names a script) is one of:

* **inert**: Markdown, ``docs/``, ``planning/``, ``.agents/``, ``.githooks/``,
  or a workflow other than the build gate's own (``build.yml``,
  ``build-macos.yml`` and ``.github/actions/**`` run inside the gate);
* **a lane test**: the script an entry of ``tools/ci/source_selftests.json``
  executes. The gate excludes those on PR and merge-group events and the
  required ``Enforce version & skill sync`` context runs every one of them;
* **a tier check**: the script an entry of
  ``tools/ci/wide_non_native_checks.json`` executes. Those are the gate's own
  repository scanners that read ``tools/**`` without naming a file, and the
  same required context runs them whenever the variable is set;
* **a CMake registration of a lane or tier test**, and nothing else in that
  CMake file names the stem;
* **another admitted file**, recursively (a greatest fixpoint, so a helper
  imported only by a lane-tested script is admitted with it).

Anything else (C++ or test sources, ``tools/ci/**``, ``tools/cmake/**``, a
gate-side CMake registration, the gate's workflows, an unrecognised file)
keeps the native build. So does every error: a failed ``git grep``, an
unreadable manifest, or a search that exceeds its budget.

What it cannot see is a reference assembled at run time from parts (an
f-string that builds a script name). The gate scanners that walk ``tools/``
without naming files are enumerated by ``test_wide_non_native.py``, which fails
when a new one appears that neither the tier runs nor a reviewed exemption
explains.
"""
from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parents[2]
LANE_MANIFEST = "tools/ci/source_selftests.json"
TIER_MANIFEST = "tools/ci/wide_non_native_checks.json"

# Only these trees may be admitted. Everything outside them keeps its ordinary
# classify_changes.py verdict.
CANDIDATE_PREFIXES = (
    "tools/scripts/",
    "tools/testing/",
    "tools/import-validation/",
)

# Never admitted, whatever the reference graph says: the classifier decides
# its own trust, so a change to it must be validated by the path it gates.
HARD_NATIVE = frozenset(
    {
        "tools/scripts/classify_changes.py",
        "tools/scripts/test_classify_changes.py",
        "tools/scripts/wide_non_native.py",
        "tools/scripts/test_wide_non_native.py",
        "tools/scripts/resolve_classify_base.py",
        "tools/scripts/generated_version_bump_check.py",
    }
)

# Workflow and action files that execute inside the native gate job.
GATE_WORKFLOWS = (
    ".github/workflows/build.yml",
    ".github/workflows/build-macos.yml",
)

INERT_PREFIXES = (
    "docs/",
    "planning/",
    ".agents/",
    ".githooks/",
    ".claude/",
    ".codex/",
    ".github/ISSUE_TEMPLATE/",
    ".github/vellum-change-events/",
    ".github/vellum-expansion-watch-events/",
)
# The replay fixture lists every path of 177 merged pull requests and is read
# only by this module's own test; as a referrer it would block everything.
REPLAY_FIXTURE = "tools/scripts/fixtures/wide_non_native_replay.json"
INERT_EXACT = frozenset(
    {
        LANE_MANIFEST,
        TIER_MANIFEST,
        REPLAY_FIXTURE,
        ".gitattributes",
        "CODEOWNERS",
        ".gitignore",
    }
)

# A single classification may run at most this many searches (each is one
# pass over the tree, about a second) and follows helpers at most MAX_DEPTH
# levels deep. Exceeding either means the reference graph is too wide to
# reason about cheaply: native.
DEFAULT_GREP_BUDGET = 12
MAX_DEPTH = 4

_ADD_TEST = re.compile(r"add_test\s*\(\s*NAME\s+([^\s)]+)")


@dataclass
class Decision:
    admitted: bool
    reason: str


@dataclass
class _Context:
    repo: Path
    lane_files: frozenset[str]
    tier_files: frozenset[str]
    test_names: frozenset[str]
    budget: int
    grep_calls: int = 0
    referrer_cache: dict[str, list[str] | None] = field(default_factory=dict)


def _manifest_scripts(repo: Path, rel: str) -> tuple[set[str], set[str]]:
    """Return (script paths, test names) a source-selftest style manifest runs."""
    data = json.loads((repo / rel).read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError(f"{rel}: expected schema_version 1")
    scripts: set[str] = set()
    names: set[str] = set()
    for entry in data["tests"]:
        names.add(entry["name"])
        argv = entry["argv"]
        if argv[:2] == ["-m", "unittest"] and entry.get("cwd"):
            cwd = entry["cwd"].replace("{repo}", "").lstrip("/")
            scripts.add(f"{cwd}/{argv[2]}.py")
        else:
            scripts.add(argv[0].replace("{repo}", "").lstrip("/"))
    return scripts, names


def _stem(path: str) -> str:
    return PurePosixPath(path).stem


def _is_inert(path: str) -> bool:
    if path.endswith(".md") or path in INERT_EXACT:
        return True
    if path.startswith(INERT_PREFIXES):
        return True
    if path.startswith(".github/workflows/"):
        return path not in GATE_WORKFLOWS
    return False


def _prefetch(ctx: _Context, stems: set[str]) -> bool:
    """Fill the referrer cache for ``stems`` with ONE git grep; False on error.

    ``git grep -l -w -F -f`` finds every tracked file naming any of the stems
    in a single pass; the per-stem attribution then reads only those files.
    """
    pending = sorted(s for s in stems if s not in ctx.referrer_cache)
    if not pending:
        return True
    if ctx.grep_calls >= ctx.budget:
        return False
    ctx.grep_calls += 1
    proc = subprocess.run(
        ["git", "grep", "-l", "-w", "-F", "-f", "-"],
        cwd=ctx.repo,
        input="\n".join(pending) + "\n",
        capture_output=True,
        text=True,
        check=False,
    )
    # git grep exits 1 when nothing matches; anything else is an error.
    if proc.returncode not in (0, 1):
        return False
    word = re.compile(
        rb"(?<![A-Za-z0-9_])("
        + b"|".join(re.escape(s.encode()) for s in pending)
        + rb")(?![A-Za-z0-9_])"
    )
    found: dict[str, list[str]] = {s: [] for s in pending}
    for rel in proc.stdout.splitlines():
        try:
            data = (ctx.repo / rel).read_bytes()
        except OSError:
            return False
        for stem in {m.decode() for m in word.findall(data)}:
            found[stem].append(rel)
    ctx.referrer_cache.update(found)
    return True


def _referrers(ctx: _Context, stem: str) -> list[str] | None:
    if stem not in ctx.referrer_cache and not _prefetch(ctx, {stem}):
        return None
    return ctx.referrer_cache.get(stem)


def _warm(ctx: _Context, paths: set[str]) -> bool:
    """Prefetch the reference graph breadth-first, one grep per level."""
    frontier = {_stem(p) for p in paths}
    seen: set[str] = set()
    for _ in range(MAX_DEPTH):
        frontier -= seen
        if not frontier:
            return True
        if not _prefetch(ctx, frontier):
            return False
        seen |= frontier
        frontier = {
            _stem(ref)
            for stem in frontier
            for ref in ctx.referrer_cache[stem]
            if _candidate(ref)
        }
    return True


def _cmake_reference_is_lane_registration(
    ctx: _Context, cmake_path: str, stem: str
) -> bool:
    """True when every mention of ``stem`` sits in a lane/tier add_test call."""
    try:
        text = (ctx.repo / cmake_path).read_text(encoding="utf-8")
    except OSError:
        return False
    word = re.compile(rf"(?<![A-Za-z0-9_]){re.escape(stem)}(?![A-Za-z0-9_])")
    for match in word.finditer(text):
        start = text.rfind("add_test", 0, match.start())
        if start < 0:
            return False
        close = text.find(")", start)
        if close < match.start():
            return False  # the mention is outside that add_test call
        name = _ADD_TEST.match(text, start)
        if not name or name.group(1) not in ctx.test_names:
            return False
    return True


def _is_cmake(path: str) -> bool:
    name = PurePosixPath(path).name
    return name == "CMakeLists.txt" or name.endswith((".cmake", ".cmake.in"))


def _candidate(path: str) -> bool:
    return path.startswith(CANDIDATE_PREFIXES) and path not in HARD_NATIVE


def _acceptable(ctx: _Context, ref: str) -> bool:
    """A referrer that never lets the gate observe what it names."""
    return _is_inert(ref) or ref in ctx.lane_files or ref in ctx.tier_files


def _hard_blockers(ctx: _Context, path: str) -> list[str] | None:
    """Referrers of ``path`` that are neither acceptable nor admissible nodes.

    Returns None when the reference search for ``path`` never ran (a node
    beyond the prefetched depth): that node simply cannot be admitted.
    Referrers that are themselves candidate files are returned separately by
    the caller's fixpoint, so they are skipped here.
    """
    stem = _stem(path)
    refs = ctx.referrer_cache.get(stem)
    if refs is None:
        return None
    blockers: list[str] = []
    for ref in refs:
        if ref == path or _acceptable(ctx, ref):
            continue
        if _candidate(ref):
            continue  # judged by the fixpoint
        if _is_cmake(ref) and not ref.startswith("tools/cmake/"):
            if _cmake_reference_is_lane_registration(ctx, ref, stem):
                continue
        blockers.append(ref)
    return blockers


def classify(
    files: list[str],
    *,
    repo: Path = REPO_ROOT,
    budget: int = DEFAULT_GREP_BUDGET,
    referrer_cache: dict[str, list[str]] | None = None,
) -> dict[str, Decision]:
    """Per-file admission decisions for the files the base classifier kept.

    The caller passes only files it did NOT already find skip-safe. Every
    candidate-tree file reached by the prefetch (the changed files and the
    unchanged helpers that name them) is a node. A greatest fixpoint starts by
    assuming every node admissible, then repeatedly drops a node that has a
    blocking referrer, or a candidate referrer that has itself been dropped,
    until nothing changes. A changed file is admitted when it survives.
    """
    try:
        lane_files, lane_names = _manifest_scripts(repo, LANE_MANIFEST)
        tier_files, tier_names = _manifest_scripts(repo, TIER_MANIFEST)
    except (OSError, ValueError, KeyError, TypeError, IndexError) as exc:
        return {f: Decision(False, f"manifest unreadable: {exc}") for f in files}
    ctx = _Context(
        repo=repo,
        lane_files=frozenset(lane_files),
        tier_files=frozenset(tier_files),
        test_names=frozenset(lane_names | tier_names),
        budget=budget,
    )
    if referrer_cache is not None:
        # A caller classifying many file sets against ONE tree (the replay
        # test) shares the stem -> referrers map; it depends only on the tree.
        ctx.referrer_cache = referrer_cache
    decisions: dict[str, Decision] = {}
    changed_candidates = {p for p in files if _candidate(p)}
    for path in files:
        if path not in changed_candidates:
            decisions[path] = Decision(False, "outside the widened tooling trees")
    if not changed_candidates:
        return decisions
    if not _warm(ctx, changed_candidates):
        for path in changed_candidates:
            decisions[path] = Decision(
                False, "reference search failed or exceeded its budget"
            )
        return decisions

    # Nodes: changed candidates plus every candidate-tree file that names one
    # of the searched stems.
    nodes = set(changed_candidates)
    for refs in ctx.referrer_cache.values():
        nodes.update(r for r in refs if _candidate(r) and not _acceptable(ctx, r))
    reason: dict[str, str] = {}
    candidate_refs: dict[str, list[str]] = {}
    alive: set[str] = set()
    for node in nodes:
        blockers = _hard_blockers(ctx, node)
        if blockers is None:
            reason[node] = "beyond the searched reference depth"
            continue
        if blockers:
            shown = ", ".join(sorted(blockers)[:4])
            more = f" (+{len(blockers) - 4} more)" if len(blockers) > 4 else ""
            reason[node] = f"named by {shown}{more}"
            continue
        candidate_refs[node] = [
            r
            for r in ctx.referrer_cache[_stem(node)]
            if r != node and _candidate(r) and not _acceptable(ctx, r)
        ]
        alive.add(node)
    dropped = True
    while dropped:
        dropped = False
        for node in sorted(alive):
            dead = [r for r in candidate_refs[node] if r not in alive]
            if dead:
                alive.discard(node)
                reason[node] = f"named by {dead[0]}, which the gate can observe"
                dropped = True
    for path in changed_candidates:
        if path in alive:
            decisions[path] = Decision(
                True,
                "observed only by lane/tier checks on the required hosted context",
            )
        else:
            decisions[path] = Decision(False, reason[path])
    return decisions
