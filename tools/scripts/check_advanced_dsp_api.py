#!/usr/bin/env python3
"""Fail when a Round 2 public DSP method disappears from its API reference.

This is deliberately a small C++ class-body scanner rather than a second API
manifest. The headers remain the source of truth: adding a public method makes
this check fail until advanced-dsp-api.md names it. It ignores constructors and
operators, and it does not attempt to document fields or private helpers.
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
REFERENCE = ROOT / "docs/reference/advanced-dsp-api.md"

# Public Round 2 authoring types. Values are the concrete template/struct names
# in source; keys make failures readable and let aliases stay stable.
TYPES: dict[str, tuple[str, str]] = {
    "FeedforwardCompressor": ("feedforward_compressor.hpp", "FeedforwardCompressorT"),
    "VcaCompressor": ("vca_compressor.hpp", "VcaCompressorT"),
    "DiodeBridgeGain": ("diode_bridge_compressor.hpp", "DiodeBridgeGainT"),
    "TransformerBracket": ("diode_bridge_compressor.hpp", "TransformerBracketT"),
    "DiodeBridgeCompressor": ("diode_bridge_compressor.hpp", "DiodeBridgeCompressorT"),
    "FetCompressor": ("fet_compressor.hpp", "FetCompressorT"),
    "Saturator": ("saturator.hpp", "SaturatorT"),
    "DiodeClipper": ("distortion.hpp", "DiodeClipperT"),
    "FeedbackClipper": ("distortion.hpp", "FeedbackClipperT"),
    "ToneStack": ("distortion.hpp", "ToneStackT"),
    "FuzzPair": ("fuzz_pair.hpp", "FuzzPairT"),
    "TapeMachine": ("tape_machine.hpp", "TapeMachineT"),
    "SpeakerModel": ("speaker_cabinet.hpp", "SpeakerModelT"),
    "PhaserStages": ("phaser_stages.hpp", "PhaserStagesT"),
    "DelayVibrato": ("vibrato.hpp", "DelayVibratoT"),
    "PhaseVibrato": ("vibrato.hpp", "PhaseVibratoT"),
    "UniVibe": ("vibrato.hpp", "UniVibeT"),
    "ChorusEnsemble": ("chorus_family.hpp", "ChorusEnsembleT"),
    "Flanger": ("flanger.hpp", "FlangerT"),
    "SsbFrequencyShifter": ("frequency_shifter_ssb.hpp", "SsbFrequencyShifterT"),
    "LeslieRotary": ("leslie_rotary.hpp", "LeslieRotaryT"),
    "ScannerVibrato": ("scanner_vibrato.hpp", "ScannerVibratoT"),
    "PitchShifter": ("pitch_shifter.hpp", "PitchShifterT"),
    "YinTracker": ("yin_tracker.hpp", "YinTrackerT"),
    "DiatonicMap": ("harmony_engine.hpp", "DiatonicMapT"),
    "HarmonyEngine": ("harmony_engine.hpp", "HarmonyEngineT"),
    "CyclicStretch": ("cyclic_stretch.hpp", "CyclicStretchT"),
    "GranularEngine": ("granular.hpp", "GranularEngineT"),
    "SpectralEnvelope": ("additive_spectral_envelope.hpp", "SpectralEnvelope"),
    "VoiceTable": ("additive_bank.hpp", "VoiceTable"),
    "AdditiveBank": ("additive_bank.hpp", "AdditiveBankT"),
    "Vocoder": ("vocoder.hpp", "VocoderT"),
    "StageSeq": ("stage_sequencer.hpp", "StageSeqT"),
    "CartesianWalk": ("cartesian_walk.hpp", "CartesianWalkT"),
    "Rungler": ("rungler.hpp", "RunglerT"),
    "QuantizeScale": ("scale_quantizer.hpp", "QuantizeScaleT"),
    "ProbGate": ("probability_gate.hpp", "ProbGateT"),
    "GateLogic": ("gate_logic.hpp", "GateLogicT"),
    "NonlinAmbience": ("nonlin_ambience.hpp", "NonlinAmbienceT"),
    "ZeroLatencyConvolver": ("zero_latency_convolver.hpp", "ZeroLatencyConvolverT"),
}

# Class-scope language/library calls that have function syntax but are not
# members. Real public methods with these names are not used by the selected API.
NON_METHOD_CALLS = {"static_assert", "min", "max"}


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


def code_only(text: str) -> str:
    """Blank comments and literals while preserving offsets and line structure."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                         re.DOTALL)
    return pattern.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)


def public_methods(text: str, class_name: str) -> set[str]:
    text = code_only(text)
    declaration = re.search(rf"\b(class|struct)\s+{re.escape(class_name)}\b[^{{]*{{", text)
    if declaration is None:
        raise ValueError(f"class {class_name} not found")
    opening = text.find("{", declaration.start())
    body = text[opening + 1 : matching_brace(text, opening)]
    default_public = declaration.group(1) == "struct"
    public = default_public
    depth = 0
    methods: set[str] = set()
    i = 0
    while i < len(body):
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
        elif depth == 0:
            access = re.match(r"(public|private|protected)\s*:", body[i:])
            if access:
                public = access.group(1) == "public"
                i += access.end() - 1
            elif public and body[i] == "(":
                prefix = body[:i]
                name = re.search(r"([A-Za-z_]\w*)\s*$", prefix)
                if name:
                    candidate = name.group(1)
                    if candidate != class_name and candidate not in {
                        "if", "for", "while", "switch", *NON_METHOD_CALLS
                    }:
                        methods.add(candidate)
        i += 1
    return methods


def main() -> int:
    documented = REFERENCE.read_text(encoding="utf-8")
    signal = ROOT / "core/signal/include/pulp/signal"
    missing: list[str] = []
    for public_name, (header_name, class_name) in TYPES.items():
        source = (signal / header_name).read_text(encoding="utf-8")
        try:
            methods = public_methods(source, class_name)
        except ValueError as error:
            print(f"advanced DSP API check: {header_name}: {error}", file=sys.stderr)
            return 1
        for method in sorted(methods):
            if re.search(rf"`{re.escape(method)}\s*\(", documented) is None:
                missing.append(f"{public_name}::{method}() ({header_name})")
    if missing:
        print("advanced DSP API reference is missing public methods:", file=sys.stderr)
        for item in missing:
            print(f"  - {item}", file=sys.stderr)
        print("Document each method in docs/reference/advanced-dsp-api.md.", file=sys.stderr)
        return 1
    print(f"advanced DSP API reference covers public methods for {len(TYPES)} Round 2 types")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
