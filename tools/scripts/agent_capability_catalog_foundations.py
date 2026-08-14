"""Declarative installed agent capability records."""
from agent_capability_registry_types import binding, capability

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
        key="audio.boundary-diagnostics",
        domain="audio",
        summary=(
            "Deterministic non-realtime diagnosis of processor, standalone-boundary, "
            "and device telemetry for reported missing or incorrect audio."
        ),
        rt_class="control",
        lifecycle={
            "construction": "none",
            "prepare": "none",
            "process": "control",
            "reset": "none",
            "release": "result destruction off audio",
        },
        state_model=(
            "Pure report construction from already-published probe snapshots, optional "
            "device counters, and an explicit silence threshold."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain=(
            "optional processor and standalone-boundary probe snapshots, optional device "
            "statistics, and a linear-amplitude silence threshold"
        ),
        output_domain="boundary diagnosis and owned stage-by-stage diagnostic text",
        units=["linear amplitude", "frames", "hertz", "event count"],
        latency="zero",
        tail="none",
        scheduling="caller-clocked after probe publication; never on the audio thread",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_function",
                include="pulp/audio/audio_boundary_report.hpp",
                qualified_name="pulp::audio::build_boundary_report",
                target="Pulp::audio",
                header_fingerprint=(
                    "sha256:45c6b48d154b28a714f220c5f27e5b7f201ea8b3540b6f1fdf2eca44de99307e"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::build_boundary_report",
            "operation": "function_call",
            "arguments": "pulp::audio::BoundaryReportInputs{}",
        }],
    ),
    capability(
        key="audio.realtime-output-probe",
        domain="audio",
        summary=(
            "Prepared realtime-safe output telemetry with optional bounded capture and "
            "non-realtime snapshot consumption."
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
            "prepare allocates scalar-summary publication and optional fixed-capacity "
            "capture storage; each audio callback updates only bounded counters and "
            "publishes the latest snapshot."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain=(
            "prepared channel and block limits, output audio blocks, and optional "
            "capture, silence, and clip-threshold settings"
        ),
        output_domain=(
            "latest output snapshot, release-safe statistics, and optional bounded "
            "captured output frames"
        ),
        units=["linear amplitude", "frames", "hertz", "event count"],
        latency="zero telemetry latency beyond the next published callback snapshot",
        tail="optional configured capture history until drained, overwritten, or reset",
        scheduling=(
            "analyze_output on one audio producer; latest, statistics, and capture reads "
            "on one non-realtime consumer"
        ),
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/audio/audio_probe.hpp",
                qualified_name="pulp::audio::AudioProbe",
                target="Pulp::audio",
                header_fingerprint=(
                    "sha256:6451a59ef45103ad6816f6278e9a987c3fcc9b1e0d4530ef55975eed530240ad"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::AudioProbe",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "2, 64, 48000.0",
        }],
    ),
    capability(
        key="midi.mpe-voice-tracker",
        contract_version={"major": 2, "minor": 0},
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
            "explicit monotonic note identities, fail-closed generation "
            "exhaustion, FIFO deferred note-offs, and transactional lifecycle "
            "rejection."
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
]
