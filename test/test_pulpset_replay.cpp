// Self-test for the `.pulpset` render/replay harness (delivery item "G4").
// Drives a deterministic in-test Processor (a gated sine whose level is a parameter) so
// that param + MIDI steers in a script produce measurable, reproducible audio changes —
// exactly the offline-render/regression capability the generative-plugin demos rely on.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/pulpset.hpp>
#include <pulp/format/processor.hpp>

#include <cmath>

using namespace pulp;

namespace {

// Deterministic instrument: a 440 Hz sine, gated by MIDI note-on/off, scaled by param 0.
class GatedSine : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "GatedSine",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.gatedsine",
            .version = "0.0.1",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Out", 2}},
            .accepts_midi = true,
        };
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({0, "Level", "", {0.0f, 1.0f, 0.5f}});
    }
    void prepare(const format::PrepareContext& ctx) override { sr_ = (float)ctx.sample_rate; }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        for (const auto& ev : midi_in) {
            if (ev.is_note_on() && ev.velocity() > 0) gate_ = true;
            else if (ev.is_note_off() || ev.is_note_on()) gate_ = false;
        }
        const float level = state().get_value(0);
        const std::size_t ns = out.num_samples();
        auto L = out.channel(0);
        auto R = out.num_channels() > 1 ? out.channel(1) : out.channel(0);
        for (std::size_t i = 0; i < ns; ++i) {
            float s = gate_ ? std::sin(phase_) * level : 0.0f;
            phase_ += 2.0f * 3.14159265f * 440.0f / sr_;
            if (phase_ > 6.2831853f) phase_ -= 6.2831853f;
            L[i] = s; R[i] = s;
        }
    }
private:
    float sr_ = 48000.0f, phase_ = 0.0f;
    bool gate_ = false;
};

std::unique_ptr<format::HeadlessHost> make_host() {
    auto host = std::make_unique<format::HeadlessHost>(
        [] { return std::unique_ptr<format::Processor>(std::make_unique<GatedSine>()); });
    host->prepare(48000.0, 512, /*in*/0, /*out*/2);
    return host;
}

} // namespace

TEST_CASE("Pulpset::parse reads param + note events sorted by sample", "[pulpset][g4]") {
    auto ps = format::Pulpset::parse(
        "# arrangement\n"
        "24000 param 0 0.9\n"
        "0 note_on 60 100\n"
        "0 param 0 0.2\n");
    REQUIRE(ps.events.size() == 3);
    REQUIRE(ps.events.front().sample == 0);           // sorted
    REQUIRE(ps.events.back().sample == 24000);
}

TEST_CASE("Pulpset replay: a param steer mid-render changes the audio", "[pulpset][g4]") {
    auto host = make_host();
    // Note on for the whole render; Level 0.2 for the first half, 0.9 for the second.
    auto ps = format::Pulpset::parse(
        "0 note_on 60 100\n"
        "0 param 0 0.2\n"
        "24000 param 0 0.9\n");
    auto r = format::render(*host, ps, /*total_samples=*/48000, /*block=*/512);

    REQUIRE(r.frames() == 48000);
    double seg0 = format::segment_rms(r.left, 0, 24000);
    double seg1 = format::segment_rms(r.left, 24000, 48000);
    REQUIRE(seg0 > 1e-3);                  // audible (note gated on)
    REQUIRE(seg1 > seg0 * 2.0);            // the 0.2 -> 0.9 steer measurably raised level
}

TEST_CASE("Pulpset replay: MIDI note_off gates the instrument silent", "[pulpset][g4]") {
    auto host = make_host();
    auto ps = format::Pulpset::parse(
        "0 note_on 60 100\n"
        "0 param 0 0.8\n"
        "24000 note_off 60\n");
    auto r = format::render(*host, ps, 48000, 512);
    double on = format::segment_rms(r.left, 0, 24000);
    double off = format::segment_rms(r.left, 24576, 48000);   // skip the boundary block
    REQUIRE(on > 1e-3);
    REQUIRE(off < 1e-5);                   // silent after note_off
}

TEST_CASE("Pulpset replay is deterministic (bit-exact across runs)", "[pulpset][g4]") {
    auto ps = format::Pulpset::parse("0 note_on 60 100\n0 param 0 0.7\n");
    auto h1 = make_host(); auto a = format::render(*h1, ps, 8000, 512);
    auto h2 = make_host(); auto b = format::render(*h2, ps, 8000, 512);
    REQUIRE(a.left == b.left);
    REQUIRE(a.right == b.right);
}
