#!/usr/bin/env python3
"""Hostile tests for validation build-directory serialization."""

from __future__ import annotations

import os
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_dir_lock


class BuildDirLockTest(unittest.TestCase):
    @contextmanager
    def lock_root(self, root: Path):
        with mock.patch.dict(
            "os.environ", {build_dir_lock.LOCK_ROOT_ENV: str(root)}, clear=False
        ):
            yield

    def test_remainder_requires_a_real_command(self) -> None:
        with self.assertRaises(SystemExit):
            build_dir_lock.parse_args(["--build-dir", "build", "--"])

    @unittest.skipIf(sys.platform == "win32", "timing probe uses POSIX Python command")
    def test_two_processes_cannot_enter_the_same_build_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            lock_root = root / "host-state"
            events = root / "events.txt"
            child = (
                "import pathlib,sys,time; "
                "p=pathlib.Path(sys.argv[1]); tag=sys.argv[2]; "
                "p.open('a').write(tag+'-start\\n'); time.sleep(0.2); "
                "p.open('a').write(tag+'-end\\n')"
            )
            wrapper = Path(build_dir_lock.__file__).resolve()
            def command(tag: str) -> list[str]:
                return [
                    sys.executable,
                    str(wrapper),
                    "--build-dir",
                    str(build),
                    "--",
                    sys.executable,
                    "-c",
                    child,
                    str(events),
                    tag,
                ]
            with self.lock_root(lock_root):
                first = subprocess.Popen(command("first"))
                time.sleep(0.05)
                second = subprocess.Popen(command("second"))
                self.assertEqual(first.wait(timeout=5), 0)
                self.assertEqual(second.wait(timeout=5), 0)
            self.assertEqual(
                events.read_text(encoding="utf-8").splitlines(),
                ["first-start", "first-end", "second-start", "second-end"],
            )

    def test_lock_lives_outside_checkout_and_persists_after_unlock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout = root / "checkout"
            build = checkout / "build"
            state = root / "host-state"
            checkout.mkdir()
            with self.lock_root(state):
                with build_dir_lock.exclusive_build_dir(build):
                    path = build_dir_lock.lock_path_for(build)
                    self.assertTrue(path.is_file())
                    self.assertEqual(path.parent, state.resolve())
                self.assertFalse(path.is_relative_to(checkout))
                self.assertTrue(path.is_file())
                self.assertFalse((checkout / ".build.pulp-validation.lock").exists())
                self.assertEqual(stat.S_IMODE(state.stat().st_mode), 0o700)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    @unittest.skipIf(sys.platform == "win32", "POSIX permission hardening")
    def test_existing_lock_state_permissions_are_restricted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            state = root / "state"
            state.mkdir(mode=0o777)
            state.chmod(0o777)
            with self.lock_root(state):
                path = build_dir_lock.lock_path_for(root / "build")
                path.write_bytes(b"")
                path.chmod(0o666)
                with build_dir_lock.exclusive_build_dir(root / "build"):
                    pass
                self.assertEqual(stat.S_IMODE(state.stat().st_mode), 0o700)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_canonical_aliases_share_one_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real = root / "real"
            real.mkdir()
            alias = root / "alias"
            alias.symlink_to(real, target_is_directory=True)
            with self.lock_root(root / "state"):
                self.assertEqual(
                    build_dir_lock.lock_path_for(real / "build"),
                    build_dir_lock.lock_path_for(alias / "build"),
                )

    def test_same_basename_in_different_checkouts_does_not_collide(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.lock_root(root / "state"):
                first = build_dir_lock.lock_path_for(root / "one" / "build")
                second = build_dir_lock.lock_path_for(root / "two" / "build")
                self.assertNotEqual(first, second)
                self.assertEqual(len(first.stem.removeprefix("build-dir-")), 64)

    def test_identity_marker_fails_closed_on_digest_collision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            state = root / "state"
            with self.lock_root(state), mock.patch.object(
                build_dir_lock.hashlib, "sha256"
            ) as sha256:
                sha256.return_value.hexdigest.return_value = "a" * 64
                with build_dir_lock.exclusive_build_dir(root / "one" / "build"):
                    pass
                with self.assertRaisesRegex(OSError, "identity collision"):
                    with build_dir_lock.exclusive_build_dir(root / "two" / "build"):
                        pass

    @unittest.skipIf(sys.platform == "win32", "POSIX symlink hardening")
    def test_symlink_lock_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            state = root / "state"
            victim = root / "victim"
            victim.write_text("do not touch", encoding="utf-8")
            with self.lock_root(state):
                state.mkdir(mode=0o700)
                path = build_dir_lock.lock_path_for(root / "build")
                path.symlink_to(victim)
                with self.assertRaises(OSError):
                    with build_dir_lock.exclusive_build_dir(root / "build"):
                        pass
                self.assertEqual(victim.read_text(encoding="utf-8"), "do not touch")

    def test_relative_override_is_rejected(self) -> None:
        with mock.patch.dict(
            "os.environ", {build_dir_lock.LOCK_ROOT_ENV: "relative"}, clear=False
        ):
            with self.assertRaisesRegex(ValueError, "absolute path"):
                build_dir_lock.lock_root()


if __name__ == "__main__":
    unittest.main()
