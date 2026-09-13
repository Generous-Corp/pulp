#include <pulp/tools/timeline/device_catalog.hpp>

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
    return "{\"binding_key\":" + quote_json_string(entry.binding_key) + ",\"display_name\":" +
           quote_json_string(entry.display_name) + ",\"manufacturer\":" +
           quote_json_string(entry.manufacturer) + ",\"category\":" +
           quote_json_string(entry.category) + ",\"summary\":" +
           quote_json_string(entry.summary) + ",\"domain\":" + quote_json_string(entry.domain) +
           ",\"num_audio_inputs\":" + std::to_string(entry.num_audio_inputs) +
           ",\"num_audio_outputs\":" + std::to_string(entry.num_audio_outputs) +
           ",\"latency_samples\":" + std::to_string(entry.latency_samples) +
           ",\"is_instrument\":" + (entry.is_instrument ? "true" : "false") + "}";
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
