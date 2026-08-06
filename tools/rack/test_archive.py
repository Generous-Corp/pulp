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

    def test_extract_checks_decompressor_failure(self) -> None:
        producer = _Producer(returncode=7)
        with mock.patch.object(archive, "_zstd", return_value="zstd"), \
             mock.patch.object(archive, "_tar", return_value="tar"), \
             mock.patch.object(archive.subprocess, "Popen",
                               return_value=producer), \
             mock.patch.object(archive.subprocess, "run",
                               side_effect=[
                                   SimpleNamespace(returncode=0, stdout=b"tar"),
                                   SimpleNamespace(returncode=0,
                                                   stdout=b"Thing/plugin.json\n"),
                                   SimpleNamespace(returncode=0)]):
            self.assertFalse(archive.extract_all("bad.vcvplugin", "."))

    def test_extract_checks_tar_consumer_failure(self) -> None:
        producer = _Producer(returncode=0)
        with mock.patch.object(archive, "_zstd", return_value="zstd"), \
             mock.patch.object(archive, "_tar", return_value="tar"), \
             mock.patch.object(archive.subprocess, "Popen",
                               return_value=producer), \
             mock.patch.object(archive.subprocess, "run",
                               side_effect=[
                                   SimpleNamespace(returncode=0, stdout=b"tar"),
                                   SimpleNamespace(returncode=0,
                                                   stdout=b"Thing/plugin.json\n"),
                                   SimpleNamespace(returncode=9)]):
            self.assertFalse(archive.extract_all("bad.vcvplugin", "."))

    def test_failed_extract_never_publishes_partial_plugin(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            def partial(_archive, stage):
                os.makedirs(os.path.join(stage, "Thing"))
                with open(os.path.join(stage, "Thing", "partial"), "w") as f:
                    f.write("incomplete\n")
                return False

            with mock.patch.object(archive, "_extract_all_into",
                                   side_effect=partial):
                self.assertFalse(archive.extract_all("bad.vcvplugin", tmp))
            self.assertFalse(os.path.exists(os.path.join(tmp, "Thing")))

    def test_successful_extract_atomically_publishes_one_plugin(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            def complete(_archive, stage):
                os.makedirs(os.path.join(stage, "Thing"))
                with open(os.path.join(stage, "Thing", "plugin.json"), "w") as f:
                    f.write("{}\n")
                return True

            with mock.patch.object(archive, "_extract_all_into",
                                   side_effect=complete):
                self.assertTrue(archive.extract_all("good.vcvplugin", tmp))
            self.assertTrue(os.path.isfile(
                os.path.join(tmp, "Thing", "plugin.json")))
            self.assertFalse(any(name.startswith(".forge-extract-")
                                 for name in os.listdir(tmp)))

    def test_extract_rejects_links_and_never_replaces_a_dangling_link(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            outside = os.path.join(tmp, "outside")
            os.makedirs(outside)

            def linked(_archive, stage):
                os.symlink(outside, os.path.join(stage, "Thing"))
                return True

            with mock.patch.object(archive, "_extract_all_into",
                                   side_effect=linked):
                self.assertFalse(archive.extract_all("linked.vcvplugin", tmp))
            self.assertFalse(os.path.lexists(os.path.join(tmp, "Thing")))

            dangling = os.path.join(tmp, "Thing")
            os.symlink(os.path.join(tmp, "missing"), dangling)

            def complete(_archive, stage):
                os.makedirs(os.path.join(stage, "Thing"))
                with open(os.path.join(stage, "Thing", "plugin.json"), "w") as f:
                    f.write("{}\n")
                return True

            with mock.patch.object(archive, "_extract_all_into",
                                   side_effect=complete):
                self.assertFalse(archive.extract_all("good.vcvplugin", tmp))
            self.assertTrue(os.path.islink(dangling))

    def test_extract_rejects_parent_and_absolute_archive_members(self) -> None:
        for member in ("../outside", "Thing/../../outside", "/tmp/outside"):
            with self.subTest(member=member):
                listing = SimpleNamespace(returncode=0, stdout=member + "\n")
                with mock.patch.object(archive, "_zstd", return_value=None), \
                     mock.patch.object(archive.subprocess, "run",
                                       return_value=listing):
                    self.assertFalse(archive._extract_all_into(
                        "hostile.vcvplugin", "."))


if __name__ == "__main__":
    unittest.main()
