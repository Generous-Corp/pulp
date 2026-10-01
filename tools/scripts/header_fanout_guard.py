#!/usr/bin/env python3
"""Guard tracked headers from quietly gaining compile fan-out.

A header's fan-out is the number of translation units whose preprocessed
closure contains it: every one of them recompiles when the header changes,
and every executable that links one of them relinks and re-tests. One new
include of a heavy header from a widely included one (an "umbrella include")
can multiply that number without any single file looking large, so the
hotspot-size guard cannot see it.

The ledger (`header_fanout_guard.json`) lists the tracked headers with a
`max_tus` reference ceiling each. The check is rebase-stable, like the
hotspot guard: a header over its ceiling is a violation only when THIS range
grew its fan-out (head vs merge-base) and no `Fanout-Grow: <path|all>`
trailer authorizes it. A new test that includes a header directly grows it by
one and passes while there is headroom; a new include edge between headers
that pulls hundreds of existing translation units in goes red.

Fan-out is computed statically from `#include` lines at a git ref: no build,
the same answer on every host, and conditional includes counted on every
platform (an upper bound, which is the safe direction for a ceiling). Only
in-repo headers resolve; system and `external/` headers are leaves.

`--build-dir DIR` additionally reports, from a built Ninja tree, how many
executables each tracked header reaches: `compile` counts executables that
link an object compiled from it; `total` adds executables reached only
because a static library they link contains such an object. The difference is
link-level fan-out, which include hygiene cannot change.
"""

from __future__ import annotations

import argparse
import collections
import io
import json
import math
import os
import posixpath
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

DEFAULT_CONFIG = "tools/scripts/header_fanout_guard.json"
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")
TU_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")
SOURCE_SUFFIXES = HEADER_SUFFIXES + TU_SUFFIXES
# Vendored trees and the private planning submodule never hold Pulp sources.
EXCLUDED_PREFIXES = ("external/", "planning/", "build")
# Include search roots beyond every `*/include` directory in the tree.
EXTRA_ROOTS = ("", "test", "test/support")
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]')
TRAILER_RE = re.compile(r"\s*Fanout-Grow:\s*(\S+)")


@dataclass(frozen=True)
class TrackedHeader:
    path: str
    max_tus: int
    note: str = ""


def parse_config(raw_data: Any) -> tuple[TrackedHeader, ...]:
    if not isinstance(raw_data, dict):
        raise ValueError("config root must be an object")
    if raw_data.get("schema_version") != 1:
        raise ValueError("schema_version must be 1")
    entries = raw_data.get("headers")
    if not isinstance(entries, list) or not entries:
        raise ValueError("headers must be a non-empty list")
    seen: set[str] = set()
    out: list[TrackedHeader] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ValueError(f"headers[{index}] must be an object")
        path = entry.get("path")
        if not isinstance(path, str) or not path or path.startswith("/") or "\\" in path:
            raise ValueError(f"headers[{index}].path must be a repo-relative string")
        if not path.endswith(HEADER_SUFFIXES):
            raise ValueError(f"headers[{index}].path is not a header: {path}")
        if path in seen:
            raise ValueError(f"duplicate header path: {path}")
        seen.add(path)
        max_tus = entry.get("max_tus")
        if not isinstance(max_tus, int) or isinstance(max_tus, bool) or max_tus <= 0:
            raise ValueError(f"headers[{index}].max_tus must be a positive integer")
        note = entry.get("note", "")
        if not isinstance(note, str):
            raise ValueError(f"headers[{index}].note must be a string")
        out.append(TrackedHeader(path, max_tus, note))
    return tuple(out)


def _git(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(["git", *args], capture_output=True, text=True, cwd=cwd)
    if result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def _is_source(path: str) -> bool:
    return path.endswith(SOURCE_SUFFIXES) and not path.startswith(EXCLUDED_PREFIXES)


class IncludeGraph:
    """Resolved in-repo `#include` edges over a set of source files."""

    def __init__(self, files: Iterable[str], includes: dict[str, list[str]]) -> None:
        self.files = frozenset(files)
        roots = {posixpath.dirname(f) for f in self.files}
        include_roots = sorted(
            {inc for directory in roots for inc in self._include_ancestors(directory)}
        )
        self.roots = tuple(include_roots) + EXTRA_ROOTS
        self.edges: dict[str, set[str]] = collections.defaultdict(set)
        for src, names in includes.items():
            for name in names:
                target = self.resolve(src, name)
                if target is not None and target != src:
                    self.edges[src].add(target)
        self.includers: dict[str, set[str]] = collections.defaultdict(set)
        for src, targets in self.edges.items():
            for target in targets:
                self.includers[target].add(src)

    @staticmethod
    def _include_ancestors(directory: str) -> Iterable[str]:
        parts = directory.split("/")
        for i, part in enumerate(parts):
            if part == "include":
                yield "/".join(parts[: i + 1])

    def resolve(self, src: str, name: str) -> str | None:
        local = posixpath.normpath(posixpath.join(posixpath.dirname(src), name))
        if local in self.files:
            return local
        for root in self.roots:
            candidate = posixpath.normpath(posixpath.join(root, name)) if root else posixpath.normpath(name)
            if candidate in self.files:
                return candidate
        return None

    def translation_units(self, header: str, without: tuple[str, str] | None = None) -> set[str]:
        """TUs whose include closure holds `header`, optionally ignoring one (src, target) edge."""
        seen = {header}
        stack = [header]
        while stack:
            node = stack.pop()
            for includer in self.includers.get(node, ()):
                if (includer, node) == without:
                    continue
                if includer not in seen:
                    seen.add(includer)
                    stack.append(includer)
        return {path for path in seen if path.endswith(TU_SUFFIXES)}


def graph_at_ref(ref: str, cwd: Path | None = None) -> IncludeGraph:
    listing = _git("ls-tree", "-r", "--name-only", ref, cwd=cwd)
    files = [line for line in listing.splitlines() if _is_source(line)]
    includes: dict[str, list[str]] = collections.defaultdict(list)
    result = subprocess.run(
        ["git", "grep", "-z", "-I", "-E", r"^[[:space:]]*#[[:space:]]*(include|import)[[:space:]]*[<\"]",
         ref, "--"],
        capture_output=True, text=True, cwd=cwd,
    )
    # git grep exits 1 on "no match", which is an empty graph, not an error.
    if result.returncode not in (0, 1):
        raise RuntimeError(f"git grep at {ref}: {result.stderr.strip()}")
    prefix = f"{ref}:"
    for line in result.stdout.splitlines():
        path, _, text = line.partition("\0")
        if path.startswith(prefix):
            path = path[len(prefix):]
        if not _is_source(path):
            continue
        match = INCLUDE_RE.match(text)
        if match:
            includes[path].append(match.group(1))
    return IncludeGraph(files, includes)


def merge_base(base: str, head: str, cwd: Path | None = None) -> str | None:
    result = subprocess.run(["git", "merge-base", base, head], capture_output=True, text=True, cwd=cwd)
    return result.stdout.strip() if result.returncode == 0 else None


def fanout_grow_overrides(base: str, head: str, cwd: Path | None = None) -> frozenset[str]:
    """Headers authorized to grow via a `Fanout-Grow: <path|all>` trailer on any commit in range."""
    result = subprocess.run(
        ["git", "log", "--format=%B%x00", f"{base}..{head}"],
        capture_output=True, text=True, cwd=cwd,
    )
    if result.returncode != 0:
        return frozenset()
    return frozenset(
        m.group(1) for line in result.stdout.splitlines() if (m := TRAILER_RE.match(line))
    )


def new_include_edges(before: IncludeGraph, after: IncludeGraph, header: str) -> list[tuple[str, str, int]]:
    """Include edges added in range that `header`'s reach depends on, with the TUs each one adds.

    An added edge counts only when removing it alone would shrink the reach, so
    an edge into a header that its includer already reached another way is not
    blamed for the growth.
    """
    reach_after = len(after.translation_units(header))
    out = []
    for src, targets in after.edges.items():
        for target in targets - before.edges.get(src, set()):
            if target != header and header not in _closure(after, target):
                continue
            pulled = reach_after - len(after.translation_units(header, without=(src, target)))
            if pulled > 0:
                out.append((src, target, pulled))
    return sorted(out, key=lambda row: -row[2])


def _closure(graph: IncludeGraph, start: str) -> set[str]:
    seen = {start}
    stack = [start]
    while stack:
        node = stack.pop()
        for target in graph.edges.get(node, ()):
            if target not in seen:
                seen.add(target)
                stack.append(target)
    return seen


def check(
    headers: tuple[TrackedHeader, ...],
    head_graph: IncludeGraph,
    base_graph: IncludeGraph | None,
    overrides: frozenset[str],
) -> tuple[list[str], list[str]]:
    failures: list[str] = []
    notes: list[str] = []
    for tracked in headers:
        if tracked.path not in head_graph.files:
            failures.append(
                f"{tracked.path}: tracked header no longer exists; remove or rename its ledger entry"
            )
            continue
        reach = len(head_graph.translation_units(tracked.path))
        if reach <= tracked.max_tus:
            continue
        base_reach = (
            len(base_graph.translation_units(tracked.path))
            if base_graph is not None and tracked.path in base_graph.files
            else 0
        )
        if base_graph is not None and reach <= base_reach:
            notes.append(
                f"{tracked.path}: {reach} TUs is over its ceiling {tracked.max_tus} but this range "
                f"did not grow it (merge-base {base_reach}); not a violation"
            )
            continue
        if tracked.path in overrides or "all" in overrides:
            notes.append(
                f"{tracked.path}: fan-out {base_reach} -> {reach} TUs, authorized by Fanout-Grow"
            )
            continue
        detail = [
            f"{tracked.path}: fan-out grew {base_reach} -> {reach} translation units, over its "
            f"ceiling {tracked.max_tus}."
        ]
        if base_graph is not None:
            for src, target, pulled in new_include_edges(base_graph, head_graph, tracked.path)[:5]:
                detail.append(f"    new include {src} -> {target} reaches {pulled} of them")
        detail.append(
            "    Include it where it is used (a .cpp, or a narrower header) or forward-declare; "
            f"deliberate growth needs a `Fanout-Grow: {tracked.path} reason=\"...\"` trailer."
        )
        failures.append("\n".join(detail))
    return failures, notes


def suggested_ceiling(reach: int) -> int:
    """Headroom for ordinary growth (new tests); an umbrella include blows well past it."""
    return reach + max(10, math.ceil(reach * 0.1))


# --- executable reach from a built Ninja tree ---------------------------------

LINK_RULE_RE = re.compile(r"_EXECUTABLE_LINKER")
LIBRARY_SUFFIXES = (".a", ".dylib", ".so", ".lib", ".dll")


def executable_reach(build_dir: Path, source_root: Path, headers: Iterable[str]) -> dict[str, tuple[int, int]]:
    """{header: (compile_exes, total_exes)} from build.ninja + `ninja -t deps`."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
    import affected_tests_shadow as ats  # noqa: PLC0415 - optional, build-tree mode only

    edges = ats.parse_build_ninja((build_dir / "build.ninja").read_text(encoding="utf-8", errors="replace"))
    deps_text = subprocess.run(
        ["ninja", "-C", str(build_dir), "-t", "deps"], capture_output=True, text=True, check=True
    ).stdout
    graph = ats.Graph(build_dir, edges, ats.parse_ninja_deps(deps_text))
    # A link edge's first output is the binary; the rest are byproducts such as
    # Catch2's discovered `*_tests.cmake` files.
    exes = {graph.norm(outs[0]) for outs, _ins, rule in edges if outs and LINK_RULE_RE.search(rule)}
    if not exes:
        raise RuntimeError(f"no executable link edges in {build_dir / 'build.ninja'}")
    direct_inputs: dict[str, set[str]] = collections.defaultdict(set)
    for outs, ins, rule in edges:
        if outs and LINK_RULE_RE.search(rule):
            direct_inputs[graph.norm(outs[0])].update(graph.norm(i) for i in ins)
    out: dict[str, tuple[int, int]] = {}
    for header in headers:
        absolute = os.path.realpath(source_root / header)
        objects = {graph.norm(o) for o in graph.src_to_out.get(absolute, ())}
        total = graph.affected_outputs([absolute]) & exes
        compiled = {
            exe for exe in total
            if direct_inputs[exe] & objects
        }
        out[header] = (len(compiled), len(total))
    return out


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default="origin/main", help="git base; growth is measured from its merge-base")
    parser.add_argument("--head", default="HEAD", help="git ref whose include graph is checked")
    parser.add_argument("--config", default=DEFAULT_CONFIG, help="ledger path")
    parser.add_argument("--mode", choices=("hint", "report"), default="report",
                        help="hint prints findings and exits 0; report fails on a violation")
    parser.add_argument("--table", action="store_true",
                        help="print every tracked header's current fan-out and ceiling")
    parser.add_argument("--top", type=int, default=0,
                        help="print the N in-repo headers with the widest fan-out at --head")
    parser.add_argument("--build-dir", type=Path,
                        help="also report executable reach per tracked header from this Ninja build")
    return parser


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            os.set_blocking(stream.fileno(), True)
        except (OSError, ValueError, io.UnsupportedOperation):
            pass
    args = build_parser().parse_args(argv)
    soft = 0 if args.mode == "hint" else 2
    try:
        root = Path(_git("rev-parse", "--show-toplevel").strip())
    except RuntimeError:
        print("header_fanout_guard: not in a git working tree", file=sys.stderr)
        return soft
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = root / config_path
    try:
        headers = parse_config(json.loads(config_path.read_text(encoding="utf-8")))
        head_graph = graph_at_ref(args.head, cwd=root)
        if not head_graph.edges:
            raise RuntimeError(f"no #include edges resolved at {args.head}; the graph is blind")
        mb = merge_base(args.base, args.head, cwd=root)
        base_graph = graph_at_ref(mb, cwd=root) if mb else None
        overrides = fanout_grow_overrides(mb, args.head, cwd=root) if mb else frozenset()
        failures, notes = check(headers, head_graph, base_graph, overrides)
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"header_fanout_guard: error: {exc}", file=sys.stderr)
        return soft

    if mb is None:
        notes.append(f"no merge-base for {args.base}..{args.head}; every header over its ceiling fails")
    for note in notes:
        print(f"header_fanout_guard: note: {note}", file=sys.stderr)

    if args.table or args.build_dir:
        exe = {}
        if args.build_dir:
            try:
                exe = executable_reach(args.build_dir, root, [h.path for h in headers])
            except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
                print(f"header_fanout_guard: error: executable reach: {exc}", file=sys.stderr)
                return soft
        cols = "TUs  ceiling" + ("  exes(compile/total)" if exe else "")
        print(f"{cols}  header")
        for tracked in headers:
            reach = len(head_graph.translation_units(tracked.path)) if tracked.path in head_graph.files else 0
            extra = f"  {exe[tracked.path][0]:>7}/{exe[tracked.path][1]:<5}      " if exe else ""
            print(f"{reach:>4}  {tracked.max_tus:>7}{extra}  {tracked.path}")
    if args.top:
        widest = sorted(
            ((len(head_graph.translation_units(h)), h) for h in head_graph.files if h.endswith(HEADER_SUFFIXES)),
            reverse=True,
        )[: args.top]
        for reach, header in widest:
            print(f"{reach:>4}  suggested ceiling {suggested_ceiling(reach):>4}  {header}")

    if failures:
        print("header_fanout_guard: tracked header fan-out grew past its ceiling:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 0 if args.mode == "hint" else 1
    print(f"header_fanout_guard: {len(headers)} tracked header(s) within their fan-out ceilings",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
