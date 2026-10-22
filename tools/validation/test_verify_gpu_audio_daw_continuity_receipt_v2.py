#!/usr/bin/env python3
"""Self-test for the WP-K v2 DAW-continuity receipt verifier.

Runs are synthesized with WP-h1's `synthesize_run` and then given what a
non-REAPER host run adds: the plug-in's `host_process` event and `claim` file,
the control kind's request fields, a host realtime render, host evidence.
Each rejection is paired with the receipt it was derived from, and every
success rule has a planted negative.

WP-h1's per-block delivery analysis (`delivery_disposition`, Fable M2) is
stubbed here with the shape the v2 evaluation consumes; with the library's
own implementation absent, a fault cell is INCONCLUSIVE (tested below).
"""
from __future__ import annotations

import copy
import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "verify_gpu_audio_daw_continuity_receipt_v2",
    HERE / "verify_gpu_audio_daw_continuity_receipt_v2.py")
V2 = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = V2
SPEC.loader.exec_module(V2)
gfc, hosts = V2.gfc, V2.hosts

FRAMES, FAULT, PRE, POST = 9600, 3000, 1000, 4800
NONCE = "0123456789abcdef0123456789abcdef"
HOST_PID, HOST_RESPONSIBLE, PLUGIN_PID = 4242, 4242, 5151
LAUNCH, START, CLAIMED = 1_800_000_000_000_000, 1_800_000_001_000_000, 1_800_000_002_000_000
LOGIC = "/Applications/Logic Pro.app/Contents/MacOS/Logic Pro"
AUHOSTING = ("/System/Library/Frameworks/AudioToolbox.framework/XPCServices/"
             "AUHostingServiceXPC_arrow.xpc/Contents/MacOS/AUHostingServiceXPC_arrow")
NAME = "Pulp GPU Fault Continuity"
GOOD_DISPOSITION = {"pre_window_gpu_share": 1.0, "pre_window_quanta": 96,
                    "gpu_frozen_after_applied": True, "all_fallback_after": True,
                    "silence_blocks_delta": 0, "first_fallback_quanta_after_applied": 1,
                    "lead_blocks": 2}


def _rewrite_events(run: Path, edit) -> None:
    events = edit(gfc.load_events(run / "events.jsonl"))
    with (run / "events.jsonl").open("w", encoding="utf-8") as stream:
        for event in events:
            stream.write(json.dumps(event, sort_keys=True) + "\n")


def make_run(run: Path, *, kind: str = "fault", fault: str = "force_loss_before",
             mode: str = "auhostingservice_arrow", break_fallback: bool = False,
             process_path: str = AUHOSTING, responsible: int = HOST_RESPONSIBLE,
             plugin_pid: int = PLUGIN_PID, planted: str | None = None,
             host_id: str = "logic", fmt: str = "au", realtime: bool = True,
             start: int = START, claim_pid: int | None = None,
             responsible_path: str | None = None) -> dict:
    """Synthesize one run directory and its v2 receipt."""
    synth_fault = "render_force_loss" if fault == hosts.POSITIVE_CONTROL else fault
    gfc.synthesize_run(run, fault=synth_fault, fault_sample=FAULT, frames=FRAMES,
                       break_fallback=break_fallback, nonce=NONCE, pid=plugin_pid, tail=POST,
                       window_post=POST)
    if planted is None and kind == "planted_negative":
        planted = "no_fallback"

    def edit(events):
        out = [{"event": "host_process", "pid": plugin_pid, "ppid": 1,
                "responsible_pid": responsible,
                "responsible_path": responsible_path if responsible_path is not None else
                (GARAGEBAND if host_id == "garageband" else LOGIC),
                "sandboxed": 1 if "AUHosting" in process_path else 0,
                "start_unix_us": start, "claimed_unix_us": CLAIMED,
                "process_path": process_path, "config_source": "rendezvous",
                "nonce": NONCE}]
        for event in events:
            if event["event"] == "render_loss" and fault == hosts.POSITIVE_CONTROL:
                continue
            if event["event"] in ("fault_armed", "fault_fired"):
                event = dict(event, fault=fault)
            if event["event"] == "fault_armed":
                event = dict(event, planted_negative=planted)
            if event["event"] == "prepared" and planted == "gpu_off":
                event = dict(event, path="staged")
            out.append(event)
        return out

    _rewrite_events(run, edit)
    (run / "claim").write_text(f"{claim_pid or plugin_pid}\n{CLAIMED}\n", encoding="utf-8")
    (run / "preflight.json").write_text(json.dumps(
        {"status": "ok", "quiet_host": False, "reasons": ["load"]}) + "\n", encoding="utf-8")
    write_host_evidence(run, host_id)
    if realtime:
        write_realtime_from(run / "bounce.wav", run / "realtime.wav")
    return receipt_for(run, kind=kind, fault=fault, mode=mode, host_id=host_id, fmt=fmt,
                       realtime=realtime)


GARAGEBAND = "/Applications/GarageBand.app/Contents/MacOS/GarageBand"


def write_host_evidence(run: Path, host_id: str = "logic", plugin: str = NAME,
                        pid: int = HOST_PID) -> None:
    (run / "host_evidence.json").write_text(json.dumps(
        {"plugins": [plugin], "host_pid": pid,
         "host_executable": GARAGEBAND if host_id == "garageband" else LOGIC}) + "\n",
        encoding="utf-8")


def write_realtime_from(source: Path, target: Path) -> None:
    """The host's realtime render of the fault pass. A real one differs from
    the offline bounce in rounding (GPU blocks before the fault); one ulp-scale
    change keeps it a distinct artifact that still nulls."""
    _, channels = gfc.read_wav(source)
    channels = [list(ch) for ch in channels]
    channels[0][0] += 1e-7
    gfc.write_wav_f32(target, channels)


def host_block(host_id: str = "logic") -> dict:
    return {"id": host_id, "version": "12.4", "pid": HOST_PID,
            "responsible_pid": HOST_RESPONSIBLE,
            "executable": GARAGEBAND if host_id == "garageband" else LOGIC,
            "launch_unix_us": LAUNCH, "binary_arch": "x86_64-rosetta", "launch": "open -g"}


def receipt_for(run: Path, *, kind: str, fault: str, mode: str, host_id: str = "logic",
                fmt: str = "au", realtime: bool = True) -> dict:
    host = host_block(host_id)
    names = ["stimulus.wav", "ir.f32", "events.jsonl", "capture.bin", "preflight.json", "claim",
             "host_evidence.json", "bounce.wav"] + (["realtime.wav"] if realtime else [])
    artifacts = gfc.artifact_digests(run, names)
    evidence = hosts.evaluate_v2(
        run, control_kind=kind, fault=fault, fault_sample=FAULT, tolerance=gfc.DEFAULT_TOLERANCE,
        window_pre=PRE, window_post=POST, host_alive=True, bounce_path=run / "bounce.wav",
        realtime_path=(run / "realtime.wav") if realtime else None,
        render_frames=FRAMES + POST, nonce=NONCE, host=host, hosting_mode=mode,
        listed=list(artifacts))
    preflight = gfc.summarize_preflight(
        json.loads((run / "preflight.json").read_text(encoding="utf-8")))
    receipt = hosts.build_receipt_v2(
        control_kind=kind, trigger_transport="dry_run", nonce=NONCE, host=host,
        hosting_mode=mode, host_settings={"process_buffer_range": "medium"},
        plugin={"format": fmt, "name": NAME, "executable_sha256": "a" * 64},
        code_under_test={"commit": "b" * 40}, artifacts=artifacts,
        bounce_artifact="bounce.wav",
        host_realtime_artifact="realtime.wav" if realtime else None,
        preflight=preflight, host_quiet=preflight["quiet_host"],
        analysis={"tolerance": gfc.DEFAULT_TOLERANCE, "window": {"pre": PRE, "post": POST},
                  "render_frames": FRAMES + POST},
        column_outcome=hosts.column_outcome(evidence, control_kind=kind, host_id=host_id),
        **{k: evidence[k] for k in V2.RECOMPUTED})
    (run / "receipt.json").write_text(json.dumps(receipt, indent=1), encoding="utf-8")
    return receipt


def resign(receipt: dict) -> dict:
    receipt["receipt_sha256"] = gfc.receipt_digest(receipt)
    return receipt


class V2Receipt(unittest.TestCase):
    disposition = GOOD_DISPOSITION

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self._had = hasattr(gfc, "delivery_disposition")
        self._saved = getattr(gfc, "delivery_disposition", None)
        gfc.delivery_disposition = lambda *a, **k: dict(self.disposition)

    def tearDown(self):
        if self._had:
            gfc.delivery_disposition = self._saved
        else:
            del gfc.delivery_disposition
        self._tmp.cleanup()

    def run_dir(self, name: str) -> Path:
        path = self.root / name
        path.mkdir()
        return path

    def verify(self, receipt: dict, run: Path) -> tuple[str, str]:
        return V2.verify(receipt, run, allow_dry_run=True)

    def main(self, run: Path) -> int:
        return V2.main([str(run / "receipt.json"), "--allow-dry-run"])

    # ── Fault rows ─────────────────────────────────────────────────────

    def test_a_fault_row_in_auhostingservice_verifies_as_pass(self):
        run = self.run_dir("fault")
        receipt = make_run(run)
        self.assertEqual(self.verify(receipt, run), ("pass", "pass"))
        self.assertEqual(receipt["binding"]["outcome"], "bound")
        self.assertEqual(self.main(run), V2.EXIT_OK)

    def test_every_fault_kind_passes_in_process(self):
        for fault in gfc.FAULT_KINDS:
            with self.subTest(fault=fault):
                run = self.run_dir(f"inproc-{fault}")
                receipt = make_run(run, fault=fault, mode="in_process", plugin_pid=HOST_PID,
                                   process_path=LOGIC)
                self.assertEqual(self.verify(receipt, run), ("pass", "pass"))

    def test_m1_no_host_realtime_render_is_inconclusive(self):
        run = self.run_dir("no-realtime")
        receipt = make_run(run, realtime=False)
        self.assertEqual(self.verify(receipt, run), ("inconclusive", "inconclusive"))
        self.assertIn("no host realtime render of the fault pass (M1)", receipt["verdict_reasons"])

    def test_m1_the_bounce_cannot_stand_in_for_the_realtime_render(self):
        run = self.run_dir("realtime-is-bounce")
        make_run(run)
        receipt = receipt_for(run, kind="fault", fault="force_loss_before",
                              mode="auhostingservice_arrow")
        receipt["host_realtime_artifact"] = "bounce.wav"
        with self.assertRaisesRegex(V2.ReceiptError, "re-derive"):
            self.verify(resign(receipt), run)
        shutil.copyfile(run / "bounce.wav", run / "realtime.wav")
        receipt = receipt_for(run, kind="fault", fault="force_loss_before",
                              mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt, run)[0], "inconclusive")
        self.assertIn("the host realtime render is the offline bounce itself",
                      receipt["verdict_reasons"])

    def test_m1_a_failing_host_realtime_render_is_a_fail(self):
        run = self.run_dir("realtime-broken")
        make_run(run)
        bad = self.run_dir("bad-source")
        gfc.synthesize_run(bad, fault="force_loss_before", fault_sample=FAULT, frames=FRAMES,
                           break_fallback=True, nonce=NONCE, pid=PLUGIN_PID, tail=POST,
                           window_post=POST)
        write_realtime_from(bad / "bounce.wav", run / "realtime.wav")
        receipt = receipt_for(run, kind="fault", fault="force_loss_before",
                              mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt, run)[0], "fail")
        self.assertTrue(receipt["verdict_reasons"][0].startswith("host realtime render null"))

    def test_garageband_is_capped_at_plugin_capture_only(self):
        run = self.run_dir("garageband")
        receipt = make_run(run, host_id="garageband", realtime=False, mode="in_process",
                           plugin_pid=HOST_PID, process_path=GARAGEBAND)
        self.assertEqual(self.verify(receipt, run), ("pass", "pass_plugin_capture_only"))
        self.assertEqual(self.main(run), V2.EXIT_NOT_OK)

    def test_logic_evidence_in_a_garageband_run_is_another_daw(self):
        run = self.run_dir("garageband-logic")
        receipt = make_run(run, host_id="garageband", realtime=False, mode="in_process",
                           plugin_pid=HOST_PID, process_path=LOGIC)
        self.assertEqual(receipt["binding"]["outcome"], "wrong_daw")
        self.assertEqual(self.verify(receipt, run)[0], "fail")

    # ── M2: GPU in the loop ────────────────────────────────────────────

    def test_m2_without_the_delivery_analysis_a_fault_cell_is_inconclusive(self):
        del gfc.delivery_disposition
        try:
            run = self.run_dir("no-disposition")
            receipt = make_run(run)
            self.assertEqual(self.verify(receipt, run)[0], "inconclusive")
            self.assertTrue(any("per-block delivery counters" in r
                                for r in receipt["verdict_reasons"]))
        finally:
            gfc.delivery_disposition = lambda *a, **k: dict(self.disposition)

    def test_m2_delivery_bar(self):
        cases = {
            "share 0.95": ({"pre_window_gpu_share": 0.95}, "pass", "pass"),
            "share 0.7 is a degraded host": ({"pre_window_gpu_share": 0.7}, "pass",
                                             "pass_degraded_host"),
            "share 0.4 is void": ({"pre_window_gpu_share": 0.4}, "inconclusive", "inconclusive"),
            "GPU kept delivering": ({"gpu_frozen_after_applied": False}, "inconclusive",
                                    "inconclusive"),
            "a non-fallback block after": ({"all_fallback_after": False}, "inconclusive",
                                           "inconclusive"),
            "silence delivered": ({"silence_blocks_delta": 2}, "inconclusive", "inconclusive"),
            "late takeover": ({"first_fallback_quanta_after_applied": 4}, "inconclusive",
                              "inconclusive"),
        }
        for label, (change, verdict, outcome) in cases.items():
            with self.subTest(label):
                self.disposition = dict(GOOD_DISPOSITION, **change)
                run = self.run_dir(label.replace(" ", "-"))
                receipt = make_run(run)
                self.assertEqual(self.verify(receipt, run), (verdict, outcome))
        self.disposition = GOOD_DISPOSITION

    # ── Controls ───────────────────────────────────────────────────────

    def test_the_positive_control_passes_and_is_not_a_fault_pass(self):
        run = self.run_dir("control")
        receipt = make_run(run, kind="positive_control", fault=hosts.POSITIVE_CONTROL)
        self.assertEqual(self.verify(receipt, run), ("pass", "control_passed"))

    def test_a_positive_control_where_the_gpu_did_not_carry_the_stream_fails_the_column(self):
        run = self.run_dir("control-fallback")
        make_run(run, kind="positive_control", fault=hosts.POSITIVE_CONTROL)

        def starve(events):
            for e in events:
                if e["event"] == "health" and e["phase"] == "after":
                    e["status"]["fallback_blocks"] = 300
            return events

        _rewrite_events(run, starve)
        receipt = receipt_for(run, kind="positive_control", fault=hosts.POSITIVE_CONTROL,
                              mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt, run)[1], "control_inconclusive")
        self.assertEqual(self.main(run), V2.EXIT_NOT_OK)

    def test_a_provider_fault_applied_during_the_control_is_a_fail(self):
        run = self.run_dir("control-applied")
        make_run(run, kind="positive_control", fault=hosts.POSITIVE_CONTROL)
        _rewrite_events(run, lambda ev: ev + [{"event": "provider_fault_applied",
                                               "point": "before_submit"}])
        receipt = receipt_for(run, kind="positive_control", fault=hosts.POSITIVE_CONTROL,
                              mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt, run), ("fail", "control_fail"))

    def test_the_planted_dropout_is_proved_by_a_fail_at_the_fault(self):
        run = self.run_dir("negative")
        receipt = make_run(run, kind="planted_negative", break_fallback=True)
        self.assertEqual(self.verify(receipt, run), ("fail", "negative_proved"))
        self.assertEqual(receipt["nulls"]["plugin"]["first_deviating_sample"], FAULT)
        self.assertEqual(self.main(run), V2.EXIT_OK)

    def test_a_planted_negative_that_deviates_before_the_fault_is_not_proved(self):
        # A FAIL that starts before the fault is a broken run, not the
        # fallback being corrupted: it proves nothing about the negative.
        run = self.run_dir("negative-early")
        make_run(run, kind="planted_negative", break_fallback=True)
        cap = gfc.read_capture(run / "capture.bin")
        early = cap.positions.index(FAULT - 500)
        blocks, block = [], 128
        for start in range(0, cap.frames, block):
            end = min(start + block, cap.frames)
            pos = cap.positions[start]
            flags = (gfc.FLAG_PLAYING if pos >= 0 else 0) | (
                (gfc.FLAG_SEGMENT_START | (1 << gfc.PREPARE_INDEX_SHIFT)) if start == 0 else 0)
            if start in cap.fault_block_starts:
                flags |= gfc.FLAG_FAULT_BLOCK
            outs = [[0.0 if early <= start + i < early + 64 else v
                     for i, v in enumerate(ch[start:end])] for ch in cap.outputs]
            blocks.append({"position": pos if pos >= 0 else None, "flags": flags,
                           "inputs": [ch[start:end] for ch in cap.inputs], "outputs": outs})
        gfc.write_capture(run / "capture.bin", cap.sample_rate, blocks)
        receipt = receipt_for(run, kind="planted_negative", fault="force_loss_before",
                              mode="auhostingservice_arrow")
        self.assertEqual(receipt["nulls"]["plugin"]["first_deviating_sample"], FAULT - 500)
        self.assertEqual(self.verify(receipt, run), ("fail", "negative_not_proved"))

    def test_a_planted_negative_that_never_fired_proves_nothing(self):
        run = self.run_dir("negative-never-fired")
        make_run(run, kind="planted_negative", break_fallback=True)
        _rewrite_events(run, lambda ev: [e for e in ev if e["event"] not in (
            "fault_fired", "provider_fault_applied")])
        receipt = receipt_for(run, kind="planted_negative", fault="force_loss_before",
                              mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt, run), ("fail", "negative_not_proved"))

    def test_a_planted_negative_in_an_unbound_process_proves_nothing(self):
        run = self.run_dir("negative-unbound")
        receipt = make_run(run, kind="planted_negative", break_fallback=True, responsible=999)
        self.assertEqual(self.verify(receipt, run), ("fail", "negative_not_proved"))

    def test_a_planted_negative_that_passes_means_the_harness_has_no_teeth(self):
        run = self.run_dir("negative-passes")
        receipt = make_run(run, kind="planted_negative", break_fallback=False)
        self.assertEqual(self.verify(receipt, run)[1], "negative_not_proved")
        self.assertEqual(self.main(run), V2.EXIT_NOT_OK)

    def test_gpu_off_is_proved_by_an_inconclusive_on_the_path(self):
        run = self.run_dir("gpu-off")
        receipt = make_run(run, kind="planted_negative", planted="gpu_off")
        self.assertEqual(self.verify(receipt, run), ("inconclusive", "negative_proved"))
        run2 = self.run_dir("gpu-off-but-shared")
        make_run(run2, kind="planted_negative", planted="gpu_off")
        # On the shared path and unbound: INCONCLUSIVE, but not for the path.
        _rewrite_events(run2, lambda ev: [dict(e, path="shared_memory")
                                          if e["event"] == "prepared" else e for e in ev
                                          if e["event"] != "host_process"])
        receipt2 = receipt_for(run2, kind="planted_negative", fault="force_loss_before",
                               mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt2, run2), ("inconclusive", "negative_not_proved"))

    def test_a_planted_negative_must_not_ride_on_a_fault_row(self):
        run = self.run_dir("planted-in-fault")
        receipt = make_run(run, kind="fault", planted="no_fallback")
        self.assertEqual(self.verify(receipt, run)[0], "fail")
        self.assertTrue(any("planted_negative" in r for r in receipt["verdict_reasons"]))

    # ── M5: process binding ────────────────────────────────────────────

    def test_binding_records_each_conjunct(self):
        run = self.run_dir("bound")
        receipt = make_run(run)
        self.assertEqual(receipt["binding"]["conjuncts"], {
            "nonce": "held", "claim_pid": "held", "process_path": "held", "start_time": "held",
            "responsible_pid": "held", "bitwig_owner": "not_applicable"})

    def test_an_unbound_process_is_inconclusive(self):
        cases = {
            "claim names another pid": dict(claim_pid=7777),
            "started before the host launched": dict(start=LAUNCH - 1),
            "declared arrow, ran in the plain service": dict(
                process_path=AUHOSTING.replace("_arrow", "")),
            "AUHostingService owned by someone else": dict(responsible=999),
            "in_process but another pid": dict(mode="in_process", process_path=LOGIC),
        }
        for label, kw in cases.items():
            with self.subTest(label):
                run = self.run_dir(label.replace(" ", "-"))
                receipt = make_run(run, **kw)
                self.assertEqual(receipt["binding"]["outcome"], "unbound")
                self.assertEqual(self.verify(receipt, run)[0], "inconclusive")

    def test_an_unavailable_responsible_pid_is_bindable_only_in_process(self):
        run = self.run_dir("no-responsible-inproc")
        receipt = make_run(run, responsible=-1, mode="in_process", plugin_pid=HOST_PID,
                           process_path=LOGIC)
        self.assertEqual(receipt["binding"]["conjuncts"]["responsible_pid"], "unavailable")
        self.assertEqual(receipt["binding"]["outcome"], "bound")
        # Out of process it is the only link back to the host, so it is required.
        run2 = self.run_dir("no-responsible-oop")
        receipt2 = make_run(run2, responsible=-1)
        self.assertEqual(receipt2["binding"]["outcome"], "unbound")
        self.assertEqual(self.verify(receipt2, run2)[0], "inconclusive")

    def test_an_out_of_process_plugin_serving_another_daw_is_a_fail(self):
        run = self.run_dir("oop-other-daw")
        receipt = make_run(run, responsible=777,
                           responsible_path="/Applications/REAPER.app/Contents/MacOS/REAPER")
        self.assertEqual(receipt["binding"]["outcome"], "wrong_daw")
        self.assertEqual(self.verify(receipt, run)[0], "fail")

    def test_evidence_from_another_daw_is_a_fail(self):
        run = self.run_dir("wrong-daw")
        receipt = make_run(run, mode="in_process", plugin_pid=HOST_PID,
                           process_path="/Applications/REAPER.app/Contents/MacOS/REAPER")
        self.assertEqual(receipt["binding"]["outcome"], "wrong_daw")
        self.assertEqual(self.verify(receipt, run)[0], "fail")

    def test_a_fail_still_shows_the_unmet_preconditions(self):
        run = self.run_dir("fail-with-preconditions")
        make_run(run, mode="in_process", plugin_pid=HOST_PID,
                 process_path="/Applications/REAPER.app/Contents/MacOS/REAPER")
        _rewrite_events(run, lambda ev: [dict(e, config_source="none")
                                         if e["event"] == "host_process" else e for e in ev])
        receipt = receipt_for(run, kind="fault", fault="force_loss_before", mode="in_process")
        self.assertEqual(self.verify(receipt, run)[0], "fail")
        self.assertIn("also: the plug-in had no run configuration (config_source='none')",
                      receipt["verdict_reasons"])

    def test_a_run_with_no_host_process_event_is_inconclusive(self):
        run = self.run_dir("no-host-process")
        make_run(run)
        _rewrite_events(run, lambda ev: [e for e in ev if e["event"] != "host_process"])
        receipt = receipt_for(run, kind="fault", fault="force_loss_before",
                              mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt, run)[0], "inconclusive")

    def test_an_unconfigured_plugin_is_inconclusive(self):
        run = self.run_dir("no-config")
        make_run(run)
        _rewrite_events(run, lambda ev: [dict(e, config_source="none")
                                         if e["event"] == "host_process" else e for e in ev])
        receipt = receipt_for(run, kind="fault", fault="force_loss_before",
                              mode="auhostingservice_arrow")
        self.assertEqual(self.verify(receipt, run)[0], "inconclusive")

    # ── Receipt integrity ──────────────────────────────────────────────

    def test_rejections(self):
        run = self.run_dir("base")
        good = make_run(run)
        cases = {
            "receipt_sha256": lambda r: r.update(host_alive=False),
            "column_outcome": lambda r: resign(r.update(column_outcome="negative_proved") or r),
            "re-derive": lambda r: resign(r.update(verdict="fail") or r),
            "does not load": lambda r: resign(r.update(plugin=dict(r["plugin"], format="vst3"))
                                              or r),
            "unknown hosting mode": lambda r: resign(r.update(hosting_mode="remote") or r),
            "required artifact claim": lambda r: resign(
                r.update(artifacts={k: v for k, v in r["artifacts"].items()
                                    if k != "claim"}) or r),
            "host realtime artifact not digest-bound": lambda r: resign(
                r.update(artifacts={k: v for k, v in r["artifacts"].items()
                                    if k != "realtime.wav"}) or r),
            "binding does not re-derive": lambda r: resign(
                r.update(binding=dict(r["binding"], outcome="wrong_daw")) or r),
        }
        for needle, mutate in cases.items():
            with self.subTest(needle):
                receipt = copy.deepcopy(good)
                mutate(receipt)
                with self.assertRaisesRegex(V2.ReceiptError, needle):
                    self.verify(receipt, run)
        self.assertEqual(self.verify(good, run), ("pass", "pass"))

    def test_a_live_receipt_needs_its_host_fields(self):
        run = self.run_dir("live")
        good = make_run(run)
        for field, needle in (("launch_unix_us", "launch_unix_us"), ("pid", "host.pid")):
            with self.subTest(field):
                receipt = copy.deepcopy(good)
                receipt["trigger_transport"] = "rendezvous_file"
                receipt["host"].pop(field)
                with self.assertRaisesRegex(V2.ReceiptError, needle):
                    V2.verify(resign(receipt), run)

    def test_a_live_receipt_needs_host_evidence_showing_the_plugin(self):
        run = self.run_dir("evidence")
        receipt = make_run(run)
        receipt["trigger_transport"] = "rendezvous_file"
        for kw, needle in ((dict(plugin="Channel EQ"), "never shows the plug-in"),
                           (dict(pid=HOST_PID + 1), "different host process")):
            with self.subTest(needle):
                write_host_evidence(run, **kw)
                receipt["artifacts"]["host_evidence.json"] = gfc.sha256_file(
                    run / "host_evidence.json")
                with self.assertRaisesRegex(V2.ReceiptError, needle):
                    V2.verify(resign(copy.deepcopy(receipt)), run)

    def test_a_live_receipt_needs_the_live_window_floor(self):
        run = self.run_dir("floor")
        receipt = make_run(run)
        receipt["trigger_transport"] = "rendezvous_file"
        with self.assertRaisesRegex(V2.ReceiptError, "post must be >="):
            V2.verify(resign(receipt), run)


class Pattern(unittest.TestCase):
    def _capture(self, times: list[int], frames: int = 512):
        cap = gfc.Capture(48000, 1, [[]], [[]], [], [0])
        cap.callbacks = [(t, frames, False) for t in times]
        return cap

    def test_paced_and_bursty_calling(self):
        period = int(512 * 1e9 / 48000)
        paced = self._capture([i * period for i in range(200)])
        self.assertEqual(hosts.calling_pattern(paced)["classification"], "paced")
        # Anticipative/prefetch shape: four back-to-back calls, then a wait.
        times, t = [], 0
        for i in range(200):
            times.append(t)
            t += 1000 if i % 4 != 3 else 4 * period
        bursty = hosts.calling_pattern(self._capture(times))
        self.assertEqual(bursty["classification"], "bursty")
        self.assertGreater(bursty["burst_fraction"], 0.5)
        self.assertEqual(hosts.calling_pattern(self._capture([0] * 50))["classification"],
                         "unknown")
        self.assertEqual(hosts.calling_pattern(gfc.Capture(48000, 1, [[]], [[]], [], [0]))
                         ["classification"], "unknown")


if __name__ == "__main__":
    unittest.main()
