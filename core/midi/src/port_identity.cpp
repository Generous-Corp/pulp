#include <pulp/midi/device.hpp>

namespace pulp::midi {

int resolve_port(const std::vector<MidiPortInfo>& ports, const std::string& saved_id,
                 const std::string& saved_stable_key) {
    if (!saved_id.empty()) {
        for (size_t i = 0; i < ports.size(); ++i)
            if (ports[i].id == saved_id && ports[i].online)
                return static_cast<int>(i);
    }
    if (saved_stable_key.empty())
        return -1;
    int found = -1;
    for (size_t i = 0; i < ports.size(); ++i) {
        if (!ports[i].online || ports[i].stable_key != saved_stable_key)
            continue;
        if (found >= 0)
            return -1; // two ports share the key: refuse rather than guess
        found = static_cast<int>(i);
    }
    return found;
}

} // namespace pulp::midi
