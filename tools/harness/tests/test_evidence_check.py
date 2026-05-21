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

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.base import CatalogEntry, Result  # noqa: E402
from tools.harness.status import Status  # noqa: E402
from tools.harness.verifier import check_evidence, _grace_active  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
