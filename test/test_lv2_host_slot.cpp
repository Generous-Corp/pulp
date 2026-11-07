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

#include "harness/rt_allocation_probe.hpp"
#include "lv2_worker.hpp"
#include "support/unique_temp_dir.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    kSeenBeat = 21,
    kSeenFps = 22,
    kCutoff = 23,
    kSeenCutoff = 24,
    kSeenCount = 25,
    kSeenBig = 26,
    kSeenFlag = 27,
    kSeenFine = 28,
    kWorkerRefusals = 29,
    kMalformedEvents = 30,
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

std::uint32_t param_id(const PluginSlot& slot, const std::string& name) {
    for (const auto& p : slot.parameters())
        if (p.name == name)
            return p.id;
    FAIL("no parameter " << name);
    return 0;
}

std::uint32_t amount_id(const PluginSlot& slot) {
    return param_id(slot, "Amount");
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
        // Realtime-ready, as a host's audio-thread buffer is.
        midi_out.reserve(4096);
        midi_out.set_realtime_capacity_limit(true);
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
    // Level and Cutoff are control inputs; the five patch:writable properties
    // follow in URI order. The free-wheeling input belongs to the host, and
    // outputs are not parameters.
    REQUIRE(params.size() == 7);
    REQUIRE(params[0].id == kLevel);
    REQUIRE(params[0].name == "Level");
    REQUIRE_THAT(params[0].default_value, WithinAbs(0.5f, 1e-6f));
    REQUIRE(params[1].id == kCutoff);
    const std::vector<std::string> props{"Amount", "Big", "Count", "Fine", "Flag"};
    for (std::size_t i = 0; i < props.size(); ++i) {
        INFO(props[i]);
        REQUIRE(params[2 + i].name == props[i]);
        REQUIRE(params[2 + i].id >= 0x40000000u);
        REQUIRE(params[2 + i].flags.automatable);
    }
    REQUIRE_THAT(params[2].max_value, WithinAbs(20.0f, 1e-6f));
    REQUIRE_THAT(params[2].default_value, WithinAbs(1.0f, 1e-6f));
    // Integer, Long, and Bool ranges are stepped; Float and Double are not.
    REQUIRE(params[3].flags.stepped);
    REQUIRE(params[4].flags.stepped);
    REQUIRE_FALSE(params[5].flags.stepped);
    REQUIRE(params[6].flags.stepped);
    // Before prepare() the plugin has not run, so its latency is unknown.
    REQUIRE(slot->latency_query() == PluginSlot::LatencyQuery::Unsupported);
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
    // time:beat counts beats of the beat unit (eighths here) since the start;
    // time:framesPerSecond is the sample rate.
    REQUIRE_THAT(slot->get_parameter(kSeenBeat), WithinAbs(15.0f, 1e-4f));
    REQUIRE_THAT(slot->get_parameter(kSeenFps), WithinAbs(kRate, 1e-3f));

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

namespace {

/// A bundle in a scratch directory whose manifest points at the fixture's
/// module, so a test can describe the plugin any way it likes.
struct ScratchBundle {
    fs::path dir;
    fs::path bundle;

    ScratchBundle(const char* tag, const std::string& manifest_extra,
                  const std::string& plugin_ttl) {
        dir = pulp::test::make_unique_temp_dir(tag);
        bundle = dir / "Scratch.lv2";
        fs::create_directories(bundle);
        const fs::path module = fs::path(PULP_TEST_LV2_HOST_INSTRUMENT_BUNDLE) /
                                ("lv2_host_instrument" PULP_TEST_LV2_MODULE_SUFFIX);
        REQUIRE(fs::exists(module));
        std::ofstream manifest(bundle / "manifest.ttl");
        manifest << "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                    "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                    "<http://example.com/pulp/lv2-host-instrument>\n"
                    "    a lv2:Plugin ;\n"
                    "    lv2:binary <file://"
                 << module.string() << "> ;\n"
                 << manifest_extra << "    rdfs:seeAlso <plugin.ttl> .\n";
        std::ofstream(bundle / "plugin.ttl") << plugin_ttl;
    }
    ~ScratchBundle() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::unique_ptr<PluginSlot> load() const {
        PluginInfo info;
        info.format = PluginFormat::LV2;
        info.path = bundle.string();
        return PluginSlot::load(info);
    }
};

/// The fixture's own description, for scratch bundles that only change the
/// manifest.
std::string fixture_ttl() {
    std::ifstream in(fs::path(PULP_TEST_LV2_HOST_INSTRUMENT_BUNDLE) / "plugin.ttl");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("LV2 slot drops whole events when an input sequence is full",
          "[host][lv2][slot][lilv][overflow]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    const auto amount = amount_id(*slot);
    Block block;
    slot->set_parameter(amount, 2.0f);
    block.run(*slot); // Also carries the first block's patch:Get.
    REQUIRE_THAT(slot->get_parameter(kSeenAmount), WithinAbs(2.0f, 1e-6f));

    // The control port asks for 16384 bytes (rsz:minimumSize). A sequence
    // header takes 16 and a three-byte MIDI event 24, so 682 events fit. The
    // host sends 2048 note-ons in the first 100 frames and an automated patch
    // parameter at frame 120, which sorts after them and no longer fits.
    for (int i = 0; i < 2048; ++i)
        block.midi_in.add(midi(0x90, static_cast<std::uint8_t>(i % 128), 100, i * 100 / 2048));
    block.params.push(pulp::host::ParameterEvent{amount, 120, 9.0f, 0});
    block.run(*slot);
    block.params.clear();
    // Exactly the events that fit arrived, whole; nothing torn or invented.
    REQUIRE_THAT(slot->get_parameter(kMalformedEvents), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kNoteOns), WithinAbs(682.0f, 1e-3f));
    REQUIRE_THAT(slot->get_parameter(kSeenAmount), WithinAbs(2.0f, 1e-6f));

    // The next block has room again.
    block.params.push(pulp::host::ParameterEvent{amount, 0, 9.0f, 0});
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(kSeenAmount), WithinAbs(9.0f, 1e-6f));
}

TEST_CASE("LV2 slot sends patch:Set in the atom type of each property's range",
          "[host][lv2][slot][lilv][patch]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    Block block;
    // The fixture accepts a value only in the declared type, so each of these
    // lands only when the host forged that type.
    slot->set_parameter(param_id(*slot, "Count"), 42.4f);   // atom:Int
    slot->set_parameter(param_id(*slot, "Big"), 123456.0f); // atom:Long
    slot->set_parameter(param_id(*slot, "Flag"), 1.0f);     // atom:Bool
    slot->set_parameter(param_id(*slot, "Fine"), 0.25f);    // atom:Double
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(kSeenCount), WithinAbs(42.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBig), WithinAbs(123456.0f, 1e-3f));
    REQUIRE_THAT(slot->get_parameter(kSeenFlag), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenFine), WithinAbs(0.25f, 1e-6f));

    // Values are clamped to the declared bounds before they are sent.
    slot->set_parameter(param_id(*slot, "Count"), 1000.0f);
    slot->set_parameter(param_id(*slot, "Flag"), 0.2f);
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(kSeenCount), WithinAbs(100.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenFlag), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("LV2 slot sends only the transport fields the host supplied",
          "[host][lv2][slot][lilv][transport]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    Block block;
    auto ctx = transport(7.5, true, 48000);
    // Only the tempo is known: position, meter, and play state stay out.
    ctx.transport_validity.set(fmt::TransportField::Tempo);
    block.run(*slot, ctx);
    REQUIRE_THAT(slot->get_parameter(kTimeMessages), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBpm), WithinAbs(133.0f, 1e-4f));
    REQUIRE_THAT(slot->get_parameter(kSeenBeat), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBar), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenBeatsPerBar), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenSpeed), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kSeenFps), WithinAbs(kRate, 1e-3f));

    // Adding the beat position (no meter) sends time:beat in quarter notes.
    ctx.transport_validity.set(fmt::TransportField::BeatPosition);
    ctx.transport_jump = true;
    block.run(*slot, ctx);
    REQUIRE_THAT(slot->get_parameter(kSeenBeat), WithinAbs(7.5f, 1e-4f));
    REQUIRE_THAT(slot->get_parameter(kSeenBar), WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("LV2 slot drops a trace message logged from run() without allocating",
          "[host][lv2][slot][lilv][rt]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    Block block;
    block.midi_in.add(midi(0xB0, 22, 1, 0));
    block.run(*slot); // Warm up: first-block work happens once.

    // The probe sees allocations on this thread; confirm it can see one.
    {
        pulp::test::RtAllocationProbe control;
        // A direct call: the compiler may not remove it as it may a new-expression.
        void* allocates = ::operator new(64);
        ::operator delete(allocates);
        REQUIRE(control.saw_allocation());
    }
    block.midi_in.add(midi(0xB0, 22, 2, 0));
    pulp::midi::MidiBuffer in_copy = block.midi_in;
    block.midi_in.clear();
    {
        pulp::test::RtAllocationProbe probe;
        pulp::audio::BufferView<float> out(block.out_ptrs.data(), 2, kFrames);
        pulp::audio::BufferView<const float> in;
        block.midi_out.clear();
        slot->process(out, in, in_copy, block.midi_out, block.params, kFrames);
        CHECK_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("LV2 slot saves state while blocks keep running", "[host][lv2][slot][lilv][state]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    std::thread audio([&] {
        Block block;
        while (!stop.load()) {
            block.midi_in.add(midi(0x90, 60, 100, 0));
            block.run(*slot);
            blocks.fetch_add(1);
        }
    });
    std::size_t saved = 0;
    while (blocks.load() < 400) {
        saved += slot->save_state().empty() ? 0 : 1;
    }
    stop.store(true);
    audio.join();
    REQUIRE(saved > 0);
    // Every block ran the plugin: a save never made one pass through and
    // lose its note.
    REQUIRE_THAT(slot->get_parameter(kNoteOns),
                 WithinAbs(static_cast<float>(blocks.load()), 1e-3f));
}

TEST_CASE("LV2 slot refuses work it cannot queue and recovers", "[host][lv2][slot][lilv][worker]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    Block block;
    // 10000 requests at once; the request queue holds 8192 of these.
    block.midi_in.add(midi(0xB0, 21, 100, 0));
    block.run(*slot);
    REQUIRE(slot->get_parameter(kWorkerRefusals) > 0.0f);

    // The queue drains, and once it has room a request is worked and
    // answered again. Each block offers one, as a playing instrument would.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (slot->get_parameter(kWorkerResponses) < 1.0f &&
           std::chrono::steady_clock::now() < deadline) {
        block.midi_in.add(midi(0x90, 60, 100, 0));
        block.run(*slot);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(slot->get_parameter(kWorkerResponses) >= 1.0f);
}

TEST_CASE("LV2 slot scales lv2:sampleRate port ranges by the prepared rate",
          "[host][lv2][slot][lilv]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    const auto params = slot->parameters();
    const auto cutoff =
        std::find_if(params.begin(), params.end(), [](const auto& p) { return p.id == kCutoff; });
    REQUIRE(cutoff != params.end());
    REQUIRE_THAT(cutoff->max_value, WithinAbs(24000.0f, 1e-2f));
    REQUIRE_THAT(cutoff->default_value, WithinAbs(12000.0f, 1e-2f));
    Block block;
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(kSeenCutoff), WithinAbs(12000.0f, 1e-2f));
    slot->set_parameter(kCutoff, 3000.0f);
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(kSeenCutoff), WithinAbs(3000.0f, 1e-2f));
}

TEST_CASE("LV2 slot clamps the plugin's event times and stops at a bad event",
          "[host][lv2][slot][lilv][overflow]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    Block block;
    // CC 24 makes the plugin emit a note stamped ten blocks ahead.
    block.midi_in.add(midi(0xB0, 24, 1, 3));
    block.run(*slot);
    bool late_note = false;
    for (const auto& e : block.midi_out)
        if (e.message.data()[0] == 0x90 && e.message.data()[1] == 99) {
            late_note = true;
            REQUIRE(e.sample_offset == kFrames - 1);
        }
    REQUIRE(late_note);

    // CC 25 makes it emit an object whose size runs past the sequence: the
    // events before it arrive, and nothing after it is read.
    block.midi_in.add(midi(0xB0, 7, 64, 1));
    block.midi_in.add(midi(0xB0, 25, 1, 2));
    block.run(*slot);
    REQUIRE(block.midi_out.size() == 2); // The two echoed controllers.
    block.run(*slot);
    REQUIRE(slot->is_loaded());
}

TEST_CASE("LV2 slot sends All Notes Off after a bypass", "[host][lv2][slot][lilv]") {
    auto slot = load();
    REQUIRE(slot->prepare(kRate, kFrames));
    Block block;
    block.midi_in.add(midi(0x90, 60, 127, 0));
    block.run(*slot);
    REQUIRE_THAT(block.left[kFrames - 1], WithinAbs(0.5f, 1e-6f));
    // The note-off arrives while the plugin is bypassed, so it never sees it.
    slot->set_bypass(true);
    block.midi_in.add(midi(0x80, 60, 0, 0));
    block.run(*slot);
    slot->set_bypass(false);
    block.run(*slot);
    REQUIRE_THAT(block.left[kFrames - 1], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("LV2 slot refuses bundles it cannot host safely", "[host][lv2][slot][lilv]") {
    SECTION("a plugin.ttl that does not parse") {
        ScratchBundle b("pulp-lv2-bad-ttl", "",
                        "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                        "<http://example.com/pulp/lv2-host-instrument> lv2:port [ { ;;\n");
        REQUIRE(b.load() == nullptr);
    }
    SECTION("a required port of a kind the host cannot connect") {
        std::string ttl = fixture_ttl();
        const std::string marker = "    lv2:port [\n";
        const auto at = ttl.find(marker);
        REQUIRE(at != std::string::npos);
        ttl.insert(at + marker.size(),
                   "        a lv2:InputPort , <http://example.com/pulp/not-a-port-class> ;\n"
                   "        lv2:index 31 ; lv2:symbol \"strange\" ; lv2:name \"Strange\"\n"
                   "    ] , [\n");
        ScratchBundle b("pulp-lv2-strange-port", "", ttl);
        REQUIRE(b.load() == nullptr);
    }
    SECTION("a plugin that requires a fixed block length") {
        ScratchBundle b("pulp-lv2-fixed-block",
                        "    lv2:requiredFeature "
                        "<http://lv2plug.in/ns/ext/buf-size#fixedBlockLength> ;\n",
                        fixture_ttl());
        REQUIRE(b.load() == nullptr);
    }
    SECTION("the control: the unchanged description loads") {
        ScratchBundle b("pulp-lv2-control", "", fixture_ttl());
        REQUIRE(b.load() != nullptr);
    }
}

TEST_CASE("LV2 slot leaves no partial event in a default-size sequence",
          "[host][lv2][slot][lilv][overflow]") {
    // Without rsz:minimumSize the sequence gets the default 8192 bytes: 340
    // three-byte events fill 8160 of the 8176 after the header, leaving 16,
    // enough for an event's timestamp and header but not its body.
    std::string ttl = fixture_ttl();
    const std::string size_decl = "rsz:minimumSize 16384 ;";
    const auto at = ttl.find(size_decl);
    REQUIRE(at != std::string::npos);
    ttl.erase(at, size_decl.size());
    ScratchBundle b("pulp-lv2-default-size", "", ttl);
    auto slot = b.load();
    REQUIRE(slot);
    REQUIRE(slot->prepare(kRate, kFrames));
    const auto amount = amount_id(*slot);
    Block block;
    slot->set_parameter(amount, 2.0f);
    block.run(*slot);
    for (int i = 0; i < 400; ++i)
        block.midi_in.add(midi(0x90, static_cast<std::uint8_t>(i % 128), 100, i * 100 / 400));
    block.params.push(pulp::host::ParameterEvent{amount, 120, 9.0f, 0});
    block.run(*slot);
    REQUIRE_THAT(slot->get_parameter(kMalformedEvents), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kNoteOns), WithinAbs(340.0f, 1e-3f));
    REQUIRE_THAT(slot->get_parameter(kSeenAmount), WithinAbs(2.0f, 1e-6f));
}
