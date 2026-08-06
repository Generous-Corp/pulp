#!/usr/bin/env python3
"""Corpus for the DSP vocabulary extractor.

The extractor parses `core/signal` headers with regexes and hands the result
to a model as the list of DSP it may use. That makes a silent regression
uniquely expensive: if a parse breaks, the vocabulary shrinks or empties, no
error is raised, and the model simply hand-rolls whatever it can no longer
see. The first module ever generated this way used none of Pulp's DSP for
exactly that reason -- a wrong signature in the prompt is worse than an
absent one, because the model quietly avoids the class.

So this pins a floor on the total, and pins specific classes with the methods
that actually get called in generated modules. Anything that silently drops
one of these fails here rather than showing up months later as modules that
mysteriously stopped using the SDK.

    python3 tools/test_dsp_vocabulary.py
"""
from __future__ import annotations

import json
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXTRACTOR = os.path.join(HERE, "dsp_vocabulary.py")

# Classes generated modules have actually reached for, with a method each that
# must survive. Picked from real generations rather than by reading the
# headers, so the corpus tracks what the pipeline depends on.
MUST_HAVE = [
    ("oscillator.hpp", "OscillatorT", None),
    ("adsr.hpp", "AdsrT", "note_on"),
    ("svf.hpp", "SvfT", None),
    ("mod_tools.hpp", "AttenuverterT", None),
    ("analog_vcf.hpp", None, None),
    ("stage_sequencer.hpp", None, None),
    ("chaos.hpp", None, None),
    ("rng.hpp", None, None),
    ("trigger.hpp", None, None),
    ("dc_blocker.hpp", None, None),
    ("smoothed_value.hpp", None, None),
]

# Measured at the time of writing: 246 classes across 161 headers. A floor
# well below that catches a parser collapse without failing on ordinary churn.
MIN_CLASSES = 180
MIN_HEADERS = 120


def main():
    spec = importlib.util.spec_from_file_location("pulp_dsp_vocabulary_test", EXTRACTOR)
    assert spec is not None and spec.loader is not None
    extractor = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extractor)
    fixture = """
struct Fixture {
    float process(float input) {
        std::vector<float> work(static_cast<std::size_t>(4));
        return calibration_tables(input);
    }
    int declared(int amount);
};
"""
    parsed = extractor.public_methods(fixture, "Fixture")
    if parsed != ["process(float input)", "declared(int amount)"]:
        print(f"FAIL: inline method bodies leaked into the API surface: {parsed}")
        return 1

    r = subprocess.run([sys.executable, EXTRACTOR, "--json"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL: extractor exited {r.returncode}\n{r.stderr[:500]}")
        return 1
    try:
        doc = json.loads(r.stdout)
    except json.JSONDecodeError as e:
        print(f"FAIL: --json did not produce JSON ({e}) — the machine-readable "
              f"contract is what Forge's exporter consumes")
        return 1

    n_headers = len(doc)
    n_classes = sum(len(v) for v in doc.values())
    bad = 0

    malformed = [
        f"{row.get('class')}.{method}"
        for classes in doc.values()
        for row in classes
        if isinstance(row, dict)
        for method in row.get("methods", [])
        if not isinstance(method, str) or method.count("(") != method.count(")")
    ]
    if malformed:
        print(f"  WRONG  malformed method signatures: {malformed[:5]}")
        bad += 1

    print(f"  {n_classes} classes across {n_headers} headers")
    if n_classes < MIN_CLASSES or n_headers < MIN_HEADERS:
        print(f"  WRONG  below the floor ({MIN_CLASSES} classes / "
              f"{MIN_HEADERS} headers) — the parser has probably broken")
        bad += 1

    # Header keys may be paths or bare names depending on the caller; match on
    # the basename so this does not fail on a cosmetic change.
    by_base = {os.path.basename(k): v for k, v in doc.items()}

    for header, cls, method in MUST_HAVE:
        entry = by_base.get(header)
        if entry is None:
            print(f"  WRONG  {header} is not in the vocabulary")
            bad += 1
            continue
        names = {c.get("class") for c in entry if isinstance(c, dict)}
        if cls and cls not in names:
            print(f"  WRONG  {header} lost {cls} (has: {sorted(names)[:5]})")
            bad += 1
            continue
        if method:
            found = next((c for c in entry
                          if isinstance(c, dict) and c.get("class") == cls), {})
            ms = " ".join(str(m) for m in found.get("methods", []))
            if method not in ms:
                print(f"  WRONG  {cls}.{method} is no longer described")
                bad += 1
                continue
        print(f"  ok     {header}" + (f" · {cls}" if cls else ""))

    print(f"\n{'FAIL' if bad else 'ok'}: {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
