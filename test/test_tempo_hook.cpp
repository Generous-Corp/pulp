// Processor::on_host_tempo_changed hook tests (workstream 01 slice 1.10).

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

class TempoAwareProcessor : public PlainProcessor {
public:
    double last_bpm = 0.0;
    int calls = 0;
    void on_host_tempo_changed(double bpm) override {
        last_bpm = bpm;
        ++calls;
    }
};

} // namespace

TEST_CASE("default on_host_tempo_changed is a no-op",
          "[processor][tempo]") {
    PlainProcessor p;
    p.on_host_tempo_changed(128.0);
    p.on_host_tempo_changed(90.5);
    SUCCEED("default override returned cleanly");
}

TEST_CASE("overriding plugin receives tempo updates",
          "[processor][tempo]") {
    TempoAwareProcessor p;
    p.on_host_tempo_changed(120.0);
    REQUIRE(p.calls == 1);
    REQUIRE(p.last_bpm == 120.0);
    p.on_host_tempo_changed(174.0);
    REQUIRE(p.calls == 2);
    REQUIRE(p.last_bpm == 174.0);
}
