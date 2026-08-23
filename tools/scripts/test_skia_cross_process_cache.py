#!/usr/bin/env python3
"""Two-process proof that configure reuses setup's immutable Skia generation."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RESOLVER = ROOT / "tools/cmake/PulpSkiaCache.cmake"


class CrossProcessSkiaCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.cache_root = self.root / "cache" / "skia"
        self.output = self.root / "resolved.txt"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def setup_process_destination(self) -> str:
        env = dict(os.environ, HOME=str(self.root),
                   PULP_SKIA_CACHE_ROOT=str(self.cache_root))
        env.pop("PULP_SKIA_CACHE", None)
        env.pop("PULP_SKIA_CACHE_KEYING", None)
        result = subprocess.run(
            ["bash", str(ROOT / "setup.sh"), "--print-skia-ci-dest"],
            cwd=ROOT, env=env, check=True, capture_output=True, text=True,
        )
        return result.stdout.strip()

    def configure_process(self, **overrides: str) -> tuple[str, str]:
        script = self.root / "resolve.cmake"
        script.write_text(
            f'set(PULP_ROOT_DIR "{ROOT.as_posix()}")\n'
            f'include("{RESOLVER.as_posix()}")\n'
            'pulp_resolve_skia_cache("darwin-arm64" "" cache args)\n'
            f'file(WRITE "{self.output.as_posix()}" "${{cache}}\\n${{args}}\\n")\n',
            encoding="utf-8",
        )
        env = dict(os.environ, HOME=str(self.root),
                   PULP_SKIA_CACHE_ROOT=str(self.cache_root))
        env.pop("PULP_SKIA_CACHE", None)
        env.pop("PULP_SKIA_CACHE_KEYING", None)
        env.update(overrides)
        subprocess.run(["cmake", "-P", str(script)], cwd=ROOT, env=env,
                       check=True, capture_output=True, text=True)
        return tuple(self.output.read_text(encoding="utf-8").splitlines())

    def test_fresh_configure_process_resolves_setup_exact_generation(self) -> None:
        setup_dest = self.setup_process_destination()
        configure_dest, fetch_args = self.configure_process()
        self.assertEqual(configure_dest, setup_dest)
        self.assertRegex(configure_dest, r"darwin-arm64-[0-9a-f]{64}$")
        self.assertIn(f"--cache-root;{self.cache_root}", fetch_args)
        self.assertNotIn("skia-build", configure_dest)

    def test_removing_keyed_propagation_exposes_legacy_duplicate_path(self) -> None:
        setup_dest = self.setup_process_destination()
        configure_dest, fetch_args = self.configure_process(PULP_SKIA_CACHE_KEYING="0")
        self.assertNotEqual(configure_dest, setup_dest)
        self.assertEqual(configure_dest, str(self.root / ".cache/pulp/skia-build"))
        self.assertIn(f"--dest;{configure_dest}", fetch_args)

    def test_legacy_override_matches_setup_rollback_contract(self) -> None:
        legacy = self.root / "legacy-skia"
        configure_dest, fetch_args = self.configure_process(
            PULP_SKIA_CACHE_KEYING="0", PULP_SKIA_CACHE=str(legacy)
        )
        self.assertEqual(configure_dest, str(legacy))
        self.assertIn(f"--dest;{legacy}", fetch_args)

    def test_ci_and_worktree_surfaces_call_same_manifest_fetcher(self) -> None:
        setup = (ROOT / "setup.sh").read_text(encoding="utf-8")
        worktree = (ROOT / "tools/ci/pulp-worktree.sh").read_text(encoding="utf-8")
        dependencies = (ROOT / "tools/cmake/PulpDependencies.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn("--cache-root \"$cache_root\" --print-cache-dest", setup)
        self.assertIn("--cache-root \"$SKIA_CACHE_ROOT\" --print-cache-dest", worktree)
        self.assertIn("pulp_resolve_skia_cache", dependencies)

    def test_nonrelease_macos_workflows_do_not_refetch_into_checkout(self) -> None:
        for relative in (
            ".github/workflows/build.yml",
            ".github/workflows/examples-validation.yml",
            ".github/workflows/nightly-full-build.yml",
            ".github/workflows/coverage.yml",
        ):
            with self.subTest(workflow=relative):
                text = (ROOT / relative).read_text(encoding="utf-8")
                matches = [
                    line for line in text.splitlines()
                    if "fetch_skia_for_release.py darwin-arm64" in line
                ]
                self.assertTrue(matches)
                for line in matches:
                    # Multi-line commands must carry the continuation that
                    # binds the following --cache-root argument.
                    self.assertTrue(line.rstrip().endswith("\\") or relative.endswith("coverage.yml"))
                self.assertIn("--cache-root", text)

    def test_windows_without_a_published_slice_preserves_local_fallback(self) -> None:
        dependencies = (ROOT / "tools/cmake/PulpDependencies.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "if(NOT _pulp_local_skia AND _pulp_skia_plat)", dependencies
        )

    def test_cmake_validates_complete_generation_before_accepting_hit(self) -> None:
        dependencies = (ROOT / "tools/cmake/PulpDependencies.cmake").read_text(
            encoding="utf-8"
        )
        execute = dependencies.index("execute_process(", dependencies.index("pulp_resolve_skia_cache"))
        glob = dependencies.index("file(GLOB _pulp_cache_skia", execute)
        self.assertLess(execute, glob)
        self.assertIn("RESULT_VARIABLE _pulp_skia_fetch_result", dependencies)

    def test_keyed_mode_does_not_prefer_retained_worktree_materialization(self) -> None:
        dependencies = (ROOT / "tools/cmake/PulpDependencies.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn('set(_pulp_skia_keying "$ENV{PULP_SKIA_CACHE_KEYING}")', dependencies)
        self.assertIn("--validate-only", dependencies)
        self.assertIn('if(NOT _pulp_local_skia_valid EQUAL 0)', dependencies)

    def test_keyed_failure_never_falls_back_to_unvalidated_checkout(self) -> None:
        dependencies = (ROOT / "tools/cmake/PulpDependencies.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn("keyed cache resolution failed", dependencies)
        self.assertIn("pinned cache generation validation/provisioning", dependencies)
        self.assertIn("refusing unvalidated external/skia-build fallback", dependencies)

    def test_release_managed_checkout_bundle_disables_autofetch(self) -> None:
        release = (ROOT / ".github/workflows/release-cli.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("-DPULP_SKIA_AUTOFETCH=OFF", release)


if __name__ == "__main__":
    unittest.main(verbosity=2)
