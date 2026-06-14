#!/usr/bin/env python3
"""Tests for split desktop reporting run helper modules."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module(name: str):
    return load_local_ci_module(f"{name}.py", add_module_dir=True)


class ReportingRunHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.config = {"desktop_automation": {"artifact_root": str(self.root / "artifacts")}}

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def artifact_root(self, config: dict) -> Path:
        path = Path(config["desktop_automation"]["artifact_root"])
        path.mkdir(parents=True, exist_ok=True)
        return path

    def test_run_manifest_module_scans_and_sorts_manifests(self) -> None:
        mod = load_module("reporting_run_manifest")
        root = self.artifact_root(self.config)
        old_bundle = root / "mac" / "smoke" / "old"
        new_bundle = root / "mac" / "smoke" / "new"
        bad_bundle = root / "mac" / "smoke" / "bad"
        for bundle in (old_bundle, new_bundle, bad_bundle):
            bundle.mkdir(parents=True)
        (old_bundle / "manifest.json").write_text(json.dumps({"label": "old", "completed_at": "2026-01-01T00:00:00Z"}))
        (new_bundle / "manifest.json").write_text(json.dumps({"label": "new", "completed_at": "2026-01-02T00:00:00Z"}))
        (bad_bundle / "manifest.json").write_text("{")

        manifests = mod.desktop_run_manifests(
            self.config,
            target_name="mac",
            action="smoke",
            desktop_artifact_root_fn=self.artifact_root,
        )

        self.assertEqual([manifest["label"] for manifest in manifests], ["new", "old"])
        self.assertEqual(manifests[0]["artifacts"]["bundle_dir"], str(new_bundle))
        self.assertEqual(
            mod.desktop_rollup_dir(self.config, "mac", desktop_artifact_root_fn=self.artifact_root),
            root / "mac",
        )

    def test_run_rollup_and_prune_helpers_write_and_select_expected_paths(self) -> None:
        rollup = load_module("reporting_run_rollup")
        prune = load_module("reporting_run_prune")
        writes: dict[Path, str] = {}
        rollup_dir = self.artifact_root(self.config) / "mac"
        rollup_dir.mkdir()
        manifests = [
            {"label": "new", "completed_at": "2999-01-01T00:00:00+00:00", "artifacts": {"bundle_dir": str(rollup_dir / "new")}},
            {"label": "old", "completed_at": "2000-01-01T00:00:00+00:00", "artifacts": {"bundle_dir": str(rollup_dir / "old")}},
            {"label": "old-duplicate", "completed_at": "2000-01-01T00:00:00+00:00", "artifacts": {"bundle_dir": str(rollup_dir / "old")}},
        ]
        (rollup_dir / "old").mkdir()

        rollup.write_desktop_run_rollups(
            self.config,
            target_name="mac",
            desktop_rollup_dir_fn=lambda _config, _target_name=None: rollup_dir,
            desktop_run_manifests_fn=lambda _config, **_kwargs: manifests,
            desktop_run_summary_fn=lambda _config, manifest: {"label": manifest["label"]},
            desktop_proof_summaries_fn=lambda *_args, **_kwargs: [{"proof": True}],
            atomic_write_text_fn=lambda path, text: writes.__setitem__(path, text),
        )
        removed = prune.prune_desktop_run_manifests(
            self.config,
            older_than_days=1,
            desktop_run_manifests_fn=lambda _config, **_kwargs: manifests,
        )

        self.assertEqual(json.loads(writes[rollup_dir / "latest-run.json"])["label"], "new")
        self.assertEqual(json.loads(writes[rollup_dir / "latest-proof.json"]), {"proof": True})
        self.assertEqual(removed, [rollup_dir / "old"])


if __name__ == "__main__":
    unittest.main()
