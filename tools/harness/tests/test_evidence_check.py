"""Tests for the evidence.tests gate (pulp #1657 control #1).

Verifies:
* `check_evidence()` demotes PASS→SUPPORTED_NO_EVIDENCE when
  ``entry.tests`` is empty after the grace period.
* During the grace period, PASS is retained with a warning.
* Entries with an existing test file stay PASS.
* `partial` entries are not affected.
* `_audit._evidence_grace_until` controls the grace window.

Invocation::

    python3 -m unittest tools.harness.tests.test_evidence_check
"""

from __future__ import annotations

import json
import sys
import unittest
from datetime import date, timedelta
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.base import CatalogEntry, Result  # noqa: E402
from tools.harness.status import Status  # noqa: E402
from tools.harness.verifier import (  # noqa: E402
    _TEST_CASE_INDEX_CACHE,
    _extract_test_case_index,
    _extract_test_case_tags,
    _grace_active,
    _validate_test_ref,
    check_evidence,
)


def _make_entry(
    surface: str = "css",
    name: str = "css/prop",
    status: str = "supported",
    tests: list | None = None,
) -> CatalogEntry:
    return CatalogEntry(
        surface=surface,
        name=name,
        status=status,
        tests=tests or [],
    )


def _make_result(entry: CatalogEntry, status: Status = Status.PASS) -> Result:
    return Result(entry=entry, status=status)


class GraceActiveTests(unittest.TestCase):
    def test_no_grace_when_none(self) -> None:
        self.assertFalse(_grace_active(None))

    def test_no_grace_when_empty(self) -> None:
        self.assertFalse(_grace_active(""))

    def test_grace_active_before_deadline(self) -> None:
        future = (date.today() + timedelta(days=30)).isoformat()
        self.assertTrue(_grace_active(future))

    def test_grace_inactive_after_deadline(self) -> None:
        past = (date.today() - timedelta(days=1)).isoformat()
        self.assertFalse(_grace_active(past))

    def test_grace_active_on_deadline(self) -> None:
        today_str = date.today().isoformat()
        self.assertTrue(_grace_active(today_str))

    def test_invalid_date_no_grace(self) -> None:
        self.assertFalse(_grace_active("not-a-date"))


class EvidenceCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = TemporaryDirectory()
        self.repo_root = Path(self.tmp.name)
        _TEST_CASE_INDEX_CACHE.clear()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _compat(self, grace_until: str | None = None) -> dict:
        compat: dict = {}
        if grace_until:
            compat["_audit"] = {"_evidence_grace_until": grace_until}
        return compat

    def test_supported_with_valid_test_stays_pass(self) -> None:
        test_path = self.repo_root / "test/test_foo.cpp"
        test_path.parent.mkdir(parents=True, exist_ok=True)
        test_path.write_text('TEST_CASE("foo evidence", "[tag]") {}\n')
        entry = _make_entry(name="css/foo", tests=["test/test_foo.cpp [tag]"])
        results = [_make_result(entry, Status.PASS)]
        updated = check_evidence(self.repo_root, results, self._compat())
        self.assertEqual(updated[0].status, Status.PASS)

    def test_supported_without_tests_during_grace_stays_pass(self) -> None:
        future = (date.today() + timedelta(days=30)).isoformat()
        entry = _make_entry(name="css/noTests", tests=[])  # empty tests
        results = [_make_result(entry, Status.PASS)]
        updated = check_evidence(self.repo_root, results, self._compat(future))
        self.assertEqual(updated[0].status, Status.PASS)

    def test_supported_without_tests_after_grace_demotes(self) -> None:
        # Use a grace date that is definitely in the past.
        past_grace = "2020-01-01"
        entry = _make_entry(name="css/noTests", tests=[])
        results = [_make_result(entry, Status.PASS)]
        updated = check_evidence(self.repo_root, results, self._compat(past_grace))
        self.assertEqual(updated[0].status, Status.SUPPORTED_NO_EVIDENCE)
        self.assertIn("evidence.tests is empty", updated[0].detail or "")

    def test_supported_with_nonexistent_test_file_after_grace_demotes(self) -> None:
        past = (date.today() - timedelta(days=1)).isoformat()
        entry = _make_entry(name="css/missingTests",
                            tests=["test/nonexistent.cpp [tag]"])
        results = [_make_result(entry, Status.PASS)]
        updated = check_evidence(self.repo_root, results, self._compat(past))
        self.assertEqual(updated[0].status, Status.SUPPORTED_NO_EVIDENCE)
        self.assertIn("test/nonexistent.cpp", updated[0].detail or "")
        self.assertIn("not found", updated[0].detail or "")

    def test_supported_with_valid_and_dangling_tests_stays_pass_and_warns(self) -> None:
        test_path = self.repo_root / "test/test_good.cpp"
        test_path.parent.mkdir(parents=True, exist_ok=True)
        test_path.write_text('TEST_CASE("good evidence", "[tag]") {}\n')
        entry = _make_entry(
            name="css/mixedEvidence",
            tests=[
                "test/test_good.cpp [tag]",
                "test/test_good.cpp [missing-tag]",
            ],
        )
        results = [_make_result(entry, Status.PASS)]
        with self.assertLogs("pulp.harness.verifier", level="WARNING") as logs:
            updated = check_evidence(self.repo_root, results, self._compat())
        self.assertEqual(updated[0].status, Status.PASS)
        log_text = "\n".join(logs.output)
        self.assertIn("dangling test reference", log_text)
        self.assertIn("missing-tag", log_text)

    def test_supported_with_all_dangling_tests_during_grace_stays_pass_and_warns(self) -> None:
        future = (date.today() + timedelta(days=30)).isoformat()
        entry = _make_entry(
            name="css/graceDangling",
            tests=["test/missing.cpp [tag]", "test/also_missing.cpp [tag]"],
        )
        results = [_make_result(entry, Status.PASS)]
        with self.assertLogs("pulp.harness.verifier", level="WARNING") as logs:
            updated = check_evidence(self.repo_root, results, self._compat(future))
        self.assertEqual(updated[0].status, Status.PASS)
        log_text = "\n".join(logs.output)
        self.assertIn("grace period until", log_text)
        self.assertIn("test/missing.cpp", log_text)
        self.assertIn("test/also_missing.cpp", log_text)

    def test_partial_without_tests_not_affected(self) -> None:
        past = (date.today() - timedelta(days=1)).isoformat()
        entry = _make_entry(name="css/partial", status="partial", tests=[])
        results = [_make_result(entry, Status.DIVERGE)]
        updated = check_evidence(self.repo_root, results, self._compat(past))
        self.assertEqual(updated[0].status, Status.DIVERGE)

    def test_unsupported_entry_with_pass_status_not_affected(self) -> None:
        past = (date.today() - timedelta(days=1)).isoformat()
        entry = _make_entry(name="css/notSupported", status="unsupported", tests=[])
        results = [_make_result(entry, Status.PASS)]
        updated = check_evidence(self.repo_root, results, self._compat(past))
        self.assertEqual(updated[0].status, Status.PASS)

    def test_supported_entry_with_non_pass_result_not_affected(self) -> None:
        past = (date.today() - timedelta(days=1)).isoformat()
        entry = _make_entry(name="css/diverge", status="supported", tests=[])
        results = [_make_result(entry, Status.DIVERGE)]
        updated = check_evidence(self.repo_root, results, self._compat(past))
        self.assertEqual(updated[0].status, Status.DIVERGE)

    def test_mixed_results_handled_correctly(self) -> None:
        test_path = self.repo_root / "test/test_good.cpp"
        test_path.parent.mkdir(parents=True, exist_ok=True)
        test_path.write_text('TEST_CASE("good evidence", "[tag]") {}\n')
        past = (date.today() - timedelta(days=1)).isoformat()

        good = _make_entry(name="css/good", tests=["test/test_good.cpp [tag]"])
        bad = _make_entry(name="css/bad", tests=[])
        partial = _make_entry(name="css/partial", status="partial", tests=[])

        results = [
            _make_result(good, Status.PASS),
            _make_result(bad, Status.PASS),
            _make_result(partial, Status.DIVERGE),
        ]
        updated = check_evidence(self.repo_root, results, self._compat(past))

        self.assertEqual(updated[0].status, Status.PASS)
        self.assertEqual(updated[1].status, Status.SUPPORTED_NO_EVIDENCE)
        self.assertEqual(updated[2].status, Status.DIVERGE)

    def test_no_audit_section_treats_as_no_grace(self) -> None:
        entry = _make_entry(name="css/foo", tests=[])
        results = [_make_result(entry, Status.PASS)]
        updated = check_evidence(self.repo_root, results, {})
        self.assertEqual(updated[0].status, Status.SUPPORTED_NO_EVIDENCE)

    def test_test_case_index_extracts_concatenated_names_tags_and_caches(self) -> None:
        test_path = self.repo_root / "test/test_widget.cpp"
        test_path.parent.mkdir(parents=True, exist_ok=True)
        test_path.write_text(
            'TEST_CASE("first " "case", "[alpha][ beta ]") {}\n'
            'TEST_CASE("single arg " "case") {}\n',
            encoding="utf-8",
        )

        tags, names = _extract_test_case_index(test_path)
        test_path.write_text("not parsed after cache fill\n", encoding="utf-8")
        cached_tags, cached_names = _extract_test_case_index(test_path)

        self.assertEqual(tags, {"alpha", "beta"})
        self.assertEqual(names, {"first case", "single arg case"})
        self.assertIs(tags, cached_tags)
        self.assertIs(names, cached_names)
        self.assertEqual(_extract_test_case_tags(test_path), {"alpha", "beta"})
        self.assertIn(test_path, _TEST_CASE_INDEX_CACHE)

    def test_test_case_index_handles_missing_unreadable_and_empty_names(self) -> None:
        missing = self.repo_root / "test/missing.cpp"
        self.assertEqual(_extract_test_case_index(missing), (set(), set()))

        unreadable = self.repo_root / "test/unreadable.cpp"
        unreadable.parent.mkdir(parents=True, exist_ok=True)
        unreadable.write_text('TEST_CASE("hidden", "[hidden]") {}\n', encoding="utf-8")
        original_read_text = Path.read_text

        def fake_read_text(path: Path, *args: object, **kwargs: object) -> str:
            if path == unreadable:
                raise OSError("permission denied")
            return original_read_text(path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", fake_read_text):
            self.assertEqual(_extract_test_case_index(unreadable), (set(), set()))
        self.assertIn(unreadable, _TEST_CASE_INDEX_CACHE)

        empty_name = self.repo_root / "test/empty.cpp"
        empty_name.write_text('TEST_CASE("") {}\n', encoding="utf-8")
        self.assertEqual(_extract_test_case_index(empty_name), (set(), set()))

    def test_validate_test_ref_accepts_typed_fixture_ids_and_existing_paths(self) -> None:
        js_path = self.repo_root / "packages/app/widget.test.ts"
        js_path.parent.mkdir(parents=True, exist_ok=True)
        js_path.write_text("test('widget', () => {})\n", encoding="utf-8")
        cpp_path = self.repo_root / "test/test_widget.cpp"
        cpp_path.parent.mkdir(parents=True, exist_ok=True)
        cpp_path.write_text('TEST_CASE("widget", "[widget]") {}\n', encoding="utf-8")

        for ref in (
            "cannot-validate: host limitation",
            "semantic:yoga/aspect-ratio",
            "visual:harness/golden-id",
            "dom:react-native/event-fixture",
            "unit:plain-fixture-id",
            "unit:test/test_widget.cpp",
            "behavior:packages/app/widget.test.ts",
        ):
            with self.subTest(ref=ref):
                ok, reason = _validate_test_ref(self.repo_root, ref)
                self.assertTrue(ok)
                self.assertEqual(reason, "")

    def test_validate_test_ref_rejects_missing_typed_and_named_paths(self) -> None:
        for ref, needle in (
            ("unit:test/missing.cpp", "typed ref"),
            ("behavior:packages/app/missing.test.ts", "typed ref"),
            ("test/missing.ts::case", "file `test/missing.ts` not found"),
            ("test/missing.cpp::case", "file `test/missing.cpp` not found"),
            ("test/missing.cpp [tag]", "file `test/missing.cpp` not found"),
            ("", "empty test reference"),
        ):
            with self.subTest(ref=ref):
                ok, reason = _validate_test_ref(self.repo_root, ref)
                self.assertFalse(ok)
                self.assertIn(needle, reason)

    def test_validate_test_ref_checks_cpp_names_and_preserves_bracket_precedence(self) -> None:
        cpp_path = self.repo_root / "test/test_widget.cpp"
        cpp_path.parent.mkdir(parents=True, exist_ok=True)
        cpp_path.write_text(
            'TEST_CASE("named case", "[tag]") {}\n'
            'TEST_CASE("name " "with concat", "[concat]") {}\n',
            encoding="utf-8",
        )

        ok, reason = _validate_test_ref(self.repo_root, 'test/test_widget.cpp::"named case"')
        self.assertTrue(ok)
        self.assertEqual(reason, "")

        ok, reason = _validate_test_ref(self.repo_root, "test/test_widget.cpp::name with concat")
        self.assertTrue(ok)
        self.assertEqual(reason, "")

        ok, reason = _validate_test_ref(
            self.repo_root,
            "test/test_widget.cpp [tag] (View::method mention)",
        )
        self.assertTrue(ok)
        self.assertEqual(reason, "")

        ok, reason = _validate_test_ref(self.repo_root, "test/test_widget.cpp::missing case")
        self.assertFalse(ok)
        self.assertIn("TEST_CASE name 'missing case' not found", reason)

    def test_validate_test_ref_handles_path_only_and_tag_edge_cases(self) -> None:
        path_only = self.repo_root / "test/path_only.txt"
        path_only.parent.mkdir(parents=True, exist_ok=True)
        path_only.write_text("exists\n", encoding="utf-8")
        no_cases = self.repo_root / "test/no_cases.cpp"
        no_cases.write_text("// no TEST_CASE declarations\n", encoding="utf-8")
        tagged = self.repo_root / "test/tagged.cpp"
        tagged.write_text('TEST_CASE("tagged", "[present]") {}\n', encoding="utf-8")

        ok, reason = _validate_test_ref(self.repo_root, "test/path_only.txt")
        self.assertTrue(ok)
        self.assertEqual(reason, "")

        ok, reason = _validate_test_ref(self.repo_root, "test/tagged.cpp []")
        self.assertTrue(ok)
        self.assertEqual(reason, "")

        ok, reason = _validate_test_ref(self.repo_root, "test/no_cases.cpp [missing]")
        self.assertFalse(ok)
        self.assertIn("no TEST_CASE was found", reason)

        ok, reason = _validate_test_ref(self.repo_root, "test/tagged.cpp [missing]")
        self.assertFalse(ok)
        self.assertIn("tag(s) ['missing'] not found", reason)

    def test_check_evidence_after_grace_summarizes_all_dangling_reasons(self) -> None:
        past = (date.today() - timedelta(days=1)).isoformat()
        test_path = self.repo_root / "test/test_foo.cpp"
        test_path.parent.mkdir(parents=True, exist_ok=True)
        test_path.write_text('TEST_CASE("foo evidence", "[real]") {}\n', encoding="utf-8")
        entry = _make_entry(
            name="css/allDangling",
            tests=[
                "test/test_foo.cpp [missing]",
                "test/absent.cpp [tag]",
            ],
        )

        updated = check_evidence(self.repo_root, [_make_result(entry)], self._compat(past))

        self.assertEqual(updated[0].status, Status.SUPPORTED_NO_EVIDENCE)
        self.assertIn("every test reference is dangling", updated[0].detail or "")
        self.assertIn("test/test_foo.cpp [missing]", updated[0].detail or "")
        self.assertIn("test/absent.cpp [tag]", updated[0].detail or "")
        self.assertIn("tag(s) ['missing']", updated[0].detail or "")
        self.assertIn("not found", updated[0].detail or "")


if __name__ == "__main__":
    unittest.main()
