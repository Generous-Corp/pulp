import stat
import tempfile
import unittest
from pathlib import Path

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
                    "callback_end_ns": 10, "result_visible_ns": 11}
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

    def test_steady_receipt_requires_same_process_residency(self):
        base = {
            "schema": "pulp.gpu-audio-paced-convolution.v1", "status": "completed",
            "gpu_receipt_authenticated": True, "declared_slots": 2,
            "declared_lead_blocks": 1, "run_kind": "steady",
            "authenticated_terminal_records": 1, "terminal_record_count": 1,
            "retired_success": 1, "retired_failure": 0, "admissions_attempted": 1,
            "admissions_enqueued": 1, "admissions_dropped": 0, "trace_attempted": 1,
            "trace_enqueued": 1, "trace_dropped": 0, "trace_sampled_out": 0,
            "trace_invalid": 0, "high_water_in_flight": 1, "retired_success": 1,
            "measured_blocks": campaign.REQUIRED_MEASURED_BLOCKS,
            "provider_identity_status": "passed", "native_runtime_identity_status": "passed",
            "adapter_vendor_id": 1, "adapter_device_id": 1,
        }
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

    def test_lower_block_count_rejected_before_probe(self):
        with tempfile.TemporaryDirectory() as root:
            probe = Path(root) / "probe"
            probe.write_text("#!/bin/sh\nexit 0\n")
            probe.chmod(probe.stat().st_mode | stat.S_IXUSR)
            args = campaign.parse_args([
                "--probe", str(probe), "--output-dir", str(Path(root) / "out"), "--blocks", "1024"
            ])
            with self.assertRaises(RuntimeError):
                campaign.run(args)


if __name__ == "__main__":
    unittest.main()
