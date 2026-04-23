// CLAP Adapter implementation
// Implements the CLAP C API wrapping a Pulp Processor

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <pulp/runtime/log.hpp>
#include <clap/ext/preset-load.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace pulp::format::clap_adapter {

static PulpClapPlugin* get_self(const clap_plugin_t* plugin) {
    return static_cast<PulpClapPlugin*>(plugin->plugin_data);
}

// Read a CLAP event by value from the (possibly-misaligned) header
// pointer handed out by `clap_input_events.get()`. Hosts may pack
// variable-size events back-to-back without padding to the strictest
// member alignment, so `reinterpret_cast<const Foo*>(hdr)` can yield a
// pointer that violates alignof(Foo) — access to any 8-byte member
// (e.g. `double velocity` in `clap_event_note_t`) is then UB. UBSan
// caught this at clap_adapter.cpp:200 via test_clap_midi_events.cpp:772.
// See #688. memcpy into a stack local guarantees proper alignment.
template <typename T>
static T load_event(const clap_event_header_t* hdr) {
    T out;
    std::memcpy(&out, hdr, sizeof(T));
    return out;
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
                const auto ev = load_event<clap_event_param_value_t>(hdr);
                self->store.set_value(
                    static_cast<state::ParamID>(ev.param_id),
                    static_cast<float>(ev.value));
            } else if (hdr->type == CLAP_EVENT_PARAM_MOD) {
                const auto ev = load_event<clap_event_param_mod_t>(hdr);
                self->store.set_mod_offset(
                    static_cast<state::ParamID>(ev.param_id),
                    static_cast<float>(ev.amount));
            } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
                const auto ev = load_event<clap_event_param_gesture_t>(hdr);
                self->store.begin_gesture(static_cast<state::ParamID>(ev.param_id));
            } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_END) {
                const auto ev = load_event<clap_event_param_gesture_t>(hdr);
                self->store.end_gesture(static_cast<state::ParamID>(ev.param_id));
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
        // #277: guard against a host that reports a sidechain bus but
        // hands us a null data32 pointer (bus deactivated). Mirrors the
        // defensive guard the VST3 adapter already has.
        if (sc_bus.data32) {
            sc_channels = (std::min)(static_cast<int>(sc_bus.channel_count), kMaxChannels);
            for (int ch = 0; ch < sc_channels; ++ch) {
                self->sidechain_ptrs[ch] = sc_bus.data32[ch];
                if (!self->sidechain_ptrs[ch]) {
                    // Any null per-channel pointer demotes the whole bus
                    // to "not supplied" so Processor::set_sidechain sees
                    // nullptr rather than a half-valid BufferView.
                    sc_channels = 0;
                    break;
                }
            }
        }
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
    // Track whether any native CLAP_EVENT_MIDI2 packet was delivered so we
    // can skip the MIDI 1.0 → UMP synthesis path when the host is speaking
    // UMP natively. The host still sends CLAP_EVENT_NOTE_* in parallel for
    // note events; those populate midi_in for the MPE tracker and plugins
    // that only consume MIDI 1.0.
    bool host_delivered_ump = false;
    // Clear the UMP sidecar up-front: we append to it directly when the
    // host sends CLAP_EVENT_MIDI2 during the event loop below.
    if (self->ump_enabled) {
        self->ump_buffer.clear();
    }
    // One-time debug log when a plugin that didn't opt into MPE drops a
    // note-expression event.
    bool note_expression_drop_logged = false;
    if (in_events) {
        uint32_t event_count = in_events->size(in_events);
        for (uint32_t i = 0; i < event_count; ++i) {
            auto* hdr = in_events->get(in_events, i);
            if (hdr->type == CLAP_EVENT_NOTE_ON) {
                const auto ev = load_event<clap_event_note_t>(hdr);
                auto me = midi::MidiEvent::note_on(
                    static_cast<uint8_t>(ev.channel),
                    static_cast<uint8_t>(ev.key),
                    static_cast<uint8_t>(ev.velocity * 127.0));
                me.sample_offset = static_cast<int32_t>(hdr->time);
                midi_in.add(me);
            } else if (hdr->type == CLAP_EVENT_NOTE_OFF) {
                const auto ev = load_event<clap_event_note_t>(hdr);
                auto me = midi::MidiEvent::note_off(
                    static_cast<uint8_t>(ev.channel),
                    static_cast<uint8_t>(ev.key),
                    static_cast<uint8_t>(ev.velocity * 127.0));
                me.sample_offset = static_cast<int32_t>(hdr->time);
                midi_in.add(me);
            } else if (hdr->type == CLAP_EVENT_NOTE_CHOKE) {
                // NOTE_CHOKE has no MIDI 1.0 equivalent (the host is
                // telling the plugin "force-release this voice, no
                // matching note-off is coming"). The closest analogue is
                // a zero-velocity note-off on the same (channel, key).
                // See clap/events.h around CLAP_EVENT_NOTE_CHOKE — the
                // velocity field is spec-ignored.
                const auto ev = load_event<clap_event_note_t>(hdr);
                auto me = midi::MidiEvent::note_off(
                    static_cast<uint8_t>(ev.channel),
                    static_cast<uint8_t>(ev.key),
                    0);
                me.sample_offset = static_cast<int32_t>(hdr->time);
                midi_in.add(me);
            } else if (hdr->type == CLAP_EVENT_NOTE_EXPRESSION) {
                // CLAP per-note expression. Pulp already has an MPE
                // sidecar (`MpeBuffer`); map the expression to the
                // closest MIDI 1.0 message and let the MPE tracker turn
                // it into per-note state below. Only do this when the
                // plugin opted into MPE — otherwise there is no sensible
                // channel-wide interpretation (e.g. a per-note pressure
                // would collapse onto whichever voice happens to share
                // the channel). Plugins that consume UMP can access
                // per-note expressions natively via the UMP sidecar;
                // future work can extend that path.
                const auto ev = load_event<clap_event_note_expression_t>(hdr);
                if (!self->mpe_enabled) {
                    if (!note_expression_drop_logged) {
                        runtime::log_debug(
                            "CLAP: dropping CLAP_EVENT_NOTE_EXPRESSION; "
                            "plugin did not opt in to MPE");
                        note_expression_drop_logged = true;
                    }
                } else {
                    const auto channel = static_cast<uint8_t>(
                        ev.channel < 0 ? 0 : ev.channel & 0x0F);
                    // NOTE: ev.key selects the target note for per-note
                    // routing. The MIDI 1.0 synthesis path below emits
                    // channel-wide messages (channel AT, pitch bend,
                    // CCs); the MpeVoiceTracker narrows them back to the
                    // matching member-channel voice by convention. A
                    // future pass can route via the UMP sidecar for true
                    // per-note fidelity.
                    const auto t = static_cast<int32_t>(hdr->time);
                    midi::MidiEvent me{};
                    bool emitted = false;
                    switch (ev.expression_id) {
                        case CLAP_NOTE_EXPRESSION_PRESSURE: {
                            // 0..1 → channel aftertouch (7-bit).
                            const double v = std::clamp(ev.value, 0.0, 1.0);
                            const uint8_t v7 = static_cast<uint8_t>(v * 127.0 + 0.5);
                            me = {choc::midi::ShortMessage(
                                static_cast<uint8_t>(0xD0 | (channel & 0x0F)),
                                v7, 0), t, 0.0};
                            emitted = true;
                            break;
                        }
                        case CLAP_NOTE_EXPRESSION_TUNING: {
                            // ±120 semitones → 14-bit pitch bend clamped
                            // to ±2 semitones default MPE member range.
                            // We normalise the raw value to the ±48st
                            // member-bend default so the MpeVoiceTracker
                            // expands it back correctly.
                            const double norm = std::clamp(
                                ev.value / static_cast<double>(
                                    midi::MpeVoiceTracker::kDefaultMemberBendSemitones),
                                -1.0, 1.0);
                            const int bend14 = static_cast<int>(
                                std::lround(8192.0 + norm * 8191.0));
                            const auto pb = static_cast<uint16_t>(
                                std::clamp(bend14, 0, 16383));
                            me = midi::MidiEvent::pitch_bend(channel, pb);
                            me.sample_offset = t;
                            emitted = true;
                            break;
                        }
                        case CLAP_NOTE_EXPRESSION_BRIGHTNESS: {
                            // Brightness / timbre → CC 74.
                            const double v = std::clamp(ev.value, 0.0, 1.0);
                            const auto v7 = static_cast<uint8_t>(v * 127.0 + 0.5);
                            me = midi::MidiEvent::cc(channel, 74, v7);
                            me.sample_offset = t;
                            emitted = true;
                            break;
                        }
                        case CLAP_NOTE_EXPRESSION_VOLUME: {
                            // Volume (log domain in CLAP spec) → CC 7.
                            const double v = std::clamp(ev.value, 0.0, 4.0);
                            const auto v7 = static_cast<uint8_t>(
                                (v / 4.0) * 127.0 + 0.5);
                            me = midi::MidiEvent::cc(channel, 7, v7);
                            me.sample_offset = t;
                            emitted = true;
                            break;
                        }
                        case CLAP_NOTE_EXPRESSION_PAN: {
                            // 0..1 → CC 10.
                            const double v = std::clamp(ev.value, 0.0, 1.0);
                            const auto v7 = static_cast<uint8_t>(v * 127.0 + 0.5);
                            me = midi::MidiEvent::cc(channel, 10, v7);
                            me.sample_offset = t;
                            emitted = true;
                            break;
                        }
                        case CLAP_NOTE_EXPRESSION_VIBRATO:
                        case CLAP_NOTE_EXPRESSION_EXPRESSION:
                        default:
                            // Vibrato / expression and any future/unknown
                            // IDs have no universally sensible MIDI 1.0
                            // mapping; UMP-aware plugins should consume
                            // these via the native UMP sidecar.
                            break;
                    }
                    if (emitted) {
                        midi_in.add(me);
                    }
                }
            } else if (hdr->type == CLAP_EVENT_MIDI) {
                // Raw MIDI 1.0 event — the host's fallback channel for
                // CC, pitch bend, channel aftertouch, poly aftertouch,
                // and program change. `port_index` is informational for
                // multi-port plugins; single-port plugins accept all.
                const auto ev = load_event<clap_event_midi_t>(hdr);
                midi::MidiEvent me{
                    choc::midi::ShortMessage(ev.data[0], ev.data[1], ev.data[2]),
                    static_cast<int32_t>(hdr->time),
                    0.0};
                midi_in.add(me);
            } else if (hdr->type == CLAP_EVENT_MIDI_SYSEX) {
                // Workstream 01 — route CLAP sysex into MidiBuffer's
                // variable-length sidecar (issue #239).
                const auto ev = load_event<clap_event_midi_sysex_t>(hdr);
                if (ev.buffer && ev.size > 0) {
                    midi_in.add_sysex(
                        std::vector<uint8_t>(ev.buffer, ev.buffer + ev.size),
                        static_cast<int32_t>(hdr->time),
                        0.0);
                }
#if defined(CLAP_VERSION_GE) && CLAP_VERSION_GE(1, 1, 0)
            } else if (hdr->type == CLAP_EVENT_MIDI2) {
                // Native MIDI 2.0 UMP packet — append directly to the
                // UMP sidecar when the plugin opted in. We flag
                // host_delivered_ump so the MIDI 1.0 → UMP synthesis
                // path below does not double-append the same voice
                // events. CLAP_EVENT_MIDI2 is an enum value (not a
                // #define), so we gate on CLAP_VERSION_GE(1,1,0), which
                // is the release that introduced it.
                if (self->ump_enabled) {
                    const auto ev = load_event<clap_event_midi2_t>(hdr);
                    midi::UmpPacket p{};
                    p.words[0] = ev.data[0];
                    p.words[1] = ev.data[1];
                    p.words[2] = ev.data[2];
                    p.words[3] = ev.data[3];
                    p.word_count = midi::UmpPacket::size_for_type(p.message_type());
                    self->ump_buffer.add(p, static_cast<int32_t>(hdr->time));
                    host_delivered_ump = true;
                }
#endif
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

    // UMP sidecar. The host can deliver three event streams in the same
    // block:
    //   * CLAP_EVENT_NOTE_ON / _OFF — collected into midi_in.
    //   * CLAP_EVENT_MIDI (raw MIDI 1.0 channel-voice + CC/PB/AT) —
    //     also collected into midi_in.
    //   * CLAP_EVENT_MIDI2 — appended directly to self->ump_buffer
    //     during the event loop above (sets host_delivered_ump=true).
    //
    // CLAP does not require hosts to consolidate notes-vs-CCs into a
    // single transport. A real host may emit notes via CLAP_EVENT_NOTE_*
    // and CCs via CLAP_EVENT_MIDI2, expecting both halves to reach a
    // supports_ump processor. The earlier "skip midi1_to_ump when any
    // MIDI2 was delivered" branch silently dropped the note half of
    // those mixed streams from the UMP buffer (Codex review on PR #627).
    //
    // Convert midi_in → UMP unconditionally so a supports_ump processor
    // sees the union. The CLAP spec guarantees the host won't redundantly
    // encode the same logical event in two transports, so duplicates
    // don't arise from a spec-conformant host. host_delivered_ump is
    // retained as a hint for future MIDI2-aware filtering (e.g. only
    // synthesize event types not present in the native stream) but no
    // longer gates the synthesis itself.
    if (self->ump_enabled) {
        midi::midi1_to_ump(midi_in, self->ump_buffer);
        (void)host_delivered_ump;
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

        // Bridge processor-emitted MIDI (midi_out) back to the host.
        // Before this code, plugins that produced CC, pitch bend, channel
        // aftertouch, program change, etc. had those events silently
        // dropped by the CLAP adapter.
        //
        // CLAP's out_events contract requires events to be pushed in
        // ascending sample-offset order across ALL event types in the
        // block. The earlier two-pass shape (all shorts, then all sysex)
        // sorted shorts internally but emitted the entire sysex tail
        // afterward, which violated the contract whenever a sysex
        // scheduled at offset N preceded a short scheduled at offset
        // N+1 (Codex P2 review on PR #627). Hosts that strictly enforce
        // the ordering reject out-of-order events; lenient hosts get
        // wrong timing.
        //
        // Merge the two streams in (sample_offset, kind) order and
        // push as we go. midi_out's short and sysex vectors are each
        // already sorted (sort() runs below for shorts; sysex entries
        // are appended in source-sample order by the processor); we
        // walk them with a two-cursor merge.
        midi_out.sort();
        const auto& shorts = midi_out;
        const auto& sysexes = midi_out.sysex();
        std::size_t si = 0;        // short cursor
        std::size_t xi = 0;        // sysex cursor
        const std::size_t s_end = shorts.size();
        const std::size_t x_end = sysexes.size();
        auto sample_at = [](int32_t off) -> int32_t { return off < 0 ? 0 : off; };

        auto emit_short = [&](const midi::MidiEvent& me) {
            clap_event_midi_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.type = CLAP_EVENT_MIDI;
            ev.header.time = static_cast<uint32_t>(sample_at(me.sample_offset));
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.flags = 0;
            ev.port_index = 0;
            ev.data[0] = me.data()[0];
            ev.data[1] = me.size() > 1 ? me.data()[1] : uint8_t{0};
            ev.data[2] = me.size() > 2 ? me.data()[2] : uint8_t{0};
            out_events->try_push(out_events, &ev.header);
        };

        auto emit_sysex = [&](const auto& se) {
            if (se.data.empty()) return;
            clap_event_midi_sysex_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.type = CLAP_EVENT_MIDI_SYSEX;
            ev.header.time = static_cast<uint32_t>(sample_at(se.sample_offset));
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.flags = 0;
            ev.port_index = 0;
            ev.buffer = se.data.data();
            ev.size = static_cast<uint32_t>(se.data.size());
            out_events->try_push(out_events, &ev.header);
        };

        while (si < s_end || xi < x_end) {
            const bool take_short =
                xi >= x_end ||
                (si < s_end && sample_at(shorts.begin()[si].sample_offset) <=
                                 sample_at(sysexes[xi].sample_offset));
            if (take_short) {
                emit_short(shorts.begin()[si]);
                ++si;
            } else {
                emit_sysex(sysexes[xi]);
                ++xi;
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
