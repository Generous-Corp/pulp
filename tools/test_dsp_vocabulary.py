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

import importlib.util
import json
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
    ("dither.hpp", "DitherQuantizerT", "set_dither_mode"),
    ("velvet_noise.hpp", "VelvetNoiseGridT", "next"),
    ("trigger.hpp", None, None),
    ("dc_blocker.hpp", None, None),
    ("smoothed_value.hpp", None, None),
    ("beat_repeat_kernel.hpp", "BeatRepeatKernelT", "trigger"),
    ("character_delay.hpp", "CharacterDelayT", "process"),
    ("additive_bank.hpp", "AdditiveBankT", "process"),
]

# Measured at the time of writing: 246 classes across 161 headers. A floor
# well below that catches a parser collapse without failing on ordinary churn.
MIN_CLASSES = 180
MIN_HEADERS = 120


def extractor_module():
    spec = importlib.util.spec_from_file_location("pulp_dsp_vocabulary_test", EXTRACTOR)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    extractor = extractor_module()
    fixture = """
struct Fixture {
    enum class Mode {
        first, // a trailing comment must not consume the next line
        second,
        third = 3, /* nor may a block comment hide later values */
        fourth,
    };
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
    enums = extractor.public_enums(fixture, "Fixture")
    if enums != [["Mode", ["first", "second", "third", "fourth"]]]:
        print(f"FAIL: enum comments hid public choices: {enums}")
        return 1

    identity_fixture = """
namespace pulp::signal::osc {
class PhaseAccumulator {
public:
    int advance(double increment);
};
template <typename SampleType = float>
class ScalarOscillator {
public:
    SampleType process();
};
}
"""
    identities = extractor.scan_text(identity_fixture)
    phase = next(row for row in identities if row["class"] == "PhaseAccumulator")
    scalar = next(row for row in identities if row["class"] == "ScalarOscillator")
    if phase.get("qualified_name") != "pulp::signal::osc::PhaseAccumulator" or phase.get("template") is not None:
        print(f"FAIL: non-template identity is not exact: {phase}")
        return 1
    if scalar.get("qualified_name") != "pulp::signal::osc::ScalarOscillator" or scalar.get("template") != "typename SampleType = float":
        print(f"FAIL: template identity is not exact: {scalar}")
        return 1
    synthetic = """
class Probe {
  public:
    void prepare() { helper(private_state_); }
    void reset() noexcept { private_state_ = 0; }
    int retained_bytes() const noexcept { return private_state_; }
  private:
    int helper(int value) { return value; }
    int private_state_{};
};
"""
    methods = extractor.public_methods(synthetic, "Probe")
    expected = ["prepare()", "reset()", "retained_bytes()"]
    if methods != expected:
        print(f"FAIL: public/private scanner returned {methods!r}, expected {expected!r}")
        return 1

    separator_fixture = """
class DigitSeparated {
  public:
    static constexpr int capacity = 192'000;
    static constexpr double gain = 0x1.A'Bp2;
    void process();
};
"""
    methods = extractor.public_methods(separator_fixture, "DigitSeparated")
    if methods != ["process()"]:
        print(f"FAIL: C++ digit separator corrupted the class body: {methods!r}")
        return 1

    adjacent_character_fixture = """
class CharacterAdjacent {
  public:
    int select(char value) { switch (value) { case'a': return 1; } return 0; }
    void after();
};
"""
    methods = extractor.public_methods(adjacent_character_fixture, "CharacterAdjacent")
    if methods != ["select(char value)", "after()"]:
        print(f"FAIL: adjacent character literal corrupted the class body: {methods!r}")
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
    registry_problems = extractor.module_capability_problems(doc)
    if registry_problems:
        print(f"  WRONG  module capability registry: {registry_problems}")
        bad += 1
    else:
        print("  ok     every curated module capability resolves exactly")

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

    phase_rows = doc.get("osc/phase.hpp", [])
    phase = next((row for row in phase_rows
                  if row.get("class") == "PhaseAccumulator"), {})
    if phase.get("qualified_name") != "pulp::signal::osc::PhaseAccumulator" or phase.get("template") is not None:
        print(f"  WRONG  PhaseAccumulator identity is incomplete: {phase}")
        bad += 1
    else:
        print("  ok     PhaseAccumulator is advertised as an exact non-template type")

    compact = extractor.shortlist(
        doc,
        "6HP clock generator with phase reset, rate, and pulse width",
    )
    compact_rows = [row for rows in compact.values() for row in rows]
    compact_names = {row.get("qualified_name") for row in compact_rows}
    if not (3 <= len(compact_rows) <= 8):
        print(f"  WRONG  request shortlist has {len(compact_rows)} classes")
        bad += 1
    elif "pulp::signal::osc::PhaseAccumulator" not in compact_names:
        print("  WRONG  clock/phase shortlist omitted PhaseAccumulator")
        bad += 1
    else:
        print(f"  ok     request shortlist is {len(compact_rows)} exact classes")
    if not any(row.get("capability") == "phase-clock" and
               row.get("capability_role") == "primary"
               for row in compact_rows):
        print("  WRONG  clock helpers can satisfy the shortlist without its primary")
        bad += 1
    vca_rows = [row for rows in extractor.shortlist(
        doc, "4HP VCA with gain CV and audio input").values() for row in rows]
    if not any(row.get("capability") == "vca" and
               row.get("qualified_name") == "pulp::signal::VcaT" and
               row.get("capability_role") == "primary" for row in vca_rows):
        print("  WRONG  VCA request omitted its exact primary primitive")
        bad += 1
    compact_markdown = extractor.markdown(compact)
    if "pulp::signal::osc::PhaseAccumulator<float>" in compact_markdown:
        print("  WRONG  non-template PhaseAccumulator gained <float> in prompt")
        bad += 1
    if "pulp::signal::OscillatorT<float>" not in extractor.markdown(doc):
        print("  WRONG  scalar template lost its concrete <float> use form")
        bad += 1
    if "reset(double phase = 0.0)" not in " ".join(phase.get("methods", [])):
        print("  WRONG  PhaseAccumulator omitted its reset lifecycle method")
        bad += 1
    forbidden = {"PhaseVocoderT", "MultichannelPhaseCoordinatorT",
                 "TransientPhasePolicyT"}
    if forbidden & {row.get("class") for row in compact_rows}:
        print("  WRONG  block/offline phase APIs leaked into module shortlist")
        bad += 1
    obscure = extractor.shortlist(doc, "an unusual bespoke utility")
    if any(not row.get("capability") for rows in obscure.values() for row in rows):
        print("  WRONG  obscure request escaped the curated capability registry")
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
