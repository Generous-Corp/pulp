#!/usr/bin/env python3
"""Tests for Windows remote repository checkout helpers."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("windows_repo_checkout.py", add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsRepoCheckoutTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_windows_repo_checkout_uses_injected_runner_and_safety_policy(self) -> None:
        scripts: list[dict] = []
        unsafe_calls: list[tuple[str | None, str | None]] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout='{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Pulp","head_exists":true,"setup_exists":true}\n'
            )

        def fake_unsafe(repo_path, home_dir):
            unsafe_calls.append((repo_path, home_dir))
            return repo_path == r"C:\Users\dev"

        probe = self.mod.probe_windows_repo_checkout(
            "win",
            "Owner's Repo",
            run_windows_ssh_powershell_fn=fake_run,
            windows_repo_path_is_unsafe_fn=fake_unsafe,
        )

        self.assertFalse(probe["repo_path_unsafe"])
        self.assertIn("$RepoRaw = 'Owner''s Repo'", scripts[0]["script"])
        self.assertIn("git -C $Repo remote 2>$null", scripts[0]["script"])
        self.assertIn("Where-Object { $_ -eq 'origin' }", scripts[0]["script"])
        self.assertIn("git -C $Repo rev-parse --verify --quiet HEAD 2>$null", scripts[0]["script"])
        self.assertEqual(scripts[0]["timeout"], 60)
        self.assertEqual(unsafe_calls, [(r"C:\Pulp", r"C:\Users\dev")])

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

    def test_ensure_windows_remote_repo_checkout_adds_origin_only_when_missing(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout=(
                    '{"home_dir":"C:\\\\Users\\\\dev",'
                    '"repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate",'
                    '"head_exists":true,'
                    '"setup_exists":true,'
                    '"origin_url":"https://github.com/danielraffel/pulp"}\n'
                )
            )

        result = self.mod.ensure_windows_remote_repo_checkout(
            "win",
            r"C:\Users\dev\pulp-validate",
            remote_url="https://github.com/danielraffel/pulp",
            bundle_name=None,
            bundle_ref=None,
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {
                "home_dir": r"C:\Users\dev",
                "repo_path": r"C:\Users\dev\pulp-validate",
                "head_exists": True,
                "setup_exists": True,
                "origin_url": "",
            },
            windows_repo_path_is_unsafe_fn=lambda _repo, _home: False,
            windows_default_repo_checkout_path_fn=lambda home: home + r"\pulp-validate",
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertEqual(result["origin_url"], "https://github.com/danielraffel/pulp")
        self.assertIn("git -C $Repo remote 2>$null", scripts[0]["script"])
        self.assertIn("if (-not $OriginUrl -and $RemoteUrl)", scripts[0]["script"])
        self.assertIn("$NeedsMaterialize = $false", scripts[0]["script"])

    def test_ensure_windows_remote_repo_checkout_materializes_incomplete_checkout_from_origin(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout=(
                    '{"home_dir":"C:\\\\Users\\\\dev",'
                    '"repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate",'
                    '"head":"abc123",'
                    '"head_exists":true,'
                    '"setup_path":"C:\\\\Users\\\\dev\\\\pulp-validate\\\\setup.sh",'
                    '"setup_exists":true,'
                    '"origin_url":"https://github.com/danielraffel/pulp"}\n'
                )
            )

        result = self.mod.ensure_windows_remote_repo_checkout(
            "win",
            r"C:\Users\dev\pulp-validate",
            remote_url="https://github.com/danielraffel/pulp",
            bundle_name=None,
            bundle_ref=None,
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {
                "home_dir": r"C:\Users\dev",
                "repo_path": r"C:\Users\dev\pulp-validate",
                "head_exists": True,
                "setup_exists": False,
                "origin_url": "https://github.com/danielraffel/pulp",
            },
            windows_repo_path_is_unsafe_fn=lambda _repo, _home: False,
            windows_default_repo_checkout_path_fn=lambda home: home + r"\pulp-validate",
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertTrue(result["setup_exists"])
        self.assertEqual(result["origin_url"], "https://github.com/danielraffel/pulp")
        self.assertIn("$NeedsMaterialize = $true", scripts[0]["script"])
        self.assertIn("[Environment]::SetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', '1', 'Process')", scripts[0]["script"])
        self.assertIn("git -C $Repo fetch --depth 1 origin main", scripts[0]["script"])
        self.assertIn("git -C $Repo checkout --force -B main FETCH_HEAD", scripts[0]["script"])

    def test_ensure_windows_remote_repo_checkout_prefers_bundle_materialization(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout=(
                    '{"home_dir":"C:\\\\Users\\\\dev",'
                    '"repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate",'
                    '"head":"abc123",'
                    '"head_exists":true,'
                    '"setup_path":"C:\\\\Users\\\\dev\\\\pulp-validate\\\\setup.sh",'
                    '"setup_exists":true,'
                    '"origin_url":"https://github.com/danielraffel/pulp"}\n'
                )
            )

        result = self.mod.ensure_windows_remote_repo_checkout(
            "win",
            r"C:\Users\dev\pulp-validate",
            remote_url="https://github.com/danielraffel/pulp",
            bundle_name="pulp-ci-123.bundle",
            bundle_ref="refs/pulp-ci-bundles/123",
            probe_windows_repo_checkout_fn=lambda _host, _repo_path: {
                "home_dir": r"C:\Users\dev",
                "repo_path": r"C:\Users\dev\pulp-validate",
                "head_exists": False,
                "setup_exists": False,
                "origin_url": "https://github.com/danielraffel/pulp",
            },
            windows_repo_path_is_unsafe_fn=lambda _repo, _home: False,
            windows_default_repo_checkout_path_fn=lambda home: home + r"\pulp-validate",
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertTrue(result["setup_exists"])
        self.assertIn("$Bundle = 'pulp-ci-123.bundle'", scripts[0]["script"])
        self.assertIn("$BundleRef = 'refs/pulp-ci-bundles/123'", scripts[0]["script"])
        self.assertIn("[Environment]::SetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', '1', 'Process')", scripts[0]["script"])
        self.assertIn("git -C $Repo fetch $BundlePath \"$BundleRef`:refs/pulp-ci-bundles/source\"", scripts[0]["script"])

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
