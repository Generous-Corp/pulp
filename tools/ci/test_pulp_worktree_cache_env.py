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

import os
import platform
import subprocess
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("pulp-worktree.sh")


def _emit_env(**overrides: str) -> str:
    # `env` prints cache_env() with no side effects (no ccache/git calls).
    env = dict(os.environ)
    env.update(overrides)
    r = subprocess.run(
        ["bash", str(SCRIPT), "env", "feature/example"],
        capture_output=True, text=True, check=False, env=env,
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

    def test_fetchcontent_uses_canonical_host_source_root(self) -> None:
        if platform.system() == "Darwin":
            expected = Path.home() / "Library/Caches/Pulp/fetchcontent-src"
        else:
            expected = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "pulp/fetchcontent-src"
        self.assertIn(
            f'export PULP_SHARED_FETCHCONTENT_SOURCE_DIR="{expected}"', self.env
        )
        self.assertNotIn("FETCHCONTENT_BASE_DIR", self.env)
        self.assertNotIn(".cache/pulp-ci/fetchcontent-src", self.env)

    def test_keyed_skia_defers_exact_generation_resolution_to_cmake(self) -> None:
        self.assertIn('export PULP_SKIA_CACHE_ROOT=', self.env)
        self.assertNotIn('export PULP_SKIA_CACHE=', self.env)
        self.assertNotIn('export SKIA_DIR=', self.env)
        self.assertNotIn('skia-build/build/mac-gpu', self.env)
        self.assertNotIn(f'{Path.home()}/.cache/pulp-ci/skia-build', self.env)

    def test_legacy_target_gets_exact_validated_generation(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('[ -f "$repo/tools/cmake/PulpSkiaCache.cmake" ]', text)
        cache_env = text.split("cache_env()", 1)[1].split("cmd_new()", 1)[0]
        keyed_legacy = cache_env.split(
            'elif [ "$SKIA_CACHE_KEYING" = 1 ]; then', 1
        )[1].split("else", 1)[0]
        self.assertIn("PULP_SKIA_CACHE", keyed_legacy)
        self.assertIn("SKIA_DIR", keyed_legacy)
        self.assertIn('"$skia_cache_dir"', keyed_legacy)

    def test_legacy_symlink_is_an_explicit_rollback_only(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn("PULP_WORKTREE_LEGACY_SKIA_SYMLINK", text)
        self.assertNotIn('ln -sfn "$CACHE_ROOT/skia-build" "$wt/external/skia-build" 2>/dev/null || true\n  touch', text)

    def test_keying_rollback_uses_setup_legacy_mutable_destination(self) -> None:
        legacy = "/tmp/pulp-test-legacy-skia-build"
        env = _emit_env(PULP_SKIA_CACHE_KEYING="0", PULP_SKIA_CACHE=legacy)
        self.assertIn(f'export PULP_SKIA_CACHE="{legacy}"', env)
        self.assertIn(f'export SKIA_DIR="{legacy}"', env)
        self.assertNotIn("export PULP_SKIA_CACHE_ROOT=", env)
        self.assertNotRegex(env, r"darwin-arm64-[0-9a-f]{64}")

    def test_cache_seed_is_pin_validated_and_locked(self) -> None:
        text = SCRIPT.read_text()
        self.assertNotIn("rsync -a", text)
        self.assertIn("fetch_skia_for_release.py", text)
        self.assertIn("--cache-lock-timeout", text)
        self.assertIn("--cache-root", text)
        self.assertIn("--print-cache-dest", text)

    def test_new_worktree_resolves_its_own_manifest_pin(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('ensure_cache "$wt"', text)
        self.assertIn('cache_env "$wt" > "$wt/.pulp-ci-env"', text)
        self.assertIn(
            '(cd "$repo" && python3 "$REPO_ROOT/tools/scripts/fetch_skia_for_release.py"',
            text,
        )

    def test_failed_provisioning_can_resume_without_deleting_worktree(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('note "resuming existing worktree: $wt"', text)
        self.assertNotIn('worktree remove', text.split("cmd_new()", 1)[1].split("cmd_env()", 1)[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
