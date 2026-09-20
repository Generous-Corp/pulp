"""Declarative registry for the installed agent capability contract."""
from __future__ import annotations

from typing import Any

from agent_capability_catalog_foundations import EXPORTS as FOUNDATION_EXPORTS
from agent_capability_catalog_performance import EXPORTS as PERFORMANCE_EXPORTS
from agent_capability_catalog_signal import EXPORTS as SIGNAL_EXPORTS
from agent_capability_catalog_timing import EXPORTS as TIMING_EXPORTS

REVIEWED_MINIMAL_TARGETS = {
    "pulp/host/signal_graph.hpp": "Pulp::host",
    "pulp/host/signal_graph_runtime.hpp": "Pulp::host",
    "pulp/host/signal_graph_prepared_topology_edit.hpp": "Pulp::host",
    "pulp/host/custom_node_type.hpp": "Pulp::host",
    "pulp/host/sample_region_authoring.hpp": "Pulp::host",
    "pulp/host/sample_region_proof.hpp": "Pulp::host",
    "pulp/host/sample_region_parameters.hpp": "Pulp::host",

    "pulp/format/processor.hpp": "Pulp::format",
    "pulp/format/processor_node_adapter.hpp": "Pulp::format",
    "pulp/audio/instrument_voice_allocator.hpp": "Pulp::audio",
    "pulp/audio/midi_voice_modulation_adapter.hpp": "Pulp::audio",
    "pulp/audio/onset_detector.hpp": "Pulp::audio",
    "pulp/audio/unison_voice_stack.hpp": "Pulp::audio",
    "pulp/audio/voice_modulation_sources.hpp": "Pulp::audio",
    "pulp/audio/voice_runtime_facade.hpp": "Pulp::audio",
    "pulp/audio/wavetable_authoring.hpp": "Pulp::audio",
    "pulp/midi/arpeggiator.hpp": "Pulp::midi",
    "pulp/midi/controller_utility_kernels.hpp": "Pulp::midi",
    "pulp/midi/mpe_voice_tracker.hpp": "Pulp::midi",
    "pulp/midi/note_utility_kernels.hpp": "Pulp::midi",
    "pulp/midi/routing_utility_kernels.hpp": "Pulp::midi",
    "pulp/midi/chord_memory.hpp": "Pulp::midi",
    "pulp/midi/humanize.hpp": "Pulp::midi",
    "pulp/midi/latch.hpp": "Pulp::midi",
    "pulp/midi/note_delay.hpp": "Pulp::midi",
    "pulp/midi/note_repeat.hpp": "Pulp::midi",
    "pulp/midi/step_player.hpp": "Pulp::midi",
    "pulp/midi/strum.hpp": "Pulp::midi",
    "pulp/music/chord.hpp": "Pulp::music",
    "pulp/music/harmony.hpp": "Pulp::music",
    "pulp/music/markov.hpp": "Pulp::music",
    "pulp/music/music.hpp": "Pulp::music",
    "pulp/music/pitch.hpp": "Pulp::music",
    "pulp/music/pattern.hpp": "Pulp::music",
    "pulp/music/pattern_development.hpp": "Pulp::music",
    "pulp/music/rhythm_relationship.hpp": "Pulp::music",
    "pulp/music/spelling.hpp": "Pulp::music",
    "pulp/music/voicing.hpp": "Pulp::music",
    "pulp/playback/program.hpp": "Pulp::playback",
    "pulp/sequence/host_transport_projector.hpp": "Pulp::sequence",
    "pulp/timeline/compile_context.hpp": "Pulp::timeline",
    "pulp/timeline/model.hpp": "Pulp::timeline",
    "pulp/timeline/note_modifier.hpp": "Pulp::timeline",
    "pulp/signal/saturator.hpp": "Pulp::signal",
    "pulp/signal/analysis_frontends.hpp": "Pulp::signal",
    "pulp/signal/additive_bank.hpp": "Pulp::signal",
    "pulp/signal/spectral_feature_frontends.hpp": "Pulp::signal",
    "pulp/signal/audio_matrix_mixer.hpp": "Pulp::signal",
    "pulp/signal/breakpoint_envelope.hpp": "Pulp::signal",
    "pulp/signal/beat_repeat_kernel.hpp": "Pulp::signal",
    "pulp/signal/commuted_string_excitation.hpp": "Pulp::signal",
    "pulp/signal/cross_feedback_multitap_delay.hpp": "Pulp::signal",
    "pulp/signal/de_esser.hpp": "Pulp::signal",
    "pulp/signal/dither.hpp": "Pulp::signal",
    "pulp/signal/dust.hpp": "Pulp::signal",
    "pulp/signal/dynamic_eq.hpp": "Pulp::signal",
    "pulp/signal/dynamics_contract.hpp": "Pulp::signal",
    "pulp/signal/explicit_q_resonator_bank.hpp": "Pulp::signal",
    "pulp/signal/expander.hpp": "Pulp::signal",
    "pulp/signal/auto_ducked_send.hpp": "Pulp::signal",
    "pulp/signal/early_reflections.hpp": "Pulp::signal",
    "pulp/signal/comb_filter.hpp": "Pulp::signal",
    "pulp/signal/filter_morph.hpp": "Pulp::signal",
    "pulp/signal/formant_filter_bank.hpp": "Pulp::signal",
    "pulp/signal/graphic_eq.hpp": "Pulp::signal",
    "pulp/signal/tilt_eq.hpp": "Pulp::signal",
    "pulp/signal/parallel_dynamics.hpp": "Pulp::signal",
    "pulp/signal/transfer_curve.hpp": "Pulp::signal",
    "pulp/signal/diffusion_network.hpp": "Pulp::signal",
    "pulp/signal/spectral_cross_synthesis.hpp": "Pulp::signal",
    "pulp/signal/fm_operator_engine.hpp": "Pulp::signal",
    "pulp/signal/fir_design.hpp": "Pulp::signal",
    "pulp/signal/fractional_delay.hpp": "Pulp::signal",
    "pulp/signal/headphone_crossfeed.hpp": "Pulp::signal",
    "pulp/signal/lfsr.hpp": "Pulp::signal",
    "pulp/signal/linkwitz_riley.hpp": "Pulp::signal",
    "pulp/signal/mid_side.hpp": "Pulp::signal",
    "pulp/signal/modulation_curve.hpp": "Pulp::signal",
    "pulp/signal/multi_channel_meter.hpp": "Pulp::signal",
    "pulp/signal/sub_oscillator.hpp": "Pulp::signal",
    "pulp/signal/blit_oscillator.hpp": "Pulp::signal",
    "pulp/signal/nlms_adaptive_filter.hpp": "Pulp::signal",
    "pulp/signal/noise_tilt.hpp": "Pulp::signal",
    "pulp/signal/nonlinear_shaping.hpp": "Pulp::signal",
    "pulp/signal/nway_crossfade.hpp": "Pulp::signal",
    "pulp/signal/path_latency_aligner.hpp": "Pulp::signal",
    "pulp/signal/path_switcher.hpp": "Pulp::signal",
    "pulp/signal/particle_collision_exciter.hpp": "Pulp::signal",
    "pulp/signal/particle_percussion_voice.hpp": "Pulp::signal",
    "pulp/signal/drum/tom.hpp": "Pulp::signal",
    "pulp/signal/rise_fall_generator.hpp": "Pulp::signal",
    "pulp/signal/reed_waveguide_loop.hpp": "Pulp::signal",
    "pulp/signal/scope_capture.hpp": "Pulp::signal",
    "pulp/signal/six_band_eq.hpp": "Pulp::signal",
    "pulp/signal/source_filter_analysis.hpp": "Pulp::signal",
    "pulp/signal/spectrum_trace.hpp": "Pulp::signal",
    "pulp/signal/spectral_delay_matrix.hpp": "Pulp::signal",
    "pulp/signal/spectral_band_mask.hpp": "Pulp::signal",
    "pulp/signal/spectral_mask_processor.hpp": "Pulp::signal",
    "pulp/signal/spectral_gate_blur.hpp": "Pulp::signal",
    "pulp/signal/spectral_morph.hpp": "Pulp::signal",
    "pulp/signal/sos_cascade.hpp": "Pulp::signal",
    "pulp/signal/supersaw.hpp": "Pulp::signal",
    "pulp/signal/true_peak_limiter.hpp": "Pulp::signal",
    "pulp/signal/transient_designer.hpp": "Pulp::signal",
    "pulp/signal/unit_delay.hpp": "Pulp::signal",
    "pulp/signal/unison.hpp": "Pulp::signal",
    "pulp/signal/velvet_noise.hpp": "Pulp::signal",
    "pulp/signal/wavetable.hpp": "Pulp::signal",
    "pulp/signal/waveguide_junction.hpp": "Pulp::signal",
    "pulp/signal/waveguide_line.hpp": "Pulp::signal",
    "pulp/signal/waveguide_reflection_filter.hpp": "Pulp::signal",
    "pulp/signal/waveguide_reed_exciter.hpp": "Pulp::signal",
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
    "pulp/timebase/compiled_tempo_map.hpp": "Pulp::timebase",
}

EXPORTS = [
    *FOUNDATION_EXPORTS,
    *TIMING_EXPORTS,
    *PERFORMANCE_EXPORTS,
    *SIGNAL_EXPORTS,
]

# Reviewed public signal APIs that intentionally remain outside the legacy
# generator-facing vocabulary compatibility projection.
LEGACY_SIGNAL_VOCABULARY_EXCLUSIONS = {
    "pulp/signal/waveset_transformer.hpp",
}

# Public headers can leave the frozen legacy bucket only through one of these
# explicit reviewed classifications or a capability binding above.
REVIEWED_HEADERS: list[dict[str, Any]] = [
    {
        "include": "pulp/signal/character_delay/reverse.hpp",
        "fingerprint": "sha256:8ffe9c4341a734e18aeae9900554cb042acfc3dd0982b243cde8705067140c91",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Character Delay's internal reverse segmenter, specialized for continuously "
            "slewed window lengths and modulated fractional reads. docs/reference/"
            "reverse-buffer.md records the decision to keep it internal and to expose "
            "ReverseBufferT as the public reverse primitive, so it makes no installed "
            "agent capability claim of its own."
        ),
    },
    {
        "include": "pulp/signal/reverse_buffer.hpp",
        "fingerprint": "sha256:cf64121f3ebd0bf931a60baf60860c8562c3d0dd7c4b2c866da07c38e72e5db8",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Prepared fixed-capacity streaming window reversal with explicit boundary "
            "shaping and a finite tail; it is a bounded buffering primitive intended for "
            "composition inside effects rather than an advertised generator DSP claim. "
            "It has no in-tree consumer today beyond the signal umbrella header."
        ),
    },
    {
        "include": "pulp/signal/simd_buffer.hpp",
        "fingerprint": "sha256:7780d3b9a8e734dbacd9b2d7d06c5d07328fe167da4d8d3ea88938c71ae5a973",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "SIMD-width-aligned sample storage and its aligned allocate/free pair, used "
            "for composition inside DSP kernels rather than advertised as a capability of "
            "its own. Its allocator branches per platform because the C11 aligned_alloc "
            "it wraps is unavailable below Android API 28, which is a portability detail "
            "of the primitive and not a change to the surface it presents."
        ),
    },
    {
        "include": "pulp/signal/tempo_delay.hpp",
        "fingerprint": "sha256:3ae02d16e00b3463e563d55be1ee0e7abc79db03173e7e1bae2d024ff3a4c059",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Pure BeatDivision-to-fractional-delay conversion over the compiled tempo "
            "range; it is a bounded conversion primitive intended for composition inside "
            "delay kernels rather than an advertised generator DSP claim. It has no "
            "in-tree consumer today beyond the signal umbrella header."
        ),
    },
    {
        "include": "pulp/audio/planar_audio_ring_buffer.hpp",
        "fingerprint": "sha256:3234f8016508d561dee810e774000fc32421aa1c2da4f6ad8f8edc0b4a03acbe",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Lock-free SPSC sample transport and overrun accounting used by realtime "
            "capture paths; it is bounded infrastructure rather than a generator DSP claim."
        ),
    },
    {
        "include": "pulp/audio/audio_probe.hpp",
        "fingerprint": "sha256:dfc218a6cd7bee7048c8c543be697165f127ee3d8cb5db99fbc0f23297232a5b",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Optional prepared output-boundary diagnostics and capture plumbing; "
            "it is a reusable observability surface rather than a generator DSP claim."
        ),
    },
    {
        "include": "pulp/signal/freeze_loop_sampler.hpp",
        "fingerprint": "sha256:fa2406081bd7e78a0097eab797da14d2e1854cd0a6972cf6971728050b19348a",
        "disposition": "capability_support",
        "capability_keys": ["signal.beat-repeat-kernel"],
        "rationale": (
            "Single rolling dry-history and immutable exact-capture owner reused by the "
            "beat-repeat kernel; legacy freeze and snapshot behavior remains source compatible."
        ),
    },
    {
        "include": "pulp/signal/wavetable.hpp",
        "fingerprint": "sha256:94e86b5efcbc54989b76312ed5bd270d29bc6d5758e3a240ca3b8ffc4bbecaae",
        "disposition": "capability_support",
        "capability_keys": ["audio.wavetable-authoring"],
        "rationale": (
            "The existing realtime Wavetable consumer and its shared band-ceiling plan are "
            "the installed output boundary for the offline wavetable-authoring capability."
        ),
    },
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
    {
        "include": "pulp/signal/lofi_chain.hpp",
        "fingerprint": "sha256:1b4def5ba6eb434e0ec7b46a350990f5356a54dc4215d5e21d262a4d55594024",
        "disposition": "capability_support",
        "capability_keys": ["signal.dither-quantizer"],
        "rationale": (
            "Publishes floor_shape, the dead-zone geometry the header's saturator already "
            "used, as a reusable transfer curve so a caller no longer has to reimplement it "
            "to get the same shape without the tanh."
        ),
    },
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
        "fingerprint": "sha256:e57fef7ae8facdc36a69e1a432de6b830495e6342a3356d5c684efe222e36176",
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
        "include": "pulp/midi/detail/arpeggiator_math.hpp",
        "fingerprint": "sha256:63f62d6a1272a3cfb360f6caf94b4ae4acf73e6c9a1942d7d8ea530eb595e050",
        "disposition": "capability_support",
        "capability_keys": ["midi.arpeggiator"],
        "rationale": (
            "Shared saturating clock and projection arithmetic supports the "
            "arpeggiator implementation without defining a separate capability."
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
        "fingerprint": "sha256:40f4e08a25956811f4466b031734e5fd418e313e4861007304dd33a33ca50e0e",
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
        "include": "pulp/midi/detail/note_schedule.hpp",
        "fingerprint": "sha256:2c3079340fe726d87e09042f023fb307aa7c0a0a9a25719cb1d4d0b16eb51e23",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Private tick-to-sample projection and scheduled-note queue shared by the note "
            "repeat and note delay kernels; it binds no capability of its own."
        ),
    },
    {
        "include": "pulp/midi/utility_contract.hpp",
        "fingerprint": "sha256:3ecd5aa5c92a2ac31e133059c97915257df2018b904f9fce39d1b0c6f1dadb70",
        "disposition": "capability_support",
        "capability_keys": [
            "midi.arpeggiator", "midi.channel-routing", "midi.chord-memory",
            "midi.controller-mapping", "midi.humanize", "midi.keyboard-split", "midi.latch",
            "midi.linear-step-player", "midi.monophonic-note-selection", "midi.note-delay",
            "midi.note-length-shaping", "midi.note-range-filtering", "midi.note-repeat",
            "midi.scale-aware-mpe-pitch", "midi.strum-spread",
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
        "fingerprint": "sha256:4c95583dd9992f762edb2a00191ecd785ba3fac600d5528adf24167ad13a389e",
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
        "fingerprint": "sha256:0b42a15443f679c5eb3c7f92940403f4886ff7286a62dfaa2b1e50645758ae27",
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
        "fingerprint": "sha256:6fb9907ba6984a2b76af66bd3a8f9a4e3008f4f0f29526beb6fbed64cafa6400",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "FastMath is shared scalar/SIMD implementation infrastructure rather "
            "than a standalone semantic DSP unit. Its bounded trig profiles are "
            "advertised through accepted consumer capabilities, not as raw math."
        ),
    },
    {
        "include": "pulp/signal/ladder_filter.hpp",
        "fingerprint": "sha256:1c586ff41aceec5c574002dbb5d9ae8b7d4a052c5e605a5fd27988fc7d7a7399",
        "disposition": "capability_support",
        "capability_keys": ["signal.drum-tom-voice"],
        "rationale": (
            "The generic ladder owns the compile-time saturation dispatch used by "
            "the measured tom consumer; it is not independently advertised."
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
        "include": "pulp/signal/frequency_response.hpp",
        "fingerprint": "sha256:0b571e77442e31ca61913ccbef792f1f775b691e730006bde351ca18f57581f8",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "This is the signal module umbrella include; it exposes no distinct "
            "consumer capability beyond the headers it aggregates."
        ),
    },
    {
        "include": "pulp/signal/detail/schroeder_allpass.hpp",
        "fingerprint": "sha256:64f8f2857c873c37bc94102544459d528459dd2c422eb0bf812a3d1d4b411760",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "This internal header shares the pure scalar Schroeder allpass recurrence "
            "used by several processors; it is not an independent authoring surface."
        ),
    },
    {
        "include": "pulp/signal/signal.hpp",
        "fingerprint": "sha256:0d3538c3925bfb69da24958a73ee0f723514e62959e1dab02d7658d3fbd8db30",
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
    {
        "include": "pulp/signal/waveset_transformer.hpp",
        "fingerprint": "sha256:9d0dcf5f66673d434f93481081be084dcfa7674613b801bb3d731117463aa267",
        "disposition": "unsupported_capability",
        "capability_keys": [],
        "rationale": (
            "The bounded WavesetTransformer is a signal-only public DSP API; it has no "
            "typed generator binding and makes no installed agent capability claim."
        ),
    },
    {
        "include": "pulp/playback/audio_renderer.hpp",
        "fingerprint": "sha256:6050c1569f5e90b404fc4dc8a95498964a796a418a1fe62a9ec4929bb8e57e2f",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "The audio renderer's error vocabulary (AudioRendererErrorCode, including "
            "the OfflineStretchRequired refusal) over forward-declared program, "
            "compiler-task, conversion-artifact and realtime-stretch types. It is the "
            "engine-internal render entry point composed by the playback module, not an "
            "advertised installed agent capability of its own."
        ),
    },
    {
        "include": "pulp/playback/audio_renderer_limits.hpp",
        "fingerprint": "sha256:4fc9eb204afba8208594e823cf27a3b89e5e0979ed8cfa064b34bdf111b0d0d0",
        "disposition": "capability_support",
        "capability_keys": ["sequence.controller-playback"],
        "rationale": (
            "Shared compile-path and realtime ceilings for the audio renderer, "
            "deliberately split out of the renderer API so the structural "
            "PlaybackProgram header can bound itself without pulling buffer or decoder "
            "surfaces. pulp/playback/program.hpp is the include the "
            "sequence.controller-playback bindings name, and it includes this header "
            "directly, so the advertised bindings do not compile without it."
        ),
    },
    {
        "include": "pulp/playback/automation_cursor.hpp",
        "fingerprint": "sha256:e333f64f1409003719d8c56feeea591d5f2dd9d47849c525a28d6c3e7df8e97e",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Realtime block reader over a compiled automation program: "
            "AutomationTransition seeds and linear ramps, AutomationBlockEvent, "
            "AutomationCursorCode, and AutomationProgramAdoption. It is a bounded "
            "realtime cursor primitive intended for composition inside a renderer, and "
            "it advertises no installed agent capability of its own."
        ),
    },
    {
        "include": "pulp/playback/automation_limits.hpp",
        "fingerprint": "sha256:c4a7cb9dd6f07fed1c34a5f5d590de305ac2a32039528a7d2d3bfe930dd0cc05",
        "disposition": "capability_support",
        "capability_keys": ["sequence.controller-playback"],
        "rationale": (
            "AutomationPlaybackLimits and its kMaximum* hard ceilings, with "
            "web_defaults() and platform_defaults() presets that bound a compiled "
            "automation program before it reaches the audio thread. "
            "pulp/playback/program.hpp includes it directly, so it is part of the "
            "compile-time surface the sequence.controller-playback bindings are "
            "declared over."
        ),
    },
    {
        "include": "pulp/playback/automation_program.hpp",
        "fingerprint": "sha256:c12cec87efad34083f509829ab4a6b51cde276c3cafb7ed05f0d9697a7b0b716",
        "disposition": "capability_support",
        "capability_keys": ["sequence.controller-playback"],
        "rationale": (
            "AutomationProgramInstanceToken, AutomationProgramErrorCode/Error, and "
            "AutomationProgramSegment — the compiled automation vocabulary a track "
            "program is expressed over, carrying both tick and sample bounds so a "
            "segment means the same thing to the document and to the render path. It "
            "reaches pulp/playback/program.hpp through track_mixer_program.hpp, so the "
            "sequence.controller-playback bindings depend on it at compile time."
        ),
    },
    {
        "include": "pulp/playback/automation_recording.hpp",
        "fingerprint": "sha256:06ebed4aa0f2135a325a97048c91646841357d4967e610985c64c3ba3f5df697",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Automation capture vocabulary — AutomationRecordMode Touch/Latch/Write, "
            "RecordedAutomationPoint, and the record and curve-materialization error "
            "enums. It describes how captured gestures become lane data inside the "
            "playback module and makes no installed agent capability claim."
        ),
    },
    {
        "include": "pulp/playback/buffered_content_source.hpp",
        "fingerprint": "sha256:67d7fbf99fa998a65c6204c7e246f1ffa8ab448953179f3e05d24eae4b143a39",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Ring-backed source for content produced ahead of the playhead — the "
            "buffering half of timeline::ProductionMode::Buffered, with its thread "
            "model documented in the header. Its own documentation states that Pulp "
            "ships no inference runtime, no model weights, and no producer that "
            "performs inference, so this is a bounded buffering primitive for a host- "
            "supplied producer rather than an advertised capability."
        ),
    },
    {
        "include": "pulp/playback/capture_engine.hpp",
        "fingerprint": "sha256:cddd98aca3fdc18fb931a42f71009752949f8e2dc10530ef4e158d8f071bc0ce",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "CaptureTrackConfig and CaptureEngineConfig, including the default "
            "preallocation cap that bounds how much a capture may reserve up front. It "
            "is the bounded recording-buffer substrate the recording coordinator "
            "composes, not an installed agent capability."
        ),
    },
    {
        "include": "pulp/playback/chord_pattern_renderer.hpp",
        "fingerprint": "sha256:60e4e8779676eedff5f0e2f4b31b7f2cb610079168cbdaa84c7c3b774d3f919a",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Registration of the pulp.playback.chord_pattern content schema and its "
            "ChordPatternContent value type against the compile-context registry. It is "
            "one registered content kind inside the playback compiler rather than an "
            "advertised consumer capability; the capability claim, if one is made, "
            "belongs to the content surface that authors these patterns."
        ),
    },
    {
        "include": "pulp/playback/clip_launch.hpp",
        "fingerprint": "sha256:4d6a6bae3dafd68547a534466441add75ba3db6b388879d6602b71ec2ef504e5",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Realtime clip-launch engine on the monotonic clock, with a constexpr "
            "next_launch_boundary() and documented saturation behaviour at the ends of "
            "its range. It is a bounded realtime scheduling primitive composed inside "
            "the playback engine and binds no installed agent capability of its own."
        ),
    },
    {
        "include": "pulp/playback/compile_context_registry.hpp",
        "fingerprint": "sha256:d7da460ae42d89c31bb01a5c1f30da582d96a8abd33e447f2a865f5537a4422f",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Registry of content-program output kinds and renderer state policies "
            "(Reset versus CarryByItemId) plus the ContentFragmentNote fragment type. "
            "It is the extension seam the program compiler resolves registered content "
            "through, not an advertised installed capability."
        ),
    },
    {
        "include": "pulp/playback/compile_executor.hpp",
        "fingerprint": "sha256:047d2b5985fd195f281f501c42435de4b1865140bbebbbe7860cca00a2c4ce44",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "CompileSliceBudget, CompileTaskStatus, the abstract CompileTask and "
            "CompileExecutor interfaces, and the DeferredCompileExecutor. It is "
            "scheduling infrastructure that lets compilation be sliced against a "
            "budget, with no capability surface of its own."
        ),
    },
    {
        "include": "pulp/playback/dirty_track_resolver.hpp",
        "fingerprint": "sha256:d0280b67a3ed178db15c9022854b3adcc034b6b0a985fdf1ef39a7c772f76e24",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "DirtyTrackSet and resolve_dirty_tracks(): the canonical translation from "
            "transaction dirtiness to root-track compiler dirtiness in one fail-closed "
            "path. It is a pure internal derivation shared by the compiler and makes no "
            "installed agent capability claim."
        ),
    },
    {
        "include": "pulp/playback/event_compensation.hpp",
        "fingerprint": "sha256:cf9986a4cd145275093e73311281ec8eb6abc52bf4de97eec4da6e8304849db8",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "EventCompensationShift, expressed in samples rather than ticks by "
            "deliberate design, with the documented invariant that compensation shifts "
            "the scheduling window and never rewrites event data. It is a bounded "
            "scheduling adjustment composed inside the renderer, not an advertised "
            "capability."
        ),
    },
    {
        "include": "pulp/playback/external_sync.hpp",
        "fingerprint": "sha256:7ef8447d4783267894611f1a14032347fcbab6e2f2e6dd2208ab1053717e764f",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "MTC vocabulary — MtcFrameRate (including 29.97 drop), MtcTimecode, "
            "validity checking and conversion to samples. It is a bounded timecode "
            "conversion primitive for an external-sync source and advertises no "
            "installed agent capability of its own."
        ),
    },
    {
        "include": "pulp/playback/generated_event_source.hpp",
        "fingerprint": "sha256:6253ce3419ffa5a655f0f941ef9995d6980e6175efdd9fe32685bdfc95f7d57b",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Bounded single-producer/single-consumer handoff for event batches produced "
            "ahead of the playhead. A missing batch advances as event silence and "
            "requests an active-note flush so a lost note-off cannot become a stuck "
            "note. It is a realtime handoff primitive for a host-supplied producer "
            "rather than an advertised capability."
        ),
    },
    {
        "include": "pulp/playback/midi_capture_materializer.hpp",
        "fingerprint": "sha256:e6734737a53c8cb9186d17cee9a562500bf29823aacad7b25f67157b0ba3c7b2",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Configuration, result and error types for turning a MIDI capture buffer "
            "into materialized lane data. It is one internal step of the recording path "
            "inside the playback module and makes no installed agent capability claim."
        ),
    },
    {
        "include": "pulp/playback/note_renderer.hpp",
        "fingerprint": "sha256:b4abb63c3c555e5478ce5153e9f967e7fb074d744211c6fa6366720e04cbed15",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Note emission for one render block, including the detail-namespace offset- "
            "in-range helper and NoteRenderCode. It is the internal note half of the "
            "renderer shell rather than an advertised consumer capability."
        ),
    },
    {
        "include": "pulp/playback/offline_stretch_artifact.hpp",
        "fingerprint": "sha256:9df2a677c997570db86c0b0810c766c6735744efda48d56e68eeba83e7c48703",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Offline stretch artifact identity: a pinned algorithm version, its "
            "configuration, and a key built from content hashes plus the source range "
            "so a cached artifact can only be reused for exactly the input it was "
            "rendered from. It is cache-identity infrastructure, not an advertised "
            "capability."
        ),
    },
    {
        "include": "pulp/playback/production_class.hpp",
        "fingerprint": "sha256:8321d125d7df42c7892b5ee99accef450073072e79aa575af898ea334faadec4",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Derivations of what a compiled program may honestly claim about being "
            "replayed — provider and track production declarations and the resulting "
            "program reproducibility. It is an internal honesty derivation over the "
            "program graph and binds no installed agent capability of its own."
        ),
    },
    {
        "include": "pulp/playback/program_compiler.hpp",
        "fingerprint": "sha256:ff4c9febf95f0752b9c77e3ac113fce02cb3bff99c2cdc5e1a870b0135bb6412",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "The compile path from committed document deltas to exactly recompiled "
            "tracks: TrackCompilePolicy, CompileInvalidationInput, and a compiler that "
            "rejects a request whose project or revision does not match. It is the "
            "producing side of a compiled program; the installed claim is made by the "
            "compiled program surface it emits, not by the compiler entry point."
        ),
    },
    {
        "include": "pulp/playback/program_identity.hpp",
        "fingerprint": "sha256:92ea83fe26d3d39d244ba0860cb9094ff08e0392c1bc19ce6279412b1adbc0b4",
        "disposition": "capability_support",
        "capability_keys": ["sequence.controller-playback"],
        "rationale": (
            "ProgramGeneration, RendererProgramKey, and is_monotonic_renderer_adoption: "
            "the identity vocabulary that lets a renderer refuse a non-monotonic "
            "program swap rather than adopt a stale publication. "
            "pulp/playback/program.hpp includes it directly and expresses program "
            "publication in these types, so it is required support for the "
            "sequence.controller-playback bindings rather than an independent claim."
        ),
    },
    {
        "include": "pulp/playback/program_wire.hpp",
        "fingerprint": "sha256:f9c86057b4ad3169207bf924d797ca0ccdf931c691b42c65d2212073740f554b",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "The flat, pointer-free byte layout one compiled program generation is "
            "published in, plus its native encoder and validating decoder — the "
            "crossing form for a consumer that does not share the producer's address "
            "space, such as an AudioWorklet and its Worker. Sections tile the payload "
            "exactly and a length that disagrees with its content is a rejection rather "
            "than an interpretation; a static_assert refuses a big-endian host. It is a "
            "serialization surface with no typed capability binding of its own today."
        ),
    },
    {
        "include": "pulp/playback/realtime_stretch_renderer.hpp",
        "fingerprint": "sha256:25b344ef63523fdad5f4af59072a908b488042f2e149494ca36ec3e1beb32073",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "RealtimeStretchRenderCode and the live stretch runtime that owns a "
            "disjoint mutable lane per track, with explicit codes for unsupported "
            "scrubbing, impossible ratios, backpressure and underflow. It is a bounded "
            "realtime renderer composed inside the playback engine, not an advertised "
            "installed capability."
        ),
    },
    {
        "include": "pulp/playback/realtime_stretch_state_bank.hpp",
        "fingerprint": "sha256:dd02d3ef9b543e3d8744772ccfc35d850e6df4fae9d762eadcf49a74a223c28a",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "RealtimeStretchStateSpec and the state bank's refusal vocabulary — "
            "duplicate identity, state, channel, time-ratio and byte limits, processor- "
            "prepare rejection and allocation failure. It is a bounded preallocated "
            "state pool for the realtime stretch path and makes no installed agent "
            "capability claim."
        ),
    },
    {
        "include": "pulp/playback/recording_commit.hpp",
        "fingerprint": "sha256:87f362d0519e153abdcced725703cfc23bc0e5af87000d47433ec4669fd0581f",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "RecordingTakeCommitRequest, SealedRecordingTake and RecordingCommitError: "
            "the sealed handoff from a finished capture to the document commands and "
            "media asset that land it. It is the internal commit step of the recording "
            "path rather than an advertised capability surface."
        ),
    },
    {
        "include": "pulp/playback/recording_coordinator.hpp",
        "fingerprint": "sha256:96dfd4b5b13477a83bfe35aa40b8ec5fd7806496fe0c045c14a38b337a022572",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Recording monitor modes and resolved paths (Off/Direct/Software/Auto), "
            "recording sources, and the track and coordinator configuration around "
            "them. It is the playback module's internal recording orchestration and "
            "binds no installed agent capability of its own."
        ),
    },
    {
        "include": "pulp/playback/stable_renderer_shell.hpp",
        "fingerprint": "sha256:d44b66f80099a3e80044fed6f7a21fdf752957a6879edcd8d76846bec9a87bc0",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "RendererCarryState — what survives a program swap — and "
            "PlaybackProgramBlock, the move-only non-owning block view whose "
            "documentation requires an enclosing immutable audio-thread publication to "
            "keep the program alive for its whole lifetime. It is the realtime borrow "
            "and latch side of the program surface, composed by the renderer rather "
            "than advertised on its own."
        ),
    },
    {
        "include": "pulp/playback/tempo_sync.hpp",
        "fingerprint": "sha256:7c5c6607b7bbbc26622b3f6ab2400c7b9727362ccea6b81a69da3184bbbeae8b",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "The backend-independent external-clock seam: an opaque TempoSyncHostTime "
            "that carries its source identity so a device, wall, or different Link "
            "clock cannot be handed to the wrong transport, and a TempoSyncCommand "
            "bundle applied at the first output sample of a block. It is the licensing- "
            "safe abstraction an optional third-party tempo-sync SDK plugs in behind, "
            "not an installed capability of its own."
        ),
    },
    {
        "include": "pulp/playback/track_automation_program.hpp",
        "fingerprint": "sha256:5c329e154a3d2b33f3817760735982d8f7328a44e3c076c24a23016e658b2535",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Immutable compiler-supplied grouping of one track's compiled automation "
            "lanes, with a refusal vocabulary that reports the colliding control "
            "verbatim on a duplicate lane or target. Its own documentation is explicit "
            "that it validates the grouping without proving document provenance, so it "
            "is internal compiled structure rather than an advertised capability."
        ),
    },
    {
        "include": "pulp/playback/track_automation_renderer.hpp",
        "fingerprint": "sha256:46f89f442e6010891f5ec1123ed135a767ab7f1388771a20bccfbe9b37a2bf66",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Realtime emission of automation to device placements: TrackAutomationEvent "
            "with its sample offset and ramp duration, DeviceAutomationBatch with its "
            "coalescing flag, and a renderer code set that refuses a program, track or "
            "tempo-map mismatch rather than emitting into the wrong target. It is a "
            "bounded realtime emitter composed inside the renderer and makes no "
            "installed agent capability claim today."
        ),
    },
    {
        "include": "pulp/playback/track_mixer_program.hpp",
        "fingerprint": "sha256:88c89096272bdd2c56b6b756fd426a7c7e09861286032a3e29ed7df47f966396",
        "disposition": "capability_support",
        "capability_keys": ["sequence.controller-playback"],
        "rationale": (
            "The compiled form of a track's own level and stereo placement, where a "
            "non-null automation lane supersedes the authored constant entirely so a "
            "lane and a constant never both apply. A static_assert ties "
            "kMaximumTrackMixerGain to timeline::kMaximumTrackGainLinear, which is what "
            "keeps the render path from honouring an automated gain the document would "
            "refuse to store, and transparent() is what keeps an untouched track bit- "
            "identical to its pre-mixer render. pulp/playback/program.hpp includes it "
            "directly, so it is compile-time support for the sequence.controller- "
            "playback bindings."
        ),
    },
    {
        "include": "pulp/playback/transport.hpp",
        "fingerprint": "sha256:fd25dfd0e69c3355cce4802bc9bb40771218d35cb4d7c2fd529749b25fceb993",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "The transport control surface — play, stop, loop, scrub and tempo — with a "
            "refusal vocabulary that rejects a loop or scrub window too short for the "
            "maximum block and an epoch advance that fails rather than wrap to an "
            "aliased identity. It is the playback engine's own control object; its "
            "agent-facing exposure is claimed by the capability rows that bind a "
            "transport surface, not by this header."
        ),
    },
    {
        "include": "pulp/signal/convolver.hpp",
        "fingerprint": "sha256:7b6f5cc6d4bd8a3c71c07149958db256d08aa0f264544df0abfa07eb3a4b5825",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Uniform partitioned convolution engine with a lock-free live IR swap and an "
            "opt-in swap crossfade. docs/reference/modules.md documents it as a bounded DSP "
            "primitive a plugin drives from its own process() at a fixed block size, and its "
            "generator-facing shape is already published through the signal compatibility "
            "vocabulary, so it makes no installed agent capability claim of its own."
        ),
    },
    {
        "include": "pulp/signal/convolver_messages.hpp",
        "fingerprint": "sha256:2d4d3361cfa9099d4cd549fc250de196c1053c9c5448fa15b540089433e74e29",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Audio-thread hand-off plumbing behind PartitionedConvolver: the per-IR state, "
            "the convolver's shared input history, and the lock-free swapper that shuttles "
            "them between a worker thread and the audio thread over runtime::Handoff. "
            "Real-time ownership-transfer infrastructure for that engine rather than an "
            "advertised generator surface; it carries no capability claim of its own."
        ),
    },
]
SURFACE_TOMBSTONES: list[dict[str, Any]] = []
CAPABILITY_TOMBSTONES: list[dict[str, Any]] = []
