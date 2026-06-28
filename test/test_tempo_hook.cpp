// Processor::on_host_tempo_changed hook tests.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <cmath>
#include <limits>
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
    REQUIRE_NOTHROW(p.on_host_tempo_changed(128.0));
    REQUIRE_NOTHROW(p.on_host_tempo_changed(90.5));
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

// ── Edge BPM values ─────────────────────────────────────────────────────

TEST_CASE("tempo hook tolerates zero and negative BPM values",
          "[processor][tempo][edge]") {
    // DAW buffer glitches can briefly surface 0.0 or negative BPM while
    // the transport is switching modes. The hook contract is "receive
    // the value verbatim" — plugins guard their own state; the hook
    // must not saturate or clamp.
    TempoAwareProcessor p;
    p.on_host_tempo_changed(0.0);
    REQUIRE(p.last_bpm == 0.0);
    p.on_host_tempo_changed(-10.0);
    REQUIRE(p.last_bpm == -10.0);
    REQUIRE(p.calls == 2);
}

TEST_CASE("tempo hook preserves extreme BPM values without loss",
          "[processor][tempo][edge]") {
    TempoAwareProcessor p;
    // Absurd lows and highs cover the usual "off-the-chart" fuzz cases.
    p.on_host_tempo_changed(0.1);
    REQUIRE(p.last_bpm == 0.1);
    p.on_host_tempo_changed(9999.0);
    REQUIRE(p.last_bpm == 9999.0);
    p.on_host_tempo_changed(std::numeric_limits<double>::max());
    REQUIRE(p.last_bpm == std::numeric_limits<double>::max());
}

TEST_CASE("tempo hook tolerates subnormal fractions without rounding",
          "[processor][tempo][edge]") {
    TempoAwareProcessor p;
    // MIDI clock / tap-tempo paths can produce arbitrary fractions.
    p.on_host_tempo_changed(120.3333333333);
    REQUIRE(p.last_bpm == 120.3333333333);
    p.on_host_tempo_changed(1.0 / 3.0);
    REQUIRE(p.last_bpm == 1.0 / 3.0);
}

// ── Rapid / same-block changes ──────────────────────────────────────────

TEST_CASE("rapid tempo changes within a single audio block each land",
          "[processor][tempo][rapid]") {
    // Automation lanes can emit many tempo updates within one buffer
    // when the tempo curve is dense. Every change must reach the
    // plugin — Pulp does not coalesce tempo events.
    TempoAwareProcessor p;
    const double values[] = {120.0, 120.5, 121.0, 121.5, 122.0, 122.5, 123.0};
    for (double v : values) p.on_host_tempo_changed(v);
    REQUIRE(p.calls == static_cast<int>(sizeof(values) / sizeof(values[0])));
    REQUIRE(p.last_bpm == 123.0);
}

TEST_CASE("identical tempo re-notifications still fire the hook",
          "[processor][tempo][idempotence]") {
    // The hook contract doesn't promise de-duplication — some hosts
    // re-notify on every process block regardless of change. A plugin
    // that debounces internally depends on seeing every call.
    TempoAwareProcessor p;
    p.on_host_tempo_changed(120.0);
    p.on_host_tempo_changed(120.0);
    p.on_host_tempo_changed(120.0);
    REQUIRE(p.calls == 3);
    REQUIRE(p.last_bpm == 120.0);
}

// ── Exception safety ───────────────────────────────────────────────────

TEST_CASE("exception thrown by tempo hook propagates to the caller",
          "[processor][tempo][exception]") {
    // Pins the current contract: the base declaration is a plain
    // virtual, not noexcept. Future noexcept changes must be
    // intentional — this test fails loudly if the hook is silenced.
    class ThrowingProcessor : public PlainProcessor {
    public:
        void on_host_tempo_changed(double) override {
            throw std::runtime_error("tempo hook blew up");
        }
    };
    ThrowingProcessor p;
    REQUIRE_THROWS_AS(p.on_host_tempo_changed(120.0), std::runtime_error);
}
