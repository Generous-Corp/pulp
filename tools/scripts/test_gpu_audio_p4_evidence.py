#!/usr/bin/env python3
from __future__ import annotations

import contextlib
import copy
import hashlib
import importlib.util
import json
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


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
        "start_clock_domain": "mach_continuous_time",
        "end_clock_domain": "mach_continuous_time",
        "start_observer": "p4_fixture",
        "end_observer": "p4_fixture",
        "callback_mode": "AllowProcessEvents",
        "event_pump_strategy": "bounded_worker_poll",
        "timestamp_scope": "nonoverlapping_named_span",
        "correlation_method": "not_required" if available else "unavailable",
        "uncertainty_ns": 0 if available else None,
        "instrumentation_overhead_ns": 0 if available else None,
        "instrumentation_control": "synthetic_clock_control",

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
                        "gpu_terminal_counts": {name: 2 if name == "completed" else 0
                                                for name in MODULE.GPU_TERMINALS},
                        "delivery_counts": {name: 2 if name == "gpu" else 0
                                            for name in MODULE.DELIVERIES},
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


def refresh_all(records: list[dict]) -> None:
    for record in records[1:]:
        if record["record_kind"] == "trial_begin":
            digest = hashlib.sha256()
            terminals = {name: 0 for name in MODULE.GPU_TERMINALS}
            deliveries = {name: 0 for name in MODULE.DELIVERIES}
        elif record["record_kind"] == "block":
            digest.update(MODULE._canonical(record) + b"\n")
            terminals[record["gpu_terminal"]] += 1
            deliveries[record["delivery"]] += 1
        elif record["record_kind"] == "trial_end":
            record.update(blocks_sha256=digest.hexdigest(), gpu_terminal_counts=terminals,
                          delivery_counts=deliveries)


def confirmation_fixture() -> list[dict]:
    source = fixture()
    manifest = copy.deepcopy(source[0])
    manifest.update(campaign="confirmation", expected_trials=61, expected_matched_pairs=30,
                    bootstrap_resamples=10_000)
    records = [manifest]
    for trial_id in range(61):
        path = ("staged_async" if trial_id % 2 == 0 else "shared_async") if trial_id < 60 else "staged_sync"
        for original in source[1:]:
            if original["path"] != path:
                continue
            record = copy.deepcopy(original)
            record.update(trial_id=trial_id, pair_id=trial_id // 2 if trial_id < 60 else None)
            if record["record_kind"] == "block":
                record["engine_id"] = trial_id + 1
                if path == "shared_async":
                    for name in MODULE.CPU_TIMINGS:
                        record["timings"][name]["value_ns"] = 40
            records.append(record)
    refresh_all(records)
    return records


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
                for name in MODULE.CPU_TIMINGS:
                    record["timings"][name]["value_ns"] = 40
        refresh_digest(records, "shared_async")
        self.assertEqual(MODULE.validate_records(records), [])
        gate = MODULE.summarize(records)["row_gate"]
        self.assertEqual(gate["status"], "pass")
        self.assertEqual(gate["program_verdict"], "unassigned")

    def test_complete_confirmation_positive_control(self) -> None:
        records = confirmation_fixture()
        self.assertEqual(MODULE.validate_records(records), [])
        summary = MODULE.summarize(records)
        self.assertEqual(summary["row_gate"]["status"], "pass")
        self.assertEqual(summary["verdict"], "unassigned")
        self.assertEqual(summary["matched_improvement"]["total_cpu_per_block_percent"]["pairs"], 30)

    def test_device_loss_cannot_hide_in_block_dispositions(self) -> None:
        records = confirmation_fixture()
        self.assertEqual(MODULE.validate_records(records), [])
        for record in records:
            if record["record_kind"] == "block":
                record.update(gpu_terminal="device_lost", delivery="cpu_fallback")
        refresh_all(records)
        errors = MODULE.validate_records(records)
        self.assertTrue(any("requires trial_end device_loss" in error for error in errors), errors)
        for record in records:
            if record["record_kind"] == "trial_end":
                record["device_loss"] = True
        self.assertTrue(any("terminal health failure" in error for error in MODULE.validate_records(records)))

    def test_disposition_counters_must_match_blocks(self) -> None:
        records = fixture()
        self.assertEqual(MODULE.validate_records(records), [])
        end = next(record for record in records if record["record_kind"] == "trial_end")
        end["gpu_terminal_counts"]["provider_failure"] = 1
        end["delivery_counts"]["gpu"] = True
        errors = MODULE.validate_records(records)
        self.assertTrue(any("gpu_terminal_counts must exactly match" in error for error in errors), errors)
        self.assertTrue(any("delivery_counts must exactly match" in error for error in errors), errors)

    def test_confirmation_cannot_measure_only_priming_or_fallback(self) -> None:
        for delivery in ("priming", "cpu_fallback", "silence", "passthrough"):
            with self.subTest(delivery=delivery):
                records = confirmation_fixture()
                self.assertEqual(MODULE.validate_records(records), [])
                for record in records:
                    if record["record_kind"] == "block" and record["path"] == "shared_async":
                        record["delivery"] = delivery
                refresh_all(records)
                errors = MODULE.validate_records(records)
                self.assertTrue(any("requires eligible GPU delivery" in error for error in errors), errors)

    def test_timing_provenance_and_verdict_eligibility(self) -> None:
        changes = (
            ("relation", "inferred", "verdict timing unavailable or inferred"),
            ("end_clock_domain", "uncorrelated_gpu_clock", "matching endpoint clock_domains"),
            ("end_observer", "different_observer", "matching endpoint observers"),
            ("instrumentation_control", "", "instrumentation_control must be non-empty"),
            ("uncertainty_ns", None, "uncertainty_ns must be finite"),
            ("value_ns", 10 ** 400, "value_ns must be finite"),
        )
        for field, value, expected in changes:
            with self.subTest(field=field, value=str(value)[:40]):
                records = confirmation_fixture()
                self.assertEqual(MODULE.validate_records(records), [])
                block = next(record for record in records if record["record_kind"] == "block")
                block["timings"]["submit_to_completion"][field] = value
                refresh_all(records)
                errors = MODULE.validate_records(records)
                self.assertTrue(any(expected in error for error in errors), errors)

    def test_correlated_observation_requires_explicit_method(self) -> None:
        value = observation(100)
        value.update(relation="correlated", end_clock_domain="gpu_ticks", end_observer="gpu_query",
                     correlation_method="calibrated_affine_clock_map", uncertainty_ns=2)
        errors = []
        MODULE._check_timing("gpu_elapsed", value, "control", errors)
        self.assertEqual(errors, [])
        value["correlation_method"] = "uncorrelated"
        MODULE._check_timing("gpu_elapsed", value, "control", errors)
        self.assertTrue(any("requires a correlation method" in error for error in errors))

    def test_inferred_screening_data_remains_recorded_but_cannot_win(self) -> None:
        records = fixture()
        for record in records:
            if record["record_kind"] == "block":
                record["timings"]["submit_to_completion"]["relation"] = "inferred"
        refresh_all(records)
        self.assertEqual(MODULE.validate_records(records), [])
        summary = MODULE.summarize(records)
        self.assertEqual(summary["row_gate"]["status"], "fail")
        self.assertEqual(summary["matched_improvement"]["submit_to_completion_p99_percent"]["pairs"], 0)

    def test_service_cpu_cannot_hide_an_apparent_cpu_win(self) -> None:
        records = confirmation_fixture()
        self.assertEqual(MODULE.summarize(records)["row_gate"]["status"], "pass")
        for record in records:
            if record["record_kind"] == "block" and record["path"] == "shared_async":
                record["timings"]["event_processing_cpu"]["value_ns"] = 500
                record["timings"]["retirement_cpu"]["value_ns"] = 500
        refresh_all(records)
        self.assertEqual(MODULE.validate_records(records), [])
        self.assertEqual(MODULE.summarize(records)["row_gate"]["status"], "fail")
        records[2]["timings"].pop("worker_other_cpu")
        refresh_all(records)
        self.assertTrue(any("worker_other_cpu" in error for error in MODULE.validate_records(records)))

    def test_ui_outlier_cannot_hide_matched_trial_regressions(self) -> None:
        records = confirmation_fixture()
        self.assertEqual(MODULE.validate_records(records), [])
        self.assertTrue(MODULE.summarize(records)["row_gate"]["checks"]["ui_frame_p99"]["passed"])
        for record in records:
            if record["record_kind"] == "trial_end":
                if record["path"] == "staged_async" and record["pair_id"] == 0:
                    record["ui_frame_p99"]["value_ns"] = 2000
                if record["path"] == "shared_async":
                    record["ui_frame_p99"]["value_ns"] = 1800
        self.assertEqual(MODULE.validate_records(records), [])
        summary = MODULE.summarize(records)
        self.assertFalse(summary["row_gate"]["checks"]["ui_frame_p99"]["passed"])
        self.assertEqual(summary["matched_ui_frame_p99"], {"pairs": 30, "max_regression_percent": 80})

    def test_provenance_state_is_bounded_and_drift_is_rejected(self) -> None:
        tracker = MODULE.ProvenanceTracker()
        errors = MODULE.Diagnostics()
        value = observation(100)
        tracker.check("shared_async", "submit_to_completion", value, "control", errors)
        self.assertEqual(errors, [])
        for index in range(10_000):
            value["api_source"] = f"changing_source_{index}"
            tracker.check("shared_async", "submit_to_completion", value, "control", errors)
        self.assertEqual(len(tracker.first), 1)
        self.assertEqual(len(tracker.mismatches), 1)
        self.assertEqual(len(errors), 1)
        records = confirmation_fixture()
        records[2]["timings"]["submit_to_completion"]["api_source"] = "changed_source"
        refresh_all(records)
        self.assertTrue(any("provenance must be constant" in error for error in MODULE.validate_records(records)))
        for index in range(10_000):
            errors.append(f"bad block {index}")
        self.assertEqual(len(errors), 101)

    def test_exact_identity_types_and_stable_trial_engine(self) -> None:
        records = fixture()
        self.assertEqual(MODULE.validate_records(records), [])
        records[2]["trial_id"] = False
        records[3]["engine_id"] += 100
        refresh_all(records)
        errors = MODULE.validate_records(records)
        self.assertTrue(any("block identity does not match" in error for error in errors), errors)
        self.assertTrue(any("engine_id and generation must be constant" in error for error in errors), errors)

    def test_trial_and_pair_ids_are_uint64_and_spool_without_narrowing(self) -> None:
        records = fixture()
        original_trial_id = records[1]["trial_id"]
        for record in records:
            if record.get("trial_id") == original_trial_id:
                record["trial_id"] = MODULE.UINT64_MAX
        refresh_all(records)
        self.assertEqual(MODULE.validate_records(records), [])
        self.assertEqual(MODULE.summarize(records)["trial_count"], 3)

        records[1]["trial_id"] = MODULE.UINT64_MAX + 1
        errors = MODULE.validate_records(records)
        self.assertTrue(any("trial_id must be a unique uint64" in error for error in errors), errors)

        records = fixture()
        records[1]["pair_id"] = MODULE.UINT64_MAX + 1
        errors = MODULE.validate_records(records)
        self.assertTrue(any("pair_id must be a uint64" in error for error in errors), errors)

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

    def test_sync_reference_cannot_reset_async_trial_alternation(self) -> None:
        source = fixture()
        manifest = copy.deepcopy(source[0])
        manifest.update(expected_trials=5, expected_matched_pairs=2)
        groups: dict[str, list[dict]] = {}
        current: list[dict] = []
        for record in source[1:]:
            if record["record_kind"] == "trial_begin":
                current = []
            current.append(copy.deepcopy(record))
            if record["record_kind"] == "trial_end":
                groups[current[0]["path"]] = current

        def clone(path: str, trial_id: int, pair_id: int | None,
                  engine_delta: int = 0) -> list[dict]:
            group = copy.deepcopy(groups[path])
            for record in group:
                record.update(trial_id=trial_id, pair_id=pair_id)
                if record["record_kind"] == "block":
                    record["engine_id"] += engine_delta
            return group

        records = [manifest]
        for group in (
            clone("staged_async", 0, 0),
            clone("shared_async", 3, 1, 100),
            clone("staged_sync", 2, None),
            clone("shared_async", 1, 0),
            clone("staged_async", 4, 1, 100),
        ):
            records.extend(group)
        refresh_all(records)
        errors = MODULE.validate_records(records)
        self.assertTrue(any("must alternate async paths" in error for error in errors), errors)

        valid = [copy.deepcopy(manifest)]
        for group in (
            clone("staged_async", 0, 0),
            clone("shared_async", 1, 0),
            clone("staged_sync", 2, None),
            clone("staged_async", 3, 1, 100),
            clone("shared_async", 4, 1, 100),
        ):
            valid.extend(group)
        refresh_all(valid)
        self.assertEqual(MODULE.validate_records(valid), [])

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

    def test_raw_json_rejects_duplicate_keys_and_nonfinite_constants(self) -> None:
        cases = (
            ('{"record_kind":"manifest","record_kind":"block"}\n', "duplicate object key"),
            ('{"record_kind":"manifest","nested":{"value":1,"value":2}}\n',
             "duplicate object key"),
            ('{"record_kind":"manifest","value":NaN}\n', "non-finite JSON constant"),
            ('{"record_kind":"manifest","value":Infinity}\n', "non-finite JSON constant"),
            ('{"record_kind":"manifest","value":1e999}\n', "non-finite JSON number"),
            ('{"record_kind":"manifest","value":"\\ud800"}\n',
             "unpaired Unicode surrogate"),
        )
        with tempfile.TemporaryDirectory() as root:
            raw = Path(root) / "raw.jsonl"
            for payload, expected in cases:
                with self.subTest(payload=payload):
                    raw.write_text(payload, encoding="utf-8")
                    parsed, errors = MODULE.read_jsonl(raw)
                    try:
                        self.assertTrue(any(expected in error for error in errors), errors)
                    finally:
                        parsed.close()

            raw.write_bytes(b'{"record_kind":"manifest","value":"\xff"}\n')
            parsed, errors = MODULE.read_jsonl(raw)
            try:
                self.assertTrue(any("invalid UTF-8" in error for error in errors), errors)
            finally:
                parsed.close()

            raw.write_text('{"value":0}\n', encoding="utf-8")
            with mock.patch.object(MODULE, "_strict_json_loads",
                                   side_effect=RecursionError("nested too deeply")):
                parsed, errors = MODULE.read_jsonl(raw)
            try:
                self.assertTrue(any("invalid JSON" in error for error in errors), errors)
            finally:
                parsed.close()

    def test_build_flags_require_only_nonempty_strings(self) -> None:
        for invalid in (float("nan"), 3, True, ""):
            with self.subTest(invalid=invalid):
                records = fixture()
                records[0]["build_flags"].append(invalid)
                errors = MODULE.validate_records(records)
                self.assertTrue(any("build_flags must be non-empty strings" in error
                                    for error in errors), errors)

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

    def test_disk_percentiles_and_bootstrap_have_known_answers(self) -> None:
        metrics = MODULE._MetricStore()
        try:
            for value in (0, 10, 30, 100):
                metrics.add("shared_async", 0, "latency", value)
            metrics.add("staged_async", 0, "latency", 999)
            metrics.add("shared_async", 1, "latency", 999)
            metrics.finish()
            for percentile, expected in ((50, 20), (95, 89.5), (99, 97.9), (99.9, 99.79)):
                self.assertAlmostEqual(metrics.percentile("shared_async", "latency", percentile, 0), expected)
            self.assertEqual(metrics.mean("shared_async", "latency", 0), 35)
            self.assertEqual(metrics.count("shared_async", "latency", 0), 4)
        finally:
            metrics.close()
        confidence = MODULE._bootstrap_mean_ci([0, 10], 91, 10_000)
        self.assertEqual(confidence, {"pairs": 2, "mean": 5, "ci95_low": 0, "ci95_high": 10})
        self.assertEqual(confidence, MODULE._bootstrap_mean_ci([0, 10], 91, 10_000))
        constant = MODULE._bootstrap_mean_ci([7, 7, 7], 12, 100)
        self.assertEqual((constant["mean"], constant["ci95_low"], constant["ci95_high"]), (7, 7, 7))

    def test_priming_alone_is_not_an_overload_exercise(self) -> None:
        records = fixture()
        records[0].update(campaign="overload")
        records[0]["row"]["load"] = "overload"
        for record in records:
            if record["record_kind"] == "block":
                record["delivery"] = "priming"
        refresh_all(records)
        self.assertTrue(any("did not exercise" in error for error in MODULE.validate_records(records)))
        records[2]["delivery"] = "cpu_fallback"
        refresh_all(records)
        self.assertEqual(MODULE.validate_records(records), [])

    def test_cli_rejects_changed_raw_and_output_input_aliases(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"benchmark")
            records[0]["binary_sha256"] = MODULE._sha256_file(binary)
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records))
            raw_digest = MODULE._sha256_file(raw)
            summary = root_path / "summary.json"
            argv = [str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                    "--summary", str(summary)]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(MODULE.main(), 0)
            summary.unlink()
            original_hash = MODULE._sha256_file
            calls = 0

            def changed_hash(path: Path) -> str:
                nonlocal calls
                if path == raw:
                    calls += 1
                    return raw_digest if calls == 1 else "0" * 64
                return original_hash(path)

            stderr = io.StringIO()
            with mock.patch.object(sys, "argv", argv), mock.patch.object(MODULE, "_sha256_file", changed_hash), contextlib.redirect_stderr(stderr):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("evidence file changed", stderr.getvalue())
            self.assertFalse(summary.exists())
            argv[-1] = str(raw)
            with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(stderr):
                self.assertEqual(MODULE.main(), 1)
            self.assertEqual(MODULE._sha256_file(raw), raw_digest)

    def test_cli_rejects_hard_link_aliases_and_preserves_inputs(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"benchmark")
            records[0]["binary_sha256"] = MODULE._sha256_file(binary)
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records),
                           encoding="utf-8")
            raw_bytes = raw.read_bytes()
            binary_bytes = binary.read_bytes()
            summary = root_path / "summary.json"
            os.link(raw, summary)
            csv_path = root_path / "blocks.csv"
            argv = [str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                    "--summary", str(summary), "--csv", str(csv_path)]
            stderr = io.StringIO()
            with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(stderr):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("summary output aliases evidence input", stderr.getvalue())
            self.assertEqual(raw.read_bytes(), raw_bytes)
            self.assertEqual(binary.read_bytes(), binary_bytes)

            summary.unlink()
            summary.write_text("existing summary", encoding="utf-8")
            os.link(binary, csv_path)
            stderr = io.StringIO()
            with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(stderr):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("CSV output aliases benchmark input", stderr.getvalue())
            self.assertEqual(raw.read_bytes(), raw_bytes)
            self.assertEqual(binary.read_bytes(), binary_bytes)

            csv_path.unlink()
            csv_path.write_text("existing csv", encoding="utf-8")
            summary.unlink()
            os.link(csv_path, summary)
            stderr = io.StringIO()
            with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(stderr):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("CSV output aliases summary output", stderr.getvalue())
            self.assertEqual(raw.read_bytes(), raw_bytes)
            self.assertEqual(binary.read_bytes(), binary_bytes)

    def test_cli_rejects_symbolic_link_alias_and_preserves_inputs(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"benchmark")
            records[0]["binary_sha256"] = MODULE._sha256_file(binary)
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records),
                           encoding="utf-8")
            raw_bytes = raw.read_bytes()
            binary_bytes = binary.read_bytes()
            summary = root_path / "summary.json"
            summary.symlink_to(raw)
            argv = [str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                    "--summary", str(summary)]
            stderr = io.StringIO()
            with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(stderr):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("summary output aliases evidence input", stderr.getvalue())
            self.assertTrue(summary.is_symlink())
            self.assertEqual(raw.read_bytes(), raw_bytes)
            self.assertEqual(binary.read_bytes(), binary_bytes)

    def test_path_identity_guard_rejects_every_existing_alias_pair(self) -> None:
        labels = ("evidence input", "benchmark input", "summary output", "CSV output")
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            for first in range(len(labels)):
                for second in range(first + 1, len(labels)):
                    with self.subTest(first=labels[first], second=labels[second]):
                        paths = [root_path / f"path-{index}" for index in range(len(labels))]
                        for index, path in enumerate(paths):
                            path.write_bytes(f"content-{index}".encode())
                        paths[second].unlink()
                        os.link(paths[first], paths[second])
                        error = MODULE._distinct_paths(list(zip(labels, paths)))
                        self.assertEqual(error, f"{labels[second]} aliases {labels[first]}")
                        for path in paths:
                            path.unlink()

            loop = root_path / "loop"
            loop.symlink_to(loop.name)
            error = MODULE._distinct_paths([("summary output", loop)])
            self.assertIsNotNone(error)
            self.assertIn("summary output", error)

    def test_cli_atomically_replaces_outputs_and_rolls_back_second_failure(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"benchmark")
            records[0]["binary_sha256"] = MODULE._sha256_file(binary)
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records),
                           encoding="utf-8")
            raw_digest = MODULE._sha256_file(raw)
            summary = root_path / "summary.json"
            csv_path = root_path / "blocks.csv"
            summary.write_text("old summary", encoding="utf-8")
            csv_path.write_text("old csv", encoding="utf-8")
            old_inodes = (summary.stat().st_ino, csv_path.stat().st_ino)
            argv = [str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                    "--summary", str(summary), "--csv", str(csv_path)]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(MODULE.main(), 0)
            for actual, old in zip((summary.stat().st_ino, csv_path.stat().st_ino), old_inodes):
                self.assertNotEqual(actual, old)
            self.assertEqual(json.loads(summary.read_text(encoding="utf-8"))["evidence_sha256"],
                             raw_digest)
            self.assertIn("evidence_sha256", csv_path.read_text(encoding="utf-8"))
            self.assertEqual(MODULE._sha256_file(raw), raw_digest)

            summary.write_text("preserve summary", encoding="utf-8")
            csv_path.write_text("preserve csv", encoding="utf-8")
            preserved = {path: (path.read_bytes(), path.stat().st_ino)
                         for path in (summary, csv_path)}
            real_replace = os.replace
            publication_count = 0

            def fail_second_publication(source: Path, destination: Path) -> None:
                nonlocal publication_count
                if Path(source).suffix == ".tmp" and Path(destination) in preserved:
                    publication_count += 1
                    if publication_count == 2:
                        raise OSError("planted second replace failure")
                real_replace(source, destination)

            stderr = io.StringIO()
            with (mock.patch.object(sys, "argv", argv),
                  mock.patch.object(MODULE.os, "replace", side_effect=fail_second_publication),
                  contextlib.redirect_stderr(stderr)):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("planted second replace failure", stderr.getvalue())
            self.assertEqual(publication_count, 2)
            for path, (content, inode) in preserved.items():
                self.assertEqual(path.read_bytes(), content)
                self.assertEqual(path.stat().st_ino, inode)
            self.assertEqual(MODULE._sha256_file(raw), raw_digest)
            self.assertEqual(list(root_path.glob(".*.tmp")), [])
            self.assertEqual(list(root_path.glob(".*.rollback")), [])

            summary.unlink()
            csv_path.unlink()
            publication_count = 0
            stderr = io.StringIO()
            with (mock.patch.object(sys, "argv", argv),
                  mock.patch.object(MODULE.os, "replace", side_effect=fail_second_publication),
                  contextlib.redirect_stderr(stderr)):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("planted second replace failure", stderr.getvalue())
            self.assertFalse(summary.exists())
            self.assertFalse(csv_path.exists())
            self.assertEqual(list(root_path.glob(".*.tmp")), [])
            self.assertEqual(list(root_path.glob(".*.rollback")), [])

    def test_cli_rechecks_path_identity_immediately_before_publication(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"benchmark")
            records[0]["binary_sha256"] = MODULE._sha256_file(binary)
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records),
                           encoding="utf-8")
            raw_bytes = raw.read_bytes()
            summary = root_path / "summary.json"
            summary.write_text("preserve summary", encoding="utf-8")
            argv = [str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                    "--summary", str(summary)]
            original_hash = MODULE._sha256_file
            binary_hashes = 0

            def relink_before_publication(path: Path) -> str:
                nonlocal binary_hashes
                digest = original_hash(path)
                if path == binary:
                    binary_hashes += 1
                    if binary_hashes == 2:
                        summary.unlink()
                        os.link(raw, summary)
                return digest

            stderr = io.StringIO()
            with (mock.patch.object(sys, "argv", argv),
                  mock.patch.object(MODULE, "_sha256_file", relink_before_publication),
                  contextlib.redirect_stderr(stderr)):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("path identity changed during analysis", stderr.getvalue())
            self.assertIn("summary output aliases evidence input", stderr.getvalue())
            self.assertEqual(raw.read_bytes(), raw_bytes)
            self.assertEqual(list(root_path.glob(".*.tmp")), [])

    def test_summary_serialization_failure_preserves_all_files(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"benchmark")
            records[0]["binary_sha256"] = MODULE._sha256_file(binary)
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records),
                           encoding="utf-8")
            summary = root_path / "summary.json"
            csv_path = root_path / "blocks.csv"
            summary.write_text("preserve summary", encoding="utf-8")
            csv_path.write_text("preserve csv", encoding="utf-8")
            before = {path: path.read_bytes() for path in (raw, binary, summary, csv_path)}
            argv = [str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                    "--summary", str(summary), "--csv", str(csv_path)]
            stderr = io.StringIO()
            with (mock.patch.object(sys, "argv", argv),
                  mock.patch.object(MODULE, "summarize", return_value={"bad": float("nan")}),
                  contextlib.redirect_stderr(stderr)):
                self.assertEqual(MODULE.main(), 1)
            self.assertIn("cannot serialize summary", stderr.getvalue())
            self.assertEqual({path: path.read_bytes() for path in before}, before)
            self.assertEqual(list(root_path.glob(".*.tmp")), [])

    def test_cli_rejects_inputs_changed_during_output_staging(self) -> None:
        records = fixture()
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            binary = root_path / "benchmark"
            binary.write_bytes(b"benchmark")
            records[0]["binary_sha256"] = MODULE._sha256_file(binary)
            raw = root_path / "raw.jsonl"
            raw.write_text("".join(json.dumps(record) + "\n" for record in records),
                           encoding="utf-8")
            summary = root_path / "summary.json"
            csv_path = root_path / "blocks.csv"
            argv = [str(MODULE_PATH), str(raw), "--benchmark-binary", str(binary),
                    "--summary", str(summary), "--csv", str(csv_path)]
            original_hash = MODULE._sha256_file
            for changed, expected, changed_call in (
                (raw, "evidence file changed during analysis", 3),
                (binary, "benchmark binary changed during analysis", 2),
            ):
                with self.subTest(changed=changed.name):
                    summary.write_text("preserve summary", encoding="utf-8")
                    csv_path.write_text("preserve csv", encoding="utf-8")
                    before = {path: path.read_bytes()
                              for path in (raw, binary, summary, csv_path)}
                    calls = 0

                    def changed_hash(path: Path) -> str:
                        nonlocal calls
                        if path == changed:
                            calls += 1
                            if calls == changed_call:
                                return "0" * 64
                        return original_hash(path)

                    stderr = io.StringIO()
                    with (mock.patch.object(sys, "argv", argv),
                          mock.patch.object(MODULE, "_sha256_file", changed_hash),
                          contextlib.redirect_stderr(stderr)):
                        self.assertEqual(MODULE.main(), 1)
                    self.assertIn(expected, stderr.getvalue())
                    self.assertEqual({path: path.read_bytes() for path in before}, before)
                    self.assertEqual(list(root_path.glob(".*.tmp")), [])

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
