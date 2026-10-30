#!/usr/bin/env python3
"""Build and verify v2 GPU-fault DAW receipts for real hosts and REAPER.

This module owns receipt construction; real_host_smoke.py owns the GUI-host
lifecycle and the CLI that invokes it. REAPER wrapping copies an already
verified v1 run and re-evaluates the evidence without launching a DAW.
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]


def load_module(name: str, path: Path):
    import importlib.util
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


V2 = load_module("verify_gpu_audio_daw_continuity_receipt_v2",
                 REPO / "tools/validation/verify_gpu_audio_daw_continuity_receipt_v2.py")
gfc, hosts = V2.gfc, V2.hosts


def read_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def write_json(path: Path, value: Any) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(path)


def write_receipt(out: Path, *, control_kind: str, fault: str, fault_sample: int,
                   transport: str, nonce: str, host: dict, hosting_mode: str, settings: dict,
                   plugin: dict, code_commit: str, bounce: bool, realtime: bool,
                   tolerance: float, window_pre: int, window_post: int, render_frames: int,
                   host_alive: bool, extra_artifacts: tuple[str, ...] = ()
                   ) -> tuple[dict, str, str]:
    """Evaluate the run in `out`, write its v2 receipt there, and verify it."""
    names = ["stimulus.wav", "ir.f32", "events.jsonl", "capture.bin", "preflight.json",
             "claim", "host_evidence.json", *extra_artifacts]
    if bounce:
        names.append("bounce.wav")
    if realtime:
        names += ["realtime.wav", hosts.REALTIME_PROVENANCE]
    artifacts = gfc.artifact_digests(out, names)
    preflight_doc = gfc.summarize_preflight(read_json(out / "preflight.json", {}))
    evidence = hosts.evaluate_v2(
        out, control_kind=control_kind, fault=fault, fault_sample=fault_sample,
        tolerance=tolerance, window_pre=window_pre, window_post=window_post,
        host_alive=host_alive, bounce_path=(out / "bounce.wav") if bounce else None,
        realtime_path=(out / "realtime.wav") if realtime else None,
        render_frames=render_frames, nonce=nonce, host=host, hosting_mode=hosting_mode,
        host_quiet=preflight_doc["quiet_host"], code_under_test={"commit": code_commit},
        listed=list(artifacts))
    receipt = hosts.build_receipt_v2(
        control_kind=control_kind, trigger_transport=transport,
        nonce=nonce, host=host, hosting_mode=hosting_mode, host_settings=settings,
        plugin=plugin, code_under_test={"commit": code_commit}, artifacts=artifacts,
        bounce_artifact="bounce.wav" if bounce else None,
        host_realtime_artifact="realtime.wav" if realtime else None,
        preflight=preflight_doc, host_quiet=preflight_doc["quiet_host"],
        analysis={"tolerance": tolerance, "window": {"pre": window_pre, "post": window_post},
                  "render_frames": render_frames},
        column_outcome=hosts.column_outcome(evidence, control_kind=control_kind,
                                            host_id=host["id"]),
        **{k: evidence[k] for k in V2.RECOMPUTED})
    write_json(out / "receipt.json", receipt)
    verdict, outcome = V2.verify(receipt, out)
    return receipt, verdict, outcome


def _preflight_sampled_us(doc: dict) -> int | None:
    """When the preflight sampled the machine, from host_vitals' own
    `sampled_at` (whole seconds, so this rounds down). REAPER's harness takes
    the preflight before it launches REAPER, which makes this a lower bound
    on the launch."""
    for obs in doc.get("observations", []):
        if Path((obs.get("argv") or [""])[0]).name != "host_vitals.sh":
            continue
        try:
            return int(json.loads(obs.get("stdout", ""))["sampled_at"]) * 1_000_000
        except (ValueError, KeyError, TypeError):
            return None
    return None


def reaper_control_kind(controls: dict, fault_kind: str) -> str:
    """The v2 control kind of a v1 REAPER run, from what the plug-in armed."""
    if controls.get("planted_negative") or controls.get("gpu_off"):
        return "planted_negative"
    return hosts.POSITIVE_CONTROL if fault_kind == hosts.POSITIVE_CONTROL else "fault"


def wrap_reaper(reaper_run: Path, out: Path, *, code_commit: str) -> tuple[dict, str, str]:
    """A v2 receipt for a REAPER run that reaper_smoke.py already verified as
    v1, so REAPER rows read in the same terms as every other host (a planted
    negative is `negative_proved`, not just "fail"). Nothing is re-run: the
    v1 artifacts are copied and re-evaluated."""
    v1_receipt = json.loads((reaper_run / "receipt.json").read_text(encoding="utf-8"))
    V2.v1.verify(v1_receipt, reaper_run)
    if out.exists():
        raise SystemExit(f"wrap-reaper: {out} exists")
    out.mkdir(parents=True)
    for name in v1_receipt["artifacts"]:
        shutil.copyfile(reaper_run / name, out / name)
    launch = _preflight_sampled_us(read_json(out / "preflight.json", {}))
    if launch is None:
        raise SystemExit("wrap-reaper: preflight.json has no host_vitals sample to bound the "
                         "launch")
    v1_host, plugin = v1_receipt["host"], v1_receipt["plugin"]
    log_text = (out / "reaper.out.txt").read_text(encoding="utf-8", errors="replace")
    write_json(out / "host_evidence.json", {
        "plugins": [plugin["name"]] if plugin["name"] in log_text else [],
        "host_pid": v1_host["reaper_pid"], "host_executable": v1_host["reaper_bin"],
        "source": "reaper.out.txt"})
    host = {"id": "reaper", "version": v1_host["reaper_version"], "pid": v1_host["reaper_pid"],
            "responsible_pid": -1, "executable": v1_host["reaper_bin"], "engine_pids": [],
            "launch_unix_us": launch, "launch_source": "preflight host_vitals sampled_at",
            "binary_arch": v1_host.get("machine"), "launch": "reaper_smoke.py"}
    fault, analysis = v1_receipt["fault"], v1_receipt["analysis"]
    return write_receipt(
        out, control_kind=reaper_control_kind(v1_receipt["controls"], fault["kind"]),
        fault=fault["kind"],
        fault_sample=fault["requested_project_sample"],
        transport=v1_receipt["trigger_transport"], nonce=v1_receipt["nonce"], host=host,
        hosting_mode="in_process",
        settings={"template": "reaper_smoke",
                  "anticipative_fx": v1_receipt.get("anticipative_fx")},
        plugin={k: plugin[k] for k in ("format", "name", "executable_sha256")},
        code_commit=code_commit, bounce=v1_receipt.get("bounce_artifact") is not None,
        realtime=v1_receipt.get("host_realtime_artifact") is not None,
        tolerance=analysis["tolerance"], window_pre=analysis["window"]["pre"],
        window_post=analysis["window"]["post"], render_frames=analysis["render_frames"],
        host_alive=bool(v1_receipt["host_alive"]), extra_artifacts=("reaper.out.txt",))


