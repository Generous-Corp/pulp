#pragma once

// Generic CLAP entry point generator
// Plugin developers include this and call PULP_CLAP_PLUGIN() with their factory function.
// All CLAP boilerplate (factory, extensions, entry point) is generated automatically.
//
// Usage (in one .cpp file per plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/clap_entry.hpp>
//   PULP_CLAP_PLUGIN(my_namespace::create_my_processor)

#include <pulp/format/processor.hpp>
#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/detail/locale_independent_float.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/parameter_text.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/format/clap_adapter.hpp>
#if defined(PULP_CLAP_GUI) && PULP_CLAP_GUI
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/gpu_host_select.hpp>
#endif
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>
#include <clap/clap.h>
#include <clap/ext/audio-ports-config.h>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <string_view>

// Internal implementation — do not call directly
namespace pulp::format::clap_generic {

// Per-plugin record. One CLAP binary can host many plugins (a bundle); the
// single-plugin macro registers exactly one. Only the factory is stored at
// static init — the descriptor (which requires CALLING the factory to build a
// Processor) is filled lazily in init_record(), invoked from entry_init(), the
// host's module-load callback. Constructing a Processor during C++ static
// initialization is unsafe (std::string/global order), so this deferral is
// load-bearing, not cosmetic.
struct ClapPluginRecord {
    ProcessorFactory factory = nullptr;
    PluginDescriptor desc{};
    clap_plugin_descriptor_t clap_desc{};
    const char* features[4] = {};
    bool initialized = false;
    // Bundle records publish themselves to the shared keyed registry (for
    // cross-format enumeration + per-plugin editor-asset lookup). The
    // legacy single-plugin PULP_CLAP_PLUGIN path leaves the keyed table
    // empty — matching AU/VST3, where only the bundle macros register keyed
    // entries — so single-plugin binaries keep their documented contract.
    bool publish_keyed = false;
};

// Fixed-capacity storage — no heap, static-init safe. A combined bundle
// registers one record per plugin; 64 is far above any real CLAP suite.
inline constexpr uint32_t kMaxClapPlugins = 64;
inline ClapPluginRecord g_clap_records[kMaxClapPlugins];
inline uint32_t g_clap_record_count = 0;

// Deduplicated view over the initialized records — the addressable plugin set
// the CLAP factory advertises. Built by init_all_records(); a record whose id
// collides with an earlier one (developer error in a bundle) is excluded so the
// host never sees an advertised-but-unreachable plugin (create_plugin resolves
// an id to the FIRST matching record, so only that one is addressable).
inline ClapPluginRecord* g_clap_index[kMaxClapPlugins] = {};
inline uint32_t g_clap_index_count = 0;

// Static-init-safe: stores the factory only, never calls it. `publish_keyed`
// records also register into the shared keyed registry at init time.
inline void register_clap_record(ProcessorFactory factory, bool publish_keyed) {
    if (g_clap_record_count >= kMaxClapPlugins) return;
    auto& rec = g_clap_records[g_clap_record_count++];
    rec.factory = factory;
    rec.publish_keyed = publish_keyed;
}

// Deferred descriptor build (NOT at static init). Idempotent.
inline void init_record(ClapPluginRecord& rec) {
    if (rec.initialized || !rec.factory) return;
    auto proc = rec.factory();
    if (!proc) return;
    rec.desc = proc->descriptor();

    switch (rec.desc.category) {
        case PluginCategory::Effect:
            rec.features[0] = CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
            break;
        case PluginCategory::Instrument:
            rec.features[0] = CLAP_PLUGIN_FEATURE_INSTRUMENT;
            break;
        case PluginCategory::MidiEffect:
            rec.features[0] = CLAP_PLUGIN_FEATURE_NOTE_EFFECT;
            break;
    }
    rec.features[1] = nullptr;

    // c_str() views into rec.desc, which lives in the stable g_clap_records
    // array for the process lifetime — so these pointers stay valid.
    rec.clap_desc = {
        .clap_version = CLAP_VERSION,
        .id = rec.desc.bundle_id.c_str(),
        .name = rec.desc.name.c_str(),
        .vendor = rec.desc.manufacturer.c_str(),
        .url = "",
        .manual_url = "",
        .support_url = "",
        .version = rec.desc.version.c_str(),
        .description = "",
        .features = rec.features,
    };

    rec.initialized = true;
}

inline void init_all_records() {
    g_clap_index_count = 0;
    for (uint32_t i = 0; i < g_clap_record_count; ++i) {
        auto& rec = g_clap_records[i];
        init_record(rec);
        if (!rec.initialized) continue;  // factory returned null

        // Exclude a duplicate id: the host must not be handed an advertised
        // descriptor it cannot instantiate (create_plugin resolves to the
        // first record with that id). First writer wins; later dupes drop.
        bool dupe = false;
        for (uint32_t j = 0; j < g_clap_index_count; ++j) {
            if (std::strcmp(g_clap_index[j]->clap_desc.id, rec.clap_desc.id) == 0) {
                dupe = true;
                break;
            }
        }
        if (dupe) continue;
        g_clap_index[g_clap_index_count++] = &rec;

        // Publish to the shared keyed registry only AFTER dedup, so a duplicate
        // id never lands an unreachable second entry in registered_plugins().
        // Bundle records only — the legacy single-plugin path leaves it empty
        // (see publish_keyed). Guard on find_plugin so publication is idempotent:
        // a matched re-init won't double-publish, and in a combined AU+VST3+CLAP
        // bundle the one logical plugin (shared bundle_id across formats) keeps a
        // single keyed entry for editor-asset lookup. AU codes are 0 for CLAP.
        if (rec.publish_keyed && find_plugin(rec.clap_desc.id) == nullptr) {
            register_plugin(PluginRegistration{
                .id = rec.clap_desc.id,
                .factory = rec.factory,
            });
        }
    }
}

// Test-only: clear all records back to empty. Registration is meant to happen
// once at static init and never be torn down; this exists so a test can swap the
// binary's plugin set deterministically. NOT for production use.
inline void reset_clap_records_for_testing() {
    for (uint32_t i = 0; i < g_clap_record_count; ++i) {
        g_clap_records[i] = ClapPluginRecord{};
    }
    g_clap_record_count = 0;
    for (uint32_t i = 0; i < g_clap_index_count; ++i) g_clap_index[i] = nullptr;
    g_clap_index_count = 0;
}

// Find a record by CLAP id (bundle_id). Null if unknown or not yet initialized.
inline ClapPluginRecord* find_clap_record(const char* id) {
    if (!id) return nullptr;
    for (uint32_t i = 0; i < g_clap_record_count; ++i) {
        if (g_clap_records[i].initialized &&
            strcmp(g_clap_records[i].clap_desc.id, id) == 0) {
            return &g_clap_records[i];
        }
    }
    return nullptr;
}

// ── Audio ports extension (multi-bus) ──────────────────────────────────
inline uint32_t audio_ports_count(const clap_plugin_t* plugin, bool is_input) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto desc = self->processor ? self->processor->descriptor()
                                : self->descriptor_snapshot;
    return static_cast<uint32_t>(is_input ? desc.input_buses.size() : desc.output_buses.size());
}

inline bool audio_ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                            clap_audio_port_info_t* info) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto desc = self->processor ? self->processor->descriptor()
                                : self->descriptor_snapshot;
    auto& buses = is_input ? desc.input_buses : desc.output_buses;

    if (index >= buses.size()) return false;
    auto& bus = buses[index];

    int channel_count = bus.default_channels;
    if (!desc.supported_bus_layouts.empty()) {
        const auto selected = std::min<std::size_t>(
            self->selected_bus_layout, desc.supported_bus_layouts.size() - 1);
        const auto& layout = desc.supported_bus_layouts[selected];
        const auto& widths = is_input ? layout.inputs : layout.outputs;
        if (index < widths.size()) channel_count = widths[index];
    }

    info->id = static_cast<clap_id>((is_input ? 0 : 100) + index);
    runtime::copy_c_string(info->name, bus.name);
    info->channel_count = static_cast<uint32_t>(std::max(0, channel_count));
    // Every Pulp port accepts 64-bit buffers: clap_process() converts f64 at
    // the adapter boundary for f32-internal processors and dispatches
    // process_f64() natively when the descriptor opts in — the same posture as
    // the VST3 adapter's unconditional canProcessSampleSize(kSample64). A host
    // only offers data64 to ports carrying SUPPORTS_64BITS, so without this
    // flag the whole f64 path is unreachable from spec-compliant hosts.
    // PREFERS_64BITS is advertised only when the processor is natively
    // double-precision (supports_f64_audio) — for boundary-converted plugins a
    // 64-bit stream buys nothing, so the host shouldn't be nudged toward it.
    // REQUIRES_COMMON_SAMPLE_SIZE is deliberately NOT set: clap_process()
    // handles mixed 32/64 buses by demoting the block to the f32 path.
    info->flags = (index == 0) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->flags |= CLAP_AUDIO_PORT_SUPPORTS_64BITS;
    if (desc.effective_capabilities().supports_f64_audio) {
        info->flags |= CLAP_AUDIO_PORT_PREFERS_64BITS;
    }
    info->port_type = channel_count == 1 ? CLAP_PORT_MONO
                    : channel_count == 2 ? CLAP_PORT_STEREO : nullptr;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

inline const clap_plugin_audio_ports_t audio_ports_ext = {
    .count = audio_ports_count, .get = audio_ports_get,
};

inline uint32_t audio_ports_config_count(const clap_plugin_t* plugin) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self || !self->processor) return 0;
    return static_cast<uint32_t>(
        self->processor->descriptor().supported_bus_layouts.size());
}

inline bool audio_ports_config_get(const clap_plugin_t* plugin, uint32_t index,
                                   clap_audio_ports_config_t* config) {
    if (!config) return false;
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self || !self->processor) return false;
    const auto desc = self->processor->descriptor();
    if (index >= desc.supported_bus_layouts.size()) return false;
    const auto& layout = desc.supported_bus_layouts[index];
    std::memset(config, 0, sizeof(*config));
    config->id = index;
    runtime::copy_c_string(config->name,
        layout.name.empty() ? std::string("Layout ") + std::to_string(index + 1)
                            : layout.name);
    config->input_port_count = static_cast<uint32_t>(layout.inputs.size());
    config->output_port_count = static_cast<uint32_t>(layout.outputs.size());
    config->has_main_input = !layout.inputs.empty() && layout.inputs.front() > 0;
    config->main_input_channel_count = config->has_main_input
        ? static_cast<uint32_t>(layout.inputs.front()) : 0;
    config->main_input_port_type = layout.inputs.empty() ? nullptr
        : layout.inputs.front() == 1 ? CLAP_PORT_MONO
        : layout.inputs.front() == 2 ? CLAP_PORT_STEREO : nullptr;
    config->has_main_output = !layout.outputs.empty() && layout.outputs.front() > 0;
    config->main_output_channel_count = config->has_main_output
        ? static_cast<uint32_t>(layout.outputs.front()) : 0;
    config->main_output_port_type = layout.outputs.empty() ? nullptr
        : layout.outputs.front() == 1 ? CLAP_PORT_MONO
        : layout.outputs.front() == 2 ? CLAP_PORT_STEREO : nullptr;
    return true;
}

inline bool audio_ports_config_select(const clap_plugin_t* plugin, clap_id config_id) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self || !self->processor) return false;
    const auto desc = self->processor->descriptor();
    if (config_id >= desc.supported_bus_layouts.size()) return false;
    const auto& layout = desc.supported_bus_layouts[config_id];
    if (!self->processor->is_bus_layout_supported(
            {layout.inputs, layout.outputs})) return false;
    self->selected_bus_layout = config_id;
    return true;
}

inline const clap_plugin_audio_ports_config_t audio_ports_config_ext = {
    .count = audio_ports_config_count,
    .get = audio_ports_config_get,
    .select = audio_ports_config_select,
};

// ── State extension ────────────────────────────────────────────────────
inline bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self || !self->processor) return false;
    const auto data = plugin_state_io::serialize(self->store, *self->processor);
    // CLAP's stream->write() is spec'd to return a short-write count on
    // success — callers MUST loop. Hosts (and clap-validator's
    // `state-reproducibility-flush` with a 23-byte write cap) exercise
    // this path. A single write() returning less than data.size() is
    // NOT an error; only negative or zero returns are.
    std::size_t written = 0;
    while (written < data.size()) {
        const auto n = stream->write(stream,
                                     data.data() + written,
                                     data.size() - written);
        if (n <= 0) return false;
        written += static_cast<std::size_t>(n);
    }
    return true;
}

inline bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self || !self->processor) return false;
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    while (true) {
        auto read = stream->read(stream, buf, sizeof(buf));
        if (read <= 0) break;
        data.insert(data.end(), buf, buf + read);
    }
    const bool ok = plugin_state_io::deserialize(data, self->store, *self->processor);
    // state.load is a main-thread call. A restored state can name a different
    // derived source than the live one (SuperConvolver: a different impulse
    // response), and a worker-less processor — every wasm build — has no thread
    // to notice. Reconcile here or the audio thread renders the OLD state for
    // the rest of the session. Default no-op for processors that don't opt in.
    self->processor->on_non_realtime_tick();
    return ok;
}

inline const clap_plugin_state_t state_ext = { .save = state_save, .load = state_load };

// ── Params extension ───────────────────────────────────────────────────
inline uint32_t params_count(const clap_plugin_t* plugin) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    return static_cast<uint32_t>(self->store.param_count());
}

inline bool params_get_info(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* info) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto params = self->store.all_params();
    if (index >= params.size()) return false;
    auto& p = params[index];
    memset(info, 0, sizeof(*info));
    info->id = p.id;
    runtime::copy_c_string(info->name, p.name);
    info->min_value = p.range.min;
    info->max_value = p.range.max;
    info->default_value = p.range.default_value;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    const bool is_bypass = state::is_bypass_param(p);
    if (state::is_discrete_param(p) || is_bypass)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (is_bypass) info->flags |= CLAP_PARAM_IS_BYPASS;
    return true;
}

inline bool params_get_value(const clap_plugin_t* plugin, clap_id param_id, double* value) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    *value = self->store.get_value(static_cast<state::ParamID>(param_id));
    return true;
}

inline bool params_value_to_text(const clap_plugin_t* plugin, clap_id param_id,
                                  double value, char* display, uint32_t size) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto* info = self->store.info(static_cast<state::ParamID>(param_id));
    if (!info) return false;
    const auto out = format_parameter_text(*info, static_cast<float>(value));
    if (out.empty()) return false;
    snprintf(display, size, "%s", out.c_str());
    return true;
}

inline bool params_text_to_value(const clap_plugin_t* plugin, clap_id param_id,
                                 const char* text, double* value) {
    if (!text) return false;

    if (!plugin) return false;
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    const auto* info = self->store.info(static_cast<state::ParamID>(param_id));
    if (!info) return false;
    const auto parsed = parse_parameter_text(*info, text);
    if (!parsed) return false;
    *value = *parsed;
    return true;
}

inline void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                          const clap_output_events_t*) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!in) return;
    uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        auto* hdr = in->get(in, i);
        // CLAP event-space gate: skip third-party-extension namespaces
        // so their type IDs can't alias core PARAM_VALUE. Mirrors the
        // guard in clap_adapter.cpp's process() dispatch loops.
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        // memcpy into a stack local to avoid UBSan "misaligned address"
        // when hdr isn't aligned to the struct's alignof (for example,
        // 8 for clap_event_param_value_t's `double value`).
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            clap_event_param_value_t ev;
            std::memcpy(&ev, hdr, sizeof(ev));
            self->store.set_value(static_cast<state::ParamID>(ev.param_id),
                                  static_cast<float>(ev.value));
        } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
            clap_event_param_gesture_t ev;
            std::memcpy(&ev, hdr, sizeof(ev));
            self->store.begin_gesture(static_cast<state::ParamID>(ev.param_id));
        } else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_END) {
            clap_event_param_gesture_t ev;
            std::memcpy(&ev, hdr, sizeof(ev));
            self->store.end_gesture(static_cast<state::ParamID>(ev.param_id));
        }
    }
}

inline const clap_plugin_params_t params_ext = {
    .count = params_count, .get_info = params_get_info,
    .get_value = params_get_value, .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value, .flush = params_flush,
};

// ── Note ports extension (for instruments) ─────────────────────────────
inline uint32_t note_ports_count(const clap_plugin_t* plugin, bool is_input) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    const auto& desc = self->processor ? self->processor->descriptor()
                                       : self->descriptor_snapshot;
    if (is_input && desc.accepts_midi) return 1;
    if (!is_input && desc.produces_midi) return 1;
    return 0;
}

inline bool note_ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                            clap_note_port_info_t* info) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    const auto& desc = self->processor ? self->processor->descriptor()
                                       : self->descriptor_snapshot;
    if (index != 0) return false;
    if (is_input && !desc.accepts_midi) return false;
    if (!is_input && !desc.produces_midi) return false;

    info->id = is_input ? 0 : 1;
    runtime::copy_c_string(info->name, is_input ? "Note In" : "Note Out");
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    return true;
}

inline const clap_plugin_note_ports_t note_ports_ext = {
    .count = note_ports_count, .get = note_ports_get,
};

// ── Latency extension ───────────────────────────────────────────────────
inline uint32_t latency_get(const clap_plugin_t* plugin) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self->processor) return 0;
    // CLAP reports latency as unsigned; clamp a negative
    // latency_samples() to 0 unless the quirk is filtered out
    // (PULP_HOST_QUIRKS=off).
    return static_cast<uint32_t>(
        reported_latency_samples(self->processor->latency_samples(),
                                 self->host_quirks));
}

inline const clap_plugin_latency_t latency_ext = { .get = latency_get };

// ── Tail extension ──────────────────────────────────────────────────────
inline uint32_t tail_get(const clap_plugin_t* plugin) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!self->processor) return 0;
    auto tail = self->processor->descriptor().tail_samples;
    if (tail < 0) return UINT32_MAX; // infinite tail
    return static_cast<uint32_t>(tail);
}

inline const clap_plugin_tail_t tail_ext = { .get = tail_get };

// ── GUI extension (only in plugin targets that define PULP_CLAP_GUI) ──

#if defined(PULP_CLAP_GUI) && PULP_CLAP_GUI

inline bool gui_is_api_supported(const clap_plugin_t*, const char* api, bool is_floating) {
    if (pulp::format::detail::editor_launch_blocked_by_environment()) return false;
    if (is_floating) return false;
#ifdef __APPLE__
    return strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
#elif defined(_WIN32)
    return strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
#elif defined(__linux__)
    return strcmp(api, CLAP_WINDOW_API_X11) == 0;
#else
    (void)api;
    return false;
#endif
}

inline bool gui_get_preferred_api(const clap_plugin_t*, const char** api, bool* is_floating) {
    if (pulp::format::detail::editor_launch_blocked_by_environment()) return false;
    *is_floating = false;
#ifdef __APPLE__
    *api = CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    *api = CLAP_WINDOW_API_WIN32;
#elif defined(__linux__)
    *api = CLAP_WINDOW_API_X11;
#else
    return false;
#endif
    return true;
}

inline bool gui_create(const clap_plugin_t* plugin, const char*, bool) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (pulp::format::detail::editor_launch_blocked_by_environment()) {
        runtime::log_info("CLAP editor: disabled in headless/CI/test environment");
        return false;
    }
    if (!p->processor || !p->processor->has_editor()) return false;

    std::string editor_error;
    p->bridge = std::make_unique<ViewBridge>(
        *p->processor, p->store, p->owner_alive.capture());
    if (!p->bridge->open(&editor_error)) {
        runtime::log_error("CLAP editor: bridge->open failed ({})", editor_error);
        p->bridge.reset();
        return false;
    }

    const auto& hints = p->bridge->size_hints();
    const auto gpu = decide_gpu_host(*p->bridge);
    view::PluginViewHost::Options opts;
    opts.size = {hints.preferred_width, hints.preferred_height};
    opts.use_gpu = gpu.use_gpu;

    p->editor_host = view::PluginViewHost::create(*p->bridge->view(), opts);
    if (p->editor_host) {
        warn_if_unexpected_cpu_fallback(gpu, p->editor_host.get());
        // Pump the scripted UI session (async results, timers, rAF) per vsync.
        p->editor_host->set_idle_callback(make_scripted_idle_pump(*p->bridge));
        // Route navigator.gpu / canvas.getContext('webgpu') through
        // the host's live GpuSurface instead of the JS mock path.
        if (auto* scripted = p->bridge->scripted_ui()) {
            scripted->attach_gpu_surface(p->editor_host->gpu_surface());
            if (p->editor_host->gpu_surface()) {
                runtime::log_info(
                    "[plugin-gpu-host] GpuSurface attached to WidgetBridge "
                    "via ScriptedUiSession (CLAP)");
            }
        }
        // Resize contract (ViewSize), mirroring the VST3 adapter:
        //   - not resizable (min==0): pin the viewport at preferred so an
        //     off-size pane letterbox-scales the content (today's behavior).
        //   - resizable + aspect_ratio>0: pin viewport + lock aspect; the
        //     adjust_size / get_resize_hints path enforces the aspect on drag.
        //   - resizable + aspect_ratio==0: honor "free drag within [min,max]" —
        //     NO viewport, NO aspect lock; the root reflows via Yoga at the host
        //     size and adjust_size only clamps min/max.
        // `resizable` follows gui_can_resize's convention (min>0 on both axes).
        const bool resizable =
            hints.min_width > 0 && hints.min_height > 0;
        const bool free_resize = resizable && hints.aspect_ratio <= 0.0;
        if (hints.preferred_width > 0 && hints.preferred_height > 0 &&
            !free_resize) {
            p->editor_host->set_design_viewport(
                static_cast<float>(hints.preferred_width),
                static_cast<float>(hints.preferred_height));
            p->editor_host->set_fixed_aspect_ratio(
                static_cast<float>(hints.preferred_width) /
                static_cast<float>(hints.preferred_height));
        }
        runtime::log_info("CLAP editor: created ({}x{}, mode={}, gpu={})",
                          hints.preferred_width, hints.preferred_height,
                          gpu.mode, p->editor_host->is_gpu_backed());
    } else {
        runtime::log_error("CLAP editor: failed to create host");
        p->bridge->close();
        p->bridge.reset();
    }
    return p->editor_host != nullptr;
}

inline void gui_destroy(const clap_plugin_t* plugin) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    p->editor_host.reset();
    if (p->bridge) {
        p->bridge->close();
        p->bridge.reset();
    }
    p->editor_visible = false;
}

inline bool gui_set_scale(const clap_plugin_t*, double) {
    return false;  // Cocoa uses logical pixels — no explicit scaling needed
}

inline bool gui_get_size(const clap_plugin_t* plugin, uint32_t* width, uint32_t* height) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!p->processor) return false;
    if (p->bridge) {
        const auto& h = p->bridge->size_hints();
        *width = h.preferred_width;
        *height = h.preferred_height;
    } else {
        auto [w, ht] = p->processor->editor_size();
        *width = w;
        *height = ht;
    }
    return true;
}

// Editor resize negotiation: the plugin advertises proportional resize
// locked to the editor's design aspect. The host's design viewport
// (set in gui_create) scales content to fit the host window without
// re-layout — so any (w, h) the DAW lands on still looks correct.
// Resize capability is declared by non-zero min_width/min_height in
// view_size() — the same bounds gui_get_resize_hints exposes. Plugins
// that use the base-class default (min_width=0, min_height=0) are not
// resizable; plugins that declare bounds (e.g. GpuEditorSmoke {320,240})
// are. This check works before gui_create (no bridge needed).
inline bool gui_can_resize(const clap_plugin_t* plugin) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!p->processor) return false;
    const auto vs = p->processor->view_size();
    return vs.min_width > 0 && vs.min_height > 0;
}

inline bool gui_get_resize_hints(const clap_plugin_t* plugin,
                                 clap_gui_resize_hints_t* hints) {
    if (!hints) return false;
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!p->bridge) return false;
    const auto& size_hints = p->bridge->size_hints();
    if (size_hints.preferred_width == 0 || size_hints.preferred_height == 0) {
        return false;
    }
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    // preserve_aspect_ratio tracks the resize contract: a resizable editor with
    // aspect_ratio==0 drags freely (no lock); otherwise the aspect is held.
    const bool resizable =
        size_hints.min_width > 0 && size_hints.min_height > 0;
    const bool free_resize = resizable && size_hints.aspect_ratio <= 0.0;
    hints->preserve_aspect_ratio = !free_resize;
    hints->aspect_ratio_width = size_hints.preferred_width;
    hints->aspect_ratio_height = size_hints.preferred_height;
    return true;
}

inline bool gui_adjust_size(const clap_plugin_t* plugin,
                            uint32_t* width, uint32_t* height) {
    if (!width || !height || *width == 0 || *height == 0) return false;
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!p->bridge) return false;
    const auto& size_hints = p->bridge->size_hints();
    if (size_hints.preferred_width == 0 || size_hints.preferred_height == 0) {
        return false;
    }
    // Free-resize contract: resizable (min>0) with aspect_ratio==0 means "any
    // ratio; drag freely within [min,max]". Clamp each axis independently — NO
    // aspect snap — matching gui_get_resize_hints.preserve_aspect_ratio=false.
    const bool resizable =
        size_hints.min_width > 0 && size_hints.min_height > 0;
    const bool free_resize = resizable && size_hints.aspect_ratio <= 0.0;
    if (free_resize) {
        if (size_hints.min_width > 0 && *width < size_hints.min_width)
            *width = size_hints.min_width;
        if (size_hints.min_height > 0 && *height < size_hints.min_height)
            *height = size_hints.min_height;
        if (size_hints.max_width > 0 && *width > size_hints.max_width)
            *width = size_hints.max_width;
        if (size_hints.max_height > 0 && *height > size_hints.max_height)
            *height = size_hints.max_height;
        return true;
    }
    // Snap to the design aspect ratio — pick the largest box with the
    // design aspect that fits within the requested rectangle. Same shape
    // the standalone host's drag-to-resize lands on.
    const double design_aspect =
        static_cast<double>(size_hints.preferred_width) /
        static_cast<double>(size_hints.preferred_height);
    const double req_aspect =
        static_cast<double>(*width) /
        static_cast<double>(*height);
    if (req_aspect > design_aspect) {
        // Requested rect is too wide → shrink width to height * aspect.
        *width = static_cast<uint32_t>(
            static_cast<double>(*height) * design_aspect + 0.5);
    } else if (req_aspect < design_aspect) {
        // Requested rect is too tall → shrink height to width / aspect.
        *height = static_cast<uint32_t>(
            static_cast<double>(*width) / design_aspect + 0.5);
    }
    // Respect plugin min/max constraints when defined. A naive clamp
    // after the aspect snap would re-introduce off-aspect rects (e.g.
    // design 2:1, request (500,1000) snaps to (500,250); if min_height
    // is 400, a naive clamp gives (500,400) — no longer 2:1). After
    // any clamp, re-snap by EXPANDING the other dimension to restore
    // the design aspect. The advertised preserve_aspect_ratio=true
    // contract MUST hold.
    auto resnap = [&]() {
        // Use whichever dimension expanded by the clamp; expand the
        // other to match the design aspect.
        const double aspect_now =
            static_cast<double>(*width) / static_cast<double>(*height);
        if (aspect_now > design_aspect) {
            *height = static_cast<uint32_t>(
                static_cast<double>(*width) / design_aspect + 0.5);
        } else if (aspect_now < design_aspect) {
            *width = static_cast<uint32_t>(
                static_cast<double>(*height) * design_aspect + 0.5);
        }
    };
    if (size_hints.min_width > 0 && *width < size_hints.min_width) {
        *width = size_hints.min_width;
        resnap();
    }
    if (size_hints.min_height > 0 && *height < size_hints.min_height) {
        *height = size_hints.min_height;
        resnap();
    }
    if (size_hints.max_width > 0 && *width > size_hints.max_width) {
        *width = size_hints.max_width;
        resnap();
    }
    if (size_hints.max_height > 0 && *height > size_hints.max_height) {
        *height = size_hints.max_height;
        resnap();
    }
    return true;
}

inline bool gui_set_size(const clap_plugin_t* plugin, uint32_t width, uint32_t height) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (p->bridge) p->bridge->resize(width, height);
    if (p->editor_host) {
        p->editor_host->set_size(width, height);
        return true;
    }
    return false;
}

inline bool gui_set_parent(const clap_plugin_t* plugin, const clap_window_t* window) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!p->editor_host) return false;

    bool attached = false;
#ifdef __APPLE__
    if (strcmp(window->api, CLAP_WINDOW_API_COCOA) == 0) {
        p->editor_host->attach_to_parent(window->cocoa);
        attached = true;
    }
#elif defined(_WIN32)
    if (strcmp(window->api, CLAP_WINDOW_API_WIN32) == 0) {
        p->editor_host->attach_to_parent(window->win32);
        attached = true;
    }
#elif defined(__linux__)
    if (strcmp(window->api, CLAP_WINDOW_API_X11) == 0) {
        p->editor_host->attach_to_parent(reinterpret_cast<void*>(window->x11));
        attached = true;
    }
#endif
    if (attached && p->bridge) {
        p->bridge->notify_attached();
    }
    return attached;
}

inline bool gui_set_transient(const clap_plugin_t*, const clap_window_t*) {
    return false;  // No floating window support
}

inline void gui_suggest_title(const clap_plugin_t*, const char*) {
    // No-op for embedded windows
}

inline bool gui_show(const clap_plugin_t* plugin) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (p->editor_host) {
        p->editor_visible = true;
        p->editor_host->repaint();
        return true;
    }
    return false;
}

inline bool gui_hide(const clap_plugin_t* plugin) {
    auto* p = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    p->editor_visible = false;
    return true;
}

inline const clap_plugin_gui_t gui_ext = {
    .is_api_supported = gui_is_api_supported,
    .get_preferred_api = gui_get_preferred_api,
    .create = gui_create,
    .destroy = gui_destroy,
    .set_scale = gui_set_scale,
    .get_size = gui_get_size,
    .can_resize = gui_can_resize,
    .get_resize_hints = gui_get_resize_hints,
    .adjust_size = gui_adjust_size,
    .set_size = gui_set_size,
    .set_parent = gui_set_parent,
    .set_transient = gui_set_transient,
    .suggest_title = gui_suggest_title,
    .show = gui_show,
    .hide = gui_hide,
};

#endif // PULP_CLAP_GUI

// ── Extension dispatch ─────────────────────────────────────────────────
inline const void* get_static_extension(const clap_plugin_t* plugin, const char* id) {
    if (!id) return nullptr;
#if defined(PULP_CLAP_GUI) && PULP_CLAP_GUI
    if (strcmp(id, CLAP_EXT_GUI) == 0) {
        if (pulp::format::detail::editor_launch_blocked_by_environment()) return nullptr;
        return &gui_ext;
    }
#endif
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audio_ports_ext;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG) == 0)
        return audio_ports_config_count(plugin) > 0 ? &audio_ports_config_ext : nullptr;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &note_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &params_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &state_ext;
    if (strcmp(id, CLAP_EXT_LATENCY) == 0) return &latency_ext;
    if (strcmp(id, CLAP_EXT_TAIL) == 0) return &tail_ext;
    return nullptr;
}

inline const void* get_extension(const clap_plugin_t* plugin, const char* id) {
    if (const void* ext = get_static_extension(plugin, id)) {
        return ext;
    }
    return clap_adapter::clap_get_extension(plugin, id);
}

// ── Plugin creation ────────────────────────────────────────────────────
inline const clap_plugin_t* create_plugin(const clap_plugin_factory_t*,
                                           const clap_host_t* host,
                                           const char* plugin_id) {
    // Resolve which of the binary's plugins the host asked for. In a
    // single-plugin binary there is exactly one record; in a bundle, match by id.
    ClapPluginRecord* rec = find_clap_record(plugin_id);
    if (!rec) return nullptr;

    auto* instance = new clap_adapter::PulpClapPlugin();
    instance->factory = rec->factory;
    // Cache this plugin's descriptor so pre-init extension queries (audio/note
    // ports) resolve per-plugin metadata without a shared global.
    instance->descriptor_snapshot = rec->desc;
    // Keep the host pointer so clap_on_main_thread() can republish
    // latency / tail changes the processor flagged.
    instance->host = host;
    instance->plugin = {
        .desc = &rec->clap_desc,
        .plugin_data = instance,
        .init = clap_adapter::clap_init,
        .destroy = clap_adapter::clap_destroy,
        .activate = clap_adapter::clap_activate,
        .deactivate = clap_adapter::clap_deactivate,
        .start_processing = clap_adapter::clap_start_processing,
        .stop_processing = clap_adapter::clap_stop_processing,
        .reset = clap_adapter::clap_reset,
        .process = clap_adapter::clap_process,
        .get_extension = get_extension,
        .on_main_thread = clap_adapter::clap_on_main_thread,
    };
    return &instance->plugin;
}

// Enumerate over the deduplicated index (built in init_all_records) so the host
// only ever sees addressable, uniquely-identified plugins — never a failed-init
// hole or an advertised-but-unreachable duplicate id.
inline uint32_t get_plugin_count(const clap_plugin_factory_t*) {
    return g_clap_index_count;
}
inline const clap_plugin_descriptor_t* get_plugin_descriptor(const clap_plugin_factory_t*, uint32_t i) {
    if (i >= g_clap_index_count) return nullptr;
    return &g_clap_index[i]->clap_desc;
}

inline const clap_plugin_factory_t plugin_factory = {
    .get_plugin_count = get_plugin_count,
    .get_plugin_descriptor = get_plugin_descriptor,
    .create_plugin = create_plugin,
};

inline bool entry_init(const char*) {
    // Build every registered plugin's descriptor here — the host's module-load
    // callback, safely after C++ static initialization — never at static init.
    init_all_records();
    return true;
}
// CLAP's init/deinit are nestable and matched, and { return true; } / {} pairs
// are explicitly sanctioned by the entry contract. Our records are an immutable,
// bounded (kMaxClapPlugins) module-lifetime cache: init_record() is idempotent,
// so a re-init after a matched deinit reuses identical descriptors, and the
// storage does not grow per cycle. Final DSO unload reclaims it. This mirrors the
// AU/VST3 registrars, which run at static init and never unregister. Nothing to
// release here — and doing so would be actively wrong, since a record's clap_desc
// backs any clap_plugin_t the host has not yet destroyed.
inline void entry_deinit() {}
inline const void* entry_get_factory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &plugin_factory;
    return nullptr;
}

} // namespace pulp::format::clap_generic

// ── Public macros ──────────────────────────────────────────────────────

// The one CLAP module entry symbol. Exactly ONE per binary — the single-plugin
// macro emits it for you; a bundle emits it once via PULP_CLAP_BUNDLE_ENTRY().
#define PULP_CLAP_BUNDLE_ENTRY() \
    extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = { \
        .clap_version = CLAP_VERSION, \
        .init = pulp::format::clap_generic::entry_init, \
        .deinit = pulp::format::clap_generic::entry_deinit, \
        .get_factory = pulp::format::clap_generic::entry_get_factory, \
    };

// Register one plugin in a multi-plugin CLAP bundle. Repeat once per plugin,
// each with a distinct `Ident` token (used only to name the registrar). The
// factory is stored at static init; its descriptor is built later in
// entry_init() (never at static init). Pair N of these with ONE
// PULP_CLAP_BUNDLE_ENTRY(). Bundles do NOT set the legacy global factory slot —
// each plugin is resolved by id at create_plugin().
#define PULP_CLAP_BUNDLE_PLUGIN(Ident, factory_fn) \
    namespace { \
        struct Ident##_ClapRegistrar { \
            Ident##_ClapRegistrar() { \
                pulp::format::clap_generic::register_clap_record(factory_fn, \
                                                                 /*publish_keyed=*/true); \
            } \
        } Ident##_clap_registrar_; \
    }

// Place in ONE .cpp file per single-plugin CLAP bundle. Registers the one
// plugin, keeps the legacy global factory slot (single-plugin back-compat), and
// emits the module entry. Unchanged usage; descriptor build now happens in
// entry_init() rather than at static init, so no Processor is constructed during
// C++ static initialization.
#define PULP_CLAP_PLUGIN(factory_fn) \
    namespace { \
        struct PulpClapInit { \
            PulpClapInit() { \
                pulp::format::clap_generic::register_clap_record(factory_fn, \
                                                                 /*publish_keyed=*/false); \
                pulp::format::register_plugin(factory_fn); \
            } \
        } _pulp_clap_init; \
    } \
    PULP_CLAP_BUNDLE_ENTRY()
