#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fleet_lib as F  # noqa: E402


def executable(path: Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class RunnerPolicyFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="fleet-runner-policy-")
        self.home = Path(self.temp.name)
        self.runner = self.home / "actions-ci" / "Shipyard-test-01"
        self.runner.mkdir(parents=True)
        self.key = {
            "name": "persistent_runner_policy",
            "kind": "actions_runner_policy",
            "runner_globs": [str(self.home / "actions-ci" / "*")],
            "runner_version": "2.335.1",
            "runner_sha256": "e1a9bc7a3661e06fa0b129d15c2064fe65dc81a431001d8958a9db1409b73769",
            "require_internal_apfs": False,
            "apply": "runner_harden",
            "value": "system-first-private-toolchain-v1",
            "verified": True,
        }
        self._write_runner(disable_update=True)
        self.addCleanup(self.temp.cleanup)

    def _write_runner(self, *, disable_update: bool) -> None:
        (self.runner / ".runner").write_text(
            json.dumps({
                "agentName": "Shipyard-test-01",
                "gitHubUrl": "https://github.com/danielraffel/Shipyard",
                "disableUpdate": disable_update,
            }),
            encoding="utf-8",
        )
        (self.runner / ".runner_migrated").write_text(
            json.dumps({
                "agentName": "Shipyard-test-01",
                "gitHubUrl": "https://github.com/danielraffel/Shipyard",
                "disableUpdate": disable_update,
            }),
            encoding="utf-8",
        )
        (self.runner / ".fleet-runner-package.json").write_text(
            json.dumps({
                "runner_sha256": self.key["runner_sha256"],
                "runner_version": self.key["runner_version"],
            }),
            encoding="utf-8",
        )
        executable(
            self.runner / "bin" / "Runner.Listener",
            "#!/bin/sh\nprintf '2.335.1\\n'\n",
        )
        executable(self.runner / "externals/node20/bin/node", "#!/bin/sh\nexit 0\n")
        executable(self.runner / "config.sh", "#!/bin/sh\nexit 0\n")
        executable(self.runner / "run.sh", "#!/bin/sh\nexit 0\n")
        rustup_home, cargo_home = F._runner_private_paths(self.runner)
        rustup_home.mkdir(parents=True, exist_ok=True)
        cargo_home.mkdir(parents=True, exist_ok=True)
        for name in ("cargo", "rustup"):
            executable(cargo_home / "bin" / name, f"#!/bin/sh\nprintf '{name} ok\\n'\n")
        (self.runner / ".env").write_text(
            f"CCACHE_DIR={self.home}/cache\n"
            f"RUSTUP_HOME={rustup_home}\n"
            f"CARGO_HOME={cargo_home}\n",
            encoding="utf-8",
        )
        (self.runner / ".path").write_text(
            F._runner_path_value(self.runner) + "\n", encoding="utf-8"
        )
        executable(
            self.runner / "svc.sh",
            "#!/bin/sh\nprintf '%s\\n' \"$1\" > service-state\n",
        )

    def test_compliant_policy_checks_version_path_private_tools_and_listener(self) -> None:
        with mock.patch.object(F, "_runner_process_present", return_value=True):
            result = F.probe_actions_runner_policy(self.key)
        self.assertEqual(result.state, F.OK, result.detail)
        self.assertIn("1 configured runner", result.detail)

    def test_wrong_path_update_policy_and_symlinked_toolchain_are_drift(self) -> None:
        self._write_runner(disable_update=False)
        (self.runner / ".path").write_text("/opt/homebrew/bin:/usr/bin\n", encoding="utf-8")
        rustup_home, _ = F._runner_private_paths(self.runner)
        rustup_home.rmdir()
        rustup_home.symlink_to(self.home / "shared-rustup", target_is_directory=True)
        with mock.patch.object(F, "_runner_process_present", return_value=True):
            result = F.probe_actions_runner_policy(self.key)
        self.assertEqual(result.state, F.DRIFT)
        self.assertIn("disableUpdate", result.detail)
        self.assertIn("system-first", result.detail)
        self.assertIn("symlinked", result.detail)

    def test_apply_is_idle_gated_and_recovers_an_offline_listener(self) -> None:
        self._write_runner(disable_update=False)
        (self.runner / ".path").unlink()
        (self.runner / ".env").write_text("CCACHE_DIR=/keep/me\n", encoding="utf-8")

        def process_present(runner_dir: Path, process_name: str):
            if process_name == "Runner.Worker":
                return False
            return (runner_dir / "service-state").read_text().strip() == "start" \
                if (runner_dir / "service-state").exists() else False

        def ensure_before_publish(runner_dir: Path) -> None:
            self.assertEqual((runner_dir / ".env").read_text(), "CCACHE_DIR=/keep/me\n")
            self.assertFalse((runner_dir / ".path").exists())

        before = F.Probe(F.DRIFT, "bad")
        with mock.patch.object(F, "_runner_process_present", side_effect=process_present), \
             mock.patch.object(F, "_ensure_runner_rust", side_effect=ensure_before_publish):
            outcome, detail = F.apply_actions_runner_policy(self.key, before)
        self.assertEqual(outcome, "fixed", detail)
        config = json.loads((self.runner / ".runner").read_text(encoding="utf-8"))
        self.assertIs(config["disableUpdate"], True)
        migrated = json.loads(
            (self.runner / ".runner_migrated").read_text(encoding="utf-8")
        )
        self.assertIs(migrated["disableUpdate"], True)
        self.assertTrue((self.runner / ".path").read_text().startswith(F.SYSTEM_PATH_PREFIX))
        env = F._read_env_file(self.runner / ".env")
        self.assertEqual(env["CCACHE_DIR"], "/keep/me")
        self.assertEqual(env["RUSTUP_HOME"], str(self.runner / "_toolcache/rustup"))
        self.assertEqual(env["CARGO_HOME"], str(self.runner / "_toolcache/cargo"))
        self.assertEqual((self.runner / "service-state").read_text().strip(), "start")

    def test_apply_refuses_to_touch_an_active_worker(self) -> None:
        original = (self.runner / ".runner").read_bytes()
        with mock.patch.object(F, "_runner_process_present", return_value=True):
            outcome, detail = F.apply_actions_runner_policy(self.key, F.Probe(F.DRIFT))
        self.assertEqual(outcome, "manual")
        self.assertIn("active Runner.Worker", detail)
        self.assertEqual((self.runner / ".runner").read_bytes(), original)

    def test_apply_restores_both_runtime_configs_when_second_publish_fails(self) -> None:
        self._write_runner(disable_update=False)
        originals = {
            path: path.read_bytes()
            for path in (self.runner / ".runner", self.runner / ".runner_migrated")
        }
        real_atomic_write = F._atomic_write

        def fail_migrated(path, text):
            if Path(path).name == ".runner_migrated":
                raise OSError("injected second-config failure")
            return real_atomic_write(path, text)

        def process_present(_runner_dir: Path, process_name: str):
            return False if process_name in ("Runner.Worker", "Runner.Listener") else None

        with mock.patch.object(F, "_runner_process_present", side_effect=process_present), \
             mock.patch.object(F, "_ensure_runner_rust"), \
             mock.patch.object(F, "_atomic_write", side_effect=fail_migrated):
            outcome, detail = F.apply_actions_runner_policy(self.key, F.Probe(F.DRIFT))
        self.assertEqual(outcome, "failed")
        self.assertIn("injected second-config failure", detail)
        self.assertEqual(
            {
                path: path.read_bytes()
                for path in (self.runner / ".runner", self.runner / ".runner_migrated")
            },
            originals,
        )

    def test_missing_package_marker_forces_reinstall_even_when_listener_version_matches(self) -> None:
        (self.runner / ".fleet-runner-package.json").unlink()

        def process_present(runner_dir: Path, process_name: str):
            if process_name == "Runner.Worker":
                return False
            state = runner_dir / "service-state"
            return state.exists() and state.read_text().strip() == "start"

        with mock.patch.object(F, "_runner_process_present", side_effect=process_present), \
             mock.patch.object(F, "_install_runner_package", return_value=None) as install, \
             mock.patch.object(F, "_ensure_runner_rust"):
            outcome, detail = F.apply_actions_runner_policy(self.key, F.Probe(F.DRIFT))
        self.assertEqual(outcome, "fixed", detail)
        install.assert_called_once_with(
            self.runner, self.key["runner_version"], self.key["runner_sha256"],
        )
        self.assertTrue(F._runner_package_marker_matches(
            self.runner, self.key["runner_version"], self.key["runner_sha256"],
        ))

    def test_migrated_runtime_config_without_update_pin_is_drift(self) -> None:
        migrated_path = self.runner / ".runner_migrated"
        migrated = json.loads(migrated_path.read_text(encoding="utf-8"))
        migrated.pop("disableUpdate")
        migrated_path.write_text(json.dumps(migrated), encoding="utf-8")
        with mock.patch.object(F, "_runner_process_present", return_value=True):
            result = F.probe_actions_runner_policy(self.key)
        self.assertEqual(result.state, F.DRIFT)
        self.assertIn(".runner_migrated disableUpdate", result.detail)

    def test_no_configured_runners_is_observed_and_compliant(self) -> None:
        self.runner.rename(self.runner.with_name("not-a-runner"))
        (self.runner.with_name("not-a-runner") / ".runner").unlink()
        result = F.probe_actions_runner_policy(self.key)
        self.assertEqual(result.state, F.OK, result.detail)
        self.assertIn("0 configured runner", result.detail)

    def test_unreadable_fixed_glob_parent_is_unobservable_not_zero_runners(self) -> None:
        fixed_parent = self.home / "actions-ci"

        def access(path: Path, mode: int) -> bool:
            return Path(path) != fixed_parent

        with mock.patch.object(F.os, "access", side_effect=access):
            result = F.probe_actions_runner_policy(self.key)
        self.assertEqual(result.state, F.UNOBS)
        self.assertIn(str(fixed_parent), result.detail)

    def test_sha256_file_streams_the_expected_digest(self) -> None:
        sample = self.home / "sample"
        sample.write_bytes(b"known bytes")
        self.assertEqual(
            F._sha256_file(sample),
            "25cb6d61356e5cada4238d160f3a77522e550e27a69758da40cd281c7ef2c8dc",
        )

    def test_runner_archive_allows_relative_link_that_stays_inside_root(self) -> None:
        member = tarfile.TarInfo("bin/node20/bin/corepack")
        member.type = tarfile.SYMTYPE
        member.linkname = "../lib/node_modules/corepack/dist/corepack.js"
        self.assertFalse(F._archive_link_escapes(member))

    def test_runner_archive_relative_link_escape_is_detectable(self) -> None:
        member = tarfile.TarInfo("bin/tool")
        member.type = tarfile.SYMTYPE
        member.linkname = "../../outside"
        self.assertTrue(F._archive_link_escapes(member))

    def test_runner_archive_hardlink_target_is_archive_root_relative(self) -> None:
        symlink = tarfile.TarInfo("dir/link")
        symlink.type = tarfile.SYMTYPE
        symlink.linkname = "../inside"
        self.assertFalse(F._archive_link_escapes(symlink))

        hardlink = tarfile.TarInfo("dir/link")
        hardlink.type = tarfile.LNKTYPE
        hardlink.linkname = "../inside"
        self.assertTrue(F._archive_link_escapes(hardlink))

    def test_runner_archive_allows_root_relative_hardlink_inside_root(self) -> None:
        member = tarfile.TarInfo("dir/link")
        member.type = tarfile.LNKTYPE
        member.linkname = "inside/target"
        self.assertFalse(F._archive_link_escapes(member))

    def test_runner_archive_rejects_member_under_symlink_ancestor(self) -> None:
        ancestor = tarfile.TarInfo("a")
        ancestor.type = tarfile.SYMTYPE
        ancestor.linkname = "."
        nested = tarfile.TarInfo("a/b/link")
        nested.type = tarfile.SYMTYPE
        nested.linkname = "../../outside"
        with self.assertRaisesRegex(RuntimeError, "symlink ancestor"):
            F._validate_runner_archive_members([ancestor, nested])

    def test_unversioned_runner_layout_rolls_back_every_package_entry(self) -> None:
        old_listener = (self.runner / "bin/Runner.Listener").read_bytes()
        old_run = (self.runner / "run.sh").read_bytes()
        archive = self.home / "unversioned-runner.tar.gz"
        source = self.home / "unversioned-package"
        executable(source / "bin/Runner.Listener", "#!/bin/sh\nprintf 'new\n'\n")
        executable(source / "externals/node20/bin/node", "#!/bin/sh\nprintf 'new-node\n'\n")
        executable(source / "run.sh", "#!/bin/sh\nprintf 'new-root\n'\n")
        with tarfile.open(archive, "w:gz") as tf:
            for path in source.iterdir():
                tf.add(path, arcname=path.name)
        with tarfile.open(archive, "r:gz") as tf:
            publication = F._extract_runner_package(tf, self.runner, "2.335.1")
        self.assertEqual(
            subprocess.check_output([self.runner / "bin/Runner.Listener"], text=True).strip(),
            "new",
        )
        publication.rollback()
        self.assertEqual((self.runner / "bin/Runner.Listener").read_bytes(), old_listener)
        self.assertEqual((self.runner / "run.sh").read_bytes(), old_run)

    def test_versioned_runner_layout_switches_links_without_writing_old_tree(self) -> None:
        executable(self.runner / "run.sh", "#!/bin/sh\nprintf 'old-root\n'\n")
        for name in ("bin", "externals"):
            old = self.runner / f"{name}.2.336.0"
            old.mkdir()
            (old / "sentinel").write_text("old", encoding="utf-8")
            plain = self.runner / name
            if plain.exists():
                shutil.rmtree(plain)
            plain.symlink_to(old, target_is_directory=True)

        archive = self.home / "runner.tar.gz"
        source = self.home / "package"
        executable(source / "bin" / "Runner.Listener", "#!/bin/sh\nprintf '2.335.1\\n'\n")
        executable(source / "externals" / "node", "#!/bin/sh\nexit 0\n")
        executable(source / "run.sh", "#!/bin/sh\nprintf 'new-root\n'\n")
        with tarfile.open(archive, "w:gz") as tf:
            tf.add(source / "bin", arcname="bin")
            tf.add(source / "externals", arcname="externals")
            tf.add(source / "run.sh", arcname="run.sh")

        with tarfile.open(archive, "r:gz") as tf:
            publication = F._extract_runner_package(tf, self.runner, "2.335.1")

        self.assertEqual((self.runner / "bin.2.336.0/sentinel").read_text(), "old")
        self.assertEqual((self.runner / "externals.2.336.0/sentinel").read_text(), "old")
        self.assertEqual(
            (self.runner / "bin").resolve(), (self.runner / "bin.2.335.1").resolve()
        )
        self.assertEqual(
            (self.runner / "externals").resolve(),
            (self.runner / "externals.2.335.1").resolve(),
        )
        self.assertEqual(
            subprocess.check_output([self.runner / "bin/Runner.Listener"], text=True).strip(),
            "2.335.1",
        )
        self.assertEqual(
            subprocess.check_output([self.runner / "run.sh"], text=True).strip(),
            "new-root",
        )
        publication.commit()

    def test_versioned_runner_layout_rollback_restores_links_and_replaced_trees(self) -> None:
        executable(self.runner / "run.sh", "#!/bin/sh\nprintf 'old-root\n'\n")
        for name in ("bin", "externals"):
            old = self.runner / f"{name}.2.336.0"
            old.mkdir()
            (old / "sentinel").write_text("live-old", encoding="utf-8")
            replaced = self.runner / f"{name}.2.335.1"
            replaced.mkdir()
            (replaced / "sentinel").write_text("preexisting", encoding="utf-8")
            plain = self.runner / name
            if plain.exists():
                shutil.rmtree(plain)
            plain.symlink_to(old, target_is_directory=True)

        archive = self.home / "runner-rollback.tar.gz"
        source = self.home / "rollback-package"
        executable(source / "bin" / "Runner.Listener", "#!/bin/sh\nprintf 'new\n'\n")
        executable(source / "externals" / "node", "#!/bin/sh\nexit 0\n")
        executable(source / "run.sh", "#!/bin/sh\nprintf 'new-root\n'\n")
        with tarfile.open(archive, "w:gz") as tf:
            tf.add(source / "bin", arcname="bin")
            tf.add(source / "externals", arcname="externals")
            tf.add(source / "run.sh", arcname="run.sh")

        with tarfile.open(archive, "r:gz") as tf:
            publication = F._extract_runner_package(tf, self.runner, "2.335.1")
        publication.rollback()

        for name in ("bin", "externals"):
            self.assertEqual(
                (self.runner / name).resolve(),
                (self.runner / f"{name}.2.336.0").resolve(),
            )
            self.assertEqual(
                (self.runner / f"{name}.2.335.1/sentinel").read_text(),
                "preexisting",
            )
            self.assertFalse(list(self.runner.glob(f".{name}.*.fleet-backup-*")))
        self.assertEqual(
            subprocess.check_output([self.runner / "run.sh"], text=True).strip(),
            "old-root",
        )

    def test_package_rollback_restores_backup_before_new_tree_publishes(self) -> None:
        old = self.runner / "bin.old"
        target = self.runner / "bin.new"
        backup = self.runner / ".bin.new.fleet-backup-test"
        old.mkdir()
        backup.mkdir()
        (backup / "sentinel").write_text("preexisting", encoding="utf-8")
        plain = self.runner / "bin"
        shutil.rmtree(plain)
        plain.symlink_to(old, target_is_directory=True)
        publication = F._RunnerPackagePublication(
            self.runner, {"bin": old}, {"bin": target}, {"bin": backup}, set()
        )
        publication.rollback()
        self.assertEqual(plain.resolve(), old.resolve())
        self.assertEqual((target / "sentinel").read_text(), "preexisting")

    def test_package_rollback_does_not_delete_a_still_linked_new_tree(self) -> None:
        old_targets = {}
        targets = {}
        for name in ("bin", "externals"):
            old = self.runner / f"{name}.old"
            new = self.runner / f"{name}.new"
            old.mkdir()
            new.mkdir()
            link = self.runner / name
            if link.exists():
                shutil.rmtree(link)
            link.symlink_to(new, target_is_directory=True)
            old_targets[name] = old
            targets[name] = new
        publication = F._RunnerPackagePublication(
            self.runner, old_targets, targets, {}, {"bin", "externals"}
        )
        real_replace = os.replace

        def fail_bin_link_restore(source, destination):
            if Path(destination) == self.runner / "bin":
                raise OSError("injected link rollback failure")
            return real_replace(source, destination)

        with mock.patch.object(F.os, "replace", side_effect=fail_bin_link_restore):
            with self.assertRaisesRegex(RuntimeError, "restore bin link"):
                publication.rollback()
        self.assertTrue((self.runner / "bin.new").is_dir())
        self.assertEqual((self.runner / "bin").resolve(), (self.runner / "bin.new").resolve())

    def test_policy_file_transaction_restores_every_original_byte(self) -> None:
        paths = [self.runner / ".runner", self.runner / ".runner_migrated", self.runner / ".env"]
        originals = {path: path.read_bytes() for path in paths}
        tx = F._RunnerFileTransaction()
        for path in paths:
            tx.snapshot(path)
            F._atomic_write(path, "changed\n")
        tx.rollback()
        self.assertEqual({path: path.read_bytes() for path in paths}, originals)

    def test_incomplete_rollback_keeps_original_listener_stopped(self) -> None:
        file_tx = mock.Mock()
        file_tx.rollback.side_effect = RuntimeError("injected restore failure")
        with mock.patch.object(F, "_runner_process_present", return_value=False), \
             mock.patch.object(F, "_run_checked") as run_checked:
            errors = F._rollback_runner_policy(
                [self.runner], [self.runner], file_tx, [],
            )
        self.assertIn("policy-file rollback", "; ".join(errors))
        run_checked.assert_not_called()

    def test_rollback_rechecks_worker_after_listener_stops(self) -> None:
        file_tx = mock.Mock()
        worker_observations = iter((False, True))

        def process_present(_runner_dir: Path, process_name: str):
            if process_name == "Runner.Worker":
                return next(worker_observations)
            return True

        with mock.patch.object(F, "_runner_process_present", side_effect=process_present), \
             mock.patch.object(F, "_run_checked"), \
             mock.patch.object(F, "_wait_runner_process", return_value=True):
            errors = F._rollback_runner_policy(
                [self.runner], [self.runner], file_tx, [],
            )
        self.assertIn("Runner.Worker appeared", "; ".join(errors))
        file_tx.rollback.assert_not_called()

    def test_package_layout_requires_root_scripts_and_externals_runtime(self) -> None:
        shutil.rmtree(self.runner / "externals")
        (self.runner / "run.sh").unlink()
        findings = F._runner_package_layout_findings(self.runner)
        self.assertTrue(any("run.sh" in finding for finding in findings))
        self.assertTrue(any("Node runtime" in finding for finding in findings))

    def test_successful_rollback_requires_restored_listener_to_converge(self) -> None:
        file_tx = mock.Mock()
        with mock.patch.object(F, "_runner_process_present", return_value=False), \
             mock.patch.object(F, "_run_checked"), \
             mock.patch.object(F, "_wait_runner_process", return_value=False):
            errors = F._rollback_runner_policy(
                [self.runner], [self.runner], file_tx, [],
            )
        self.assertIn("restored listener did not start", "; ".join(errors))


if __name__ == "__main__":
    unittest.main()
