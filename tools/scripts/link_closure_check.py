#!/usr/bin/env python3
"""Check that a set of Pulp targets does not reach a forbidden set through links.

Input is a link graph written at generate time by
test/cmake/link_closure_graph.cmake: one record per target (every buildsystem
target, plus the imported targets, aliases and object libraries they name),
`name<TAB>type<TAB>LINK_LIBRARIES<TAB>INTERFACE_LINK_LIBRARIES<TAB>SOURCES<TAB>REFS`
(REFS: the words CMake resolved to a target; absent in older dumps),
with the values as CMake stored them (generator expressions unevaluated). An
edge is any word of those values that names a node, so a link, a
`$<TARGET_OBJECTS:objs>` source and an imported target are all followed, and a
raw `libname.a` / `name.lib` path counts as the target `name`.

The walk follows both properties from every root, through every Pulp target it
reaches, so a forbidden target that sits behind an intermediate archive is found
as well as one linked directly. A PRIVATE link of a static library is recorded as
`$<LINK_ONLY:dep>`; it is followed too, because the dependency still lands in the
final link, which is the failure this check exists to catch. Names are
normalised so `pulp::x`, `Pulp::x` and `pulp-x` are one target.

A root may name an `--allow` edge (`from>to`) for a dependency the layering
still owes; every allowed edge is printed, so an exemption never goes unseen.

A graph that cannot be trusted is never a PASS: a record with fewer than four
fields (a truncated dump) and a reachable target-shaped name (`pulp-*` name, a name the writer's REFS field or a `$<TARGET_OBJECTS:x>`-style
expression names as a target) that
has no record are both INCONCLUSIVE. Paths, flags and
generator-expression tokens are not target-shaped, so they are not held against
the graph.

Exit codes: 0 no root reaches a forbidden target, 1 a root does (each path is
printed), 2 the input could not be read, the graph holds a malformed record, a
root is absent, a reachable target has no record, or the graph is too small to
be a real one.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import sys
from collections import deque
from pathlib import Path

MIN_GRAPH_TARGETS = 20
WORD_RE = re.compile(r"[A-Za-z0-9_+.@/-]+")
# Only Pulp's own names are inferred to be targets; a third-party `ns::name` may be an
# imported target that CMake keeps in a directory scope the writer cannot see, so the
# writer's REFS field is what names the rest.
# Third-party package namespaces that are leaf libraries to Pulp's layering: they never
# link a Pulp target, so a directory-scoped import of one the writer cannot see is safe
# to leave uninspected. Any other unseen `ns::name` is INCONCLUSIVE (or `--external-ok`).
LEAF_EXTERNAL_DEFAULT = ["threads::*", "skia::*", "v8::*", "sdl3::*", "catch2::*"]
EXTERNAL_NS_RE = re.compile(r"^[a-z0-9_+-]+::[a-z0-9_+:-]+$")
TARGET_SHAPED_RE = re.compile(r"^pulp-[a-z0-9_+-]+$")
ORDINARY_TARGET_RE = re.compile(r"^[a-z_][a-z0-9_+.-]*$")
SOURCE_SUFFIX_RE = re.compile(r"\.(?:c|cc|cpp|cxx|m|mm|h|hh|hpp|hxx|swift|rs)$")
SYSTEM_LINK_NAMES = {"atomic", "c", "dl", "dwmapi", "gcc", "gcc_s", "m", "pthread",
                     "rt", "stdc++"}
GENERATOR_TOKENS = {"build_interface", "config", "install_interface", "link_only",
                   "target_objects", "target_file", "target_file_base_name",
                   "target_file_name", "target_file_dir", "target_linker_file"}
EXPLICIT_TARGET_RE = re.compile(
    r"\$<TARGET_(?:OBJECTS|FILE|FILE_DIR|FILE_NAME|LINKER_FILE|SONAME_FILE|PROPERTY|"
    r"NAME_IF_EXISTS|BUNDLE_DIR):([A-Za-z0-9_+.@:-]+)", re.IGNORECASE)
ARCHIVE_RE = re.compile(r"^(?:lib)?(.+?)\.(?:a|so|dylib|lib|dll)(?:\.[0-9.]+)?$")


def canonical(name: str) -> str:
    """`pulp::view-core`, `Pulp::view-core` and `pulp-view-core` are one target."""
    lowered = name.lower()
    if lowered.startswith("pulp::"):
        return "pulp-" + lowered[len("pulp::"):]
    return lowered


def referenced_targets(value: str) -> list[str]:
    """Every name a property value may refer to, generator syntax stripped."""
    seen: list[str] = []

    def add(name: str) -> None:
        name = canonical(name)
        if name and name not in seen:
            seen.append(name)

    for word in WORD_RE.findall(value.replace("::", "@NS@")):
        word = word.replace("@NS@", "::")
        add(word)
        base = word.rsplit("/", 1)[-1]
        if base != word:
            add(base)
        archive = ARCHIVE_RE.match(base)
        if archive:
            add(archive.group(1))
            add(base[:-len(base.rsplit(".", 1)[-1]) - 1])
    return seen


class Graph:
    def __init__(self) -> None:
        self.edges: dict[str, list[str]] = {}
        self.types: dict[str, str] = {}
        self.malformed: list[str] = []
        # Names a generator expression names as a CMake target, whatever their spelling.
        self.explicit: set[str] = set()
        self.link_edges: set[tuple[str, str]] = set()
        self.archive_derived: set[str] = set()
        self.path_derived: set[str] = set()
        self.opaque: set[str] = set()
        self.display_names: dict[str, str] = {}

    @classmethod
    def parse(cls, text: str) -> "Graph":
        graph = cls()
        for line in text.splitlines():
            if not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) < 4 or not fields[0].strip():
                graph.malformed.append(line.strip()[:80])
                continue
            name = canonical(fields[0].strip())
            graph.display_names[name] = name
            if len(fields) >= 6:
                # The writer lists the words CMake resolved to a target.
                graph.explicit.update(canonical(w) for w in re.split(r"[;\s]+", fields[5]) if w)
            graph.types[name] = fields[1].strip()
            if graph.types[name] == "IMPORTED_DIRECTORY_SCOPED":
                graph.opaque.add(name)
            targets: list[str] = []
            for field_index, value in enumerate(fields[2:5]):
                if value.endswith("-NOTFOUND"):
                    continue
                graph.explicit.update(canonical(m) for m in EXPLICIT_TARGET_RE.findall(value))
                raw_words = WORD_RE.findall(value)
                for raw_index, raw in enumerate(raw_words):
                    base = raw.rsplit("/", 1)[-1]
                    if base != raw:
                        graph.path_derived.update({canonical(raw), canonical(base)})
                    if raw_index and raw_words[raw_index - 1].lower() == "-framework":
                        graph.path_derived.add(canonical(raw))
                    archive = ARCHIVE_RE.match(base)
                    if archive:
                        graph.archive_derived.update({canonical(archive.group(1)),
                                                      canonical(base),
                                                      canonical(base.rsplit(".", 1)[0])})
                for target in referenced_targets(value):
                    if target != name and target not in targets:
                        targets.append(target)
                    if field_index < 2:
                        graph.link_edges.add((name, target))
            graph.edges.setdefault(name, [])
            for target in targets:
                if target not in graph.edges[name]:
                    graph.edges[name].append(target)
        return graph

    @classmethod
    def from_codemodel(cls, build_dir: Path, padding_text: str = "") -> "Graph":
        """Read CMake's generated target model, including local imports."""
        reply_dir = build_dir / ".cmake" / "api" / "v1" / "reply"
        codemodels = sorted(reply_dir.glob("codemodel-v2-*.json"),
                            key=lambda path: path.stat().st_mtime_ns)
        if not codemodels:
            raise ValueError(f"CMake codemodel is not available in {reply_dir}")
        model = json.loads(codemodels[-1].read_text(encoding="utf-8"))
        version = model.get("version", {})
        if (version.get("major"), version.get("minor", 0)) < (2, 9):
            raise ValueError("unsupported CMake codemodel version "
                             f"{version.get('major')}.{version.get('minor')}; "
                             "dependency records are incomplete")
        configuration = model["configurations"][0]
        entries = configuration.get("targets", []) + configuration.get("abstractTargets", [])
        counts: dict[str, int] = {}
        for entry in entries:
            display = canonical(entry["name"])
            counts[display] = counts.get(display, 0) + 1
        names: dict[str, str] = {}
        for entry in entries:
            display = canonical(entry["name"])
            identity = display
            if counts[display] > 1:
                suffix = hashlib.sha1(entry["id"].encode()).hexdigest()[:10]
                identity = f"{display}@{suffix}"
            names[entry["id"]] = identity
        graph = cls()
        for entry in entries:
            target_path = reply_dir / entry["jsonFile"]
            target = json.loads(target_path.read_text(encoding="utf-8"))
            name = names.get(target.get("id"), canonical(target["name"]))
            graph.display_names[name] = canonical(target["name"])
            graph.types[name] = target.get("type", "")
            graph.edges.setdefault(name, [])
            for property_name in ("linkLibraries", "interfaceLinkLibraries",
                                  "compileDependencies", "interfaceCompileDependencies",
                                  "objectDependencies"):
                for dependency in target.get(property_name, []):
                    dependency_id = dependency.get("id")
                    dependency_name = (names.get(dependency_id)
                                       if dependency_id else dependency.get("fragment"))
                    if not dependency_name:
                        continue
                    dependency_name = canonical(dependency_name)
                    for target_name in referenced_targets(dependency_name):
                        if target_name != name and target_name not in graph.edges[name]:
                            graph.edges[name].append(target_name)
                        if dependency_id:
                            graph.explicit.add(target_name)
                            graph.link_edges.add((name, target_name))
            for source in target.get("sources", []):
                source_name = source.get("path")
                if source_name:
                    graph.edges[name].append(canonical(source_name))
        # Preserve the fixture's padding records and any older writer-only
        # records while preferring codemodel edges for targets it describes.
        if padding_text:
            padding = cls.parse(padding_text)
            for name, edges in padding.edges.items():
                if name not in graph.edges or not graph.edges[name]:
                    graph.edges[name] = edges
                    graph.types[name] = padding.types.get(name, "")
                    graph.display_names[name] = padding.display_names.get(name, name)
        return graph

    def display(self, name: str) -> str:
        return self.display_names.get(name, name)

    def resolve_roots(self, requested: str) -> list[str]:
        canonical_name = canonical(requested)
        return [name for name in self.edges
                if name == canonical_name or self.display(name) == canonical_name]


def matches(name: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)


def looks_like_unresolved_target(name: str) -> bool:
    """Treat an unseen ordinary target/alias as unsafe, but ignore source paths and flags."""
    return (name not in SYSTEM_LINK_NAMES and name not in GENERATOR_TOKENS
            and ORDINARY_TARGET_RE.fullmatch(name) is not None
            and SOURCE_SUFFIX_RE.search(name) is None)


def find_paths(graph: Graph, root: str, forbidden: list[str],
               allowed_edges: set[tuple[str, str]],
               unresolved: list[str] | None = None,
               external: list[str] | None = None) -> list[list[str]]:
    """Shortest path from root to each forbidden target it reaches.

    A reachable, target-shaped name with no record is appended to `unresolved`:
    its own closure was never inspected.
    """
    parent: dict[str, str | None] = {root: None}
    queue: deque[str] = deque([root])
    found: list[list[str]] = []
    while queue:
        current = queue.popleft()
        for target in graph.edges.get(current, []):
            if ((graph.display(current), graph.display(target)) in allowed_edges
                    or target in parent):
                continue
            parent[target] = current
            if matches(graph.display(target), forbidden):
                path = [target]
                while parent[path[-1]] is not None:
                    path.append(parent[path[-1]])  # type: ignore[arg-type]
                found.append(list(reversed(path)))
                continue
            if target in graph.opaque:
                if external is not None and EXTERNAL_NS_RE.match(target):
                    external.append(target)
                elif unresolved is not None:
                    unresolved.append(target)
                continue
            if target not in graph.edges:
                if external is not None and EXTERNAL_NS_RE.match(target):
                    external.append(target)
                elif (unresolved is not None
                      and (TARGET_SHAPED_RE.match(graph.display(target))
                           or target in graph.explicit
                           or ((current, target) in graph.link_edges
                               and target not in graph.archive_derived
                               and target not in graph.path_derived
                               and looks_like_unresolved_target(target)))):
                    unresolved.append(target)
            queue.append(target)
    return found


def check(graph: Graph, roots: list[str], forbidden: list[str],
          allowed_edges: set[tuple[str, str]],
          external_ok: list[str] | None = None) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    notes: list[str] = []
    ok_patterns = [canonical(p) for p in LEAF_EXTERNAL_DEFAULT + (external_ok or [])]
    for root in roots:
        root_candidates = graph.resolve_roots(root)
        if not root_candidates:
            errors.append(f"INCONCLUSIVE root {root} is not in the link graph")
            continue
        if len(root_candidates) > 1:
            errors.append(f"INCONCLUSIVE root {root} is ambiguous across directory scopes: "
                          f"{root_candidates}")
            continue
        root_id = root_candidates[0]
        unresolved: list[str] = []
        external: list[str] = []
        paths = find_paths(graph, root_id, forbidden, allowed_edges, unresolved, external)
        leaf = [e for e in dict.fromkeys(external)
                if matches(graph.display(e), ok_patterns)]
        for target in dict.fromkeys(external):
            if target not in leaf:
                errors.append(f"INCONCLUSIVE root {root} reaches imported target "
                              f"{graph.display(target)}, "
                              "which has no record (a directory-scoped import the writer "
                              "cannot see); its closure was not inspected. If it is a leaf "
                              f"third-party library, pass --external-ok '{target}'")
        if leaf:
            notes.append(f"{root}: NOT INSPECTED (leaf third-party imported targets with no "
                         f"record): {leaf}")
        for target in unresolved:
            errors.append(f"INCONCLUSIVE root {root} reaches {graph.display(target)}, which has no "
                          "record in the link graph, so its closure was not inspected")
        for path in paths:
            shown = [graph.display(name) for name in path]
            errors.append(f"FAIL {root} reaches {shown[-1]}: {' -> '.join(shown)}")
        if not paths and not unresolved and not any(
                e.startswith("INCONCLUSIVE root " + root + " ") for e in errors):
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
    parser.add_argument("--external-ok", action="append", default=[],
                        help="leaf third-party imported target (or glob) that may stay "
                             "uninspected (repeatable)")
    args = parser.parse_args(argv)

    try:
        raw_graph = args.graph.read_text(encoding="utf-8")
        marker = args.graph.with_suffix(".codemodel")
        if marker.is_file():
            build_dir = Path(marker.read_text(encoding="utf-8").strip())
            try:
                graph = Graph.from_codemodel(build_dir, raw_graph)
            except ValueError as error:
                if not ("codemodel is not available" in str(error)
                        or "unsupported CMake codemodel version" in str(error)):
                    raise
                # File API replies are produced after the first configure, and
                # older supported CMake versions lack dependency records. The
                # native writer remains the safe fallback; opaque imports still
                # fail closed until a capable codemodel is available.
                graph = Graph.parse(raw_graph)
        else:
            graph = Graph.parse(raw_graph)
    except OSError as error:
        print(f"link-closure: INCONCLUSIVE: {error}", file=sys.stderr)
        return 2
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"link-closure: INCONCLUSIVE: malformed CMake codemodel: {error}",
              file=sys.stderr)
        return 2
    if graph.malformed:
        print(f"link-closure: INCONCLUSIVE: {len(graph.malformed)} malformed graph "
              f"record(s), the dump is truncated or corrupt: {graph.malformed[:3]}",
              file=sys.stderr)
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
    forbidden = [canonical(pattern) for pattern in args.forbid]
    errors, notes = check(graph, roots, forbidden, allowed, args.external_ok)
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
