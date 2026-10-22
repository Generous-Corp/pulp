#!/usr/bin/env python3
"""Self-test for the WP-h1 DAW-continuity receipt verifier.

Receipts come from the harness's own dry run (reaper_smoke.py --mode gpu-fault
--dry-run), so the producer and the verifier are exercised together. Each
rejection case is paired with the passing receipt it was derived from.
"""
from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
HARNESS = REPO / "tools/testing/daw-smoke/reaper_smoke.py"
SPEC = importlib.util.spec_from_file_location("verify_daw_continuity",
                                              HERE / "verify_gpu_audio_daw_continuity_receipt.py")
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)
gfc = VERIFY.gfc

FRAMES, FAULT, POST = 6000, 3000, 2400


def dry_run(out: Path, fault: str = "force_loss_between", *extra: str) -> int:
    return subprocess.run(
        [sys.executable, str(HARNESS), "--mode", "gpu-fault", "--gpu-fault", fault,
         "--dry-run", "--stimulus-frames", str(FRAMES), "--fault-at-sample", str(FAULT),
         "--window-post", str(POST), "--out-dir", str(out), *extra],
        capture_output=True, text=True, check=False).returncode


def load(out: Path) -> dict:
    return json.loads((out / "receipt.json").read_text())


def resign(receipt: dict) -> dict:
    receipt["receipt_sha256"] = gfc.receipt_digest(receipt)
    return receipt


class DaWContinuityReceipt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.good = root / "good"
        cls.bad = root / "bad"
        cls.good_exit = dry_run(cls.good)
        cls.bad_exit = dry_run(cls.bad, "force_loss_between", "--dry-run-break-fallback")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def verify(self, receipt: dict, run_dir: Path, **kw) -> str:
        return VERIFY.verify(receipt, run_dir, allow_dry_run=True, **kw)

    # Positive controls.
    def test_continuous_fallback_verifies_as_pass(self):
        self.assertEqual(self.good_exit, 0)
        self.assertEqual(self.verify(load(self.good), self.good), "pass")

    def test_every_fault_kind_has_a_passing_dry_run(self):
        for kind in gfc.FAULT_KINDS:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                self.assertEqual(dry_run(Path(tmp), kind), 0)
                self.assertEqual(self.verify(load(Path(tmp)), Path(tmp)), "pass")

    # The planted negative: no fallback after the fault.
    def test_silence_after_the_fault_is_a_verified_fail_at_the_fault_sample(self):
        self.assertEqual(self.bad_exit, 1)
        receipt = load(self.bad)
        self.assertEqual(self.verify(receipt, self.bad), "fail")
        self.assertEqual(receipt["nulls"]["plugin"]["first_deviating_sample"], FAULT)
        self.assertEqual(receipt["nulls"]["bounce"]["first_deviating_sample"], FAULT)

    # Rejections, each against the passing receipt.
    def test_a_dry_run_is_not_daw_evidence(self):
        with self.assertRaisesRegex(VERIFY.ReceiptError, "not DAW evidence"):
            VERIFY.verify(load(self.good), self.good)

    def test_an_edited_body_breaks_the_receipt_digest(self):
        receipt = load(self.good)
        receipt["host_alive"] = False
        with self.assertRaisesRegex(VERIFY.ReceiptError, "receipt_sha256"):
            self.verify(receipt, self.good)

    def test_edited_numbers_do_not_re_derive(self):
        receipt = load(self.bad)
        receipt["nulls"]["plugin"]["passed"] = True
        receipt["verdict"] = "pass"
        receipt["verdict_reasons"] = []
        with self.assertRaisesRegex(VERIFY.ReceiptError, "re-derive"):
            self.verify(resign(receipt), self.bad)

    def test_a_swapped_capture_fails_its_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            for item in self.good.iterdir():
                (run / item.name).write_bytes(item.read_bytes())
            (run / "capture.bin").write_bytes((self.bad / "capture.bin").read_bytes())
            with self.assertRaisesRegex(VERIFY.ReceiptError, "capture.bin digest"):
                self.verify(load(run), run)

    def test_a_swapped_capture_with_rebound_digests_does_not_re_derive(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            for item in self.good.iterdir():
                (run / item.name).write_bytes(item.read_bytes())
            (run / "capture.bin").write_bytes((self.bad / "capture.bin").read_bytes())
            receipt = load(run)
            receipt["artifacts"]["capture.bin"] = gfc.sha256_file(run / "capture.bin")
            with self.assertRaisesRegex(VERIFY.ReceiptError, "re-derive"):
                self.verify(resign(receipt), run)

    def test_an_in_process_trigger_is_not_control_op_evidence(self):
        receipt = load(self.good)
        with self.assertRaisesRegex(VERIFY.ReceiptError, "control-op evidence required"):
            self.verify(receipt, self.good, require_control_op=True)

    def test_a_control_op_claim_needs_the_exchange(self):
        receipt = load(self.good)
        receipt["trigger_transport"] = "control_op"
        receipt["control_op"] = None
        with self.assertRaisesRegex(VERIFY.ReceiptError, "gpu_audio_fault_inject"):
            self.verify(resign(receipt), self.good)

    def test_a_noisy_host_is_refused_when_quiet_is_required(self):
        with self.assertRaisesRegex(VERIFY.ReceiptError, "quiet host required"):
            self.verify(load(self.good), self.good, require_quiet_host=True)

    def test_host_quiet_must_agree_with_the_preflight(self):
        receipt = load(self.good)
        receipt["host_quiet"] = True
        with self.assertRaisesRegex(VERIFY.ReceiptError, "disagrees"):
            self.verify(resign(receipt), self.good)

    def test_an_artifact_name_cannot_escape_the_run_dir(self):
        receipt = load(self.good)
        receipt["artifacts"]["../outside"] = "0" * 64
        with self.assertRaisesRegex(VERIFY.ReceiptError, "escapes"):
            self.verify(resign(receipt), self.good)

    def test_receipt_matches_the_checked_in_schema(self):
        schema = json.loads((REPO / "docs/contracts/gpu-audio-daw-continuity-v1.schema.json")
                            .read_text())
        receipt = load(self.good)
        self.assertEqual(schema["properties"]["schema"]["const"], receipt["schema"])
        self.assertEqual(set(schema["required"]) - set(receipt), set())
        self.assertEqual(set(receipt) - set(schema["properties"]), set())
        try:
            import jsonschema
        except ImportError:
            return
        jsonschema.validate(receipt, schema)
        jsonschema.validate(load(self.bad), schema)

    def test_cli_exit_codes(self):
        self.assertEqual(VERIFY.main([str(self.good / "receipt.json"), "--allow-dry-run"]), 0)
        self.assertEqual(VERIFY.main([str(self.bad / "receipt.json"), "--allow-dry-run"]), 1)
        self.assertEqual(VERIFY.main([str(self.good / "receipt.json")]), 2)


class Decisions(unittest.TestCase):
    """The verdict rules, each with its passing counterpart."""

    OK = {"passed": True, "max_abs_error": 0.0, "first_deviating_sample": None,
          "last_deviating_sample": None}

    def decide(self, **kw):
        base = dict(fault="force_loss_before", plugin_null=self.OK, bounce_null=self.OK,
                    health_ok=True, fault_fired=True, host_alive=True, realtime_at_fault=True)
        base.update(kw)
        return gfc.decide(**base)[0]

    def test_all_checks_pass(self):
        self.assertEqual(self.decide(), "pass")

    def test_a_host_crash_is_a_fail_not_an_inconclusive(self):
        self.assertEqual(self.decide(host_alive=False, fault="render_force_loss"), "fail")

    def test_an_unfired_fault_is_inconclusive(self):
        self.assertEqual(self.decide(fault_fired=False), "inconclusive")

    def test_an_offline_fault_block_is_inconclusive(self):
        self.assertEqual(self.decide(realtime_at_fault=False), "inconclusive")

    def test_a_missing_bounce_is_inconclusive(self):
        self.assertEqual(self.decide(bounce_null=None), "inconclusive")

    def test_a_fault_the_counters_never_saw_is_inconclusive(self):
        self.assertEqual(self.decide(health_ok=False), "inconclusive")


class HealthExpectations(unittest.TestCase):
    def snaps(self, after: dict) -> dict:
        before = {"provider_state": "ready", "recovery_reason": "none",
                  "produced_blocks": 10, "fallback_blocks": 0, "missed_blocks": 0}
        return {"before": before, "at": before, "after": {**before, **after}}

    def test_loss_needs_provider_lost_and_fallback(self):
        ok = self.snaps({"recovery_reason": "provider-lost", "fallback_blocks": 5})
        self.assertTrue(gfc.health_expectation("force_loss_after", ok)[0])
        self.assertFalse(gfc.health_expectation(
            "force_loss_after", self.snaps({"fallback_blocks": 5}))[0])
        self.assertFalse(gfc.health_expectation(
            "force_loss_after", self.snaps({"recovery_reason": "provider-lost"}))[0])

    def test_render_loss_must_leave_audio_untouched(self):
        ok = self.snaps({"produced_blocks": 50})
        self.assertTrue(gfc.health_expectation("render_force_loss", ok)[0])
        self.assertFalse(gfc.health_expectation(
            "render_force_loss",
            self.snaps({"produced_blocks": 50, "recovery_reason": "provider-lost"}))[0])
        self.assertFalse(gfc.health_expectation("render_force_loss", self.snaps({}))[0])

    def test_a_gpu_that_never_delivered_is_not_met(self):
        # Every block already CPU fallback (anticipative FX): the fault tested
        # only the fallback, so even a clean null proves nothing about the GPU.
        snaps = self.snaps({"recovery_reason": "provider-lost", "fallback_blocks": 5})
        for phase in snaps.values():
            phase["delivery"] = {"gpu_blocks": 0}
        self.assertFalse(gfc.health_expectation("force_loss_after", snaps)[0])
        snaps["before"]["delivery"] = {"gpu_blocks": 3}
        self.assertTrue(gfc.health_expectation("force_loss_after", snaps)[0])

    def test_missing_phase_is_not_met(self):
        snaps = self.snaps({"produced_blocks": 50})
        del snaps["at"]
        self.assertFalse(gfc.health_expectation("render_force_loss", snaps)[0])


class CaptureFormat(unittest.TestCase):
    def test_round_trip_and_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "c.bin"
            blocks = [{"position": None, "flags": gfc.FLAG_SEGMENT_START,
                       "inputs": [[0.5, 0.25]] * 2, "outputs": [[0.0, 0.0]] * 2},
                      {"position": 0, "flags": gfc.FLAG_PLAYING | gfc.FLAG_FAULT_BLOCK,
                       "inputs": [[1.0, 2.0]] * 2, "outputs": [[3.0, 4.0]] * 2}]
            gfc.write_capture(path, 48000, blocks)
            cap = gfc.read_capture(path)
            self.assertEqual(cap.positions, [-1, -1, 0, 1])
            self.assertEqual(cap.outputs[1], [0.0, 0.0, 3.0, 4.0])
            self.assertEqual(cap.fault_block_starts, [2])
            # A cut final record (REAPER stopped mid-write) is dropped and counted.
            path.write_bytes(path.read_bytes()[:-3])
            cut = gfc.read_capture(path)
            self.assertEqual(cut.positions, [-1, -1])
            self.assertEqual(cut.truncated_tail_bytes, 16 + 2 * 2 * 2 * 4 - 3)

    def test_a_reprepare_restarts_the_reference(self):
        ir = [1.0, 0.5]
        cap = gfc.Capture(48000, 1, [[1.0, 0.0, 1.0, 0.0]], [[0.0] * 4], [-1] * 4, [0, 2])
        self.assertEqual(gfc.plugin_reference(cap, ir, 0)[0], [1.0, 0.5, 1.0, 0.5])
        self.assertEqual(gfc.plugin_reference(cap, ir, 1)[0], [0.0, 1.0, 0.0, 1.0])

    def test_non_finite_output_fails_and_stays_json_safe(self):
        result = gfc.null_test([[0.0, float("nan")]], [[0.0, 0.0]], 0, tolerance=1e-4)
        self.assertFalse(result.passed)
        self.assertIsNone(result.as_dict()["max_abs_error"])
        json.dumps(result.as_dict(), allow_nan=False)


if __name__ == "__main__":
    unittest.main()
