#!/usr/bin/env python3
"""Contract tests for the local-ci module ownership map."""

from __future__ import annotations

import ast
from pathlib import Path
import re
import unittest


LOCAL_CI_DIR = Path(__file__).resolve().parent
MODULE_MAP = LOCAL_CI_DIR / "MODULE_MAP.md"


class LocalCiModuleMapTests(unittest.TestCase):
    def test_every_production_python_module_has_an_ownership_row(self) -> None:
        mapped_modules = set(
            re.findall(r"^\| `([^`]+\.py)` \|", MODULE_MAP.read_text(), flags=re.MULTILINE)
        )
        production_modules = {
            path.name
            for path in LOCAL_CI_DIR.glob("*.py")
            if not path.name.startswith("test_") and path.name != "local_ci.py"
        }

        self.assertEqual(sorted(production_modules - mapped_modules), [])
        self.assertEqual(sorted(mapped_modules - production_modules), [])

    def test_every_binding_module_has_a_matching_test_file(self) -> None:
        binding_modules = {
            path.name
            for path in LOCAL_CI_DIR.glob("*_bindings.py")
            if not path.name.startswith("test_")
        }
        test_modules = {path.name for path in LOCAL_CI_DIR.glob("test_*_bindings.py")}

        missing_tests = sorted(f"test_{module_name}" for module_name in binding_modules if f"test_{module_name}" not in test_modules)

        self.assertEqual(missing_tests, [])

    def test_binding_modules_do_not_redefine_shared_lookup_helpers(self) -> None:
        forbidden_names = {"_binding", "_print_binding"}
        offenders: list[str] = []
        for path in sorted(LOCAL_CI_DIR.glob("*_bindings.py")):
            if path.name.startswith("test_"):
                continue
            tree = ast.parse(path.read_text(), filename=str(path))
            for node in ast.walk(tree):
                if isinstance(node, ast.FunctionDef) and node.name in forbidden_names:
                    offenders.append(f"{path.name}:{node.name}")

        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
