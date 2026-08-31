#!/usr/bin/env python3
"""Self-test for setup_bootstrap_plan_guard.py.

A guard that cannot fail is worth nothing, so every case here regresses a
copy of the real setup.sh in a scratch directory and requires the guard to
reject it. The unmutated copy is the control: if that one does not pass, the
mutants prove nothing.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GUARD = REPO_ROOT / "tools" / "scripts" / "setup_bootstrap_plan_guard.py"
SETUP = REPO_ROOT / "setup.sh"


def run_guard(setup: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(GUARD), "--setup", str(setup)],
        capture_output=True,
        text=True,
        timeout=300,
    )


class SetupBootstrapPlanGuardTest(unittest.TestCase):
    def _copy(self, tmp: str) -> Path:
        target = Path(tmp) / "setup.sh"
        shutil.copy2(SETUP, target)
        return target

    def _assert_rejects(self, old: str, new: str, expect: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            setup = self._copy(tmp)
            text = setup.read_text()
            self.assertIn(old, text, f"anchor not found in setup.sh: {old!r}")
            setup.write_text(text.replace(old, new, 1))
            result = run_guard(setup)
            self.assertEqual(
                result.returncode,
                1,
                f"guard accepted a regressed setup.sh\n{result.stdout}{result.stderr}",
            )
            self.assertIn(expect, result.stdout + result.stderr)

    def test_unmutated_copy_passes(self) -> None:
        """Control — without this the rejections below could be anything."""
        with tempfile.TemporaryDirectory() as tmp:
            result = run_guard(self._copy(tmp))
            self.assertEqual(
                result.returncode, 0, f"{result.stdout}{result.stderr}"
            )

    def test_repo_setup_passes(self) -> None:
        result = run_guard(SETUP)
        self.assertEqual(result.returncode, 0, f"{result.stdout}{result.stderr}")

    def test_debug_default_is_rejected(self) -> None:
        self._assert_rejects("BUILD_TYPE=Release\n", "BUILD_TYPE=Debug\n", "default build type")

    def test_examples_on_by_default_is_rejected(self) -> None:
        self._assert_rejects("BUILD_EXAMPLES=OFF\n", "BUILD_EXAMPLES=ON\n", "default examples setting")

    def test_configure_dropping_examples_flag_is_rejected(self) -> None:
        """The printed plan and the real configure can drift; both are checked."""
        self._assert_rejects(
            '    -DPULP_BUILD_EXAMPLES="$BUILD_EXAMPLES"',
            "",
            "configure does not pass",
        )

    def test_configure_hardcoding_a_build_type_is_rejected(self) -> None:
        self._assert_rejects(
            '-DCMAKE_BUILD_TYPE="$BUILD_TYPE"',
            "-DCMAKE_BUILD_TYPE=Debug",
            "configure does not pass",
        )

    def test_build_claiming_its_own_parallelism_is_rejected(self) -> None:
        self._assert_rejects(
            'cmake --build "$REPO_ROOT/build"',
            'cmake --build "$REPO_ROOT/build" -j"$(sysctl -n hw.ncpu)"',
            "sets its own parallelism",
        )

    def test_broken_debug_override_is_rejected(self) -> None:
        self._assert_rejects(
            "        --debug)   BUILD_TYPE=Debug ;;\n",
            "        --debug)   : ;;\n",
            "--debug resolved to",
        )

    def test_broken_examples_override_is_rejected(self) -> None:
        self._assert_rejects(
            "        --examples) BUILD_EXAMPLES=ON ;;\n",
            "        --examples) : ;;\n",
            "--examples resolved to",
        )


if __name__ == "__main__":
    unittest.main()
