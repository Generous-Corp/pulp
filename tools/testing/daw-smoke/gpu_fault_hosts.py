#!/usr/bin/env python3
"""Real-host extensions of the GPU fault-continuity evidence (WP-K).

WP-h1's gpu_fault_continuity.py owns the capture format, the CPU reference,
the null analysis and the per-block delivery analysis for one run. This module
adds what the other hosts (Logic, GarageBand, Bitwig) and the WP-K controls
need, without changing how a v1 receipt is judged:

* control kinds: a fault row, the positive control (fires, applies nothing),
  the planted negatives (a dropout or a continuous-but-wrong fallback after
  the fault, which must FAIL; the GPU path switched off, which must be
  INCONCLUSIVE), and an offline-fault row (never a GPU-continuity PASS);
* binding the plug-in's process to the host this run launched, as a recorded
  conjunction (Fable's review, M5), from the plug-in's `host_process` event
  and its `claim` file;
* the host's own realtime render as the primary realtime artifact (M1), with
  GarageBand, which cannot render in realtime, capped at "plug-in capture only";
* the per-block delivery bar (M2) applied to a fault cell: a bounded pre-fault
  GPU share, frozen GPU and all-fallback after the applied fault, no silence;
* calling-pattern statistics from capture callback-entry times, when present;
* the `pulp.gpu-audio.daw-continuity.v2` receipt and its evaluation.

Every input this module needs from WP-h1's library that is not there yet makes
a run INCONCLUSIVE, never PASS.
"""
from __future__ import annotations

import importlib.util
import re
import statistics
import sys
from pathlib import Path
from typing import Any, Sequence

_LIB_PATH = Path(__file__).resolve().parent / "gpu_fault_continuity.py"
if "gpu_fault_continuity" in sys.modules:
    gfc = sys.modules["gpu_fault_continuity"]
else:
    _spec = importlib.util.spec_from_file_location("gpu_fault_continuity", _LIB_PATH)
    gfc = importlib.util.module_from_spec(_spec)
    sys.modules[_spec.name] = gfc
    _spec.loader.exec_module(gfc)

SCHEMA_V2 = "pulp.gpu-audio.daw-continuity.v2"
HOSTS = ("reaper", "logic", "garageband", "bitwig")
# Which plug-in formats each host can load.
HOST_FORMATS = {"reaper": ("clap", "vst3", "au"), "logic": ("au",), "garageband": ("au",),
                "bitwig": ("clap", "vst3")}
# Hosts that can render the track in realtime (the plug-in sees a realtime,
# playing transport), so the host itself attests the realtime pass (M1).
REALTIME_RENDER_HOSTS = ("reaper", "logic", "bitwig")
HOSTING_MODES = ("in_process", "auhostingservice", "auhostingservice_arrow",
                 "bitwig_plugin_host")
CONTROL_KINDS = ("fault", "positive_control", "planted_negative", "offline_fault")
POSITIVE_CONTROL = "positive_control"
# Planted negatives and what each must verify as. The first three corrupt the
# audio after the fault (a dropout, a one-quantum misalignment, a stale
# fallback) and must FAIL; gpu_off keeps the plug-in off the shared-I/O path
# and must be INCONCLUSIVE on that precondition.
PLANTED_NEGATIVES = {"no_fallback": "fail", "fallback_misaligned": "fail",
                     "fallback_stale": "fail", "gpu_off": "inconclusive"}
NOT_SHARED_PATH = "not on the shared-I/O GPU path"
# M2: the GPU must have carried the stream just before the fault.
GPU_SHARE_PASS = 0.9
GPU_SHARE_VOID = 0.5
# A positive control must show the GPU path carried the stream, or every fault
# row in its column only tested the fallback.
CONTROL_MAX_FALLBACK_SHARE = 0.01
# Capture pacing: a callback that starts sooner than this fraction of the
# previous callback's duration after it came in a burst.
BURST_GAP_FRACTION = 0.25
BURSTY_FRACTION = 0.05

_AUHOSTING = re.compile(
    r"^/System/Library/Frameworks/AudioToolbox\.framework/(?:Versions/A/)?XPCServices/"
    r"AUHostingServiceXPC(_arrow)?\.xpc/Contents/MacOS/AUHostingServiceXPC(_arrow)?$")
# Modes where the plug-in runs in a process the host does not own by pid, so
# the responsible process is the only link back to the host and is required.
OUT_OF_PROCESS_MODES = ("auhostingservice", "auhostingservice_arrow", "bitwig_plugin_host")
_BITWIG_HOST = re.compile(r"/Bitwig Studio\.app/Contents/MacOS/Bitwig Plug-in Host [^/]+\.app/")
_BITWIG_ENGINE = re.compile(r"/Bitwig Studio\.app/Contents/MacOS/Bitwig Audio Engine [^/]+\.app/")
# Executables of known hosts, to tell evidence from another DAW (FAIL) from
# evidence from a process that is merely not this run's plug-in host
# (a scanner, a validator: INCONCLUSIVE).
DAW_EXECUTABLES = {
    "logic": re.compile(r"/Logic Pro[^/]*\.app/Contents/MacOS/"),
    "garageband": re.compile(r"/GarageBand\.app/Contents/MacOS/"),
    "bitwig": re.compile(r"/Bitwig Studio\.app/Contents/MacOS/"),
    "reaper": re.compile(r"/REAPER[^/]*\.app/Contents/MacOS/"),
    "ableton": re.compile(r"/Ableton Live[^/]*\.app/Contents/MacOS/"),
    "cubase": re.compile(r"/Cubase[^/]*\.app/Contents/MacOS/"),
    "studio_one": re.compile(r"/Studio One[^/]*\.app/Contents/MacOS/"),
}


# ── Calling pattern ───────────────────────────────────────────────────────

def calling_pattern(capture: Any) -> dict[str, Any]:
    """How the host called the plug-in on the realtime path. `paced` means
    callbacks arrived about one block apart; `bursty` means a meaningful share
    came back to back (anticipative FX, Logic prefetch), which turns a GPU
    test into a test of the fallback. `unknown` without callback times."""
    raw = getattr(capture, "callbacks", None) or []
    calls = [(c[0], c[1]) for c in raw if not (len(c) > 2 and c[2])]
    if len(calls) < 3 or all(t == 0 for t, _ in calls):
        return {"classification": "unknown", "callbacks": len(calls)}
    ratios, bursts = [], 0
    for (t0, f0), (t1, _) in zip(calls, calls[1:]):
        block_ns = f0 * 1e9 / capture.sample_rate
        if block_ns <= 0:
            continue
        gap = t1 - t0
        ratios.append(gap / block_ns)
        bursts += gap < BURST_GAP_FRACTION * block_ns
    if not ratios:
        return {"classification": "unknown", "callbacks": len(calls)}
    ordered = sorted(ratios)
    p99 = ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))]
    burst_fraction = bursts / len(ratios)
    median = statistics.median(ratios)
    paced = burst_fraction < BURSTY_FRACTION and 0.5 <= median <= 2.0
    return {"classification": "paced" if paced else "bursty", "callbacks": len(calls),
            "median_gap_over_block": round(median, 6), "p99_gap_over_block": round(p99, 6),
            "burst_fraction": round(burst_fraction, 6)}


# ── Process binding (M5) ──────────────────────────────────────────────────

def parse_claim(text: str | None) -> dict[str, int] | None:
    """The claim file: "<pid>\\n<claimed_unix_us>\\n"."""
    if text is None:
        return None
    lines = text.strip().splitlines()
    try:
        return {"pid": int(lines[0]), "claimed_unix_us": int(lines[1]) if len(lines) > 1 else 0}
    except (ValueError, IndexError):
        return None


def _path_allowed(path: str, pid: Any, host: dict, mode: str) -> bool:
    if mode == "in_process":
        if host.get("id") == "bitwig":
            # "Within Bitwig" runs plug-ins in the audio engine process.
            return bool(_BITWIG_ENGINE.search(path)) and pid in host.get("engine_pids", [])
        return pid == host.get("pid") and path == host.get("executable")
    if mode in ("auhostingservice", "auhostingservice_arrow"):
        match = _AUHOSTING.search(path)
        return bool(match) and bool(match.group(1)) == (mode == "auhostingservice_arrow")
    if mode == "bitwig_plugin_host":
        return bool(_BITWIG_HOST.search(path))
    return False


def host_binding(events: Sequence[dict], *, claim_text: str | None, host: dict,
                 hosting_mode: str, nonce: str) -> dict[str, Any]:
    """The six-conjunct binding of the plug-in process to the launched host.
    Each conjunct is "held", "failed" or "unavailable"; `outcome` is `bound`,
    `unbound` (a process that is not this run's plug-in host, such as a
    scanner that won the claim: INCONCLUSIVE) or `wrong_daw` (evidence from
    another DAW: FAIL). A missing responsible pid is recorded, never counted
    as a match."""
    procs = [e for e in events if e.get("event") == "host_process"]
    prepares = [e for e in events if e.get("event") == "prepared"]
    proc = procs[0] if procs else {}
    claim = parse_claim(claim_text)
    c: dict[str, str] = {}

    def mark(name: str, held: bool | None) -> None:
        c[name] = "unavailable" if held is None else ("held" if held else "failed")

    mark("nonce", bool(proc) and proc.get("nonce") == nonce and bool(prepares) and
         all(e.get("nonce") == nonce for e in prepares) and all(p == proc for p in procs))
    mark("claim_pid", bool(proc) and claim is not None and claim["pid"] == proc.get("pid") and
         claim["claimed_unix_us"] == proc.get("claimed_unix_us") and bool(prepares) and
         all(e.get("pid") == proc.get("pid") for e in prepares))
    path = str(proc.get("process_path", ""))
    mark("process_path", bool(proc) and _path_allowed(path, proc.get("pid"), host, hosting_mode))
    launch, start, claimed = (host.get("launch_unix_us"), proc.get("start_unix_us"),
                              proc.get("claimed_unix_us"))
    if not proc or not launch or not start or not claimed:
        mark("start_time", None)
    else:
        mark("start_time", launch <= start <= claimed)
    host_resp, resp = host.get("responsible_pid"), proc.get("responsible_pid")
    if host_resp in (None, -1) or resp in (None, -1):
        mark("responsible_pid", None)
    else:
        mark("responsible_pid", host_resp == resp)
    if host.get("id") == "bitwig" and hosting_mode == "bitwig_plugin_host":
        owners = {p for p in (host.get("pid"), *host.get("engine_pids", [])) if p}
        mark("bitwig_owner", bool(owners) and (proc.get("ppid") in owners or resp in owners))
    else:
        c["bitwig_owner"] = "not_applicable"

    # Another DAW's executable as the plug-in process, or as the process
    # responsible for it (an out-of-process plug-in serving another DAW).
    responsible_path = str(proc.get("responsible_path", ""))
    other_daw = next((name for name, pattern in DAW_EXECUTABLES.items()
                      if name != host.get("id") and (pattern.search(path) or
                                                     pattern.search(responsible_path))), None)
    required = ("nonce", "claim_pid", "process_path", "start_time")
    if hosting_mode in OUT_OF_PROCESS_MODES:
        required += ("responsible_pid",)
    if other_daw:
        outcome, why = "wrong_daw", f"the plug-in evidence comes from {other_daw} ({path!r})"
    elif all(c[k] == "held" for k in required) and "failed" not in (c["responsible_pid"],
                                                                     c["bitwig_owner"]):
        outcome, why = "bound", ""
    else:
        unmet = [k for k, v in c.items() if v == "failed" or (k in required and v != "held")]
        outcome, why = "unbound", ("the plug-in process is not bound to this run's host: " +
                                   ", ".join(f"{k} {c[k]}" for k in unmet))
    return {"conjuncts": c, "outcome": outcome, "reason": why, "host_process": proc or None}


# ── Expectations ──────────────────────────────────────────────────────────

def _disposition(base: dict, capture: Any, events: Sequence[dict],
                 fault_sample: int) -> dict | None:
    """WP-h1's per-block delivery analysis (M2), when the library has it."""
    if base.get("disposition") is not None:
        return base["disposition"]
    fn = getattr(gfc, "delivery_disposition", None)
    if fn is None or capture is None:
        return None
    applied = gfc.first_event(events, "provider_fault_applied") or {}
    prepared = gfc.first_event(events, "prepared") or {}
    return fn(capture, fault_sample, applied.get("project_sample"),
              int(prepared.get("lead_blocks", 0)))


def fault_delivery_expectation(disposition: dict | None) -> tuple[str, str]:
    """M2 for a fault cell: `ok`, `degraded` (pre-fault GPU share in
    [50 %, 90 %): quotable only as a degraded host), or `void` with why."""
    if disposition is None:
        return "void", "the capture has no per-block delivery counters (M2)"
    share = disposition.get("pre_window_gpu_share")
    if share is None or share < GPU_SHARE_VOID:
        return "void", f"pre-fault GPU share {share} is under {GPU_SHARE_VOID:.0%}"
    for key, want, why in (
            ("gpu_frozen_after_applied", True, "the GPU kept delivering after the applied fault"),
            ("all_fallback_after", True,
             "not every block after the applied fault came from the fallback"),
            ("silence_blocks_delta", 0, "silence blocks were delivered")):
        if disposition.get(key) != want:
            return "void", why
    first = disposition.get("first_fallback_quanta_after_applied")
    lead = disposition.get("lead_blocks")
    if first is None or lead is None or first > lead + 1:
        return "void", (f"the first fallback block came {first} quanta after the fault "
                        f"(lead {lead})")
    if share < GPU_SHARE_PASS:
        return "degraded", f"pre-fault GPU share {share:.3f} is under {GPU_SHARE_PASS:.0%}"
    return "ok", f"pre-fault GPU share {share:.3f}; fallback took over within {first} quanta"


def positive_control_expectation(snapshots: dict[str, dict],
                                 disposition: dict | None) -> tuple[bool, str]:
    missing = [p for p in gfc.HEALTH_PHASES if p not in snapshots]
    if missing:
        return False, "missing health snapshots: " + ", ".join(missing)
    if any(snapshots[p].get("provider_state") != "ready" for p in gfc.HEALTH_PHASES):
        return False, "the provider left `ready` during the positive control"
    before, after = snapshots["before"], snapshots["after"]
    gpu = gfc.gpu_delivered(after) - gfc.gpu_delivered(before)
    fallback = int(after.get("fallback_blocks", 0)) - int(before.get("fallback_blocks", 0))
    total = gpu + fallback
    if total <= 0 or gpu / total < GPU_SHARE_PASS:
        return False, (f"the GPU delivered {gpu} of {total} quanta during the control, under "
                       f"{GPU_SHARE_PASS:.0%}; this column would only test the fallback")
    if fallback > max(1, CONTROL_MAX_FALLBACK_SHARE * total):
        return False, f"{fallback} fallback quanta during a run with no fault"
    if disposition is not None:
        share = disposition.get("pre_window_gpu_share")
        if share is None or share < GPU_SHARE_PASS:
            return False, f"pre-window GPU share {share} is under {GPU_SHARE_PASS:.0%}"
    return True, f"GPU delivered {gpu} of {total} quanta; {fallback} fallback"


def _event_preconditions(events: Sequence[dict], ir_taps: int) -> list[str]:
    """The run-level preconditions WP-h1's evaluate_run applies, recomputed
    here so the v2 verdict does not depend on parsing its reason strings."""
    out: list[str] = []
    prepares = [e for e in events if e.get("event") == "prepared"]
    if not prepares or any(e.get("ok") is not True for e in prepares):
        out.append("the plug-in did not prepare its GPU engine every time")
    if any(e.get("path") != "shared_memory" for e in prepares):
        out.append(f"the plug-in is {NOT_SHARED_PATH} "
                   f"(paths={sorted({str(e.get('path')) for e in prepares})})")
    if any(e.get("ir_taps") != ir_taps for e in prepares):
        out.append("the plug-in loaded a different impulse response")
    if gfc.first_event(events, "capture_overflow") is not None:
        out.append("the plug-in dropped capture records")
    refused = gfc.first_event(events, "fault_refused")
    if refused is not None:
        out.append(f"the plug-in refused the request: "
                   f"{refused.get('reason') or refused.get('detail')}")
    return out


def _usable(run_dir: Path, name: str, listed: Sequence[str] | None) -> bool:
    return (run_dir / name).is_file() and (listed is None or name in listed)


def evaluate_v2(run_dir: Path, *, control_kind: str, fault: str, fault_sample: int,
                tolerance: float, window_pre: int, window_post: int, host_alive: bool,
                bounce_path: Path | None, realtime_path: Path | None, render_frames: int,
                nonce: str, host: dict, hosting_mode: str,
                listed: Sequence[str] | None = None) -> dict[str, Any]:
    """The v2 evidence for one run. The null analysis, capture checks and
    health snapshots are WP-h1's (`evaluate_run`); the verdict is recomputed
    here per control kind, with the host binding and the realtime render."""
    if control_kind not in CONTROL_KINDS:
        raise ValueError(f"unknown control kind {control_kind!r}")
    # evaluate_run judges only WP-h1's fault kinds; a positive control borrows
    # the render-fault shape (no provider fault expected) for its analysis.
    base_fault = fault if fault in gfc.FAULT_KINDS else gfc.RENDER_FAULTS[0]
    base = gfc.evaluate_run(run_dir, fault=base_fault, fault_sample=fault_sample,
                            tolerance=tolerance, window_pre=window_pre,
                            window_post=window_post, host_alive=host_alive,
                            bounce_path=bounce_path, render_frames=render_frames,
                            nonce=nonce, reaper_pid=None, listed=listed)
    events = (gfc.load_events(run_dir / "events.jsonl")
              if _usable(run_dir, "events.jsonl", listed) else [])
    if fault not in gfc.FAULT_KINDS:
        # Restore the real fault name the analysis was asked about.
        base["fault"] = dict(base["fault"], kind=fault)
    ir = gfc.read_f32(run_dir / "ir.f32") if _usable(run_dir, "ir.f32", listed) else []
    claim_text = ((run_dir / "claim").read_text(encoding="utf-8")
                  if _usable(run_dir, "claim", listed) else None)
    binding = host_binding(events, claim_text=claim_text, host=host,
                           hosting_mode=hosting_mode, nonce=nonce)
    mismatches: list[str] = []
    preconditions = _event_preconditions(events, len(ir))
    if binding["outcome"] == "wrong_daw":
        mismatches.append(binding["reason"])
    elif binding["outcome"] == "unbound":
        preconditions.append(binding["reason"])
    proc = binding["host_process"] or {}
    if proc and proc.get("config_source") not in ("env", "rendezvous"):
        preconditions.append(f"the plug-in had no run configuration "
                             f"(config_source={proc.get('config_source')!r})")

    armed = gfc.first_event(events, "fault_armed") or {}
    planted = armed.get("planted_negative")
    if (control_kind == "planted_negative") != (planted in PLANTED_NEGATIVES):
        mismatches.append(f"control kind {control_kind!r} but the plug-in armed "
                          f"planted_negative={planted!r}")
    if (control_kind == POSITIVE_CONTROL) != (fault == POSITIVE_CONTROL):
        mismatches.append(f"control kind {control_kind!r} with fault {fault!r}")
    if armed and armed.get("after_project_sample") != fault_sample + window_post:
        mismatches.append("fault_armed observes a different window than the analysis")
    for name in ("fault_armed", "fault_fired"):
        event = gfc.first_event(events, name)
        if event is not None and (event.get("fault") != fault or
                                  event.get("at_project_sample") != fault_sample):
            mismatches.append(f"{name} does not match the requested fault and sample")

    capture = None
    if _usable(run_dir, "capture.bin", listed):
        try:
            capture = gfc.read_capture(run_dir / "capture.bin")
        except ValueError:
            capture = None
    disposition = _disposition(base, capture, events, fault_sample)

    fired = gfc.first_event(events, "fault_fired")
    applied = gfc.first_event(events, "provider_fault_applied")
    render_loss = gfc.first_event(events, "render_loss")
    snapshots = base["health"]["snapshots"]
    delivery, delivery_note = "not_applicable", ""
    if control_kind == POSITIVE_CONTROL:
        if applied is not None:
            mismatches.append("a provider fault was applied during the positive control")
        took_effect = fired is not None
        health_ok, health_note = positive_control_expectation(snapshots, disposition)
    else:
        took_effect = fired is not None and (
            bool(render_loss and render_loss.get("performed")) if fault in gfc.RENDER_FAULTS
            else applied is not None)
        health_ok, health_note = base["health"]["expectation_met"], base["health"]["note"]
        if control_kind == "fault" and fault in gfc.AUDIO_FAULTS:
            delivery, delivery_note = fault_delivery_expectation(disposition)
            if delivery == "void":
                preconditions.append(f"GPU-in-the-loop bar not met: {delivery_note}")
    realtime = bool(fired) and not bool(fired.get("offline", True))
    if control_kind == "offline_fault":
        # Offline renders run the CPU path by design: this row can only show
        # that a fault arriving during a bounce leaves it exact.
        realtime, health_ok = True, True
        if fired is not None and not fired.get("offline"):
            mismatches.append("an offline_fault row whose fault fired on a realtime block")

    # M1: the host's own realtime render of the track, nulled like the bounce.
    nulls = dict(base["nulls"])
    realtime_null = None
    if realtime_path is not None and bounce_path is not None and (
            realtime_path.name == bounce_path.name or
            (_usable(run_dir, realtime_path.name, listed) and
             _usable(run_dir, bounce_path.name, listed) and
             gfc.sha256_file(realtime_path) == gfc.sha256_file(bounce_path))):
        # A realtime pass delivers GPU blocks before the fault, which round
        # differently from the offline CPU path: identical bytes mean the
        # "realtime render" is the bounce, not a second artifact.
        preconditions.append("the host realtime render is the offline bounce itself")
        realtime_path = None
    if realtime_path is not None and _usable(run_dir, realtime_path.name, listed):
        _, rendered = gfc.read_wav(realtime_path)
        _, stimulus = gfc.read_wav(run_dir / "stimulus.wav")
        reference = gfc.bounce_reference(stimulus, ir, len(rendered[0]))
        realtime_null = gfc.null_test(rendered, reference, fault_sample, tolerance=tolerance,
                                      window_pre=window_pre, window_post=window_post,
                                      required_frames=render_frames).as_dict()
    nulls["host_realtime"] = realtime_null
    if (host.get("id") in REALTIME_RENDER_HOSTS and realtime_null is None and
            control_kind in ("fault", POSITIVE_CONTROL)):
        preconditions.append("no host realtime render of the fault pass (M1)")

    verdict, reasons = gfc.decide(fault=base_fault, plugin_null=nulls["plugin"],
                                  bounce_null=nulls["bounce"], health_ok=health_ok,
                                  fault_fired=took_effect, host_alive=host_alive,
                                  realtime_at_fault=realtime, preconditions=preconditions,
                                  mismatches=mismatches)
    if realtime_null is not None and not realtime_null["passed"] and verdict != "fail":
        verdict, reasons = "fail", [f"host realtime render null failed: max_abs_error="
                                    f"{realtime_null['max_abs_error']} first="
                                    f"{realtime_null['first_deviating_sample']}"]
    if verdict == "fail" and preconditions:
        # S4: a FAIL still shows the harness preconditions that were also unmet.
        reasons = reasons + [f"also: {p}" for p in preconditions if p not in reasons]

    negative_proved = None
    if control_kind == "planted_negative":
        # A negative proves something only on an otherwise sound run: the
        # fault took effect in a process bound to this host, and nothing
        # else about the run was off.
        bound = binding["outcome"] == "bound" and not mismatches
        if PLANTED_NEGATIVES.get(planted) == "fail":
            sound = bound and took_effect
            # Proved only by a plug-in null that fails, and not before the
            # fault: what these negatives corrupt is the fallback.
            plugin = nulls["plugin"]
            negative_proved = bool(
                sound and not preconditions and verdict == "fail" and plugin is not None and
                not plugin["passed"] and plugin["first_deviating_sample"] is not None and
                plugin["first_deviating_sample"] >= fault_sample)
        else:
            # gpu_off: the GPU path is off, so a provider fault may be refused;
            # the proof is that the path precondition, and only it (plus that
            # refusal), kept the run from passing.
            negative_proved = bool(
                bound and verdict == "inconclusive" and
                any(NOT_SHARED_PATH in p for p in preconditions) and
                all(NOT_SHARED_PATH in p or p.startswith("the plug-in refused the request")
                    for p in preconditions))
    return {
        "fault": base["fault"],
        "pdc_samples": base["pdc_samples"],
        "plugin_identity": base["plugin_identity"],
        "binding": {k: v for k, v in binding.items() if k != "host_process"},
        "plugin_process": binding["host_process"],
        "capture": base["capture"],
        "calling_pattern": (calling_pattern(capture) if capture else
                            {"classification": "unknown", "callbacks": 0}),
        "disposition": disposition,
        "delivery": {"bar": delivery, "note": delivery_note},
        "offline_path": {k: base.get(k) for k in ("offline_records_in_bounce",
                                                  "offline_path_checked")},
        "nulls": nulls,
        "health": {"snapshots": snapshots, "expectation_met": health_ok, "note": health_note},
        "host_alive": host_alive,
        "verdict": verdict,
        "verdict_reasons": reasons,
        "planted_negative": planted,
        "negative_proved": negative_proved,
    }


def column_outcome(evidence: dict, *, control_kind: str, host_id: str) -> str:
    """What a verified receipt contributes to its matrix column."""
    verdict = evidence["verdict"]
    if control_kind == "planted_negative":
        return "negative_proved" if evidence["negative_proved"] else "negative_not_proved"
    if control_kind == POSITIVE_CONTROL:
        return "control_passed" if verdict == "pass" else f"control_{verdict}"
    if control_kind == "offline_fault":
        return f"offline_{verdict}"
    if verdict != "pass":
        return verdict
    if host_id not in REALTIME_RENDER_HOSTS:
        return "pass_plugin_capture_only"
    if evidence["delivery"]["bar"] == "degraded":
        return "pass_degraded_host"
    return "pass"


def build_receipt_v2(**fields: Any) -> dict[str, Any]:
    receipt = {"schema": SCHEMA_V2, **fields}
    receipt["receipt_sha256"] = gfc.receipt_digest(receipt)
    return receipt
