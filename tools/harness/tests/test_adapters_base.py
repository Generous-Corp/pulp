"""Tests for shared harness adapter primitives.

Invocation::

    python3 -m unittest tools.harness.tests.test_adapters_base
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.adapters import base  # noqa: E402
from tools.harness.adapters.base import (  # noqa: E402
    ADAPTERS,
    AdapterBase,
    CatalogEntry,
    Result,
    register_adapter,
)
from tools.harness.status import Status  # noqa: E402


class RegistrySnapshot:
    def __enter__(self) -> "RegistrySnapshot":
        self.saved = dict(ADAPTERS)
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        ADAPTERS.clear()
        ADAPTERS.update(self.saved)


class RegisterAdapterTests(unittest.TestCase):
    def test_register_adapter_stamps_surface_and_returns_class(self) -> None:
        with RegistrySnapshot():

            @register_adapter("unit-surface")
            class UnitAdapter(AdapterBase):
                pass

            self.assertIs(ADAPTERS["unit-surface"], UnitAdapter)
            self.assertIs(base.ADAPTERS, ADAPTERS)
            self.assertEqual(UnitAdapter.surface, "unit-surface")
            self.assertTrue(issubclass(UnitAdapter, AdapterBase))

    def test_register_adapter_rejects_invalid_names(self) -> None:
        for name in ("", None, 7):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    register_adapter(name)  # type: ignore[arg-type]

    def test_register_adapter_tolerates_unstampable_classes(self) -> None:
        with RegistrySnapshot():
            decorated = register_adapter("builtin-int")(int)

            self.assertIs(decorated, int)
            self.assertIs(ADAPTERS["builtin-int"], int)
            self.assertFalse(hasattr(int, "surface"))


class CatalogEntryTests(unittest.TestCase):
    def test_from_compat_json_copies_payload_fields(self) -> None:
        entry = CatalogEntry.from_compat_json(
            "css",
            "css/opacity",
            {
                "status": "supported",
                "mapsTo": "View::setOpacity",
                "supportedValues": ("0", "1"),
                "unsupportedValues": ("auto",),
                "tests": ("unit:test_opacity",),
                "notes": "covered",
                "issue": "#123",
            },
        )

        self.assertEqual(entry.surface, "css")
        self.assertEqual(entry.name, "css/opacity")
        self.assertEqual(entry.short_name, "opacity")
        self.assertEqual(entry.status, "supported")
        self.assertEqual(entry.maps_to, "View::setOpacity")
        self.assertEqual(entry.supported_values, ["0", "1"])
        self.assertEqual(entry.unsupported_values, ["auto"])
        self.assertEqual(entry.tests, ["unit:test_opacity"])
        self.assertEqual(entry.notes, "covered")
        self.assertEqual(entry.issue, "#123")
        self.assertIs(entry.expected_status, Status.PASS)

    def test_from_compat_json_defaults_missing_sequence_fields(self) -> None:
        entry = CatalogEntry.from_compat_json("custom", "loose-name", {})

        self.assertEqual(entry.short_name, "loose-name")
        self.assertIsNone(entry.status)
        self.assertEqual(entry.supported_values, [])
        self.assertEqual(entry.unsupported_values, [])
        self.assertEqual(entry.tests, [])
        self.assertIs(entry.expected_status, Status.NOT_IMPL)


class ResultTests(unittest.TestCase):
    def test_drifts_uses_catalog_expected_status(self) -> None:
        supported = CatalogEntry(surface="css", name="css/opacity", status="supported")
        self.assertFalse(Result(supported, Status.PASS).drifts)
        self.assertTrue(Result(supported, Status.NOT_IMPL).drifts)

    def test_to_dict_preserves_public_result_shape(self) -> None:
        entry = CatalogEntry(
            surface="css",
            name="css/color",
            status="supported",
            issue="#456",
        )
        result = Result(
            entry=entry,
            status=Status.SUPPORTED_NO_EVIDENCE,
            detail="missing evidence",
            matched_supported=["red"],
            unmatched_supported=["blue"],
            extra_unsupported=["blink"],
        )

        self.assertEqual(
            result.to_dict(),
            {
                "name": "css/color",
                "surface": "css",
                "catalog_status": "supported",
                "harness_status": "SUPPORTED-NO-EVIDENCE",
                "drift": True,
                "detail": "missing evidence",
                "matched_supported": ["red"],
                "unmatched_supported": ["blue"],
                "extra_unsupported": ["blink"],
                "issue": "#456",
            },
        )


class AdapterBaseTests(unittest.TestCase):
    def test_base_adapter_run_is_abstract(self) -> None:
        adapter = AdapterBase(REPO_ROOT)
        entry = CatalogEntry(surface="css", name="css/opacity")

        with self.assertRaises(NotImplementedError):
            adapter.run(entry)


if __name__ == "__main__":
    unittest.main()
