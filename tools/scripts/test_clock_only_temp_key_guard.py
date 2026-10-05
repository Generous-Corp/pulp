#!/usr/bin/env python3
"""Self-test for clock_only_temp_key_guard.py."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import clock_only_temp_key_guard as guard  # noqa: E402

CLOCK_ONLY = textwrap.dedent('''
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("pulp-x-" + std::to_string(stamp));
        fs::create_directories(path);
    }
''')


def repo(files: dict[str, str]) -> tempfile.TemporaryDirectory:
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    return tmp


class SiteTests(unittest.TestCase):
    def test_a_clock_only_key_is_a_site(self) -> None:
        self.assertEqual(guard.sites(CLOCK_ONLY.splitlines()), [3])

    def test_a_pid_serial_or_the_helper_makes_it_unique(self) -> None:
        for extra in ("std::to_string(::getpid())", "counter.fetch_add(1)",
                      "std::random_device{}()", "current_process_id()"):
            with self.subTest(extra=extra):
                text = CLOCK_ONLY.replace('std::to_string(stamp)',
                                          f'std::to_string(stamp) + {extra}')
                self.assertEqual(guard.sites(text.splitlines()), [])
        helper = 'path = pulp::test::make_unique_temp_dir("pulp-x");\n'
        self.assertEqual(guard.sites(helper.splitlines()), [])

    def test_a_clock_read_with_no_temp_path_nearby_is_not_a_site(self) -> None:
        text = "auto t0 = std::chrono::steady_clock::now().time_since_epoch();\n" * 3
        self.assertEqual(guard.sites(text.splitlines()), [])

    def test_the_skip_marker_exempts_its_line(self) -> None:
        text = CLOCK_ONLY.replace(".count();", ".count();  // clock-only-temp-key-guard: skip x")
        self.assertEqual(guard.sites(text.splitlines()), [])


class ScanTests(unittest.TestCase):
    def test_only_test_sources_are_scanned_and_the_exit_codes(self) -> None:
        with repo({"test/test_x.cpp": CLOCK_ONLY, "core/x.cpp": CLOCK_ONLY}) as td:
            found, seen = guard.scan(Path(td))
            self.assertEqual([(str(p), line) for p, line in found], [("test/test_x.cpp", 3)])
            self.assertEqual(seen, 1)
            self.assertEqual(guard.main(["--root", td, "--ledger", str(Path(td) / "none.json")]), 1)
        fixed = 'auto path = pulp::test::make_unique_temp_dir("x");\nauto t = fs::temp_directory_path();\n'
        with repo({"test/test_x.cpp": fixed}) as td:
            self.assertEqual(guard.main(["--root", td, "--ledger", str(Path(td) / "none.json")]), 0)

    def test_no_temp_paths_is_an_instrument_failure_not_a_pass(self) -> None:
        with repo({"test/test_x.cpp": "int main() {}\n"}) as td:
            self.assertEqual(guard.main(["--root", td, "--ledger", str(Path(td) / "none.json")]), 2)

    def test_this_checkout_is_clean(self) -> None:
        found, seen = guard.scan(Path(__file__).resolve().parents[2])
        self.assertGreater(seen, 100)
        self.assertEqual(guard.judge(found, guard.load_ledger(guard.LEDGER)), [])


class LedgerTests(unittest.TestCase):
    def test_every_listed_file_says_why(self) -> None:
        ledger = Path(__file__).resolve().parents[2] / "tools/scripts/clock_only_temp_key_guard.json"
        self.assertEqual(ledger, guard.LEDGER)
        for name, entry in json.loads(ledger.read_text())["sites"].items():
            with self.subTest(name=name):
                self.assertGreater(entry["count"], 0)
                self.assertGreater(len(entry["reason"]), 20)

    def run_with(self, files: dict[str, str], counts: dict[str, int]) -> int:
        with repo(files) as td:
            ledger = Path(td) / "ledger.json"
            ledger.write_text(json.dumps({"schema": 1, "sites": {
                name: {"count": count, "reason": "r"} for name, count in counts.items()}}))
            return guard.main(["--root", td, "--ledger", str(ledger)])

    def test_a_listed_site_passes_and_a_new_one_beside_it_fails(self) -> None:
        self.assertEqual(self.run_with({"test/test_x.cpp": CLOCK_ONLY}, {"test/test_x.cpp": 1}), 0)
        self.assertEqual(self.run_with({"test/test_x.cpp": CLOCK_ONLY + CLOCK_ONLY},
                                       {"test/test_x.cpp": 1}), 1)
        self.assertEqual(self.run_with({"test/test_y.cpp": CLOCK_ONLY},
                                       {"test/test_x.cpp": 1}), 1)

    def test_a_count_that_shrank_must_be_lowered(self) -> None:
        fixed = 'auto p = pulp::test::make_unique_temp_dir("x");\nauto t = fs::temp_directory_path();\n'
        self.assertEqual(self.run_with({"test/test_x.cpp": fixed}, {"test/test_x.cpp": 1}), 1)


if __name__ == "__main__":
    unittest.main()
