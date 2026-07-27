#include "clap_note_name.hpp"

#include <pulp/format/clap_adapter.hpp>
#include <pulp/runtime/system.hpp>

#include <cstring>
#include <vector>

namespace pulp::format::clap_adapter {

namespace {

PulpClapPlugin* get_self(const clap_plugin_t* plugin) {
    if (!plugin) return nullptr;
    return static_cast<PulpClapPlugin*>(plugin->plugin_data);
}

// Both entry points are [main-thread] per the extension, and note_names() is
// documented as a host/main-thread call, so re-reading the processor here needs
// no gate. A host that asks for a count and then walks it sees a consistent
// list because it does both from the same thread.
std::vector<NoteName> note_names_of(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    if (!self || !self->processor) return {};
    return self->processor->note_names();
}

uint32_t note_name_count(const clap_plugin_t* plugin) {
    return static_cast<uint32_t>(note_names_of(plugin).size());
}

bool note_name_get(const clap_plugin_t* plugin,
                   uint32_t index,
                   clap_note_name_t* note_name) {
    if (!note_name) return false;
    const auto names = note_names_of(plugin);
    if (index >= names.size()) return false;

    const auto& src = names[index];
    std::memset(note_name, 0, sizeof(*note_name));
    runtime::copy_c_string(note_name->name, src.name);
    note_name->port = src.port;
    note_name->key = src.key;
    note_name->channel = src.channel;
    return true;
}

const clap_plugin_note_name_t s_note_name = {
    .count = note_name_count,
    .get = note_name_get,
};

} // namespace

const clap_plugin_note_name_t* note_name_extension() {
    return &s_note_name;
}

} // namespace pulp::format::clap_adapter
