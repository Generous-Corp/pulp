from __future__ import annotations

import os
import tarfile
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from archive_sdk import archive_tree
from check_archive_mtimes import check_archive

# Fixed instants so the tests do not depend on the wall clock.
BASE = 1_700_000_000
FUTURE = BASE + 8 * 3600


class ArchiveSdkTest(unittest.TestCase):
    def _build_tree(self, base: Path) -> Path:
        root = base / "pulp-sdk"
        (root / "bin").mkdir(parents=True)
        (root / "lib").mkdir()
        (root / "empty").mkdir()
        tool = root / "bin" / "tool"
        tool.write_text("#!/bin/sh\n", encoding="utf-8")
        tool.chmod(0o755)
        secret = root / "lib" / "private.dat"
        secret.write_text("x", encoding="utf-8")
        secret.chmod(0o600)
        (root / "lib" / "libx.so.1").write_text("lib", encoding="utf-8")
        os.symlink("libx.so.1", root / "lib" / "libx.so")
        os.symlink("missing-target", root / "lib" / "dangling")
        return root

    def test_archive_preserves_prefix_modes_and_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self._build_tree(Path(temporary))
            archive = Path(temporary) / "sdk.tar.gz"
            archive_tree(root, archive, BASE)
            with tarfile.open(archive, "r:gz") as tar:
                members = {member.name: member for member in tar.getmembers()}
            self.assertTrue(all(name == "pulp-sdk" or name.startswith("pulp-sdk/") for name in members))
            self.assertTrue(members["pulp-sdk"].isdir())
            self.assertTrue(members["pulp-sdk/empty"].isdir())
            self.assertEqual(members["pulp-sdk/bin/tool"].mode & 0o777, 0o755)
            self.assertEqual(members["pulp-sdk/lib/private.dat"].mode & 0o777, 0o600)
            for name, target in (("libx.so", "libx.so.1"), ("dangling", "missing-target")):
                link = members[f"pulp-sdk/lib/{name}"]
                self.assertTrue(link.issym(), name)
                self.assertEqual(link.linkname, target)

    def test_archive_normalizes_all_tar_mtimes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "pulp-sdk"
            root.mkdir()
            payload = root / "file.txt"
            payload.write_text("payload", encoding="utf-8")
            os.utime(payload, (FUTURE, FUTURE))
            os.utime(root, (FUTURE, FUTURE))
            archive = Path(temporary) / "sdk.tar.gz"
            archive_tree(root, archive, 123456789)
            self.assertEqual(check_archive(archive, 123456789), [])
            with tarfile.open(archive, "r:gz") as tar:
                self.assertEqual({member.mtime for member in tar.getmembers()}, {123456789})
            with archive.open("rb") as raw:
                header = raw.read(8)
            self.assertEqual(int.from_bytes(header[4:8], "little"), 123456789)

    def test_checker_rejects_future_tar_member(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "future.tar.gz"
            with tarfile.open(archive, "w:gz") as tar:
                member = tarfile.TarInfo("future.txt")
                member.size = 0
                member.mtime = FUTURE
                tar.addfile(member)
            self.assertEqual(len(check_archive(archive, BASE)), 1)

    def test_checker_rejects_future_zip_member(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "future.zip"
            with zipfile.ZipFile(archive, "w") as zipped:
                info = zipfile.ZipInfo("future.txt", (2030, 1, 1, 0, 0, 0))
                zipped.writestr(info, "")
            self.assertEqual(len(check_archive(archive, BASE)), 1)


if __name__ == "__main__":
    unittest.main()
