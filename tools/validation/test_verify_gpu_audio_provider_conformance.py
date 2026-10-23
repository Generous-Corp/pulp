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


COUNTERS = {
    # name: (submissions, retired_success, retired_failure, rejected_callbacks)
    "nominal": (4, 4, 0, 0),
    "force_loss_before_submit": (1, 0, 1, 0),
    "force_loss_between_submit_and_registration": (1, 0, 1, 0),
    "force_loss_after_registration": (1, 0, 1, 0),
    "late_completion": (1, 1, 0, 0),
    "missing_completion": (1, 1, 0, 0),
    "refused_submission": (0, 0, 0, 0),
    "terminal_busy_retry": (1, 1, 0, 1),
    "teardown_quiescence": (2, 2, 0, 0),
    "drain_after_loss": (2, 0, 2, 0),
    "stale_generation_replay": (2, 2, 0, 1),
    "differential_cpu_reference": (32, 32, 0, 0),
    "differential_provider_pair": (64, 64, 0, 0),
}


def row(name, verdict="passed"):
    submissions, success, failure, rejected = COUNTERS[name]
    return {"name": name, "verdict": verdict, "reason": "ok", "submissions": submissions,
            "retired_success": success, "retired_failure": failure,
            "rejected_callbacks": rejected,
            "max_abs_error": 0.0 if name in MODULE.DIFFERENTIAL_SCENARIOS else None}


def receipt(kind="real"):
    real = kind == "real"
    rows = [row(name) for name in MODULE.REQUIRED_SCENARIOS]
    rows.append(row("differential_provider_pair"))
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
                "adapter_device_id": 1,
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


def named(value, name):
    return next(item for item in value["scenarios"] if item["name"] == name)


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

    def test_kind_is_derived_from_identity_not_the_label(self):
        # A real provider labelled fake: its identity still derives "real".
        value = receipt("real")
        value["provider"]["kind"] = "fake"
        self.assert_rejected(value, "disagrees with the kind its identity derives")
        # A provider that names no backend cannot be admitted under any label.
        for label in ("fake", "real"):
            value = receipt("fake")
            value["provider"]["kind"] = label
            value["provider"]["identity"]["adapter_backend"] = ""
            self.assert_rejected(value, "names no adapter backend")

    def test_require_kind(self):
        self.assertEqual(MODULE.verify_receipt(receipt("real"), require_kind="real")["kind"],
                         "real")
        self.assert_rejected(receipt("fake"), "real is required", require_kind="real")
        self.assert_rejected(receipt("real"), "fake is required", require_kind="fake")

    def test_program_is_pinned(self):
        for key, bad in (("scale", 2.0), ("bias", 0.0), ("tolerance", 1.0e-3),
                         ("name", "something-else")):
            value = receipt()
            value["program"][key] = bad
            self.assert_rejected(value, f"program.{key}")

    def test_counters_are_sanity_checked(self):
        for name, field, bad in (("nominal", "submissions", 0),
                                 ("nominal", "rejected_callbacks", 1),
                                 ("force_loss_after_registration", "retired_success", 1),
                                 ("drain_after_loss", "retired_failure", 0),
                                 ("refused_submission", "retired_failure", 1),
                                 ("late_completion", "rejected_callbacks", 1),
                                 ("teardown_quiescence", "retired_success", 0),
                                 ("stale_generation_replay", "rejected_callbacks", 2),
                                 ("differential_cpu_reference", "submissions", 3),
                                 ("differential_provider_pair", "submissions", 32)):
            value = receipt()
            named(value, name)[field] = bad
            self.assert_rejected(value, f"scenario {name} passed with impossible counters")

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
        self.assert_rejected(value, "disagrees with the kind")
        value = receipt("fake")
        value["provider"]["identity"]["adapter_backend"] = "metal"
        self.assert_rejected(value, "disagrees with the kind")

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
        named(value, "force_loss_after_registration")["verdict"] = "failed"
        self.assert_rejected(value, "scenarios failed: force_loss_after_registration")

    def test_writer_verdict_must_agree(self):
        value = receipt()
        value["verdict"] = "failed"
        self.assert_rejected(value, "writer verdict")

    def test_differential_residual_is_recomputed(self):
        value = receipt()
        row = next(row for row in value["scenarios"]
                   if row["name"] == "differential_cpu_reference")
        row["max_abs_error"] = 2.0e-5
        self.assert_rejected(value, "residual within tolerance")
        row["max_abs_error"] = None
        self.assert_rejected(value, "residual within tolerance")

    def test_skips_are_typed_and_optional_to_forbid(self):
        value = receipt()
        busy = named(value, "terminal_busy_retry")
        busy["verdict"] = "skipped"
        busy["reason"] = "busy_retry_unobservable"
        self.assertEqual(MODULE.verify_receipt(value)["skipped"], ["terminal_busy_retry"])
        self.assert_rejected(value, "was skipped", forbid_skips=True)
        busy["reason"] = ""
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
            self.assertEqual(MODULE.main([str(path), "--require-kind", "fake"]), 1)
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

    def test_missing_receipt_skips_only_when_asked(self):
        with tempfile.TemporaryDirectory() as directory:
            absent = Path(directory) / "absent.json"
            self.assertEqual(MODULE.main([str(absent), "--skip-if-missing"]), MODULE.SKIP_EXIT)
            # Without the flag an absent receipt is a failure, never a pass.
            self.assertEqual(MODULE.main([str(absent)]), 1)
            # The flag never excuses a receipt that is present but bad.
            value = receipt("real")
            value["provider"]["identity"]["authenticated"] = False
            absent.write_text(json.dumps(value), encoding="utf-8")
            self.assertEqual(MODULE.main([str(absent), "--skip-if-missing"]), 1)


if __name__ == "__main__":
    unittest.main()
