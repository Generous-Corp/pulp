#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("gpu_audio_p4_evidence.py")
SPEC = importlib.util.spec_from_file_location("gpu_audio_p4_evidence", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def observation(value: int | None, *, available: bool = True) -> dict:
    return {
        "value_ns": value if available else None,
        "availability": "available" if available else "unavailable",
        "clock_domain": "mach_continuous_time",
        "observer": "p4_fixture",
        "api_source": "fixture_clock",
        "relation": "direct" if available else "unavailable",
    }


def fixture() -> list[dict]:
    records = [{
        "schema": MODULE.SCHEMA,
        "record_kind": "manifest",
        "campaign": "screening",
        "campaign_id": "fixture",
        "source_revision": "a" * 40,
        "binary_sha256": "b" * 64,
        "machine_id": "test-machine",
        "machine_model": "Apple test",
        "os_version": "macOS test",
        "adapter_name": "Apple test GPU",
        "adapter_backend": "Metal",
        "adapter_registry_id": 1234,
        "adapter_vendor_id": 0x106B,
        "adapter_device_id": 1,
        "provider": "Dawn Metal",
        "provider_revision": "c" * 40,
        "provider_asset_sha256": "d" * 64,
        "generated_utc": "2026-09-17T00:00:00Z",
        "build_type": "Release",
        "build_flags": ["-O3", "-DNDEBUG"],
        "paced": True,
        "warmup_blocks": 8,
        "expected_trials": 3,
        "expected_matched_pairs": 1,
        "expected_staged_sync_trials": 1,
        "expected_blocks_per_trial": 2,
        "bootstrap_seed": 17,
        "bootstrap_resamples": 1000,
        "row": {"block_frames": 128, "sample_rate_hz": 48000, "channels": 2,
                "ir_frames": 4096, "inflight_depth": 2, "lead_blocks": 2,
                "load": "quiet", "deadline_ns": 1000, "watchdog_ns": 5000},
    }]
    for trial_id, path in enumerate(("staged_async", "shared_async", "staged_sync")):
        pair_id = 0 if path != "staged_sync" else None
        records.append({"schema": MODULE.SCHEMA, "record_kind": "trial_begin",
                        "trial_id": trial_id, "pair_id": pair_id, "path": path})
        digest = hashlib.sha256()
        for ordinal in range(2):
            block = {
                "schema": MODULE.SCHEMA, "record_kind": "block", "trial_id": trial_id,
                "pair_id": pair_id, "path": path, "block_ordinal": ordinal,
                "engine_id": 7 + trial_id,
                "generation": 1, "sequence": ordinal,
                "timings": {name: observation(100 + ordinal) for name in MODULE.TIMINGS},
                "transfers": {name: 0 for name in MODULE.TRANSFER_COUNTERS},
                "gpu_terminal": "completed", "delivery": "gpu",
                "deadline_miss": False, "watchdog_expiry": False,
                "late_completion": False, "resync_drop": False,
            }
            records.append(block)
            digest.update(MODULE._canonical(block) + b"\n")
        records.append({"schema": MODULE.SCHEMA, "record_kind": "trial_end",
                        "trial_id": trial_id, "pair_id": pair_id, "path": path, "block_count": 2,
                        "blocks_sha256": digest.hexdigest(),
                        "ui_frame_p99": observation(1000), "duration": observation(2000),
                        "device_loss": False,
                        "audio_xrun": False, "driver_stall": False})
    for staged in (record for record in records if record.get("record_kind") == "block"
                   and record.get("path") == "staged_async"):
        staged["transfers"]["write_buffer_calls"] = 1
    # Refresh the staged trial digest after installing its positive control.
    staged_end = next(record for record in records if record.get("record_kind") == "trial_end"
                      and record.get("path") == "staged_async")
    digest = hashlib.sha256()
    for record in records:
        if record.get("record_kind") == "block" and record.get("path") == "staged_async":
            digest.update(MODULE._canonical(record) + b"\n")
    staged_end["blocks_sha256"] = digest.hexdigest()
    for sync in (record for record in records if record.get("record_kind") == "block"
                 and record.get("path") == "staged_sync"):
        sync["transfers"]["write_buffer_calls"] = 1
    sync_end = next(record for record in records if record.get("record_kind") == "trial_end"
                    and record.get("path") == "staged_sync")
    digest = hashlib.sha256()
    for record in records:
        if record.get("record_kind") == "block" and record.get("path") == "staged_sync":
            digest.update(MODULE._canonical(record) + b"\n")
    sync_end["blocks_sha256"] = digest.hexdigest()
    return records


def refresh_digest(records: list[dict], path: str) -> None:
    digest = hashlib.sha256()
    for record in records:
        if record.get("record_kind") == "block" and record.get("path") == path:
            digest.update(MODULE._canonical(record) + b"\n")
    end = next(record for record in records if record.get("record_kind") == "trial_end"
               and record.get("path") == path)
    end["blocks_sha256"] = digest.hexdigest()


class EvidenceTests(unittest.TestCase):
    def test_valid_fixture_and_summary(self) -> None:
        records = fixture()
        self.assertEqual(MODULE.validate_records(records), [])
        summary = MODULE.summarize(records)
        self.assertEqual(summary["trial_count"], 3)
        self.assertEqual(summary["paths"]["shared_async"]["blocks"], 2)
        self.assertAlmostEqual(
            summary["paths"]["shared_async"]["timings_ns"]["submit_to_completion"]["p99_9"],
            100.999,
        )
        self.assertEqual(summary["verdict"], "unassigned")
        self.assertEqual(
            summary["matched_improvement"]["submit_to_completion_p99_percent"]["pairs"],
            1,
        )
        self.assertEqual(
            summary["matched_improvement"]["submit_to_completion_p99_ns"]["mean"],
            0,
        )
        self.assertEqual(summary["provider"]["revision"], "c" * 40)
        self.assertEqual(
            summary["paths"]["shared_async"]["timing_provenance"]
            ["submit_to_completion"][0]["clock_domain"],
            "mach_continuous_time",
        )
        self.assertEqual(summary["row_gate"]["status"], "fail")

    def test_row_gate_passes_a_cpu_win_without_claiming_program_verdict(self) -> None:
        records = fixture()
        for record in records:
            if record.get("record_kind") == "block" and record.get("path") == "shared_async":
                for name in ("callback_cpu", "worker_pack_copy", "encode_cpu", "submit_cpu"):
                    record["timings"][name]["value_ns"] = 40
        refresh_digest(records, "shared_async")
        self.assertEqual(MODULE.validate_records(records), [])
        gate = MODULE.summarize(records)["row_gate"]
        self.assertEqual(gate["status"], "pass")
        self.assertEqual(gate["program_verdict"], "unassigned")

    def test_truncation_and_digest_are_rejected(self) -> None:
        records = fixture()[:-1]
        errors = MODULE.validate_records(records)
        self.assertTrue(any("open trial" in error for error in errors), errors)
        records = fixture()
        records[-1]["blocks_sha256"] = "0" * 64
        errors = MODULE.validate_records(records)
        self.assertTrue(any("blocks_sha256 mismatch" in error for error in errors), errors)

    def test_named_shared_transfer_is_rejected(self) -> None:
        records = fixture()
        shared = next(record for record in records if record.get("record_kind") == "block"
                      and record.get("path") == "shared_async")
        shared["transfers"]["write_buffer_calls"] = 1
        errors = MODULE.validate_records(records)
        self.assertTrue(any("named payload transfer" in error for error in errors), errors)

    def test_duplicate_block_identity_is_rejected(self) -> None:
        records = fixture()
        shared_blocks = [record for record in records if record.get("record_kind") == "block"
                         and record.get("path") == "shared_async"]
        shared_blocks[1]["sequence"] = shared_blocks[0]["sequence"]
        errors = MODULE.validate_records(records)
        self.assertTrue(any("duplicate block identity" in error for error in errors), errors)

    def test_malformed_pair_is_rejected(self) -> None:
        records = fixture()
        shared_begin = next(record for record in records if record.get("record_kind") == "trial_begin"
                            and record.get("path") == "shared_async")
        shared_begin["pair_id"] = 1
        errors = MODULE.validate_records(records)
        self.assertTrue(any("matched pair" in error for error in errors), errors)

    def test_staged_transfer_control_must_fire(self) -> None:
        records = fixture()
        staged = next(record for record in records if record.get("record_kind") == "block"
                      and record.get("path") == "staged_async")
        staged["transfers"]["write_buffer_calls"] = 0
        errors = MODULE.validate_records(records)
        self.assertTrue(any("completed staged_async block has no named payload transfer" in error
                            for error in errors), errors)

    def test_normal_confirmation_rejects_deadline_miss(self) -> None:
        records = fixture()
        records[0]["campaign"] = "confirmation"
        block = next(record for record in records if record.get("record_kind") == "block")
        block["deadline_miss"] = True
        errors = MODULE.validate_records(records)
        self.assertTrue(any("normal-load confirmation/default block missed" in error
                            for error in errors), errors)

    def test_unavailable_is_not_zero(self) -> None:
        records = fixture()
        block = next(record for record in records if record.get("record_kind") == "block")
        block["timings"]["gpu_elapsed"] = observation(0, available=False)
        block["timings"]["gpu_elapsed"]["value_ns"] = 0
        errors = MODULE.validate_records(records)
        self.assertTrue(any("must be null when unavailable" in error for error in errors), errors)

    def test_transfer_keys_and_derived_deadline_are_closed_world(self) -> None:
        records = fixture()
        block = next(record for record in records if record.get("record_kind") == "block")
        block["transfers"]["mystery_copy_calls"] = 1
        block["deadline_miss"] = True
        errors = MODULE.validate_records(records)
        self.assertTrue(any("exactly the declared counter keys" in error for error in errors), errors)
        self.assertTrue(any("deadline_miss disagrees" in error for error in errors), errors)

    def test_terminal_delivery_combinations_are_consistent(self) -> None:
        records = fixture()
        block = next(record for record in records if record.get("record_kind") == "block")
        block["gpu_terminal"] = "late_rejected"
        errors = MODULE.validate_records(records)
        self.assertTrue(any("GPU delivery requires" in error for error in errors), errors)
        self.assertTrue(any("late_completion must agree" in error for error in errors), errors)

    def test_malformed_nested_types_fail_closed_without_crashing(self) -> None:
        records = fixture()
        records[0]["expected_trials"] = True
        begin = next(record for record in records if record.get("record_kind") == "trial_begin")
        begin["path"] = []
        block = next(record for record in records if record.get("record_kind") == "block")
        block["timings"]["gpu_elapsed"] = []
        errors = MODULE.validate_records(records)
        self.assertTrue(any("expected_trials" in error for error in errors), errors)
        self.assertTrue(any("path is invalid" in error for error in errors), errors)
        self.assertTrue(any("observation object" in error for error in errors), errors)

    def test_confirmation_and_default_minima_are_enforced(self) -> None:
        records = fixture()
        records[0]["campaign"] = "confirmation"
        errors = MODULE.validate_records(records)
        self.assertTrue(any("at least 30 matched pairs" in error for error in errors), errors)
        records[0]["campaign"] = "default"
        errors = MODULE.validate_records(records)
        self.assertTrue(any("at least 100000 blocks" in error for error in errors), errors)
        self.assertTrue(any("concurrent UI/GPU load" in error for error in errors), errors)
        self.assertTrue(any("confirmation_campaign_id" in error for error in errors), errors)
        self.assertTrue(any("confirmation_summary_sha256" in error for error in errors), errors)

    def test_campaign_load_and_trial_shape_bypasses_are_rejected(self) -> None:
        records = fixture()
        records[0]["row"]["load"] = "overload"
        errors = MODULE.validate_records(records)
        self.assertTrue(any("campaign=overload" in error for error in errors), errors)
        records = fixture()
        records[0]["expected_trials"] = 2
        records[0]["expected_staged_sync_trials"] = 0
        errors = MODULE.validate_records(records)
        self.assertTrue(any("exactly one staged_sync" in error for error in errors), errors)

    def test_overload_campaign_requires_budgeted_failure_evidence(self) -> None:
        records = fixture()
        records[0]["campaign"] = "overload"
        records[0]["row"]["load"] = "overload"
        errors = MODULE.validate_records(records)
        self.assertTrue(any("did not exercise" in error for error in errors), errors)
        records[0]["row"]["watchdog_ns"] = records[0]["row"]["deadline_ns"]
        errors = MODULE.validate_records(records)
        self.assertTrue(any("must be greater" in error for error in errors), errors)

    def test_confirmation_requires_complete_positive_pair_metrics(self) -> None:
        records = fixture()
        records[0]["campaign"] = "confirmation"
        records[0]["expected_matched_pairs"] = 30
        records[0]["expected_trials"] = 61
        records[0]["bootstrap_resamples"] = 10_000
        for block in (record for record in records if record.get("record_kind") == "block"
                      and record.get("path") == "staged_async"):
            block["timings"]["submit_to_completion"]["value_ns"] = 0
        refresh_digest(records, "staged_async")
        errors = MODULE.validate_records(records)
        self.assertTrue(any("baseline must be positive" in error for error in errors), errors)
        self.assertTrue(any("completed matched pair count" in error for error in errors), errors)

    def test_binary_identity_is_verified(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            binary = Path(root) / "benchmark"
            binary.write_bytes(b"benchmark-v1")
            records[0]["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
            self.assertEqual(MODULE.validate_binary(records[0], binary), [])
            binary.write_bytes(b"benchmark-v2")
            self.assertTrue(any("does not match" in error
                                for error in MODULE.validate_binary(records[0], binary)))

    def test_cli_writes_plot_ready_csv(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            raw = Path(root) / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
            parsed, errors = MODULE.read_jsonl(raw)
            self.assertEqual(errors, [])
            self.assertEqual(MODULE.validate_records(parsed), [])
            csv_path = Path(root) / "blocks.csv"
            MODULE.write_csv(parsed, csv_path)
            text = csv_path.read_text(encoding="utf-8")
            self.assertIn("submit_to_completion_value_ns", text)
            self.assertIn("submit_to_completion_clock_domain", text)
            self.assertIn("write_buffer_calls", text)
            self.assertIn("provider_revision", text)
            self.assertIn("trial_end", text)
            parsed.close()
        self.assertEqual(len(text.splitlines()), 10)

    def test_cli_verifies_binary_and_emits_detached_provenance(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"p4-benchmark")
            records[0]["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records),
                           encoding="utf-8")
            summary = root_path / "summary.json"
            csv_path = root_path / "blocks.csv"
            completed = subprocess.run(
                [sys.executable, str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                 "--summary", str(summary), "--csv", str(csv_path)],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads(summary.read_text(encoding="utf-8"))
            self.assertEqual(result["binary_sha256"], records[0]["binary_sha256"])
            self.assertEqual(result["evidence_sha256"], hashlib.sha256(raw.read_bytes()).hexdigest())
            self.assertEqual(result["adapter"]["registry_id"], 1234)
            self.assertEqual(result["row_gate"]["program_verdict"], "unassigned")


if __name__ == "__main__":
    unittest.main()
