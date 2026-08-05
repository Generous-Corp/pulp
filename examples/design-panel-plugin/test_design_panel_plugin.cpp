// The plugin processes audio, and its panel is wired to the DSP that does it.
//
// Every check this example already had — dlopen, auval, clap-validator,
// headless screenshot, the native-invariants ctest — passes on a plugin whose
// process() copies input to output and whose knobs drive nothing. That was in
// fact its state: the panel bound `time`/`feedback`/`tone`/`mix` while the
// plugin declared `Macro A` and `Macro B`, so zero of five controls resolved.
// It rendered beautifully and was silent.
//
// So these are the assertions that class of failure cannot survive.

#include "design_panel_plugin.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace {

constexpr double kSampleRate = 48000.0;

/// Render `frames` of audio through a fresh plugin, with `input` as channel 0.
std::vector<float> render(const std::vector<std::pair<state::ParamID, float>>& params,
                          const std::vector<float>& input) {
    DesignPanelProcessor plugin;
    state::StateStore store;
    plugin.set_state_store(&store);
    plugin.define_parameters(store);
    for (const auto& [id, value] : params) store.set_value(id, value);
    plugin.prepare({
        .sample_rate = kSampleRate,
        .max_buffer_size = static_cast<int>(input.size()),
        .input_channels = 1,
        .output_channels = 1,
    });

    std::vector<float> out = input;
    float* out_ch[1] = {out.data()};
    const float* in_ch[1] = {input.data()};
    audio::BufferView<float> ob(out_ch, 1, input.size());
    audio::BufferView<const float> ib(in_ch, 1, input.size());
    midi::MidiBuffer m_in, m_out;
    format::ProcessContext pc;
    pc.sample_rate = kSampleRate;
    pc.num_samples = static_cast<int>(input.size());
    plugin.process(ob, ib, m_in, m_out, pc);
    return out;
}

std::vector<float> impulse(std::size_t frames) {
    std::vector<float> v(frames, 0.0f);
    v[0] = 1.0f;
    return v;
}

/// Sample indices where |x| rises above a threshold, one per contiguous burst.
std::vector<std::size_t> peaks(const std::vector<float>& x, float threshold) {
    std::vector<std::size_t> found;
    bool above = false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const bool now = std::fabs(x[i]) > threshold;
        if (now && !above) found.push_back(i);
        above = now;
    }
    return found;
}

}  // namespace

TEST_CASE("mix at 0 leaves the signal bit-identical", "[design-panel][audio]") {
    // Not "quiet" — identical. A wet path leaking at mix 0 is the difference
    // between a bypass a mastering engineer trusts and one they do not, and an
    // amplitude threshold would pass a signal that is merely close.
    const auto in = impulse(4096);
    const auto out = render({{kTime, 0.05f}, {kFeedback, 0.7f},
                             {kTone, 1.0f}, {kMix, 0.0f}}, in);
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) CHECK(out[i] == in[i]);
}

TEST_CASE("an impulse produces repeats at the delay time", "[design-panel][audio]") {
    // The claim "it is a delay" is exactly this: energy reappears one delay
    // period later, and again after that.
    const float seconds = 0.05f;
    const auto expected = static_cast<std::size_t>(seconds * kSampleRate);
    const auto out = render({{kTime, seconds}, {kFeedback, 0.6f},
                             {kTone, 1.0f}, {kMix, 1.0f}}, impulse(16384));

    const auto found = peaks(out, 0.05f);
    REQUIRE(found.size() >= 3);

    // Within a millisecond of the requested period. The read head is smoothed
    // toward its target rather than jumped to, so the first repeat lands a
    // little late by design; the spacing is what must hold.
    const auto tolerance = static_cast<std::size_t>(0.001 * kSampleRate);
    for (std::size_t i = 1; i + 1 < found.size(); ++i) {
        const auto gap = found[i + 1] - found[i];
        CHECK(gap > expected - tolerance);
        CHECK(gap < expected + tolerance);
    }
}

TEST_CASE("feedback controls how long the tail runs", "[design-panel][audio]") {
    const auto low = render({{kTime, 0.02f}, {kFeedback, 0.2f},
                             {kTone, 1.0f}, {kMix, 1.0f}}, impulse(16384));
    const auto high = render({{kTime, 0.02f}, {kFeedback, 0.9f},
                              {kTone, 1.0f}, {kMix, 1.0f}}, impulse(16384));
    CHECK(peaks(high, 0.02f).size() > peaks(low, 0.02f).size());
}

TEST_CASE("tone darkens the repeats", "[design-panel][audio]") {
    // Measured as high-frequency energy in the tail: a dark setting must lose
    // treble the bright one keeps. Comparing total energy would not
    // distinguish a filter from a gain.
    const auto hf_energy = [](const std::vector<float>& x) {
        double sum = 0.0;
        for (std::size_t i = 1; i < x.size(); ++i) {
            const double d = x[i] - x[i - 1];  // first difference ~ highpass
            sum += d * d;
        }
        return sum;
    };
    const auto dark = render({{kTime, 0.02f}, {kFeedback, 0.7f},
                              {kTone, 0.0f}, {kMix, 1.0f}}, impulse(16384));
    const auto bright = render({{kTime, 0.02f}, {kFeedback, 0.7f},
                                {kTone, 1.0f}, {kMix, 1.0f}}, impulse(16384));
    CHECK(hf_energy(dark) < hf_energy(bright));
}

TEST_CASE("feedback cannot run away", "[design-panel][audio][rt-safety]") {
    // A user can drag feedback to its maximum; the plugin must stay finite and
    // bounded there. Self-oscillation is musical, a divergence is a defect.
    const auto out = render({{kTime, 0.01f}, {kFeedback, 1.0f},
                             {kTone, 1.0f}, {kMix, 1.0f}},
                            impulse(48000));
    for (const float s : out) {
        REQUIRE(std::isfinite(s));
        CHECK(std::fabs(s) < 8.0f);
    }
}

TEST_CASE("every control the panel declares resolves to a parameter",
          "[design-panel][binding]") {
    // The failure this catches is silent by construction: a control whose key
    // matches nothing renders, turns, and moves no parameter. It cost this
    // example a full round of four-surface verification to notice.
    DesignPanelProcessor plugin;
    state::StateStore store;
    plugin.set_state_store(&store);
    plugin.define_parameters(store);

    auto editor = plugin.create_view();
    REQUIRE(editor != nullptr);

    const auto* ctx = plugin.binding_for_test();
    REQUIRE(ctx != nullptr);
    const auto& resolved = ctx->resolutions();
    REQUIRE_FALSE(resolved.empty());
    for (const auto& [key, ok] : resolved) {
        INFO("control bound to pulpParamKey '" << key << "'");
        CHECK(ok);
    }
}
