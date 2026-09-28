#!/usr/bin/env python3
"""Tests for tools/ci/ios_gate_digest.py (shadow-mode iOS gate identity).

What must hold:
- the digest is a function of the gate's inputs and the toolchain only: a
  docs, workflow, skill or tools/scripts change leaves it unchanged; a change
  to a core source, a CMake file, a dependency pin, the gate's own script, a
  path in IOS_COMPILE_REQUIRED_PATTERNS, or the toolchain id changes it;
- the same tree digests identically from two clones (no path or time input);
- lookup reports a hit only for an unexpired artifact with the exact name;
- the annotation is a `notice` carrying the schema, mode, verdict and digest.

Run:
    python3 tools/ci/test_ios_gate_digest.py
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ios_gate_digest as igd  # noqa: E402

TOOLCHAIN = "Xcode 27.0 | 27.0 | 27.0"


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                          check=True, env={"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                                           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
                                           "PATH": "/usr/bin:/bin:/opt/homebrew/bin"}).stdout


def write(repo: Path, rel: str, text: str) -> None:
    p = repo / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


class Fixture:
    def __init__(self, tmp: Path) -> None:
        self.repo = tmp / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        for rel, text in {
            "CMakeLists.txt": "project(pulp)\n",
            "core/midi/src/midi.cpp": "int a;\n",
            "core/midi/include/pulp/midi/midi.hpp": "#pragma once\n",
            "core/README.md": "notes\n",
            "examples/ios-gpu/CMakeLists.txt": "add_executable(x x.mm)\n",
            "tools/deps/manifest.json": "{}\n",
            "test/cmake/test_ios_compile_gate.sh": "#!/bin/bash\n",
            "test/ios/harness.mm": "// harness\n",
            "test/test_biquad.cpp": "// not an iOS input\n",
            "docs/guide.md": "docs\n",
            ".github/workflows/build.yml": "on: push\n",
            "tools/scripts/classify_changes.py": "# unrelated tool\n",
            ".agents/skills/ci/SKILL.md": "skill\n",
        }.items():
            write(self.repo, rel, text)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "base")

    def digest(self) -> str:
        return igd.compute(self.repo, "HEAD", TOOLCHAIN)["digest"]

    def commit(self, rel: str, text: str) -> None:
        write(self.repo, rel, text)
        git(self.repo, "add", rel)
        git(self.repo, "commit", "-q", "-m", f"touch {rel}")


class DigestTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fx = Fixture(Path(tmp.name))
        self.base = self.fx.digest()

    def test_non_inputs_leave_the_digest_unchanged(self) -> None:
        for rel in ("docs/guide.md", ".github/workflows/build.yml", "core/README.md",
                    "tools/scripts/classify_changes.py", ".agents/skills/ci/SKILL.md",
                    "test/test_biquad.cpp"):
            self.fx.commit(rel, f"changed {rel}\n")
            self.assertEqual(self.fx.digest(), self.base, rel)

    def test_each_input_class_changes_the_digest(self) -> None:
        seen = {self.base}
        for rel in ("core/midi/src/midi.cpp", "core/midi/include/pulp/midi/midi.hpp",
                    "CMakeLists.txt", "examples/ios-gpu/CMakeLists.txt",
                    "tools/deps/manifest.json", "test/cmake/test_ios_compile_gate.sh",
                    "test/ios/harness.mm"):
            self.fx.commit(rel, f"changed {rel}\n")
            d = self.fx.digest()
            self.assertNotIn(d, seen, rel)
            seen.add(d)

    def test_toolchain_is_part_of_the_identity(self) -> None:
        other = igd.compute(self.fx.repo, "HEAD", "Xcode 26.4 | 26.4 | 26.4")["digest"]
        self.assertNotEqual(other, self.base)

    def test_same_tree_digests_identically_from_another_clone(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            clone = Path(tmp) / "elsewhere"
            git(self.fx.repo.parent, "clone", "-q", str(self.fx.repo), str(clone))
            self.assertEqual(igd.compute(clone, "HEAD", TOOLCHAIN)["digest"], self.base)

    def test_input_count_and_classification(self) -> None:
        r = igd.compute(self.fx.repo, "HEAD", TOOLCHAIN)
        # 7 inputs: CMakeLists, 2 core files, examples CMakeLists, deps manifest,
        # gate script, test/ios harness. core/README.md is excluded as *.md.
        self.assertEqual(r["inputs"], 7)
        self.assertTrue(igd.is_input("core/view/src/widgets.cpp"))
        self.assertTrue(igd.is_input("test/cmake/quality_tests.cmake"))
        self.assertFalse(igd.is_input("core/view/README.md"))
        self.assertFalse(igd.is_input("tools/scripts/gates.sh"))


class LookupAndNoteTests(unittest.TestCase):
    def test_lookup_hits_only_an_unexpired_exact_name(self) -> None:
        d = "a" * 64
        page = {"artifacts": [
            {"name": igd.artifact_name(d), "expired": True, "created_at": "2026-09-01T00:00:00Z",
             "workflow_run": {"id": 1}},
            {"name": igd.artifact_name(d), "expired": False, "created_at": "2026-09-02T00:00:00Z",
             "workflow_run": {"id": 2}},
            {"name": igd.artifact_name(d), "expired": False, "created_at": "2026-09-03T00:00:00Z",
             "workflow_run": {"id": 3}},
            {"name": "ios-gate-ok-other", "expired": False, "created_at": "2026-09-04T00:00:00Z",
             "workflow_run": {"id": 4}}]}
        seen = []

        def fetch(url, token):
            seen.append((url, token))
            return page
        r = igd.lookup("O/R", "tok", d, fetch=fetch)
        self.assertTrue(r["found"])
        self.assertEqual(r["source_run_id"], 3)
        self.assertEqual(r["candidates"], 2)
        self.assertIn(f"name={igd.artifact_name(d)}", seen[0][0])
        miss = igd.lookup("O/R", "tok", "b" * 64, fetch=lambda u, t: {"artifacts": []})
        self.assertFalse(miss["found"])
        expired_only = igd.lookup("O/R", "tok", d, fetch=lambda u, t: {"artifacts": page["artifacts"][:1]})
        self.assertFalse(expired_only["found"])

    def test_note_is_a_notice_with_schema_and_verdict(self) -> None:
        line = igd.note("would_skip", "c" * 64, "merge_group", 42)
        self.assertTrue(line.startswith("::notice title=ios-gate-shadow::"))
        rec = json.loads(line.split("::", 2)[2])
        self.assertEqual(rec["schema"], igd.SCHEMA)
        self.assertEqual(rec["mode"], "shadow")
        self.assertEqual((rec["verdict"], rec["event"], rec["source_run_id"]),
                         ("would_skip", "merge_group", 42))

    def test_cli_compute_prints_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            proc = subprocess.run([sys.executable, str(HERE / "ios_gate_digest.py"), "compute",
                                   "--repo", str(fx.repo), "--toolchain-id", TOOLCHAIN],
                                  capture_output=True, text=True, timeout=60)
            expected = fx.digest()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        out = json.loads(proc.stdout)
        self.assertEqual(out["digest"], expected)
        self.assertEqual(len(out["digest"]), 64)


if __name__ == "__main__":
    unittest.main()
