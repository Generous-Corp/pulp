#include <pulp/midi/ble_midi_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeAndroidBlePeripheral {
  public:
    FakeAndroidBlePeripheral(std::string id, std::string name)
        : id_(std::move(id)), name_(std::move(name)) {}

    void connect() {
        auto& registry = pulp::midi::BleMidiPortRegistry::instance();
        registry.register_input(input_id(), name_);
        registry.register_output(output_id(), name_, [this](const std::vector<uint8_t>& bytes) {
            writes_.push_back(bytes);
        });
        ++registration_events_;
    }

    void receive(const std::vector<uint8_t>& bytes, double timestamp) {
        pulp::midi::BleMidiPortRegistry::instance().deliver_message(input_id(), bytes, timestamp);
    }

    void disconnect() {
        auto& registry = pulp::midi::BleMidiPortRegistry::instance();
        registry.unregister_input(input_id());
        registry.unregister_output(output_id());
        ++teardown_events_;
    }

    std::string input_id() const {
        return "ble-midi-in:" + id_;
    }
    std::string output_id() const {
        return "ble-midi-out:" + id_;
    }
    const std::vector<std::vector<uint8_t>>& writes() const {
        return writes_;
    }
    int registration_events() const {
        return registration_events_;
    }
    int teardown_events() const {
        return teardown_events_;
    }

  private:
    std::string id_;
    std::string name_;
    std::vector<std::vector<uint8_t>> writes_;
    int registration_events_ = 0;
    int teardown_events_ = 0;
};

} // namespace

TEST_CASE("Android BLE MIDI registry routes a fake peripheral lifecycle",
          "[ble-midi][android][registry]") {
    FakeAndroidBlePeripheral peripheral("AA:BB:CC:DD:EE:FF", "Fake BLE MIDI");
    auto& registry = pulp::midi::BleMidiPortRegistry::instance();
    std::vector<uint8_t> received;
    double received_at = 0.0;

    peripheral.connect();
    REQUIRE(peripheral.registration_events() == 1);
    REQUIRE(registry.is_input(peripheral.input_id()));
    REQUIRE(registry.is_output(peripheral.output_id()));
    REQUIRE(registry.attach_input(peripheral.input_id(),
                                  [&](const pulp::midi::MidiEvent& event) {
                                      received.assign(event.message.data(),
                                                      event.message.data() + event.message.size());
                                      received_at = event.timestamp;
                                  },
                                  {}));

    const std::vector<uint8_t> note_on{0x90, 0x3c, 0x64};
    peripheral.receive(note_on, 0.125);
    REQUIRE(received == note_on);
    REQUIRE(received_at == 0.125);

    auto output = registry.output_sink(peripheral.output_id());
    REQUIRE(output);
    const std::vector<uint8_t> note_off{0x80, 0x3c, 0x00};
    output(note_off);
    REQUIRE(peripheral.writes() == std::vector<std::vector<uint8_t>>{note_off});

    peripheral.disconnect();
    REQUIRE(peripheral.teardown_events() == 1);
    REQUIRE_FALSE(registry.is_input(peripheral.input_id()));
    REQUIRE_FALSE(registry.is_output(peripheral.output_id()));

    output(note_on);
    REQUIRE(peripheral.writes() == std::vector<std::vector<uint8_t>>{note_off});
}
