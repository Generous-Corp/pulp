from __future__ import annotations

import tarfile
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from archive_sdk import archive_tree
from check_archive_mtimes import check_archive


class ArchiveSdkTest(unittest.TestCase):
    def test_archive_normalizes_all_tar_mtimes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "pulp-sdk"
            root.mkdir()
            (root / "file.txt").write_text("payload", encoding="utf-8")
            archive = Path(temporary) / "sdk.tar.gz"
            archive_tree(root, archive, 123456789)
            self.assertEqual(check_archive(archive, 123456789), [])
            with tarfile.open(archive, "r:gz") as tar:
                self.assertEqual({member.mtime for member in tar.getmembers()}, {123456789})

    def test_checker_rejects_future_tar_member(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "future.tar.gz"
            with tarfile.open(archive, "w:gz") as tar:
                member = tarfile.TarInfo("future.txt")
                member.size = 0
                member.mtime = time.time() + 3600
                tar.addfile(member)
            self.assertEqual(len(check_archive(archive, time.time())), 1)

    def test_checker_rejects_future_zip_member(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "future.zip"
            with zipfile.ZipFile(archive, "w") as zipped:
                info = zipfile.ZipInfo("future.txt", time.localtime(time.time() + 3600)[:6])
                zipped.writestr(info, "")
            self.assertEqual(len(check_archive(archive, time.time())), 1)


if __name__ == "__main__":
    unittest.main()
