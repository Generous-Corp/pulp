// CLAP Adapter implementation
// Implements the CLAP C API wrapping a Pulp Processor

#include "clap_remote_controls.hpp"

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>
#include <clap/ext/preset-load.h>
#include <algorithm>
#include <array>
#include <span>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace pulp::format::clap_adapter {

namespace {
constexpr std::size_t kRealtimeMidiEventCapacity = state::ParameterEventQueue::kCapacity;
constexpr std::size_t kRealtimeMidiSysexCapacity = 128;
constexpr std::size_t kRealtimeMidiSysexPayloadCapacity = 4096;

void resize_f64_boundary_scratch(
    std::array<std::vector<float>, kMaxChannels>& scratch,
    std::size_t frames) {
    for (auto& channel : scratch) channel.assign(frames, 0.0f);
}

void copy_f64_to_f32(const double* src, float* dst, uint32_t count) {
    if (!src || !dst) return;
    for (uint32_t i = 0; i < count; ++i)
        dst[i] = static_cast<float>(src[i]);
}

void copy_f32_to_f64(const float* src, double* dst, uint32_t count) {
    if (!src || !dst) return;
    for (uint32_t i = 0; i < count; ++i)
        dst[i] = static_cast<double>(src[i]);
}

void zero_f64(double* dst, uint32_t count) {
    if (!dst) return;
    std::fill_n(dst, count, 0.0);
}
}

static PulpClapPlugin* get_self(const clap_plugin_t* plugin) {
    return static_cast<PulpClapPlugin*>(plugin->plugin_data);
}

static void clear_midi_event_buffers(PulpClapPlugin& self) {
    self.midi_in.clear();
    self.midi_in.clear_sysex();
    self.midi_out.clear();
    self.midi_out.clear_sysex();
}

// Read a CLAP event by value from the (possibly-misaligned) header
// pointer handed out by `clap_input_events.get()`. Hosts may pack
// variable-size events back-to-back without padding to the strictest
// member alignment, so `reinterpret_cast<const Foo*>(hdr)` can yield a
// pointer that violates alignof(Foo) — access to any 8-byte member
// (e.g. `double velocity` in `clap_event_note_t`) is then UB. UBSan
// caught this in the CLAP MIDI event tests. memcpy into a stack local
// guarantees proper alignment.
template <typename T>
static T load_event(const clap_event_header_t* hdr) {
    // Guard against a short/malformed host event: copying sizeof(T) bytes when
    // hdr->size < sizeof(T) reads past the event (OOB / UB on the audio
    // thread). Copy only the bytes the host actually provided; the rest stay
    // zero-initialised. memcpy-into-a-stack-local also fixes alignment.
    T out{};
    std::memcpy(&out, hdr, std::min(static_cast<std::size_t>(hdr->size), sizeof(T)));
    return out;
}

bool clap_init(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    self->processor = self->factory();
    if (!self->processor) return false;
    self->processor->set_state_store(&self->store);
    self->processor->define_parameters(self->store);

    // Resolve host accommodations once via the runtime policy
    // (PULP_HOST_QUIRKS env / set_host_quirk_policy API).
    {
        const auto host_info = detect_host_info();
        self->host_quirks = resolved_quirks(host_info.type, host_info.version);
    }

    // Inject an automatable Bypass param when the plugin declared none, then
    // detect it (the same boolean-range heuristic VST3/AU use) so
    // clap_process() can honor it with a pass-through short-circuit. No-op
    // when the quirk is off.
    maybe_synthesize_bypass(self->store, self->host_quirks);
    for (const auto& p : self->store.all_params()) {
        // Declared designation wins; falls back to the legacy name/range
        // heuristic for params that declare none (see is_bypass_param).
        if (state::is_bypass_param(p)) {
            self->bypass_param_id = p.id;
            break;
        }
    }

    // Opt this plugin instance into the process-wide MainThreadDispatcher
    // backend. On macOS this installs (or refcount-bumps) a Cocoa backend
    // posting to `dispatch_get_main_queue`, so any
    // `MainThreadDispatcher::call_async` posted while a Pulp CLAP plugin is
    // loaded inside the DAW reaches the host's main thread. On non-mac
    // platforms this is a no-op (returns 0) and the dispatcher stays in "no
    // backend" state until a platform-specific path lands.
    self->main_thread_token = pulp::events::register_plugin_backend();

    // Create PresetManager from plugin descriptor
    auto desc = self->processor->descriptor();
    self->preset_manager = std::make_unique<state::PresetManager>(
        self->store, desc.manufacturer, desc.name);

    const auto caps = desc.effective_capabilities();

    // Wire MPE sidecar when the plugin opts in.
    self->mpe_enabled = caps.supports_mpe;
    if (self->mpe_enabled) {
        midi::bind_tracker_to_buffer(
            self->mpe_tracker, self->mpe_buffer, self->mpe_current_sample_offset);
        runtime::log_info("CLAP: MPE sidecar enabled for '{}'", desc.name);
    }

    // Wire UMP sidecar when the plugin opts in.
    self->ump_enabled = caps.supports_ump;
    if (self->ump_enabled) {
        runtime::log_info("CLAP: UMP sidecar enabled for '{}'", desc.name);
    }

    runtime::log_info("CLAP: initialized '{}'", desc.name);
    return true;
}

void clap_destroy(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    // Symmetric teardown of the MainThreadDispatcher backend installed in
    // clap_init().
    if (self->main_thread_token != 0) {
        pulp::events::unregister_plugin_backend(self->main_thread_token);
        self->main_thread_token = 0;
    }
    delete self;
}

bool clap_activate(const clap_plugin_t* plugin, double sr, uint32_t, uint32_t max_frames) {
    auto* self = get_self(plugin);
    self->sample_rate = sr;
    self->max_buffer_size = static_cast<int>(max_frames);
    clear_midi_event_buffers(*self);
    self->midi_in.reserve(kRealtimeMidiEventCapacity,
                          kRealtimeMidiSysexCapacity,
                          kRealtimeMidiSysexPayloadCapacity);
    self->midi_out.reserve(kRealtimeMidiEventCapacity,
                           kRealtimeMidiSysexCapacity,
                           kRealtimeMidiSysexPayloadCapacity);
    self->midi_in.set_realtime_capacity_limit(true);
    self->midi_out.set_realtime_capacity_limit(true);
    self->mpe_buffer.reserve(kRealtimeMidiEventCapacity);
    self->ump_buffer.reserve(kRealtimeMidiEventCapacity);
    self->mpe_buffer.set_realtime_capacity_limit(true);
    self->ump_buffer.set_realtime_capacity_limit(true);
    self->param_snapshot.reserve(self->store.all_params().size());
    // Sized here, not on the audio thread: process() calls assign()/resize() to
    // all_params().size() at block start, which would grow-and-allocate on the
    // first block without this reserve (param_snapshot mirrors it above).
    self->output_param_has_event.reserve(self->store.all_params().size());

    auto desc = self->processor->descriptor();
    PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(max_frames);
    ctx.input_channels = desc.default_input_channels();
    ctx.output_channels = desc.default_output_channels();
    self->processor->prepare(ctx);
    self->processor->prepare_f64_fallback_scratch(ctx);
    self->native_f64_enabled = desc.effective_capabilities().supports_f64_audio;

    const auto scratch_frames = static_cast<std::size_t>(max_frames);
    resize_f64_boundary_scratch(self->f64_input_scratch, scratch_frames);
    resize_f64_boundary_scratch(self->f64_output_scratch, scratch_frames);
    resize_f64_boundary_scratch(self->f64_sidechain_scratch, scratch_frames);
    for (auto& bus : self->f64_aux_output_scratch) {
        resize_f64_boundary_scratch(bus, scratch_frames);
    }

    // Cache the descriptor-declared channel count of each secondary (aux)
    // output bus so clap_process() can report a bus's declared layout without
    // calling descriptor() — which allocates — on the audio thread. Index i
    // corresponds to host output bus i+1 (the main bus at index 0 is handled
    // separately). Buses beyond the preallocated row count are not routed.
    for (int i = 0; i < kMaxOutputBuses - 1; ++i) {
        const std::size_t bus_index = static_cast<std::size_t>(i) + 1;
        self->declared_aux_channels[i] =
            bus_index < desc.output_buses.size()
                ? desc.output_buses[bus_index].default_channels
                : 0;
    }

    // Size the per-channel dry delay used by the bypass pass-through. The host
    // compensates the plugin path by its reported latency, so the bypassed dry
    // copy must be delayed by exactly that many samples to stay aligned with
    // the host's plugin-delay-compensation. Allocate here, never in
    // clap_process(); a 0 latency leaves the delay lines unused so the bypass
    // pass-through stays a zero-copy memcpy.
    self->bypass_delay_samples = reported_latency_samples(
        self->processor->latency_samples(), self->host_quirks);
    if (self->bypass_delay_samples < 0) self->bypass_delay_samples = 0;
    if (self->bypass_delay_samples > 0) {
        for (auto& line : self->bypass_dry_delay) {
            line.prepare(self->bypass_delay_samples);
        }
    }

    // Reset the playhead snapshot — the next process() call is the
    // first block of a fresh activation cycle and must NOT diff against
    // stale transport data from the previous activation. Without this,
    // the first block after a deactivate / reactivate (or reset) would
    // spuriously raise tempo_changed / time_sig_changed /
    // transport_changed against the prior session's last block.
    self->playhead_prev = {};
    return true;
}

void clap_deactivate(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    self->processor->release();
    clear_midi_event_buffers(*self);
    self->playhead_prev = {};
}

bool clap_start_processing(const clap_plugin_t*) { return true; }
void clap_stop_processing(const clap_plugin_t*) {}
void clap_reset(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    clear_midi_event_buffers(*self);
    self->playhead_prev = {};
}

bool clap_param_modulation_lane(const PulpClapPlugin& self,
                                const clap_event_param_mod_t& event,
                                state::ModulationLane& lane) {
    const auto param_id = static_cast<state::ParamID>(event.param_id);
    const auto* info = self.store.info(param_id);
    if (!info) {
        return false;
    }

    lane = state::ModulationLane{
        .source = {
            .id = kClapHostModulationSourceId,
            .scope = state::ModulationScope::Global,
            .rate = state::ModulationRate::Control,
            .units = "CLAP PARAM_MOD",
        },
        .target = {
            .param_id = param_id,
            .scope = state::ModulationScope::Global,
            .param_rate = info->rate,
            .modulatable = info->range.step <= 0.0f,
            .writable = true,
            .units = info->unit,
        },
        .mix = state::ModulationMixMode::Add,
        .depth = static_cast<float>(event.amount),
    };
    return state::validate_modulation_lane(lane).accepted;
}

clap_process_status clap_process(const clap_plugin_t* plugin, const clap_process_t* process) {
    auto* self = get_self(plugin);
    if (!self->processor) return CLAP_PROCESS_ERROR;

    auto num_samples = process->frames_count;
    if (num_samples == 0) return CLAP_PROCESS_CONTINUE;

    // Flush denormals to zero for the whole audio-callback body so quiet tails
    // in recursive filter/reverb/feedback state can't stall the host's audio
    // thread, then restore its prior FP mode on scope exit. See
    // docs/guides/dsp-threading.md "Numeric mode".
    pulp::signal::ScopedFlushDenormals flush_denormals;

    // Max-frames contract guard (defensive; well-behaved hosts never exceed the
    // advertised max). The Processor and all its scratch buffers were sized in
    // clap_activate() to self->max_buffer_size. A render larger than that would
    // overrun them and corrupt DSP state. CLAP has no clean per-block reject, so
    // clamp the processed region to the prepared max and zero the un-processable
    // tail [max, original) on every main output channel below so it reads back
    // as clean silence rather than garbage.
    const auto original_num_samples = num_samples;
    if (self->max_buffer_size > 0 &&
        num_samples > static_cast<uint32_t>(self->max_buffer_size)) {
        num_samples = static_cast<uint32_t>(self->max_buffer_size);
    }

    // Reset per-buffer modulation offsets before applying new events
    self->store.reset_all_mod();
    self->param_events.clear();

    // Handle parameter, modulation, and gesture events from host
    auto* in_events = process->in_events;
    if (in_events) {
        uint32_t event_count = in_events->size(in_events);
        for (uint32_t i = 0; i < event_count; ++i) {
            auto* hdr = in_events->get(in_events, i);
            // CLAP event-space gate: non-zero namespace IDs belong to
            // third-party extensions we don't implement. Per spec, those
            // must be ignored — including their types, which may alias
            // core type IDs. clap-validator `param-set-wrong-namespace`.
            if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
                const auto ev = load_event<clap_event_param_value_t>(hdr);
                self->param_events.push({
                    static_cast<state::ParamID>(ev.param_id),
                    static_cast<int32_t>(hdr->time),
                    static_cast<float>(ev.value),
                });
                // RT-safe write: atomic store + non-allocating SPSC push
                // for Main listeners. The editor calls store.pump_listeners()
                // from its UI tick to deliver the queued notifications;
                // Audio listeners fire inline here. Replaces set_value(),
                // which dispatches a heap-allocated lambda through the
                // EventLoop when a Main listener is attached.
                self->store.set_value_rt(
                    static_cast<state::ParamID>(ev.param_id),
                    static_cast<float>(ev.value));
            } else if (hdr->type == CLAP_EVENT_PARAM_MOD) {
                const auto ev = load_event<clap_event_param_mod_t>(hdr);
                state::ModulationLane lane;
                if (clap_param_modulation_lane(*self, ev, lane)) {
                    self->store.set_mod_offset(
                        static_cast<state::ParamID>(ev.param_id),
                        static_cast<float>(ev.amount));
                }
            } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
                const auto ev = load_event<clap_event_param_gesture_t>(hdr);
                self->store.begin_gesture(static_cast<state::ParamID>(ev.param_id));
            } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_END) {
                const auto ev = load_event<clap_event_param_gesture_t>(hdr);
                self->store.end_gesture(static_cast<state::ParamID>(ev.param_id));
            }
        }
    }
    self->param_events.sort();

    // Build audio buffer views (no allocation — uses pre-allocated arrays).
    // Bus 0 routes to the main input/output; bus 1 (when present) routes to
    // Processor::set_sidechain(). Additional input buses beyond index 1 are
    // currently ignored — the Processor API exposes a single sidechain slot.
    int in_channels = 0, out_channels = 0;
    int sc_channels = 0;
    bool main_input_f64 = false;
    bool main_output_f64 = false;
    bool sidechain_f64 = false;
    std::array<bool, kMaxOutputBuses - 1> aux_output_f64{};

    if (process->audio_inputs_count > 0) {
        auto& bus = process->audio_inputs[0];
        main_input_f64 = bus.data64 != nullptr;
        in_channels = (std::min)(static_cast<int>(bus.channel_count), kMaxChannels);
        for (int ch = 0; ch < in_channels; ++ch) {
            if (main_input_f64) {
                const double* src = bus.data64[ch];
                self->input64_ptrs[ch] = src;
                auto* dst = self->f64_input_scratch[static_cast<std::size_t>(ch)].data();
                copy_f64_to_f32(src, dst, num_samples);
                self->input_ptrs[ch] = src ? dst : nullptr;
            } else {
                self->input64_ptrs[ch] = nullptr;
                self->input_ptrs[ch] = bus.data32 ? bus.data32[ch] : nullptr;
            }
        }
    }
    if (process->audio_inputs_count > 1) {
        auto& sc_bus = process->audio_inputs[1];
        // Guard against a host that reports a sidechain bus but hands us a null
        // data pointer (bus deactivated). Mirrors the defensive guard the VST3
        // adapter already has.
        sidechain_f64 = sc_bus.data64 != nullptr;
        if ((sidechain_f64 && sc_bus.data64) || (!sidechain_f64 && sc_bus.data32)) {
            sc_channels = (std::min)(static_cast<int>(sc_bus.channel_count), kMaxChannels);
            for (int ch = 0; ch < sc_channels; ++ch) {
                if (sidechain_f64) {
                    const double* src = sc_bus.data64[ch];
                    self->sidechain64_ptrs[ch] = src;
                    auto* dst = self->f64_sidechain_scratch[static_cast<std::size_t>(ch)].data();
                    copy_f64_to_f32(src, dst, num_samples);
                    self->sidechain_ptrs[ch] = src ? dst : nullptr;
                } else {
                    self->sidechain64_ptrs[ch] = nullptr;
                    self->sidechain_ptrs[ch] = sc_bus.data32[ch];
                }
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
        main_output_f64 = bus.data64 != nullptr;
        out_channels = (std::min)(static_cast<int>(bus.channel_count), kMaxChannels);
        for (int ch = 0; ch < out_channels; ++ch) {
            if (main_output_f64) {
                self->output64_ptrs[ch] =
                    bus.data64 && bus.data64[ch] ? bus.data64[ch] : nullptr;
                auto& scratch = self->f64_output_scratch[static_cast<std::size_t>(ch)];
                std::fill(scratch.begin(), scratch.end(), 0.0f);
                self->output_ptrs[ch] = bus.data64 && bus.data64[ch]
                    ? scratch.data()
                    : nullptr;
            } else {
                self->output64_ptrs[ch] = nullptr;
                self->output_ptrs[ch] = bus.data32 ? bus.data32[ch] : nullptr;
            }
        }
        // When the render was clamped above, the processor only writes the
        // first num_samples frames; zero the un-processable tail on every main
        // output channel so the host reads silence past the prepared max.
        if (original_num_samples > num_samples) {
            for (int ch = 0; ch < out_channels; ++ch) {
                if (main_output_f64) {
                    if (bus.data64 && bus.data64[ch]) {
                        zero_f64(bus.data64[ch] + num_samples,
                                 original_num_samples - num_samples);
                    }
                } else if (self->output_ptrs[ch]) {
                    std::memset(self->output_ptrs[ch] + num_samples, 0,
                                sizeof(float) * (original_num_samples - num_samples));
                }
            }
        }
    }
    // Secondary (aux) output buses route to the Processor's richer
    // process(ProcessBuffers&) surface for multi-out instruments (drum
    // machines, multitimbral, stem renderers). Each routed aux bus is
    // pre-zeroed here first so that:
    //   * a single-output Processor (the default process(ProcessBuffers&),
    //     which only writes the main output) leaves aux buses silent rather
    //     than handing the host uninitialised memory, and
    //   * a multi-out Processor that writes only some aux channels still emits
    //     silence on the channels it skipped.
    // Host output buses beyond kMaxOutputBuses are zero-filled but not routed,
    // preserving the prior safe behaviour for unusually wide bus layouts.
    const uint32_t routed_output_buses =
        (std::min)(process->audio_outputs_count,
                   static_cast<uint32_t>(kMaxOutputBuses));
    for (uint32_t b = 1; b < process->audio_outputs_count; ++b) {
        auto& bus = process->audio_outputs[b];
        // An inactive/deactivated bus can report channel_count > 0 while
        // data32/data64 == nullptr. Guard the bus pointer before indexing it so the
        // pre-zero loop never dereferences null on the audio thread.
        const bool bus_f64 = bus.data64 != nullptr;
        if ((b - 1) < aux_output_f64.size()) {
            aux_output_f64[b - 1] = bus_f64;
        }
        if ((bus_f64 && !bus.data64) || (!bus_f64 && !bus.data32)) continue;
        for (uint32_t ch = 0; ch < bus.channel_count; ++ch) {
            if (bus_f64) {
                if (bus.data64[ch]) {
                    zero_f64(bus.data64[ch], original_num_samples);
                }
            } else if (bus.data32[ch]) {
                std::memset(bus.data32[ch], 0, sizeof(float) * original_num_samples);
            }
        }
    }

    audio::BufferView<const float> input_view(
        self->input_ptrs, in_channels, num_samples);
    audio::BufferView<float> output_view(
        self->output_ptrs, out_channels, num_samples);
    audio::BufferView<const float> sidechain_view(
        self->sidechain_ptrs, sc_channels, num_samples);
    std::array<ProcessBusBufferView<const float>, 2> input_buses{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main,
                     in_channels, false, process->audio_inputs_count > 0},
            .buffer = input_view,
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain,
                     sc_channels, true, sc_channels > 0},
            .buffer = sidechain_view,
        },
    }};
    // Build one ProcessBusBufferView per routed output bus. Index 0 is the
    // main output (already wired into output_view above); each subsequent entry
    // wires the host's aux output channel pointers into per-bus storage.
    std::array<ProcessBusBufferView<float>, kMaxOutputBuses> output_buses{};
    output_buses[0] = {
        .info = {"Main Out", 0, BusDirection::Output, BusRole::Main,
                 out_channels, false, process->audio_outputs_count > 0},
        .buffer = output_view,
    };
    for (uint32_t b = 1; b < routed_output_buses; ++b) {
        auto& bus = process->audio_outputs[b];
        int aux_channels =
            (std::min)(static_cast<int>(bus.channel_count), kMaxChannels);
        // Row b-1 backs host output bus b (the main bus at index 0 uses
        // output_ptrs, not this storage).
        float** ptrs = self->aux_output_ptrs[b - 1];
        const bool bus_f64 = aux_output_f64[b - 1];
        bool active = bus_f64 ? bus.data64 != nullptr : bus.data32 != nullptr;
        if (active) {
            for (int ch = 0; ch < aux_channels; ++ch) {
                if (bus_f64) {
                    self->aux_output64_ptrs[b - 1][ch] =
                        bus.data64 && bus.data64[ch] ? bus.data64[ch] : nullptr;
                    auto& scratch =
                        self->f64_aux_output_scratch[b - 1][static_cast<std::size_t>(ch)];
                    std::fill(scratch.begin(), scratch.end(), 0.0f);
                    ptrs[ch] = bus.data64[ch] ? scratch.data() : nullptr;
                } else {
                    self->aux_output64_ptrs[b - 1][ch] = nullptr;
                    ptrs[ch] = bus.data32[ch];
                }
                if (!ptrs[ch]) {
                    // A null per-channel pointer demotes the whole bus to
                    // inactive so the Processor sees an empty bus rather than a
                    // half-valid BufferView.
                    active = false;
                    break;
                }
            }
        }
        if (!active) aux_channels = 0;
        // declared_channels reports the descriptor's declared layout (cached in
        // clap_activate); buffer.num_channels() carries the actual routed count.
        // Keeping these distinct lets matches_declared_layout() detect a
        // host-vs-declared channel-count mismatch instead of being tautological.
        output_buses[b] = {
            .info = {"Aux Out", b, BusDirection::Output, BusRole::Aux,
                     self->declared_aux_channels[b - 1], true, active},
            .buffer = audio::BufferView<float>(ptrs, aux_channels, num_samples),
        };
    }
    ProcessBuffers process_buffers{
        .inputs = ProcessBusBufferSet<const float>(input_buses),
        .outputs = ProcessBusBufferSet<float>(
            std::span(output_buses.data(), routed_output_buses)),
    };
    bool has_host_f64 = main_input_f64 || main_output_f64 || sidechain_f64;
    bool all_active_routed_buses_are_f64 = true;
    if (process->audio_inputs_count > 0 && in_channels > 0 && !main_input_f64) {
        all_active_routed_buses_are_f64 = false;
    }
    if (process->audio_outputs_count > 0 && out_channels > 0 && !main_output_f64) {
        all_active_routed_buses_are_f64 = false;
    }
    if (sc_channels > 0 && !sidechain_f64) {
        all_active_routed_buses_are_f64 = false;
    }

    audio::BufferView<const double> input64_view(
        self->input64_ptrs, in_channels, num_samples);
    audio::BufferView<double> output64_view(
        self->output64_ptrs, out_channels, num_samples);
    audio::BufferView<const double> sidechain64_view(
        self->sidechain64_ptrs, sc_channels, num_samples);
    std::array<ProcessBusBufferView<const double>, 2> input64_buses{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main,
                     in_channels, false, process->audio_inputs_count > 0},
            .buffer = input64_view,
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain,
                     sc_channels, true, sc_channels > 0},
            .buffer = sidechain64_view,
        },
    }};
    std::array<ProcessBusBufferView<double>, kMaxOutputBuses> output64_buses{};
    output64_buses[0] = {
        .info = {"Main Out", 0, BusDirection::Output, BusRole::Main,
                 out_channels, false, process->audio_outputs_count > 0},
        .buffer = output64_view,
    };
    for (uint32_t b = 1; b < routed_output_buses; ++b) {
        auto& bus = process->audio_outputs[b];
        int aux_channels =
            (std::min)(static_cast<int>(bus.channel_count), kMaxChannels);
        double** ptrs = self->aux_output64_ptrs[b - 1];
        bool active = aux_output_f64[b - 1] && bus.data64 != nullptr;
        if (active) {
            has_host_f64 = true;
            for (int ch = 0; ch < aux_channels; ++ch) {
                ptrs[ch] = bus.data64[ch];
                if (!ptrs[ch]) {
                    active = false;
                    break;
                }
            }
        }
        if (output_buses[b].active() && !aux_output_f64[b - 1]) {
            all_active_routed_buses_are_f64 = false;
        }
        if (!active) aux_channels = 0;
        output64_buses[b] = {
            .info = {"Aux Out", b, BusDirection::Output, BusRole::Aux,
                     self->declared_aux_channels[b - 1], true, active},
            .buffer = audio::BufferView<double>(ptrs, aux_channels, num_samples),
        };
    }
    ProcessBuffers64 process_buffers64{
        .inputs = ProcessBusBufferSet<const double>(input64_buses),
        .outputs = ProcessBusBufferSet<double>(
            std::span(output64_buses.data(), routed_output_buses)),
    };
    const bool native_f64 =
        self->native_f64_enabled && has_host_f64 && all_active_routed_buses_are_f64;

    // Build MIDI from CLAP note events. Reuse per-instance scratch buffers so
    // capacity survives warmup and the steady-state process path stays RT-safe.
    auto& midi_in = self->midi_in;
    auto& midi_out = self->midi_out;
    clear_midi_event_buffers(*self);
    const uint32_t event_count = in_events ? in_events->size(in_events) : 0;
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
        for (uint32_t i = 0; i < event_count; ++i) {
            auto* hdr = in_events->get(in_events, i);
            // Same CLAP event-space gate as the param/gesture loop above.
            if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
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
                // CLAP gives us a host-owned payload pointer. midi_in owns a
                // preallocated payload pool so the copy below does not allocate
                // on the process path.
                const auto ev = load_event<clap_event_midi_sysex_t>(hdr);
                if (ev.buffer && ev.size > 0) {
                    midi_in.add_sysex_copy(
                        ev.buffer,
                        ev.size,
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
    ctx.process_mode = pulp::format::ProcessMode::Realtime;
    ctx.render_speed_hint = pulp::format::RenderSpeedHint::Realtime;
    if (process->transport) {
        const auto* tr = process->transport;
        const uint32_t flags = tr->flags;

        ctx.is_playing = (flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        ctx.is_recording = (flags & CLAP_TRANSPORT_IS_RECORDING) != 0;
        if (flags & CLAP_TRANSPORT_HAS_TEMPO) {
            ctx.tempo_bpm = tr->tempo;
        }
        if (flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
            ctx.position_beats =
                static_cast<double>(tr->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        }
        if (flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) {
            const double seconds =
                static_cast<double>(tr->song_pos_seconds) / CLAP_SECTIME_FACTOR;
            ctx.position_samples = static_cast<int64_t>(
                std::llround(seconds * ctx.sample_rate));
        }
        if (flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
            ctx.time_sig_numerator = static_cast<int>(tr->tsig_num);
            ctx.time_sig_denominator = static_cast<int>(tr->tsig_denom);
        }

        // Cycle / loop range. CLAP gates this on CLAP_TRANSPORT_IS_LOOP_ACTIVE;
        // loop_start_beats / loop_end_beats are CLAP fixed-point
        // `clap_beattime` so they convert through the same factor as
        // song_pos_beats.
        ctx.is_looping = (flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) != 0;
        if (ctx.is_looping) {
            ctx.loop_start_beats =
                static_cast<double>(tr->loop_start_beats) / CLAP_BEATTIME_FACTOR;
            ctx.loop_end_beats =
                static_cast<double>(tr->loop_end_beats) / CLAP_BEATTIME_FACTOR;
        }

        // Bar index. CLAP exposes `bar_number` directly (bar at song pos 0 has
        // bar 0), so prefer that over deriving from beats. Hosts that don't
        // supply a beats timeline leave `bar_number` at 0, which matches the
        // documented default.
        if (flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
            ctx.bar = static_cast<int64_t>(tr->bar_number);
        } else {
            pulp::format::detail::derive_bar_from_beats(ctx);
        }

        // Host clock / SMPTE frame rate are intentionally left at the
        // documented "host did not provide" sentinels: CLAP 1.2.2's
        // `clap_event_transport` carries neither field. `ctx.host_time_ns`
        // stays 0, `ctx.frame_rate` stays FrameRate::unknown; plugin authors
        // must check before use.
    }

    // Diff against the previous block to populate the three change flags.
    // Stateful; updates `self->playhead_prev` in place.
    pulp::format::detail::compute_playhead_changes(ctx, self->playhead_prev);

    // Snapshot parameter values to detect plugin-side changes.
    // INVARIANT: every scratch vector sized to all_params().size() here runs on
    // the audio thread, so each must be reserve()d to that size in
    // clap_activate() or the first block grow-and-allocates. Adding a new
    // per-param scratch below requires a matching reserve there; the
    // `ClapSlot::process is allocation-free` rt-safety test guards this.
    auto all_params = self->store.all_params();
    self->param_snapshot.resize(all_params.size());
    self->output_param_has_event.assign(all_params.size(), 0);
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
    // those mixed streams from the UMP buffer.
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
    self->processor->set_param_events(&self->param_events);
    self->output_param_events.clear();
    self->processor->set_output_param_events(&self->output_param_events);

    // Process! Wrap the plugin call in a ScopedNoAlloc so any debug
    // hooks (operator new override, sanitizer integration) can flag
    // a plugin that allocates on the audio thread. The guard is a
    // thread-local counter — zero cost in NDEBUG, ~1 ns in debug.
    // When the Bypass param (declared or synthesized) is engaged, the adapter
    // does the pass-through itself: copy main input -> main output
    // (null-guarded), zero any output channel without a matching input, and
    // skip the Processor entirely. Mirrors the VST3 processBlockBypassed
    // behavior.
    const bool bypassed = self->bypass_param_id != 0 &&
                          self->store.get_value(self->bypass_param_id) >= 0.5f;
    if (bypassed) {
        // Bypass is pure host-buffer passthrough (no processor scratch), so it
        // safely handles the full original block rather than the clamped count.
        // The dry-delay line is sized independently of the block size (to the
        // reported latency in clap_activate()), so it too processes the full
        // original_num_samples — bypass stays a true 1:1 host passthrough with
        // no clamp tail to zero.
        const bool delayed = self->bypass_delay_samples > 0 &&
            out_channels <= static_cast<int>(self->bypass_dry_delay.size());
        for (int ch = 0; ch < out_channels; ++ch) {
            const bool has_input_channel =
                process->audio_inputs_count > 0 &&
                ch < in_channels &&
                ch < static_cast<int>(process->audio_inputs[0].channel_count);
            const double* in64 =
                has_input_channel && process->audio_inputs[0].data64
                    ? process->audio_inputs[0].data64[ch]
                    : nullptr;
            const float* in32 =
                has_input_channel && process->audio_inputs[0].data32
                    ? process->audio_inputs[0].data32[ch]
                    : nullptr;

            if (main_output_f64) {
                auto* out64 = process->audio_outputs_count > 0 &&
                                      process->audio_outputs[0].data64
                                  ? process->audio_outputs[0].data64[ch]
                                  : nullptr;
                if (out64 == nullptr) continue;
                if (in64 != nullptr) {
                    if (delayed) {
                        auto& line = self->bypass_dry_delay[ch];
                        for (uint32_t n = 0; n < original_num_samples; ++n) {
                            out64[n] = static_cast<double>(line.process(
                                static_cast<float>(in64[n]),
                                static_cast<float>(self->bypass_delay_samples)));
                        }
                    } else {
                        std::memcpy(out64, in64,
                                    sizeof(double) * original_num_samples);
                    }
                } else if (in32 != nullptr) {
                    if (delayed) {
                        auto& line = self->bypass_dry_delay[ch];
                        for (uint32_t n = 0; n < original_num_samples; ++n) {
                            out64[n] = static_cast<double>(line.process(
                                in32[n],
                                static_cast<float>(self->bypass_delay_samples)));
                        }
                    } else {
                        copy_f32_to_f64(in32, out64, original_num_samples);
                    }
                } else {
                    zero_f64(out64, original_num_samples);
                }
                continue;
            }

            auto* out32 = process->audio_outputs_count > 0 &&
                                  process->audio_outputs[0].data32
                              ? process->audio_outputs[0].data32[ch]
                              : nullptr;
            if (out32 == nullptr) continue;
            if (in32 != nullptr || in64 != nullptr) {
                if (delayed) {
                    // The plugin path is host-compensated by its reported
                    // latency, so the bypassed dry signal must be delayed by
                    // the same amount to stay sample-aligned with the host's
                    // plugin-delay-compensation. The delay line is fed only
                    // while bypassed: a one-time transient in the first
                    // bypass_delay_samples after engaging bypass is accepted
                    // (bypass toggling is a user action, not sample-critical)
                    // in exchange for a zero-cost non-bypassed path. Steady
                    // state emits input[n - latency].
                    auto& line = self->bypass_dry_delay[ch];
                    for (uint32_t n = 0; n < original_num_samples; ++n) {
                        const float sample = in32 ? in32[n] : static_cast<float>(in64[n]);
                        out32[n] = line.process(
                            sample,
                            static_cast<float>(self->bypass_delay_samples));
                    }
                } else if (in32 != nullptr) {
                    std::memcpy(out32, in32,
                                sizeof(float) * original_num_samples);
                } else {
                    copy_f64_to_f32(in64, out32, original_num_samples);
                }
            } else {
                std::memset(out32, 0, sizeof(float) * original_num_samples);
            }
        }
        self->processor->set_sidechain(nullptr);
    } else {
        pulp::runtime::ScopedNoAlloc no_alloc_guard;
        if (native_f64) {
            self->processor->process_f64(process_buffers64, midi_in, midi_out, ctx);
        } else {
            self->processor->process(process_buffers, midi_in, midi_out, ctx);
        }
        if (!native_f64 && main_output_f64 && process->audio_outputs_count > 0 &&
            process->audio_outputs[0].data64) {
            auto& bus = process->audio_outputs[0];
            for (int ch = 0; ch < out_channels; ++ch) {
                copy_f32_to_f64(self->output_ptrs[ch], bus.data64[ch], num_samples);
            }
        }
        for (uint32_t b = 1; b < routed_output_buses; ++b) {
            if (native_f64 || !aux_output_f64[b - 1]) continue;
            auto& bus = process->audio_outputs[b];
            if (!bus.data64) continue;
            int aux_channels =
                (std::min)(static_cast<int>(bus.channel_count), kMaxChannels);
            for (int ch = 0; ch < aux_channels; ++ch) {
                copy_f32_to_f64(self->aux_output_ptrs[b - 1][ch],
                                bus.data64[ch],
                                num_samples);
            }
        }
    }

    // Return trigger / momentary params (panic, reset, tap) to their default
    // now that the Processor has observed this block. Done before the
    // output-event scan below so the host records the auto-reset as automation.
    self->store.reset_triggers_rt();

    // Emit output parameter events for any values the plugin changed,
    // so the host can record automation
    auto* out_events = process->out_events;
    if (out_events) {
        // Sort the sample-accurate explicit output events (offset ascending) and
        // mark the skip-set so the offset-0 snapshot fallback below does not
        // double-report a param that already emitted explicit events. Both the
        // queue and the skip-set were sized at block start — no allocation here.
        self->output_param_events.sort();
        const int32_t out_frames = static_cast<int32_t>(process->frames_count);
        const int32_t out_last_frame = out_frames > 0 ? out_frames - 1 : 0;
        for (auto it = self->output_param_events.begin();
             it != self->output_param_events.end(); ++it) {
            for (std::size_t i = 0; i < all_params.size(); ++i) {
                if (all_params[i].id == it->param_id) {
                    self->output_param_has_event[i] = 1;
                    break;
                }
            }
        }
        // (1) Snapshot-diff fallback, at time 0, for params WITHOUT explicit
        // events. These are all at offset 0 (the earliest time), so emitting
        // them before the offset-ordered merge below preserves CLAP's global
        // ascending-time contract.
        for (std::size_t i = 0; i < all_params.size(); ++i) {
            if (self->output_param_has_event[i]) continue;
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
        // scheduled at offset N preceded a short scheduled at offset N+1. Hosts
        // that strictly enforce the ordering reject out-of-order events;
        // lenient hosts get wrong timing.
        //
        // Merge the two streams in (sample_offset, kind) order and
        // push as we go. midi_out's short and sysex vectors are each
        // already sorted (sort() runs below for shorts; sysex entries
        // are appended in source-sample order by the processor); we
        // walk them with a three-cursor merge that ALSO interleaves the
        // sample-accurate output parameter events (self->output_param_events,
        // already sorted above), so params, shorts, and sysex all emit in one
        // globally ascending sample-offset stream.
        midi_out.sort();
        const auto& shorts = midi_out;
        const auto& sysexes = midi_out.sysex();
        std::size_t pi = 0;        // output-param cursor
        std::size_t si = 0;        // short cursor
        std::size_t xi = 0;        // sysex cursor
        const std::size_t p_end = self->output_param_events.size();
        const std::size_t s_end = shorts.size();
        const std::size_t x_end = sysexes.size();
        auto sample_at = [](int32_t off) -> int32_t { return off < 0 ? 0 : off; };
        auto param_at = [&](std::size_t i) -> int32_t {
            int32_t off = self->output_param_events.begin()[i].sample_offset;
            if (off < 0) return 0;
            if (off > out_last_frame) return out_last_frame;  // clamp into block
            return off;
        };

        auto emit_param = [&](const state::ParameterEvent& pe, int32_t off) {
            clap_event_param_value_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.header.time = static_cast<uint32_t>(off);
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.flags = 0;
            ev.param_id = pe.param_id;
            ev.cookie = nullptr;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = static_cast<double>(pe.value);  // CLAP param values are plain domain
            out_events->try_push(out_events, &ev.header);
        };

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

        while (pi < p_end || si < s_end || xi < x_end) {
            // Peek the next offset of each stream (INT32_MAX = exhausted), then
            // emit the earliest. Params win ties (params <= shorts <= sysex),
            // which is a valid non-decreasing order for equal offsets.
            const int32_t po = pi < p_end ? param_at(pi) : INT32_MAX;
            const int32_t so =
                si < s_end ? sample_at(shorts.begin()[si].sample_offset) : INT32_MAX;
            const int32_t xo =
                xi < x_end ? sample_at(sysexes[xi].sample_offset) : INT32_MAX;
            if (pi < p_end && po <= so && po <= xo) {
                emit_param(self->output_param_events.begin()[pi], po);
                ++pi;
            } else if (si < s_end && so <= xo) {
                emit_short(shorts.begin()[si]);
                ++si;
            } else {
                emit_sysex(sysexes[xi]);
                ++xi;
            }
        }
    }

    // If the processor flagged a latency / tail change during this block, ask
    // the host to schedule a main-thread callback. The actual
    // `clap_host_latency->changed()` / `clap_host_tail->changed()` push runs
    // from clap_on_main_thread() so we never call host APIs from process().
    // Peek (don't consume) so the on_main_thread handler still sees the same
    // edge.
    if (self->host && self->host->request_callback && self->processor &&
        (self->processor->latency_change_pending() ||
         self->processor->tail_change_pending())) {
        self->host->request_callback(self->host);
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
    if (!plugin) return nullptr;
    if (!id) return nullptr;
    auto* self = get_self(plugin);
    if (!self) return nullptr;

    // Only expose preset-load if the plugin has a PresetManager
    if (self->preset_manager) {
        if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0) return &s_preset_load;
        if (std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0) return &s_preset_load;
    }

    if (self->store.param_count() > 0) {
        if (std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS) == 0) {
            return remote_controls_extension();
        }
        if (std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT) == 0) {
            return remote_controls_extension();
        }
    }

    // ARA companion factory. Only ask for it in builds that actually link the
    // ARA implementation; WebCLAP/WASI intentionally excludes that native TU.
#ifdef PULP_HAS_ARA
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
#else
    if (std::strcmp(id, kClapAraFactoryExtension) == 0) return nullptr;
#endif

    return nullptr;
}

void clap_on_main_thread(const clap_plugin_t* plugin) {
    // Drain RT-safe pending flags the processor may have set during process()
    // and republish to the host on the main thread. CLAP hosts call
    // request_callback() in response to the process_status flag we set when a
    // flag becomes pending, and this entrypoint runs on the main thread.
    auto* self = get_self(plugin);
    if (!self || !self->processor || !self->host) return;

    const bool latency_changed =
        self->processor->consume_latency_changed_flag();
    const bool tail_changed =
        self->processor->consume_tail_changed_flag();

    if (latency_changed) {
        auto* ext = static_cast<const clap_host_latency_t*>(
            self->host->get_extension(self->host, CLAP_EXT_LATENCY));
        if (ext && ext->changed) ext->changed(self->host);
    }
    if (tail_changed) {
        auto* ext = static_cast<const clap_host_tail_t*>(
            self->host->get_extension(self->host, CLAP_EXT_TAIL));
        if (ext && ext->changed) ext->changed(self->host);
    }
}

} // namespace pulp::format::clap_adapter
