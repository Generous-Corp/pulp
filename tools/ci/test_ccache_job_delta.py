#!/usr/bin/env python3
"""Tests for tools/ci/ccache_job_delta.py.

What must hold:
- the per-job line is the difference of two `--print-stats` snapshots, not the
  cumulative counters either snapshot carries;
- hits are direct + preprocessed, misses are `cache_miss`, and the rate is
  computed over the job's own cacheable calls;
- a counter that went backwards (cache zeroed between snapshots) reads as the
  after value, never negative;
- a missing or counter-less snapshot exits 2 and prints no per-job line, so an
  absent line can never be read as a hit.

Run:
    python3 tools/ci/test_ccache_job_delta.py
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ccache_job_delta as cjd  # noqa: E402

SCRIPT = HERE / "ccache_job_delta.py"


def snapshot(**counters: int) -> str:
    base = {
        "autoconf_test": 0, "cache_miss": 0, "called_for_link": 0,
        "direct_cache_hit": 0, "preprocessed_cache_hit": 0,
        "could_not_use_precompiled_header": 0, "files_in_cache": 1371887,
        "stats_updated_timestamp": 1790537689,
    }
    base.update(counters)
    return "".join(f"{k}\t{v}\n" for k, v in sorted(base.items()))


class ParseTests(unittest.TestCase):
    def test_tab_separated_integers_parse_and_other_lines_are_ignored(self) -> None:
        parsed = cjd.parse_print_stats("cache_miss\t12\nnot a stat line\nfoo\tbar\n")
        self.assertEqual(parsed, {"cache_miss": 12})

    def test_old_spellings_map_to_current_keys(self) -> None:
        parsed = cjd.parse_print_stats("cache_hit_direct\t3\ncache_hit_preprocessed\t1\n")
        self.assertEqual(parsed, {"direct_cache_hit": 3, "preprocessed_cache_hit": 1})


class DeltaTests(unittest.TestCase):
    def test_per_job_counts_are_the_difference_not_the_cumulative_value(self) -> None:
        before = cjd.parse_print_stats(snapshot(direct_cache_hit=67000, cache_miss=142,
                                                preprocessed_cache_hit=10, called_for_link=5))
        after = cjd.parse_print_stats(snapshot(direct_cache_hit=77016, cache_miss=177,
                                               preprocessed_cache_hit=10, called_for_link=7))
        d = cjd.delta(before, after)
        self.assertEqual(d["hits"], 10016)
        self.assertEqual(d["misses"], 35)
        self.assertEqual(d["cacheable"], 10051)
        self.assertEqual(d["uncacheable"], 2)
        self.assertEqual(cjd.render(d),
                         "ccache per-job: cacheable 10051, hits 10016 (99.65%), misses 35, uncacheable 2")

    def test_a_zeroed_cache_reads_as_the_after_value_never_negative(self) -> None:
        before = cjd.parse_print_stats(snapshot(direct_cache_hit=67000, cache_miss=142))
        after = cjd.parse_print_stats(snapshot(direct_cache_hit=9000, cache_miss=20))
        d = cjd.delta(before, after)
        self.assertEqual((d["hits"], d["misses"]), (9000, 20))

    def test_no_cacheable_calls_renders_a_zero_rate(self) -> None:
        d = cjd.delta({}, {"cache_miss": 0, "direct_cache_hit": 0})
        self.assertIn("hits 0 (0.00%)", cjd.render(d))


class CommandTests(unittest.TestCase):
    def run_tool(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              capture_output=True, text=True, timeout=60)

    def test_prints_one_grep_line_and_one_json_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            b = Path(tmp, "before.txt"); a = Path(tmp, "after.txt")
            b.write_text(snapshot(direct_cache_hit=100, cache_miss=1), encoding="utf-8")
            a.write_text(snapshot(direct_cache_hit=160, cache_miss=4), encoding="utf-8")
            proc = self.run_tool(str(b), str(a))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        lines = proc.stdout.splitlines()
        self.assertEqual(lines[0], "ccache per-job: cacheable 63, hits 60 (95.24%), misses 3, uncacheable 0")
        self.assertTrue(lines[1].startswith("ccache per-job json: {"), lines)
        self.assertIn('"misses": 3', lines[1])

    def test_missing_snapshot_exits_2_without_a_per_job_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            a = Path(tmp, "after.txt")
            a.write_text(snapshot(direct_cache_hit=1), encoding="utf-8")
            proc = self.run_tool(str(Path(tmp, "absent.txt")), str(a))
        self.assertEqual(proc.returncode, 2)
        self.assertNotIn("ccache per-job: cacheable", proc.stdout)
        self.assertIn("unreadable", proc.stderr)

    def test_snapshot_without_counters_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            b = Path(tmp, "before.txt"); a = Path(tmp, "after.txt")
            b.write_text("ccache not available\n", encoding="utf-8")
            a.write_text(snapshot(direct_cache_hit=1), encoding="utf-8")
            proc = self.run_tool(str(b), str(a))
        self.assertEqual(proc.returncode, 2)
        self.assertIn("no ccache counters", proc.stderr)


if __name__ == "__main__":
    unittest.main()
