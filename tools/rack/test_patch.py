#!/usr/bin/env python3
"""Corpus for the patch checks: what they must accept, what they must reject.

A check nobody has tested against real material is a liability rather than a
safeguard. Every gate written for this pipeline so far has been wrong on first
contact -- two manifest rules rejected correct modules, the behavioural gate
failed six of eleven working ones, and the capability preflight read "hat" out
of "that" and refused an ambient drone. All were found by running them, none
by reading them.

So each rule below is pinned twice: a patch that must pass, and a patch that
must fail *for that specific reason*. A rejection for the wrong reason is
counted as a failure here, because a check that rejects everything would
otherwise look perfect.

    python3 tools/rack/test_patch.py
"""
from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P  # noqa: E402


def mod(mid, plugin, model, pos=(0, 0), params=None):
    return {"id": mid, "plugin": plugin, "model": model,
            "pos": list(pos), "params": params or []}


def cable(cid, om, op, im, ip):
    return {"id": cid, "outputModuleId": om, "outputId": op,
            "inputModuleId": im, "inputId": ip, "color": "#3695ef"}


def voice(**over):
    """A patch that works: oscillator into filter into output."""
    p = {"version": "2.6.6",
         "modules": [mod(1, "Fundamental", "VCO"),
                     mod(2, "Fundamental", "VCF", (10, 0)),
                     mod(3, "Core", "AudioInterface2", (20, 0))],
         "cables": [cable(1, 1, 0, 2, 0), cable(2, 2, 0, 3, 0)]}
    p.update(over)
    return p


# (name, patch, expected-to-pass, substring the failure must mention)
CASES = [
    # ── must pass ────────────────────────────────────────────────────────────
    ("a plain working voice", voice(), True, None),
    ("our own modules end to end", {
        "version": "2.6.6",
        "modules": [mod(1, "ForgeModular", "VCO"),
                    mod(2, "ForgeModular", "VCA", (10, 0)),
                    mod(3, "Core", "AudioInterface2", (20, 0))],
        "cables": [cable(1, 1, 0, 2, 0), cable(2, 2, 0, 3, 0)]}, True, None),
    ("modules from several vendors mixed", {
        "version": "2.6.6",
        "modules": [mod(1, "ForgeModular", "LFO"),
                    mod(2, "Fundamental", "VCF", (10, 0)),
                    mod(3, "Core", "AudioInterface2", (20, 0))],
        "cables": [cable(1, 1, 0, 2, 1), cable(2, 2, 0, 3, 0)]}, True, None),
    ("a stereo pair into the interface", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0), cable(2, 1, 1, 2, 1)]}, True, None),

    # ── must fail, each for its own reason ───────────────────────────────────
    ("empty patch", {"version": "2.6.6", "modules": [], "cables": []},
     False, "no modules"),
    ("two modules sharing an id", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(1, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 1, 0)]}, False, "duplicate module ids"),
    ("a plugin that is not installed", {
        "version": "2.6.6",
        "modules": [mod(1, "NoSuchVendor", "Whatever"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "not installed"),
    ("a model the plugin does not have", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "NotAThing"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "has no model"),
    ("a module with no position", {
        "version": "2.6.6",
        "modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO"},
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "no valid pos"),
    ("a cable to a module that is not there", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": [cable(1, 1, 0, 99, 0)]}, False, "not in the patch"),
    ("nothing that can be heard", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Fundamental", "VCF", (10, 0))],
        "cables": [cable(1, 1, 0, 2, 0)]}, False, "cannot be heard"),
    ("an interface with nothing patched in", {
        "version": "2.6.6",
        "modules": [mod(1, "Fundamental", "VCO"),
                    mod(2, "Core", "AudioInterface2", (10, 0))],
        "cables": []}, False, "nothing patched into it"),
]


def check_disambiguation(inv) -> int:
    """Two of the same model must not read as one.

    A cross-modulation patch is two oscillators each modulating the other. If
    both are called "VCO", the explanation describes an oscillator modulating
    itself -- a correct patch explained wrongly, which on a teaching surface
    is worse than an obvious error.
    """
    pch = {"version": "2.6.6",
           "modules": [mod(1, "Fundamental", "VCO", (0, 0)),
                       mod(2, "Fundamental", "VCO", (10, 0)),
                       mod(3, "Core", "AudioInterface2", (20, 0))],
           "cables": [cable(1, 1, 0, 2, 1), cable(2, 2, 0, 1, 1),
                      cable(3, 2, 0, 3, 0)]}
    text = P.explain(pch, inv)
    if "VCO 1" not in text or "VCO 2" not in text:
        print("  WRONG  two VCOs are not told apart:\n" +
              "\n".join("         " + l for l in text.splitlines()))
        return 1
    print("  ok     two of the same model are numbered")
    return 0


def main():
    inv = P.inventory()
    if "Fundamental" not in inv or "Core" not in inv:
        print("SKIP: needs Rack with Fundamental installed")
        return 0

    bad = 0
    for name, pch, should_pass, expect in CASES:
        errs = P.lint(pch, inv)
        passed = not errs
        if passed != should_pass:
            bad += 1
            got = "passed" if passed else f"failed: {errs[0]}"
            print(f"  WRONG  {name}\n         expected to "
                  f"{'pass' if should_pass else 'fail'}, {got}")
            continue
        # Failing for the wrong reason is its own bug: a check that rejected
        # everything would otherwise score perfectly on the negative cases.
        if expect and not any(expect in e for e in errs):
            bad += 1
            print(f"  WRONG  {name}\n         failed, but not for '{expect}': {errs}")
            continue
        print(f"  ok     {name}")

    bad += check_disambiguation(inv)
    print(f"\n{len(CASES) + 1 - bad}/{len(CASES) + 1} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
