"""Tests for the visual-harness Dockerfile / manifest Skia-pin lint (P9-4)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.visual import check_skia_pin  # noqa: E402

_DOCKERFILE = """\
# syntax=docker/dockerfile:1.7
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG SKIA_RELEASE_TAG=chrome/m144
ARG SKIA_LINUX_X64_SHA256=aaaa
ARG SKIA_LINUX_X64_URL=https://example.test/skia.zip
ARG SKIA_PYTHON_VERSION=144.0.post2
"""

_MANIFEST_TEMPLATE = {
    "dependencies": [
        {
            "name": "Skia",
            "determinism": {
                "skia_branch": "chrome/m144",
                "skia_python_smoke_version": "144.0.post2",
                "release_assets": {
                    "linux-x64": {
                        "url": "https://example.test/skia.zip",
                        "sha256": "aaaa",
                    }
                },
            },
        }
    ]
}


def _write_manifest(path: Path, manifest: dict) -> Path:
    import json

    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


class CheckSkiaPinTests(unittest.TestCase):
    def _dockerfile(self, body: str = _DOCKERFILE) -> Path:
        path = Path(self._tmpdir.name) / "visual-harness.Dockerfile"
        path.write_text(body, encoding="utf-8")
        return path

    def setUp(self) -> None:
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)

    # --- repo-state guard: the live repo must currently be in sync ----------

    def test_live_repo_pin_is_in_sync(self) -> None:
        """The lint must pass against the current checked-in files."""
        self.assertEqual(check_skia_pin.main([]), 0)

    # --- match --------------------------------------------------------------

    def test_compare_returns_no_drift_when_pins_match(self) -> None:
        docker = check_skia_pin.read_dockerfile_pin(self._dockerfile())
        manifest_path = _write_manifest(
            Path(self._tmpdir.name) / "manifest.json", _MANIFEST_TEMPLATE
        )
        manifest = check_skia_pin.read_manifest_pin(manifest_path)
        self.assertEqual(check_skia_pin.compare(docker, manifest), [])

    # --- mismatch -----------------------------------------------------------

    def test_compare_reports_release_tag_drift(self) -> None:
        docker = check_skia_pin.read_dockerfile_pin(self._dockerfile())
        import copy

        drifted = copy.deepcopy(_MANIFEST_TEMPLATE)
        drifted["dependencies"][0]["determinism"]["skia_branch"] = "chrome/m145"
        manifest_path = _write_manifest(
            Path(self._tmpdir.name) / "manifest.json", drifted
        )
        manifest = check_skia_pin.read_manifest_pin(manifest_path)

        drift = check_skia_pin.compare(docker, manifest)
        self.assertEqual(len(drift), 1)
        self.assertIn("Skia release tag", drift[0])
        self.assertIn("chrome/m144", drift[0])
        self.assertIn("chrome/m145", drift[0])

    def test_compare_reports_sha_and_url_drift(self) -> None:
        docker = check_skia_pin.read_dockerfile_pin(self._dockerfile())
        import copy

        drifted = copy.deepcopy(_MANIFEST_TEMPLATE)
        asset = drifted["dependencies"][0]["determinism"]["release_assets"][
            "linux-x64"
        ]
        asset["sha256"] = "bbbb"
        asset["url"] = "https://example.test/other.zip"
        manifest_path = _write_manifest(
            Path(self._tmpdir.name) / "manifest.json", drifted
        )
        manifest = check_skia_pin.read_manifest_pin(manifest_path)

        drift = check_skia_pin.compare(docker, manifest)
        self.assertEqual(len(drift), 2)
        joined = "\n".join(drift)
        self.assertIn("SHA-256", joined)
        self.assertIn("URL", joined)

    def test_missing_arg_line_raises_check_error(self) -> None:
        body = "\n".join(
            line
            for line in _DOCKERFILE.splitlines()
            if "SKIA_PYTHON_VERSION" not in line
        )
        with self.assertRaises(check_skia_pin.CheckError):
            check_skia_pin.read_dockerfile_pin(self._dockerfile(body))

    def test_manifest_without_skia_raises_check_error(self) -> None:
        manifest_path = _write_manifest(
            Path(self._tmpdir.name) / "manifest.json", {"dependencies": []}
        )
        with self.assertRaises(check_skia_pin.CheckError):
            check_skia_pin.read_manifest_pin(manifest_path)


if __name__ == "__main__":
    unittest.main()
