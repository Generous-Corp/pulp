// Tests for Workstream 03 items 3.7, 3.8, and 3.11 — bus-layout
// validation, processBlock precision contract, and the cross-adapter
// RT-safe latency / tail change notification pattern.
//
// These tests exercise the Processor-side API surface only. Adapter-
// specific wiring (VST3 setBusArrangements, AU PropertyChanged, CLAP
// host_latency->changed()) is covered separately in
// test_vst3_plugin_state.cpp and test_au_plugin_state.mm; here we pin
// the contract those adapters depend on so refactors can't silently
// break the bridge.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <atomic>
#include <thread>
#include <type_traits>

using namespace pulp::format;

namespace {

// Minimal Processor concrete: descriptor with one stereo main bus.
class StereoEffect : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "StereoEffect";
        d.input_buses  = {{"Main In",  2, false}};
        d.output_buses = {{"Main Out", 2, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

// Effect with a sidechain bus: main + sidechain in, main out.
class SidechainEffect : public StereoEffect {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d = StereoEffect::descriptor();
        d.name = "SidechainEffect";
        d.input_buses  = {{"Main In", 2, false}, {"Sidechain", 2, true}};
        return d;
    }
};

// Override that requires sidechain channels == main channels (the
// classic "linked sidechain" contract).
class LinkedSidechainEffect : public SidechainEffect {
public:
    bool is_bus_layout_supported(const BusesLayout& l) const override {
        if (l.inputs.size() != 2) return false;
        if (l.outputs.size() != 1) return false;
        // Main and sidechain must agree.
        if (l.inputs[0] != l.inputs[1]) return false;
        // Output mirrors main.
        if (l.outputs[0] != l.inputs[0]) return false;
        return true;
    }
};

} // namespace

// ── Item 3.7 — bus layout validation ──────────────────────────────────────

TEST_CASE("Processor::is_bus_layout_supported default policy accepts mono/stereo "
          "matching the descriptor's bus count",
          "[processor][bus-layout][item-3.7]") {
    StereoEffect p;

    // Matches descriptor (1 in / 1 out, both stereo).
    Processor::BusesLayout ok{{2}, {2}};
    REQUIRE(p.is_bus_layout_supported(ok));

    // Mono main is fine.
    Processor::BusesLayout mono{{1}, {1}};
    REQUIRE(p.is_bus_layout_supported(mono));

    // Stereo in, mono out is allowed by the default policy — the
    // adapter is the one that enforces matching counts via the
    // descriptor; the default validator just confirms channel counts
    // are in {1, 2}.
    Processor::BusesLayout mixed{{2}, {1}};
    REQUIRE(p.is_bus_layout_supported(mixed));
}

TEST_CASE("Processor::is_bus_layout_supported default policy rejects "
          "non-mono/stereo channel counts and bus-count mismatches",
          "[processor][bus-layout][item-3.7]") {
    StereoEffect p;

    // 6-channel (5.1) is out of scope for the default validator.
    Processor::BusesLayout surround{{6}, {6}};
    REQUIRE_FALSE(p.is_bus_layout_supported(surround));

    // Wrong input bus count (descriptor declares 1, host proposes 2).
    Processor::BusesLayout wrong_count{{2, 2}, {2}};
    REQUIRE_FALSE(p.is_bus_layout_supported(wrong_count));

    // Empty per-side means "no opinion" — must be accepted so adapters
    // that only pass the side they care about don't trip the validator.
    Processor::BusesLayout empty_in{{}, {2}};
    REQUIRE(p.is_bus_layout_supported(empty_in));
    Processor::BusesLayout empty_out{{2}, {}};
    REQUIRE(p.is_bus_layout_supported(empty_out));
}

TEST_CASE("Processor::is_bus_layout_supported override can enforce a "
          "linked-sidechain contract",
          "[processor][bus-layout][item-3.7]") {
    LinkedSidechainEffect p;

    // Stereo main + stereo sidechain + stereo out — all linked.
    Processor::BusesLayout linked_stereo{{2, 2}, {2}};
    REQUIRE(p.is_bus_layout_supported(linked_stereo));

    // Mono everywhere — also linked.
    Processor::BusesLayout linked_mono{{1, 1}, {1}};
    REQUIRE(p.is_bus_layout_supported(linked_mono));

    // Stereo main / mono sidechain — rejected, sidechain must match main.
    Processor::BusesLayout mismatched_sc{{2, 1}, {2}};
    REQUIRE_FALSE(p.is_bus_layout_supported(mismatched_sc));

    // Stereo main / stereo sidechain / mono out — rejected by the
    // mirror-output rule the plugin's override added.
    Processor::BusesLayout mismatched_out{{2, 2}, {1}};
    REQUIRE_FALSE(p.is_bus_layout_supported(mismatched_out));
}

// ── Item 3.8 — processBlock precision contract ────────────────────────────

TEST_CASE("Processor::process is declared with float-precision BufferView. "
          "All four adapters today route only float buffers; double-"
          "precision support is opt-in per future Processor overload.",
          "[processor][precision][item-3.8]") {
    // The contract under audit: the single virtual `process()` on
    // pulp::format::Processor is `BufferView<float>` only — there is
    // no `BufferView<double>` overload. Adapters wire float input ↔
    // output buffers in every format (VST3 channelBuffers32, AU
    // float32 render block, CLAP audio_buffer_t.data32, AAX float
    // pages). A regression that adds a second virtual or that
    // changes the element type to a wider scalar would silently
    // un-implement every adapter; this static_assert pins the
    // contract until item 3.8's follow-up adds an explicit double
    // overload.
    using ProcessSig = void (Processor::*)(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const ProcessContext&);
    static_assert(std::is_assignable_v<ProcessSig&, decltype(&Processor::process)>,
                  "Processor::process() must remain a float-precision "
                  "virtual until item 3.8's follow-up adds a double "
                  "overload deliberately; do not silently change the "
                  "element type.");

    // Runtime smoke: invoke process() with a float buffer to prove
    // the adapter-facing path is callable (not a compile-only check).
    StereoEffect p;
    float buf_in[2][8] = {};
    float buf_out[2][8] = {};
    const float* in_ptrs[2]  = {buf_in[0],  buf_in[1]};
    float*       out_ptrs[2] = {buf_out[0], buf_out[1]};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 8);
    pulp::audio::BufferView<float>       out_view(out_ptrs, 2, 8);
    pulp::midi::MidiBuffer mi, mo;
    ProcessContext ctx; ctx.sample_rate = 48000.0; ctx.num_samples = 8;
    p.process(out_view, in_view, mi, mo, ctx);
    SUCCEED("process() ran with float buffers");
}

// ── Item 3.11 — latency / tail change notifications (RT-safe) ─────────────

TEST_CASE("flag_latency_changed / consume_latency_changed_flag round-trip "
          "yields exactly one consume per flag",
          "[processor][latency][item-3.11]") {
    StereoEffect p;
    REQUIRE_FALSE(p.consume_latency_changed_flag());
    p.flag_latency_changed();
    REQUIRE(p.consume_latency_changed_flag());
    // Edge has been drained.
    REQUIRE_FALSE(p.consume_latency_changed_flag());
}

TEST_CASE("flag_tail_changed / consume_tail_changed_flag round-trip "
          "yields exactly one consume per flag",
          "[processor][latency][item-3.11]") {
    StereoEffect p;
    REQUIRE_FALSE(p.consume_tail_changed_flag());
    p.flag_tail_changed();
    REQUIRE(p.consume_tail_changed_flag());
    REQUIRE_FALSE(p.consume_tail_changed_flag());
}

TEST_CASE("latency_change_pending / tail_change_pending peek does not "
          "drain the flag",
          "[processor][latency][item-3.11]") {
    StereoEffect p;
    p.flag_latency_changed();
    p.flag_tail_changed();
    REQUIRE(p.latency_change_pending());
    REQUIRE(p.tail_change_pending());
    // Peek twice — still pending, still pending.
    REQUIRE(p.latency_change_pending());
    REQUIRE(p.tail_change_pending());
    // Consume drains.
    REQUIRE(p.consume_latency_changed_flag());
    REQUIRE(p.consume_tail_changed_flag());
    REQUIRE_FALSE(p.latency_change_pending());
    REQUIRE_FALSE(p.tail_change_pending());
}

TEST_CASE("flag_*_changed -> consume_*_changed_flag is data-race-free "
          "when the flag is set from one thread and drained from another",
          "[processor][latency][item-3.11][threading]") {
    // Hammer the flag from an "audio" thread and drain from a "main"
    // thread. The contract: every drain that observes `true` corresponds
    // to at least one preceding set, and we never observe undefined
    // behaviour. This is the minimal smoke we need to gate the
    // RT-safety claim; a TSan run extends it.
    StereoEffect p;
    std::atomic<bool> stop{false};
    std::atomic<int> seen{0};

    std::thread audio([&]{
        for (int i = 0; i < 100000 && !stop.load(); ++i) {
            p.flag_latency_changed();
        }
    });
    std::thread main_t([&]{
        // Drain until audio thread is done plus a tail drain.
        while (!stop.load()) {
            if (p.consume_latency_changed_flag()) ++seen;
        }
        // Final drain to catch any remaining edge.
        if (p.consume_latency_changed_flag()) ++seen;
    });

    audio.join();
    stop.store(true);
    main_t.join();

    // At least one set was observed.
    REQUIRE(seen.load() >= 1);
    REQUIRE_FALSE(p.consume_latency_changed_flag());
}
