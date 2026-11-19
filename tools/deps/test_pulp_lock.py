#!/usr/bin/env python3
"""Negative controls for the pulp.lock readers."""

from __future__ import annotations

import copy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import pulp_lock


class PulpLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.lock = pulp_lock.load()

    def test_missing_field_fails_closed(self) -> None:
        lock = copy.deepcopy(self.lock)
        del lock["vellum"]["sdk_digest"]
        self.assertTrue(any("exactly tag, sha" in e for e in pulp_lock.validate(lock)))

    def test_tag_sha_disagreement_fails_closed(self) -> None:
        sha = self.lock["vellum"]["sha"]
        self.assertTrue(any("tag resolves" in e for e in pulp_lock.validate(self.lock, tag_sha="0" * 40)))
        self.assertFalse(pulp_lock.validate(self.lock, tag_sha=sha))

    def test_digest_mismatch_fails_cmake_reader(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock_path = root / "pulp.lock"
            lock_path.write_text(json.dumps(self.lock), encoding="utf-8")
            archive = root / "sdk.tar.gz"
            archive.write_bytes(b"deliberately wrong bytes")
            reader = Path(__file__).parents[1] / "cmake" / "PulpVellumLock.cmake"
            result = subprocess.run(
                ["cmake", f"-DPULP_VELLUM_LOCK_FILE={lock_path}",
                 f"-DPULP_VELLUM_SDK_ARCHIVE={archive}", "-P", str(reader)],
                capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("digest mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
