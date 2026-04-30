#!/usr/bin/env python3
"""Fixture tests for tools/scripts/lcov_strip_excl.py.

Locks in the LCOV_EXCL propagation contract that closes pulp #1058.

`llvm-cov export --format=lcov` does NOT honor `LCOV_EXCL_LINE` or
`LCOV_EXCL_START..STOP` markers. Without `lcov_strip_excl.py`, lines
inside an LCOV_EXCL block leak through into the Cobertura XML and the
diff-cover "missing lines" report — turning the markers into
documentation-only with no runtime effect.

These tests fail loudly the moment that propagation step is removed
from either `tools/scripts/local_diff_cover.sh` or
`scripts/run_coverage.sh`, or if the strip helper itself stops
honoring a marker.

Run:
    python3 tools/scripts/test_lcov_strip_excl.py
"""

from __future__ import annotations

import importlib.util
import io
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from xml.etree import ElementTree as ET


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
STRIP_SCRIPT = REPO_ROOT / "tools" / "scripts" / "lcov_strip_excl.py"
LOCAL_SCRIPT = REPO_ROOT / "tools" / "scripts" / "local_diff_cover.sh"
CI_SCRIPT = REPO_ROOT / "scripts" / "run_coverage.sh"
CONVERTER = REPO_ROOT / "tools" / "scripts" / "lcov_cobertura.py"


def _import_strip_module():
    """Import lcov_strip_excl as a module so we can call filter_lcov() in-process.

    The script doubles as a CLI; importing lets the unit tests skip
    subprocess overhead for the inner-loop assertions.
    """
    spec = importlib.util.spec_from_file_location(
        "lcov_strip_excl", str(STRIP_SCRIPT)
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lcov_strip_excl = _import_strip_module()


# ---------------------------------------------------------------------------
# Marker-grammar tests — feed source text directly to compute_exclusions
# ---------------------------------------------------------------------------

class ComputeExclusionsTests(unittest.TestCase):
    """compute_exclusions() honors every LCOV_EXCL marker variant."""

    def test_lcov_excl_line_marks_only_that_line(self) -> None:
        src = "\n".join([
            "int a;",            # 1
            "int b; // LCOV_EXCL_LINE",  # 2
            "int c;",            # 3
        ])
        em = lcov_strip_excl.compute_exclusions(src)
        self.assertEqual(em.lines, {2})

    def test_lcov_excl_start_stop_block_inclusive(self) -> None:
        src = "\n".join([
            "int a;",                       # 1
            "// LCOV_EXCL_START",           # 2  (excluded — marker line)
            "int b;",                       # 3  (excluded)
            "int c;",                       # 4  (excluded)
            "// LCOV_EXCL_STOP",            # 5  (excluded — marker line)
            "int d;",                       # 6
        ])
        em = lcov_strip_excl.compute_exclusions(src)
        self.assertEqual(em.lines, {2, 3, 4, 5})

    def test_lcov_excl_br_line_branch_only(self) -> None:
        src = "\n".join([
            "int a;",                          # 1
            "if (x) { y(); } // LCOV_EXCL_BR_LINE",  # 2
            "int c;",                          # 3
        ])
        em = lcov_strip_excl.compute_exclusions(src)
        self.assertEqual(em.lines, set(),
                         "BR_LINE excludes branches only, not lines")
        self.assertEqual(em.branch_lines, {2})

    def test_orphan_stop_warns_and_no_exclusion(self) -> None:
        src = "// LCOV_EXCL_STOP\nint a;\n"
        em = lcov_strip_excl.compute_exclusions(src)
        self.assertEqual(em.lines, set())

    def test_unterminated_start_excludes_to_eof(self) -> None:
        # lcov(1) behavior: orphan START excludes everything to EOF.
        src = "int a;\n// LCOV_EXCL_START\nint b;\nint c;\n"
        em = lcov_strip_excl.compute_exclusions(src)
        # Lines 2, 3, 4 should be excluded.
        self.assertIn(2, em.lines)
        self.assertIn(3, em.lines)
        self.assertIn(4, em.lines)

    def test_word_boundary_does_not_match_substring(self) -> None:
        # `LCOV_EXCL_LINE_FOO` is NOT a real marker; must not trigger.
        src = "int a; // LCOV_EXCL_LINE_FOO\n"
        em = lcov_strip_excl.compute_exclusions(src)
        self.assertEqual(em.lines, set())


# ---------------------------------------------------------------------------
# End-to-end .lcov filter tests — fixtures + filter_lcov()
# ---------------------------------------------------------------------------

def _make_lcov_for(src_path: str, lines: list) -> str:
    """Build a minimal .lcov record covering `lines` of file `src_path`.

    `lines` is a list of (line_number, hits) pairs.
    """
    da = "\n".join(f"DA:{n},{hits}" for n, hits in lines)
    lf = len(lines)
    lh = sum(1 for _, h in lines if h > 0)
    return f"TN:\nSF:{src_path}\n{da}\nLF:{lf}\nLH:{lh}\nend_of_record\n"


class FilterLcovTests(unittest.TestCase):
    """filter_lcov() drops DA records on LCOV_EXCL'd lines.

    This is THE anti-drift test for #1058 — if the propagation step
    ever stops working, this fails loudly with an actionable message.
    """

    def test_lines_inside_lcov_excl_block_do_not_appear_in_filtered_output(self) -> None:
        # Source: 6 lines, lines 3-5 wrapped in LCOV_EXCL_START..STOP.
        src = "\n".join([
            "int a;",              # 1
            "int b;",              # 2
            "// LCOV_EXCL_START",  # 3
            "int c;",              # 4
            "// LCOV_EXCL_STOP",   # 5
            "int d;",              # 6
        ])
        # Pretend llvm-cov-export emitted DA records for every line —
        # this is the shape that exposes the bug: lines 3-5 are
        # PRESENT in the .lcov even though they're LCOV_EXCL'd in src.
        lcov_in = _make_lcov_for("/fake/foo.cpp", [
            (1, 1), (2, 1), (3, 0), (4, 0), (5, 0), (6, 1),
        ])
        filtered = lcov_strip_excl.filter_lcov(
            lcov_in,
            source_lookup=lambda p: src if p == "/fake/foo.cpp" else None,
        )
        # Lines 3, 4, 5 must be gone.
        for excluded in (3, 4, 5):
            self.assertNotIn(
                f"DA:{excluded},", filtered,
                f"line {excluded} is inside LCOV_EXCL_START..STOP but "
                f"still appears in filtered .lcov — propagation broken; "
                f"see pulp #1058",
            )
        # Lines 1, 2, 6 must remain.
        for kept in (1, 2, 6):
            self.assertIn(f"DA:{kept},", filtered)

    def test_lf_and_lh_recomputed_after_strip(self) -> None:
        src = "// LCOV_EXCL_START\nint a;\n// LCOV_EXCL_STOP\nint b;\n"
        lcov_in = _make_lcov_for("/fake/bar.cpp", [
            (1, 0), (2, 0), (3, 0), (4, 1),
        ])
        # All 4 lines counted in the input.
        self.assertIn("LF:4", lcov_in)
        self.assertIn("LH:1", lcov_in)
        filtered = lcov_strip_excl.filter_lcov(
            lcov_in,
            source_lookup=lambda p: src if p == "/fake/bar.cpp" else None,
        )
        # After filtering, only line 4 remains. LF must be 1, LH must be 1.
        self.assertIn("LF:1", filtered)
        self.assertIn("LH:1", filtered)
        # And the stale totals must NOT be present.
        self.assertNotIn("LF:4", filtered)

    def test_brda_records_filtered_on_excluded_line(self) -> None:
        src = "int a;\n// LCOV_EXCL_LINE\nint c;\n"
        # Line 2 has a branch record we need to drop.
        lcov_in = (
            "TN:\n"
            "SF:/fake/branchy.cpp\n"
            "DA:1,1\n"
            "DA:2,0\n"
            "DA:3,1\n"
            "BRDA:2,0,0,1\n"
            "BRDA:2,0,1,0\n"
            "BRDA:3,0,0,1\n"
            "LF:3\nLH:2\n"
            "BRF:3\nBRH:2\n"
            "end_of_record\n"
        )
        filtered = lcov_strip_excl.filter_lcov(
            lcov_in,
            source_lookup=lambda p: src if p == "/fake/branchy.cpp" else None,
        )
        # Line-2 DA gone, line-2 BRDA gone, line-3 BRDA kept.
        self.assertNotIn("DA:2,", filtered)
        self.assertNotIn("BRDA:2,", filtered)
        self.assertIn("BRDA:3,", filtered)

    def test_no_markers_passes_through_unchanged(self) -> None:
        # When the source has no LCOV_EXCL markers, nothing must change.
        src = "int a;\nint b;\nint c;\n"
        lcov_in = _make_lcov_for("/fake/clean.cpp", [(1, 1), (2, 1), (3, 1)])
        filtered = lcov_strip_excl.filter_lcov(
            lcov_in,
            source_lookup=lambda p: src if p == "/fake/clean.cpp" else None,
        )
        for n in (1, 2, 3):
            self.assertIn(f"DA:{n},1", filtered)
        self.assertIn("LF:3", filtered)
        self.assertIn("LH:3", filtered)

    def test_unreadable_source_passes_records_through(self) -> None:
        # If we can't open the source file (vendored / generated /
        # outside-tree), records pass through — the strip helper must
        # NOT drop coverage just because it failed to read the source.
        lcov_in = _make_lcov_for("/no/such/file.cpp", [(1, 1), (2, 1)])
        filtered = lcov_strip_excl.filter_lcov(
            lcov_in,
            source_lookup=lambda p: None,
        )
        self.assertIn("DA:1,1", filtered)
        self.assertIn("DA:2,1", filtered)


# ---------------------------------------------------------------------------
# End-to-end pipeline test — strip → cobertura. This is THE acceptance
# criterion in pulp #1058: lines inside LCOV_EXCL must not appear in
# the resulting Cobertura XML.
# ---------------------------------------------------------------------------

class CoberturaPropagationTests(unittest.TestCase):
    """Verify LCOV_EXCL'd lines are stripped before reaching cobertura XML.

    Invokes the strip helper as a subprocess (matching the way
    local_diff_cover.sh / run_coverage.sh use it), then runs the
    same lcov_cobertura.py converter, then asserts the excluded
    line numbers are absent from the resulting <line number="..."/>
    elements.
    """

    def test_lines_inside_lcov_excl_block_do_not_appear_in_cobertura(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)

            # 1. Create a source file with an LCOV_EXCL'd block.
            src_path = tmp / "subject.cpp"
            src_path.write_text(
                "int a = 1;\n"             # 1  kept
                "int b = 2;\n"             # 2  kept
                "// LCOV_EXCL_START\n"     # 3  excluded
                "int c = 3;\n"             # 4  excluded
                "int d = 4;\n"             # 5  excluded
                "// LCOV_EXCL_STOP\n"      # 6  excluded
                "int e = 5;\n"             # 7  kept
            )

            # 2. Synthesize the .lcov shape llvm-cov-export would emit
            #    — note that lines 3-6 are PRESENT, exposing the bug.
            raw_lcov = tmp / "raw.lcov"
            raw_lcov.write_text(
                f"TN:\n"
                f"SF:{src_path}\n"
                "DA:1,1\nDA:2,1\nDA:3,0\nDA:4,0\nDA:5,0\nDA:6,0\nDA:7,1\n"
                "LF:7\nLH:3\n"
                "end_of_record\n"
            )

            # 3. Run the strip helper as a subprocess (mirrors how the
            #    real pipeline uses it).
            filt_lcov = tmp / "filtered.lcov"
            result = subprocess.run(
                [sys.executable, str(STRIP_SCRIPT),
                 str(raw_lcov), str(filt_lcov)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(filt_lcov.exists())

            # 4. Convert filtered .lcov → Cobertura XML.
            xml_out = tmp / "cobertura.xml"
            result = subprocess.run(
                [sys.executable, str(CONVERTER), str(filt_lcov),
                 "--output", str(xml_out),
                 "--base-dir", tmpdir],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(xml_out.exists())

            # 5. Parse the XML and collect every <line number="..."/>.
            tree = ET.parse(xml_out)
            line_numbers = {
                int(el.get("number"))
                for el in tree.iter("line")
                if el.get("number") is not None
            }

            # Excluded lines must NOT be present. This is the
            # acceptance criterion from pulp #1058.
            for excluded in (3, 4, 5, 6):
                self.assertNotIn(
                    excluded, line_numbers,
                    f"line {excluded} is inside LCOV_EXCL_START..STOP but "
                    f"still appears in cobertura XML — propagation broken; "
                    f"see pulp #1058. If you reverted the strip step in "
                    f"local_diff_cover.sh / run_coverage.sh, restore it.",
                )

            # Sanity: lines we KEPT must be present.
            for kept in (1, 2, 7):
                self.assertIn(
                    kept, line_numbers,
                    f"line {kept} should remain in cobertura XML but is missing",
                )


# ---------------------------------------------------------------------------
# Wiring tests — both pipeline scripts must invoke the strip helper.
# Anti-drift guard: if a future edit drops the strip step from one of
# the scripts, the propagation gap reopens silently. These tests fail
# the moment that wire is cut.
# ---------------------------------------------------------------------------

class PipelineWiringTests(unittest.TestCase):
    """tools/scripts/local_diff_cover.sh and scripts/run_coverage.sh
    must both run the LCOV_EXCL strip step BEFORE invoking the
    Cobertura converter (pulp #1058).
    """

    def test_local_script_invokes_strip_helper(self) -> None:
        text = LOCAL_SCRIPT.read_text()
        self.assertIn(
            "lcov_strip_excl.py", text,
            "tools/scripts/local_diff_cover.sh must invoke "
            "lcov_strip_excl.py between `llvm-cov export` and the "
            "Cobertura converter — without it, LCOV_EXCL_START..STOP "
            "markers leak into the diff-cover report (pulp #1058).",
        )

    def test_ci_script_invokes_strip_helper(self) -> None:
        text = CI_SCRIPT.read_text()
        self.assertIn(
            "lcov_strip_excl.py", text,
            "scripts/run_coverage.sh must invoke lcov_strip_excl.py "
            "between `llvm-cov export` and the Cobertura converter — "
            "without it, LCOV_EXCL_START..STOP markers leak into the "
            "Cobertura XML uploaded to Codecov (pulp #1058).",
        )

    @staticmethod
    def _exec_pos(text: str, var_name: str, needle: str) -> int:
        """Return the offset of the FIRST `python3 "${VAR}"` invocation.

        Variable assignments like `LCOV_COBERTURA="${REPO_ROOT}/.../lcov_cobertura.py"`
        appear textually before the actual subprocess call. We want to
        compare the order of EXECUTION — so we look for either:
          1. `python3 "${LCOV_FOO}" ...`  (variable form, what we use), OR
          2. `python3 [path/to/]<needle>` (literal form, future-proof).
        """
        # Variable form: python3 "${LCOV_STRIP_EXCL}"
        var_form = re.compile(
            r"python3\s+\"\$\{" + re.escape(var_name) + r"\}\"",
        )
        # Literal form: python3 path/to/lcov_strip_excl.py
        lit_form = re.compile(
            r"python3\s+(?:\")?(?:[^\s\"]*?/)?" + re.escape(needle),
        )
        positions = []
        m = var_form.search(text)
        if m:
            positions.append(m.start())
        m = lit_form.search(text)
        if m:
            positions.append(m.start())
        return min(positions) if positions else -1

    def test_strip_runs_before_cobertura_in_local_script(self) -> None:
        # Order matters: strip must happen BEFORE the converter sees
        # the file, otherwise the excluded lines are already in the
        # XML by the time we'd strip.
        text = LOCAL_SCRIPT.read_text()
        strip_idx = self._exec_pos(text, "LCOV_STRIP_EXCL", "lcov_strip_excl.py")
        cobertura_idx = self._exec_pos(text, "LCOV_COBERTURA", "lcov_cobertura.py")
        self.assertGreater(strip_idx, 0,
                           "no `python3 ... lcov_strip_excl.py` invocation")
        self.assertGreater(cobertura_idx, 0,
                           "no `python3 ... lcov_cobertura.py` invocation")
        self.assertLess(
            strip_idx, cobertura_idx,
            "lcov_strip_excl.py must run BEFORE lcov_cobertura.py in "
            "tools/scripts/local_diff_cover.sh — otherwise excluded "
            "lines reach the Cobertura XML (pulp #1058).",
        )

    def test_strip_runs_before_cobertura_in_ci_script(self) -> None:
        text = CI_SCRIPT.read_text()
        strip_idx = self._exec_pos(text, "LCOV_STRIP_EXCL", "lcov_strip_excl.py")
        cobertura_idx = self._exec_pos(text, "LCOV_COBERTURA", "lcov_cobertura.py")
        self.assertGreater(strip_idx, 0,
                           "no `python3 ... lcov_strip_excl.py` invocation")
        self.assertGreater(cobertura_idx, 0,
                           "no `python3 ... lcov_cobertura.py` invocation")
        self.assertLess(
            strip_idx, cobertura_idx,
            "lcov_strip_excl.py must run BEFORE lcov_cobertura.py in "
            "scripts/run_coverage.sh — otherwise excluded lines reach "
            "the Cobertura XML uploaded to Codecov (pulp #1058).",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
