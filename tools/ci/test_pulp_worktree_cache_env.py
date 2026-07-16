#!/usr/bin/env python3
"""Tests for tools/ci/pulp-worktree.sh cache_env emission.

pulp-worktree.sh drives the shipyard/per-worktree build lane: each branch
gets its own build dir but shares one ccache root. `cache_env` prints the
environment every worktree build sources. That env is safety-critical:
depend mode with mtime compiler keying on a SHARED cache serves a
stale/false-hit object that corrupts unrelated TUs (the #3504 scar). These
tests pin the emitted env to the #3504-safe combo so the scar can't silently
return, and run anywhere — `pulp-worktree.sh env` is a pure string emit with
no ccache/git dependency.

Run:  python3 tools/ci/test_pulp_worktree_cache_env.py
"""
from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("pulp-worktree.sh")


def _emit_env() -> str:
    # `env` prints cache_env() with no side effects (no ccache/git calls).
    r = subprocess.run(
        ["bash", str(SCRIPT), "env", "feature/example"],
        capture_output=True, text=True, check=False,
    )
    assert r.returncode == 0, r.stderr
    return r.stdout


class CacheEnvContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.env = _emit_env()

    def test_syntax_is_valid(self) -> None:
        r = subprocess.run(["bash", "-n", str(SCRIPT)],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_depend_mode_is_off(self) -> None:
        # The safe combo build.yml also forces: depend mode OFF via the negated
        # NO-form (ccache rejects CCACHE_DEPEND=false).
        self.assertIn("export CCACHE_NODEPEND=true", self.env)

    def test_depend_mode_never_enabled(self) -> None:
        # The #3504 scar guard — the old, corrupting emission must be gone.
        self.assertNotIn("CCACHE_DEPEND=true", self.env)

    def test_compiler_check_is_content(self) -> None:
        # Content-key the compiler so a shared cache never serves an mtime-only
        # false hit.
        self.assertIn("export CCACHE_COMPILERCHECK=content", self.env)

    def test_cross_worktree_normalization_preserved(self) -> None:
        # BASEDIR + NOHASHDIR must stay — they are what make cross-worktree
        # hits possible in the first place.
        self.assertIn("export CCACHE_BASEDIR=", self.env)
        self.assertIn("export CCACHE_NOHASHDIR=true", self.env)


if __name__ == "__main__":
    unittest.main(verbosity=2)
