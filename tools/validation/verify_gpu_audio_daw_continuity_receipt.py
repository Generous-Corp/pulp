#!/usr/bin/env python3
"""Fail-closed verifier for `pulp.gpu-audio.daw-continuity.v1` receipts (WP-h1).

A receipt is accepted only when it is internally consistent and its evidence
re-derives from the artifacts it names: every artifact digest matches, and the
null analysis and verdict are recomputed from the raw capture, bounce, stimulus
and impulse response rather than read from the receipt. A receipt whose numbers
were edited, or whose artifacts were swapped, is rejected.

Exit codes: 0 verified PASS, 1 verified but not PASS (FAIL or INCONCLUSIVE, a
legitimate finding), 2 invalid receipt.

What a PASS here does and does not prove: the plug-in's output stayed
sample-exact against an independent CPU reference across one injected fault
while it ran in REAPER with its editor open (the REAPER bounce, rendered offline
afterwards, only cross-checks the offline path and PDC). The receipt is
self-contained: the verifier proves internal consistency and run binding
(nonce, pid, digests), not that a careful forger did not fabricate every file. It does not prove render-owner loss propagation,
resource replay or teardown callback suppression; those stay Vellum-gated.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
_LIB_PATH = REPO / "tools/testing/daw-smoke/gpu_fault_continuity.py"
_spec = importlib.util.spec_from_file_location("gpu_fault_continuity", _LIB_PATH)
gfc = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = gfc
_spec.loader.exec_module(gfc)

EXIT_PASS, EXIT_NOT_PASS, EXIT_INVALID = 0, 1, 2
SHA256 = re.compile(r"^[0-9a-f]{64}$")
# realtime.wav is the host's own realtime render of the fault pass: the primary
# host-side evidence, so every receipt binds it.
REQUIRED_ARTIFACTS = ("stimulus.wav", "ir.f32", "events.jsonl", "capture.bin", "preflight.json",
                      "realtime.wav")
# A live run must also bind the host's own log, which names the plug-in it
# loaded, and the run directory's claim, which names the owning process.
LIVE_ARTIFACTS = ("reaper.out.txt", "claim")


class ReceiptError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReceiptError(message)


def _close(a: Any, b: Any) -> bool:
    if a is None or b is None:
        return a is b
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return math.isclose(float(a), float(b), rel_tol=1e-9, abs_tol=1e-12)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_close(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_close(a[k], b[k]) for k in a)
    return a == b


def verify(receipt: dict[str, Any], run_dir: Path, *, allow_dry_run: bool = False,
           require_control_op: bool = False, require_quiet_host: bool = False) -> str:
    """Return the re-derived verdict, or raise ReceiptError."""
    _require(receipt.get("schema") == gfc.SCHEMA, f"schema is not {gfc.SCHEMA}")
    _require(receipt.get("receipt_sha256") == gfc.receipt_digest(receipt),
             "receipt_sha256 does not match the receipt body")

    fault = receipt.get("fault") or {}
    kind = fault.get("kind")
    _require(kind in gfc.FAULT_KINDS, f"unknown fault kind {kind!r}")
    transport = receipt.get("trigger_transport")
    _require(transport in gfc.TRIGGER_TRANSPORTS, f"unknown trigger transport {transport!r}")
    if transport == "dry_run":
        _require(allow_dry_run, "a dry-run receipt is not DAW evidence")
    if require_control_op:
        _require(transport == "control_op",
                 f"control-op evidence required but the fault arrived via {transport}")
    if transport == "control_op":
        control = receipt.get("control_op") or {}
        _require(control.get("operation") == "gpu_audio_fault_inject",
                 "control_op transport without a gpu_audio_fault_inject exchange")
        _require(isinstance(control.get("response"), dict), "control_op response missing")

    plugin = receipt.get("plugin") or {}
    _require(plugin.get("format") in ("clap", "vst3", "au"), "plugin.format missing")
    _require(bool(plugin.get("name")), "plugin.name missing")
    host = receipt.get("host") or {}
    live = transport != "dry_run"
    nonce = receipt.get("nonce")
    _require(isinstance(nonce, str) and len(nonce) >= 16, "run nonce missing")
    reaper_pid = host.get("reaper_pid") if live else None
    if live:
        _require(bool(host.get("reaper_version")), "host.reaper_version missing")
        _require(isinstance(reaper_pid, int) and reaper_pid > 0, "host.reaper_pid missing")
        _require(SHA256.match(str(plugin.get("executable_sha256", ""))) is not None,
                 "plugin.executable_sha256 missing")

    artifacts = receipt.get("artifacts")
    _require(isinstance(artifacts, dict), "artifacts map missing")
    for name in REQUIRED_ARTIFACTS + (LIVE_ARTIFACTS if live else ()):
        _require(name in artifacts, f"required artifact {name} not listed")
    for name, digest in artifacts.items():
        _require("/" not in name and name not in ("", ".", ".."), f"artifact name {name!r} escapes the run dir")
        _require(SHA256.match(str(digest)) is not None, f"artifact {name} has no sha256")
        path = run_dir / name
        _require(path.is_file(), f"artifact {name} is missing from {run_dir}")
        _require(gfc.sha256_file(path) == digest, f"artifact {name} digest mismatch")

    # The preflight summary and host_quiet are re-derived from the bound
    # preflight.json, never taken from the receipt.
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
        log_text = (run_dir / "reaper.out.txt").read_text(encoding="utf-8", errors="replace")
        _require(plugin["name"] in log_text, "reaper.out.txt never mentions the plug-in")

    analysis = receipt.get("analysis") or {}
    tolerance = analysis.get("tolerance")
    window = analysis.get("window") or {}
    _require(isinstance(tolerance, (int, float)) and 0 < tolerance <= 1e-2,
             "analysis.tolerance must be in (0, 1e-2]")
    _require(isinstance(window.get("pre"), int) and isinstance(window.get("post"), int)
             and window["pre"] >= 0 and window["post"] > 0, "analysis.window malformed")

    bounce_name = receipt.get("bounce_artifact")
    if bounce_name is not None:
        _require(bounce_name in artifacts, "bounce artifact not digest-bound")
    _require(receipt.get("realtime_artifact") == "realtime.wav",
             "the host's realtime render is not the bound realtime.wav")
    floor = gfc.MIN_WINDOW_POST if live else gfc.MIN_WINDOW_POST_DRY_RUN
    _require(window["post"] >= floor, f"analysis.window.post is shorter than {floor} samples")
    # The render length comes from the bound stimulus, not from the receipt:
    # the harness renders the whole stimulus plus the post-fault window.
    _, stimulus = gfc.read_wav(run_dir / "stimulus.wav")
    render_frames = len(stimulus[0]) + window["post"]
    _require(analysis.get("render_frames") == render_frames,
             "analysis.render_frames does not match the bound stimulus")
    # host_alive stays self-attested (a crash is a FAIL either way), but only
    # digest-listed artifacts are read and the events must carry this run's
    # nonce and REAPER's pid.
    recomputed = gfc.evaluate_run(
        run_dir, fault=kind, fault_sample=int(fault.get("requested_project_sample", -1)),
        tolerance=float(tolerance), window_pre=window["pre"], window_post=window["post"],
        host_alive=bool(receipt.get("host_alive")),
        bounce_path=(run_dir / bounce_name) if bounce_name else None,
        render_frames=render_frames, nonce=nonce, reaper_pid=reaper_pid,
        listed=list(artifacts), realtime_path=run_dir / "realtime.wav")
    for key in ("fault", "pdc_samples", "plugin_identity", "capture", "nulls", "health",
                "delivery", "controls", "host_alive", "verdict", "verdict_reasons"):
        _require(_close(receipt.get(key), recomputed.get(key)),
                 f"{key} does not re-derive from the artifacts")
    _require(recomputed["verdict"] in gfc.VERDICTS, "verdict outside the closed set")
    if recomputed["verdict"] == "pass":
        _require(all(recomputed["nulls"][k] is not None for k in ("plugin", "realtime", "bounce")),
                 "a PASS needs the plug-in, realtime-render and bounce nulls")
        _require(bool(recomputed["delivery"] and recomputed["delivery"].get("met")),
                 "a PASS needs the per-block delivery requirements met")
    return recomputed["verdict"]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("receipt", type=Path)
    ap.add_argument("--run-dir", type=Path,
                    help="Directory holding the artifacts (default: the receipt's directory).")
    ap.add_argument("--allow-dry-run", action="store_true")
    ap.add_argument("--require-control-op", action="store_true")
    ap.add_argument("--require-quiet-host", action="store_true")
    args = ap.parse_args(argv)
    try:
        receipt = json.loads(args.receipt.read_text(encoding="utf-8"))
        _require(isinstance(receipt, dict), "receipt is not a JSON object")
        verdict = verify(receipt, args.run_dir or args.receipt.parent,
                         allow_dry_run=args.allow_dry_run,
                         require_control_op=args.require_control_op,
                         require_quiet_host=args.require_quiet_host)
    except (OSError, ValueError) as exc:
        print(f"INVALID: {exc}")
        return EXIT_INVALID
    print(f"VERIFIED verdict={verdict}")
    return EXIT_PASS if verdict == "pass" else EXIT_NOT_PASS


if __name__ == "__main__":
    raise SystemExit(main())
