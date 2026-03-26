// CLAP Adapter implementation
// Implements the CLAP C API wrapping a Pulp Processor

#include <pulp/format/clap_adapter.hpp>
#include <pulp/runtime/log.hpp>
#include <clap/ext/preset-load.h>
#include <cstring>
#include <filesystem>

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

    // Create PresetManager from plugin descriptor
    auto& desc = self->processor->descriptor();
    self->preset_manager = std::make_unique<state::PresetManager>(
        self->store, desc.vendor, desc.name);

    runtime::log_info("CLAP: initialized '{}'", desc.name);
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

    // Reset per-buffer modulation offsets before applying new events
    self->store.reset_all_mod();

    // Handle parameter, modulation, and gesture events from host
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
            } else if (hdr->type == CLAP_EVENT_PARAM_MOD) {
                auto* ev = reinterpret_cast<const clap_event_param_mod_t*>(hdr);
                self->store.set_mod_offset(
                    static_cast<state::ParamID>(ev->param_id),
                    static_cast<float>(ev->amount));
            } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
                auto* ev = reinterpret_cast<const clap_event_param_gesture_t*>(hdr);
                self->store.begin_gesture(static_cast<state::ParamID>(ev->param_id));
            } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_END) {
                auto* ev = reinterpret_cast<const clap_event_param_gesture_t*>(hdr);
                self->store.end_gesture(static_cast<state::ParamID>(ev->param_id));
            }
        }
    }

    // Build audio buffer views (no allocation — uses pre-allocated arrays)
    int in_channels = 0, out_channels = 0;

    if (process->audio_inputs_count > 0) {
        auto& bus = process->audio_inputs[0];
        in_channels = std::min(static_cast<int>(bus.channel_count), kMaxChannels);
        for (int ch = 0; ch < in_channels; ++ch)
            self->input_ptrs[ch] = bus.data32[ch];
    }

    if (process->audio_outputs_count > 0) {
        auto& bus = process->audio_outputs[0];
        out_channels = std::min(static_cast<int>(bus.channel_count), kMaxChannels);
        for (int ch = 0; ch < out_channels; ++ch)
            self->output_ptrs[ch] = bus.data32[ch];
    }

    audio::BufferView<const float> input_view(
        self->input_ptrs, in_channels, num_samples);
    audio::BufferView<float> output_view(
        self->output_ptrs, out_channels, num_samples);

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

    // Snapshot parameter values to detect plugin-side changes
    auto all_params = self->store.all_params();
    self->param_snapshot.resize(all_params.size());
    for (std::size_t i = 0; i < all_params.size(); ++i) {
        self->param_snapshot[i] = self->store.get_value(all_params[i].id);
    }

    // Process!
    self->processor->process(output_view, input_view, midi_in, midi_out, ctx);

    // Emit output parameter events for any values the plugin changed,
    // so the host can record automation
    auto* out_events = process->out_events;
    if (out_events) {
        for (std::size_t i = 0; i < all_params.size(); ++i) {
            float current = self->store.get_value(all_params[i].id);
            if (current != self->param_snapshot[i]) {
                clap_event_param_value_t ev{};
                ev.header.size = sizeof(ev);
                ev.header.type = CLAP_EVENT_PARAM_VALUE;
                ev.header.time = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.flags = 0;
                ev.param_id = all_params[i].id;
                ev.cookie = nullptr;
                ev.note_id = -1;
                ev.port_index = -1;
                ev.channel = -1;
                ev.key = -1;
                ev.value = static_cast<double>(current);
                out_events->try_push(out_events, &ev.header);
            }
        }
    }

    return CLAP_PROCESS_CONTINUE;
}

// ── Preset load extension ─────────────────────────────────────────────────

static bool preset_from_location(const clap_plugin_t* plugin,
                                  uint32_t location_kind,
                                  const char* location,
                                  const char* /*load_key*/) {
    auto* self = get_self(plugin);
    if (!self->preset_manager || !location) return false;

    // CLAP_PRESET_DISCOVERY_LOCATION_FILE = 0
    if (location_kind != 0) return false;

    return self->preset_manager->load(std::filesystem::path(location));
}

static const clap_plugin_preset_load_t s_preset_load = {
    .from_location = preset_from_location
};

const void* clap_get_extension(const clap_plugin_t* plugin, const char* id) {
    auto* self = get_self(plugin);

    // Only expose preset-load if the plugin has a PresetManager
    if (self->preset_manager) {
        if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0) return &s_preset_load;
        if (std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0) return &s_preset_load;
    }

    return nullptr;
}

void clap_on_main_thread(const clap_plugin_t*) {}

} // namespace pulp::format::clap_adapter
