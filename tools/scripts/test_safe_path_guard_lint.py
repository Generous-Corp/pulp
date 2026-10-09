#!/usr/bin/env python3
"""Positive and negative controls for safe_path_guard_lint.py."""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("safe_path_guard_lint.py")
SPEC = importlib.util.spec_from_file_location("safe_path_guard_lint", MODULE_PATH)
assert SPEC and SPEC.loader
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)

BARE = """
bool safe_member_path(const fs::path& rel) {
    if (rel.empty() || rel.is_absolute()) return false;
    return true;
}
"""

HELPER = """
bool safe_member_path(const fs::path& rel) {
    return pulp::runtime::is_safe_relative_path(rel);
}
"""


class SafePathGuardLintTests(unittest.TestCase):
    def test_bare_is_absolute_guard_is_reported(self) -> None:
        found = lint.scan_text(BARE, "core/x/src/a.cpp")
        self.assertEqual(len(found), 1, found)
        self.assertIn("safe_member_path()", found[0])
        self.assertIn("core/x/src/a.cpp:2", found[0])

    def test_is_relative_guard_is_reported(self) -> None:
        text = "bool is_safe_rel(const fs::path& p) { return p.is_relative(); }\n"
        self.assertEqual(len(lint.scan_text(text, "tools/cli/a.cpp")), 1)

    def test_guard_through_the_helper_is_clean(self) -> None:
        self.assertEqual(lint.scan_text(HELPER, "core/x/src/a.cpp"), [])

    def test_declaration_and_unrelated_names_are_ignored(self) -> None:
        text = (
            "bool safe_archive_rel(const fs::path& rel);\n"
            "bool safe_id_component(std::string_view v) { return fs::path(v).is_absolute(); }\n"
        )
        self.assertEqual(lint.scan_text(text, "tools/cli/a.hpp"), [])

    def test_skip_marker_suppresses(self) -> None:
        text = BARE.replace("{\n", "{  // safe-path-lint: skip trusted build path\n", 1)
        self.assertEqual(lint.scan_text(text, "core/x/src/a.cpp"), [])

    def test_main_scans_a_tree_and_fails_on_a_violation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "core/x/src").mkdir(parents=True)
            (root / "tools/cli").mkdir(parents=True)
            (root / "tools/cli/ok.cpp").write_text(HELPER, encoding="utf-8")
            self.assertEqual(lint.main(["--root", str(root)]), 0)
            (root / "core/x/src/bad.cpp").write_text(BARE, encoding="utf-8")
            self.assertEqual(lint.main(["--root", str(root)]), 1)

    def test_an_empty_tree_is_a_scan_error_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(lint.main(["--root", directory]), 2)


if __name__ == "__main__":
    unittest.main()
