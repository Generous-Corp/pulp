#!/usr/bin/env python3
"""Render the legacy `pulp::signal` vocabulary from the capability manifest.

This lives in Pulp's tools/ rather than beside any one consumer, because more
than one thing needs it and none of them should own it:

  * the Rack module generator feeds it to a model, so the model is told what
    Pulp really provides instead of guessing;
  * Forge's graph -> C++ exporter validates its emitter table against it.

The installed agent-capability manifest is now the consumer-facing authority.
`scan_headers()` remains private regeneration plumbing used by the manifest
writer/freshness gate; normal JSON and Markdown output read the checked
manifest projection so consumers cannot observe a competing inventory.

Output: markdown for prompting, or `--json` for machine consumers.

    dsp_vocabulary.py            # markdown, for a prompt
    dsp_vocabulary.py --json     # {header: [{class, methods}]}, for a validator
"""
from __future__ import annotations

import json
import os
import pathlib
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
    ("Noise & output", ["noise_source", "chaos", "rng", "dither",
                        "velvet_noise", "lofi_chain"]),
    ("Drums", ["drum/"]),
    ("Physical modeling", ["karplus_strong", "modal_bank", "bridged_t_resonator"]),
    ("Effects", ["delay_line", "character_delay", "chorus", "flanger", "phaser",
                 "reverb", "fdn_reverb", "waveshaper", "distortion", "saturator",
                 "fuzz_pair", "granular", "pitch_shifter", "frequency_shifter_ssb",
                 "vocoder", "dc_blocker"]),
    ("Analysis", ["yin_tracker", "envelope_follower"]),
]

def code_only(text: str) -> str:
    """Blank comments and literals while preserving offsets and newlines."""
    pattern = re.compile(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    return pattern.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)


def matching_brace(text: str, opening: int) -> int:
    depth = 0
    for pos in range(opening, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return pos
    raise ValueError("unclosed class body")


def public_declarations(text: str, cls: str, *, source_is_code: bool = False) -> str:
    """Return only top-level public class text, with method bodies blanked."""
    source = text if source_is_code else code_only(text)
    declaration = re.search(rf"\b(class|struct)\s+{re.escape(cls)}\b[^{{]*{{", source)
    if declaration is None:
        return ""
    opening = source.find("{", declaration.start())
    body = source[opening + 1:matching_brace(source, opening)]
    public = declaration.group(1) == "struct"
    depth = 0
    out = list(" " * len(body))
    i = 0
    while i < len(body):
        if depth == 0:
            access = re.match(r"(public|private|protected)\s*:", body[i:])
            if access:
                public = access.group(1) == "public"
                i += access.end()
                continue
        char = body[i]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
        elif public and depth == 0:
            out[i] = char
        i += 1
    return "".join(out)


def public_methods(text: str, cls: str, *, source_is_code: bool = False):
    """Public methods of `cls`, in declaration order, as `name(args)`."""
    src = public_declarations(text, cls, source_is_code=source_is_code)
    out = []
    for mm in re.finditer(
            r"^\s*(?:\[\[[^\]]*\]\]\s*)?(?:inline\s+|static\s+|constexpr\s+|virtual\s+)*"
            r"([A-Za-z_][\w:<>,\s\*&]*?)\s+([a-z_]\w*)\s*\(([^);]*)\)",
            src, re.M):
        ret, name, args = mm.group(1).strip(), mm.group(2), mm.group(3).strip()
        if name.startswith("operator"):
            continue
        args = re.sub(r"\s+", " ", args)
        out.append(f"{name}({args})" if args else f"{name}()")
    seen, uniq = set(), []
    for s in out:
        if s not in seen:
            seen.add(s)
            uniq.append(s)
    return uniq


def scan_headers():
    """Regeneration input; consumers use scan(), which reads the manifest."""
    found = {}
    for root, _, files in os.walk(SIGNAL):
        for fn in sorted(files):
            if not fn.endswith(".hpp"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, SIGNAL)
            text = code_only(open(path, errors="ignore").read())
            classes = []
            for m in re.finditer(r"^(?:template\s*<[^\n]*>\s*)?(?:class|struct)\s+(\w+T?)\b(?!\s*;)",
                                 text, re.M):
                cls = m.group(1)
                if cls.endswith("Params") or cls.startswith("_"):
                    continue
                meth = public_methods(text, cls, source_is_code=True)
                if meth:
                    classes.append({"class": cls, "methods": meth})
            if classes:
                found[rel] = classes
    # os.walk() does not promise directory traversal order. Preserve a canonical
    # projection so APFS and Linux filesystems generate byte-identical manifests.
    return {rel: found[rel] for rel in sorted(found)}


def scan():
    manifest = pathlib.Path(HERE).parent / "docs/status/agent-capabilities.json"
    document = json.loads(manifest.read_text())
    return document["compatibility"]["signal_vocabulary"]["entries"]


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
