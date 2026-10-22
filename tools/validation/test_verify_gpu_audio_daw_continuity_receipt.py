#!/usr/bin/env python3
"""Self-test for the WP-h1 DAW-continuity receipt verifier.

Receipts come from the harness's own dry run (reaper_smoke.py --mode gpu-fault
--dry-run), so the producer and the verifier are exercised together. Each
rejection case is paired with the passing receipt it was derived from.
"""
from __future__ import annotations

import copy
from unittest import mock
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

FRAMES, FAULT, PRE, POST = 9600, 3000, 1000, 4800


def dry_run(out: Path, fault: str = "force_loss_between", *extra: str) -> int:
    return subprocess.run(
        [sys.executable, str(HARNESS), "--mode", "gpu-fault", "--gpu-fault", fault,
         "--dry-run", "--stimulus-frames", str(FRAMES), "--fault-at-sample", str(FAULT),
         "--window-pre", str(PRE), "--window-post", str(POST), "--out-dir", str(out), *extra],
        capture_output=True, text=True, encoding="utf-8", check=False).returncode


def load(out: Path) -> dict:
    return json.loads((out / "receipt.json").read_text(encoding="utf-8"))


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
        # The host compensates PDC, so in its realtime render the dropout starts
        # PDC samples earlier; the fault-free offline bounce still nulls.
        self.assertEqual(receipt["nulls"]["host_realtime"]["first_deviating_sample"],
                         FAULT - receipt["pdc_samples"])
        self.assertTrue(receipt["nulls"]["bounce"]["passed"])

    # M3: continuous-but-wrong planted negatives must FAIL; the path negative
    # must be INCONCLUSIVE ("not on the shared-I/O path").
    def test_every_planted_negative_verifies_as_fail(self):
        for planted in gfc.PLANTED:
            with self.subTest(planted=planted), tempfile.TemporaryDirectory() as tmp:
                self.assertEqual(dry_run(Path(tmp), "force_loss_after", "--planted", planted), 1)
                self.assertEqual(self.verify(load(Path(tmp)), Path(tmp)), "fail")

    def test_gpu_off_verifies_as_inconclusive(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(dry_run(Path(tmp), "force_loss_after", "--gpu-off"), 3)
            receipt = load(Path(tmp))
            self.assertEqual(self.verify(receipt, Path(tmp)), "inconclusive")
            self.assertTrue(any("shared-I/O" in r for r in receipt["verdict_reasons"]))

    # M2: the per-block delivery stream.
    def test_delivery_requirements(self):
        run = self.copy_run(self.good)
        cap = gfc.read_capture(run / "capture.bin")
        kw = dict(fault="force_loss_between", fault_sample=FAULT, applied_sample=FAULT + 128,
                  quantum=128, lead_blocks=2, window_post=POST)
        ok = gfc.delivery_analysis(cap, **kw)
        self.assertTrue(ok["met"], ok["note"])
        self.assertEqual(ok["pre_fault_gpu_share"], 1.0)

        def rewrite(edit):
            records = [dict(r) for r in cap.records]
            edit(records)
            return gfc.delivery_analysis(gfc.Capture(**{**cap.__dict__, "records": records}),
                                         **kw)
        # A GPU that kept delivering after the loss.
        def gpu_after(records):
            for r in records:
                if r["position"] >= FAULT + 1024:
                    r["gpu"] += 5
        self.assertFalse(rewrite(gpu_after)["met"])
        # Mostly fallback before the fault: void.
        def mostly_fallback(records):
            bump = 0
            for r in records:
                if 0 <= r["position"] < FAULT:
                    bump += 3
                r["fallback"] += bump
        self.assertTrue(rewrite(mostly_fallback)["void"])
        # Any silence block is reported.
        def silence(records):
            for r in records:
                if r["position"] >= FAULT:
                    r["silence"] += 1
        self.assertGreater(rewrite(silence)["silence_blocks"], 0)

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
                            .read_text(encoding="utf-8"))
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

    # The reviewer's forgery probes, each now rejected.
    def copy_run(self, source: Path) -> Path:
        run = Path(self.tmp.name) / f"copy-{len(list(Path(self.tmp.name).iterdir()))}"
        run.mkdir()
        for item in source.iterdir():
            (run / item.name).write_bytes(item.read_bytes())
        return run

    def test_a_relabeled_dry_run_claiming_a_quiet_host_is_rejected(self):
        receipt = load(self.good)
        receipt["trigger_transport"] = "in_process_file"
        receipt["host"]["reaper_version"] = "7.0"
        receipt["host"]["reaper_pid"] = 4242
        receipt["plugin"]["executable_sha256"] = "0" * 64
        receipt["preflight"] = {"status": "ok", "quiet_host": True, "reasons": []}
        receipt["host_quiet"] = True
        with self.assertRaisesRegex(VERIFY.ReceiptError, "reaper.out.txt not listed|preflight"):
            VERIFY.verify(resign(receipt), self.good, require_quiet_host=True)

    def test_a_quiet_preflight_summary_must_come_from_preflight_json(self):
        receipt = load(self.good)
        receipt["preflight"] = {**receipt["preflight"], "quiet_host": True}
        receipt["host_quiet"] = True
        with self.assertRaisesRegex(VERIFY.ReceiptError, "preflight summary"):
            self.verify(resign(receipt), self.good)

    def test_capture_must_be_digest_bound(self):
        receipt = load(self.good)
        del receipt["artifacts"]["capture.bin"]
        with self.assertRaisesRegex(VERIFY.ReceiptError, "capture.bin not listed"):
            self.verify(resign(receipt), self.good)

    def test_a_truncated_bounce_cannot_pass(self):
        run = self.copy_run(self.good)
        _, bounce = gfc.read_wav(run / "bounce.wav")
        gfc.write_wav_f32(run / "bounce.wav", [p[:FAULT - 500] for p in bounce])
        receipt = load(run)
        receipt["artifacts"]["bounce.wav"] = gfc.sha256_file(run / "bounce.wav")
        with self.assertRaisesRegex(VERIFY.ReceiptError, "re-derive"):
            self.verify(resign(receipt), run)
        # And recomputed honestly, it is a FAIL for not covering the window.
        evidence = gfc.evaluate_run(run, fault="force_loss_between", fault_sample=FAULT,
                                    tolerance=1e-4, window_pre=PRE, window_post=POST,
                                    host_alive=True, bounce_path=run / "bounce.wav",
                                    render_frames=FRAMES + POST)
        self.assertFalse(evidence["nulls"]["bounce"]["covered"])
        self.assertEqual(evidence["verdict"], "fail")

    def test_a_shrunk_window_and_render_length_are_rejected(self):
        # Re-review probe: cut the bounce just past the fault and shrink the
        # receipt's own window and render length to match.
        run = self.copy_run(self.good)
        _, bounce = gfc.read_wav(run / "bounce.wav")
        gfc.write_wav_f32(run / "bounce.wav", [p[:FAULT + 2] for p in bounce])
        receipt = load(run)
        receipt["artifacts"]["bounce.wav"] = gfc.sha256_file(run / "bounce.wav")
        receipt["analysis"]["window"]["post"] = 1
        receipt["analysis"]["render_frames"] = FAULT + 1
        with self.assertRaisesRegex(VERIFY.ReceiptError, "window.post is shorter"):
            self.verify(resign(receipt), run)
        receipt["analysis"]["window"]["post"] = gfc.MIN_WINDOW_POST_DRY_RUN
        with self.assertRaisesRegex(VERIFY.ReceiptError, "render_frames does not match"):
            self.verify(resign(receipt), run)
        # And a legal-length window that differs from what the plug-in was
        # asked to observe is a mismatch.
        receipt["analysis"]["render_frames"] = FRAMES + gfc.MIN_WINDOW_POST_DRY_RUN
        with self.assertRaisesRegex(VERIFY.ReceiptError, "re-derive"):
            self.verify(resign(receipt), run)

    def test_the_analysed_window_must_be_the_one_the_plugin_observed(self):
        evidence = gfc.evaluate_run(self.good, fault="force_loss_between", fault_sample=FAULT,
                                    tolerance=1e-4, window_pre=PRE, window_post=POST - 100,
                                    host_alive=True, bounce_path=self.good / "bounce.wav",
                                    realtime_path=self.good / "realtime.wav")
        self.assertEqual(evidence["verdict"], "fail")
        self.assertTrue(any("observes until" in r for r in evidence["verdict_reasons"]))

    def test_a_host_that_fed_silence_is_inconclusive_not_a_fail(self):
        # Seen live: REAPER's online render occasionally fed the plug-in
        # silence instead of the stimulus; the output nulled trivially.
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            gfc.synthesize_run(run, fault="force_loss_after", fault_sample=FAULT, frames=FRAMES,
                               tail=POST)
            cap = gfc.read_capture(run / "capture.bin")
            blocks = []
            for r in cap.records:
                start, n = r["stream"], r["frames"]
                blocks.append({"position": None if r["position"] < 0 else r["position"],
                               "flags": (gfc.FLAG_PLAYING if r["position"] >= 0 else 0) |
                                        ((gfc.FLAG_SEGMENT_START | (1 << gfc.PREPARE_INDEX_SHIFT))
                                         if r["segment_start"] else 0),
                               "inputs": [[0.0] * n] * 2, "outputs": [[0.0] * n] * 2,
                               "counters": (r["gpu"], r["fallback"], r["silence"], r["priming"])})
            gfc.write_capture(run / "capture.bin", 48000, blocks)
            result = gfc.evaluate_run(run, fault="force_loss_after", fault_sample=FAULT,
                                      tolerance=1e-4, window_pre=PRE, window_post=POST,
                                      host_alive=True, bounce_path=None)
            self.assertEqual(result["verdict"], "inconclusive", result["verdict_reasons"])
            self.assertTrue(any("was not the stimulus" in r
                                for r in result["verdict_reasons"]))

    def test_a_capture_overflow_is_inconclusive_not_an_audio_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            gfc.synthesize_run(run, fault="worker_stall", fault_sample=FAULT, frames=FRAMES, tail=POST,
                               break_fallback=True)
            with (run / "events.jsonl").open("a", encoding="utf-8") as stream:
                stream.write('{"event": "capture_overflow", "dropped_records": 1}\n')
            kw = dict(fault="worker_stall", fault_sample=FAULT, tolerance=1e-4, window_pre=PRE,
                      window_post=POST, host_alive=True, bounce_path=None)
            result = gfc.evaluate_run(run, **kw)
            self.assertIsNone(result["nulls"]["plugin"])
            self.assertEqual(result["verdict"], "inconclusive")

    def test_a_corrupt_capture_is_inconclusive_not_an_audio_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            gfc.synthesize_run(run, fault="worker_stall", fault_sample=FAULT, frames=FRAMES, tail=POST)
            blob = bytearray((run / "capture.bin").read_bytes())
            blob[20 + 8:20 + 12] = (1 << 30).to_bytes(4, "little")  # absurd frame count
            (run / "capture.bin").write_bytes(bytes(blob))
            result = gfc.evaluate_run(run, fault="worker_stall", fault_sample=FAULT, tolerance=1e-4,
                                      window_pre=PRE, window_post=POST, host_alive=True,
                                      bounce_path=None)
            self.assertIsNone(result["nulls"]["plugin"])
            self.assertEqual(result["verdict"], "inconclusive")

    def test_a_relabeled_fault_kind_is_a_mismatch(self):
        receipt = load(self.good)
        receipt["fault"]["kind"] = "delay_completion"
        with self.assertRaisesRegex(VERIFY.ReceiptError, "re-derive"):
            self.verify(resign(receipt), self.good)
        evidence = gfc.evaluate_run(self.good, fault="delay_completion", fault_sample=FAULT,
                                    tolerance=1e-4, window_pre=PRE, window_post=POST,
                                    host_alive=True, bounce_path=self.good / "bounce.wav",
                                    realtime_path=self.good / "realtime.wav")
        self.assertEqual(evidence["verdict"], "fail")
        self.assertIn("names fault", evidence["verdict_reasons"][0])

    def test_a_live_receipt_needs_a_one_second_window(self):
        run = self.copy_run(self.good)
        (run / "reaper.out.txt").write_text("CLAP: initialized 'dry-run'\n", encoding="utf-8")
        (run / "claim").write_text("4242\n", encoding="utf-8")
        receipt = load(run)
        receipt["artifacts"]["claim"] = gfc.sha256_file(run / "claim")
        receipt["trigger_transport"] = "in_process_file"
        receipt["host"]["reaper_version"] = "7.82"
        receipt["host"]["reaper_pid"] = 4242
        receipt["plugin"]["executable_sha256"] = "0" * 64
        receipt["artifacts"]["reaper.out.txt"] = gfc.sha256_file(run / "reaper.out.txt")
        with self.assertRaisesRegex(VERIFY.ReceiptError, "shorter than 48000"):
            VERIFY.verify(resign(receipt), run)

    def test_a_fault_applied_at_the_wrong_call_site_is_a_mismatch(self):
        # Re-review probe3: a loss fault whose applied point is a stall.
        run = self.copy_run(self.good)
        events = (run / "events.jsonl").read_text(encoding="utf-8").replace(
            gfc.FAULT_POINTS["force_loss_between"], "process_events_stall")
        (run / "events.jsonl").write_text(events, encoding="utf-8")
        evidence = gfc.evaluate_run(run, fault="force_loss_between", fault_sample=FAULT,
                                    tolerance=1e-4, window_pre=PRE, window_post=POST,
                                    host_alive=True, bounce_path=run / "bounce.wav",
                      realtime_path=run / "realtime.wav")
        self.assertEqual(evidence["verdict"], "fail")
        self.assertIn("applied at", evidence["verdict_reasons"][0])

    def test_foreign_input_in_the_capture_is_rejected(self):
        # M6: plant "microphone" audio into the captured input of one block.
        # The output still nulls against its own input, so only the input check
        # can tell; the verifier must refuse the receipt.
        run = self.copy_run(self.good)
        cap = gfc.read_capture(run / "capture.bin")
        blocks = []
        for r in cap.records:
            start, n = r["stream"], r["frames"]
            inputs = [list(cap.inputs[c][start:start + n]) for c in range(cap.channels)]
            if r["position"] == FAULT - FAULT % 128:
                inputs[1][5] += 0.25  # a foreign sample on the second channel
            flags = ((gfc.FLAG_PLAYING if r["position"] >= 0 else 0) |
                     ((gfc.FLAG_SEGMENT_START | (1 << gfc.PREPARE_INDEX_SHIFT))
                      if r["segment_start"] else 0))
            blocks.append({"position": None if r["position"] < 0 else r["position"],
                           "flags": flags, "inputs": inputs,
                           "outputs": [list(cap.outputs[c][start:start + n])
                                       for c in range(cap.channels)],
                           "counters": (r["gpu"], r["fallback"], r["silence"], r["priming"]),
                           "host_time_ns": r["host_time_ns"]})
        gfc.write_capture(run / "capture.bin", 48000, blocks)
        receipt = load(run)
        receipt["artifacts"]["capture.bin"] = gfc.sha256_file(run / "capture.bin")
        with self.assertRaisesRegex(VERIFY.ReceiptError, "not the bound stimulus"):
            self.verify(resign(receipt), run)
        # Positive control: the untouched capture is accepted.
        self.assertEqual(self.verify(load(self.good), self.good), "pass")

    def test_foreign_input_while_stopped_is_rejected(self):
        # M6 covers the whole run: a microphone heard while the transport is
        # stopped is still host input in the capture.
        cap = gfc.read_capture(self.good / "capture.bin")
        stopped = cap.positions.index(-1)
        cap.inputs[0][stopped] = 0.01
        _, stim = gfc.read_wav(self.good / "stimulus.wav")
        self.assertEqual(gfc.captured_input_matches_stimulus(cap, stim[0]), stopped)
        cap.inputs[0][stopped] = 0.0
        self.assertIsNone(gfc.captured_input_matches_stimulus(cap, stim[0]))

    def test_wpk_disposition_view(self):
        cap = gfc.read_capture(self.good / "capture.bin")
        d = gfc.delivery_disposition(cap, FAULT, FAULT + 128, 2, fault="force_loss_between",
                                     window_post=POST)
        self.assertEqual(d["pre_window_gpu_share"], 1.0)
        self.assertTrue(d["gpu_frozen_after_applied"])
        self.assertTrue(d["all_fallback_after"])
        self.assertEqual(d["silence_blocks_delta"], 0)
        self.assertLessEqual(d["first_fallback_quanta_after_applied"], d["lead_blocks"] + 1)

    def test_a_fault_on_another_device_is_a_mismatch(self):
        # S1: the applied fault must name this plug-in's provider device.
        run = self.copy_run(self.good)
        events = (run / "events.jsonl").read_text(encoding="utf-8")
        lines = [json.loads(line) for line in events.splitlines() if line.strip()]
        for event in lines:
            if event.get("event") == "provider_fault_applied":
                event["device"] = "0xdeadbeef"
        (run / "events.jsonl").write_text("\n".join(json.dumps(e) for e in lines) + "\n",
                                          encoding="utf-8")
        evidence = gfc.evaluate_run(run, fault="force_loss_between", fault_sample=FAULT,
                                    tolerance=1e-4, window_pre=PRE, window_post=POST,
                                    host_alive=True, bounce_path=run / "bounce.wav",
                                    realtime_path=run / "realtime.wav")
        self.assertEqual(evidence["verdict"], "fail")
        self.assertIn("not this plug-in's provider", evidence["verdict_reasons"][0])

    def test_a_claim_won_by_another_process_is_inconclusive(self):
        # M5: a scanner that won the claim means the test did not run in the
        # intended process; INCONCLUSIVE (unbound), not FAIL.
        run = self.copy_run(self.good)
        (run / "claim").write_text("99999\n1\n", encoding="utf-8")
        evidence = gfc.evaluate_run(run, fault="force_loss_between", fault_sample=FAULT,
                                    tolerance=1e-4, window_pre=PRE, window_post=POST,
                                    host_alive=True, bounce_path=run / "bounce.wav",
                                    realtime_path=run / "realtime.wav")
        self.assertEqual(evidence["verdict"], "inconclusive")
        self.assertTrue(any(r.startswith("unbound") for r in evidence["verdict_reasons"]))
        # Positive control: the claim naming the preparing process passes.
        (run / "claim").write_text("0\n1\n", encoding="utf-8")
        evidence = gfc.evaluate_run(run, fault="force_loss_between", fault_sample=FAULT,
                                    tolerance=1e-4, window_pre=PRE, window_post=POST,
                                    host_alive=True, bounce_path=run / "bounce.wav",
                                    realtime_path=run / "realtime.wav")
        self.assertEqual(evidence["verdict"], "pass")

    def test_a_foreign_nonce_is_a_mismatch(self):
        receipt = load(self.good)
        receipt["nonce"] = "f" * 32
        with self.assertRaisesRegex(VERIFY.ReceiptError, "re-derive"):
            self.verify(resign(receipt), self.good)

    def test_a_fabricated_control_op_is_rejected(self):
        receipt = load(self.good)
        receipt["trigger_transport"] = "control_op"
        receipt["control_op"] = {"operation": "gpu_audio_fault_inject", "response": {}}
        with self.assertRaisesRegex(VERIFY.ReceiptError, "reaper_version|reaper_pid|reaper.out.txt"):
            VERIFY.verify(resign(receipt), self.good, require_control_op=True)

    def test_cli_exit_codes(self):
        self.assertEqual(VERIFY.main([str(self.good / "receipt.json"), "--allow-dry-run"]), 0)
        self.assertEqual(VERIFY.main([str(self.bad / "receipt.json"), "--allow-dry-run"]), 1)
        self.assertEqual(VERIFY.main([str(self.good / "receipt.json")]), 2)


def live_receipt(run: Path, *, quiet: bool, realtime_seconds: float | None = None,
                 realtime_copy_of_bounce: bool = False) -> dict:
    """A receipt shaped like a live REAPER run, built from a synthetic run the
    same way the harness builds one (the 1 s live window floor is patched down
    by the caller to keep the pure-Python reference fast)."""
    nonce = "n" * 32
    gfc.synthesize_run(run, fault="force_loss_after", fault_sample=FAULT, frames=FRAMES,
                       tail=POST, nonce=nonce, pid=4242)
    (run / "preflight.json").write_text(json.dumps(
        {"status": "passed" if quiet else "blocked", "quiet_host": quiet,
         "reasons": [] if quiet else ["load_above_quiet_threshold"]}), encoding="utf-8")
    (run / "reaper.out.txt").write_text("CLAP: initialized 'Pulp GPU Fault Continuity'\n",
                                        encoding="utf-8")
    (run / "claim").write_text("4242\n1\n", encoding="utf-8")
    render_frames = FRAMES + POST
    (run / "realtime_render.json").unlink()
    if realtime_seconds is not None:
        (run / "realtime_render.json").write_text(json.dumps(
            {"artifact": "realtime.wav", "mode": "realtime", "host": "reaper",
             "sample_rate": 48000, "started_unix_us": 0,
             "finished_unix_us": int(realtime_seconds * 1e6)}), encoding="utf-8")
    if realtime_copy_of_bounce:
        (run / "realtime.wav").write_bytes((run / "bounce.wav").read_bytes())
    names = ["stimulus.wav", "ir.f32", "events.jsonl", "capture.bin", "preflight.json",
             "reaper.out.txt", "claim", "realtime.wav", "realtime_render.json", "bounce.wav"]
    artifacts = gfc.artifact_digests(run, names)
    summary = gfc.summarize_preflight(json.loads((run / "preflight.json").read_text(encoding="utf-8")))
    evidence = gfc.evaluate_run(run, fault="force_loss_after", fault_sample=FAULT,
                                tolerance=1e-4, window_pre=PRE, window_post=POST,
                                host_alive=True, bounce_path=run / "bounce.wav",
                                render_frames=render_frames, nonce=nonce, reaper_pid=4242,
                                listed=list(artifacts), realtime_path=run / "realtime.wav",
                                host_quiet=summary["quiet_host"])
    return gfc.build_receipt(
        trigger_transport="in_process_file", control_op=None, nonce=nonce,
        analysis={"tolerance": 1e-4, "render_frames": render_frames,
                  "window": {"pre": PRE, "post": POST}},
        bounce_artifact="bounce.wav", host_realtime_artifact="realtime.wav",
        preflight=summary, host_quiet=summary["quiet_host"], artifacts=artifacts,
        host={"reaper_version": "7.82", "reaper_pid": 4242},
        plugin={"format": "clap", "name": "Pulp GPU Fault Continuity",
                "executable_sha256": "0" * 64}, **evidence)


class LiveRules(unittest.TestCase):
    """Rules that apply only to live receipts: the quiet-host rule and the
    realtime render's provenance."""

    def setUp(self):
        patcher = mock.patch.object(gfc, "MIN_WINDOW_POST", POST)
        patcher.start()
        self.addCleanup(patcher.stop)

    def verify(self, quiet=True, **kw):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            receipt = live_receipt(run, quiet=quiet, **kw)
            return VERIFY.verify(receipt, run), receipt

    def test_a_quiet_live_run_passes(self):
        self.assertEqual(self.verify(quiet=True, realtime_seconds=(FRAMES + POST) / 48000)[0],
                         "pass")

    def test_a_busy_host_is_inconclusive_never_pass(self):
        verdict, receipt = self.verify(quiet=False, realtime_seconds=(FRAMES + POST) / 48000)
        self.assertEqual(verdict, "inconclusive")
        self.assertTrue(any(r.startswith("host_not_quiet") for r in receipt["verdict_reasons"]))

    def test_a_render_faster_than_realtime_is_not_realtime_evidence(self):
        verdict, receipt = self.verify(realtime_seconds=0.1 * (FRAMES + POST) / 48000)
        self.assertEqual(verdict, "inconclusive")
        self.assertTrue(any("not a realtime pass" in r for r in receipt["verdict_reasons"]))

    def test_a_realtime_render_copied_from_the_bounce_is_not_evidence(self):
        verdict, receipt = self.verify(realtime_seconds=(FRAMES + POST) / 48000,
                                       realtime_copy_of_bounce=True)
        self.assertEqual(verdict, "inconclusive")
        self.assertTrue(any("byte-identical" in r for r in receipt["verdict_reasons"]))

    def test_a_live_receipt_needs_realtime_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            receipt = live_receipt(run, quiet=True)
            with self.assertRaisesRegex(VERIFY.ReceiptError, "realtime_render.json not listed"):
                VERIFY.verify(receipt, run)


class Decisions(unittest.TestCase):
    """The verdict rules, each with its passing counterpart."""

    OK = {"passed": True, "max_abs_error": 0.0, "first_deviating_sample": None,
          "last_deviating_sample": None}

    def decide(self, **kw):
        base = dict(fault="force_loss_before", plugin_null=self.OK, bounce_null=self.OK,
                    realtime_null=self.OK, delivery={"met": True, "degraded_host": False},
                    health_ok=True, fault_fired=True, host_alive=True, realtime_at_fault=True)
        base.update(kw)
        return gfc.decide(**base)[0]

    def test_all_checks_pass(self):
        self.assertEqual(self.decide(), "pass")

    def test_a_host_crash_is_a_fail_not_an_inconclusive(self):
        self.assertEqual(self.decide(host_alive=False, fault="render_force_loss"), "fail")

    def test_a_failed_null_is_a_fail_even_with_a_failed_precondition(self):
        broken = {**self.OK, "passed": False, "max_abs_error": 0.5}
        self.assertEqual(self.decide(plugin_null=broken, preconditions=["capture overflow"]),
                         "fail")
        self.assertEqual(self.decide(preconditions=["capture overflow"]), "inconclusive")

    def test_evidence_from_another_run_is_a_fail(self):
        self.assertEqual(self.decide(mismatches=["foreign nonce"]), "fail")

    def test_a_missing_realtime_render_is_inconclusive(self):
        self.assertEqual(self.decide(realtime_null=None), "inconclusive")

    def test_a_failed_realtime_render_null_is_a_fail(self):
        broken = {**self.OK, "passed": False, "max_abs_error": 0.5}
        self.assertEqual(self.decide(realtime_null=broken), "fail")

    def test_unmet_delivery_requirements_are_inconclusive(self):
        self.assertEqual(self.decide(delivery={"met": False, "note": "x"}), "inconclusive")

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

    def test_loss_needs_fallback_and_frozen_gpu_delivery(self):
        def snaps(at_gpu, after_gpu, fallback, reason="none"):
            out = self.snaps({"fallback_blocks": fallback, "recovery_reason": reason})
            out["before"]["delivery"] = {"gpu_blocks": 5}
            out["at"] = {**out["at"], "delivery": {"gpu_blocks": at_gpu}}
            out["after"]["delivery"] = {"gpu_blocks": after_gpu}
            return out
        self.assertTrue(gfc.health_expectation("force_loss_after", snaps(7, 7, 4))[0])
        # Still delivering after the "loss": the loss never landed.
        self.assertFalse(gfc.health_expectation("force_loss_after", snaps(7, 30, 4))[0])
        # Nothing covered the stream.
        self.assertFalse(gfc.health_expectation("force_loss_after", snaps(7, 7, 0))[0])
        # S3: naming the loss does not excuse a GPU that kept delivering.
        self.assertFalse(gfc.health_expectation(
            "force_loss_after", snaps(7, 9, 4, "provider-lost"))[0])

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
        snaps["before"] = {**snaps["before"], "delivery": {"gpu_blocks": 3}}
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
            self.assertEqual(cut.truncated_tail_bytes, gfc.CAPTURE_RECORD.size + 2 * 2 * 2 * 4 - 3)

    def test_a_reprepare_restarts_the_reference(self):
        ir = [1.0, 0.5]
        cap = gfc.Capture(48000, 1, [[1.0, 0.0, 1.0, 0.0]], [[0.0] * 4], [-1] * 4, [0, 2],
                          segment_prepares=[1, 2])
        self.assertEqual(gfc.plugin_reference(cap, ir, [0, 0])[0], [1.0, 0.5, 1.0, 0.5])
        self.assertEqual(gfc.plugin_reference(cap, ir, [1, 1])[0], [0.0, 1.0, 0.0, 1.0])
        # Each segment uses the PDC its own prepare reported.
        self.assertEqual(gfc.plugin_reference(cap, ir, [0, 1])[0], [1.0, 0.5, 0.0, 1.0])
        with self.assertRaisesRegex(ValueError, "prepared events"):
            gfc.plugin_reference(cap, ir, [0])

    def test_a_segment_pairs_with_the_prepare_it_follows(self):
        # The VST3 adapter prepares several times before processing again; only
        # the last of those prepares owns the audio that follows.
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            gfc.synthesize_run(run, fault="worker_stall", fault_sample=FAULT, frames=FRAMES, tail=POST)
            events = (run / "events.jsonl").read_text(encoding="utf-8")
            stale = json.dumps({"event": "prepared", "ok": True, "pdc_samples": 999,
                                "path": "shared_memory", "ir_taps": gfc.IR_TAPS,
                                "nonce": "dry-run", "pid": 0, "prepare_index": 0,
                                "provider_device": "0x1d2e3f"})
            (run / "events.jsonl").write_text(stale + "\n" + events, encoding="utf-8")
            kw = dict(fault="worker_stall", fault_sample=FAULT, tolerance=1e-4, window_pre=PRE,
                      window_post=POST, host_alive=True, bounce_path=run / "bounce.wav",
                      realtime_path=run / "realtime.wav")
            self.assertEqual(gfc.evaluate_run(run, **kw)["verdict"], "pass")
            # Positive control for the pairing: point the segment at the stale
            # prepare and the null fails on its wrong PDC.
            events = (run / "events.jsonl").read_text(encoding="utf-8")
            (run / "events.jsonl").write_text(
                events.replace('"prepare_index": 0', '"prepare_index": 9')
                      .replace('"prepare_index": 1', '"prepare_index": 0')
                      .replace('"prepare_index": 9', '"prepare_index": 1'), encoding="utf-8")
            self.assertEqual(gfc.evaluate_run(run, **kw)["verdict"], "fail")

    def test_non_finite_output_fails_and_stays_json_safe(self):
        result = gfc.null_test([[0.0, float("nan")]], [[0.0, 0.0]], 0, tolerance=1e-4)
        self.assertFalse(result.passed)
        self.assertIsNone(result.as_dict()["max_abs_error"])
        json.dumps(result.as_dict(), allow_nan=False)


if __name__ == "__main__":
    unittest.main()
