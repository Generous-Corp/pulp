#!/usr/bin/env python3
"""Unit tests for tools/scripts/host_pump_lint.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "host_pump_lint.py"

spec = importlib.util.spec_from_file_location("host_pump_lint", SCRIPT)
assert spec and spec.loader
hpl = importlib.util.module_from_spec(spec)
sys.modules["host_pump_lint"] = hpl
spec.loader.exec_module(hpl)


class HostPumpLintTests(unittest.TestCase):
    def test_scan_file_ignores_missing_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = pathlib.Path(td) / "missing.cpp"
            self.assertEqual(hpl.scan_file(missing), [])

    def test_scan_file_accepts_paired_calls_in_same_block(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = pathlib.Path(td) / "main.cpp"
            source.write_text(
                """
void tick() {
    bridge->poll_async_results();
    bridge->service_frame_callbacks();
}
""",
                encoding="utf-8",
            )
            self.assertEqual(hpl.scan_file(source), [])

    def test_scan_file_flags_unpaired_poll_before_block_close(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            source = root / "examples" / "design-tool" / "main.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                """
void tick() {
    bridge->poll_async_results();
}
void later() {
    bridge->service_frame_callbacks();
}
""",
                encoding="utf-8",
            )

            with mock.patch.object(hpl, "REPO_ROOT", root):
                violations = hpl.scan_file(source)

            self.assertEqual(len(violations), 1)
            self.assertIn("examples/design-tool/main.cpp:3", violations[0])
            self.assertIn("not paired", violations[0])

    def test_scan_file_accepts_inline_skip_marker(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = pathlib.Path(td) / "main.cpp"
            source.write_text(
                f"bridge->poll_async_results(); // {hpl.SKIP_MARKER} - one-shot\n",
                encoding="utf-8",
            )
            self.assertEqual(hpl.scan_file(source), [])

    def test_scan_file_respects_window_limit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = pathlib.Path(td) / "main.cpp"
            gap = "\n".join("    do_work();" for _ in range(hpl.WINDOW_LINES + 1))
            source.write_text(
                f"""
void tick() {{
    bridge->poll_async_results();
{gap}
    bridge->service_frame_callbacks();
}}
""",
                encoding="utf-8",
            )
            self.assertEqual(len(hpl.scan_file(source)), 1)

    def test_main_reports_violations_and_read_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            host = root / "host.cpp"
            host.write_text("bridge->poll_async_results();\n", encoding="utf-8")

            err = io.StringIO()
            with mock.patch.object(hpl, "REPO_ROOT", root), \
                 mock.patch.object(hpl, "HOST_FILES", ["host.cpp"]), \
                 contextlib.redirect_stderr(err):
                self.assertEqual(hpl.main(), 1)
            self.assertIn("idle-pump pairing violations", err.getvalue())

            with mock.patch.object(hpl, "HOST_FILES", ["host.cpp"]), \
                 mock.patch.object(hpl, "scan_file", side_effect=OSError("denied")), \
                 contextlib.redirect_stderr(err := io.StringIO()):
                self.assertEqual(hpl.main(), 2)
            self.assertIn("failed to read", err.getvalue())


if __name__ == "__main__":
    unittest.main()
