// The instrument makes sound, and its panel is wired to the synth that does.
//
// The sibling effect example shipped once with a panel bound to macro names
// the plugin did not declare: zero of five controls resolved, and it passed
// dlopen, auval, clap-validator, a headless screenshot and the native-render
// invariants. It rendered beautifully and drove nothing.
//
// These are the assertions that class of failure cannot survive, restated for
// an instrument — where the equivalent silent failure is a synth that scans,
// opens, shows a panel, and never sounds.

#include "design_panel_instrument.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace {

constexpr double kSampleRate = 48000.0;

struct Rendered {
    std::vector<float> left;
    std::size_t voices_after = 0;
};

/// Render `frames` through a fresh instrument, feeding `midi` at block start.
Rendered render(const std::vector<std::pair<state::ParamID, float>>& params,
                const std::vector<midi::MidiEvent>& events,
                std::size_t frames) {
    DesignPanelInstrument plugin;
    state::StateStore store;
    plugin.set_state_store(&store);
    plugin.define_parameters(store);
    for (const auto& [id, value] : params) store.set_value(id, value);
    plugin.prepare({
        .sample_rate = kSampleRate,
        .max_buffer_size = static_cast<int>(frames),
        .input_channels = 0,
        .output_channels = 1,
    });

    std::vector<float> out(frames, 0.0f);
    float* out_ch[1] = {out.data()};
    audio::BufferView<float> ob(out_ch, 1, frames);
    audio::BufferView<const float> ib(nullptr, 0, 0);
    midi::MidiBuffer m_in, m_out;
    for (const auto& e : events) m_in.add(e);
    format::ProcessContext pc;
    pc.sample_rate = kSampleRate;
    pc.num_samples = static_cast<int>(frames);
    plugin.process(ob, ib, m_in, m_out, pc);
    return {std::move(out), plugin.synth_for_test().active_voices()};
}

float peak(const std::vector<float>& x) {
    float p = 0.0f;
    for (const float s : x) p = std::max(p, std::fabs(s));
    return p;
}

float rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    to = std::min(to, x.size());
    if (to <= from) return 0.0f;
    for (std::size_t i = from; i < to; ++i) sum += double(x[i]) * x[i];
    return static_cast<float>(std::sqrt(sum / double(to - from)));
}

/// High-frequency energy, as the first difference. Distinguishes a filter from
/// a gain, which comparing total energy cannot.
double hf_energy(const std::vector<float>& x) {
    double sum = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        const double d = x[i] - x[i - 1];
        sum += d * d;
    }
    return sum;
}

const std::vector<std::pair<state::ParamID, float>> kDefaults = {
    {kAttack, 0.005f}, {kRelease, 0.05f},
    {kCutoff, 0.8f}, {kResonance, 0.1f}, {kDrive, 0.0f}};

}  // namespace

TEST_CASE("an instrument with no notes is exactly silent",
          "[design-panel-instrument][audio]") {
    // Exactly, not approximately. An instrument that leaks anything at idle
    // adds noise to every silent bar of a session, and a threshold would pass
    // a buffer the plugin simply forgot to clear.
    const auto r = render(kDefaults, {}, 4096);
    for (const float s : r.left) CHECK(s == 0.0f);
    CHECK(r.voices_after == 0);
}

TEST_CASE("a note produces sound", "[design-panel-instrument][audio]") {
    const auto r = render(kDefaults, {midi::MidiEvent::note_on(0, 60, 100)}, 4096);
    CHECK(peak(r.left) > 0.01f);
    CHECK(r.voices_after == 1);
}

TEST_CASE("note-off releases the voice", "[design-panel-instrument][audio]") {
    // The note ends and the voice frees. A synth that never releases runs out
    // of voices after eight notes and then goes silent for the rest of the
    // session — the kind of defect that only appears after a minute of play.
    const float release = 0.02f;
    const auto frames = static_cast<std::size_t>(kSampleRate * 0.5);
    auto params = kDefaults;
    params[1] = {kRelease, release};

    const auto held = render(params, {midi::MidiEvent::note_on(0, 60, 100)}, frames);
    CHECK(held.voices_after == 1);

    const auto stopped = render(
        params,
        {midi::MidiEvent::note_on(0, 60, 100),
         midi::MidiEvent::note_off(static_cast<int32_t>(frames / 4), 60)},
        frames);
    CHECK(stopped.voices_after == 0);
    // And the tail is actually quiet, not merely marked inactive.
    CHECK(rms(stopped.left, frames * 3 / 4, frames) < 0.001f);
}

TEST_CASE("attack shapes the onset", "[design-panel-instrument][audio]") {
    // A long attack must be quieter early than a short one. Comparing peaks
    // over the whole buffer would not distinguish them, since both reach full
    // level eventually.
    const auto frames = static_cast<std::size_t>(kSampleRate * 0.2);
    auto fast = kDefaults, slow = kDefaults;
    fast[0] = {kAttack, 0.001f};
    slow[0] = {kAttack, 0.5f};

    const auto a = render(fast, {midi::MidiEvent::note_on(0, 60, 100)}, frames);
    const auto b = render(slow, {midi::MidiEvent::note_on(0, 60, 100)}, frames);
    const auto window = static_cast<std::size_t>(kSampleRate * 0.01);
    CHECK(rms(a.left, 0, window) > rms(b.left, 0, window) * 2.0f);
}

TEST_CASE("cutoff changes brightness, not just level",
          "[design-panel-instrument][audio]") {
    const auto frames = static_cast<std::size_t>(kSampleRate * 0.2);
    auto dark = kDefaults, bright = kDefaults;
    dark[2] = {kCutoff, 0.15f};
    bright[2] = {kCutoff, 0.95f};

    const auto d = render(dark, {midi::MidiEvent::note_on(0, 60, 100)}, frames);
    const auto b = render(bright, {midi::MidiEvent::note_on(0, 60, 100)}, frames);
    CHECK(hf_energy(d.left) < hf_energy(b.left));
}

TEST_CASE("the synth is polyphonic", "[design-panel-instrument][audio]") {
    // A chord holds three voices. A monophonic bug — each note stealing the
    // last — still makes sound, so peak level alone would not catch it.
    const auto r = render(kDefaults,
                          {midi::MidiEvent::note_on(0, 60, 100),
                           midi::MidiEvent::note_on(0, 64, 100),
                           midi::MidiEvent::note_on(0, 67, 100)},
                          4096);
    CHECK(r.voices_after == 3);
}

TEST_CASE("a repeated note retriggers rather than stacking",
          "[design-panel-instrument][audio]") {
    // Same note twice is one voice. Stacking doubles the level of every
    // repeated key, which a held sustain pedal turns into clipping.
    const auto r = render(kDefaults,
                          {midi::MidiEvent::note_on(0, 60, 100),
                           midi::MidiEvent::note_on(64, 60, 100)},
                          4096);
    CHECK(r.voices_after == 1);
}

TEST_CASE("output stays finite and bounded at extremes",
          "[design-panel-instrument][audio][rt-safety]") {
    // Every macro at maximum, eight voices, full drive and resonance. A user
    // can do this; self-oscillation is musical, divergence is a defect.
    std::vector<midi::MidiEvent> chord;
    for (int n = 0; n < 8; ++n)
        chord.push_back(midi::MidiEvent::note_on(0, 48 + n * 3, 127));

    const auto r = render({{kAttack, 0.001f}, {kRelease, 8.0f},
                           {kCutoff, 1.0f}, {kResonance, 1.0f}, {kDrive, 1.0f}},
                          chord, static_cast<std::size_t>(kSampleRate));
    for (const float s : r.left) {
        REQUIRE(std::isfinite(s));
        CHECK(std::fabs(s) < 16.0f);
    }
}

TEST_CASE("every control the panel declares resolves to a parameter",
          "[design-panel-instrument][binding]") {
    // The failure this catches is silent by construction: a control whose key
    // matches nothing renders, turns, and moves no parameter. No screenshot
    // and no validator can show it.
    DesignPanelInstrument plugin;
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

TEST_CASE("the descriptor is an instrument, not an effect",
          "[design-panel-instrument][format]") {
    // What makes a host scan this as `aumu` and offer it on an instrument
    // track. Getting it wrong produces a plugin that loads fine and cannot be
    // played, which no audio assertion above would notice.
    DesignPanelInstrument plugin;
    const auto d = plugin.descriptor();
    CHECK(d.category == format::PluginCategory::Instrument);
    CHECK(d.accepts_midi);
    CHECK(d.input_buses.empty());
    REQUIRE_FALSE(d.output_buses.empty());
    // Infinite: the release outlives note-off and a note can arrive any time.
    CHECK(d.tail_samples == -1);
}
