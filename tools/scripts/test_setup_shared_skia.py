#!/usr/bin/env python3
"""Contract tests for setup.sh's shared pinned Skia provisioning."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent.parent
SETUP = ROOT / "setup.sh"


class SharedSkiaSetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SETUP.read_text()

    def test_ci_fetch_uses_explicit_shared_destination(self) -> None:
        self.assertIn('SKIA_CACHE_ROOT="${PULP_SKIA_CACHE_ROOT:-$HOME/.cache/pulp/skia}"', self.text)
        self.assertIn('--cache-root "$cache_root" --print-cache-dest', self.text)
        self.assertIn('SKIA_CI_DEST="$(resolve_skia_ci_dest)"', self.text)
        self.assertIn('export SKIA_DIR="$SKIA_CI_DEST"', self.text)
        self.assertIn('SKIA_CHECK="${SKIA_CI_DEST:-$REPO_ROOT/external/skia-build}"', self.text)

    def test_legacy_per_worktree_fetch_requires_rollback_switch(self) -> None:
        self.assertIn("PULP_SETUP_SKIA_PER_WORKTREE", self.text)
        self.assertIn("resolve_skia_ci_dest", self.text)
        self.assertIn("$REPO_ROOT/external/skia-build", self.text)

    def test_shared_cache_fetch_is_bounded_and_serialized(self) -> None:
        self.assertIn("PULP_SKIA_CACHE_LOCKING", self.text)
        self.assertIn("--cache-lock-timeout", self.text)
        self.assertIn("PULP_SKIA_CACHE_KEYING", self.text)

    def test_baked_tart_skia_is_validated_in_place(self) -> None:
        self.assertIn("PULP_USE_BAKED_SKIA:-0", self.text)
        self.assertIn('SKIA_CI_DEST="$SKIA_DIR"', self.text)
        self.assertIn('SKIA_FETCH_ARGS=(--dest "$SKIA_CI_DEST")', self.text)

    def test_release_platform_fetches_remain_outside_setup(self) -> None:
        self.assertNotIn("darwin-x64 --dest", self.text)
        self.assertNotIn("darwin-universal --dest", self.text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
