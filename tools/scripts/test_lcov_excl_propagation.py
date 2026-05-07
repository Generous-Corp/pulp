#!/usr/bin/env python3
"""Contract test for issue #1058: LCOV_EXCL markers must propagate.

Background
----------
`llvm-cov export --format=lcov` is not gcov-aware; it does NOT honor
`LCOV_EXCL_START` / `LCOV_EXCL_STOP` comments in source. Pulp's coverage
pipeline historically went straight from llvm-cov to lcov_cobertura.py,
so any LCOV_EXCL block was silently documentation-only.

Fix (Path A from issue #1058)
-----------------------------
Pipe the raw .lcov through `lcov --remove <raw> '<no-op-pattern>'
--filter region --output-file <out>` so lcov's source-aware filter
strips lines that fall inside an `LCOV_EXCL_START..STOP` range before
the cobertura conversion sees them.

What this test verifies
-----------------------
1. Build a synthetic source file with a known LCOV_EXCL_START/STOP
   block on a specific line range.
2. Synthesize a matching .lcov tracefile that includes hit counts for
   lines INSIDE the excluded range.
3. Run the same `lcov --filter region` invocation the production shell
   scripts use.
4. Assert the excluded line numbers are absent from the output and
   non-excluded lines remain.

Skipped cleanly when `lcov` is not on PATH so this test does not block
contributors / CI lanes that haven't installed lcov yet.

Run:
    python3 tools/scripts/test_lcov_excl_propagation.py
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import unittest


LCOV_AVAILABLE = shutil.which("lcov") is not None


@unittest.skipUnless(LCOV_AVAILABLE, "lcov not installed (brew install lcov / apt install lcov)")
class LcovExclPropagation(unittest.TestCase):
    """`lcov --filter region` strips LCOV_EXCL'd line ranges from .lcov."""

    def test_lcov_excl_block_strips_lines_from_lcov_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = pathlib.Path(tmp_str)

            # Source with a clearly delineated EXCL block on lines 6-8.
            source = tmp / "sample.cpp"
            source.write_text(textwrap.dedent(
                """\
                int keep_me(int x) {
                    return x + 1;
                }

                // LCOV_EXCL_START
                int drop_me(int x) {
                    return x * 2;
                }
                // LCOV_EXCL_STOP

                int also_keep(int x) {
                    return x - 1;
                }
                """
            ))

            # Synthesize a tracefile with hit counts on lines 2 (kept),
            # 7 (excluded — inside the EXCL block), and 12 (kept).
            raw_lcov = tmp / "coverage.raw.lcov"
            raw_lcov.write_text(
                "TN:\n"
                f"SF:{source}\n"
                "DA:2,5\n"
                "DA:7,3\n"
                "DA:12,1\n"
                "LF:3\n"
                "LH:3\n"
                "end_of_record\n"
            )

            filtered_lcov = tmp / "coverage.lcov"

            result = subprocess.run(
                [
                    "lcov",
                    "--remove", str(raw_lcov),
                    "/__pulp_unmatched__/*",
                    "--output-file", str(filtered_lcov),
                    "--filter", "region",
                    "--ignore-errors", "unused",
                    "--rc", "branch_coverage=1",
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(
                result.returncode, 0,
                msg=f"lcov failed: stdout={result.stdout!r} stderr={result.stderr!r}",
            )
            self.assertTrue(
                filtered_lcov.exists(),
                msg="lcov did not write output file",
            )

            content = filtered_lcov.read_text()

            # Excluded line (inside LCOV_EXCL_START..STOP) MUST be gone.
            self.assertNotIn(
                "DA:7,",
                content,
                msg=(
                    "Line 7 is inside an LCOV_EXCL_START..STOP block but still "
                    "appears in the filtered output. The toolchain is not "
                    "honoring LCOV_EXCL markers — issue #1058 has regressed."
                ),
            )

            # Non-excluded lines must survive.
            self.assertIn("DA:2,5", content, msg="non-excluded line 2 disappeared")
            self.assertIn("DA:12,1", content, msg="non-excluded line 12 disappeared")

            # Line totals should reflect the drop (3 → 2).
            self.assertIn(
                "LF:2",
                content,
                msg="line count summary did not update after filter",
            )


if __name__ == "__main__":
    unittest.main()
