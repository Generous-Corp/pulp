#!/usr/bin/env python3
"""Facade boundary tests for local_ci.py."""

from __future__ import annotations

import ast
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("local_ci.py")


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

    @staticmethod
    def _references_binding_module(node: ast.FunctionDef) -> bool:
        for child in ast.walk(node):
            if isinstance(child, ast.Name) and child.id.endswith("_bindings"):
                return True
        return False


if __name__ == "__main__":
    unittest.main()
