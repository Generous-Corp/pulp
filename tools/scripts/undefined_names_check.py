#!/usr/bin/env python3
"""Report names a Python module reads but never defines.

A function that reads a module-level name nothing binds raises NameError only
when that line runs. A refactor that moves or inlines an assignment can leave
such a read behind on a path no test reaches. That is how a comparison in the
importer differential lab passed every refusal test and then raised NameError
on every successful run.

This is the core of pyflakes' undefined-name check, built on the standard
library's `symtable`, so the gate adds no dependency. In each scope, a name
that resolves to the module's globals (not local, not a closure, not declared
`global`) must be bound somewhere at module level, be a builtin, or be one of
the module attributes Python provides. A module with a star import is skipped
and reported, because its globals cannot be known statically.

  python3 tools/scripts/undefined_names_check.py tools/import-validation

Exit 0 when every scanned file is clean, 1 when a name is undefined, 2 when no
Python file was found (a scan of nothing proves nothing).
"""

from __future__ import annotations

import argparse
import ast
import builtins
import symtable
import sys
from pathlib import Path

MODULE_ATTRIBUTES = {
    "__file__", "__name__", "__doc__", "__spec__", "__loader__", "__package__",
    "__builtins__", "__path__", "__annotations__", "__dict__", "__debug__",
}
KNOWN = set(dir(builtins)) | MODULE_ATTRIBUTES


def _module_bindings(table: symtable.SymbolTable) -> set[str]:
    return {s.get_name() for s in table.get_symbols()
            if s.is_assigned() or s.is_imported() or s.is_namespace()
            or s.is_parameter()}


def _walk(table: symtable.SymbolTable):
    yield table
    for child in table.get_children():
        yield from _walk(child)


def undefined_names(source: str, filename: str) -> list[tuple[int, str]] | None:
    """(line, name) for each undefined read, or None for a star-importing module."""
    tree = ast.parse(source, filename)
    if any(isinstance(n, ast.ImportFrom) and any(a.name == "*" for a in n.names)
           for n in ast.walk(tree)):
        return None
    module = symtable.symtable(source, filename, "exec")
    bound = _module_bindings(module) | KNOWN
    # A name declared `global` and assigned in a function is a module binding too.
    for table in _walk(module):
        for symbol in table.get_symbols():
            if symbol.is_declared_global() and symbol.is_assigned():
                bound.add(symbol.get_name())
    offenders: list[tuple[symtable.SymbolTable, str]] = []
    for table in _walk(module):
        for symbol in table.get_symbols():
            name = symbol.get_name()
            if not symbol.is_referenced() or name in bound:
                continue
            if table is module:
                if not (symbol.is_assigned() or symbol.is_imported()):
                    offenders.append((table, name))
            elif symbol.is_global() and not symbol.is_declared_global():
                offenders.append((table, name))
    found = set()
    for table, name in offenders:
        lines = _load_lines(tree, table, name, module)
        found.update((line, name) for line in (lines or [table.get_lineno()]))
    return sorted(found)


SCOPES = (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)


def _loads(node: ast.AST, name: str, *, stop_at_scopes: bool) -> list[int]:
    lines = []
    stack = list(ast.iter_child_nodes(node))
    while stack:
        child = stack.pop()
        if stop_at_scopes and isinstance(child, SCOPES):
            # A default or decorator is evaluated in the enclosing scope.
            stack.extend(getattr(child, "decorator_list", []))
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
                stack.extend(child.args.defaults + [d for d in child.args.kw_defaults if d])
            continue
        if isinstance(child, ast.Name) and isinstance(child.ctx, ast.Load) and child.id == name:
            lines.append(child.lineno)
        stack.extend(ast.iter_child_nodes(child))
    return lines


def _load_lines(tree: ast.Module, table: symtable.SymbolTable, name: str,
                module: symtable.SymbolTable) -> list[int]:
    """Lines reading `name` in the scope `table` describes (best effort)."""
    if table is module:
        return _loads(tree, name, stop_at_scopes=True)
    for node in ast.walk(tree):
        if not isinstance(node, SCOPES):
            continue
        starts = {node.lineno, *(d.lineno for d in getattr(node, "decorator_list", []))}
        node_name = "lambda" if isinstance(node, ast.Lambda) else node.name
        if node_name == table.get_name() and table.get_lineno() in starts:
            return _loads(node, name, stop_at_scopes=True)
    return []


def scan(roots: list[Path]) -> tuple[int, list[str], list[str]]:
    files = sorted({p for root in roots
                    for p in ([root] if root.is_file() else root.rglob("*.py"))
                    if p.suffix == ".py"})
    problems, skipped = [], []
    for path in files:
        result = undefined_names(path.read_text(encoding="utf-8"), str(path))
        if result is None:
            skipped.append(f"{path}: star import; globals unknowable, not checked")
            continue
        problems.extend(f"{path}:{line}: undefined name '{name}'" for line, name in result)
    return len(files), problems, skipped


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("roots", nargs="+", type=Path)
    args = parser.parse_args(argv)
    count, problems, skipped = scan(args.roots)
    for line in skipped:
        print(f"undefined-names: {line}")
    if count == 0:
        print("undefined-names: no Python files found; nothing was checked", file=sys.stderr)
        return 2
    for line in problems:
        print(f"undefined-names: {line}", file=sys.stderr)
    if problems:
        print(f"undefined-names: {len(problems)} undefined name read(s) in {count} file(s)",
              file=sys.stderr)
        return 1
    print(f"undefined-names: ok ({count} file(s) checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
