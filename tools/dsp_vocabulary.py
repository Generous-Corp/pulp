#!/usr/bin/env python3
"""Derive what `pulp::signal` actually exposes, straight from the headers.

This lives in Pulp's tools/ rather than beside any one consumer, because more
than one thing needs it and none of them should own it:

  * the Rack module generator feeds it to a model, so the model is told what
    Pulp really provides instead of guessing;
  * Forge's graph -> C++ exporter validates its emitter table against it.

Both previously hand-maintained their own idea of the API, and both got it
wrong in the same way: a plausible-but-invented signature that only a compiler
caught. A single derived source removes that whole class of drift, and means a
new class in core/signal reaches every consumer without anyone editing a list.

Output: markdown for prompting, or `--json` for machine consumers.

    dsp_vocabulary.py            # markdown, for a prompt
    dsp_vocabulary.py --json     # {header: [{class, methods}]}, for a validator
"""
from __future__ import annotations

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIGNAL = os.path.normpath(os.path.join(HERE, "..", "core", "signal",
                                       "include", "pulp", "signal"))

# Grouped so the prompt reads as "here is what you reach for", not a flat dump.
GROUPS = [
    ("Oscillators", ["oscillator", "osc/", "wavetable", "lfo", "phase_distortion",
                     "additive_bank", "square_osc_bank"]),
    ("Filters", ["svf", "ladder_filter", "analog_vcf", "tpt_filter",
                 "ota_cascade_filter", "linkwitz_riley", "biquad"]),
    ("Envelopes & dynamics", ["adsr", "envelope", "decay_envelope",
                              "ballistics_filter", "compressor", "noise_gate"]),
    ("Amplitude & routing", ["vca", "gain", "crossfade", "panner",
                             "lowpass_gate", "vactrol", "smoothed_value"]),
    ("CV utilities", ["mod_tools", "scale_quantizer", "harmony_engine"]),
    ("Clocks, gates & triggers", ["trigger", "gate_logic", "probability_gate"]),
    ("Sequencing", ["modular_sequencing", "stage_sequencer", "cartesian_walk",
                    "rungler"]),
    ("Noise & chaos", ["noise_source", "chaos", "rng"]),
    ("Drums", ["drum/"]),
    ("Physical modeling", ["karplus_strong", "modal_bank", "bridged_t_resonator"]),
    ("Effects", ["delay_line", "character_delay", "chorus", "flanger", "phaser",
                 "reverb", "fdn_reverb", "waveshaper", "distortion", "saturator",
                 "fuzz_pair", "granular", "pitch_shifter", "frequency_shifter_ssb",
                 "vocoder", "dc_blocker"]),
    ("Analysis", ["yin_tracker", "envelope_follower"]),
]

# Methods every template has; listing them per class is noise.
BORING = {"reset", "prepare", "set_sample_rate", "clear"}


def public_methods(text: str, cls: str):
    """Public methods of `cls`, in declaration order, as `name(args)`."""
    m = re.search(rf"(?:class|struct)\s+{re.escape(cls)}\b", text)
    if not m:
        return []
    body, depth, i, started = [], 0, m.end(), False
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
            started = True
        elif c == "}":
            depth -= 1
            if started and depth == 0:
                break
        if started:
            body.append(c)
        i += 1
    src = "".join(body)
    # Only the public section: templates here are public-first, and a private:
    # marker ends what a caller may touch.
    cut = re.search(r"\bprivate\s*:", src)
    if cut:
        src = src[:cut.start()]
    out = []
    for mm in re.finditer(
            r"^\s*(?:\[\[[^\]]*\]\]\s*)?(?:inline\s+|static\s+|constexpr\s+|virtual\s+)*"
            r"([A-Za-z_][\w:<>,\s\*&]*?)\s+([a-z_]\w*)\s*\(([^);]*)\)",
            src, re.M):
        ret, name, args = mm.group(1).strip(), mm.group(2), mm.group(3).strip()
        if name in BORING or name.startswith("operator"):
            continue
        args = re.sub(r"\s+", " ", args)
        out.append(f"{name}({args})" if args else f"{name}()")
    seen, uniq = set(), []
    for s in out:
        if s not in seen:
            seen.add(s)
            uniq.append(s)
    return uniq[:7]


def scan():
    found = {}
    for root, _, files in os.walk(SIGNAL):
        for fn in sorted(files):
            if not fn.endswith(".hpp"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, SIGNAL)
            text = open(path, errors="ignore").read()
            classes = []
            for m in re.finditer(r"^(?:template[^\n]*\n)?(?:class|struct)\s+(\w+T?)\b(?!\s*;)",
                                 text, re.M):
                cls = m.group(1)
                if cls.endswith("Params") or cls.startswith("_"):
                    continue
                meth = public_methods(text, cls)
                if meth:
                    classes.append({"class": cls, "methods": meth})
            if classes:
                found[rel] = classes
    return found


def grouped(found):
    out, used = [], set()
    for title, pats in GROUPS:
        rows = []
        for rel in sorted(found):
            if rel in used:
                continue
            if any(p in rel for p in pats):
                used.add(rel)
                rows.append((rel, found[rel]))
        if rows:
            out.append((title, rows))
    rest = [(r, found[r]) for r in sorted(found) if r not in used]
    if rest:
        out.append(("Other", rest))
    return out


def markdown(found):
    L = ["Every class below is a template — instantiate with `<float>`. All are **per-sample**",
         "and allocation-free. **Prefer these over hand-writing DSP**: they are tested, they are",
         "shared with the DAW products, and a fix to one benefits both.", ""]
    for title, rows in grouped(found):
        L.append(f"### {title}")
        L.append("")
        for rel, classes in rows:
            for c in classes:
                meths = " · ".join(f"`{m}`" for m in c["methods"])
                L.append(f"- `#include <pulp/signal/{rel}>` → **`{c['class']}<float>`**")
                if meths:
                    L.append(f"  - {meths}")
        L.append("")
    return "\n".join(L)


if __name__ == "__main__":
    f = scan()
    if "--json" in sys.argv:
        print(json.dumps(f, indent=2))
    else:
        n = sum(len(v) for v in f.values())
        print(markdown(f))
        print(f"<!-- {n} classes across {len(f)} headers, extracted from core/signal -->",
              file=sys.stderr)
