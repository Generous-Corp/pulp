#!/usr/bin/env python3
"""python_floor_syntax_check.py flags exactly what Python 3.11 cannot parse."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import python_floor_syntax_check as floor  # noqa: E402

# Python 3.12 accepts each of these; Python 3.11 rejects each.
REJECTED = {
    "same quote in a field": 'row = {"a": 1}\nprint(f"{row["a"]}")\n',
    "same quote, single-quoted": "row = {'a': 1}\nprint(f'{row['a']}')\n",
    "triple quote reused": 'x = 1\ny = f"""{"""z"""}"""\n',
    "outer quote in a nested f-string": 'x = 1\ny = f"{f"{x}"}"\n',
    "backslash in a field": "xs = ['a']\ny = f\"{'\\n'.join(xs)}\"\n",
    "comment in a field": 'x = 1\ny = f"""{x  # note\n}"""\n',
    "line break in a single-quoted field": 'x = 1\ny = f"{x\n}"\n',
}
# Both versions accept each of these.
ACCEPTED = {
    "other quote in a field": "row = {'a': 1}\nprint(f\"{row['a']}\")\n",
    "single quote inside a triple-quoted f-string": 'x = {"a": 1}\ny = f"""{x["a"]}"""\n',
    "nested format spec": 'x, w = 1.5, 6\ny = f"{x:>{w}.2f}"\n',
    "nested f-string, other quote": "x = 1\ny = f\"{f'{x}'}\"\n",
    "backslash in the literal part": 'x = 1\ny = f"a\\tb{x}"\n',
    "line break in a triple-quoted field": 'x = 1\ny = f"""{x\n}"""\n',
    "plain string with a backslash": 'y = "a\\nb"\n',
}


def find_python_311() -> str | None:
    for name in ("python3.11",):
        found = shutil.which(name)
        if found:
            return found
    return None


class FloorViolationTests(unittest.TestCase):
    def test_each_rejected_construct_is_flagged(self) -> None:
        for name, source in REJECTED.items():
            with self.subTest(name=name):
                self.assertTrue(floor.floor_violations(source, name), source)

    def test_each_accepted_construct_is_clean(self) -> None:
        for name, source in ACCEPTED.items():
            with self.subTest(name=name):
                self.assertEqual(floor.floor_violations(source, name), [])

    def test_control_the_running_interpreter_parses_every_sample(self) -> None:
        # The rejected samples must be valid on the interpreter the check runs
        # under (3.12+), or the cases above prove nothing about the scan.
        if sys.version_info < (3, 12):
            self.skipTest("the token scan only runs on Python 3.12+")
        for name, source in {**REJECTED, **ACCEPTED}.items():
            with self.subTest(name=name):
                compile(source, name, "exec", dont_inherit=True)

    def test_samples_agree_with_a_real_python_311(self) -> None:
        # The ground truth: what an actual 3.11 compiles. Skips (not passes)
        # where no 3.11 is installed; the hosted runners have one on PATH only
        # sometimes, so the case above carries the check when it is absent.
        py311 = find_python_311()
        if py311 is None:
            self.skipTest("no python3.11 on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            for expected_ok, samples in ((False, REJECTED), (True, ACCEPTED)):
                for name, source in samples.items():
                    with self.subTest(name=name):
                        path = Path(tmp) / "sample.py"
                        path.write_text(source, encoding="utf-8")
                        result = subprocess.run(
                            [py311, "-c", "import sys; compile(open(sys.argv[1], encoding='utf-8').read(), sys.argv[1], 'exec')", str(path)],
                            capture_output=True, text=True, encoding="utf-8", check=False)
                        self.assertEqual(result.returncode == 0, expected_ok, result.stderr)

    def test_a_repeated_construct_on_one_line_is_reported_once(self) -> None:
        source = 'r = {"a": 1}\nprint(f"{r["a"]}{r["a"]}")\n'
        self.assertEqual(len(floor.floor_violations(source, "x")), 1)


class CommandLineTests(unittest.TestCase):
    def run_check(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(Path(floor.__file__)), *args],
                              capture_output=True, text=True, encoding="utf-8", check=False)

    def test_a_bad_file_fails_and_names_the_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.py"
            bad.write_text(REJECTED["same quote in a field"], encoding="utf-8")
            result = self.run_check(str(bad))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("bad.py:2", result.stderr)

    def test_a_clean_file_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "good.py"
            good.write_text(ACCEPTED["other quote in a field"], encoding="utf-8")
            result = self.run_check(str(good))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_the_tracked_tools_tree_is_clean(self) -> None:
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        # Control: the scan really covered the tree.
        self.assertRegex(result.stdout, r"ok \(\d{4,} file\(s\)")


if __name__ == "__main__":
    unittest.main()
