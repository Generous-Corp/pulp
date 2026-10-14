#!/usr/bin/env python3
"""Check that the Rust CLI cargo step is an incremental Ninja edge.

`experimental/pulp-rs/CMakeLists.txt` bridges `cargo build` into the CMake
graph. When that bridge is an `add_custom_target(... ALL COMMAND cargo ...)`
it has no real output, so Ninja runs cargo on every build, including a no-op
one. The bridge is instead an `add_custom_command` whose outputs are the
binaries, whose inputs are the Cargo manifests plus Cargo's own dep-info file,
and which Ninja can therefore skip.

This check reads the generated build.ninja and fails if the cargo edge has
lost any of the properties that make it skippable:

  * it writes the Cargo binary (not a symbolic `CMakeFiles/pulp-rust-cli`
    output that never exists and so is always dirty);
  * it declares a depfile (Cargo's dep-info, which lists every Rust source);
  * it lists Cargo.toml and Cargo.lock as inputs (dep-info omits them);
  * its `cargo build` runs with --locked, so the build uses the committed
    Cargo.lock exactly instead of rewriting it or resolving new crate versions.

Exit codes: 0 pass, 1 fail, 77 skip (no build.ninja, i.e. not a Ninja build,
or the Rust CLI is not part of this build).
"""

from __future__ import annotations

import argparse
import shlex
import sys
from pathlib import Path

CARGO_DESC = "Building pulp-rs (Rust CLI)"
SKIP = 77


def parse_edges(text: str) -> list[dict]:
    """Return build edges as dicts of outputs, inputs, and edge variables."""
    edges: list[dict] = []
    current: dict | None = None
    for raw in text.splitlines():
        if raw.startswith("build "):
            head = raw[len("build "):]
            outs, _, rest = head.partition(": ")
            outs = outs.split(" | ")[0].split()
            parts = rest.split()
            rule = parts[0] if parts else ""
            ins = [p for p in parts[1:] if p not in ("|", "||")]
            current = {"outputs": outs, "rule": rule, "inputs": ins, "vars": {}}
            edges.append(current)
        elif current is not None and raw.startswith("  ") and " = " in raw:
            key, _, value = raw.strip().partition(" = ")
            current["vars"][key] = value
        elif raw.strip() == "" or not raw.startswith(" "):
            current = None
    return edges


def check(text: str) -> list[str]:
    """Return a list of problems; empty means the edge is incremental."""
    edges = [e for e in parse_edges(text)
             if e["vars"].get("DESC", "").startswith(CARGO_DESC)]
    if len(edges) != 1:
        return [f"expected exactly one cargo edge, found {len(edges)}"]
    edge = edges[0]
    problems = []
    if any("CMakeFiles/pulp-rust-cli" in out for out in edge["outputs"]):
        problems.append(
            "cargo edge writes the symbolic CMakeFiles/pulp-rust-cli output, "
            "so it is always dirty and cargo runs on every build")
    if not any(Path(out).name.split(".")[0] == "pulp" for out in edge["outputs"]):
        problems.append("cargo edge does not declare the Cargo binary as an output")
    if "depfile" not in edge["vars"]:
        problems.append("cargo edge has no depfile, so Rust source edits are untracked")
    for manifest in ("Cargo.toml", "Cargo.lock"):
        if not any(Path(i).name == manifest for i in edge["inputs"]):
            problems.append(f"cargo edge does not depend on {manifest}")
    if not cargo_build_is_locked(edge["vars"].get("COMMAND", "")):
        problems.append(
            "cargo edge runs `cargo build` without --locked, so a build can "
            "rewrite Cargo.lock or resolve crate versions nobody reviewed")
    return problems


def cargo_build_is_locked(command: str) -> bool:
    """True when every `cargo build` step in the edge command passes --locked."""
    steps = [step.strip() for step in command.split("&&")]
    builds = []
    for step in steps:
        try:
            words = shlex.split(step)
        except ValueError:
            words = step.split()
        for index, word in enumerate(words[:-1]):
            if Path(word).name.split(".")[0] == "cargo" and words[index + 1] == "build":
                builds.append(words[index + 2:])
                break
    return bool(builds) and all("--locked" in args or "--frozen" in args for args in builds)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    ninja = args.build_dir / "build.ninja"
    if not ninja.is_file():
        print(f"SKIP: {ninja} not found (not a Ninja build)")
        return SKIP
    text = ninja.read_text(encoding="utf-8", errors="replace")
    if CARGO_DESC not in text:
        print("SKIP: the Rust CLI cargo step is not part of this build")
        return SKIP
    problems = check(text)
    for problem in problems:
        print(f"FAIL: {problem}")
    if not problems:
        print("PASS: the cargo edge is incremental (real outputs, depfile, manifests) and --locked")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
