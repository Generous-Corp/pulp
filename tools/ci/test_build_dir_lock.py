#!/usr/bin/env python3
"""Hostile tests for validation build-directory serialization."""

from __future__ import annotations

import errno
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
                "p.open('a').write(tag+'-start\\n'); time.sleep(0.5); "
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
                # Spawn the contender only once the first holder is inside
                # the lock. A fixed head start loses to interpreter start-up
                # on a loaded runner, which lets the second process win the
                # lock and makes a correct serialisation read as a failure.
                deadline = time.monotonic() + 10
                while "first-start" not in (
                    events.read_text(encoding="utf-8") if events.exists() else ""
                ):
                    self.assertIsNone(first.poll(), "first holder exited early")
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.01)
                second = subprocess.Popen(command("second"))
                self.assertEqual(first.wait(timeout=10), 0)
                self.assertEqual(second.wait(timeout=10), 0)
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

    @unittest.skipIf(sys.platform == "win32", "uses POSIX directory permissions")
    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root ignores modes")
    def test_unwritable_default_root_falls_back_to_temp(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            readonly = base / "readonly"
            readonly.mkdir()
            readonly.chmod(0o500)
            tmp = base / "tmp"
            tmp.mkdir()
            try:
                env = {k: v for k, v in os.environ.items() if k != build_dir_lock.LOCK_ROOT_ENV}
                with mock.patch.dict("os.environ", env, clear=True), mock.patch.object(
                    build_dir_lock, "lock_root", return_value=readonly / "locks"
                ), mock.patch.object(
                    build_dir_lock, "fallback_lock_root", return_value=tmp / "locks"
                ):
                    with build_dir_lock.exclusive_build_dir(base / "build"):
                        pass
                    fallback = build_dir_lock.fallback_lock_root()
                    self.assertTrue(any(fallback.glob("build-dir-*.lock")))
                    self.assertTrue(fallback.resolve().is_relative_to(tmp.resolve()))
            finally:
                readonly.chmod(0o700)

    def test_explicit_root_is_not_replaced_by_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            with self.lock_root(root), mock.patch.object(
                build_dir_lock, "_open_lock_in_root", side_effect=PermissionError("no")
            ):
                with self.assertRaises(PermissionError):
                    with build_dir_lock.exclusive_build_dir(Path(directory) / "b"):
                        pass

    def test_read_only_default_root_uses_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            fallback = base / "fallback"
            default = base / "default"
            fallback_open = (fallback / "lock", object())
            env = {k: v for k, v in os.environ.items() if k != build_dir_lock.LOCK_ROOT_ENV}
            with mock.patch.dict("os.environ", env, clear=True), mock.patch.object(
                build_dir_lock, "lock_root", return_value=default
            ), mock.patch.object(
                build_dir_lock, "fallback_lock_root", return_value=fallback
            ), mock.patch.object(
                build_dir_lock,
                "_open_lock_in_root",
                side_effect=[fallback_open, OSError(errno.EROFS, "read-only")],
            ):
                opened = build_dir_lock._open_locks(base / "build")
            self.assertEqual(opened, [fallback_open])

    def test_nonblocking_lock_io_error_is_not_reported_as_busy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.lock_root(Path(directory) / "state"), mock.patch.object(
                build_dir_lock, "_acquire_lock", side_effect=OSError(errno.EIO, "I/O error")
            ):
                with self.assertRaises(OSError):
                    with build_dir_lock.exclusive_build_dir(Path(directory) / "build", wait=False):
                        pass

    @unittest.skipIf(sys.platform == "win32", "timing probe uses POSIX permissions")
    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root ignores modes")
    def test_confined_fallback_contender_cannot_bypass_ordinary_holder(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            ordinary_root = base / "ordinary-state"
            readonly = base / "readonly"
            readonly.mkdir()
            readonly.chmod(0o500)
            tmp_ordinary = base / "tmp-ordinary"
            tmp_confined = base / "tmp-confined"
            tmp_ordinary.mkdir()
            tmp_confined.mkdir()
            build = base / "build"
            events = base / "events.txt"
            module_dir = str(Path(build_dir_lock.__file__).resolve().parent)
            worker = """import pathlib,sys,time
sys.path.insert(0,sys.argv[4])
import build_dir_lock
build_dir_lock.lock_root=lambda: pathlib.Path(sys.argv[2])
if sys.argv[5] == 'ordinary':
    real_acquire = build_dir_lock._acquire_lock
    count = [0]
    def pause_after_coordination(lock_file, *, wait):
        real_acquire(lock_file, wait=wait)
        count[0] += 1
        if count[0] == 1:
            pathlib.Path(sys.argv[6]).write_text('ready')
            time.sleep(1)
    build_dir_lock._acquire_lock = pause_after_coordination
child = \"import pathlib,sys,time; p=pathlib.Path(sys.argv[1]); p.open('a').write(sys.argv[2]+'-start\\\\n'); time.sleep(1); p.open('a').write(sys.argv[2]+'-end\\\\n')\"
sys.exit(build_dir_lock.main(['--no-wait','--build-dir',sys.argv[1],'--',sys.executable,'-c',child,sys.argv[3],sys.argv[5]]))
"""
            barrier = base / "coordination-acquired"
            env_ordinary = {**os.environ, "TMPDIR": str(tmp_ordinary)}
            env_confined = {**os.environ, "TMPDIR": str(tmp_confined)}
            ordinary = subprocess.Popen(
                [sys.executable, "-c", worker, str(build), str(ordinary_root),
                 str(events), module_dir, "ordinary", str(barrier)],
                env=env_ordinary, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            try:
                deadline = time.monotonic() + 10
                while not barrier.exists():
                    self.assertIsNone(ordinary.poll(), "ordinary holder exited early")
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.01)
                confined = subprocess.run(
                    [sys.executable, "-c", worker, str(build), str(readonly / "locks"),
                     str(events), module_dir, "confined", str(barrier)],
                    env=env_confined, capture_output=True, text=True, encoding="utf-8", timeout=20,
                )
            finally:
                ordinary.terminate()
                ordinary.wait(timeout=10)
                readonly.chmod(0o700)
            self.assertEqual(confined.returncode, build_dir_lock.BUSY_EXIT, confined.stderr)
            self.assertNotIn(
                "confined-start",
                events.read_text(encoding="utf-8") if events.exists() else "",
            )

    def test_relative_override_is_rejected(self) -> None:
        with mock.patch.dict(
            "os.environ", {build_dir_lock.LOCK_ROOT_ENV: "relative"}, clear=False
        ):
            with self.assertRaisesRegex(ValueError, "absolute path"):
                build_dir_lock.lock_root()

    def test_fallback_root_is_stable_without_numeric_uid(self) -> None:
        with mock.patch.object(build_dir_lock.os, "geteuid", None), mock.patch.object(
            tempfile, "gettempdir", return_value="/tmp"
        ):
            self.assertEqual(
                build_dir_lock.fallback_lock_root(), build_dir_lock.fallback_lock_root()
            )

    @unittest.skipUnless(sys.platform == "win32", "Windows fallback path")
    def test_windows_fallback_root_is_independent_of_process_temp(self) -> None:
        with mock.patch.object(build_dir_lock.os, "name", "nt"), mock.patch.dict(
            "os.environ", {"SystemRoot": r"C:\Windows"}, clear=False
        ), mock.patch.object(tempfile, "gettempdir", return_value=r"D:\sandbox-temp"):
            root = build_dir_lock.fallback_lock_root()
            self.assertIn("Windows", str(root))
            self.assertNotIn("sandbox-temp", str(root))


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
            if build_dir_lock.pid_alive(build_pid) is False:
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
