#!/usr/bin/env python3
"""Build and validate Pulp's installed agent capability contract.

The consumer manifest describes design-time public API facts. Runtime grants,
instances, activation, policy, risk decisions, and receipts belong to the
unified control platform and are rejected here.
"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
from typing import Any

import agent_capability_surface as surface
import json_schema_lite
from agent_capability_evolution import (
    contract_payload,
    manifest_evolution_problems as evolution_problems,
    surface_evolution_problems,
)


SCHEMA = "pulp.agent-capabilities.v1"
SCHEMA_MINOR = 1
MANIFEST_REVISION = 6
SURFACE_INVENTORY_VERSION = 9
HISTORY_SCHEMA = "pulp.agent-capability-history.v1"
HISTORY_FILE = pathlib.Path("tools/agent-capabilities/contract-history.json")
SNAPSHOT = pathlib.Path("docs/status/agent-capabilities.json")
MANIFEST_SCHEMA_FILE = pathlib.Path(
    "docs/status/agent-capabilities.schema.json"
)
COMPILE_FIXTURE = pathlib.Path("test/test_agent_capability_compile.cpp")
DOMAINS = {"signal", "music", "midi", "audio", "timebase", "sequence", "offline"}
ROOT_DOMAINS = {item["domain"] for item in surface.PUBLIC_ROOTS}
RT_CLASSES = {"audio", "control", "any", "offline", "mixed"}
STATUSES = {
    "stable",
    "usable",
    "experimental",
    "partial",
    "unsupported",
    "deprecated",
}
BINDING_KINDS = {"cpp_type", "cpp_function"}
EVOLUTION_STATES = {"active", "deprecated"}
REQUIRED_FEATURES = [
    "capability-contract-version-v1",
    "coverage-state-v1",
    "design-runtime-separation-v1",
    "determinism-contract-v1",
    "tombstones-v1",
    "typed-bindings-v1",
]
FORBIDDEN_NUMERIC_CONTRACT_FIELDS = {"min", "max", "default", "range", "choices"}
FORBIDDEN_RUNTIME_CONTROL_FIELDS = {
    "activation",
    "authorization",
    "authorizations",
    "consent",
    "grant",
    "grants",
    "instance",
    "instance_id",
    "operation",
    "operation_id",
    "operations",
    "policies",
    "policy",
    "receipt",
    "receipts",
    "revocation",
    "risk",
    "risk_class",
    "session",
    "session_id",
}

# Reviewed ownership is explicit because public include prefixes do not imply
# link ownership: source-bearing APIs can live behind specialized targets.
REVIEWED_MINIMAL_TARGETS = {
    "pulp/audio/instrument_voice_allocator.hpp": "Pulp::audio",
    "pulp/audio/midi_voice_modulation_adapter.hpp": "Pulp::audio",
    "pulp/audio/onset_detector.hpp": "Pulp::audio",
    "pulp/audio/unison_voice_stack.hpp": "Pulp::audio",
    "pulp/audio/voice_runtime_facade.hpp": "Pulp::audio",
    "pulp/midi/arpeggiator.hpp": "Pulp::midi",
    "pulp/midi/controller_utility_kernels.hpp": "Pulp::midi",
    "pulp/midi/mpe_voice_tracker.hpp": "Pulp::midi",
    "pulp/midi/note_utility_kernels.hpp": "Pulp::midi",
    "pulp/midi/routing_utility_kernels.hpp": "Pulp::midi",
    "pulp/music/chord.hpp": "Pulp::music",
    "pulp/music/harmony.hpp": "Pulp::music",
    "pulp/music/markov.hpp": "Pulp::music",
    "pulp/music/music.hpp": "Pulp::music",
    "pulp/music/pitch.hpp": "Pulp::music",
    "pulp/music/pattern.hpp": "Pulp::music",
    "pulp/music/rhythm_relationship.hpp": "Pulp::music",
    "pulp/music/spelling.hpp": "Pulp::music",
    "pulp/music/voicing.hpp": "Pulp::music",
    "pulp/sequence/host_transport_projector.hpp": "Pulp::sequence",
    "pulp/signal/saturator.hpp": "Pulp::signal",
    "pulp/signal/analysis_frontends.hpp": "Pulp::signal",
    "pulp/signal/audio_matrix_mixer.hpp": "Pulp::signal",
    "pulp/signal/breakpoint_envelope.hpp": "Pulp::signal",
    "pulp/signal/dither.hpp": "Pulp::signal",
    "pulp/signal/dust.hpp": "Pulp::signal",
    "pulp/signal/dynamics_contract.hpp": "Pulp::signal",
    "pulp/signal/fm_operator_engine.hpp": "Pulp::signal",
    "pulp/signal/fractional_delay.hpp": "Pulp::signal",
    "pulp/signal/lfsr.hpp": "Pulp::signal",
    "pulp/signal/linkwitz_riley.hpp": "Pulp::signal",
    "pulp/signal/mid_side.hpp": "Pulp::signal",
    "pulp/signal/modulation_curve.hpp": "Pulp::signal",
    "pulp/signal/multi_channel_meter.hpp": "Pulp::signal",
    "pulp/signal/noise_tilt.hpp": "Pulp::signal",
    "pulp/signal/nonlinear_shaping.hpp": "Pulp::signal",
    "pulp/signal/nway_crossfade.hpp": "Pulp::signal",
    "pulp/signal/path_latency_aligner.hpp": "Pulp::signal",
    "pulp/signal/path_switcher.hpp": "Pulp::signal",
    "pulp/signal/rise_fall_generator.hpp": "Pulp::signal",
    "pulp/signal/scope_capture.hpp": "Pulp::signal",
    "pulp/signal/six_band_eq.hpp": "Pulp::signal",
    "pulp/signal/source_filter_analysis.hpp": "Pulp::signal",
    "pulp/signal/spectrum_trace.hpp": "Pulp::signal",
    "pulp/signal/sos_cascade.hpp": "Pulp::signal",
    "pulp/signal/supersaw.hpp": "Pulp::signal",
    "pulp/signal/true_peak_limiter.hpp": "Pulp::signal",
    "pulp/signal/unison.hpp": "Pulp::signal",
    "pulp/signal/velvet_noise.hpp": "Pulp::signal",
    "pulp/signal/fft_backend.hpp": "Pulp::signal-fft-backend",
    "pulp/signal/modal_spec.hpp": "Pulp::signal-modal-spec",
    "pulp/signal/mirrored_history_buffer.hpp": "Pulp::signal",
    "pulp/signal/osc/minblep.hpp": "Pulp::signal",
    "pulp/signal/windowing.hpp": "Pulp::signal",
    "pulp/timebase/quantize.hpp": "Pulp::timebase",
    "pulp/timebase/tick.hpp": "Pulp::timebase",
    "pulp/timebase/beat_division.hpp": "Pulp::timebase",
    "pulp/timebase/coordinate_random.hpp": "Pulp::timebase",
    "pulp/timebase/grid_projection.hpp": "Pulp::timebase",
    "pulp/timebase/groove_kernel.hpp": "Pulp::timebase",
    "pulp/timebase/ratchet.hpp": "Pulp::timebase",
    "pulp/timebase/trigger_grid.hpp": "Pulp::timebase",
}


def availability() -> dict[str, Any]:
    return {
        "state": "available",
        "platforms": ["all"],
        "required_features": [],
    }


def binding(
    *,
    role: str,
    kind: str,
    include: str,
    qualified_name: str,
    target: str,
    header_fingerprint: str,
    address_expression: str | None = None,
) -> dict[str, Any]:
    result = {
        "role": role,
        "kind": kind,
        "include": include,
        "qualified_name": qualified_name,
        "target": target,
        "availability": availability(),
        "_header_fingerprint": header_fingerprint,
    }
    if address_expression is not None:
        result["_address_expression"] = address_expression
    return result


def capability(**row: Any) -> dict[str, Any]:
    row.setdefault("contract_version", {"major": 1, "minor": 0})
    row.setdefault("status", "usable")
    row.setdefault(
        "evolution", {"state": "active", "introduced_in": {"major": 1, "minor": 0}}
    )
    return row


# Every row is an explicit consumer promise about public API that already
# ships. Header fingerprints are maintenance data and are stripped from the
# installed manifest.
EXPORTS = [
    capability(
        key="signal.minblep",
        domain="signal",
        summary=(
            "Fixed-capacity minimum-phase bandlimited-step correction for "
            "oscillator discontinuities."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "none",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model=(
            "A compile-time-bounded slot array accumulates causal corrections; "
            "insertion reports invalid inputs and capacity exhaustion explicitly."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="fractional oscillator discontinuity positions and step heights",
        output_domain="causal bandlimited-step correction samples",
        units=["normalized sample position", "sample amplitude", "samples"],
        latency="zero",
        tail="32 samples per inserted discontinuity",
        scheduling="sample-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/signal/osc/minblep.hpp",
                qualified_name="pulp::signal::osc::MinBlepAccumulator<>",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:838567bf66f86f232dc6239d74b77cb7a456eb854831b73f70e9641e0d21c9f6"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "entrypoint",
                "binding": "pulp::signal::osc::MinBlepAccumulator<>",
                "operation": "member_call",
                "member": "insert",
                "arguments": "0.5, 1.0",
            },
        ],
    ),
    capability(
        key="signal.bounded-sample-history",
        domain="signal",
        summary=(
            "Prepared fixed-capacity sample history with a contiguous "
            "oldest-to-newest view."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "audio",
            "release": "destruction-off-audio",
        },
        state_model=(
            "prepare allocates two mirrored copies of a fixed capacity; push, "
            "window, reset, and accessors allocate no memory afterward. The "
            "state is single-thread DSP history, not a synchronization primitive."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="sample values and prepared sample capacity",
        output_domain="contiguous oldest-to-newest sample history",
        units=["samples", "sample count"],
        latency="zero",
        tail="capacity-sample-history-until-overwritten-or-reset",
        scheduling="sample-synchronous single-thread use",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/signal/mirrored_history_buffer.hpp",
                qualified_name="pulp::signal::MirroredHistoryBuffer<float>",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:5f203acc21d01060d814ec0a19a82cd9aa8ffc669a9836fa929af888c4f31c2a"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::signal::MirroredHistoryBuffer<float>",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "8",
        }],
    ),
    capability(
        key="signal.window-functions",
        domain="signal",
        summary=(
            "Reusable analysis-window generation and allocation-free in-place "
            "application."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "none",
            "prepare": "control",
            "process": "audio-with-precomputed-window",
            "reset": "none",
            "release": "generated-vector-destruction-off-audio",
        },
        state_model=(
            "Stateless static utility; generate returns caller-owned allocated "
            "coefficients and apply mutates caller-owned samples without allocation."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="window size, window family, optional Kaiser parameter, and samples",
        output_domain="window coefficients or in-place windowed samples",
        units=["samples", "sample count", "normalized ratio"],
        latency="zero",
        tail="none",
        scheduling="control-generated and sample-array-applied",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/signal/windowing.hpp",
                qualified_name="pulp::signal::WindowFunction",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:672043aa1a9a0d0a0cd28a82cd7b7d81e57de035b963e0226c23b85b42ce1f7f"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::signal::WindowFunction",
            "operation": "member_call",
            "member": "generate",
            "arguments": "8, pulp::signal::WindowFunction::Type::hann",
        }],
    ),
    capability(
        key="signal.saturator",
        contract_version={"major": 1, "minor": 1},
        domain="signal",
        summary="Stateful saturation with an explicit anti-aliasing policy.",
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model=(
            "Instance state is reset explicitly; prepare/configuration precedes "
            "processing."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="audio samples",
        output_domain="audio samples",
        units=["samples", "decibels", "hertz", "normalized ratio"],
        latency="implementation-reported",
        tail="none",
        scheduling="sample-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/signal/saturator.hpp",
                qualified_name="pulp::signal::SaturatorT<float>",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:575576593e3edcd18104082feebfc48584a3f160cc5dcd24d8d48909cb8e6b2b"
                ),
            )
        ],
        forge_descriptor={"catalog": "forge-catalog.json", "node_key": "saturator"},
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::signal::SaturatorT<float>",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "48000.0",
        }],
    ),
    capability(
        key="signal.sos-cascade",
        domain="signal",
        summary=(
            "Fixed-capacity transactional execution of stable normalized "
            "second-order-section cascades."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model=(
            "A prepared fixed-capacity section array owns recursive DF2T state; "
            "coefficient installation is whole-cascade transactional and occurs "
            "at a block boundary."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="audio samples and normalized SOS coefficients",
        output_domain="audio samples",
        units=["samples", "normalized coefficients", "section count"],
        latency="zero",
        tail="recursive IIR decay until reset or denormal snap",
        scheduling="sample-synchronous; coefficient changes at block boundaries",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/signal/sos_cascade.hpp",
                qualified_name="pulp::signal::SosCascadeT<float>",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:8cdd74d36ba12f1b19d85a28d57b501e5b96f8c3a1eae3f82cd96dbb7f027d79"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::signal::SosCascadeT<float>",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "4",
        }],
    ),
    capability(
        key="audio.instrument-voice-allocator",
        contract_version={"major": 1, "minor": 1},
        domain="audio",
        summary=(
            "Prepared finite voice allocation with choke, steal, release, and "
            "termination records."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "control-or-audio-when-quiescent",
            "release": "destruction-off-audio",
        },
        state_model=(
            "prepare allocates a fixed voice table; trigger and release mutate "
            "prepared slots only."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="voice trigger and release events",
        output_domain="voice allocation and termination records",
        units=["MIDI note", "frames", "voice index"],
        latency="zero",
        tail="termination-fade-frames",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/audio/instrument_voice_allocator.hpp",
                qualified_name="pulp::audio::InstrumentVoiceAllocator",
                target="Pulp::audio",
                header_fingerprint=(
                    "sha256:f500f22c9e5ce4a1a26a03245b68e645ef6752dd2f4d0f59cf122681e0a2327f"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::InstrumentVoiceAllocator",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "1",
        }],
    ),
    capability(
        key="midi.mpe-voice-tracker",
        contract_version={"major": 1, "minor": 1},
        domain="midi",
        summary=(
            "Fixed-capacity MIDI 1.0 and UMP MPE note ownership and expression "
            "tracking."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "none",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model=(
            "Fixed 128-slot note table with nonzero uint64 generations, "
            "fail-closed generation exhaustion, FIFO deferred note-offs, and "
            "transactional lifecycle rejection."
        ),
        seed_model="none",
        determinism={
            "repeatability": "not_promised",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="MIDI events and UMP packets",
        output_domain="owned per-note expression state",
        units=["MIDI note", "MIDI channel", "semitones", "normalized ratio"],
        latency="zero",
        tail="owned-notes-until-release-or-reset",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/midi/mpe_voice_tracker.hpp",
                qualified_name="pulp::midi::MpeVoiceTracker",
                target="Pulp::midi",
                header_fingerprint=(
                    "sha256:aca221889b5e62dd37fe9890c1d8009f7d8479a0b83f4a49986bde3fa2a6dbdc"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::MpeVoiceTracker",
            "operation": "member_call",
            "member": "reset",
            "arguments": "",
        }],
    ),
    capability(
        key="music.chord-spelling",
        domain="music",
        summary=(
            "Fixed-capacity pitch-class and chord spelling under an explicit "
            "accidental policy."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "none",
            "release": "none",
        },
        state_model="Pure fixed-capacity value transforms with no retained state.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="12-TET pitch classes, chord formulas, and accidental policy",
        output_domain="spelled pitch classes, chord tones, and names",
        units=["pitch class", "semitones", "note letter"],
        latency="zero",
        tail="none",
        scheduling="pure",
        status="experimental",
        bindings=[
            binding(
                role="pitch-class-operation",
                kind="cpp_function",
                include="pulp/music/spelling.hpp",
                qualified_name="pulp::music::spell_pitch_class",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:2a0f8acc58dd5525631d694ae6b989ec7d50a2bf305d176414ee51a542673c13"
                ),
            ),
            binding(
                role="name-operation",
                kind="cpp_function",
                include="pulp/music/spelling.hpp",
                qualified_name="pulp::music::spelling_name",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:2a0f8acc58dd5525631d694ae6b989ec7d50a2bf305d176414ee51a542673c13"
                ),
            ),
            binding(
                role="formula-operation",
                kind="cpp_function",
                include="pulp/music/spelling.hpp",
                qualified_name="pulp::music::spell_chord",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:2a0f8acc58dd5525631d694ae6b989ec7d50a2bf305d176414ee51a542673c13"
                ),
                address_expression=(
                    "static_cast<std::optional<pulp::music::SpelledChord> (*)("
                    "pulp::music::PitchClass, const pulp::music::ChordFormula&, "
                    "pulp::music::AccidentalPolicy) noexcept>("
                    "&pulp::music::spell_chord)"
                ),
            ),
            binding(
                role="chord-operation",
                kind="cpp_function",
                include="pulp/music/spelling.hpp",
                qualified_name="pulp::music::spell_chord",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:2a0f8acc58dd5525631d694ae6b989ec7d50a2bf305d176414ee51a542673c13"
                ),
                address_expression=(
                    "static_cast<std::optional<pulp::music::SpelledChord> (*)("
                    "const pulp::music::Chord&, pulp::music::AccidentalPolicy) noexcept>("
                    "&pulp::music::spell_chord)"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "pitch-class-operation",
                "binding": "pulp::music::spell_pitch_class",
                "operation": "function_call",
                "arguments": (
                    "pulp::music::PitchClass::c_sharp, "
                    "pulp::music::AccidentalPolicy::prefer_flats"
                ),
            },
            {
                "role": "name-operation",
                "binding": "pulp::music::spelling_name",
                "operation": "function_call",
                "arguments": "pulp::music::SpelledPitchClass{}",
            },
            {
                "role": "formula-operation",
                "binding": "pulp::music::spell_chord",
                "operation": "function_call",
                "arguments": (
                    "pulp::music::PitchClass::c, "
                    "*pulp::music::ChordFormula::for_quality("
                    "pulp::music::ChordQuality::major), "
                    "pulp::music::AccidentalPolicy::prefer_sharps"
                ),
            },
            {
                "role": "chord-operation",
                "binding": "pulp::music::spell_chord",
                "operation": "function_call",
                "arguments": (
                    "*pulp::music::Chord::construct(60, "
                    "*pulp::music::ChordFormula::for_quality("
                    "pulp::music::ChordQuality::major)), "
                    "pulp::music::AccidentalPolicy::prefer_sharps"
                ),
            },
        ],
    ),
    capability(
        key="music.chord-recognition",
        domain="music",
        summary=(
            "Ranked recognition over Pulp's stable 12-quality chord catalog with "
            "explicit ambiguity and inversion evidence."
        ),
        rt_class="control",
        lifecycle={
            "construction": "control-or-offline",
            "prepare": "none",
            "process": "control-or-offline",
            "reset": "none",
            "release": "none",
        },
        state_model="Pure bounded candidate ranking with no retained state.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="12-TET pitch-class sets or bounded MIDI-note collections",
        output_domain="ranked named-chord candidates and inversion evidence",
        units=["pitch class", "MIDI note", "candidate count"],
        latency="request-bound",
        tail="none",
        scheduling="request-synchronous",
        status="partial",
        bindings=[
            binding(
                role="pitch-class-operation",
                kind="cpp_function",
                include="pulp/music/harmony.hpp",
                qualified_name="pulp::music::recognize_chord",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:8e5ce059f95c6817e05a9b6cf6bf94599d54fdf82da7fe7e9a12ffe27b0b9e60"
                ),
                address_expression=(
                    "static_cast<std::optional<pulp::music::ChordRecognitionList> (*)("
                    "pulp::music::PitchClassSet, std::optional<pulp::music::PitchClass>) "
                    "noexcept>(&pulp::music::recognize_chord)"
                ),
            ),
            binding(
                role="midi-operation",
                kind="cpp_function",
                include="pulp/music/harmony.hpp",
                qualified_name="pulp::music::recognize_chord",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:8e5ce059f95c6817e05a9b6cf6bf94599d54fdf82da7fe7e9a12ffe27b0b9e60"
                ),
                address_expression=(
                    "static_cast<std::optional<pulp::music::ChordRecognitionList> (*)("
                    "std::span<const int>) noexcept>(&pulp::music::recognize_chord)"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "pitch-class-operation",
                "binding": "pulp::music::recognize_chord",
                "operation": "function_call",
                "arguments": (
                    "*pulp::music::PitchClassSet::from_mask(0x091u), "
                    "pulp::music::PitchClass::c"
                ),
            },
            {
                "role": "midi-operation",
                "binding": "pulp::music::recognize_chord",
                "operation": "function_call",
                "arguments": (
                    "[]() { static constexpr int pitches[]{60, 64, 67}; "
                    "return std::span<const int>{pitches}; }()"
                ),
            },
        ],
    ),
    capability(
        key="music.chord-voicing",
        domain="music",
        summary=(
            "Constrained chord voicing and deterministic minimum-motion voice "
            "leading over the bounded MIDI domain."
        ),
        rt_class="control",
        lifecycle={
            "construction": "control-or-offline",
            "prepare": "none",
            "process": "control-or-offline",
            "reset": "none",
            "release": "none",
        },
        state_model="Pure bounded search with fixed local work tables and no retained state.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="chord formulas, prior voices, MIDI range, and voicing constraints",
        output_domain="ordered MIDI-note voicings and summed semitone motion",
        units=["MIDI note", "semitones", "voice count"],
        latency="request-bound",
        tail="none",
        scheduling="request-synchronous",
        status="experimental",
        bindings=[
            binding(
                role="voicing-operation",
                kind="cpp_function",
                include="pulp/music/voicing.hpp",
                qualified_name="pulp::music::voice_chord",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:2e9c5c02b29c3cba699b5f4c6cf89207626aa3fd19cbe4b46bfaf2642925d95d"
                ),
            ),
            binding(
                role="voice-leading-operation",
                kind="cpp_function",
                include="pulp/music/voicing.hpp",
                qualified_name="pulp::music::minimum_motion_voice_leading",
                target="Pulp::music",
                header_fingerprint=(
                    "sha256:2e9c5c02b29c3cba699b5f4c6cf89207626aa3fd19cbe4b46bfaf2642925d95d"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "voicing-operation",
                "binding": "pulp::music::voice_chord",
                "operation": "function_call",
                "arguments": (
                    "60, *pulp::music::ChordFormula::for_quality("
                    "pulp::music::ChordQuality::major), pulp::music::VoicingConstraints{}"
                ),
            },
            {
                "role": "voice-leading-operation",
                "binding": "pulp::music::minimum_motion_voice_leading",
                "operation": "function_call",
                "arguments": (
                    "[]() { static constexpr int pitches[]{60, 64, 67}; "
                    "return std::span<const int>{pitches}; }(), "
                    "pulp::music::PitchClass::f, "
                    "*pulp::music::ChordFormula::for_quality("
                    "pulp::music::ChordQuality::major), pulp::music::MidiRange{}"
                ),
            },
        ],
    ),
    capability(
        key="timebase.tick",
        contract_version={"major": 1, "minor": 1},
        domain="timebase",
        summary=(
            "Saturating integer musical position on the 705600-tick "
            "quarter-note grid."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model="Value type with saturating arithmetic over signed 64-bit ticks.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="document ticks",
        output_domain="document ticks",
        units=["ticks"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/timebase/tick.hpp",
                qualified_name="pulp::timebase::TickPosition",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:e648c10386afc397349a341787aa4773326b551fb8f8f33e08bc9925aea42452"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::timebase::TickPosition",
            "operation": "construct",
            "arguments": "1",
        }],
    ),
    capability(
        key="timebase.swing",
        contract_version={"major": 1, "minor": 1},
        domain="timebase",
        summary=(
            "Exact rational swing projection with bounded-rounding recovery over "
            "integer document ticks."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model=(
            "Pure value and free-function transforms; unswing recovers within the "
            "documented rounding bound, and invalid inputs leave positions unchanged."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="document ticks and rational swing",
        output_domain="document ticks",
        units=["ticks", "rational ratio"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="configuration",
                kind="cpp_type",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::SwingRatio",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
            binding(
                role="forward-operation",
                kind="cpp_function",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::swing_position",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
            binding(
                role="inverse-operation",
                kind="cpp_function",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::unswing_position",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "configuration",
                "binding": "pulp::timebase::SwingRatio",
                "operation": "construct",
                "arguments": "1, 2",
            },
            {
                "role": "forward-operation",
                "binding": "pulp::timebase::swing_position",
                "operation": "function_call",
                "arguments": (
                    "pulp::timebase::TickPosition{1}, "
                    "pulp::timebase::TickDuration{2}, pulp::timebase::kStraightSwing"
                ),
            },
            {
                "role": "inverse-operation",
                "binding": "pulp::timebase::unswing_position",
                "operation": "function_call",
                "arguments": (
                    "pulp::timebase::TickPosition{1}, "
                    "pulp::timebase::TickDuration{2}, pulp::timebase::kStraightSwing"
                ),
            },
        ],
    ),
    capability(
        key="timebase.beat-division",
        domain="timebase",
        summary="Canonical persisted beat-division vocabulary with exact tick conversion.",
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model="Pure enum, fraction, and tick-duration value conversion.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="persisted beat-division ordinal",
        output_domain="exact quarter-note fraction and document tick duration",
        units=["quarter-note fraction", "ticks"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="vocabulary",
                kind="cpp_type",
                include="pulp/timebase/beat_division.hpp",
                qualified_name="pulp::timebase::BeatDivision",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:a6f8a5abd5184d33e08c38ed24565b8ed18df756ce63688594a7a3b4ca6ed570"
                ),
            ),
            binding(
                role="tick-conversion",
                kind="cpp_function",
                include="pulp/timebase/beat_division.hpp",
                qualified_name="pulp::timebase::division_ticks",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:a6f8a5abd5184d33e08c38ed24565b8ed18df756ce63688594a7a3b4ca6ed570"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "vocabulary",
                "binding": "pulp::timebase::BeatDivision",
                "operation": "construct",
                "arguments": "pulp::timebase::BeatDivision::Quarter",
            },
            {
                "role": "tick-conversion",
                "binding": "pulp::timebase::division_ticks",
                "operation": "function_call",
                "arguments": "pulp::timebase::BeatDivision::Quarter",
            },
        ],
    ),
    capability(
        key="timebase.coordinate-random",
        domain="timebase",
        summary="Stateless seeded probability keyed by stable musical coordinates.",
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "none",
            "release": "none",
        },
        state_model="Pure coordinate hash and exact integer-ratio predicate with no mutable RNG.",
        seed_model="caller-supplied 64-bit seed plus tick, lane, cycle, and stream coordinates",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="seed, musical coordinate, and exact probability ratio",
        output_domain="deterministic probability selection",
        units=["ticks", "unsigned integer ratio"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="coordinate",
                kind="cpp_type",
                include="pulp/timebase/coordinate_random.hpp",
                qualified_name="pulp::timebase::RandomCoordinate",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:327487f1a49eb2f8729a6db7ba493ad220f634fec2cb4c7295a300f0a68ddae5"
                ),
            ),
            binding(
                role="probability-operation",
                kind="cpp_function",
                include="pulp/timebase/coordinate_random.hpp",
                qualified_name="pulp::timebase::coordinate_chance",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:327487f1a49eb2f8729a6db7ba493ad220f634fec2cb4c7295a300f0a68ddae5"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "coordinate",
                "binding": "pulp::timebase::RandomCoordinate",
                "operation": "construct",
                "arguments": "pulp::timebase::TickPosition{0}, 0, 0, 0",
            },
            {
                "role": "probability-operation",
                "binding": "pulp::timebase::coordinate_chance",
                "operation": "function_call",
                "arguments": "0, pulp::timebase::RandomCoordinate{}, 1, 2",
            },
        ],
    ),
    capability(
        key="timebase.grid-projection",
        domain="timebase",
        summary="Bounded projection of musical grid points through resolved transport ranges.",
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "caller-replaces-resolved-ranges",
            "release": "none",
        },
        state_model=(
            "Pure projection over caller-owned immutable compiled tempo and meter maps, "
            "resolved ranges, and fixed output storage."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="resolved half-open transport ranges and beat division",
        output_domain="bounded frame-offset grid events with timeline and monotonic coordinates",
        units=["frames", "samples", "ticks", "bars"],
        latency="zero",
        tail="none",
        scheduling="block-synchronous event projection",
        bindings=[
            binding(
                role="projection-operation",
                kind="cpp_function",
                include="pulp/timebase/grid_projection.hpp",
                qualified_name="pulp::timebase::project_grid",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:cf70650f506ab6b7ca648ca906219b4837a500e8aca8c56095b6acef626bf30c"
                ),
            )
        ],
        _link_probes=[{
            "role": "projection-operation",
            "binding": "pulp::timebase::project_grid",
            "operation": "function_call",
            "arguments": (
                "pulp::timebase::CompiledTempoMap::compile("
                "pulp::timebase::TempoMap{}, pulp::timebase::RationalRate{48000, 1}).value(), "
                "pulp::timebase::CompiledMeterMap::compile(pulp::timebase::MeterMap{}).value(), "
                "pulp::timebase::GridProjectionRequest{}, "
                "std::span<const pulp::timebase::GridProjectionRange>{}, "
                "std::span<pulp::timebase::GridProjectionPoint>{}"
            ),
        }],
    ),
    capability(
        key="timebase.groove-kernel",
        domain="timebase",
        summary="Fixed-capacity swing and groove projection that rejects event reordering.",
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "factory-validation-on-control",
            "process": "audio",
            "reset": "replace-immutable-value",
            "release": "none",
        },
        state_model="Immutable copied groove table with bounded validation and allocation-free lookup.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="authored event ticks, rational swing, and fixed-capacity groove steps",
        output_domain="order-preserving event ticks and velocity scale",
        units=["ticks", "rational ratio", "per-thousand scale"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="validated-factory",
                kind="cpp_function",
                include="pulp/timebase/groove_kernel.hpp",
                qualified_name="pulp::timebase::OrderPreservingGrooveKernel::create",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:169e0104d2ea6164d6924de224ac636eaf11d617f5b844b4a2facaf0cbaa0286"
                ),
            )
        ],
        _link_probes=[{
            "role": "validated-factory",
            "binding": "pulp::timebase::OrderPreservingGrooveKernel::create",
            "operation": "function_call",
            "arguments": "pulp::timebase::GrooveKernelInput{}",
        }],
    ),
    capability(
        key="timebase.trigger-grid",
        domain="timebase",
        summary="Fixed-capacity authored trigger grid with block-invariant window projection.",
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "configure-and-author-on-control",
            "process": "audio",
            "reset": "control-or-audio-when-quiescent",
            "release": "none",
        },
        state_model="Inline fixed-capacity track-step cells; projection does not mutate the grid.",
        seed_model="caller supplies one stable 64-bit probability word per configured coordinate",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="authored trigger cells, cycle origin, half-open tick window, and probability words",
        output_domain="bounded step-major trigger events",
        units=["ticks", "MIDI velocity", "unsigned integer ratio"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/timebase/trigger_grid.hpp",
                qualified_name="pulp::timebase::TriggerGrid<>",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:6a5b7dd7f2b1185ed3a3f5c8ae1ffcf506415fe6083ec772f984010476b337ca"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::timebase::TriggerGrid<>",
            "operation": "member_call",
            "member": "configure",
            "arguments": "1, 1, pulp::timebase::TickDuration{1}",
        }],
    ),
    capability(
        key="timebase.ratchet",
        domain="timebase",
        summary="Clock-locked bounded ratchet subdivision over half-open tick intervals.",
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "none",
            "release": "none",
        },
        state_model="Pure fixed-capacity integer subdivision with caller-owned output storage.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="clock interval, hit count, and half-open projection window",
        output_domain="bounded tick-position ratchet onsets",
        units=["ticks", "event count"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="projection-operation",
                kind="cpp_function",
                include="pulp/timebase/ratchet.hpp",
                qualified_name="pulp::timebase::project_ratchet_interval<>",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:93f1ae1c80b5c5ad255180067adabcc331577e63a6f77b57b06255891926718d"
                ),
            )
        ],
        _link_probes=[{
            "role": "projection-operation",
            "binding": "pulp::timebase::project_ratchet_interval<>",
            "operation": "function_call",
            "arguments": (
                "pulp::timebase::TickPosition{0}, pulp::timebase::TickPosition{4}, 2, "
                "pulp::timebase::TickPosition{0}, pulp::timebase::TickPosition{4}, "
                "std::span<pulp::timebase::TickPosition>{}"
            ),
        }],
    ),
    capability(
        key="sequence.host-transport-projector",
        contract_version={"major": 1, "minor": 1},
        domain="sequence",
        summary=(
            "Prepared projection from host callback transport into Pulp playback "
            "snapshots."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "control-or-audio-when-quiescent",
            "release": "none",
        },
        state_model=(
            "Prepared tempo-map reference plus bounded callback history and playback "
            "epoch."
        ),
        seed_model="none",
        determinism={
            "repeatability": "not_promised",
            "block_partition": "fixed_partition_only",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="host process context",
        output_domain="playback transport snapshot",
        units=["samples", "ticks", "beats per minute"],
        latency="zero",
        tail="none",
        scheduling="block-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/sequence/host_transport_projector.hpp",
                qualified_name="pulp::sequence::HostTransportProjector",
                target="Pulp::sequence",
                header_fingerprint=(
                    "sha256:3c0a31d541635c9339fadb13b584e9bdec7a4e9e274afd595450f707c8344051"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::sequence::HostTransportProjector",
            "operation": "member_call",
            "member": "reset",
            "arguments": "",
        }],
    ),
    capability(
        key="audio.midi-voice-modulation-adapter",
        domain="audio",
        summary=(
            "Fixed-capacity translation from MIDI voice state into generation-qualified "
            "per-voice modulation state."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "any",
            "prepare": "destination modulation buffers on control; adapter none",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model=(
            "A compile-time-bounded voice array retains activity, note, channel, velocity, "
            "expression, and a monotonic nonzero generation watermark."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="MIDI note identity, expression, and generation-qualified voice events",
        output_domain="fixed per-voice modulation state",
        units=["MIDI note", "MIDI channel", "normalized controller", "generation"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/audio/midi_voice_modulation_adapter.hpp",
            qualified_name="pulp::audio::MidiVoiceModulationAdapter<128>",
            target="Pulp::audio",
            header_fingerprint="sha256:8fb955d00ce0ed708d96c397d5bd9cc7f2926c1414ce9ac9976ad6abdae9ef7a",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::MidiVoiceModulationAdapter<128>",
            "operation": "member_call",
            "member": "reset",
            "arguments": "",
        }],
    ),
    capability(
        key="audio.unison-voice-stack",
        domain="audio",
        summary=(
            "Exclusive fixed-capacity unison stack ownership over a prepared instrument "
            "voice allocator."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control with borrowed prepared-empty allocator",
            "process": "audio",
            "reset": "audio while ownership is exclusive and quiescent",
            "release": "borrowed owner lifetime ends off audio",
        },
        state_model=(
            "A fixed logical-note table owns allocator mutation and retains oldest serials, "
            "generation-qualified child identities, and exact termination state."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="logical note triggers, unison voice layouts, and voice terminations",
        output_domain="generation-qualified child voice allocation and termination records",
        units=["MIDI note", "cents", "normalized gain", "voice generation"],
        latency="zero",
        tail="caller-selected termination tail",
        scheduling="event-synchronous exclusive allocator ownership",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/audio/unison_voice_stack.hpp",
            qualified_name="pulp::audio::UnisonVoiceStackManager<>",
            target="Pulp::audio",
            header_fingerprint="sha256:e5cef314c00880c58fc33618a6657b27811c1afcf003220b9e3176435d8c737d",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::UnisonVoiceStackManager<>",
            "operation": "member_call",
            "member": "prepared",
            "arguments": "",
        }],
    ),
    capability(
        key="audio.voice-note-modulation",
        domain="audio",
        summary=(
            "Transactional conversion of note and controller state into six constant "
            "per-voice modulation lanes."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "none",
            "prepare": "destination modulation buffer on control",
            "process": "audio",
            "reset": "none",
            "release": "none",
        },
        state_model="Stateless validation followed by an atomic six-lane destination overwrite.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="note, velocity, gate, pitch bend, pressure, timbre, and expression",
        output_domain="typed constant voice-modulation lanes",
        units=["MIDI note", "cents", "normalized controller", "frames"],
        latency="zero",
        tail="none",
        scheduling="one transactional write per audio block",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_function",
            include="pulp/audio/voice_runtime_facade.hpp",
            qualified_name="pulp::audio::VoiceNoteModulationBridge::write",
            target="Pulp::audio",
            header_fingerprint="sha256:4237f8cdf64ed1a28330822f41ea8d3fc5a42d955a5b964a54ba24106a7ceab6",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::VoiceNoteModulationBridge::write",
            "operation": "function_call",
            "arguments": (
                "*[]() { static pulp::audio::VoiceModulationBuffer value; return &value; }(), "
                "1u, pulp::audio::VoiceNoteModulationInput{}, "
                "pulp::audio::VoiceNoteModulationRouting{}"
            ),
        }],
    ),
    capability(
        key="audio.voice-runtime-facade",
        domain="audio",
        summary=(
            "Non-owning typed facade over Pulp instrument or synthesiser voice owners "
            "without allocator type erasure."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control borrow",
            "prepare": "owner-specific control",
            "process": "audio",
            "reset": "owner-specific quiescent call",
            "release": "none; facade is non-owning",
        },
        state_model=(
            "The facade retains only an owner pointer; the owner retains voices and spans "
            "passed to facade calls are never retained."
        ),
        seed_model="owner-defined",
        determinism={
            "repeatability": "not_promised",
            "block_partition": "fixed_partition_only",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="voice triggers, note expression, lifecycle events, and owner policy",
        output_domain="typed voice allocation, modulation, telemetry, and termination records",
        units=["MIDI note", "frames", "voice index", "voice generation"],
        latency="owner-defined",
        tail="owner-defined",
        scheduling="event-synchronous over one exclusively borrowed owner",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/audio/voice_runtime_facade.hpp",
            qualified_name=(
                "pulp::audio::VoiceRuntimeFacade<pulp::audio::InstrumentVoiceAllocator>"
            ),
            target="Pulp::audio",
            header_fingerprint="sha256:4237f8cdf64ed1a28330822f41ea8d3fc5a42d955a5b964a54ba24106a7ceab6",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::VoiceRuntimeFacade<pulp::audio::InstrumentVoiceAllocator>",
            "operation": "construct",
            "arguments": (
                "*[]() { static pulp::audio::InstrumentVoiceAllocator value; return &value; }()"
            ),
        }],
    ),
    capability(
        key="audio.onset-detection",
        domain="audio",
        summary="Offline energy, spectral-flux, or high-frequency-content onset detection.",
        rt_class="offline",
        lifecycle={
            "construction": "offline",
            "prepare": "none",
            "process": "offline",
            "reset": "none",
            "release": "result vector destruction off audio",
        },
        state_model="Each call is stateless and returns an owning bounded marker vector.",
        seed_model="none",
        determinism={
            "repeatability": "tolerance_bounded",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="finite mono audio frames and an offline analysis configuration",
        output_domain="ordered onset frame markers with confidence and method",
        units=["audio frames", "normalized confidence", "samples"],
        latency="offline whole-input analysis",
        tail="none",
        scheduling="background or offline only",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/audio/onset_detector.hpp",
            qualified_name="pulp::audio::OnsetDetector",
            target="Pulp::audio",
            header_fingerprint="sha256:d0cac999018c626de8cbd2165150788c1d3be62777cc639d550d7f80f9f56560",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::OnsetDetector",
            "operation": "member_call",
            "member": "detect",
            "arguments": (
                "pulp::audio::BufferView<const float>{}, pulp::audio::OnsetDetectionConfig{}"
            ),
        }],
    ),
    capability(
        key="midi.arpeggiator",
        domain="midi",
        summary=(
            "Bounded sample-accurate held-note arpeggiator with latch, octave, gate, swing, "
            "ordering, ownership, and transport policies."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and compile tempo map on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "Fixed held, pattern, sounding, ownership, and release-debt ledgers retain clock "
            "state and one pending specification without allocation."
        ),
        seed_model="explicit spec random_seed indexed by musical step",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="held MIDI notes, absolute sample block, tempo map, and transport event",
        output_domain="owned note-on and note-off MIDI event stream",
        units=["samples", "beats", "MIDI note", "octaves", "rational gate"],
        latency="sample-scheduled within the supplied block",
        tail="owned note releases and bounded release debt",
        scheduling="absolute-sample transport-aware",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/arpeggiator.hpp",
            qualified_name="pulp::midi::Arpeggiator<>",
            target="Pulp::midi",
            header_fingerprint="sha256:5178607c04e0070ad9e22ab028a2c87aa80c925350e0743614c0fb44aa426445",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::Arpeggiator<>",
            "operation": "member_call",
            "member": "valid",
            "arguments": "",
        }],
    ),
    capability(
        key="midi.controller-mapping",
        domain="midi",
        summary="Fixed-capacity controller remapping with bounded physical-domain smoothing.",
        rt_class="audio",
        lifecycle={
            "construction": "any",
            "prepare": "set rules and reserve output on control",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model="Fixed rule table plus per-rule smoother value, target, and absolute sample.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="MIDI controller events, mapping rules, and absolute samples",
        output_domain="remapped MIDI controller events and smoothed values",
        units=["MIDI controller", "normalized value", "samples"],
        latency="zero event latency",
        tail="smoothing state until target convergence or reset",
        scheduling="absolute-sample event stream",
        bindings=[binding(
            role="entrypoint", kind="cpp_type",
            include="pulp/midi/controller_utility_kernels.hpp",
            qualified_name="pulp::midi::ControllerMapper<>", target="Pulp::midi",
            header_fingerprint="sha256:4e28f94ab7787db493e743d3f8cf65d1d6c0015dfd15e9425f6006dfe1d9c711",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::midi::ControllerMapper<>",
            "operation": "member_call", "member": "reset", "arguments": "",
        }],
    ),
    capability(
        key="midi.scale-aware-mpe-pitch",
        domain="midi",
        summary="Scale-aware conversion of MPE member pitch into bounded cents with glide.",
        rt_class="audio",
        lifecycle={
            "construction": "any", "prepare": "none", "process": "audio",
            "reset": "audio", "release": "none",
        },
        state_model="Fixed sanitized specification and current, target, and initialized state.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact", "block_partition": "fixed_partition_only",
            "platform_scope": "same_build", "transport_history": "irrelevant",
        },
        input_domain="MPE note state, musical scale, and glide sample count",
        output_domain="current and target pitch offset",
        units=["cents", "semitones", "samples", "seconds"],
        latency="zero",
        tail="glide state until convergence or reset",
        scheduling="event update plus block advance",
        bindings=[binding(
            role="entrypoint", kind="cpp_type",
            include="pulp/midi/controller_utility_kernels.hpp",
            qualified_name="pulp::midi::ScaleAwareMpePitch", target="Pulp::midi",
            header_fingerprint="sha256:4e28f94ab7787db493e743d3f8cf65d1d6c0015dfd15e9425f6006dfe1d9c711",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::midi::ScaleAwareMpePitch",
            "operation": "member_call", "member": "reset", "arguments": "",
        }],
    ),
    capability(
        key="midi.note-length-shaping",
        domain="midi",
        summary="Fixed-capacity note duration shaping with overlap ownership and release debt.",
        rt_class="audio",
        lifecycle={
            "construction": "any", "prepare": "reserve output on control",
            "process": "audio", "reset": "audio with prepared output", "release": "none",
        },
        state_model=(
            "Fixed due-note slots and ordered forwarded or suppressed ownership, release debt, "
            "quarantine, and one pending specification."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact", "block_partition": "invariant",
            "platform_scope": "cross_platform", "transport_history": "input",
        },
        input_domain="MIDI note stream, duration specification, and absolute sample block",
        output_domain="duration-shaped owned MIDI note stream",
        units=["samples", "MIDI note", "MIDI channel"],
        latency="zero for attacks; releases scheduled by duration",
        tail="scheduled releases and bounded release debt",
        scheduling="absolute-sample transport-aware",
        bindings=[binding(
            role="entrypoint", kind="cpp_type",
            include="pulp/midi/note_utility_kernels.hpp",
            qualified_name="pulp::midi::NoteLengthShaper<>", target="Pulp::midi",
            header_fingerprint="sha256:6b4005421eb2f5f98b88765883154451e800a76188a8608887755201f9cc21e6",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::midi::NoteLengthShaper<>",
            "operation": "member_call", "member": "valid", "arguments": "",
        }],
    ),
    capability(
        key="midi.monophonic-note-selection",
        domain="midi",
        summary="Low, high, or last-note monophonic selection with legato and glide state.",
        rt_class="audio",
        lifecycle={
            "construction": "any", "prepare": "reserve output on control",
            "process": "audio", "reset": "audio with prepared output", "release": "none",
        },
        state_model=(
            "Fixed 2048-key depth, velocity, and ordering tables retain selected pitch and "
            "one pending specification."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact", "block_partition": "fixed_partition_only",
            "platform_scope": "same_build", "transport_history": "irrelevant",
        },
        input_domain="MIDI notes, selection priority, legato policy, and glide advance",
        output_domain="owned monophonic MIDI stream and pitch state",
        units=["MIDI note", "samples", "seconds"],
        latency="zero event latency",
        tail="held-note selection and glide until release or reset",
        scheduling="event stream plus explicit glide advance",
        bindings=[binding(
            role="entrypoint", kind="cpp_type",
            include="pulp/midi/note_utility_kernels.hpp",
            qualified_name="pulp::midi::MonophonicNoteSelector", target="Pulp::midi",
            header_fingerprint="sha256:6b4005421eb2f5f98b88765883154451e800a76188a8608887755201f9cc21e6",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::midi::MonophonicNoteSelector",
            "operation": "member_call", "member": "pitch_state", "arguments": "",
        }],
    ),
    capability(
        key="midi.channel-routing",
        domain="midi",
        summary="Lifecycle-safe MIDI channel routing with overlapping-note ownership.",
        rt_class="audio",
        lifecycle={
            "construction": "any", "prepare": "reserve MIDI and UMP output on control",
            "process": "audio", "reset": "audio with prepared output", "release": "none",
        },
        state_model="Fixed forwarded and suppressed MIDI and UMP ledgers plus release debt.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact", "block_partition": "fixed_partition_only",
            "platform_scope": "cross_platform", "transport_history": "irrelevant",
        },
        input_domain="MIDI 1 or UMP channel voice events and a channel route specification",
        output_domain="routed lifecycle-complete MIDI 1 or UMP event stream",
        units=["MIDI channel", "MIDI group", "sample offset"],
        latency="zero",
        tail="bounded release debt until emitted or reset",
        scheduling="input-stable event order with lifecycle reconciliation",
        bindings=[binding(
            role="entrypoint", kind="cpp_type", include="pulp/midi/routing_utility_kernels.hpp",
            qualified_name="pulp::midi::ChannelRouter", target="Pulp::midi",
            header_fingerprint="sha256:0090aca546341f6958fde703b8e0c7641999ecdd58186d2adb96957267d55718",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::midi::ChannelRouter",
            "operation": "member_call", "member": "valid", "arguments": "",
        }],
    ),
    capability(
        key="midi.note-range-filtering",
        domain="midi",
        summary="Lifecycle-safe note-range filtering for MIDI 1 and UMP event streams.",
        rt_class="audio",
        lifecycle={
            "construction": "any", "prepare": "reserve MIDI and UMP output on control",
            "process": "audio", "reset": "audio with prepared output", "release": "none",
        },
        state_model="Fixed forwarded and suppressed MIDI and UMP ledgers plus release debt.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact", "block_partition": "fixed_partition_only",
            "platform_scope": "cross_platform", "transport_history": "irrelevant",
        },
        input_domain="MIDI 1 or UMP notes and inclusive note-range specification",
        output_domain="filtered lifecycle-complete MIDI 1 or UMP event stream",
        units=["MIDI note", "sample offset"],
        latency="zero", tail="bounded release debt until emitted or reset",
        scheduling="input-stable event order with lifecycle reconciliation",
        bindings=[binding(
            role="entrypoint", kind="cpp_type", include="pulp/midi/routing_utility_kernels.hpp",
            qualified_name="pulp::midi::NoteRangeFilter", target="Pulp::midi",
            header_fingerprint="sha256:0090aca546341f6958fde703b8e0c7641999ecdd58186d2adb96957267d55718",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::midi::NoteRangeFilter",
            "operation": "member_call", "member": "valid", "arguments": "",
        }],
    ),
    capability(
        key="midi.keyboard-split",
        domain="midi",
        summary="Lifecycle-safe two-way keyboard split for MIDI 1 and UMP event streams.",
        rt_class="audio",
        lifecycle={
            "construction": "any", "prepare": "reserve both MIDI and UMP outputs on control",
            "process": "audio", "reset": "audio with prepared outputs", "release": "none",
        },
        state_model="Two fixed forwarded and suppressed MIDI and UMP ledgers plus release debt.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact", "block_partition": "fixed_partition_only",
            "platform_scope": "cross_platform", "transport_history": "irrelevant",
        },
        input_domain="MIDI 1 or UMP notes and split-point routing specification",
        output_domain="two lifecycle-complete MIDI 1 or UMP event streams",
        units=["MIDI note", "MIDI channel", "sample offset"],
        latency="zero", tail="bounded release debt until emitted or reset",
        scheduling="input-stable event order with lifecycle reconciliation",
        bindings=[binding(
            role="entrypoint", kind="cpp_type", include="pulp/midi/routing_utility_kernels.hpp",
            qualified_name="pulp::midi::KeyboardSplit", target="Pulp::midi",
            header_fingerprint="sha256:0090aca546341f6958fde703b8e0c7641999ecdd58186d2adb96957267d55718",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::midi::KeyboardSplit",
            "operation": "member_call", "member": "valid", "arguments": "",
        }],
    ),
    capability(
        key="music.markov-transition",
        domain="music",
        summary="Prepared fixed-capacity weighted Markov transition selection.",
        rt_class="any",
        lifecycle={
            "construction": "any", "prepare": "control or non-concurrent any",
            "process": "any", "reset": "any", "release": "none",
        },
        state_model="Fixed prepared cumulative transition table and per-state totals.",
        seed_model="caller supplies each uint64 random word",
        determinism={
            "repeatability": "bit_exact", "block_partition": "not_applicable",
            "platform_scope": "cross_platform", "transport_history": "irrelevant",
        },
        input_domain="bounded transition weights, current state, and caller random word",
        output_domain="next state index or explicit preparation error",
        units=["state index", "integer weight", "random word"],
        latency="zero", tail="none", scheduling="one draw per requested transition",
        bindings=[binding(
            role="entrypoint", kind="cpp_type", include="pulp/music/markov.hpp",
            qualified_name="pulp::music::PreparedMarkovModel<>", target="Pulp::music",
            header_fingerprint="sha256:8b013fa744ad12df04d4efc454330258afa3977371a66582b91448e6f1c086ef",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::music::PreparedMarkovModel<>",
            "operation": "member_call", "member": "clear", "arguments": "",
        }],
    ),
    capability(
        key="music.pattern-generation",
        domain="music",
        summary=(
            "Bounded Euclidean, versioned recipe, walking, cellular, and looping-shift-register "
            "pattern operations."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any", "prepare": "configure walker at any non-concurrent point",
            "process": "any", "reset": "any", "release": "none",
        },
        state_model=(
            "Functions return fixed BinaryPattern values; only PatternWalker retains bounded "
            "cursor, direction, length, and mode state."
        ),
        seed_model="caller supplies each uint64 random word",
        determinism={
            "repeatability": "bit_exact", "block_partition": "not_applicable",
            "platform_scope": "cross_platform", "transport_history": "irrelevant",
        },
        input_domain="step counts, pulses, signed rotation, rules, mutation chance, and random word",
        output_domain="fixed-capacity binary patterns and walker step indices",
        units=["steps", "pulses", "signed rotation", "probability ratio", "random word"],
        latency="zero", tail="none", scheduling="caller-clocked",
        bindings=[
            binding(
                role="euclidean", kind="cpp_function", include="pulp/music/pattern.hpp",
                qualified_name="pulp::music::euclidean_pattern<64>", target="Pulp::music",
                header_fingerprint="sha256:ca7cc580012662880465801996dca4b6b6e23aec07c6ec07d573788347bf32f5",
            ),
            binding(
                role="recipe", kind="cpp_function", include="pulp/music/pattern.hpp",
                qualified_name="pulp::music::materialize_pattern<64>", target="Pulp::music",
                header_fingerprint="sha256:ca7cc580012662880465801996dca4b6b6e23aec07c6ec07d573788347bf32f5",
            ),
            binding(
                role="walker", kind="cpp_type", include="pulp/music/pattern.hpp",
                qualified_name="pulp::music::PatternWalker<>", target="Pulp::music",
                header_fingerprint="sha256:ca7cc580012662880465801996dca4b6b6e23aec07c6ec07d573788347bf32f5",
            ),
            binding(
                role="cellular", kind="cpp_function", include="pulp/music/pattern.hpp",
                qualified_name="pulp::music::cellular_evolve<64>", target="Pulp::music",
                header_fingerprint="sha256:ca7cc580012662880465801996dca4b6b6e23aec07c6ec07d573788347bf32f5",
            ),
            binding(
                role="shift_register", kind="cpp_function", include="pulp/music/pattern.hpp",
                qualified_name="pulp::music::looping_shift_register<64>", target="Pulp::music",
                header_fingerprint="sha256:ca7cc580012662880465801996dca4b6b6e23aec07c6ec07d573788347bf32f5",
            ),
        ],
        _link_probes=[
            {"role": "euclidean", "binding": "pulp::music::euclidean_pattern<64>",
             "operation": "function_call", "arguments": "8u, 3u, 0"},
            {"role": "recipe", "binding": "pulp::music::materialize_pattern<64>",
             "operation": "function_call", "arguments": "pulp::music::EuclideanPatternRecipe{}"},
            {"role": "walker", "binding": "pulp::music::PatternWalker<>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "cellular", "binding": "pulp::music::cellular_evolve<64>",
             "operation": "function_call",
             "arguments": "pulp::music::BinaryPattern<>{}, 30u, pulp::music::CellularBoundary::wrap"},
            {"role": "shift_register", "binding": "pulp::music::looping_shift_register<64>",
             "operation": "function_call",
             "arguments": "pulp::music::BinaryPattern<>{}, pulp::music::MutationChance{}, 0u"},
        ],
    ),
    capability(
        key="music.rhythm-relationship",
        domain="music",
        summary=(
            "Deterministic derivation of a target rhythm from a source lane using relationship, "
            "phase, length, collision, and density policies."
        ),
        rt_class="any",
        lifecycle={
            "construction": "none", "prepare": "none", "process": "any",
            "reset": "none", "release": "none",
        },
        state_model="Pure fixed-capacity operation with no retained state.",
        seed_model="explicit seed, cycle, and lane coordinates provide stateless draws",
        determinism={
            "repeatability": "bit_exact", "block_partition": "not_applicable",
            "platform_scope": "cross_platform", "transport_history": "irrelevant",
        },
        input_domain="source mask and relationship, mapping, phase, collision, density, and draw policy",
        output_domain="derived fixed-capacity target rhythm or explicit error",
        units=["steps", "signed phase", "onset count", "seed", "cycle", "lane"],
        latency="zero", tail="none", scheduling="caller-clocked whole-pattern derivation",
        bindings=[binding(
            role="entrypoint", kind="cpp_function",
            include="pulp/music/rhythm_relationship.hpp",
            qualified_name="pulp::music::derive_rhythm_relationship<64>", target="Pulp::music",
            header_fingerprint="sha256:bcb234becb7b912a516ebf4d33152fcbf4e326b59251a5977b4c1a2e653de433",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::music::derive_rhythm_relationship<64>",
            "operation": "function_call",
            "arguments": "pulp::music::BinaryPattern<>{}, pulp::music::RhythmRelationshipConfig{}",
        }],
    ),
    capability(
        key="signal.streaming-analysis-frontends", domain="signal",
        summary="Prepared streaming chroma and onset-novelty analysis frontends.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control; may allocate retained FFT storage",
                   "process": "audio analysis owner", "reset": "audio analysis owner",
                   "release": "destruction off audio"},
        state_model=(
            "Fixed-capacity window, history, and scratch plus retained FFT storage and "
            "cadence, timestamp, chroma, and novelty state."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="prepared planar finite audio blocks",
        output_domain="timestamped chroma or onset-novelty frames",
        units=["samples", "frames", "hertz", "normalized magnitude"],
        latency="analysis readiness N-1 samples; chroma center lookback N/2",
        tail="no padded analysis tail", scheduling="streaming hop cadence",
        bindings=[
            binding(role="chroma", kind="cpp_type", include="pulp/signal/analysis_frontends.hpp",
                    qualified_name="pulp::signal::ChromaFrontEndT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:5ccf5005856974faaf4e834abd28a0fc9e25cd816cf8a8a91d3baffc0f92a17e"),
            binding(role="onset_novelty", kind="cpp_type", include="pulp/signal/analysis_frontends.hpp",
                    qualified_name="pulp::signal::OnsetNoveltyFrontEndT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:5ccf5005856974faaf4e834abd28a0fc9e25cd816cf8a8a91d3baffc0f92a17e"),
        ],
        _link_probes=[
            {"role": "chroma", "binding": "pulp::signal::ChromaFrontEndT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "onset_novelty", "binding": "pulp::signal::OnsetNoveltyFrontEndT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.routing-primitives", domain="signal",
        summary=(
            "Bounded matrix, mid-side, N-way crossfade, click-free switching, and path-latency "
            "alignment primitives."
        ),
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control for matrix and aligner storage",
                   "process": "audio", "reset": "audio", "release": "destruction off audio"},
        state_model=(
            "Fixed matrix ramps and switch weights plus prepared bounded per-path delay storage."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio buffers, signed gains, path positions, and intrinsic latencies",
        output_domain="routed, transformed, switched, and latency-aligned audio",
        units=["samples", "frames", "linear gain", "path index"],
        latency="zero except aligner maximum declared path latency",
        tail="prepared delay history until drained or reset", scheduling="sample-continuous",
        bindings=[
            binding(role="matrix", kind="cpp_type", include="pulp/signal/audio_matrix_mixer.hpp",
                    qualified_name="pulp::signal::AudioMatrixMixerT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:80ac853fa55eb0bee8730a05dd7cfac32847832eb4f44cb6e0ad5507781db6b1"),
            binding(role="mid_side", kind="cpp_function", include="pulp/signal/mid_side.hpp",
                    qualified_name="pulp::signal::mid_side_encode_block<float>", target="Pulp::signal",
                    header_fingerprint="sha256:709cb80f95a21bd20ec21ac9f055da2d41cf47b86ce91cdd444a408e00ff6443",
                    address_expression=("static_cast<bool (*)(const float*, const float*, float*, "
                                        "float*, std::size_t) noexcept>("
                                        "&pulp::signal::mid_side_encode_block<float>)")),
            binding(role="nway", kind="cpp_function", include="pulp/signal/nway_crossfade.hpp",
                    qualified_name="pulp::signal::nway_constant_power_gains<float>", target="Pulp::signal",
                    header_fingerprint="sha256:b1d33521a93b3cefd167f9441b6523b83fdffe1b873a20fb50caedac790ce9be",
                    address_expression=("static_cast<bool (*)(float, std::span<float>) noexcept>("
                                        "&pulp::signal::nway_constant_power_gains<float>)")),
            binding(role="aligner", kind="cpp_type", include="pulp/signal/path_latency_aligner.hpp",
                    qualified_name="pulp::signal::PathLatencyAlignerT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:1b50ede989667a0a3354900646e3079df8a550474604cf7958c92a7b27f58e8a"),
            binding(role="switcher", kind="cpp_type", include="pulp/signal/path_switcher.hpp",
                    qualified_name="pulp::signal::ClickFreePathSwitcherT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:adbf41862725587e477711ac144f5326cdcff3a31560aba47708a42d5cd471f1"),
        ],
        _link_probes=[
            {"role": "matrix", "binding": "pulp::signal::AudioMatrixMixerT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "mid_side", "binding": "pulp::signal::mid_side_encode_block<float>",
             "operation": "function_call", "arguments": "nullptr, nullptr, nullptr, nullptr, 0"},
            {"role": "nway", "binding": "pulp::signal::nway_constant_power_gains<float>",
             "operation": "function_call", "arguments": "0.0f, std::span<float>{}"},
            {"role": "aligner", "binding": "pulp::signal::PathLatencyAlignerT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "switcher", "binding": "pulp::signal::ClickFreePathSwitcherT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.modulation-primitives", domain="signal",
        summary="Shared curve vocabulary, bounded breakpoint envelopes, and rise/fall generators.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control configuration",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed-capacity breakpoint program plus sample phase and stage state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="authored endpoints, curves, sample durations, and triggers",
        output_domain="sample-synchronous caller-domain modulation values",
        units=["samples", "frames", "caller-defined value", "normalized progress"],
        latency="zero", tail="program-defined until idle", scheduling="sample-synchronous",
        bindings=[
            binding(role="curve", kind="cpp_type", include="pulp/signal/modulation_curve.hpp",
                    qualified_name="pulp::signal::ModulationCurve", target="Pulp::signal",
                    header_fingerprint="sha256:da89147c18e3c0479bf02294273bc7ec8ff76bc83cc7e198e645b14fe100a298"),
            binding(role="breakpoint", kind="cpp_type", include="pulp/signal/breakpoint_envelope.hpp",
                    qualified_name="pulp::signal::BreakpointEnvelopeT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:c70262f82754eff2117acabf88d55e8ccc86d0e783d6cea6e04d6af6a671a9f7"),
            binding(role="rise_fall", kind="cpp_type", include="pulp/signal/rise_fall_generator.hpp",
                    qualified_name="pulp::signal::RiseFallGeneratorT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:67185305462aff7fef2f089d00f0bf6eeaa5708436e3f042fca6b8271c4b3fa3"),
        ],
        _link_probes=[
            {"role": "curve", "binding": "pulp::signal::ModulationCurve",
             "operation": "construct", "arguments": ""},
            {"role": "breakpoint", "binding": "pulp::signal::BreakpointEnvelopeT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "rise_fall", "binding": "pulp::signal::RiseFallGeneratorT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.dither-quantizer", domain="signal",
        summary="Seeded quantization with selectable TPDF dither and first- or second-order shaping.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "none", "process": "audio",
                   "reset": "audio", "release": "none"},
        state_model="Fixed random generator and first- or second-order quantization error feedback.",
        seed_model="public uint32 seed",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio samples, bit depth, dither mode, and shaping order",
        output_domain="quantized audio samples", units=["samples", "bits", "normalized amplitude"],
        latency="zero", tail="continuous dither while enabled; reset clears error state",
        scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/dither.hpp",
                         qualified_name="pulp::signal::DitherQuantizerT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:72536ca7644208062b3769cceddfc8aa935d4b441db999b5a61171947446a9af")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::DitherQuantizerT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.stochastic-sources", domain="signal",
        summary="Seeded dust, LFSR, tilted continuous noise, and velvet-noise grid sources.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control configuration and NoiseTilt prepare",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed integer RNG, LFSR, and grid state plus fixed NoiseTilt biquad state.",
        seed_model="public seed per instance",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="seed, event density, register mask, spectral tilt, and level",
        output_domain="bounded stochastic audio samples, impulses, and control values",
        units=["samples", "hertz", "decibels per octave", "normalized amplitude"],
        latency="zero", tail="zero", scheduling="sample-synchronous",
        bindings=[
            binding(role="dust", kind="cpp_type", include="pulp/signal/dust.hpp",
                    qualified_name="pulp::signal::DustT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:89ad91c2a9a54ab6f897d379c3587df4566c402abad75b1f71a802b66291684d"),
            binding(role="lfsr", kind="cpp_type", include="pulp/signal/lfsr.hpp",
                    qualified_name="pulp::signal::LfsrT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:16f353a4e8c715db25f84696bf5ff50ecdbd96aa29364dc3be1232c9e8a209de"),
            binding(role="tilt", kind="cpp_type", include="pulp/signal/noise_tilt.hpp",
                    qualified_name="pulp::signal::NoiseTiltT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:016c8e7e818ad23543da6e37c1e96e41557ed53f898afe80f8d9d65666912adb"),
            binding(role="velvet", kind="cpp_type", include="pulp/signal/velvet_noise.hpp",
                    qualified_name="pulp::signal::VelvetNoiseGridT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:22c73b19189ad33fe12be75c3e8d20f6c7b098810756e31c6bd2dc3f02b75973"),
        ],
        _link_probes=[
            {"role": role, "binding": name, "operation": "member_call", "member": "reset", "arguments": ""}
            for role, name in [
                ("dust", "pulp::signal::DustT<float>"), ("lfsr", "pulp::signal::LfsrT<float>"),
                ("tilt", "pulp::signal::NoiseTiltT<float>"),
                ("velvet", "pulp::signal::VelvetNoiseGridT<float>"),
            ]
        ],
    ),
    capability(
        key="signal.fm-operator-engine", domain="signal",
        summary="Fixed-capacity eight-operator FM and phase-modulation synthesis engine.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control", "process": "audio",
                   "reset": "audio", "release": "none"},
        state_model="Fixed operator routing, phase, feedback, and linear envelope state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="note frequency, velocity, operator settings, and routing matrices",
        output_domain="mono FM or phase-modulated audio",
        units=["samples", "hertz", "frames", "linear gain", "radians"],
        latency="zero", tail="exact longest configured release", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/fm_operator_engine.hpp",
                         qualified_name="pulp::signal::FmOperatorEngineT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:b92825aa49b916e8c346bf3331fa1dc45759e9589b16a726d861e13c0815c452")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::FmOperatorEngineT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.fractional-delay", domain="signal",
        summary="Prepared causal fractional-delay history and line with Lagrange or Thiran interpolation.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control; may allocate bounded history",
                   "process": "audio", "reset": "audio", "release": "destruction off audio"},
        state_model="Prepared bounded circular history plus interpolation and first-order all-pass state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio samples and causal fractional delay requests",
        output_domain="delayed interpolated audio or explicit status",
        units=["samples", "frames"],
        latency="zero processing overhead; requested delay is signal-path latency",
        tail="prepared delay history until overwritten or reset", scheduling="sample-synchronous",
        bindings=[
            binding(role="line", kind="cpp_type", include="pulp/signal/fractional_delay.hpp",
                    qualified_name="pulp::signal::FractionalDelayLineT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:df2c07847d8b5f49134a7f274c1114cd475c8ed9ac820044ec8c7da3906a5a17"),
            binding(role="history", kind="cpp_type", include="pulp/signal/fractional_delay.hpp",
                    qualified_name="pulp::signal::FractionalDelayHistoryT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:df2c07847d8b5f49134a7f274c1114cd475c8ed9ac820044ec8c7da3906a5a17"),
        ],
        _link_probes=[
            {"role": "line", "binding": "pulp::signal::FractionalDelayLineT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "history", "binding": "pulp::signal::FractionalDelayHistoryT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.nonlinear-shaping", domain="signal",
        summary="Antialiased multistage wavefolding, Chebyshev harmonic shaping, and ring modulation.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control; may allocate oversampler storage",
                   "process": "audio", "reset": "audio", "release": "destruction off audio"},
        state_model="Oversampler state plus fixed shaper stages, DC policy, harmonics, and carrier state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio and shaping, alias-policy, harmonic, or carrier controls",
        output_domain="nonlinearly shaped audio",
        units=["samples", "hertz", "normalized amplitude", "oversampling factor"],
        latency="reported by alias policy", tail="twice FIR latency or infinite for nonzero DC output",
        scheduling="sample-synchronous",
        bindings=[
            binding(role="wavefolder", kind="cpp_type", include="pulp/signal/nonlinear_shaping.hpp",
                    qualified_name="pulp::signal::MultistageWavefolderT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:bce155041cc613f9e47890bd07c1a4fa9717695dc3fb5c43c32d4152a471a219"),
            binding(role="chebyshev", kind="cpp_type", include="pulp/signal/nonlinear_shaping.hpp",
                    qualified_name="pulp::signal::ChebyshevHarmonicShaperT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:bce155041cc613f9e47890bd07c1a4fa9717695dc3fb5c43c32d4152a471a219"),
            binding(role="ring", kind="cpp_type", include="pulp/signal/nonlinear_shaping.hpp",
                    qualified_name="pulp::signal::NonlinearRingModulatorT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:bce155041cc613f9e47890bd07c1a4fa9717695dc3fb5c43c32d4152a471a219"),
        ],
        _link_probes=[
            {"role": role, "binding": name, "operation": "member_call", "member": "reset", "arguments": ""}
            for role, name in [
                ("wavefolder", "pulp::signal::MultistageWavefolderT<float>"),
                ("chebyshev", "pulp::signal::ChebyshevHarmonicShaperT<float>"),
                ("ring", "pulp::signal::NonlinearRingModulatorT<float>"),
            ]
        ],
    ),
    capability(
        key="signal.scope-capture", domain="signal",
        summary="Fixed-capacity triggered waveform capture with pretrigger and holdoff.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control configuration",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed pretrigger ring plus armed, triggered, holdoff, and capture state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio samples and trigger, pretrigger, holdoff, and capture policy",
        output_domain="fixed-capacity captured waveform frame",
        units=["samples", "frames", "normalized amplitude"],
        latency="zero audio-path latency; capture readiness is separate",
        tail="none", scheduling="sample-synchronous trigger observation",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/scope_capture.hpp",
                         qualified_name="pulp::signal::ScopeCaptureT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:0989e11d3171977da2f2e61d945fb1442fe9a9dfa9df94eb436b19fed3f104b5")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::ScopeCaptureT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.spectrum-trace", domain="signal",
        summary="Fixed-capacity FFT-bin aggregation, smoothing, weighting, and peak-hold trace.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control configuration",
                   "process": "audio analysis", "reset": "audio", "release": "none"},
        state_model="Fixed trace, smoothing, weighting, and peak-hold arrays without FFT ownership.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite FFT magnitude bins and display-band configuration",
        output_domain="conditioned spectrum trace bands",
        units=["bins", "hertz", "decibels", "frames"],
        latency="zero algorithmic", tail="smoothing and peak state until reset",
        scheduling="one update per supplied spectrum frame",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/spectrum_trace.hpp",
                         qualified_name="pulp::signal::SpectrumTraceT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:2db177e808a86cbe881d3a8c636eb33716db1a0ed556fbf1049a5d23583c5bee")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::SpectrumTraceT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.six-band-eq", domain="signal",
        summary="Fixed six-band stereo equalizer with atomic parameter updates and bounded transitions.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control coefficient design",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed per-channel six-band biquad banks plus optional transition bank state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus six plain-domain frequency, gain, and Q controls",
        output_domain="equalized audio", units=["samples", "hertz", "decibels", "Q"],
        latency="zero", tail="recursive IIR decay", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/six_band_eq.hpp",
                         qualified_name="pulp::signal::SixBandEqT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:85f2051455ad88b04aa92f73aa44605ec07007acd8761df8deecaa755b08066a")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::SixBandEqT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.source-filter-analysis", domain="signal",
        summary="Prepared offline cepstral spectral-envelope and linear-predictive analysis.",
        rt_class="offline",
        lifecycle={"construction": "control", "prepare": "offline; may allocate bounded storage",
                   "process": "offline allocation-free after prepare",
                   "reset": "reprepare or next analysis transaction",
                   "release": "destruction off audio"},
        state_model="Bounded FFT, scratch, coefficient, and model storage with transactional publication.",
        seed_model="none",
        determinism={"repeatability": "tolerance_bounded", "block_partition": "not_applicable",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="log-magnitude frames or finite time-domain samples",
        output_domain="cepstral envelope or linear-predictive model",
        units=["bins", "samples", "normalized coefficients", "power"],
        latency="offline whole-frame analysis", tail="none", scheduling="offline request",
        bindings=[
            binding(role="cepstral", kind="cpp_type", include="pulp/signal/source_filter_analysis.hpp",
                    qualified_name="pulp::signal::CepstralEnvelopeAnalyzerT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:eb64faa37ff3e4b93fc884943fda3ea4acc723b61b9b8593abac3f0102dd3d26"),
            binding(role="lpc", kind="cpp_type", include="pulp/signal/source_filter_analysis.hpp",
                    qualified_name="pulp::signal::LpcAnalyzerT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:eb64faa37ff3e4b93fc884943fda3ea4acc723b61b9b8593abac3f0102dd3d26"),
        ],
        _link_probes=[
            {"role": "cepstral", "binding": "pulp::signal::CepstralEnvelopeAnalyzerT<float>",
             "operation": "construct", "arguments": ""},
            {"role": "lpc", "binding": "pulp::signal::LpcAnalyzerT<float>",
             "operation": "construct", "arguments": ""},
        ],
    ),
    capability(
        key="signal.unison-voice-primitives", domain="signal",
        summary="Deterministic unison voice layout and PolyBLEP supersaw oscillator bank.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control configure and prepare",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed deterministic layout parameters and bounded oscillator array.",
        seed_model="public seed and note-instance identity",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="note frequency, unison specification, seed, and absolute frame",
        output_domain="stereo unison audio and per-child layout parameters",
        units=["samples", "hertz", "cents", "frames", "pan", "linear gain"],
        latency="zero", tail="continuous while voice is active", scheduling="absolute-frame audio",
        bindings=[
            binding(role="layout", kind="cpp_type", include="pulp/signal/unison.hpp",
                    qualified_name="pulp::signal::UnisonLayout<>", target="Pulp::signal",
                    header_fingerprint="sha256:0a3a14899d57ff5040b47823bb98c36ed055bf98b6946d2a8892c44a95a686a5"),
            binding(role="supersaw", kind="cpp_type", include="pulp/signal/supersaw.hpp",
                    qualified_name="pulp::signal::SupersawT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:c790c2b734d6a86f02ffedd560a0b14ba7529dbb5ce50b4e93a29b414217f338"),
        ],
        _link_probes=[
            {"role": "layout", "binding": "pulp::signal::UnisonLayout<>",
             "operation": "member_call", "member": "configure",
             "arguments": "pulp::signal::UnisonSpec{}"},
            {"role": "supersaw", "binding": "pulp::signal::SupersawT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.true-peak-limiter", domain="signal",
        summary="Prepared intersample true-peak limiter with explicit lookahead and channel linking.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control; may allocate detector and delay storage",
                   "process": "audio", "reset": "audio", "release": "destruction off audio"},
        state_model="Prepared intersample detector, scheduling horizon, lookahead delay, and gain state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite interleaved audio, ceiling, lookahead, release, and channel-link policy",
        output_domain="true-peak-limited audio and gain-reduction telemetry",
        units=["samples", "frames", "decibels true peak", "milliseconds"],
        latency="detector latency plus scheduling horizon plus user lookahead",
        tail="exact latency_samples", scheduling="sample-synchronous lookahead",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/true_peak_limiter.hpp",
                         qualified_name="pulp::signal::TruePeakLimiterT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:027278e6f446a6eb769576cea18e0007648d7d5bcd25231023f1d09a844a84ca")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::TruePeakLimiterT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.dynamics-envelope-contract", domain="signal",
        summary="Canonical mono/stereo peak or RMS envelope followers and gain-reduction vocabulary.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control prepare and setters",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed attack-release detector state and canonical scalar gain-reduction value.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="signed audio samples and attack, release, detector, and stereo-link policy",
        output_domain="mono or stereo envelopes and non-negative attenuation magnitude",
        units=["samples", "milliseconds", "decibels", "normalized link"],
        latency="zero", tail="recursive envelope decay until reset", scheduling="sample-synchronous",
        bindings=[
            binding(role="mono", kind="cpp_type", include="pulp/signal/dynamics_contract.hpp",
                    qualified_name="pulp::signal::EnvelopeFollowerT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:968978abf54ee71ee7789cfc79fe21347b97708b9f068812908a5b855d35a406"),
            binding(role="stereo", kind="cpp_type", include="pulp/signal/dynamics_contract.hpp",
                    qualified_name="pulp::signal::StereoEnvelopeFollowerT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:968978abf54ee71ee7789cfc79fe21347b97708b9f068812908a5b855d35a406"),
        ],
        _link_probes=[
            {"role": "mono", "binding": "pulp::signal::EnvelopeFollowerT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "stereo", "binding": "pulp::signal::StereoEnvelopeFollowerT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.linkwitz-riley-crossover", domain="signal",
        summary="Fixed-capacity multi-band Linkwitz-Riley crossover with bounded realtime retuning.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control prepare and bounded sweep configuration",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed cascaded crossover and sweep banks with recursive filter state.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio, ordered crossover frequencies, band count, and transition samples",
        output_domain="phase-aligned crossover bands",
        units=["samples", "hertz", "bands", "frames"],
        latency="zero", tail="recursive IIR decay", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/linkwitz_riley.hpp",
                         qualified_name="pulp::signal::LinkwitzRileyCrossoverT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:38fca0b926e0b4cb492528f95a69af89eb7ec5a0e83ed8692eb195c0cd35fa64")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::LinkwitzRileyCrossoverT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.multi-channel-meter", domain="signal",
        summary="Fixed-storage peak, RMS, loudness, correlation, and clip metering for up to 16 channels.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control", "process": "audio",
                   "reset": "audio", "release": "none"},
        state_model="Fixed peak, RMS, K-weighted loudness, correlation, and clip accumulators.",
        seed_model="none",
        determinism={"repeatability": "tolerance_bounded", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite planar audio, channel count, roles, and sample rate",
        output_domain="lock-free multi-channel meter snapshot",
        units=["samples", "frames", "decibels full scale", "LUFS", "correlation"],
        latency="measurement-window readiness; no audio-path delay",
        tail="measurement history until reset", scheduling="block accumulation and snapshot",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/multi_channel_meter.hpp",
                         qualified_name="pulp::signal::MultiChannelMeterT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:a9437e126faff12dfc60d80e59381fe0f5f005b3638d445d06daf4b8cb2fd695")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::MultiChannelMeterT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
]

# Public headers can leave the frozen legacy bucket only through one of these
# explicit reviewed classifications or a capability binding above.
REVIEWED_HEADERS: list[dict[str, Any]] = [
    {
        "include": "pulp/signal/detail/audio_range.hpp",
        "fingerprint": "sha256:de70fcaa00f30b7a20c1f8632619635fa774e239f81ae7d6c09cf03ded64ba0b",
        "disposition": "capability_support",
        "capability_keys": ["signal.routing-primitives"],
        "rationale": (
            "Installed byte-range overlap predicates enforce the routing primitives' "
            "aliasing contracts; they are not an independent DSP operation."
        ),
    },
    {
        "include": "pulp/signal/ballistics_filter.hpp",
        "fingerprint": "sha256:58f673ad243d35a4df20b30d0bd68b76034ac04d57f028b9835df9ead58498c1",
        "disposition": "capability_support",
        "capability_keys": ["signal.dynamics-envelope-contract"],
        "rationale": (
            "Implementation base for the exact EnvelopeFollower contract while retaining "
            "the established legacy ballistics timing convention."
        ),
    },
    *[
        {
            "include": include,
            "fingerprint": fingerprint,
            "disposition": "capability_support",
            "capability_keys": ["signal.dynamics-envelope-contract"],
            "rationale": (
                "Existing dynamics processor adopting the shared exact envelope or canonical "
                "non-negative gain-reduction telemetry contract; its topology is not a new "
                "capability claim in this slice."
            ),
        }
        for include, fingerprint in [
            ("pulp/signal/compressor.hpp", "sha256:9bf4c81430a11eedaae9e69b19dd8c5ce9fd5c50191e111fe86c3cada3e73fb3"),
            ("pulp/signal/diode_bridge_compressor.hpp", "sha256:2384a18231c9cfcff6b8823613050334776113aac5e6c7de76f6f3cd44e942a3"),
            ("pulp/signal/feedforward_compressor.hpp", "sha256:afcd1e63df356725f2256c0b449900c894e1120f016ac511512cc94b11263524"),
            ("pulp/signal/fet_compressor.hpp", "sha256:45e9524c1ca8d7a5e5a3c7e18c997710e0d5a13b05a5c3c2717f7640c5dd03ee"),
            ("pulp/signal/noise_gate.hpp", "sha256:da905cabc0fd988ab957fbd5d7f55c95119d1883a1384ef6be854d12f6d4f6ce"),
            ("pulp/signal/vca_compressor.hpp", "sha256:2f484f202dc2d75d2e87fc5683d8a6efa34f4143b120c761f5c16c330877077c"),
        ]
    ],
    *[
        {
            "include": include,
            "fingerprint": fingerprint,
            "disposition": "capability_support",
            "capability_keys": ["signal.dither-quantizer"],
            "rationale": (
                "Existing processor now consumes the deterministic dither and noise-shaping "
                "primitive while preserving its established default processing contract."
            ),
        }
        for include, fingerprint in [
            ("pulp/signal/character_delay/vintage.hpp", "sha256:b7a48feafacc26cd0329f97b7d898fbb8921dbb7e779498dc1eabd178ed2ab32"),
            ("pulp/signal/lofi_chain.hpp", "sha256:8c3ed2535478714195f46f13591c90b3b7c63affc6a6e8a7407ebedbf9f1799b"),
        ]
    ],
    *[
        {
            "include": include,
            "fingerprint": fingerprint,
            "disposition": "capability_support",
            "capability_keys": ["signal.fm-operator-engine"],
            "rationale": (
                "Existing drum voice composes the reusable fixed-capacity FM operator engine "
                "while retaining its voice-specific contract."
            ),
        }
        for include, fingerprint in [
            ("pulp/signal/drum/fm.hpp", "sha256:6858bf1217026bddf4a89db818ba528c8671261236304393c61626bf4fbe9219"),
            ("pulp/signal/drum/fm6.hpp", "sha256:1059887e7cfa8785366482b2171b0306d49f950adf35da2a88b64067249b57e6"),
        ]
    ],
    {
        "include": "pulp/signal/fft.hpp",
        "fingerprint": "sha256:534bc21f2c1c023d11871a7fe2ebd387f13194603117f00df7fcf20bc7c41c60",
        "disposition": "capability_support",
        "capability_keys": ["signal.source-filter-analysis", "signal.streaming-analysis-frontends"],
        "rationale": (
            "Shared prepared FFT and retained-storage accounting underpin the curated analysis "
            "capabilities; this slice adds no separate FFT authoring promise."
        ),
    },
    {
        "include": "pulp/signal/nonlin_ambience.hpp",
        "fingerprint": "sha256:aa58d912e15593e959118a912f5146774523a213446de5c8f19212d3eb9e7375",
        "disposition": "capability_support",
        "capability_keys": ["signal.dither-quantizer", "signal.stochastic-sources"],
        "rationale": (
            "Existing ambience processor adopts deterministic dither and velvet-noise "
            "primitives without a new ambience contract in this slice."
        ),
    },
    {
        "include": "pulp/signal/nonlin_ambience_design.hpp",
        "fingerprint": "sha256:1b9e4b5a8178283964fcacac595ac5540576f35bdffb717b77e3aaf92bf98cd4",
        "disposition": "capability_support",
        "capability_keys": ["signal.stochastic-sources"],
        "rationale": (
            "Control-side velvet tap design consumes deterministic coordinate draws; it is "
            "design support rather than a runtime entrypoint."
        ),
    },
    {
        "include": "pulp/signal/oscillator.hpp",
        "fingerprint": "sha256:eeec21c0a6b5e6cdbf9f81af6ad99fd873029ca6b0f154daadf3c72732ba4509",
        "disposition": "capability_support",
        "capability_keys": ["signal.unison-voice-primitives"],
        "rationale": (
            "Supersaw uses validated phase reset for deterministic per-voice phases; the generic "
            "oscillator remains outside this slice's promise."
        ),
    },
    *[
        {
            "include": include,
            "fingerprint": fingerprint,
            "disposition": "capability_support",
            "capability_keys": ["signal.source-filter-analysis"],
            "rationale": (
                "Existing pitch-time or formant-warping surface composes the transactional "
                "cepstral analyzer and retained-storage admission contract."
            ),
        }
        for include, fingerprint in [
            ("pulp/signal/realtime_pitch_time_geometry.hpp", "sha256:ffdb4ca7daa0ba17d53f78cf42eab0759c084cb2df4f0aa10bcf6be3e355fab0"),
            ("pulp/signal/realtime_pitch_time_processor.hpp", "sha256:519987d3e9742d2be9fcab3289808ed58b8f1fe5e245e92885c900726ad15809"),
            ("pulp/signal/spectral_envelope_shifter.hpp", "sha256:7b5faef1b77c5bf406357f58c8988c5fc4d2e769ec5882d17f053f672f24fe42"),
        ]
    ],
    {
        "include": "pulp/signal/rungler.hpp",
        "fingerprint": "sha256:c45a151c41449b0a7823ff148f1ed8cc0174f9c369a9bb95c07213ef97b19b1b",
        "disposition": "capability_support",
        "capability_keys": ["signal.stochastic-sources"],
        "rationale": (
            "The established Rungler now composes the configurable LFSR while preserving its "
            "existing DAC and event behavior."
        ),
    },
    {
        "include": "pulp/midi/block_ops.hpp",
        "fingerprint": "sha256:563569c0bb61029b1b374a8d91812a77818cb9f3db18cc2706a4c570860dd8a0",
        "disposition": "capability_support",
        "capability_keys": [
            "midi.arpeggiator", "midi.channel-routing", "midi.controller-mapping",
            "midi.keyboard-split", "midi.monophonic-note-selection",
            "midi.note-length-shaping", "midi.note-range-filtering",
            "midi.scale-aware-mpe-pitch",
        ],
        "rationale": (
            "Shared clear, drop, copy, and sidecar-accounting operations support the "
            "bounded MIDI utility block contracts without defining a semantic transform."
        ),
    },
    {
        "include": "pulp/midi/midi.hpp",
        "fingerprint": "sha256:874271b4160eac446ab91943b192f7652c4e340afbb373b70c3affc8ee6a49e8",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": "Convenience umbrella include only; operation-owning headers are bound directly.",
    },
    {
        "include": "pulp/midi/mpe_buffer.hpp",
        "fingerprint": "sha256:024033345347c33b06bc1df4f05240496933797410640ea7eb38bfe810ff8eaa",
        "disposition": "capability_support",
        "capability_keys": ["midi.mpe-voice-tracker"],
        "rationale": (
            "Transactional prepared event storage and tracker binding support MPE voice "
            "ownership but are not a distinct musical transform."
        ),
    },
    {
        "include": "pulp/midi/mpe_synth_voice.hpp",
        "fingerprint": "sha256:2426bb49591fb4510aaab4a70931899c4928793f34b23935f14b6af19c918db4",
        "disposition": "capability_support",
        "capability_keys": ["midi.mpe-voice-tracker"],
        "rationale": (
            "The allocator adapter is parameterized by a consumer-defined abstract Voice; "
            "the concrete tracker remains the honest typed entrypoint."
        ),
    },
    {
        "include": "pulp/midi/ump_buffer.hpp",
        "fingerprint": "sha256:bc5816405c9ef8a2fdf9336d86212319dd70a83a9633c7d515b6c3010c5019e6",
        "disposition": "capability_support",
        "capability_keys": [
            "midi.arpeggiator", "midi.channel-routing", "midi.controller-mapping",
            "midi.keyboard-split", "midi.monophonic-note-selection",
            "midi.mpe-voice-tracker", "midi.note-length-shaping",
            "midi.note-range-filtering", "midi.scale-aware-mpe-pitch",
        ],
        "rationale": (
            "Prepared UMP sidecar storage and overflow accounting support the complete-block "
            "contracts; semantic routing remains in the bound utility kernels."
        ),
    },
    {
        "include": "pulp/midi/utility_contract.hpp",
        "fingerprint": "sha256:3ecd5aa5c92a2ac31e133059c97915257df2018b904f9fce39d1b0c6f1dadb70",
        "disposition": "capability_support",
        "capability_keys": [
            "midi.arpeggiator", "midi.channel-routing", "midi.controller-mapping",
            "midi.keyboard-split", "midi.monophonic-note-selection",
            "midi.note-length-shaping", "midi.note-range-filtering",
            "midi.scale-aware-mpe-pitch",
        ],
        "rationale": (
            "Shared overflow, ordering, transport, reporting, and fail-closed emission "
            "vocabulary supports every MIDI utility kernel."
        ),
    },
    {
        "include": "pulp/midi/utility_kernels.hpp",
        "fingerprint": "sha256:d768529f97202108cc87903ac71ccfc727adeade6ecd50c66db8c3e394d98865",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": "Convenience umbrella include only; each utility family is bound directly.",
    },
    {
        "include": "pulp/music/detail/random_range.hpp",
        "fingerprint": "sha256:dd46f6f6f589aafdb08cc00c896cd87fc5465d5e1bd88c8266cf7e45808e5faf",
        "disposition": "capability_support",
        "capability_keys": ["music.markov-transition", "music.pattern-generation"],
        "rationale": (
            "Portable bit-exact bounded reduction supports caller-random Markov and pattern "
            "draws but is an implementation detail rather than a musical operation."
        ),
    },
    {
        "include": "pulp/signal/units.hpp",
        "fingerprint": "sha256:2f0af86ba3fccbb3017339235c05b7d43b492939c111fc67e93ee2046ee6e264",
        "disposition": "capability_support",
        "capability_keys": ["timebase.beat-division"],
        "rationale": (
            "The signal Division compatibility vocabulary now derives from the "
            "canonical timebase BeatDivision table. The broad signal unit-conversion "
            "header remains outside this slice's generator-facing claims."
        ),
    },
    {
        "include": "pulp/music/chord.hpp",
        "fingerprint": "sha256:2715a6ef063ec9815edd4955a23906bc38182b7c5e99db0a75073f14f9ce8323",
        "disposition": "capability_support",
        "capability_keys": [
            "music.chord-recognition",
            "music.chord-spelling",
            "music.chord-voicing",
        ],
        "rationale": (
            "Shared fixed-capacity chord values and named-quality formulas support "
            "the curated spelling, recognition, and voicing operations."
        ),
    },
    {
        "include": "pulp/music/music.hpp",
        "fingerprint": "sha256:5db5616ed1cd0cf326f31638f7c351d8d982d7c8b51d99f516fe3ac3edf5929b",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Convenience umbrella include only; consumers use the operation-owning "
            "headers named by each typed capability binding."
        ),
    },
    {
        "include": "pulp/music/pitch.hpp",
        "fingerprint": "sha256:2b26eed1a1e6ecfe6357ebbcf95c7ac0d94cd73d6416723144015125cda3e660",
        "disposition": "capability_support",
        "capability_keys": [
            "music.chord-recognition",
            "music.chord-spelling",
            "music.chord-voicing",
        ],
        "rationale": (
            "Checked 12-TET pitch-class values and sets are shared inputs to the "
            "curated spelling, recognition, and voicing operations."
        ),
    },
    {
        "include": "pulp/signal/interpolator.hpp",
        "fingerprint": "sha256:87600671e64ed34870e2302ca2765b3539d3ec23116db02b85ef813a43916952",
        "disposition": "capability_support",
        "capability_keys": ["signal.window-functions"],
        "rationale": (
            "The interpolator reuses the window implementation internally but "
            "does not add a distinct generator-facing window contract."
        ),
    },
    {
        "include": "pulp/signal/resampler.hpp",
        "fingerprint": "sha256:1bd78bf7111b9bfe5b1fd923ba0304390956ddf92337ed788b06a0b865a196e8",
        "disposition": "capability_support",
        "capability_keys": ["signal.bounded-sample-history"],
        "rationale": (
            "The resampler adopts the bounded history primitive for its delay "
            "storage; this slice does not introduce a new resampler contract."
        ),
    },
    {
        "include": "pulp/signal/stft.hpp",
        "fingerprint": "sha256:a326b986439d9382932a05682db29f862c0fb371a27acf701eea41d3ec873a32",
        "disposition": "capability_support",
        "capability_keys": [
            "signal.bounded-sample-history",
            "signal.window-functions",
        ],
        "rationale": (
            "STFT consumes both reusable primitives while retaining its existing "
            "analysis API; it is not a newly claimed capability in this slice."
        ),
    },
    {
        "include": "pulp/signal/fast_math.hpp",
        "fingerprint": "sha256:040569eb66723784d089120ecb679b78df4f206678a77a2f2881e0a874456de4",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "FastMath is a shared scalar implementation utility rather than a "
            "standalone semantic DSP unit. The corrected exp2 operation has no "
            "product consumer outside its own documentation and tests, so exposing "
            "it as a generator capability would overstate demonstrated adoption."
        ),
    },
    {
        "include": "pulp/signal/osc/detail/minblep_table.hpp",
        "fingerprint": "sha256:9354ed2187ec030386544ccffe90b85f1ed35f539a861983ee994c04dde05b31",
        "disposition": "capability_support",
        "capability_keys": ["signal.minblep"],
        "rationale": (
            "Generated residual coefficients are an installed implementation "
            "dependency of signal.minblep, not an independent authoring surface."
        ),
    },
    {
        "include": "pulp/signal/biquad.hpp",
        "fingerprint": "sha256:938e359bcd792fb4b8d4205d94ec5ab2db6684ec99ea1f927a417539cb09c55b",
        "disposition": "capability_support",
        "capability_keys": ["signal.sos-cascade"],
        "rationale": (
            "The bounded SOS executor consumes the normalized biquad coefficient "
            "type and uses its shared stability predicate and runtime section."
        ),
    },
    {
        "include": "pulp/signal/iir_design.hpp",
        "fingerprint": "sha256:d45c17451d8410069e3cffd4f911f2a532d525383ff033775d0c38b7c7decb10",
        "disposition": "capability_support",
        "capability_keys": ["signal.sos-cascade"],
        "rationale": (
            "The public high-order IIR design helpers produce normalized SOS "
            "coefficient vectors accepted by the bounded cascade executor."
        ),
    },
    {
        "include": "pulp/signal/signal.hpp",
        "fingerprint": "sha256:4f065844dfc4347b27fa302b922af2a99d74dae295be6958c11f51aa3394381a",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "This is the signal module umbrella include; it exposes no distinct "
            "consumer capability beyond the headers it aggregates."
        ),
    },
    {
        "include": "pulp/signal/harmony_engine.hpp",
        "fingerprint": "sha256:edf2c597ddeb7b31f07c7cf094e4cfb2b833b4f4fb6b00376b36635d11734f79",
        "disposition": "unsupported_capability",
        "capability_keys": [],
        "rationale": (
            "The harmonizer is a public DSP API, but it does not yet have the "
            "typed bindings, lifecycle contract, parameter semantics, and link "
            "probe required for a generator-facing capability claim."
        ),
    },
]
SURFACE_TOMBSTONES: list[dict[str, Any]] = []
CAPABILITY_TOMBSTONES: list[dict[str, Any]] = []


def repo_root() -> pathlib.Path:
    path = pathlib.Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "core").is_dir():
            return parent
    raise SystemExit("agent-capabilities: could not locate the repository root")


def strip_private(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: strip_private(item)
            for key, item in value.items()
            if not key.startswith("_")
        }
    if isinstance(value, list):
        return [strip_private(item) for item in value]
    return copy.deepcopy(value)


def public_rows() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for source in EXPORTS:
        row = strip_private(source)
        row["contract_digest"] = surface.canonical_digest(contract_payload(row))
        rows.append(row)
    return rows


def binding_claims() -> list[dict[str, Any]]:
    claims: list[dict[str, Any]] = []
    for row in EXPORTS:
        for item in row["bindings"]:
            claims.append(
                {
                    "include": item["include"],
                    "fingerprint": item["_header_fingerprint"],
                    "capability_key": row["key"],
                }
            )
    return claims


def legacy_signal_projection(root: pathlib.Path) -> dict[str, Any]:
    path = root / "tools/dsp_vocabulary.py"
    spec = importlib.util.spec_from_file_location("pulp_dsp_vocabulary_source", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.scan_headers()


def build_surface(root: pathlib.Path) -> tuple[dict[str, Any], list[str]]:
    return surface.build_surface_document(
        root,
        binding_claims=binding_claims(),
        reviewed_headers=REVIEWED_HEADERS,
        tombstones=SURFACE_TOMBSTONES,
        baseline=surface.load_baseline(root),
        known_capability_keys={row["key"] for row in EXPORTS},
        inventory_version=SURFACE_INVENTORY_VERSION,
    )


def coverage_from_surface(
    surface_document: dict[str, Any], rows: list[dict[str, Any]]
) -> dict[str, Any]:
    domains: dict[str, dict[str, Any]] = {}
    for domain in sorted(DOMAINS):
        domains[domain] = {
            "state": "partial" if domain in ROOT_DOMAINS else "not_inventoried",
            "capabilities": sum(row["domain"] == domain for row in rows),
        }
    return {
        "state": "partial",
        "absence_semantics": "unknown",
        "domains": domains,
    }


def document(root: pathlib.Path | None = None) -> dict[str, Any]:
    root = root or repo_root()
    rows = sorted(public_rows(), key=lambda row: row["key"])
    surface_document, problems = build_surface(root)
    if problems:
        raise RuntimeError("; ".join(problems))
    counts = {
        domain: sum(row["domain"] == domain for row in rows)
        for domain in sorted(DOMAINS)
    }
    return {
        "$schema": "agent-capabilities.schema.json",
        "schema": SCHEMA,
        "schema_minor": SCHEMA_MINOR,
        "manifest_revision": MANIFEST_REVISION,
        "required_features": REQUIRED_FEATURES,
        "coverage": coverage_from_surface(surface_document, rows),
        "capabilities": rows,
        "tombstones": sorted(CAPABILITY_TOMBSTONES, key=lambda row: row["key"]),
        "counts": {"total": len(rows), "by_domain": counts},
        "compatibility": {
            "signal_vocabulary": {
                "schema": "pulp.signal-vocabulary.compat.v1",
                "source": (
                    "curated manifest regeneration scan of public signal headers"
                ),
                "entries": legacy_signal_projection(root),
            }
        },
    }


def compile_fixture() -> str:
    public = public_rows()
    includes = sorted(
        {item["include"] for row in public for item in row["bindings"]}
    )
    lines = [
        "// Generated by tools/scripts/agent_capability_manifest.py --write.",
        "// Every typed binding is mechanically referenced below.",
        "",
    ]
    lines.extend(f"#include <{header}>" for header in includes)
    lines.extend(["", "int main() {"])
    binding_index = 0
    source_by_key = {row["key"]: row for row in EXPORTS}
    for row in sorted(public, key=lambda item: item["key"]):
        lines.extend(["    {", f"        // {row['key']}"])
        for item in row["bindings"]:
            name = item["qualified_name"]
            if item["kind"] == "cpp_type":
                lines.append(f"        static_assert(sizeof({name}) > 0);")
            else:
                source_binding = next(
                    candidate
                    for candidate in source_by_key[row["key"]]["bindings"]
                    if candidate["role"] == item["role"]
                    and candidate["qualified_name"] == name
                )
                address = source_binding.get("_address_expression", f"&{name}")
                lines.append(
                    f"        auto *volatile binding_{binding_index} = {address};"
                )
                lines.append(f"        (void)binding_{binding_index};")
            binding_index += 1
        for probe in render_link_probes(source_by_key[row["key"]]):
            lines.append(f"        {probe}")
        lines.append("    }")
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


def _render_link_probe(probe: dict[str, Any], index: int) -> str:
    binding = probe["binding"]
    arguments = probe["arguments"]
    operation = probe["operation"]
    if operation == "construct":
        return (
            f"{binding} probe_value_{index}{{{arguments}}}; "
            f"(void)probe_value_{index};"
        )
    if operation == "member_call":
        return (
            f"{binding} probe_value_{index}{{}}; "
            f"(void)probe_value_{index}.{probe['member']}({arguments});"
        )
    if operation == "function_call":
        return f"(void){binding}({arguments});"
    raise ValueError(f"unsupported link probe operation: {operation!r}")


def render_link_probes(row: dict[str, Any]) -> list[str]:
    return [
        _render_link_probe(probe, index)
        for index, probe in enumerate(row["_link_probes"])
    ]


def render_link_probe(row: dict[str, Any]) -> str:
    """Render all operational probes for one installed consumer source."""
    return " ".join(render_link_probes(row))


def _link_probe_problems(row: dict[str, Any]) -> list[str]:
    key = row.get("key", "<unknown>")
    probes = row.get("_link_probes")
    if not isinstance(probes, list) or not probes:
        return [f"{key} requires operational installed-consumer probes"]
    binding_entries = [
        ((item["role"], item["qualified_name"]), item["kind"])
        for item in row.get("bindings", [])
        if isinstance(item, dict)
        and isinstance(item.get("role"), str)
        and isinstance(item.get("qualified_name"), str)
    ]
    bindings = dict(binding_entries)
    problems: list[str] = []
    if len(binding_entries) != len(bindings):
        problems.append(f"{key} advertised bindings repeat a role/name probe identity")
    probed_bindings: list[tuple[str, str]] = []
    for index, probe in enumerate(probes):
        where = f"{key} link probe[{index}]"
        if not isinstance(probe, dict):
            problems.append(f"{where} must be an object")
            continue
        operation = probe.get("operation")
        required = {"role", "binding", "operation", "arguments"}
        if operation == "member_call":
            required.add("member")
        if set(probe) != required:
            problems.append(f"{where} fields are not exact for {operation!r}")
            continue
        binding = (probe.get("role"), probe.get("binding"))
        if binding not in bindings:
            problems.append(f"{where} must name an advertised binding")
            continue
        probed_bindings.append(binding)
        if not isinstance(probe.get("arguments"), str):
            problems.append(f"{where} arguments must be a C++ argument string")
        if operation in {"construct", "member_call"} and bindings[binding] != "cpp_type":
            problems.append(f"{where} {operation} requires a cpp_type binding")
        if operation == "function_call" and bindings[binding] != "cpp_function":
            problems.append(f"{where} function_call requires a cpp_function binding")
        if operation == "member_call" and not re.fullmatch(
            r"[A-Za-z_][A-Za-z0-9_]*", probe.get("member", "")
        ):
            problems.append(f"{where} member_call has an invalid member")
        if operation not in {"construct", "member_call", "function_call"}:
            problems.append(f"{where} operation is invalid")
        try:
            _render_link_probe(probe, index)
        except (KeyError, TypeError, ValueError) as error:
            problems.append(f"{where} could not render: {error}")
    if len(probed_bindings) != len(set(probed_bindings)):
        problems.append(f"{key} operational probes repeat an advertised binding")
    missing = sorted(set(bindings) - set(probed_bindings))
    if missing:
        problems.append(
            f"{key} advertised bindings lack operational probes: "
            + ", ".join(f"{role}:{name}" for role, name in missing)
        )
    extra = sorted(set(probed_bindings) - set(bindings))
    if extra:
        problems.append(
            f"{key} operational probes name unknown bindings: "
            + ", ".join(f"{role}:{name}" for role, name in extra)
        )
    try:
        render_link_probes(row)
    except (KeyError, TypeError, ValueError) as error:
        problems.append(f"{key} operational probes could not render: {error}")
    return problems


def history_entry(
    manifest_document: dict[str, Any], surface_document: dict[str, Any]
) -> dict[str, Any]:
    material = {
        "manifest": {
            "schema": manifest_document["schema"],
            "schema_minor": manifest_document["schema_minor"],
            "manifest_revision": manifest_document["manifest_revision"],
            "required_features": copy.deepcopy(
                manifest_document["required_features"]
            ),
            "coverage": copy.deepcopy(manifest_document["coverage"]),
            "capabilities": copy.deepcopy(manifest_document["capabilities"]),
            "tombstones": copy.deepcopy(manifest_document["tombstones"]),
            "counts": copy.deepcopy(manifest_document["counts"]),
            "compatibility": copy.deepcopy(manifest_document["compatibility"]),
        },
        "surface": {
            "schema": surface_document["schema"],
            "inventory_version": surface_document["inventory_version"],
            "headers": [
                {
                    "include": row["include"],
                    "fingerprint": row["fingerprint"],
                    "disposition": row["disposition"],
                }
                for row in surface_document["headers"]
            ],
            "tombstones": copy.deepcopy(surface_document["tombstones"]),
        },
    }
    return {
        **material,
        "entry_digest": surface.canonical_digest(material),
    }


def history_document(entries: list[dict[str, Any]]) -> dict[str, Any]:
    return {"schema": HISTORY_SCHEMA, "entries": copy.deepcopy(entries)}


def history_problems(
    history: Any,
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
) -> list[str]:
    if not isinstance(history, dict):
        return ["capability history must be an object"]
    if set(history) != {"schema", "entries"}:
        return ["capability history fields must be exactly schema and entries"]
    if history.get("schema") != HISTORY_SCHEMA:
        return [f"capability history schema must be {HISTORY_SCHEMA}"]
    entries = history.get("entries")
    if not isinstance(entries, list) or not entries:
        return ["capability history must contain at least one entry"]
    problems: list[str] = []
    previous: dict[str, Any] | None = None
    for index, entry in enumerate(entries):
        where = f"capability history entries[{index}]"
        if not isinstance(entry, dict) or set(entry) != {
            "entry_digest",
            "manifest",
            "surface",
        }:
            problems.append(f"{where} fields are not exact")
            continue
        material = {"manifest": entry["manifest"], "surface": entry["surface"]}
        if entry.get("entry_digest") != surface.canonical_digest(material):
            problems.append(f"{where} digest does not match its material")
        if previous is not None:
            problems.extend(
                evolution_problems(
                    previous["manifest"],
                    entry["manifest"],
                    allow_unpublished_migration=False,
                )
            )
            problems.extend(
                surface_evolution_problems(
                    previous["surface"], entry["surface"], surface.SURFACE_SCHEMA
                )
            )
        previous = entry
    current_entry = history_entry(current_manifest, current_surface)
    if previous is not None:
        problems.extend(
            evolution_problems(
                previous["manifest"],
                current_entry["manifest"],
                allow_unpublished_migration=False,
            )
        )
        problems.extend(
            surface_evolution_problems(
                previous["surface"], current_entry["surface"], surface.SURFACE_SCHEMA
            )
        )
    return _deduplicate(problems)


def protected_base_problems(
    root: pathlib.Path,
    history: dict[str, Any],
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
) -> list[str]:
    """Compare against protected-tip artifacts, which this checkout cannot edit."""
    base_ref = os.environ.get("PULP_AGENT_CAPABILITY_BASE_REF", "origin/main")
    if _git_output(root, ["rev-parse", "--is-inside-work-tree"]) != "true":
        # Source archives have no independently addressable protected history.
        # Their self-contained history is still checked above; PR/CI checkouts
        # must resolve or fetch the immutable protected tip below.
        return []
    protected_tip = _resolve_protected_tip(root, base_ref)
    if protected_tip is None:
        return [
            f"could not resolve protected capability history base {base_ref!r}; "
            "set PULP_AGENT_CAPABILITY_BASE_REF to the CI base ref"
        ]
    tip_manifest = _git_json(root, protected_tip, SNAPSHOT)
    old_manifest = tip_manifest
    old_surface = _git_json(root, protected_tip, surface.SURFACE_SNAPSHOT)
    old_history = _git_json(root, protected_tip, HISTORY_FILE)
    if old_manifest is None and old_surface is None and old_history is None:
        if len(history.get("entries", [])) != 1:
            return ["initial capability history bootstrap must contain exactly one entry"]
        return []
    problems: list[str] = []
    if old_manifest is None or old_surface is None or old_history is None:
        return ["protected base has an incomplete capability history contract"]
    problems.extend(append_only_history_problems(old_history, history))
    problems.extend(
        evolution_problems(
            old_manifest,
            current_manifest,
            allow_unpublished_migration=False,
        )
    )
    problems.extend(
        surface_evolution_problems(
            old_surface, current_surface, surface.SURFACE_SCHEMA
        )
    )
    return _deduplicate(problems)


def append_only_history_problems(previous: Any, current: Any) -> list[str]:
    old_entries = previous.get("entries") if isinstance(previous, dict) else None
    new_entries = current.get("entries") if isinstance(current, dict) else None
    if not isinstance(old_entries, list) or not isinstance(new_entries, list):
        return ["protected capability history entries are invalid"]
    if new_entries[: len(old_entries)] != old_entries:
        return ["capability history is not append-only relative to the protected base"]
    return []


def validate(doc: Any, root: pathlib.Path) -> list[str]:
    schema_path = root / MANIFEST_SCHEMA_FILE
    if not schema_path.is_file():
        return [f"manifest schema is missing: {schema_path}"]
    schema_document = json.loads(schema_path.read_text())
    problems = json_schema_lite.validate(doc, schema_document)
    if not isinstance(doc, dict):
        return problems
    if doc.get("schema") != SCHEMA:
        problems.append(f"schema must be exactly {SCHEMA}")
    if doc.get("schema_minor") != SCHEMA_MINOR:
        problems.append(f"schema_minor must be exactly {SCHEMA_MINOR}")
    revision = doc.get("manifest_revision")
    if isinstance(revision, bool) or not isinstance(revision, int) or revision < 1:
        problems.append("manifest_revision must be a positive integer")
    if doc.get("required_features") != REQUIRED_FEATURES:
        problems.append("required_features must be the sorted supported feature set")

    rows = doc.get("capabilities")
    if not isinstance(rows, list):
        return problems
    required_features = doc.get("required_features")
    if (
        isinstance(required_features, list)
        and "determinism-contract-v1" in required_features
    ):
        for index, row in enumerate(rows):
            if isinstance(row, dict) and "determinism" not in row:
                problems.append(
                    f"capabilities[{index}] requires determinism because the manifest "
                    "requires determinism-contract-v1"
                )
    expected_rows = {row["key"]: row for row in public_rows()}
    for source_row in EXPORTS:
        problems.extend(_link_probe_problems(source_row))
    expected_bindings = {
        key: {_binding_contract(item) for item in row["bindings"]}
        for key, row in expected_rows.items()
    }
    forge_path = root / "docs/status/forge-catalog.json"
    forge_keys: set[str] = set()
    if forge_path.exists():
        forge_keys = {
            row.get("key")
            for row in json.loads(forge_path.read_text()).get("nodes", [])
        }

    seen: set[str] = set()
    for index, row in enumerate(rows):
        where = f"capabilities[{index}]"
        if not isinstance(row, dict):
            continue
        key = row.get("key")
        if isinstance(key, str):
            if key in seen:
                problems.append(f"duplicate capability key: {key}")
            seen.add(key)
        if row.get("domain") not in DOMAINS:
            problems.append(f"{where} has invalid domain {row.get('domain')!r}")
        if row.get("rt_class") not in RT_CLASSES:
            problems.append(f"{where} has invalid rt_class {row.get('rt_class')!r}")
        if row.get("status") not in STATUSES:
            problems.append(f"{where} has invalid status {row.get('status')!r}")
        if row.get("status") == "planned":
            problems.append(f"{where} may not advertise planned work")
        version = row.get("contract_version")
        if not _valid_version(version, minimum_major=1):
            problems.append(
                f"{where}.contract_version must have major >= 1 and minor >= 0"
            )
        expected_digest = surface.canonical_digest(contract_payload(row))
        if row.get("contract_digest") != expected_digest:
            problems.append(f"{where}.contract_digest does not match its contract")

        evolution = row.get("evolution")
        if isinstance(evolution, dict):
            state = evolution.get("state")
            if state not in EVOLUTION_STATES:
                problems.append(f"{where}.evolution.state is invalid")
            introduced = evolution.get("introduced_in")
            if not _valid_version(introduced):
                problems.append(f"{where}.evolution.introduced_in is invalid")
            elif _valid_version(version, minimum_major=1) and _version_tuple(
                introduced
            ) > _version_tuple(version):
                problems.append(
                    f"{where}.evolution.introduced_in exceeds contract_version"
                )
            if state == "active":
                if set(evolution) != {"state", "introduced_in"}:
                    problems.append(f"{where}.evolution active fields are not exact")
                if row.get("status") == "deprecated":
                    problems.append(f"{where} active capability may not be deprecated")
            if state == "deprecated":
                if set(evolution) != {
                    "state",
                    "introduced_in",
                    "deprecated_in",
                    "replacement_key",
                }:
                    problems.append(f"{where}.evolution deprecated fields are not exact")
                deprecated = evolution.get("deprecated_in")
                if not _valid_version(deprecated):
                    problems.append(f"{where}.evolution.deprecated_in is invalid")
                elif _valid_version(introduced) and _valid_version(
                    version, minimum_major=1
                ) and not (
                    _version_tuple(introduced)
                    <= _version_tuple(deprecated)
                    <= _version_tuple(version)
                ):
                    problems.append(
                        f"{where}.evolution versions must satisfy introduced <= "
                        "deprecated <= contract"
                    )
                if row.get("status") != "deprecated":
                    problems.append(
                        f"{where} deprecated evolution requires deprecated status"
                    )
                replacement = evolution.get("replacement_key")
                if replacement is not None and not isinstance(replacement, str):
                    problems.append(f"{where}.evolution.replacement_key is invalid")

        binding_identities: set[tuple[Any, ...]] = set()
        binding_contracts: set[tuple[Any, ...]] = set()
        for binding_index, item in enumerate(row.get("bindings", [])):
            if not isinstance(item, dict):
                continue
            binding_where = f"{where}.bindings[{binding_index}]"
            identity = tuple(
                _identity_value(item.get(field))
                for field in (
                    "role",
                    "kind",
                    "include",
                    "qualified_name",
                    "target",
                )
            )
            if identity in binding_identities:
                problems.append(f"{binding_where} duplicates another typed binding")
            binding_identities.add(identity)
            contract = _binding_contract(item)
            binding_contracts.add(contract)
            if (
                not isinstance(key, str)
                or key not in expected_bindings
                or contract not in expected_bindings[key]
            ):
                problems.append(
                    f"{binding_where} advertises a binding outside curated exports"
                )
            include = item.get("include")
            if isinstance(include, str) and not list(
                root.glob(f"core/*/include/{include}")
            ):
                problems.append(f"{binding_where} advertises missing include {include}")
            if isinstance(include, str):
                expected_target = _minimal_target_for_include(include)
                if expected_target is None:
                    problems.append(
                        f"{binding_where} include has no covered public target owner"
                    )
                elif item.get("target") != expected_target:
                    problems.append(
                        f"{binding_where}.target must be minimal owning target "
                        f"{expected_target}"
                    )
            if item.get("kind") not in BINDING_KINDS:
                problems.append(f"{binding_where}.kind is invalid")
        if (
            isinstance(key, str)
            and key in expected_bindings
            and binding_contracts != expected_bindings[key]
        ):
            problems.append(f"{where}.bindings must exactly match curated exports")

        descriptor = row.get("forge_descriptor")
        if descriptor is not None:
            if (
                not isinstance(descriptor, dict)
                or set(descriptor) != {"catalog", "node_key"}
                or descriptor.get("catalog") != "forge-catalog.json"
                or not isinstance(descriptor.get("node_key"), str)
                or not descriptor.get("node_key")
            ):
                problems.append(f"{where} has invalid forge_descriptor reference")
            elif descriptor.get("node_key") not in forge_keys:
                problems.append(
                    f"{where} references missing Forge descriptor "
                    f"{descriptor.get('node_key')!r}"
                )

        for path, field in _object_fields(row):
            if field in FORBIDDEN_NUMERIC_CONTRACT_FIELDS:
                problems.append(
                    f"{where}{path} duplicates Forge numeric contract field {field!r}"
                )
            if field in FORBIDDEN_RUNTIME_CONTROL_FIELDS:
                problems.append(
                    f"{where}{path} contains runtime control field {field!r}; "
                    "design capabilities and runtime control are separate contracts"
                )

    if [row.get("key") for row in rows if isinstance(row, dict)] != sorted(seen):
        problems.append("capabilities must be sorted by stable key")
    if seen != set(expected_rows):
        problems.append("capability keys must exactly match curated exports")

    replacement_edges: dict[str, str] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        replacement = row.get("evolution", {}).get("replacement_key")
        key = row.get("key")
        if not isinstance(replacement, str) or not isinstance(key, str):
            continue
        if replacement == key:
            problems.append(f"{key} replacement_key may not reference itself")
        elif replacement not in seen:
            problems.append(f"{key} replacement_key does not name a live capability")
        else:
            replacement_edges[key] = replacement

    tombstones = doc.get("tombstones")
    tombstone_keys: set[str] = set()
    if isinstance(tombstones, list):
        for index, item in enumerate(tombstones):
            if not isinstance(item, dict):
                continue
            key = item.get("key")
            if isinstance(key, str):
                if key in seen:
                    problems.append(
                        f"capability tombstone overlaps a live key: {key}"
                    )
                if key in tombstone_keys:
                    problems.append(f"duplicate capability tombstone: {key}")
                tombstone_keys.add(key)
            if not _valid_digest(item.get("last_contract_digest")):
                problems.append(
                    f"tombstones[{index}].last_contract_digest is invalid"
                )
            removed_revision = item.get("removed_in_manifest_revision")
            if (
                isinstance(removed_revision, bool)
                or not isinstance(removed_revision, int)
                or removed_revision < 1
                or (isinstance(revision, int) and removed_revision > revision)
            ):
                problems.append(
                    f"tombstones[{index}].removed_in_manifest_revision is invalid"
                )
            if item.get("status") != "removed":
                problems.append(f"tombstones[{index}].status must be removed")
            introduced = item.get("introduced_in")
            deprecated = item.get("deprecated_in")
            last_version = item.get("last_contract_version")
            if not all(
                _valid_version(value)
                for value in (introduced, deprecated, last_version)
            ):
                problems.append(f"tombstones[{index}] lifecycle versions are invalid")
            elif not (
                _version_tuple(introduced)
                <= _version_tuple(deprecated)
                <= _version_tuple(last_version)
            ):
                problems.append(
                    f"tombstones[{index}] lifecycle versions must satisfy "
                    "introduced <= deprecated <= last_contract_version"
                )
            replacement = item.get("replacement_key")
            if replacement is not None:
                if replacement == key:
                    problems.append(f"{key} replacement_key may not reference itself")
                elif replacement not in seen:
                    problems.append(
                        f"{key} replacement_key does not name a live capability"
                    )
                elif isinstance(key, str):
                    replacement_edges[key] = replacement

    problems.extend(_replacement_cycle_problems(replacement_edges))

    actual_by_domain = {
        domain: sum(
            isinstance(row, dict) and row.get("domain") == domain for row in rows
        )
        for domain in sorted(DOMAINS)
    }
    counts = doc.get("counts")
    if not isinstance(counts, dict) or counts != {
        "total": len(rows),
        "by_domain": actual_by_domain,
    }:
        problems.append("counts must exactly match capabilities by domain")

    try:
        current_surface, surface_problems = build_surface(root)
        problems.extend(surface_problems)
        surface_schema_path = root / surface.SURFACE_SCHEMA_FILE
        if not surface_schema_path.is_file():
            problems.append(f"surface schema is missing: {surface_schema_path}")
        else:
            problems.extend(
                json_schema_lite.validate(
                    current_surface,
                    json.loads(surface_schema_path.read_text()),
                    "surface",
                )
            )
        expected_coverage = coverage_from_surface(current_surface, rows)
        if doc.get("coverage") != expected_coverage:
            problems.append("coverage must exactly match the reviewed public surface ledger")
    except (RuntimeError, json.JSONDecodeError) as error:
        problems.append(f"could not validate public surface coverage: {error}")

    compatibility = doc.get("compatibility")
    projection = (
        compatibility.get("signal_vocabulary")
        if isinstance(compatibility, dict)
        else None
    )
    if isinstance(projection, dict):
        if projection.get("entries") != legacy_signal_projection(root):
            problems.append(
                "compatibility signal vocabulary is stale against public signal headers"
            )
    return _deduplicate(problems)


def rendered(root: pathlib.Path | None = None) -> str:
    return json.dumps(document(root), indent=2, ensure_ascii=False) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Pulp installed agent capabilities")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--json", action="store_true", help="emit the consumer manifest")
    mode.add_argument("--check", action="store_true", help="validate all generated artifacts")
    mode.add_argument("--write", action="store_true", help="regenerate checked artifacts")
    mode.add_argument("--validate", metavar="PATH", help="validate a manifest fixture")
    mode.add_argument(
        "--bootstrap-surface",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--snapshot", type=pathlib.Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--migrate-unpublished-v1", action="store_true", help=argparse.SUPPRESS
    )
    args = parser.parse_args()
    root = repo_root()

    if args.bootstrap_surface:
        path = root / surface.LEGACY_BASELINE
        if path.exists():
            print(
                f"agent-capabilities: INVALID: frozen baseline already exists: {path}",
                file=sys.stderr,
            )
            return 1
        baseline = surface.baseline_document(
            root, {claim["include"] for claim in binding_claims()}
        )
        problems = surface.validate_baseline(baseline)
        if problems:
            return _print_problems(problems)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(surface.rendered(baseline))
        print(
            f"agent-capabilities: bootstrapped frozen baseline with "
            f"{baseline['frozen_count']} headers"
        )
        return 0

    if args.migrate_unpublished_v1 and not args.write:
        return _print_problems(["--migrate-unpublished-v1 requires --write"])

    if args.validate:
        try:
            fixture = json.loads(pathlib.Path(args.validate).read_text())
        except (OSError, json.JSONDecodeError) as error:
            return _print_problems([f"could not read fixture: {error}"])
        return _print_problems(validate(fixture, root), success_message=None)

    try:
        doc = document(root)
        surface_document, surface_problems = build_surface(root)
    except (RuntimeError, json.JSONDecodeError) as error:
        return _print_problems([str(error)])
    problems = validate(doc, root) + surface_problems
    if problems:
        return _print_problems(problems)

    output = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    surface_output = surface.rendered(surface_document)
    snapshot = args.snapshot or root / SNAPSHOT
    surface_snapshot = root / surface.SURFACE_SNAPSHOT
    history_path = root / HISTORY_FILE
    fixture = root / COMPILE_FIXTURE

    if args.json:
        sys.stdout.write(output)
        return 0
    if args.write:
        previous = _load_optional_json(snapshot)
        previous_surface = _load_optional_json(surface_snapshot)
        base_ref = os.environ.get("PULP_AGENT_CAPABILITY_BASE_REF", "origin/main")
        protected_tip = _resolve_protected_tip(root, base_ref)
        initial_bootstrap = bool(
            protected_tip and _git_json(root, protected_tip, SNAPSHOT) is None
        )
        problems: list[str] = []
        if not (args.migrate_unpublished_v1 and initial_bootstrap):
            problems.extend(
                evolution_problems(
                    previous,
                    doc,
                    allow_unpublished_migration=args.migrate_unpublished_v1,
                )
            )
            problems.extend(
                surface_evolution_problems(
                    previous_surface, surface_document, surface.SURFACE_SCHEMA
                )
            )
        if problems:
            return _print_problems(problems)
        history = _load_optional_json(history_path)
        entries = [] if args.migrate_unpublished_v1 else (
            copy.deepcopy(history.get("entries", []))
            if isinstance(history, dict) and history.get("schema") == HISTORY_SCHEMA
            else []
        )
        if (
            not args.migrate_unpublished_v1
            and isinstance(previous, dict)
            and isinstance(previous_surface, dict)
        ):
            previous_entry = history_entry(previous, previous_surface)
            if not entries or entries[-1] != previous_entry:
                entries.append(previous_entry)
        if not entries:
            entries.append(history_entry(doc, surface_document))
        history = history_document(entries)
        problems = history_problems(history, doc, surface_document)
        problems.extend(
            protected_base_problems(
                root, history, doc, surface_document
            )
        )
        if problems:
            return _print_problems(problems)
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(output)
        surface_snapshot.parent.mkdir(parents=True, exist_ok=True)
        surface_snapshot.write_text(surface_output)
        history_path.parent.mkdir(parents=True, exist_ok=True)
        history_path.write_text(json.dumps(history, indent=2, ensure_ascii=False) + "\n")
        fixture.write_text(compile_fixture())
        print(
            f"agent-capabilities: wrote {SNAPSHOT}, {surface.SURFACE_SNAPSHOT}, "
            f"{HISTORY_FILE}, and {COMPILE_FIXTURE} "
            f"({len(doc['capabilities'])} capabilities)"
        )
        return 0
    if args.check:
        stale: list[str] = []
        if not snapshot.exists() or snapshot.read_text() != output:
            stale.append(f"{snapshot} differs from curated exports")
        if not surface_snapshot.exists() or surface_snapshot.read_text() != surface_output:
            stale.append(f"{surface.SURFACE_SNAPSHOT} differs from public headers")
        if not fixture.exists() or fixture.read_text() != compile_fixture():
            stale.append(f"{COMPILE_FIXTURE} differs from typed bindings")
        history = _load_optional_json(history_path)
        history_issues = history_problems(history, doc, surface_document)
        if isinstance(history, dict):
            history_issues.extend(
                protected_base_problems(root, history, doc, surface_document)
            )
        stale.extend(history_issues)
        if stale:
            for item in stale:
                print(f"agent-capabilities: STALE: {item}", file=sys.stderr)
            return 1
        print(
            f"agent-capabilities: fresh; {len(doc['capabilities'])} keys and "
            f"{surface_document['counts']['public_headers']} public headers checked"
        )
        return 0

    print("Pulp agent capability manifest")
    print(f"  schema        {SCHEMA} minor {SCHEMA_MINOR}")
    print(f"  revision      {MANIFEST_REVISION}")
    print(f"  capabilities  {len(doc['capabilities'])}")
    print(f"  coverage      {doc['coverage']['state']} (absence means unknown)")
    for domain, count in doc["counts"]["by_domain"].items():
        if count:
            print(f"  {domain:<12} {count}")
    return 0


def _object_fields(value: Any, path: str = "") -> list[tuple[str, str]]:
    fields: list[tuple[str, str]] = []
    if isinstance(value, dict):
        for key, item in value.items():
            child = f"{path}.{key}"
            fields.append((child, key))
            fields.extend(_object_fields(item, child))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            fields.extend(_object_fields(item, f"{path}[{index}]"))
    return fields


def _valid_digest(value: Any) -> bool:
    return bool(
        isinstance(value, str)
        and re.fullmatch(r"sha256:[0-9a-f]{64}", value)
    )


def _valid_version(value: Any, *, minimum_major: int = 0) -> bool:
    return bool(
        isinstance(value, dict)
        and set(value) == {"major", "minor"}
        and isinstance(value.get("major"), int)
        and not isinstance(value.get("major"), bool)
        and value["major"] >= minimum_major
        and isinstance(value.get("minor"), int)
        and not isinstance(value.get("minor"), bool)
        and value["minor"] >= 0
    )


def _minimal_target_for_include(include: str) -> str | None:
    return REVIEWED_MINIMAL_TARGETS.get(include)


def _version_tuple(value: dict[str, Any]) -> tuple[int, int]:
    return value["major"], value["minor"]


def _replacement_cycle_problems(edges: dict[str, str]) -> list[str]:
    problems: list[str] = []
    for start in sorted(edges):
        path: list[str] = []
        seen: set[str] = set()
        current = start
        while current in edges:
            if current in seen:
                cycle = path[path.index(current) :] + [current]
                problems.append("replacement_key cycle: " + " -> ".join(cycle))
                break
            seen.add(current)
            path.append(current)
            current = edges[current]
    return _deduplicate(problems)


def _binding_contract(item: dict[str, Any]) -> tuple[Any, ...]:
    return (
        _identity_value(item.get("role")),
        _identity_value(item.get("kind")),
        _identity_value(item.get("include")),
        _identity_value(item.get("qualified_name")),
        _identity_value(item.get("target")),
        _identity_value(item.get("availability")),
    )


def _identity_value(value: Any) -> str:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    )


def _deduplicate(problems: list[str]) -> list[str]:
    return list(dict.fromkeys(problems))


def _load_optional_json(path: pathlib.Path) -> Any:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as error:
        return {"_invalid": str(error)}


def _git_output(root: pathlib.Path, arguments: list[str]) -> str | None:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=root, text=True, capture_output=True
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _git_json(root: pathlib.Path, revision: str, path: pathlib.Path) -> Any:
    output = _git_output(root, ["show", f"{revision}:{path.as_posix()}"])
    if output is None:
        return None
    try:
        return json.loads(output)
    except json.JSONDecodeError:
        return {"_invalid": True}


def _resolve_protected_tip(root: pathlib.Path, base_ref: str) -> str | None:
    explicit_ref = "PULP_AGENT_CAPABILITY_BASE_REF" in os.environ
    if explicit_ref:
        tip = _git_output(
            root, ["rev-parse", "--verify", f"{base_ref}^{{commit}}"]
        )
        if tip is not None:
            return tip
    candidate: str | None = None
    event_path = (
        os.environ.get("GITHUB_EVENT_PATH")
        if os.environ.get("GITHUB_ACTIONS") == "true"
        else None
    )
    if event_path is not None:
        try:
            event = json.loads(pathlib.Path(event_path).read_text())
            candidate = event.get("pull_request", {}).get("base", {}).get("sha")
            candidate = candidate or event.get("merge_group", {}).get("base_sha")
            before = event.get("before")
            if (
                candidate is None
                and isinstance(before, str)
                and before != "0" * 40
            ):
                candidate = before
        except (OSError, json.JSONDecodeError, AttributeError):
            candidate = None
    if isinstance(candidate, str) and re.fullmatch(r"[0-9a-fA-F]{40}", candidate):
        if _git_output(root, ["cat-file", "-e", f"{candidate}^{{commit}}"]) is None:
            try:
                fetched = subprocess.run(
                    ["git", "fetch", "--no-tags", "--depth=1", "origin", candidate],
                    cwd=root,
                    text=True,
                    capture_output=True,
                )
            except OSError:
                return None
            if fetched.returncode != 0:
                return None
        return candidate.lower()
    return _git_output(root, ["rev-parse", "--verify", f"{base_ref}^{{commit}}"])


def _print_problems(
    problems: list[str], success_message: str | None = "agent-capabilities: valid"
) -> int:
    for problem in _deduplicate(problems):
        print(f"agent-capabilities: INVALID: {problem}", file=sys.stderr)
    if problems:
        return 1
    if success_message:
        print(success_message)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
