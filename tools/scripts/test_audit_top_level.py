#!/usr/bin/env python3
"""Coverage-lane unit tests for tools/audit.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "audit.py"

spec = importlib.util.spec_from_file_location("top_level_audit", SCRIPT)
assert spec and spec.loader
audit = importlib.util.module_from_spec(spec)
sys.modules["top_level_audit"] = audit
spec.loader.exec_module(audit)


def write_required_docs(root: pathlib.Path) -> None:
    (root / "LICENSE.md").write_text("MIT\n", encoding="utf-8")
    (root / "NOTICE.md").write_text("Notices\n", encoding="utf-8")
    (root / "DEPENDENCIES.md").write_text("Dependencies\n", encoding="utf-8")


class AuditTopLevelTests(unittest.TestCase):
    def run_main(self, *args: str) -> tuple[int, str]:
        stdout = io.StringIO()
        with mock.patch.object(sys, "argv", ["audit.py", *args]), \
             contextlib.redirect_stdout(stdout):
            with self.assertRaises(SystemExit) as ctx:
                audit.main()
        return int(ctx.exception.code), stdout.getvalue()

    def test_license_files_accept_license_fallback_and_report_missing_docs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            (root / "LICENSE").write_text("MIT\n", encoding="utf-8")

            errors = audit.check_license_files(root)

        self.assertNotIn("No LICENSE.md or LICENSE file found", errors)
        self.assertEqual(
            errors,
            ["No NOTICE.md file found", "No DEPENDENCIES.md file found"],
        )

    def test_license_only_main_reports_missing_license_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            (root / "NOTICE.md").write_text("Notices\n", encoding="utf-8")
            (root / "DEPENDENCIES.md").write_text("Dependencies\n", encoding="utf-8")

            code, output = self.run_main(str(root), "--license")

        self.assertEqual(code, 1)
        self.assertIn("1 issues found", output)
        self.assertIn("No LICENSE.md or LICENSE file found", output)

    def test_vendor_scan_finds_forbidden_files_and_directories_but_skips_roots(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            (root / "tools").mkdir()
            (root / "tools" / "aax-validator").write_text("binary\n", encoding="utf-8")
            (root / "plugins" / "ASIOSDK").mkdir(parents=True)
            (root / "build" / "aax-validator").parent.mkdir()
            (root / "build" / "aax-validator").write_text("ignored\n", encoding="utf-8")
            (root / "external" / "ASIOSDK").mkdir(parents=True)

            errors = audit.check_vendor_files(root)

        joined = "\n".join(errors)
        self.assertIn("Vendor file found:", joined)
        self.assertIn("tools/aax-validator", joined)
        self.assertIn("Vendor directory found:", joined)
        self.assertIn("plugins/ASIOSDK", joined)
        self.assertNotIn("build/aax-validator", joined)
        self.assertNotIn("external/ASIOSDK", joined)

    def test_vendor_scan_allows_pulp_owned_pro_tools_host_quirks_header(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            header = (
                root
                / "core"
                / "format"
                / "include"
                / "pulp"
                / "format"
                / "host_quirks"
                / "pro_tools.hpp"
            )
            header.parent.mkdir(parents=True)
            header.write_text("#pragma once\n", encoding="utf-8")
            (root / "other" / "pro_tools.hpp").parent.mkdir()
            (root / "other" / "pro_tools.hpp").write_text("#pragma once\n", encoding="utf-8")

            errors = audit.check_vendor_files(root)

        joined = "\n".join(errors)
        self.assertNotIn(str(header), joined)
        self.assertIn("other/pro_tools.hpp", joined)

    @unittest.skipIf(
        not hasattr(pathlib.Path, "symlink_to"),
        "pathlib symlink support unavailable",
    )
    def test_vendor_scan_does_not_follow_symlink_into_allowlisted_header(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            header = (
                root
                / "core"
                / "format"
                / "include"
                / "pulp"
                / "format"
                / "host_quirks"
                / "pro_tools.hpp"
            )
            header.parent.mkdir(parents=True)
            header.write_text("#pragma once\n", encoding="utf-8")
            link = root / "other" / "pro_tools.hpp"
            link.parent.mkdir()
            try:
                link.symlink_to(header)
            except OSError as exc:
                self.skipTest(f"symlink unavailable: {exc}")

            errors = audit.check_vendor_files(root)

        joined = "\n".join(errors)
        self.assertIn("other/pro_tools.hpp", joined)

    def test_walk_fallback_yields_os_walk_entries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            nested = root / "nested"
            nested.mkdir()
            (nested / "file.txt").write_text("content\n", encoding="utf-8")

            walked = list(audit._walk(root))

        self.assertEqual(pathlib.Path(walked[0][0]), root)
        self.assertIn("nested", walked[0][1])
        self.assertEqual(pathlib.Path(walked[1][0]), nested)
        self.assertEqual(walked[1][2], ["file.txt"])

    def test_license_only_main_skips_vendor_and_private_audit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_required_docs(root)
            (root / "aax-validator").write_text("would fail full audit\n", encoding="utf-8")
            private_dir = root / ".private"
            private_dir.mkdir()
            (private_dir / "audit-naming.py").write_text(
                "import sys\nprint('should not run')\nsys.exit(1)\n",
                encoding="utf-8",
            )

            code, output = self.run_main(str(root), "--license")

        self.assertEqual(code, 0)
        self.assertIn("PASSED", output)
        self.assertNotIn("extended audit", output.lower())

    def test_main_reports_all_errors_and_extended_audit_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_required_docs(root)
            private_dir = root / ".private"
            private_dir.mkdir()
            private_audit = private_dir / "audit-naming.py"
            private_audit.write_text("placeholder\n", encoding="utf-8")

            failed = subprocess.CompletedProcess(
                args=["audit-naming.py"],
                returncode=1,
                stdout="VIOLATION: bad name\nSECOND LINE\n",
                stderr="ignored stderr\n",
            )
            with mock.patch.object(audit.subprocess, "run", return_value=failed) as run:
                code, output = self.run_main(str(root))

        self.assertEqual(code, 1)
        self.assertIn("Running extended audit", output)
        self.assertIn("3 issues found:", output)
        self.assertIn("Extended naming audit failed:", output)
        self.assertIn("  VIOLATION: bad name", output)
        self.assertIn("  SECOND LINE", output)
        run.assert_called_once_with(
            [
                sys.executable,
                str(root.resolve() / ".private" / "audit-naming.py"),
                str(root.resolve()),
                "--check-source",
                "--quiet",
            ],
            capture_output=True,
            text=True,
        )

    def test_main_passes_when_extended_audit_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_required_docs(root)
            private_dir = root / ".private"
            private_dir.mkdir()
            (private_dir / "audit-naming.py").write_text("placeholder\n", encoding="utf-8")

            passed = subprocess.CompletedProcess(
                args=["audit-naming.py"],
                returncode=0,
                stdout="clean\n",
                stderr="",
            )
            with mock.patch.object(audit.subprocess, "run", return_value=passed):
                code, output = self.run_main(str(root))

        self.assertEqual(code, 0)
        self.assertIn("Running extended audit", output)
        self.assertIn("PASSED: all checks clean", output)
        self.assertNotIn("Extended naming audit failed", output)

    def test_main_success_path_prints_passed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_required_docs(root)

            code, output = self.run_main(str(root))

        self.assertEqual(code, 0)
        self.assertIn("PASSED: all checks clean", output)

    def test_script_entrypoint_runs_main(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write_required_docs(root)

            stdout = io.StringIO()
            with mock.patch.object(sys, "argv", [str(SCRIPT), str(root), "--license"]), \
                 contextlib.redirect_stdout(stdout):
                with self.assertRaises(SystemExit) as ctx:
                    runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(int(ctx.exception.code), 0)
        self.assertIn("PASSED: all checks clean", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
