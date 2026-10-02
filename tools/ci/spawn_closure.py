#!/usr/bin/env python3
"""The executables a test executable runs or loads at run time, from the
CMake codemodel.

A test that spawns another built program (a CLI, a tool, a fixture binary)
or loads a module at run time has a verdict that moves with that program's
code, yet nothing in its own link graph reaches it: the program's path comes
from a `$<TARGET_FILE:x>` compile definition, an ENVIRONMENT entry, or a path
the test computes at run time, and the only build-graph trace is the
`add_dependencies(<test> x)` that makes sure x is built first. Ninja records
that as an order-only (`||`) edge, which carries no content, so a selector
working from build.ninja alone would skip the test when only x changed.

The codemodel's per-target `dependencies` include those manually added
targets. So a test executable's spawn closure is every EXECUTABLE or
MODULE_LIBRARY target reachable from it through `dependencies`, walking only
through executables, modules and utility targets (a utility target can stage
or wrap a runtime program). Linked libraries are not walked: their content
already reaches the executable through the link. The closure is transitive,
because a spawned tool that spawns another tool runs it on the test's behalf.

An `add_dependencies` kept only for build ordering also lands in the closure.
That over-approximates, which costs reuse and never correctness.

Input is the recorded target shape that tools/ci/codemodel_digest.py writes
and the reuse record keeps: `{name: {"type", "artifacts", "dependencies"}}`,
artifacts spelled `<build>/<path>`. Every consumer (the merge-group
affected-tests shadow, the reuse replay, the reuse key) reads spawn edges
through this one function.
"""
from __future__ import annotations

from typing import Iterable, Mapping

BUILD_TOKEN = "<build>/"
# Targets whose artifact runs or loads at run time without being linked.
RUNTIME_TYPES = frozenset({"EXECUTABLE", "MODULE_LIBRARY"})
# Targets the walk passes through on the way to a runtime target.
WALK_TYPES = RUNTIME_TYPES | {"UTILITY"}


def _rel(artifact: str) -> str:
    return artifact[len(BUILD_TOKEN):] if artifact.startswith(BUILD_TOKEN) else artifact


class SpawnIndex:
    """Spawn closures over one codemodel, keyed by build-relative artifact."""

    def __init__(self, targets: Mapping[str, Mapping]) -> None:
        self._targets = targets
        self._by_artifact: dict[str, str] = {}
        for name, target in targets.items():
            if target.get("type") in RUNTIME_TYPES:
                for artifact in target.get("artifacts") or []:
                    self._by_artifact[_rel(artifact)] = name
        self._closures: dict[str, frozenset[str]] = {}

    def closure(self, artifact: str) -> frozenset[str]:
        """Build-relative artifacts of the runtime targets `artifact` runs or
        loads, excluding itself; empty for an artifact the codemodel does not
        describe."""
        artifact = _rel(artifact)
        if artifact in self._closures:
            return self._closures[artifact]
        start = self._by_artifact.get(artifact)
        found: set[str] = set()
        if start is not None:
            seen, stack = {start}, [start]
            while stack:
                for dep in self._targets.get(stack.pop(), {}).get("dependencies") or []:
                    target = self._targets.get(dep)
                    if dep in seen or target is None or target.get("type") not in WALK_TYPES:
                        continue
                    seen.add(dep)
                    stack.append(dep)
                    if target.get("type") in RUNTIME_TYPES:
                        found.update(_rel(a) for a in target.get("artifacts") or [])
        found.discard(artifact)
        self._closures[artifact] = frozenset(found)
        return self._closures[artifact]

    def expand(self, artifacts: Iterable[str]) -> set[str]:
        """`artifacts` (build-relative) plus everything each runs or loads."""
        out = {_rel(a) for a in artifacts}
        for artifact in list(out):
            out |= self.closure(artifact)
        return out
