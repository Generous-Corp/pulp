#!/usr/bin/env python3
"""Run and retain the synchronized Rack proof for one acid patch.

This is orchestration, not another evaluator.  ``acid_taps`` decides whether
the patch has one unambiguous eight-signal witness plan, ``fidelity`` runs all
eight signals on one Rack sample clock, and ``acid_behavior`` owns the musical
behavior verdict.  Every input to that verdict is retained beside it.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import os
import shutil
import sys
import tempfile
from typing import Callable

import acid_behavior
import acid_taps
import fidelity
import measure_ranges


SCHEMA = "forge.acid-runtime-proof.v1"
DEFAULT_SECONDS = 8.0


def _write_json(path: str, value) -> str:
    with open(path, "w", encoding="utf-8") as out:
        json.dump(value, out, indent=2, sort_keys=True)
        out.write("\n")
    return os.path.abspath(path)


def _unmeasured(reason: str) -> dict:
    return {
        "verdict": acid_taps.UNMEASURED,
        "scope": "acid-behavior",
        "reasons": [reason],
        "structural_failures": [],
        "measurement_failures": [reason],
        "behavior_failures": [],
        "observations": {},
    }


def _failed(reasons: list[str]) -> dict:
    return {
        "verdict": acid_taps.FAIL,
        "scope": "acid-behavior",
        "reasons": list(reasons),
        "structural_failures": list(reasons),
        "measurement_failures": [],
        "behavior_failures": [],
        "observations": {},
    }


def _capture(path: str, frames: list[list[float]], order: tuple[str, ...]
             ) -> tuple[str, dict]:
    """Retain channel-major little-endian float32 without lossy JSON bloat."""
    values = array.array("f")
    lengths = []
    offsets = []
    for channel in frames:
        offsets.append(len(values))
        lengths.append(len(channel))
        values.extend(float(value) for value in channel)
    if sys.byteorder != "little":
        values.byteswap()
    payload = values.tobytes()
    with open(path, "wb") as out:
        out.write(payload)
    return os.path.abspath(path), {
        "encoding": "channel-major-float32-le",
        "channel_order": list(order),
        "channel_lengths": lengths,
        "channel_offsets_float32": offsets,
        "sha256": hashlib.sha256(payload).hexdigest(),
    }


def evaluate(
        patch: dict, inv: dict, artifact_dir: str, *,
        seconds: float = DEFAULT_SECONDS,
        planner: Callable = acid_taps.plan,
        rack_finder: Callable = measure_ranges.rack_binary,
        probe_builder: Callable = fidelity.build_probe,
        instrumenter: Callable = fidelity.instrument,
        runner: Callable = fidelity.run,
        structural_checker: Callable = fidelity.structural_diff,
        behavior_evaluator: Callable = acid_behavior.evaluate) -> dict:
    """Return PASS/FAIL/UNMEASURED and retain the evidence behind it.

    Dependency arguments are deliberate: CI can prove orchestration and
    verdict propagation without Rack.  Production callers use the real APIs.
    An unavailable instrument is UNMEASURED and is never converted into a
    patch defect.
    """
    artifacts: dict[str, str] = {}
    try:
        os.makedirs(artifact_dir, exist_ok=True)
    except OSError as exc:
        result = _unmeasured(f"acid proof artifacts could not be retained: {exc}")
        result["artifacts"] = {}
        return result

    artifacts["source_patch"] = _write_json(
        os.path.join(artifact_dir, "acid-source.vcv"), patch)
    try:
        plan = planner(patch, inv)
    except (Exception, SystemExit) as exc:
        result = _unmeasured(f"the acid tap plan could not be built: {exc}")
        result["artifacts"] = artifacts
        artifacts["verdict"] = _write_json(
            os.path.join(artifact_dir, "acid-verdict.json"), result)
        return result
    artifacts["plan"] = _write_json(
        os.path.join(artifact_dir, "acid-plan.json"), plan.as_dict())
    if plan.status != acid_taps.READY or plan.failures:
        result = behavior_evaluator(plan, {})
        result["artifacts"] = artifacts
        artifacts["verdict"] = _write_json(
            os.path.join(artifact_dir, "acid-verdict.json"), result)
        return result

    rack = rack_finder()
    if not rack:
        result = _unmeasured("no Rack executable is available for the acid proof")
        result["artifacts"] = artifacts
        artifacts["verdict"] = _write_json(
            os.path.join(artifact_dir, "acid-verdict.json"), result)
        return result

    stage = tempfile.mkdtemp(prefix="acid-runtime-")
    try:
        try:
            probe = probe_builder(stage)
            given, used = instrumenter(patch, plan.taps)
            if used != plan.taps:
                result = _unmeasured(
                    "the fidelity probe did not preserve the complete acid tap plan")
            else:
                run_patch = os.path.join(stage, "acid-under-test.vcv")
                _write_json(run_patch, given)
                artifacts["instrumented_patch"] = _write_json(
                    os.path.join(artifact_dir, "acid-instrumented.vcv"), given)
                got = runner(rack, run_patch, probe, seconds)
                artifacts["rack_log"] = os.path.abspath(
                    os.path.join(artifact_dir, "acid-rack.log"))
                with open(artifacts["rack_log"], "w", encoding="utf-8") as out:
                    out.write(got.log or "")
                if got.engine is not None:
                    artifacts["engine"] = _write_json(
                        os.path.join(artifact_dir, "acid-engine.json"), got.engine)
                if got.signal is not None:
                    artifacts["signal"] = _write_json(
                        os.path.join(artifact_dir, "acid-signal.json"), got.signal)
                if got.frames:
                    raw_path, raw_meta = _capture(
                        os.path.join(artifact_dir, "acid-capture.f32"),
                        got.frames, acid_taps.ORDER[:len(got.frames)])
                    artifacts["capture"] = raw_path
                    raw_meta.update({
                        "schema": SCHEMA,
                        "sample_rate": (got.signal or {}).get("sampleRate"),
                        "frames": (got.signal or {}).get("frames"),
                        "tap_plan": plan.as_dict(),
                    })
                    artifacts["capture_metadata"] = _write_json(
                        os.path.join(artifact_dir, "acid-capture.json"),
                        raw_meta)

                if got.why:
                    result = _unmeasured(
                        f"the synchronized Rack capture did not complete: {got.why}")
                elif got.engine is None:
                    result = _unmeasured(
                        "Rack produced samples but no engine snapshot; structural "
                        "fidelity is unmeasured")
                else:
                    structural = structural_checker(given, got.engine)
                    if structural:
                        result = _failed(structural)
                    else:
                        series = {
                            name: got.frames[index]
                            for index, name in enumerate(acid_taps.ORDER)
                            if index < len(got.frames)
                        }
                        result = behavior_evaluator(plan, series)
        except (Exception, SystemExit) as exc:                 # measurement boundary
            result = _unmeasured(f"the synchronized acid proof could not run: {exc}")
    finally:
        shutil.rmtree(stage, ignore_errors=True)

    result["artifacts"] = artifacts
    # Write last so this is a durable index of every artifact retained above.
    artifacts["verdict"] = _write_json(
        os.path.join(artifact_dir, "acid-verdict.json"), result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=("Replay one acid patch through the synchronized real-Rack "
                     "behavior gate without a model call."))
    parser.add_argument("patch", help="source .vcv patch to validate")
    parser.add_argument("--artifact-dir", required=True,
                        help="directory that will retain the complete proof")
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS,
                        help=f"Rack capture duration (default: {DEFAULT_SECONDS:g})")
    args = parser.parse_args(argv)
    if args.seconds <= 0:
        parser.error("--seconds must be greater than zero")
    try:
        with open(args.patch, encoding="utf-8") as source:
            patch_document = json.load(source)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        print(f"acid-runtime-gate: cannot read {args.patch}: {exc}", file=sys.stderr)
        return 2

    # Import lazily so importing this module in the build-gate tests does not
    # pay for or depend on the user's installed Rack catalogue.
    import patch as patch_module
    result = evaluate(patch_document, patch_module.inventory(),
                      args.artifact_dir, seconds=args.seconds)
    print(json.dumps({
        "verdict": result.get("verdict"),
        "reasons": result.get("reasons") or [],
        "artifacts": result.get("artifacts") or {},
    }, indent=2, sort_keys=True))
    verdict = result.get("verdict")
    if verdict == acid_taps.PASS:
        return 0
    if verdict == acid_taps.FAIL:
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
