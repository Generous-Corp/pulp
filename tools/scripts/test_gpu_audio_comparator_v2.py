import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import gpu_audio_comparator_v2 as comparator

FFT_SHA = "f" * 64
MUL_SHA = "e" * 64
PORT_SHA = "d" * 64
BLOCKS = 400
WARMUP = 32
MIN_BLOCKS = 100
# (name, side, production_eligible, role)
LANES = (
    ("metal_shared_event", "metal", True, "gating"),
    ("metal_completed_handler", "metal", True, "gating"),
    (comparator.ROBUSTNESS_PROBE_LANE, "metal", False, comparator.ROBUSTNESS_PROBE_ROLE),
    ("dawn_imported_timed_wait_any", "dawn", True, "gating"),
    ("dawn_imported_process_events_poll50us", "dawn", True, "gating"),
    ("dawn_imported_wait_any_poll50us", "dawn", True, "gating"),
    ("dawn_staged_timestamps_timed_wait_any", "dawn", False, "gating"),
)
REFERENCE_LANE = "dawn_imported_process_events_poll50us"


def passes(frames):
    n = frames * 2
    log2 = n.bit_length() - 1
    fft_groups = (2 * (n // 2) + 255) // 256
    out = [{"groups": fft_groups, "kernel": "stockham_fft", "ns": 1 << s, "sign": -1,
            "workgroup": 256} for s in range(log2)]
    out.append({"groups": (2 * n + 255) // 256, "kernel": "complex_multiply_ir", "ns": 0,
                "sign": 0, "workgroup": 256})
    out += [{"groups": fft_groups, "kernel": "stockham_fft", "ns": 1 << s, "sign": 1,
             "workgroup": 256} for s in range(log2)]
    return out


def kernel(frames, program=comparator.PROGRAM_ID, fft=FFT_SHA, mul=MUL_SHA, port=PORT_SHA,
           robustness="tint_equivalent"):
    canon = {"channels": 2, "fft_size": frames * 2, "fft_wgsl_sha256": fft, "frames": frames,
             "ir_length": frames, "metal_port_sha256": port, "mul_wgsl_sha256": mul,
             "passes": passes(frames), "program": program, "robustness": robustness}
    return {"canonical": canon,
            "kernel_digest": hashlib.sha256(comparator.canonical(canon)).hexdigest()}


def cell(lane, side, role, frames, load, *, s2v=200_000, tail=None, cpu=20_000,
         proc_cpu=40_000, wake=150_000, busy=60_000, misses=0, blocks=BLOCKS):
    period = frames * 1e9 / 48_000
    samples = {f: [] for f in comparator.SAMPLE_FIELDS}
    timed = side == "metal" or "staged" in lane
    for i in range(blocks):
        release = int(i * period)
        submit = release + 1_000
        latency = tail if tail is not None and i >= blocks - max(1, blocks // 100) else s2v
        if i < misses:
            latency = int(3 * period)
        visible = submit + latency
        samples["release_ns"].append(release)
        samples["submit_ns"].append(submit)
        samples["submit_end_ns"].append(submit + 500)
        samples["visible_ns"].append(visible)
        samples["gpu_busy_ns"].append(busy if timed else -1)
        samples["gpu_end_ns"].append(visible - wake if side == "metal" else -1)
        samples["callback_ns"].append(visible - 5_000 if side == "metal" else -1)
        samples["callback_cpu_ns"].append(3_000 if side == "metal" else -1)
        samples["worker_cpu_ns"].append(cpu)
        samples["oracle"].append(1)
        samples["stalled"].append(0)
    driven = blocks + WARMUP
    return {"lane": lane, "side": side, "role": role, "frames": frames, "load": load,
            "status": "completed", "reason": "", "blocks": blocks, "warmup_blocks": WARMUP,
            "kernel": kernel(frames, robustness="none" if role != "gating"
                             else "tint_equivalent"),
            "oracle_failures": 0, "stalls": 0, "gpu_timestamps": timed,
            "process_cpu_total_ns": proc_cpu * driven + 7_000 + 11_000,
            "render_cpu_in_window_ns": 7_000, "oracle_cpu_ns": 11_000,
            "driven_blocks": driven,
            "render_load": {"active": load == "render", "status": "completed",
                            "dropped_frames": 0, "thread_cpu_ns": 0,
                            "frame_ns": [4_000_000] * 10 if load == "render" else []},
            "samples": samples}


def receipt(overrides=None, *, quiet=True, blocks=BLOCKS, host="host-a",
            assembled_at="2026-10-22T08:00:00+00:00", binary="b" * 64):
    """A complete, matched, bound receipt; `overrides` maps (lane, frames, load) to
    keyword arguments for that cell."""
    overrides = overrides or {}
    cells = []
    for lane, side, _, role in LANES:
        for frames in (64, 128, 256):
            for load in ("idle", "render"):
                cells.append(cell(lane, side, role, frames, load, blocks=blocks,
                                  **overrides.get((lane, frames, load), {})))
    sample = {"status": "passed", "quiet_host": True, "host_id": host, "reasons": [],
              "source_revision": "a" * 40}
    preflight = {"before": dict(sample), "after": dict(sample)}
    if not quiet:
        preflight["after"].update(status="blocked", quiet_host=False,
                                  reasons=["load_above_quiet_threshold"])
    body = {
        "schema": comparator.SCHEMA, "mode": "campaign", "planted_unmatched_kernel": False,
        "dawn": {"dawn_revision": "d" * 40},
        "kernel": {
            "program": comparator.PROGRAM_ID,
            "provider_wgsl": {"ok": True, "fft_sha256": FFT_SHA, "mul_sha256": MUL_SHA,
                              "build_fft_sha256": FFT_SHA, "build_mul_sha256": MUL_SHA},
            "parity": [{"frames": c["frames"], "lane": c["lane"], "load": c["load"],
                        "reference_lane": REFERENCE_LANE, "max_abs_diff": 1e-7,
                        "tolerance": 1e-4} for c in cells],
        },
        "config": {"sample_rate": 48_000, "channels": 2, "flight_depth": 2,
                   "lead_blocks": 2, "blocks_per_cell": blocks, "warmup_blocks": WARMUP,
                   "frames": [64, 128, 256], "paced": True,
                   "lanes": [{"name": n, "side": s, "production_eligible": e, "role": r}
                             for n, s, e, r in LANES]},
        "cells": cells,
        "host_preflight": preflight,
        "host_preflight_sha256": hashlib.sha256(comparator.canonical(preflight)).hexdigest(),
        "host_quiet": quiet,
        "source_revision": "a" * 40,
        "source_dirty": False,
        "source_git_error": False,
        "binary_sha256": binary,
        "assembled_at": assembled_at,
    }
    return seal(body)


def pair(overrides=None, **kwargs):
    return (receipt(overrides, assembled_at="2026-10-22T08:00:00+00:00", **kwargs),
            receipt(overrides, assembled_at="2026-10-22T09:00:00+00:00", **kwargs))


def seal(body):
    body = dict(body)
    body["receipt_sha256"] = comparator.receipt_digest(body)
    return body


def find(r, lane, frames, load="idle"):
    return next(c for c in r["cells"]
                if c["lane"] == lane and c["frames"] == frames and c["load"] == load)


def evaluate(*receipts):
    return comparator.evaluate(list(receipts), min_blocks=MIN_BLOCKS)


def gating_lanes(side):
    return [n for n, s, e, r in LANES if s == side and r == "gating"]


def write_pair(directory, receipts):
    paths = []
    for index, r in enumerate(receipts):
        path = Path(directory) / f"r{index}.json"
        path.write_text(json.dumps(r), encoding="utf-8")
        paths.append(str(path))
    return paths


class ComparatorV2Tests(unittest.TestCase):
    def test_percentile_nearest_rank(self):
        values = list(range(1, 1001))
        self.assertEqual(comparator.percentile(values, 0.5), 500)
        self.assertEqual(comparator.percentile(values, 0.99), 990)
        self.assertEqual(comparator.percentile(values, 0.999), 999)
        self.assertIsNone(comparator.percentile([], 0.5))

    # -- positive controls -------------------------------------------------

    def test_positive_control_two_quiet_matched_campaigns_do_not_fire(self):
        result = evaluate(*pair())
        self.assertEqual(result["verdict"], "GATE_DOES_NOT_FIRE", result["reasons"])
        self.assertEqual(result["thresholds"]["status"], "PROPOSED")
        self.assertFalse(result["thresholds"]["ratified"])
        criteria = result["campaigns"][0]["thresholds"]["criteria"]
        self.assertEqual(set(criteria.values()), {"does_not_fire"})

    def test_single_campaign_is_never_conclusive(self):
        result = evaluate(receipt())
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("counted_campaigns_1_of_2", result["reasons"])

    def test_robustness_cost_row_is_reported_and_never_gates(self):
        overrides = {(comparator.ROBUSTNESS_PROBE_LANE, 64, load): {"tail": 2_000_000,
                                                                   "busy": 40_000}
                     for load in ("idle", "render")}
        result = evaluate(*pair(overrides))
        self.assertEqual(result["verdict"], "GATE_DOES_NOT_FIRE", result["reasons"])
        row = next(r for r in result["campaigns"][0]["robustness_cost"]
                   if r["frames"] == 64 and r["load"] == "idle")
        self.assertEqual(row["guard_cost_gpu_busy_p50_ns"], 20_000)

    # -- campaign identity (duplicates, order, binary, source) --------------

    def test_same_receipt_twice_is_refused(self):
        r = receipt()
        result = evaluate(r, copy.deepcopy(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("duplicate_campaign", result["reasons"])

    def test_same_timestamp_is_refused_as_duplicate(self):
        a = receipt()
        b = receipt()
        b["cells"][0]["stalls"] = 1
        result = evaluate(a, seal(b))
        self.assertIn("duplicate_campaign", result["reasons"])

    def test_campaigns_out_of_time_order_are_not_counted(self):
        first, second = pair()
        result = evaluate(second, first)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("campaigns_not_time_ordered", result["reasons"])

    def test_campaigns_from_different_binaries_are_not_counted(self):
        first = receipt(assembled_at="2026-10-22T08:00:00+00:00", binary="1" * 64)
        second = receipt(assembled_at="2026-10-22T09:00:00+00:00", binary="2" * 64)
        result = evaluate(first, second)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("campaigns_not_same_binary_and_source", result["reasons"])

    def test_different_hosts_do_not_make_two_campaigns(self):
        first = receipt(assembled_at="2026-10-22T08:00:00+00:00", host="a")
        second = receipt(assembled_at="2026-10-22T09:00:00+00:00", host="b")
        result = evaluate(first, second)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("campaigns_not_same_host_and_dawn_revision", result["reasons"])

    def test_dirty_source_tree_is_not_counted(self):
        first, second = pair()
        first["source_dirty"] = True
        result = evaluate(seal(first), second)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("source_tree_dirty", result["reasons"])

    def test_bind_marks_untracked_source_tree_pollution_dirty(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "comparator"
            binary.write_bytes(b"test binary")

            def git(*args):
                if args and args[0] == "status":
                    return "?? untracked-source-pollution.txt"
                return "a" * 40

            with patch.object(comparator, "_git", side_effect=git):
                bound = comparator.bind({}, binary, None)

        self.assertTrue(bound["source_dirty"])

    def test_bind_marks_git_failure_dirty_and_refuses(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "comparator"
            binary.write_bytes(b"test binary")

            with patch.object(comparator, "_git", return_value=None):
                bound = comparator.bind({}, binary, None)

        self.assertIsNone(bound["source_revision"])
        self.assertTrue(bound["source_dirty"])
        self.assertTrue(bound["source_git_error"])

    def test_missing_source_revision_is_refused(self):
        first, second = pair()
        first["source_revision"] = None
        result = evaluate(seal(first), second)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("source_revision_invalid", result["reasons"])

    def test_missing_source_dirty_is_refused(self):
        first, second = pair()
        del first["source_dirty"]
        result = evaluate(seal(first), second)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("source_dirty_invalid", result["reasons"])

    def test_missing_source_git_error_is_refused(self):
        first, second = pair()
        del first["source_git_error"]
        result = evaluate(seal(first), second)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("source_git_error_invalid", result["reasons"])

    def test_non_boolean_source_provenance_is_refused(self):
        for field in ("source_dirty", "source_git_error"):
            with self.subTest(field=field):
                first, second = pair()
                first[field] = "false"
                result = evaluate(seal(first), second)
                self.assertEqual(result["verdict"], "REFUSED")
                self.assertIn(f"{field}_invalid", result["reasons"])

    def test_empty_source_revision_is_refused(self):
        first, second = pair()
        first["source_revision"] = ""
        result = evaluate(seal(first), second)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("source_revision_invalid", result["reasons"])

    # -- kernel matching ----------------------------------------------------

    def test_planted_unmatched_kernel_is_refused_by_digest(self):
        first, second = pair()
        find(first, "metal_shared_event", 64)["kernel"] = kernel(
            64, program="pulp.native_metal.complex_multiply.v0")
        result = evaluate(seal(first), second)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("kernel_digest_mismatch:64", result["reasons"])
        self.assertIn("unmatched_program:metal_shared_event:"
                      "pulp.native_metal.complex_multiply.v0", result["reasons"])

    def test_stale_msl_port_is_refused_by_digest(self):
        r = receipt()
        for frames in (64, 128, 256):
            for load in ("idle", "render"):
                find(r, "metal_shared_event", frames, load)["kernel"] = kernel(
                    frames, fft="0" * 64)
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("kernel_digest_mismatch:64", result["reasons"])

    def test_edited_msl_without_repin_is_refused_by_digest(self):
        r = receipt()
        find(r, "metal_completed_handler", 128)["kernel"] = kernel(128, port="9" * 64)
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("kernel_digest_mismatch:128", result["reasons"])

    def test_unguarded_metal_cannot_gate(self):
        r = receipt()
        find(r, "metal_shared_event", 64)["kernel"] = kernel(64, robustness="none")
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("gating_kernel_not_tint_equivalent:metal_shared_event:64",
                      result["reasons"])

    def test_robustness_probe_must_be_the_same_program(self):
        r = receipt()
        find(r, comparator.ROBUSTNESS_PROBE_LANE, 256)["kernel"] = kernel(
            256, robustness="none", mul="1" * 64)
        result = evaluate(seal(r))
        self.assertIn("kernel_digest_mismatch:256:robustness_probe", result["reasons"])

    def test_dawn_cell_must_run_the_provider_wgsl(self):
        r = receipt()
        r["kernel"]["provider_wgsl"]["fft_sha256"] = "1" * 64
        r["kernel"]["provider_wgsl"]["build_fft_sha256"] = "1" * 64
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("dawn_kernel_not_provider_wgsl:dawn_imported_timed_wait_any",
                      result["reasons"])

    def test_source_wgsl_must_match_the_build(self):
        r = receipt()
        r["kernel"]["provider_wgsl"]["build_mul_sha256"] = "2" * 64
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("provider_wgsl_differs_from_build", result["reasons"])

    def test_forged_digest_is_refused(self):
        r = receipt()
        find(r, "metal_shared_event", 128)["kernel"]["canonical"]["passes"].pop()
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("kernel_digest_forged:metal_shared_event:128", result["reasons"])

    def test_parity_failure_alone_is_refused(self):
        r = receipt()
        r["kernel"]["parity"][0]["max_abs_diff"] = 0.5
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertTrue(any(x.startswith("kernel_parity_failed") for x in result["reasons"]))
        self.assertFalse(any(x.startswith("kernel_digest") for x in result["reasons"]))

    def test_parity_ignores_the_receipt_tolerance(self):
        r = receipt()
        r["kernel"]["parity"][0]["max_abs_diff"] = 0.5
        r["kernel"]["parity"][0]["tolerance"] = 1.0
        result = evaluate(seal(r))
        self.assertTrue(any(x.startswith("kernel_parity_failed") for x in result["reasons"]))

    def test_parity_reference_must_be_a_dawn_production_lane(self):
        r = receipt()
        for p in r["kernel"]["parity"]:
            p["reference_lane"] = "metal_shared_event"
        result = evaluate(seal(r))
        self.assertTrue(any(x.startswith("kernel_parity_reference_not_dawn_production")
                            for x in result["reasons"]))

    def test_missing_parity_is_refused(self):
        r = receipt()
        r["kernel"]["parity"].pop()
        self.assertEqual(evaluate(seal(r))["verdict"], "REFUSED")

    # -- receipt and sample integrity --------------------------------------

    def test_tampered_samples_are_refused(self):
        r = receipt()
        find(r, "dawn_imported_timed_wait_any", 64)["samples"]["visible_ns"][3] += 1
        result = evaluate(r)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("receipt_digest_mismatch", result["reasons"])

    def test_out_of_order_samples_are_refused(self):
        r = receipt()
        c = find(r, "dawn_imported_timed_wait_any", 64)
        c["samples"]["visible_ns"][5] = c["samples"]["submit_ns"][5] - 10
        result = evaluate(seal(r))
        self.assertIn("sample_order:dawn_imported_timed_wait_any:64:idle", result["reasons"])

    def test_negative_submit_is_refused(self):
        r = receipt()
        find(r, "metal_shared_event", 64)["samples"]["submit_ns"][0] = -5
        self.assertIn("sample_order:metal_shared_event:64:idle", evaluate(seal(r))["reasons"])

    def test_gpu_end_after_visible_is_refused(self):
        r = receipt()
        c = find(r, "metal_shared_event", 128)
        c["samples"]["gpu_end_ns"][7] = c["samples"]["visible_ns"][7] + 500_000
        self.assertIn("sample_gpu_end_order:metal_shared_event:128:idle",
                      evaluate(seal(r))["reasons"])

    def test_paravirtual_metal_gpu_clock_drift_is_inconclusive_not_refused(self):
        # The VM GPU's clock drifts past the margin under load; the receipt is
        # not counted, but it is not an impossible timeline either.
        def drifted(device_name):
            r = receipt()
            r["device"] = {"metal_name": device_name, "registry_id": 1, "unified_memory": True}
            c = find(r, "metal_shared_event", 128)
            c["samples"]["gpu_end_ns"][7] = c["samples"]["visible_ns"][7] + 500_000
            c["samples"]["gpu_end_ns"][9] = c["samples"]["visible_ns"][9] + 500_000
            find(r, "dawn_staged_timestamps_timed_wait_any", 64)["samples"]["gpu_busy_ns"][2] = 5_000_000
            return evaluate(seal(r))

        result = drifted("Apple Paravirtual device")
        self.assertNotEqual(result["verdict"], "REFUSED", result["reasons"])
        self.assertIn("gpu_clock_untrusted:paravirtual_metal:metal_shared_event:128:idle",
                      result["reasons"])
        self.assertIn("gpu_clock_untrusted:paravirtual_metal:"
                      "dawn_staged_timestamps_timed_wait_any:64:idle", result["reasons"])
        # One reason per cell, however many samples drift.
        self.assertEqual(sum(r.startswith("gpu_clock_untrusted:paravirtual_metal:metal_shared_event:128")
                             for r in result["reasons"]), 1)
        # Control: the same drift on a physical GPU is still refused.
        result = drifted("Apple M3 Ultra")
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("sample_gpu_end_order:metal_shared_event:128:idle", result["reasons"])

    def test_paravirtual_metal_still_refuses_host_clock_disorder(self):
        r = receipt()
        r["dawn"]["adapter_name"] = "Apple Paravirtual device"
        c = find(r, "dawn_imported_timed_wait_any", 64)
        c["samples"]["visible_ns"][5] = c["samples"]["submit_ns"][5] - 10
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("sample_order:dawn_imported_timed_wait_any:64:idle", result["reasons"])

    def test_gpu_busy_longer_than_latency_is_refused(self):
        r = receipt()
        c = find(r, "dawn_staged_timestamps_timed_wait_any", 64)
        c["samples"]["gpu_busy_ns"][2] = 5_000_000
        self.assertIn("sample_gpu_busy_exceeds_latency:"
                      "dawn_staged_timestamps_timed_wait_any:64:idle",
                      evaluate(seal(r))["reasons"])

    def test_cell_without_samples_is_refused_not_crashed(self):
        r = receipt()
        del find(r, "metal_shared_event", 64)["samples"]
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertTrue(any(x.startswith("malformed_receipt") for x in result["reasons"]))

    def test_tampered_preflight_is_refused(self):
        r = receipt(quiet=False)
        r["host_preflight"]["after"]["status"] = "passed"
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("host_preflight_digest_mismatch", result["reasons"])

    def test_oracle_failures_are_refused(self):
        r = receipt()
        c = find(r, "metal_shared_event", 256)
        c["samples"]["oracle"][0] = 0
        c["oracle_failures"] = 1
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("oracle_failures:metal_shared_event:256:idle", result["reasons"])

    def test_understated_oracle_count_is_refused(self):
        r = receipt()
        find(r, "metal_shared_event", 256)["samples"]["oracle"][0] = 0
        self.assertIn("oracle_count_mismatch:metal_shared_event:256",
                      evaluate(seal(r))["reasons"])

    def test_cli_binary_check_refuses_another_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "comparator"
            binary.write_bytes(b"not the binary")
            paths = write_pair(directory, pair())
            args = comparator.parse_args(["verify", *paths, "--binary", str(binary),
                                          "--min-blocks", str(MIN_BLOCKS)])
            self.assertEqual(comparator.cmd_verify(args), 3)

    def test_cli_lowered_block_floor_cannot_reach_a_gate_verdict(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = write_pair(directory, pair())
            out = Path(directory) / "evaluation.json"
            args = comparator.parse_args(["verify", *paths, "--min-blocks",
                                          str(MIN_BLOCKS), "--output", str(out)])
            comparator.cmd_verify(args)
            evaluation = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(evaluation["verdict"], "INCONCLUSIVE")
        self.assertIn("min_blocks_below_campaign_floor", evaluation["reasons"])

    # -- host quietness and coverage ---------------------------------------

    def test_noisy_host_is_evaluated_but_not_counted(self):
        result = evaluate(*pair(quiet=False))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("host_not_quiet", result["reasons"])
        self.assertIsNotNone(result["campaigns"][0]["thresholds"])

    def test_quiet_receipt_flag_cannot_override_a_noisy_preflight(self):
        r = receipt(quiet=False)
        r["host_quiet"] = True
        result = evaluate(seal(r), receipt(assembled_at="2026-10-22T09:00:00+00:00"))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("host_not_quiet", result["reasons"])

    def test_preflight_must_cover_the_measured_revision(self):
        r = receipt()
        r["source_revision"] = "b" * 40
        self.assertIn("host_preflight_source_mismatch", evaluate(seal(r))["reasons"])

    def test_missing_after_preflight_is_not_counted(self):
        r = receipt()
        del r["host_preflight"]["after"]
        self.assertIn("host_preflight_missing", evaluate(seal(r))["reasons"])

    def test_starved_worker_is_not_counted(self):
        r = receipt()
        c = find(r, "metal_shared_event", 128, "render")
        for i in range(c["blocks"] // 50):
            for field in ("submit_ns", "submit_end_ns", "visible_ns", "gpu_end_ns",
                          "callback_ns"):
                c["samples"][field][i] += 3_000_000
        result = evaluate(seal(r))
        self.assertIn("worker_starved:metal_shared_event:128:render", result["reasons"])

    def test_undersampled_campaign_is_not_counted(self):
        result = comparator.evaluate(list(pair()), min_blocks=BLOCKS + 1)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("undersampled", result["reasons"])

    def test_lane_subset_is_not_counted(self):
        first, second = pair()
        for r in (first, second):
            r["cells"] = [c for c in r["cells"]
                          if c["lane"] != "dawn_imported_wait_any_poll50us"]
            r["kernel"]["parity"] = [p for p in r["kernel"]["parity"]
                                     if p["lane"] != "dawn_imported_wait_any_poll50us"]
        result = evaluate(seal(first), seal(second))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("coverage_incomplete:dawn_imported_wait_any_poll50us:64:idle",
                      result["reasons"])

    def test_missing_render_load_is_inconclusive(self):
        first, second = pair()
        for r in (first, second):
            r["cells"] = [c for c in r["cells"] if c["load"] != "render"]
            r["kernel"]["parity"] = [p for p in r["kernel"]["parity"] if p["load"] != "render"]
        result = evaluate(seal(first), seal(second))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("render_load_unavailable:dawn_imported_timed_wait_any:64:render",
                      result["reasons"])

    def test_missing_dawn_timestamps_is_inconclusive(self):
        first, second = pair()
        for r in (first, second):
            for c in r["cells"]:
                if c["side"] == "dawn":
                    c["gpu_timestamps"] = False
                    c["samples"]["gpu_busy_ns"] = [-1] * c["blocks"]
        result = evaluate(seal(first), seal(second))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("dawn_gpu_timestamps_unavailable", result["reasons"])

    # -- thresholds ---------------------------------------------------------

    def dawn(self, **kwargs):
        return {(lane, 64, load): kwargs for lane in gating_lanes("dawn")
                for load in ("idle", "render")}

    def test_criterion_i_fires_on_dawn_tail(self):
        result = evaluate(*pair(self.dawn(tail=900_000)))  # 67.5% of 1.333 ms
        self.assertEqual(result["verdict"], "GATE_FIRES")
        self.assertIn("criterion_i_fires_in_all_counted_campaigns", result["reasons"])

    def test_criterion_i_boundary_does_not_fire(self):
        result = evaluate(*pair(self.dawn(tail=666_666)))  # just under 50%
        self.assertEqual(result["verdict"], "GATE_DOES_NOT_FIRE")

    def test_criterion_i_needs_metal_well_under_quarter_period(self):
        overrides = self.dawn(tail=900_000)
        for lane in gating_lanes("metal"):
            for load in ("idle", "render"):
                overrides[(lane, 64, load)] = {"tail": 400_000}
        result = evaluate(*pair(overrides))
        self.assertEqual(result["campaigns"][0]["thresholds"]["criteria"]["i"],
                         "does_not_fire")

    def test_criterion_i_must_fire_in_both_campaigns(self):
        fired = receipt(self.dawn(tail=900_000))
        result = evaluate(fired, receipt(assembled_at="2026-10-22T09:00:00+00:00"))
        self.assertEqual(result["verdict"], "GATE_DOES_NOT_FIRE")

    def test_best_production_dawn_lane_decides(self):
        # Only one production Dawn lane is slow: the best one still passes.
        overrides = {("dawn_imported_timed_wait_any", 64, load): {"tail": 900_000}
                     for load in ("idle", "render")}
        self.assertEqual(evaluate(*pair(overrides))["verdict"], "GATE_DOES_NOT_FIRE")

    def test_ineligible_lanes_never_decide(self):
        # The staged lane is fast, but it is not a production configuration.
        overrides = self.dawn(tail=900_000)
        for load in ("idle", "render"):
            overrides[("dawn_staged_timestamps_timed_wait_any", 64, load)] = {"s2v": 100_000}
        self.assertEqual(evaluate(*pair(overrides))["verdict"], "GATE_FIRES")

    def test_criterion_ii_uses_process_cpu(self):
        result = evaluate(*pair(self.dawn(proc_cpu=150_000)))  # > 2x and > 10%
        self.assertIn("criterion_ii_fires_in_all_counted_campaigns", result["reasons"])
        ii = result["campaigns"][0]["thresholds"]["per_load"]["idle"]["ii"]
        self.assertEqual(ii["basis"], "process_cpu_minus_render_and_oracle_per_block")
        self.assertEqual(ii["metal_cpu_per_block_ns"], 40_000)

    def test_criterion_ii_ignores_worker_only_cpu(self):
        # Worker CPU omits Metal's dispatch-thread completion work, so it can
        # never fire the criterion on its own.
        result = evaluate(*pair(self.dawn(cpu=500_000)))
        self.assertEqual(result["campaigns"][0]["thresholds"]["criteria"]["ii"],
                         "does_not_fire")

    def test_criterion_ii_needs_ten_percent_of_period(self):
        result = evaluate(*pair(self.dawn(proc_cpu=120_000)))  # > 2x metal, < 133 us
        self.assertEqual(result["campaigns"][0]["thresholds"]["criteria"]["ii"],
                         "does_not_fire")

    def test_criterion_iii_never_decided_by_the_staged_proxy(self):
        overrides = {(lane, 64, load): {"wake": 50_000} for lane in gating_lanes("metal")
                     for load in ("idle", "render")}
        result = evaluate(*pair(overrides))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("criterion_iii_not_evaluable", result["reasons"])
        iii = result["campaigns"][0]["thresholds"]["per_load"]["idle"]["iii"]
        self.assertEqual(iii["basis"], "dawn_wake_unobservable")
        self.assertIsNotNone(iii["dawn_wake_upper_bound_p99_ns_non_production_proxy"])

    def test_criterion_iii_slow_metal_wake_cannot_fire(self):
        iii = evaluate(*pair())["campaigns"][0]["thresholds"]["per_load"]["idle"]["iii"]
        self.assertEqual(iii["basis"], "metal_wake_not_below_threshold")
        self.assertEqual(iii["state"], "does_not_fire")

    def test_criterion_iv_fires_on_render_misses(self):
        overrides = {(lane, 64, "render"): {"misses": 2} for lane in gating_lanes("dawn")}
        result = evaluate(*pair(overrides))
        self.assertIn("criterion_iv_fires_in_all_counted_campaigns", result["reasons"])

    def test_criterion_iv_does_not_fire_when_metal_also_misses(self):
        overrides = {(lane, 64, "render"): {"misses": 2} for lane in gating_lanes("dawn")}
        for lane in gating_lanes("metal"):
            overrides[(lane, 64, "render")] = {"misses": 1}
        result = evaluate(*pair(overrides))
        self.assertEqual(result["campaigns"][0]["thresholds"]["criteria"]["iv"],
                         "does_not_fire")

    def test_receipt_digest_ignores_only_its_own_field(self):
        r = receipt()
        self.assertEqual(r["receipt_sha256"], comparator.receipt_digest(r))
        r["mode"] = "smoke"
        self.assertNotEqual(r["receipt_sha256"], comparator.receipt_digest(r))
        json.dumps(r)  # receipts stay plain JSON


if __name__ == "__main__":
    unittest.main()
