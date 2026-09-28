#!/usr/bin/env python3
"""Tests for gate_python_imports_check.py."""

from __future__ import annotations

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gate_python_imports_check as check  # noqa: E402


class Tree:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.files: dict[str, list[Path]] = {}

    def write(self, rel: str, body: str) -> Path:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(body), encoding="utf-8")
        name = path.parent.name if path.name == "__init__.py" else path.stem
        self.files.setdefault(name, []).append(path)
        return path

    def problems(self, script: Path, allowed: set[str] = frozenset({"numpy"})) -> list[str]:
        return check.violations([script], set(allowed), repo=self.root, modules=self.files)


class ImportScanTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tree = Tree(Path(self._tmp.name).resolve())

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_control_stdlib_and_allowed_packages_pass(self) -> None:
        script = self.tree.write("tools/scripts/test_ok.py", """
            import json, os
            from pathlib import Path
            import numpy
        """)
        self.assertEqual(self.tree.problems(script), [])

    def test_an_import_inside_a_function_is_found(self) -> None:
        # The shape that escaped: yaml imported inside a helper the full suite
        # reaches, never at module top level.
        script = self.tree.write("tools/scripts/test_nested.py", """
            import unittest
            class T(unittest.TestCase):
                @classmethod
                def setUpClass(cls):
                    import yaml
                    cls.doc = yaml.safe_load("a: 1")
        """)
        problems = self.tree.problems(script)
        self.assertEqual(len(problems), 1)
        self.assertIn("'yaml'", problems[0])

    def test_an_import_through_a_repo_helper_is_found(self) -> None:
        self.tree.write("tools/scripts/workflow_reader.py", "import yaml\n")
        script = self.tree.write("tools/scripts/test_uses_helper.py", "import workflow_reader\n")
        problems = self.tree.problems(script)
        self.assertEqual(len(problems), 1)
        self.assertIn("workflow_reader.py", problems[0])

    def test_a_guarded_import_is_allowed(self) -> None:
        script = self.tree.write("tools/scripts/test_guarded.py", """
            import unittest
            try:
                import yaml
            except ImportError:
                raise unittest.SkipTest("PyYAML is not installed")
        """)
        self.assertEqual(self.tree.problems(script), [])

    def test_a_guard_that_catches_something_else_is_not_a_guard(self) -> None:
        script = self.tree.write("tools/scripts/test_wrong_guard.py", """
            try:
                import yaml
            except KeyError:
                pass
        """)
        self.assertEqual(len(self.tree.problems(script)), 1)

    def test_type_checking_imports_are_ignored(self) -> None:
        script = self.tree.write("tools/scripts/test_typing.py", """
            from typing import TYPE_CHECKING
            if TYPE_CHECKING:
                import yaml
        """)
        self.assertEqual(self.tree.problems(script), [])

    def test_a_fallback_in_an_import_error_handler_is_allowed(self) -> None:
        script = self.tree.write("tools/scripts/test_toml.py", """
            try:
                import tomllib
            except ImportError:
                import tomli as tomllib
        """)
        self.assertEqual(self.tree.problems(script), [])

    def test_a_version_conditional_import_is_allowed(self) -> None:
        script = self.tree.write("tools/scripts/test_version.py", """
            import sys
            if sys.version_info < (3, 11):
                import tomli
        """)
        self.assertEqual(self.tree.problems(script), [])

    def test_a_repo_root_namespace_import_is_followed(self) -> None:
        self.tree.write("tools/scripts/reader.py", "import yaml\n")
        script = self.tree.write("tools/scripts/test_ns.py",
                                 "from tools.scripts import reader\n")
        problems = self.tree.problems(script)
        self.assertEqual(len(problems), 1)
        self.assertIn("reader.py", problems[0])

    def test_a_relative_import_is_followed(self) -> None:
        self.tree.write("tools/pkg/__init__.py", "")
        self.tree.write("tools/pkg/helper.py", "import yaml\n")
        script = self.tree.write("tools/pkg/test_rel.py", "from . import helper\n")
        self.assertEqual(len(self.tree.problems(script)), 1)

    def test_a_lane_with_no_packages_rejects_what_the_gate_allows(self) -> None:
        script = self.tree.write("tools/scripts/test_np.py", "import numpy\n")
        self.assertEqual(self.tree.problems(script), [])
        self.assertEqual(len(self.tree.problems(script, allowed=set())), 1)


class InventoryTests(unittest.TestCase):
    def test_only_python_commands_contribute_scripts(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw).resolve()
            script = root / "tools" / "scripts" / "test_x.py"
            script.parent.mkdir(parents=True)
            script.write_text("pass\n")
            document = {"tests": [
                {"name": "a", "command": ["/usr/bin/python3", str(script), "--flag"]},
                {"name": "b", "command": ["/bin/bash", str(root / "x.sh")]},
                {"name": "c", "command": ["/usr/bin/python3", "-c", "print(1)"]},
                {"name": "d", "command": ["/usr/bin/python3", "/elsewhere/other.py"]},
            ]}
            self.assertEqual(check.ctest_python_scripts(document, repo=root), [script])


class RepositoryTests(unittest.TestCase):
    def test_gate_packages_are_derived_from_the_lock_the_gate_installs(self) -> None:
        modules = check.gate_modules()
        # Control: the lock really is read (PIL comes from the pillow mapping).
        self.assertIn("numpy", modules)
        self.assertIn("PIL", modules)
        self.assertNotIn("yaml", modules)

    def test_the_escaped_test_is_caught(self) -> None:
        # The pre-fix test from the merge-group ejection, reduced to its shape.
        with tempfile.TemporaryDirectory(dir=check.REPO_ROOT / "tools" / "scripts") as raw:
            script = Path(raw) / "test_escaped.py"
            script.write_text(
                "import unittest\n"
                "class T(unittest.TestCase):\n"
                "    def test_nightly(self):\n"
                "        import yaml\n", encoding="utf-8")
            problems = check.violations([script], check.gate_modules())
        self.assertEqual(len(problems), 1)
        self.assertIn("'yaml'", problems[0])


if __name__ == "__main__":
    unittest.main()
