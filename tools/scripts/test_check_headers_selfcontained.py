#!/usr/bin/env python3
"""Unit coverage for check_headers_selfcontained.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("check_headers_selfcontained.py")
SPEC = importlib.util.spec_from_file_location("check_headers_selfcontained", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


class HeaderSelfContainedTests(unittest.TestCase):
    def test_module_detection_and_compile_arg_filtering(self) -> None:
        self.assertEqual(checker.module_of("core/view/src/view.cpp"), "core/view")
        self.assertEqual(checker.module_of("x/core/audio/test.cpp"), "core/audio")
        self.assertIsNone(checker.module_of("tools/scripts/tool.py"))

        filtered = checker.filter_args(
            [
                "clang++",
                "-Iinclude",
                "-DDEBUG=1",
                "-std=c++20",
                "-c",
                "widget.cpp",
                "-o",
                "widget.cpp.o",
                "extra.o",
                "-Wall",
            ],
            "core/view/src/widget.cpp",
        )
        self.assertEqual(filtered, ["-Iinclude", "-DDEBUG=1", "-std=c++20", "-Wall"])
        self.assertNotIn("-c", filtered)
        self.assertNotIn("widget.cpp", filtered)
        self.assertNotIn("widget.cpp.o", filtered)

    def test_load_module_flags_accepts_arguments_and_command_forms(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            cc = Path(td) / "compile_commands.json"
            cc.write_text(
                json.dumps(
                    [
                        {
                            "file": "core/view/src/a.cpp",
                            "directory": "/tmp/view",
                            "arguments": ["c++", "-Iview", "-c", "a.cpp", "-o", "a.o"],
                        },
                        {
                            "file": "core/view/src/b.cpp",
                            "directory": "/tmp/ignored",
                            "arguments": ["c++", "-Iignored", "-c", "b.cpp"],
                        },
                        {
                            "file": "core/audio/src/a.cpp",
                            "directory": "/home/runner/build/core/sample_bank_manifest",
                            "command": "c++ -Iconsumer -c a.cpp -oCMakeFiles/pulp-sample-bank-manifest.dir/__/audio/src/a.cpp.o",
                        },
                        {
                            "file": "core/audio/src/a.cpp",
                            "directory": "/home/runner/build/core/audio",
                            "command": "c++ '-I/audio include' -c a.cpp -o 'CMakeFiles/pulp-audio.dir/src/a.cpp.o'",
                        },
                        {"file": "tools/scripts/x.py", "directory": "/tmp/tools", "arguments": ["python"]},
                    ]
                ),
                encoding="utf-8",
            )
            flags = checker.load_module_flags(cc)

        self.assertEqual(sorted(flags), ["core/audio", "core/view"])
        self.assertEqual(flags["core/view"], (["-Iview"], "/tmp/view"))
        self.assertEqual(
            flags["core/audio"],
            (["-I/audio include"], "/home/runner/build/core/audio"),
        )
        self.assertNotIn("tools/scripts", flags)

    def test_module_target_score_prefers_canonical_owner(self) -> None:
        self.assertEqual(
            checker.module_target_score(
                ["c++", "-o", "core/audio/CMakeFiles/pulp-audio.dir/src/a.cpp.o"],
                "core/audio",
            ),
            2,
        )
        self.assertEqual(
            checker.module_target_score(
                ["c++", "-o", "core/audio/CMakeFiles/helper.dir/src/a.cpp.o"],
                "core/audio",
            ),
            1,
        )
        self.assertEqual(
            checker.module_target_score(
                ["c++", "-o", "core/sample_bank_manifest/CMakeFiles/consumer.dir/a.cpp.o"],
                "core/audio",
            ),
            0,
        )

    def test_module_target_score_handles_ci_output_shapes(self) -> None:
        ci_directory = "/home/runner/work/pulp/pulp/build/core/audio"
        self.assertEqual(
            checker.module_target_score(
                ["c++", "-o", "CMakeFiles/pulp-audio.dir/src/a.cpp.o"],
                "core/audio",
                ci_directory,
            ),
            2,
        )
        self.assertEqual(
            checker.module_target_score(
                [
                    "c++",
                    "-I/consumer/CMakeFiles/pulp-audio.dir/fake-include",
                    "-oCMakeFiles/pulp-sample-bank-manifest.dir/a.cpp.o",
                ],
                "core/audio",
                "/home/runner/work/pulp/pulp/build/core/sample_bank_manifest",
            ),
            0,
        )
        self.assertEqual(
            checker.module_target_score(
                ["c++", "-oCMakeFiles/pulp-audio.dir/src/a.cpp.o"],
                "core/audio",
                ci_directory,
            ),
            2,
        )
        self.assertEqual(
            checker.module_target_score(
                ["c++"],
                "core/audio",
                f"{ci_directory}/../sample_bank_manifest",
                "../audio/CMakeFiles/pulp-audio.dir/src/a.cpp.o",
            ),
            2,
        )

    def test_command_split_honors_posix_and_windows_quoting(self) -> None:
        self.assertEqual(
            checker.split_compile_command("c++ '-I/audio include' -c a.cpp", windows=False),
            ["c++", "-I/audio include", "-c", "a.cpp"],
        )
        windows_args = checker.split_compile_command(
                'cl.exe /I"C:\\Program Files\\SDK\\include" /c a.cpp '
                '/Fo"C:\\build output\\core\\audio\\CMakeFiles\\pulp-audio.dir\\a.cpp.obj"',
                windows=True,
            )
        self.assertEqual(
            windows_args,
            [
                "cl.exe",
                "/IC:\\Program Files\\SDK\\include",
                "/c",
                "a.cpp",
                "/FoC:\\build output\\core\\audio\\CMakeFiles\\pulp-audio.dir\\a.cpp.obj",
            ],
        )
        self.assertEqual(checker.module_target_score(windows_args, "core/audio"), 2)
        self.assertEqual(
            checker.module_target_score(
                ["cl.exe", "/FO", "C:\\build\\core\\audio\\CMakeFiles\\pulp-audio.dir\\a.obj"],
                "core/audio",
            ),
            2,
        )
        self.assertEqual(
            checker.filter_args(
                ["cl.exe", "/Iinclude", "/c", "a.cpp", "/FOC:\\build\\A.OBJ"],
                "core/audio/src/a.cpp",
            ),
            ["/Iinclude"],
        )

    def test_collect_headers_supports_lists_changed_and_default_scan(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            public = root / "core/view/include/pulp/view/widget.hpp"
            skipped = root / "core/view/include/pulp/view/generated_gen.hpp"
            private = root / "core/view/src/private.hpp"
            for path in (public, skipped, private):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("// header\n", encoding="utf-8")
            header_list = root / "headers.txt"
            header_list.write_text(
                "\n".join(
                    [
                        "core/view/include/pulp/view/widget.hpp",
                        "core/view/include/pulp/view/generated_gen.hpp",
                        "core/view/src/private.hpp",
                        "README.md",
                    ]
                ),
                encoding="utf-8",
            )

            list_args = types.SimpleNamespace(headers=str(header_list), changed=None)
            self.assertEqual(checker.collect_headers(list_args, root), [public])

            changed_args = types.SimpleNamespace(headers=None, changed="origin/main")
            with mock.patch.object(
                checker.subprocess,
                "run",
                return_value=types.SimpleNamespace(
                    stdout="core/view/include/pulp/view/widget.hpp\nREADME.md\n"
                ),
            ) as run:
                self.assertEqual(checker.collect_headers(changed_args, root), [public])
            run.assert_called_once()

            default_args = types.SimpleNamespace(headers=None, changed=None)
            self.assertEqual(checker.collect_headers(default_args, root), [public])

    def test_check_header_returns_diagnostics_and_removes_temp_tu(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            header = root / "core/view/include/pulp/view/widget.hpp"
            header.parent.mkdir(parents=True)
            header.write_text("// header\n", encoding="utf-8")

            with mock.patch.object(
                checker.subprocess,
                "run",
                return_value=types.SimpleNamespace(returncode=0, stderr="", stdout=""),
            ) as run:
                self.assertIsNone(checker.check_header(header, ["-Iinclude"], td, "clang++"))
            argv = run.call_args.args[0]
            self.assertEqual(argv[:2], ["clang++", "-Iinclude"])
            self.assertIn("-fsyntax-only", argv)
            self.assertEqual(run.call_args.kwargs["cwd"], td)
            self.assertEqual(run.call_args.kwargs["timeout"], 120)
            self.assertFalse(Path(argv[-1]).exists())

            with mock.patch.object(
                checker.subprocess,
                "run",
                return_value=types.SimpleNamespace(returncode=1, stderr="", stdout="stdout diagnostic"),
            ):
                self.assertEqual(checker.check_header(header, [], td, "clang++"), "stdout diagnostic")

    def test_main_reports_setup_and_empty_header_paths(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            missing = Path(td) / "missing.json"
            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), "--compile-commands", str(missing), "--repo-root", td],
            ), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = checker.main()
        self.assertEqual(rc, 2)
        self.assertIn("not found", stderr.getvalue())

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cc = root / "compile_commands.json"
            cc.write_text("[]", encoding="utf-8")
            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), "--compile-commands", str(cc), "--repo-root", str(root)],
            ), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr := io.StringIO()):
                rc = checker.main()
        self.assertEqual(rc, 2)
        self.assertIn("no module flags", stderr.getvalue())

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cc = root / "compile_commands.json"
            cc.write_text(
                json.dumps(
                    [{"file": "core/view/src/view.cpp", "directory": td, "arguments": ["c++", "-c", "view.cpp"]}]
                ),
                encoding="utf-8",
            )
            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), "--compile-commands", str(cc), "--repo-root", str(root)],
            ), contextlib.redirect_stdout(stdout := io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                rc = checker.main()
        self.assertEqual(rc, 0)
        self.assertIn("No public headers", stdout.getvalue())

    def test_main_reports_success_and_failure_summaries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            header = root / "core/view/include/pulp/view/widget.hpp"
            source = root / "core/view/src/view.cpp"
            header.parent.mkdir(parents=True)
            source.parent.mkdir(parents=True)
            header.write_text("// header\n", encoding="utf-8")
            source.write_text("// source\n", encoding="utf-8")
            cc = root / "compile_commands.json"
            cc.write_text(
                json.dumps(
                    [{"file": str(source), "directory": td, "arguments": ["c++", "-Iinclude", "-c", str(source)]}]
                ),
                encoding="utf-8",
            )

            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), "--compile-commands", str(cc), "--repo-root", str(root)],
            ), mock.patch.object(checker, "check_header", return_value=None) as check, \
                 contextlib.redirect_stdout(stdout := io.StringIO()):
                rc = checker.main()
            self.assertEqual(rc, 0)
            self.assertIn("Checked 1 self-contained header", stdout.getvalue())
            self.assertIn("all checked headers", stdout.getvalue())
            check.assert_called_once()

            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), "--compile-commands", str(cc), "--repo-root", str(root)],
            ), mock.patch.object(checker, "check_header", return_value="missing include"), \
                 contextlib.redirect_stdout(stdout := io.StringIO()):
                rc = checker.main()
            self.assertEqual(rc, 1)
            self.assertIn("NOT self-contained", stdout.getvalue())
            self.assertIn("::error file=core/view/include/pulp/view/widget.hpp", stdout.getvalue())
            self.assertIn("missing include", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
