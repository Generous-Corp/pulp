// RT-safety coverage for the host graph hot path.
//
// SignalGraph::process() is the audio-thread entry for hosted/graph rendering.
// Its RCU snapshot design (atomic raw-pointer load under a reader-count guard,
// deliberately avoiding atomic<shared_ptr>) is meant to run with no heap
// allocation and no blocking lock. Until now that contract was only asserted
// for NativeCoreProcessor::process and StateStore writes (test_rt_safety.cpp);
// the graph walk itself had no abort-trap coverage.
//
// On UNIX this binary links rt_intercept_test_support.cpp, so an allocation or
// pthread lock taken inside the ScopedRtProcessProbe scope aborts the process
// (the strong pulp_rt_trap_if_no_alloc_scope override). Off the trap build it
// falls back to the counting RtAllocationProbe. Either backend makes
// allocation_count() == 0 the assertion.

#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace {

using pulp::host::SignalGraph;

// A 2-channel scratch block with stable channel-pointer storage so building a
// BufferView never allocates inside the measured region.
struct StereoBlock {
    explicit StereoBlock(std::size_t frames, float fill = 0.0f)
        : left(frames, fill), right(frames, fill),
          mutable_channels{left.data(), right.data()},
          const_channels{left.data(), right.data()} {}

    pulp::audio::BufferView<float> mutable_view(std::uint32_t frames) noexcept {
        return {mutable_channels.data(), mutable_channels.size(), frames};
    }
    pulp::audio::BufferView<const float> const_view(std::uint32_t frames) const noexcept {
        return {const_channels.data(), const_channels.size(), frames};
    }

    std::vector<float> left;
    std::vector<float> right;
    std::array<float*, 2> mutable_channels;
    std::array<const float*, 2> const_channels;
};

}  // namespace

TEST_CASE("SignalGraph process() audio hot path does not allocate or lock",
          "[host][graph][rt-safety][no-alloc]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(2, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));

    constexpr std::uint32_t kFrames = 64;
    REQUIRE(graph.prepare(48000.0, static_cast<int>(kFrames)));

    StereoBlock in(kFrames, 0.25f);
    StereoBlock out(kFrames, 0.0f);
    auto out_view = out.mutable_view(kFrames);
    auto in_view = in.const_view(kFrames);

    // Warm up outside the probe: the first block after prepare() is allowed to
    // touch lazily-initialized state. Steady-state blocks are the RT contract.
    graph.process(out_view, in_view, static_cast<int>(kFrames));
    graph.process(out_view, in_view, static_cast<int>(kFrames));

    std::size_t allocation_count = 0;
    std::size_t allocated_bytes = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        for (int block = 0; block < 8; ++block) {
            graph.process(out_view, in_view, static_cast<int>(kFrames));
        }
        allocation_count = probe.allocation_count();
        allocated_bytes = probe.allocated_bytes();
    }

    REQUIRE(allocation_count == 0);
    REQUIRE(allocated_bytes == 0);
}

TEST_CASE("SignalGraph first process() block after prepare() is RT-safe",
          "[host][graph][rt-safety][no-alloc]") {
    // A DAW's very first audio callback after prepare() is still realtime, so
    // the first block must be allocation/lock-free with no warm-up.
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(2, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));

    constexpr std::uint32_t kFrames = 64;
    REQUIRE(graph.prepare(48000.0, static_cast<int>(kFrames)));

    StereoBlock in(kFrames, 0.25f);
    StereoBlock out(kFrames, 0.0f);
    auto out_view = out.mutable_view(kFrames);
    auto in_view = in.const_view(kFrames);

    std::size_t allocation_count = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        graph.process(out_view, in_view, static_cast<int>(kFrames));
        allocation_count = probe.allocation_count();
    }

    REQUIRE(allocation_count == 0);
}

TEST_CASE("SignalGraph process() short and oversized blocks stay RT-safe",
          "[host][graph][rt-safety][no-alloc]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(2, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));

    constexpr std::uint32_t kMaxBlock = 128;
    // The oversized-block guard zero-fills `num_samples` (not the view's frame
    // count), so the backing storage must be >= the largest count we pass.
    constexpr std::uint32_t kOversized = kMaxBlock + 64;  // 192 > max_block_size
    constexpr std::uint32_t kCapacity = 256;              // >= kOversized
    REQUIRE(graph.prepare(48000.0, static_cast<int>(kMaxBlock)));

    StereoBlock in(kCapacity, 0.5f);
    StereoBlock out(kCapacity, 0.0f);
    auto out_view = out.mutable_view(kCapacity);
    auto in_view = in.const_view(kCapacity);

    graph.process(out_view, in_view, static_cast<int>(kMaxBlock));

    std::size_t allocation_count = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        // A partial block (the common DAW case where the host hands a smaller
        // count than max_block_size).
        graph.process(out_view, in_view, 33);
        // An oversized block takes the zero-fill guard path
        // (signal_graph.cpp: num_samples > max_block_size). Must still be
        // allocation-free. Backed by kCapacity frames so the guard's
        // num_samples-wide memset stays in bounds.
        graph.process(out_view, in_view, static_cast<int>(kOversized));
        // A non-positive block is an early return.
        graph.process(out_view, in_view, 0);
        allocation_count = probe.allocation_count();
    }

    REQUIRE(allocation_count == 0);
}

TEST_CASE("SignalGraph process() with injected MIDI stays RT-safe",
          "[host][graph][rt-safety][no-alloc][midi]") {
    SignalGraph graph;
    const auto midi_in = graph.add_midi_input_node("MIDI In");
    const auto midi_out = graph.add_midi_output_node("MIDI Out");
    REQUIRE(graph.connect_midi(midi_in, midi_out));

    constexpr std::uint32_t kFrames = 64;
    REQUIRE(graph.prepare(48000.0, static_cast<int>(kFrames)));

    pulp::midi::MidiBuffer injected;
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 8;
    injected.add(note);
    injected.add(pulp::midi::MidiEvent::cc(0, 1, 64));
    injected.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 12);
    pulp::midi::UmpBuffer injected_ump;
    injected_ump.add(pulp::midi::UmpPacket::note_on_2(0, 0, 64, 0x8000), 16);
    injected.attach_ump(&injected_ump);

    StereoBlock in(kFrames, 0.0f);
    StereoBlock out(kFrames, 0.0f);
    auto out_view = out.mutable_view(kFrames);
    auto in_view = in.const_view(kFrames);

    // Warm up with an empty mailbox, OUTSIDE the probe.
    graph.process(out_view, in_view, static_cast<int>(kFrames));

    std::size_t allocation_count = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        // A single producer may inject on the audio thread immediately before
        // process(). The fixed event, SysEx, and UMP mailbox storage must keep
        // both operations allocation- and lock-free.
        REQUIRE(graph.inject_midi(midi_in, injected));
        graph.process(out_view, in_view, static_cast<int>(kFrames));
        allocation_count = probe.allocation_count();
    }

    REQUIRE(allocation_count == 0);

    // Draining the egress mailbox is a control-thread operation; just prove it
    // is reachable after an RT-safe block.
    pulp::midi::MidiBuffer extracted;
    static_cast<void>(graph.extract_midi(midi_out, extracted));
}
