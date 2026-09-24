#!/usr/bin/env python3
"""Select the coverage-build targets and CTests a diff can actually move.

``local_diff_cover.sh`` measures *changed* lines only. Building and running the
whole tree to measure a handful of lines in one library is the dominant cost of
the pre-push gate (a cold instrumented build of every target in every
worktree), so this helper narrows both halves to the part of the build graph a
diff reaches:

``plan``
    Read the CMake File API codemodel of the configured coverage build and the
    diff against the pinned compare ref, then print a JSON plan::

        {"schema": "pulp.diff-cover-plan/v1",
         "mode": "targeted" | "all" | "none",
         "reason": "<one line a human can act on>",
         "targets": ["pulp-view", "pulp-test-view", ...],   # targeted only
         "measured_files": [...], "owners": {...}}

    * ``none``: no changed file carries a line diff-cover can measure (every
      native change is under an ignored tree or a ``diff_cover_excludes``
      pattern). diff-cover would report nothing, so building is pure cost.
    * ``targeted``: every measured file maps to at least one owning target.
      The build set is the owners plus every buildable target that transitively
      depends on them (link *and* ``add_dependencies`` edges, which the File API
      reports together). Owners are always built, so a changed library that no
      test reaches still contributes its uncovered lines instead of silently
      dropping out of the report.
    * ``all``: the mapping is ambiguous (no codemodel, or a measured file with
      no owner). The caller builds everything, as before, and prints ``reason``.

``tests``
    After the targeted build, write the names of the CTests whose command
    references an artifact of a built target (or whose unbuilt Catch2
    placeholder names one) to a file for ``ctest --tests-from-file``.

The selection only ever *removes* work whose result cannot reach the measured
lines. It never widens what counts as covered, so a narrowed run can read
lower than a full one (a test that exercises the change through an undeclared
runtime path is not selected) but never higher. CI's coverage workflow stays
the authority.

A different selector can replace the built-in one through
``PULP_DIFF_COVER_SELECTOR_CMD``: the command is run with
``--build-dir <dir> --base <sha>`` appended and must print the same plan JSON.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

PLAN_SCHEMA = "pulp.diff-cover-plan/v1"

# Mirrors the suffix set local_diff_cover.sh's preflight treats as native.
NATIVE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".m", ".mm",
    ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tpp",
    ".ixx", ".cppm",
}

# Mirror local_diff_cover.sh's preflight: only these are proof a path cannot
# affect the native build. Anything else (a `.cmake` file, an extensionless
# path, a deletion or rename) keeps the whole-tree build when the diff has no
# measured line of its own.
KNOWN_NON_NATIVE_SUFFIXES = {
    ".css", ".html", ".js", ".json", ".md", ".mjs", ".py", ".rst",
    ".sh", ".toml", ".ts", ".tsx", ".txt", ".xml", ".yaml", ".yml",
}
KNOWN_NON_NATIVE_PATHS = {".githooks/pre-push"}

# Must stay identical to COVERAGE_IGNORE_REGEX in local_diff_cover.sh: files
# under these trees are dropped from the llvm-cov export, so diff-cover never
# measures them. test_diff_cover_targets.py pins the two copies together.
COVERAGE_IGNORE_REGEX = (
    r"(^|/)(_deps|external|test|[Cc]atch2|build|build-cov|build-coverage"
    r"|examples|fetchcontent-src|sandbox-e2e)/"
)

BUILDABLE_TYPES = {
    "EXECUTABLE", "STATIC_LIBRARY", "SHARED_LIBRARY", "MODULE_LIBRARY",
    "OBJECT_LIBRARY",
}

NOT_BUILT_PLACEHOLDER = re.compile(r"^(?P<target>.+)_NOT_BUILT(-[0-9a-f]+)?$")


class PlanError(RuntimeError):
    """The plan cannot be computed; the caller must fall back to all."""


# ── changed files ──────────────────────────────────────────────────────────


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    ).stdout


def changed_records(repo: Path, base: str) -> list[tuple[str, str]]:
    """(status, path) for every change diff-cover can see.

    The three layers diff-cover reports over (merge-base...HEAD, staged,
    unstaged) are read independently, like local_diff_cover.sh's preflight,
    so an unstaged restore can never hide a committed change.
    """
    merge_base = _git(repo, "merge-base", base, "HEAD").strip()
    records: list[tuple[str, str]] = []
    for args in ((merge_base, "HEAD"), ("--cached", "HEAD"), ()):
        raw = _git(repo, "diff", "--name-status", "-z", "--find-renames",
                   "--find-copies", *args, "--").split("\0")
        if raw and raw[-1] == "":
            raw.pop()
        i = 0
        while i < len(raw):
            status = raw[i]
            i += 1
            paths = 2 if status[:1] in {"R", "C"} else 1
            # The last path is the one that exists after the change.
            records.append((status, raw[i + paths - 1]))
            i += paths
    return records


def load_excludes(config_json: Path) -> list[str]:
    cfg = json.loads(config_json.read_text(encoding="utf-8"))
    return list(cfg.get("diff_cover_excludes") or [])


def is_native(path: str) -> bool:
    # Any suffix counts (`x.hpp.in`), matching the shell preflight.
    return any(sfx in NATIVE_SUFFIXES for sfx in PurePosixPath(path.lower()).suffixes)


def definitely_non_native(path: str) -> bool:
    if path in KNOWN_NON_NATIVE_PATHS:
        return True
    suffixes = PurePosixPath(path.lower()).suffixes
    return bool(suffixes) and suffixes[-1] in KNOWN_NON_NATIVE_SUFFIXES


def is_measured(path: str, excludes: Iterable[str]) -> bool:
    """True when diff-cover can report a line of ``path``."""
    if not is_native(path):
        return False
    if re.search(COVERAGE_IGNORE_REGEX, path):
        return False
    base = PurePosixPath(path).name
    for pattern in excludes:
        # diff-cover matches each pattern against the basename and the
        # absolute path; a `**/name` glob therefore matches any directory.
        if fnmatch.fnmatch(base, pattern) or fnmatch.fnmatch(path, pattern):
            return False
        if pattern.startswith("**/") and fnmatch.fnmatch(base, pattern[3:]):
            return False
    return True


# ── codemodel ──────────────────────────────────────────────────────────────


class Codemodel:
    def __init__(self, build_dir: Path) -> None:
        reply = build_dir / ".cmake" / "api" / "v1" / "reply"
        try:
            index_files = sorted(reply.glob("index-*.json"))
            if not index_files:
                raise PlanError(f"no CMake File API reply under {reply}")
            index = json.loads(index_files[-1].read_text(encoding="utf-8"))
            cm_name = None
            for obj in index.get("objects", []):
                if obj.get("kind") == "codemodel" and obj.get("version", {}).get("major") == 2:
                    cm_name = obj["jsonFile"]
            if cm_name is None:
                raise PlanError("File API reply has no codemodel-v2 object")
            codemodel = json.loads((reply / cm_name).read_text(encoding="utf-8"))
            self.source_root = Path(codemodel["paths"]["source"]).resolve()
            self.build_root = Path(codemodel["paths"]["build"]).resolve()
            config = codemodel["configurations"][0]
        except PlanError:
            raise
        except (OSError, ValueError, KeyError, IndexError) as exc:
            raise PlanError(f"CMake File API codemodel unreadable: {exc}") from exc

        self.targets: dict[str, dict[str, Any]] = {}
        id_to_name: dict[str, str] = {}
        for ref in config.get("targets", []):
            data = json.loads((reply / ref["jsonFile"]).read_text(encoding="utf-8"))
            name = data["name"]
            id_to_name[data["id"]] = name
            self.targets[name] = data

        self.deps: dict[str, set[str]] = {}
        self.rdeps: dict[str, set[str]] = {n: set() for n in self.targets}
        for name, data in self.targets.items():
            deps = {id_to_name[d["id"]] for d in data.get("dependencies", [])
                    if d.get("id") in id_to_name}
            self.deps[name] = deps
            for dep in deps:
                self.rdeps[dep].add(name)

        self.source_owners: dict[str, set[str]] = {}
        self.dir_owners: dict[str, set[str]] = {}
        for name, data in self.targets.items():
            for src in data.get("sources", []):
                rel = self._relative(src.get("path", ""))
                if rel is not None:
                    self.source_owners.setdefault(rel, set()).add(name)
            src_dir = data.get("paths", {}).get("source", "")
            if src_dir not in ("", "."):
                self.dir_owners.setdefault(src_dir, set()).add(name)

    def _relative(self, path: str) -> str | None:
        p = Path(path)
        if not p.is_absolute():
            return PurePosixPath(path).as_posix()
        try:
            return p.resolve().relative_to(self.source_root).as_posix()
        except ValueError:
            return None

    def owners(self, path: str) -> tuple[set[str], str]:
        """Targets that compile ``path``, and how they were found."""
        direct = self.source_owners.get(path)
        if direct:
            return set(direct), "source"
        # Headers and #included fragments are usually not listed as sources.
        # Attribute them to the targets declared by the deepest CMake
        # directory that contains them; the root directory is never used,
        # because it would claim every file in the tree.
        parent = PurePosixPath(path).parent
        while str(parent) not in ("", "."):
            owners = self.dir_owners.get(parent.as_posix())
            if owners:
                return set(owners), f"directory {parent.as_posix()}"
            parent = parent.parent
        return set(), "unowned"

    def artifacts(self, name: str) -> list[Path]:
        out = []
        for art in self.targets[name].get("artifacts", []):
            p = Path(art["path"])
            out.append(p if p.is_absolute() else self.build_root / p)
        return out

    def consumers_closure(self, seeds: Iterable[str]) -> set[str]:
        seen = set(seeds)
        stack = list(seen)
        while stack:
            for consumer in self.rdeps.get(stack.pop(), ()):
                if consumer not in seen:
                    seen.add(consumer)
                    stack.append(consumer)
        return seen


# ── plan ───────────────────────────────────────────────────────────────────


def compute_plan(repo: Path, build_dir: Path, base: str, config_json: Path,
                 tier: str = "likely") -> dict[str, Any]:
    excludes = load_excludes(config_json)
    records = changed_records(repo, base)
    measured: list[str] = []
    retained: list[str] = []
    for status, path in records:
        kind = status[:1]
        if kind not in {"A", "M"}:
            # Deletions, renames, copies and type changes are what the
            # preflight refuses to classify; keep that refusal.
            retained.append(f"{status} {path}")
        if kind in {"A", "M", "R", "C"} and is_measured(path, excludes):
            measured.append(path)
        elif kind in {"A", "M"} and not is_native(path) and not definitely_non_native(path):
            retained.append(path)
    measured = sorted(set(measured))
    plan: dict[str, Any] = {
        "schema": PLAN_SCHEMA,
        "changed_files": len(records),
        "measured_files": measured,
        "owners": {},
        "targets": [],
    }
    if not measured and not retained:
        plan["mode"] = "none"
        plan["reason"] = (
            "no changed file has a line diff-cover measures "
            "(only ignored trees, diff_cover_excludes, or non-native files)"
        )
        return plan
    if not measured:
        plan["mode"] = "all"
        plan["reason"] = (
            "no measured line, but a change the preflight cannot classify keeps "
            "the whole-tree build: " + ", ".join(sorted(set(retained)))
        )
        return plan

    try:
        model = Codemodel(build_dir)
    except PlanError as exc:
        plan["mode"] = "all"
        plan["reason"] = str(exc)
        return plan

    owners: set[str] = set()
    unowned: list[str] = []
    for path in measured:
        found, how = model.owners(path)
        plan["owners"][path] = {"targets": sorted(found), "via": how}
        if not found:
            unowned.append(path)
        owners |= found
    if unowned:
        plan["mode"] = "all"
        plan["reason"] = "no CMake target owns " + ", ".join(unowned)
        return plan

    buildable = sum(1 for t in model.targets.values() if t.get("type") in BUILDABLE_TYPES)
    closure = sorted(
        name for name in model.consumers_closure(owners)
        if model.targets[name].get("type") in BUILDABLE_TYPES
    )
    if not closure:
        plan["mode"] = "all"
        plan["reason"] = "owning targets are not buildable: " + ", ".join(sorted(owners))
        return plan
    plan["mode"] = "targeted"
    plan["closure_targets"] = closure

    likely: set[str] = set()
    blocker = "closure requested"
    if tier == "likely":
        blocker = likely_tier_blocker(model, plan["owners"], owners)
        if blocker is None:
            likely = likely_tests(model, measured, set(closure))
            if not likely:
                blocker = "no test source names or includes a changed file"
    if blocker is None:
        chosen = sorted(owners | likely)
        plan["tier"] = "likely"
        plan["targets"] = chosen
        plan["reason"] = (
            f"{len(chosen)}/{buildable} targets (owners + tests that name or include "
            f"the changed files); widens to {len(closure)} if coverage falls short"
        )
    else:
        plan["tier"] = "closure"
        plan["targets"] = closure
        plan["reason"] = (
            f"{len(closure)}/{buildable} targets (owners + every transitive consumer) "
            f"for {len(measured)} measured file(s) [{blocker}]"
        )
    return plan


_INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]', re.M)


def likely_tests(model: Codemodel, measured: list[str], candidates: set[str]) -> set[str]:
    """Test executables most likely to exercise the changed files.

    A test target qualifies when one of its test/ sources is named after a
    changed file (``test_<stem>*.cpp``) or includes the changed file's
    companion header (``<stem>.h``/``.hpp``/``.hh``). This is a guess, and
    only safe because the caller widens to the full closure whenever the
    guessed run falls short of the threshold: a run over fewer tests can only
    cover fewer lines, so a pass here is a pass over the closure too.
    """
    stems = {PurePosixPath(p).stem for p in measured}
    headers = {f"{stem}{ext}" for stem in stems for ext in (".h", ".hpp", ".hh")}
    chosen: set[str] = set()
    for name in candidates:
        data = model.targets[name]
        if data.get("type") != "EXECUTABLE":
            continue
        for src in data.get("sources", []):
            rel = model._relative(src.get("path", ""))
            if rel is None or not rel.startswith("test/"):
                continue
            base = PurePosixPath(rel).stem
            if any(base == f"test_{stem}" or base.startswith(f"test_{stem}_") for stem in stems):
                chosen.add(name)
                break
            try:
                text = (model.source_root / rel).read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if any(PurePosixPath(inc).name in headers for inc in _INCLUDE.findall(text)):
                chosen.add(name)
                break
    return chosen


TRANSLATION_UNIT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}


def likely_tier_blocker(model: Codemodel, owner_map: dict[str, Any], owners: set[str]) -> str | None:
    """Why the likely tier could report a false pass, or None when it cannot.

    The likely tier omits most consumers, so it may measure a changed line as
    *uncovered* that another test would have covered: the caller widens to the
    closure on failure, so that costs time, never correctness. What it must
    never do is leave a changed line *unmeasured*, because a line absent from
    the report cannot fail. Two cases can do that, and both keep the closure:

    * a header (or any file found by directory): an inline function only has
      a coverage mapping in the TUs that instantiate it, which may all sit in
      consumers the likely tier skips;
    * an owner whose own artifact the report does not scan: its lines then
      appear only through a consumer that links it.
    """
    for path, info in owner_map.items():
        if info["via"] != "source" or PurePosixPath(path.lower()).suffix not in TRANSLATION_UNIT_SUFFIXES:
            return f"{path} is not a listed translation unit"
    for name in sorted(owners):
        if not _artifact_is_scanned(model, name):
            return f"owner {name} is not scanned by the coverage report"
    return None


def _artifact_is_scanned(model: Codemodel, name: str) -> bool:
    """Mirror local_diff_cover.sh's llvm-cov -object discovery."""
    kind = model.targets[name].get("type")
    for path in model.artifacts(name):
        try:
            rel = path.resolve().relative_to(model.build_root)
        except ValueError:
            continue
        top = rel.parts[0] if rel.parts else ""
        if kind == "STATIC_LIBRARY" and re.fullmatch(r"libpulp-.*\.a|pulp-.*\.lib", rel.name):
            return True
        if kind == "EXECUTABLE" and (
            (top == "test" and len(rel.parts) <= 3)
            or (top in {"tools", "inspect"} and len(rel.parts) <= 4)
        ):
            return True
    return False


def external_plan(cmd: str, build_dir: Path, base: str) -> dict[str, Any]:
    argv = shlex.split(cmd) + ["--build-dir", str(build_dir), "--base", base]
    try:
        out = subprocess.run(argv, check=True, stdout=subprocess.PIPE, text=True).stdout
        plan = json.loads(out)
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        return {"schema": PLAN_SCHEMA, "mode": "all", "targets": [],
                "reason": f"external selector failed: {exc}"}
    if plan.get("schema") != PLAN_SCHEMA or plan.get("mode") not in {"targeted", "all", "none"}:
        return {"schema": PLAN_SCHEMA, "mode": "all", "targets": [],
                "reason": "external selector returned an unrecognised plan"}
    return plan


# ── tests ──────────────────────────────────────────────────────────────────


def _references(token: Any, artifact_paths: set[str]) -> bool:
    """A command token names a built artifact, bare or as ``--flag=<path>``."""
    if not isinstance(token, str):
        return False
    candidate = token.rsplit("=", 1)[-1]
    if not os.path.isabs(candidate):
        return False
    return candidate in artifact_paths or str(Path(candidate).resolve()) in artifact_paths


def select_tests(build_dir: Path, targets: Iterable[str]) -> list[str]:
    model = Codemodel(build_dir)
    wanted = set(targets)
    artifact_paths = {
        str(p.resolve()) for name in wanted if name in model.targets
        for p in model.artifacts(name)
    }
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    )
    tests = json.loads(proc.stdout).get("tests", [])
    selected: list[str] = []
    for test in tests:
        name = test.get("name", "")
        placeholder = NOT_BUILT_PLACEHOLDER.match(name)
        if placeholder and placeholder.group("target") in wanted:
            selected.append(name)
            continue
        if any(_references(token, artifact_paths) for token in test.get("command") or []):
            selected.append(name)
    # ctest --tests-from-file runs every registration of a listed name, so
    # duplicates add nothing; keep first-seen order for a readable file.
    return list(dict.fromkeys(selected))


# ── cli ────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_plan = sub.add_parser("plan")
    p_plan.add_argument("--repo", type=Path, required=True)
    p_plan.add_argument("--build-dir", type=Path, required=True)
    p_plan.add_argument("--base", required=True)
    p_plan.add_argument("--config", type=Path, required=True)
    p_plan.add_argument("--tier", choices=("likely", "closure"), default="likely")
    p_tests = sub.add_parser("tests")
    p_tests.add_argument("--build-dir", type=Path, required=True)
    p_tests.add_argument("--plan", type=Path, required=True)
    p_tests.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)

    if args.cmd == "plan":
        cmd = os.environ.get("PULP_DIFF_COVER_SELECTOR_CMD", "").strip()
        if cmd:
            plan = external_plan(cmd, args.build_dir, args.base)
        else:
            try:
                plan = compute_plan(args.repo, args.build_dir, args.base, args.config, args.tier)
            except (OSError, subprocess.CalledProcessError, ValueError) as exc:
                plan = {"schema": PLAN_SCHEMA, "mode": "all", "targets": [],
                        "reason": f"selector error: {exc}"}
        json.dump(plan, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
        return 0

    plan = json.loads(args.plan.read_text(encoding="utf-8"))
    try:
        names = select_tests(args.build_dir, plan.get("targets", []))
    except (OSError, subprocess.CalledProcessError, ValueError, PlanError) as exc:
        print(f"[diff_cover_targets] test selection failed: {exc}", file=sys.stderr)
        return 1
    args.out.write_text("".join(n + "\n" for n in names), encoding="utf-8")
    print(len(names))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
