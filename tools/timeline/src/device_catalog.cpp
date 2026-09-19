#include <pulp/tools/timeline/device_catalog.hpp>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/timeline_device_resolver.hpp>
#include <pulp/timeline/schema_json.hpp>

namespace pulp::tools::timeline {

namespace {

using pulp::timeline::quote_json_string;

// The stable public spelling of a device domain. Deliberately hand-mapped
// rather than derived from the enumerator name: this string is the axis the
// chain-shape refusal is written in, so it is part of the offline contract and
// must not move when the host enumerator is renamed.
std::string domain_name(pulp::host::BuiltInDeviceDomain domain) {
    return domain == pulp::host::BuiltInDeviceDomain::EventToEvent ? "event-to-event"
                                                                   : "event-to-audio";
}

std::string entry_json(const DeviceCatalogEntry& entry) {
    std::string json = "{\"binding_key\":" + quote_json_string(entry.binding_key) +
                       ",\"display_name\":" + quote_json_string(entry.display_name) +
                       ",\"manufacturer\":" + quote_json_string(entry.manufacturer) +
                       ",\"category\":" + quote_json_string(entry.category) +
                       ",\"summary\":" + quote_json_string(entry.summary) +
                       ",\"domain\":" + quote_json_string(entry.domain) +
                       ",\"num_audio_inputs\":" + std::to_string(entry.num_audio_inputs) +
                       ",\"num_audio_outputs\":" + std::to_string(entry.num_audio_outputs) +
                       ",\"latency_samples\":" + std::to_string(entry.latency_samples) +
                       ",\"is_instrument\":" + (entry.is_instrument ? "true" : "false") +
                       ",\"parameters\":[";
    bool first = true;
    for (const auto& parameter : entry.parameters) {
        if (!first)
            json += ",";
        first = false;
        json += "{\"id\":" + std::to_string(parameter.id) +
                ",\"name\":" + quote_json_string(parameter.name) +
                ",\"unit\":" + quote_json_string(parameter.unit) +
                ",\"min\":" + std::to_string(parameter.min_value) +
                ",\"max\":" + std::to_string(parameter.max_value) +
                ",\"default\":" + std::to_string(parameter.default_value) +
                ",\"automatable\":" + (parameter.automatable ? "true" : "false") +
                ",\"read_only\":" + (parameter.read_only ? "true" : "false") +
                ",\"hidden\":" + (parameter.hidden ? "true" : "false") +
                ",\"stepped\":" + (parameter.stepped ? "true" : "false") +
                ",\"is_bypass\":" + (parameter.is_bypass ? "true" : "false") +
                ",\"rampable\":" + (parameter.rampable ? "true" : "false") +
                ",\"modulatable\":" + (parameter.modulatable ? "true" : "false") + "}";
    }
    return json + "]}";
}

} // namespace

std::vector<DeviceCatalogEntry> device_catalog() {
    std::vector<DeviceCatalogEntry> entries;
    const auto catalog = pulp::host::builtin_device_catalog();
    entries.reserve(catalog.size());
    for (const auto& descriptor : catalog) {
        DeviceCatalogEntry entry;
        entry.binding_key = std::string(descriptor.binding_key);
        entry.display_name = std::string(descriptor.display_name);
        entry.manufacturer = std::string(descriptor.manufacturer);
        entry.category = std::string(descriptor.category);
        entry.summary = std::string(descriptor.summary);
        entry.domain = domain_name(descriptor.domain);
        entry.num_audio_inputs = descriptor.num_audio_inputs;
        entry.num_audio_outputs = descriptor.num_audio_outputs;
        entry.latency_samples = descriptor.latency_samples;
        entry.is_instrument = descriptor.is_instrument;
        auto slot =
            pulp::host::load_builtin_plugin(pulp::host::builtin_device_plugin_info(descriptor));
        if (slot != nullptr) {
            for (const auto& parameter : slot->parameters()) {
                entry.parameters.push_back({
                    parameter.id,
                    parameter.name,
                    parameter.unit,
                    parameter.min_value,
                    parameter.max_value,
                    parameter.default_value,
                    parameter.flags.automatable,
                    parameter.flags.read_only,
                    parameter.flags.hidden,
                    parameter.flags.stepped,
                    parameter.flags.is_bypass,
                    parameter.flags.rampable,
                    parameter.flags.modulatable,
                });
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::size_t admitted_device_chain_length() noexcept {
    return pulp::host::kAdmittedDeviceChainLength;
}

int event_device_latency_ceiling_samples() noexcept {
    return pulp::host::event_device_latency_ceiling_samples();
}

std::string device_catalog_json() {
    std::string json = "{\"devices\":[";
    bool first = true;
    for (const auto& entry : device_catalog()) {
        if (!first)
            json += ",";
        first = false;
        json += entry_json(entry);
    }
    json += "],\"max_chain_length\":" + std::to_string(admitted_device_chain_length());
    json += ",\"latency_ceiling_samples\":" +
            std::to_string(event_device_latency_ceiling_samples()) + "}";
    return json;
}

} // namespace pulp::tools::timeline
