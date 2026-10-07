import stat
import tempfile
import unittest
import json
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest.mock import patch

import gpu_audio_p2_campaign as campaign


class P2CampaignContractTests(unittest.TestCase):
    @staticmethod
    def _rows():
        bindings = {
            "provider_asset_sha256": "a" * 64,
            "dawn_archive_sha256": "b" * 64,
            "provider_asset_manifest_sha256": "c" * 64,
            "dawn_archive_manifest_sha256": "d" * 64,
        }
        provenance = {
            "kind": "provenance", "schema": "pulp.gpu-audio.p2.raw.v1",
            "engine_id": 7,
            "provider_identity_status": "passed", "provider_observed_identity": "passed",
            "provider_revision": "e" * 40, "adapter_name": "Apple GPU",
            "adapter_backend": "metal", "adapter_vendor_id": 0x106B,
            "adapter_device_id": 1, "native_runtime_identity_status": "passed",
            "native_runtime_name": "Metal", "native_runtime_backend": "metal",
            "run_kind": "cold",
            "executable_observed_sha256": "f" * 64,
            "provider_asset_sha256": bindings["provider_asset_sha256"],
            "dawn_archive_sha256": bindings["dawn_archive_sha256"],
            "manifest_bindings": bindings,
        }
        provenance["provenance_manifest_sha256"] = campaign.provenance_manifest_sha256(provenance)
        admission = {"kind": "admission", "engine_id": 7, "generation": 1, "sequence": 2}
        terminal = {"kind": "record", "engine_id": 7, "trace_kind": 0, "generation": 1,
                    "sequence": 2, "valid_stages": 1, "gpu_terminal": 1,
                    "admission_identity_matched": True}
        delivery = {"kind": "record", "engine_id": 7, "trace_kind": 2, "generation": 1,
                    "sequence": 2, "delivery": 1, "callback_timing_available": True,
                    "callback_end_ns": 10, "result_visible_ns": 11,
                    "admitted": True, "callback_only": False}
        return [provenance, admission, terminal, delivery]

    def test_identity_rows_reject_duplicate_or_missing_terminals(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        campaign.validate_identity_rows(rows, expected_sha)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows + [rows[1]], expected_sha)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows[:-1], expected_sha)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows, "different-probe")
        missing_native = [dict(row) for row in rows]
        missing_native[0].pop("native_runtime_identity_status")
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(missing_native, expected_sha)
        mismatch = [dict(row) for row in rows]
        mismatch[0]["provider_identity_status"] = "mismatch"
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)
        for field in ("provider_asset_sha256", "dawn_archive_sha256",
                      "provider_asset_manifest_sha256", "dawn_archive_manifest_sha256"):
            invalid = [dict(row) for row in rows]
            if field in ("provider_asset_sha256", "dawn_archive_sha256"):
                invalid[0][field] = "unknown"
            else:
                invalid[0]["manifest_bindings"] = dict(invalid[0]["manifest_bindings"])
                invalid[0]["manifest_bindings"][field] = "unknown"
            with self.assertRaises(RuntimeError):
                campaign.validate_identity_rows(invalid, expected_sha)

    def test_lossless_observer_retains_per_admission_terminal_and_delivery(self):
        rows = self._rows()
        observer = campaign.LosslessLifecycleObserver()
        for row in rows:
            observer.observe(row)
        observer.validate(rows[0]["executable_observed_sha256"])
        self.assertEqual(observer.rows, tuple(rows))
        identities = observer.identities()
        self.assertEqual(set(identities), {(7, 1, 2)})
        self.assertEqual(set(identities[(7, 1, 2)]), {"terminal", "delivery"})

    def test_observe_jsonl_rejects_blank_or_malformed_rows(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "raw.jsonl"
            path.write_text('{"kind":"provenance"}\n\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "blank line"):
                campaign.observe_jsonl(path)
            path.write_text('{not-json}\n', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "invalid JSON"):
                campaign.observe_jsonl(path)

    def test_manifest_digest_binds_asset_and_archive_hashes(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        digest = rows[0]["provenance_manifest_sha256"]
        campaign.validate_identity_rows(rows, expected_sha, digest)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows, expected_sha, "0" * 64)
        changed = [dict(row) for row in rows]
        changed[0]["manifest_bindings"] = dict(changed[0]["manifest_bindings"])
        changed[0]["manifest_bindings"]["dawn_archive_sha256"] = "1" * 64
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(changed, expected_sha)

    def test_observed_device_id_zero_is_allowed(self):
        rows = self._rows()
        rows[0]["adapter_device_id"] = 0
        campaign.validate_identity_rows(rows, rows[0]["executable_observed_sha256"])

    def test_provider_and_native_backends_must_be_matching_metal(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        non_metal = [dict(row) for row in rows]
        non_metal[0]["adapter_backend"] = "vulkan"
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(non_metal, expected_sha)
        mismatch = [dict(row) for row in rows]
        mismatch[0]["native_runtime_backend"] = "vulkan"
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)

    def test_provenance_engine_identity_must_match_every_row(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        mismatch = [dict(row) for row in rows]
        mismatch[0]["engine_id"] = 8
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)
        mismatch = [dict(row) for row in rows]
        mismatch[1]["engine_id"] = 8
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(mismatch, expected_sha)

    def test_raw_census_requires_admission_and_receipt_counter_match(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        callback_only = {
            "kind": "record", "engine_id": 7, "trace_kind": 2, "generation": 1,
            "sequence": 3, "delivery": 2, "callback_timing_available": True,
            "callback_end_ns": 20, "result_visible_ns": 21,
            "admitted": False, "callback_only": True,
        }
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows[:1] + [callback_only], expected_sha)
        receipt = {
            "warmup_blocks": 0, "measured_blocks": 1, "declared_lead_blocks": 0,
            "total_callbacks": 1, "callback_count": 1, "measured_callback_count": 1,
            "admission_granularity": "one_per_callback", "admission_record_count": 1,
            "delivery_record_count": 1, "admissions_enqueued": 1,
            "terminal_record_count": 1, "authenticated_terminal_records": 1,
        }
        campaign.validate_identity_rows(rows, expected_sha, receipt=receipt)
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(rows, expected_sha,
                                            receipt={**receipt, "admissions_enqueued": 2})

    def test_raw_census_rejects_truncated_callback_stream(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        receipt = {
            "warmup_blocks": 0, "measured_blocks": 1, "declared_lead_blocks": 0,
            "total_callbacks": 1, "callback_count": 1, "measured_callback_count": 1,
            "admission_granularity": "one_per_callback", "admission_record_count": 1,
            "delivery_record_count": 1, "admissions_enqueued": 1,
            "terminal_record_count": 1, "authenticated_terminal_records": 1,
        }
        campaign.validate_identity_rows(rows, expected_sha, receipt=receipt)
        with self.assertRaisesRegex(RuntimeError, "delivery census|identity multisets"):
            campaign.validate_identity_rows(rows[:-1], expected_sha, receipt=receipt)
        with self.assertRaisesRegex(RuntimeError, "callback geometry|callback count"):
            campaign.validate_identity_rows(
                rows, expected_sha, receipt={**receipt, "callback_count": 2,
                                             "total_callbacks": 2,
                                             "delivery_record_count": 2,
                                             "admission_record_count": 2,
                                             "admissions_enqueued": 2,
                                             "terminal_record_count": 2,
                                             "authenticated_terminal_records": 2})

    def test_negative_control_requires_observed_oracle_failure(self):
        valid = {"status": "failed", "negative_control": True,
                 "oracle_failed_blocks": campaign.NEGATIVE_CONTROL_FAILED_BLOCKS}
        campaign.validate_negative_control_receipt(valid)
        for invalid in (
                {"status": "failed", "negative_control": True, "oracle_failed_blocks": 0},
                {"status": "failed", "negative_control": True},
                {"status": "completed", "negative_control": True,
                 "oracle_failed_blocks": campaign.NEGATIVE_CONTROL_FAILED_BLOCKS},
                {"status": "failed", "negative_control": False,
                 "oracle_failed_blocks": campaign.NEGATIVE_CONTROL_FAILED_BLOCKS},
        ):
            with self.assertRaisesRegex(RuntimeError, "negative control"):
                campaign.validate_negative_control_receipt(invalid)

    def test_delivery_disposition_must_match_terminal_census(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        invalid = [dict(row) for row in rows]
        invalid[-1]["delivery"] = 2  # CPU fallback is a typed disposition.
        # The fallback row remains valid because terminal and delivery are
        # intentionally orthogonal; a late GPU result may still fall back.
        campaign.validate_identity_rows(invalid, expected_sha)
        missing_timing = [dict(row) for row in rows]
        missing_timing[-1]["callback_timing_available"] = False
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(missing_timing, expected_sha)
        bad_gpu_pair = [dict(row) for row in rows]
        bad_gpu_pair[-1]["delivery"] = 1
        bad_gpu_pair[-2]["gpu_terminal"] = 2
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(bad_gpu_pair, expected_sha)

    def test_callback_only_delivery_rows_are_retained_and_classified(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        callback_only = {
            "kind": "record", "engine_id": 7, "trace_kind": 2, "generation": 1,
            "sequence": 3, "delivery": 2, "callback_timing_available": True,
            "callback_end_ns": 20, "result_visible_ns": 21,
            "admitted": False, "callback_only": True,
        }
        campaign.validate_identity_rows(rows + [callback_only], expected_sha)
        wrong_classification = list(rows) + [dict(callback_only, admitted=True)]
        with self.assertRaises(RuntimeError):
            campaign.validate_identity_rows(wrong_classification, expected_sha)

    def test_identity_rows_retain_known_lifecycle_kinds_and_reject_unknown_or_malformed(self):
        rows = self._rows()
        expected_sha = rows[0]["executable_observed_sha256"]
        unknown = [dict(row) for row in rows]
        unknown.append(dict(rows[-1], trace_kind=99))
        with self.assertRaisesRegex(RuntimeError, "unknown trace kind"):
            campaign.validate_identity_rows(unknown, expected_sha)
        eligible = dict(rows[-1], trace_kind=1, generation=1, sequence=3,
                        valid_stages=1, gpu_work_admitted=False,
                        output_eligible=False, gpu_terminal=0, delivery=0)
        recovery = dict(eligible, trace_kind=3, next_generation=2)
        campaign.validate_identity_rows(rows + [eligible, recovery], expected_sha)
        zero_stage_eligible = dict(eligible, sequence=4, valid_stages=0)
        campaign.validate_identity_rows(rows + [zero_stage_eligible], expected_sha)
        malformed = dict(eligible, valid_stages=-1)
        with self.assertRaisesRegex(RuntimeError, "malformed known trace kind"):
            campaign.validate_identity_rows(rows + [malformed], expected_sha)
        malformed_recovery = dict(recovery, next_generation=1)
        with self.assertRaisesRegex(RuntimeError, "malformed Recovery"):
            campaign.validate_identity_rows(rows + [malformed_recovery], expected_sha)

    def test_steady_receipt_requires_same_process_residency(self):
        base = {
            "schema": "pulp.gpu-audio-paced-convolution.v1", "status": "completed",
            "gpu_receipt_authenticated": True, "declared_slots": 2,
            "declared_lead_blocks": 1, "run_kind": "steady",
            "authenticated_terminal_records": 500001, "terminal_record_count": 500001,
            "retired_success": 500001, "retired_failure": 0, "admissions_attempted": 500001,
            "admissions_enqueued": 1, "admissions_dropped": 0, "trace_attempted": 1,
            "trace_enqueued": 1, "trace_dropped": 0, "trace_sampled_out": 0,
            "trace_invalid": 0, "high_water_in_flight": 1,
            "measured_blocks": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND,
            "measured_blocks_per_repetition": campaign.REQUIRED_MEASURED_BLOCKS,
            "steady_repetitions": campaign.RUNS_PER_KIND,
            "warmup_blocks": 0, "total_callbacks": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "callback_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "measured_callback_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND,
            "admission_granularity": "one_per_callback",
            "admission_record_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "delivery_record_count": campaign.REQUIRED_MEASURED_BLOCKS * campaign.RUNS_PER_KIND + 1,
            "provider_identity_status": "passed", "native_runtime_identity_status": "passed",
            "adapter_vendor_id": 1, "adapter_device_id": 1,
        }
        base["admissions_enqueued"] = base["admission_record_count"]
        base["admissions_attempted"] = base["admission_record_count"]
        base["trace_attempted"] = base["trace_enqueued"] = base["admission_record_count"]
        invalid = dict(base)
        with self.assertRaises(RuntimeError):
            campaign.validate_receipt(invalid, 2, 1, "steady")
        valid = dict(base, steady_semantics="same_process_resident",
                     same_process_resident=True, process_id=42,
                     residency_session_id="session-1", prepared_sessions=1,
                     reprepare_count=0)
        campaign.validate_receipt(valid, 2, 1, "steady")
    def test_defaults_to_required_100k_blocks(self):
        args = campaign.parse_args(["--probe", "/bin/true", "--output-dir", "/tmp/p2-contract-test"])
        self.assertEqual(args.blocks, campaign.REQUIRED_MEASURED_BLOCKS)

    def test_source_provenance_rejects_tracked_dirty_tree(self):
        dirty = type("Result", (), {"returncode": 0, "stdout": " M tools/scripts/gpu_audio_p2_campaign.py\n"})()
        with patch.object(campaign.subprocess, "run", return_value=dirty):
            with self.assertRaisesRegex(RuntimeError, "tracked modifications"):
                campaign._source_provenance()

    def test_manifest_provenance_rejects_driver_or_source_mismatch(self):
        source = "a" * 40
        driver = "b" * 64
        manifest = {"source_revision": source, "driver_sha256": driver,
                    "source_tree_clean": True}
        campaign.validate_manifest_provenance(manifest, source, driver)
        with self.assertRaisesRegex(RuntimeError, "source revision"):
            campaign.validate_manifest_provenance({**manifest, "source_revision": "c" * 40}, source, driver)
        with self.assertRaisesRegex(RuntimeError, "driver hash"):
            campaign.validate_manifest_provenance({**manifest, "driver_sha256": "d" * 64}, source, driver)
        with self.assertRaisesRegex(RuntimeError, "clean tracked"):
            campaign.validate_manifest_provenance({**manifest, "source_tree_clean": False}, source, driver)

    def test_post_trial_provenance_recheck_rejects_mid_run_mutation(self):
        with patch.object(campaign, "_source_provenance",
                          side_effect=[("a" * 40, "b" * 64), ("c" * 40, "b" * 64)]):
            initial = campaign._source_provenance()
            with self.assertRaisesRegex(RuntimeError, "provenance changed"):
                campaign._require_unchanged_source(initial)

    def test_source_provenance_rejects_untracked_driver_copy(self):
        with tempfile.TemporaryDirectory() as root:
            copied = Path(root) / "gpu_audio_p2_campaign.py"
            copied.write_text("# copied driver\n")
            with patch.object(campaign, "DRIVER_PATH", copied):
                clean = type("Result", (), {"returncode": 0, "stdout": ""})()
                with patch.object(campaign.subprocess, "run", return_value=clean):
                    with self.assertRaisesRegex(RuntimeError, "canonical tracked file"):
                        campaign._source_provenance()

    def test_plan_only_declares_complete_matrix(self):
        args = campaign.parse_args(["--plan-only"])
        self.assertEqual(campaign.run(args), 0)

    def test_plan_only_accepts_a_bounded_matrix_subset(self):
        args = campaign.parse_args(["--plan-only", "--slots", "16,2", "--leads", "8,1"])
        self.assertEqual(args.slots, (2, 16))
        self.assertEqual(args.leads, (1, 8))
        output = StringIO()
        with redirect_stdout(output):
            self.assertEqual(campaign.run(args), 0)
        plan = json.loads(output.getvalue())
        self.assertEqual(plan["cells"], [
            {"slots": 2, "lead": 1}, {"slots": 2, "lead": 8},
            {"slots": 16, "lead": 1}, {"slots": 16, "lead": 8},
        ])
        self.assertEqual(plan["trial_count"], 4 * 2 * campaign.RUNS_PER_KIND)

    def test_matrix_axis_rejects_duplicates_and_unknown_values(self):
        with self.assertRaises(SystemExit):
            campaign.parse_args(["--plan-only", "--slots", "2,2"])
        with self.assertRaises(SystemExit):
            campaign.parse_args(["--plan-only", "--leads", "3"])

    def test_lower_block_count_rejected_before_probe(self):
        with tempfile.TemporaryDirectory() as root:
            probe = Path(root) / "probe"
            probe.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            probe.chmod(probe.stat().st_mode | stat.S_IXUSR)
            args = campaign.parse_args([
                "--probe", str(probe), "--output-dir", str(Path(root) / "out"), "--blocks", "1024"
            ])
            with self.assertRaises(RuntimeError):
                campaign.run(args)


if __name__ == "__main__":
    unittest.main()
