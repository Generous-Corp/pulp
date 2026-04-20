#pragma once

// Generic LV2 entry point generator
// Plugin developers include this and call PULP_LV2_PLUGIN() with their factory function.
// All LV2 boilerplate (descriptor, instantiate, run, cleanup) is generated automatically.
//
// Usage (in one .cpp file per plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/lv2_entry.hpp>
//   PULP_LV2_PLUGIN(my_namespace::create_my_processor, "http://pulp.audio/plugins/my_plugin")

#include <pulp/format/processor.hpp>
#include <pulp/format/lv2_adapter.hpp>
#include <pulp/runtime/log.hpp>

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <cstring>
#include <vector>

namespace pulp::format::lv2_generic {

// Populated at static init by PULP_LV2_PLUGIN
inline ProcessorFactory g_factory = nullptr;
inline const char* g_uri = nullptr;

// ── LV2 Callbacks ────────────────────────────────────────────────────────

inline LV2_Handle instantiate(
    const LV2_Descriptor*,
    double sample_rate,
    const char*,
    const LV2_Feature* const* features)
{
    if (!g_factory) return nullptr;

    // Resolve LV2_URID_Map — required by any real LV2 host. Pre-production
    // fix (workstream 01 slice 1.5): we previously ignored the features
    // array entirely, which would cause real hosts to fail to load the
    // plugin in surprising ways. Now we fail instantiation loudly and
    // return nullptr so the host surfaces a clear error.
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
    inst->processor = g_factory();
    if (!inst->processor) {
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

    // Count audio channels from descriptor
    auto desc = inst->processor->descriptor();
    inst->num_audio_inputs = 0;
    for (const auto& bus : desc.input_buses) {
        inst->num_audio_inputs += bus.default_channels;
    }
    inst->num_audio_outputs = 0;
    for (const auto& bus : desc.output_buses) {
        inst->num_audio_outputs += bus.default_channels;
    }
    inst->accepts_midi = desc.accepts_midi;
    inst->produces_midi = desc.produces_midi;

    // Prepare the processor
    format::PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_buffer_size = 4096;  // LV2 doesn't specify upfront; use reasonable max
    inst->processor->prepare(ctx);

    return static_cast<LV2_Handle>(inst);
}

inline void connect_port(LV2_Handle handle, uint32_t port, void* data) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(handle);

    int audio_in_end = inst->num_audio_inputs;
    int audio_out_end = audio_in_end + inst->num_audio_outputs;
    int control_end = audio_out_end + inst->num_params;
    // MIDI atom ports follow control ports. A plug-in with accepts_midi
    // gets one atom input port at index control_end. Workstream 01 #241.
    int atom_in_end = control_end + (inst->accepts_midi ? 1 : 0);
    // #491: plug-ins with produces_midi get one atom output port right
    // after the (optional) input. TTL emits these in the same order in
    // generate_plugin_ttl(), so the host's port index matches.
    int atom_out_end = atom_in_end + (inst->produces_midi ? 1 : 0);

    int idx = static_cast<int>(port);

    if (idx < audio_in_end) {
        inst->audio_in_ports[idx] = static_cast<float*>(data);
    } else if (idx < audio_out_end) {
        inst->audio_out_ports[idx - audio_in_end] = static_cast<float*>(data);
    } else if (idx < control_end) {
        inst->control_in_ports[idx - audio_out_end] = static_cast<float*>(data);
    } else if (idx < atom_in_end) {
        // LV2 atom input port — host hands us an LV2_Atom_Sequence buffer.
        inst->midi_in_atom = data;
    } else if (idx < atom_out_end) {
        // LV2 atom output port — host pre-allocates an LV2_Atom_Sequence
        // buffer sized by lv2:minimumSize in the TTL. run() writes
        // outgoing MIDI events into it.
        inst->midi_out_atom = data;
    }
}

inline void activate(LV2_Handle) {
    // Nothing needed — processor is prepared at instantiation
}

// #491: write a MidiBuffer into an LV2_Atom_Sequence output port. Uses
// the standard lv2_atom_sequence_clear + append_event helpers. On entry
// the host's out_seq->atom.size carries the buffer capacity; clear()
// resets it to sizeof(body) and append_event() increments it per event.
// Events that don't fit are dropped, not truncated — matches every
// other format adapter's overflow behavior. Exposed non-static for unit
// testing; all arguments validated so missing URIDs are a no-op.
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

    // Read control port values into the parameter store
    for (int i = 0; i < inst->num_params; ++i) {
        if (inst->control_in_ports[i]) {
            float value = *inst->control_in_ports[i];
            inst->store.set_value(inst->param_ids[i], value);
        }
    }

    // Build buffer views
    const float* in_ptrs[lv2_adapter::kMaxChannels] = {};
    float* out_ptrs[lv2_adapter::kMaxChannels] = {};

    for (int i = 0; i < inst->num_audio_inputs && i < lv2_adapter::kMaxChannels; ++i) {
        in_ptrs[i] = inst->audio_in_ports[i];
    }
    for (int i = 0; i < inst->num_audio_outputs && i < lv2_adapter::kMaxChannels; ++i) {
        out_ptrs[i] = inst->audio_out_ports[i];
    }

    audio::BufferView<const float> input(in_ptrs,
        static_cast<size_t>(inst->num_audio_inputs), n_samples);
    audio::BufferView<float> output(out_ptrs,
        static_cast<size_t>(inst->num_audio_outputs), n_samples);

    // MIDI: parse the connected LV2_Atom_Sequence (if any) and promote
    // each MidiEvent into the Processor's MidiBuffer. Sysex atoms are
    // currently ignored here; the CLAP/VST3/AU sysex sidecar wiring in
    // issue #239 is the in-progress path for variable-length events.
    midi::MidiBuffer midi_in, midi_out;
    if (inst->midi_in_atom && inst->urid_atom_sequence && inst->urid_midi_event) {
        const auto* seq = static_cast<const LV2_Atom_Sequence*>(inst->midi_in_atom);
        if (seq->atom.type == inst->urid_atom_sequence) {
            LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
                if (ev->body.type != inst->urid_midi_event) continue;
                const auto* data = reinterpret_cast<const uint8_t*>(ev + 1);
                const uint32_t size = ev->body.size;
                if (size >= 1 && size <= 3 && (data[0] & 0x80)) {
                    midi::MidiEvent me;
                    me.message = choc::midi::ShortMessage(
                        data[0],
                        size > 1 ? data[1] : uint8_t{0},
                        size > 2 ? data[2] : uint8_t{0});
                    me.sample_offset = static_cast<int32_t>(ev->time.frames);
                    midi_in.add(me);
                }
            }
        }
    }

    format::ProcessContext proc_ctx;
    proc_ctx.sample_rate = inst->sample_rate;
    proc_ctx.num_samples = static_cast<int>(n_samples);

    inst->processor->process(output, input, midi_in, midi_out, proc_ctx);

    // #491: serialize outgoing MIDI back to the LV2 atom output port.
    // Prior to this, any Processor that populated midi_out (arpeggiator,
    // note generator, MIDI-effect passthrough) had its events silently
    // dropped when hosted via LV2.
    write_midi_out_to_sequence(
        static_cast<LV2_Atom_Sequence*>(inst->midi_out_atom),
        inst->urid_atom_sequence,
        inst->urid_midi_event,
        midi_out);
}

inline void deactivate(LV2_Handle) {
    // Nothing needed
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

// The LV2 descriptor
inline LV2_Descriptor g_lv2_descriptor = {
    nullptr,       // URI — set at static init
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    nullptr        // extension_data
};

} // namespace pulp::format::lv2_generic

// ── PULP_LV2_PLUGIN Macro ───────────────────────────────────────────────

#define PULP_LV2_PLUGIN(factory_fn, plugin_uri)                                 \
    namespace {                                                                  \
    struct PulpLv2Init {                                                         \
        PulpLv2Init() {                                                          \
            pulp::format::lv2_generic::g_factory = factory_fn;                   \
            pulp::format::lv2_generic::g_uri = plugin_uri;                       \
            pulp::format::lv2_generic::g_lv2_descriptor.URI = plugin_uri;        \
        }                                                                        \
    } s_lv2_init;                                                                \
    }                                                                            \
                                                                                 \
    extern "C" {                                                                 \
    LV2_SYMBOL_EXPORT                                                            \
    const LV2_Descriptor* lv2_descriptor(uint32_t index) {                       \
        return (index == 0) ? &pulp::format::lv2_generic::g_lv2_descriptor       \
                            : nullptr;                                           \
    }                                                                            \
    }
