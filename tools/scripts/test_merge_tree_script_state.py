from __future__ import annotations

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import merge_tree_script_state as state


class MergeTreeScriptStateTests(unittest.TestCase):
    def test_run_does_not_mask_failure_with_not_checked_notice(self):
        with tempfile.TemporaryDirectory() as td:
            script = Path(td) / "failing.py"
            script.write_text("print('NOT CHECKED locally: compiled inventory'); raise SystemExit(1)\n")
            result = state._run([sys.executable, str(script)], Path(td), os.environ.copy())
            self.assertEqual(result, 1)

    def test_ledger_checker_is_selected_from_materialized_merge_tree(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            def git(*args: str) -> str:
                return subprocess.run(["git", *args], cwd=root, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-q", "-b", "main")
            git("config", "user.name", "test")
            git("config", "user.email", "test@example.invalid")
            checker = root / "tools/scripts/vellum_inheritance_ledger_check.py"
            checker.parent.mkdir(parents=True)
            checker.write_text(
                "import sys\n"
                "from pathlib import Path\n"
                "raise SystemExit(1 if Path('ledger.drift').exists() else 0)\n"
            )
            checker.chmod(checker.stat().st_mode | stat.S_IXUSR)
            (root / "ledger.drift").write_text("drift\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            (root / "head.txt").write_text("head\n")
            git("add", "head.txt")
            git("commit", "-qm", "head")
            result = subprocess.run(
                [sys.executable, str(state.__file__), "--repo-root", str(root),
                 "--base", base, "--head", "HEAD"],
                cwd=root, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("vellum-inheritance-ledger", result.stderr)
            self.assertIn(state.STALE, result.stderr)
            self.assertNotIn("consumption-census-schema", result.stderr)


if __name__ == "__main__":
    unittest.main()
