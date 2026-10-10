#!/usr/bin/env python3
from __future__ import annotations
import hashlib, tempfile, unittest
from pathlib import Path
import generate

class GenerateTests(unittest.TestCase):
    def fixture(self, root: Path):
        (root / "tools/toolchain").mkdir(parents=True)
        (root / "tools/toolchain/manifest.toml").write_text(generate.SOURCE.read_text(), encoding="utf-8")
        (root / ".shipyard").mkdir()
        body = "header\n" + generate.BEGIN + "\nold\n" + generate.END + "\nfooter\n"
        (root / ".shipyard/vm-image.toml").write_text(body)
        (root / ".shipyard/vm-image.intel.toml").write_text(body)
    def test_five_runs_are_byte_identical(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); self.fixture(root)
            generate.generate(root, None, False)
            first=(root/".shipyard/vm-image.toml").read_bytes()
            for _ in range(4): generate.generate(root, None, False)
            self.assertEqual(first, (root/".shipyard/vm-image.toml").read_bytes())
            generate.generate(root, None, True)
    def test_missing_authority_field_refuses(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); (root/"tools/toolchain").mkdir(parents=True)
            (root/"tools/toolchain/manifest.toml").write_text('schema=1\n[toolchain]\nxcode="26.5"\n')
            with self.assertRaises(ValueError): generate.load(root/"tools/toolchain/manifest.toml")
    def test_planted_hand_edit_refuses(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); self.fixture(root); generate.generate(root,None,False)
            path=root/".shipyard/vm-image.toml"; path.write_text(path.read_text().replace('cmake = "4.4.3"','cmake = "4.3.3"'), encoding="utf-8")
            with self.assertRaises(ValueError): generate.generate(root,None,True)
    def test_source_digest_is_in_generated_block(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); self.fixture(root); generate.generate(root,None,False)
            digest=hashlib.sha256((root/"tools/toolchain/manifest.toml").read_bytes()).hexdigest()
            self.assertIn(f"source_sha256 = {digest}", (root/".shipyard/vm-image.toml").read_text())
if __name__ == "__main__": unittest.main()
