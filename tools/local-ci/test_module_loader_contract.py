#!/usr/bin/env python3
"""Contract tests for local-ci test module loading helpers."""

from __future__ import annotations

from pathlib import Path
import unittest


LOCAL_CI_DIR = Path(__file__).parent
LEGACY_LOADER_TOKENS = ("load_module_from_path", "MODULE_PATH")
INTENTIONAL_LEGACY_LOADER_TESTS = {
    "test_binding_installer_fallback_contract.py",
    "test_local_ci_facade_bindings.py",
    "test_windows_validation_script.py",
}


class ModuleLoaderContractTests(unittest.TestCase):
    def test_only_intentional_tests_use_path_loaders_or_source_module_paths(self) -> None:
        offenders: list[str] = []

        for path in sorted(LOCAL_CI_DIR.glob("test_*.py")):
            if path.name == Path(__file__).name:
                continue
            if path.name in INTENTIONAL_LEGACY_LOADER_TESTS:
                continue
            text = path.read_text()
            matched_tokens = [token for token in LEGACY_LOADER_TOKENS if token in text]
            if matched_tokens:
                offenders.append(f"{path.name}: {', '.join(matched_tokens)}")

        self.assertEqual(offenders, [])

    def test_intentional_path_loader_exceptions_are_still_present(self) -> None:
        missing = [
            name
            for name in sorted(INTENTIONAL_LEGACY_LOADER_TESTS)
            if not (LOCAL_CI_DIR / name).exists()
        ]
        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main()
