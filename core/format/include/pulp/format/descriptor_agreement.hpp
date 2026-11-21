#pragma once

// Agreement check between a plug-in's static descriptor (answered to a host
// scan without building a Processor) and the descriptor() of the Processor
// built later. Everything a format derives its descriptor, ports and feature
// flags from is compared, so a plug-in cannot silently advertise ports or MIDI
// support it does not have.

#include <pulp/format/plugin_descriptor.hpp>
#include <pulp/runtime/log.hpp>

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace pulp::format {

/// Names of the fields that differ between two descriptors, comma separated.
/// Empty when they agree.
inline std::string descriptor_mismatch(const PluginDescriptor& a, const PluginDescriptor& b) {
    std::string out;
    auto note = [&](bool differs, const char* field) {
        if (!differs)
            return;
        if (!out.empty())
            out += ", ";
        out += field;
    };
    auto buses_differ = [](const std::vector<BusInfo>& x, const std::vector<BusInfo>& y) {
        if (x.size() != y.size())
            return true;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (x[i].name != y[i].name || x[i].default_channels != y[i].default_channels ||
                x[i].optional != y[i].optional || x[i].role != y[i].role)
                return true;
        }
        return false;
    };
    bool layouts_differ = a.supported_bus_layouts.size() != b.supported_bus_layouts.size();
    for (std::size_t i = 0; !layouts_differ && i < a.supported_bus_layouts.size(); ++i) {
        const auto& x = a.supported_bus_layouts[i];
        const auto& y = b.supported_bus_layouts[i];
        layouts_differ = x.inputs != y.inputs || x.outputs != y.outputs || x.name != y.name;
    }
    note(a.name != b.name, "name");
    note(a.manufacturer != b.manufacturer, "manufacturer");
    note(a.bundle_id != b.bundle_id, "bundle_id");
    note(a.version != b.version, "version");
    note(a.category != b.category, "category");
    note(buses_differ(a.input_buses, b.input_buses), "input_buses");
    note(buses_differ(a.output_buses, b.output_buses), "output_buses");
    note(a.accepts_midi != b.accepts_midi, "accepts_midi");
    note(a.produces_midi != b.produces_midi, "produces_midi");
    note(a.supports_mpe != b.supports_mpe, "supports_mpe");
    note(a.supports_ump != b.supports_ump, "supports_ump");
    note(a.tail_samples != b.tail_samples, "tail_samples");
    note(a.node_capabilities.supports_mpe != b.node_capabilities.supports_mpe ||
             a.node_capabilities.supports_ump != b.node_capabilities.supports_ump ||
             a.node_capabilities.supports_f64_audio != b.node_capabilities.supports_f64_audio ||
             a.node_capabilities.consumes_audio_rate_modulations !=
                 b.node_capabilities.consumes_audio_rate_modulations ||
             a.node_capabilities.writes_input_buffers != b.node_capabilities.writes_input_buffers,
         "node_capabilities");
    note(a.supports_f64_audio != b.supports_f64_audio, "supports_f64_audio");
    note(layouts_differ, "supported_bus_layouts");
    note(a.voice_count != b.voice_count, "voice_count");
    note(a.supports_program_change != b.supports_program_change, "supports_program_change");
    return out;
}

/// Number of disagreements detected so far in this process (all plug-ins).
inline std::atomic<unsigned>& descriptor_disagreement_count() {
    static std::atomic<unsigned> n{0};
    return n;
}

/// Compare and, on a mismatch, count it and log once per plug-in id (also in
/// release builds). Returns true when the descriptors agree.
inline bool check_descriptor_agreement(const PluginDescriptor& static_desc,
                                       const PluginDescriptor& built_desc) {
    const std::string diff = descriptor_mismatch(static_desc, built_desc);
    if (diff.empty())
        return true;
    descriptor_disagreement_count().fetch_add(1, std::memory_order_relaxed);
    static std::mutex m;
    static std::set<std::string> logged;
    bool first;
    {
        std::lock_guard<std::mutex> lock(m);
        first = logged.insert(static_desc.bundle_id).second;
    }
    if (first) {
        runtime::log_error("{}: static descriptor disagrees with Processor::descriptor() in: {}; "
                           "the host was shown the static one",
                           static_desc.bundle_id, diff);
    }
    return false;
}

} // namespace pulp::format
