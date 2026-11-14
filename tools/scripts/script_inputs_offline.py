#!/usr/bin/env python3
"""Build-free check: a push must not leave a test/ctest_script_inputs/ fragment stale.

`script-test-inputs-drift` (`script_test_inputs.py --check`) needs a configured
build, so a gate host (m3, m5) cannot run it before a push, and a fragment that
went stale when its test or the test's imports changed surfaced only on the
required macOS gate. `script_inputs_precheck.py` already catches a NEW script
test with no fragment; this catches the other half, a fragment the push makes
wrong, with no configure:

1. It lists the files the range changes (``--base...--head`` plus uncommitted
   edits) and selects the fragments those files can reach: a script test whose
   entry, recorded or newly resolved imports changed, or whose registering
   CMake file changed;
   and a compiled test executable whose recorded sources changed, which gained
   a new source next to one it already records, or which is new.
2. It rebuilds each selected record offline. A script test's ctest command is
   reconstructed from its static ``add_test(NAME ... COMMAND ...)`` call and
   ``set_tests_properties`` (variables a gate configure fixes, such as
   ``${CMAKE_SOURCE_DIR}``, are expanded), then run through the generator's own
   ``script_test_inputs.inputs_for``. A compiled entry is judged per changed
   file with the generator's own spawn and data signals.
3. It fails with the exact fragment diff and the command that writes it
   (``--write``), or, where only a configure can produce the record, with the
   configure-based regeneration command.

What a static read cannot reproduce is reported as NOT CHECKED with its
reason, never passed silently: a registration inside a function, macro or
foreach body, a command that names a variable only a configure resolves, a
configure-generated entry script, a grouped test suite, a record whose
content needs configure-time evidence (compile definitions, runtime targets).
The configured ``script-test-inputs-drift`` check stays the authority for those.

    python3 tools/scripts/script_inputs_offline.py --base origin/main
    python3 tools/scripts/script_inputs_offline.py --base origin/main --write

Exit 0 when nothing it can check is stale (NOT CHECKED lines may be printed),
1 when a fragment is stale or missing, 2 when the range cannot be read.
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ci"))

import cmake_registration_impact as registrations  # noqa: E402
import script_inputs_store as store  # noqa: E402
import script_test_inputs as sti  # noqa: E402

# What a gate configure expands each interpreter variable to. The generator
# keys on the program's basename, so any spelling of the interpreter will do.
INTERPRETERS = {
    "Python3_EXECUTABLE": "python3", "PYTHON_EXECUTABLE": "python3",
    "Python_EXECUTABLE": "python3", "CMAKE_COMMAND": "cmake",
    "_PULP_NODE_FOR_TESTS": "node", "NODE_EXECUTABLE": "node",
    "BASH": "bash", "BASH_EXECUTABLE": "bash", "PULP_PYTHON3_FOR_TESTS": "python3",
    "CMAKE_CTEST_COMMAND": "ctest",
    # Host tools outside the checkout: never an input.
    "CMAKE_NM": "/usr/bin/nm", "CMAKE_OSX_SYSROOT": "/nonexistent-pulp-sysroot",
}
# The build tree. The generator never records a path under it unless it is the
# entry script itself (a configure-generated runner, not checkable here), so a
# path outside the checkout stands in for it.
BINARY_STAND_IN = "/nonexistent-pulp-binary-dir"
BINARY_VARS = {"CMAKE_BINARY_DIR", "CMAKE_CURRENT_BINARY_DIR", "PROJECT_BINARY_DIR",
               "PULP_BINARY_DIR"}
SOURCE_VARS = {"CMAKE_SOURCE_DIR", "PROJECT_SOURCE_DIR", "PULP_SOURCE_DIR",
               "pulp_SOURCE_DIR", "Pulp_SOURCE_DIR"}
VAR = re.compile(r"\$\{([A-Za-z0-9_]+)\}")
GENEX = re.compile(r"\$<[^<>]*>")
SUITE_HELPER = "pulp_add_test_suite"
CONFIGURE_ONLY = "tools/scripts/configure_only.py"
REGENERATE_MANUAL = (
    "    npm ci --prefix tools/import-design/jsx-runtime\n"
    "    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON "
    "-DPULP_BUILD_EXAMPLES=OFF\n"
    "    python3 tools/scripts/script_test_inputs.py --build-dir build --write\n"
    "    git add test/ctest_script_inputs/\n"
    "  (on a gate host, configure on a TARTCI_ROLE=light host such as m1, or a VM)")
REGENERATE_CONFIGURE_ONLY = (
    "    python3 tools/scripts/configure_only.py -- \\\n"
    "        python3 tools/scripts/script_test_inputs.py --build-dir {build} --write\n"
    "    git add test/ctest_script_inputs/\n"
    "  (configure only, no compile; allowed on a gate host)")


def regenerate(root: Path) -> str:
    """The configure-based regeneration recipe this checkout supports."""
    return REGENERATE_CONFIGURE_ONLY if (root / CONFIGURE_ONLY).is_file() else REGENERATE_MANUAL


@dataclass
class Report:
    stale: list[str] = field(default_factory=list)          # exact diffs we can write
    needs_configure: list[str] = field(default_factory=list)  # certainly stale, content unknown
    not_checked: list[str] = field(default_factory=list)
    writes: dict[str, str] = field(default_factory=dict)    # store-relative path -> text
    checked: int = 0


# -- git -------------------------------------------------------------------

def _git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8", check=True).stdout


def changed_files(root: Path, base: str, head: str) -> set[str]:
    """Files the range changes, plus uncommitted edits when ``head`` is HEAD."""
    out = set(_git(root, "diff", "--name-only", "--no-renames", f"{base}...{head}").split())
    if head == "HEAD":
        out |= set(_git(root, "diff", "--name-only", "--no-renames", "HEAD").split())
    return {f for f in out if f}


def added_lines(root: Path, base: str, head: str, path: str) -> set[int]:
    """Head-side line numbers the range adds to ``path``."""
    text = _git(root, "diff", "-U0", "--no-renames", f"{base}...{head}", "--", path)
    lines: set[int] = set()
    for m in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", text, re.M):
        start, count = int(m.group(1)), int(m.group(2) or "1")
        lines.update(range(start, start + count))
    return lines


# -- static CMake reading --------------------------------------------------

def cmake_files(root: Path) -> list[str]:
    out = _git(root, "ls-files", "-z", "--", "*.cmake", "*CMakeLists.txt")
    return sorted(p for p in out.split("\0")
                  if p and not p.startswith(("external/", "docs/", "build")))


_INCLUDE_ARG = re.compile(r"^\$\{(CMAKE_SOURCE_DIR|CMAKE_CURRENT_SOURCE_DIR|CMAKE_CURRENT_LIST_DIR)\}/(.+)$")
# Files the test tree includes (test/CMakeLists.txt and its include chain),
# per checkout; set by check().
_TEST_CHAIN: dict[Path, set[str]] = {}
_INCLUDE_ANCESTORS: dict[tuple[Path, str | None], dict[str, set[str]]] = {}


def _included(root: Path, path: str, top_level_only: bool, ref: str | None = None) -> list[str]:
    """Repo paths ``path`` includes with a literal include() argument."""
    if ref is None:
        try:
            text = (root / path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return []
    else:
        result = subprocess.run(["git", "-C", str(root), "show", f"{ref}:{path}"],
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            return []
        text = result.stdout
    calls = registrations.calls(text)
    registrations._blocks(calls)
    here = os.path.dirname(path)
    out = []
    for call in calls:
        if call.command != "include" or not call.args or (top_level_only and call.block):
            continue
        m = _INCLUDE_ARG.match(call.args[0])
        if m:
            base = "" if m.group(1) == "CMAKE_SOURCE_DIR" else here
            out.append(os.path.normpath(os.path.join(base, m.group(2))))
        elif re.fullmatch(r"[A-Za-z0-9_]+", call.args[0]):
            out.append(f"tools/cmake/{call.args[0]}.cmake")
    return out


def test_chain(root: Path) -> set[str]:
    """Every file test/CMakeLists.txt reaches through include(): processed
    with CMAKE_CURRENT_SOURCE_DIR = test."""
    if root not in _TEST_CHAIN:
        seen: set[str] = set()
        stack = ["test/CMakeLists.txt"]
        while stack:
            cur = stack.pop()
            if cur in seen:
                continue
            seen.add(cur)
            stack.extend(p for p in _included(root, cur, False) if p.startswith("test/"))
        _TEST_CHAIN[root] = seen
    return _TEST_CHAIN[root]


def source_dir_of(path: str, root: Path | None = None) -> str | None:
    """CMAKE_CURRENT_SOURCE_DIR while ``path`` is processed, repo-relative, or
    None when only the include chain decides it. A test/cmake file the test
    tree does not include (a standalone consumer project, say) is None."""
    if os.path.basename(path) == "CMakeLists.txt":
        return os.path.dirname(path)
    if path.startswith("test/cmake/") and (root is None or path in test_chain(root)):
        return "test"
    return None


def include_ancestor_map(root: Path, ref: str | None = None) -> dict[str, set[str]]:
    """Map each test-tree CMake file to the files that include it."""
    key = (root, ref)
    if key in _INCLUDE_ANCESTORS:
        return _INCLUDE_ANCESTORS[key]
    parents: dict[str, set[str]] = {}
    seen: set[str] = set()
    stack = ["test/CMakeLists.txt"]
    while stack:
        path = stack.pop()
        if path in seen:
            continue
        seen.add(path)
        for child in _included(root, path, False, ref):
            parents.setdefault(child, set()).add(path)
            if child not in seen:
                stack.append(child)

    ancestors: dict[str, set[str]] = {}
    for target in seen:
        ancestors[target] = {target}
        stack = [target]
        while stack:
            path = stack.pop()
            for parent in parents.get(path, ()):
                if parent not in ancestors[target]:
                    ancestors[target].add(parent)
                    stack.append(parent)
    _INCLUDE_ANCESTORS[key] = ancestors
    return ancestors


def include_ancestors(root: Path, target: str, ref: str | None = None) -> set[str]:
    """CMake files whose include chain reaches ``target`` in the test tree."""
    return include_ancestor_map(root, ref).get(target, {target})


def _file_sets(calls: list[registrations.Call]) -> dict[str, list[str]]:
    """Variables a file sets exactly once, at top level, to a literal list."""
    seen: dict[str, list[list[str]]] = {}
    for call in calls:
        if call.command == "set" and call.args:
            # A set inside any block (an if branch the gate may not enter, a
            # loop, a function) is not a value a static read can trust.
            value = call.args[1:] if call.block is None else ["\0mutated"]
            seen.setdefault(call.args[0], []).append(value)
        elif call.command in ("list", "unset", "string", "math", "file",
                              "cmake_parse_arguments", "get_filename_component",
                              "cmake_path", "separate_arguments"):
            # Any identifier-shaped argument may be the output variable.
            for arg in call.args:
                if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", arg):
                    seen.setdefault(arg, []).append(["\0mutated"])
    return {name: values[0] for name, values in seen.items()
            if len(values) == 1 and "\0mutated" not in values[0]
            and not any(v in ("PARENT_SCOPE", "CACHE") for v in values[0])}


def expand(token: str, root: Path, path: str, sets: dict[str, list[str]],
           depth: int = 0) -> tuple[str | None, str]:
    """(expanded token, reason it cannot be) for one CMake argument."""
    token = GENEX.sub(BINARY_STAND_IN, token)
    unresolved = ""

    def sub(m: re.Match) -> str:
        nonlocal unresolved
        name = m.group(1)
        if name in INTERPRETERS:
            return INTERPRETERS[name]
        if name in SOURCE_VARS:
            return str(root)
        if name in BINARY_VARS:
            return BINARY_STAND_IN
        if name == "CMAKE_CURRENT_LIST_DIR":
            return str(root / os.path.dirname(path))
        if name == "CMAKE_CURRENT_SOURCE_DIR":
            sd = source_dir_of(path, root)
            if sd is not None:
                return str(root / sd)
        if name in sets and depth < 4:
            if len(sets[name]) != 1:
                unresolved = unresolved or f"${{{name}}}"
            else:
                raw = sets[name][0]
                # A semicolon-separated CMake list expands to several
                # arguments. Treating it as one path silently drops every
                # element after the first; only configure has the exact list
                # semantics, so keep this registration explicitly unchecked.
                if ";" in raw:
                    unresolved = unresolved or f"${{{name}}} (a multi-value CMake list)"
                else:
                    value, why = expand(raw, root, path, sets, depth + 1)
                    if value is not None:
                        return value
        unresolved = unresolved or f"${{{name}}}"
        return m.group(0)

    out = VAR.sub(sub, token)
    if unresolved:
        return None, f"names {unresolved}, which only a configure resolves"
    return out, ""


@dataclass
class Registration:
    path: str
    line: int
    command: list[str] | None
    reason: str  # why it cannot be reconstructed, or ""
    properties: dict[str, list[str]] = field(default_factory=dict)


def _properties(calls: list[registrations.Call], name: str) -> dict[str, list[str]]:
    props: dict[str, list[str]] = {}
    for call in calls:
        if call.command != "set_tests_properties" or "PROPERTIES" not in call.args:
            continue
        i = call.args.index("PROPERTIES")
        if name not in call.args[:i]:
            continue
        rest = call.args[i + 1:]
        for key, value in zip(rest[0::2], rest[1::2]):
            props.setdefault(key, []).append(value)
    return props


def _inside_body(call: registrations.Call) -> str:
    block = call.block
    while block is not None:
        if block.header.command in ("function", "macro", "foreach", "while"):
            return block.header.command
        block = block.header.block
    return ""


@dataclass
class Parsed:
    path: str
    text: str
    calls: list[registrations.Call]
    sets: dict[str, list[str]]


_PARSED: dict[Path, list[Parsed]] = {}


def parsed(root: Path) -> list[Parsed]:
    """Every tracked CMake file that registers a test or a test executable."""
    if root not in _PARSED:
        out = []
        for path in cmake_files(root):
            try:
                text = (root / path).read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if not any(word in text for word in ("add_test", "add_executable", SUITE_HELPER,
                                                 "target_sources")):
                continue
            calls = registrations.calls(text)
            registrations._blocks(calls)
            out.append(Parsed(path, text, calls, _file_sets(calls)))
        _PARSED[root] = out
    return _PARSED[root]


def script_registrations(root: Path) -> dict[str, list[Registration]]:
    """Every static ``add_test(NAME <literal> COMMAND ...)``, by test name."""
    found: dict[str, list[Registration]] = {}
    for f in parsed(root):
        path, calls, sets = f.path, f.calls, f.sets
        for call in calls:
            args = call.args
            if call.command != "add_test" or len(args) < 2 or args[0] != "NAME":
                continue
            name = args[1]
            if "${" in name or "$<" in name:
                continue
            reg = Registration(path, call.first, None, "")
            body = _inside_body(call)
            if body:
                reg.reason = f"registered inside a {body}() body"
            elif "COMMAND" not in args:
                reg.reason = "add_test without COMMAND"
            else:
                command = args[args.index("COMMAND") + 1:]
                wd = None
                for keyword in ("WORKING_DIRECTORY", "CONFIGURATIONS", "COMMAND_EXPAND_LISTS"):
                    if keyword in command:
                        k = command.index(keyword)
                        if keyword == "WORKING_DIRECTORY" and k + 1 < len(command):
                            wd = command[k + 1]
                        command = command[:k]
                expanded = []
                for token in command:
                    value, why = expand(token, root, path, sets)
                    if value is None:
                        reg.reason = f"its command {why}"
                        break
                    expanded.append(value)
                reg.command = expanded if not reg.reason else None
                reg.properties = _properties(calls, name)
                if wd is not None:
                    reg.properties.setdefault("WORKING_DIRECTORY", []).insert(0, wd)
                for key in ("WORKING_DIRECTORY", "ENVIRONMENT"):
                    values = reg.properties.get(key) or []
                    resolved = []
                    for value in values:
                        out, why = expand(value, root, path, sets)
                        if out is None:
                            reg.reason = reg.reason or f"its {key} {why}"
                            break
                        resolved.append(out)
                    if values:
                        reg.properties[key] = resolved
            found.setdefault(name, []).append(reg)
    return found


def ctest_record(reg: Registration) -> dict:
    """The shape of one ctest --show-only=json-v1 test the generator reads."""
    props = []
    wd = (reg.properties.get("WORKING_DIRECTORY") or [None])[-1]
    props.append({"name": "WORKING_DIRECTORY", "value": wd or BINARY_STAND_IN})
    env: list[str] = []
    for value in reg.properties.get("ENVIRONMENT") or []:
        env.extend(v for v in value.split(";") if v)
    if env:
        props.append({"name": "ENVIRONMENT", "value": env})
    return {"command": reg.command, "properties": props}


# -- fragments -------------------------------------------------------------

def fragment_text(kind: str, name: str, record: dict | None, scanned: bool = True,
                  target: str | None = None) -> str:
    """A fragment's exact bytes, the way ``--write`` writes it."""
    if kind == "tests":
        return store.dumps({"name": name, "record": record})
    frag: dict = {"name": name, "scanned": scanned}
    if target is not None:
        frag["target"] = target
    if record is not None:
        frag["record"] = record
    return store.dumps(frag)


def unified(rel: str, old: str | None, new: str) -> str:
    lines = difflib.unified_diff((old or "").splitlines(keepends=True),
                                 new.splitlines(keepends=True),
                                 fromfile=f"a/{store.FRAGMENT_DIR.as_posix()}/{rel}"
                                 if old is not None else "/dev/null",
                                 tofile=f"b/{store.FRAGMENT_DIR.as_posix()}/{rel}")
    return "".join(lines)


def _paths(rec: dict) -> set[str]:
    return {p for p in [rec.get("entry") or "", *(rec.get("inputs") or [])]
            if p and not p.startswith(sti.BINARY_DIR_TOKEN)}


# -- script tests ----------------------------------------------------------

def check_script_tests(root: Path, base: str, changed: set[str], doc: dict, report: Report,
                       regs: dict[str, list[Registration]]) -> None:
    tests = doc.get("tests") or {}
    changed_cmake = {f for f in changed if f.endswith((".cmake", "CMakeLists.txt"))}
    changed_python = any(f.endswith(".py") for f in changed)
    walker, tracked = sti.Walker(root), sti.tracked_paths(root)
    for name, rec in sorted(tests.items()):
        mine = regs.get(name) or []
        via_cmake = {r.path for r in mine} & changed_cmake
        include_changes = set().union(*(
            include_ancestors(root, r.path) | include_ancestors(root, r.path, base)
            for r in mine))
        include_changes &= changed_cmake
        include_changes -= via_cmake
        if include_changes:
            report.not_checked.append(
                f"{name}: its CMake include chain changed ({', '.join(sorted(include_changes))}); "
                "the configure decides whether it is registered")
        selected = bool(sti.touched_by(changed, _paths(rec)) or via_cmake)
        where = f"tests/{store.slug(name)}.json"
        if (rec.get("entry") or "").startswith(sti.BINARY_DIR_TOKEN):
            if selected or (changed_python and rec.get("kind") == "python"):
                report.not_checked.append(f"{name}: its entry script is generated by configure")
            continue
        if not mine:
            if selected or (changed_python and rec.get("kind") == "python"):
                report.not_checked.append(f"{name}: no static add_test(NAME {name} ...) found")
            continue
        usable = [r for r in mine if not r.reason]
        if len(mine) > 1:
            if selected or (changed_python and rec.get("kind") == "python"):
                report.not_checked.append(f"{name}: registered {len(mine)} times "
                                          f"({', '.join(f'{r.path}:{r.line}' for r in mine)}); "
                                          "the configure picks one")
            continue
        if not usable:
            if selected or (changed_python and rec.get("kind") == "python"):
                report.not_checked.append(f"{name} ({mine[0].path}:{mine[0].line}): {mine[0].reason}")
            continue
        new = sti.inputs_for(ctest_record(usable[0]), root, None, walker=walker, tracked=tracked)
        if new is None:
            if selected or (changed_python and rec.get("kind") == "python"):
                report.not_checked.append(f"{name}: the generator records no entry for its "
                                          "reconstructed command")
            continue
        # A formerly missing module is absent from the recorded inputs. Use
        # the generator's import resolution, including transitive imports and
        # search paths, before deciding that a Python change cannot reach us.
        # This avoids blaming an already-stale record for an unrelated module.
        if not selected and rec.get("kind") == "python":
            selected = sti.touched_by(changed, _paths(new))
        if not selected:
            continue
        report.checked += 1
        if new != rec and (selected or sti.touched_by(changed, _paths(new))):
            text = fragment_text("tests", name, new)
            report.stale.append(unified(where, fragment_text("tests", name, rec), text))
            report.writes[where] = text


# -- compiled test executables ----------------------------------------------
#
# A compiled entry is drawn from the configure's source list for its target,
# which a static read cannot reproduce in general (shared source variables,
# generator expressions, functions). So this half never rebuilds a record. It
# judges one changed file at a time, and only for an executable it can prove
# compiles that file; a disagreement between the file's own signals and the
# record is certain drift, whose new content the configure supplies.

SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")
HEADER_EXTS = (".h", ".hh", ".hpp")
# Files whose working-tree text this check reads.
READ_SUFFIXES = sti.SCRIPT_SUFFIXES + SOURCE_EXTS + HEADER_EXTS + (".cmake", ".json")
# Commands whose first argument is a target and whose remaining arguments
# include its sources.
SOURCE_LISTING = {"add_executable", SUITE_HELPER, "target_sources"}
SUITE_KEYWORDS = {"NO_PCH", "MAY_BE_EMPTY", "TIMEOUT", "TEST_SPEC", "TEST_PREFIX", "GROUP",
                  "SOURCES", "LIBRARIES", "INCLUDE_DIRS", "COMPILE_DEFINITIONS",
                  "PROPERTIES", "DISCOVERY_ARGS", "LABELS"}
# Conditions every gate configure satisfies (script_inputs_precheck.GATE_TRUE).
GATE_TRUE = {"Python3_Interpreter_FOUND", "Python3_FOUND", "PULP_BUILD_TESTS"}
# A deliberate exclusion, as script_inputs_precheck reads it.
NOT_GATED = re.compile(r"#\s*script-inputs:\s*not-gated\b")
# Edits that change what the generator records for entries this check does
# not select.
GENERATOR_FILES = ("tools/scripts/script_test_inputs.py", "tools/scripts/script_inputs_store.py",
                   "tools/cmake/PulpTestData.cmake")


_SIGNALS: dict[tuple[str, Path, str], bool] = {}


def _spawns(root: Path, rel: str) -> bool:
    key = ("spawn", root, rel)
    if key not in _SIGNALS:
        try:
            text = (root / rel).read_text(encoding="utf-8", errors="replace")
            _SIGNALS[key] = bool(sti.SPAWN_SIGNAL.search(sti.strip_c_comments(text)))
        except OSError:
            _SIGNALS[key] = False
    return _SIGNALS[key]


def _fixed_data_signal(root: Path, rel: str) -> bool:
    key = ("data", root, rel)
    if key not in _SIGNALS:
        _SIGNALS[key] = bool(sti.source_signals(root, rel, []))
    return _SIGNALS[key]


def _spawned_at(root: Path, ref: str, rel: str) -> bool:
    text = subprocess.run(["git", "-C", str(root), "show", f"{ref}:{rel}"], capture_output=True,
                          text=True, encoding="utf-8", errors="replace").stdout
    return bool(sti.SPAWN_SIGNAL.search(sti.strip_c_comments(text)))


def _resolve_source(token: str, root: Path, f: Parsed) -> str | None:
    if not token.endswith(SOURCE_EXTS + HEADER_EXTS):
        return None
    value, _ = expand(token, root, f.path, f.sets)
    if value is None:
        return None
    sd = source_dir_of(f.path, root)
    p = Path(value)
    if not p.is_absolute():
        if sd is None:
            return None
        p = root / sd / p
    rel = sti._rel(p, root)
    return os.path.normpath(rel) if rel else None


def suite_sources(call: registrations.Call, root: Path, f: Parsed) -> tuple[list[str] | None, str]:
    """The sources pulp_add_test_suite compiles, or (None, why)."""
    args = call.args
    if source_dir_of(f.path, root) is None:
        return None, "its source directory depends on the include chain"
    if "SOURCES" in args:
        i = args.index("SOURCES") + 1
        raw = []
        while i < len(args) and args[i] not in SUITE_KEYWORDS:
            raw.append(args[i])
            i += 1
    else:
        stem = re.sub(r"^pulp-test-", "", args[0]).replace("-", "_")
        raw = [f"test_{stem}.cpp"]
    out = []
    for token in raw:
        rel = _resolve_source(token, root, f)
        if rel is None:
            return None, f"its source {token!r} names something only a configure resolves"
        out.append(rel)
    return out, ""


def static_sources(root: Path) -> dict[str, set[str]]:
    """target -> sources its static registrations name. A suite in a GROUP
    adds its sources to the group's executable."""
    out: dict[str, set[str]] = {}
    for f in parsed(root):
        for call in f.calls:
            if call.command not in SOURCE_LISTING or not call.args or _inside_body(call):
                continue
            target = call.args[0]
            if "${" in target:
                continue
            if call.command == SUITE_HELPER:
                srcs, _ = suite_sources(call, root, f)
                if "GROUP" in call.args:
                    i = call.args.index("GROUP") + 1
                    target = call.args[i] if i < len(call.args) else target
            else:
                srcs = [r for r in (_resolve_source(t, root, f) for t in call.args[1:]) if r]
            out.setdefault(target, set()).update(srcs or [])
    return out


def list_siblings(root: Path, base: str, head: str, changed: set[str]) -> dict[str, list[set[str]]]:
    """source -> the other sources of each set() list a changed CMake file
    newly adds it to. A shared list compiles into whichever targets use it."""
    out: dict[str, list[set[str]]] = {}
    for f in parsed(root):
        if f.path not in changed:
            continue
        added = added_lines(root, base, head, f.path)
        for call in f.calls:
            if call.command != "set" or not any(call.first <= n <= call.last for n in added):
                continue
            lines = f.text.splitlines()[call.first - 1:call.last]
            fresh = set()
            for n, line in enumerate(lines, start=call.first):
                if n in added:
                    fresh.update(r for r in (_resolve_source(t, root, f) for t in
                                             re.findall(r"[^\s()\"]+", line)) if r)
            members = {r for r in (_resolve_source(t, root, f) for t in call.args[1:]) if r}
            for rel in fresh:
                out.setdefault(rel, []).append(members - {rel})
    return out


SOURCE_TOKEN = re.compile(r"(?<![A-Za-z0-9_./+-])([A-Za-z0-9_./+-]+"
                          r"\.(?:c|cc|cpp|cxx|m|mm|h|hh|hpp))(?![A-Za-z0-9_./+-])")


def _cmake_source_token(root: Path, path: str, token: str) -> str | None:
    """Resolve a source-looking token from a CMake diff to a repo path."""
    token = token.strip("\"'")
    if token.startswith(("test/", "tools/", "core/", "examples/", "integrations/")):
        rel = token
    else:
        directory = source_dir_of(path, root)
        if directory is None:
            return None
        rel = os.path.normpath(os.path.join(directory, token))
    if rel == "." or rel.startswith("../"):
        return None
    return rel


def cmake_source_changes(root: Path, base: str, head: str,
                         changed: set[str]) -> tuple[set[str], set[str]]:
    """Source paths added/removed by CMake-only edits.

    The configured source list is authoritative, but a diff can still tell us
    which existing source a list edit names. Feeding those paths through the
    same executable checks catches a stale spawn/data record without a source
    file diff; removals are reported explicitly because the old record itself
    would otherwise make membership look true.
    """
    added: set[str] = set()
    removed: set[str] = set()
    for path in sorted(f for f in changed if f.endswith((".cmake", "CMakeLists.txt"))):
        file_added: set[str] = set()
        file_removed: set[str] = set()
        diff = _git(root, "diff", "-U0", "--no-renames", f"{base}...{head}", "--", path)
        for line in diff.splitlines():
            if not line or line.startswith(("+++", "---")) or line[0] not in "+-":
                continue
            body = line[1:].lstrip()
            if body.startswith("#"):
                continue
            target = file_added if line[0] == "+" else file_removed
            for token in SOURCE_TOKEN.findall(body):
                rel = _cmake_source_token(root, path, token)
                if rel:
                    target.add(rel)
        # A modified line appears on both sides of the diff. Keep additions
        # for signal checks, but do not call its unchanged sources removals.
        added.update(file_added)
        removed.update(file_removed - file_added)
    return added, removed


def _compiles(root: Path, exe: str, target: str, rec: dict | None,
              static: dict[str, set[str]], siblings: dict[str, list[set[str]]],
              src: str) -> bool | None:
    """Whether ``exe`` compiles ``src``; None means shared-list membership is uncertain."""
    rec = rec or {}
    if src in set(rec.get("spawning_sources") or []) | set(rec.get("detected_sources") or []):
        return True
    if src in static.get(target, set()) or src in static.get(exe, set()):
        return True
    spawning = set(rec.get("spawning_sources") or [])
    for members in siblings.get(src, []):
        # Shared sources suggest membership, but do not prove this target
        # consumes the list: even an unused set() can name those sources.
        callers = {m for m in members if _spawns(root, m)}
        if callers and callers <= spawning:
            return None
    return False


def check_executables(root: Path, base: str, head: str, changed: set[str], doc: dict,
                      report: Report) -> None:
    records = doc.get("executables") or {}
    targets = doc.get("executable_targets") or {}
    names = sorted(set(records) | set(doc.get("executables_scanned") or []))
    changed_src = set(f for f in changed if f.endswith(SOURCE_EXTS + HEADER_EXTS))
    cmake_added, cmake_removed = cmake_source_changes(root, base, head, changed)
    changed_src |= cmake_added | cmake_removed
    changed_src = sorted(changed_src)
    if not changed_src or not names:
        return
    static = static_sources(root)
    siblings = list_siblings(root, base, head, changed)
    listed = set()
    for rec in records.values():
        listed |= set(rec.get("spawning_sources") or [])
    merge_base = _git(root, "merge-base", base, head).strip()
    for src in changed_src:
        if (src.startswith("test/") and src.endswith(HEADER_EXTS) and src not in listed
                and (root / src).is_file() and _spawns(root, src)
                and not _spawned_at(root, merge_base, src)):
            report.not_checked.append(f"{src} now calls a process or loader API; which "
                                      "executables include it only a configure knows")
    for exe in names:
        rec = records.get(exe)
        target = targets.get(exe, exe)
        spawning = set((rec or {}).get("spawning_sources") or [])
        detected = set((rec or {}).get("detected_sources") or [])
        reasons: list[str] = []
        for src in sorted(cmake_removed & sti.compiled_entry_paths(rec or {})):
            # A shared source may have left another target's list while this
            # executable still names it in add_executable/target_sources.
            if src in static.get(target, set()) or src in static.get(exe, set()):
                continue
            reasons.append(f"{src} was removed from a CMake source list, but the entry still "
                           "records it; configure is required to regenerate this fragment")
        for src in changed_src:
            if not (root / src).is_file():
                listed = src in sti.compiled_entry_paths(rec or {})
                if listed:
                    reasons.append(f"{src} is deleted, and the entry still lists it")
                continue
            compiles = _compiles(root, exe, target, rec, static, siblings, src)
            if compiles is None:
                report.not_checked.append(f"{exe}: {src} shares a source list with recorded "
                                          "sources, but target consumption is not proven")
            if not compiles:
                continue
            report.checked += 1
            spawns = _spawns(root, src)
            if spawns and src not in spawning:
                reasons.append(f"{src} calls a process or loader API, and the entry does not "
                               "list it under spawning_sources")
            elif not spawns and src in spawning:
                reasons.append(f"{src} no longer calls a process or loader API, and the entry "
                               "still lists it under spawning_sources")
            # Data signals are read from compiled sources only, never headers;
            # a definition-only signal needs the configure, so only a fixed
            # signal is judged here.
            if (src.endswith(SOURCE_EXTS) and src not in detected
                    and _fixed_data_signal(root, src)):
                reasons.append(f"{src} reads the checkout, and the entry does not list it "
                               "under detected_sources")
        if reasons:
            report.needs_configure.append(
                f"executables/{store.slug(exe)}.json ({exe}):\n      " + "\n      ".join(reasons))


def _branch_conditions(call: registrations.Call, calls: list[registrations.Call]) -> list[list[str]] | None:
    """The if() conditions enclosing ``call``, or None when one is negated
    (an else/elseif branch) or the call sits in a body."""
    conds = []
    block = call.block
    anchor = call
    while block is not None:
        header = block.header
        if header.command != "if":
            return None
        if any(c.command in ("elseif", "else") and c.block is block and c.first < anchor.first
               for c in calls):
            return None
        conds.append(header.args)
        anchor = header
        block = header.block
    return conds


def _target_terms(cond: list[str]) -> set[str] | None:
    """The TARGET names of a pure ``TARGET a AND TARGET b`` condition (gate-true
    tokens allowed), or None for anything else."""
    names: set[str] = set()
    i = 0
    while i < len(cond):
        token = cond[i]
        if token == "TARGET" and i + 1 < len(cond):
            names.add(cond[i + 1])
            i += 2
        elif token == "AND" or token in GATE_TRUE:
            i += 1
        else:
            return None
    return names


def _exe_names(call: registrations.Call) -> set[str]:
    names = {call.args[0]} if call.args else set()
    if call.command == SUITE_HELPER and "GROUP" in call.args:
        i = call.args.index("GROUP") + 1
        if i < len(call.args):
            names.add(call.args[i])
    return names


def gate_targets(root: Path, known: set[str]) -> set[str]:
    """TARGET names the gate configure evidently defines: each guards an
    executable the gate inventory already has, or is a dependency fetched at
    the top level of a build module."""
    out: set[str] = set()
    for path in _included(root, "CMakeLists.txt", True):
        if not path.startswith("tools/cmake/"):
            continue
        try:
            text = (root / path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "FetchContent_MakeAvailable" not in text:
            continue
        calls = registrations.calls(text)
        registrations._blocks(calls)
        out.update(arg for c in calls if c.command == "fetchcontent_makeavailable"
                   and c.block is None for arg in c.args)
    for f in parsed(root):
        for call in f.calls:
            if call.command not in ("add_executable", SUITE_HELPER, "pulp_add_test_group"):
                continue
            if not (_exe_names(call) & known):
                continue
            for cond in _branch_conditions(call, f.calls) or []:
                out |= _target_terms(cond) or set()
    return out


def check_new_executables(root: Path, base: str, head: str, changed: set[str], doc: dict,
                          report: Report) -> None:
    """A test executable the range adds needs an executables/ fragment."""
    if not doc.get("executables_scanned"):
        return  # a list written without the compiled scan has nothing to compare
    known = (set(doc.get("executables_scanned") or []) | set(doc.get("executables") or {})
             | set((doc.get("executable_targets") or {}).values()))
    merge_base = _git(root, "merge-base", base, head).strip()
    trusted: set[str] | None = None
    defined = {c.args[0] for g in parsed(root) for c in g.calls
               if c.command in ("add_executable", SUITE_HELPER, "pulp_add_test_group") and c.args}
    for path in sorted(f for f in changed if f.startswith("test/")
                       and f.endswith((".cmake", "CMakeLists.txt"))):
        old = subprocess.run(["git", "-C", str(root), "show", f"{merge_base}:{path}"],
                             capture_output=True, text=True, encoding="utf-8").stdout
        old_names = {c.args[0] for c in registrations.calls(old)
                     if c.command in ("add_executable", SUITE_HELPER) and c.args}
        # An executable the range removes leaves a fragment the generator
        # drops; a name still defined elsewhere is not removed.
        for name in sorted(old_names - defined):
            if name in known and "${" not in name:
                report.needs_configure.append(
                    f"executables/{store.slug(name)}.json ({name}): {path} no longer defines "
                    "it, and its fragment remains")
        f = next((g for g in parsed(root) if g.path == path), None)
        if f is None:
            continue
        if source_dir_of(f.path, root) is None:
            if any(c.command in ("add_executable", SUITE_HELPER) and c.args
                   and c.args[0] not in old_names and c.args[0] not in known for c in f.calls):
                report.not_checked.append(f"{f.path}: a new executable here, but the test tree "
                                          "does not include this file, so the gate may not scan it")
            continue
        for call in f.calls:
            if call.command not in ("add_executable", SUITE_HELPER) or not call.args:
                continue
            name = call.args[0]
            if name in old_names or name in known:
                continue
            where = f"{f.path}:{call.first}"
            rel = f"executables/{store.slug(name)}.json"
            if "${" in name:
                report.not_checked.append(f"{where}: executable name {name!r} is computed")
                continue
            if "GROUP" in call.args:
                continue  # the group's executable; its sources are judged above
            lines = f.text.splitlines()[max(call.first - 2, 0):call.last]
            if any(NOT_GATED.search(line) for line in lines):
                continue
            body = _inside_body(call)
            if body:
                report.not_checked.append(f"{where}: {name} is defined inside a {body}() body")
                continue
            conds = _branch_conditions(call, f.calls)
            if trusted is None:
                trusted = gate_targets(root, known)
            terms = [(_target_terms(c), c) for c in conds or []]
            unknown = [" ".join(c) for t, c in terms if t is None or not t <= trusted]
            if conds is None or unknown:
                guard = "an else branch" if conds is None else f"if({unknown[0]})"
                report.not_checked.append(f"{where}: {name} has no fragment, but it sits under "
                                          f"{guard}, which a static read cannot show the gate "
                                          "configure enters")
                continue
            if call.command == SUITE_HELPER:
                sources, why = suite_sources(call, root, f)
            else:
                sources = [r for r in (_resolve_source(t, root, f) for t in call.args[1:]) if r]
                why = "" if sources else "its sources name something only a configure resolves"
            if not sources:
                report.needs_configure.append(f"{rel} ({name}, {where}): missing; {why}")
                continue
            report.checked += 1
            callers = sti.spawning_sources(root, sources)
            data = [s for s in sources if s.endswith(SOURCE_EXTS) and _fixed_data_signal(root, s)]
            if callers or data:
                what = "start a process" if callers else "read the checkout"
                report.needs_configure.append(
                    f"{rel} ({name}, {where}): missing; its sources {what} "
                    f"({', '.join(callers or data)}), so its record needs configure evidence")
                continue
            text = fragment_text("executables", name, None)
            report.stale.append(unified(rel, None, text))
            report.writes[rel] = text


# -- driver ----------------------------------------------------------------

def check(root: Path, base: str, head: str = "HEAD") -> Report:
    report = Report()
    root = root.resolve()
    # The tree may have moved since the last call.
    _PARSED.pop(root, None)
    _TEST_CHAIN.pop(root, None)
    for key in [key for key in _INCLUDE_ANCESTORS if key[0] == root]:
        _INCLUDE_ANCESTORS.pop(key, None)
    _SIGNALS.clear()
    if head != "HEAD":
        tip = _git(root, "rev-parse", "HEAD").strip()
        if _git(root, "rev-parse", head).strip() != tip:
            report.not_checked.append(f"the pushed commit {head} is not the checked-out HEAD; "
                                      "the offline recompute reads the working tree")
            return report
    changed = changed_files(root, base, head)
    if head != "HEAD":
        # The pushed commit is judged, but files are read from the working
        # tree: an uncommitted edit to anything this check reads would answer
        # for a commit that does not carry it.
        dirty = [line[3:] for line in _git(root, "status", "--porcelain", "--untracked-files=no",
                                           "--no-renames").splitlines() if len(line) > 3]
        relevant = [f for f in dirty if f in changed or store.is_store_path(f)
                    or f.endswith(READ_SUFFIXES) or os.path.basename(f) == "CMakeLists.txt"]
        if relevant:
            report.not_checked.append(
                f"uncommitted edits to {', '.join(relevant[:3])}"
                f"{' and more' if len(relevant) > 3 else ''}: the offline recompute reads the "
                "working tree, so commit or stash them to have the pushed commit judged")
            return report
    if not changed:
        return report
    test_chain(root)
    doc = (store.load_ref(root, head) if head != "HEAD" else store.load(root)) or {}
    regs = script_registrations(root)
    check_script_tests(root, base, changed, doc, report, regs)
    check_executables(root, base, head, changed, doc, report)
    check_new_executables(root, base, head, changed, doc, report)
    touched = sorted(f for f in changed if f in GENERATOR_FILES)
    if touched:
        report.not_checked.append(f"{', '.join(touched)} changed, so entries this check did "
                                  "not select may drift too")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", "--changed-from", dest="base", default="origin/main",
                        help="the ref the push is compared with (default origin/main)")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--write", action="store_true",
                        help="write every fragment this check can recompute exactly")
    args = parser.parse_args(argv)
    try:
        report = check(args.root, args.base, args.head)
    except Exception as error:  # noqa: BLE001 - any crash is "could not run", never "stale"
        detail = getattr(error, "stderr", "") or str(error)
        print(f"script-inputs-offline: cannot read {args.base}...{args.head}: "
              f"{str(detail).strip()}", file=sys.stderr)
        return 2
    for line in report.not_checked:
        print(f"script-inputs-offline: NOT CHECKED {line} (the configured "
              "script-test-inputs-drift check still runs it)", file=sys.stderr)
    if args.write and report.writes:
        target = store.location(args.root.resolve())
        for rel, text in sorted(report.writes.items()):
            (target / rel).parent.mkdir(parents=True, exist_ok=True)
            (target / rel).write_text(text, encoding="utf-8")
            print(f"script-inputs-offline: wrote {store.FRAGMENT_DIR.as_posix()}/{rel}")
        report.stale = []
    failed = bool(report.stale or report.needs_configure)
    if report.stale:
        print("script-inputs-offline: stale fragment(s); CI's script-test-inputs-drift "
              "fails on these:", file=sys.stderr)
        for diff in report.stale:
            sys.stderr.write(diff if diff.endswith("\n") else diff + "\n")
        print("  fix (no configure needed):\n"
              f"    python3 tools/scripts/script_inputs_offline.py --base {args.base} --write\n"
              "    git add test/ctest_script_inputs/", file=sys.stderr)
    if report.needs_configure:
        print("script-inputs-offline: fragment(s) this push makes stale whose new content "
              "needs a configure:", file=sys.stderr)
        for line in report.needs_configure:
            print(f"  {line}", file=sys.stderr)
        print("  regenerate from a gate-profile configure:\n" + regenerate(args.root),
              file=sys.stderr)
    if failed:
        return 1
    print(f"script-inputs-offline: ok ({report.checked} recomputed, "
          f"{len(report.not_checked)} not checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
