import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("verify_gpu_audio_provider_conformance.py")
SPEC = importlib.util.spec_from_file_location("provider_conformance", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def receipt(kind="real"):
    real = kind == "real"
    rows = []
    for name in MODULE.REQUIRED_SCENARIOS:
        rows.append({"name": name, "verdict": "passed", "reason": "ok", "submissions": 1,
                     "retired_success": 1, "retired_failure": 0, "rejected_callbacks": 0,
                     "max_abs_error": 0.0 if name in MODULE.DIFFERENTIAL_SCENARIOS else None})
    rows.append({"name": "differential_provider_pair", "verdict": "passed", "reason": "ok",
                 "submissions": 32, "retired_success": 0, "retired_failure": 0,
                 "rejected_callbacks": 0, "max_abs_error": 2.0e-7})
    return {
        "schema": MODULE.SCHEMA,
        "kit_version": 1,
        "provider": {
            "name": "dawn" if real else "fake",
            "kind": kind,
            "identity": {
                "authenticated": True,
                "provider_revision": "a" * 40 if real else "fake-provider-v1",
                "adapter_name": "Apple M3" if real else "fake-adapter",
                "adapter_backend": "metal" if real else "fake",
                "adapter_vendor_id": 4203 if real else 1,
                "adapter_device_id": 1 if real else 1,
                "native_runtime_authenticated": real,
                "native_runtime_name": "Apple M3" if real else "",
                "native_runtime_backend": "metal" if real else "",
            },
            "capabilities": {key: False for key in MODULE.CAPABILITY_FIELDS},
        },
        "program": {"name": "affine-1.75x+0.25", "scale": 1.75, "bias": 0.25, "frames": 64,
                    "slots": 2, "blocks": 32, "tolerance": 1.0e-5},
        "required_scenarios": list(MODULE.REQUIRED_SCENARIOS),
        "scenarios": rows,
        "row_count": len(rows),
        "capacity": len(rows),
        "truncated": False,
        "dropped_rows": 0,
        "verdict": "passed",
    }


class ProviderConformanceVerifierTest(unittest.TestCase):
    def assert_rejected(self, value, message, **kwargs):
        with self.assertRaisesRegex(MODULE.ReceiptError, message):
            MODULE.verify_receipt(value, **kwargs)

    def test_positive_controls(self):
        self.assertEqual(MODULE.verify_receipt(receipt("real"))["verdict"], "PASS")
        self.assertEqual(MODULE.verify_receipt(receipt("fake"))["verdict"], "PASS")
        self.assertEqual(
            MODULE.verify_receipt(receipt("real"), forbid_skips=True, require_pair=True)["kind"],
            "real")

    def test_unauthenticated_real_provider_fails(self):
        value = receipt("real")
        value["provider"]["identity"]["authenticated"] = False
        value["provider"]["identity"]["native_runtime_authenticated"] = False
        self.assert_rejected(value, "authenticated SharedIoProviderIdentity")

    def test_unauthenticated_fake_provider_is_admitted(self):
        value = receipt("fake")
        value["provider"]["identity"]["authenticated"] = False
        self.assertEqual(MODULE.verify_receipt(value)["verdict"], "PASS")

    def test_missing_identity_fails(self):
        value = receipt("fake")
        del value["provider"]["identity"]
        self.assert_rejected(value, "identity")
        value = receipt("real")
        del value["provider"]["identity"]["adapter_name"]
        self.assert_rejected(value, "exactly the SharedIoProviderIdentity fields")

    def test_kind_and_backend_must_agree(self):
        value = receipt("real")
        value["provider"]["identity"]["adapter_backend"] = "fake"
        self.assert_rejected(value, "cannot report a fake backend")
        value = receipt("fake")
        value["provider"]["identity"]["adapter_backend"] = "metal"
        self.assert_rejected(value, "cannot claim a real backend")

    def test_native_authentication_requires_provider_authentication(self):
        value = receipt("fake")
        value["provider"]["identity"]["authenticated"] = False
        value["provider"]["identity"]["native_runtime_authenticated"] = True
        self.assert_rejected(value, "native runtime authentication")

    def test_truncated_receipt_fails(self):
        value = receipt()
        value["truncated"] = True
        self.assert_rejected(value, "truncated")
        value = receipt()
        value["dropped_rows"] = 1
        self.assert_rejected(value, "dropped rows")

    def test_row_count_must_match_rows(self):
        value = receipt()
        value["scenarios"].pop()
        self.assert_rejected(value, "row_count")

    def test_missing_scenario_fails(self):
        value = receipt()
        value["scenarios"] = [row for row in value["scenarios"]
                              if row["name"] != "teardown_quiescence"]
        value["row_count"] = len(value["scenarios"])
        self.assert_rejected(value, "missing scenarios: teardown_quiescence")

    def test_duplicate_scenario_fails(self):
        value = receipt()
        value["scenarios"].append(copy.deepcopy(value["scenarios"][0]))
        value["row_count"] = len(value["scenarios"])
        value["capacity"] = len(value["scenarios"])
        self.assert_rejected(value, "more than once")

    def test_failed_row_fails_even_when_writer_says_passed(self):
        value = receipt()
        value["scenarios"][3]["verdict"] = "failed"
        self.assert_rejected(value, "scenarios failed: force_loss_after_registration")

    def test_writer_verdict_must_agree(self):
        value = receipt()
        value["verdict"] = "failed"
        self.assert_rejected(value, "writer verdict")

    def test_differential_residual_is_recomputed(self):
        value = receipt()
        row = next(row for row in value["scenarios"]
                   if row["name"] == "differential_cpu_reference")
        row["max_abs_error"] = 1.0e-3
        self.assert_rejected(value, "residual within tolerance")
        row["max_abs_error"] = None
        self.assert_rejected(value, "residual within tolerance")

    def test_skips_are_typed_and_optional_to_forbid(self):
        value = receipt()
        value["scenarios"][7]["verdict"] = "skipped"
        value["scenarios"][7]["reason"] = "busy_retry_unobservable"
        self.assertEqual(MODULE.verify_receipt(value)["skipped"], ["terminal_busy_retry"])
        self.assert_rejected(value, "was skipped", forbid_skips=True)
        value["scenarios"][7]["reason"] = ""
        self.assert_rejected(value, "must carry a reason")

    def test_require_pair(self):
        value = receipt()
        value["scenarios"] = [row for row in value["scenarios"]
                              if row["name"] != "differential_provider_pair"]
        value["row_count"] = len(value["scenarios"])
        self.assertEqual(MODULE.verify_receipt(value)["verdict"], "PASS")
        self.assert_rejected(value, "provider-pair", require_pair=True)

    def test_unknown_scenario_and_schema_fail(self):
        value = receipt()
        value["scenarios"][0]["name"] = "made_up"
        self.assert_rejected(value, "unknown scenario")
        value = receipt()
        value["schema"] = "pulp.gpu-audio.provider-conformance.v0"
        self.assert_rejected(value, "schema")

    def test_file_level_truncation_and_duplicates_fail(self):
        text = json.dumps(receipt())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "receipt.json"
            path.write_text(text[: len(text) // 2], encoding="utf-8")
            with self.assertRaisesRegex(MODULE.ReceiptError, "cannot parse"):
                MODULE.load_receipt(path)
            path.write_text('{"schema": "a", "schema": "b"}', encoding="utf-8")
            with self.assertRaisesRegex(MODULE.ReceiptError, "duplicate JSON key"):
                MODULE.load_receipt(path)
            path.write_text(text.replace("0.0", "NaN", 1), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.ReceiptError, "non-finite"):
                MODULE.load_receipt(path)
            path.write_text(text, encoding="utf-8")
            self.assertEqual(MODULE.main([str(path), "--forbid-skips"]), 0)
            value = receipt()
            value["provider"]["identity"]["authenticated"] = False
            path.write_text(json.dumps(value), encoding="utf-8")
            self.assertEqual(MODULE.main([str(path)]), 1)

    def test_contract_schema_matches_the_receipt_shape(self):
        try:
            import jsonschema
        except ImportError:
            self.skipTest("jsonschema is not installed")
        schema_path = (Path(__file__).resolve().parents[2] / "docs" / "contracts"
                       / "gpu-audio-provider-conformance-v1.schema.json")
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        jsonschema.Draft202012Validator.check_schema(schema)
        validator = jsonschema.Draft202012Validator(schema)
        validator.validate(receipt("real"))
        validator.validate(receipt("fake"))
        truncated = receipt()
        truncated["truncated"] = True
        self.assertFalse(validator.is_valid(truncated))
        unauthenticated = receipt("real")
        unauthenticated["provider"]["identity"]["authenticated"] = False
        self.assertFalse(validator.is_valid(unauthenticated))
        missing_identity = receipt("fake")
        del missing_identity["provider"]["identity"]
        self.assertFalse(validator.is_valid(missing_identity))
        self.assertEqual(list(MODULE.REQUIRED_SCENARIOS),
                         schema["properties"]["required_scenarios"]["const"])


if __name__ == "__main__":
    unittest.main()
