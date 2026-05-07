"""Tests for the catalog-status -> harness-Status mapping (pulp #1475).

Pins the five-value catalog vocabulary:

* `supported`  -> PASS
* `partial`    -> DIVERGE
* `noop`       -> NO_OP   (added in #1475)
* `missing`    -> NOT_IMPL
* `wontfix`    -> OOS
* unknown / None / "" -> NOT_IMPL (defensive default)

Plus a round-trip sanity check via :class:`CatalogEntry.expected_status`,
which is what every adapter's drift detection actually consults.

Invocation::

    python3 -m unittest tools.harness.tests.test_status
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

# Make the repo root importable when this file is run directly.
HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters.base import CatalogEntry  # noqa: E402
from tools.harness.status import (  # noqa: E402
    Status,
    map_catalog_status_to_expected,
)


class MapCatalogStatusToExpectedTests(unittest.TestCase):
    """Direct tests on the public helper."""

    def test_supported_maps_to_pass(self) -> None:
        self.assertIs(map_catalog_status_to_expected("supported"), Status.PASS)

    def test_partial_maps_to_diverge(self) -> None:
        self.assertIs(map_catalog_status_to_expected("partial"), Status.DIVERGE)

    def test_noop_maps_to_no_op(self) -> None:
        # The pulp #1475 vocabulary extension. Without this, css/animation*
        # and css/touchAction (intentional bridge NO-OPs) drift forever.
        self.assertIs(map_catalog_status_to_expected("noop"), Status.NO_OP)

    def test_missing_maps_to_not_impl(self) -> None:
        self.assertIs(map_catalog_status_to_expected("missing"), Status.NOT_IMPL)

    def test_wontfix_maps_to_oos(self) -> None:
        self.assertIs(map_catalog_status_to_expected("wontfix"), Status.OOS)

    def test_none_defaults_to_not_impl(self) -> None:
        self.assertIs(map_catalog_status_to_expected(None), Status.NOT_IMPL)

    def test_empty_string_defaults_to_not_impl(self) -> None:
        # Empty / unknown statuses fall back to NOT_IMPL — defensive.
        self.assertIs(map_catalog_status_to_expected(""), Status.NOT_IMPL)

    def test_unknown_value_defaults_to_not_impl(self) -> None:
        self.assertIs(
            map_catalog_status_to_expected("definitely-not-a-status"),
            Status.NOT_IMPL,
        )

    def test_case_insensitive(self) -> None:
        # The catalog is hand-edited; tolerate accidental capitalisation.
        self.assertIs(map_catalog_status_to_expected("NOOP"), Status.NO_OP)
        self.assertIs(map_catalog_status_to_expected("NoOp"), Status.NO_OP)
        self.assertIs(map_catalog_status_to_expected("Supported"), Status.PASS)

    def test_strips_whitespace(self) -> None:
        self.assertIs(map_catalog_status_to_expected("  noop  "), Status.NO_OP)


class CatalogEntryExpectedStatusTests(unittest.TestCase):
    """Round-trip the mapping through :class:`CatalogEntry`.

    Drift detection in :meth:`Result.drifts` compares the harness
    verdict against ``entry.expected_status`` — which is just
    :func:`map_catalog_status_to_expected` reading the entry's
    ``status`` field. If this round-trip diverges, every adapter
    silently mis-classifies drift.
    """

    @staticmethod
    def _make_entry(status: str) -> CatalogEntry:
        return CatalogEntry(surface="css", name="css/__t__", status=status)

    def test_noop_entry_expected_status_is_no_op(self) -> None:
        entry = self._make_entry("noop")
        self.assertIs(entry.expected_status, Status.NO_OP)

    def test_partial_entry_expected_status_is_diverge(self) -> None:
        entry = self._make_entry("partial")
        self.assertIs(entry.expected_status, Status.DIVERGE)

    def test_full_round_trip_for_every_vocabulary_entry(self) -> None:
        # Single source of truth — if a future PR adds a new catalog
        # status, both sides need an entry here.
        cases = [
            ("supported", Status.PASS),
            ("partial", Status.DIVERGE),
            ("noop", Status.NO_OP),
            ("missing", Status.NOT_IMPL),
            ("wontfix", Status.OOS),
        ]
        for catalog_status, expected in cases:
            with self.subTest(status=catalog_status):
                entry = self._make_entry(catalog_status)
                self.assertIs(entry.expected_status, expected)


if __name__ == "__main__":
    unittest.main()
