#!/usr/bin/env python3
"""Focused tests for the pinned Node runtime preparer."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("prepare_node_runtime.py")
spec = importlib.util.spec_from_file_location("prepare_node_runtime", SCRIPT)
assert spec and spec.loader
pnr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pnr)


class PrepareNodeRuntimeTests(unittest.TestCase):
    def test_verified_archive_materializes_executable_and_license(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            tree = root / "node-v1-test"
            (tree / "bin").mkdir(parents=True)
            (tree / "bin" / "node").write_bytes(b"portable-node")
            (tree / "LICENSE").write_text("complete license", encoding="utf-8")
            archive = root / "node-test.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                bundle.add(tree, arcname=tree.name)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            original = pnr.ARTIFACTS["darwin-arm64"]
            pnr.ARTIFACTS["darwin-arm64"] = (archive.name, digest)
            try:
                output = root / "stage" / "node"
                pnr.prepare("darwin-arm64", output, archive)
            finally:
                pnr.ARTIFACTS["darwin-arm64"] = original
            self.assertEqual(output.read_bytes(), b"portable-node")
            self.assertEqual(
                output.with_name("node.LICENSE").read_text(encoding="utf-8"),
                "complete license",
            )
            self.assertTrue(output.stat().st_mode & 0o100)

    def test_tar_runtime_does_not_extract_unneeded_npm_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            tree = root / "node-v1-test"
            (tree / "bin").mkdir(parents=True)
            (tree / "bin" / "node").write_bytes(b"portable-node")
            (tree / "LICENSE").write_text("complete license", encoding="utf-8")
            archive = root / "node-test.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                bundle.add(tree, arcname=tree.name)
                npm = tarfile.TarInfo(f"{tree.name}/bin/npm")
                npm.type = tarfile.SYMTYPE
                npm.linkname = "../lib/node_modules/npm/bin/npm-cli.js"
                bundle.addfile(npm)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            original = pnr.ARTIFACTS["linux-x64"]
            pnr.ARTIFACTS["linux-x64"] = (archive.name, digest)
            filter_error = tarfile.LinkOutsideDestinationError(
                npm,
                root / "lib/node_modules/npm/bin/npm-cli.js",
            )
            try:
                output = root / "stage" / "node"
                with mock.patch.object(
                    tarfile.TarFile,
                    "extractall",
                    side_effect=filter_error,
                ):
                    pnr.prepare("linux-x64", output, archive)
            finally:
                pnr.ARTIFACTS["linux-x64"] = original
            self.assertEqual(output.read_bytes(), b"portable-node")
            self.assertEqual(
                output.with_name("node.LICENSE").read_text(encoding="utf-8"),
                "complete license",
            )

    def test_tar_runtime_rejects_node_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            archive = root / "node-test.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                node = tarfile.TarInfo("node-v1-test/bin/node")
                node.type = tarfile.SYMTYPE
                node.linkname = "../../outside-node"
                bundle.addfile(node)
                license_info = tarfile.TarInfo("node-v1-test/LICENSE")
                license_bytes = b"complete license"
                license_info.size = len(license_bytes)
                bundle.addfile(license_info, io.BytesIO(license_bytes))
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            original = pnr.ARTIFACTS["linux-x64"]
            pnr.ARTIFACTS["linux-x64"] = (archive.name, digest)
            try:
                with self.assertRaisesRegex(RuntimeError, "regular file"):
                    pnr.prepare("linux-x64", root / "stage" / "node", archive)
            finally:
                pnr.ARTIFACTS["linux-x64"] = original

    def test_checksum_mismatch_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            archive = Path(td) / "bad.tar.gz"
            archive.write_bytes(b"not the pinned archive")
            with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                pnr.prepare("darwin-arm64", Path(td) / "node", archive)


if __name__ == "__main__":
    unittest.main()
