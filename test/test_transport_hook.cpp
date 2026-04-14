// Processor::on_host_transport_changed tests (workstream 01 slice 1.11).

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

using namespace pulp::format;

namespace {

class PlainProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override { return {}; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

class TransportAwareProcessor : public PlainProcessor {
public:
    bool last_playing = false;
    double last_pos = -1.0;
    int calls = 0;
    void on_host_transport_changed(bool playing, double pos) override {
        last_playing = playing;
        last_pos = pos;
        ++calls;
    }
};

} // namespace

TEST_CASE("default on_host_transport_changed is a no-op",
          "[processor][transport]") {
    PlainProcessor p;
    p.on_host_transport_changed(true, 0.0);
    p.on_host_transport_changed(false, 42.5);
    SUCCEED("default override returned cleanly");
}

TEST_CASE("overriding plugin receives play/stop + locate events",
          "[processor][transport]") {
    TransportAwareProcessor p;
    p.on_host_transport_changed(true, 0.0);
    REQUIRE(p.calls == 1);
    REQUIRE(p.last_playing);
    REQUIRE(p.last_pos == 0.0);

    p.on_host_transport_changed(true, 10.5);   // mid-play locate
    REQUIRE(p.calls == 2);
    REQUIRE(p.last_pos == 10.5);

    p.on_host_transport_changed(false, 10.5);  // stop
    REQUIRE(p.calls == 3);
    REQUIRE_FALSE(p.last_playing);
}
