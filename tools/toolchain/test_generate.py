#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import re
import stat
import tempfile
import unittest
from pathlib import Path

import generate

REPO = generate.ROOT


class GenerateTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        (root / "tools/toolchain").mkdir(parents=True)
        (root / "tools/toolchain/manifest.toml").write_text(
            generate.SOURCE.read_text(encoding="utf-8"), encoding="utf-8"
        )
        (root / ".shipyard").mkdir()
        body = (
            "header\n[toolchain]\n"
            "# handwritten sentinel survives generation\n"
            "handwritten = true\n\n"
            "[brew]\npackages = [\n  \"old\",\n]\n\n"
            f"{generate.BEGIN}\nold\n{generate.END}\nfooter\n"
        )
        (root / ".shipyard/vm-image.toml").write_text(body, encoding="utf-8")
        (root / ".shipyard/vm-image.intel.toml").write_text(body, encoding="utf-8")
        for name in (generate.MISE_CONFIG, generate.MISE_LOCK):
            (root / name).write_bytes((REPO / name).read_bytes())

    def test_five_runs_are_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            first = (root / ".shipyard/vm-image.toml").read_bytes()
            for _ in range(4):
                generate.generate(root, None, False)
            self.assertEqual(first, (root / ".shipyard/vm-image.toml").read_bytes())
            generate.generate(root, None, True)

    def test_non_owned_keys_and_comments_survive(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            text = (root / ".shipyard/vm-image.intel.toml").read_text(encoding="utf-8")
            self.assertIn("# handwritten sentinel survives generation", text)
            self.assertIn("handwritten = true", text)
            self.assertNotIn('xcode = "26.5"', text)

    def test_platform_non_owned_fields_survive(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            tart = root / "tartci"
            (tart / "manifests").mkdir(parents=True)
            (tart / "manifests/pulp.macos.toml").write_text(
                f"[toolchain]\n\n[brew]\npackages = [\n  \"old\",\n]\n\n{generate.BEGIN}\nold\n{generate.END}\n",
                encoding="utf-8",
            )
            for platform, manager, extra in (("linux", "apt", "ninja = \"system\"\n"), ("windows", "choco", "msvc = \"14.44\"\nninja = \"system\"\n")):
                path = tart / "manifests" / f"pulp.{platform}.toml"
                path.write_text(
                    f"[toolchain]\n{extra}\n[{manager}]\npackages = [\n  \"old\",\n]\n\n{generate.BEGIN}\nold\n{generate.END}\n",
                    encoding="utf-8",
                )
            generate.generate(root, tart, False)
            linux = (tart / "manifests/pulp.linux.toml").read_text(encoding="utf-8")
            windows = (tart / "manifests/pulp.windows.toml").read_text(encoding="utf-8")
            self.assertIn('ninja = "system"', linux)
            self.assertIn('msvc = "14.44"', windows)
            self.assertNotIn("rust_targets", linux)
            self.assertNotIn("rust_targets", windows)

    def test_missing_authority_field_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "tools/toolchain").mkdir(parents=True)
            (root / "tools/toolchain/manifest.toml").write_text(
                'schema=1\n[toolchain]\nxcode="26.5"\n', encoding="utf-8"
            )
            with self.assertRaises(ValueError):
                generate.load(root / "tools/toolchain/manifest.toml")

    def test_planted_hand_edit_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            path = root / ".shipyard/vm-image.toml"
            path.write_text(
                path.read_text(encoding="utf-8").replace('cmake = "4.3.3"', 'cmake = "4.3.4"'),
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                generate.generate(root, None, True)

    def test_source_digest_is_in_generated_block(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            digest = hashlib.sha256(
                (root / "tools/toolchain/manifest.toml").read_bytes()
            ).hexdigest()
            text = (root / ".shipyard/vm-image.toml").read_text(encoding="utf-8")
            self.assertIn(f"source_sha256 = {digest}", text)



class StrictPinTests(unittest.TestCase):
    """The manifest pins exact versions, and every generated file agrees."""

    def fixture(self, root: Path) -> None:
        GenerateTests.fixture(self, root)  # type: ignore[arg-type]
        generate.generate(root, None, False)

    def assert_check_refuses(self, root: Path, needle: str) -> None:
        with self.assertRaises(ValueError) as ctx:
            generate.generate(root, None, True)
        self.assertIn(needle, str(ctx.exception))

    def test_checked_in_files_match_the_manifest(self) -> None:
        # The drift test proper: the committed .mise.toml, mise.lock and VM
        # image files are exactly what the committed manifest generates.
        generate.generate(REPO, None, True)

    def test_floating_selectors_are_refused(self) -> None:
        source = generate.SOURCE.read_text(encoding="utf-8")
        for key, floating in (("cmake", "homebrew"), ("rust", "stable"), ("python", "3.12"), ("tart", "latest")):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as td:
                path = Path(td) / "manifest.toml"
                path.write_text(
                    re.sub(rf'^{key} = "[^"]*"', f'{key} = "{floating}"', source, count=1, flags=re.M),
                    encoding="utf-8",
                )
                with self.assertRaises(ValueError) as ctx:
                    generate.load(path)
                self.assertIn("non-exact", str(ctx.exception))

    def test_hand_edited_mise_tools_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            path = root / generate.MISE_CONFIG
            path.write_text(path.read_text(encoding="utf-8").replace('ninja = "1.13.2"', 'ninja = "1.13.1"'), encoding="utf-8")
            self.assert_check_refuses(root, "generated mise block is stale or hand-edited")

    def test_hand_edited_lock_version_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            path = root / generate.MISE_LOCK
            path.write_text(path.read_text(encoding="utf-8").replace('version = "4.3.3"', 'version = "4.3.4"', 1), encoding="utf-8")
            self.assert_check_refuses(root, "cmake is locked at '4.3.4'")

    def test_hand_edited_lock_checksum_refuses(self) -> None:
        # A checksum edit keeps the lock structurally valid; only the lock
        # digest recorded in .mise.toml can see it.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            path = root / generate.MISE_LOCK
            text = path.read_text(encoding="utf-8")
            digest = re.search(r'checksum = "sha256:([0-9a-f]{64})"', text).group(1)
            path.write_text(text.replace(digest, "0" * 64, 1), encoding="utf-8")
            generate.check_mise_lock(path, generate.load(root / "tools/toolchain/manifest.toml")[0])
            self.assert_check_refuses(root, "generated mise block is stale or hand-edited")

    def test_lock_missing_a_platform_checksum_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            path = root / generate.MISE_LOCK
            text = path.read_text(encoding="utf-8")
            text = re.sub(r'(\[tools\.node\."platforms\.windows-arm64"\]\n)checksum = "[^"]*"\n', r"\1", text)
            path.write_text(text, encoding="utf-8")
            data = generate.load(root / "tools/toolchain/manifest.toml")[0]
            with self.assertRaises(ValueError) as ctx:
                generate.check_mise_lock(path, data)
            self.assertIn("node@26.10.0 has no checksum and url for windows-arm64", str(ctx.exception))

    def test_lock_with_an_unmanifested_tool_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            path = root / generate.MISE_LOCK
            path.write_text(path.read_text(encoding="utf-8") + '\n[[tools.jq]]\nversion = "1.8.1"\nbackend = "aqua:jqlang/jq"\n', encoding="utf-8")
            data = generate.load(root / "tools/toolchain/manifest.toml")[0]
            with self.assertRaises(ValueError) as ctx:
                generate.check_mise_lock(path, data)
            self.assertIn("tools not in the manifest: jq", str(ctx.exception))

    def test_every_manifest_tool_is_in_the_generated_profile(self) -> None:
        data, _ = generate.load()
        text = (REPO / generate.MISE_CONFIG).read_text(encoding="utf-8")
        for key, (name, _backend, _checksum, _os) in generate.MISE_TOOLS.items():
            self.assertRegex(text, rf'(?m)^"?{re.escape(name)}"? = .*"{re.escape(data["toolchain"][key])}"')
        self.assertIn(data["mise"]["version"], re.search(r"(?m)^min_version = (.*)$", text).group(1))
        if data["mise"]["enforcement"] == "warn":
            self.assertIn("soft =", text)
        for prefix in data["npm"]["prefixes"]:
            self.assertTrue((REPO / prefix / "package-lock.json").is_file(), prefix)
            self.assertIn(f"npm ci --prefix {prefix}", text)

    def test_lock_is_written_only_by_the_pinned_mise(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            fake = root / "mise"
            fake.write_text("#!/bin/sh\necho 2026.5.16 macos-arm64\n", encoding="utf-8")
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            before = (root / generate.MISE_LOCK).read_bytes()
            with self.assertRaises(ValueError) as ctx:
                generate.generate(root, None, False, mise=fake)
            self.assertIn("must be written by mise", str(ctx.exception))
            self.assertEqual(before, (root / generate.MISE_LOCK).read_bytes())


if __name__ == "__main__":
    unittest.main()
