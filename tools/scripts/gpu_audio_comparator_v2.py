#!/usr/bin/env python3
"""Run, bind, and verify pulp.gpu-audio.comparator.v2 receipts (WP-Q).

The comparator binary (pulp-test-native-metal-audio-comparator) runs the same
shared-I/O convolution program on Dawn and on native Metal and emits raw
per-block samples. This tool:

  run     binds a host preflight, the source revision and the binary digest to
          a campaign receipt, then verifies it;
  smoke   the ctest entry point: a short matched run that must not be refused,
          plus a planted unmatched-kernel run that must be refused;
  verify  recomputes every statistic from the raw samples and evaluates the
          PROPOSED Metal-provider gate thresholds over one or two receipts.

The thresholds are proposals from the GPU-audio roadmap (WP-Q) and are not
ratified; the verdict says so. A receipt from a host that is not quiet, or
with fewer than 10,000 blocks per cell, is evaluated but never counted.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
from typing import Any

SCHEMA = "pulp.gpu-audio.comparator.v2"
EVALUATION_SCHEMA = "pulp.gpu-audio.comparator.v2.evaluation"
PROGRAM_ID = "pulp.shared_io_convolution.v1"
REPO_ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = REPO_ROOT / "tools/scripts/gpu_audio_p2_host_preflight.py"
REQUIRED_FRAMES = (64, 128, 256)
REQUIRED_LOADS = ("idle", "render")
GATE_FRAMES = 64
MIN_COUNTED_BLOCKS = 10_000
SAMPLE_FIELDS = (
    "release_ns", "submit_ns", "submit_end_ns", "visible_ns", "gpu_busy_ns",
    "gpu_end_ns", "callback_ns", "worker_cpu_ns", "oracle", "stalled",
)

# PROPOSED by the roadmap; the owner ratifies. Fractions are of the block
# period at GATE_FRAMES; latencies are nanoseconds.
THRESHOLDS: dict[str, Any] = {
    "status": "PROPOSED",
    "ratified": False,
    "source": "GPU-audio roadmap WP-Q (2026-10-22)",
    "frames": GATE_FRAMES,
    "consecutive_quiet_campaigns": 2,
    "i_dawn_p999_fraction_gt": 0.50,
    "i_metal_p999_fraction_lt": 0.25,
    "ii_cpu_ratio_gt": 2.0,
    "ii_dawn_cpu_fraction_gt": 0.10,
    "iii_dawn_wake_p99_ns_gt": 300_000,
    "iii_metal_wake_p99_ns_lt": 100_000,
    "iv_dawn_miss_rate_gt": 0.0001,
    "iv_metal_miss_rate_eq": 0.0,
}


def canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def receipt_digest(receipt: dict[str, Any]) -> str:
    body = {k: v for k, v in receipt.items() if k != "receipt_sha256"}
    return sha256_hex(canonical(body))


def percentile(values: list[float], q: float) -> float | None:
    """Nearest-rank percentile; None for an empty sample."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(q * len(ordered)))
    return float(ordered[min(rank, len(ordered)) - 1])


def summarize(values: list[float]) -> dict[str, Any]:
    return {
        "count": len(values),
        "p50": percentile(values, 0.50),
        "p99": percentile(values, 0.99),
        "p999": percentile(values, 0.999),
        "max": float(max(values)) if values else None,
        "mean": float(sum(values) / len(values)) if values else None,
    }


def cell_metrics(cell: dict[str, Any], config: dict[str, Any]) -> dict[str, Any]:
    s = cell["samples"]
    period = cell["frames"] * 1e9 / config["sample_rate"]
    lead = config["lead_blocks"] * period
    n = cell["blocks"]
    s2v, busy, wake, cb_to_worker, wake_bound, lag = [], [], [], [], [], []
    misses = 0
    for i in range(n):
        visible, submit, release = s["visible_ns"][i], s["submit_ns"][i], s["release_ns"][i]
        ok = s["oracle"][i] == 1 and visible >= 0 and submit >= 0
        if ok:
            s2v.append(visible - submit)
            lag.append(submit - release)
        if not ok or visible > release + lead:
            misses += 1
        if s["gpu_busy_ns"][i] >= 0:
            busy.append(s["gpu_busy_ns"][i])
            if ok:
                # Submit-to-visible minus GPU busy bounds wake latency from
                # above when no host-domain completion time exists.
                wake_bound.append(visible - submit - s["gpu_busy_ns"][i])
        if ok and s["gpu_end_ns"][i] >= 0:
            wake.append(visible - s["gpu_end_ns"][i])
        if ok and s["callback_ns"][i] >= 0:
            cb_to_worker.append(visible - s["callback_ns"][i])
    cpu = [v for v in s["worker_cpu_ns"] if v >= 0]
    frames = cell.get("render_load", {}).get("frame_ns", [])
    return {
        "lane": cell["lane"], "side": cell["side"], "frames": cell["frames"],
        "load": cell["load"], "blocks": n, "period_ns": period,
        "submit_to_visible_ns": summarize(s2v),
        "gpu_busy_ns": summarize(busy),
        "worker_cpu_ns": summarize(cpu),
        "wake_ns": summarize(wake),
        "callback_to_worker_ns": summarize(cb_to_worker),
        "wake_upper_bound_ns": summarize(wake_bound),
        "release_lag_ns": summarize(lag),
        "misses": misses,
        "miss_rate": misses / n if n else None,
        "stalls": int(sum(s["stalled"])),
        "render": {
            "active": bool(cell.get("render_load", {}).get("active")),
            "frames": len(frames),
            "frame_ns": summarize(frames),
            "dropped_frames": cell.get("render_load", {}).get("dropped_frames", 0),
        },
    }


def _check_kernel(receipt: dict[str, Any], completed: list[dict[str, Any]],
                  refusals: list[str]) -> None:
    kernel = receipt.get("kernel", {})
    wgsl = kernel.get("provider_wgsl", {})
    if not wgsl.get("ok"):
        refusals.append("provider_wgsl_unavailable")
        return
    by_frames: dict[int, set[str]] = {}
    for cell in completed:
        canon = cell["kernel"].get("canonical")
        digest = cell["kernel"].get("kernel_digest")
        if not isinstance(canon, dict) or sha256_hex(canonical(canon)) != digest:
            refusals.append(f"kernel_digest_forged:{cell['lane']}:{cell['frames']}")
            continue
        if canon.get("program") != PROGRAM_ID:
            refusals.append(f"unmatched_program:{cell['lane']}:{canon.get('program')}")
        if cell["side"] == "dawn" and (
                canon.get("fft_wgsl_sha256") != wgsl.get("fft_sha256") or
                canon.get("mul_wgsl_sha256") != wgsl.get("mul_sha256")):
            refusals.append(f"dawn_kernel_not_provider_wgsl:{cell['lane']}")
        by_frames.setdefault(cell["frames"], set()).add(digest)
    for frames, digests in sorted(by_frames.items()):
        if len(digests) != 1:
            refusals.append(f"kernel_digest_mismatch:{frames}")
    parity = {(p["lane"], p["frames"], p["load"]): p for p in kernel.get("parity", [])}
    for cell in completed:
        entry = parity.get((cell["lane"], cell["frames"], cell["load"]))
        if entry is None:
            refusals.append(f"kernel_parity_missing:{cell['lane']}:{cell['frames']}")
        elif entry.get("max_abs_diff") is None or entry["max_abs_diff"] > entry["tolerance"]:
            refusals.append(f"kernel_parity_failed:{cell['lane']}:{cell['frames']}")


def host_quiet(preflight: dict[str, Any] | None) -> bool:
    return bool(preflight) and all(
        isinstance(preflight.get(k), dict) and preflight[k].get("status") == "passed" and
        preflight[k].get("quiet_host") is True for k in ("before", "after"))


def assess(receipt: dict[str, Any], min_blocks: int = MIN_COUNTED_BLOCKS) -> dict[str, Any]:
    """Assess one receipt: refusals, inconclusive reasons, and metrics."""
    refusals: list[str] = []
    inconclusive: list[str] = []
    if receipt.get("schema") != SCHEMA:
        return {"refusals": ["schema_mismatch"], "inconclusive": [], "counted": False,
                "metrics": []}
    if receipt.get("receipt_sha256") != receipt_digest(receipt):
        refusals.append("receipt_digest_mismatch")
    config = receipt["config"]
    cells = receipt.get("cells", [])
    completed = [c for c in cells if c.get("status") == "completed"]
    for cell in cells:
        if cell.get("status") == "failed":
            refusals.append(f"cell_failed:{cell['lane']}:{cell['frames']}:{cell['load']}")
    for cell in completed:
        samples = cell.get("samples", {})
        if (cell.get("blocks") != config.get("blocks_per_cell") or
                any(len(samples.get(f, [])) != cell.get("blocks") for f in SAMPLE_FIELDS)):
            refusals.append(f"sample_shape:{cell['lane']}:{cell['frames']}:{cell['load']}")
        elif sum(1 for v in samples["oracle"] if v != 1) != cell.get("oracle_failures"):
            refusals.append(f"oracle_count_mismatch:{cell['lane']}:{cell['frames']}")
        elif cell.get("oracle_failures"):
            refusals.append(f"oracle_failures:{cell['lane']}:{cell['frames']}:{cell['load']}")
    _check_kernel(receipt, completed, refusals)

    if not config.get("paced"):
        refusals.append("not_paced")
    for frames in REQUIRED_FRAMES:
        for load in REQUIRED_LOADS:
            for side in ("metal", "dawn"):
                if not any(c["frames"] == frames and c["load"] == load and c["side"] == side
                           for c in completed):
                    reason = ("render_load_unavailable" if load == "render"
                              else "coverage_incomplete")
                    inconclusive.append(f"{reason}:{side}:{frames}:{load}")
    for cell in completed:
        if cell["load"] == "render" and cell.get("render_load", {}).get("status") != "completed":
            inconclusive.append(f"render_load_incomplete:{cell['lane']}:{cell['frames']}")
    if not any(c["side"] == "dawn" and c.get("gpu_timestamps") and
               any(v >= 0 for v in c["samples"]["gpu_busy_ns"]) for c in completed):
        inconclusive.append("dawn_gpu_timestamps_unavailable")
    if receipt.get("planted_unmatched_kernel"):
        inconclusive.append("planted_unmatched_kernel_run")
    if receipt.get("mode") != "campaign":
        inconclusive.append(f"mode_{receipt.get('mode')}")
    if config.get("blocks_per_cell", 0) < min_blocks:
        inconclusive.append("undersampled")
    # The host must pass the P2 preflight both before and after the campaign,
    # at the revision being measured.
    preflight = receipt.get("host_preflight")
    if not isinstance(preflight, dict) or not all(
            isinstance(preflight.get(k), dict) for k in ("before", "after")):
        inconclusive.append("host_preflight_missing")
        preflight = None
    elif receipt.get("host_preflight_sha256") != sha256_hex(canonical(preflight)):
        refusals.append("host_preflight_digest_mismatch")
    elif not (host_quiet(preflight) and receipt.get("host_quiet") is True):
        inconclusive.append("host_not_quiet")
    elif any(preflight[k].get("source_revision") != receipt.get("source_revision")
             for k in ("before", "after")):
        inconclusive.append("host_preflight_source_mismatch")

    metrics = [cell_metrics(c, config) for c in completed
               if not any(r.startswith("sample_shape:" + c["lane"]) for r in refusals)]
    # A preflight brackets the run but cannot see a load spike inside it; a
    # worker that starts blocks more than a period late was starved.
    for m in metrics:
        lag = m["release_lag_ns"]["p99"]
        if lag is not None and lag > m["period_ns"]:
            inconclusive.append(f"worker_starved:{m['lane']}:{m['frames']}:{m['load']}")
    counted = not refusals and not inconclusive
    lanes = {lane["name"]: lane for lane in config.get("lanes", [])}
    return {
        "refusals": sorted(set(refusals)),
        "inconclusive": sorted(set(inconclusive)),
        "counted": counted,
        "host_id": preflight["before"].get("host_id") if preflight else None,
        "dawn_revision": receipt.get("dawn", {}).get("dawn_revision"),
        "metrics": metrics,
        "production_eligible": {name: bool(lane.get("production_eligible", True))
                                for name, lane in lanes.items()},
    }


def _best(metrics: list[dict[str, Any]], side: str, load: str, key: str, stat: str,
          eligible: dict[str, bool] | None = None) -> tuple[float | None, str | None]:
    best: tuple[float | None, str | None] = (None, None)
    for m in metrics:
        if m["side"] != side or m["load"] != load or m["frames"] != GATE_FRAMES:
            continue
        if eligible is not None and not eligible.get(m["lane"], True):
            continue
        value = m[key][stat] if key != "miss_rate" else m["miss_rate"]
        if value is not None and (best[0] is None or value < best[0]):
            best = (value, m["lane"])
    return best


def evaluate_thresholds(assessment: dict[str, Any]) -> dict[str, Any]:
    """Evaluate the PROPOSED criteria at GATE_FRAMES using the best-performing
    production-eligible Dawn lane against the best Metal lane, per load."""
    t = THRESHOLDS
    metrics = assessment["metrics"]
    eligible = assessment["production_eligible"]
    period = GATE_FRAMES * 1e9 / 48_000
    out: dict[str, Any] = {}
    for load in REQUIRED_LOADS:
        dawn_p999, dawn_p999_lane = _best(metrics, "dawn", load, "submit_to_visible_ns", "p999",
                                          eligible)
        metal_p999, metal_p999_lane = _best(metrics, "metal", load, "submit_to_visible_ns",
                                            "p999")
        dawn_cpu, dawn_cpu_lane = _best(metrics, "dawn", load, "worker_cpu_ns", "mean", eligible)
        metal_cpu, metal_cpu_lane = _best(metrics, "metal", load, "worker_cpu_ns", "mean")
        metal_wake, metal_wake_lane = _best(metrics, "metal", load, "wake_ns", "p99")
        dawn_wake, dawn_wake_lane = _best(metrics, "dawn", load, "wake_ns", "p99", eligible)
        # Dawn exposes no host-domain completion time; the staged timestamp
        # lane's (submit-to-visible minus GPU busy) is an upper bound.
        dawn_wake_bound, dawn_bound_lane = _best(metrics, "dawn", load, "wake_upper_bound_ns",
                                                 "p99")
        dawn_miss, dawn_miss_lane = _best(metrics, "dawn", load, "miss_rate", "", eligible)
        metal_miss, metal_miss_lane = _best(metrics, "metal", load, "miss_rate", "")

        def state(fires: bool | None) -> str:
            return "not_evaluable" if fires is None else ("fires" if fires else "does_not_fire")

        i = None
        if dawn_p999 is not None and metal_p999 is not None:
            i = (dawn_p999 > t["i_dawn_p999_fraction_gt"] * period and
                 metal_p999 < t["i_metal_p999_fraction_lt"] * period)
        ii = None
        if dawn_cpu is not None and metal_cpu is not None:
            ii = (dawn_cpu > t["ii_cpu_ratio_gt"] * metal_cpu and
                  dawn_cpu > t["ii_dawn_cpu_fraction_gt"] * period)
        iii = None
        iii_basis = "not_evaluable"
        if metal_wake is not None and metal_wake >= t["iii_metal_wake_p99_ns_lt"]:
            iii, iii_basis = False, "metal_wake_not_below_threshold"
        elif dawn_wake is not None and metal_wake is not None:
            iii = dawn_wake > t["iii_dawn_wake_p99_ns_gt"]
            iii_basis = "direct"
        elif dawn_wake_bound is not None and dawn_wake_bound <= t["iii_dawn_wake_p99_ns_gt"]:
            iii, iii_basis = False, "dawn_upper_bound_within_threshold"
        iv = None
        if load == "render" and dawn_miss is not None and metal_miss is not None:
            iv = (dawn_miss > t["iv_dawn_miss_rate_gt"] and
                  metal_miss == t["iv_metal_miss_rate_eq"])
        out[load] = {
            "period_ns": period,
            "i": {"state": state(i), "dawn_p999_ns": dawn_p999, "dawn_lane": dawn_p999_lane,
                  "metal_p999_ns": metal_p999, "metal_lane": metal_p999_lane},
            "ii": {"state": state(ii), "dawn_cpu_mean_ns": dawn_cpu, "dawn_lane": dawn_cpu_lane,
                   "metal_cpu_mean_ns": metal_cpu, "metal_lane": metal_cpu_lane},
            "iii": {"state": state(iii), "basis": iii_basis, "dawn_wake_p99_ns": dawn_wake,
                    "dawn_lane": dawn_wake_lane, "dawn_wake_upper_bound_p99_ns": dawn_wake_bound,
                    "dawn_bound_lane": dawn_bound_lane, "metal_wake_p99_ns": metal_wake,
                    "metal_lane": metal_wake_lane},
            "iv": {"state": state(iv) if load == "render" else "render_load_only",
                   "dawn_miss_rate": dawn_miss, "dawn_lane": dawn_miss_lane,
                   "metal_miss_rate": metal_miss, "metal_lane": metal_miss_lane},
        }
    criteria = {}
    for name in ("i", "ii", "iii", "iv"):
        states = [out[load][name]["state"] for load in REQUIRED_LOADS
                  if out[load][name]["state"] != "render_load_only"]
        criteria[name] = ("fires" if "fires" in states else
                          "not_evaluable" if "not_evaluable" in states else "does_not_fire")
    return {"per_load": out, "criteria": criteria}


def evaluate(receipts: list[dict[str, Any]],
             min_blocks: int = MIN_COUNTED_BLOCKS) -> dict[str, Any]:
    assessments = [assess(r, min_blocks) for r in receipts]
    campaigns = []
    for a in assessments:
        campaigns.append({
            "refusals": a["refusals"], "inconclusive": a["inconclusive"],
            "counted": a["counted"], "host_id": a["host_id"],
            "dawn_revision": a["dawn_revision"],
            "thresholds": evaluate_thresholds(a) if not a["refusals"] else None,
            "metrics": a["metrics"],
        })
    reasons: list[str] = []
    if any(c["refusals"] for c in campaigns):
        verdict = "REFUSED"
        reasons = sorted({r for c in campaigns for r in c["refusals"]})
    else:
        counted = [c for c in campaigns if c["counted"]]
        if len(counted) < THRESHOLDS["consecutive_quiet_campaigns"]:
            verdict = "INCONCLUSIVE"
            reasons = sorted({r for c in campaigns for r in c["inconclusive"]})
            if len(counted) < THRESHOLDS["consecutive_quiet_campaigns"]:
                reasons.append(f"counted_campaigns_{len(counted)}_of_"
                               f"{THRESHOLDS['consecutive_quiet_campaigns']}")
        elif len({(c["host_id"], c["dawn_revision"]) for c in counted}) != 1:
            verdict = "INCONCLUSIVE"
            reasons = ["campaigns_not_same_host_and_dawn_revision"]
        else:
            fired = [name for name in ("i", "ii", "iii", "iv")
                     if all(c["thresholds"]["criteria"][name] == "fires" for c in counted)]
            unknown = [name for name in ("i", "ii", "iii", "iv")
                       if any(c["thresholds"]["criteria"][name] == "not_evaluable"
                              for c in counted)]
            if fired:
                verdict = "GATE_FIRES"
                reasons = [f"criterion_{name}_fires_in_all_counted_campaigns" for name in fired]
            elif unknown:
                verdict = "INCONCLUSIVE"
                reasons = [f"criterion_{name}_not_evaluable" for name in unknown]
            else:
                verdict = "GATE_DOES_NOT_FIRE"
    return {
        "schema": EVALUATION_SCHEMA,
        "verdict": verdict,
        "reasons": reasons,
        "thresholds": THRESHOLDS,
        "campaigns": campaigns,
    }


# ---------------------------------------------------------------------------
# Running and binding


def _git(*args: str) -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO_ROOT), *args], check=True,
                              capture_output=True, text=True, encoding="utf-8").stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def run_binary(binary: Path, output: Path, extra: list[str]) -> int:
    command = [str(binary), "--output", str(output), *extra]
    print("+ " + " ".join(command), file=sys.stderr, flush=True)
    return subprocess.run(command, check=False).returncode


def run_preflight(path: Path, errors: list[str]) -> dict[str, Any] | None:
    result = subprocess.run([sys.executable, str(PREFLIGHT), "--output", str(path)],
                            check=False, capture_output=True, text=True, encoding="utf-8")
    print(result.stdout.strip() or result.stderr.strip(), file=sys.stderr)
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        errors.append(result.stderr.strip() or f"preflight exited {result.returncode}")
        return None


def bind(raw: dict[str, Any], binary: Path, preflight: dict[str, Any] | None,
         preflight_errors: list[str] | None = None) -> dict[str, Any]:
    receipt = dict(raw)
    receipt["host_preflight_errors"] = list(preflight_errors or [])
    receipt["host_preflight"] = preflight
    receipt["host_preflight_sha256"] = (sha256_hex(canonical(preflight))
                                        if preflight is not None else None)
    receipt["host_quiet"] = host_quiet(preflight)
    receipt["source_revision"] = _git("rev-parse", "HEAD")
    receipt["source_dirty"] = bool(_git("status", "--porcelain", "--untracked-files=no"))
    receipt["binary_sha256"] = sha256_hex(binary.read_bytes())
    receipt["host"] = {"node": platform.node(), "machine": platform.machine(),
                       "os": platform.platform()}
    receipt["assembled_at"] = datetime.now(timezone.utc).isoformat()
    receipt["receipt_sha256"] = receipt_digest(receipt)
    return receipt


def _fmt_us(value: float | None) -> str:
    return "-" if value is None else f"{value / 1000:.1f}"


def print_summary(evaluation: dict[str, Any]) -> None:
    for index, campaign in enumerate(evaluation["campaigns"]):
        print(f"campaign {index}: counted={campaign['counted']} "
              f"inconclusive={campaign['inconclusive']} refusals={campaign['refusals']}")
        header = ("lane", "fr", "load", "s2v p50", "p99", "p99.9", "max", "gpu p50",
                  "cpu mean", "wake p99", "bound p99", "miss", "ui p99", "drop")
        print("  " + " | ".join(header) + "   (us unless noted)")
        for m in sorted(campaign["metrics"], key=lambda m: (m["frames"], m["load"], m["lane"])):
            s = m["submit_to_visible_ns"]
            print("  " + " | ".join([
                m["lane"], str(m["frames"]), m["load"], _fmt_us(s["p50"]), _fmt_us(s["p99"]),
                _fmt_us(s["p999"]), _fmt_us(s["max"]), _fmt_us(m["gpu_busy_ns"]["p50"]),
                _fmt_us(m["worker_cpu_ns"]["mean"]), _fmt_us(m["wake_ns"]["p99"]),
                _fmt_us(m["wake_upper_bound_ns"]["p99"]), f"{m['misses']}/{m['blocks']}",
                _fmt_us(m["render"]["frame_ns"]["p99"]) if m["render"]["active"] else "-",
                str(m["render"]["dropped_frames"]) if m["render"]["active"] else "-",
            ]))
        if campaign["thresholds"]:
            print(f"  PROPOSED criteria at {GATE_FRAMES} frames: "
                  f"{campaign['thresholds']['criteria']}")
    print(f"verdict: {evaluation['verdict']} reasons={evaluation['reasons']} "
          f"(thresholds {THRESHOLDS['status']}, not ratified)")


def cmd_run(args: argparse.Namespace) -> int:
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        raw_path = Path(scratch) / "raw.json"
        errors: list[str] = []
        before = (None if args.skip_preflight
                  else run_preflight(Path(scratch) / "before.json", errors))
        extra = ["--blocks", str(args.blocks), "--warmup", str(args.warmup)]
        if args.frames:
            extra += ["--frames", args.frames]
        if args.loads:
            extra += ["--loads", args.loads]
        if args.lanes:
            extra += ["--lanes", args.lanes]
        code = run_binary(args.binary, raw_path, extra)
        if code == 77:
            return 77
        after = (None if args.skip_preflight
                 else run_preflight(Path(scratch) / "after.json", errors))
        preflight = ({"before": before, "after": after}
                     if before is not None and after is not None else None)
        try:
            raw = json.loads(raw_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            print(f"comparator produced no receipt: {exc}", file=sys.stderr)
            return 1
    receipt = bind(raw, args.binary, preflight, errors)
    args.out.write_text(json.dumps(receipt, sort_keys=True) + "\n", encoding="utf-8")
    evaluation = evaluate([receipt])
    evaluation_path = args.out.with_suffix(".evaluation.json")
    evaluation_path.write_text(json.dumps(evaluation, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
    print_summary(evaluation)
    print(f"receipt: {args.out}\nevaluation: {evaluation_path}")
    return 0 if code == 0 and evaluation["verdict"] != "REFUSED" else 1


def cmd_smoke(args: argparse.Namespace) -> int:
    args.out_dir.mkdir(parents=True, exist_ok=True)
    matched_raw = args.out_dir / "matched.raw.json"
    code = run_binary(args.binary, matched_raw, ["--smoke"])
    if code == 77:
        return 77
    if code != 0:
        print(f"matched smoke run failed with exit {code}", file=sys.stderr)
        return 1
    matched = bind(json.loads(matched_raw.read_text(encoding="utf-8")), args.binary, None)
    matched_eval = evaluate([matched])
    # Planted negative: the Metal lane runs the old unmatched complex_multiply
    # kernel; the receipt must be refused on its kernel digest.
    planted_raw = args.out_dir / "planted.raw.json"
    code = run_binary(args.binary, planted_raw,
                      ["--smoke", "--plant-unmatched-kernel", "--frames", "64",
                       "--loads", "idle", "--lanes",
                       "metal_shared_event,dawn_imported_timed_wait_any"])
    if code != 0:
        print(f"planted run did not detect the unmatched kernel (exit {code})", file=sys.stderr)
        return 1
    planted = bind(json.loads(planted_raw.read_text(encoding="utf-8")), args.binary, None)
    planted_eval = evaluate([planted])
    (args.out_dir / "matched.evaluation.json").write_text(
        json.dumps(matched_eval, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.out_dir / "planted.evaluation.json").write_text(
        json.dumps(planted_eval, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print_summary(matched_eval)
    ok = True
    if matched_eval["verdict"] == "REFUSED":
        print(f"matched smoke receipt refused: {matched_eval['reasons']}", file=sys.stderr)
        ok = False
    if matched_eval["verdict"] != "INCONCLUSIVE" or "undersampled" not in matched_eval["reasons"]:
        print("matched smoke receipt was not marked undersampled", file=sys.stderr)
        ok = False
    if planted_eval["verdict"] != "REFUSED" or not any(
            r.startswith("kernel_digest_mismatch") for r in planted_eval["reasons"]):
        print(f"planted unmatched kernel was not refused: {planted_eval['reasons']}",
              file=sys.stderr)
        ok = False
    print(f"planted verdict: {planted_eval['verdict']} reasons={planted_eval['reasons']}")
    return 0 if ok else 1


def cmd_verify(args: argparse.Namespace) -> int:
    receipts = [json.loads(Path(p).read_text(encoding="utf-8")) for p in args.receipts]
    evaluation = evaluate(receipts, args.min_blocks)
    if args.output:
        args.output.write_text(json.dumps(evaluation, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
    print_summary(evaluation)
    return 3 if evaluation["verdict"] == "REFUSED" else 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="run a bound campaign receipt")
    run.add_argument("--binary", type=Path, required=True)
    run.add_argument("--out", type=Path, required=True)
    run.add_argument("--blocks", type=int, default=MIN_COUNTED_BLOCKS)
    run.add_argument("--warmup", type=int, default=512)
    run.add_argument("--frames")
    run.add_argument("--loads")
    run.add_argument("--lanes")
    run.add_argument("--skip-preflight", action="store_true")
    run.set_defaults(func=cmd_run)
    smoke = sub.add_parser("smoke", help="ctest smoke plus planted negative")
    smoke.add_argument("--binary", type=Path, required=True)
    smoke.add_argument("--out-dir", type=Path, required=True)
    smoke.set_defaults(func=cmd_smoke)
    verify = sub.add_parser("verify", help="evaluate one or two receipts")
    verify.add_argument("receipts", nargs="+")
    verify.add_argument("--output", type=Path)
    verify.add_argument("--min-blocks", type=int, default=MIN_COUNTED_BLOCKS)
    verify.set_defaults(func=cmd_verify)
    return parser.parse_args(argv)


if __name__ == "__main__":
    arguments = parse_args(sys.argv[1:])
    raise SystemExit(arguments.func(arguments))
