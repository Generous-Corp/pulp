#!/usr/bin/env python3
import hashlib
import io
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
        (self.integration / "pulp-vellum" / "bridge.hpp").write_text("bridge\n", encoding="utf-8")
        source = self.root / "source"
        (source / "sdk" / "share" / "doc" / "Vellum").mkdir(parents=True)
        (source / "sdk" / "lib").mkdir()
        (source / "sdk" / "share" / "doc" / "Vellum" / "NOTICE.md").write_text("Vellum notice\n", encoding="utf-8")
        (source / "sdk" / "lib" / "libvellum-gpu.dylib").write_bytes(b"gpu")
        self.archive = self.root / "vellum-sdk-0.2.0-darwin-arm64.tar.gz"
        with tarfile.open(self.archive, "w:gz") as tar:
            tar.add(source / "sdk", arcname="sdk")
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.lock = self.root / "pulp.lock"
        self.lock.write_text(json.dumps({"vellum": {"tag": "v0.2.0", "sdk_digest": digest, "sdk_url": "https://example.invalid/vellum-sdk.tar.gz"}}), encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_matching_digest_stages_sdk_integrations_and_notice(self):
        prefix = self.root / "prefix"
        staged = bundle.bundle(lock_path=self.lock, prefix=prefix, platform="darwin-arm64",
                               archive=self.archive, archive_dir=None, integrations=self.integration)
        self.assertTrue((staged / "sdk" / "lib" / "libvellum-gpu.dylib").is_file())
        self.assertTrue((prefix / "share/pulp/integrations/pulp-vellum/bridge.hpp").is_file())
        self.assertIn("## Vellum SDK", (prefix / "NOTICE.md").read_text(encoding="utf-8"))

    def test_changed_archive_fails_closed(self):
        self.archive.write_bytes(self.archive.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "vellum.sdk_digest"):
            bundle.bundle(lock_path=self.lock, prefix=self.root / "prefix", platform="darwin-arm64",
                          archive=self.archive, archive_dir=None, integrations=self.integration)

    def test_missing_archive_names_lock_digest(self):
        digest = json.loads(self.lock.read_text(encoding="utf-8"))["vellum"]["sdk_digest"]
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

    def test_hardlink_name_is_resolved_from_extraction_root(self):
        archive = self.root / "hardlink.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("sdk/link")
            info.type = tarfile.LNKTYPE
            info.linkname = "../outside"
            tar.addfile(info)
        with self.assertRaisesRegex(ValueError, "link escapes destination"):
            bundle._safe_extract(archive, self.root / "destination")

    def test_member_written_through_symlink_is_refused(self):
        archive = self.root / "symlink-follow.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            link = tarfile.TarInfo("sdk/link")
            link.type = tarfile.SYMTYPE
            link.linkname = "real"
            tar.addfile(link)
            data = tarfile.TarInfo("sdk/link/file")
            data.size = 1
            tar.addfile(data, io.BytesIO(b"x"))
        with self.assertRaisesRegex(ValueError, "follows symlink"):
            bundle._safe_extract(archive, self.root / "destination")

    def test_special_member_is_refused_on_every_python(self):
        archive = self.root / "fifo.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("sdk/pipe")
            info.type = tarfile.FIFOTYPE
            tar.addfile(info)
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            bundle._safe_extract(archive, self.root / "destination")

    def test_hardlink_to_symlink_member_is_refused(self):
        archive = self.root / "hardlink-symlink.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            link = tarfile.TarInfo("sdk/link")
            link.type = tarfile.SYMTYPE
            link.linkname = "real"
            tar.addfile(link)
            hard = tarfile.TarInfo("sdk/hard")
            hard.type = tarfile.LNKTYPE
            hard.linkname = "sdk/link"
            tar.addfile(hard)
        with self.assertRaisesRegex(ValueError, "hardlink targets a symlink"):
            bundle._safe_extract(archive, self.root / "destination")

    def test_setuid_and_world_write_bits_are_stripped(self):
        archive = self.root / "setuid.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("sdk/tool")
            info.size = 1
            info.mode = 0o6777
            tar.addfile(info, io.BytesIO(b"x"))
        destination = self.root / "destination"
        bundle._safe_extract(archive, destination)
        mode = (destination / "sdk" / "tool").stat().st_mode & 0o7777
        self.assertEqual(mode & 0o7022, 0, oct(mode))

    def test_failed_staging_leaves_no_partial_sdk(self):
        prefix = self.root / "prefix"
        original = bundle._merge_notice

        def fail(*args, **kwargs):
            raise ValueError("synthetic notice failure")

        bundle._merge_notice = fail
        try:
            with self.assertRaisesRegex(ValueError, "synthetic notice failure"):
                bundle.bundle(lock_path=self.lock, prefix=prefix, platform="darwin-arm64",
                              archive=self.archive, archive_dir=None, integrations=self.integration)
        finally:
            bundle._merge_notice = original
        self.assertFalse((prefix / "share/pulp/vellum-sdk/0.2.0").exists())
        # A rerun must be able to stage the SDK after the failed attempt.
        self.assertTrue(bundle.bundle(lock_path=self.lock, prefix=prefix, platform="darwin-arm64",
                                      archive=self.archive, archive_dir=None, integrations=self.integration).is_dir())

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
        (staged / "sdk/share/doc/Vellum/NOTICE.md").write_text("Vellum notice\n", encoding="utf-8")
        bundle._merge_notice(prefix, staged)
        first = (prefix / "NOTICE.md").read_text(encoding="utf-8")
        bundle._merge_notice(prefix, staged)
        self.assertEqual(first, (prefix / "NOTICE.md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
