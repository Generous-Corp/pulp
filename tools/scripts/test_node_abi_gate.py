#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
GATE = REPO_ROOT / "tools" / "scripts" / "node_abi_gate.py"
sys.path.insert(0, str(GATE.parent))

import node_abi_gate as nag  # noqa: E402


def run(cmd: list[str], cwd: Path) -> tuple[int, str]:
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def git(cwd: Path, *args: str) -> None:
    env = os.environ.copy()
    subprocess.run(
        ["git", "-C", str(cwd), *args],
        check=True,
        env=env,
        capture_output=True,
        text=True,
    )


class NodeAbiParserUnitTests(unittest.TestCase):
    def test_repo_root_and_git_show_handle_success_and_failure(self) -> None:
        ok = subprocess.CompletedProcess(["git"], 0, stdout="/tmp/repo\n", stderr="")
        fail = subprocess.CompletedProcess(["git"], 1, stdout="", stderr="fatal")

        with mock.patch.object(subprocess, "run", return_value=ok):
            self.assertEqual(nag.repo_root(), Path("/tmp/repo"))
            self.assertEqual(nag.git_show("origin/main", "foo.hpp"), "/tmp/repo\n")

        with mock.patch.object(subprocess, "run", return_value=fail):
            self.assertIsNone(nag.repo_root())
            self.assertIsNone(nag.git_show("origin/main", "foo.hpp"))

    def test_strip_comments_and_class_body_errors(self) -> None:
        stripped = nag.strip_comments("class A { /* block */ int x; // tail\n};")
        self.assertNotIn("block", stripped)
        self.assertNotIn("tail", stripped)
        nested = nag.class_body("class WithBody { void f() { if (ok) { call(); } } };", "WithBody")
        self.assertIn("call();", nested)

        with self.assertRaisesRegex(ValueError, "class Missing not found"):
            nag.class_body("class Other {};", "Missing")
        with self.assertRaisesRegex(ValueError, "body is unbalanced"):
            nag.class_body("class Broken { public: void x();", "Broken")

    def test_parameter_split_and_default_stripping_respect_nested_syntax(self) -> None:
        params = "std::pair<int, float> value, void (*callback)(int, float), int n = 7"

        self.assertEqual(
            [nag._normalize_param(p) for p in nag._split_params(params)],
            ["std::pair<int,float>", "void(*callback)(int,float)", "int"],
        )
        self.assertEqual(nag._normalize_param("const Foo * value = nullptr"), "const Foo*")

    def test_virtual_order_canonicalizes_names_defaults_and_comments(self) -> None:
        text = textwrap.dedent(
            """\
            class Processor {
            public:
              // ignored comment virtual void fake();
              virtual void prepare(
                  int samples = 128,
                  std::pair<int, float> p = std::pair<int, float>()) const;
              virtual Result render(void (*callback)(int, float)) noexcept = 0;
              virtual malformed;
            };
            """
        )

        decls = nag.virtual_order(text, "Processor")

        self.assertEqual([d.name for d in decls], ["prepare", "render"])
        self.assertEqual(
            [d.signature for d in decls],
            [
                "void prepare(int,std::pair<int,float>)const",
                "Result render(void(*callback)(int,float))noexcept=0",
            ],
        )

    def test_matching_paren_reports_unbalanced_declarations(self) -> None:
        with self.assertRaisesRegex(ValueError, "unbalanced parentheses"):
            nag._canonical_virtual_signature("virtual void broken(int value")
        with self.assertRaisesRegex(ValueError, "missing method name"):
            nag._canonical_virtual_signature("virtual (int value)")

    def test_render_mismatch_reports_signature_changes_and_removals(self) -> None:
        old = [
            nag.VirtualDecl("a", "void a()"),
            nag.VirtualDecl("b", "void b(int)"),
        ]
        changed = [nag.VirtualDecl("a", "void a()"), nag.VirtualDecl("b", "void b(float)")]
        removed = [nag.VirtualDecl("a", "void a()")]
        appended = old + [nag.VirtualDecl("c", "void c()")]

        changed_body = nag.render_mismatch("Processor", old, changed)
        removed_body = nag.render_mismatch("Processor", old, removed)
        appended_body = nag.render_mismatch("Processor", old, appended)

        self.assertIn("first mismatch at index 1", changed_body)
        self.assertIn("void b(float)", changed_body)
        self.assertIn("<missing>", removed_body)
        self.assertIn("removed virtual method", removed_body)
        self.assertNotIn("first mismatch", appended_body)
        self.assertNotIn("removed virtual method", appended_body)


class NodeAbiSurfaceUnitTests(unittest.TestCase):
    def test_check_surface_reports_missing_base_and_current_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            with mock.patch.object(nag, "git_show", return_value=None):
                self.assertIn(
                    "could not read",
                    nag.check_surface(root, "origin/main", "Processor", "processor.hpp") or "",
                )

            with mock.patch.object(nag, "git_show", return_value="class Processor {};"):
                self.assertIn(
                    "current file missing",
                    nag.check_surface(root, "origin/main", "Processor", "processor.hpp") or "",
                )

    def test_check_surface_allows_append_and_rejects_reorder(self) -> None:
        base = textwrap.dedent(
            """\
            class Processor {
            public:
              virtual ~Processor() = default;
              virtual void descriptor();
            };
            """
        )
        appended = textwrap.dedent(
            """\
            class Processor {
            public:
              virtual ~Processor() = default;
              virtual void descriptor();
              virtual void release();
            };
            """
        )
        reordered = textwrap.dedent(
            """\
            class Processor {
            public:
              virtual void descriptor();
              virtual ~Processor() = default;
            };
            """
        )
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            current = root / "processor.hpp"
            current.write_text(appended)
            with mock.patch.object(nag, "git_show", return_value=base):
                self.assertIsNone(nag.check_surface(root, "origin/main", "Processor", "processor.hpp"))

            current.write_text(reordered)
            with mock.patch.object(nag, "git_show", return_value=base):
                self.assertIn(
                    "virtual order is not additive-only",
                    nag.check_surface(root, "origin/main", "Processor", "processor.hpp") or "",
                )

    def test_check_surface_reports_parse_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "processor.hpp").write_text("class Processor {")
            with mock.patch.object(nag, "git_show", return_value="class Processor {};"):
                self.assertIn(
                    "body is unbalanced",
                    nag.check_surface(root, "origin/main", "Processor", "processor.hpp") or "",
                )


class NodeAbiMainUnitTests(unittest.TestCase):
    def test_main_returns_mode_specific_code_outside_repo(self) -> None:
        stderr = io.StringIO()
        with mock.patch.object(nag, "repo_root", return_value=None), \
             contextlib.redirect_stderr(stderr):
            self.assertEqual(nag.main(["--mode", "hint"]), 0)
            self.assertEqual(nag.main(["--mode", "report"]), 2)

    def test_main_reports_success_when_all_surfaces_pass(self) -> None:
        stdout = io.StringIO()
        with mock.patch.object(nag, "repo_root", return_value=Path("/repo")), \
             mock.patch.object(nag, "check_surface", return_value=None), \
             contextlib.redirect_stdout(stdout):
            self.assertEqual(nag.main([]), 0)

        self.assertIn("additive-only", stdout.getvalue())

    def test_main_reports_findings_and_hint_mode_stays_green(self) -> None:
        stderr = io.StringIO()
        with mock.patch.object(nag, "repo_root", return_value=Path("/repo")), \
             mock.patch.object(nag, "check_surface", side_effect=["Processor: bad", None]), \
             contextlib.redirect_stderr(stderr):
            self.assertEqual(nag.main(["--mode", "report"]), 1)
        self.assertIn("Processor: bad", stderr.getvalue())

        with mock.patch.object(nag, "repo_root", return_value=Path("/repo")), \
             mock.patch.object(nag, "check_surface", side_effect=["Processor: bad", None]), \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(nag.main(["--mode", "hint"]), 0)

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        proc = subprocess.CompletedProcess(["git"], 1, stdout="", stderr="fatal")
        argv = [str(GATE), "--mode", "report"]

        with mock.patch.object(sys, "argv", argv), \
             mock.patch.object(subprocess, "run", return_value=proc), \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(GATE), run_name="__main__")

        self.assertEqual(cm.exception.code, 2)


class NodeAbiGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-node-abi-gate-"))
        git(self.tmp, "init", "-q", "-b", "main")
        git(self.tmp, "config", "user.email", "test@example.com")
        git(self.tmp, "config", "user.name", "Test")
        git(self.tmp, "config", "commit.gpgsign", "false")
        self.processor = (
            self.tmp / "core" / "format" / "include" / "pulp" / "format" / "processor.hpp"
        )
        self.plugin_slot = (
            self.tmp / "core" / "host" / "include" / "pulp" / "host" / "plugin_slot.hpp"
        )
        self.write_headers(["~Processor", "descriptor", "prepare"],
                           ["~PluginSlot", "info", "process"])
        git(self.tmp, "add", ".")
        git(self.tmp, "commit", "-q", "-m", "initial")
        git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_headers(self, processor_methods: list[str], slot_methods: list[str]) -> None:
        self.processor.parent.mkdir(parents=True, exist_ok=True)
        self.plugin_slot.parent.mkdir(parents=True, exist_ok=True)
        self.processor.write_text(self.header("Processor", processor_methods))
        self.plugin_slot.write_text(self.header("PluginSlot", slot_methods))

    @staticmethod
    def header(class_name: str, methods: list[str]) -> str:
        decls: list[str] = []
        for method in methods:
            if method.startswith("virtual "):
                decls.append(f"    {method}")
            elif method.startswith("~"):
                decls.append(f"    virtual {method}() = default;")
            else:
                decls.append(f"    virtual void {method}() {{}}")
        return textwrap.dedent(f"""\
            #pragma once
            class {class_name} {{
            public:
            {chr(10).join(decls)}
            }};
            """)

    def gate(self) -> tuple[int, str]:
        return run(["python3", str(GATE), "--base", "origin/main"], self.tmp)

    def test_appended_virtuals_pass(self) -> None:
        self.write_headers(["~Processor", "descriptor", "prepare", "release"],
                           ["~PluginSlot", "info", "process", "parameters"])
        code, out = self.gate()
        self.assertEqual(code, 0, out)

    def test_inserted_virtual_fails(self) -> None:
        self.write_headers(["~Processor", "descriptor", "inserted", "prepare"],
                           ["~PluginSlot", "info", "process"])
        code, out = self.gate()
        self.assertEqual(code, 1)
        self.assertIn("Processor: virtual order is not additive-only", out)

    def test_removed_virtual_fails(self) -> None:
        self.write_headers(["~Processor", "descriptor"],
                           ["~PluginSlot", "info", "process"])
        code, out = self.gate()
        self.assertEqual(code, 1)
        self.assertIn("current order removed virtual method", out)

    def test_signature_change_fails(self) -> None:
        self.write_headers(
            ["~Processor", "descriptor", "virtual void prepare(double samples) {}"],
            ["~PluginSlot", "info", "process"],
        )
        code, out = self.gate()
        self.assertEqual(code, 1)
        self.assertIn("do not re-signature existing virtuals", out)
        self.assertIn("void prepare()", out)
        self.assertIn("void prepare(double)", out)

    def test_parameter_name_only_change_passes(self) -> None:
        self.write_headers(
            ["~Processor", "descriptor", "virtual void prepare(int samples) {}"],
            ["~PluginSlot", "info", "process"],
        )
        git(self.tmp, "add", ".")
        git(self.tmp, "commit", "-q", "-m", "named parameter baseline")
        git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")

        self.write_headers(
            ["~Processor", "descriptor", "virtual void prepare(int block_size) {}"],
            ["~PluginSlot", "info", "process"],
        )
        code, out = self.gate()
        self.assertEqual(code, 0, out)


if __name__ == "__main__":
    unittest.main()
