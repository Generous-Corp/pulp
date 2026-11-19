#!/usr/bin/env python3
import hashlib
import json
import tarfile
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))
import bundle_vellum_sdk as bundle


class BundleVellumSdkTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.integration = self.root / "integrations"
        (self.integration / "pulp-vellum").mkdir(parents=True)
        (self.integration / "pulp-vellum" / "bridge.hpp").write_text("bridge\n")
        source = self.root / "source"
        (source / "sdk" / "share" / "doc" / "Vellum").mkdir(parents=True)
        (source / "sdk" / "lib").mkdir()
        (source / "sdk" / "share" / "doc" / "Vellum" / "NOTICE.md").write_text("Vellum notice\n")
        (source / "sdk" / "lib" / "libvellum-gpu.dylib").write_bytes(b"gpu")
        self.archive = self.root / "vellum-sdk-0.2.0-darwin-arm64.tar.gz"
        with tarfile.open(self.archive, "w:gz") as tar:
            tar.add(source / "sdk", arcname="sdk")
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.lock = self.root / "pulp.lock"
        self.lock.write_text(json.dumps({"vellum": {"tag": "v0.2.0", "sdk_digest": digest}}))

    def tearDown(self):
        self.tmp.cleanup()

    def test_matching_digest_stages_sdk_integrations_and_notice(self):
        prefix = self.root / "prefix"
        staged = bundle.bundle(lock_path=self.lock, prefix=prefix, platform="darwin-arm64",
                               archive=self.archive, archive_dir=None, integrations=self.integration)
        self.assertTrue((staged / "sdk" / "lib" / "libvellum-gpu.dylib").is_file())
        self.assertTrue((prefix / "share/pulp/integrations/pulp-vellum/bridge.hpp").is_file())
        self.assertIn("## Vellum SDK", (prefix / "NOTICE.md").read_text())

    def test_changed_archive_fails_closed(self):
        self.archive.write_bytes(self.archive.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "vellum.sdk_digest"):
            bundle.bundle(lock_path=self.lock, prefix=self.root / "prefix", platform="darwin-arm64",
                          archive=self.archive, archive_dir=None, integrations=self.integration)

    def test_missing_archive_names_lock_digest(self):
        digest = json.loads(self.lock.read_text())["vellum"]["sdk_digest"]
        with self.assertRaisesRegex(FileNotFoundError, "vellum.sdk_digest"):
            bundle.bundle(lock_path=self.lock, prefix=self.root / "prefix", platform="darwin-arm64",
                          archive=None, archive_dir=self.root / "missing", integrations=self.integration)
        self.assertTrue(digest)

    def test_unsupported_platform_is_refused_before_archive_lookup(self):
        with self.assertRaisesRegex(ValueError, "publishes no SDK for linux-x64 yet"):
            bundle.bundle(lock_path=self.lock, prefix=self.root / "prefix", platform="linux-x64",
                          archive=self.archive, archive_dir=None, integrations=self.integration)

    def test_tar_member_cannot_escape_destination(self):
        archive = self.root / "escape.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("../outside")
            info.size = 1
            tar.addfile(info, __import__("io").BytesIO(b"x"))
        with self.assertRaisesRegex(ValueError, "escapes destination"):
            bundle._safe_extract(archive, self.root / "destination")

    def test_symlink_member_cannot_escape_destination(self):
        archive = self.root / "link.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("sdk/link")
            info.type = tarfile.SYMTYPE
            info.linkname = "../../outside"
            tar.addfile(info)
        with self.assertRaisesRegex(ValueError, "link escapes destination"):
            bundle._safe_extract(archive, self.root / "destination")

    def test_existing_staged_sdk_is_not_overwritten(self):
        prefix = self.root / "prefix"
        bundle.bundle(lock_path=self.lock, prefix=prefix, platform="darwin-arm64",
                      archive=self.archive, archive_dir=None, integrations=self.integration)
        with self.assertRaisesRegex(FileExistsError, "refusing to overwrite staged Vellum SDK"):
            bundle.bundle(lock_path=self.lock, prefix=prefix, platform="darwin-arm64",
                          archive=self.archive, archive_dir=None, integrations=self.integration)

    def test_notice_merge_is_idempotent(self):
        prefix = self.root / "prefix"
        prefix.mkdir()
        staged = self.root / "staged"
        (staged / "sdk/share/doc/Vellum").mkdir(parents=True)
        (staged / "sdk/share/doc/Vellum/NOTICE.md").write_text("Vellum notice\n")
        bundle._merge_notice(prefix, staged)
        first = (prefix / "NOTICE.md").read_text()
        bundle._merge_notice(prefix, staged)
        self.assertEqual(first, (prefix / "NOTICE.md").read_text())


if __name__ == "__main__":
    unittest.main()
