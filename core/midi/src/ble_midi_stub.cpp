// Cross-platform fallback BleMidiCentral for builds without a real
// backend wired (Linux/Windows scaffold, Android, headless tests).
//
// Reports is_available() == false so callers degrade gracefully; the
// scan / connect entry points are safe no-ops that surface
// BleMidiError::Unsupported through the state callback if invoked.
//
// On Apple targets the real CoreBluetooth-backed central in
// ble_midi_coremidi.mm overrides the create_ble_midi_central()
// definition via its own translation unit; this file is only
// compiled for non-Apple platforms (see CMakeLists.txt).

#include <pulp/midi/ble_midi.hpp>

#include <memory>
#include <string>
#include <vector>

namespace pulp::midi {

namespace {

class StubBleMidiCentral final : public BleMidiCentral {
public:
    bool is_available() const override { return false; }

    bool start_scan(BleMidiScanCallback) override { return false; }
    void stop_scan() override {}
    bool is_scanning() const override { return false; }

    std::vector<BleMidiPeripheral> known_peripherals() const override {
        return {};
    }

    bool connect(const std::string& id) override {
        if (state_cb_) {
            state_cb_(id, BleMidiConnectionState::Failed,
                      BleMidiError::Unsupported);
        }
        return false;
    }

    void disconnect(const std::string&) override {}

    void set_state_callback(BleMidiStateCallback cb) override {
        state_cb_ = std::move(cb);
    }

    std::string midi_input_port_for(const std::string&) const override {
        return {};
    }
    std::string midi_output_port_for(const std::string&) const override {
        return {};
    }

private:
    BleMidiStateCallback state_cb_;
};

}  // namespace

std::unique_ptr<BleMidiCentral> create_ble_midi_central() {
    return std::make_unique<StubBleMidiCentral>();
}

}  // namespace pulp::midi
