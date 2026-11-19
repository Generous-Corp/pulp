#!/usr/bin/env python3
"""Fixture tests for tools/scripts/regen_agent_capability_after_merge.py.

Each test builds a throwaway git repository whose "main" and PR branch both
edit the two generated capability trees, merges main into the PR, and drives
the real script against a small stand-in for agent_capability_manifest.py
(passed through --manifest) so no real SDK state is touched.

Run:
    python3 tools/scripts/test_regen_agent_capability_after_merge.py
"""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("regen_agent_capability_after_merge.py")

INCLUDE = "pulp/host/h.hpp"
HEADER = "core/host/include/" + INCLUDE
FRAGMENT = "tools/agent-capabilities/fingerprints/" + INCLUDE + ".json"
SURFACE = "docs/status/agent-capability-surface/headers/" + INCLUDE + ".json"

FAKE_MANIFEST = '''#!/usr/bin/env python3
import hashlib, pathlib, sys
root = pathlib.Path.cwd()
inc = "pulp/host/h.hpp"
header = root / "core/host/include" / inc
frag = root / "tools/agent-capabilities/fingerprints" / (inc + ".json")
surf = root / "docs/status/agent-capability-surface/headers" / (inc + ".json")
def digest():
    return hashlib.sha256(header.read_bytes()).hexdigest()
mode = sys.argv[1]
if mode == "--accept-fingerprint":
    frag.write_text(digest() + "\\n")
elif mode == "--write":
    surf.write_text(frag.read_text())
elif mode == "--check":
    bad = []
    if frag.read_text().strip() != digest():
        bad.append("public header fingerprint changed: " + inc + "; expected x, got y")
    if surf.read_text() != frag.read_text():
        bad.append("surface stale")
    print("\\n".join(bad))
    sys.exit(1 if bad else 0)
'''


def git(root, *args, check=True):
    return subprocess.run(["git", "-C", str(root), *args], text=True,
                          capture_output=True, check=check)


def write(root, rel, text):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def header_text(first="a", fifth="e"):
    return "\n".join([first, "b", "c", "d", fifth]) + "\n"


class Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name) / "repo"
        self.root.mkdir()
        git(self.root, "init", "-q", "-b", "main")
        git(self.root, "config", "user.email", "t@example.invalid")
        git(self.root, "config", "user.name", "t")
        git(self.root, "config", "rerere.enabled", "false")
        write(self.root, "fake_manifest.py", FAKE_MANIFEST)
        write(self.root, HEADER, header_text())
        self.sync = self.digest(header_text()) + "\n"
        write(self.root, FRAGMENT, self.sync)
        write(self.root, SURFACE, self.sync)
        write(self.root, "src.txt", "1\n2\n3\n4\n5\n")
        self.commit("base")

    def tearDown(self):
        self._tmp.cleanup()

    def commit(self, msg):
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", msg)

    def digest(self, text):
        return hashlib.sha256(text.encode()).hexdigest()

    def advance_main(self, files):
        git(self.root, "checkout", "-q", "main")
        for rel, text in files.items():
            write(self.root, rel, text)
        self.commit("main change")
        git(self.root, "update-ref", "refs/remotes/origin/main", "main")

    def run_script(self, *extra):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--manifest", str(self.root / "fake_manifest.py"),
             *extra],
            cwd=self.root, text=True, capture_output=True)

    def standard_scenario(self):
        """Main and the PR both rewrite the generated files; the header merges clean."""
        git(self.root, "branch", "-q", "pr")
        self.advance_main({HEADER: header_text(first="A"), FRAGMENT: "fmain\n",
                           SURFACE: "fmain\n"})
        git(self.root, "checkout", "-q", "pr")
        write(self.root, HEADER, header_text(fifth="E"))
        write(self.root, FRAGMENT, "fpr\n")
        write(self.root, SURFACE, "fpr\n")
        write(self.root, "other.txt", "pr only\n")
        self.commit("pr change")
        self.pr_head = git(self.root, "rev-parse", "HEAD").stdout.strip()


class RegenTests(Fixture):
    def test_generated_conflict_takes_main_then_requires_commit_then_regenerates(self):
        self.standard_scenario()
        git(self.root, "merge", "-q", "--no-commit", "origin/main", check=False)
        self.assertTrue((self.root / ".git/MERGE_HEAD").exists())

        first = self.run_script()
        self.assertEqual(first.returncode, 4, first.stderr)
        self.assertIn("commit the merge", first.stderr)
        self.assertEqual((self.root / FRAGMENT).read_text(), "fmain\n")
        self.assertEqual((self.root / SURFACE).read_text(), "fmain\n")
        self.assertEqual(git(self.root, "diff", "--name-only", "--diff-filter=U").stdout, "")

        git(self.root, "commit", "-q", "-m", "merge main")
        second = self.run_script("--previous-head", self.pr_head)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("other.txt", second.stdout)
        digest = self.digest((self.root / HEADER).read_text())
        self.assertEqual((self.root / FRAGMENT).read_text().strip(), digest)
        self.assertEqual((self.root / SURFACE).read_text(), (self.root / FRAGMENT).read_text())
        self.assertTrue((self.root / "other.txt").exists())

    def test_unresolved_source_conflict_refuses(self):
        git(self.root, "branch", "-q", "pr")
        self.advance_main({"src.txt": "1\nmain\n3\n4\n5\n"})
        git(self.root, "checkout", "-q", "pr")
        write(self.root, "src.txt", "1\npr\n3\n4\n5\n")
        self.commit("pr change")
        git(self.root, "merge", "-q", "--no-commit", "origin/main", check=False)
        result = self.run_script()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("src.txt", result.stderr)
        # The refusal must not have touched the conflicted source file.
        self.assertIn("<<<<<<<", (self.root / "src.txt").read_text())

    def test_committed_conflict_markers_outside_generated_trees_refuse(self):
        git(self.root, "branch", "-q", "pr")
        self.advance_main({"src.txt": "9\n"})
        git(self.root, "checkout", "-q", "pr")
        write(self.root, "src.txt", "<<<<<<< ours\nx\n=======\ny\n>>>>>>> theirs\n")
        self.commit("pr change")
        result = self.run_script()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("src.txt", result.stderr)

    def test_empty_diff_result_fails(self):
        git(self.root, "branch", "-q", "pr")
        self.advance_main({FRAGMENT: self.sync, SURFACE: self.sync, "src.txt": "m\n"})
        git(self.root, "checkout", "-q", "pr")
        # The PR only ever touched the two generated files.
        write(self.root, FRAGMENT, "fpr\n")
        write(self.root, SURFACE, "fpr\n")
        self.commit("pr change")
        prev = git(self.root, "rev-parse", "HEAD").stdout.strip()
        git(self.root, "merge", "-q", "--no-commit", "origin/main", check=False)
        self.assertEqual(self.run_script().returncode, 4)
        git(self.root, "commit", "-q", "-m", "merge main")
        result = self.run_script("--previous-head", prev)
        self.assertEqual(result.returncode, 5, result.stdout + result.stderr)
        self.assertIn("empty", result.stderr.lower())

    def test_lost_more_than_half_fails(self):
        self.standard_scenario()
        # Previous head claimed many files relative to main; fake it with a head
        # that touches five files while the current tree touches fewer.
        git(self.root, "checkout", "-q", "-b", "wide", self.pr_head)
        for name in "abcde":
            write(self.root, f"wide_{name}.txt", name)
        self.commit("wide")
        wide = git(self.root, "rev-parse", "HEAD").stdout.strip()
        git(self.root, "checkout", "-q", "pr")
        git(self.root, "merge", "-q", "--no-commit", "origin/main", check=False)
        self.run_script()
        git(self.root, "commit", "-q", "-m", "merge main")
        result = self.run_script("--previous-head", wide)
        self.assertEqual(result.returncode, 5, result.stdout + result.stderr)
        self.assertIn("lost", result.stderr)

    def test_no_conflict_is_a_noop(self):
        git(self.root, "branch", "-q", "pr")
        self.advance_main({"src.txt": "1\n2\n3\n4\nmain\n"})
        git(self.root, "checkout", "-q", "pr")
        write(self.root, "other.txt", "pr only\n")
        self.commit("pr change")
        git(self.root, "merge", "-q", "-m", "merge main", "origin/main")
        before = git(self.root, "status", "--porcelain").stdout
        head = git(self.root, "rev-parse", "HEAD").stdout
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(git(self.root, "status", "--porcelain").stdout, before)
        self.assertEqual(git(self.root, "rev-parse", "HEAD").stdout, head)


if __name__ == "__main__":
    unittest.main()
