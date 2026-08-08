"""Declarative installed agent capability records."""
from agent_capability_registry_types import binding, capability

EXPORTS = [
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
            header_fingerprint="sha256:7ebb777b9c33fbf66871da573e6a1c4c5bdd7809a03cec01cbc27fcb5a1d7278",
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
]
