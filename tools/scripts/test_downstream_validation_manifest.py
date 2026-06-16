#!/usr/bin/env python3
from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "verify_downstream_validation_manifest.py"
MANIFEST = REPO_ROOT / "tools" / "validation" / "downstream" / "consumer-validation.json"


def load_module():
    spec = importlib.util.spec_from_file_location("verify_downstream_validation_manifest", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


validator = load_module()


class DownstreamValidationManifestTests(unittest.TestCase):
    def load_manifest(self):
        return json.loads(MANIFEST.read_text(encoding="utf-8"))

    def run_validator(self, manifest) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(SCRIPT), str(path)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

    def test_repository_manifest_is_valid(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(MANIFEST)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("downstream_validation_manifest_ok=true", result.stdout)

    def test_rejects_missing_consumer(self):
        manifest = self.load_manifest()
        manifest["consumers"] = manifest["consumers"][:-1]
        result = self.run_validator(manifest)
        self.assertEqual(result.returncode, 1)
        self.assertIn("roadmap P0.4 downstream repos", result.stderr)

    def test_rejects_debug_sdk_recipe(self):
        manifest = self.load_manifest()
        manifest["canonical_sdk_recipe"]["build_type"] = "Debug"
        result = self.run_validator(manifest)
        self.assertEqual(result.returncode, 1)
        self.assertIn("build_type must be Release", result.stderr)

    def test_local_report_marks_missing_checkouts_without_failing_schema(self):
        manifest = self.load_manifest()
        for consumer in manifest["consumers"]:
            consumer["local_checkout_hint"] = f"/Volumes/Workshop/Code/{consumer['name']}-missing"
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(path), "--check-local"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("status=missing", result.stdout)
        self.assertIn("downstream_validation_manifest_ok=true", result.stdout)

    def test_require_clean_fails_on_missing_checkout(self):
        manifest = self.load_manifest()
        for consumer in manifest["consumers"]:
            consumer["local_checkout_hint"] = f"/Volumes/Workshop/Code/{consumer['name']}-missing"
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(path),
                    "--check-local",
                    "--require-clean",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertEqual(result.returncode, 1)
        self.assertIn("downstream_validation_local_clean=false", result.stderr)

    def test_validate_manifest_function_has_no_side_effects(self):
        manifest = self.load_manifest()
        before = copy.deepcopy(manifest)
        self.assertEqual(validator.validate_manifest(manifest), [])
        self.assertEqual(manifest, before)


if __name__ == "__main__":
    unittest.main()
