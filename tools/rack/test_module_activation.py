#!/usr/bin/env python3
"""Source-backed module activation contracts reject structurally valid silence."""

from __future__ import annotations

import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P  # noqa: E402


def inventory(version: str = "2.0.48") -> dict:
    def module(name: str) -> dict:
        return {"name": name, "description": "", "tags": ["Oscillator"],
                "inputs": [None] * 17, "outputs": ["Audio L", "Audio R"]}
    return {"CVfunk": {"name": "CV funk", "version": version,
                       "modules": {"Aulos": module("Aulos"),
                                   "Glass": module("Glass")}},
            "Core": {"name": "Core", "version": "2.6.6", "modules": {
                "AudioInterface2": {"name": "Audio 2", "description": "",
                                    "tags": ["Audio"],
                                    "inputs": ["L", "R"], "outputs": []}}},
            "Fundamental": {"name": "Fundamental", "version": "2.6.6",
                            "modules": {"LFO": {"name": "LFO",
                            "description": "", "tags": ["LFO"],
                            "inputs": [], "outputs": [None] * 4}}}}


def silent_chain() -> dict:
    modules = [
        {"id": 5, "plugin": "CVfunk", "model": "Aulos", "pos": [0, 0],
         "params": [{"id": 22, "value": 1.0}]},
        {"id": 6, "plugin": "CVfunk", "model": "Aulos", "pos": [20, 0],
         "params": [{"id": 22, "value": 1.0}]},
        {"id": 7, "plugin": "CVfunk", "model": "Glass", "pos": [40, 0]},
        {"id": 8, "plugin": "CVfunk", "model": "Glass", "pos": [60, 0]},
        {"id": 11, "plugin": "Fundamental", "model": "LFO", "pos": [80, 0]},
        {"id": 16, "plugin": "Fundamental", "model": "LFO", "pos": [90, 0]},
        {"id": 20, "plugin": "Core", "model": "AudioInterface2",
         "pos": [100, 0]},
    ]
    cables = [
        {"id": 1, "outputModuleId": 5, "outputId": 0,
         "inputModuleId": 6, "inputId": 13},
        {"id": 2, "outputModuleId": 6, "outputId": 0,
         "inputModuleId": 7, "inputId": 12},
        {"id": 3, "outputModuleId": 7, "outputId": 0,
         "inputModuleId": 8, "inputId": 12},
        {"id": 4, "outputModuleId": 8, "outputId": 0,
         "inputModuleId": 20, "inputId": 0},
        {"id": 5, "outputModuleId": 8, "outputId": 1,
         "inputModuleId": 20, "inputId": 1},
        {"id": 6, "outputModuleId": 16, "outputId": 3,
         "inputModuleId": 7, "inputId": 1},
        {"id": 7, "outputModuleId": 11, "outputId": 3,
         "inputModuleId": 8, "inputId": 1},
    ]
    return {"modules": modules, "cables": cables}


def check_module_activation_contracts() -> tuple[int, int]:
    bad, ran = 0, 5
    original = silent_chain()
    errors = P.module_activation_contract_errors(original, inventory())
    aulos = [error for error in errors if "Aulos" in error]
    glass = [error for error in errors if "Glass" in error]
    if len(aulos) != 2 or len(glass) != 2:
        bad += 1
        print(f"  WRONG  exact M5 silence topology was not fully rejected: {errors}")
    else:
        print("  ok     both dormant Aulos and both dormant Glass stages are rejected")

    repaired = copy.deepcopy(original)
    for cable in repaired["cables"]:
        if cable["id"] in (1, 2):
            cable["outputId"] = 1
    repaired["cables"] += [
        {"id": 8, "outputModuleId": 16, "outputId": 3,
         "inputModuleId": 7, "inputId": 0},
        {"id": 9, "outputModuleId": 11, "outputId": 3,
         "inputModuleId": 8, "inputId": 0},
    ]
    repaired_errors = P.module_activation_contract_errors(repaired, inventory())
    if repaired_errors:
        bad += 1
        print(f"  WRONG  activated topology is still rejected: {repaired_errors}")
    else:
        print("  ok     right-drone outputs plus Glass V/Oct activation pass")

    unused = {"modules": [original["modules"][2]], "cables": []}
    if P.module_activation_contract_errors(unused, inventory()):
        bad += 1
        print("  WRONG  an unused Glass is accused of silencing a signal path")
    else:
        print("  ok     an unused module does not create an activation false positive")

    if P.module_activation_contract_errors(original, inventory("2.0.49")):
        bad += 1
        print("  WRONG  an unknown vendor version inherited a guessed DSP contract")
    else:
        print("  ok     unknown vendor versions remain runtime-measured, not guessed")

    dormant_right = copy.deepcopy(original)
    dormant_right["cables"][0]["outputId"] = 1
    dormant_right["modules"][0]["params"] = []
    right_errors = P.module_activation_contract_errors(dormant_right, inventory())
    if not any("Audio R output 1" in error for error in right_errors):
        bad += 1
        print("  WRONG  a dormant Aulos right voice escaped activation checks")
    else:
        print("  ok     Aulos Audio R also requires Gate or explicit Drone state")
    return bad, ran


def main() -> int:
    bad, ran = check_module_activation_contracts()
    print(f"\n{ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
