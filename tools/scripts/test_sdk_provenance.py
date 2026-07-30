#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("sdk_provenance.py")
spec = importlib.util.spec_from_file_location("sdk_provenance", SCRIPT)
assert spec and spec.loader
provenance = importlib.util.module_from_spec(spec)
sys.modules["sdk_provenance"] = provenance
spec.loader.exec_module(provenance)

VERSION = "9.8.7"


class SdkProvenanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.prefix = self.root / "prefix"
        self.build = self.root / "build"
        self.source = self.root / "source"
        self.prefix.mkdir()
        self.build.mkdir()
        self.source.mkdir()
        (self.prefix / "version.txt").write_text(f"{VERSION}\n", encoding="utf-8")
        (self.prefix / "sdk_build_type.txt").write_text("Release\n", encoding="utf-8")
        (self.build / "CMakeCache.txt").write_text(
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=OFF\n",
            encoding="utf-8",
        )
        subprocess.run(["git", "init", "-q", self.source], check=True)
        subprocess.run(
            ["git", "-C", self.source, "-c", "user.name=Test", "-c", "user.email=test@example.com",
             "commit", "--allow-empty", "-qm", "fixture"],
            check=True,
        )
        self.sha = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()
        subprocess.run(
            ["git", "-C", self.source, "-c", "tag.gpgSign=false", "tag", f"v{VERSION}"],
            check=True,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def marker(self, **overrides: object) -> dict[str, object]:
        arguments = {
            "prefix": self.prefix,
            "build_dir": self.build,
            "source_dir": self.source,
            "release_tag": f"v{VERSION}",
            "source_sha": self.sha,
            "platform": "darwin-arm64",
        }
        arguments.update(overrides)
        return provenance.build_release_marker(**arguments)

    def test_stamp_and_verify_positive_release_marker(self) -> None:
        marker = self.marker()
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            ),
            marker,
        )
        self.assertEqual(marker["features"], {"audio_probes": False, "inspector": False})
        self.assertEqual(
            stat.S_IMODE((self.prefix / "sdk-provenance.json").stat().st_mode),
            0o644,
        )

    def test_stamp_succeeds_without_posix_fchmod(self) -> None:
        marker = self.marker()
        with mock.patch.object(provenance.os, "fchmod", None, create=True):
            provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        self.assertEqual(
            json.loads(
                (self.prefix / "sdk-provenance.json").read_text(encoding="utf-8")
            ),
            marker,
        )

    def test_rejects_tag_version_mismatch(self) -> None:
        with self.assertRaisesRegex(provenance.ProvenanceError, "does not match"):
            self.marker(release_tag="v1.2.3")

    def test_rejects_tag_source_mismatch(self) -> None:
        subprocess.run(
            ["git", "-C", self.source, "-c", "user.name=Test", "-c", "user.email=test@example.com",
             "commit", "--allow-empty", "-qm", "later"],
            check=True,
        )
        later = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()
        with self.assertRaisesRegex(provenance.ProvenanceError, "identify one commit"):
            self.marker(source_sha=later)

    def test_rejects_development_features(self) -> None:
        (self.build / "CMakeCache.txt").write_text(
            "PULP_ENABLE_AUDIO_PROBES:BOOL=OFF\n"
            "PULP_ENABLE_INSPECTOR:BOOL=ON\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "inspector"):
            self.marker()

    def test_rejects_dirty_tracked_source(self) -> None:
        tracked = self.source / "tracked.txt"
        tracked.write_text("clean\n", encoding="utf-8")
        subprocess.run(["git", "-C", self.source, "add", tracked.name], check=True)
        subprocess.run(
            [
                "git",
                "-C",
                self.source,
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@example.com",
                "commit",
                "-qm",
                "tracked fixture",
            ],
            check=True,
        )
        later = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()
        subprocess.run(
            [
                "git",
                "-C",
                self.source,
                "-c",
                "tag.gpgSign=false",
                "tag",
                "-f",
                f"v{VERSION}",
                later,
            ],
            check=True,
        )
        tracked.write_text("dirty\n", encoding="utf-8")
        with self.assertRaisesRegex(provenance.ProvenanceError, "clean tracked source"):
            self.marker(source_sha=later)

    def test_verify_rejects_marker_for_different_prefix_version(self) -> None:
        marker = self.marker()
        marker["sdk_version"] = "1.2.3"
        marker["source_git_ref"] = "v1.2.3"
        (self.prefix / "sdk-provenance.json").write_text(
            json.dumps(marker), encoding="utf-8"
        )
        with self.assertRaisesRegex(provenance.ProvenanceError, "selected SDK prefix"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            )

    def test_verify_rejects_wrong_expected_commit(self) -> None:
        marker = self.marker()
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        with self.assertRaisesRegex(provenance.ProvenanceError, "source_git_sha"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha="b" * 40,
            )

    def test_verify_rejects_wrong_expected_platform(self) -> None:
        marker = self.marker()
        provenance.write_atomically(self.prefix / "sdk-provenance.json", marker)
        with self.assertRaisesRegex(provenance.ProvenanceError, "platform"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="linux-x64",
                expected_source_sha=self.sha,
            )

    def test_verify_rejects_missing_marker(self) -> None:
        with self.assertRaisesRegex(provenance.ProvenanceError, "cannot read valid"):
            provenance.verify_release_marker(
                self.prefix,
                expected_platform="darwin-arm64",
                expected_source_sha=self.sha,
            )


if __name__ == "__main__":
    unittest.main()
