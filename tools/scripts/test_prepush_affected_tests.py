#!/usr/bin/env python3
"""Focused controls for the pre-push affected-test wrapper."""
from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prepush_affected_tests", ROOT / "tools/scripts/prepush_affected_tests.py"
)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def test_missing_build_is_an_explicit_skip() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        assert module.main(["--root", tmp]) == 77


def test_find_build_requires_configure_marker() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "build").mkdir()
        assert module.find_build(root, None) is None
        (root / "build" / "CMakeCache.txt").write_text(
            "# configured\n", encoding="utf-8"
        )
        assert module.find_build(root, None) == (root / "build").resolve()


if __name__ == "__main__":
    test_missing_build_is_an_explicit_skip()
    test_find_build_requires_configure_marker()
    print("prepush_affected_tests: ok")
