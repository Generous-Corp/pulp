#!/usr/bin/env python3
"""Declared inputs for script-driven ctests: the `.pydeps` pattern applied to ctest.

The affected-test shadow (`tools/ci/affected_tests_shadow.py`) can follow the
build graph to every compiled test, but a ctest whose command is a Python,
Node or shell script declares no inputs anywhere the graph can see. So the
shadow counts every such test as affected whenever any script surface
changed: 402 script-driven entries holding 44% of the gate's test-seconds
run on 173 of 200 replayed merged PRs, most of which touched none of their
files.

Chromium solved the same blind spot for `gn analyze` by generating and
checking in `.pydeps` files (transitive Python imports) that presubmit keeps
fresh. This tool does that for Pulp's ctest inventory:

  script_test_inputs.py --build-dir <dir> --write     # regenerate the list
  script_test_inputs.py --build-dir <dir> --check     # drift check (pr-fast), diff-scoped
  script_test_inputs.py --build-dir <dir> --check --full   # every entry, no base

For each script-driven test it records, relative to the repo root: the
entry script; its transitive local imports (Python `import`/`from`, Node
relative `import`/`require`, shell `source`/`.`); repository paths the
command line names; and repository paths the scripts name literally. A test
whose inputs cannot be bounded this way (a cmake-driven nested build, a test
with no command) gets NO entry, and the shadow keeps its fail-closed rule for
it. The output is sorted and deterministic; `--check` compares only tests
present in the current configuration, so a Linux inventory does not report
drift against a list written on macOS. `--check` is also DIFF-SCOPED: a
generated file drifts whenever main moves, so it fails only for drift the
change under test reaches (its diff touches the test's entry script or an
old/new input); other drift is reported as advisory. `--full` compares every
entry.

Exit codes: 0 in sync (or written); 1 drift (`--check`); 2 inventory unreadable.
"""
from __future__ import annotations

import argparse
import ast
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SCHEMA = "pulp-ctest-script-inputs/v1"
DEFAULT_LIST = Path("test") / "ctest_script_inputs.json"
INTERPRETERS = ("python", "python3", "node", "bash", "sh", "zsh")
UNDECLARABLE_EXES = ("cmake", "ninja", "make", "ctest", "xcodebuild", "cargo")
# An entry script that lives in the build tree (a configure-generated runner)
# is recorded under this token, never under the build directory's name, so
# a list written from `build-gate`, `build`, or a directory outside the
# checkout is byte-identical.
BINARY_DIR_TOKEN = "${CMAKE_BINARY_DIR}"
CENSUS_PROFILES = Path("docs") / "status" / "consumption-profiles.json"
# The gate configures with these switches (build.yml); the consumption census
# records the same scope per measured profile and is read first.
GATE_BUILD_SCOPE = {"PULP_BUILD_TESTS": "ON", "PULP_BUILD_EXAMPLES": "OFF"}
# Prefixes a literal string must start with to count as a repository path.
REPO_PREFIXES = ("tools/", "test/", "hooks/", ".githooks/", ".github/", "docs/", "ship/",
                 "core/", "examples/", "templates/", "inspect/", "experimental/", "cmake/", "external/")
LITERAL_RE = re.compile(r"""["'](?P<p>(?:%s)[A-Za-z0-9_./@+-]+)["']""" % "|".join(re.escape(p) for p in REPO_PREFIXES))
NODE_IMPORT_RE = re.compile(r"""(?:from\s+|import\s*\(?\s*|require\s*\(\s*)["'](?P<s>\.{1,2}/[^"']+)["']""")
SHELL_SOURCE_LINE_RE = re.compile(r"""^\s*(?:source|\.)\s+(?P<rest>.+)$""")
SHELL_TAIL_RE = re.compile(r"""(?P<f>[A-Za-z0-9_./-]+\.(?:sh|bash))["']?\s*$""")


def _rel(path: Path, root: Path) -> str | None:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None


def tracked_paths(root: Path) -> set[str]:
    """Repo-relative paths git tracks. Only these can be inputs: an untracked
    file (a build directory inside the checkout, a generated file) would make
    the list depend on the environment that generated it."""
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return set()
    return {p.decode("utf-8", "replace") for p in out.split(b"\0") if p}


def is_tracked(rel: str, tracked: set[str]) -> bool:
    if not tracked:
        return True  # not a git checkout (tests): keep the existence rule
    if rel in tracked:
        return True
    prefix = rel.rstrip("/") + "/"
    return rel not in (".", "") and any(t.startswith(prefix) for t in tracked)


def gate_build_scope(root: Path) -> dict[str, str]:
    """The build scope the list must reflect: the switches the census recorded
    for its measured profiles (they agree), else the gate's own configure."""
    try:
        doc = json.loads((root / CENSUS_PROFILES).read_text(encoding="utf-8"))
        scopes = [p.get("build_scope") for p in doc.get("profiles", {}).values() if isinstance(p, dict)]
        scopes = [sc for sc in scopes if isinstance(sc, dict) and sc]
        if scopes and all(sc == scopes[0] for sc in scopes):
            return dict(scopes[0])
    except (OSError, json.JSONDecodeError, AttributeError):
        pass
    return dict(GATE_BUILD_SCOPE)


def registered_from(test: dict, inventory: dict) -> str | None:
    """The CMake file whose add_test registered this test, from the ctest
    json-v1 backtrace graph; None for a test that carries no backtrace."""
    graph = inventory.get("backtraceGraph") or {}
    nodes, files = graph.get("nodes") or [], graph.get("files") or []
    idx = test.get("backtrace")
    if not isinstance(idx, int) or idx >= len(nodes):
        return None
    node = nodes[idx]
    while "file" not in node and isinstance(node.get("parent"), int) and node["parent"] < len(nodes):
        node = nodes[node["parent"]]
    f = node.get("file")
    return files[f] if isinstance(f, int) and f < len(files) else None


def outside_gate_profile(inventory: dict, root: Path, scope: dict[str, str] | None = None) -> set[str]:
    """Tests a configure with PULP_BUILD_EXAMPLES=ON registers from examples/
    that the gate (examples OFF) never lists. Filtering by where the
    registration lives, not by the local cache, makes `--write` from an
    examples-ON build byte-identical to one from the gate's configure."""
    scope = gate_build_scope(root) if scope is None else scope
    if scope.get("PULP_BUILD_EXAMPLES", "OFF") != "OFF":
        return set()
    examples = (root / "examples").resolve()
    out = set()
    for t in inventory.get("tests", []):
        src = registered_from(t, inventory)
        if src and _rel(Path(src), examples) is not None:
            out.add(t.get("name", ""))
    return out


class Walker:
    def __init__(self, root: Path) -> None:
        self.root = root.resolve()

    # -- resolution helpers -------------------------------------------------
    def _in_repo(self, p: Path) -> bool:
        return _rel(p, self.root) is not None and p.exists()

    def python_module_candidates(self, name: str, search: list[Path]) -> list[Path]:
        parts = name.split(".")
        out = []
        for base in search:
            out.append(base.joinpath(*parts).with_suffix(".py"))
            out.append(base.joinpath(*parts) / "__init__.py")
        return out

    def walk_python(self, entry: Path, search: list[Path], seen: set[Path]) -> None:
        if entry in seen or not self._in_repo(entry):
            return
        seen.add(entry)
        try:
            tree = ast.parse(entry.read_text(encoding="utf-8", errors="replace"))
        except SyntaxError:
            return
        local_search = [entry.parent] + search
        for node in ast.walk(tree):
            names: list[str] = []
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom):
                if node.level:  # relative import
                    base = entry.parent
                    for _ in range(node.level - 1):
                        base = base.parent
                    mod = node.module or ""
                    for cand in self.python_module_candidates(mod, [base]) if mod else [base / "__init__.py"]:
                        self.walk_python(cand, search, seen)
                    for a in node.names:
                        for cand in self.python_module_candidates((mod + "." if mod else "") + a.name, [base]):
                            self.walk_python(cand, search, seen)
                    continue
                names = [node.module] if node.module else []
                # `from pkg import mod`: the alias may be a submodule file.
                names += [f"{node.module}.{a.name}" for a in node.names] if node.module else []
            for name in names:
                for cand in self.python_module_candidates(name, local_search):
                    if self._in_repo(cand):
                        self.walk_python(cand, search, seen)
                        break
        self.literal_paths(entry, seen)

    def walk_node(self, entry: Path, seen: set[Path]) -> None:
        if entry in seen or not self._in_repo(entry):
            return
        seen.add(entry)
        text = entry.read_text(encoding="utf-8", errors="replace")
        for m in NODE_IMPORT_RE.finditer(text):
            spec = m.group("s")
            for cand in (entry.parent / spec, *(entry.parent / (spec + ext) for ext in (".mjs", ".js", ".cjs", ".json")),
                         entry.parent / spec / "index.mjs", entry.parent / spec / "index.js"):
                if cand.is_file():
                    self.walk_node(cand, seen)
                    break
        self.literal_paths(entry, seen)

    def walk_shell(self, entry: Path, seen: set[Path]) -> None:
        if entry in seen or not self._in_repo(entry):
            return
        seen.add(entry)
        text = entry.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            m = SHELL_SOURCE_LINE_RE.match(line)
            if not m:
                continue
            rest = m.group("rest")
            if "$" in rest:
                # `source "$(dirname "$0")/lib.sh"` style: the file is the
                # trailing path component, looked up beside the script.
                tail = SHELL_TAIL_RE.search(rest)
                cands = [entry.parent / os.path.basename(tail.group("f"))] if tail else []
            else:
                spec = rest.strip().strip("\"'").split()[0] if rest.strip() else ""
                cands = ([Path(spec)] if spec.startswith("/") else [entry.parent / spec, self.root / spec]) if spec else []
            for cand in cands:
                if cand.is_file():
                    self.walk_shell(cand, seen)
                    break
        self.literal_paths(entry, seen)

    def literal_paths(self, entry: Path, seen: set[Path]) -> None:
        text = entry.read_text(encoding="utf-8", errors="replace")
        for m in LITERAL_RE.finditer(text):
            rel = m.group("p")
            # Prose guides are named by lint-style tests as subjects, not read
            # as data; docs/status/ manifests and contracts are real inputs.
            if rel.startswith("docs/") and not rel.startswith("docs/status/"):
                continue
            cand = self.root / rel
            if cand.exists():
                seen.add(cand)


def classify(command: list[str], root: Path) -> tuple[str, Path | None, list[str]]:
    """(kind, entry script, extra args). kind in python|node|shell|undeclarable."""
    if not command:
        return "undeclarable", None, []
    exe = os.path.basename(command[0])
    if exe in UNDECLARABLE_EXES or exe.startswith(UNDECLARABLE_EXES):
        return "undeclarable", None, []
    args = command[1:]
    if exe.startswith("python"):
        i = 0
        while i < len(args) and args[i].startswith("-") and args[i] != "-m":
            i += 1
        if i < len(args) and args[i] == "-m":
            return "python-module", None, args[i + 1:]
        return ("python", Path(args[i]), args[i + 1:]) if i < len(args) else ("undeclarable", None, [])
    if exe == "node":
        i = 0
        while i < len(args) and args[i].startswith("-"):
            i += 1
        return ("node", Path(args[i]), args[i + 1:]) if i < len(args) else ("undeclarable", None, [])
    if exe in ("bash", "sh", "zsh"):
        i = 0
        while i < len(args) and args[i].startswith("-"):
            i += 1
        return ("shell", Path(args[i]), args[i + 1:]) if i < len(args) else ("undeclarable", None, [])
    if command[0].endswith(".py"):
        return "python", Path(command[0]), args
    if command[0].endswith((".sh", ".bash")):
        return "shell", Path(command[0]), args
    if command[0].endswith((".mjs", ".js")):
        return "node", Path(command[0]), args
    return "undeclarable", None, []


def inputs_for(test: dict, root: Path, build_dir: Path | None = None) -> dict | None:
    props = {p["name"]: p["value"] for p in test.get("properties", [])}
    wd = Path(props.get("WORKING_DIRECTORY") or root)
    kind, entry, args = classify(test.get("command") or [], root)
    if kind == "undeclarable":
        return None
    w = Walker(root)
    seen: set[Path] = set()
    search = [wd, root / "tools" / "scripts", root / "tools" / "ci"]
    env = props.get("ENVIRONMENT") or []
    for e in (env if isinstance(env, list) else [env]):
        if e.startswith("PYTHONPATH="):
            search += [Path(p) if os.path.isabs(p) else wd / p for p in e.split("=", 1)[1].split(os.pathsep) if p]
    if kind == "python-module":
        mods = list(args)
        if mods and mods[0] == "unittest":
            # `python -m unittest <module>...`: the test modules are the entries.
            mods = [m for m in mods[1:] if not m.startswith("-")]
        entries = [next((c for c in w.python_module_candidates(m.split("::")[0], [wd] + search) if c.is_file()), None)
                   for m in mods[:1]]
        entry = entries[0] if entries else None
        if entry is None:
            return None
        args = []
        kind = "python"
    if not entry.is_absolute():
        entry = wd / entry
    generated = _rel(entry, build_dir) if build_dir else None
    if generated is None and not w._in_repo(entry):
        return None
    if kind == "python":
        w.walk_python(entry, search, seen)
    elif kind == "node":
        w.walk_node(entry, seen)
    else:
        w.walk_shell(entry, seen)
    for a in args:
        for cand in (Path(a), wd / a):
            if cand.is_absolute() and w._in_repo(cand):
                seen.add(cand)
    tracked = tracked_paths(root)
    rels = sorted({r for r in (_rel(p, root) for p in seen) if r and r != "." and is_tracked(r, tracked)})
    entry_rel = f"{BINARY_DIR_TOKEN}/{generated}" if generated is not None else _rel(entry, root)
    return {"kind": kind, "entry": entry_rel, "inputs": rels}


def build_list(inventory: dict, root: Path, build_dir: Path | None = None) -> dict:
    tests = {}
    excluded = outside_gate_profile(inventory, root)
    for t in inventory.get("tests", []):
        name = t.get("name", "")
        if not name or name.endswith("_NOT_BUILT") or name in excluded:
            continue
        cmd = t.get("command") or []
        if cmd and _rel(Path(cmd[0]), root) is None and not os.path.basename(cmd[0]).startswith(INTERPRETERS):
            continue  # a compiled test binary or a tool outside the repo: the graph owns it
        rec = inputs_for(t, root, build_dir)
        if rec is not None:
            tests[name] = rec
    return {"schema": SCHEMA, "tests": dict(sorted(tests.items()))}


def load_inventory(build_dir: str | None, inventory_json: str | None) -> dict | None:
    try:
        if inventory_json:
            return json.loads(Path(inventory_json).read_text(encoding="utf-8"))
        proc = subprocess.run(["ctest", "--test-dir", build_dir, "-N", "--show-only=json-v1"],
                              capture_output=True, text=True, check=True)
        return json.loads(proc.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"script-test-inputs: inventory unreadable: {exc}", file=sys.stderr)
        return None


def drift(current: dict, checked_in: dict) -> list[tuple[str, str, set[str]]]:
    """(kind, test, paths involved) for tests present in the current configuration
    whose entry is missing or stale. `paths` is the entry script plus the old and
    new input lists: what a change has to touch to be this test's own drift."""
    problems = []
    ci = checked_in.get("tests", {})
    for name, rec in current["tests"].items():
        paths = set(rec.get("inputs") or []) | {rec.get("entry") or ""}
        if name not in ci:
            problems.append(("missing from list", name, paths - {""}))
        elif ci[name] != rec:
            paths |= set(ci[name].get("inputs") or []) | {ci[name].get("entry") or ""}
            problems.append(("stale entry", name, paths - {""}))
    return problems


def touched_by(changed: set[str], paths: set[str]) -> bool:
    return any(f == p or f.startswith(p.rstrip("/") + "/") for f in changed for p in paths)


def advisory_here() -> bool:
    """A merge group reports drift but never fails on it; a pull-request head
    (or a local run) blocks. Overridable for tests and for a deliberate
    blocking run in a group: PULP_SCRIPT_INPUTS_STRICT=1."""
    if os.environ.get("PULP_SCRIPT_INPUTS_STRICT") == "1":
        return False
    return os.environ.get("GITHUB_EVENT_NAME") == "merge_group"


def resolve_base(root: Path, explicit: str | None) -> str | None:
    """The ref this check is diff-scoped against, or None for a full compare.
    Order: --base, PULP_SCRIPT_INPUTS_BASE, origin/<GITHUB_BASE_REF> (pull
    request), HEAD^1 (merge group: the validated main the group is built on)."""
    candidates = [explicit, os.environ.get("PULP_SCRIPT_INPUTS_BASE")]
    if os.environ.get("GITHUB_BASE_REF"):
        candidates.append("origin/" + os.environ["GITHUB_BASE_REF"])
    if os.environ.get("GITHUB_EVENT_NAME") == "merge_group":
        candidates.append("HEAD^1")
    for c in candidates:
        if c and subprocess.run(["git", "-C", str(root), "rev-parse", "--verify", "--quiet", c + "^{commit}"],
                                capture_output=True).returncode == 0:
            return c
    return None


def changed_files(root: Path, base: str) -> set[str]:
    mb = subprocess.run(["git", "-C", str(root), "merge-base", base, "HEAD"], capture_output=True, text=True)
    anchor = mb.stdout.strip() if mb.returncode == 0 and mb.stdout.strip() else base
    out = subprocess.run(["git", "-C", str(root), "diff", "--name-only", anchor, "HEAD"],
                         capture_output=True, text=True, check=True).stdout
    return {f for f in out.split() if f}


# Files whose edit can change what the generator records: the scripts it walks.
SCRIPT_SUFFIXES = (".py", ".sh", ".bash", ".zsh", ".js", ".mjs", ".cjs")


def touch_reasons(root: Path, base: str, list_path: Path | None = None) -> list[str]:
    """Why a change since ``base`` can put the checked-in list out of date, or [].

    Answerable without a configured build, so a caller can decide whether a
    configure is worth paying for. A change can introduce drift only by editing
    the list itself, a declared entry or input file, a script under a declared
    input directory, or a CMake file that registers tests (a new or re-pointed
    script test). A C++ or data edit under a directory input cannot change what
    the generator records, so it is not a reason.
    """
    changed = changed_files(root, base)
    # Uncommitted edits count too: gates.sh is often run before the commit.
    wt = subprocess.run(["git", "-C", str(root), "diff", "--name-only", "HEAD"],
                        capture_output=True, text=True)
    changed |= {f for f in wt.stdout.split() if f} if wt.returncode == 0 else set()
    list_path = list_path or root / DEFAULT_LIST
    rel_list = _rel(list_path, root) or DEFAULT_LIST.as_posix()
    try:
        checked_in = json.loads(list_path.read_text(encoding="utf-8")).get("tests", {})
    except (OSError, json.JSONDecodeError, AttributeError):
        checked_in = {}
    files: dict[str, str] = {}
    dirs: dict[str, str] = {}
    for name, rec in sorted(checked_in.items()):
        for p in [rec.get("entry") or "", *(rec.get("inputs") or [])]:
            if not p or p.startswith(BINARY_DIR_TOKEN):
                continue
            (dirs if (root / p).is_dir() else files).setdefault(p.rstrip("/"), name)
    reasons = []
    for f in sorted(changed):
        under = next((d for d in dirs if f.startswith(d + "/")), None)
        if f == rel_list:
            reasons.append(f"{f} (the list itself; regenerate it with --write, never hand-edit it)")
        elif f in files:
            reasons.append(f"{f} (declared input of {files[f]})")
        elif under and f.endswith(SCRIPT_SUFFIXES):
            reasons.append(f"{f} (script under {under}/, declared by {dirs[under]})")
        elif f.endswith(("CMakeLists.txt", ".cmake")):
            path = root / f
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                text = ""
            # A deleted CMake file may have removed a registration.
            if f.startswith("test/") or "add_test(" in text or not path.exists():
                reasons.append(f"{f} (CMake that can register tests)")
    return reasons


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("--build-dir")
    ap.add_argument("--inventory-json", help="a saved `ctest --show-only=json-v1` document (tests)")
    ap.add_argument("--list", default=None, help=f"the checked-in list (default {DEFAULT_LIST})")
    ap.add_argument("--base", default=None, help="diff-scope --check to changes since this ref "
                    "(default: PULP_SCRIPT_INPUTS_BASE, origin/$GITHUB_BASE_REF, or HEAD^1 in a merge group)")
    ap.add_argument("--full", action="store_true", help="--check every entry, ignoring any base")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    a = ap.parse_args(argv[1:])
    if not a.build_dir and not a.inventory_json:
        ap.error("--build-dir or --inventory-json is required")
    root = Path(a.repo_root).resolve()
    list_path = Path(a.list) if a.list else root / DEFAULT_LIST
    inventory = load_inventory(a.build_dir, a.inventory_json)
    if inventory is None:
        return 2
    build_dir = Path(a.build_dir).resolve() if a.build_dir else None
    current = build_list(inventory, root, build_dir)
    total_scripts = sum(1 for t in inventory.get("tests", []) if t.get("command") and
                        os.path.basename(t["command"][0]).startswith(INTERPRETERS))
    if a.write:
        list_path.parent.mkdir(parents=True, exist_ok=True)
        list_path.write_text(json.dumps(current, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print(f"script-test-inputs: wrote {len(current['tests'])} declared tests "
              f"(of {total_scripts} interpreter-driven entries) to {list_path}")
        excluded = outside_gate_profile(inventory, root)
        if excluded:
            print(f"script-test-inputs: excluded {len(excluded)} test(s) registered from examples/: "
                  f"the gate configures with PULP_BUILD_EXAMPLES=OFF and never lists them")
        if build_dir is None:
            print("script-test-inputs: note: no --build-dir, so an entry generated into a build tree "
                  f"could not be recorded as {BINARY_DIR_TOKEN}; pass --build-dir alongside --inventory-json")
        return 0
    try:
        checked_in = json.loads(list_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        checked_in = {"tests": {}}
    problems = drift(current, checked_in)
    if not current["tests"]:
        print("script-test-inputs: ERROR: no script-driven test found; wrong build directory?", file=sys.stderr)
        return 2
    # Diff-scoped: a generated file drifts every time main moves, and a check
    # that reddens every PR for someone else's script is a treadmill. Only
    # drift the PR's own change reaches (its diff touches the test's entry
    # script or an old/new input) blocks; the rest is reported as advisory.
    base = None if a.full else resolve_base(root, a.base)
    changed = changed_files(root, base) if base else None
    # A missing entry is this change's to add only when the change touches the
    # test's own script: a test absent from every macOS inventory (a Linux-only
    # registration) can never be in a macOS-written list, and a shared
    # directory input must not turn that into a red on every unrelated PR.
    def owns(kind: str, name: str, paths: set[str]) -> bool:
        if changed is None:
            return True
        if kind == "missing from list":
            entry = current["tests"][name].get("entry") or ""
            return bool(entry) and entry in changed
        return touched_by(changed, paths)
    blocking = [pr for pr in problems if owns(*pr)]
    advisory = [pr for pr in problems if pr not in blocking]
    scope = f"diff-scoped against {base}" if base else "full compare (no base resolved)"
    if advisory:
        print(f"script-test-inputs: note: {len(advisory)} entr{'y' if len(advisory) == 1 else 'ies'} drifted from "
              f"scripts this change does not touch ({scope}); not blocking. Refresh when convenient with\n"
              f"  python3 tools/scripts/script_test_inputs.py --build-dir {a.build_dir or '<configured build dir>'} --write")
        for kind, name, _ in advisory[:15]:
            print(f"  {kind}: {name}")
    if blocking:
        build_hint = a.build_dir or "<configured build dir>"
        fix = ("Fix: regenerate the list from a configure of THIS tree and commit it:\n"
               f"  python3 tools/scripts/script_test_inputs.py --build-dir {build_hint} --write\n"
               f"  git add {DEFAULT_LIST.as_posix()}")
        print(f"script-test-inputs: {len(blocking)} drift problem(s) in scripts this change touches ({scope}).")
        for kind, name, _ in blocking[:40]:
            print(f"  {kind}: {name}")
        print(fix)
        if advisory_here():
            # The enforcement point is the PULL-REQUEST HEAD, where the pr-fast
            # tier runs on every push and the author sees the verdict. A merge
            # group is too late and too expensive: it ejects the whole batch,
            # and a PR whose head ran before this check existed gets no earlier
            # signal. A stale entry today only informs the shadow selector, so
            # the group reports and lets the PR land; the next push to any PR
            # touching those scripts is blocked until the list is regenerated.
            # If a GATING selector ever consumes this list, drop this advisory
            # branch: a stale list would then skip real tests in the group.
            names = ", ".join(name for _, name, _ in blocking[:10])
            print(f"::warning title=script-test inputs stale (advisory in a merge group)::{names} — "
                  f"regenerate with script_test_inputs.py --write on the next push")
            return 0
        return 1
    print(f"script-test-inputs: OK, {len(current['tests'])} declared tests in sync for this change "
          f"({scope}; {total_scripts} interpreter-driven entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
