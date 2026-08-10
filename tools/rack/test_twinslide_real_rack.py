#!/usr/bin/env python3
"""Opt-in exact-version TwinSlide authoring -> real Rack/DSP proof."""

from __future__ import annotations

import copy
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P  # noqa: E402
from test_acid_preflight import twinslide_pattern  # noqa: E402


def source_patch() -> dict:
    return {
        "version": "2.6.6",
        "modules": [
            {"id": 1, "plugin": "ForgeModular", "model": "LFO",
             "pos": [0, 0], "params": [{"id": 0, "value": 0.0}]},
            {"id": 2, "plugin": "TwinSlide", "model": "TwinSlide",
             "pos": [6, 0], "params": [
                 {"id": 13, "value": 0.72},
                 {"id": 58, "value": 0.36},
                 {"id": 59, "value": 0.76},
                 {"id": 60, "value": 0.72},
                 {"id": 61, "value": 0.42},
                 {"id": 62, "value": 0.72},
                 {"id": 67, "value": 0.57},
                 {"id": 68, "value": 0.68},
                 {"id": 69, "value": 0.44},
                 {"id": 70, "value": 0.69},
                 {"id": 71, "value": 0.62}],
             "data": {"forgePattern": twinslide_pattern()}},
            {"id": 3, "plugin": "Core", "model": "AudioInterface2",
             "pos": [38, 0]},
        ],
        "cables": [
            {"id": 1, "outputModuleId": 1, "outputId": 1,
             "inputModuleId": 2, "inputId": 1, "color": "#ffb437"},
            {"id": 2, "outputModuleId": 2, "outputId": 4,
             "inputModuleId": 3, "inputId": 0, "color": "#00b56e"},
            {"id": 3, "outputModuleId": 2, "outputId": 5,
             "inputModuleId": 3, "inputId": 1, "color": "#00b56e"},
        ],
    }


def cli_replay() -> pathlib.Path:
    """Exercise saved response -> public CLI -> all real gates -> .vcv."""
    response_text = (
        "```json patch\n" + json.dumps(source_patch()) + "\n```\n"
        "```json why\n{}\n```\n")
    root = pathlib.Path(tempfile.mkdtemp(prefix="forge-twinslide-cli-"))
    # Keep the directory on failure so the exact replay remains inspectable.
    cli_replay._root = root
    response = root / "model-response.txt"
    output = root / "twinslide-cli.vcv"
    response.write_text(response_text, encoding="utf-8")
    env = os.environ.copy()
    env["FORGE_ATTEMPT_DIR"] = str(root / "attempts")
    prompt = ("Build exactly three modules: @ForgeModular/LFO, "
              "@TwinSlide/TwinSlide, and @Core/AudioInterface2. Create two "
              "contrasting synchronized acid melodies in TwinSlide with "
              "audible accents and slides, clocked by the LFO, and route "
              "TwinSlide outputs A and B to stereo.")
    completed = subprocess.run(
        [sys.executable, os.path.join(HERE, "patch.py"), "build", prompt,
         "--response-file", str(response), "--out", str(output)],
        cwd=HERE, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=180, check=False)
    if completed.returncode != 0 or not output.is_file():
        raise RuntimeError(
            "full saved-response CLI replay failed:\n" + completed.stdout)
    print(completed.stdout, end="")
    return output


def measured_outputs(patch: dict) -> tuple[str, dict]:
    P.configure_audio(patch)
    verdict, report = P.audibility(patch)
    behavior = P._behaviour_json(report)
    if behavior is None:
        raise RuntimeError(f"real Rack emitted no behavior JSON:\n{report}")
    return verdict, {entry["source"]: entry
                     for entry in behavior.get("cables") or []}


def main(argv: list[str]) -> int:
    if "--with-rack" not in argv:
        print("TwinSlide real-Rack proof is opt-in; rerun with --with-rack")
        return 0
    inv = P.inventory()
    version = inv.get("TwinSlide", {}).get("version")
    if version != "2.1.6":
        print(f"WRONG: exact TwinSlide 2.1.6 is required; found {version!r}")
        return 1

    try:
        cli_output = cli_replay()
        with cli_output.open(encoding="utf-8") as source:
            cli_patch = json.load(source)
        cli_errors = P.lint(cli_patch, inv)
    except (OSError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"WRONG: {exc}")
        return 1
    if cli_errors:
        print("WRONG: public CLI output failed reparse:")
        print("\n".join(f"  {error}" for error in cli_errors))
        return 1
    lfo = next((module for module in cli_patch.get("modules") or []
                if module.get("plugin") == "ForgeModular" and
                module.get("model") == "LFO"), {})
    lfo_rate = next((param.get("value") for param in lfo.get("params") or []
                     if param.get("id") == 0), None)
    if lfo_rate != 0.0:
        print("WRONG: the self-contained TwinSlide capability was overwritten "
              f"by the generic acid witness; LFO raw rate is {lfo_rate!r}")
        return 1

    patch, errors = P.prepare_and_lint(source_patch(), inv)
    if errors:
        print("WRONG: authored patch failed static contract:")
        print("\n".join(f"  {error}" for error in errors))
        return 1
    P.configure_audio(patch)
    verdict, report = P.audibility(patch)
    if verdict != P.AUDIBLE:
        print(f"WRONG: real Rack verdict was {verdict}\n{report}")
        return 1
    runtime = P.module_runtime_contract_errors(patch, inv, report)
    if runtime:
        print("WRONG: real TwinSlide behavior missed its contract:")
        print("\n".join(f"  {error}" for error in runtime))
        return 1

    baseline_behavior = P._behaviour_json(report)
    assert baseline_behavior is not None
    baseline_outputs = {entry["source"]: entry
                        for entry in baseline_behavior.get("cables") or []}

    no_accent = copy.deepcopy(patch)
    no_accent_state = next(module["data"] for module in no_accent["modules"]
                           if module.get("plugin") == "TwinSlide")
    no_accent_state["attributes"] = [
        value & ~4 for value in no_accent_state["attributes"]]
    no_accent_verdict, no_accent_outputs = measured_outputs(no_accent)
    accent_changes_output = all(
        baseline_outputs[source]["peak_abs_v"]
        - no_accent_outputs[source]["peak_abs_v"] >= 0.25
        and baseline_outputs[source]["spectrum"]["centroid_mean_hz"]
        - no_accent_outputs[source]["spectrum"]["centroid_mean_hz"] >= 50.0
        for source in ("TwinSlide out 4", "TwinSlide out 5"))
    if no_accent_verdict != P.AUDIBLE or not accent_changes_output:
        print("WRONG: removing the encoded accent flags did not produce the "
              "expected measured dynamic and spectral change")
        return 1

    no_slide_dsp = copy.deepcopy(patch)
    no_slide_state = next(module["data"] for module in no_slide_dsp["modules"]
                          if module.get("plugin") == "TwinSlide")
    no_slide_state["attributes"] = [
        value & ~8 for value in no_slide_state["attributes"]]
    no_slide_verdict, no_slide_outputs = measured_outputs(no_slide_dsp)
    slide_changes_pitch_motion = all(
        baseline_outputs[source]["pitch"]["distinct_pitches"]
        > no_slide_outputs[source]["pitch"]["distinct_pitches"]
        and baseline_outputs[source]["pitch"]["pitch_changes"]
        > no_slide_outputs[source]["pitch"]["pitch_changes"]
        for source in ("TwinSlide out 4", "TwinSlide out 5"))
    if no_slide_verdict != P.AUDIBLE or not slide_changes_pitch_motion:
        print("WRONG: removing the encoded slide flags did not reduce measured "
              "pitch motion on both outputs")
        return 1

    markerless = copy.deepcopy(patch)
    state = next(module["data"] for module in markerless["modules"]
                 if module.get("plugin") == "TwinSlide")
    state.pop("forgeContract", None)
    if P._twinslide_compiled_state_errors(state):
        print("WRONG: markerless compiled state no longer reparses")
        return 1
    if P.module_runtime_contract_errors(markerless, inv, report):
        print("WRONG: markerless state did not retain the real DSP contract")
        return 1

    no_slide = source_patch()
    module = next(module for module in no_slide["modules"]
                  if module.get("plugin") == "TwinSlide")
    module["params"].append({"id": 13, "value": 0.0})
    _, negative = P.prepare_and_lint(no_slide, inv)
    if not any("param 13 in 0.1..2" in error for error in negative):
        print("WRONG: zero slide rate passed the prompt-derived static gate")
        return 1

    print("ok: saved response passed the public CLI and exact TwinSlide 2.1.6 "
          "authored state passed real Rack/DSP; matched controls proved audible "
          "accent and slide effects, markerless reparse, and zero-slide static "
          "rejection")
    replay_root = getattr(cli_replay, "_root", None)
    if replay_root is not None:
        shutil.rmtree(replay_root, ignore_errors=True)
        del cli_replay._root
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
