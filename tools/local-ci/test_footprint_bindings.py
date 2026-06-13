#!/usr/bin/env python3
"""Binding tests for local-ci footprint helper facades."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("footprint_bindings.py")
footprint_bindings = load_module_from_path(MODULE_PATH, module_name="footprint_bindings", add_module_dir=True)


class FakeFootprint:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def format_size_bytes(self, value):
        self.calls.append(("format_size_bytes", value))
        return "formatted"

    def path_size_bytes(self, path):
        self.calls.append(("path_size_bytes", path))
        return 42

    def local_ci_state_footprint(self):
        self.calls.append(("local_ci_state_footprint",))
        return {"total_bytes": 42}

    def state_footprint_lines(self, footprint, *, indent=""):
        self.calls.append(("state_footprint_lines", footprint, indent))
        return [f"{indent}line"]

    def describe_path_for_cleanup(self, path):
        self.calls.append(("describe_path_for_cleanup", path))
        return "relative/path"


class FootprintBindingTests(unittest.TestCase):
    def test_footprint_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeFootprint()
        bindings = {"_footprint": fake}
        path = Path("state")
        footprint = {"total_bytes": 42}

        self.assertEqual(footprint_bindings.format_size_bytes(bindings, 42), "formatted")
        self.assertEqual(footprint_bindings.path_size_bytes(bindings, path), 42)
        self.assertEqual(footprint_bindings.local_ci_state_footprint(bindings), footprint)
        self.assertEqual(footprint_bindings.state_footprint_lines(bindings, footprint, indent="  "), ["  line"])
        self.assertEqual(footprint_bindings.describe_path_for_cleanup(bindings, path), "relative/path")
        self.assertEqual(
            fake.calls,
            [
                ("format_size_bytes", 42),
                ("path_size_bytes", path),
                ("local_ci_state_footprint",),
                ("state_footprint_lines", footprint, "  "),
                ("describe_path_for_cleanup", path),
            ],
        )

    def test_install_footprint_helpers_wires_named_exports(self) -> None:
        fake = FakeFootprint()
        bindings = {"_footprint": fake}

        footprint_bindings.install_footprint_helpers(bindings, ("format_size_bytes", "path_size_bytes"))

        self.assertEqual(bindings["format_size_bytes"](42), "formatted")
        self.assertEqual(bindings["path_size_bytes"](Path("state")), 42)
        self.assertEqual(bindings["format_size_bytes"].__name__, "format_size_bytes")
        self.assertEqual([call[0] for call in fake.calls], ["format_size_bytes", "path_size_bytes"])

    def test_install_footprint_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        footprint_bindings.future_footprint_helper = lambda _bindings: "future"

        footprint_bindings.install_footprint_helpers(bindings, ("future_footprint_helper",))

        self.assertEqual(bindings["future_footprint_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
