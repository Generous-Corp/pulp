#!/usr/bin/env python3
"""One Forge generation at a time per output directory, held by an OS lock."""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import generation_lock  # noqa: E402

HOLDER = """
import os, sys, time
sys.path.insert(0, {here!r})
import generation_lock
generation_lock.acquire(sys.argv[1])
open(sys.argv[2], "w").close()
time.sleep(60)
"""


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = tmp.name
        self.env = dict(os.environ,
                        FORGE_GENERATION_LOCK_DIR=os.path.join(self.root, "locks"))
        self._saved = os.environ.get(generation_lock.LOCK_DIR_ENV)
        os.environ[generation_lock.LOCK_DIR_ENV] = self.env[generation_lock.LOCK_DIR_ENV]
        self.addCleanup(self._restore)

    def _restore(self) -> None:
        if self._saved is None:
            os.environ.pop(generation_lock.LOCK_DIR_ENV, None)
        else:
            os.environ[generation_lock.LOCK_DIR_ENV] = self._saved
        for fd in generation_lock._held.values():
            os.close(fd)
        generation_lock._held.clear()

    def hold(self, directory: str) -> subprocess.Popen:
        """A separate process holding the lock on `directory`."""
        ready = os.path.join(self.root, f"ready-{time.monotonic_ns()}")
        proc = subprocess.Popen(
            [sys.executable, "-c", HOLDER.format(here=HERE), directory, ready],
            env=self.env)
        self.addCleanup(lambda: (proc.kill(), proc.wait()))
        for _ in range(500):
            if os.path.exists(ready):
                return proc
            time.sleep(0.01)
        self.fail("lock holder never became ready")


class LockTest(Fixture):
    def test_a_second_generation_on_the_same_directory_is_refused(self) -> None:
        plugins = os.path.join(self.root, "plugins")
        self.hold(plugins)
        with self.assertRaises(generation_lock.GenerationBusy):
            generation_lock.acquire(plugins)

    def test_a_different_directory_is_not_refused(self) -> None:
        self.hold(os.path.join(self.root, "plugins-a"))
        generation_lock.acquire(os.path.join(self.root, "plugins-b"))

    def test_the_same_directory_by_another_spelling_is_the_same_lock(self) -> None:
        plugins = os.path.join(self.root, "plugins")
        os.makedirs(plugins)
        os.symlink(plugins, os.path.join(self.root, "alias"))
        self.hold(plugins)
        with self.assertRaises(generation_lock.GenerationBusy):
            generation_lock.acquire(os.path.join(self.root, "alias"))

    def test_a_crashed_holder_releases_the_lock(self) -> None:
        plugins = os.path.join(self.root, "plugins")
        holder = self.hold(plugins)
        os.kill(holder.pid, signal.SIGKILL)
        holder.wait()
        generation_lock.acquire(plugins)  # no stale marker to clear

    def test_taking_the_lock_never_writes_into_the_directory(self) -> None:
        plugins = os.path.join(self.root, "never-created")
        generation_lock.acquire(plugins)
        self.assertFalse(os.path.exists(plugins))


class EntryPointTest(Fixture):
    """Both generators take the lock before doing any work."""

    def run_tool(self, args: list[str], plugins: str) -> subprocess.CompletedProcess:
        # A bare PATH and an empty HOME: past the lock, the tool stops at its
        # missing-prerequisite check, so no run here can reach a model.
        home = os.path.join(self.root, "home")
        os.makedirs(home, exist_ok=True)
        env = dict(self.env, RACK_PLUGIN_DIR=plugins, HOME=home, PATH="/usr/bin:/bin")
        return subprocess.run([sys.executable, *args], cwd=HERE, env=env,
                              capture_output=True, text=True, timeout=120)

    def test_patch_build_is_refused_while_its_plugin_directory_is_held(self) -> None:
        plugins = os.path.join(self.root, "plugins")
        self.hold(plugins)
        r = self.run_tool(["patch.py", "build", "a drone"], plugins)
        self.assertEqual(r.returncode, generation_lock.BUSY_EXIT, r.stdout + r.stderr)
        self.assertIn("another Forge generation is already running", r.stdout)
        # Control: the same build against an unheld directory gets past the lock.
        other = self.run_tool(["patch.py", "build", "a drone"],
                              os.path.join(self.root, "other"))
        self.assertNotEqual(other.returncode, generation_lock.BUSY_EXIT)
        self.assertNotIn("already running", other.stdout)

    def test_generate_is_refused_while_its_install_directory_is_held(self) -> None:
        plugins = os.path.join(self.root, "plugins")
        self.hold(plugins)
        r = self.run_tool(["generate.py", "a sine", "--install-dir", plugins], plugins)
        self.assertEqual(r.returncode, generation_lock.BUSY_EXIT, r.stdout + r.stderr)
        self.assertIn("another Forge generation is already running", r.stdout)
        other_dir = os.path.join(self.root, "other")
        other = self.run_tool(["generate.py", "a sine", "--install-dir", other_dir], other_dir)
        self.assertNotEqual(other.returncode, generation_lock.BUSY_EXIT)
        self.assertNotIn("already running", other.stdout)


if __name__ == "__main__":
    unittest.main()
