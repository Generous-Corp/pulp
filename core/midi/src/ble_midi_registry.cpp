// Process-wide BLE-MIDI port registry — cross-platform, Skia-free.
// See ble_midi_registry.hpp for the rationale (the off-Apple analog of
// CoreMIDI auto-exposing OS-paired BLE peripherals as MIDI endpoints).

#include <pulp/midi/ble_midi_registry.hpp>

#include <choc/audio/choc_MIDI.h>

#include <map>
#include <mutex>

namespace pulp::midi {

struct BleMidiPortRegistry::Impl {
    mutable std::mutex mu;

    struct InputEntry {
        std::string name;
        MidiInputCallback callback;        // set on attach_input (host opened)
        MidiSysexCallback sysex_callback;  // set on attach_input
    };
    struct OutputEntry {
        std::string name;
        std::function<void(const std::vector<uint8_t>&)> sink;  // central GATT write
    };

    std::map<std::string, InputEntry> inputs;
    std::map<std::string, OutputEntry> outputs;
};

BleMidiPortRegistry::Impl& BleMidiPortRegistry::impl() const {
    // Function-local static: one registry, constructed on first use, with a
    // lifetime that outlives every central / MidiSystem instance.
    static Impl storage;
    return storage;
}

BleMidiPortRegistry& BleMidiPortRegistry::instance() {
    static BleMidiPortRegistry registry;
    return registry;
}

// ── Central side ────────────────────────────────────────────────────────────

void BleMidiPortRegistry::register_input(const std::string& port_id,
                                         const std::string& name) {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    auto& entry = d.inputs[port_id];
    entry.name = name;
    // Preserve any already-attached host callback (a connect after the host
    // pre-opened the port). attach_input handles the usual ordering.
}

void BleMidiPortRegistry::register_output(
    const std::string& port_id, const std::string& name,
    std::function<void(const std::vector<uint8_t>&)> sink) {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    auto& entry = d.outputs[port_id];
    entry.name = name;
    entry.sink = std::move(sink);
}

void BleMidiPortRegistry::unregister_input(const std::string& port_id) {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    d.inputs.erase(port_id);
}

void BleMidiPortRegistry::unregister_output(const std::string& port_id) {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    d.outputs.erase(port_id);
}

void BleMidiPortRegistry::deliver_message(const std::string& port_id,
                                          const std::vector<uint8_t>& bytes,
                                          double timestamp_sec) {
    if (bytes.empty()) return;
    // Copy the callbacks out under the lock, then invoke them unlocked so the
    // host callback can call back into the registry without deadlocking.
    MidiInputCallback cb;
    MidiSysexCallback sysex_cb;
    {
        auto& d = impl();
        std::lock_guard<std::mutex> lock(d.mu);
        auto it = d.inputs.find(port_id);
        if (it == d.inputs.end()) return;
        cb = it->second.callback;
        sysex_cb = it->second.sysex_callback;
    }

    // SysEx (0xF0…) is delivered through the sysex channel; everything else is a
    // short (1-3 byte) channel-voice / system message.
    if (bytes.front() == 0xF0) {
        if (sysex_cb) sysex_cb(bytes, timestamp_sec);
        return;
    }
    if (!cb) return;
    MidiEvent evt;
    const uint8_t d0 = bytes.size() > 0 ? bytes[0] : 0;
    const uint8_t d1 = bytes.size() > 1 ? bytes[1] : 0;
    const uint8_t d2 = bytes.size() > 2 ? bytes[2] : 0;
    evt.message = choc::midi::ShortMessage(d0, d1, d2);
    evt.timestamp = timestamp_sec;
    cb(evt);
}

// ── MidiSystem side ─────────────────────────────────────────────────────────

std::vector<MidiPortInfo> BleMidiPortRegistry::list_inputs() const {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    std::vector<MidiPortInfo> out;
    out.reserve(d.inputs.size());
    for (const auto& [id, entry] : d.inputs) {
        MidiPortInfo info;
        info.id = id;
        info.name = entry.name.empty() ? id : entry.name;
        info.is_input = true;
        info.is_output = false;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<MidiPortInfo> BleMidiPortRegistry::list_outputs() const {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    std::vector<MidiPortInfo> out;
    out.reserve(d.outputs.size());
    for (const auto& [id, entry] : d.outputs) {
        MidiPortInfo info;
        info.id = id;
        info.name = entry.name.empty() ? id : entry.name;
        info.is_input = false;
        info.is_output = true;
        out.push_back(std::move(info));
    }
    return out;
}

bool BleMidiPortRegistry::is_input(const std::string& port_id) const {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    return d.inputs.find(port_id) != d.inputs.end();
}

bool BleMidiPortRegistry::is_output(const std::string& port_id) const {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    return d.outputs.find(port_id) != d.outputs.end();
}

bool BleMidiPortRegistry::attach_input(const std::string& port_id,
                                       MidiInputCallback callback,
                                       MidiSysexCallback sysex_callback) {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    auto it = d.inputs.find(port_id);
    if (it == d.inputs.end()) return false;
    it->second.callback = std::move(callback);
    it->second.sysex_callback = std::move(sysex_callback);
    return true;
}

void BleMidiPortRegistry::detach_input(const std::string& port_id) {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    auto it = d.inputs.find(port_id);
    if (it == d.inputs.end()) return;
    it->second.callback = nullptr;
    it->second.sysex_callback = nullptr;
}

std::function<void(const std::vector<uint8_t>&)>
BleMidiPortRegistry::output_sink(const std::string& port_id) const {
    auto& d = impl();
    std::lock_guard<std::mutex> lock(d.mu);
    auto it = d.outputs.find(port_id);
    if (it == d.outputs.end()) return {};
    return it->second.sink;
}

}  // namespace pulp::midi
