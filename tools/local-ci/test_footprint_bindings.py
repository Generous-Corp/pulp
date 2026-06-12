#!/usr/bin/env python3
"""Binding tests for local-ci footprint helper facades."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import footprint_bindings  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
