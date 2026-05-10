"""Tests for the test-reference validator (pulp #1737).

Verifies that `_validate_test_ref()` correctly accepts/rejects each form
of catalog test reference:

* Bracketed `[tag]` form — tag must appear in some TEST_CASE in the file.
* Bare `path::"name"` quoted form — name must match a TEST_CASE first arg.
* Bare `path::name` unquoted form — same name match.
* Typed prefixes (`unit:` / `semantic:` / `visual:` / `dom:` /
  `behavior:` / `cannot-validate:`) — accepted on prefix alone.
* Free-form description text after the bracket tags must NOT trip the
  `::` name-form parser when `[` precedes `::`.

Invocation::

    python3 -m unittest tools.harness.tests.test_validate_test_ref
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent))

from tools.harness.verifier import (
    _validate_test_ref,
    _extract_test_case_index,
    _TEST_CASE_INDEX_CACHE,
)


SAMPLE_TEST_CPP = '''\
TEST_CASE("first test name", "[fast][issue-100]") {
    REQUIRE(true);
}

TEST_CASE("multi-line "
          "name across two literals",
          "[slow][issue-200]") {
    REQUIRE(true);
}

TEST_CASE("name only no tags") {
    REQUIRE(true);
}
'''


class ValidateTestRefTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = TemporaryDirectory()
        self.repo = Path(self.tmpdir.name)
        # Mirror the Pulp layout: tests live under test/.
        (self.repo / "test").mkdir()
        (self.repo / "test" / "sample.cpp").write_text(SAMPLE_TEST_CPP)
        # Clear the verifier's per-process cache so each test sees a
        # fresh index of the sample file (the cache is keyed on absolute
        # Path, so different TemporaryDirectory paths normally don't
        # collide — but be defensive).
        _TEST_CASE_INDEX_CACHE.clear()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()
        _TEST_CASE_INDEX_CACHE.clear()

    # ── Index extraction ────────────────────────────────────────────────
    def test_extracts_tags_and_names(self) -> None:
        tags, names = _extract_test_case_index(self.repo / "test" / "sample.cpp")
        self.assertIn("fast", tags)
        self.assertIn("issue-100", tags)
        self.assertIn("slow", tags)
        self.assertIn("issue-200", tags)
        self.assertIn("first test name", names)
        # Multi-line name should be joined.
        self.assertIn("multi-line name across two literals", names)
        self.assertIn("name only no tags", names)

    # ── Bracketed [tag] form ───────────────────────────────────────────
    def test_bracket_tag_present(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, "test/sample.cpp [issue-100]"
        )
        self.assertTrue(ok, msg=reason)

    def test_bracket_tag_missing(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, "test/sample.cpp [does-not-exist]"
        )
        self.assertFalse(ok)
        self.assertIn("does-not-exist", reason)

    def test_multiple_bracket_tags_all_present(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, "test/sample.cpp [fast][issue-100]"
        )
        self.assertTrue(ok, msg=reason)

    def test_multiple_bracket_tags_one_missing(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, "test/sample.cpp [fast][does-not-exist]"
        )
        self.assertFalse(ok)
        self.assertIn("does-not-exist", reason)

    # ── path::"quoted name" form ───────────────────────────────────────
    def test_quoted_name_form_present(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, 'test/sample.cpp::"first test name"'
        )
        self.assertTrue(ok, msg=reason)

    def test_quoted_name_form_missing(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, 'test/sample.cpp::"renamed test"'
        )
        self.assertFalse(ok)
        self.assertIn("renamed test", reason)

    # ── path::name (unquoted) form ─────────────────────────────────────
    def test_unquoted_name_form_present(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, "test/sample.cpp::first test name"
        )
        self.assertTrue(ok, msg=reason)

    def test_unquoted_name_form_missing(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, "test/sample.cpp::renamed test"
        )
        self.assertFalse(ok)
        self.assertIn("renamed test", reason)

    def test_multi_line_name_joined(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo,
            "test/sample.cpp::multi-line name across two literals",
        )
        self.assertTrue(ok, msg=reason)

    # ── Typed prefixes ─────────────────────────────────────────────────
    def test_unit_prefix_accepted(self) -> None:
        ok, _ = _validate_test_ref(self.repo, "unit:foo/bar.test")
        self.assertTrue(ok)

    def test_semantic_prefix_accepted(self) -> None:
        ok, _ = _validate_test_ref(self.repo, "semantic:yoga/foo")
        self.assertTrue(ok)

    def test_cannot_validate_prefix_accepted(self) -> None:
        ok, _ = _validate_test_ref(
            self.repo, "cannot-validate:#999 platform-only"
        )
        self.assertTrue(ok)

    # ── File-existence ─────────────────────────────────────────────────
    def test_missing_file_rejected(self) -> None:
        ok, reason = _validate_test_ref(self.repo, "test/missing.cpp")
        self.assertFalse(ok)
        self.assertIn("not found", reason)

    def test_missing_file_with_tags_rejected(self) -> None:
        ok, reason = _validate_test_ref(
            self.repo, "test/missing.cpp [issue-100]"
        )
        self.assertFalse(ok)
        self.assertIn("not found", reason)

    # ── Free-form description text after [tag] ──────────────────────────
    def test_description_text_after_brackets_ignored(self) -> None:
        """Free-form `(View::method...)` after `[tag]` must NOT trip the
        `::` name-form parser. The bracket-tag form takes precedence
        when `[` precedes `::`."""
        ok, reason = _validate_test_ref(
            self.repo,
            "test/sample.cpp [issue-100] (some description with View::method)",
        )
        self.assertTrue(ok, msg=reason)

    # ── Empty + edge cases ─────────────────────────────────────────────
    def test_empty_ref_rejected(self) -> None:
        ok, reason = _validate_test_ref(self.repo, "")
        self.assertFalse(ok)

    def test_path_only_no_tags_present(self) -> None:
        ok, _ = _validate_test_ref(self.repo, "test/sample.cpp")
        self.assertTrue(ok)

    def test_non_cpp_path_with_name_form_only_checks_existence(self) -> None:
        """`.ts` (jest) and other non-Catch2 test paths get file-existence
        check only — we don't grep TEST_CASE in non-.cpp files."""
        (self.repo / "test" / "sample.test.ts").write_text(
            "describe('foo', () => { it('bar', () => {}); });"
        )
        ok, _ = _validate_test_ref(
            self.repo, 'test/sample.test.ts::"bar"'
        )
        self.assertTrue(ok)
        # Missing .ts file still rejected.
        ok2, reason = _validate_test_ref(
            self.repo, 'test/missing.test.ts::"bar"'
        )
        self.assertFalse(ok2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
