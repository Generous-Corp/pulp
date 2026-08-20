"""Declarative registry for the installed agent capability contract."""
from __future__ import annotations

from typing import Any

from agent_capability_catalog_foundations import EXPORTS as FOUNDATION_EXPORTS
from agent_capability_catalog_performance import EXPORTS as PERFORMANCE_EXPORTS
from agent_capability_catalog_signal import EXPORTS as SIGNAL_EXPORTS
from agent_capability_catalog_timing import EXPORTS as TIMING_EXPORTS

REVIEWED_MINIMAL_TARGETS = {
    "pulp/audio/instrument_voice_allocator.hpp": "Pulp::audio",
    "pulp/audio/midi_voice_modulation_adapter.hpp": "Pulp::audio",
    "pulp/audio/onset_detector.hpp": "Pulp::audio",
    "pulp/audio/unison_voice_stack.hpp": "Pulp::audio",
    "pulp/audio/voice_runtime_facade.hpp": "Pulp::audio",
    "pulp/audio/wavetable_authoring.hpp": "Pulp::audio",
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
    "pulp/music/pattern_development.hpp": "Pulp::music",
    "pulp/music/rhythm_relationship.hpp": "Pulp::music",
    "pulp/music/spelling.hpp": "Pulp::music",
    "pulp/music/voicing.hpp": "Pulp::music",
    "pulp/sequence/host_transport_projector.hpp": "Pulp::sequence",
    "pulp/signal/saturator.hpp": "Pulp::signal",
    "pulp/signal/analysis_frontends.hpp": "Pulp::signal",
    "pulp/signal/audio_matrix_mixer.hpp": "Pulp::signal",
    "pulp/signal/breakpoint_envelope.hpp": "Pulp::signal",
    "pulp/signal/beat_repeat_kernel.hpp": "Pulp::signal",
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
    "pulp/signal/nlms_adaptive_filter.hpp": "Pulp::signal",
    "pulp/signal/noise_tilt.hpp": "Pulp::signal",
    "pulp/signal/nonlinear_shaping.hpp": "Pulp::signal",
    "pulp/signal/nway_crossfade.hpp": "Pulp::signal",
    "pulp/signal/path_latency_aligner.hpp": "Pulp::signal",
    "pulp/signal/path_switcher.hpp": "Pulp::signal",
    "pulp/signal/particle_collision_exciter.hpp": "Pulp::signal",
    "pulp/signal/particle_percussion_voice.hpp": "Pulp::signal",
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
        "include": "pulp/signal/reverse_buffer.hpp",
        "fingerprint": "sha256:cf64121f3ebd0bf931a60baf60860c8562c3d0dd7c4b2c866da07c38e72e5db8",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Prepared fixed-capacity streaming window reversal with explicit boundary "
            "shaping and a finite tail; it is a bounded buffering surface reused by "
            "effects rather than an advertised generator DSP claim."
        ),
    },
    {
        "include": "pulp/signal/tempo_delay.hpp",
        "fingerprint": "sha256:3ae02d16e00b3463e563d55be1ee0e7abc79db03173e7e1bae2d024ff3a4c059",
        "disposition": "infrastructure",
        "capability_keys": [],
        "rationale": (
            "Pure BeatDivision-to-fractional-delay conversion over the compiled tempo "
            "range; it is a bounded conversion surface reused by delay kernels rather "
            "than an advertised generator DSP claim."
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
        "fingerprint": "sha256:027d6cb42c8816b8b01b82510eaed6ee55507febc444b1dcfa1bce8257247875",
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
]
SURFACE_TOMBSTONES: list[dict[str, Any]] = []
CAPABILITY_TOMBSTONES: list[dict[str, Any]] = []
