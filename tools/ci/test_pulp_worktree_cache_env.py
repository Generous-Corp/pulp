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
import shutil
import subprocess
import tempfile
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


class HostCcacheConfigTests(unittest.TestCase):
    """Gate VMs mount the shared ccache directory read-write, so its
    ccache.conf is guest-writable. A guest that plants `prefix_command` there
    must not get it run by the next host build: host ccache reads its own
    config (CCACHE_CONFIGPATH) beside the mounted directory."""

    def test_the_host_reads_a_config_outside_the_shared_cache(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            env = _emit_env(PULP_CI_CACHE=root, PULP_SKIA_CACHE_KEYING="0")
            values = dict(line[len("export "):].split("=", 1)
                          for line in env.splitlines() if line.startswith("export CCACHE_"))
            config = values["CCACHE_CONFIGPATH"].strip('"')
            cache = values["CCACHE_DIR"].strip('"')
            self.assertEqual(config, f"{root}/ccache-host.conf")
            self.assertFalse((config + "/").startswith(cache + "/"), (config, cache))

    def test_a_config_inside_the_shared_cache_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            for inside in (f"{root}/ccache", f"{root}/ccache/ccache.conf"):
                with self.subTest(path=inside):
                    environ = dict(os.environ, PULP_CI_CACHE=root,
                                   PULP_SKIA_CACHE_KEYING="0",
                                   PULP_HOST_CCACHE_CONFIG=inside)
                    r = subprocess.run(["bash", str(SCRIPT), "env", "feature/example"],
                                       capture_output=True, text=True, encoding="utf-8",
                                       check=False, env=environ)
                    self.assertNotEqual(r.returncode, 0)
                    self.assertIn("guest-writable", r.stderr)

    def host_compile(self, root: Path, *, keep_configpath: bool) -> bool:
        """Compile one file through ccache with the host environment, after a
        guest planted prefix_command in the shared ccache.conf; whether the
        planted command ran."""
        marker = root / "planted-ran"
        planted = root / "planted.sh"
        planted.write_text(f'#!/bin/sh\ntouch "{marker}"\nexec "$@"\n', encoding="utf-8")
        planted.chmod(0o755)
        shared = root / "ccache"
        shared.mkdir(exist_ok=True)
        (shared / "ccache.conf").write_text(f"prefix_command = {planted}\n", encoding="utf-8")
        source = root / "unit.c"
        # A fresh body every call: a cache hit compiles nothing, so it would
        # skip prefix_command and read as "not run" whatever the config.
        source.write_text(f"int unit(void) {{ return 0x{os.urandom(3).hex()}; }}\n",
                          encoding="utf-8")
        env_text = _emit_env(PULP_CI_CACHE=str(root), PULP_SKIA_CACHE_KEYING="0")
        script = env_text + ("" if keep_configpath else "unset CCACHE_CONFIGPATH\n") + \
            f'ccache cc -c "{source}" -o "{root}/unit.o"\n'
        r = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                           encoding="utf-8", check=False, timeout=120,
                           env=dict(os.environ, HOME=str(root)))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((root / "unit.o").exists(), "the compile ran")
        ran = marker.exists()
        marker.unlink(missing_ok=True)
        (root / "unit.o").unlink(missing_ok=True)
        return ran

    @unittest.skipUnless(shutil.which("ccache") and shutil.which("cc"),
                         "needs ccache and cc on PATH")
    def test_a_planted_prefix_command_never_runs_in_a_host_build(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertFalse(self.host_compile(root, keep_configpath=True),
                             "the host build ran the guest's prefix_command")
            # Negative control, after: on the same cache, without the host
            # config, the plant does run, so the check above could see it.
            self.assertTrue(self.host_compile(root, keep_configpath=False),
                            "control: ccache reads the shared ccache.conf")


if __name__ == "__main__":
    unittest.main(verbosity=2)
