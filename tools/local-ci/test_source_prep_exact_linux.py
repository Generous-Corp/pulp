#!/usr/bin/env python3
"""No-network tests for Linux exact-SHA source materialization."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("source_prep_exact_linux.py", add_module_dir=True)


class SourcePrepExactLinuxTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def request(self, **overrides) -> dict:
        request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "a" * 40,
            "prepare_command": None,
            "prepare_timeout_secs": 120.0,
        }
        request.update(overrides)
        return request

    def test_prepare_linux_exact_sha_source_builds_remote_script_and_context(self) -> None:
        bundle_dir = self.root / "bundle-linux"
        bundle_dir.mkdir()
        source_request = self.request(prepare_command="./scripts/build-ui.sh")
        run_calls = []
        fetched = []

        def fake_run(command, **kwargs):
            run_calls.append((command, kwargs))
            if len(run_calls) == 1:
                return subprocess.CompletedProcess(command, 0, stdout="/home/dev\n", stderr="")
            return subprocess.CompletedProcess(command, 0, stdout="__PULP_PREPARED__:reused\n", stderr="")

        context = self.mod.prepare_linux_exact_sha_source(
            bundle_dir,
            "ubuntu",
            "host",
            "./build/ui-preview --flag",
            source_request,
            sync_job_bundle_to_ssh_host_fn=lambda host, job: ("source.bundle", "refs/pulp-ci-bundles/test"),
            git_origin_clone_url_fn=lambda root: "https://example.invalid/pulp.git",
            desktop_source_cache_key_fn=lambda request: "cache-key",
            root=self.repo,
            run_fn=fake_run,
            fetch_ssh_artifact_fn=lambda *args, **kwargs: fetched.append((args, kwargs)) or True,
            rewrite_launch_command_for_posix_root_fn=lambda command, root: f"{root}:{command}",
        )

        remote_script = run_calls[1][0][-1]
        self.assertIn('export PATH="$HOME/.local/bin:$PATH"', remote_script)
        self.assertIn("export GIT_LFS_SKIP_SMUDGE=1", remote_script)
        self.assertIn("export PULP_REQUIRE_PREPARE_STAMP=1", remote_script)
        self.assertIn("bundle_ref=refs/pulp-ci-bundles/test", remote_script)
        self.assertIn('git -C "$prepared_root" fetch "$bundle" "$bundle_ref:refs/pulp-ci-bundles/source"', remote_script)
        self.assertIn('git -C "$prepared_root" remote add origin "$remote_url"', remote_script)
        self.assertEqual(context["prepared_state"], "reused")
        self.assertEqual(context["prepared_root"], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(context["prepared_root_display"], "~/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(context["launch_command"], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key:./build/ui-preview --flag")
        self.assertEqual(fetched[0][0][1], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key/prepare.log")


if __name__ == "__main__":
    unittest.main()
