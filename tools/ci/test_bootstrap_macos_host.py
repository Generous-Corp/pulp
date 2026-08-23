#!/usr/bin/env python3
"""Tests for tools/ci/bootstrap-macos-host.sh.

The bootstrap script provisions a Mac as a Pulp self-hosted CI runner
host. It is the turnkey artifact the M5 setup reuses verbatim, so it is
safety-critical: a broken, non-idempotent, or silently-passing bootstrap
mis-provisions a CI host. These tests pin the script's contract —
argument handling, syntax, and the `--check` dry run (which must report
every provisioning phase and mutate nothing).

Cross-platform tests run everywhere; the `--check` dry-run tests are
macOS-only (the script hard-exits at preflight off macOS by design).

Run:  python3 tools/ci/test_bootstrap_macos_host.py
"""
from __future__ import annotations

import os
import platform
import subprocess
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("bootstrap-macos-host.sh")
BREWFILE = Path(__file__).with_name("Brewfile")
IS_MACOS = platform.system() == "Darwin"


def _run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(SCRIPT), *args],
        capture_output=True, text=True, check=False,
    )


class ScriptContractTests(unittest.TestCase):
    """Cross-platform — syntax + argument handling."""

    def test_script_exists_and_is_executable(self) -> None:
        self.assertTrue(SCRIPT.is_file(), SCRIPT)
        self.assertTrue(os.access(SCRIPT, os.X_OK), f"{SCRIPT} not executable")

    def test_syntax_is_valid(self) -> None:
        r = subprocess.run(["bash", "-n", str(SCRIPT)],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_help_exits_zero_with_usage(self) -> None:
        r = _run("--help")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Usage", r.stdout + r.stderr)

    def test_unknown_arg_exits_2(self) -> None:
        # An unrecognized flag must fail loudly, not be silently ignored.
        r = _run("--definitely-not-a-real-flag")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)

    def test_runner_env_disables_ccache_depend_mode(self) -> None:
        # Host registration must be safe before any workflow-level override.
        body = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("CCACHE_NODEPEND=true", body)
        self.assertIn("CCACHE_COMPILERCHECK=content", body)
        self.assertNotIn("CCACHE_DEPEND=true", body)
        self.assertIn("CCACHE_SLOPPINESS=time_macros,pch_defines", body)

    def test_runner_env_uses_canonical_fetchcontent_source_root(self) -> None:
        body = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("PULP_SHARED_FETCHCONTENT_SOURCE_DIR=$FETCHCONTENT_SOURCE_ROOT", body)
        self.assertNotIn("FETCHCONTENT_BASE_DIR=$FETCHCONTENT_SOURCE_ROOT", body)
        self.assertIn("$HOME/Library/Caches/Pulp/fetchcontent-src", body)

    def test_runner_env_persists_resolved_skia_cache_root(self) -> None:
        body = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("PULP_SKIA_CACHE_ROOT=$SKIA_CACHE_ROOT", body)

    def _tune_ccache_body(self) -> str:
        # Isolate the tune_ccache() function so assertions about the SHARED
        # ccache.conf can't be satisfied by an unrelated line (e.g. the runner
        # .env's CCACHE_NODEPEND in register_runners).
        body = SCRIPT.read_text(encoding="utf-8")
        start = body.index("tune_ccache() {")
        end = body.index("\n}", start)
        return body[start:end]

    def test_tune_ccache_writes_correctness_combo(self) -> None:
        # The durable fleet-wide ccache fix: cross-worktree normalization
        # (base_dir + hash_dir=false) and the #3504 correctness combo
        # (compiler_check=content + depend_mode=false) are written into the
        # SHARED ccache.conf, so a cold-by-construction multi-worktree host
        # actually shares hits and never serves a stale depend-mode object.
        fn = self._tune_ccache_body()
        self.assertIn('"base_dir=$base_dir"', fn)
        self.assertIn('base_dir="$(dirname "$work_root")"', fn)
        self.assertIn('"hash_dir=false"', fn)
        self.assertIn('"compiler_check=content"', fn)
        self.assertIn('"depend_mode=false"', fn)

    def test_tune_ccache_never_enables_depend_or_mtime_keying(self) -> None:
        # The scar guard: depend mode must never be turned back on, and the
        # compiler must never be keyed by mtime, in the shared config.
        fn = self._tune_ccache_body()
        self.assertNotIn("depend_mode=true", fn)
        self.assertNotIn("compiler_check=mtime", fn)
        self.assertNotIn("hash_dir=true", fn)

    def test_ccache_cap_lifted_to_200g(self) -> None:
        # Cap raised so a 100+-worktree host stops evicting a warm cache.
        body = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("PULP_CCACHE_MAX_SIZE:-200G", body)

    def test_skia_bootstrap_uses_only_manifest_keyed_cache(self) -> None:
        body = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('--cache-root "$SKIA_CACHE_ROOT"', body)
        self.assertNotIn("cache/skia-build", body)
        self.assertNotIn('rsync -a "$REPO_ROOT/external/skia-build/"', body)


class BrewfileTests(unittest.TestCase):
    """The Brewfile the bootstrap consumes."""

    def test_brewfile_present(self) -> None:
        self.assertTrue(BREWFILE.is_file(), BREWFILE)

    def test_brewfile_lists_core_toolchain(self) -> None:
        body = BREWFILE.read_text(encoding="utf-8")
        for dep in ("cmake", "ninja", "ccache", "gh", "git-lfs"):
            self.assertIn(f'"{dep}"', body, f"Brewfile missing {dep}")


@unittest.skipUnless(IS_MACOS, "bootstrap-macos-host.sh runs only on macOS")
class CheckModeTests(unittest.TestCase):
    """macOS-only — the `--check` dry run."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.result = _run("--check")

    def test_check_mode_exits_zero(self) -> None:
        self.assertEqual(self.result.returncode, 0, self.result.stderr)

    def test_check_mode_reports_every_phase(self) -> None:
        out = self.result.stdout
        for phase in ("Preflight", "Xcode", "Homebrew dependencies",
                      "Cache layout", "ccache tuning", "Skia",
                      "Shipyard", "Verify build dependencies", "Done"):
            self.assertIn(phase, out, f"phase '{phase}' missing from --check")

    def test_check_mode_is_a_dry_run(self) -> None:
        # Every mutating command must be reported as "would run:", never
        # executed — that is the whole guarantee of --check.
        self.assertIn("would run:", self.result.stdout)

    def test_check_mode_confirms_provisioned(self) -> None:
        self.assertIn("host provisioned", self.result.stdout)

    def test_check_mode_runner_phase_skipped_without_flag(self) -> None:
        # Without --with-runners, registration must not happen.
        self.assertIn("runner registration skipped", self.result.stdout)

    def test_check_mode_emits_ccache_correctness_config(self) -> None:
        # The --check dry run reports the exact `ccache --set-config` calls it
        # would make: prove the correctness combo actually reaches the emitted
        # config (not just the source), and that depend mode is never re-enabled.
        out = self.result.stdout
        for kv in ("compiler_check=content", "depend_mode=false",
                   "hash_dir=false", "base_dir="):
            self.assertIn(kv, out, f"tune_ccache did not emit {kv}")
        self.assertNotIn("depend_mode=true", out)
        self.assertNotIn("compiler_check=mtime", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
