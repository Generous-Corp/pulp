#!/usr/bin/env python3
"""Fail-closed verifier for `pulp.gpu-audio.daw-continuity.v2` receipts (WP-K).

v2 extends WP-h1's v1 receipt to hosts other than REAPER and to the WP-K
controls. It is verified the same way: every artifact digest must match, and
the evidence and verdict are recomputed from the raw capture, events, bounce,
stimulus and impulse response, never read from the receipt. On top of v1:

* the host block names the host, its version, pid and launch time, how the
  plug-in was hosted (in process, AUHostingService, a Bitwig plug-in host),
  and the plug-in's `host_process` event and `claim` file must bind to it
  (six recorded conjuncts; an unbound run is INCONCLUSIVE, evidence from
  another DAW is FAIL);
* where the host can render in realtime (REAPER, Logic, Bitwig), its own
  realtime render of the fault pass is required and nulled; GarageBand cells
  are capped at "pass_plugin_capture_only";
* a fault cell needs the per-block delivery bar: pre-fault GPU share >= 90 %
  (50-90 % is "pass_degraded_host", below that the cell is void), the GPU
  frozen and every block fallback after the applied fault, no silence;
* `control_kind` is one of fault, positive_control, planted_negative,
  offline_fault, and each has its own success: a fault or a positive control
  PASSes; a planted negative that corrupts the fallback (no_fallback,
  fallback_misaligned, fallback_stale) must FAIL from the fault sample on, and
  gpu_off must be INCONCLUSIVE on the shared-I/O path precondition; an
  offline-fault row is never GPU-continuity evidence;
* the code under test is named by commit.

`receipt_sha256` is an unkeyed digest of the receipt body: it detects
accidental edits and binds the body to the artifacts it lists, which are
re-derived here. It is integrity-only, not authenticity: anyone who can
rewrite every artifact and recompute the digests can produce a consistent
receipt. Nothing here should be read as a signature.

Exit codes: 0 verified and the row did what its kind requires without
qualification (fault `pass`, `control_passed`, `negative_proved`); 1 verified
but not that, including the qualified passes `pass_degraded_host` and
`pass_plugin_capture_only` (a legitimate finding, quoted with its
qualification); 2 invalid receipt.

What a fault-row PASS proves is exactly what v1's does, for the host, format,
hosting mode and settings this receipt names: the plug-in's output stayed
sample-exact against an independent CPU reference across one injected fault.
It does not prove behaviour under a real device loss, GPU resume, other nodes,
or anything Vellum-gated (render-owner loss propagation, resource replay,
teardown callback suppression).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]


def _load(name: str, path: Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# v1 loads WP-h1's library and registers it; everything below shares that one
# instance, so captures and receipts are the same types on both sides.
v1 = _load("verify_gpu_audio_daw_continuity_receipt",
           REPO / "tools/validation/verify_gpu_audio_daw_continuity_receipt.py")
gfc = v1.gfc
hosts = _load("gpu_fault_hosts", REPO / "tools/testing/daw-smoke/gpu_fault_hosts.py")

EXIT_OK, EXIT_NOT_OK, EXIT_INVALID = 0, 1, 2
SHA256 = re.compile(r"^[0-9a-f]{64}$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
REQUIRED_ARTIFACTS = ("stimulus.wav", "ir.f32", "events.jsonl", "capture.bin", "preflight.json",
                      "claim")
# A live run binds the host-side evidence that the plug-in was where the
# receipt says (an AX snapshot of the insert slot, or REAPER's log).
LIVE_ARTIFACTS = ("host_evidence.json",)
RECOMPUTED = ("fault", "pdc_samples", "plugin_identity", "binding", "plugin_process", "capture",
              "calling_pattern", "delivery", "controls", "offline_path", "nulls", "health",
              "host_alive", "verdict", "verdict_reasons", "planted_negative", "negative_proved")
FULL_OUTCOMES = ("pass", "control_passed", "negative_proved")

ReceiptError = v1.ReceiptError
_require = v1._require
_close = v1._close


def verify(receipt: dict[str, Any], run_dir: Path, *, allow_dry_run: bool = False,
           require_quiet_host: bool = False) -> tuple[str, str]:
    """Return (verdict, column outcome), or raise ReceiptError."""
    _require(receipt.get("schema") == hosts.SCHEMA_V2, f"schema is not {hosts.SCHEMA_V2}")
    _require(receipt.get("receipt_sha256") == gfc.receipt_digest(receipt),
             "receipt_sha256 does not match the receipt body")

    kind = receipt.get("control_kind")
    _require(kind in hosts.CONTROL_KINDS, f"unknown control kind {kind!r}")
    fault = receipt.get("fault") or {}
    fault_kind = fault.get("kind")
    allowed = gfc.FAULT_KINDS + (hosts.POSITIVE_CONTROL,)
    _require(fault_kind in allowed, f"unknown fault kind {fault_kind!r}")
    transport = receipt.get("trigger_transport")
    _require(transport in ("in_process_file", "rendezvous_file", "dry_run"),
             f"unknown trigger transport {transport!r}")
    live = transport != "dry_run"
    if not live:
        _require(allow_dry_run, "a dry-run receipt is not DAW evidence")

    host = receipt.get("host") or {}
    _require(host.get("id") in hosts.HOSTS, f"unknown host {host.get('id')!r}")
    _require(bool(host.get("version")), "host.version missing")
    mode = receipt.get("hosting_mode")
    _require(mode in hosts.HOSTING_MODES, f"unknown hosting mode {mode!r}")
    plugin = receipt.get("plugin") or {}
    _require(plugin.get("format") in hosts.HOST_FORMATS[host["id"]],
             f"{host['id']} does not load {plugin.get('format')!r} plug-ins")
    _require(bool(plugin.get("name")), "plugin.name missing")
    nonce = receipt.get("nonce")
    _require(isinstance(nonce, str) and len(nonce) >= 16, "run nonce missing")
    if live:
        _require(isinstance(host.get("pid"), int) and host["pid"] > 0, "host.pid missing")
        _require(isinstance(host.get("launch_unix_us"), int) and host["launch_unix_us"] > 0,
                 "host.launch_unix_us missing")
        _require(SHA256.match(str(plugin.get("executable_sha256", ""))) is not None,
                 "plugin.executable_sha256 missing")
        code = receipt.get("code_under_test") or {}
        _require(COMMIT.match(str(code.get("commit", ""))) is not None,
                 "code_under_test.commit missing")

    artifacts = receipt.get("artifacts")
    _require(isinstance(artifacts, dict), "artifacts map missing")
    for name in REQUIRED_ARTIFACTS + (LIVE_ARTIFACTS if live else ()):
        _require(name in artifacts, f"required artifact {name} not listed")
    for name, digest in artifacts.items():
        _require("/" not in name and name not in ("", ".", ".."),
                 f"artifact name {name!r} escapes the run dir")
        _require(SHA256.match(str(digest)) is not None, f"artifact {name} has no sha256")
        path = run_dir / name
        _require(path.is_file(), f"artifact {name} is missing from {run_dir}")
        _require(gfc.sha256_file(path) == digest, f"artifact {name} digest mismatch")

    try:
        preflight_doc = json.loads((run_dir / "preflight.json").read_text(encoding="utf-8"))
    except ValueError as exc:
        raise ReceiptError(f"preflight.json unreadable: {exc}") from exc
    _require(isinstance(preflight_doc, dict), "preflight.json is not an object")
    summary = gfc.summarize_preflight(preflight_doc)
    _require(_close(receipt.get("preflight"), summary),
             "preflight summary does not re-derive from preflight.json")
    _require(receipt.get("host_quiet") is summary["quiet_host"],
             "host_quiet disagrees with preflight.json")
    if require_quiet_host:
        _require(summary["quiet_host"], "quiet host required; preflight was not quiet")
    if live:
        # Host-side evidence (an AX snapshot of the insert slot, or REAPER's
        # log summary) must name the plug-in and the same host process the
        # receipt declares. Like the rest of the receipt it is self-attested;
        # binding it to the host block means editing one requires editing both
        # consistently with the plug-in's own host_process event.
        try:
            evidence = json.loads((run_dir / "host_evidence.json").read_text(encoding="utf-8"))
        except ValueError as exc:
            raise ReceiptError(f"host_evidence.json unreadable: {exc}") from exc
        _require(isinstance(evidence, dict), "host_evidence.json is not an object")
        _require(plugin["name"] in json.dumps(evidence.get("plugins", [])),
                 "host_evidence.json never shows the plug-in")
        _require(evidence.get("host_pid") == host.get("pid") and
                 evidence.get("host_executable") == host.get("executable"),
                 "host_evidence.json names a different host process")

    analysis = receipt.get("analysis") or {}
    tolerance = analysis.get("tolerance")
    window = analysis.get("window") or {}
    _require(isinstance(tolerance, (int, float)) and 0 < tolerance <= 1e-2,
             "analysis.tolerance must be in (0, 1e-2]")
    floor = gfc.MIN_WINDOW_POST if live else getattr(gfc, "MIN_WINDOW_POST_DRY_RUN",
                                                     gfc.MIN_WINDOW_POST)
    _require(isinstance(window.get("pre"), int) and isinstance(window.get("post"), int)
             and window["pre"] >= 0 and window["post"] >= floor,
             f"analysis.window malformed (post must be >= {floor})")
    bounce_name = receipt.get("bounce_artifact")
    if bounce_name is not None:
        _require(bounce_name in artifacts, "bounce artifact not digest-bound")
    realtime_name = receipt.get("host_realtime_artifact")
    if realtime_name is not None:
        _require(realtime_name in artifacts, "host realtime artifact not digest-bound")
    _, stimulus = gfc.read_wav(run_dir / "stimulus.wav")
    render_frames = len(stimulus[0]) + window["post"]
    _require(analysis.get("render_frames") == render_frames,
             "analysis.render_frames does not match the bound stimulus")

    # M6: no host input may reach the capture. WP-h1's check covers the whole
    # run and every channel: the bound stimulus (input=host) or the plug-in's
    # internal stimulus (input=internal) wherever the transport played, exact
    # silence everywhere else. The input mode comes from the plug-in's own
    # digest-bound fault_armed, never from the receipt; a live-template row
    # (track input "No Input", monitoring off) must run on the internal one.
    armed = gfc.first_event(gfc.load_events(run_dir / "events.jsonl"), "fault_armed") or {}
    input_mode = armed.get("input", "host")
    if (receipt.get("host_settings") or {}).get("template") == "live":
        _require(input_mode == "internal",
                 "a live-template row must run on the plug-in's internal stimulus")
    try:
        capture = gfc.read_capture(run_dir / "capture.bin")
    except ValueError as exc:
        raise ReceiptError(f"capture.bin unreadable: {exc}") from exc
    foreign = gfc.captured_input_matches_stimulus(
        capture, stimulus[0] if input_mode == "host" else None)
    _require(foreign is None, f"captured input is not the expected {input_mode} stimulus "
                              f"(first difference at stream index {foreign}); it may hold "
                              "host input")

    recomputed = hosts.evaluate_v2(
        run_dir, control_kind=kind, fault=fault_kind,
        fault_sample=int(fault.get("requested_project_sample", -1)),
        tolerance=float(tolerance), window_pre=window["pre"], window_post=window["post"],
        host_alive=bool(receipt.get("host_alive")),
        bounce_path=(run_dir / bounce_name) if bounce_name else None,
        realtime_path=(run_dir / realtime_name) if realtime_name else None,
        render_frames=render_frames, nonce=nonce, host=host, hosting_mode=mode,
        host_quiet=summary["quiet_host"] if live else None, listed=list(artifacts))
    for key in RECOMPUTED:
        _require(_close(receipt.get(key), recomputed.get(key)),
                 f"{key} does not re-derive from the artifacts")
    verdict = recomputed["verdict"]
    _require(verdict in gfc.VERDICTS, "verdict outside the closed set")
    if verdict == "pass":
        _require(recomputed["nulls"]["plugin"] is not None and
                 recomputed["nulls"]["bounce"] is not None,
                 "a PASS needs both the plug-in and the bounce null")
    outcome = hosts.column_outcome(recomputed, control_kind=kind, host_id=host["id"])
    _require(receipt.get("column_outcome") == outcome,
             "column_outcome does not re-derive from the artifacts")
    return verdict, outcome


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("receipt", type=Path)
    ap.add_argument("--run-dir", type=Path,
                    help="Directory holding the artifacts (default: the receipt's directory).")
    ap.add_argument("--allow-dry-run", action="store_true")
    ap.add_argument("--require-quiet-host", action="store_true")
    args = ap.parse_args(argv)
    try:
        receipt = json.loads(args.receipt.read_text(encoding="utf-8"))
        _require(isinstance(receipt, dict), "receipt is not a JSON object")
        verdict, outcome = verify(receipt, args.run_dir or args.receipt.parent,
                                  allow_dry_run=args.allow_dry_run,
                                  require_quiet_host=args.require_quiet_host)
    except (OSError, ValueError) as exc:
        print(f"INVALID: {exc}")
        return EXIT_INVALID
    print(f"VERIFIED control_kind={receipt.get('control_kind')} verdict={verdict} "
          f"outcome={outcome}")
    return EXIT_OK if outcome in FULL_OUTCOMES else EXIT_NOT_OK


if __name__ == "__main__":
    raise SystemExit(main())
