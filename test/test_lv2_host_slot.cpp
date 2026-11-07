// Hosting a real LV2 instrument through PluginSlot.
//
// The plugin under test is test/fixtures/lv2_host_instrument, built as an LV2
// bundle (manifest, Turtle description, module) and loaded through lilv exactly
// as a third-party plugin is. It reports what it observed on output control
// ports, which the slot exposes through get_parameter(port index), so every
// assertion here is about what the plugin itself saw or did.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/format/process_block.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>

#include "lv2_worker.hpp"
#include "support/unique_temp_dir.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace fmt = pulp::format;
using Catch::Matchers::WithinAbs;
using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginSlot;

namespace {

constexpr const char* kUri = "http://example.com/pulp/lv2-host-instrument";
constexpr int kFrames = 128;
constexpr double kRate = 48000.0;

// Port indices of the fixture (plugin.ttl).
enum Port : std::uint32_t {
    kLevel = 4,
    kFreeWheel = 6,
    kSeenBpm = 8,
    kSeenSpeed = 9,
    kSeenBarBeat = 10,
    kSeenBar = 11,
    kSeenBeatUnit = 12,
    kSeenBeatsPerBar = 13,
    kSeenMaxBlock = 14,
    kSeenSampleRate = 15,
    kNoteOns = 16,
    kWorkerResponses = 17,
    kSeenFreeWheel = 18,
    kSeenAmount = 19,
    kTimeMessages = 20,
};

std::unique_ptr<PluginSlot> load() {
    REQUIRE(fs::exists(PULP_TEST_LV2_HOST_INSTRUMENT_BUNDLE));
    PluginInfo info;
    info.format = PluginFormat::LV2;
    info.path = PULP_TEST_LV2_HOST_INSTRUMENT_BUNDLE;
    info.unique_id = kUri;
    auto slot = PluginSlot::load(info);
    REQUIRE(slot);
    REQUIRE(slot->is_loaded());
    return slot;
}

std::uint32_t amount_id(const PluginSlot& slot) {
    for (const auto& p : slot.parameters())
        if (p.name == "Amount")
            return p.id;
    FAIL("no Amount parameter");
    return 0;
}

pulp::midi::MidiEvent midi(std::uint8_t a, std::uint8_t b, std::uint8_t c, int frame) {
    pulp::midi::MidiEvent e;
    e.message = choc::midi::ShortMessage(a, b, c);
    e.sample_offset = frame;
    return e;
}

/// One block of host buffers: no audio input (an instrument), stereo out.
struct Block {
    std::vector<float> left = std::vector<float>(kFrames, 9.0f);
    std::vector<float> right = std::vector<float>(kFrames, 9.0f);
    std::array<float*, 2> out_ptrs{left.data(), right.data()};
    std::array<fmt::ProcessBusBufferView<float>, 1> outs{};
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::host::ParameterEventQueue params;

    Block() {
        outs[0].buffer = pulp::audio::BufferView<float>(out_ptrs.data(), 2, kFrames);
    }

    void run(PluginSlot& slot, const fmt::ProcessContext& ctx) {
        fmt::ProcessBuffers buffers{fmt::ProcessBusBufferSet<const float>{},
                                    fmt::ProcessBusBufferSet<float>{std::span(outs)}};
        midi_out.clear();
        slot.process(buffers, midi_in, midi_out, params, kFrames, ctx);
        midi_in.clear();
    }

    void run(PluginSlot& slot) {
        pulp::audio::BufferView<float> out(out_ptrs.data(), 2, kFrames);
        pulp::audio::BufferView<const float> in;
        midi_out.clear();
        slot.process(out, in, midi_in, midi_out, params, kFrames);
        midi_in.clear();
    }
};

fmt::ProcessContext transport(double beats, bool playing, std::int64_t frame) {
    fmt::ProcessContext ctx;
    ctx.sample_rate = kRate;
    ctx.num_samples = kFrames;
    ctx.is_playing = playing;
    ctx.tempo_bpm = 133.0;
    ctx.time_sig_numerator = 6;
    ctx.time_sig_denominator = 8;
    ctx.position_beats = beats;
    ctx.position_samples = frame;
    return ctx;
}

} // namespace

TEST_CASE("LV2 slot describes the instrument's ports and patch parameters",
          "[host][lv2][slot][lilv]") {
    auto slot = load();
    REQUIRE(slot->info().unique_id == kUri);
    REQUIRE(slot->info().name == "Pulp LV2 Host Instrument");
    REQUIRE(slot->wants_transport());

    const auto params = slot->parameters();
    // Level is a control input; Amount is a patch:writable property. The
    // free-wheeling input belongs to the host, and outputs are not parameters.
    REQUIRE(params.size() == 2);
    REQUIRE(params[0].id == kLevel);
    REQUIRE(params[0].name == "Level");
    REQUIRE_THAT(params[0].default_value, WithinAbs(0.5f, 1e-6f));
    REQUIRE(params[1].name == "Amount");
    REQUIRE(params[1].id >= 0x40000000u);
    REQUIRE_THAT(params[1].max_value, WithinAbs(20.0f, 1e-6f));
    REQUIRE_THAT(params[1].default_value, WithinAbs(1.0f, 1e-6f));
    REQUIRE(params[1].flags.automatable);
}

TEST_CASE("LV2 slot plays notes, returns the plugin's MIDI, and passes options",
          "[host][lv2][slot][lilv]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    // options:options reached instantiate(): buf-size maxBlockLength and
    // param:sampleRate. The latency port was read by the silent block prepare()
    // runs, so the value is there before any audio.
    REQUIRE(slot->latency_query() == PluginSlot::LatencyQuery::Available);
    REQUIRE(slot->latency_samples() == 64);
    REQUIRE_THAT(slot->get_parameter(kSeenMaxBlock), WithinAbs(kFrames, 1e-3f));
    REQUIRE_THAT(slot->get_parameter(kSeenSampleRate), WithinAbs(kRate, 1e-3f));

    Block block;
    block.midi_in.add(midi(0x90, 60, 127, 32));
    block.run(*slot);
    for (int i = 0; i < 32; ++i)
        REQUIRE_THAT(block.left[static_cast<std::size_t>(i)], WithinAbs(0.0f, 1e-6f));
    for (int i = 32; i < kFrames; ++i) {
        REQUIRE_THAT(block.left[static_cast<std::size_t>(i)], WithinAbs(0.5f, 1e-6f));
        REQUIRE_THAT(block.right[static_cast<std::size_t>(i)], WithinAbs(0.5f, 1e-6f));
    }
    REQUIRE_THAT(slot->get_parameter(kNoteOns), WithinAbs(1.0f, 1e-6f));

    // The plugin echoes MIDI on its notify port; it comes back as host MIDI at
    // the same frame.
    REQUIRE(block.midi_out.size() == 1);
    const auto& echoed = *block.midi_out.begin();
    REQUIRE(echoed.sample_offset == 32);
    REQUIRE(echoed.message.data()[0] == 0x90);
    REQUIRE(echoed.message.data()[1] == 60);

    // The Level control port scales the voice; note-off silences it.
    slot->set_parameter(kLevel, 0.25f);
    block.midi_in.add(midi(0x90, 62, 127, 0));
    block.midi_in.add(midi(0x80, 62, 0, 64));
    block.run(*slot);
    REQUIRE_THAT(block.left[10], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(block.left[100], WithinAbs(0.0f, 1e-6f));
    REQUIRE(block.midi_out.size() == 2);
    slot->release();
}

TEST_CASE("LV2 slot sends the host transport as time:Position", "[host][lv2][slot][lilv]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    Block block;

    // 7.5 quarter notes in 6/8 is 15 eighth notes: bar 2, beat 3.
    block.run(*slot, transport(7.5, true, 48000));
    REQUIRE_THAT(slot->get_parameter(kTimeMessages), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBpm), WithinAbs(133.0f, 1e-4f));
    REQUIRE_THAT(slot->get_parameter(kSeenSpeed), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBar), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBarBeat), WithinAbs(3.0f, 1e-4f));
    REQUIRE_THAT(slot->get_parameter(kSeenBeatUnit), WithinAbs(8.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBeatsPerBar), WithinAbs(6.0f, 1e-6f));

    // A block that continues where the last one ended carries no new position.
    block.run(*slot, transport(7.5 + 133.0 / 60.0 * kFrames / kRate, true, 48000 + kFrames));
    REQUIRE_THAT(slot->get_parameter(kTimeMessages), WithinAbs(1.0f, 1e-6f));

    // Stopping, and then a locate, each send one.
    block.run(*slot, transport(7.6, false, 48000 + 2 * kFrames));
    REQUIRE_THAT(slot->get_parameter(kTimeMessages), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenSpeed), WithinAbs(0.0f, 1e-6f));
    block.run(*slot, transport(0.0, false, 0));
    REQUIRE_THAT(slot->get_parameter(kTimeMessages), WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBar), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("LV2 slot runs the worker on a thread, or in the block when offline",
          "[host][lv2][slot][lilv][worker]") {
    SECTION("realtime: the response arrives on a later block") {
        auto slot = load();
        REQUIRE(slot->prepare(kRate, kFrames));
        Block block;
        block.midi_in.add(midi(0x90, 60, 100, 0));
        block.run(*slot, transport(0.0, false, 0));
        REQUIRE_THAT(slot->get_parameter(kSeenFreeWheel), WithinAbs(0.0f, 1e-6f));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (slot->get_parameter(kWorkerResponses) < 1.0f &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            block.run(*slot, transport(0.0, false, 0));
        }
        REQUIRE_THAT(slot->get_parameter(kWorkerResponses), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("offline: work runs inside the block and answers deterministically") {
        auto slot = load();
        REQUIRE(slot->prepare(kRate, kFrames));
        Block block;
        auto ctx = transport(0.0, false, 0);
        ctx.process_mode = fmt::ProcessMode::Offline;
        block.midi_in.add(midi(0x90, 60, 100, 0));
        block.midi_in.add(midi(0x90, 64, 100, 10));
        block.run(*slot, ctx);
        // The plugin writes its counter during run(); responses land after it,
        // so the very next block shows both, with no waiting.
        block.run(*slot, ctx);
        REQUIRE_THAT(slot->get_parameter(kWorkerResponses), WithinAbs(2.0f, 1e-6f));
        REQUIRE_THAT(slot->get_parameter(kSeenFreeWheel), WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("LV2 slot exchanges patch:Set with the plugin", "[host][lv2][slot][lilv]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    const auto amount = amount_id(*slot);
    Block block;

    slot->set_parameter(amount, 7.5f);
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(kSeenAmount), WithinAbs(7.5f, 1e-6f));

    // Automation for a patch parameter is forged at its frame.
    block.params.push(pulp::host::ParameterEvent{amount, 40, 3.25f, 0});
    block.run(*slot);
    block.params.clear();
    REQUIRE_THAT(slot->get_parameter(kSeenAmount), WithinAbs(3.25f, 1e-6f));

    // The plugin changes its own property (CC 20) and reports it with patch:Set.
    block.midi_in.add(midi(0xB0, 20, 42, 5));
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(amount), WithinAbs(4.2f, 1e-5f));
}

TEST_CASE("LV2 slot saves and restores plugin state and port values",
          "[host][lv2][slot][lilv][state]") {
    std::vector<std::uint8_t> saved;
    {
        auto slot = load();
        REQUIRE(slot->prepare(kRate, kFrames));
        const auto amount = amount_id(*slot);
        Block block;
        slot->set_parameter(amount, 6.0f);
        slot->set_parameter(kLevel, 0.75f);
        block.midi_in.add(midi(0x90, 60, 100, 0));
        block.midi_in.add(midi(0x90, 61, 100, 1));
        block.run(*slot);
        REQUIRE_THAT(slot->get_parameter(kNoteOns), WithinAbs(2.0f, 1e-6f));

        // A re-prepare builds a new instance; the state carries across.
        REQUIRE(slot->prepare(kRate, kFrames));
        block.run(*slot);
        REQUIRE_THAT(slot->get_parameter(kNoteOns), WithinAbs(2.0f, 1e-6f));
        REQUIRE_THAT(slot->get_parameter(kLevel), WithinAbs(0.75f, 1e-6f));
        REQUIRE_THAT(slot->get_parameter(amount), WithinAbs(6.0f, 1e-6f));

        saved = slot->save_state();
        REQUIRE_FALSE(saved.empty());
    }

    SECTION("restored before prepare") {
        auto slot = load();
        REQUIRE(slot->restore_state(saved));
        REQUIRE(slot->prepare(kRate, kFrames));
        Block block;
        block.run(*slot);
        REQUIRE_THAT(slot->get_parameter(kNoteOns), WithinAbs(2.0f, 1e-6f));
        REQUIRE_THAT(slot->get_parameter(kLevel), WithinAbs(0.75f, 1e-6f));
        // The host asks the plugin for its properties after a restore.
        REQUIRE_THAT(slot->get_parameter(amount_id(*slot)), WithinAbs(6.0f, 1e-6f));
    }

    SECTION("restored into a running instance") {
        auto slot = load();
        REQUIRE(slot->prepare(kRate, kFrames));
        Block block;
        block.run(*slot);
        REQUIRE(slot->restore_state(saved));
        block.run(*slot);
        REQUIRE_THAT(slot->get_parameter(kNoteOns), WithinAbs(2.0f, 1e-6f));
        REQUIRE_THAT(slot->get_parameter(amount_id(*slot)), WithinAbs(6.0f, 1e-6f));
    }

    SECTION("garbage is refused") {
        auto slot = load();
        const std::string junk = "this is not turtle {";
        REQUIRE_FALSE(slot->restore_state({junk.begin(), junk.end()}));
    }
}

TEST_CASE("LV2 instrument in a SignalGraph gets notes, transport, and delay compensation",
          "[host][lv2][slot][lilv][graph]") {
    auto owned = load();
    PluginSlot* synth_slot = owned.get(); // Observation ports, read after each block.
    pulp::host::SignalGraph graph;
    // The routed executor is the graph path that hands the transport to a
    // plugin that asks for it.
    graph.set_canonical_executor_routing_enabled(true);
    const auto keys = graph.add_midi_input_node();
    const auto synth = graph.add_plugin_node(std::move(owned), 0, 2, "lv2-instrument");
    const auto out = graph.add_output_node(2);
    REQUIRE(graph.connect_midi(keys, synth));
    REQUIRE(graph.connect(synth, 0, out, 0));
    REQUIRE(graph.connect(synth, 1, out, 1));
    REQUIRE(graph.prepare(kRate, kFrames));
    // The plugin's lv2:reportsLatency port is the graph's latency.
    REQUIRE(graph.latency_samples() == 64);

    std::vector<float> left(kFrames), right(kFrames);
    std::array<float*, 2> out_ptrs{left.data(), right.data()};
    pulp::audio::BufferView<float> output(out_ptrs.data(), 2, kFrames);
    std::vector<float> silence(kFrames, 0.0f);
    std::array<const float*, 2> in_ptrs{silence.data(), silence.data()};
    pulp::audio::BufferView<const float> input(in_ptrs.data(), 2, kFrames);
    pulp::midi::MidiBuffer notes;
    notes.add(midi(0x90, 69, 127, 0));
    REQUIRE(graph.inject_midi(keys, notes));
    graph.process(output, input, kFrames, transport(7.5, true, 48000));
    // An empty input view has no frames and fails block validation, which
    // would send the block down the walk that carries no transport.
    REQUIRE(graph.routed_walk_fallbacks() == 0);

    REQUIRE_THAT(synth_slot->get_parameter(kNoteOns), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(synth_slot->get_parameter(kTimeMessages), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(synth_slot->get_parameter(kSeenBpm), WithinAbs(133.0f, 1e-4f));
    float peak = 0.0f;
    for (float s : left)
        peak = std::max(peak, s);
    REQUIRE(peak > 0.4f); // Level 0.5 at full velocity reached the graph output.
}

TEST_CASE("LV2 slot refuses a plugin that requires a feature it does not provide",
          "[host][lv2][slot][lilv]") {
    const fs::path dir = pulp::test::make_unique_temp_dir("pulp-lv2-required-feature");
    const fs::path bundle = dir / "NeedsMore.lv2";
    fs::create_directories(bundle);
    const fs::path module = fs::path(PULP_TEST_LV2_HOST_INSTRUMENT_BUNDLE) /
                            ("lv2_host_instrument" PULP_TEST_LV2_MODULE_SUFFIX);
    REQUIRE(fs::exists(module));
    {
        std::ofstream ttl(bundle / "manifest.ttl");
        ttl << "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
               "<http://example.com/pulp/lv2-host-instrument>\n"
               "    a lv2:Plugin ;\n"
               "    lv2:binary <file://"
            << module.string()
            << "> ;\n"
               "    lv2:requiredFeature <http://example.com/pulp/not-a-feature> .\n";
    }
    PluginInfo info;
    info.format = PluginFormat::LV2;
    info.path = bundle.string();
    REQUIRE(PluginSlot::load(info) == nullptr);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("LV2 worker message ring keeps message boundaries across the wrap",
          "[host][lv2][worker]") {
    pulp::host::detail::Lv2MessageRing ring(64);
    REQUIRE(ring.capacity() == 64);
    std::vector<std::uint8_t> out(ring.capacity());
    std::uint32_t size = 0;
    for (int round = 0; round < 50; ++round) {
        const std::array<std::uint8_t, 13> msg{static_cast<std::uint8_t>(round)};
        REQUIRE(ring.write(msg.data(), static_cast<std::uint32_t>(msg.size())));
        REQUIRE(ring.write(msg.data(), static_cast<std::uint32_t>(msg.size())));
        for (int k = 0; k < 2; ++k) {
            REQUIRE(ring.read(out, size));
            REQUIRE(size == 13);
            REQUIRE(out[0] == static_cast<std::uint8_t>(round));
        }
        REQUIRE_FALSE(ring.read(out, size));
    }
    // A message larger than the free space is refused whole.
    const std::vector<std::uint8_t> big(61, 1);
    REQUIRE_FALSE(ring.write(big.data(), static_cast<std::uint32_t>(big.size())));
    REQUIRE(ring.empty());
}
