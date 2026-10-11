#!/usr/bin/env python3
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MiseProfileTests(unittest.TestCase):
    def test_profile_is_opt_in_and_locked(self) -> None:
        text = (ROOT / ".mise.toml").read_text(encoding="utf-8")
        self.assertIn("auto_install = false", text)
        self.assertIn("locked = true", text)
        self.assertNotIn("auto_update", text)
        self.assertIn("install_visual_python_deps.sh", text)

    def test_no_lane_invokes_mise(self) -> None:
        for path in (ROOT / ".github", ROOT / ".shipyard", ROOT / "tools/ci"):
            if not path.exists():
                continue
            for file_path in path.rglob("*"):
                if file_path.is_file() and file_path.suffix in {".yml", ".yaml", ".toml", ".sh", ".py"}:
                    text = file_path.read_text(encoding="utf-8", errors="ignore")
                    if "mise run" in text:
                        self.fail(f"lane depends on mise: {file_path}")


if __name__ == "__main__":
    unittest.main()
