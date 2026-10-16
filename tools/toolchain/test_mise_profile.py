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
        # A pinned mise never asks for a newer release.
        self.assertIn("disable_update_warning = true", text)
        # mise ignores self_update.* in a project file and warns on every run.
        self.assertNotRegex(text, r"(?m)^\s*self_update")
        self.assertIn("install_visual_python_deps.sh", text)

    def test_visual_task_uses_authoritative_hashed_lock(self) -> None:
        installer = (ROOT / "tools/ci/install_visual_python_deps.sh").read_text(encoding="utf-8")
        self.assertIn("--require-hashes", installer)
        self.assertIn("tools/motion/visual/requirements.lock", installer)
        self.assertTrue((ROOT / "tools/motion/visual/requirements.lock").is_file())

    def test_mise_is_credited_as_developer_tooling(self) -> None:
        # mise is developer-installed and never redistributed, so it is
        # credited in DEPENDENCIES.md and the licensing reference, not in
        # NOTICE.md, which covers redistributed code.
        self.assertIn("| mise | MIT |", (ROOT / "DEPENDENCIES.md").read_text(encoding="utf-8"))
        self.assertIn("[jdx/mise]", (ROOT / "docs/reference/licensing.md").read_text(encoding="utf-8"))

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
