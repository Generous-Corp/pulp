#!/usr/bin/env python3
"""Failure-boundary tests for .vcvplugin archive creation."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

import archive


class _Pipe:
    def close(self) -> None:
        pass


class _Producer:
    def __init__(self, returncode: int) -> None:
        self.args = ["tar"]
        self.stdout = _Pipe()
        self._returncode = returncode

    def wait(self) -> int:
        return self._returncode

    def poll(self) -> int:
        return self._returncode


class ArchiveCreationTests(unittest.TestCase):
    def _failure_case(self, producer_status: int,
                      consumer_status: int) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = os.path.join(tmp, "partial.vcvplugin")
            producer = _Producer(producer_status)

            def consume(args, **_kwargs):
                with open(output, "wb") as partial:
                    partial.write(b"not a complete archive")
                return SimpleNamespace(args=args, returncode=consumer_status)

            with mock.patch.object(archive, "_zstd", return_value="zstd"), \
                 mock.patch.object(archive, "_tar", return_value="tar"), \
                 mock.patch.object(archive.subprocess, "Popen",
                                   return_value=producer), \
                 mock.patch.object(archive.subprocess, "run",
                                   side_effect=consume):
                with self.assertRaises(subprocess.CalledProcessError):
                    archive.create(output, tmp, "Thing")

            self.assertFalse(os.path.exists(output))

    def test_tar_failure_is_checked_and_partial_archive_is_deleted(self) -> None:
        self._failure_case(producer_status=7, consumer_status=0)

    def test_zstd_failure_is_checked_and_partial_archive_is_deleted(self) -> None:
        self._failure_case(producer_status=0, consumer_status=9)

    def test_bsdtar_failure_deletes_its_partial_archive(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = os.path.join(tmp, "partial.vcvplugin")

            def fail(args, **_kwargs):
                with open(output, "wb") as partial:
                    partial.write(b"not a complete archive")
                raise subprocess.CalledProcessError(5, args)

            with mock.patch.object(archive, "_zstd", return_value=None), \
                 mock.patch.object(archive, "_tar", return_value="tar"), \
                 mock.patch.object(archive.subprocess, "run", side_effect=fail):
                with self.assertRaises(subprocess.CalledProcessError):
                    archive.create(output, tmp, "Thing")

            self.assertFalse(os.path.exists(output))


if __name__ == "__main__":
    unittest.main()
