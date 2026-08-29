#!/usr/bin/env python3
"""Measure what a consumer of one installed Pulp target actually has to take.

Pulp exports ~55 targets from one monorepo, and a project that wants only the
DSP or only the timebase still inherits whatever those targets link. This
script turns that inheritance into numbers: for every installed target, the set
of targets its consumers reach, the third-party archives and system libraries
that land on their link line, and how much public header surface the target
offers in exchange.

The measurement is BUILD-TIME DERIVED. Its input is the target-property graph a
configured CMake build tree writes to `consumption-census-facts.json`, not a
reading of the CMakeLists files: a static approximation of a graph CMake builds
out of generator expressions, aliases and namespaced imports would be a guess
wearing a number's clothes. A build tree is therefore required, and one
configured with `PULP_BUILD_TESTS=ON` (the facts dump lives in the test
manifest, so a tests-off tree writes no facts).

Modes:

    --write         regenerate docs/status/consumption-profiles.json
    --check         regenerate in memory and diff against the committed file
    --cross-check   verify the closure against `cmake --graphviz`, which is
                    CMake's own answer to the same question
    --link-probe    measure a trivial consumer of each target against an
                    installed SDK: configure time, link time, binary size
    --report        print the ranked human summary
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import json_schema_lite  # noqa: E402  (needs the path insert above)

FACTS_NAME = "consumption-census-facts.json"
CENSUS_RELPATH = Path("docs/status/consumption-profiles.json")
SCHEMA_RELPATH = Path("docs/status/consumption-profiles.schema.json")
CENSUS_SCHEMA_VERSION = 1

# Generator-expression heads the link properties actually use. An unknown head
# is an error rather than a silent pass-through: a genex this does not
# understand is a dependency edge measured wrong, and a census that quietly
# mismeasures is worse than one that refuses to run.
GENEX_PASSTHROUGH = {"LINK_ONLY", "TARGET_NAME", "BUILD_INTERFACE"}
GENEX_DROP = {"INSTALL_INTERFACE"}
GENEX_LINK_LIBRARY = "LINK_LIBRARY"
FRAMEWORK_FEATURES = {"FRAMEWORK", "WEAK_FRAMEWORK", "NEEDED_FRAMEWORK", "REEXPORT_FRAMEWORK"}

# Directories whose targets are Pulp's own code. Anything else a build tree
# defines (fetched dependencies, vendored sources) is third-party weight from a
# consumer's point of view even when the target carries a `pulp-` name.
# Switches that decide which CONSUMERS a build tree builds, not what the
# exported targets link. Examples and tests depend on the SDK, never the other
# way round, so they cannot change a closure — verified by generating the
# census from an examples-on and an examples-off tree and comparing. They are
# recorded for provenance but excluded from profile matching, so the gate stays
# live on both the examples-off required lane and the examples-on local one.
BUILD_SCOPE_FEATURES = ("PULP_BUILD_TESTS", "PULP_BUILD_EXAMPLES")

PULP_SOURCE_ROOTS = ("core/", "inspect/", "ship/", "tools/", "apple/", "bindings/", "experimental/")


class CensusError(RuntimeError):
    """A measurement could not be made. Never a partial or guessed result."""


# ── generator expressions ───────────────────────────────────────────────────


def _balanced(text: str) -> bool:
    depth = 0
    index = 0
    while index < len(text):
        if text.startswith("$<", index):
            depth += 1
            index += 2
            continue
        if text[index] == ">":
            depth -= 1
            if depth < 0:
                return False
        index += 1
    return depth == 0


def resolve_genex(entry: str) -> tuple[str, str] | None:
    """Reduce one raw link entry to a ``(kind, name)`` pair.

    ``kind`` is ``"name"`` for something that may name a target and
    ``"framework"`` for an Apple framework. ``None`` means the entry does not
    apply to this build tree (an install-interface-only dependency).
    """
    text = entry.strip()
    for _ in range(16):
        if not text.startswith("$<"):
            break
        if not text.endswith(">") or not _balanced(text):
            raise CensusError(f"unparsable generator expression in link property: {entry!r}")
        head, _, body = text[2:-1].partition(":")
        if head in GENEX_DROP:
            return None
        if head in GENEX_PASSTHROUGH:
            text = body.strip()
            continue
        if head == GENEX_LINK_LIBRARY:
            feature, _, argument = body.partition(",")
            if feature in FRAMEWORK_FEATURES:
                return ("framework", argument.strip())
            raise CensusError(f"unsupported $<LINK_LIBRARY:{feature}> feature in {entry!r}")
        raise CensusError(f"unsupported generator-expression head $<{head}:...> in {entry!r}")
    else:
        raise CensusError(f"generator expression nested past the unwrap limit: {entry!r}")

    if text.startswith("$<"):
        raise CensusError(f"generator expression left unresolved: {entry!r}")
    return ("name", text)


# ── graph ───────────────────────────────────────────────────────────────────


def node_id(kind: str, name: str) -> str:
    return name if kind == "target" else f"{kind}:{name}"


def raw_framework_flag(name: str) -> str | None:
    """The framework a raw `-framework Foo` link flag names, else None."""
    for flag in ("-framework ", "-weak_framework "):
        if name.startswith(flag):
            return name[len(flag):].strip()
    return None


class Graph:
    """The consumer-visible link graph of one configured build tree."""

    def __init__(self, facts: dict):
        self.facts = facts
        self.targets: dict[str, dict] = facts["targets"]
        self.resolution: dict[str, dict] = facts["name_resolution"]
        self.edges: dict[str, list[str]] = {}
        self.node_kind: dict[str, str] = {}
        for name in self.targets:
            self.node_kind[name] = "target"
        self.absorbed_edges: dict[str, list[str]] = {}
        for name, target in self.targets.items():
            self.edges[name] = self._resolve_edges(target["interface_link_libraries"])
            # Object libraries absorbed as $<TARGET_OBJECTS:...> plus build-order
            # dependencies. Neither reaches a consumer's link line, so neither is
            # part of a closure; they exist so the cross-check can account for
            # every edge `cmake --graphviz` draws.
            self.absorbed_edges[name] = sorted(
                set(target.get("absorbed_object_libraries", ()))
                | set(target.get("manually_added_dependencies", ()))
            )

    def _resolve_edges(self, entries: list[str]) -> list[str]:
        out: list[str] = []
        for entry in entries:
            resolved = resolve_genex(entry)
            if resolved is None:
                continue
            kind, name = resolved
            if kind == "framework":
                identifier = node_id("framework", name)
                self.node_kind[identifier] = "framework"
                out.append(identifier)
                continue
            framework = raw_framework_flag(name)
            if framework is not None:
                identifier = node_id("framework", framework)
                self.node_kind[identifier] = "framework"
                out.append(identifier)
                continue
            if name.startswith("/") or name.startswith("\\"):
                # An absolute archive path is machine-specific; the census
                # records the archive, never where this host happened to
                # unpack it.
                identifier = node_id("archive", name.rsplit("/", 1)[-1])
                self.node_kind[identifier] = "archive"
                out.append(identifier)
                continue
            entry_resolution = self.resolution.get(name)
            if entry_resolution is not None:
                real = entry_resolution["target"]
                if real in self.targets:
                    out.append(real)
                    continue
                identifier = node_id("imported", real)
                self.node_kind[identifier] = "imported"
                out.append(identifier)
                continue
            identifier = node_id("system", name)
            self.node_kind[identifier] = "system"
            out.append(identifier)
        # A target may name the same dependency through several genex wrappers.
        return sorted(dict.fromkeys(out))

    def closure(self, root: str) -> set[str]:
        """Every node a consumer of ``root`` reaches, ``root`` included."""
        if root not in self.targets:
            raise CensusError(f"no such target in the build tree: {root}")
        seen = {root}
        stack = [root]
        while stack:
            current = stack.pop()
            for dependency in self.edges.get(current, ()):
                if dependency not in seen:
                    seen.add(dependency)
                    stack.append(dependency)
        return seen

    def accounted_closure(self, root: str) -> set[str]:
        """The closure plus every non-link edge `cmake --graphviz` also draws."""
        seen = {root}
        stack = [root]
        while stack:
            current = stack.pop()
            for dependency in list(self.edges.get(current, ())) + list(
                self.absorbed_edges.get(current, ())
            ):
                if dependency not in seen:
                    seen.add(dependency)
                    stack.append(dependency)
        return seen

    def is_pulp_target(self, name: str) -> bool:
        target = self.targets.get(name)
        if target is None:
            return False
        source_dir = target["source_dir"]
        return bool(source_dir) and source_dir.startswith(PULP_SOURCE_ROOTS)


# ── inputs ──────────────────────────────────────────────────────────────────


def load_facts(build_dir: Path) -> dict:
    path = build_dir / FACTS_NAME
    if not path.is_file():
        raise CensusError(
            f"{path} is missing. The census reads the target graph a configure writes, so "
            f"point --build-dir at a tree configured with PULP_BUILD_TESTS=ON and reconfigure it "
            f"(cmake -S <source> -B {build_dir})."
        )
    facts = json.loads(path.read_text())
    if facts.get("facts_schema") != 1:
        raise CensusError(f"{path} has facts_schema {facts.get('facts_schema')}, expected 1")
    return facts


def find_export_file(build_dir: Path) -> Path:
    candidates = sorted(build_dir.glob("CMakeFiles/Export/*/PulpTargets.cmake"))
    if not candidates:
        raise CensusError(
            f"no generated PulpTargets.cmake under {build_dir}/CMakeFiles/Export — the build tree "
            f"has no Pulp SDK export, so there is no installed public surface to census."
        )
    # A reconfigure that changes the install destination leaves the previous
    # export directory behind. Identical leftovers are harmless; genuinely
    # different ones mean the build tree exports two different SDKs and no
    # closure read from it could be trusted.
    contents = {path.read_text() for path in candidates}
    if len(contents) > 1:
        raise CensusError(
            f"{build_dir} carries {len(candidates)} PulpTargets.cmake exports that disagree; "
            f"reconfigure into a clean build tree before measuring."
        )
    return candidates[0]


def installed_targets(build_dir: Path, graph: Graph) -> list[tuple[str, str]]:
    """Pair every exported ``Pulp::<name>`` with the target that produces it.

    The exported set is read from the export file CMake generates, so it cannot
    drift from what `cmake --install` actually ships.
    """
    text = find_export_file(build_dir).read_text()
    exported = sorted(set(re.findall(r"^add_library\(Pulp::([^\s)]+)", text, re.M)))
    if not exported:
        raise CensusError("the generated PulpTargets.cmake exports no libraries")

    by_export_name: dict[str, str] = {}
    for name, target in graph.targets.items():
        export_name = target.get("export_name") or ""
        if export_name:
            by_export_name.setdefault(export_name, name)

    pairs: list[tuple[str, str]] = []
    unmatched: list[str] = []
    for export_name in exported:
        target = by_export_name.get(export_name)
        if target is None and export_name in graph.targets:
            target = export_name
        if target is None:
            unmatched.append(export_name)
            continue
        pairs.append((export_name, target))
    if unmatched:
        raise CensusError(
            "exported names with no library target in the facts dump: "
            + ", ".join(unmatched)
            + " — the facts dump and the export set disagree, so the census would be incomplete."
        )
    return pairs


def public_header_roots(target: dict, source_root: Path, binary_dir: Path) -> tuple[list[str], int]:
    """Exported include roots inside the source tree, and how many were not.

    Generated and fetched include roots live under the build directory, whose
    NAME is a local choice (`build`, `build-cov`, `build-<ci-key>`). Recording
    them would publish one machine's directory layout and make every other
    build directory read as drift, so they are counted rather than named.
    """
    roots: list[str] = []
    excluded = 0
    for entry in target["interface_include_directories"]:
        resolved = resolve_genex(entry)
        if resolved is None:
            continue
        kind, value = resolved
        if kind != "name":
            continue
        path = Path(value)
        if not path.is_absolute():
            continue
        if path == binary_dir or binary_dir in path.parents:
            excluded += 1
            continue
        try:
            relative = path.relative_to(source_root)
        except ValueError:
            excluded += 1
            continue
        roots.append(relative.as_posix())
    return sorted(dict.fromkeys(roots)), excluded


def count_headers(source_root: Path, roots: list[str]) -> int:
    total = 0
    for root in roots:
        directory = source_root / root
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if path.suffix in (".h", ".hpp") and path.is_file():
                total += 1
    return total


# ── census ──────────────────────────────────────────────────────────────────


def profile_key(facts: dict) -> str:
    system = facts["system_name"].lower()
    processor = facts["system_processor"].lower()
    return f"{system}-{processor}"


def build_profile(build_dir: Path, source_root: Path) -> dict:
    """Measure the one consumption profile this build tree represents."""
    facts = load_facts(build_dir)
    graph = Graph(facts)
    pairs = installed_targets(build_dir, graph)
    binary_dir = Path(facts["binary_dir"])

    rows: dict[str, dict] = {}
    for export_name, target_name in pairs:
        target = graph.targets[target_name]
        closure = graph.closure(target_name)

        pulp_targets = sorted(n for n in closure if graph.is_pulp_target(n))
        third_party = sorted(
            n for n in closure if n in graph.targets and not graph.is_pulp_target(n)
        )
        frameworks = sorted(n.split(":", 1)[1] for n in closure if graph.node_kind.get(n) == "framework")
        imported = sorted(n.split(":", 1)[1] for n in closure if graph.node_kind.get(n) == "imported")
        system_libs = sorted(n.split(":", 1)[1] for n in closure if graph.node_kind.get(n) == "system")
        archives = sorted(n.split(":", 1)[1] for n in closure if graph.node_kind.get(n) == "archive")

        roots, generated_roots = public_header_roots(target, source_root, binary_dir)
        header_count = count_headers(source_root, roots)

        rows[export_name] = {
            "target": target_name,
            "exported_as": f"Pulp::{export_name}",
            "type": target["type"],
            "source_dir": target["source_dir"],
            "closure": {
                "node_count": len(closure),
                "pulp_targets": pulp_targets,
                "third_party_targets": third_party,
            },
            "external_dependencies": {
                "count": (
                    len(frameworks)
                    + len(imported)
                    + len(system_libs)
                    + len(third_party)
                    + len(archives)
                ),
                "third_party_targets": third_party,
                "prebuilt_archives": archives,
                "imported_targets": imported,
                "frameworks": frameworks,
                "system_libraries": system_libs,
            },
            "public_headers": {
                "count": header_count,
                "roots": roots,
                "generated_include_roots": generated_roots,
            },
            "direct_dependencies": sorted(graph.edges.get(target_name, ())),
        }

    ranked = sorted(
        rows.items(),
        key=lambda item: (-item[1]["closure"]["node_count"], item[0]),
    )
    return {
        "key": profile_key(facts),
        "system_name": facts["system_name"],
        "system_processor": facts["system_processor"],
        "features": {
            name: value
            for name, value in facts["features"].items()
            if name not in BUILD_SCOPE_FEATURES
        },
        "build_scope": {
            name: facts["features"][name]
            for name in BUILD_SCOPE_FEATURES
            if name in facts["features"]
        },
        "summary": {
            "installed_target_count": len(rows),
            "ranked_by_closure": [
                {
                    "exported_as": row["exported_as"],
                    "closure_node_count": row["closure"]["node_count"],
                    "public_header_count": row["public_headers"]["count"],
                }
                for _, row in ranked
            ],
        },
        "targets": rows,
    }


DERIVATION = {
    "method": "cmake-target-property-graph",
    "input": f"<build-dir>/{FACTS_NAME}, written by every configure",
    "build_time_derived": True,
    "closure_definition": (
        "Nodes reachable from the target through INTERFACE_LINK_LIBRARIES, the target itself "
        "included. This is what a consumer inherits from "
        "target_link_libraries(app PRIVATE Pulp::<name>); a static library's PRIVATE "
        "dependencies are part of it, because CMake records them for consumers as "
        "$<LINK_ONLY:...>."
    ),
    "limitations": [
        "Each profile is one configured build tree: one platform, one feature set. A tree "
        "that disagrees with a profile's features is measuring a different SDK, and the "
        "drift gate skips rather than compares it.",
        "$<INSTALL_INTERFACE:...> dependencies are dropped and $<BUILD_INTERFACE:...> kept, "
        "so the graph is the build tree's. Where the two name the same library under "
        "different aliases (Pulp::yogacore vs yogacore) the build-tree name is recorded.",
        "Closure counts declared link dependencies, not symbols. It is the weight a consumer "
        "inherits on the link line, not the weight the linker keeps after dead-stripping; "
        "link_probe measures the latter.",
        "Object libraries absorbed as $<TARGET_OBJECTS:...> and build-order dependencies from "
        "add_dependencies() are deliberately not closure members: neither reaches a "
        "consumer's link line.",
        "Public header counts come from each target's exported include directories, so "
        "headers a target ships but does not export are not counted.",
        "Include roots under the build directory (generated headers, fetched dependencies) "
        "are counted, not named, and their headers are not counted: the build directory's "
        "name is a local choice and must not reach a published file.",
    ],
}


def census_document(profiles: dict[str, dict]) -> dict:
    """Wrap one or more measured profiles in the published document."""
    return {
        "$schema": "./consumption-profiles.schema.json",
        "schema": CENSUS_SCHEMA_VERSION,
        "generated_by": "tools/scripts/consumption_census.py",
        "derivation": DERIVATION,
        "profiles": {key: profiles[key] for key in sorted(profiles)},
    }


# ── cross-check ─────────────────────────────────────────────────────────────


def graphviz_closures(source_root: Path, build_dir: Path) -> dict[str, set[str]]:
    """Closures as `cmake --graphviz` computes them, for cross-checking.

    CMake resolves aliases, namespaced imports and generator expressions
    itself, so its graph is an instrument this script does not share any code
    with. Disagreement means this script's genex handling is wrong.
    """
    with tempfile.TemporaryDirectory() as tmp:
        dot_path = Path(tmp) / "pulp.dot"
        subprocess.run(
            ["cmake", f"--graphviz={dot_path}", "-S", str(source_root), "-B", str(build_dir)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        dot = dot_path.read_text()

    names: dict[str, str] = {}
    for match in re.finditer(r'"(node\d+)"\s*\[\s*label\s*=\s*"([^"]*)"', dot):
        names[match.group(1)] = match.group(2).split("\\n")[0]
    edges: dict[str, set[str]] = {}
    for match in re.finditer(r'"(node\d+)"\s*->\s*"(node\d+)"', dot):
        edges.setdefault(match.group(1), set()).add(match.group(2))

    by_name = {}
    for identifier, name in names.items():
        by_name.setdefault(name, identifier)

    def walk(root_name: str) -> set[str]:
        start = by_name[root_name]
        seen = {start}
        stack = [start]
        while stack:
            current = stack.pop()
            for dependency in edges.get(current, ()):
                if dependency not in seen:
                    seen.add(dependency)
                    stack.append(dependency)
        return {normalise_graphviz_name(names[n]) for n in seen}

    return {name: walk(name) for name in by_name}


def bare_name(name: str) -> str:
    """Reduce a node name to something both instruments spell the same way.

    `cmake --graphviz` writes a framework as `-framework Cocoa` when the link
    property held a raw flag and as `Cocoa` when it held
    `$<LINK_LIBRARY:FRAMEWORK,Cocoa>`, and writes prebuilt archives as absolute
    paths. The cross-check is about which nodes are reachable, not about how
    either instrument spells them.
    """
    for prefix in ("-weak_framework ", "-framework ", "-l"):
        if name.startswith(prefix):
            name = name[len(prefix):].strip()
            break
    for prefix in ("framework:", "imported:", "system:", "archive:"):
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    return name.rsplit("/", 1)[-1]


def normalise_graphviz_name(name: str) -> str:
    return bare_name(name)


def cross_check(source_root: Path, build_dir: Path, profile: dict) -> list[str]:
    facts = load_facts(build_dir)
    graph = Graph(facts)
    reference = graphviz_closures(source_root, build_dir)
    problems: list[str] = []
    for export_name, row in sorted(profile["targets"].items()):
        target = row["target"]
        if target not in reference:
            problems.append(f"{target}: absent from the cmake --graphviz graph")
            continue
        mine = {bare_name(n) for n in graph.closure(target)}
        accounted = {bare_name(n) for n in graph.accounted_closure(target)}
        theirs = reference[target]
        unaccounted = theirs - accounted
        invented = mine - theirs
        if unaccounted:
            problems.append(
                f"{target}: cmake --graphviz reaches nodes this census cannot account for: "
                f"{sorted(unaccounted)}"
            )
        if invented:
            problems.append(
                f"{target}: this census reaches nodes cmake --graphviz does not: "
                f"{sorted(invented)}"
            )
    return problems


# ── link probe ──────────────────────────────────────────────────────────────


PROBE_MAIN = "int main() { return 0; }\n"

PROBE_CMAKE = """cmake_minimum_required(VERSION 3.20)
project(pulp_consumption_probe CXX)
find_package(Pulp REQUIRED)
{targets}
"""


ARCHIVE_RE = re.compile(r"(?:^|\s)(/\S+\.a)")
FRAMEWORK_RE = re.compile(r"-framework\s+(\S+)")


def read_link_line(build: Path, target: str) -> dict:
    """Read the link command CMake generated for one probe executable.

    This is the census's third instrument and the only one that looks at the
    INSTALLED export rather than the build tree: it is the literal argument
    list the linker receives when somebody writes
    target_link_libraries(app PRIVATE Pulp::<name>) against a shipped SDK.
    """
    link_txt = build / "CMakeFiles" / f"{target}.dir" / "link.txt"
    if not link_txt.is_file():
        return {"link_line": "unavailable"}
    text = link_txt.read_text()
    archives = sorted(dict.fromkeys(ARCHIVE_RE.findall(text)))
    total = 0
    for archive in archives:
        path = Path(archive)
        if path.is_file():
            total += path.stat().st_size
    return {
        "link_line_archives": len(archives),
        "link_line_archive_bytes": total,
        "link_line_frameworks": len(set(FRAMEWORK_RE.findall(text))),
    }


def cmake_version(cmake: str) -> str:
    result = subprocess.run([cmake, "--version"], capture_output=True, text=True)
    first = result.stdout.splitlines()[0] if result.stdout else ""
    return first.replace("cmake version ", "").strip()


def link_probe(sdk_prefix: Path, profile: dict, build_type: str) -> dict:
    """Configure and link a trivial consumer of each exported target.

    Closure counts what a consumer declares. This counts what the toolchain
    then does with it: how long `find_package` plus one link takes, and how
    large a binary that calls nothing at all still ends up.
    """
    cmake = shutil.which("cmake")
    if cmake is None:
        raise CensusError("cmake not found on PATH")
    exported = sorted(profile["targets"])
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "main.cpp").write_text(PROBE_MAIN)
        blocks = []
        for export_name in exported:
            safe = re.sub(r"[^A-Za-z0-9_]", "_", export_name)
            blocks.append(
                f"add_executable(probe_{safe} EXCLUDE_FROM_ALL main.cpp)\n"
                f"target_link_libraries(probe_{safe} PRIVATE Pulp::{export_name})"
            )
        (root / "CMakeLists.txt").write_text(PROBE_CMAKE.format(targets="\n".join(blocks)))

        build = root / "build"
        started = time.monotonic()
        configure = subprocess.run(
            [
                cmake,
                "-S",
                str(root),
                "-B",
                str(build),
                f"-DCMAKE_BUILD_TYPE={build_type}",
                f"-DCMAKE_PREFIX_PATH={sdk_prefix}",
            ],
            capture_output=True,
            text=True,
        )
        configure_seconds = time.monotonic() - started
        if configure.returncode != 0:
            raise CensusError(
                "the trivial-consumer probe project failed to configure against "
                f"{sdk_prefix}:\n{configure.stdout}\n{configure.stderr}"
            )

        results: dict[str, dict] = {}
        for export_name in exported:
            safe = re.sub(r"[^A-Za-z0-9_]", "_", export_name)
            link_line = read_link_line(build, f"probe_{safe}")
            started = time.monotonic()
            built = subprocess.run(
                [cmake, "--build", str(build), "--target", f"probe_{safe}"],
                capture_output=True,
                text=True,
            )
            elapsed = time.monotonic() - started
            if built.returncode != 0:
                results[export_name] = {"status": "link-failed", "detail": built.stderr[-2000:]}
                continue
            binary = build / f"probe_{safe}"
            results[export_name] = {
                "status": "ok",
                "link_seconds": round(elapsed, 3),
                "binary_bytes": binary.stat().st_size if binary.is_file() else None,
                **link_line,
            }
    return {
        "status": "measured",
        "measured_on": {
            "system": platform.system(),
            "machine": platform.machine(),
            "cmake": cmake_version(cmake),
        },
        "sdk_prefix_build_type": build_type,
        "configure_seconds_all_targets": round(configure_seconds, 3),
        "measured_target_count": len(results),
        "note": (
            "Host-measured wall-clock and byte counts. They are not compared by the drift gate: "
            "they move with the machine, the toolchain and the disk cache, so a CI difference "
            "would say nothing about the SDK. A graph-only regeneration carries this block "
            "forward unchanged, so it is a snapshot that can predate the closures beside it; "
            "re-run with --link-probe to refresh it."
        ),
        "targets": results,
    }


# ── entry points ────────────────────────────────────────────────────────────


def validate_schema(census: dict, schema_path: Path) -> list[str]:
    if not schema_path.is_file():
        raise CensusError(f"{schema_path} is missing; the census has no schema to validate against")
    schema = json.loads(schema_path.read_text())
    return list(json_schema_lite.validate(census, schema))


def render(document: dict) -> str:
    return json.dumps(document, indent=2, sort_keys=False) + "\n"


def diffable(profile: dict) -> dict:
    """The part of a profile the drift gate compares.

    Host measurements and the build-scope switches are provenance, not
    findings: comparing them would fail a build tree that measured the same
    SDK on a different machine or with examples enabled.
    """
    return {
        key: value
        for key, value in profile.items()
        if key not in ("link_probe", "build_scope")
    }


def report(profile: dict) -> str:
    lines = [
        f"profile={profile['key']} "
        f"targets={profile['summary']['installed_target_count']}",
        f"{'exported':34s} {'closure':>7s} {'external':>9s} {'headers':>7s}",
    ]
    for entry in profile["summary"]["ranked_by_closure"]:
        row = profile["targets"][entry["exported_as"].removeprefix("Pulp::")]
        lines.append(
            f"{entry['exported_as']:34s} "
            f"{entry['closure_node_count']:7d} "
            f"{row['external_dependencies']['count']:9d} "
            f"{entry['public_header_count']:7d}"
        )
    return "\n".join(lines)


def describe_drift(committed: dict, current: dict) -> list[str]:
    """Name what changed, so the failure says more than "they differ"."""
    lines: list[str] = []
    old_targets = committed.get("targets", {})
    new_targets = current["targets"]
    for name in sorted(set(new_targets) - set(old_targets)):
        lines.append(f"new exported target: Pulp::{name}")
    for name in sorted(set(old_targets) - set(new_targets)):
        lines.append(f"exported target gone: Pulp::{name}")
    for name in sorted(set(old_targets) & set(new_targets)):
        if old_targets[name] == new_targets[name]:
            continue
        old_count = old_targets[name].get("closure", {}).get("node_count")
        new_count = new_targets[name]["closure"]["node_count"]
        if old_count != new_count:
            lines.append(f"Pulp::{name}: closure {old_count} -> {new_count}")
        else:
            lines.append(f"Pulp::{name}: closure detail changed")
    if committed.get("features") != current.get("features"):
        lines.append("the profile's feature set changed")
    return lines or ["the profile differs from the build tree"]


def load_document(census_path: Path, schema_path: Path) -> dict:
    if not census_path.is_file():
        raise CensusError(f"{census_path} is missing")
    document = json.loads(census_path.read_text())
    problems = validate_schema(document, schema_path)
    if problems:
        raise CensusError(
            f"{census_path} violates its schema: " + "; ".join(problems[:5])
        )
    return document


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--source-root", type=Path, default=None)
    parser.add_argument("--census", type=Path, default=None)
    parser.add_argument("--schema", type=Path, default=None)
    parser.add_argument("--write", action="store_true", help="rewrite this host's profile")
    parser.add_argument("--check", action="store_true", help="diff against the committed profile")
    parser.add_argument("--cross-check", action="store_true", help="verify against cmake --graphviz")
    parser.add_argument("--report", action="store_true", help="print the ranked summary")
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="validate the committed census against its schema, without a build tree",
    )
    parser.add_argument("--link-probe", action="store_true")
    parser.add_argument("--sdk-prefix", type=Path, default=None)
    parser.add_argument("--build-type", default="Release")
    return parser.parse_args(argv)


def run_validate_only(census_path: Path, schema_path: Path) -> int:
    """Schema-check the committed document without measuring anything.

    This needs no build tree, so it still runs on a platform whose profile the
    census does not record and whose drift check therefore skips.
    """
    document = load_document(census_path, schema_path)
    profiles = document["profiles"]
    print(
        "consumption_census_schema_valid=true "
        f"profiles={len(profiles)} keys={','.join(sorted(profiles))}"
    )
    return 0


def run_check(profile: dict, census_path: Path, schema_path: Path, build_dir: Path) -> int:
    # A census that is missing or invalid is a broken artifact, not a broken
    # instrument, so it reports as drift rather than as "could not measure".
    try:
        document = load_document(census_path, schema_path)
    except CensusError as error:
        print(f"consumption_census: {error}", file=sys.stderr)
        return 1
    key = profile["key"]
    recorded = document["profiles"].get(key)
    if recorded is None:
        print(
            f"consumption_census_skipped reason=profile-not-recorded host={key} "
            f"recorded={','.join(sorted(document['profiles']))}"
        )
        return 77
    if recorded["features"] != profile["features"]:
        differing = sorted(
            name
            for name in set(recorded["features"]) | set(profile["features"])
            if recorded["features"].get(name) != profile["features"].get(name)
        )
        print(f"consumption_census_skipped reason=features differing={','.join(differing)}")
        return 77
    if diffable(recorded) != diffable(profile):
        print("consumption_census: the census no longer matches the build tree", file=sys.stderr)
        for line in describe_drift(recorded, profile):
            print(f"  {line}", file=sys.stderr)
        print(
            "  regenerate with: python3 tools/scripts/consumption_census.py "
            f"--build-dir {build_dir} --write",
            file=sys.stderr,
        )
        return 1
    print(
        f"consumption_census_verified=true targets={profile['summary']['installed_target_count']} "
        f"profile={key}"
    )
    return 0


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    source_root = (args.source_root or Path(__file__).resolve().parents[2]).resolve()
    census_path = args.census or (source_root / CENSUS_RELPATH)
    schema_path = args.schema or (source_root / SCHEMA_RELPATH)
    build_dir = args.build_dir.resolve()

    try:
        if args.validate_only:
            return run_validate_only(census_path, schema_path)

        profile = build_profile(build_dir, source_root)

        if args.cross_check:
            problems = cross_check(source_root, build_dir, profile)
            if problems:
                print("consumption_census: cross-check FAILED", file=sys.stderr)
                for problem in problems:
                    print(f"  {problem}", file=sys.stderr)
                return 1
            print(f"consumption_census_cross_check=agreed targets={len(profile['targets'])}")

        if args.check:
            return run_check(profile, census_path, schema_path, build_dir)

        # Profiles other than this host's are carried through untouched: a
        # macOS run must not silently delete a Linux profile it cannot measure.
        profiles: dict[str, dict] = {}
        if census_path.is_file():
            try:
                profiles = dict(load_document(census_path, schema_path)["profiles"])
            except CensusError:
                # Regenerating over a census that no longer validates is the
                # documented repair path, so a rewrite starts from scratch.
                profiles = {}

        if args.link_probe:
            if args.sdk_prefix is None:
                raise CensusError("--link-probe requires --sdk-prefix pointing at an installed SDK")
            profile["link_probe"] = link_probe(args.sdk_prefix.resolve(), profile, args.build_type)
        else:
            previous = profiles.get(profile["key"], {}).get("link_probe")
            if previous is not None:
                profile["link_probe"] = previous

        profiles[profile["key"]] = profile
        document = census_document(profiles)

        problems = validate_schema(document, schema_path)
        if problems:
            print(
                "consumption_census: the generated census violates its own schema",
                file=sys.stderr,
            )
            for problem in problems:
                print(f"  {problem}", file=sys.stderr)
            return 2

        if args.report:
            print(report(profile))

        if args.write:
            census_path.write_text(render(document))
            print(
                f"consumption_census_written={census_path} "
                f"profile={profile['key']} targets={len(profile['targets'])}"
            )
            return 0

        if not (args.report or args.cross_check):
            print(render(document), end="")
        return 0
    except CensusError as error:
        print(f"consumption_census: {error}", file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as error:
        print(f"consumption_census: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
