// CLAP Adapter implementation
// Implements the CLAP C API wrapping a Pulp Processor

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <pulp/runtime/log.hpp>
#include <clap/ext/preset-load.h>
#include <algorithm>
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
    auto desc = self->processor->descriptor();
    self->preset_manager = std::make_unique<state::PresetManager>(
        self->store, desc.manufacturer, desc.name);

    // Wire MPE sidecar when the plugin opts in.
    self->mpe_enabled = desc.supports_mpe;
    if (self->mpe_enabled) {
        midi::bind_tracker_to_buffer(
            self->mpe_tracker, self->mpe_buffer, self->mpe_current_sample_offset);
        runtime::log_info("CLAP: MPE sidecar enabled for '{}'", desc.name);
    }

    // Wire UMP sidecar when the plugin opts in.
    self->ump_enabled = desc.supports_ump;
    if (self->ump_enabled) {
        runtime::log_info("CLAP: UMP sidecar enabled for '{}'", desc.name);
    }

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

    auto desc = self->processor->descriptor();
    PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(max_frames);
    ctx.input_channels = desc.default_input_channels();
    ctx.output_channels = desc.default_output_channels();
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

    // Build audio buffer views (no allocation — uses pre-allocated arrays).
    // Bus 0 routes to the main input/output; bus 1 (when present) routes to
    // Processor::set_sidechain(). Additional input buses beyond index 1 are
    // currently ignored — the Processor API exposes a single sidechain slot.
    // Workstream 01 slice 1.1: previously only bus 0 was routed and
    // set_sidechain() was never called; sidechain compressors could not
    // function through the CLAP adapter.
    int in_channels = 0, out_channels = 0;
    int sc_channels = 0;

    if (process->audio_inputs_count > 0) {
        auto& bus = process->audio_inputs[0];
        in_channels = (std::min)(static_cast<int>(bus.channel_count), kMaxChannels);
        for (int ch = 0; ch < in_channels; ++ch)
            self->input_ptrs[ch] = bus.data32[ch];
    }
    if (process->audio_inputs_count > 1) {
        auto& sc_bus = process->audio_inputs[1];
        sc_channels = (std::min)(static_cast<int>(sc_bus.channel_count), kMaxChannels);
        for (int ch = 0; ch < sc_channels; ++ch)
            self->sidechain_ptrs[ch] = sc_bus.data32[ch];
    }

    if (process->audio_outputs_count > 0) {
        auto& bus = process->audio_outputs[0];
        out_channels = (std::min)(static_cast<int>(bus.channel_count), kMaxChannels);
        for (int ch = 0; ch < out_channels; ++ch)
            self->output_ptrs[ch] = bus.data32[ch];
    }
    // Secondary output buses are zero-filled so hosts do not read
    // uninitialised memory on multi-out instruments. Full multi-out routing
    // to Processor is tracked separately (audit 5.2).
    for (uint32_t b = 1; b < process->audio_outputs_count; ++b) {
        auto& bus = process->audio_outputs[b];
        for (uint32_t ch = 0; ch < bus.channel_count; ++ch) {
            if (bus.data32[ch]) {
                std::memset(bus.data32[ch], 0, sizeof(float) * num_samples);
            }
        }
    }

    audio::BufferView<const float> input_view(
        self->input_ptrs, in_channels, num_samples);
    audio::BufferView<float> output_view(
        self->output_ptrs, out_channels, num_samples);
    audio::BufferView<const float> sidechain_view(
        self->sidechain_ptrs, sc_channels, num_samples);
    self->processor->set_sidechain(sc_channels > 0 ? &sidechain_view : nullptr);

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
            } else if (hdr->type == CLAP_EVENT_MIDI_SYSEX) {
                // Workstream 01 — route CLAP sysex into MidiBuffer's
                // variable-length sidecar (issue #239).
                auto* ev = reinterpret_cast<const clap_event_midi_sysex_t*>(hdr);
                if (ev->buffer && ev->size > 0) {
                    midi_in.add_sysex(
                        std::vector<uint8_t>(ev->buffer, ev->buffer + ev->size),
                        static_cast<int32_t>(hdr->time),
                        0.0);
                }
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

    // MPE sidecar: run inbound MIDI through the voice tracker and attach
    // the resulting expression buffer to the processor for the duration
    // of this process() call. Events stay sample-ordered because midi_in
    // is already in host delivery order.
    if (self->mpe_enabled) {
        self->mpe_buffer.clear();
        for (const auto& ev : midi_in) {
            self->mpe_current_sample_offset = ev.sample_offset;
            self->mpe_tracker.process(ev);
        }
        self->processor->set_mpe_input(&self->mpe_buffer);
    } else {
        self->processor->set_mpe_input(nullptr);
    }

    // UMP sidecar: until CLAP ships CLAP_EVENT_MIDI2, synthesise the UMP
    // stream by converting the inbound MIDI 1.0 events to MIDI 2.0
    // Channel Voice packets. Plugins that declare supports_ump see the
    // same sample-ordered stream the host would otherwise deliver natively.
    if (self->ump_enabled) {
        self->ump_buffer.clear();
        midi::midi1_to_ump(midi_in, self->ump_buffer);
        self->processor->set_ump_input(&self->ump_buffer);
    } else {
        self->processor->set_ump_input(nullptr);
    }

    // Process!
    self->processor->process(output_view, input_view, midi_in, midi_out, ctx);

    // Emit output parameter events for any values the plugin changed,
    // so the host can record automation
    auto* out_events = process->out_events;
    if (out_events) {
        for (std::size_t i = 0; i < all_params.size(); ++i) {
            float current = self->store.get_value(all_params[i].id);
            if (std::memcmp(&current, &self->param_snapshot[i], sizeof(float)) != 0) {
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

    // ARA companion factory (workstream 06 slice 6.5). Lazily instantiate
    // the controller the first time an ARA-aware host asks for it, then
    // hand back the factory pointer. Returns nullptr if the plugin did
    // not override create_ara_document_controller().
    if (std::strcmp(id, kClapAraFactoryExtension) == 0) {
        // Guard: hosts may query extensions before clap_init populated
        // self->processor. The factory is process-global so we can still
        // return it; the per-plug-in controller lazy-creates only once
        // we have a live Processor instance.
        if (!self->ara_controller && self->processor) {
            self->ara_controller = self->processor->create_ara_document_controller();
        }
        return ara_companion_factory_for(self->ara_controller.get());
    }

    return nullptr;
}

void clap_on_main_thread(const clap_plugin_t*) {}

} // namespace pulp::format::clap_adapter
