// Web MIDI API integration for WASM builds
// Provides MidiSystem and MidiInput implementations using the Web MIDI API
// via Emscripten's JavaScript interop.

#ifdef __EMSCRIPTEN__

#include <pulp/midi/device.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/log.hpp>
#include <emscripten.h>
#include <vector>
#include <functional>
#include <mutex>

namespace pulp::midi {

// ── Web MIDI Input ──────────────────────────────────────────────────────

class WebMidiInput : public MidiInput {
public:
    bool open(const std::string& port_id, MidiCallback callback) override {
        callback_ = std::move(callback);
        port_id_ = port_id;

        // Request MIDI access and listen on the port
        EM_ASM({
            if (!navigator.requestMIDIAccess) {
                console.warn('Web MIDI API not available');
                return;
            }
            navigator.requestMIDIAccess().then(function(access) {
                window._pulpMidiAccess = access;
                var portId = UTF8ToString($0);

                // Find the requested port (or first available)
                var input = null;
                access.inputs.forEach(function(port) {
                    if (!input && (portId === '' || port.id === portId)) {
                        input = port;
                    }
                });

                if (input) {
                    input.onmidimessage = function(msg) {
                        // Forward to C++: status, data1, data2, timestamp
                        var d = msg.data;
                        Module._pulp_web_midi_message(
                            d.length > 0 ? d[0] : 0,
                            d.length > 1 ? d[1] : 0,
                            d.length > 2 ? d[2] : 0
                        );
                    };
                    window._pulpMidiInput = input;
                    console.log('Pulp: MIDI input connected: ' + input.name);
                }
            }).catch(function(err) {
                console.warn('Web MIDI access denied:', err);
            });
        }, port_id_.c_str());

        open_ = true;
        return true;
    }

    void close() override {
        if (!open_) return;
        EM_ASM({
            if (window._pulpMidiInput) {
                window._pulpMidiInput.onmidimessage = null;
                window._pulpMidiInput = null;
            }
        });
        open_ = false;
    }

    bool is_open() const override { return open_; }

    // Called from JS when a MIDI message arrives
    void on_message(uint8_t status, uint8_t data1, uint8_t data2) {
        if (!callback_) return;

        MidiEvent event;
        if ((status & 0xF0) == 0x90 && data2 > 0) {
            event = MidiEvent::note_on(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && data2 == 0)) {
            event = MidiEvent::note_off(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) == 0xB0) {
            event = MidiEvent::cc(status & 0x0F, data1, data2);
        } else {
            return; // Unsupported message type for now
        }

        callback_(event);
    }

private:
    MidiCallback callback_;
    std::string port_id_;
    bool open_ = false;
};

// Global instance for JS interop
static WebMidiInput* g_web_midi_input = nullptr;

// ── Web MIDI System ─────────────────────────────────────────────────────

class WebMidiSystem : public MidiSystem {
public:
    std::vector<MidiPortInfo> enumerate_inputs() override {
        // In WASM, we can't synchronously query ports.
        // Return a placeholder; actual port discovery happens in JS.
        return {{"web-midi-default", "Web MIDI Input"}};
    }

    std::vector<MidiPortInfo> enumerate_outputs() override {
        return {{"web-midi-out", "Web MIDI Output"}};
    }

    std::unique_ptr<MidiInput> create_input() override {
        auto input = std::make_unique<WebMidiInput>();
        g_web_midi_input = input.get();
        return input;
    }

    std::unique_ptr<MidiOutput> create_output() override {
        return nullptr; // TODO: implement Web MIDI output
    }
};

// ── Emscripten exports ──────────────────────────────────────────────────

extern "C" {

EMSCRIPTEN_KEEPALIVE
void _pulp_web_midi_message(uint8_t status, uint8_t data1, uint8_t data2) {
    if (g_web_midi_input) g_web_midi_input->on_message(status, data1, data2);
}

} // extern "C"

// Factory override for WASM builds
std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<WebMidiSystem>();
}

} // namespace pulp::midi

#endif // __EMSCRIPTEN__
