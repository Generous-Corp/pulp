#!/usr/bin/env python3
"""Tests for tools/check-docs-consistency.py.

Exercises the parsers and the cross-check against synthetic fixtures so a
regression that drops accessibility coverage, mismatches a status, or
introduces an out-of-vocabulary status is caught even when the live
support-matrix.yaml and capabilities.md happen to be clean.
"""
from __future__ import annotations

import importlib.util
import contextlib
import io
import sys
import unittest
from pathlib import Path
from textwrap import dedent

# Hyphen in module filename — load explicitly via importlib.
_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "check_docs_consistency",
    _HERE / "check-docs-consistency.py",
)
assert _SPEC and _SPEC.loader
cdc = importlib.util.module_from_spec(_SPEC)
sys.modules["check_docs_consistency"] = cdc
_SPEC.loader.exec_module(cdc)


CLEAN_MATRIX = dedent(
    """\
    schema_version: 1
    platform_maturity:
      accessibility:
        macos:
          status: usable
          notes: NSAccessibilityElement.
        ios:
          status: usable
          notes: UIAccessibilityElement.
        android:
          status: usable
          notes: TalkBack JNI bridge.
        windows:
          status: partial
          notes: UIA stub.
        linux:
          status: partial
          notes: AT-SPI stub.
      ime_composition:
        status: usable
        platform: macos
        notes: NSTextInputClient.
    """
)

CLEAN_CAPS = dedent(
    """\
    # Capabilities

    ### Platform Maturity

    | Capability | Status | Platform | Notes |
    |---|---|---|---|
    | VoiceOver accessibility | usable | macOS | NSAccessibilityElement |
    | VoiceOver accessibility | usable | iOS | UIAccessibilityElement |
    | TalkBack accessibility | usable | Android | JNI bridge |
    | UIA accessibility | partial | Windows | Role map only |
    | AT-SPI accessibility | partial | Linux | Role map only |
    """
)


class ParserTests(unittest.TestCase):
    def test_extract_matrix_accessibility_clean(self):
        result = cdc.extract_matrix_accessibility(CLEAN_MATRIX)
        self.assertEqual(
            result,
            {
                "macos": "usable",
                "ios": "usable",
                "android": "usable",
                "windows": "partial",
                "linux": "partial",
            },
        )

    def test_extract_matrix_accessibility_missing_block(self):
        self.assertEqual(cdc.extract_matrix_accessibility("schema_version: 1\n"), {})

    def test_extract_all_statuses_finds_everything(self):
        statuses = cdc.extract_all_statuses(CLEAN_MATRIX)
        # 5 accessibility platforms + 1 ime_composition.
        self.assertEqual(len(statuses), 6)
        values = {s for _, s in statuses}
        self.assertSetEqual(values, {"usable", "partial"})

    def test_extract_all_statuses_strips_quote_styles(self):
        statuses = cdc.extract_all_statuses(
            "root:\n  a:\n    status: \"usable\"\n  b:\n    status: 'partial'\n"
        )
        self.assertEqual(statuses, [(3, "usable"), (5, "partial")])

    def test_parse_platform_maturity_rows_clean(self):
        rows = cdc.parse_platform_maturity_rows(CLEAN_CAPS)
        self.assertEqual(len(rows), 5)
        macos_voiceover = next(r for r in rows if r[2] == "macOS")
        self.assertEqual(macos_voiceover[0], "VoiceOver accessibility")
        self.assertEqual(macos_voiceover[1], "usable")

    def test_parse_platform_maturity_rows_missing_table(self):
        self.assertEqual(cdc.parse_platform_maturity_rows("# Capabilities\n"), [])

    def test_parse_platform_maturity_rows_skips_malformed_rows(self):
        caps = dedent(
            """\
            ### Platform Maturity

            | Capability | Status | Platform | Notes |
            |---|---|---|---|
            | VoiceOver accessibility | usable | macOS | ok |
            | malformed | row |
            """
        )

        self.assertEqual(
            cdc.parse_platform_maturity_rows(caps),
            [("VoiceOver accessibility", "usable", "macOS")],
        )


class _FakeMain:
    """Patch MATRIX_PATH / CAPABILITIES_PATH and call cdc.main()."""

    def __init__(self, matrix_text: str, caps_text: str):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.matrix = root / "support-matrix.yaml"
        self.caps = root / "capabilities.md"
        self.matrix.write_text(matrix_text)
        self.caps.write_text(caps_text)

    def __enter__(self):
        self._orig_matrix = cdc.MATRIX_PATH
        self._orig_caps = cdc.CAPABILITIES_PATH
        cdc.MATRIX_PATH = self.matrix
        cdc.CAPABILITIES_PATH = self.caps
        return self

    def __exit__(self, *exc):
        cdc.MATRIX_PATH = self._orig_matrix
        cdc.CAPABILITIES_PATH = self._orig_caps
        self._tmp.cleanup()


class IntegrationTests(unittest.TestCase):
    def test_clean_inputs_return_zero(self):
        with _FakeMain(CLEAN_MATRIX, CLEAN_CAPS):
            self.assertEqual(cdc.main(), 0)

    def test_missing_ios_in_capabilities_md_is_drift(self):
        broken_caps = CLEAN_CAPS.replace(
            "| VoiceOver accessibility | usable | iOS | UIAccessibilityElement |\n",
            "",
        )
        with _FakeMain(CLEAN_MATRIX, broken_caps):
            self.assertEqual(cdc.main(), 1)

    def test_status_mismatch_is_drift(self):
        broken_caps = CLEAN_CAPS.replace(
            "| TalkBack accessibility | usable | Android | JNI bridge |",
            "| TalkBack accessibility | partial | Android | JNI bridge |",
        )
        with _FakeMain(CLEAN_MATRIX, broken_caps):
            self.assertEqual(cdc.main(), 1)

    def test_invalid_status_vocabulary_is_drift(self):
        broken_matrix = CLEAN_MATRIX.replace(
            "      status: usable\n      notes: NSAccessibilityElement.",
            "      status: maybe-works\n      notes: NSAccessibilityElement.",
            1,
        )
        with _FakeMain(broken_matrix, CLEAN_CAPS):
            self.assertEqual(cdc.main(), 1)

    def test_missing_platform_in_matrix_is_drift(self):
        broken_matrix = CLEAN_MATRIX.replace(
            "    ios:\n      status: usable\n      notes: UIAccessibilityElement.\n",
            "",
        )
        with _FakeMain(broken_matrix, CLEAN_CAPS):
            self.assertEqual(cdc.main(), 1)

    def test_missing_matrix_file_returns_input_error(self):
        with _FakeMain(CLEAN_MATRIX, CLEAN_CAPS) as fake:
            fake.matrix.unlink()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(cdc.main(), 2)
            self.assertIn("Failed to read", stderr.getvalue())

    def test_missing_capabilities_file_returns_input_error(self):
        with _FakeMain(CLEAN_MATRIX, CLEAN_CAPS) as fake:
            fake.caps.unlink()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(cdc.main(), 2)
            self.assertIn("Failed to read", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
