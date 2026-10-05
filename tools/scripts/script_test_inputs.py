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

Compiled tests are folded into the same list under `executables`, one
`kind: compiled` entry per test executable that reads the checkout at run
time. Configure writes the evidence (tools/cmake/PulpTestData.cmake): every
test executable's sources, the definitions that point into the checkout, and
a `<exe>.inputs.json` per `pulp_test_data()` declaration. A source that names
PULP_SOURCE_DIR, test/fixtures, or one of its executable's checkout
definitions reads data; without a declaration covering it the executable is
`data: undeclared` and a selector must never skip it. `--check` also holds a
ratchet: a source that is undeclared here but was not undeclared in the base
list fails, so the undeclared backlog can only shrink.

  script_test_inputs.py --build-dir <dir> --data-summary   # compiled data counts, JSON

Exit codes: 0 in sync (or written); 1 drift or a new undeclared source
(`--check`); 2 inventory unreadable.
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Comments are dropped before a source is scanned, so prose such as "the
# effect system (bloom)" or a program named in a comment is not a call.
from gate_common import strip_c_comments  # noqa: E402

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
# The compiled entries are configuration-dependent too: examples add runtime
# edges, and a sanitizer or Debug configure changes which targets exist. The
# list is generated from the gate's configure, so a build of any other shape
# cannot be compared with it, or written into it.
SKIP_EXIT = 77
# The list is written from the required macOS gate's configure. Another
# platform registers a different set of tests (Linux-only selftests, other
# feature switches), so its registrations cannot be compared with the list.
GATE_SYSTEM_NAME = "Darwin"
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


def _cache_values(build_dir: Path) -> dict[str, str] | None:
    try:
        text = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    out = {}
    for line in text.splitlines():
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):[A-Z]+=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2).strip()
    return out


def outside_gate_profile_build(build_dir: Path | None) -> list[str]:
    """Why this build's compiled entries cannot stand for the gate's: each
    switch whose value differs from the gate's configure (examples off, a
    Release build, no sanitizer). Empty for a gate-shaped build, and for one
    with no CMakeCache.txt to read (no build tree, no compiled entries)."""
    cache = _cache_values(build_dir) if build_dir else None
    if cache is None:
        return []
    off = lambda v: v.upper() in ("", "OFF", "FALSE", "0", "NO", "N", "NONE") or v.upper().endswith("-NOTFOUND")
    reasons = []
    if not off(cache.get("PULP_BUILD_EXAMPLES", "OFF")):
        reasons.append(f"PULP_BUILD_EXAMPLES={cache['PULP_BUILD_EXAMPLES']} (the gate configures OFF)")
    if not off(cache.get("PULP_SANITIZER", "")):
        reasons.append(f"PULP_SANITIZER={cache['PULP_SANITIZER']} (the gate builds without one)")
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    if build_type and build_type != "Release":
        reasons.append(f"CMAKE_BUILD_TYPE={build_type} (the gate builds Release)")
    return reasons


def configured_system(build_dir: Path) -> str | None:
    """CMAKE_SYSTEM_NAME as the configure recorded it. It is not a cache
    variable: CMake writes it to CMakeFiles/<version>/CMakeSystem.cmake."""
    for path in sorted(build_dir.glob("CMakeFiles/*/CMakeSystem.cmake"),
                       key=lambda q: q.stat().st_mtime, reverse=True):
        try:
            m = re.search(r'set\(CMAKE_SYSTEM_NAME "([^"]*)"\)', path.read_text(encoding="utf-8"))
        except OSError:
            continue
        if m:
            return m.group(1)
    return None


def outside_gate_platform(build_dir: Path | None) -> str | None:
    """Why this build cannot be compared with the list at all: it targets a
    different system than the gate's. None for a gate-platform build, and for
    one with no CMakeCache.txt to read."""
    system = configured_system(build_dir) if build_dir else None
    if not system or system == GATE_SYSTEM_NAME:
        return None
    return (f"CMAKE_SYSTEM_NAME={system} (the list is written from the {GATE_SYSTEM_NAME} gate's "
            "configure, and another platform registers different tests)")


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


TEST_DATA_DIR = Path("test") / "test-data"
# Text in a compiled test source that means it opens files from the checkout.
DATA_SIGNALS = ("PULP_SOURCE_DIR", "test/fixtures")
# Finding the checkout by walking up from the working directory, with no
# definition at all: ctest runs a test in <build>/<dir>, so two levels up is
# the checkout root. One level up is the build tree, where tests find binaries.
WALK_UP_SIGNAL = "current_path()/../.."
# Or from the test's own source file, which lives in the checkout.
SOURCE_FILE_SIGNAL = "path(__FILE__)"
SOURCE_FILE = re.compile(r"path\(\s*__FILE__\s*\)")
WALK_UP = re.compile(r'current_path\(\)\s*/\s*(?:"\.\./\.\.|"\.\."\s*/\s*"\.\.")')

# Calls that start or load another program or module. The class names are the repo's
# own process API (core/platform/child_process.hpp,
# core/events/child_process_manager.hpp; this script's selftest checks every
# class there that starts a process is listed), plus the C and platform
# entry points. A compiled test whose sources (or the test/ headers they
# include) call one runs something at run time; whether that something is a
# program this repo builds is what its spawn edges, or a reviewed
# pulp_test_spawns(NONE REASON ...), say.
SPAWN_CLASSES = ("ChildProcess", "ChildProcessManager", "ConnectedChildProcess")
# And its free functions that run a program.
SPAWN_FUNCTIONS = ("exec",)
# The repo's plugin and module loaders (core/host): PluginSlot::load, the
# scanner calls that open bundles from disk, and the dlopen shim. PluginSlot alone is an interface that
# tests implement in memory, so only its loader counts.
LOAD_APIS = (r"PluginSlot::load\s*\(", r"scan_clap_bundle\s*\(", r"scan_vst3_bundle\s*\(",
             r"scan_lv2_bundle\s*\(", r"scan_directory\s*\(", r"scan_audio_units\s*\(", r"dl_open\s*\(",
             r"load_node_pack\s*\(", r"add_plugin_node\s*\(", r"add_plugin_node_from_drop\s*\(",
             r"GraphSerializer::from_json\s*\(")
# Public core/host functions that reach a loader but are not in LOAD_APIS,
# each with the mechanism that covers a test calling it (loader_coverage).
LOAD_API_EXEMPTIONS = Path("tools/ci/load_api_exemptions.json")
_PLATFORM_LOADER = re.compile(r"(?<![\w.>])(?:::)?(?:dlopen|LoadLibrary(?:Ex)?[AW]?|CFBundleCreate\w*"
                              r"|CFBundleLoadExecutable\w*)\s*\(")
_FUNCTION_DEFINITION = re.compile(
    r"(?<![\w:~])((?:\w+::)*~?\w+)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*"
    r"(?:(?:const|noexcept|override|final)\s*)*(?:->\s*[^{;]+)?\{")
_NOT_A_FUNCTION = frozenset({"if", "for", "while", "switch", "catch", "return", "sizeof", "defined"})
_HOST_SOURCES = (".cpp", ".cc", ".mm", ".hpp", ".h")


def _definitions(root: Path, prefix: str) -> list[tuple[str, str, str]]:
    """(qualified name, file, body) for every function defined under prefix."""
    out = []
    for path in sorted((root / prefix).rglob("*")):
        if path.suffix not in _HOST_SOURCES or not path.is_file():
            continue
        text = strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
        for match in _FUNCTION_DEFINITION.finditer(text):
            name = match.group(1)
            if name in _NOT_A_FUNCTION:
                continue
            end, depth = match.end(), 1
            while end < len(text) and depth:
                depth += {"{": 1, "}": -1}.get(text[end], 0)
                end += 1
            out.append((name, path.relative_to(root).as_posix(), text[match.end():end]))
    return out


def loader_coverage(root: Path) -> list[str]:
    """Public core/host functions that reach a plugin or module loader but
    that LOAD_APIS cannot see called, and are not exempt.

    The seeds are the platform loaders, dl_open, every LOAD_APIS function and
    every core/host function whose own body calls a platform loader (public
    or not, so a public wrapper of an internal loader is caught). A function
    is public when it is defined in, or its name is declared in, a header
    under core/host/include. One level, by name: a public function whose
    body calls a seed must match a LOAD_APIS pattern or be exempt."""
    definitions = _definitions(root, "core/host")
    direct = {name for name, _, body in definitions if _PLATFORM_LOADER.search(body)}
    seed_names = sorted({name.split("::")[-1] for name in direct} | {"dl_open"})
    seed_call = re.compile(r"(?<![\w])(?:%s)\s*\(" % "|".join(map(re.escape, seed_names)))
    load_apis = [re.compile(r"\b" + api) for api in LOAD_APIS]
    headers = "\n".join(strip_c_comments(p.read_text(encoding="utf-8", errors="replace"))
                        for p in sorted((root / "core/host/include").rglob("*.hpp")))
    exempt = set((_read_json(root / LOAD_API_EXEMPTIONS) or {}).get("exemptions") or {})
    missing, seen = [], set()
    for name, rel, body in definitions:
        short = name.split("::")[-1]
        public = rel.startswith("core/host/include/") or re.search(r"\b%s\s*\(" % re.escape(short), headers)
        if not public or name in seen:
            continue
        if not (_PLATFORM_LOADER.search(body) or seed_call.search(body) or any(a.search(body) for a in load_apis)):
            continue
        seen.add(name)
        if any(a.search(name + "(") or a.search(short + "(") for a in load_apis) or name in exempt:
            continue
        missing.append(f"{name} ({rel})")
    return sorted(missing)
SPAWN_SIGNAL = re.compile(
    r"\b(?:%s)\b" % "|".join(SPAWN_CLASSES)
    + r"|(?<![\w.>])(?:%s)\s*\(" % "|".join(SPAWN_FUNCTIONS)
    + r"|\bposix_spawnp?\s*\(|\bpopen\s*\(|(?:\bstd::|(?<![\w.>:]))system\s*\(|\bexec[lv]p?e?\s*\("
    + r"|\bfork\s*\(|\bNSTask\b|\bCreateProcess[AW]?\s*\("
    # Loading a module or plugin bundle at run time is the same edge as running
    # a program: the repo's own loaders, then the platform ones.
    + r"|\b(?:%s)" % "|".join(LOAD_APIS)
    + r"|\bdlopen\s*\(|\bLoadLibrary(?:Ex)?[AW]?\s*\(|\bCFBundle(?:Create|LoadExecutable)\w*\s*\(")
EXECUTABLE_SCANS = ("data", "spawns")
INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s*"([^"]+)"', re.M)


def _read_json(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def source_signals(root: Path, rel: str, defines: list[str]) -> list[str]:
    """The data-read signals a source's text carries: the fixed ones plus the
    names of its executable's definitions that point into the checkout."""
    try:
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    hits = [sig for sig in DATA_SIGNALS if sig in text]
    if WALK_UP.search(text):
        hits.append(WALK_UP_SIGNAL)
    if SOURCE_FILE.search(text):
        hits.append(SOURCE_FILE_SIGNAL)
    for name in defines:
        if name not in hits and re.search(r"\b%s\b" % re.escape(name), text):
            hits.append(name)
    return hits


def _test_include_closure(root: Path, rel: str) -> list[str]:
    """`rel` and every header under test/ it reaches through quoted includes."""
    seen: list[str] = []
    stack = [rel]
    while stack:
        cur = stack.pop()
        if cur in seen:
            continue
        try:
            text = (root / cur).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        seen.append(cur)
        for name in INCLUDE.findall(text):
            for base in (os.path.dirname(cur), "test", "test/support"):
                cand = os.path.normpath(os.path.join(base, name))
                if cand.startswith("test/") and (root / cand).is_file():
                    stack.append(cand)
                    break
    return seen


def spawning_sources(root: Path, sources: list[str]) -> list[str]:
    """The files (sources and the test/ headers they include) that call a
    process API."""
    hits = set()
    for src in sources:
        for f in _test_include_closure(root, src):
            try:
                if SPAWN_SIGNAL.search(strip_c_comments((root / f).read_text(encoding="utf-8", errors="replace"))):
                    hits.add(f)
            except OSError:
                continue
    return sorted(hits)


# A string literal, and the file suffixes a built program or module carries.
STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ARTIFACT_SUFFIX = re.compile(r"\.(?:exe|clap|vst3|component|so|dylib|bundle|app|dll)$", re.I)


CATCH_INCLUDE = re.compile(r"^(.+)-[0-9a-f]+_include\.cmake$")
# add_test(<name> "<program>" ...): the program is the first quoted argument
# after the name, which CMake writes as a bracket or quoted argument.
CTEST_COMMAND = re.compile(r'add_test\(\s*(?:\[(=*)\[.*?\]\1\]|"(?:[^"\\]|\\.)*"|[^\s()]+)\s+"((?:[^"\\]|\\.)*)"', re.S)
CTEST_INCLUDE = re.compile(r'include\(\s*"([^"]+)"\s*\)')


def ctest_programs(build_dir: Path) -> set[str] | None:
    """The built programs ctest runs, by file name: the program of every
    add_test whose program lies under the build directory, plus the target a
    Catch2 discovery include is named after (its tests only exist once the
    target is built). A built program a test only passes as an argument (to
    a script, say) is not one: the test that runs it is scanned instead.
    None when the build directory has no CTestTestfile."""
    files = sorted(build_dir.rglob("CTestTestfile.cmake"))
    if not files:
        return None
    build = os.path.realpath(build_dir)
    names: set[str] = set()
    for f in files:
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in CTEST_COMMAND.finditer(text):
            prog = m.group(2)
            if os.path.isabs(prog) and os.path.realpath(prog).startswith(build + os.sep):
                names.add(re.sub(r"\.exe$", "", os.path.basename(prog), flags=re.I))
        for inc in CTEST_INCLUDE.findall(text):
            m = CATCH_INCLUDE.match(os.path.basename(inc))
            if m:
                names.add(m.group(1))
    return names


def test_executables(build_dir: Path | None) -> dict[str, dict] | None:
    """executables.json's records for the executables this list scans: every
    one under test/, and any other one ctest runs. Each is keyed by its
    artifact, the program name a ctest command carries and so what a
    selector looks it up by (pulp-cli is run as pulp-cpp), and keeps its
    target name under `target`. None without the index."""
    if build_dir is None:
        return None
    index = _read_json(build_dir / TEST_DATA_DIR / "executables.json")
    if index is None:
        return None
    artifacts = load_runtime_artifacts(build_dir) or {}
    run = ctest_programs(build_dir)
    out = {}
    for target, rec in sorted((index.get("executables") or {}).items()):
        program = str((artifacts.get(target) or {}).get("artifact") or target)
        # A ctest command names the artifact; a Catch2 discovery include is
        # named after the target.
        if not (rec.get("under_test", True) or run is None or program in run or target in run):
            continue
        if program in out:
            raise ValueError(f"{out[program]['target']} and {target} both build {program}; "
                             "a selector could not tell their tests apart")
        out[program] = dict(rec, target=target)
    return out


def load_runtime_artifacts(build_dir: Path | None) -> dict[str, dict] | None:
    """Every program or module the tree builds: target -> {artifact, runtime_targets}."""
    if build_dir is None:
        return None
    doc = _read_json(build_dir / TEST_DATA_DIR / "runtime-targets.json")
    return None if doc is None else dict(doc.get("artifacts") or {})


def named_programs(root: Path, sources: list[str], artifacts: dict[str, dict]) -> dict[str, list[str]]:
    """Targets whose artifact file name appears as a path component of a string
    literal in `sources` or the test/ headers they include, with where."""
    by_name: dict[str, set[str]] = {}
    for target, rec in artifacts.items():
        by_name.setdefault(str(rec.get("artifact") or target).lower(), set()).add(target)
    found: dict[str, list[str]] = {}
    for src in sources:
        for f in _test_include_closure(root, src):
            try:
                text = strip_c_comments((root / f).read_text(encoding="utf-8", errors="replace"))
            except OSError:
                continue
            for literal in STRING_LITERAL.finditer(text):
                for part in re.split(r"[/\\]", literal.group(1)):
                    for target in by_name.get(ARTIFACT_SUFFIX.sub("", part).lower(), ()):
                        line = text.count("\n", 0, literal.start()) + 1
                        found.setdefault(target, []).append(f"{f}:{line}")
    return found


def runtime_closure(direct: list[str], artifacts: dict[str, dict]) -> set[str]:
    """`direct` and every program or module those depend on in turn."""
    seen, stack = set(), list(direct)
    while stack:
        target = stack.pop()
        if target in seen:
            continue
        seen.add(target)
        stack.extend((artifacts.get(target) or {}).get("runtime_targets") or [])
    return seen


def spawn_state(root: Path, rec: dict, exe: str = "",
                artifacts: dict[str, dict] | None = None) -> tuple[str | None, list[str], list[str]]:
    """(`declared` | `none` | `undeclared` | None, the spawning files, the
    programs it names without an edge) for one executables.json record. None:
    nothing in it starts a process; `artifacts` None (no runtime-targets
    index) leaves every spawner undeclared.

    An edge or a reviewed NONE is not enough on its own: every program this
    tree builds that the code names (a path component of a string literal,
    such as "pulp-cpp") must be reached by an edge, directly or through
    another program it runs, or be reviewed as named but not run. One that is
    not leaves the executable `undeclared`."""
    spawning = spawning_sources(root, list(rec.get("sources") or []))
    if rec.get("absent_spawns"):
        # It declares a program this configuration does not build.
        return "undeclared", spawning, sorted(rec["absent_spawns"])
    if not spawning:
        return None, [], []
    if rec.get("runtime_targets"):
        state = "declared"
    elif rec.get("spawns_none"):
        state = "none"
    else:
        return "undeclared", spawning, []
    if artifacts is None:
        # Without the index of what the tree builds, the names cannot be checked.
        return "undeclared", spawning, []
    named = named_programs(root, list(rec.get("sources") or []), artifacts)
    covered = runtime_closure(list(rec.get("runtime_targets") or []), artifacts)
    covered |= set(rec.get("named_not_run") or []) | {exe}
    # Targets that share an artifact name (one plugin's AU, VST3 and CLAP
    # builds) are one name in the code; an edge to any of them answers it.
    by_artifact: dict[str, set[str]] = {}
    for target in named:
        by_artifact.setdefault(str(artifacts[target].get("artifact") or target).lower(), set()).add(target)
    unmatched = sorted(t for group in by_artifact.values() if not group & covered for t in group)
    return ("undeclared" if unmatched else state), spawning, unmatched


# pulp_test_spawns(<test> NONE REASON "...") reviews that a test starts and
# loads nothing this tree builds. A build artifact can still reach such a test
# without an edge when its sources name a build path: an environment variable
# that names a build directory or binary, or a directory walk beside a literal
# build path. Those reviews must be confirmed by the test's owner, in data.
NONE_BUILD_PATH_REVIEWS = Path("tools/ci/spawn_none_build_path_reviews.json")
_GETENV = re.compile(r'\bgetenv\s*\(\s*"([^"]+)"')
_BUILD_ENV = re.compile(r"BUILD|BINARY|_BIN$|OUT(?:PUT)?_DIR|ARTIFACT|^PATH$")
_DIR_WALK = re.compile(r"\bdirectory_iterator\b|\bglob\s*\(")
_BUILD_LITERAL = re.compile(r'"[^"\n]*(?:\bbuild(?:-[\w.-]+)?/|CMAKE_BINARY_DIR)[^"\n]*"')
_PLACEHOLDER_REASON = re.compile(r"\b(?:legacy|predates|todo|tbd|unknown)\b", re.I)


def none_build_path_signals(root: Path, sources: list[str]) -> list[str]:
    """How a NONE-reviewed test's sources could reach a build artifact."""
    found = []
    for src in sources:
        path = root / src
        if not path.is_file():
            continue
        text = strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
        names = sorted({n for n in _GETENV.findall(text) if _BUILD_ENV.search(n)})
        if names:
            found.append(f"{src}: getenv {', '.join(names)}")
        literal = _BUILD_LITERAL.search(text)
        if literal and _DIR_WALK.search(text):
            found.append(f"{src}: walks a directory and names {literal.group(0)}")
    return found


def none_review_problems(root: Path, build_dir: Path | None) -> list[tuple[str, str, set[str]]]:
    """NONE reviews whose reason is no claim, or whose sources name a build
    path without an owner's confirmation in NONE_BUILD_PATH_REVIEWS."""
    index = test_executables(build_dir) or {}
    reviews = (_read_json(root / NONE_BUILD_PATH_REVIEWS) or {}).get("reviews") or {}
    problems = []
    for exe, rec in sorted(index.items()):
        if not rec.get("spawns_none"):
            continue
        target = rec.get("target", exe)
        reason = rec.get("spawns_none_reason") or ""
        if not reason.strip() or _PLACEHOLDER_REASON.search(reason):
            problems.append(("NONE review states no claim", f"{target}: {reason!r}", set()))
        signals = none_build_path_signals(root, list(rec.get("sources") or []))
        if signals and target not in reviews:
            problems.append(("NONE review names a build path", f"{target}: {'; '.join(signals)}",
                             set(rec.get("sources") or [])))
    for target in sorted(set(reviews) - {rec.get("target", exe) for exe, rec in index.items()
                                          if rec.get("spawns_none")}):
        problems.append(("stale NONE build-path review", target, {NONE_BUILD_PATH_REVIEWS.as_posix()}))
    return problems


def compiled_entries(root: Path, build_dir: Path | None) -> dict | None:
    """`kind: compiled` entries from the configure-time evidence, or None when
    the build directory carries none (a tree configured before the helper)."""
    index = test_executables(build_dir)
    if index is None:
        return None
    out = {}
    artifacts = load_runtime_artifacts(build_dir)
    # A configure-generated source (tools/cli/generated/*.cpp) arrives
    # repo-relative under the build directory's own name when that directory
    # sits inside the checkout. Record it under the token instead, as entry
    # scripts are, so the list does not depend on which build dir wrote it.
    build_rel = _rel(build_dir, root)

    def tokenized(paths) -> list[str]:
        if not build_rel or build_rel == ".":
            return sorted(paths)
        prefix = build_rel + "/"
        return sorted(f"{BINARY_DIR_TOKEN}/{p[len(prefix):]}" if p.startswith(prefix) else p for p in paths)

    for exe, rec in sorted(index.items()):
        target = rec.get("target", exe)
        decl = _read_json(build_dir / TEST_DATA_DIR / f"{target}.inputs.json") or {}
        declared_sources = set(decl.get("sources") or [])
        defines = list(rec.get("tree_defines") or [])
        reading = {src for src in rec.get("sources") or [] if source_signals(root, src, defines)}
        data_sources = reading | declared_sources
        spawns, spawning, unmatched = spawn_state(root, rec, target, artifacts)
        if not data_sources and spawns is None and not decl.get("whole_checkout"):
            continue
        whole = bool(decl.get("whole_checkout"))
        # WHOLE_CHECKOUT covers every read, so nothing it reads is undeclared.
        undeclared = [] if whole else sorted(reading - declared_sources)
        entry = {"kind": "compiled",
                 "data": ("whole_checkout" if whole else
                          ("undeclared" if undeclared else "declared") if data_sources else "none"),
                 "inputs": sorted(set(decl.get("inputs") or [])),
                 "sources": tokenized(data_sources), "undeclared_sources": tokenized(undeclared)}
        if data_sources or whole:
            # Only the sources a signal matched, so a reader can tell a scan
            # that saw the declared readers from one that only echoes
            # pulp_test_data's SOURCES.
            entry["detected_sources"] = tokenized(reading)
        if spawns is not None:
            entry["spawns"] = spawns
            entry["spawning_sources"] = spawning
            entry["runtime_targets"] = sorted(rec.get("runtime_targets") or [])
            if unmatched:
                entry["unmatched_programs"] = unmatched
        out[exe] = entry
    return out


def data_summary(root: Path, build_dir: Path | None) -> dict | None:
    """The data-manifest proxy: executables with a declaration over those that
    read PULP_SOURCE_DIR, plus the wider undeclared count."""
    index = test_executables(build_dir)
    entries = compiled_entries(root, build_dir)
    if index is None or entries is None:
        return None
    psd = sorted(exe for exe, rec in index.items()
                 if any("PULP_SOURCE_DIR" in source_signals(root, s, []) for s in rec.get("sources") or []))
    with_manifest = sorted(exe for exe in psd if entries.get(exe, {}).get("inputs"))
    return {"executables": len(index),
            "reading_pulp_source_dir": len(psd), "reading_pulp_source_dir_with_manifest": len(with_manifest),
            "data_reading": sum(1 for e in entries.values() if e["data"] != "none"),
            "declared": sum(1 for e in entries.values() if e["data"] == "declared"),
            "undeclared": sum(1 for e in entries.values() if e["data"] == "undeclared"),
            "undeclared_sources": sorted({s for e in entries.values() for s in e["undeclared_sources"]})}


def undeclared_sources(doc: dict) -> set[str]:
    return {s for e in (doc.get("executables") or {}).values() for s in e.get("undeclared_sources") or []}


def base_list(root: Path, base: str | None) -> dict | None:
    """The list as the base ref has it, or None when the base predates
    compiled entries (then the checked-in list is the baseline)."""
    if not base:
        return None
    proc = subprocess.run(["git", "-C", str(root), "show", f"{base}:{DEFAULT_LIST.as_posix()}"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    try:
        doc = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return None
    return doc if "executables" in doc else None


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
    doc = {"schema": SCHEMA, "tests": dict(sorted(tests.items()))}
    compiled = compiled_entries(root, build_dir)
    if compiled is not None:
        doc["executables"] = compiled
        # What every test executable was scanned for. An executable with no
        # entry was scanned and showed neither, which a list written before a
        # scan existed cannot say: a reader treats a missing scan as unknown.
        doc["executables_scanned_for"] = list(EXECUTABLE_SCANS)
        # And which executables were scanned: a missing entry means "clean" only
        # for a name in this list; one that is not here was never scanned.
        scanned = test_executables(build_dir) or {}
        doc["executables_scanned"] = sorted(scanned)
        # Every name above is the program ctest runs; the few built from a
        # target of another name map back to it, which is the name
        # pulp_test_data() and pulp_test_spawns() take.
        doc["executable_targets"] = {name: rec["target"] for name, rec in sorted(scanned.items())
                                     if rec["target"] != name}
    return doc


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
    ce = checked_in.get("executables") or {}
    for name, rec in (current.get("executables") or {}).items():
        paths = compiled_entry_paths(rec)
        if name not in ce:
            problems.append(("missing compiled entry", name, paths))
        elif ce[name] != rec:
            paths |= compiled_entry_paths(ce[name])
            problems.append(("stale compiled entry", name, paths))
    return problems


def compiled_entry_paths(rec: dict) -> set[str]:
    """What a change must touch to own a compiled entry's drift: the sources
    whose text decided it, data readers and spawn sites alike (a spawns-only
    entry has no `sources`), and its declared inputs."""
    return (set(rec.get("sources") or []) | set(rec.get("spawning_sources") or [])
            | set(rec.get("undeclared_sources") or []) | set(rec.get("inputs") or []))


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
    mode.add_argument("--data-summary", action="store_true", help="print the compiled-test data counts as JSON")
    a = ap.parse_args(argv[1:])
    if not a.build_dir and not a.inventory_json:
        ap.error("--build-dir or --inventory-json is required")
    root = Path(a.repo_root).resolve()
    list_path = Path(a.list) if a.list else root / DEFAULT_LIST
    inventory = load_inventory(a.build_dir, a.inventory_json)
    if inventory is None:
        return 2
    build_dir = Path(a.build_dir).resolve() if a.build_dir else None
    if a.data_summary:
        summary = data_summary(root, build_dir)
        if summary is None:
            print("script-test-inputs: no compiled-test evidence under the build directory "
                  f"({TEST_DATA_DIR.as_posix()}); reconfigure it", file=sys.stderr)
            return 2
        print(json.dumps(summary, indent=1, sort_keys=True))
        return 0
    off_platform = outside_gate_platform(build_dir)
    if off_platform and a.write:
        print("script-test-inputs: refusing to write: " + off_platform + ".\nRegenerate from a "
              f"{GATE_SYSTEM_NAME} gate-profile configure.", file=sys.stderr)
        return 2
    if off_platform:
        print("script-test-inputs: SKIPPED: " + off_platform + ". The required gate checks the list; "
              "this is a skip, not a pass.")
        return SKIP_EXIT
    current = build_list(inventory, root, build_dir)
    off_profile = outside_gate_profile_build(build_dir) if "executables" in current else []
    total_scripts = sum(1 for t in inventory.get("tests", []) if t.get("command") and
                        os.path.basename(t["command"][0]).startswith(INTERPRETERS))
    if a.write and off_profile:
        print("script-test-inputs: refusing to write: the compiled entries depend on the configuration and "
              "this build is not the gate's: " + "; ".join(off_profile) + ".\nRegenerate from a gate-profile "
              "configure (-DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=OFF, no PULP_SANITIZER).",
              file=sys.stderr)
        return 2
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
    if off_profile:
        # Script entries are filtered to the gate's registrations and still
        # compare; this build's compiled entries cannot, so they are left out
        # and the result says so.
        current = {k: v for k, v in current.items() if k != "executables"}
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
        if kind == "missing compiled entry":
            # Only the entry's own sources, not its declared inputs: a shared
            # fixture directory must not make every new entry this change's.
            rec = current["executables"][name]
            return bool((compiled_entry_paths(rec) - set(rec.get("inputs") or [])) & changed)
        return touched_by(changed, paths)
    blocking = [pr for pr in problems if owns(*pr)]
    advisory = [pr for pr in problems if pr not in blocking]
    # The undeclared-data ratchet: a source that reads the checkout without a
    # declaration and is not in the base list's backlog. It is this change's
    # when the change edits that source, or edits the test tree's CMake while
    # the executable was already listed (a dropped declaration). An executable
    # the base list never saw and whose source this change leaves alone is
    # another configuration's (a Linux-only test against a macOS-written list):
    # reported, not blocking.
    if "executables" in current:
        baseline = base_list(root, base) or checked_in
        known = undeclared_sources(baseline)
        listed = set(baseline.get("executables") or {})
        cmake_touched = changed is not None and any(
            f.startswith("test/") and (f.endswith(".cmake") or f.endswith("CMakeLists.txt"))
            or f == "tools/cmake/PulpTestData.cmake" for f in changed)
        for exe, rec in sorted(current["executables"].items()):
            for src in rec["undeclared_sources"]:
                if src in known:
                    continue
                item = ("new undeclared data source", f"{exe}: {src}", {src})
                if changed is None or src in changed or (exe in listed and cmake_touched):
                    blocking.append(item)
                else:
                    advisory.append(item)
    if "executables" in current:
        blocking.extend(none_review_problems(root, build_dir))
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
        if any(kind.startswith(("NONE review", "stale NONE")) for kind, _, _ in blocking):
            print("A pulp_test_spawns(<test> NONE REASON ...) review must state what makes the test load\n"
                  "nothing this tree builds. When its sources name a build path (a build-directory or binary\n"
                  f"environment variable, or a directory walk beside a build path), its owner confirms it in\n"
                  f"{NONE_BUILD_PATH_REVIEWS.as_posix()} with the reason the path never holds a tree-built artifact.")
        if any(kind == "new undeclared data source" for kind, _, _ in blocking):
            print("A compiled test source reads the checkout (it names PULP_SOURCE_DIR, test/fixtures, or a\n"
                  "definition pointing into the checkout) without declaring what it reads. Declare it next to\n"
                  "its registration in test/cmake/*_tests.cmake:\n"
                  "  pulp_test_data(<suite> PATHS <repo-relative files, dirs or globs>)\n"
                  "then regenerate the list. Undeclared tests can never be skipped by a selector.")
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
    if off_profile:
        print("script-test-inputs: SKIPPED the compiled entries: this build is not the gate's ("
              + "; ".join(off_profile) + "), and the list holds the gate's. They are checked on a "
              "gate-profile build.")
        return SKIP_EXIT
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
