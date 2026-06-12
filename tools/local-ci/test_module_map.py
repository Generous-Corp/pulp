#!/usr/bin/env python3
"""Contract tests for the local-ci module ownership map."""

from __future__ import annotations

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


if __name__ == "__main__":
    unittest.main()
