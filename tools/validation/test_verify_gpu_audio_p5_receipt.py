import copy
import io
import importlib.util
import unittest
import json
import hashlib
import os
import tempfile
import sys
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("verify_gpu_audio_p5_receipt.py")
SPEC = importlib.util.spec_from_file_location("p5_receipt", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def receipt():
    identity = {
        "source_revision": "a" * 40,
        "provider_identity": "dawn:provider-rev",
        "executable_sha256": "b" * 64,
        "model_sha256": "c" * 64,
        "resident_plan_sha256": "d" * 64,
    }
    rows = []
    for sequence, admission_id in enumerate(("admission-1", "admission-2"), 1):
        for phase, timestamp in (("admitted", 1000 * sequence), ("terminal", 1000 * sequence + 25)):
            rows.append({"admission_id": admission_id, "phase": phase, "sequence": sequence,
                         "identity": copy.deepcopy(identity),
                         "timing": {"admission_ns": 1000 * sequence, "terminal_ns": timestamp,
                                    "duration_ns": timestamp - 1000 * sequence},
                         **({"disposition": "completed"} if phase == "terminal" else {})})
    return {"schema": MODULE.SCHEMA, "expected_admissions": 2, "identity": identity,
            "rows": rows, "disposition_census": {"completed": 2}}


class P5ReceiptVerifierTests(unittest.TestCase):
    def test_valid_receipt_reproduces_census(self):
        result = MODULE.verify_receipt(receipt())
        self.assertEqual(result["verdict"], "PASS")
        self.assertEqual(result["disposition_census"], {"completed": 2})

    def test_synthetic_p4_raw_fixture_uses_authoritative_validator(self):
        path = Path(__file__).parents[1] / "scripts" / "test_gpu_audio_p4_evidence.py"
        spec = importlib.util.spec_from_file_location("p4_fixture", path)
        module = importlib.util.module_from_spec(spec); assert spec.loader is not None; spec.loader.exec_module(module)
        records = module.fixture()
        self.assertEqual(module.MODULE.validate_records(records), [])

    def assert_rejected(self, mutate):
        value = receipt()
        mutate(value)
        with self.assertRaises(MODULE.ReceiptError):
            MODULE.verify_receipt(value)

    def test_missing_terminal_row_rejected(self):
        self.assert_rejected(lambda value: value["rows"].pop())

    def test_duplicate_row_rejected(self):
        self.assert_rejected(lambda value: value["rows"].append(copy.deepcopy(value["rows"][0])))

    def test_reordered_terminal_before_admission_rejected(self):
        def reorder(value):
            value["rows"][0], value["rows"][1] = value["rows"][1], value["rows"][0]
        self.assert_rejected(reorder)

    def test_forged_provenance_rejected(self):
        self.assert_rejected(lambda value: value["rows"][2]["identity"].update(model_sha256="e" * 64))

    def test_census_mismatch_rejected(self):
        self.assert_rejected(lambda value: value["disposition_census"].update(completed=1))

    def test_boolean_numeric_fields_rejected(self):
        self.assert_rejected(lambda value: value.update(expected_admissions=True))
        self.assert_rejected(lambda value: value["disposition_census"].update(completed=True))

    def test_census_zero_entries_must_name_known_dispositions(self):
        accepted = receipt()
        accepted["disposition_census"]["cpu_fallback"] = 0
        self.assertEqual(MODULE.verify_receipt(accepted)["disposition_census"], {"completed": 2})
        self.assert_rejected(lambda value: value["disposition_census"].update(forged=0))

    def run_cli_receipt(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "receipt.json"
            path.write_text(text, encoding="utf-8")
            with mock.patch("sys.stdout", new_callable=io.StringIO) as stdout:
                code = MODULE.main([str(path)])
            return code, json.loads(stdout.getvalue())

    def test_cli_receipt_uses_strict_json(self):
        code, result = self.run_cli_receipt(json.dumps(receipt()))
        self.assertEqual((code, result["verdict"]), (0, "PASS"))
        # A duplicate key would otherwise let the last census silently win.
        forged = json.dumps(receipt())[:-1] + ', "disposition_census": {"completed": 2}}'
        code, result = self.run_cli_receipt(forged)
        self.assertEqual((code, result["verdict"]), (1, "FAIL"))
        self.assertIn("duplicate JSON key disposition_census", result["reason"])
        code, result = self.run_cli_receipt(json.dumps(receipt()).replace('"rows"', '"x": NaN, "rows"', 1))
        self.assertEqual((code, result["verdict"]), (1, "FAIL"))
        self.assertIn("non-finite JSON constant NaN", result["reason"])

    def test_strict_json_rejects_duplicate_and_nonfinite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            path.write_text('{"x":1,"x":2}', encoding="utf-8")
            with self.assertRaises(MODULE.ReceiptError):
                MODULE._load_json(path)
            path.write_text('{"x":NaN}', encoding="utf-8")
            with self.assertRaises(MODULE.ReceiptError):
                MODULE._load_json(path)

    IDENTITY = {"source_revision": "a" * 40, "binary_sha256": "b" * 64,
                "provider_revision": "c" * 40, "provider_asset_sha256": "d" * 64,
                "model_sha256": "e" * 64, "resident_plan_sha256": "f" * 64}
    KEY = (7, 2, 0)

    def artifact_fixture(self):
        """The positive artifact set: one admission and terminal for one raw block."""
        raw_record = {"record_kind": "block", "engine_id": 7, "generation": 2, "sequence": 0,
                      "gpu_terminal": "completed", "delivery": "gpu"}
        identity = self.IDENTITY
        admission = {"event": "admission", "upid": 1, "session_id": "s",
                     **{k: raw_record[k] for k in ("engine_id", "generation", "sequence")},
                     "model_hash": identity["model_sha256"],
                     "resident_plan_hash": identity["resident_plan_sha256"],
                     "provider_source_hash": identity["provider_asset_sha256"],
                     "provider_executable_hash": identity["binary_sha256"]}
        terminal = dict(admission, event="terminal", admission_ns=10, terminal_ns=20,
                        gpu_terminal="completed", outcome="success", delivery="gpu",
                        gpu_elapsed_available=False, gpu_elapsed_ns=None)
        return {"raw_manifest": {"record_kind": "manifest", **identity},
                "raw_blocks": [raw_record],
                "trace": {"schema": "pulp.gpu-audio.p5.perfetto-sql.v1", "flushed": True,
                          "rows": [admission, terminal]},
                "manifest": {"schema": "pulp.gpu-audio.p5.run-manifest.v1", **identity,
                             "upid": 1, "session_id": "s", "expected_admissions": 1}}

    def verify_fixture(self, mutate=None, paths=None):
        """Write the (mutated) fixture with consistent digests and verify it.

        ``paths`` may rewrite the (raw, trace, manifest) argument triple after
        the files are written, to exercise path-identity checks.
        """
        fixture = self.artifact_fixture()
        if mutate is not None:
            mutate(fixture)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "raw.jsonl"; trace = root / "trace.json"; manifest_path = root / "manifest.json"
            raw.write_text("raw", encoding="utf-8")
            trace.write_text(json.dumps(fixture["trace"]), encoding="utf-8")
            manifest = dict(fixture["manifest"])
            manifest.setdefault("raw_sha256", hashlib.sha256(raw.read_bytes()).hexdigest())
            manifest.setdefault("trace_sha256", hashlib.sha256(trace.read_bytes()).hexdigest())
            args = (raw, trace, manifest_path)
            if paths is not None:
                args = paths(root, *args)
                manifest["raw_sha256"] = hashlib.sha256(args[0].read_bytes()).hexdigest()
                manifest["trace_sha256"] = hashlib.sha256(args[1].read_bytes()).hexdigest()
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            records = [fixture["raw_manifest"], *fixture["raw_blocks"]]
            fake = mock.Mock(read_jsonl=lambda _: (records, []), validate_records=lambda _: [])
            with mock.patch.dict(sys.modules, {"gpu_audio_p4_evidence": fake}):
                return MODULE.verify_artifacts(*args, expected_source_revision="a" * 40)

    def assert_artifacts_rejected(self, message, mutate=None, paths=None):
        with self.assertRaises(MODULE.ReceiptError) as raised:
            self.verify_fixture(mutate, paths)
        self.assertEqual(str(raised.exception), message)

    def test_artifact_fixture_positive_reconciles_raw_and_trace(self):
        self.assertEqual(self.verify_fixture()["verdict"], "PASS")

    def test_artifact_manifest_missing_or_mistyped_fields_fail_closed(self):
        # The positive fixture above supplies every field; each mutation must
        # surface as a ReceiptError verdict rather than an uncaught exception.
        for field, value in (("raw_sha256", 7), ("trace_sha256", None)):
            with self.subTest(field=field):
                self.assert_artifacts_rejected(
                    "run manifest is missing required identity or artifact digest",
                    lambda f, field=field, value=value: f["manifest"].update({field: value}))
        self.assert_artifacts_rejected(
            "run manifest process/session identity is required",
            lambda f: f["manifest"].pop("upid"))

    def test_artifact_fixture_rejects_unflushed_trace(self):
        self.assert_artifacts_rejected(
            "Perfetto capture is not marked flushed",
            lambda f: f["trace"].update(flushed=False))

    def test_artifact_fixture_rejects_gpu_timing_availability_mismatch(self):
        def mutate(f):
            f["raw_blocks"][0]["timings"] = {"gpu_elapsed": {"availability": "available", "value_ns": 5}}
        self.assert_artifacts_rejected(
            f"Perfetto GPU timing availability disagrees with raw P4 block {self.KEY}", mutate)

    def test_artifact_fixture_rejects_gpu_elapsed_mismatch(self):
        def mutate(f):
            f["raw_blocks"][0]["timings"] = {"gpu_elapsed": {"availability": "available", "value_ns": 5}}
            f["trace"]["rows"][1].update(gpu_elapsed_available=True, gpu_elapsed_ns=6)
        self.assert_artifacts_rejected(
            f"Perfetto GPU elapsed timing disagrees with raw P4 block {self.KEY}", mutate)

    def test_artifact_fixture_accepts_matching_gpu_elapsed(self):
        def mutate(f):
            f["raw_blocks"][0]["timings"] = {"gpu_elapsed": {"availability": "available", "value_ns": 5}}
            f["trace"]["rows"][1].update(gpu_elapsed_available=True, gpu_elapsed_ns=5)
        self.assertEqual(self.verify_fixture(mutate)["verdict"], "PASS")

    def test_artifact_fixture_rejects_each_forged_hash(self):
        for field in ("model_hash", "resident_plan_hash", "provider_source_hash",
                      "provider_executable_hash"):
            with self.subTest(field=field):
                self.assert_artifacts_rejected(
                    f"Perfetto row 0 has forged {field}",
                    lambda f, field=field: f["trace"]["rows"][0].update({field: "0" * 64}))

    def test_artifact_fixture_rejects_terminal_outcome_mismatch(self):
        self.assert_artifacts_rejected(
            "Perfetto outcome disagrees with gpu_terminal for row 1",
            lambda f: f["trace"]["rows"][1].update(outcome="device_lost"))

    def test_artifact_fixture_rejects_raw_key_mismatch(self):
        self.assert_artifacts_rejected(
            "Perfetto lifecycle identities do not exactly match raw P4 blocks",
            lambda f: f["raw_blocks"][0].update(sequence=1))

    def test_artifact_fixture_rejects_raw_disposition_mismatch(self):
        self.assert_artifacts_rejected(
            f"Perfetto terminal disposition disagrees with raw P4 block {self.KEY}",
            lambda f: f["raw_blocks"][0].update(delivery="cpu_fallback"))

    def test_artifact_fixture_rejects_upid_and_session_mismatch(self):
        for field, value in (("upid", 2), ("session_id", "other")):
            with self.subTest(field=field):
                self.assert_artifacts_rejected(
                    "Perfetto row 0 process/session identity disagrees with manifest",
                    lambda f, field=field, value=value: f["trace"]["rows"][0].update({field: value}))

    def test_artifact_fixture_rejects_aliased_paths(self):
        def symlink(root, raw, trace, manifest):
            alias = root / "trace-alias.json"
            alias.symlink_to(trace)
            return alias, trace, manifest
        self.assert_artifacts_rejected("artifact paths must be distinct and cannot alias",
                                       paths=symlink)

        def hardlink(root, raw, trace, manifest):
            alias = root / "trace-hardlink.json"
            os.link(trace, alias)
            return alias, trace, manifest
        self.assert_artifacts_rejected("artifact paths must not hardlink the same inode",
                                       paths=hardlink)

if __name__ == "__main__":
    unittest.main()
