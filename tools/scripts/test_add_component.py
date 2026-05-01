#!/usr/bin/env python3
"""Focused unit coverage for tools/add-component.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import runpy
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parent.parent / "add-component.py"
spec = importlib.util.spec_from_file_location("add_component", SCRIPT)
assert spec and spec.loader
add_component = importlib.util.module_from_spec(spec)
sys.modules["add_component"] = add_component
spec.loader.exec_module(add_component)


@contextlib.contextmanager
def patched_argv(args: list[str]):
    old = sys.argv[:]
    sys.argv = ["add-component.py", *args]
    try:
        yield
    finally:
        sys.argv = old


def write_registry(root: pathlib.Path, text: str) -> pathlib.Path:
    registry = root / "tools" / "components" / "registry.yaml"
    registry.parent.mkdir(parents=True)
    registry.write_text(text, encoding="utf-8")
    return registry


def run_main(root: pathlib.Path, args: list[str]) -> tuple[int | None, str, str]:
    fake_script = root / "tools" / "add-component.py"
    out = io.StringIO()
    err = io.StringIO()
    with patched_argv(args), \
            mock.patch.object(add_component, "__file__", str(fake_script)), \
            contextlib.redirect_stdout(out), \
            contextlib.redirect_stderr(err):
        rc = add_component.main()
    return rc, out.getvalue(), err.getvalue()


class RegistryParsingTests(unittest.TestCase):
    def test_parse_registry_reads_component_fields_and_skips_comments(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            registry = write_registry(pathlib.Path(td), """
# Pulp component registry
components:
  - name: preset-browser
    description: Load and search presets
    status: planned
    phase: 14

  - name: meter
    description: Display levels
    status: implemented
""")

            self.assertEqual(
                add_component.parse_registry(registry),
                [
                    {
                        "name": "preset-browser",
                        "description": "Load and search presets",
                        "status": "planned",
                        "phase": "14",
                    },
                    {
                        "name": "meter",
                        "description": "Display levels",
                        "status": "implemented",
                    },
                ],
            )


class MainTests(unittest.TestCase):
    def test_missing_registry_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            rc, out, err = run_main(pathlib.Path(td), ["--list"])

        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("component registry not found", err)

    def test_list_empty_registry_reports_phase_message(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_registry(root, "# comments only\ncomponents:\n")

            rc, out, err = run_main(root, ["--list"])

        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("No components available yet.", out)
        self.assertIn("Phase 14", out)

    def test_list_non_empty_registry_prints_each_component_status(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_registry(root, """
components:
  - name: preset-browser
    description: Load and search presets
    status: planned
    phase: 14
  - name: meter
    description: Display levels
    status: implemented
""")

            rc, out, err = run_main(root, ["--list"])

        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("Available components:", out)
        self.assertIn("preset-browser [planned]", out)
        self.assertIn("Load and search presets", out)
        self.assertIn("meter [implemented]", out)

    def test_unknown_component_reports_available_names(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_registry(root, """
components:
  - name: preset-browser
    description: Load and search presets
    status: planned
""")

            rc, out, err = run_main(root, ["does-not-exist"])

        self.assertEqual(rc, 1)
        self.assertIn('component "does-not-exist" not found', err)
        self.assertIn("Available: preset-browser", out)

    def test_unknown_component_with_empty_registry_reports_no_components(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_registry(root, "components:\n")

            rc, out, err = run_main(root, ["missing"])

        self.assertEqual(rc, 1)
        self.assertIn('component "missing" not found', err)
        self.assertIn("No components available yet. Coming in Phase 14.", out)

    def test_planned_component_returns_not_implemented_message(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_registry(root, """
components:
  - name: preset-browser
    description: Load and search presets
    status: planned
    phase: 14
""")

            rc, out, err = run_main(root, ["preset-browser"])

        self.assertEqual(rc, 1)
        self.assertEqual(err, "")
        self.assertIn('Component "preset-browser" is planned for Phase 14', out)

    def test_implemented_component_reports_success(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_registry(root, """
components:
  - name: meter
    description: Display levels
    status: implemented
""")

            rc, out, err = run_main(root, ["meter"])

        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertEqual(out, 'Added component "meter" to your project.\n')

    def test_script_entry_point_exits_with_main_status(self) -> None:
        out = io.StringIO()
        err = io.StringIO()

        with patched_argv(["--list"]), \
                contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(err), \
                self.assertRaises(SystemExit) as raised:
            runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(raised.exception.code, 0)
        self.assertEqual(err.getvalue(), "")
        self.assertIn("No components available yet.", out.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
