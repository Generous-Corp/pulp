// LV2 Adapter Implementation for Pulp
// Maps pulp::format::Processor to the LV2 C API
// Reference: https://lv2plug.in/ns/lv2core (ISC license)

#include <pulp/format/lv2_adapter.hpp>
#include <pulp/runtime/log.hpp>

#include <sstream>
#include <iomanip>
#include <cstring>

namespace pulp::format::lv2_adapter {

// ── URID feature resolution (workstream 01 slice 1.5) ────────────────────

LV2_URID_Map* find_urid_map(const LV2_Feature* const* features) {
    if (!features) return nullptr;
    for (const LV2_Feature* const* f = features; *f != nullptr; ++f) {
        if ((*f)->URI && std::strcmp((*f)->URI, LV2_URID__map) == 0) {
            return static_cast<LV2_URID_Map*>((*f)->data);
        }
    }
    return nullptr;
}

// ── TTL Generation ───────────────────────────────────────────────────────

std::string generate_plugin_ttl(const PluginDescriptor& desc,
                                 const state::StateStore& store,
                                 const std::string& uri) {
    std::ostringstream ttl;

    // Prefixes
    ttl << "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n";
    ttl << "@prefix doap: <http://usefulinc.com/ns/doap#> .\n";
    ttl << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n";
    ttl << "@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n";
    ttl << "@prefix midi: <http://lv2plug.in/ns/ext/midi#> .\n";
    ttl << "@prefix urid: <http://lv2plug.in/ns/ext/urid#> .\n";
    ttl << "\n";

    // Plugin declaration
    ttl << "<" << uri << ">\n";
    ttl << "    a lv2:Plugin";

    // Plugin class based on category
    switch (desc.category) {
        case PluginCategory::Instrument:
            ttl << " , lv2:InstrumentPlugin";
            break;
        case PluginCategory::Effect:
            // Effects don't need a specific subclass
            break;
        case PluginCategory::MidiEffect:
            // MIDI processors are not format/unit converters; mapping to
            // lv2:ConverterPlugin (#276) misled hosts that group by LV2
            // class. lv2:UtilityPlugin + a more specific MIDIPlugin
            // marker is the conventional placement for MIDI effects in
            // the LV2 taxonomy.
            ttl << " , lv2:UtilityPlugin , lv2:MIDIPlugin";
            break;
    }
    ttl << " ;\n";

    ttl << "    doap:name \"" << desc.name << "\" ;\n";
    ttl << "    doap:developer [ doap:name \"" << desc.manufacturer << "\" ] ;\n";
    ttl << "    lv2:minorVersion 0 ;\n";
    ttl << "    lv2:microVersion 1 ;\n";

    // Required features
    ttl << "    lv2:requiredFeature urid:map ;\n";

    int port_index = 0;

    // Audio input ports
    for (const auto& bus : desc.input_buses) {
        for (int ch = 0; ch < bus.default_channels; ++ch) {
            if (port_index > 0) ttl << " ,\n";
            else ttl << "    lv2:port\n";
            ttl << "    [\n";
            ttl << "        a lv2:InputPort , lv2:AudioPort ;\n";
            ttl << "        lv2:index " << port_index << " ;\n";
            ttl << "        lv2:symbol \"audio_in_" << port_index << "\" ;\n";
            ttl << "        lv2:name \"" << bus.name << " " << (ch + 1) << "\"\n";
            ttl << "    ]";
            port_index++;
        }
    }

    // Audio output ports
    for (const auto& bus : desc.output_buses) {
        for (int ch = 0; ch < bus.default_channels; ++ch) {
            if (port_index > 0) ttl << " ,\n";
            else ttl << "    lv2:port\n";
            ttl << "    [\n";
            ttl << "        a lv2:OutputPort , lv2:AudioPort ;\n";
            ttl << "        lv2:index " << port_index << " ;\n";
            ttl << "        lv2:symbol \"audio_out_" << port_index << "\" ;\n";
            ttl << "        lv2:name \"" << bus.name << " " << (ch + 1) << "\"\n";
            ttl << "    ]";
            port_index++;
        }
    }

    // Control ports for parameters
    auto params = store.all_params();
    for (const auto& param : params) {
        ttl << " ,\n";
        ttl << "    [\n";
        ttl << "        a lv2:InputPort , lv2:ControlPort ;\n";
        ttl << "        lv2:index " << port_index << " ;\n";

        // Create a valid LV2 symbol from param name (lowercase, underscores)
        std::string symbol;
        for (char c : param.name) {
            if (std::isalnum(c)) symbol += static_cast<char>(std::tolower(c));
            else if (c == ' ') symbol += '_';
        }
        if (symbol.empty()) symbol = "param_" + std::to_string(param.id);

        ttl << "        lv2:symbol \"" << symbol << "\" ;\n";
        ttl << "        lv2:name \"" << param.name << "\" ;\n";
        ttl << "        lv2:default " << std::fixed << std::setprecision(4)
            << param.range.default_value << " ;\n";
        ttl << "        lv2:minimum " << param.range.min << " ;\n";
        ttl << "        lv2:maximum " << param.range.max << "\n";
        ttl << "    ]";
        port_index++;
    }

    // MIDI input port (if plugin accepts MIDI)
    if (desc.accepts_midi) {
        ttl << " ,\n";
        ttl << "    [\n";
        ttl << "        a lv2:InputPort , atom:AtomPort ;\n";
        ttl << "        lv2:index " << port_index << " ;\n";
        ttl << "        lv2:symbol \"midi_in\" ;\n";
        ttl << "        lv2:name \"MIDI In\" ;\n";
        ttl << "        atom:bufferType atom:Sequence ;\n";
        ttl << "        atom:supports midi:MidiEvent\n";
        ttl << "    ]";
        port_index++;
    }

    // MIDI output port (if plugin produces MIDI)
    if (desc.produces_midi) {
        ttl << " ,\n";
        ttl << "    [\n";
        ttl << "        a lv2:OutputPort , atom:AtomPort ;\n";
        ttl << "        lv2:index " << port_index << " ;\n";
        ttl << "        lv2:symbol \"midi_out\" ;\n";
        ttl << "        lv2:name \"MIDI Out\" ;\n";
        ttl << "        atom:bufferType atom:Sequence ;\n";
        ttl << "        atom:supports midi:MidiEvent\n";
        ttl << "    ]";
        port_index++;
    }

    ttl << " .\n";

    return ttl.str();
}

std::string generate_manifest_ttl(const std::string& plugin_uri,
                                   const std::string& binary_name) {
    std::ostringstream ttl;
    ttl << "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n";
    ttl << "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n\n";
    ttl << "<" << plugin_uri << ">\n";
    ttl << "    a lv2:Plugin ;\n";
    ttl << "    lv2:binary <" << binary_name << "> ;\n";
    ttl << "    rdfs:seeAlso <" << binary_name.substr(0, binary_name.rfind('.')) << ".ttl> .\n";
    return ttl.str();
}

} // namespace pulp::format::lv2_adapter
