// Processor::on_host_transport_changed tests (workstream 01 slice 1.11).

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <stdexcept>

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

TEST_CASE("repeated locates within a single play span each land",
          "[processor][transport][rapid]") {
    // DAWs emit locate events as the user scrubs. Nothing in the hook
    // contract debounces or coalesces — every change should reach the
    // plugin so it can re-cue internal state.
    TransportAwareProcessor p;
    p.on_host_transport_changed(true, 0.0);
    p.on_host_transport_changed(true, 1.0);
    p.on_host_transport_changed(true, 5.0);
    p.on_host_transport_changed(true, 5.5);
    p.on_host_transport_changed(true, 12.25);
    REQUIRE(p.calls == 5);
    REQUIRE(p.last_playing);
    REQUIRE(p.last_pos == 12.25);
}

TEST_CASE("on_host_transport_changed tolerates extreme and exotic positions",
          "[processor][transport][edge]") {
    TransportAwareProcessor p;

    // DAW-style looping can produce negative positions (pre-roll),
    // very-large positions (hours of session), and sub-sample
    // fractions. The hook is specified as a plain double — plugins
    // must get the exact value without saturation or truncation.
    p.on_host_transport_changed(true, -1.5);
    REQUIRE(p.last_pos == -1.5);
    p.on_host_transport_changed(true, 1.0e12);
    REQUIRE(p.last_pos == 1.0e12);
    p.on_host_transport_changed(true, 1.0 / 3.0);
    REQUIRE(p.last_pos == 1.0 / 3.0);
    REQUIRE(p.calls == 3);
}

TEST_CASE("transport hook doesn't swallow play->stop->play transition",
          "[processor][transport][state]") {
    TransportAwareProcessor p;
    p.on_host_transport_changed(true, 0.0);
    p.on_host_transport_changed(false, 4.0);
    REQUIRE_FALSE(p.last_playing);
    p.on_host_transport_changed(true, 4.0);   // resume at stop point
    REQUIRE(p.last_playing);
    REQUIRE(p.last_pos == 4.0);
    REQUIRE(p.calls == 3);
}

TEST_CASE("exception thrown by transport hook propagates to the caller",
          "[processor][transport][exception]") {
    // Contract: Pulp's base Processor declares the hook as a plain
    // virtual, not noexcept. Callers (format adapters) must be
    // prepared for implementations to throw. This case pins that
    // behaviour so future changes (e.g. adding noexcept) become
    // intentional.
    class ThrowingProcessor : public PlainProcessor {
    public:
        void on_host_transport_changed(bool, double) override {
            throw std::runtime_error("transport hook exploded");
        }
    };
    ThrowingProcessor p;
    REQUIRE_THROWS_AS(p.on_host_transport_changed(true, 0.0), std::runtime_error);
}
