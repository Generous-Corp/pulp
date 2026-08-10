#!/usr/bin/env python3
"""Real-Rack negative control for the generalized stereo contract."""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P                                             # noqa: E402


def main() -> int:
    inv = P.inventory()
    required = (("ForgeModular", "VCO"), ("ForgeModular", "VCA"),
                ("Core", "AudioInterface2"))
    missing = [f"{plugin}/{model}" for plugin, model in required
               if model not in (inv.get(plugin, {}).get("modules") or {})]
    if missing:
        print("FAIL missing real-Rack fixture modules: " + ", ".join(missing))
        return 1

    candidate = {"version": "2.6.6", "modules": [
        {"id": 1, "plugin": "ForgeModular", "model": "VCO",
         "pos": [0, 0], "params": [{"id": 0, "value": -2.0}]},
        {"id": 2, "plugin": "ForgeModular", "model": "VCA",
         "pos": [10, 0], "params": [{"id": 0, "value": 0.5}]},
        {"id": 3, "plugin": "Core", "model": "AudioInterface2",
         "pos": [20, 0]},
    ], "cables": [
        {"id": 1, "outputModuleId": 1, "outputId": 0,
         "inputModuleId": 3, "inputId": 0, "color": "#e6c229"},
        {"id": 2, "outputModuleId": 2, "outputId": 0,
         "inputModuleId": 3, "inputId": 1, "color": "#29b6e6"},
    ]}
    candidate, lint_errors = P.prepare_and_lint(candidate, inv)
    if lint_errors:
        print("FAIL fixture does not lint: " + "; ".join(lint_errors))
        return 1

    verdict, report = P.audibility(candidate)
    if verdict != P.AUDIBLE:
        print("FAIL positive side did not make the overall Rack gate audible")
        print(report)
        return 1

    idiom = {"slug": "stereo-spread", "runtime_contract": {
        "kind": "live_output_lanes", "requirements": ["left", "differs"],
        "min_mean_abs_v": 0.0001, "max_abs_correlation": 0.995}}
    errors = P.idiom_runtime_contract_errors(candidate, idiom, report)
    if not any("VCA out 0 requires mean_abs_v" in error for error in errors):
        print("FAIL one live side masked the silent stereo side")
        print(report)
        return 1
    mono = {"version": "2.6.6", "modules": [
        {"id": 1, "plugin": "ForgeModular", "model": "VCO",
         "pos": [0, 0], "params": [{"id": 0, "value": -2.0}]},
        {"id": 2, "plugin": "Fundamental", "model": "Mult",
         "pos": [10, 0]},
        {"id": 3, "plugin": "Core", "model": "AudioInterface2",
         "pos": [20, 0]},
    ], "cables": [
        {"id": 1, "outputModuleId": 1, "outputId": 0,
         "inputModuleId": 2, "inputId": 0, "color": "#e6c229"},
        {"id": 2, "outputModuleId": 2, "outputId": 0,
         "inputModuleId": 3, "inputId": 0, "color": "#e6c229"},
        {"id": 3, "outputModuleId": 2, "outputId": 1,
         "inputModuleId": 3, "inputId": 1, "color": "#29b6e6"},
    ]}
    mono, lint_errors = P.prepare_and_lint(mono, inv)
    if lint_errors:
        print("FAIL mono-copy fixture does not lint: " + "; ".join(lint_errors))
        return 1
    verdict, report = P.audibility(mono)
    errors = P.idiom_runtime_contract_errors(mono, idiom, report)
    if verdict != P.AUDIBLE or not any("effectively mono" in e for e in errors):
        print("FAIL two live exact copies passed as stereo")
        print(report)
        return 1
    print("2/2 real-Rack stereo negative controls passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
