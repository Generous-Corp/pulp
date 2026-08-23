#!/usr/bin/env python3
"""Host/JIT cache routing contracts shared by Pulp's legacy Tart surfaces."""

from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "tools/ci/bootstrap-macos-host.sh"
DIRECT_TART = ROOT / "tools/ci/tart-run-job.sh"
VM_MANIFEST = ROOT / ".shipyard/vm-image.toml"


class CacheRoutingContractTests(unittest.TestCase):
    def test_no_live_script_enables_depend_mode(self) -> None:
        for path in (BOOTSTRAP, DIRECT_TART):
            body = path.read_text(encoding="utf-8")
            self.assertIsNone(
                re.search(r"^(?:export )?CCACHE_DEPEND=true$", body, re.MULTILINE),
                path,
            )
            self.assertIn("CCACHE_NODEPEND=true", body, path)
            self.assertIn("CCACHE_COMPILERCHECK=content", body, path)

    def test_direct_tart_mounts_canonical_fetchcontent_source_root(self) -> None:
        body = DIRECT_TART.read_text(encoding="utf-8")
        canonical = "$HOME/Library/Caches/Pulp/fetchcontent-src"
        self.assertIn(canonical, body)
        self.assertIn('--dir="fetchcontent:$FETCHCONTENT_SOURCE_ROOT:ro"', body)
        self.assertIn(
            'export PULP_SHARED_FETCHCONTENT_SOURCE_DIR="$HOME/Library/Caches/Pulp/fetchcontent-src"',
            body,
        )
        self.assertIn('rsync -a "$SHARED/fetchcontent/"', body)
        self.assertNotIn("FETCHCONTENT_BASE_DIR", body)

    def test_host_runner_env_uses_same_canonical_source_root(self) -> None:
        body = BOOTSTRAP.read_text(encoding="utf-8")
        self.assertIn("$HOME/Library/Caches/Pulp/fetchcontent-src", body)
        self.assertIn("PULP_SHARED_FETCHCONTENT_SOURCE_DIR=$FETCHCONTENT_SOURCE_ROOT", body)
        self.assertNotIn("FETCHCONTENT_BASE_DIR=$FETCHCONTENT_SOURCE_ROOT", body)

    def test_manifest_matches_live_mount_policy(self) -> None:
        manifest = tomllib.loads(VM_MANIFEST.read_text(encoding="utf-8"))
        mounts = {entry["name"]: entry for entry in manifest["mounts"]}
        self.assertEqual(
            mounts["fetchcontent"]["host"],
            "~/Library/Caches/Pulp/fetchcontent-src",
        )
        self.assertEqual(mounts["fetchcontent"]["mode"], "ro")
        self.assertEqual(mounts["ccache"]["host"], "~/.cache/pulp-ci/ccache")
        self.assertNotIn("skia", mounts)


if __name__ == "__main__":
    unittest.main(verbosity=2)
