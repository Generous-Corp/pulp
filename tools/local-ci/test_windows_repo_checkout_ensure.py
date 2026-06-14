#!/usr/bin/env python3
"""Tests for Windows remote repository checkout materialization helper."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_repo_checkout_ensure.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsRepoCheckoutEnsureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_ensure_windows_remote_repo_checkout_materializes_safe_fallback_path(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout=(
                    '{"home_dir":"C:\\\\Users\\\\dev",'
                    '"repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate",'
                    '"head_exists":true,"setup_exists":true}\n'
                )
            )

        def fake_unsafe(repo_path, home_dir):
            return repo_path == r"C:\Users\dev"

        ensured = self.mod.ensure_windows_remote_repo_checkout(
            "win",
            r"C:\Users\dev",
            remote_url="https://example.invalid/pulp.git",
            bundle_name="bundle.git",
            bundle_ref="refs/pulp/source",
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {
                "home_dir": r"C:\Users\dev",
                "repo_path": r"C:\Users\dev",
                "head_exists": False,
                "setup_exists": False,
            },
            windows_repo_path_is_unsafe_fn=fake_unsafe,
            windows_default_repo_checkout_path_fn=lambda home: home + r"\pulp-validate",
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertEqual(ensured["repo_path"], r"C:\Users\dev\pulp-validate")
        self.assertIn("& git -C $Repo fetch $BundlePath", scripts[0]["script"])
        self.assertIn(r"C:\Users\dev\pulp-validate", scripts[0]["script"])
        self.assertEqual(scripts[0]["timeout"], 120)

    def test_ensure_windows_remote_repo_checkout_rejects_unstructured_probe(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "repo probe returned no structured payload"):
            self.mod.ensure_windows_remote_repo_checkout(
                "win",
                r"C:\Pulp",
                remote_url=None,
                bundle_name=None,
                bundle_ref=None,
                probe_windows_repo_checkout_fn=lambda _host, _repo_path: None,
                windows_repo_path_is_unsafe_fn=lambda _repo, _home: False,
                windows_default_repo_checkout_path_fn=lambda _home: r"C:\Pulp",
                run_windows_ssh_powershell_fn=lambda *_args, **_kwargs: completed(),
            )


if __name__ == "__main__":
    unittest.main()
