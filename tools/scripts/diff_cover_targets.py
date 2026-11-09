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


# ── ctest command/artifact references ──────────────────────────────────────


def _command_path_refs(command: Iterable[Any]) -> Iterable[str]:
    """Yield absolute paths named by a CTest command.

    CMake-script tests commonly pass generated binaries as a separate
    ``--forbidden`` argument or as the value of ``-DVAR=``, ``--flag=`` or
    ``NAME=``. A list-valued assignment may use either CMake's ``;`` or a
    fixture script's ``|`` separator.
    """
    for token in command:
        if not isinstance(token, str) or not token:
            continue
        candidates = [token]
        if "=" in token:
            candidates.append(token.rsplit("=", 1)[1])
        for candidate in candidates:
            for value in re.split(r"[;|]", candidate):
                if os.path.isabs(value):
                    yield str(Path(value).resolve())


def _artifact_index(model: Codemodel) -> dict[str, set[str]]:
    index: dict[str, set[str]] = {}
    for name in model.targets:
        for artifact in model.artifacts(name):
            index.setdefault(str(artifact.resolve()), set()).add(name)
    return index


def _ctest_json(build_dir: Path) -> list[dict[str, Any]]:
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, encoding="utf-8",
    )
    tests = json.loads(proc.stdout).get("tests", [])
    return tests if isinstance(tests, list) else []


def _install_script_artifacts(model: Codemodel) -> dict[str, set[str]]:
    """Return artifacts named by the configured build's install scripts.

    CMake script tests often invoke ``cmake -P`` (or ``cmake --install``)
    rather than naming every archive in their CTest command.  The install
    script is still authoritative about the files that command will consume;
    map those absolute paths back to codemodel targets so a focused build can
    repair a missing archive before CTest starts.
    """
    scripts: list[Path] = []
    root_script = model.build_root / "cmake_install.cmake"
    if root_script.is_file():
        scripts.append(root_script)
    for path in model.build_root.rglob("cmake_install.cmake"):
        if path not in scripts:
            scripts.append(path)
    if not scripts:
        return {}
    text_parts: list[str] = []
    for script in scripts:
        try:
            text_parts.append(script.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
    text = "\n".join(text_parts)
    index: dict[str, set[str]] = {}
    for name in model.targets:
        for artifact in model.artifacts(name):
            key = str(artifact.resolve())
            if key in text:
                index.setdefault(key, set()).add(name)
    return index


def _is_install_test(command: Iterable[Any], build_dir: Path) -> bool:
    """Whether a CTest command executes an install/build-tree staging path."""
    tokens = [token for token in command if isinstance(token, str)]
    joined = " ".join(tokens)
    if "--install" in tokens or "--target install" in joined:
        return True
    # The SDK consumer fixtures use ``cmake -D PULP_BUILD_DIR=... -P
    # test_installed_*``.  Keep this predicate narrow so an unrelated CMake
    # script test does not pull the entire SDK install set into every plan.
    return (
        any("test_installed_" in token or "sdk-consumer" in token for token in tokens)
        and any(str(build_dir.resolve()) in token for token in tokens)
    )


def artifact_closure(build_dir: Path, targets: Iterable[str],
                     names: Iterable[str] = (), regex: str = "") -> dict[str, Any]:
    """Check that selected CTests have every command/install artifact built.

    The result is intentionally a receipt-shaped object.  ``status=blocked``
    is actionable: ``missing_targets`` can be passed to the governed builder;
    if a path has no codemodel owner the receipt preserves that exact path for
    the next repair instead of allowing CTest to fail late inside an install.
    """
    model = Codemodel(build_dir)
    wanted = set(targets)
    command_index = _artifact_index(model)
    install_index = _install_script_artifacts(model)
    tests = _ctest_json(build_dir)
    name_set = set(names)
    try:
        matcher = re.compile(regex) if regex else None
    except re.error as exc:
        return {
            "schema": "pulp.diff-cover-artifact-closure/v1",
            "status": "blocked",
            "reason": f"invalid CTest regex: {exc}",
            "missing": [], "missing_targets": [], "selected_tests": [],
        }

    selected: list[dict[str, Any]] = []
    for test in tests:
        name = str(test.get("name", ""))
        command = test.get("command") or []
        command_refs = {
            owner
            for path in _command_path_refs(command)
            for owner in command_index.get(path, ())
        }
        install_test = _is_install_test(command, build_dir)
        if name_set:
            include = name in name_set
        elif matcher is not None:
            include = bool(matcher.search(name))
        else:
            include = bool(command_refs & wanted) or install_test
        if include:
            selected.append(test)

    missing: list[dict[str, str]] = []
    seen: set[tuple[str, str, str]] = set()
    for test in selected:
        name = str(test.get("name", ""))
        command = test.get("command") or []
        paths: dict[str, set[str]] = {}
        for path in _command_path_refs(command):
            owners = command_index.get(path, ())
            # A CTest command naturally names system tools (cmake, python,
            # shell).  Only retain an unmapped token when it points inside the
            # active build tree; that is the actionable missing-artifact case.
            if not owners and not path.startswith(str(model.build_root.resolve()) + os.sep):
                continue
            paths.setdefault(path, set()).update(owners)
        if _is_install_test(command, build_dir):
            for path, owners in install_index.items():
                paths.setdefault(path, set()).update(owners)
        for path, owners in paths.items():
            if Path(path).exists():
                continue
            mapped = sorted(owners) or [""]
            for owner in mapped:
                row = (name, owner, path)
                if row in seen:
                    continue
                seen.add(row)
                missing.append({"test": name, "target": owner, "path": path})

    missing_targets = sorted({row["target"] for row in missing if row["target"]})
    report: dict[str, Any] = {
        "schema": "pulp.diff-cover-artifact-closure/v1",
        "status": "blocked" if missing else "ok",
        "build_dir": str(build_dir.resolve()),
        "selected_tests": [str(test.get("name", "")) for test in selected],
        "missing": missing,
        "missing_targets": missing_targets,
    }
    if missing:
        report["reason"] = "selected CTest references missing build artifact(s)"
        report["next_action"] = (
            "build missing_targets with the governed builder, then rerun this guard"
            if missing_targets else
            "map each missing path to a configured CMake target before rerunning"
        )
    return report


def _test_artifact_targets(build_dir: Path, model: Codemodel,
                           planned: set[str]) -> set[str]:
    """Return every target artifact referenced by a selected test command."""
    index = _artifact_index(model)
    extras: set[str] = set()
    for test in _ctest_json(build_dir):
        refs = {
            owner
            for path in _command_path_refs(test.get("command") or [])
            for owner in index.get(path, ())
        }
        if refs & planned:
            extras |= refs
    return extras


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
    closure_set = {
        name for name in model.consumers_closure(owners)
        if model.targets[name].get("type") in BUILDABLE_TYPES
    }
    # A consumer test can invoke independent negative controls, static
    # archives, SDK probes, or fixtures. Their command artifacts are part of
    # this build closure even when no CMake dependency edge connects them to
    # the changed owner.
    try:
        closure_set |= {
            name for name in _test_artifact_targets(build_dir, model, closure_set)
            if model.targets.get(name, {}).get("type") in BUILDABLE_TYPES
        }
    except (OSError, subprocess.CalledProcessError, ValueError):
        # CTest may not be configured yet during a cold selector probe.
        pass
    closure = sorted(closure_set)
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
            likely = likely_tests(model, measured, closure_set)
            if likely:
                try:
                    refs = _test_artifact_targets(build_dir, model, owners | likely)
                    extras = {
                        name for name in refs
                        if model.targets.get(name, {}).get("type") in BUILDABLE_TYPES
                    }
                    closure_set |= extras
                    closure = sorted(closure_set)
                    plan["closure_targets"] = closure
                    # Keep a likely test and every command artifact it uses
                    # together; otherwise a negative control or SDK fixture
                    # can be selected but absent from the build.
                    likely |= extras - (owners | likely)
                except (OSError, subprocess.CalledProcessError, ValueError):
                    pass
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
    """A command token names a planned built artifact."""
    return any(path in artifact_paths for path in _command_path_refs([token]))


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
        if placeholder:
            target = placeholder.group("target")
            # CTest emits placeholders for targets that were not configured or
            # built. A name match alone would select utility/no-artifact
            # registrations and then run a test with no executable.
            if target in wanted and target in model.targets and any(model.artifacts(target)):
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
    p_artifacts = sub.add_parser("artifacts")
    p_artifacts.add_argument("--build-dir", type=Path, required=True)
    p_artifacts.add_argument("--plan", type=Path, required=True)
    p_artifacts.add_argument("--receipt", type=Path, required=True)
    p_artifacts.add_argument("--tests-file", type=Path)
    p_artifacts.add_argument("--regex", default="")
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
    if args.cmd == "artifacts":
        names = ()
        if args.tests_file:
            try:
                names = tuple(
                    line.strip() for line in args.tests_file.read_text(encoding="utf-8").splitlines()
                    if line.strip()
                )
            except OSError:
                names = ()
        try:
            report = artifact_closure(
                args.build_dir, plan.get("targets", []), names, args.regex)
        except (OSError, subprocess.CalledProcessError, ValueError, PlanError) as exc:
            report = {
                "schema": "pulp.diff-cover-artifact-closure/v1",
                "status": "blocked",
                "reason": f"artifact closure probe failed: {exc}",
                "missing": [], "missing_targets": [], "selected_tests": [],
                "next_action": "inspect the CMake File API and CTest JSON before rerunning",
            }
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n",
                                encoding="utf-8")
        json.dump(report, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
        return 3 if report.get("status") == "blocked" else 0
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
