#!/usr/bin/env python3
"""Coverage-lane tests for tools/check-docs-consistency.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import runpy
import sys
import tempfile
import unittest
from textwrap import dedent


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "check-docs-consistency.py"
spec = importlib.util.spec_from_file_location("check_docs_consistency", SCRIPT)
assert spec and spec.loader
cdc = importlib.util.module_from_spec(spec)
sys.modules["check_docs_consistency"] = cdc
spec.loader.exec_module(cdc)


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
    def test_extract_matrix_accessibility_clean(self) -> None:
        self.assertEqual(
            cdc.extract_matrix_accessibility(CLEAN_MATRIX),
            {
                "macos": "usable",
                "ios": "usable",
                "android": "usable",
                "windows": "partial",
                "linux": "partial",
            },
        )

    def test_extract_matrix_accessibility_missing_block(self) -> None:
        self.assertEqual(cdc.extract_matrix_accessibility("schema_version: 1\n"), {})

    def test_extract_all_statuses_strips_quotes(self) -> None:
        text = "a:\n  status: 'usable'\nb:\n  status: \"partial\"\n"
        self.assertEqual(cdc.extract_all_statuses(text), [(2, "usable"), (4, "partial")])

    def test_parse_platform_maturity_rows_ignores_short_rows_and_missing_table(self) -> None:
        rows = cdc.parse_platform_maturity_rows(CLEAN_CAPS + "| too | short |\n")

        self.assertEqual(len(rows), 5)
        self.assertIn(("VoiceOver accessibility", "usable", "macOS"), rows)
        self.assertEqual(cdc.parse_platform_maturity_rows("# no table\n"), [])


class MainTests(unittest.TestCase):
    def _run_main(
        self,
        matrix_text: str | None,
        caps_text: str | None,
    ) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            matrix_path = root / "support-matrix.yaml"
            caps_path = root / "capabilities.md"
            if matrix_text is not None:
                matrix_path.write_text(matrix_text, encoding="utf-8")
            if caps_text is not None:
                caps_path.write_text(caps_text, encoding="utf-8")
            with contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                old_matrix = cdc.MATRIX_PATH
                old_caps = cdc.CAPABILITIES_PATH
                try:
                    cdc.MATRIX_PATH = matrix_path
                    cdc.CAPABILITIES_PATH = caps_path
                    rc = cdc.main()
                finally:
                    cdc.MATRIX_PATH = old_matrix
                    cdc.CAPABILITIES_PATH = old_caps
        return rc, stdout.getvalue(), stderr.getvalue()

    def test_clean_inputs_return_zero_with_summary(self) -> None:
        rc, stdout, stderr = self._run_main(CLEAN_MATRIX, CLEAN_CAPS)

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("docs-consistency: OK", stdout)
        self.assertIn("platform_maturity.accessibility platforms checked: 5", stdout)
        self.assertIn("status values scanned: 6", stdout)

    def test_missing_capabilities_row_is_drift(self) -> None:
        broken_caps = CLEAN_CAPS.replace(
            "| VoiceOver accessibility | usable | iOS | UIAccessibilityElement |\n",
            "",
        )

        rc, stdout, stderr = self._run_main(CLEAN_MATRIX, broken_caps)

        self.assertEqual(rc, 1)
        self.assertEqual(stderr, "")
        self.assertIn("docs-consistency: DRIFT DETECTED", stdout)
        self.assertIn("Platform Maturity row 'VoiceOver accessibility'", stdout)
        self.assertIn("platform 'iOS' missing", stdout)

    def test_status_mismatch_is_drift(self) -> None:
        broken_caps = CLEAN_CAPS.replace(
            "| TalkBack accessibility | usable | Android | JNI bridge |",
            "| TalkBack accessibility | partial | Android | JNI bridge |",
        )

        rc, stdout, stderr = self._run_main(CLEAN_MATRIX, broken_caps)

        self.assertEqual(rc, 1)
        self.assertEqual(stderr, "")
        self.assertIn("matrix=usable capabilities.md=partial", stdout)

    def test_invalid_status_and_missing_platform_are_drift(self) -> None:
        broken_matrix = CLEAN_MATRIX.replace(
            "      status: usable\n      notes: NSAccessibilityElement.",
            "      status: maybe-works\n      notes: NSAccessibilityElement.",
            1,
        ).replace(
            "    ios:\n      status: usable\n      notes: UIAccessibilityElement.\n",
            "",
        )

        rc, stdout, stderr = self._run_main(broken_matrix, CLEAN_CAPS)

        self.assertEqual(rc, 1)
        self.assertEqual(stderr, "")
        self.assertIn("invalid status 'maybe-works'", stdout)
        self.assertIn("platform_maturity.accessibility.ios missing", stdout)

    def test_missing_input_files_return_input_error(self) -> None:
        rc, stdout, stderr = self._run_main(None, CLEAN_CAPS)

        self.assertEqual(rc, 2)
        self.assertEqual(stdout, "")
        self.assertIn("Failed to read", stderr)

        rc, stdout, stderr = self._run_main(CLEAN_MATRIX, None)

        self.assertEqual(rc, 2)
        self.assertEqual(stdout, "")
        self.assertIn("Failed to read", stderr)

    def test_script_entrypoint_exits_with_main_result(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as raised:
                runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(raised.exception.code, 0)
        self.assertEqual(stderr.getvalue(), "")
        self.assertIn("docs-consistency: OK", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
