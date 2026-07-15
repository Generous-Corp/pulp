#include <catch2/catch_test_macros.hpp>

#include <pulp/format/validation_assertions.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::format::validation;

namespace {

// Minimal effect: output = input * gain. Exercises the state round-trip path
// (one parameter in the StateStore) without any custom plugin-state blob.
class GainEffect final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "GainEffect",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.gain-effect",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2, false}},
            .output_buses = {{"Main Out", 2, false}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "",
            .range = {0.0f, 2.0f, 1.0f, 0.0f},
        });
        store_ = &store;
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float gain = store_ ? store_->get_value(1) : 1.0f;
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            auto o = out.channel(ch);
            auto i = (ch < in.num_channels()) ? in.channel(ch)
                                              : std::span<const float>{};
            for (std::size_t n = 0; n < o.size(); ++n) {
                o[n] = (n < i.size() ? i[n] : 0.0f) * gain;
            }
        }
    }

private:
    state::StateStore* store_ = nullptr;
};

} // namespace

TEST_CASE("check_finite flags NaN and Inf", "[validation-assertions]") {
    std::vector<float> clean{0.0f, 0.5f, -0.5f, 1.0f};
    REQUIRE(check_finite(clean));

    std::vector<float> nan_buf{0.0f, std::numeric_limits<float>::quiet_NaN()};
    REQUIRE_FALSE(check_finite(nan_buf));

    std::vector<float> inf_buf{std::numeric_limits<float>::infinity(), 0.0f};
    auto r = check_finite(inf_buf);
    REQUIRE_FALSE(r);
    REQUIRE(r.message.find("Inf") != std::string::npos);
}

TEST_CASE("check_any_nonzero and check_silent are inverses", "[validation-assertions]") {
    std::vector<float> silent(64, 0.0f);
    REQUIRE(check_silent(silent));
    REQUIRE_FALSE(check_any_nonzero(silent));

    std::vector<float> signal(64, 0.0f);
    signal[10] = 0.25f;
    REQUIRE(check_any_nonzero(signal));
    REQUIRE_FALSE(check_silent(signal));
}

TEST_CASE("check_peak_below bounds magnitude", "[validation-assertions]") {
    std::vector<float> within{0.5f, -0.9f, 0.99f};
    REQUIRE(check_peak_below(within, 1.0f));

    std::vector<float> over{0.5f, 1.5f};
    REQUIRE_FALSE(check_peak_below(over, 1.0f));
}

TEST_CASE("check_silent and check_peak_below fail closed on non-finite",
          "[validation-assertions]") {
    std::vector<float> nan_buf{0.0f, std::numeric_limits<float>::quiet_NaN()};
    REQUIRE_FALSE(check_silent(nan_buf));      // NaN is never "silent"
    REQUIRE_FALSE(check_peak_below(nan_buf, 10.0f)); // and never under a bound

    std::vector<float> inf_buf{std::numeric_limits<float>::infinity()};
    REQUIRE_FALSE(check_peak_below(inf_buf, 1.0e9f));
}

TEST_CASE("check_param_round_trip handles linear, stepped, and skewed ranges",
          "[validation-assertions]") {
    // Linear continuous.
    REQUIRE(check_param_round_trip(state::ParamRange{-24.0f, 24.0f, 0.0f, 0.0f}, 5.0f));

    // Stepped: a value off the grid still round-trips idempotently after the
    // first projection snaps it to the nearest step.
    REQUIRE(check_param_round_trip(state::ParamRange{0.0f, 3.0f, 0.0f, 1.0f}, 1.7f));

    // Skewed (e.g. a frequency control). Endpoint anchors + stability hold.
    auto freq = state::ParamRange::with_center(20.0f, 20000.0f, 1000.0f);
    REQUIRE(check_param_round_trip(freq, 1000.0f, 1.0f));
    REQUIRE(check_param_round_trip(freq, 50.0f, 1.0f));

    // Degenerate constant parameter (min == max) is valid and round-trips.
    REQUIRE(check_param_round_trip(state::ParamRange{1.0f, 1.0f, 1.0f, 0.0f}, 1.0f));

    // Fail closed on a non-finite raw input.
    REQUIRE_FALSE(check_param_round_trip(
        state::ParamRange{0.0f, 1.0f, 0.0f, 0.0f},
        std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("check_state_round_trip verifies serialization determinism",
          "[validation-assertions]") {
    format::HeadlessHost host(
        []() -> std::unique_ptr<format::Processor> {
            return std::make_unique<GainEffect>();
        });
    host.prepare(48000.0, 512);
    host.state().set_value(1, 1.5f);
    REQUIRE(check_state_round_trip(host));
    // Value preserved through the round trip.
    REQUIRE(host.state().get_value(1) == 1.5f);
}

TEST_CASE("check_sysex_payload_equal compares bytes", "[validation-assertions]") {
    std::array<uint8_t, 5> a{0xF0, 0x7E, 0x01, 0x02, 0xF7};
    std::array<uint8_t, 5> b{0xF0, 0x7E, 0x01, 0x02, 0xF7};
    REQUIRE(check_sysex_payload_equal(a, b));

    std::array<uint8_t, 4> shorter{0xF0, 0x7E, 0x01, 0xF7};
    REQUIRE_FALSE(check_sysex_payload_equal(a, shorter));

    std::array<uint8_t, 5> different{0xF0, 0x7E, 0x09, 0x02, 0xF7};
    auto r = check_sysex_payload_equal(a, different);
    REQUIRE_FALSE(r);
    REQUIRE(r.message.find("byte 2") != std::string::npos);
}

TEST_CASE("check_midi_events_equal compares short messages and sysex",
          "[validation-assertions]") {
    midi::MidiBuffer a;
    a.add(midi::MidiEvent::note_on(0, 60, 100));
    a.add(midi::MidiEvent::cc(0, 7, 64));
    a.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 0);

    midi::MidiBuffer b;
    b.add(midi::MidiEvent::note_on(0, 60, 100));
    b.add(midi::MidiEvent::cc(0, 7, 64));
    b.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 0);
    REQUIRE(check_midi_events_equal(a, b));

    // Differing short message.
    midi::MidiBuffer c;
    c.add(midi::MidiEvent::note_on(0, 61, 100));
    c.add(midi::MidiEvent::cc(0, 7, 64));
    c.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 0);
    REQUIRE_FALSE(check_midi_events_equal(a, c));

    // Differing sysex payload.
    midi::MidiBuffer d;
    d.add(midi::MidiEvent::note_on(0, 60, 100));
    d.add(midi::MidiEvent::cc(0, 7, 64));
    d.add_sysex({0xF0, 0x7D, 0x02, 0xF7}, 0);
    REQUIRE_FALSE(check_midi_events_equal(a, d));
}

TEST_CASE("check_midi_events_equal compares the UMP sidecar",
          "[validation-assertions]") {
    midi::UmpPacket p1{};
    p1.word_count = 1;
    p1.words[0] = 0x40903C64u; // arbitrary MIDI 2.0 note-on-ish word
    midi::UmpPacket p2{};
    p2.word_count = 1;
    p2.words[0] = 0x40903D64u; // different note

    // A buffer with no UMP and a buffer with an empty UMP are equivalent.
    midi::MidiBuffer none_a, none_b;
    midi::UmpBuffer empty;
    none_b.attach_ump(&empty);
    REQUIRE(check_midi_events_equal(none_a, none_b));

    // Same UMP packets compare equal.
    midi::UmpBuffer ua, ub;
    ua.add(p1, 0);
    ub.add(p1, 0);
    midi::MidiBuffer a, b;
    a.attach_ump(&ua);
    b.attach_ump(&ub);
    REQUIRE(check_midi_events_equal(a, b));

    // A differing UMP packet must fail even when short MIDI/SysEx match.
    midi::UmpBuffer uc;
    uc.add(p2, 0);
    midi::MidiBuffer c;
    c.attach_ump(&uc);
    REQUIRE_FALSE(check_midi_events_equal(a, c));

    // Present-vs-absent UMP must fail (no silent false-pass).
    midi::MidiBuffer plain;
    REQUIRE_FALSE(check_midi_events_equal(a, plain));
}
