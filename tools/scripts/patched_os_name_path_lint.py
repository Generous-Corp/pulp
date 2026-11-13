#!/usr/bin/env python3
"""Refuse `pathlib.Path(...)` built while a test has patched `os.name` to "nt".

With `os.name` patched to "nt", `pathlib.Path(...)` builds a `WindowsPath`.
Python 3.11 refuses to instantiate that off Windows ("cannot instantiate
'WindowsPath' on your system"); later Pythons accept it. So a test that does
this passes on a developer's newer Python and errors on any host whose
`python3` is 3.11. Build the path before patching, or use
`pathlib.PurePosixPath` / `pathlib.PureWindowsPath`, which never consult
`os.name`.

The lint parses each Python file and looks inside `with` blocks that patch
`os.name` to "nt" (`mock.patch.object(<...>os, "name", "nt")` or
`mock.patch("<...>os.name", "nt")`) for a call to `Path(...)`,
`pathlib.Path(...)` or `WindowsPath(...)`.

Usage: patched_os_name_path_lint.py [PATH ...]   (default: tracked *.py files)
Exit 0 clean, 1 with findings, 2 on an unreadable or unparsable file.
"""

from __future__ import annotations

import ast
import pathlib
import subprocess
import sys

CONCRETE_PATHS = {"Path", "WindowsPath"}


def _is_os_expr(node: ast.AST) -> bool:
    return (isinstance(node, ast.Name) and node.id == "os") or (
        isinstance(node, ast.Attribute) and node.attr == "os")


def _const(node: ast.AST) -> object:
    return node.value if isinstance(node, ast.Constant) else None


def _patches_os_name_to_nt(call: ast.AST) -> bool:
    if not isinstance(call, ast.Call):
        return False
    func = call.func
    name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", "")
    args = call.args
    if name == "object" and len(args) >= 3:
        return _is_os_expr(args[0]) and _const(args[1]) == "name" and _const(args[2]) == "nt"
    if name == "patch" and len(args) >= 2:
        target = _const(args[0])
        return isinstance(target, str) and (target == "os.name" or target.endswith(".os.name")) \
            and _const(args[1]) == "nt"
    return False


def _concrete_path_calls(statements: list[ast.stmt]) -> list[ast.Call]:
    found = []
    for statement in statements:
        for node in ast.walk(statement):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", "")
            if name in CONCRETE_PATHS:
                found.append(node)
    return found


def findings_in_source(source: str, filename: str = "<source>") -> list[tuple[int, str]]:
    """(line, message) for each concrete path built under a patched os.name."""
    tree = ast.parse(source, filename=filename)
    results = []
    for node in ast.walk(tree):
        if not isinstance(node, (ast.With, ast.AsyncWith)):
            continue
        if not any(_patches_os_name_to_nt(item.context_expr) for item in node.items):
            continue
        for call in _concrete_path_calls(node.body):
            results.append((call.lineno,
                            "a concrete Path is built while os.name is patched to \"nt\"; "
                            "Python 3.11 refuses that off Windows. Build it before the "
                            "patch, or use pathlib.PurePosixPath / PureWindowsPath."))
    return sorted(results)


def tracked_python_files(root: pathlib.Path) -> list[pathlib.Path]:
    out = subprocess.run(["git", "ls-files", "*.py"], cwd=root, capture_output=True, text=True,
                         check=True, encoding="utf-8").stdout
    return [root / line for line in out.splitlines() if line]


def main(argv: list[str]) -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    files = [pathlib.Path(a) for a in argv] if argv else tracked_python_files(root)
    total = 0
    for path in files:
        try:
            source = path.read_text(encoding="utf-8")
            found = findings_in_source(source, str(path))
        except (OSError, SyntaxError, UnicodeDecodeError) as error:
            print(f"patched-os-name-path-lint: cannot read {path}: {error}", file=sys.stderr)
            return 2
        for line, message in found:
            shown = path.relative_to(root) if path.is_absolute() and root in path.parents else path
            print(f"{shown}:{line}: {message}")
            total += 1
    if total:
        print(f"patched-os-name-path-lint: {total} finding(s)", file=sys.stderr)
        return 1
    print(f"patched-os-name-path-lint: ok ({len(files)} file(s) scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
