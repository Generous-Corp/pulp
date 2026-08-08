#!/usr/bin/env python3
"""Tests for install-time Mach-O re-signing."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("resign_macos_release_tree.py")
spec = importlib.util.spec_from_file_location("resign_macos_release_tree", SCRIPT)
assert spec and spec.loader
rmrt = importlib.util.module_from_spec(spec)
sys.modules["resign_macos_release_tree"] = rmrt
spec.loader.exec_module(rmrt)


class ResignMacosReleaseTreeTests(unittest.TestCase):
    def test_only_macho_products_are_signed_and_strictly_verified(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "bin").mkdir()
            (root / "lib").mkdir()
            (root / "libexec" / "pulp").mkdir(parents=True)
            binary = root / "bin" / "pulp-cpp"
            broker = root / "libexec" / "pulp" / "pulp-control-broker"
            archive = root / "lib" / "libpulp.a"
            binary.write_bytes(b"macho")
            broker.write_bytes(b"macho")
            archive.write_bytes(b"archive")

            def fake_run(args, **_kwargs):
                if args[0] == "file":
                    kind = (
                        "Mach-O 64-bit executable"
                        if args[1] in {str(binary), str(broker)}
                        else "current ar archive"
                    )
                    return mock.Mock(stdout=kind, returncode=0)
                return mock.Mock(stdout="", returncode=0)

            with mock.patch.object(rmrt.subprocess, "run", side_effect=fake_run) as run:
                self.assertEqual(rmrt.resign(root), [binary, broker])

            commands = [call.args[0] for call in run.call_args_list]
            self.assertIn(["codesign", "--force", "--sign", "-", str(binary)], commands)
            self.assertIn(
                ["codesign", "--verify", "--strict", "--verbose=2", str(binary)],
                commands,
            )
            self.assertIn(
                ["codesign", "--verify", "--strict", "--verbose=2", str(broker)],
                commands,
            )
            self.assertFalse(any(str(archive) in command and command[0] == "codesign" for command in commands))

    def test_negative_control_failed_signature_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "bin").mkdir()
            (root / "lib").mkdir()
            binary = root / "bin" / "pulp-cpp"
            binary.write_bytes(b"macho")

            def fake_run(args, **_kwargs):
                if args[0] == "file":
                    return mock.Mock(stdout="Mach-O 64-bit executable", returncode=0)
                if "--verify" in args:
                    raise subprocess.CalledProcessError(1, args)
                return mock.Mock(stdout="", returncode=0)

            with mock.patch.object(rmrt.subprocess, "run", side_effect=fake_run):
                with self.assertRaises(subprocess.CalledProcessError):
                    rmrt.resign(root)


if __name__ == "__main__":
    unittest.main()
