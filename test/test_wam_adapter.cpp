// Native unit coverage for the WAMv2 Processor->WASM bridge.
//
// The bridge ships compiled to WebAssembly (-fno-exceptions) and decodes
// untrusted host/JS input on the AudioWorklet render thread, so its hardening
// (non-throwing parameter parsing, frame-count bounding, JSON escaping) must be
// covered on a deterministic native path, not only through the emcc artifact.
//
// This compiles core/format/src/wasm/wam_adapter.cpp directly and links the
// real pulp libraries (so Processor::create_view() comes from format.cpp — no
// headless stub here, that is WASM-only).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/web/wam_adapter.hpp>
#include "../examples/pulp-gain/pulp_gain.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using pulp::format::wam::WamChainBridge;
using pulp::format::wam::WamDescriptorData;
using pulp::format::wam::WamProcessorBridge;
using pulp::format::wam::WamStage;

namespace {
float rms(const std::vector<float>& v, int count) {
    double s = 0.0;
    for (int i = 0; i < count; ++i) s += static_cast<double>(v[i]) * v[i];
    return static_cast<float>(std::sqrt(s / count));
}

// Minimal instrument that proves MIDI reaches the processor through the bridge:
// it outputs 1.0 once any MIDI event has been delivered, 0.0 before.
class MidiProbe : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return pulp::format::PluginDescriptor{
            .name = "MidiProbe",
            .category = pulp::format::PluginCategory::Instrument,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
            .accepts_midi = true,
        };
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        total_midi += static_cast<int>(midi_in.size());
        const float v = total_midi > 0 ? 1.0f : 0.0f;
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            auto ch = out.channel(c);
            for (std::size_t i = 0; i < out.num_samples(); ++i) ch[i] = v;
        }
    }
    int total_midi = 0;
};
std::unique_ptr<pulp::format::Processor> make_midi_probe() {
    return std::make_unique<MidiProbe>();
}

// Counts non-realtime ticks and records the parameter value each tick saw. Stands
// in for a processor whose control changes need heavy off-audio work (
// SuperConvolver rebuilds its impulse response when `Size` moves: decode,
// resample, FFT re-partition). In a worklet there is no other thread to do it on,
// so what matters is that the bridge asks for that work ONCE per block with the
// LATEST value — never once per control message.
struct TickCounts {
    int ticks = 0;
    std::vector<float> values_seen;
};
TickCounts g_tick_counts;

class TickProbe : public pulp::format::Processor {
public:
    static constexpr pulp::state::ParamID kSize = 1;

    pulp::format::PluginDescriptor descriptor() const override {
        return pulp::format::PluginDescriptor{
            .name = "TickProbe",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
        };
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = kSize, .name = "Size", .unit = "s",
                             .range = {0.0f, 10.0f, 1.0f, 0.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            auto ch = out.channel(c);
            for (std::size_t i = 0; i < out.num_samples(); ++i) ch[i] = 0.0f;
        }
    }
    void on_non_realtime_tick() override {
        ++g_tick_counts.ticks;
        g_tick_counts.values_seen.push_back(state().get_value(kSize));
        built_ = state().get_value(kSize);
    }
    bool non_realtime_tick_pending() const override {
        return state().get_value(kSize) != built_;
    }

private:
    float built_ = -1.0f;   // nothing built yet
};
std::unique_ptr<pulp::format::Processor> make_tick_probe() {
    return std::make_unique<TickProbe>();
}

// A two-stage rack of TickProbes. Both stages report into the same g_tick_counts
// (each keeps its OWN built_ value), so a test can assert "one tick per stage per
// block", not "one tick for the rack".
std::vector<std::unique_ptr<pulp::format::Processor>> make_tick_chain() {
    std::vector<std::unique_ptr<pulp::format::Processor>> stages;
    stages.push_back(std::make_unique<TickProbe>());
    stages.push_back(std::make_unique<TickProbe>());
    return stages;
}
} // namespace

// A knob drag delivers a BURST of parameter messages to the worklet in one turn,
// all dispatched on the render thread. Ticking inline per message would run the
// processor's whole rebuild once per message (~200 rebuilds for one Size sweep,
// only the last of which is ever audible) between two render quanta — an
// underrun while the user drags. The bridge must coalesce them into one pass.
TEST_CASE("WAM bridge coalesces the non-realtime tick to once per block",
          "[wam][rt-safety]") {
    g_tick_counts = {};
    WamProcessorBridge bridge(make_tick_probe);
    REQUIRE(bridge.initialize(48000.0, 128));
    // initialize() prepares the processor, which reconciles once.
    const int after_init = g_tick_counts.ticks;

    constexpr int CH = 2, FR = 128, N = CH * FR;
    std::vector<float> in(N, 0.0f), out(N, 0.0f);
    const float* in_ch[] = {in.data(), in.data() + FR};
    float* out_ch[] = {out.data(), out.data() + FR};

    // The drag: 200 distinct values, no render call in between.
    for (int i = 1; i <= 200; ++i)
        bridge.set_parameter_value("1", static_cast<float>(i) * 0.05f);
    REQUIRE(g_tick_counts.ticks == after_init);   // NOT ONE rebuild while dragging

    // One block later: exactly one reconcile, and it saw the LAST value.
    bridge.process(in_ch, out_ch, CH, FR);
    REQUIRE(g_tick_counts.ticks == after_init + 1);
    REQUIRE(g_tick_counts.values_seen.back() == Catch::Approx(10.0f).margin(1e-5f));

    // Nothing changed since -> no further work is asked for.
    bridge.process(in_ch, out_ch, CH, FR);
    REQUIRE(g_tick_counts.ticks == after_init + 1);

    // A state restore names a different derived source; same coalescing, and it
    // still reconciles (a restored IR must reach the audio path).
    bridge.set_parameter_value("1", 3.0f);
    auto state = bridge.get_state();
    bridge.set_parameter_value("1", 8.0f);
    bridge.process(in_ch, out_ch, CH, FR);
    const int before_restore = g_tick_counts.ticks;
    REQUIRE(bridge.set_state(state.data(), state.size()));
    REQUIRE(g_tick_counts.ticks == before_restore);   // deferred, not inline
    bridge.process(in_ch, out_ch, CH, FR);
    REQUIRE(g_tick_counts.ticks == before_restore + 1);
    REQUIRE(g_tick_counts.values_seen.back() == Catch::Approx(3.0f).margin(1e-5f));
}

TEST_CASE("WAM bridge rejects malformed parameter ids without throwing", "[wam][rt-safety]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128));

    // Under -fno-exceptions a thrown std::stoi would abort the worklet; the
    // from_chars guard must make these safe no-ops.
    REQUIRE_NOTHROW(bridge.set_parameter_value("not_a_number", 1.0f));
    REQUIRE_NOTHROW(bridge.set_parameter_value("", 0.5f));
    REQUIRE_NOTHROW(bridge.set_parameter_value("12x", 0.5f));
    REQUIRE(bridge.get_parameter_value("abc") == 0.0f);
}

TEST_CASE("WAM bridge clamps oversized blocks instead of overrunning", "[wam][rt-safety]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128)); // planar buffers sized to 128

    // Planar buffers: channel 0 = [0,256), channel 1 = [256,512).
    std::vector<float> in(2 * 256, 0.25f), out(2 * 256, -99.0f);
    const float* in_ch[] = {in.data(), in.data() + 256};
    float* out_ch[] = {out.data(), out.data() + 256};
    REQUIRE_NOTHROW(bridge.process(in_ch, out_ch, 2, 256)); // 256 > 128
    REQUIRE_NOTHROW(bridge.process(in_ch, out_ch, 2, 0));   // <= 0 guard

    for (int i = 0; i < 2 * 128; ++i) REQUIRE(std::isfinite(out[i]));
}

TEST_CASE("WAM descriptor JSON escapes quotes, backslashes, and controls", "[wam]") {
    WamDescriptorData d;
    d.name = "Ev\"il\\Name";
    d.vendor = "Acme\tInc";
    std::string json = d.to_json();
    REQUIRE(json.find("Ev\\\"il\\\\Name") != std::string::npos);
    REQUIRE(json.find("Ev\"il") == std::string::npos); // no raw unescaped quote

    // Exercise every control-escape branch.
    WamDescriptorData c;
    c.name = std::string("a\bb\fc\nd\re\tf\x01g");
    const std::string cj = c.to_json();
    REQUIRE(cj.find("\\b") != std::string::npos);
    REQUIRE(cj.find("\\f") != std::string::npos);
    REQUIRE(cj.find("\\n") != std::string::npos);
    REQUIRE(cj.find("\\r") != std::string::npos);
    REQUIRE(cj.find("\\t") != std::string::npos);
    REQUIRE(cj.find("\\u0001") != std::string::npos); // control char < 0x20
}

TEST_CASE("WAM bridge exposes parameter metadata as JSON", "[wam]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128));

    const std::string json = bridge.parameters_json();
    // PulpGain's three parameters, with id/label/unit/type fields.
    REQUIRE(json.front() == '[');
    REQUIRE(json.find("\"label\":\"Input Gain\"") != std::string::npos);
    REQUIRE(json.find("\"label\":\"Output Gain\"") != std::string::npos);
    REQUIRE(json.find("\"label\":\"Bypass\"") != std::string::npos);
    REQUIRE(json.find("\"unit\":\"dB\"") != std::string::npos);
    REQUIRE(json.find("\"type\":\"boolean\"") != std::string::npos); // Bypass
    REQUIRE(json.find("\"minValue\":-60") != std::string::npos);
    REQUIRE(json.find("\"step\":0.1") != std::string::npos);         // gain step
}

TEST_CASE("WAM bridge gain parameter and state round-trip", "[wam]") {
    WamProcessorBridge bridge(pulp::examples::create_pulp_gain);
    REQUIRE(bridge.initialize(48000.0, 128));

    constexpr int CH = 2, FR = 128, N = CH * FR;
    // Planar layout: channel 0 = [0,FR), channel 1 = [FR,2*FR).
    std::vector<float> in(N), out(N, 0.0f);
    for (int f = 0; f < FR; ++f) { in[f] = 0.5f; in[FR + f] = -0.5f; }
    const float* in_ch[] = {in.data(), in.data() + FR};
    float* out_ch[] = {out.data(), out.data() + FR};

    // Default 0 dB in/out -> unity passthrough, distinct L/R preserved.
    bridge.process(in_ch, out_ch, CH, FR);
    REQUIRE(rms(out, N) == Catch::Approx(0.5f).margin(0.01f));
    REQUIRE(out[0] == Catch::Approx(0.5f).margin(0.01f));    // L, ch0 first sample
    REQUIRE(out[FR] == Catch::Approx(-0.5f).margin(0.01f));  // R, ch1 first sample

    // Output gain +6 dB (~2x). PulpGain ids: "1" input, "2" output, "3" bypass.
    bridge.set_parameter_value("2", 6.0f);
    bridge.process(in_ch, out_ch, CH, FR);
    REQUIRE(rms(out, N) == Catch::Approx(1.0f).margin(0.03f));

    // Parameter read-back.
    bridge.set_parameter_value("1", 3.5f);
    REQUIRE(bridge.get_parameter_value("1") == Catch::Approx(3.5f).margin(1e-4f));

    // State round-trip: snapshot, mutate, restore.
    bridge.set_parameter_value("1", 7.0f);
    std::vector<uint8_t> saved = bridge.get_state();
    REQUIRE(saved.size() > 0);
    bridge.set_parameter_value("1", -12.0f);
    REQUIRE(bridge.set_state(saved.data(), saved.size()));
    REQUIRE(bridge.get_parameter_value("1") == Catch::Approx(7.0f).margin(1e-3f));
}

TEST_CASE("WAM bridge delivers scheduled MIDI to the processor", "[wam]") {
    WamProcessorBridge bridge(make_midi_probe);
    REQUIRE(bridge.initialize(48000.0, 128));

    constexpr int CH = 2, FR = 128, N = CH * FR;
    // Planar layout: channel 0 = [0,FR), channel 1 = [FR,2*FR).
    std::vector<float> in(N, 0.0f), out(N, -1.0f);
    const float* in_ch[] = {in.data(), in.data() + FR};
    float* out_ch[] = {out.data(), out.data() + FR};

    // No MIDI yet -> the probe outputs silence.
    bridge.process(in_ch, out_ch, CH, FR);
    REQUIRE(out[0] == Catch::Approx(0.0f).margin(1e-6f));

    // Schedule a note-on; the bridge must route it into the processor's midi_in,
    // which the probe reflects by driving its output high.
    bridge.schedule_midi(0x90, 60, 100, 0);
    bridge.process(in_ch, out_ch, CH, FR);
    REQUIRE(out[0] == Catch::Approx(1.0f).margin(1e-6f));
}

// A Web Audio context can switch rate (44.1 kHz vs 48 kHz) or block size after
// construction, and prepare() re-runs the processor's DSP setup. Derived state a
// processor built for the OLD rate (a resampled IR) is stale afterwards, so the
// bridge must reconcile UNCONDITIONALLY here — not only when a control write
// happened to mark it dirty. prepare() is a control-thread call, so doing the
// work inline is exactly right (there is no burst to coalesce).
TEST_CASE("WAM bridge reconciles derived state on re-prepare", "[wam][rt-safety]") {
    g_tick_counts = {};
    WamProcessorBridge bridge(make_tick_probe);
    REQUIRE(bridge.initialize(48000.0, 128));

    constexpr int CH = 2, FR = 128, N = CH * FR;
    std::vector<float> in(N, 0.0f), out(N, 0.0f);
    const float* in_ch[] = {in.data(), in.data() + FR};
    float* out_ch[] = {out.data(), out.data() + FR};

    bridge.set_parameter_value("1", 4.0f);
    bridge.process(in_ch, out_ch, CH, FR);
    const int settled = g_tick_counts.ticks;
    REQUIRE(settled >= 1);

    // Nothing is dirty now — but a rate change invalidates whatever was derived
    // at the old rate, so the re-prepare still reconciles, with the live value.
    bridge.prepare(44100.0, 256);
    REQUIRE(g_tick_counts.ticks == settled + 1);
    REQUIRE(g_tick_counts.values_seen.back() == Catch::Approx(4.0f).margin(1e-5f));

    // A no-op prepare() (bad block size) must not reach the processor at all.
    bridge.prepare(44100.0, 0);
    REQUIRE(g_tick_counts.ticks == settled + 1);
}

// The rack lane's stage carries the SAME coalescing contract as the single-plugin
// bridge: a control write only MARKS work, and at most one pass runs per render
// turn, over the latest values.
TEST_CASE("WAM rack stage coalesces the non-realtime tick", "[wam][rack][rt-safety]") {
    g_tick_counts = {};
    WamStage stage;
    REQUIRE(stage.initialize(make_tick_probe(), 48000.0, 128));
    REQUIRE(stage.ready());
    REQUIRE(g_tick_counts.ticks == 0);   // initialize() never ticks

    // The processor discovers work for ITSELF (nothing is built yet), with no
    // control write to announce it. The stage must still service that.
    stage.service_non_realtime();
    REQUIRE(g_tick_counts.ticks == 1);
    // Clean now: neither the stage's dirty flag nor the processor's own query is
    // set, so a second pass does no work.
    stage.service_non_realtime();
    REQUIRE(g_tick_counts.ticks == 1);

    // The drag: 50 distinct values in one turn. Writing a parameter must NOT run
    // the rebuild inline (that is the underrun this coalescing exists to avoid).
    for (int i = 1; i <= 50; ++i)
        stage.set_param(TickProbe::kSize, static_cast<float>(i) * 0.1f);
    REQUIRE(g_tick_counts.ticks == 1);
    REQUIRE(stage.get_param(TickProbe::kSize) == Catch::Approx(5.0f).margin(1e-5f));

    // One pass later: exactly one rebuild, and it saw the LAST value.
    stage.service_non_realtime();
    REQUIRE(g_tick_counts.ticks == 2);
    REQUIRE(g_tick_counts.values_seen.back() == Catch::Approx(5.0f).margin(1e-5f));

    // A state restore names a different derived source than the live one. It must
    // mark dirty (deferred), not rebuild inline, and must still reconcile.
    const std::vector<uint8_t> saved = stage.get_state();
    REQUIRE(saved.size() > 0);
    stage.set_param(TickProbe::kSize, 9.0f);
    stage.service_non_realtime();
    const int before_restore = g_tick_counts.ticks;

    REQUIRE(stage.set_state(saved.data(), saved.size()));
    REQUIRE(g_tick_counts.ticks == before_restore);      // deferred, not inline
    stage.service_non_realtime();
    REQUIRE(g_tick_counts.ticks == before_restore + 1);
    REQUIRE(g_tick_counts.values_seen.back() == Catch::Approx(5.0f).margin(1e-5f));

    // A rejected blob still marks dirty (the store may have been partially
    // touched, and the caller has no other way to force a reconcile).
    const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const int before_garbage = g_tick_counts.ticks;
    REQUIRE_FALSE(stage.set_state(garbage, sizeof(garbage)));
    stage.service_non_realtime();
    REQUIRE(g_tick_counts.ticks == before_garbage + 1);

    // A source that changed without a parameter write (a newly decoded IR) is
    // announced with mark_non_realtime_dirty().
    const int before_mark = g_tick_counts.ticks;
    stage.mark_non_realtime_dirty();
    stage.service_non_realtime();
    REQUIRE(g_tick_counts.ticks == before_mark + 1);

    // Re-prepare reconciles unconditionally (same contract as the single bridge).
    const int before_prepare = g_tick_counts.ticks;
    stage.prepare(44100.0, 256);
    REQUIRE(g_tick_counts.ticks == before_prepare + 1);
    stage.prepare(44100.0, 0);   // invalid block size: no-op, no tick
    REQUIRE(g_tick_counts.ticks == before_prepare + 1);
}

// The rack runs N processors inside ONE worklet, so a knob drag on ANY stage
// arrives as a burst on the render thread. Each stage gets at most one pass per
// block — 200 messages across two stages must cost two rebuilds, not 200.
TEST_CASE("WAM rack coalesces every stage's non-realtime tick to once per block",
          "[wam][rack][rt-safety]") {
    g_tick_counts = {};
    WamChainBridge chain(make_tick_chain);
    REQUIRE(chain.initialize(48000.0, 128));
    REQUIRE(g_tick_counts.ticks == 0);

    constexpr int CH = 2, FR = 128, N = CH * FR;
    std::vector<float> in(N, 0.0f), out(N, 0.0f);
    const float* in_ch[] = {in.data(), in.data() + FR};
    float* out_ch[] = {out.data(), out.data() + FR};

    // The drag: 100 values per stage, no render call in between.
    for (int i = 1; i <= 100; ++i) {
        chain.set_parameter_value("0:1", static_cast<float>(i) * 0.1f);
        chain.set_parameter_value("1:1", static_cast<float>(i) * 0.05f);
    }
    REQUIRE(g_tick_counts.ticks == 0);   // NOT ONE rebuild while dragging

    // One block later: exactly one pass per stage, each with its own last value.
    chain.process(in_ch, out_ch, CH, FR);
    REQUIRE(g_tick_counts.ticks == 2);
    REQUIRE(g_tick_counts.values_seen.size() == 2);
    REQUIRE(g_tick_counts.values_seen[0] == Catch::Approx(10.0f).margin(1e-5f));
    REQUIRE(g_tick_counts.values_seen[1] == Catch::Approx(5.0f).margin(1e-5f));

    // Nothing changed since -> the next block asks for no work at all.
    chain.process(in_ch, out_ch, CH, FR);
    REQUIRE(g_tick_counts.ticks == 2);

    // One stage dirtied -> one rebuild, and only that stage's.
    chain.set_parameter_value("1:1", 7.0f);
    chain.process(in_ch, out_ch, CH, FR);
    REQUIRE(g_tick_counts.ticks == 3);
    REQUIRE(g_tick_counts.values_seen.back() == Catch::Approx(7.0f).margin(1e-5f));
}
