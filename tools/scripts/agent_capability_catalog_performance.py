"""Declarative installed agent capability records."""
from agent_capability_registry_types import binding, capability

EXPORTS = [
    capability(
        key="audio.wavetable-authoring",
        domain="audio",
        summary=(
            "Bounded offline compilation of one recorded mono cycle into an owned "
            "band-limited stack for the existing Wavetable oscillator."
        ),
        rt_class="control",
        lifecycle={
            "construction": "none",
            "prepare": "control or worker; allocates bounded analysis and table storage",
            "process": "not applicable; returns materialized bands",
            "reset": "none",
            "release": "result destruction off audio",
        },
        state_model=(
            "Pure compilation result carrying an owned ordered band stack, resolved cycle, "
            "seam diagnostics, recipe, provenance, and canonical source/table digests."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain=(
            "finite bounded mono PCM, source sample rate, explicit or automatic cycle policy, "
            "mip recipe, and caller provenance"
        ),
        output_domain=(
            "owned WavetableEntry band stack or explicit fail-closed compile status"
        ),
        units=["samples", "frames", "hertz", "decibels", "linear amplitude"],
        latency="not applicable; offline compiler",
        tail="none",
        scheduling="one bounded control-thread compilation transaction",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_function",
            include="pulp/audio/wavetable_authoring.hpp",
            qualified_name="pulp::audio::compile_wavetable",
            target="Pulp::audio",
            header_fingerprint="sha256:52df577e59972f830983062f5c69b9c2f3796597547eb6e6c4fdc011a1951905",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::compile_wavetable",
            "operation": "function_call",
            "arguments": "pulp::audio::BufferView<const float>{}, 48000.0",
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
        key="audio.voice-modulation-sources",
        domain="audio",
        summary=(
            "Prepared fixed-memory bank of per-voice LFO and AHDSR envelope sources with "
            "free-running or retriggered phase policy and per-voice unison phase spread, "
            "feeding constant or audio-rate voice modulation lanes."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "none",
            "prepare": "control: fixed scratch and per-voice sources",
            "process": "audio",
            "reset": "audio: every voice to a fresh start",
            "release": "control",
        },
        state_model=(
            "The bank owns per-voice LFO phase/lifecycle and envelope stage state; reset() is "
            "the only reseeding point. Lane publication is delegated to the caller's "
            "VoiceModulationBuffer; constant-rate lanes publish the first sample of the block."
        ),
        seed_model="public uint32 base seed; voice i derives seed + i",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "fixed_partition_only",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="voice lifecycle events, source configuration, and lane target/rate selection",
        output_domain="constant or audio-rate voice modulation lanes",
        units=["hertz", "frames", "voice index"],
        latency="zero",
        tail="envelope release tail owned by the caller's lane consumer",
        scheduling="per voice per audio block",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/audio/voice_modulation_sources.hpp",
            qualified_name="pulp::audio::VoiceModulationSources<4>",
            target="Pulp::audio",
            header_fingerprint="sha256:b5c08f132df70323065b1d7c1d9109ddfc95ed8baeacb0c595fd5eb21771dbe8",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::audio::VoiceModulationSources<4>",
            "operation": "member_call",
            "member": "prepared",
            "arguments": "",
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
            header_fingerprint="sha256:96ba2417d37bd1ff46876463d61f53fb7345b5aa88c92c9c05f04339e37d4b25",
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
        key="midi.linear-step-player",
        domain="midi",
        summary=(
            "Bounded sample-accurate linear multi-lane step player with per-step gate, pitch "
            "offset, velocity, probability, ratchet, tie/slide, and per-lane direction modes."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and ledgers on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "Fixed lane, sounding-note, and release ledgers retain per-lane playheads and "
            "choke resolution without allocation."
        ),
        seed_model="explicit spec random_seed indexed by grid coordinate",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="lane specifications, step grid, absolute sample block, tempo map, and transport event",
        output_domain="owned note-on and note-off MIDI event stream",
        units=["ticks", "samples", "MIDI note", "velocity", "percent", "semitones"],
        latency="sample-scheduled within the supplied block",
        tail="owned note releases and bounded release debt",
        scheduling="absolute-sample transport-aware",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/step_player.hpp",
            qualified_name="pulp::midi::StepPlayer<>",
            target="Pulp::midi",
            header_fingerprint="sha256:840cc4783c205d1e0416d1115c74a1a2ac3dd15fbea9185cdcbfb679a9898cd7",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::StepPlayer<>",
            "operation": "member_call",
            "member": "valid",
            "arguments": "",
        }],
    ),
    capability(
        key="midi.chord-memory",
        domain="midi",
        summary=(
            "Bounded chord memory that captures a chord as an interval shape and replays it from "
            "single notes in parallel, per-key, and scale-degree modes, optionally revoiced by "
            "minimum-motion voice leading."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and ledgers on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "Fixed capture slots and a bounded active-trigger table retain each sounding chord's "
            "owned notes without allocation."
        ),
        seed_model="none; voicing selection is deterministic",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="captured interval shapes and a MIDI event stream",
        output_domain="owned note-on and note-off MIDI event stream",
        units=["MIDI note", "semitones", "velocity", "scale degree"],
        latency="none; chords are emitted at the trigger's own sample offset",
        tail="owned chord releases",
        scheduling="input-offset",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/chord_memory.hpp",
            qualified_name="pulp::midi::ChordMemory<>",
            target="Pulp::midi",
            header_fingerprint="sha256:3bce0cc159d68496cf145ad1fb8cd26415f0c7ba18446d5d3618d6cd7bede81b",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::ChordMemory<>",
            "operation": "member_call",
            "member": "valid",
            "arguments": "",
        }],
    ),
    capability(
        key="midi.humanize",
        domain="midi",
        summary=(
            "Seeded timing and velocity jitter over note attacks, with forward-only timing so the "
            "kernel stays causal and its latency equals the declared bound."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and ledgers on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "One pending attack per key in a fixed key-space table; no allocation on any path."
        ),
        seed_model="explicit spec seed indexed by the event's absolute sample coordinate",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="MIDI event stream with absolute block position",
        output_domain="timing- and velocity-jittered MIDI event stream",
        units=["samples", "MIDI note", "velocity"],
        latency="up to the declared timing jitter bound",
        tail="pending attacks released on flush",
        scheduling="absolute-sample",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/humanize.hpp",
            qualified_name="pulp::midi::Humanize<>",
            target="Pulp::midi",
            header_fingerprint="sha256:eb1b342c0dc35280d028d649d911262c8f2249f3c35d64badc58ad966408c02e",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::Humanize<>",
            "operation": "member_call",
            "member": "valid",
            "arguments": "",
        }],
    ),
    capability(
        key="midi.latch",
        domain="midi",
        summary=(
            "Hold and toggle note retention with depth-counted balanced releases, so repeated "
            "attacks on one key never leak or double-release."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and ledgers on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "Retention and physical-key depth counters over the whole 16x128 key space, so retention "
            "cannot overflow and needs no allocation."
        ),
        seed_model="none; the kernel is fully deterministic",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="MIDI event stream",
        output_domain="retained note-on and note-off MIDI event stream",
        units=["MIDI note", "velocity"],
        latency="none; events are emitted at their own sample offset",
        tail="retained notes released on flush",
        scheduling="input-offset",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/latch.hpp",
            qualified_name="pulp::midi::Latch",
            target="Pulp::midi",
            header_fingerprint="sha256:b690accb10c2278d4fe41bd1f886f831a9f9aacd11cf78a5a7d7e1bc76266cb6",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::Latch",
            "operation": "member_call",
            "member": "valid",
            "arguments": "",
        }],
    ),
    capability(
        key="midi.note-delay",
        domain="midi",
        summary=(
            "Bounded MIDI note delay on a tempo division or millisecond clock, with velocity decay "
            "and cumulative per-repeat transposition. The dry note passes through, so it is a send."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and ledgers on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "A fixed scheduled-note queue plus a per-key armed-source table; an echo's length is "
            "rolled back once the authored release reveals how long the source note was held."
        ),
        seed_model="none; echo placement is deterministic",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="MIDI event stream, absolute sample block, and tempo map or constant tempo",
        output_domain="dry MIDI event stream plus owned echo note-on and note-off events",
        units=["ticks", "samples", "milliseconds", "MIDI note", "velocity", "semitones", "percent"],
        latency="sample-scheduled at the authored delay",
        tail="owned echo releases",
        scheduling="absolute-sample transport-aware",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/note_delay.hpp",
            qualified_name="pulp::midi::NoteDelay<>",
            target="Pulp::midi",
            header_fingerprint="sha256:c3cdd5ca2c4b7d82efbb46f2740d42a4987796403cdb950ba2c96dc9bca3c731",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::NoteDelay<>",
            "operation": "member_call",
            "member": "valid",
            "arguments": "",
        }],
    ),
    capability(
        key="midi.note-repeat",
        domain="midi",
        summary=(
            "Bounded clock-divided note repeat with per-hit probability, velocity decay, and gate. "
            "Releasing the key cancels hits that have not started while a sounding hit keeps its gate."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and ledgers on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "A fixed scheduled-note queue where each slot owns one note's whole lifecycle, so "
            "cancelling an unstarted hit discards its attack and release together."
        ),
        seed_model="explicit spec seed indexed by the attack coordinate and hit index",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="MIDI event stream, absolute sample block, and tempo map or constant tempo",
        output_domain="owned note-on and note-off MIDI event stream",
        units=["ticks", "samples", "MIDI note", "velocity", "percent"],
        latency="sample-scheduled within the repeat series",
        tail="owned repeat releases",
        scheduling="absolute-sample transport-aware",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/note_repeat.hpp",
            qualified_name="pulp::midi::NoteRepeat<>",
            target="Pulp::midi",
            header_fingerprint="sha256:8c00626a60e2ac1813d8ac117f0ce13e0931040107c8383b9265038d932ef763",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::NoteRepeat<>",
            "operation": "member_call",
            "member": "valid",
            "arguments": "",
        }],
    ),
    capability(
        key="midi.strum-spread",
        domain="midi",
        summary=(
            "Bounded strum and spread over a near-simultaneous cluster, with direction orders, an "
            "integer shape curve, division or millisecond spacing, and seeded jitter."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "reserve output and ledgers on control",
            "process": "audio",
            "reset": "audio with prepared output",
            "release": "none",
        },
        state_model=(
            "A fixed cluster buffer holds the notes of one chord until its window closes, which is "
            "the first moment the cluster is known to be complete."
        ),
        seed_model="explicit spec seed indexed by the note's arrival coordinate",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="MIDI event stream, absolute sample block, and tempo map or constant tempo",
        output_domain="time-spread MIDI event stream",
        units=["ticks", "samples", "milliseconds", "MIDI note", "velocity"],
        latency="one cluster window",
        tail="buffered cluster notes released on flush",
        scheduling="absolute-sample transport-aware",
        bindings=[binding(
            role="entrypoint",
            kind="cpp_type",
            include="pulp/midi/strum.hpp",
            qualified_name="pulp::midi::Strum<>",
            target="Pulp::midi",
            header_fingerprint="sha256:71b3bfba299503c2dfff38864f7fc8aede43d7e2f3cfb6474e5cfdebb4a0bf67",
        )],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::midi::Strum<>",
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
            header_fingerprint="sha256:b37237cdb793ec5b300da4bb4c0850be8dab0c2bb24b243b45c554af6929a64a",
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
            header_fingerprint="sha256:b37237cdb793ec5b300da4bb4c0850be8dab0c2bb24b243b45c554af6929a64a",
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
            header_fingerprint="sha256:b37237cdb793ec5b300da4bb4c0850be8dab0c2bb24b243b45c554af6929a64a",
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
        key="music.pattern-development",
        domain="music",
        summary=(
            "Fixed-capacity exact-tick event-set development with stable IDs, nested density, "
            "regional fills, and deterministic integer A/B morphing."
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
            "Pure operations over trivially copyable fixed-capacity event records; no hidden "
            "state, allocation, scheduler, clock, or note ownership."
        ),
        seed_model=(
            "explicit seed plus exact tick, lane, cycle, stream, and stable event ID "
            "coordinates"
        ),
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain=(
            "up to 64 canonical exact-tick event records per input, onset-set operation, "
            "target count, half-open tick region, seed coordinates, and integer morph amount"
        ),
        output_domain="up to 64 canonical exact-tick event records or an explicit error",
        units=["ticks", "event count", "integer accent", "integer morph amount", "seed"],
        latency="zero",
        tail="none",
        scheduling="pure caller-clocked whole-pattern development",
        bindings=[
            binding(
                role="event-id", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::PatternEventId", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="event-role", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::PatternEventRole", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="event-record", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::PatternEvent", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="error", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::PatternDevelopmentError", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="pattern", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::DevelopmentPattern<>", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="result", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::PatternDevelopmentResult<>", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="set-operation", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::PatternSetOperation", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="density-selection", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::DensitySelection", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="regional-fill-selection", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::RegionalFillSelection", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="morph-selection", kind="cpp_type",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::PatternMorphSelection", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="make-event-id", kind="cpp_function",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::make_pattern_event_id", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="set-algebra", kind="cpp_function",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::pattern_set<64>", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="density", kind="cpp_function",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::select_pattern_density<64>", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="regional-fill", kind="cpp_function",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::apply_regional_fill<64>", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
            binding(
                role="morph", kind="cpp_function",
                include="pulp/music/pattern_development.hpp",
                qualified_name="pulp::music::morph_patterns<64>", target="Pulp::music",
                header_fingerprint="sha256:00e65ace2fac744dbca18cb278907cef86832ea5f4b97dbdabbe53004dedb0c5",
            ),
        ],
        _link_probes=[
            {"role": "event-id", "binding": "pulp::music::PatternEventId",
             "operation": "construct", "arguments": "1"},
            {"role": "event-role", "binding": "pulp::music::PatternEventRole",
             "operation": "construct", "arguments": "pulp::music::PatternEventRole::anchor"},
            {"role": "event-record", "binding": "pulp::music::PatternEvent",
             "operation": "construct", "arguments": "1, pulp::timebase::TickPosition{0}, 1000, pulp::music::PatternEventRole::anchor"},
            {"role": "error", "binding": "pulp::music::PatternDevelopmentError",
             "operation": "construct", "arguments": "pulp::music::PatternDevelopmentError::none"},
            {"role": "pattern", "binding": "pulp::music::DevelopmentPattern<>",
             "operation": "member_call", "member": "size", "arguments": ""},
            {"role": "result", "binding": "pulp::music::PatternDevelopmentResult<>",
             "operation": "construct", "arguments": ""},
            {"role": "set-operation", "binding": "pulp::music::PatternSetOperation",
             "operation": "construct", "arguments": "pulp::music::PatternSetOperation::set_union"},
            {"role": "density-selection", "binding": "pulp::music::DensitySelection",
             "operation": "construct", "arguments": ""},
            {"role": "regional-fill-selection", "binding": "pulp::music::RegionalFillSelection",
             "operation": "construct", "arguments": "pulp::timebase::TickPosition{0}, pulp::timebase::TickPosition{1}, 0, 0, pulp::timebase::RandomCoordinate{}"},
            {"role": "morph-selection", "binding": "pulp::music::PatternMorphSelection",
             "operation": "construct", "arguments": ""},
            {"role": "make-event-id", "binding": "pulp::music::make_pattern_event_id",
             "operation": "function_call", "arguments": "0, pulp::timebase::RandomCoordinate{}"},
            {"role": "set-algebra", "binding": "pulp::music::pattern_set<64>",
             "operation": "function_call", "arguments": "pulp::music::DevelopmentPattern<>{}, pulp::music::DevelopmentPattern<>{}, pulp::music::PatternSetOperation::set_union"},
            {"role": "density", "binding": "pulp::music::select_pattern_density<64>",
             "operation": "function_call", "arguments": "pulp::music::DevelopmentPattern<>{}, pulp::music::DensitySelection{}"},
            {"role": "regional-fill", "binding": "pulp::music::apply_regional_fill<64>",
             "operation": "function_call", "arguments": "pulp::music::DevelopmentPattern<>{}, pulp::music::DevelopmentPattern<>{}, pulp::music::RegionalFillSelection{{0}, {1}, 0, 0, {}}"},
            {"role": "morph", "binding": "pulp::music::morph_patterns<64>",
             "operation": "function_call", "arguments": "pulp::music::DevelopmentPattern<>{}, pulp::music::DevelopmentPattern<>{}, pulp::music::PatternMorphSelection{}"},
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
