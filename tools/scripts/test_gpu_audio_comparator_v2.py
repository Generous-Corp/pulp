import copy
import hashlib
import json
import unittest

import gpu_audio_comparator_v2 as comparator

FFT_SHA = "f" * 64
MUL_SHA = "e" * 64
BLOCKS = 400
MIN_BLOCKS = 100
LANES = (
    ("metal_shared_event", "metal", True),
    ("dawn_imported_timed_wait_any", "dawn", True),
    ("dawn_staged_timestamps_timed_wait_any", "dawn", False),
)


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


def kernel(frames, program=comparator.PROGRAM_ID, fft=FFT_SHA, mul=MUL_SHA):
    canon = {"channels": 2, "fft_size": frames * 2, "fft_wgsl_sha256": fft, "frames": frames,
             "ir_length": frames, "mul_wgsl_sha256": mul, "passes": passes(frames),
             "program": program}
    return {"canonical": canon,
            "kernel_digest": hashlib.sha256(comparator.canonical(canon)).hexdigest()}


def cell(lane, side, frames, load, *, s2v=200_000, tail=None, cpu=20_000, wake=50_000,
         busy=60_000, misses=0, blocks=BLOCKS):
    period = frames * 1e9 / 48_000
    samples = {f: [] for f in comparator.SAMPLE_FIELDS}
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
        timed = side == "metal" or "staged" in lane
        samples["gpu_busy_ns"].append(busy if timed else -1)
        samples["gpu_end_ns"].append(visible - wake if side == "metal" else -1)
        samples["callback_ns"].append(visible - 5_000 if side == "metal" else -1)
        samples["worker_cpu_ns"].append(cpu)
        samples["oracle"].append(1)
        samples["stalled"].append(0)
    return {"lane": lane, "side": side, "frames": frames, "load": load,
            "status": "completed", "reason": "", "blocks": blocks, "warmup_blocks": 32,
            "kernel": kernel(frames), "oracle_failures": 0, "stalls": 0,
            "gpu_timestamps": side == "metal" or "staged" in lane,
            "render_load": {"active": load == "render", "status": "completed",
                            "dropped_frames": 0, "thread_cpu_ns": 0,
                            "frame_ns": [4_000_000] * 10 if load == "render" else []},
            "samples": samples}


def receipt(overrides=None, *, quiet=True, blocks=BLOCKS, host="host-a"):
    """A complete, matched, bound receipt; `overrides` maps (lane, frames, load) to
    keyword arguments for that cell."""
    overrides = overrides or {}
    cells = []
    for lane, side, _ in LANES:
        for frames in (64, 128, 256):
            for load in ("idle", "render"):
                cells.append(cell(lane, side, frames, load, blocks=blocks,
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
            "provider_wgsl": {"ok": True, "fft_sha256": FFT_SHA, "mul_sha256": MUL_SHA},
            "parity": [{"frames": c["frames"], "lane": c["lane"], "load": c["load"],
                        "max_abs_diff": 1e-7, "tolerance": 1e-4} for c in cells],
        },
        "config": {"sample_rate": 48_000, "channels": 2, "flight_depth": 2,
                   "lead_blocks": 2, "blocks_per_cell": blocks, "warmup_blocks": 32,
                   "frames": [64, 128, 256], "paced": True,
                   "lanes": [{"name": n, "side": s, "production_eligible": e}
                             for n, s, e in LANES]},
        "cells": cells,
        "host_preflight": preflight,
        "host_preflight_sha256": hashlib.sha256(comparator.canonical(preflight)).hexdigest(),
        "host_quiet": quiet,
        "source_revision": "a" * 40,
    }
    return seal(body)


def seal(body):
    body = dict(body)
    body["receipt_sha256"] = comparator.receipt_digest(body)
    return body


def find(r, lane, frames, load="idle"):
    return next(c for c in r["cells"]
                if c["lane"] == lane and c["frames"] == frames and c["load"] == load)


def evaluate(*receipts):
    return comparator.evaluate(list(receipts), min_blocks=MIN_BLOCKS)


class ComparatorV2Tests(unittest.TestCase):
    def test_percentile_nearest_rank(self):
        values = list(range(1, 1001))
        self.assertEqual(comparator.percentile(values, 0.5), 500)
        self.assertEqual(comparator.percentile(values, 0.99), 990)
        self.assertEqual(comparator.percentile(values, 0.999), 999)
        self.assertIsNone(comparator.percentile([], 0.5))

    def test_positive_control_two_quiet_matched_campaigns_do_not_fire(self):
        result = evaluate(receipt(), receipt())
        self.assertEqual(result["verdict"], "GATE_DOES_NOT_FIRE", result["reasons"])
        self.assertEqual(result["thresholds"]["status"], "PROPOSED")
        self.assertFalse(result["thresholds"]["ratified"])
        criteria = result["campaigns"][0]["thresholds"]["criteria"]
        self.assertEqual(set(criteria.values()), {"does_not_fire"})

    def test_single_campaign_is_never_conclusive(self):
        result = evaluate(receipt())
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("counted_campaigns_1_of_2", result["reasons"])

    def test_planted_unmatched_kernel_is_refused_by_digest(self):
        r = receipt()
        planted = find(r, "metal_shared_event", 64)
        planted["kernel"] = kernel(64, program="pulp.native_metal.complex_multiply.v0")
        result = evaluate(seal(r), receipt())
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

    def test_dawn_cell_must_run_the_provider_wgsl(self):
        r = receipt()
        r["kernel"]["provider_wgsl"]["fft_sha256"] = "1" * 64
        result = evaluate(seal(r))
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("dawn_kernel_not_provider_wgsl:dawn_imported_timed_wait_any",
                      result["reasons"])

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

    def test_missing_parity_is_refused(self):
        r = receipt()
        r["kernel"]["parity"].pop()
        self.assertEqual(evaluate(seal(r))["verdict"], "REFUSED")

    def test_tampered_samples_are_refused(self):
        r = receipt()
        find(r, "dawn_imported_timed_wait_any", 64)["samples"]["visible_ns"][3] += 1
        result = evaluate(r)
        self.assertEqual(result["verdict"], "REFUSED")
        self.assertIn("receipt_digest_mismatch", result["reasons"])

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

    def test_noisy_host_is_evaluated_but_not_counted(self):
        result = evaluate(receipt(quiet=False), receipt(quiet=False))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("host_not_quiet", result["reasons"])
        self.assertIsNotNone(result["campaigns"][0]["thresholds"])

    def test_quiet_receipt_flag_cannot_override_a_noisy_preflight(self):
        r = receipt(quiet=False)
        r["host_quiet"] = True
        result = evaluate(seal(r), receipt())
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("host_not_quiet", result["reasons"])

    def test_preflight_must_cover_the_measured_revision(self):
        r = receipt()
        r["source_revision"] = "b" * 40
        result = evaluate(seal(r), receipt())
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("host_preflight_source_mismatch", result["reasons"])

    def test_missing_after_preflight_is_not_counted(self):
        r = receipt()
        del r["host_preflight"]["after"]
        result = evaluate(seal(r), receipt())
        self.assertIn("host_preflight_missing", result["reasons"])

    def test_undersampled_campaign_is_not_counted(self):
        result = comparator.evaluate([receipt(), receipt()], min_blocks=BLOCKS + 1)
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("undersampled", result["reasons"])

    def test_missing_render_load_is_inconclusive(self):
        r = receipt()
        r["cells"] = [c for c in r["cells"] if c["load"] != "render"]
        r["kernel"]["parity"] = [p for p in r["kernel"]["parity"] if p["load"] != "render"]
        result = evaluate(seal(r), seal(copy.deepcopy(r)))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("render_load_unavailable:dawn:64:render", result["reasons"])

    def test_missing_dawn_timestamps_is_inconclusive(self):
        r = receipt()
        for c in r["cells"]:
            if c["side"] == "dawn":
                c["gpu_timestamps"] = False
                c["samples"]["gpu_busy_ns"] = [-1] * c["blocks"]
        result = evaluate(seal(r), seal(copy.deepcopy(r)))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("dawn_gpu_timestamps_unavailable", result["reasons"])

    def test_different_hosts_do_not_make_two_campaigns(self):
        result = evaluate(receipt(host="a"), receipt(host="b"))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("campaigns_not_same_host_and_dawn_revision", result["reasons"])

    def dawn_tail(self, **kwargs):
        overrides = {}
        for lane in ("dawn_imported_timed_wait_any", "dawn_staged_timestamps_timed_wait_any"):
            for load in ("idle", "render"):
                overrides[(lane, 64, load)] = kwargs
        return overrides

    def test_criterion_i_fires_on_dawn_tail(self):
        overrides = self.dawn_tail(tail=900_000)  # 67.5% of 1.333 ms
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertEqual(result["verdict"], "GATE_FIRES")
        self.assertIn("criterion_i_fires_in_all_counted_campaigns", result["reasons"])

    def test_criterion_i_boundary_does_not_fire(self):
        overrides = self.dawn_tail(tail=666_666)  # just under 50%
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertEqual(result["verdict"], "GATE_DOES_NOT_FIRE")

    def test_criterion_i_needs_metal_well_under_quarter_period(self):
        overrides = self.dawn_tail(tail=900_000)
        for load in ("idle", "render"):
            overrides[("metal_shared_event", 64, load)] = {"tail": 400_000}
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertEqual(result["campaigns"][0]["thresholds"]["criteria"]["i"],
                         "does_not_fire")

    def test_criterion_i_must_fire_in_both_campaigns(self):
        fired = receipt(self.dawn_tail(tail=900_000))
        result = evaluate(fired, receipt())
        self.assertEqual(result["verdict"], "GATE_DOES_NOT_FIRE")

    def test_ineligible_dawn_lane_cannot_hide_the_production_lane(self):
        overrides = {}
        for load in ("idle", "render"):
            overrides[("dawn_imported_timed_wait_any", 64, load)] = {"tail": 900_000}
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertEqual(result["verdict"], "GATE_FIRES")

    def test_criterion_ii_fires_on_dawn_cpu(self):
        overrides = self.dawn_tail(cpu=150_000)  # > 2x metal and > 10% of period
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertIn("criterion_ii_fires_in_all_counted_campaigns", result["reasons"])

    def test_criterion_ii_needs_ten_percent_of_period(self):
        overrides = self.dawn_tail(cpu=120_000)  # 2x+ metal but < 133 us
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertEqual(result["campaigns"][0]["thresholds"]["criteria"]["ii"],
                         "does_not_fire")

    def test_criterion_iii_uses_upper_bound_when_dawn_wake_is_unobservable(self):
        result = evaluate(receipt(), receipt())
        iii = result["campaigns"][0]["thresholds"]["per_load"]["idle"]["iii"]
        self.assertEqual(iii["basis"], "dawn_upper_bound_within_threshold")
        self.assertEqual(iii["state"], "does_not_fire")

    def test_criterion_iii_large_bound_is_not_evaluable(self):
        overrides = {}
        for load in ("idle", "render"):
            overrides[("dawn_staged_timestamps_timed_wait_any", 64, load)] = {
                "s2v": 500_000, "busy": 60_000}
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("criterion_iii_not_evaluable", result["reasons"])

    def test_criterion_iii_slow_metal_wake_cannot_fire(self):
        overrides = {}
        for load in ("idle", "render"):
            overrides[("metal_shared_event", 64, load)] = {"wake": 150_000}
            overrides[("dawn_staged_timestamps_timed_wait_any", 64, load)] = {"s2v": 500_000}
        result = evaluate(receipt(overrides), receipt(overrides))
        iii = result["campaigns"][0]["thresholds"]["per_load"]["idle"]["iii"]
        self.assertEqual(iii["basis"], "metal_wake_not_below_threshold")

    def test_criterion_iv_fires_on_render_misses(self):
        overrides = {("dawn_imported_timed_wait_any", 64, "render"): {"misses": 2}}
        result = evaluate(receipt(overrides), receipt(overrides))
        self.assertIn("criterion_iv_fires_in_all_counted_campaigns", result["reasons"])

    def test_criterion_iv_does_not_fire_when_metal_also_misses(self):
        overrides = {("dawn_imported_timed_wait_any", 64, "render"): {"misses": 2},
                     ("metal_shared_event", 64, "render"): {"misses": 1}}
        result = evaluate(receipt(overrides), receipt(overrides))
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
