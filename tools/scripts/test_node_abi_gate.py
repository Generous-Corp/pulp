#!/usr/bin/env python3

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
GATE = REPO_ROOT / "tools" / "scripts" / "node_abi_gate.py"


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
