// pulp-midi-ports: list every MIDI source and destination the system reports,
// with the id Pulp passes to open() and the names that identify the device.
//
//   pulp-midi-ports            one line per port
//   pulp-midi-ports --json     the same data as JSON

#include "ports_json.hpp"

#include <pulp/midi/device.hpp>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) {
            json = true;
        } else {
            std::fprintf(stderr, "usage: pulp-midi-ports [--json]\n");
            return 2;
        }
    }

    auto system = pulp::midi::create_midi_system();
    if (!system) {
        std::fprintf(stderr, "no MIDI system on this platform\n");
        return 1;
    }
    const auto inputs = system->enumerate_inputs();
    const auto outputs = system->enumerate_outputs();

    if (json) {
        std::fputs(pulp::midi::tools::ports_json(inputs, outputs).c_str(), stdout);
        return 0;
    }
    auto print = [](const char* kind, const std::vector<pulp::midi::MidiPortInfo>& ports) {
        for (const auto& p : ports)
            std::printf(
                "%-3s id=%-12s online=%d name=\"%s\" device=\"%s\" entity=\"%s\" endpoint=\"%s\"\n",
                kind, p.id.c_str(), p.online ? 1 : 0, p.name.c_str(), p.device_name.c_str(),
                p.entity_name.c_str(), p.endpoint_name.c_str());
    };
    print("in", inputs);
    print("out", outputs);
    return 0;
}
