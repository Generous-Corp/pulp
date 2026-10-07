#!/usr/bin/env python3
"""Windows routes for POSIX-only calls, driven on any host by patching os.name."""

from __future__ import annotations

import hashlib
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "rack"))

import gpu_clean_agent_isolation as isolation  # noqa: E402
import gpu_clean_agent_journey as journey  # noqa: E402
import gpu_dpr_v2_evidence as evidence  # noqa: E402
import qualify_scenes  # noqa: E402


def _tripwire(*_args, **_kwargs):
    raise AssertionError("a POSIX-only call ran on the Windows route")


class SignalGroupTests(unittest.TestCase):
    def test_windows_terminates_then_kills_the_process_without_killpg(self) -> None:
        process = mock.Mock()
        with mock.patch.object(isolation.os, "name", "nt"), \
                mock.patch.object(isolation.os, "killpg", side_effect=_tripwire, create=True):
            isolation._signal_group(process, hard=False)
            isolation._signal_group(process, hard=True)
        process.terminate.assert_called_once_with()
        process.kill.assert_called_once_with()

    def test_posix_signals_the_session(self) -> None:
        process = mock.Mock(pid=4242)
        # create=True: Windows has neither os.killpg nor signal.SIGKILL.
        with mock.patch.object(isolation.os, "name", "posix"), \
                mock.patch.object(isolation.signal, "SIGKILL", 9, create=True), \
                mock.patch.object(isolation.os, "killpg", create=True) as killpg:
            isolation._signal_group(process, hard=False)
            isolation._signal_group(process, hard=True)
            self.assertEqual(killpg.call_args_list, [
                mock.call(4242, isolation.signal.SIGTERM),
                mock.call(4242, isolation.signal.SIGKILL),
            ])
        process.terminate.assert_not_called()


class DirectoryFsyncTests(unittest.TestCase):
    MODULES = (qualify_scenes, journey)

    def test_windows_skips_the_directory_descriptor(self) -> None:
        for module in self.MODULES:
            with self.subTest(module=module.__name__), \
                    mock.patch.object(module.os, "name", "nt"), \
                    mock.patch.object(module.os, "open", side_effect=_tripwire):
                module._fsync_directory(pathlib.Path("."))

    def test_posix_still_fsyncs_the_directory(self) -> None:
        if sys.platform == "win32":
            self.skipTest("POSIX route")
        for module in self.MODULES:
            with self.subTest(module=module.__name__), \
                    tempfile.TemporaryDirectory() as directory, \
                    mock.patch.object(module.os, "fsync") as fsync:
                module._fsync_directory(pathlib.Path(directory))
            fsync.assert_called_once()


class ExecutableBitTests(unittest.TestCase):
    def _snapshot(self, name: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "analyzer.py"
            payload = b"print('x')\n"
            source.write_bytes(payload)
            source.chmod(0o644)
            evidence.snapshot_regular(
                root, "analyzer.py", root / "out" / "analyzer.py", "test trace analyzer",
                max_bytes=1024, expected_sha256=hashlib.sha256(payload).hexdigest(),
                expected_bytes=len(payload), executable=True,
            )

    def test_only_windows_lacks_the_bit(self) -> None:
        with mock.patch.object(evidence.os, "name", "nt"):
            self.assertFalse(evidence._has_executable_bit())
        with mock.patch.object(evidence.os, "name", "posix"):
            self.assertTrue(evidence._has_executable_bit())

    def test_windows_has_no_executable_bit_to_require(self) -> None:
        with mock.patch.object(evidence, "_has_executable_bit", return_value=False):
            self._snapshot("nt")

    def test_posix_refuses_a_file_without_the_bit(self) -> None:
        if sys.platform == "win32":
            self.skipTest("POSIX route")
        with mock.patch.object(evidence, "_has_executable_bit", return_value=True):
            with self.assertRaisesRegex(evidence.V2EvidenceError, "not executable"):
                self._snapshot("posix")


if __name__ == "__main__":
    unittest.main()
