#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

import generate


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
            f"{generate.BEGIN}\nold\n{generate.END}\nfooter\n"
        )
        (root / ".shipyard/vm-image.toml").write_text(body, encoding="utf-8")
        (root / ".shipyard/vm-image.intel.toml").write_text(body, encoding="utf-8")

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
                path.read_text(encoding="utf-8").replace('cmake = "homebrew"', 'cmake = "brew-current"'),
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


if __name__ == "__main__":
    unittest.main()
