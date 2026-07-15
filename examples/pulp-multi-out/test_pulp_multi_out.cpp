// Headless routing tests for pulp-multi-out: prove the 8-bus topology and that
// each voice renders into its OWN output bus through the richer
// process(ProcessBuffers&) surface — the format-agnostic contract every adapter
// (VST3, CLAP, AU v2) relies on.

#include "multi_out_synth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <cmath>
#include <span>

using namespace pulp;
using namespace pulp::examples::multi_out;
using format::BusDirection;
using format::BusRole;
using format::ProcessBuffers;
using format::ProcessBusBufferSet;
using format::ProcessBusBufferView;

namespace {

// RMS of one bus's first channel — a cheap "is there signal here" probe.
float bus_rms(const audio::BufferView<float>& bus) {
    if (bus.num_channels() == 0) return 0.0f;
    const auto ch = bus.channel(0);
    double acc = 0.0;
    for (float s : ch) acc += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(acc / static_cast<double>(ch.size())));
}

// Owns 8 stereo output buffers and exposes them as a ProcessBuffers with the
// same bus roles the adapters build (index 0 = Main, 1..7 = Aux).
struct MultiOutHarness {
    static constexpr std::size_t kBuses = kNumVoices;
    std::size_t frames;
    std::array<audio::Buffer<float>, kBuses> storage;
    std::array<ProcessBusBufferView<float>, kBuses> out_views{};
    std::array<ProcessBusBufferView<const float>, 1> in_views{};

    explicit MultiOutHarness(std::size_t n) : frames(n) {
        for (std::size_t b = 0; b < kBuses; ++b) {
            storage[b] = audio::Buffer<float>(2, frames);
            out_views[b] = {
                .info = {"bus", b, BusDirection::Output,
                         b == 0 ? BusRole::Main : BusRole::Aux, 2, b != 0, true},
                .buffer = storage[b].view(),
            };
        }
        in_views[0] = {.info = {"in", 0, BusDirection::Input, BusRole::Main, 0,
                                true, false},
                       .buffer = {}};
    }

    ProcessBuffers buffers() {
        return ProcessBuffers{
            .inputs = ProcessBusBufferSet<const float>(std::span(in_views)),
            .outputs = ProcessBusBufferSet<float>(std::span(out_views)),
        };
    }
};

}  // namespace

TEST_CASE("multi-out descriptor declares 8 stereo output buses; bus 0 is main",
          "[multi-out][bus]") {
    Processor proc;
    const auto desc = proc.descriptor();
    REQUIRE(desc.output_buses.size() == 8);
    REQUIRE(desc.input_buses.empty());  // instrument: no audio input
    REQUIRE(desc.output_buses[0].name == "Voice 1 (Main)");
    REQUIRE(desc.output_buses[7].name == "Voice 8");
    for (const auto& b : desc.output_buses) REQUIRE(b.default_channels == 2);
    REQUIRE(desc.category == format::PluginCategory::Instrument);
    REQUIRE(desc.accepts_midi);
}

TEST_CASE("multi-out: first note routes to voice 0 -> output bus 0 only",
          "[multi-out][routing]") {
    state::StateStore store;
    Processor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    format::PrepareContext ctx;
    ctx.sample_rate = 48000;
    ctx.max_buffer_size = 256;
    proc.prepare(ctx);

    MultiOutHarness harness(256);
    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));

    format::ProcessContext pctx;
    pctx.sample_rate = 48000;
    pctx.num_samples = 256;
    auto buffers = harness.buffers();
    proc.process(buffers, midi_in, midi_out, pctx);

    REQUIRE(bus_rms(harness.out_views[0].buffer) > 1e-4f);
    for (std::size_t b = 1; b < MultiOutHarness::kBuses; ++b) {
        INFO("bus " << b << " must be silent");
        REQUIRE(bus_rms(harness.out_views[b].buffer) == 0.0f);
    }
}

TEST_CASE("multi-out: two notes fan out to bus 0 and bus 1 (round-robin voices)",
          "[multi-out][routing]") {
    state::StateStore store;
    Processor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    format::PrepareContext ctx;
    ctx.sample_rate = 48000;
    ctx.max_buffer_size = 256;
    proc.prepare(ctx);

    MultiOutHarness harness(256);
    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));
    midi_in.add(midi::MidiEvent::note_on(0, 64, 100));

    format::ProcessContext pctx;
    pctx.sample_rate = 48000;
    pctx.num_samples = 256;
    auto buffers = harness.buffers();
    proc.process(buffers, midi_in, midi_out, pctx);

    REQUIRE(bus_rms(harness.out_views[0].buffer) > 1e-4f);
    REQUIRE(bus_rms(harness.out_views[1].buffer) > 1e-4f);
    for (std::size_t b = 2; b < MultiOutHarness::kBuses; ++b) {
        REQUIRE(bus_rms(harness.out_views[b].buffer) == 0.0f);
    }
}

TEST_CASE(
    "multi-out: disconnected aux bus (inactive) is skipped without reordering",
    "[multi-out][routing]") {
    state::StateStore store;
    Processor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    format::PrepareContext ctx;
    ctx.sample_rate = 48000;
    ctx.max_buffer_size = 256;
    proc.prepare(ctx);

    MultiOutHarness harness(256);
    // Host leaves bus 0's aux neighbor (bus 1) disconnected: mark inactive with
    // an empty buffer. The mapping stays index-aligned; voice 0 still lands on
    // bus 0, and the inactive bus is simply skipped.
    harness.out_views[1].info.active = false;
    harness.out_views[1].buffer = {};

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));  // -> voice 0 -> bus 0
    format::ProcessContext pctx;
    pctx.sample_rate = 48000;
    pctx.num_samples = 256;
    auto buffers = harness.buffers();
    // Must not touch the inactive bus (no crash on the empty buffer).
    proc.process(buffers, midi_in, midi_out, pctx);

    REQUIRE(bus_rms(harness.out_views[0].buffer) > 1e-4f);
    REQUIRE(harness.out_views[2].info.index == 2);  // mapping unchanged
}
