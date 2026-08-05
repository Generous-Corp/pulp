// The MIDI effect generates notes, and its panel is wired to the arpeggiator
// that does.
//
// The failure mode this plugin class ships with is the STUCK NOTE: an
// arpeggiator that emits a note-on and never the matching note-off leaves the
// instrument downstream droning, and the user can only clear it with a panic
// message. It survives every structural check — the plugin loads, the panel
// renders, notes come out — so it is asserted directly here.

#include "design_panel_midi_effect.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace {

constexpr double kSampleRate = 48000.0;

struct Output {
    std::vector<midi::MidiEvent> events;
    std::size_t held_after = 0;
    int sounding_after = -1;
};

/// Run the plugin over `blocks` blocks of `block` frames, feeding `events` at
/// their stated offsets in the FIRST block only.
Output run(const std::vector<std::pair<state::ParamID, float>>& params,
           const std::vector<midi::MidiEvent>& input,
           int block, int blocks) {
    DesignPanelMidiEffect plugin;
    state::StateStore store;
    plugin.set_state_store(&store);
    plugin.define_parameters(store);
    for (const auto& [id, value] : params) store.set_value(id, value);
    plugin.prepare({
        .sample_rate = kSampleRate,
        .max_buffer_size = block,
        .input_channels = 0,
        .output_channels = 0,
    });

    Output result;
    audio::BufferView<float> ob(nullptr, 0, 0);
    audio::BufferView<const float> ib(nullptr, 0, 0);
    for (int b = 0; b < blocks; ++b) {
        midi::MidiBuffer m_in, m_out;
        if (b == 0) for (const auto& e : input) m_in.add(e);
        format::ProcessContext pc;
        pc.sample_rate = kSampleRate;
        pc.num_samples = block;
        plugin.process(ob, ib, m_in, m_out, pc);
        for (std::size_t i = 0; i < m_out.size(); ++i)
            result.events.push_back(m_out[i]);
    }
    result.held_after = plugin.arp_for_test().held();
    result.sounding_after = plugin.arp_for_test().sounding_note();
    return result;
}

std::size_t count_on(const std::vector<midi::MidiEvent>& e) {
    return static_cast<std::size_t>(std::count_if(e.begin(), e.end(),
        [](const midi::MidiEvent& m) { return m.is_note_on() && m.velocity() > 0; }));
}

/// Net open notes per pitch. Anything non-zero at the end is a stuck note.
std::map<int, int> balance(const std::vector<midi::MidiEvent>& e) {
    std::map<int, int> net;
    for (const auto& m : e) {
        if (m.is_note_on() && m.velocity() > 0) ++net[m.note()];
        else if (m.is_note_off() || m.is_note_on()) --net[m.note()];
    }
    return net;
}

const std::vector<std::pair<state::ParamID, float>> kDefaults = {
    {kRate, 8.0f}, {kGate, 0.5f}, {kSwing, 0.0f},
    {kOctaves, 1.0f}, {kVelocity, 0.8f}};

}  // namespace

TEST_CASE("no input notes produce no output", "[design-panel-arp][midi]") {
    const auto r = run(kDefaults, {}, 512, 40);
    CHECK(r.events.empty());
}

TEST_CASE("a held note is arpeggiated", "[design-panel-arp][midi]") {
    // 8 Hz over ~0.85s is about seven steps. Asserting "more than one" rather
    // than an exact count keeps the test about arpeggiating rather than about
    // block arithmetic.
    const auto r = run(kDefaults, {midi::MidiEvent::note_on(0, 60, 100)}, 512, 80);
    CHECK(count_on(r.events) > 3);
    for (const auto& e : r.events)
        if (e.is_note_on() && e.velocity() > 0) CHECK(e.note() == 60);
}

TEST_CASE("every generated note-on is matched by a note-off",
          "[design-panel-arp][midi]") {
    // The stuck-note assertion. Runs a chord, then releases every key, and
    // requires the ledger to balance to zero for every pitch.
    auto input = std::vector<midi::MidiEvent>{
        midi::MidiEvent::note_on(0, 60, 100),
        midi::MidiEvent::note_on(0, 64, 100),
        midi::MidiEvent::note_on(0, 67, 100)};
    DesignPanelMidiEffect plugin;
    state::StateStore store;
    plugin.set_state_store(&store);
    plugin.define_parameters(store);
    for (const auto& [id, value] : kDefaults) store.set_value(id, value);
    plugin.prepare({.sample_rate = kSampleRate, .max_buffer_size = 512,
                    .input_channels = 0, .output_channels = 0});

    std::vector<midi::MidiEvent> all;
    audio::BufferView<float> ob(nullptr, 0, 0);
    audio::BufferView<const float> ib(nullptr, 0, 0);
    for (int b = 0; b < 100; ++b) {
        midi::MidiBuffer m_in, m_out;
        if (b == 0) for (const auto& e : input) m_in.add(e);
        // Let go of every key half way through.
        if (b == 50) {
            m_in.add(midi::MidiEvent::note_off(0, 60));
            m_in.add(midi::MidiEvent::note_off(0, 64));
            m_in.add(midi::MidiEvent::note_off(0, 67));
        }
        format::ProcessContext pc;
        pc.sample_rate = kSampleRate;
        pc.num_samples = 512;
        plugin.process(ob, ib, m_in, m_out, pc);
        for (std::size_t i = 0; i < m_out.size(); ++i) all.push_back(m_out[i]);
    }

    REQUIRE(count_on(all) > 3);
    for (const auto& [note, net] : balance(all)) {
        INFO("note " << note << " left " << net << " open");
        CHECK(net == 0);
    }
    CHECK(plugin.arp_for_test().sounding_note() == -1);
}

TEST_CASE("a chord is walked, not just its lowest note",
          "[design-panel-arp][midi]") {
    const auto r = run(kDefaults,
                       {midi::MidiEvent::note_on(0, 60, 100),
                        midi::MidiEvent::note_on(0, 64, 100),
                        midi::MidiEvent::note_on(0, 67, 100)},
                       512, 80);
    std::vector<int> notes;
    for (const auto& e : r.events)
        if (e.is_note_on() && e.velocity() > 0) notes.push_back(e.note());
    REQUIRE(notes.size() > 3);
    const std::vector<int> distinct(notes.begin(), notes.end());
    CHECK(std::count(distinct.begin(), distinct.end(), 60) > 0);
    CHECK(std::count(distinct.begin(), distinct.end(), 64) > 0);
    CHECK(std::count(distinct.begin(), distinct.end(), 67) > 0);
}

TEST_CASE("octaves extends the figure upward", "[design-panel-arp][midi]") {
    auto two = kDefaults;
    two[3] = {kOctaves, 2.0f};
    const auto r = run(two, {midi::MidiEvent::note_on(0, 60, 100)}, 512, 80);
    std::vector<int> notes;
    for (const auto& e : r.events)
        if (e.is_note_on() && e.velocity() > 0) notes.push_back(e.note());
    REQUIRE_FALSE(notes.empty());
    CHECK(std::count(notes.begin(), notes.end(), 72) > 0);
}

TEST_CASE("rate controls how many notes are generated",
          "[design-panel-arp][midi]") {
    auto slow = kDefaults, fast = kDefaults;
    slow[0] = {kRate, 2.0f};
    fast[0] = {kRate, 16.0f};
    const auto a = run(slow, {midi::MidiEvent::note_on(0, 60, 100)}, 512, 80);
    const auto b = run(fast, {midi::MidiEvent::note_on(0, 60, 100)}, 512, 80);
    CHECK(count_on(b.events) > count_on(a.events));
}

TEST_CASE("velocity scales the generated notes", "[design-panel-arp][midi]") {
    auto quiet = kDefaults, loud = kDefaults;
    quiet[4] = {kVelocity, 0.2f};
    loud[4] = {kVelocity, 1.0f};
    const auto a = run(quiet, {midi::MidiEvent::note_on(0, 60, 100)}, 512, 40);
    const auto b = run(loud, {midi::MidiEvent::note_on(0, 60, 100)}, 512, 40);
    REQUIRE(count_on(a.events) > 0);
    REQUIRE(count_on(b.events) > 0);
    const auto first_vel = [](const std::vector<midi::MidiEvent>& e) {
        for (const auto& m : e) if (m.is_note_on() && m.velocity() > 0) return m.velocity();
        return uint8_t{0};
    };
    CHECK(first_vel(a.events) < first_vel(b.events));
}

TEST_CASE("input notes are consumed, not passed through",
          "[design-panel-arp][midi]") {
    // Forwarding the input plays the held chord underneath the pattern, which
    // is a different instrument. With one key held, every generated note is at
    // that pitch and there is no separate sustained copy.
    const auto r = run(kDefaults, {midi::MidiEvent::note_on(0, 60, 100)}, 512, 4);
    for (const auto& e : r.events) CHECK(e.note() == 60);
    // Exactly as many offs as ons, up to one still gating.
    const auto net = balance(r.events);
    for (const auto& [note, n] : net) {
        INFO("note " << note);
        CHECK(n <= 1);
    }
}

TEST_CASE("every control the panel declares resolves to a parameter",
          "[design-panel-arp][binding]") {
    DesignPanelMidiEffect plugin;
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

TEST_CASE("the descriptor is a MIDI processor, not an effect or instrument",
          "[design-panel-arp][format]") {
    // Three surfaces must agree or the AU component is invalid: this
    // descriptor, the `aumi` component type, and the AU entry macro. Declaring
    // an audio output bus here makes hosts offer it as an instrument and then
    // wonder why it is silent.
    DesignPanelMidiEffect plugin;
    const auto d = plugin.descriptor();
    CHECK(d.category == format::PluginCategory::MidiEffect);
    CHECK(d.accepts_midi);
    CHECK(d.produces_midi);
    CHECK(d.input_buses.empty());
    CHECK(d.output_buses.empty());
}
