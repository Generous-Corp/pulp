#pragma once

// JSON listing of MIDI ports for pulp-midi-ports. Kept apart from main() so a
// test can check the output without spawning the tool.

#include <pulp/midi/device.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace pulp::midi::tools {

inline std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (unsigned char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

inline std::string port_json(const MidiPortInfo& p) {
    std::string s = "    {";
    s += "\"id\": \"" + json_escape(p.id) + "\"";
    s += ", \"name\": \"" + json_escape(p.name) + "\"";
    s += ", \"device\": \"" + json_escape(p.device_name) + "\"";
    s += ", \"entity\": \"" + json_escape(p.entity_name) + "\"";
    s += ", \"endpoint\": \"" + json_escape(p.endpoint_name) + "\"";
    s += ", \"stable_key\": \"" + json_escape(p.stable_key) + "\"";
    s += std::string(", \"online\": ") + (p.online ? "true" : "false");
    s += "}";
    return s;
}

inline std::string ports_json(const std::vector<MidiPortInfo>& inputs,
                              const std::vector<MidiPortInfo>& outputs) {
    auto list = [](const std::vector<MidiPortInfo>& ports) {
        std::string s = "[";
        for (size_t i = 0; i < ports.size(); ++i)
            s += (i ? ",\n" : "\n") + port_json(ports[i]);
        s += ports.empty() ? "]" : "\n  ]";
        return s;
    };
    return "{\n  \"inputs\": " + list(inputs) + ",\n  \"outputs\": " + list(outputs) + "\n}\n";
}

} // namespace pulp::midi::tools
