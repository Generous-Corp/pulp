#!/usr/bin/env python3
"""Every Python script a test lane runs must import only what that lane has.

A ctest that imports a third-party module the gate VM's Python lacks passes on
a developer's machine and on the pull request head's fast tier, then dies with
ModuleNotFoundError in the merge group, where it ejects the whole batch. The
import can hide in a helper, or inside a function that only the full suite
reaches, so this check follows every import statement (not just top-level
ones) through repo-local modules.

What each lane provides is derived, never hand-listed:
  * the macOS gate installs exactly tools/motion/visual/requirements.lock into
    the configured Python (build.yml's visual-analysis step) on top of the
    standard library, so ctest scripts may import those packages;
  * the build-free source-selftest lane (tools/ci/source_selftests.json)
    installs nothing, so its scripts get the standard library only.

An import guarded by ``try: ... except ImportError`` (or ModuleNotFoundError)
is allowed: the script has decided what to do when the module is absent.
Imports under ``if TYPE_CHECKING:`` never run and are ignored.

Usage:
    gate_python_imports_check.py --build-dir <configured build> [--ctest ctest]
    gate_python_imports_check.py --script <path.py> [--script ...]

Exit 0 when every script is clean, 1 listing each unavailable import with the
chain that reaches it, 2 when the inventory cannot be read or names no Python
script (an empty inventory means the instrument is pointed at the wrong place).
"""

from __future__ import annotations

import argparse
import ast
import functools
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_LOCK = "tools/motion/visual/requirements.lock"
GATE_WORKFLOW = ".github/workflows/build.yml"
SOURCE_SELFTEST_MANIFEST = "tools/ci/source_selftests.json"
# Distribution name → import name, where they differ.
DIST_TO_MODULE = {"pillow": "PIL", "scikit-image": "skimage", "pyyaml": "yaml"}
GUARD_EXCEPTIONS = {"ImportError", "ModuleNotFoundError", "Exception", "BaseException"}


def gate_modules(repo: Path = REPO_ROOT) -> set[str]:
    """Import names the gate VM's Python has beyond the standard library."""
    workflow = (repo / GATE_WORKFLOW).read_text(encoding="utf-8")
    if f"lock={GATE_LOCK}" not in workflow:
        raise RuntimeError(
            f"{GATE_WORKFLOW} no longer installs {GATE_LOCK}; re-derive what the "
            "gate's Python provides before trusting this check")
    names = set()
    for line in (repo / GATE_LOCK).read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([A-Za-z0-9_.-]+)==", line)
        if match:
            dist = match.group(1).lower()
            names.add(DIST_TO_MODULE.get(dist, dist.replace("-", "_")))
    if not names:
        raise RuntimeError(f"{GATE_LOCK} pins no packages")
    return names


def tracked_python(repo: Path = REPO_ROOT) -> dict[str, list[Path]]:
    """Top-level module name → repo files that would satisfy an import of it."""
    # Untracked (not ignored) files count: a helper written alongside a new
    # test is local before it is committed.
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard", "*.py"], cwd=repo,
                         capture_output=True, check=True).stdout.decode()
    modules: dict[str, list[Path]] = {}
    for rel in filter(None, out.split("\0")):
        path = repo / rel
        name = path.parent.name if path.name == "__init__.py" else path.stem
        modules.setdefault(name, []).append(path)
    return modules


def _catches_import_error(handler: ast.ExceptHandler) -> bool:
    caught = handler.type
    if caught is None:
        return True
    names = []
    for item in (caught.elts if isinstance(caught, ast.Tuple) else [caught]):
        if isinstance(item, ast.Name):
            names.append(item.id)
        elif isinstance(item, ast.Attribute):
            names.append(item.attr)
    return bool(GUARD_EXCEPTIONS.intersection(names))


def _guarded(node: ast.AST, parents: dict[ast.AST, ast.AST]) -> bool:
    """Whether the script has decided what happens when this import fails."""
    current = node
    while current in parents:
        parent = parents[current]
        if isinstance(parent, ast.Try) and current in parent.body:
            if any(_catches_import_error(h) for h in parent.handlers):
                return True
        if isinstance(parent, ast.ExceptHandler) and _catches_import_error(parent):
            # A fallback in `except ImportError:` runs only when the preferred
            # module is missing (tomli standing in for a pre-3.11 tomllib).
            return True
        if isinstance(parent, ast.If):
            test = ast.unparse(parent.test)
            if test.endswith("TYPE_CHECKING") and current in parent.body:
                return True
            if "version_info" in test:
                return True
        current = parent
    return False


@functools.lru_cache(maxsize=None)
def imports_of(path: Path) -> list[tuple[str, list[str], int, bool]]:
    """(module, imported names, line, guarded) for every import in ``path``.

    ``module`` is the dotted name; a relative import keeps its leading dots so
    the caller resolves it against the importing file's directory.
    """
    tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"), str(path))
    parents = {child: node for node in ast.walk(tree) for child in ast.iter_child_nodes(node)}
    found = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                found.append((alias.name, [], node.lineno, _guarded(node, parents)))
        elif isinstance(node, ast.ImportFrom):
            module = "." * node.level + (node.module or "")
            found.append((module, [a.name for a in node.names], node.lineno,
                          _guarded(node, parents)))
    return found


def _module_files(base: Path, dotted: str, names: list[str]) -> list[Path]:
    """Files under ``base`` that an import of ``dotted`` (and ``names``) loads."""
    parts = [p for p in dotted.split(".") if p]
    stem = base.joinpath(*parts) if parts else base
    files = [stem.with_suffix(".py"), stem / "__init__.py"]
    files += [stem / f"{name}.py" for name in names] + [stem / name / "__init__.py" for name in names]
    return [f for f in files if f.is_file()]


def _resolve_local(name: str, importer: Path, modules: dict[str, list[Path]],
                   repo: Path) -> list[Path]:
    candidates = modules.get(name, [])
    if not candidates:
        return []
    for directory in (importer.parent, repo / "tools" / "scripts", repo / "tools" / "ci"):
        near = [c for c in candidates
                if c.parent == directory or (c.name == "__init__.py" and c.parent.parent == directory)]
        if near:
            return near
    return candidates


def _local_targets(dotted: str, names: list[str], importer: Path, repo: Path,
                   modules: dict[str, list[Path]], stdlib: set[str]) -> list[Path] | None:
    """Repo files an import loads, [] for the standard library, None if foreign."""
    if dotted.startswith("."):
        level = len(dotted) - len(dotted.lstrip("."))
        base = importer.parent
        for _ in range(level - 1):
            base = base.parent
        return [p.resolve() for p in _module_files(base, dotted.lstrip("."), names)]
    name = dotted.split(".")[0]
    if name in stdlib or name == "__main__":
        return []
    if (repo / name).is_dir():
        # A repo-root namespace package (`from tools.scripts import x`).
        return [p.resolve() for p in _module_files(repo, dotted, names)]
    local = _resolve_local(name, importer, modules, repo)
    return [p.resolve() for p in local] if local else None


def local_import_closure(script: Path, repo: Path = REPO_ROOT,
                         modules: dict[str, list[Path]] | None = None) -> set[Path]:
    """Every repo file ``script`` loads, directly or through repo helpers."""
    modules = modules if modules is not None else tracked_python(repo)
    stdlib = set(sys.stdlib_module_names)
    seen: set[Path] = set()
    stack = [script.resolve()]
    while stack:
        path = stack.pop()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        try:
            found = imports_of(path)
        except SyntaxError:
            continue
        for dotted, names, _line, _guarded in found:
            stack.extend(_local_targets(dotted, names, path, repo, modules, stdlib) or [])
    return seen


def violations(scripts: list[Path], allowed: set[str], *, repo: Path = REPO_ROOT,
               modules: dict[str, list[Path]] | None = None) -> list[str]:
    """Unavailable, unguarded imports reachable from ``scripts``."""
    modules = modules if modules is not None else tracked_python(repo)
    stdlib = set(sys.stdlib_module_names)
    problems: list[str] = []
    for script in scripts:
        seen: set[Path] = set()
        stack: list[tuple[Path, tuple[str, ...]]] = [(script.resolve(), ())]
        while stack:
            path, chain = stack.pop()
            if path in seen or not path.is_file():
                continue
            seen.add(path)
            try:
                found = imports_of(path)
            except SyntaxError as exc:
                problems.append(f"{script.relative_to(repo)}: cannot parse {path.relative_to(repo)}: {exc}")
                continue
            here = chain + (str(path.relative_to(repo)),)
            for dotted, names, line, guarded in found:
                local = _local_targets(dotted, names, path, repo, modules, stdlib)
                if local is not None:
                    stack.extend((p, here) for p in local)
                    continue
                name = dotted.split(".")[0]
                if guarded or name in allowed:
                    continue
                problems.append(
                    f"{script.relative_to(repo)}: imports '{name}' "
                    f"({' -> '.join(here)}:{line}), which this lane's Python does not have")
    return problems


def ctest_python_scripts(document: dict, repo: Path = REPO_ROOT) -> list[Path]:
    """Repo scripts that registered ctests hand to a Python interpreter."""
    scripts: set[Path] = set()
    for test in document.get("tests", []):
        command = test.get("command") or []
        if not command or "python" not in Path(command[0]).name.lower():
            continue
        for arg in command[1:]:
            if arg.endswith(".py"):
                path = Path(arg)
                if path.is_absolute() and path.is_file() and path.resolve().is_relative_to(repo):
                    scripts.add(path.resolve())
                break
    return sorted(scripts)


def source_selftest_scripts(repo: Path = REPO_ROOT) -> list[Path]:
    manifest = json.loads((repo / SOURCE_SELFTEST_MANIFEST).read_text(encoding="utf-8"))
    scripts = set()
    for entry in manifest["tests"]:
        argv = entry["argv"]
        if argv and argv[0].endswith(".py"):
            path = Path(argv[0].replace("{repo}", str(repo)))
            if path.is_file():
                scripts.add(path.resolve())
    return sorted(scripts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--script", type=Path, action="append", default=[],
                        help="check these scripts against the gate's Python instead")
    args = parser.parse_args(argv)

    try:
        allowed = gate_modules()
    except (OSError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if args.script:
        problems = violations([p.resolve() for p in args.script], allowed)
        checked = len(args.script)
    else:
        if not args.build_dir:
            parser.error("--build-dir or --script is required")
        proc = subprocess.run([args.ctest, "--test-dir", str(args.build_dir), "-N",
                               "--show-only=json-v1"], capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            print(f"ERROR: ctest -N failed ({proc.returncode}): {proc.stderr.strip()}",
                  file=sys.stderr)
            return 2
        gate = ctest_python_scripts(json.loads(proc.stdout))
        lane = source_selftest_scripts()
        if not gate:
            print("ERROR: the ctest inventory hands no repo script to Python; the "
                  "instrument is pointed at the wrong build", file=sys.stderr)
            return 2
        modules = tracked_python()
        problems = violations(gate, allowed, modules=modules)
        # The build-free lane installs nothing: standard library only.
        problems += [f"[source-selftest lane] {p}"
                     for p in violations(lane, set(), modules=modules)]
        checked = len(gate) + len(lane)

    for problem in problems:
        print(problem)
    print(f"gate-python-imports: {checked} script(s) checked against "
          f"{len(allowed)} gate package(s); {len(problems)} problem(s)", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
