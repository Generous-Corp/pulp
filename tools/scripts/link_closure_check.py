#!/usr/bin/env python3
"""Check that a set of Pulp targets does not reach a forbidden set through links.

Input is a link graph written at generate time by
test/cmake/link_closure_graph.cmake: one record per Pulp target,
`name<TAB>type<TAB>LINK_LIBRARIES<TAB>INTERFACE_LINK_LIBRARIES`, with the list
values as CMake stored them (`;`-separated, generator expressions unevaluated).

The walk follows both properties from every root, through every Pulp target it
reaches, so a forbidden target that sits behind an intermediate archive is found
as well as one linked directly. A PRIVATE link of a static library is recorded as
`$<LINK_ONLY:dep>`; it is followed too, because the dependency still lands in the
final link, which is the failure this check exists to catch. Names are
normalised so `pulp::x`, `Pulp::x` and `pulp-x` are one target.

A root may name an `--allow` edge (`from>to`) for a dependency the layering
still owes; every allowed edge is printed, so an exemption never goes unseen.

Exit codes: 0 no root reaches a forbidden target, 1 a root does (each path is
printed), 2 the input could not be read, a root is absent from the graph, or the
graph is too small to be a real one.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from collections import deque
from pathlib import Path

TARGET_RE = re.compile(r"(?<![A-Za-z0-9_+.-])((?:[Pp]ulp)(?:::|-)[A-Za-z0-9_+.:-]*[A-Za-z0-9_+])")
MIN_GRAPH_TARGETS = 20


def canonical(name: str) -> str:
    """`pulp::view-core`, `Pulp::view-core` and `pulp-view-core` are one target."""
    lowered = name.lower()
    if lowered.startswith("pulp::"):
        return "pulp-" + lowered[len("pulp::"):]
    return lowered


def referenced_targets(value: str) -> list[str]:
    seen: list[str] = []
    for match in TARGET_RE.findall(value):
        name = canonical(match)
        if name not in seen:
            seen.append(name)
    return seen


class Graph:
    def __init__(self) -> None:
        self.edges: dict[str, list[str]] = {}
        self.types: dict[str, str] = {}

    @classmethod
    def parse(cls, text: str) -> "Graph":
        graph = cls()
        for line in text.splitlines():
            if not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) < 4:
                continue
            name = canonical(fields[0].strip())
            graph.types[name] = fields[1].strip()
            targets: list[str] = []
            for value in fields[2:4]:
                if value.endswith("-NOTFOUND"):
                    continue
                for target in referenced_targets(value):
                    if target != name and target not in targets:
                        targets.append(target)
            graph.edges.setdefault(name, [])
            for target in targets:
                if target not in graph.edges[name]:
                    graph.edges[name].append(target)
        return graph


def matches(name: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)


def find_paths(graph: Graph, root: str, forbidden: list[str],
               allowed_edges: set[tuple[str, str]]) -> list[list[str]]:
    """Shortest path from root to each forbidden target it reaches."""
    parent: dict[str, str | None] = {root: None}
    queue: deque[str] = deque([root])
    found: list[list[str]] = []
    while queue:
        current = queue.popleft()
        for target in graph.edges.get(current, []):
            if (current, target) in allowed_edges or target in parent:
                continue
            parent[target] = current
            if matches(target, forbidden):
                path = [target]
                while parent[path[-1]] is not None:
                    path.append(parent[path[-1]])  # type: ignore[arg-type]
                found.append(list(reversed(path)))
                continue
            queue.append(target)
    return found


def check(graph: Graph, roots: list[str], forbidden: list[str],
          allowed_edges: set[tuple[str, str]]) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    notes: list[str] = []
    for root in roots:
        if root not in graph.edges:
            errors.append(f"INCONCLUSIVE root {root} is not in the link graph")
            continue
        paths = find_paths(graph, root, forbidden, allowed_edges)
        for path in paths:
            errors.append(f"FAIL {root} reaches {path[-1]}: {' -> '.join(path)}")
        if not paths:
            notes.append(f"{root}: closure holds none of {forbidden}")
    return errors, notes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--root", action="append", required=True,
                        help="target to check, in any spelling (repeatable)")
    parser.add_argument("--forbid", action="append", required=True,
                        help="canonical name or glob such as pulp-view* (repeatable)")
    parser.add_argument("--allow", action="append", default=[],
                        help="edge exempted for a named reason, `from>to` (repeatable)")
    args = parser.parse_args(argv)

    try:
        graph = Graph.parse(args.graph.read_text(encoding="utf-8"))
    except OSError as error:
        print(f"link-closure: INCONCLUSIVE: {error}", file=sys.stderr)
        return 2
    if len(graph.edges) < MIN_GRAPH_TARGETS:
        print(f"link-closure: INCONCLUSIVE: the graph holds {len(graph.edges)} targets, "
              f"fewer than {MIN_GRAPH_TARGETS}; the dump is not a real build graph",
              file=sys.stderr)
        return 2

    allowed: set[tuple[str, str]] = set()
    for edge in args.allow:
        source, _, target = edge.partition(">")
        if not target:
            print(f"link-closure: INCONCLUSIVE: --allow {edge!r} is not from>to",
                  file=sys.stderr)
            return 2
        allowed.add((canonical(source), canonical(target)))

    roots = [canonical(root) for root in args.root]
    errors, notes = check(graph, roots, args.forbid, allowed)
    for source, target in sorted(allowed):
        print(f"link-closure: allowed edge {source} -> {target}")
    for note in notes:
        print(f"link-closure: PASS {note}")
    if errors:
        for error in errors:
            print(f"link-closure: {error}", file=sys.stderr)
        inconclusive = any(error.startswith("INCONCLUSIVE") for error in errors)
        failed = any(error.startswith("FAIL") for error in errors)
        return 1 if failed else (2 if inconclusive else 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
