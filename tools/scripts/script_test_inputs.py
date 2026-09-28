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


def inputs_for(test: dict, root: Path) -> dict | None:
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
    if not w._in_repo(entry):
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
    rels = sorted({r for r in (_rel(p, root) for p in seen) if r})
    return {"kind": kind, "entry": _rel(entry, root), "inputs": rels}


def build_list(inventory: dict, root: Path) -> dict:
    tests = {}
    for t in inventory.get("tests", []):
        name = t.get("name", "")
        if not name or name.endswith("_NOT_BUILT"):
            continue
        cmd = t.get("command") or []
        if cmd and _rel(Path(cmd[0]), root) is None and not os.path.basename(cmd[0]).startswith(INTERPRETERS):
            continue  # a compiled test binary or a tool outside the repo: the graph owns it
        rec = inputs_for(t, root)
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
    current = build_list(inventory, root)
    total_scripts = sum(1 for t in inventory.get("tests", []) if t.get("command") and
                        os.path.basename(t["command"][0]).startswith(INTERPRETERS))
    if a.write:
        list_path.parent.mkdir(parents=True, exist_ok=True)
        list_path.write_text(json.dumps(current, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print(f"script-test-inputs: wrote {len(current['tests'])} declared tests "
              f"(of {total_scripts} interpreter-driven entries) to {list_path}")
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
    blocking = [pr for pr in problems if changed is None or touched_by(changed, pr[2])]
    advisory = [pr for pr in problems if pr not in blocking]
    scope = f"diff-scoped against {base}" if base else "full compare (no base resolved)"
    if advisory:
        print(f"script-test-inputs: note: {len(advisory)} entr{'y' if len(advisory) == 1 else 'ies'} drifted from "
              f"scripts this change does not touch ({scope}); regenerate with --write when convenient:")
        for kind, name, _ in advisory[:15]:
            print(f"  {kind}: {name}")
    if blocking:
        print(f"script-test-inputs: {len(blocking)} drift problem(s) in scripts this change touches "
              f"({scope}); regenerate with --write:")
        for kind, name, _ in blocking[:40]:
            print(f"  {kind}: {name}")
        return 1
    print(f"script-test-inputs: OK, {len(current['tests'])} declared tests in sync for this change "
          f"({scope}; {total_scripts} interpreter-driven entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
