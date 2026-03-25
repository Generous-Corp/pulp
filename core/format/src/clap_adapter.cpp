// CLAP Adapter implementation
// Implements the CLAP C API wrapping a Pulp Processor

#include <pulp/format/clap_adapter.hpp>
#include <pulp/runtime/log.hpp>
#include <cstring>

namespace pulp::format::clap_adapter {

static PulpClapPlugin* get_self(const clap_plugin_t* plugin) {
    return static_cast<PulpClapPlugin*>(plugin->plugin_data);
}

bool clap_init(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    self->processor = self->factory();
    if (!self->processor) return false;
    self->processor->set_state_store(&self->store);
    self->processor->define_parameters(self->store);
    runtime::log_info("CLAP: initialized '{}'", self->processor->descriptor().name);
    return true;
}

void clap_destroy(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    delete self;
}

bool clap_activate(const clap_plugin_t* plugin, double sr, uint32_t, uint32_t max_frames) {
    auto* self = get_self(plugin);
    self->sample_rate = sr;
    self->max_buffer_size = static_cast<int>(max_frames);

    PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(max_frames);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    self->processor->prepare(ctx);
    return true;
}

void clap_deactivate(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    self->processor->release();
}

bool clap_start_processing(const clap_plugin_t*) { return true; }
void clap_stop_processing(const clap_plugin_t*) {}
void clap_reset(const clap_plugin_t*) {}

clap_process_status clap_process(const clap_plugin_t* plugin, const clap_process_t* process) {
    auto* self = get_self(plugin);
    if (!self->processor) return CLAP_PROCESS_ERROR;

    auto num_samples = process->frames_count;
    if (num_samples == 0) return CLAP_PROCESS_CONTINUE;

    // Handle parameter events from host
    auto* in_events = process->in_events;
    if (in_events) {
        uint32_t event_count = in_events->size(in_events);
        for (uint32_t i = 0; i < event_count; ++i) {
            auto* hdr = in_events->get(in_events, i);
            if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
                auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
                self->store.set_value(
                    static_cast<state::ParamID>(ev->param_id),
                    static_cast<float>(ev->value));
            }
        }
    }

    // Build audio buffer views
    int in_channels = 0, out_channels = 0;
    std::vector<float*> in_ptrs, out_ptrs;
    std::vector<const float*> in_const_ptrs;

    if (process->audio_inputs_count > 0) {
        auto& bus = process->audio_inputs[0];
        in_channels = static_cast<int>(bus.channel_count);
        in_ptrs.resize(in_channels);
        in_const_ptrs.resize(in_channels);
        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = bus.data32[ch];
            in_const_ptrs[ch] = bus.data32[ch];
        }
    }

    if (process->audio_outputs_count > 0) {
        auto& bus = process->audio_outputs[0];
        out_channels = static_cast<int>(bus.channel_count);
        out_ptrs.resize(out_channels);
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = bus.data32[ch];
        }
    }

    audio::BufferView<const float> input_view(
        in_const_ptrs.data(), in_channels, num_samples);
    audio::BufferView<float> output_view(
        out_ptrs.data(), out_channels, num_samples);

    // Build MIDI from CLAP note events
    midi::MidiBuffer midi_in, midi_out;
    if (in_events) {
        uint32_t event_count = in_events->size(in_events);
        for (uint32_t i = 0; i < event_count; ++i) {
            auto* hdr = in_events->get(in_events, i);
            if (hdr->type == CLAP_EVENT_NOTE_ON) {
                auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
                auto me = midi::MidiEvent::note_on(
                    static_cast<uint8_t>(ev->channel),
                    static_cast<uint8_t>(ev->key),
                    static_cast<uint8_t>(ev->velocity * 127.0));
                me.sample_offset = static_cast<int32_t>(hdr->time);
                midi_in.add(me);
            } else if (hdr->type == CLAP_EVENT_NOTE_OFF) {
                auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
                auto me = midi::MidiEvent::note_off(
                    static_cast<uint8_t>(ev->channel),
                    static_cast<uint8_t>(ev->key),
                    static_cast<uint8_t>(ev->velocity * 127.0));
                me.sample_offset = static_cast<int32_t>(hdr->time);
                midi_in.add(me);
            }
        }
    }

    // Process context
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = self->sample_rate;
    ctx.num_samples = static_cast<int>(num_samples);
    if (process->transport) {
        ctx.is_playing = (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        ctx.tempo_bpm = process->transport->tempo;
        if (process->transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
            ctx.position_beats = static_cast<double>(process->transport->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        }
        if (process->transport->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
            ctx.time_sig_numerator = static_cast<int>(process->transport->tsig_num);
            ctx.time_sig_denominator = static_cast<int>(process->transport->tsig_denom);
        }
    }

    // Process!
    self->processor->process(output_view, input_view, midi_in, midi_out, ctx);

    return CLAP_PROCESS_CONTINUE;
}

const void* clap_get_extension(const clap_plugin_t*, const char*) {
    return nullptr; // Extensions added as needed
}

void clap_on_main_thread(const clap_plugin_t*) {}

} // namespace pulp::format::clap_adapter
