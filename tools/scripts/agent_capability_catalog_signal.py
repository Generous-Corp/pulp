"""Declarative installed agent capability records."""
from agent_capability_registry_types import binding, capability

EXPORTS = [
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
                    header_fingerprint="sha256:36ef815dfd9072f7bba8d72aa0e0d53662952e1ee470f1c6b0cd4d64413c55ac"),
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
                    header_fingerprint="sha256:a7874bab7001e6c6ad31fd85a7176ec13af44c5047bf38d631e207ce3ae914c6"),
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
                    header_fingerprint="sha256:375d2e2acd389ad5a7194d5c362eac77a03e0a6c4e2c89bf38b09e10d86ed5e7"),
            binding(role="history", kind="cpp_type", include="pulp/signal/fractional_delay.hpp",
                    qualified_name="pulp::signal::FractionalDelayHistoryT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:375d2e2acd389ad5a7194d5c362eac77a03e0a6c4e2c89bf38b09e10d86ed5e7"),
        ],
        _link_probes=[
            {"role": "line", "binding": "pulp::signal::FractionalDelayLineT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "history", "binding": "pulp::signal::FractionalDelayHistoryT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.nlms-adaptive-filter", domain="signal",
        summary="Prepared bounded normalized least-mean-squares adaptive FIR filtering.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control; allocates fixed retained storage",
                   "process": "single audio writer", "reset": "single audio writer",
                   "release": "destruction off audio"},
        state_model=(
            "Prepared double-precision reference history and adaptive weights plus a "
            "three-slot single-writer/single-reader coefficient publication bank."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite primary/desired and reference audio samples",
        output_domain="adaptive FIR estimate, residual error, and bounded coefficient snapshot",
        units=["samples", "frames", "taps", "normalized step size", "linear amplitude"],
        latency="zero",
        tail="reference and coefficient history until reset",
        scheduling="sample-synchronous single writer; one concurrent bounded snapshot reader",
        bindings=[binding(
            role="entrypoint", kind="cpp_type",
            include="pulp/signal/nlms_adaptive_filter.hpp",
            qualified_name="pulp::signal::NlmsAdaptiveFilterT<float>",
            target="Pulp::signal",
            header_fingerprint="sha256:ac5d8961a7dc69bd24a4ab8f6389e6e0f62eb5ac7dc433ebabe845e15366298b",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::signal::NlmsAdaptiveFilterT<float>",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "48000.0, 8",
        }],
    ),
    capability(
        key="signal.particle-percussion", domain="signal",
        summary=(
            "Deterministic depleted-energy particle collisions and prepared resonant "
            "percussion voices."
        ),
        rt_class="mixed",
        lifecycle={"construction": "control",
                   "prepare": "control; voice preparation may allocate retained modal storage",
                   "process": "audio", "reset": "audio",
                   "release": "destruction off audio"},
        state_model=(
            "Fixed seeded collision, jitter, and routing generators plus collision energy, "
            "resonator state, and prepared five-mode modal storage."
        ),
        seed_model="public uint64 seed with independently purpose-derived streams",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="normalized excitation and bounded collision, decay, and body-model controls",
        output_domain="collision events or mono particle-percussion audio",
        units=["samples", "hertz", "milliseconds", "normalized energy", "linear amplitude"],
        latency="zero",
        tail=("finite collision settling plus the longest configured resonator or modal decay "
              "when sustain_floor is positive; unbounded and reported as -1 when it is zero"),
        scheduling="sample-synchronous after control-side preparation and model configuration",
        bindings=[
            binding(
                role="collision_exciter", kind="cpp_type",
                include="pulp/signal/particle_collision_exciter.hpp",
                qualified_name="pulp::signal::ParticleCollisionExciterT<float>",
                target="Pulp::signal",
                header_fingerprint="sha256:bfacb7a8890939910c7614e354646a6c9bdfe14d506848a69aa93c93b458d66e",
            ),
            binding(
                role="voice", kind="cpp_type",
                include="pulp/signal/particle_percussion_voice.hpp",
                qualified_name="pulp::signal::ParticlePercussionVoiceT<float>",
                target="Pulp::signal",
                header_fingerprint="sha256:a9fbfb370bbb6d2b47f880d6a2687075b943ae6f4ba69213abb2a7279e5027a4",
            ),
        ],
        _link_probes=[
            {"role": "collision_exciter",
             "binding": "pulp::signal::ParticleCollisionExciterT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "voice", "binding": "pulp::signal::ParticlePercussionVoiceT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.waveguide-primitives", domain="signal",
        summary=(
            "Prepared bidirectional waveguide rails, passive reflection boundaries, and "
            "fixed-capacity pressure-wave scattering junctions."
        ),
        rt_class="mixed",
        lifecycle={"construction": "control",
                   "prepare": "control; line preparation allocates bounded retained history",
                   "process": "audio", "reset": "audio",
                   "release": "destruction off audio"},
        state_model=(
            "Two prepared double-precision traveling-wave histories and a smoothed length, "
            "one-pole boundary state, and fixed-capacity impedance plus prior-output state."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain=(
            "finite traveling pressure waves, bounded fractional line lengths, passive "
            "reflection controls, and two to four positive impedance ratios"
        ),
        output_domain="delayed, reflected, or losslessly scattered traveling pressure waves",
        units=["samples", "seconds", "linear reflection gain", "dimensionless impedance"],
        latency=(
            "current smoothed one-way line length (target reported separately); reflection "
            "and junction operations add zero samples, and two-phase boundary composition "
            "adds no hidden sample"
        ),
        tail="prepared line history plus passive one-pole boundary decay until reset",
        scheduling=(
            "sample-synchronous; feedback networks read every line output, compute all "
            "boundaries, then push every line input in the same frame"
        ),
        bindings=[
            binding(
                role="line", kind="cpp_type",
                include="pulp/signal/waveguide_line.hpp",
                qualified_name="pulp::signal::WaveguideLineT<float>",
                target="Pulp::signal",
                header_fingerprint="sha256:fbedd57acbc69cf0b827c3a17dcc93a90f43bb14af20bd57ce6d10bb4910d995",
            ),
            binding(
                role="reflection", kind="cpp_type",
                include="pulp/signal/waveguide_reflection_filter.hpp",
                qualified_name="pulp::signal::WaveguideReflectionFilterT<float>",
                target="Pulp::signal",
                header_fingerprint="sha256:13cde0afa486a160cdfc055160ac97230a191569261c7b7e9f04a5f7396141f6",
            ),
            binding(
                role="junction", kind="cpp_type",
                include="pulp/signal/waveguide_junction.hpp",
                qualified_name="pulp::signal::WaveguideJunctionT<float, 4>",
                target="Pulp::signal",
                header_fingerprint="sha256:3b2bfa32d07f91fc6bdbde2e1544df94b1d0646aca287bb88db58257902cf70d",
            ),
        ],
        _link_probes=[
            {"role": "line", "binding": "pulp::signal::WaveguideLineT<float>",
             "operation": "member_call", "member": "prepare",
             "arguments": "48000.0, 0.05"},
            {"role": "reflection",
             "binding": "pulp::signal::WaveguideReflectionFilterT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "junction", "binding": "pulp::signal::WaveguideJunctionT<float, 4>",
             "operation": "member_call", "member": "reset", "arguments": ""},
        ],
    ),
    capability(
        key="signal.reed-waveguide-loop", domain="signal",
        summary=(
            "Bounded explicit single-reed excitation and a fixed-topology bore owner with "
            "whole-loop 1x/2x/4x oversampling."
        ),
        rt_class="mixed",
        lifecycle={"construction": "control",
                   "prepare": "control; allocates bounded line and oversampler storage",
                   "process": "audio", "reset": "control; clears prepared line history",
                   "release": "destruction off audio"},
        state_model=(
            "Memoryless normalized reed valve plus owned linear-phase oversampler, "
            "bidirectional bore histories, retuning glide, and passive bell boundary state."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain=(
            "normalized finite mouth pressure, physical one-way seconds, bounded reed "
            "controls, base-rate bell controls, and oversampling factor 1, 2, or 4"
        ),
        output_domain="finite pressure wave arriving at the bell before reflection",
        units=["samples", "seconds", "normalized pressure", "oversampling factor"],
        latency="linear-phase oversampler base-rate delay; bore length is resonator state",
        tail="unbounded while the driven feedback loop sustains; reported as -1",
        scheduling=(
            "sample-synchronous; every reed boundary, bell boundary, and bidirectional "
            "line advance executes once per oversampled callback"
        ),
        bindings=[
            binding(
                role="reed_exciter", kind="cpp_type",
                include="pulp/signal/waveguide_reed_exciter.hpp",
                qualified_name="pulp::signal::ReedExciterT<float>",
                target="Pulp::signal",
                header_fingerprint="sha256:50fd8027a329b2a8c1c4dd983349e0c9ccf3b958ff3374adaa19dd00e7e8604a",
            ),
            binding(
                role="whole_loop", kind="cpp_type",
                include="pulp/signal/reed_waveguide_loop.hpp",
                qualified_name="pulp::signal::ReedWaveguideLoopT<float>",
                target="Pulp::signal",
                header_fingerprint="sha256:b397431656231ddc94fbd0618f39582ac4ed1f018573479bfbf44fc2281546a7",
            ),
        ],
        _link_probes=[
            {"role": "reed_exciter", "binding": "pulp::signal::ReedExciterT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "whole_loop", "binding": "pulp::signal::ReedWaveguideLoopT<float>",
             "operation": "member_call", "member": "prepare",
             "arguments": "48000.0, 0.05, 2"},
        ],
    ),
    capability(
        key="signal.beat-repeat-kernel", domain="signal",
        summary=(
            "Tempo-map-quantized exact dry-history capture with bounded repeat, gate, "
            "reverse/alternate playback, and direct trigger/stop/seek controls; pitch excluded."
        ),
        contract_version={"major": 2, "minor": 0},
        rt_class="mixed",
        lifecycle={"construction": "control",
                   "prepare": "control; binds sample rate and allocates one rolling-history/capture owner plus bounded transition scratch",
                   "process": "audio", "reset": "audio",
                   "release": "snapshot/restore and destruction off audio"},
        state_model=(
            "One FreezeLoopSampler dry-history/capture owner, pending compiled grid edge, "
            "prepared RationalRate, active loop phase/direction/gate, continuity cursor, "
            "and bounded transition state."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "input"},
        input_domain=(
            "finite planar dry audio, immutable CompiledTempoMap matching the prepared "
            "RationalRate, canonical BeatDivision, requested sample, contiguous block "
            "positions, and direct controls"
        ),
        output_domain="dry passthrough or repeated immutable captured audio",
        units=["samples", "frames", "canonical ticks", "normalized gate duty"],
        latency="zero",
        tail="configured bounded transition sample count",
        scheduling=(
            "strictly next canonical division edge; captures [edge-N, edge) before writing "
            "the edge dry sample; explicit or detected transport discontinuity resets the "
            "pending arm, active loop, transitions, and rolling history"
        ),
        bindings=[
            binding(
                role="kernel", kind="cpp_type",
                include="pulp/signal/beat_repeat_kernel.hpp",
                qualified_name="pulp::signal::BeatRepeatKernelT<float>",
                target="Pulp::signal",
                header_fingerprint="sha256:f7c213c193f826f3e791688dbf3dd4626f0b3766aa8659a8910e98e7691ba23e",
            ),
        ],
        _link_probes=[
            {"role": "kernel", "binding": "pulp::signal::BeatRepeatKernelT<float>",
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
        key="signal.fir-design", domain="signal",
        summary=(
            "Offline weighted least-squares design of bounded real Type I-IV linear-phase FIRs "
            "from sampled frequency targets."
        ),
        rt_class="offline",
        lifecycle={"construction": "control", "prepare": "offline design; may allocate bounded workspace",
                   "process": "offline design request", "reset": "none",
                   "release": "destruction off audio"},
        state_model=(
            "Bounded sampled targets, pivoted QR workspace, and returned coefficients, residuals, "
            "rank, and conditioning diagnostics."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "not_applicable",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite weighted frequency/amplitude samples in [0, pi] and a bounded tap/type specification",
        output_domain="real linear-phase FIR coefficients and weighted fit diagnostics",
        units=["radians per sample", "taps", "linear amplitude", "weighted RMS error"],
        latency="offline whole-design solve", tail="none", scheduling="offline request",
        bindings=[binding(
            role="designer", kind="cpp_function", include="pulp/signal/fir_design.hpp",
            qualified_name="pulp::signal::design_fir_least_squares",
            target="Pulp::signal",
            header_fingerprint="sha256:55a8d1dd6b4b8871a84b15f0e60f8ca2a840471fb092a59d313be4bff38a3162",
            address_expression=(
                "static_cast<pulp::signal::FirLeastSquaresResult (*)"
                "(std::span<const pulp::signal::FirDesignPoint>, "
                "const pulp::signal::FirLeastSquaresOptions&)>("
                "&pulp::signal::design_fir_least_squares)"
            ),
        )],
        _link_probes=[{
            "role": "designer", "binding": "pulp::signal::design_fir_least_squares",
            "operation": "function_call",
            "arguments": "std::span<const pulp::signal::FirDesignPoint>{}, pulp::signal::FirLeastSquaresOptions{}",
        }],
    ),
    capability(
        key="signal.minimum-phase-fir", domain="signal",
        summary=(
            "Offline cepstral minimum-phase FIR reconstruction from bounded one-sided "
            "magnitude bins."
        ),
        rt_class="offline",
        lifecycle={"construction": "none", "prepare": "none",
                   "process": "offline reconstruction; may allocate bounded workspace",
                   "reset": "none", "release": "returned vectors destroyed off audio"},
        state_model=(
            "Stateless reconstruction over a bounded radix-2 work spectrum, returning owned "
            "coefficients, measured magnitudes, and per-bin errors."
        ),
        seed_model="none",
        # Real-to-complex FFT round trips and a complex exponential accumulate
        # rounding, so equality holds only within a tolerance, unlike the
        # pivoted-QR linear-phase designer's bit-exact promise.
        determinism={"repeatability": "tolerance_bounded", "block_partition": "not_applicable",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain=(
            "finite nonnegative one-sided magnitude bins from DC through Nyquist for a bounded "
            "radix-2 FFT size"
        ),
        output_domain=(
            "real causal minimum-phase FIR coefficients with measured magnitude and error "
            "diagnostics"
        ),
        units=["linear magnitude", "coefficients", "FFT bins", "bytes"],
        latency="offline whole-reconstruction transform", tail="none",
        scheduling="offline request",
        bindings=[binding(
            role="reconstructor", kind="cpp_function", include="pulp/signal/fir_design.hpp",
            qualified_name="pulp::signal::reconstruct_minimum_phase_fir",
            target="Pulp::signal",
            header_fingerprint="sha256:55a8d1dd6b4b8871a84b15f0e60f8ca2a840471fb092a59d313be4bff38a3162",
            address_expression=(
                "static_cast<pulp::signal::MinimumPhaseFirResult (*)"
                "(std::span<const double>, "
                "const pulp::signal::MinimumPhaseFirOptions&)>("
                "&pulp::signal::reconstruct_minimum_phase_fir)"
            ),
        )],
        _link_probes=[{
            "role": "reconstructor", "binding": "pulp::signal::reconstruct_minimum_phase_fir",
            "operation": "function_call",
            "arguments": "std::span<const double>{}, pulp::signal::MinimumPhaseFirOptions{}",
        }],
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
                    header_fingerprint="sha256:576ed92bd47a0cce545d9eb5df9a5e83732829944550fff7416c552de2506a2f"),
            binding(role="stereo", kind="cpp_type", include="pulp/signal/dynamics_contract.hpp",
                    qualified_name="pulp::signal::StereoEnvelopeFollowerT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:576ed92bd47a0cce545d9eb5df9a5e83732829944550fff7416c552de2506a2f"),
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
        contract_version={"major": 1, "minor": 1},
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
        bindings=[
            binding(role="entrypoint", kind="cpp_type", include="pulp/signal/multi_channel_meter.hpp",
                    qualified_name="pulp::signal::MultiChannelMeterT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:44ef8439be936bb441790331e4bd4fc2b4e233962635ee49a88ebc4820b54772"),
            binding(role="loudness_support", kind="cpp_type", include="pulp/signal/multi_channel_meter.hpp",
                    qualified_name="pulp::signal::MultiChannelMeterT<float>", target="Pulp::signal",
                    header_fingerprint="sha256:44ef8439be936bb441790331e4bd4fc2b4e233962635ee49a88ebc4820b54772"),
        ],
        _link_probes=[
            {"role": "entrypoint", "binding": "pulp::signal::MultiChannelMeterT<float>",
             "operation": "member_call", "member": "reset", "arguments": ""},
            {"role": "loudness_support", "binding": "pulp::signal::MultiChannelMeterT<float>",
             "operation": "member_call", "member": "loudness_supported", "arguments": ""},
        ],
    ),
    capability(
        key="signal.transient-designer", domain="signal",
        summary="Zero-latency attack and sustain shaping from independent peak envelopes.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed fast and slow peak-envelope state plus contrast and gain telemetry.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite mono audio and attack, sustain, fast, and slow envelope controls",
        output_domain="transient-shaped mono audio plus contrast and gain telemetry",
        units=["samples", "milliseconds", "decibels", "normalized contrast"],
        latency="zero", tail="detector history until reset", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/transient_designer.hpp",
                         qualified_name="pulp::signal::TransientDesignerT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:f82fbba753fd01410ff7117cf86f9f8868d071a3979c36d87f677f99cbc34ae2")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::TransientDesignerT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.headphone-crossfeed", domain="signal",
        summary="Bounded delayed and low-pass-filtered stereo headphone crossfeed.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed stereo delay arrays, one-pole filter state, and derived mix controls.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite stereo audio plus amount, delay, cutoff, and enable controls",
        output_domain="speaker-like crossfed stereo audio",
        units=["samples", "milliseconds", "hertz", "linear gain", "normalized amount"],
        latency="zero direct-path algorithmic latency",
        tail="asymptotic one-pole decay while crossfeed is active",
        scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/headphone_crossfeed.hpp",
                         qualified_name="pulp::signal::HeadphoneCrossfeedT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:ed91124815b320271a1246f87fa35655cc02ef989a9caa6cab734af0a363268c")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::HeadphoneCrossfeedT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.spectral-band-mask", domain="signal",
        contract_version={"major": 1, "minor": 1},
        summary=(
            "Fixed-capacity zoomable band layouts compiled into allocation-free "
            "complex-spectrum gain masks with discrete-bin resolution reports."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control-side immutable mask compilation",
            "process": "audio spectral-frame owner",
            "reset": "none",
            "release": "none",
        },
        state_model=(
            "Fixed 64-slot finite-gain and categorical-mute layout plus a fixed "
            "8193-bin compiled gain table."
        ),
        seed_model="none",
        determinism={"repeatability": "tolerance_bounded", "block_partition": "not_applicable",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain=(
            "finite linear or logarithmic band layouts, prepared FFT geometry, and "
            "coherent channel groups of one-sided complex spectra"
        ),
        output_domain="immutable gain tables and in-place masked complex spectra",
        units=["bins", "channels", "frames", "hertz", "decibels", "linear gain"],
        latency="zero additional frame-domain latency",
        tail="none",
        scheduling="control-side table compilation then one supplied spectral frame per apply call",
        bindings=[
            binding(role="layout", kind="cpp_type",
                    include="pulp/signal/spectral_band_mask.hpp",
                    qualified_name="pulp::signal::SpectralBandLayoutT<float>",
                    target="Pulp::signal",
                    header_fingerprint="sha256:4875ea02702d6720a84c14103dd45ba7bc2f157ba43c163772e7c40011bf79d5"),
            binding(role="table", kind="cpp_type",
                    include="pulp/signal/spectral_band_mask.hpp",
                    qualified_name="pulp::signal::SpectralMaskTableT<float>",
                    target="Pulp::signal",
                    header_fingerprint="sha256:4875ea02702d6720a84c14103dd45ba7bc2f157ba43c163772e7c40011bf79d5"),
            binding(role="resolution", kind="cpp_type",
                    include="pulp/signal/spectral_band_mask.hpp",
                    qualified_name="pulp::signal::SpectralBandResolutionT<float>",
                    target="Pulp::signal",
                    header_fingerprint="sha256:4875ea02702d6720a84c14103dd45ba7bc2f157ba43c163772e7c40011bf79d5"),
            binding(role="analyze_resolution", kind="cpp_function",
                    include="pulp/signal/spectral_band_mask.hpp",
                    qualified_name="pulp::signal::analyze_spectral_band_resolution<float>",
                    target="Pulp::signal",
                    header_fingerprint="sha256:4875ea02702d6720a84c14103dd45ba7bc2f157ba43c163772e7c40011bf79d5",
                    address_expression=(
                        "static_cast<bool (*)(const pulp::signal::SpectralBandLayoutT<float>&, "
                        "int, float, pulp::signal::SpectralBandResolutionT<float>&) noexcept>("
                        "&pulp::signal::analyze_spectral_band_resolution<float>)"
                    )),
            binding(role="compile", kind="cpp_function",
                    include="pulp/signal/spectral_band_mask.hpp",
                    qualified_name="pulp::signal::build_spectral_mask<float>",
                    target="Pulp::signal",
                    header_fingerprint="sha256:4875ea02702d6720a84c14103dd45ba7bc2f157ba43c163772e7c40011bf79d5",
                    address_expression=(
                        "static_cast<bool (*)(const pulp::signal::SpectralBandLayoutT<float>&, "
                        "int, float, pulp::signal::SpectralMaskTableT<float>&) noexcept>("
                        "&pulp::signal::build_spectral_mask<float>)"
                    )),
            binding(role="apply", kind="cpp_function",
                    include="pulp/signal/spectral_band_mask.hpp",
                    qualified_name="pulp::signal::apply_spectral_mask<float>",
                    target="Pulp::signal",
                    header_fingerprint="sha256:4875ea02702d6720a84c14103dd45ba7bc2f157ba43c163772e7c40011bf79d5",
                    address_expression=(
                        "static_cast<bool (*)(std::complex<float>* const*, int, int, "
                        "const pulp::signal::SpectralMaskTableT<float>&) noexcept>("
                        "&pulp::signal::apply_spectral_mask<float>)"
                    )),
        ],
        _link_probes=[
            {"role": "layout", "binding": "pulp::signal::SpectralBandLayoutT<float>",
             "operation": "construct", "arguments": ""},
            {"role": "table", "binding": "pulp::signal::SpectralMaskTableT<float>",
             "operation": "construct", "arguments": ""},
            {"role": "resolution",
             "binding": "pulp::signal::SpectralBandResolutionT<float>",
             "operation": "construct", "arguments": ""},
            {"role": "analyze_resolution",
             "binding": "pulp::signal::analyze_spectral_band_resolution<float>",
             "operation": "function_call", "arguments": (
                 "pulp::signal::SpectralBandLayoutT<float>{}, 1024, 48000.0f, "
                 "[]() -> pulp::signal::SpectralBandResolutionT<float>& { "
                 "static pulp::signal::SpectralBandResolutionT<float> report; "
                 "return report; }()"
             )},
            {"role": "compile", "binding": "pulp::signal::build_spectral_mask<float>",
             "operation": "function_call", "arguments": (
                 "pulp::signal::SpectralBandLayoutT<float>{}, 1024, 48000.0f, "
                 "[]() -> pulp::signal::SpectralMaskTableT<float>& { "
                 "static pulp::signal::SpectralMaskTableT<float> table; return table; }()"
             )},
            {"role": "apply", "binding": "pulp::signal::apply_spectral_mask<float>",
             "operation": "function_call", "arguments": (
                 "nullptr, 0, 0, pulp::signal::SpectralMaskTableT<float>{}"
             )},
        ],
    ),
    capability(
        key="signal.spectral-mask-processor", domain="signal",
        summary=(
            "Streaming WOLA spectral masking with race-free frame-boundary publication, "
            "mask interpolation, and latency-aligned dry/wet mixing."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control; allocates bounded STFT and dry-delay storage",
            "process": "audio; control publishes through latest-value SPSC handoff",
            "reset": "audio",
            "release": "destruction off audio",
        },
        state_model=(
            "Prepared spectral frame engine, three-slot immutable mask-table handoff, "
            "fixed current/target interpolation curves, and latency-aligned dry delay."
        ),
        seed_model="none",
        determinism={"repeatability": "tolerance_bounded", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain=(
            "finite planar audio or coherent complex frame groups plus compiled "
            "spectral band layouts or mask tables"
        ),
        output_domain=(
            "phase- and channel-coherent masked audio or frames with categorical exact mute"
        ),
        units=["samples", "frames", "bins", "channels", "hertz", "decibels",
               "linear gain", "mix ratio"],
        latency="exact fixed spectral-frame engine latency",
        tail="conservative fixed engine latency plus one frame length",
        scheduling=(
            "streaming overlap-add with latest-table adoption and interpolation only "
            "at frame boundaries"
        ),
        bindings=[
            binding(
                role="entrypoint", kind="cpp_type",
                include="pulp/signal/spectral_mask_processor.hpp",
                qualified_name="pulp::signal::SpectralMaskProcessorT<float>",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:424014c924770f60d35e16b7179b552ca1b4747381734186fb15d6fb7bd94247"
                ),
            ),
            binding(
                role="config", kind="cpp_type",
                include="pulp/signal/spectral_mask_processor.hpp",
                qualified_name="pulp::signal::SpectralMaskProcessorConfigT<float>",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:424014c924770f60d35e16b7179b552ca1b4747381734186fb15d6fb7bd94247"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "entrypoint",
                "binding": "pulp::signal::SpectralMaskProcessorT<float>",
                "operation": "member_call", "member": "reset", "arguments": "",
            },
            {
                "role": "config",
                "binding": "pulp::signal::SpectralMaskProcessorConfigT<float>",
                "operation": "construct", "arguments": "",
            },
        ],
    ),
    capability(
        key="signal.spectral-gate", domain="signal",
        summary="Allocation-free scalar or per-bin gating of caller-owned complex spectra.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "none",
                   "process": "audio", "reset": "none", "release": "none"},
        state_model="Fixed scalar threshold only; spectral frames and optional curves are caller-owned.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "not_applicable",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite coherent channel groups of one-sided complex spectra and thresholds",
        output_domain="in-place gated complex spectra",
        units=["bins", "channels", "linear magnitude"],
        latency="zero frame-domain latency", tail="none", scheduling="one supplied spectral frame",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/spectral_gate_blur.hpp",
                         qualified_name="pulp::signal::SpectralGateT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:75cf322be3a308259d63737d67320b94a926c9b9fb290c6a751dd07ee781d845")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::SpectralGateT<float>",
                       "operation": "member_call", "member": "set_threshold_magnitude",
                       "arguments": "0.0f"}],
    ),
    capability(
        key="signal.spectral-frame-blur", domain="signal",
        summary="Prepared causal finite moving-average blur of complex spectral-frame magnitudes.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control; allocates bounded history",
                   "process": "audio frame owner", "reset": "audio frame owner",
                   "release": "destruction off audio"},
        state_model="Prepared bounded magnitude history, running means, counts, and retained phases.",
        seed_model="none",
        determinism={"repeatability": "tolerance_bounded", "block_partition": "not_applicable",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="prepared coherent channel groups of one-sided complex spectra",
        output_domain="in-place temporally blurred complex spectra",
        units=["bins", "channels", "frames", "linear magnitude", "bytes"],
        latency="zero additional frame-domain latency",
        tail="at most blur_frames retained analysis frames",
        scheduling="one supplied spectral frame per process call",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/spectral_gate_blur.hpp",
                         qualified_name="pulp::signal::SpectralFrameBlurT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:75cf322be3a308259d63737d67320b94a926c9b9fb290c6a751dd07ee781d845")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::SpectralFrameBlurT<float>",
                       "operation": "member_call", "member": "prepare", "arguments": "1, 1, 1"}],
    ),
    capability(
        key="signal.dynamic-eq", domain="signal",
        summary="Fixed-state threshold-driven single-band dynamic equalization.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control coefficient design",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed band-pass biquad, peak envelope, parameters, and gain telemetry.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite mono audio plus frequency, Q, threshold, range, and timing controls",
        output_domain="dynamically equalized mono audio plus detector and gain telemetry",
        units=["samples", "hertz", "Q", "decibels", "milliseconds", "normalized activity"],
        latency="zero", tail="recursive biquad and envelope decay until reset",
        scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/dynamic_eq.hpp",
                         qualified_name="pulp::signal::DynamicEqBandT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:eb6ed3b73030b8046706e6dfb8bfe9a143134b7dea59c289ce142fdcbb12254b")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::DynamicEqBandT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.spectral-morph", domain="signal",
        summary="Allocation-free magnitude and phase morphing between coherent complex spectra.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control; allocation-free geometry setup",
                   "process": "audio frame owner", "reset": "audio frame owner", "release": "none"},
        state_model="Fixed interpolation policy and prepared channel/bin geometry; spectral frames are caller-owned.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="two prepared coherent channel groups of one-sided complex spectra and normalized morph amounts",
        output_domain="caller-owned magnitude- and phase-morphed complex spectra",
        units=["bins", "channels", "linear magnitude", "radians", "normalized amount"],
        latency="zero additional frame-domain latency", tail="none",
        scheduling="one supplied paired spectral frame or disjoint bin partitions",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/spectral_morph.hpp",
                         qualified_name="pulp::signal::SpectralMorphT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:0991d2973294306182824a6d8b579007946dff976407511ce064774677c1adb3")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::SpectralMorphT<float>",
                       "operation": "member_call", "member": "prepare", "arguments": "1, 129"}],
    ),
    capability(
        key="signal.cross-feedback-multitap-delay", domain="signal",
        summary="Prepared wet-only stereo multitap delay with bounded normalized cross-feedback.",
        rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control; allocates bounded stereo delay storage",
                   "process": "audio", "reset": "audio", "release": "destruction off audio"},
        state_model="Prepared stereo delay histories, up to eight tap definitions, derived pan weights, and a bounded feedback matrix.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite stereo audio plus bounded tap, feedback, cross-feedback, pan, and width controls",
        output_domain="wet-only delayed stereo audio",
        units=["samples", "milliseconds", "linear gain", "normalized amount", "tap index"],
        latency="zero direct-path algorithmic latency",
        tail="latest tap delay without feedback; unbounded decay while a nonzero feedback route is active",
        scheduling="sample-continuous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/cross_feedback_multitap_delay.hpp",
                         qualified_name="pulp::signal::CrossFeedbackMultitapDelayT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:14184a88f394c3da92e4d39d7c8c5f9b481f5729acfafd7a0a7e5d773ee68e98")],
        _link_probes=[{"role": "entrypoint",
                       "binding": "pulp::signal::CrossFeedbackMultitapDelayT<float>",
                       "operation": "member_call", "member": "prepare",
                       "arguments": "48000.0, 100.0"}],
    ),
    capability(
        key="signal.de-esser", domain="signal",
        summary="Prepared split-band de-essing with an independent frequency-selective detector.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control coefficient design",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed detector biquad, peak envelope, Linkwitz-Riley split, configuration, modes, and gain telemetry.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite mono audio plus detector, split, threshold, range, timing, bypass, and listen controls",
        output_domain="de-essed mono or detector-listen audio plus detector and gain-reduction telemetry",
        units=["samples", "hertz", "Q", "decibels", "milliseconds"],
        latency="zero", tail="recursive detector, envelope, and crossover decay until reset",
        scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/de_esser.hpp",
                         qualified_name="pulp::signal::DeEsserT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:fd61c1c4680c790d8617ec55164367d535cc079878e2bcb9780077c59bbf040d")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::DeEsserT<float>",
                       "operation": "member_call", "member": "prepare", "arguments": "48000.0f"}],
    ),
    capability(
        key="signal.expander", domain="signal",
        summary="Prepared bounded stereo upward or downward expansion with shared dynamics telemetry.",
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control detector setup",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed two-channel envelope state, bounded curve configuration, bypass state, and signed gain telemetry.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite stereo audio plus expansion mode, threshold, ratio, range, knee, timing, detector, and link controls",
        output_domain="expanded stereo audio plus signed gain and gain-reduction telemetry",
        units=["samples", "decibels", "ratio", "milliseconds", "normalized link"],
        latency="zero", tail="none", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/expander.hpp",
                         qualified_name="pulp::signal::ExpanderT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:bf318bbe864c1ea706eed101e63f6469602a7c7c639ce86bffb650c13ef08741")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::ExpanderT<float>",
                       "operation": "member_call", "member": "prepare", "arguments": "48000.0f"}],
    ),
    capability(
        key="signal.explicit-q-resonator-bank", domain="signal",
        summary=(
            "Prepared fixed-capacity band-pass resonator bank with explicit frequency, Q, "
            "gain, envelope, and transactional transition controls."
        ),
        rt_class="mixed",
        lifecycle={"construction": "control",
                   "prepare": "control; allocates bounded retained state and stages recipes",
                   "process": "audio; control publishes through latest-value SPSC handoff",
                   "reset": "audio", "release": "destruction off audio"},
        state_model=(
            "Prepared fixed-capacity double-precision SVF and envelope state plus a "
            "three-slot recipe handoff and exact-duration transition state."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite mono audio and explicit per-band frequency, Q, gain, and ballistics",
        output_domain="summed resonator audio plus per-band pre-gain output and envelope",
        units=["samples", "frames", "hertz", "Q", "decibels", "milliseconds"],
        latency="zero",
        tail="recursive IIR decay bounded by the configured frequency and Q",
        scheduling="sample-synchronous with recipe adoption at sample boundaries",
        bindings=[binding(
            role="entrypoint", kind="cpp_type",
            include="pulp/signal/explicit_q_resonator_bank.hpp",
            qualified_name="pulp::signal::ExplicitQResonatorBankT<float>",
            target="Pulp::signal",
            header_fingerprint="sha256:1504f5c4bc3752754bc7591e6a6509a9aeb06df018417749bd149802463917da",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::signal::ExplicitQResonatorBankT<float>",
            "operation": "member_call", "member": "reset", "arguments": "",
        }],
    ),
    capability(
        key="signal.spectral-delay-matrix", domain="signal",
        summary=(
            "Prepared per-bin spectral delay and attenuation with bounded history and "
            "race-free frame-boundary table publication."
        ),
        rt_class="mixed",
        lifecycle={"construction": "control",
                   "prepare": "control; allocates bounded frame history and compiles tables",
                   "process": "audio; control publishes through latest-value SPSC handoff",
                   "reset": "audio; reset or history-only purge",
                   "release": "destruction off audio"},
        state_model=(
            "Prepared spectral frame engine, coherent per-channel complex-bin history, and "
            "three-slot delay/attenuation table handoff."
        ),
        seed_model="none",
        determinism={"repeatability": "tolerance_bounded", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="finite planar audio and normalized-frequency delay/attenuation breakpoints",
        output_domain="coherent planar audio with frame-quantized per-bin content delay",
        units=["samples", "frames", "bins", "milliseconds", "decibels", "linear gain"],
        latency="fixed spectral-frame engine latency; content delay is D times analysis hop",
        tail="finite engine latency plus the prepared maximum content-delay history",
        scheduling="streaming overlap-add with table adoption only at frame boundaries",
        bindings=[binding(
            role="entrypoint", kind="cpp_type", include="pulp/signal/spectral_delay_matrix.hpp",
            qualified_name="pulp::signal::SpectralDelayMatrixT<float>", target="Pulp::signal",
            header_fingerprint="sha256:53ed4a5125e74d86fc8e4f8980a6309cbf00796d85f8f5b7cb9f1a06acbe0034",
        )],
        _link_probes=[{
            "role": "entrypoint", "binding": "pulp::signal::SpectralDelayMatrixT<float>",
            "operation": "member_call", "member": "reset", "arguments": "",
        }],
    ),
    capability(
        key="signal.early-reflections", domain="signal",
        summary=(
            "Bounded feed-forward early-reflection tap bank producing a stereo reflection pattern "
            "from validated per-tap delay, gain, and pan."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control delay-line allocation",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model=(
            "Fixed-capacity delay history plus the committed tap list; feed-forward only, so no "
            "recursive state accumulates."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus a bounded tap list of delay, gain, and pan",
        output_domain="stereo early-reflection pattern",
        units=["samples", "milliseconds", "linear gain"],
        latency="per-tap delay", tail="longest configured tap",
        scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/early_reflections.hpp",
                         qualified_name="pulp::signal::EarlyReflectionsT<float>",
                         target="Pulp::signal", header_fingerprint="sha256:4e2c96110a97d359d96e1c3fbcd8f369024d8351579298729471f9ff4ebcd4d4")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::EarlyReflectionsT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.auto-ducked-send", domain="signal",
        summary=(
            "Envelope-following send that ducks a wet path against a dry key signal with bounded "
            "attack, release, threshold, and range."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control envelope setup",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Single envelope follower plus the committed duck configuration.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="wet and dry audio plus bounded threshold, range, attack, and release",
        output_domain="ducked stereo send",
        units=["samples", "decibels", "milliseconds"],
        latency="zero", tail="envelope release", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/auto_ducked_send.hpp",
                         qualified_name="pulp::signal::AutoDuckedSendT<float>",
                         target="Pulp::signal", header_fingerprint="sha256:8246383c5cfe77391c4a4d609112462ef92917bbd4d9aab6e09ac5dab4e584fb")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::AutoDuckedSendT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.comb-filter", domain="signal",
        summary=(
            "Prepared feedforward, feedback, and Schroeder allpass comb filters with exact integer "
            "delays, transactional configuration, and typed fault recovery."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control delay-line allocation",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model=(
            "Fixed-capacity fractional delay history plus the committed mode/delay/gain "
            "configuration; recursive modes snap their stored state to zero."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus a bounded mode, integer delay, and stable feedback gain",
        output_domain="comb-filtered audio with typed status",
        units=["samples", "linear gain"],
        latency="zero", tail="recursive decay in feedback and allpass modes",
        scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/comb_filter.hpp",
                         qualified_name="pulp::signal::CombFilterT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:6f5963276c7fc2f68985dba678754f7afc4216b2486f3f4c356b3344eeceda4c")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::CombFilterT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.filter-morph", domain="signal",
        summary=(
            "Continuous morph across lowpass, bandpass, highpass, and notch responses with a stable "
            "coefficient path and no discontinuity at type boundaries."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control coefficient design",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model="Biquad state plus the current morph position, cutoff, and resonance.",
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus a bounded morph position, cutoff, and resonance",
        output_domain="filtered audio",
        units=["samples", "hertz", "normalized morph position", "Q"],
        latency="zero", tail="recursive IIR decay", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/filter_morph.hpp",
                         qualified_name="pulp::signal::FilterMorphT<float>", target="Pulp::signal",
                         header_fingerprint="sha256:9065017c36f2451be9b3143efbb63650c2d70e412f197ae6fa2f3ad6871809b3")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::FilterMorphT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.formant-filter-bank", domain="signal",
        summary=(
            "Bounded parallel formant resonator bank with vowel presets and transactional "
            "reconfiguration for vocal-character shaping."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control resonator allocation",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model=(
            "Up to MaxFormants parallel resonator states plus the committed formant recipe; "
            "configuration is rejected atomically rather than partially applied."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus bounded formant center, bandwidth, and gain specifications",
        output_domain="formant-shaped audio with typed configure status",
        units=["samples", "hertz", "decibels"],
        latency="zero", tail="recursive resonator decay", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/formant_filter_bank.hpp",
                         qualified_name="pulp::signal::FormantFilterBankT<float>",
                         target="Pulp::signal",
                         header_fingerprint="sha256:3fe22c92f7793b10a219c1270d0b43d143cc025b7d9769566a98c29dd4fe62f5")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::FormantFilterBankT<float>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.graphic-eq", domain="signal",
        summary=(
            "Bounded multi-band graphic equalizer container with transactional band commits and an "
            "optional sample-counted crossfade between configurations."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control band allocation",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model=(
            "Up to MaxBands biquad sections plus an optional transition bank; a rejected "
            "configuration leaves the committed bank untouched."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus bounded per-band frequency, gain, and Q controls",
        output_domain="equalized audio with typed prepare and configure status",
        units=["samples", "hertz", "decibels", "Q"],
        latency="zero", tail="recursive IIR decay", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/graphic_eq.hpp",
                         qualified_name="pulp::signal::GraphicEqT<float, 31>", target="Pulp::signal",
                         header_fingerprint="sha256:63dafa17e4e4650647680a41b29ab3db78435aecf2136adc0195c827102d4d25")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::GraphicEqT<float, 31>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.tilt-eq", domain="signal",
        summary=(
            "Multi-channel tilt equalizer trading low- against high-shelf gain around a pivot "
            "frequency from a single control."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control coefficient design",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model=(
            "Per-channel shelf cascade state plus the committed pivot and tilt; set_config "
            "reinstalls coefficients and clears history rather than crossfading."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus a bounded pivot frequency and signed tilt amount",
        output_domain="tilt-equalized audio",
        units=["samples", "hertz", "decibels"],
        latency="zero", tail="recursive IIR decay", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type", include="pulp/signal/tilt_eq.hpp",
                         qualified_name="pulp::signal::TiltEqT<float, 2>", target="Pulp::signal",
                         header_fingerprint="sha256:ed676f18106a4465246265aeaebe138bc28c75ef197db2083b95e934418efb3d")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::TiltEqT<float, 2>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
    capability(
        key="signal.transfer-curve", domain="signal",
        summary=(
            "Bounded monotonic transfer-curve shaper prepared from validated breakpoints, with "
            "typed rejection of non-monotonic or out-of-range input."
        ),
        rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control curve preparation",
                   "process": "audio", "reset": "audio", "release": "none"},
        state_model=(
            "A prepared immutable curve table plus the committed breakpoint set; preparation is "
            "transactional so a rejected curve leaves the previous one in place."
        ),
        seed_model="none",
        determinism={"repeatability": "bit_exact", "block_partition": "invariant",
                     "platform_scope": "same_build", "transport_history": "irrelevant"},
        input_domain="audio plus a bounded monotonic breakpoint list",
        output_domain="shaped audio with typed prepare status",
        units=["samples", "linear amplitude"],
        latency="zero", tail="none", scheduling="sample-synchronous",
        bindings=[binding(role="entrypoint", kind="cpp_type",
                         include="pulp/signal/transfer_curve.hpp",
                         qualified_name="pulp::signal::TransferCurveT<float, 32>",
                         target="Pulp::signal", header_fingerprint="sha256:f9210a9d4124c63aae16c0b0d27ac9125a8f3c42c5e34f3c1b58dbc492487632")],
        _link_probes=[{"role": "entrypoint", "binding": "pulp::signal::TransferCurveT<float, 32>",
                       "operation": "member_call", "member": "reset", "arguments": ""}],
    ),
]
