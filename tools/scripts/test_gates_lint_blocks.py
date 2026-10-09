#!/usr/bin/env python3
"""Each lint block in gates.sh must turn the lint's failure into a gate failure.

Two branches adding lint blocks at the same spot in gates.sh merge into a
splice that can drop one block's `fail=1 / fi / fi`. This runs every
`if [ -f "$X" ]; then ... "$PYTHON" "$X" ...` block on its own under bash,
with a stand-in interpreter that fails and one that passes, and asserts the
block sets `fail=1` exactly when the lint fails.
"""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GATES = ROOT / "tools/scripts/gates.sh"
BLOCK_START = re.compile(r'^if \[ -f "\$(?P<var>[A-Z0-9_]+)" \]; then$')


def lint_blocks(text: str) -> dict[str, str]:
    """Top-level enforcing `if [ -f "$VAR" ]` blocks that run "$PYTHON" "$VAR"."""
    blocks: dict[str, str] = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        match = BLOCK_START.match(lines[i])
        if not match:
            i += 1
            continue
        var = match.group("var")
        j = i + 1
        while j < len(lines) and lines[j] != "fi":
            j += 1
        body = "\n".join(lines[i:j + 1])
        # The plain shape: every *_LINT, and any block that runs
        # `if ! "$PYTHON" "$VAR" ...; then fail=1`. Blocks with their own
        # exit-code mapping or skip conditions, and advisory blocks, are not
        # this shape and are left to their own tests.
        enforcing = var.endswith("_LINT") or (
            f'if ! "$PYTHON" "${var}"' in body and "fail=1" in body)
        if f'"$PYTHON" "${var}"' in body and enforcing:
            blocks[var] = body
        i = j + 1
    return blocks


def run_block(block: str, var: str, interpreter: str) -> str:
    with tempfile.TemporaryDirectory() as directory:
        lint = pathlib.Path(directory) / "lint.py"
        lint.write_text("", encoding="utf-8")
        script = (
            f'fail=0\nROOT="{directory}"\nBASE=HEAD\nPYTHON="{interpreter}"\n'
            f'{var}="{lint}"\n{block}\necho "fail=$fail"\n'
        )
        result = subprocess.run(["bash", "-c", script], capture_output=True,
                                encoding="utf-8", timeout=60)
        return result.stdout.strip().splitlines()[-1] if result.stdout.strip() else result.stderr


@unittest.skipUnless(shutil.which("bash"), "gates.sh is a bash script")
class LintBlockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.blocks = lint_blocks(GATES.read_text(encoding="utf-8"))

    def test_the_known_lint_blocks_are_found(self) -> None:
        # Control: the parser must see the lints it exists to guard.
        for var in ("WIN32_INCLUDE_LINT", "SAFE_PATH_LINT", "TEXT_ENCODING_LINT",
                    "RAW_PID_PROBE_LINT"):
            self.assertIn(var, self.blocks)

    def test_every_lint_failure_fails_the_gate(self) -> None:
        for var, block in self.blocks.items():
            with self.subTest(var):
                self.assertEqual(run_block(block, var, "false"), "fail=1")

    def test_a_passing_lint_leaves_the_gate_alone(self) -> None:
        for var, block in self.blocks.items():
            with self.subTest(var):
                self.assertEqual(run_block(block, var, "true"), "fail=0")

    def test_a_dropped_fail_assignment_is_caught(self) -> None:
        # Negative control: a block whose fail=1 went missing in a merge must
        # not read as a gate failure.
        block = self.blocks["SAFE_PATH_LINT"].replace("        fail=1\n", "        :\n", 1)
        self.assertEqual(run_block(block, "SAFE_PATH_LINT", "false"), "fail=0")


if __name__ == "__main__":
    unittest.main()
