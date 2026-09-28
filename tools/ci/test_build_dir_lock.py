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


@unittest.skipIf(sys.platform == "win32", "holder probes use POSIX signals")
class NoWaitTest(unittest.TestCase):
    """--no-wait refuses a second build into a live tree and names its holder."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.build = self.root / "build"
        self.env = {
            **os.environ,
            build_dir_lock.LOCK_ROOT_ENV: str(self.root / "state"),
        }
        self.env.pop(build_dir_lock.HELD_ENV, None)
        self.wrapper = str(Path(build_dir_lock.__file__).resolve())
        self.ready = self.root / "ready"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _holder(self) -> subprocess.Popen:
        child = (
            "import os,pathlib,sys,time; "
            "pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(30)"
        )
        proc = subprocess.Popen(
            [sys.executable, self.wrapper, "--no-wait", "--build-dir", str(self.build),
             "--", sys.executable, "-c", child, str(self.ready)],
            env=self.env,
        )
        deadline = time.monotonic() + 10
        while not self.ready.exists():
            self.assertLess(time.monotonic(), deadline, "holder never started")
            time.sleep(0.02)
        return proc

    def _second(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, self.wrapper, "--no-wait", "--build-dir", str(self.build),
             "--", sys.executable, "-c", "print('SECOND-RAN')"],
            env=self.env, capture_output=True, text=True, timeout=20,
        )

    def test_second_build_is_refused_and_names_the_live_holder(self) -> None:
        holder = self._holder()
        try:
            result = self._second()
        finally:
            holder.terminate()
            holder.wait(timeout=10)
        self.assertEqual(result.returncode, build_dir_lock.BUSY_EXIT, result.stderr)
        self.assertNotIn("SECOND-RAN", result.stdout)
        self.assertIn(f"holder pid={holder.pid} (alive)", result.stderr)
        self.assertIn("command:", result.stderr)

    def test_killed_holder_leaves_no_stale_lock(self) -> None:
        holder = self._holder()
        holder.kill()
        holder.wait(timeout=10)
        result = self._second()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SECOND-RAN", result.stdout)

    def test_terminating_the_holder_stops_its_build(self) -> None:
        holder = self._holder()
        build_pid = int(self.ready.read_text())
        holder.terminate()
        holder.wait(timeout=10)
        # The forwarded SIGTERM stopped the build child; it was not orphaned
        # to keep writing into a tree whose lock had just been released.
        deadline = time.monotonic() + 5
        while True:
            try:
                os.kill(build_pid, 0)
            except ProcessLookupError:
                break
            self.assertLess(time.monotonic(), deadline, "build outlived its lock holder")
            time.sleep(0.02)

    def test_descendant_of_the_holder_may_build_the_same_tree(self) -> None:
        nested = (
            f"import subprocess,sys; sys.exit(subprocess.run([sys.executable, {self.wrapper!r}, "
            f"'--no-wait', '--build-dir', {str(self.build)!r}, '--', sys.executable, '-c', "
            "'print(\"NESTED-RAN\")']).returncode)"
        )
        result = subprocess.run(
            [sys.executable, self.wrapper, "--no-wait", "--build-dir", str(self.build),
             "--", sys.executable, "-c", nested],
            env=self.env, capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("NESTED-RAN", result.stdout)

    def test_in_process_no_wait_raises_with_holder(self) -> None:
        holder = self._holder()
        try:
            with mock.patch.dict("os.environ", {
                build_dir_lock.LOCK_ROOT_ENV: str(self.root / "state"),
            }, clear=False):
                os.environ.pop(build_dir_lock.HELD_ENV, None)
                with self.assertRaises(build_dir_lock.BuildDirBusy) as caught:
                    with build_dir_lock.exclusive_build_dir(self.build, wait=False):
                        pass
        finally:
            holder.terminate()
            holder.wait(timeout=10)
        self.assertIsNotNone(caught.exception.holder)
        self.assertEqual(caught.exception.holder.get("pid"), holder.pid)


if __name__ == "__main__":
    unittest.main()
