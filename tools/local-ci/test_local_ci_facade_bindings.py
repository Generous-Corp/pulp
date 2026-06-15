#!/usr/bin/env python3
"""Facade boundary tests for local_ci.py."""

from __future__ import annotations

import ast
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("local_ci.py")
LOCAL_CI_DIR = MODULE_PATH.parent


class LocalCiFacadeBindingTests(unittest.TestCase):
    def test_public_facade_functions_delegate_through_binding_modules(self) -> None:
        tree = ast.parse(MODULE_PATH.read_text())
        offenders: list[str] = []

        for node in tree.body:
            if not isinstance(node, ast.FunctionDef):
                continue
            if node.name.startswith("_"):
                continue
            if not self._references_binding_module(node):
                offenders.append(f"{node.name}:{node.lineno}")

        self.assertEqual(offenders, [])

    def test_public_facade_constants_delegate_through_binding_modules(self) -> None:
        tree = ast.parse(MODULE_PATH.read_text())
        offenders: list[str] = []

        for node in tree.body:
            if not isinstance(node, ast.Assign):
                continue
            public_constant_targets = [
                target.id
                for target in node.targets
                if isinstance(target, ast.Name)
                and target.id.isupper()
                and not target.id.startswith("_")
            ]
            if not public_constant_targets:
                continue
            for child in ast.walk(node.value):
                if not isinstance(child, ast.Attribute):
                    continue
                value = child.value
                if (
                    isinstance(value, ast.Name)
                    and value.id.startswith("_")
                    and not value.id.endswith("_bindings")
                ):
                    offenders.extend(f"{target}:{node.lineno}" for target in public_constant_targets)
                    break

        self.assertEqual(offenders, [])

    def test_private_implementation_imports_are_binding_dependencies_only(self) -> None:
        tree = ast.parse(MODULE_PATH.read_text())
        binding_sources = "\n".join(
            path.read_text()
            for path in LOCAL_CI_DIR.glob("*_bindings.py")
            if not path.name.startswith("test_")
        )
        implementation_imports: dict[str, int] = {}

        for node in tree.body:
            if not isinstance(node, ast.Import):
                continue
            for alias in node.names:
                if (
                    alias.asname
                    and alias.asname.startswith("_")
                    and not alias.asname.endswith("_bindings")
                ):
                    implementation_imports[alias.asname] = node.lineno

        missing_binding_consumers = [
            f"{alias}:{lineno}"
            for alias, lineno in implementation_imports.items()
            if alias not in binding_sources
        ]
        direct_facade_uses: list[str] = []
        for node in tree.body:
            if isinstance(node, (ast.Import, ast.ImportFrom)):
                continue
            for child in ast.walk(node):
                if not isinstance(child, ast.Attribute):
                    continue
                value = child.value
                if isinstance(value, ast.Name) and value.id in implementation_imports:
                    direct_facade_uses.append(f"{value.id}.{child.attr}:{child.lineno}")

        self.assertEqual(missing_binding_consumers, [])
        self.assertEqual(direct_facade_uses, [])

    @staticmethod
    def _references_binding_module(node: ast.FunctionDef) -> bool:
        for child in ast.walk(node):
            if isinstance(child, ast.Name) and child.id.endswith("_bindings"):
                return True
        return False


if __name__ == "__main__":
    unittest.main()
