#!/usr/bin/env python3
"""Focused tests for the Shipyard Python selector."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).with_name("find_python311.py")
SPEC = importlib.util.spec_from_file_location("find_python311", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FindPython311Tests(unittest.TestCase):
    def test_current_selection_is_compatible(self) -> None:
        self.assertEqual(MODULE.main(), 0)

    def test_invalid_explicit_override_fails_closed(self) -> None:
        with mock.patch.dict(MODULE.os.environ, {"PULP_CI_PYTHON": "/missing/python"}):
            self.assertEqual(MODULE.main(), 1)

    def test_selector_prefers_named_compatible_python(self) -> None:
        def which(name: str) -> str | None:
            return "/python312" if name == "python3.12" else None

        with mock.patch.dict(MODULE.os.environ, {}, clear=True), mock.patch.object(
            MODULE.shutil, "which", side_effect=which
        ), mock.patch.object(
            MODULE, "compatible_executable", side_effect=lambda path: path
        ), mock.patch.object(MODULE, "uv_managed_python", return_value=None):
            self.assertEqual(MODULE.main(), 0)


if __name__ == "__main__":
    unittest.main()
