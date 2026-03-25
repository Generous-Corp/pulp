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
#include <pulp/format/registry.hpp>
#include <pulp/format/clap_adapter.hpp>
#include <pulp/runtime/log.hpp>
#include <clap/clap.h>
#include <cstring>
#include <cstdio>

// Internal implementation — do not call directly
namespace pulp::format::clap_generic {

// These are populated at static init by PULP_CLAP_PLUGIN
inline ProcessorFactory g_factory = nullptr;
inline PluginDescriptor g_desc{};
inline clap_plugin_descriptor_t g_clap_desc{};
inline const char* g_features[4] = {};

inline void init_descriptor() {
    if (!g_factory) return;
    auto proc = g_factory();
    if (!proc) return;
    g_desc = proc->descriptor();

    // Map category to CLAP features
    switch (g_desc.category) {
        case PluginCategory::Effect:
            g_features[0] = CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
            break;
        case PluginCategory::Instrument:
            g_features[0] = CLAP_PLUGIN_FEATURE_INSTRUMENT;
            break;
        case PluginCategory::MidiEffect:
            g_features[0] = CLAP_PLUGIN_FEATURE_NOTE_EFFECT;
            break;
    }
    g_features[1] = nullptr;

    g_clap_desc = {
        .clap_version = CLAP_VERSION,
        .id = g_desc.bundle_id.c_str(),
        .name = g_desc.name.c_str(),
        .vendor = g_desc.manufacturer.c_str(),
        .url = "",
        .manual_url = "",
        .support_url = "",
        .version = g_desc.version.c_str(),
        .description = "",
        .features = g_features,
    };
}

// ── Audio ports extension ──────────────────────────────────────────────
inline uint32_t audio_ports_count(const clap_plugin_t* plugin, bool is_input) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto desc = self->processor ? self->processor->descriptor() : g_desc;
    if (is_input)
        return desc.default_input_channels > 0 ? 1 : 0;
    return desc.default_output_channels > 0 ? 1 : 0;
}

inline bool audio_ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                            clap_audio_port_info_t* info) {
    if (index != 0) return false;
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto desc = self->processor ? self->processor->descriptor() : g_desc;

    int channels = is_input ? desc.default_input_channels : desc.default_output_channels;
    if (channels == 0) return false;

    info->id = is_input ? 0 : 1;
    strncpy(info->name, is_input ? "Audio In" : "Audio Out", CLAP_NAME_SIZE);
    info->channel_count = channels;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = channels == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    info->in_place_pair = is_input ? 1 : 0;
    return true;
}

inline const clap_plugin_audio_ports_t audio_ports_ext = {
    .count = audio_ports_count, .get = audio_ports_get,
};

// ── State extension ────────────────────────────────────────────────────
inline bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    auto data = self->store.serialize();
    return stream->write(stream, data.data(), data.size()) == static_cast<int64_t>(data.size());
}

inline bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    while (true) {
        auto read = stream->read(stream, buf, sizeof(buf));
        if (read <= 0) break;
        data.insert(data.end(), buf, buf + read);
    }
    return self->store.deserialize(data);
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
    strncpy(info->name, p.name.c_str(), CLAP_NAME_SIZE - 1);
    info->min_value = p.range.min;
    info->max_value = p.range.max;
    info->default_value = p.range.default_value;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (p.range.step >= 1.0f && (p.range.max - p.range.min) < 10.0f)
        info->flags |= CLAP_PARAM_IS_STEPPED;
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
    if (!info->unit.empty())
        snprintf(display, size, "%.2f %s", value, info->unit.c_str());
    else
        snprintf(display, size, "%.2f", value);
    return true;
}

inline bool params_text_to_value(const clap_plugin_t*, clap_id, const char* text, double* value) {
    char* end;
    *value = strtod(text, &end);
    return end != text;
}

inline void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                          const clap_output_events_t*) {
    auto* self = static_cast<clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
    if (!in) return;
    uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        auto* hdr = in->get(in, i);
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
            self->store.set_value(static_cast<state::ParamID>(ev->param_id),
                                  static_cast<float>(ev->value));
        }
    }
}

inline const clap_plugin_params_t params_ext = {
    .count = params_count, .get_info = params_get_info,
    .get_value = params_get_value, .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value, .flush = params_flush,
};

// ── Extension dispatch ─────────────────────────────────────────────────
inline const void* get_extension(const clap_plugin_t*, const char* id) {
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audio_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &params_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &state_ext;
    return nullptr;
}

// ── Plugin creation ────────────────────────────────────────────────────
inline const clap_plugin_t* create_plugin(const clap_plugin_factory_t*,
                                           const clap_host_t*,
                                           const char* plugin_id) {
    if (strcmp(plugin_id, g_clap_desc.id) != 0) return nullptr;

    auto* instance = new clap_adapter::PulpClapPlugin();
    instance->factory = g_factory;
    instance->plugin = {
        .desc = &g_clap_desc,
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

inline uint32_t get_plugin_count(const clap_plugin_factory_t*) { return 1; }
inline const clap_plugin_descriptor_t* get_plugin_descriptor(const clap_plugin_factory_t*, uint32_t i) {
    return i == 0 ? &g_clap_desc : nullptr;
}

inline const clap_plugin_factory_t plugin_factory = {
    .get_plugin_count = get_plugin_count,
    .get_plugin_descriptor = get_plugin_descriptor,
    .create_plugin = create_plugin,
};

inline bool entry_init(const char*) { return true; }
inline void entry_deinit() {}
inline const void* entry_get_factory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &plugin_factory;
    return nullptr;
}

} // namespace pulp::format::clap_generic

// ── Public macro ───────────────────────────────────────────────────────
// Place in ONE .cpp file per CLAP plugin.
#define PULP_CLAP_PLUGIN(factory_fn) \
    namespace { \
        struct PulpClapInit { \
            PulpClapInit() { \
                pulp::format::clap_generic::g_factory = factory_fn; \
                pulp::format::register_plugin(factory_fn); \
                pulp::format::clap_generic::init_descriptor(); \
            } \
        } _pulp_clap_init; \
    } \
    extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = { \
        .clap_version = CLAP_VERSION, \
        .init = pulp::format::clap_generic::entry_init, \
        .deinit = pulp::format::clap_generic::entry_deinit, \
        .get_factory = pulp::format::clap_generic::entry_get_factory, \
    };
