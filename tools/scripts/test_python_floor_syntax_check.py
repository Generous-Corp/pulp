#!/usr/bin/env python3
"""python_floor_syntax_check.py flags exactly what Python 3.11 cannot parse."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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
    "backslash in a nested f-string's text": "x = 1\ny = f\"{f'a\\\\n{x}'}\"\n",
    "continuation in a single-quoted field": 'x = 1\ny = f"{x + \\\n 1}"\n',
    "continuation in a triple-quoted field": 'x = 1\ny = f"""{x + \\\n 1}"""\n',
    "continuation right after the brace": 'x = 1\ny = f"""{\\\nx}"""\n',
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
    "backslash in a top-level format spec": "x = 1\ny = f'{x:\\\\n}'\n",
    "nested f-string without a backslash": "x = 1\ny = f\"{f'a{x}'}\"\n",
    "continuation outside the f-string": 'x = 1\ny = f"{x}" + \\\n  "a"\n',
    "continuation in the literal of a dedent block": 'x = 1\ny = f"""\\\n{x}\n"""\n',
    "line break in parens in a triple-quoted field": 'x = 1\ny = f"""{(x +\n 1)}"""\n',
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

    def test_the_tracked_tree_is_clean(self) -> None:
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        # Control: the scan really covered the tree.
        self.assertRegex(result.stdout, r"ok \(\d{4,} file\(s\)")


def _git(repo: Path, *args: str) -> str:
    env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@t", "GIT_CONFIG_NOSYSTEM": "1", "HOME": str(repo),
           "PATH": "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin"}
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True,
                          encoding="utf-8", env=env).stdout.strip()


class ChangedFromTests(unittest.TestCase):
    """--changed-from: the pre-push form. Diff-scoped, committed blobs only."""

    def make_repo(self, tmp: str) -> Path:
        repo = Path(tmp) / "repo"
        repo.mkdir()
        _git(repo, "init", "-q", "-b", "main")
        # A violation already on the base: an unrelated push must not be blocked by it.
        (repo / "old_bad.py").write_text(REJECTED["same quote in a field"], encoding="utf-8")
        _git(repo, "add", "old_bad.py")
        _git(repo, "commit", "-qm", "base")
        _git(repo, "checkout", "-qb", "feat")
        return repo

    def commit(self, repo: Path, rel: str, text: str) -> None:
        (repo / rel).parent.mkdir(parents=True, exist_ok=True)
        (repo / rel).write_text(text, encoding="utf-8")
        _git(repo, "add", rel)
        _git(repo, "commit", "-qm", rel)

    def violations(self, repo: Path) -> list[str] | None:
        files = floor.changed_python(repo, "main")
        if files is None:
            return None
        return [p for name, text in files for p in floor.floor_violations(text, name)]

    def test_a_changed_bad_file_is_flagged(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.make_repo(tmp)
            self.commit(repo, "tools/new_bad.py", REJECTED["same quote in a field"])
            found = self.violations(repo)
        self.assertEqual(len(found), 1, found)
        self.assertIn("tools/new_bad.py:2", found[0])

    def test_a_violation_only_on_the_base_does_not_block(self) -> None:
        # Negative control for the diff scope: old_bad.py is bad on main and untouched here.
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.make_repo(tmp)
            self.commit(repo, "tools/new_good.py", ACCEPTED["other quote in a field"])
            self.assertEqual(self.violations(repo), [])
            self.assertEqual(len(floor.changed_python(repo, "main")), 1)

    def test_the_committed_blob_is_judged_not_the_working_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.make_repo(tmp)
            self.commit(repo, "a.py", REJECTED["same quote in a field"])
            (repo / "a.py").write_text(ACCEPTED["other quote in a field"], encoding="utf-8")  # uncommitted fix
            self.assertEqual(len(self.violations(repo)), 1)

    def test_no_merge_base_is_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.make_repo(tmp)
            self.assertIsNone(floor.changed_python(repo, "no-such-ref"))

    def test_a_failed_blob_read_is_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.make_repo(tmp)
            self.commit(repo, "new.py", ACCEPTED["other quote in a field"])
            original = floor.subprocess.run

            def deny_show(cmd, *args, **kwargs):
                if "show" in cmd:
                    return subprocess.CompletedProcess(cmd, 1, stdout=b"", stderr=b"denied")
                return original(cmd, *args, **kwargs)

            with mock.patch.object(floor.subprocess, "run", side_effect=deny_show):
                self.assertIsNone(floor.changed_python(repo, "main"))

    def test_an_unsupported_git_path_is_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.make_repo(tmp)
            original = floor.subprocess.run

            def bad_path_diff(cmd, *args, **kwargs):
                if "diff" in cmd:
                    return subprocess.CompletedProcess(cmd, 0, stdout=b"bad_\xff.py\0", stderr=b"")
                return original(cmd, *args, **kwargs)

            with mock.patch.object(floor.subprocess, "run", side_effect=bad_path_diff):
                self.assertIsNone(floor.changed_python(repo, "main"))

    def test_the_cli_exits_1_on_a_changed_bad_file_and_2_without_a_base(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.make_repo(tmp)
            script = repo / "tools/scripts/python_floor_syntax_check.py"
            script.parent.mkdir(parents=True)
            shutil.copy2(floor.__file__, script)
            self.commit(repo, "tools/scripts/python_floor_syntax_check.py", script.read_text(encoding="utf-8"))
            self.commit(repo, "x.py", REJECTED["comment in a field"])
            bad = subprocess.run([sys.executable, str(script), "--changed-from", "main"],
                                 capture_output=True, text=True, encoding="utf-8")
            missing = subprocess.run([sys.executable, str(script), "--changed-from", "nope"],
                                     capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(bad.returncode, 1, bad.stderr)
        self.assertIn("x.py", bad.stderr)
        self.assertNotIn("old_bad.py", bad.stderr)
        self.assertEqual(missing.returncode, 2, missing.stderr)


if __name__ == "__main__":
    unittest.main()
