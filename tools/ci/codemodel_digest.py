#!/usr/bin/env python3
"""Per-target digests of the CMake codemodel, for offline reuse replay.

A build can skip a target only if everything CMake would hand the compiler
and linker for it is unchanged. The file-API codemodel reply
(`<build>/.cmake/api/v1/reply`, requested by the gate's configure) states
exactly that per target: its source list, each compile group's flags,
defines and include paths, its link command fragments, and the targets it
depends on. Recorded per job, a replay can tell which targets a change could
not have touched without re-running CMake.

Per target this records sha256 digests (32 hex characters) of:

    sources   the source paths and which compile group each belongs to
    compile   every compile group: language, flags, defines, includes, sysroot
    link      the link language and command fragments
    tests     the ctest registrations whose command runs this target's
              artifact (command and properties), or null for none
    commit_bound
              the target was declared commit-bound by the build
              (<build>/pulp-commit-bound/, written by
              `_pulp_declare_commit_bound`) or depends on such a target;
              `commit_bound_declared` is "unavailable" when the build wrote
              no declarations
    generated the CONTENT of every build-tree file the target compiles (a
              generated source) or included when it compiled (Ninja's
              dependency log), or null for none: a configure_file output
              keeps its path when VERSION moves, and a generated marker
              source carries an embedded build identity. Where the headers
              a target included are unknown (no Ninja dependency log, or a
              compiled target the log never recorded) the part is
              `unknown:<nonce>`, which never equals another record's, so an
              input nobody read cannot pass as unchanged. Every digest also
              covers the schema, so digests of two schema versions never match
    digest    all of the above, the target type and its dependency names

Paths under the build and source roots are written as `<build>/` and `<src>/`
so two checkouts or VMs with the same configuration give the same digests.
Only the listed fields are digested: backtrace indices, which move when an
unrelated line of CMake moves, are left out.

    codemodel_digest.py --build-dir B --source-root S [--ctest-json J] --out F
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

SCHEMA = "pulp-codemodel-digest/v2"
REPLY = Path(".cmake") / "api" / "v1" / "reply"
DIGEST_HEX = 32


class CodemodelError(RuntimeError):
    pass


def _digest(value: Any) -> str:
    blob = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()[:DIGEST_HEX]


def _roots(builds: list[str], sources: list[str]) -> list[tuple[str, str]]:
    """Every spelling of each root (as given, as CMake recorded it, and
    resolved: macOS reaches /tmp through /private/tmp)."""
    pairs = set()
    for paths, token in ((builds, "<build>"), (sources, "<src>")):
        for p in paths:
            if p:
                pairs |= {(os.path.normpath(p), token), (os.path.realpath(p), token)}
    # Longest first, and the build root before an equal-length source root:
    # the build directory usually lives inside the checkout.
    return sorted(pairs, key=lambda r: (-len(r[0]), r[1] != "<build>"))


def _normalise(value: Any, roots: list[tuple[str, str]]) -> Any:
    if isinstance(value, str):
        for real, token in roots:
            value = value.replace(real, token)
        return value
    if isinstance(value, list):
        return [_normalise(v, roots) for v in value]
    if isinstance(value, dict):
        return {k: _normalise(v, roots) for k, v in value.items()}
    return value


def load_reply(build_dir: Path) -> tuple[dict[str, dict], dict]:
    """(target id -> target record, codemodel paths) from the newest index."""
    reply = build_dir / REPLY
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise CodemodelError(f"no CMake file-API reply in {reply}")
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    ref = (index.get("reply") or {}).get("codemodel-v2")
    if not ref:
        raise CodemodelError("the file-API reply holds no codemodel-v2")
    model = json.loads((reply / ref["jsonFile"]).read_text(encoding="utf-8"))
    configs = model.get("configurations") or []
    if len(configs) != 1:
        raise CodemodelError(f"expected one configuration, found {len(configs)}")
    targets = {}
    for t in configs[0].get("targets", []):
        targets[t.get("id") or t["jsonFile"]] = json.loads((reply / t["jsonFile"]).read_text(encoding="utf-8"))
    return targets, model.get("paths") or {}


def _tests_by_artifact(tests: list[dict], build_dir: Path) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {}
    for test in tests:
        cmd = test.get("command") or []
        if not cmd:
            continue
        exe = cmd[0] if os.path.isabs(cmd[0]) else str(build_dir / cmd[0])
        out.setdefault(os.path.realpath(exe), []).append(test)
    return out


SKIP_DIRS = {"CMakeFiles", "Testing", "_deps"}
OBJECT_SUFFIXES = (".o", ".obj")
_TARGET_DIR = re.compile(r"(?:^|/)CMakeFiles/([^/]+)\.dir/")


class _Generated:
    """Content digests of the files the build itself writes (configure_file,
    file(GENERATE), custom commands) that a target compiles or includes. A
    path alone does not move when CMake rewrites the file, so a version bump
    or an embedded build identity would leave every digest unchanged."""

    def __init__(self, build_roots: list[str]) -> None:
        self.roots = sorted({os.path.realpath(r) for r in build_roots if r})
        self.files: dict[str, str] = {}

    def under_build(self, path: str, skip: set[str] = SKIP_DIRS) -> bool:
        real = os.path.realpath(path)
        if not any(real.startswith(r + os.sep) for r in self.roots):
            return False
        rel = next(os.path.relpath(real, r) for r in self.roots if real.startswith(r + os.sep))
        return not any(part in skip for part in Path(rel).parts)

    def file(self, path: str) -> str:
        real = os.path.realpath(path)
        if real not in self.files:
            try:
                with open(real, "rb") as fh:
                    self.files[real] = hashlib.sha256(fh.read()).hexdigest()
            except OSError:
                self.files[real] = "missing"
        return self.files[real]


def ninja_generated_headers(build_dir: Path, generated: _Generated,
                            deps_text=None) -> dict[str, set[str]] | None:
    """Target name -> build-tree headers its objects included, from Ninja's
    recorded dependencies (`ninja -t deps`, exact, written as each object
    compiled); every target with a compiled object has an entry, possibly
    empty. None when the build has no Ninja dependency log. Headers
    under `_deps` (pinned FetchContent builds) are left out; a precompiled
    header under CMakeFiles is kept, since it is the target's own input."""
    if deps_text is None:
        if not (build_dir / ".ninja_deps").is_file():
            return None
        proc = subprocess.run(["ninja", "-C", str(build_dir), "-t", "deps"],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            return None
        deps_text = proc.stdout
    out: dict[str, set[str]] = {}
    target = None
    for line in deps_text.splitlines():
        if not line.startswith(" "):
            m = _TARGET_DIR.search(line.split(":", 1)[0])
            target = m.group(1) if m else None
            if target is not None:
                out.setdefault(target, set())  # compiled, even with no generated header
            continue
        path = line.strip()
        if target is None or not path:
            continue
        full = path if os.path.isabs(path) else str(build_dir / path)
        if generated.under_build(full, skip={"_deps"}):
            out.setdefault(target, set()).add(full)
    return out


UNKNOWN = "unknown:"
# One value per digest run: an unknown part differs from every other record,
# including another record of the same tree.
NONCE = os.urandom(16).hex()
BOUND_DIR = "pulp-commit-bound"


def commit_bound_declared(build_dir: Path) -> set[str] | None:
    """Targets the build declared commit-bound (`_pulp_declare_commit_bound`
    writes <build>/pulp-commit-bound/<target>.json), or None when the build
    wrote no declarations."""
    folder = build_dir / BOUND_DIR
    if not folder.is_dir():
        return None
    names = set()
    for f in sorted(folder.iterdir()):
        if f.suffix == ".json":
            try:
                names.add(json.loads(f.read_text(encoding="utf-8"))["target"])
            except (OSError, ValueError, KeyError, TypeError):
                names.add(f.stem)
    return names


def commit_bound_closure(raw: dict[str, dict], names: dict[str, str], declared: set[str]) -> set[str]:
    """Declared targets and every target that depends on one, directly or
    through others: a target that builds or runs a commit-bound artifact has
    it in its runtime closure."""
    deps = {rec.get("name") or tid: {names.get(d.get("id"), d.get("id")) for d in rec.get("dependencies", [])}
            for tid, rec in raw.items()}
    bound = set(declared)
    changed = True
    while changed:
        changed = False
        for name, ds in deps.items():
            if name not in bound and ds & bound:
                bound.add(name)
                changed = True
    return bound


def digest_targets(build_dir: Path, source_root: Path, tests: list[dict] | None = None,
                   deps_text: str | None = None) -> dict:
    raw, paths = load_reply(build_dir)
    roots = _roots([str(build_dir), paths.get("build", "")], [str(source_root), paths.get("source", "")])
    model_build = Path(paths.get("build") or build_dir)
    model_source = Path(paths.get("source") or source_root)
    generated = _Generated([str(build_dir), paths.get("build", "")])
    headers = ninja_generated_headers(build_dir, generated, deps_text)
    by_artifact = _tests_by_artifact(tests or [], build_dir)
    names = {tid: rec.get("name", tid) for tid, rec in raw.items()}
    declared = commit_bound_declared(build_dir)
    bound = commit_bound_closure(raw, names, declared or set())
    out: dict[str, dict] = {}
    matched: set[str] = set()
    for tid, rec in sorted(raw.items(), key=lambda kv: kv[1].get("name", kv[0])):
        name = rec.get("name") or tid
        groups = rec.get("compileGroups") or []
        source_paths = [p if os.path.isabs(p) else str(model_source / p)
                        for p in (src.get("path", "") for src in rec.get("sources", []))]
        sources = [{"path": p, "group": src.get("compileGroupIndex")}
                   for p, src in zip(source_paths, rec.get("sources", []))]
        gen_files = {p for p in source_paths if generated.under_build(p) and not p.endswith(OBJECT_SUFFIXES)}
        compiles = any(g.get("sourceIndexes") for g in groups)
        known = headers is not None and (name in headers or not compiles)
        gen_files |= (headers or {}).get(name, set())
        gen_rows = sorted((_normalise(os.path.realpath(p), roots), generated.file(p)) for p in gen_files)
        compile_ = [{"language": g.get("language"),
                     "flags": [f.get("fragment") for f in g.get("compileCommandFragments", [])],
                     "defines": [d.get("define") for d in g.get("defines", [])],
                     "includes": [{"path": i.get("path"), "system": bool(i.get("isSystem"))}
                                  for i in g.get("includes", [])],
                     "frameworks": [f.get("path") for f in g.get("frameworks", [])],
                     "sysroot": (g.get("sysroot") or {}).get("path"),
                     "sources": g.get("sourceIndexes", [])} for g in groups]
        link = rec.get("link") or {}
        link_ = {"language": link.get("language"),
                 "fragments": [{"fragment": f.get("fragment"), "role": f.get("role")}
                               for f in link.get("commandFragments", [])],
                 "sysroot": (link.get("sysroot") or {}).get("path")}
        artifacts = []
        regs: list[dict] = []
        for art in rec.get("artifacts", []):
            p = art.get("path", "")
            full = p if os.path.isabs(p) else str(model_build / p)
            artifacts.append(full)
            for test in by_artifact.get(os.path.realpath(full), []):
                matched.add(test.get("name", ""))
                regs.append({"name": test.get("name"), "command": test.get("command"),
                             "properties": test.get("properties", [])})
        deps = sorted(names.get(d.get("id"), d.get("id")) for d in rec.get("dependencies", []))
        parts = {
            "sources": _digest(_normalise(sources, roots)),
            "compile": _digest(_normalise(compile_, roots)),
            "link": _digest(_normalise(link_, roots)),
            "tests": _digest(_normalise(sorted(regs, key=lambda r: r["name"] or ""), roots)) if regs else None,
            # A target whose included headers are unknown (no Ninja log, or no
            # compiled object recorded) must never compare equal to anything:
            # an input nobody read would otherwise read as unchanged.
            "generated": (_digest(gen_rows) if gen_rows else None) if known else UNKNOWN + NONCE,
        }
        out[name] = {"type": rec.get("type"), "artifacts": sorted(_normalise(artifacts, roots)),
                     "dependencies": deps, **parts,
                     "commit_bound": name in bound,
                     "digest": _digest({"schema": SCHEMA, "type": rec.get("type"), "dependencies": deps, **parts})}
    # Tests whose command is not a target's artifact (scripts run by an
    # interpreter) are keyed elsewhere; only their count is kept here.
    return {"schema": SCHEMA, "targets": out,
            "generated_headers": "ninja-deps" if headers is not None else "unavailable",
            "commit_bound_declared": sorted(declared) if declared is not None else "unavailable",
            "tests_unmatched": sum(1 for t in tests if t.get("name") not in matched) if tests is not None else None}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--ctest-json", help="`ctest --show-only=json-v1` output to attach test registrations")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv[1:])
    tests = json.loads(Path(a.ctest_json).read_text(encoding="utf-8")).get("tests", []) if a.ctest_json else None
    doc = digest_targets(Path(a.build_dir), Path(a.source_root), tests)
    Path(a.out).write_text(json.dumps(doc, sort_keys=True, separators=(",", ":")), encoding="utf-8")
    print(f"codemodel digest: {len(doc['targets'])} targets")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
