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
import sys
from pathlib import Path
from typing import Any

SCHEMA = "pulp-codemodel-digest/v1"
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


def digest_targets(build_dir: Path, source_root: Path, tests: list[dict] | None = None) -> dict:
    raw, paths = load_reply(build_dir)
    roots = _roots([str(build_dir), paths.get("build", "")], [str(source_root), paths.get("source", "")])
    model_build = Path(paths.get("build") or build_dir)
    by_artifact = _tests_by_artifact(tests or [], build_dir)
    names = {tid: rec.get("name", tid) for tid, rec in raw.items()}
    out: dict[str, dict] = {}
    matched: set[str] = set()
    for tid, rec in sorted(raw.items(), key=lambda kv: kv[1].get("name", kv[0])):
        name = rec.get("name") or tid
        groups = rec.get("compileGroups") or []
        sources = [{"path": s.get("path"), "group": s.get("compileGroupIndex")} for s in rec.get("sources", [])]
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
        }
        out[name] = {"type": rec.get("type"), "artifacts": sorted(_normalise(artifacts, roots)),
                     "dependencies": deps, **parts,
                     "digest": _digest({"type": rec.get("type"), "dependencies": deps, **parts})}
    # Tests whose command is not a target's artifact (scripts run by an
    # interpreter) are keyed elsewhere; only their count is kept here.
    return {"schema": SCHEMA, "targets": out,
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
