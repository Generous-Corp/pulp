#!/usr/bin/env python3
"""Physical book targets reach real Rack knob values through patch.py."""

from __future__ import annotations

import copy
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P  # noqa: E402
import param_units  # noqa: E402


# Measured from Fundamental/VCO. Rack stores pitch as semitones from C4 while
# displaying Hz, so 440 Hz must be written near +9 rather than as raw 440.
VCO_FREQUENCY = {
    "id": 0, "name": "Frequency",
    "min": -54.0, "max": 54.0, "default": 0.0,
    "minValue": -54.0, "maxValue": 54.0, "defaultValue": 0.0,
    "unit": " Hz", "displayBase": 1.059463,
    "displayMultiplier": 261.62561, "displayOffset": 0.0,
}


def check(ok: bool, claim: str, detail: str = "") -> int:
    if ok:
        print(f"  ok     {claim}")
        return 0
    print(f"  WRONG  {claim}" + (f": {detail}" if detail else ""))
    return 1


def inventory() -> dict:
    return {"Fundamental": {"name": "Fundamental", "version": "2.6.4",
            "modules": {"VCO": {"name": "VCO", "description": "",
                                    "tags": ["Oscillator"],
                                    "params": [copy.deepcopy(VCO_FREQUENCY)]}}}}


def test_real_differing_unit_is_written_as_a_knob_position() -> int:
    inv = inventory()
    patch = {"modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO",
                           "params": [{"id": 0, "physical": 440.0,
                                       "unit": "Hz"}]}]}
    errs = P.place_physical_targets(patch, inv)
    target = patch["modules"][0]["params"][0]
    bad = check(not errs, "a measured 440 Hz target is accepted", str(errs))
    bad += check(set(target) == {"id", "value"},
                 "the transient physical fields never reach the Rack patch",
                 str(target))
    bad += check(math.isclose(target.get("value", -1000), 9.0,
                              rel_tol=0, abs_tol=2e-4),
                 "440 Hz lands at Fundamental/VCO's nine-semitone knob position",
                 str(target))
    bad += check(not math.isclose(target.get("value", 0), 440.0),
                 "the physical number is not copied into the raw value")
    return bad


def test_refusals_are_atomic() -> int:
    inv = inventory()
    raw = {"modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO",
                          "params": [{"id": 0, "value": 2.9}]}]}
    raw_before = copy.deepcopy(raw)
    raw_errs = P.place_physical_targets(raw, inv)
    bad = check(not raw_errs and raw == raw_before,
                "ordinary raw Rack targets remain untouched", str(raw_errs))

    patch = {"modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO",
                           "params": [
                               {"id": 0, "physical": 440.0, "unit": "Hz"},
                               {"id": 0, "physical": 440.0, "unit": "ms"},
                           ]}]}
    before = copy.deepcopy(patch)
    errs = P.place_physical_targets(patch, inv)
    bad += check(any("not ms" in e for e in errs),
                 "a target in the wrong unit is refused", str(errs))
    bad += check(patch == before,
                 "one refused target prevents every partial conversion")

    unreachable = {"modules": [{"id": 1, "plugin": "Fundamental",
                                 "model": "VCO", "params": [
                                     {"id": 0, "physical": 1e30,
                                      "unit": "Hz"}]}]}
    errs = P.place_physical_targets(unreachable, inv)
    bad += check(any("cannot reach" in e for e in errs),
                 "an out-of-range physical target is refused, not clamped",
                 str(errs))
    return bad


def test_cello_rate_uses_our_manifest_shape_and_round_trips() -> int:
    inv = {"ForgeModular": {"name": "Forge Modular", "version": "0.0.0",
                            "modules": {"LFO": {"name": "LFO",
                                                "description": "",
                                                "tags": []}}}}
    P._add_port_names(inv)
    rate = inv["ForgeModular"]["modules"]["LFO"]["params"][0]
    patch = {"modules": [{"id": 1, "plugin": "ForgeModular", "model": "LFO",
                           "params": [{"id": 0, "physical": 7.5,
                                       "unit": "Hz"}]}]}
    errs = P.place_physical_targets(patch, inv)
    raw = patch["modules"][0]["params"][0].get("value")
    shown = param_units.to_display(raw, rate) if raw is not None else None
    bad = check(not errs, "the cello's 7.5 Hz rate is accepted", str(errs))
    bad += check(math.isclose(raw if raw is not None else -1000,
                              1.906890596, rel_tol=0, abs_tol=1e-7),
                 "7.5 Hz becomes Forge LFO's ~1.9069 raw knob value", str(raw))
    bad += check(math.isclose(shown if shown is not None else -1000,
                              7.5, rel_tol=0, abs_tol=1e-7),
                 "the written knob value converts forward to 7.5 Hz", str(shown))
    bad += check(not math.isclose(param_units.to_display(2.9, rate) or 0,
                                  7.5, rel_tol=0, abs_tol=0.01),
                 "the prior raw 2.9 value is explicitly not the 7.5 Hz target")
    return bad


def test_inventory_tells_the_model_the_physical_form() -> int:
    text = P.render_inventory(inventory())
    bad = check("physical" in text and "Hz" in text,
                "measured physical units reach the model inventory", text)
    contract = open(P.CONTRACT).read()
    bad += check('"physical": 40' in contract and '"unit": "Hz"' in contract,
                 "the model contract names the physical target shape")
    return bad


def main() -> int:
    bad = 0
    for fn in (test_real_differing_unit_is_written_as_a_knob_position,
               test_refusals_are_atomic,
               test_cello_rate_uses_our_manifest_shape_and_round_trips,
               test_inventory_tells_the_model_the_physical_form):
        print(f"{fn.__name__}:")
        bad += fn()
    print("\n" + ("all good" if bad == 0 else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
