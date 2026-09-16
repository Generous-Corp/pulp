#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sample_region_compat_baseline as baseline


class BaselineTests(unittest.TestCase):
    def test_frozen_receipt_includes_sdk_build_type_configure_input(self):
        repo = Path(__file__).resolve().parents[2]
        receipt = json.loads((repo / "test" / "fixtures" / "sample-region-compat" /
                              baseline.SDK_RECEIPT).read_text())
        self.assertIn("sdk_build_type.txt", receipt["files"])

    def test_builds_force_receipt_compatible_generator(self):
        success = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(baseline, "run", return_value=success) as run:
            baseline.configure_and_build(Path("repo"), Path("build"))
        self.assertEqual(run.call_args_list[0].args[0][0:3],
                         ["cmake", "-G", "Unix Makefiles"])

        with tempfile.TemporaryDirectory() as raw:
            sdk = Path(raw) / "sdk"
            pulp_dir = sdk / "lib" / "cmake" / "Pulp"
            pulp_dir.mkdir(parents=True)
            (pulp_dir / "PulpConfig.cmake").write_text("# frozen\n")
            with mock.patch.object(baseline, "verify_installed_sdk", return_value={}), \
                 mock.patch.object(baseline, "verify_sdk_build_inputs"), \
                 mock.patch.object(baseline, "run", return_value=success) as run:
                baseline.build_installed_consumer(
                    Path("repo"), sdk, Path("build"), Path("receipt"))
            self.assertEqual(run.call_args_list[0].args[0][0:3],
                             ["cmake", "-G", "Unix Makefiles"])

    def test_runner_exit_one_is_drift(self):
        results = [
            mock.Mock(returncode=1, stdout="", stderr="compatibility_drift=x\n"),
            mock.Mock(returncode=0, stdout="old-installed-sdk-consumer=pass\n", stderr=""),
        ]
        with tempfile.TemporaryDirectory() as raw:
            fixtures = Path(raw)
            (fixtures / "expected").mkdir()
            (fixtures / "expected" / "old-installed-sdk-consumer.txt").write_text(
                "old-installed-sdk-consumer=pass\n")
            with mock.patch.object(baseline, "sha256",
                                   return_value=baseline.SDK_RECEIPT_SHA256), \
                 mock.patch.object(baseline, "run", side_effect=results):
                status, _ = baseline.verify_once(Path("runner"), Path("consumer"), fixtures)
        self.assertEqual(status, baseline.DRIFT)

    def test_runner_exit_two_is_harness_failure(self):
        result = mock.Mock(returncode=2, stdout="", stderr="harness_failure=x\n")
        with mock.patch.object(baseline, "sha256",
                               return_value=baseline.SDK_RECEIPT_SHA256), \
             mock.patch.object(baseline, "run", return_value=result):
            status, _ = baseline.verify_once(Path("runner"), Path("consumer"), Path("fixtures"))
        self.assertEqual(status, baseline.HARNESS_FAILURE)

    def test_every_fixture_class_has_a_distinct_perturbation(self):
        root = Path("fixtures")
        cases = baseline.perturbations(root)
        self.assertEqual(len(cases), 12)
        self.assertEqual(len({name for name, _, _, _ in cases}), len(cases))
        by_name = {name: (path, needle) for name, path, needle, _ in cases}
        self.assertEqual(by_name["callback-feedback-output"],
                         (root / "expected" / "render-f32-bits.txt", b"[feedback_fixed]"))
        self.assertIn("old-installed-sdk", by_name)
        self.assertIn("signed-bake", by_name)
        self.assertIn("signed-bake-public-key", by_name)
        self.assertIn("signed-bake-output", by_name)
        self.assertIn("processor-virtual-order", by_name)
        self.assertIn("installed-sdk-receipt", by_name)

    def test_processor_virtuals_allow_only_trailing_additions(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            (root / "expected").mkdir()
            (root / "expected" / "processor-virtuals.txt").write_text("0:a\n1:b\n")
            with mock.patch.object(baseline, "processor_virtual_facts",
                                   return_value="0:a\n1:b\n2:c\n"):
                status, _ = baseline.verify_processor_virtuals(root, root)
            self.assertEqual(status, 0)
            with mock.patch.object(baseline, "processor_virtual_facts",
                                   return_value="0:a\n1:changed\n2:c\n"):
                status, _ = baseline.verify_processor_virtuals(root, root)
            self.assertEqual(status, baseline.DRIFT)

    def test_installed_sdk_receipt_rejects_a_mutated_consumed_file(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            sdk = root / "sdk"
            consumed = sdk / "include" / "pulp" / "surface.hpp"
            consumed.parent.mkdir(parents=True)
            consumed.write_text("frozen\n")
            provenance = {
                "schema": "pulp.sdk-provenance.v1",
                "sdk_version": "0.837.0",
                "source_git_sha": "abc",
            }
            provenance_path = sdk / "sdk-provenance.json"
            provenance_path.write_text(json.dumps(provenance))
            receipt = {
                "sdk_provenance_sha256": baseline.sha256(provenance_path),
                "provenance": provenance,
                "files": {"include/pulp/surface.hpp": baseline.sha256(consumed)},
            }
            receipt_path = root / "receipt.json"
            receipt_path.write_text(json.dumps(receipt))
            baseline.verify_installed_sdk(sdk, receipt_path)
            consumed.write_text("mutated\n")
            with self.assertRaisesRegex(RuntimeError, "file hash mismatch"):
                baseline.verify_installed_sdk(sdk, receipt_path)

    def test_installed_sdk_receipt_rejects_unreceipted_cmake_surface(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            sdk = root / "sdk"
            cmake_input = sdk / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake"
            cmake_input.parent.mkdir(parents=True)
            cmake_input.write_text("# frozen\n")
            provenance = {"schema": "pulp.sdk-provenance.v1"}
            provenance_path = sdk / "sdk-provenance.json"
            provenance_path.write_text(json.dumps(provenance))
            receipt = {
                "sdk_provenance_sha256": baseline.sha256(provenance_path),
                "provenance": provenance,
                "files": {
                    "lib/cmake/Pulp/PulpConfig.cmake": baseline.sha256(cmake_input),
                },
            }
            receipt_path = root / "receipt.json"
            receipt_path.write_text(json.dumps(receipt))
            baseline.verify_installed_sdk(sdk, receipt_path)
            (cmake_input.parent / "Unexpected.cmake").write_text("# unreceipted\n")
            with self.assertRaisesRegex(RuntimeError, "unreceipted files"):
                baseline.verify_installed_sdk(sdk, receipt_path)

    def test_installed_sdk_build_rejects_unreceipted_inputs(self):
        with mock.patch.object(baseline, "sdk_build_inputs",
                               return_value={"include/a.hpp", "lib/liba.a"}):
            with self.assertRaisesRegex(RuntimeError, "unreceipted"):
                baseline.verify_sdk_build_inputs(
                    Path("build"), Path("sdk"), {"include/a.hpp": "hash"})


if __name__ == "__main__":
    unittest.main()
