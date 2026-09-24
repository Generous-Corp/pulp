#pragma once

// Generic LV2 entry point generator
// Plugin developers include this and call PULP_LV2_PLUGIN() with their factory function.
// All LV2 boilerplate (descriptor, instantiate, run, cleanup) is generated automatically.
//
// Usage (in one .cpp file per plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/lv2_entry.hpp>
//   PULP_LV2_PLUGIN(my_namespace::create_my_processor, "http://pulp.audio/plugins/my_plugin")

#include <pulp/format/adapter_boundary.hpp>
#include <pulp/format/lv2_adapter.hpp>
#include <pulp/format/max_block_contract.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

namespace pulp::format::lv2_generic {

// Populated at static init by PULP_LV2_PLUGIN
inline ProcessorFactory g_factory = nullptr;
inline const char* g_uri = nullptr;

// Worst-case MIDI events per block the RT MIDI buffers are sized to. Matches
// the CLAP adapter's realtime MIDI capacity so behavior is uniform; add()
// beyond this drops (set_realtime_capacity_limit) rather than allocating.
inline constexpr std::size_t kRealtimeMidiEventCapacity =
    state::ParameterEventQueue::kCapacity;

// Keep the advertised atom-port capacity ahead of what a full block of MIDI
// actually needs, so raising kRealtimeMidiEventCapacity fails here rather than
// silently truncating a host-allocated sequence at run time.
static_assert(lv2_adapter::kAtomPortMinimumSize >=
                  sizeof(LV2_Atom_Sequence) +
                      kRealtimeMidiEventCapacity * (sizeof(LV2_Atom_Event) + 8),
              "lv2:minimumSize must cover a full block of outgoing MIDI");

// ── time:Position decode ─────────────────────────────────────────────────

/// Read a numeric atom body as a double, whatever integer or float type the
/// host chose for it. Returns false for a type this adapter does not model, in
/// which case the property is skipped rather than guessed at.
inline bool read_numeric_atom(const LV2_Atom* atom, const lv2_adapter::Lv2TimeUrids& urids,
                              double& out) noexcept {
    if (!atom)
        return false;
    const void* body = LV2_ATOM_BODY_CONST(atom);
    if (urids.atom_float != 0 && atom->type == urids.atom_float && atom->size == sizeof(float)) {
        float v = 0.0f;
        std::memcpy(&v, body, sizeof(v));
        out = static_cast<double>(v);
        return true;
    }
    if (urids.atom_double != 0 && atom->type == urids.atom_double && atom->size == sizeof(double)) {
        double v = 0.0;
        std::memcpy(&v, body, sizeof(v));
        out = v;
        return true;
    }
    if (urids.atom_int != 0 && atom->type == urids.atom_int && atom->size == sizeof(int32_t)) {
        int32_t v = 0;
        std::memcpy(&v, body, sizeof(v));
        out = static_cast<double>(v);
        return true;
    }
    if (urids.atom_long != 0 && atom->type == urids.atom_long && atom->size == sizeof(int64_t)) {
        int64_t v = 0;
        std::memcpy(&v, body, sizeof(v));
        out = static_cast<double>(v);
        return true;
    }
    if (urids.atom_bool != 0 && atom->type == urids.atom_bool && atom->size == sizeof(int32_t)) {
        int32_t v = 0;
        std::memcpy(&v, body, sizeof(v));
        out = v != 0 ? 1.0 : 0.0;
        return true;
    }
    return false;
}

/// Decode a `time:Position` object into @p out, overwriting only the
/// properties the host actually sent. Returns false when @p obj is not a
/// time:Position, leaving @p out untouched.
///
/// Allocation-free: the whole walk is over the host's buffer.
inline bool decode_time_position(const LV2_Atom_Object* obj, const lv2_adapter::Lv2TimeUrids& urids,
                                 lv2_adapter::Lv2TimePosition& out) noexcept {
    if (!obj || urids.time_position == 0)
        return false;
    if (obj->body.otype != urids.time_position)
        return false;

    LV2_ATOM_OBJECT_FOREACH(obj, prop) {
        const LV2_Atom* value = &prop->value;
        double numeric = 0.0;
        if (!read_numeric_atom(value, urids, numeric))
            continue;
        const LV2_URID key = prop->key;
        if (urids.time_speed != 0 && key == urids.time_speed) {
            out.speed = static_cast<float>(numeric);
            out.has_speed = true;
        } else if (urids.time_frame != 0 && key == urids.time_frame) {
            out.frame = static_cast<int64_t>(numeric);
            out.has_frame = true;
        } else if (urids.time_bar != 0 && key == urids.time_bar) {
            out.bar = static_cast<int64_t>(numeric);
            out.has_bar = true;
        } else if (urids.time_beat != 0 && key == urids.time_beat) {
            out.beat = numeric;
            out.has_beat = true;
        } else if (urids.time_beats_per_bar != 0 && key == urids.time_beats_per_bar) {
            out.beats_per_bar = static_cast<float>(numeric);
            out.has_beats_per_bar = true;
        } else if (urids.time_beat_unit != 0 && key == urids.time_beat_unit) {
            out.beat_unit = static_cast<int32_t>(numeric);
            out.has_beat_unit = true;
        } else if (urids.time_beats_per_minute != 0 && key == urids.time_beats_per_minute) {
            out.beats_per_minute = static_cast<float>(numeric);
            out.has_beats_per_minute = true;
        }
    }
    return true;
}

/// Advance a latched transport by one block of @p n_samples while it is
/// rolling, so the position keeps moving between the Positions a host chooses
/// to send. A stopped transport is left where it is.
inline void advance_time_position(lv2_adapter::Lv2TimePosition& pos, uint32_t n_samples,
                                  double sample_rate) noexcept {
    if (!pos.has_speed || pos.speed == 0.0f)
        return;
    if (n_samples == 0)
        return;
    const double frames = static_cast<double>(n_samples) * static_cast<double>(pos.speed);
    if (pos.has_frame) {
        pos.frame += static_cast<int64_t>(frames);
    }
    if (pos.has_beat && pos.has_beats_per_minute && sample_rate > 0.0 &&
        pos.beats_per_minute > 0.0f) {
        pos.beat += (frames / sample_rate) * (static_cast<double>(pos.beats_per_minute) / 60.0);
    }
}

/// Project a decoded `time:Position` onto the neutral transport every adapter
/// hands the shared ProcessContext mapper.
///
/// Unit note: LV2 counts `time:beat` in beats of `time:beatUnit`, while
/// ProcessContext counts quarter notes, so a 6/8 host's beats are scaled by
/// 4 / beatUnit. Fields LV2's time extension does not model stay invalid:
/// there is no record-arm, no cycle range, and no host clock. `framesPerSecond`
/// is the audio sample rate, NOT an SMPTE rate, so `frame_rate` stays unknown.
inline boundary::HostTransport to_host_transport(const lv2_adapter::Lv2TimePosition& pos) noexcept {
    boundary::HostTransport transport;
    if (pos.has_speed) {
        transport.is_playing = pos.speed != 0.0f;
        transport.validity.set(TransportField::Playing);
    }
    if (pos.has_beats_per_minute) {
        transport.tempo_bpm = static_cast<double>(pos.beats_per_minute);
        transport.validity.set(TransportField::Tempo);
    }
    if (pos.has_frame) {
        transport.position_samples = pos.frame;
        transport.validity.set(TransportField::SamplePosition);
    }
    const bool time_sig =
        pos.has_beats_per_bar && pos.has_beat_unit && pos.beats_per_bar > 0.0f && pos.beat_unit > 0;
    if (time_sig) {
        transport.time_sig_numerator = static_cast<int>(pos.beats_per_bar);
        transport.time_sig_denominator = static_cast<int>(pos.beat_unit);
        transport.validity.set(TransportField::TimeSignature);
    }
    if (pos.has_beat) {
        const double quarter_notes_per_beat = (pos.has_beat_unit && pos.beat_unit > 0)
                                                  ? 4.0 / static_cast<double>(pos.beat_unit)
                                                  : 1.0;
        transport.position_beats = pos.beat * quarter_notes_per_beat;
        transport.validity.set(TransportField::BeatPosition);
    }
    if (pos.has_bar) {
        transport.host_bar = pos.bar;
        transport.validity.set(TransportField::Bar);
    }
    return transport;
}

// ── LV2 Callbacks ────────────────────────────────────────────────────────

inline LV2_Handle instantiate(
    const LV2_Descriptor*,
    double sample_rate,
    const char*,
    const LV2_Feature* const* features)
{
    if (!g_factory) return nullptr;

    // Resolve LV2_URID_Map, required by any real LV2 host. Fail instantiation
    // loudly and return nullptr when the feature is absent so the host surfaces
    // a clear error.
    LV2_URID_Map* urid_map = lv2_adapter::find_urid_map(features);
    if (!urid_map) {
        pulp::runtime::log_warn(
            "lv2: host did not provide LV2_URID__map; refusing to instantiate");
        return nullptr;
    }

    auto* inst = new lv2_adapter::PulpLv2Instance();
    inst->factory = g_factory;
    inst->sample_rate = sample_rate;
    inst->urid_map = urid_map;
    inst->urid_midi_event = urid_map->map(urid_map->handle, LV2_MIDI__MidiEvent);
    inst->urid_atom_sequence = urid_map->map(urid_map->handle, LV2_ATOM__Sequence);
    inst->urid_atom_chunk = urid_map->map(urid_map->handle, LV2_ATOM__Chunk);
    inst->urid_state_blob = urid_map->map(urid_map->handle, lv2_adapter::kStateBlobUri);

    auto& time_urids = inst->time_urids;
    time_urids.atom_object = urid_map->map(urid_map->handle, LV2_ATOM__Object);
    time_urids.atom_blank = urid_map->map(urid_map->handle, LV2_ATOM__Blank);
    time_urids.atom_float = urid_map->map(urid_map->handle, LV2_ATOM__Float);
    time_urids.atom_double = urid_map->map(urid_map->handle, LV2_ATOM__Double);
    time_urids.atom_int = urid_map->map(urid_map->handle, LV2_ATOM__Int);
    time_urids.atom_long = urid_map->map(urid_map->handle, LV2_ATOM__Long);
    time_urids.atom_bool = urid_map->map(urid_map->handle, LV2_ATOM__Bool);
    time_urids.time_position = urid_map->map(urid_map->handle, LV2_TIME__Position);
    time_urids.time_bar = urid_map->map(urid_map->handle, LV2_TIME__bar);
    time_urids.time_beat = urid_map->map(urid_map->handle, LV2_TIME__beat);
    time_urids.time_beat_unit = urid_map->map(urid_map->handle, LV2_TIME__beatUnit);
    time_urids.time_beats_per_bar = urid_map->map(urid_map->handle, LV2_TIME__beatsPerBar);
    time_urids.time_beats_per_minute = urid_map->map(urid_map->handle, LV2_TIME__beatsPerMinute);
    time_urids.time_frame = urid_map->map(urid_map->handle, LV2_TIME__frame);
    time_urids.time_speed = urid_map->map(urid_map->handle, LV2_TIME__speed);

    // Block ceiling. LV2 has no argument for it; a host that supports
    // bufsz:boundedBlockLength publishes bufsz:maxBlockLength through the
    // options feature instead. Without it, fall back to the documented floor —
    // Ardour and JACK allow blocks well past it, which is exactly the case
    // run()'s clamp exists to survive.
    inst->max_block_length = lv2_adapter::max_block_length_from_options(
        lv2_adapter::find_options(features),
        urid_map->map(urid_map->handle, LV2_BUF_SIZE__maxBlockLength), time_urids.atom_int,
        time_urids.atom_long, lv2_adapter::kDefaultMaxBlockLength);
    inst->processor = g_factory();
    if (!inst->processor) {
        delete inst;
        return nullptr;
    }
    inst->processor->set_state_store(&inst->store);

    // Admission check: the audio port arrays on the instance are fixed at
    // lv2_adapter::kMaxChannels, and every index the manifest declares is a
    // port the host will connect. A descriptor whose buses sum past that
    // ceiling in either direction therefore has no valid wiring at all, so it
    // is refused here rather than dropped a port at a time in connect_port() —
    // a dropped connection renders as silence, which reads as a DSP fault
    // instead of a declaration fault. Same shape as the missing-urid:map
    // refusal above: log and hand the host a null instance.
    const auto admission_desc = inst->processor->descriptor();
    int declared_inputs = 0;
    for (const auto& bus : admission_desc.input_buses) {
        declared_inputs += bus.default_channels;
    }
    int declared_outputs = 0;
    for (const auto& bus : admission_desc.output_buses) {
        declared_outputs += bus.default_channels;
    }
    if (declared_inputs < 0 || declared_inputs > lv2_adapter::kMaxChannels ||
        declared_outputs < 0 || declared_outputs > lv2_adapter::kMaxChannels) {
        pulp::runtime::log_warn(
            "lv2: descriptor declares {} input and {} output channels; this adapter carries at "
            "most {} per direction, refusing to instantiate",
            declared_inputs, declared_outputs, lv2_adapter::kMaxChannels);
        delete inst;
        return nullptr;
    }

    // Define parameters
    inst->processor->define_parameters(inst->store);
    auto params = inst->store.all_params();
    inst->num_params = static_cast<int>(params.size());
    inst->param_ids.reserve(params.size());
    for (const auto& p : params) {
        inst->param_ids.push_back(p.id);
    }

    // Allocate control port pointer arrays
    inst->control_in_ports = new float*[inst->num_params]();
    inst->control_out_ports = new float*[inst->num_params]();

    // Audio channel counts, already summed and bounds-checked above.
    inst->num_audio_inputs = declared_inputs;
    inst->num_audio_outputs = declared_outputs;
    inst->accepts_midi = admission_desc.accepts_midi;
    inst->produces_midi = admission_desc.produces_midi;

    // Pre-reserve the RT MIDI buffers here (control thread) so run() never
    // allocates on the audio thread. set_realtime_capacity_limit(true) makes
    // add() drop-past-capacity instead of growing — mirrors the CLAP adapter.
    inst->midi_in.reserve(kRealtimeMidiEventCapacity);
    inst->midi_out.reserve(kRealtimeMidiEventCapacity);
    inst->midi_in.set_realtime_capacity_limit(true);
    inst->midi_out.set_realtime_capacity_limit(true);

    // Prepare the processor
    format::PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_buffer_size = inst->max_block_length;
    inst->processor->prepare(ctx);

    return static_cast<LV2_Handle>(inst);
}

inline void connect_port(LV2_Handle handle, uint32_t port, void* data) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(handle);

    // One ordering, shared with the manifest generator. Every slot index comes
    // from the layout rather than from arithmetic repeated here, and the audio
    // arrays are safe to index because instantiate() refuses a descriptor
    // wider than lv2_adapter::kMaxChannels in either direction.
    const lv2_adapter::Lv2PortLayout layout = inst->port_layout();

    // LV2 port numbers are uint32_t. A value past INT_MAX is not a port this
    // plugin declared; map it to a number the layout classifies as None rather
    // than letting the narrowing produce a negative index.
    const int idx = port <= static_cast<uint32_t>(std::numeric_limits<int>::max())
                        ? static_cast<int>(port)
                        : -1;

    switch (layout.kind_of(idx)) {
    case lv2_adapter::Lv2PortKind::AudioIn:
        inst->audio_in_ports[layout.slot_of(idx)] = static_cast<float*>(data);
        break;
    case lv2_adapter::Lv2PortKind::AudioOut:
        inst->audio_out_ports[layout.slot_of(idx)] = static_cast<float*>(data);
        break;
    case lv2_adapter::Lv2PortKind::Control:
        inst->control_in_ports[layout.slot_of(idx)] = static_cast<float*>(data);
        break;
    case lv2_adapter::Lv2PortKind::AtomIn:
        // LV2 atom input port — host hands us an LV2_Atom_Sequence buffer.
        inst->midi_in_atom = data;
        break;
    case lv2_adapter::Lv2PortKind::AtomOut:
        // LV2 atom output port — host pre-allocates an LV2_Atom_Sequence
        // buffer sized by lv2:minimumSize in the TTL. run() writes
        // outgoing MIDI events into it.
        inst->midi_out_atom = data;
        break;
    case lv2_adapter::Lv2PortKind::Latency:
        // Latency output control port — host reads the value run() writes
        // here for plugin delay compensation.
        inst->latency_port = static_cast<float*>(data);
        break;
    case lv2_adapter::Lv2PortKind::None:
        break;
    }
}

inline void activate(LV2_Handle) {
    // Nothing needed — processor is prepared at instantiation
}

// Write a MidiBuffer into an LV2_Atom_Sequence output port. Uses the standard
// lv2_atom_sequence_clear + append_event helpers. On entry the host's
// out_seq->atom.size carries the buffer capacity; clear() resets it to
// sizeof(body) and append_event() increments it per event. Events that don't
// fit are dropped, not truncated, matching every other format adapter's
// overflow behavior. Exposed non-static for unit testing; all arguments
// validated so missing URIDs are a no-op.
inline void write_midi_out_to_sequence(
    LV2_Atom_Sequence* out_seq,
    LV2_URID urid_atom_sequence,
    LV2_URID urid_midi_event,
    const midi::MidiBuffer& midi_out) {
    if (!out_seq || urid_atom_sequence == 0 || urid_midi_event == 0) return;
    const uint32_t capacity = out_seq->atom.size;
    out_seq->atom.type = urid_atom_sequence;
    out_seq->body.unit = 0;  // frames
    out_seq->body.pad = 0;
    lv2_atom_sequence_clear(out_seq);

    for (const auto& ev : midi_out) {
        const uint32_t msg_size = ev.size();
        if (msg_size == 0 || msg_size > 3) continue;
        // Pack LV2_Atom_Event header + up to 3 MIDI bytes on the stack;
        // append_event copies into out_seq.
        struct alignas(8) {
            LV2_Atom_Event hdr;
            uint8_t payload[3];
        } pkt{};
        pkt.hdr.time.frames = ev.sample_offset;
        pkt.hdr.body.type = urid_midi_event;
        pkt.hdr.body.size = msg_size;
        std::memcpy(pkt.payload, ev.data(), msg_size);
        if (!lv2_atom_sequence_append_event(out_seq, capacity, &pkt.hdr)) {
            break;  // out of capacity — drop remaining events
        }
    }
}

inline void run(LV2_Handle handle, uint32_t n_samples) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(handle);
    if (!inst->processor) return;

    // Honor the block ceiling prepare() sized the Processor for. A host that
    // declares bufsz:boundedBlockLength never exceeds it; one that declares
    // neither the feature nor the option can, and the shared contract is to
    // render the prefix and hand back silence for the tail rather than overrun
    // scratch. Tail zero-fill happens after the output pointers resolve.
    const uint32_t requested_samples = n_samples;
    n_samples = static_cast<uint32_t>(
        clamp_block_to_prepared_max(static_cast<int>(n_samples), inst->max_block_length));

    // Hardware flush-to-zero for the whole audio-thread render, matching every
    // other adapter (clap_adapter.cpp, vst3_adapter.cpp, au_adapter.mm, ...).
    // Protects recursive DSP feedback from denormal stalls under LV2.
    pulp::signal::ScopedFlushDenormals flush_denormals;

    // Report current processing latency to the host's latency control port
    // (see generate_plugin_ttl's lv2:reportsLatency port) for PDC.
    //
    // Clamp to the port's declared domain. The TTL gives this port a minimum of
    // zero, so a processor that returns a negative latency_samples() would
    // otherwise write an out-of-range value straight into the host's delay
    // compensation and shift the track the wrong way.
    if (inst->latency_port) {
        const int reported = inst->processor->latency_samples();
        *inst->latency_port = static_cast<float>(reported > 0 ? reported : 0);
    }

    // Read control port values into the parameter store. LV2 run() is
    // the audio thread, so use the RT-safe path — atomic store + SPSC
    // push for Main listeners, no allocation. Editor pumps via
    // store.pump_listeners() from its UI tick.
    for (int i = 0; i < inst->num_params; ++i) {
        if (inst->control_in_ports[i]) {
            float value = *inst->control_in_ports[i];
            inst->store.set_value_rt(inst->param_ids[i], value);
        }
    }

    // Build buffer views. The views below carry the instance's channel counts
    // over these fixed-size pointer arrays, which is only sound because
    // instantiate() refused any descriptor wider than kMaxChannels.
    const float* in_ptrs[lv2_adapter::kMaxChannels] = {};
    float* out_ptrs[lv2_adapter::kMaxChannels] = {};

    for (int i = 0; i < inst->num_audio_inputs && i < lv2_adapter::kMaxChannels; ++i) {
        in_ptrs[i] = inst->audio_in_ports[i];
    }
    for (int i = 0; i < inst->num_audio_outputs && i < lv2_adapter::kMaxChannels; ++i) {
        out_ptrs[i] = inst->audio_out_ports[i];
    }

    // Silence the region past the prepared maximum so an over-long block reads
    // as clean silence instead of whatever the host buffer last held.
    if (requested_samples > n_samples) {
        for (int i = 0; i < inst->num_audio_outputs && i < lv2_adapter::kMaxChannels; ++i) {
            if (!out_ptrs[i])
                continue;
            std::fill(out_ptrs[i] + n_samples, out_ptrs[i] + requested_samples, 0.0f);
        }
    }

    audio::BufferView<const float> input(in_ptrs,
        static_cast<size_t>(inst->num_audio_inputs), n_samples);
    audio::BufferView<float> output(out_ptrs,
        static_cast<size_t>(inst->num_audio_outputs), n_samples);
    std::array<ProcessBusBufferView<const float>, 1> input_buses{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main,
                     inst->num_audio_inputs, false, inst->num_audio_inputs > 0},
            .buffer = input,
        },
    }};
    std::array<ProcessBusBufferView<float>, 1> output_buses{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main,
                     inst->num_audio_outputs, false, inst->num_audio_outputs > 0},
            .buffer = output,
        },
    }};
    ProcessBuffers process_buffers{
        .inputs = ProcessBusBufferSet<const float>(input_buses),
        .outputs = ProcessBusBufferSet<float>(output_buses),
    };

    format::ProcessContext proc_ctx;
    proc_ctx.sample_rate = inst->sample_rate;
    proc_ctx.num_samples = static_cast<int>(n_samples);
    proc_ctx.process_mode = format::ProcessMode::Realtime;
    proc_ctx.render_speed_hint = format::RenderSpeedHint::Realtime;

    // Uniform param-events contract. The queue carries no atom-sourced events
    // yet; control-port params still flow via store.
    inst->param_events.clear();
    inst->processor->set_param_events(&inst->param_events);

    // RT-safety guard spans the whole audio-thread render: MIDI parse, the
    // process() call, and MIDI serialization. Because midi_in/midi_out are
    // pre-reserved instance members (see instantiate()), the parse below reuses
    // that storage instead of heap-allocating — the very allocation this guard
    // exists to catch. Reset them here, inside the guarded region.
    {
        pulp::runtime::ScopedNoAlloc no_alloc_guard;

        // MIDI: parse the connected LV2_Atom_Sequence (if any) and promote each
        // MidiEvent into the Processor's MidiBuffer. Sysex atoms are currently
        // ignored here; LV2 variable-length event support is not wired yet.
        inst->midi_in.clear();
        inst->midi_out.clear();
        bool transport_received = false;
        if (inst->midi_in_atom && inst->urid_atom_sequence && inst->urid_midi_event) {
            const auto* seq = static_cast<const LV2_Atom_Sequence*>(inst->midi_in_atom);
            if (seq->atom.type == inst->urid_atom_sequence) {
                const auto& time_urids = inst->time_urids;
                LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
                    if (ev->body.type == inst->urid_midi_event) {
                        const auto* data = reinterpret_cast<const uint8_t*>(ev + 1);
                        const uint32_t size = ev->body.size;
                        if (size >= 1 && size <= 3 && (data[0] & 0x80)) {
                            midi::MidiEvent me;
                            me.message =
                                choc::midi::ShortMessage(data[0], size > 1 ? data[1] : uint8_t{0},
                                                         size > 2 ? data[2] : uint8_t{0});
                            me.sample_offset = static_cast<int32_t>(ev->time.frames);
                            inst->midi_in.add(me);
                        }
                        continue;
                    }
                    // Transport. The host delivers time:Position on the same
                    // sequence as MIDI; a block can carry several (a seek mid
                    // block), and the last one wins for this block's context.
                    const bool is_object =
                        (time_urids.atom_object != 0 && ev->body.type == time_urids.atom_object) ||
                        (time_urids.atom_blank != 0 && ev->body.type == time_urids.atom_blank);
                    if (!is_object)
                        continue;
                    const auto* obj = reinterpret_cast<const LV2_Atom_Object*>(&ev->body);
                    if (decode_time_position(obj, time_urids, inst->transport)) {
                        inst->has_transport = true;
                        transport_received = true;
                    }
                }
            }
        }

        // Project the latched transport onto this block, then let the shared
        // mapper derive the bar and the change flags. Extrapolate only when the
        // host sent nothing new this block — a Position that just arrived is
        // already positioned at the block start.
        if (inst->has_transport) {
            if (!transport_received) {
                advance_time_position(inst->transport, n_samples, inst->sample_rate);
            }
            boundary::apply_host_transport(proc_ctx, to_host_transport(inst->transport),
                                           inst->playhead_prev);
        } else {
            // No transport ever seen: still run the diff so the snapshot tracks
            // block size and the validity mask stays honestly empty.
            boundary::apply_host_transport(proc_ctx, boundary::HostTransport{},
                                           inst->playhead_prev);
        }

        inst->processor->process(process_buffers, inst->midi_in, inst->midi_out,
                                 proc_ctx);

        // Serialize outgoing MIDI back to the LV2 atom output port.
        write_midi_out_to_sequence(
            static_cast<LV2_Atom_Sequence*>(inst->midi_out_atom),
            inst->urid_atom_sequence,
            inst->urid_midi_event,
            inst->midi_out);
    }
}

inline void deactivate(LV2_Handle) {
    // Nothing needed
}

// ── state:interface ──────────────────────────────────────────────────────
//
// One property, keyed by lv2_adapter::kStateBlobUri, holding the same
// versioned envelope plugin_state_io::serialize() produces for every other
// format: the StateStore payload plus whatever the Processor returns from
// serialize_plugin_state(). Control-port values are the host's to save and
// restore, so the envelope's parameter half is redundant under LV2 and is
// simply overwritten by the port values on the next run(); the plugin-owned
// half — sampler buffers, file references, blobs — has no other way home.

inline LV2_State_Status save_state(LV2_Handle instance, LV2_State_Store_Function store,
                                   LV2_State_Handle handle, uint32_t, const LV2_Feature* const*) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(instance);
    if (!inst || !inst->processor || !store)
        return LV2_STATE_ERR_UNKNOWN;
    if (inst->urid_state_blob == 0 || inst->urid_atom_chunk == 0) {
        return LV2_STATE_ERR_NO_FEATURE;
    }

    const auto data = plugin_state_io::serialize(inst->store, *inst->processor);
    // The spec requires size > 0 for a stored property. Nothing to save is a
    // success, not an error — restore() falls back to defaults.
    if (data.empty())
        return LV2_STATE_SUCCESS;

    return store(handle, inst->urid_state_blob, data.data(), data.size(), inst->urid_atom_chunk,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

inline LV2_State_Status restore_state(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
                                      LV2_State_Handle handle, uint32_t,
                                      const LV2_Feature* const*) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(instance);
    if (!inst || !inst->processor || !retrieve)
        return LV2_STATE_ERR_UNKNOWN;
    if (inst->urid_state_blob == 0)
        return LV2_STATE_ERR_NO_FEATURE;

    std::size_t size = 0;
    uint32_t type = 0;
    uint32_t flags = 0;
    const void* value = retrieve(handle, inst->urid_state_blob, &size, &type, &flags);
    // A host may legitimately restore an empty map to reset the plugin. The
    // spec requires falling back to defaults rather than failing.
    if (!value || size == 0)
        return LV2_STATE_SUCCESS;
    if (inst->urid_atom_chunk != 0 && type != 0 && type != inst->urid_atom_chunk) {
        return LV2_STATE_ERR_BAD_TYPE;
    }

    const auto* bytes = static_cast<const uint8_t*>(value);
    // restore() is in LV2's Instantiation threading class, so no run() call is
    // in flight and Processor::deserialize_plugin_state() gets the
    // non-concurrent context it documents.
    const bool ok = plugin_state_io::deserialize(std::span<const uint8_t>(bytes, size), inst->store,
                                                 *inst->processor);
    if (!ok)
        return LV2_STATE_ERR_UNKNOWN;

    // A restored state can name a different derived source than the live one
    // (a different impulse response, a different sample set). Reconcile it here
    // or a worker-less processor renders the old state for the rest of the
    // session.
    inst->processor->on_non_realtime_tick();
    return LV2_STATE_SUCCESS;
}

inline const LV2_State_Interface g_state_interface = {save_state, restore_state};

inline const void* extension_data(const char* uri) {
    if (uri && std::strcmp(uri, LV2_STATE__interface) == 0) {
        return &g_state_interface;
    }
    return nullptr;
}

inline void cleanup(LV2_Handle handle) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(handle);
    if (inst) {
        inst->processor->release();
        delete[] inst->control_in_ports;
        delete[] inst->control_out_ports;
        delete inst;
    }
}

// ── Offline bundle description ──────────────────────────────────────────
//
// A host discovers a plugin by reading `manifest.ttl` from the bundle, so a
// bundle that carries only the shared object is not a plugin: nothing finds it
// and the failure is silent. The generators for both files already live in
// `lv2_adapter.cpp`, and describing the plugin needs the plugin — the port
// layout comes from its descriptor and the control ports from its parameters —
// so the module describes ITSELF rather than a build script guessing.
//
// Exported so one driver can dlopen any Pulp LV2 module and ask it. Offline
// only: called by the build, never by a host, never on the audio thread.
inline int write_bundle_ttl(const char* bundle_dir, const char* binary_name) noexcept {
    if (bundle_dir == nullptr || binary_name == nullptr || g_factory == nullptr || g_uri == nullptr)
        return 1;

    // The store is declared before the Processor so it is destroyed AFTER it,
    // the same ordering PulpLv2Instance states and for the same reason:
    // Processor::state() dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor
    // is about to join. Locals are destroyed in reverse declaration order, so
    // swapping these two hands that thread a freed store.
    state::StateStore store;

    auto processor = g_factory();
    if (!processor)
        return 2;

    // Same order instantiate() uses: the descriptor decides the port layout and
    // define_parameters() decides the control ports, so both must run before
    // either file is emitted or the manifest describes a different plugin from
    // the one the host will load.
    processor->set_state_store(&store);
    processor->define_parameters(store);
    const auto descriptor = processor->descriptor();

    const std::string dir(bundle_dir);
    const std::string binary(binary_name);
    const auto stem = binary.substr(0, binary.rfind('.'));

    std::ofstream manifest(dir + "/manifest.ttl", std::ios::binary | std::ios::trunc);
    if (!manifest)
        return 3;
    manifest << lv2_adapter::generate_manifest_ttl(g_uri, binary);
    if (!manifest.flush())
        return 3;

    // `generate_manifest_ttl` points rdfs:seeAlso at <stem>.ttl, so the
    // description has to land on exactly that name or the host reads the
    // manifest, follows the pointer, and finds nothing.
    std::ofstream plugin(dir + "/" + stem + ".ttl", std::ios::binary | std::ios::trunc);
    if (!plugin)
        return 4;
    plugin << lv2_adapter::generate_plugin_ttl(descriptor, store, g_uri);
    if (!plugin.flush())
        return 4;
    return 0;
}

// The LV2 descriptor
inline LV2_Descriptor g_lv2_descriptor = {nullptr, // URI — set at static init
                                          instantiate, connect_port, activate,      run,
                                          deactivate,  cleanup,      extension_data};

} // namespace pulp::format::lv2_generic

// ── PULP_LV2_PLUGIN Macro ───────────────────────────────────────────────

#define PULP_LV2_PLUGIN(factory_fn, plugin_uri)                                                    \
    namespace {                                                                                    \
    struct PulpLv2Init {                                                                           \
        PulpLv2Init() {                                                                            \
            pulp::format::lv2_generic::g_factory = factory_fn;                                     \
            pulp::format::lv2_generic::g_uri = plugin_uri;                                         \
            pulp::format::lv2_generic::g_lv2_descriptor.URI = plugin_uri;                          \
        }                                                                                          \
    } s_lv2_init;                                                                                  \
    }                                                                                              \
                                                                                                   \
    extern "C" {                                                                                   \
    LV2_SYMBOL_EXPORT                                                                              \
    const LV2_Descriptor* lv2_descriptor(uint32_t index) {                                         \
        return (index == 0) ? &pulp::format::lv2_generic::g_lv2_descriptor : nullptr;              \
    }                                                                                              \
    LV2_SYMBOL_EXPORT                                                                              \
    int pulp_lv2_write_bundle_ttl(const char* bundle_dir, const char* binary) {                    \
        return pulp::format::lv2_generic::write_bundle_ttl(bundle_dir, binary);                    \
    }                                                                                              \
    }
