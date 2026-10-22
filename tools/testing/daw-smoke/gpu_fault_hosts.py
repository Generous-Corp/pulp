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
import json
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
PLANTED_NEGATIVES = {**{name: "fail" for name in gfc.PLANTED},
                     **{name: "inconclusive" for name in gfc.PATH_NEGATIVES}}
NOT_SHARED_PATH = "not on the shared-I/O GPU path"
# M2: the GPU must have carried the stream just before the fault.
GPU_SHARE_PASS = 0.9
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


REALTIME_PROVENANCE = "realtime_render.json"
# A realtime render cannot finish faster than the audio it renders; allow a
# little clock slack, never an offline-speed render.
REALTIME_MIN_WALL_FRACTION = 0.9


# ── Calling pattern ───────────────────────────────────────────────────────

def calling_pattern(capture: Any) -> dict[str, Any]:
    """How the host called the plug-in on the realtime path. `paced` means
    callbacks arrived about one block apart; `bursty` means a meaningful share
    came back to back (anticipative FX, Logic prefetch), which turns a GPU
    test into a test of the fallback. `unknown` without callback times."""
    records = getattr(capture, "records", None) or []
    calls = [(r.get("host_time_ns", 0), r["frames"]) for r in records if not r.get("offline")]
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

def positive_control_expectation(snapshots: dict[str, dict]) -> tuple[bool, str]:
    """The column bar on top of WP-h1's per-block delivery: across the whole
    control run the GPU carried >= 90 % of the quanta, with almost no fallback."""
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
    return True, f"GPU delivered {gpu} of {total} quanta; {fallback} fallback"


def loss_status_problem(snapshots: dict[str, dict]) -> str:
    """G2: with Batch A under test, a lost provider is reported as such by the
    transport's own status: provider_state=unavailable and
    recovery_reason=provider-lost in the `after` snapshot. Reported is not
    enough; a status that still says the provider is fine is a defect."""
    after = snapshots.get("after") or {}
    state, reason = after.get("provider_state"), after.get("recovery_reason")
    if state == "unavailable" and reason == "provider-lost":
        return ""
    return (f"the transport's status did not report the loss: provider_state={state!r}, "
            f"recovery_reason={reason!r} (Batch A requires 'unavailable' and 'provider-lost')")


def _usable(run_dir: Path, name: str, listed: Sequence[str] | None) -> bool:
    return (run_dir / name).is_file() and (listed is None or name in listed)


# Reasons WP-h1's evaluation gives for a run with the GPU path deliberately
# off, which the gpu_off path negative expects and nothing else may add to.
_PATH_NEGATIVE_REASONS = (NOT_SHARED_PATH, "path negative", "per-block delivery",
                          "the GPU carried", "GPU path delivered no blocks")
REALTIME_NOT_COMPUTED = "host realtime render null not computed"


def evaluate_v2(run_dir: Path, *, control_kind: str, fault: str, fault_sample: int,
                tolerance: float, window_pre: int, window_post: int, host_alive: bool,
                bounce_path: Path | None, realtime_path: Path | None, render_frames: int,
                nonce: str, host: dict, hosting_mode: str, host_quiet: bool | None = None,
                listed: Sequence[str] | None = None) -> dict[str, Any]:
    """The v2 evidence for one run. WP-h1's `evaluate_run` does the whole
    single-run analysis (the three nulls, per-block delivery, device binding,
    the input check, planted shapes); this adds what v2 is for and keeps the
    stricter of the two verdicts: the host binding (M5), the realtime render's
    distinctness and provenance (M1), the control-kind rules (M3), the
    positive-control column bar, and GarageBand's plug-in-capture-only cap."""
    if control_kind not in CONTROL_KINDS:
        raise ValueError(f"unknown control kind {control_kind!r}")
    mismatches: list[str] = []
    preconditions: list[str] = []

    # M1: the realtime render is its own artifact with realtime provenance,
    # checked before it is allowed into the analysis at all.
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
    if realtime_path is not None:
        problem = realtime_provenance_problem(run_dir, realtime_path.name, host,
                                              render_frames, listed)
        if problem:
            preconditions.append(f"host realtime render provenance: {problem}")
            realtime_path = None

    base = gfc.evaluate_run(run_dir, fault=fault, fault_sample=fault_sample,
                            tolerance=tolerance, window_pre=window_pre,
                            window_post=window_post, host_alive=host_alive,
                            bounce_path=bounce_path, render_frames=render_frames,
                            nonce=nonce, reaper_pid=None, listed=listed,
                            realtime_path=realtime_path, host_quiet=host_quiet)
    events = (gfc.load_events(run_dir / "events.jsonl")
              if _usable(run_dir, "events.jsonl", listed) else [])
    claim_text = ((run_dir / "claim").read_text(encoding="utf-8")
                  if _usable(run_dir, "claim", listed) else None)
    binding = host_binding(events, claim_text=claim_text, host=host,
                           hosting_mode=hosting_mode, nonce=nonce)
    if binding["outcome"] == "wrong_daw":
        mismatches.append(binding["reason"])
    elif binding["outcome"] == "unbound":
        preconditions.append(binding["reason"])
    proc = binding["host_process"] or {}
    if proc and proc.get("config_source") not in ("env", "rendezvous"):
        preconditions.append(f"the plug-in had no run configuration "
                             f"(config_source={proc.get('config_source')!r})")

    controls = base.get("controls") or {}
    planted = controls.get("planted_negative")
    gpu_off = bool(controls.get("gpu_off"))
    if gpu_off and planted is None:
        planted = "gpu_off"
    if (control_kind == "planted_negative") != (planted in PLANTED_NEGATIVES):
        mismatches.append(f"control kind {control_kind!r} but the plug-in armed "
                          f"planted_negative={planted!r}")
    # gpu_off arms nothing (the GPU path is off, so there is no provider to
    # fault); every other negative and every fault row rides on a real fault.
    wants_control_fault = control_kind == POSITIVE_CONTROL or planted == "gpu_off"
    if wants_control_fault != (fault == POSITIVE_CONTROL):
        mismatches.append(f"control kind {control_kind!r} (planted={planted!r}) with fault "
                          f"{fault!r}")
    applied = base["fault"].get("provider_fault_applied")
    if wants_control_fault and applied is not None:
        mismatches.append(f"a provider fault was applied during {planted or control_kind}")
    if control_kind == POSITIVE_CONTROL:
        ok, note = positive_control_expectation(base["health"]["snapshots"])
        if not ok:
            preconditions.append(f"positive control: {note}")
    if control_kind == "offline_fault" and base["fault"]["realtime_block"]:
        mismatches.append("an offline_fault row whose fault fired on a realtime block")

    capture = None
    if _usable(run_dir, "capture.bin", listed):
        try:
            capture = gfc.read_capture(run_dir / "capture.bin")
        except ValueError:
            capture = None

    delivery = base.get("delivery")
    if control_kind == "fault" and fault in gfc.AUDIO_FAULTS:
        # G2: the loss must be named in the status, not only survived.
        if fault in gfc.LOSS_FAULTS:
            problem = loss_status_problem(base["health"]["snapshots"])
            if problem:
                mismatches.append(problem)
        # G3: WP-h1's evaluate_run measures the takeover from the capture
        # record the interposer's host_time_ns lands in (applied_project_sample)
        # and falls back to the service thread's poll without it. A fault
        # cell may not rest on that fallback.
        applied_ns = (applied or {}).get("host_time_ns")
        if not (isinstance(applied_ns, int) and applied_ns > 0) or capture is None or \
                gfc.applied_project_sample(capture, {"host_time_ns": applied_ns}) is None:
            preconditions.append("provider_fault_applied has no host_time_ns that maps to a "
                                 "capture record (G3)")

    # The stricter of WP-h1's verdict and this one.
    base_verdict, base_reasons = base["verdict"], list(base["verdict_reasons"])
    capture_only = (host.get("id") not in REALTIME_RENDER_HOSTS and
                    base["nulls"].get("host_realtime") is None)
    if capture_only:
        # GarageBand cannot render in realtime: the missing realtime null is
        # expected and the outcome is capped instead (column_outcome).
        base_reasons = [r for r in base_reasons if r != REALTIME_NOT_COMPUTED]
        if base_verdict == "inconclusive" and not base_reasons:
            base_verdict = "pass"
    if not host_alive:
        verdict, reasons = "fail", ["host process died during the run"]
    elif mismatches:
        verdict, reasons = "fail", (mismatches + [f"also: {p}" for p in preconditions] +
                                    base_reasons)
    elif base_verdict == "fail":
        verdict, reasons = "fail", base_reasons + [f"also: {p}" for p in preconditions]
    elif preconditions:
        verdict, reasons = "inconclusive", preconditions + base_reasons
    else:
        verdict, reasons = base_verdict, base_reasons

    took_effect = bool(base["fault"]["fired"]) and (
        bool((base["fault"].get("render_loss") or {}).get("performed"))
        if fault in gfc.RENDER_FAULTS else
        True if fault in gfc.CONTROL_FAULTS else applied is not None)
    negative_proved = None
    if control_kind == "planted_negative":
        bound = binding["outcome"] == "bound" and not mismatches and not preconditions
        if PLANTED_NEGATIVES.get(planted) == "fail":
            # Fired and applied, bound, and the FAIL is the corrupted audio
            # alone: a null failing from the fault sample on, nothing else.
            # The plug-in arms the corruption in the quantum that contains the
            # fault sample, so it may begin at that quantum's start (WP-h1, G1).
            plugin = base["nulls"]["plugin"]
            quantum = int((base.get("plugin_identity") or {}).get("quantum") or 0)
            negative_proved = bool(
                bound and took_effect and verdict == "fail" and len(base_reasons) == 1 and
                base_reasons[0].startswith(("plugin null failed",
                                            "host realtime render null failed")) and
                plugin is not None and not plugin["passed"] and
                plugin["first_deviating_sample"] is not None and
                plugin["first_deviating_sample"] >= fault_sample - quantum)
        else:
            # gpu_off: fired, bound, and only the GPU-off consequences kept the
            # run from passing.
            negative_proved = bool(
                bound and base["fault"]["fired"] and verdict == "inconclusive" and
                any(NOT_SHARED_PATH in r for r in base_reasons) and
                all(any(k in r for k in _PATH_NEGATIVE_REASONS) for r in base_reasons))
    return {
        "fault": base["fault"],
        "pdc_samples": base["pdc_samples"],
        "plugin_identity": base["plugin_identity"],
        "binding": {k: v for k, v in binding.items() if k != "host_process"},
        "plugin_process": binding["host_process"],
        "capture": base["capture"],
        "calling_pattern": (calling_pattern(capture) if capture else
                            {"classification": "unknown", "callbacks": 0}),
        "delivery": delivery,
        "controls": controls,
        "offline_path": {k: base.get(k) for k in ("offline_records_in_bounce",
                                                  "offline_path_checked")},
        "nulls": base["nulls"],
        "health": base["health"],
        "host_alive": host_alive,
        "verdict": verdict,
        "verdict_reasons": reasons,
        "planted_negative": planted,
        "negative_proved": negative_proved,
    }


def realtime_provenance_problem(run_dir: Path, artifact: str, host: dict, render_frames: int,
                                listed: Sequence[str] | None) -> str:
    """Why the host realtime render's provenance record does not show a
    realtime render of this artifact by this host, or "" when it does. The
    record is written by the harness around the host's realtime render:
    {"artifact", "mode": "realtime", "host", "started_unix_us",
    "finished_unix_us", "sample_rate"}. A realtime render takes at least as
    long as the audio it renders; an offline bounce does not."""
    if not _usable(run_dir, REALTIME_PROVENANCE, listed):
        return f"{REALTIME_PROVENANCE} is missing"
    try:
        record = json.loads((run_dir / REALTIME_PROVENANCE).read_text(encoding="utf-8"))
    except ValueError:
        return f"{REALTIME_PROVENANCE} is unreadable"
    if not isinstance(record, dict):
        return f"{REALTIME_PROVENANCE} is not an object"
    if record.get("artifact") != artifact:
        return f"it describes {record.get('artifact')!r}, not {artifact!r}"
    if record.get("mode") != "realtime" or record.get("host") != host.get("id"):
        return f"mode={record.get('mode')!r} host={record.get('host')!r}"
    start, end, rate = (record.get("started_unix_us"), record.get("finished_unix_us"),
                        record.get("sample_rate"))
    if not all(isinstance(v, int) and v > 0 for v in (start, end, rate)):
        return "times or sample rate missing"
    launch = host.get("launch_unix_us")
    if isinstance(launch, int) and start < launch:
        return "the render started before the host was launched"
    needed = REALTIME_MIN_WALL_FRACTION * render_frames / rate * 1e6
    if end - start < needed:
        return (f"it took {(end - start) / 1e6:.3f} s for {render_frames / rate:.3f} s of audio, "
                f"faster than realtime")
    return ""


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
    if evidence["nulls"].get("host_realtime") is None:
        return "inconclusive"
    if (evidence.get("delivery") or {}).get("degraded_host"):
        return "pass_degraded_host"
    return "pass"


def build_receipt_v2(**fields: Any) -> dict[str, Any]:
    receipt = {"schema": SCHEMA_V2, **fields}
    receipt["receipt_sha256"] = gfc.receipt_digest(receipt)
    return receipt
