#!/usr/bin/env python3

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "sdk_identity.py"
SPEC = importlib.util.spec_from_file_location("sdk_identity", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class SdkIdentityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.prefix = Path(self.tmp.name)
        self.version = "0.700.0"
        self.source = "a" * 40
        (self.prefix / "version.txt").write_text(self.version + "\n")
        (self.prefix / "sdk_build_type.txt").write_text("Release\n")
        (self.prefix / "payload").write_text("sdk bytes\n")
        (self.prefix / "sdk-provenance.json").write_text(json.dumps({
            "schema": "pulp.sdk-provenance.v1", "kind": "release",
            "profile": "official-release", "distribution_eligible": True,
            "sdk_version": self.version, "source_git_ref": "v" + self.version,
            "source_git_sha": self.source, "source_git_dirty": False,
            "platform": "darwin-arm64", "build_type": "Release",
            "features": {"audio_probes": False, "inspector": False},
        }))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_exact_official_sdk_is_content_bound(self) -> None:
        expected = MODULE.directory_sha256(self.prefix)
        first = MODULE.identify(
            self.prefix, "darwin-arm64", self.source, self.version, expected
        )
        self.assertEqual(first["content_sha256"], expected)
        (self.prefix / "payload").write_text("changed bytes\n")
        with self.assertRaises(MODULE.PROVENANCE.ProvenanceError):
            MODULE.identify(
                self.prefix, "darwin-arm64", self.source, self.version, expected
            )

    def test_wrong_source_sha_is_rejected(self) -> None:
        with self.assertRaises(MODULE.PROVENANCE.ProvenanceError):
            MODULE.identify(
                self.prefix, "darwin-arm64", "b" * 40, self.version,
                MODULE.directory_sha256(self.prefix),
            )

    def test_current_sdk_without_capability_handoff_is_rejected(self) -> None:
        self.version = "0.790.1"
        (self.prefix / "version.txt").write_text(self.version + "\n")
        marker_path = self.prefix / "sdk-provenance.json"
        marker = json.loads(marker_path.read_text())
        marker["sdk_version"] = self.version
        marker["source_git_ref"] = "v" + self.version
        marker["features"]["inspector"] = True
        marker_path.write_text(json.dumps(marker))

        with self.assertRaises(MODULE.PROVENANCE.HandoffError):
            MODULE.identify(
                self.prefix, "darwin-arm64", self.source, self.version,
                MODULE.directory_sha256(self.prefix),
            )


if __name__ == "__main__":
    unittest.main()
