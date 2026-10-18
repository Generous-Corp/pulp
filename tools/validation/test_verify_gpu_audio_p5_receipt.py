import copy
import importlib.util
import unittest
import json
import hashlib
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

    def test_strict_json_rejects_duplicate_and_nonfinite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            path.write_text('{"x":1,"x":2}', encoding="utf-8")
            with self.assertRaises(MODULE.ReceiptError):
                MODULE._load_json(path)
            path.write_text('{"x":NaN}', encoding="utf-8")
            with self.assertRaises(MODULE.ReceiptError):
                MODULE._load_json(path)

    def test_artifact_fixture_rejects_unflushed_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); raw = root / "raw.jsonl"; trace = root / "trace.json"; manifest_path = root / "manifest.json"
            raw.write_text("raw", encoding="utf-8")
            trace.write_text(json.dumps({"schema": "pulp.gpu-audio.p5.perfetto-sql.v1",
                                         "flushed": False, "rows": []}), encoding="utf-8")
            manifest_path.write_text(json.dumps({"schema": "pulp.gpu-audio.p5.run-manifest.v1"}), encoding="utf-8")
            fake = mock.Mock(read_jsonl=lambda _: ([{"record_kind": "manifest"}], []),
                             validate_records=lambda _: [])
            with mock.patch.dict(sys.modules, {"gpu_audio_p4_evidence": fake}):
                with self.assertRaises(MODULE.ReceiptError):
                    MODULE.verify_artifacts(raw, trace, manifest_path)

    def test_artifact_fixture_positive_reconciles_raw_and_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); raw = root / "raw.jsonl"; trace = root / "trace.json"; manifest_path = root / "manifest.json"
            raw.write_text("raw", encoding="utf-8")
            raw_record = {"record_kind": "block", "engine_id": 7, "generation": 2, "sequence": 0, "gpu_terminal": "completed", "delivery": "gpu"}
            identity = {"source_revision": "a" * 40, "binary_sha256": "b" * 64,
                        "provider_revision": "c" * 40, "provider_asset_sha256": "d" * 64,
                        "model_sha256": "e" * 64, "resident_plan_sha256": "f" * 64}
            row = {"event": "admission", "upid": 1, "session_id": "s", **{k: raw_record[k] for k in ("engine_id", "generation", "sequence")},
                   "model_hash": identity["model_sha256"], "resident_plan_hash": identity["resident_plan_sha256"], "provider_source_hash": identity["provider_asset_sha256"], "provider_executable_hash": identity["binary_sha256"]}
            terminal = dict(row, event="terminal", admission_ns=10, terminal_ns=20, gpu_terminal="completed", outcome="success", delivery="gpu", gpu_elapsed_available=False, gpu_elapsed_ns=None)
            trace.write_text(json.dumps({"schema": "pulp.gpu-audio.p5.perfetto-sql.v1", "flushed": True, "rows": [row, terminal]}), encoding="utf-8")
            manifest_path.write_text(json.dumps({"schema": "pulp.gpu-audio.p5.run-manifest.v1", **identity,
                "upid": 1, "session_id": "s",
                "raw_sha256": hashlib.sha256(raw.read_bytes()).hexdigest(), "trace_sha256": hashlib.sha256(trace.read_bytes()).hexdigest(), "expected_admissions": 1}), encoding="utf-8")
            fake = mock.Mock(read_jsonl=lambda _: ([{"record_kind": "manifest", **identity}, raw_record], []), validate_records=lambda _: [])
            with mock.patch.dict(sys.modules, {"gpu_audio_p4_evidence": fake}):
                self.assertEqual(MODULE.verify_artifacts(raw, trace, manifest_path, expected_source_revision="a" * 40)["verdict"], "PASS")


if __name__ == "__main__":
    unittest.main()
