// Edge probes on the levelized parallel walk: every record must equal the one
// the routed serial walk publishes for the same graph and stimulus, observed at
// the same edge point, with one publication per block on the calling thread.

#include "harness/rt_allocation_probe.hpp"
#include "support/thread_progress.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

using pulp::graph::GraphRuntimeProbeKind;
using pulp::graph::GraphRuntimeProbeSample;
using pulp::host::NodeId;
using pulp::host::SignalGraph;
using Status = SignalGraph::EdgeProbeStatus;

constexpr int kMaxFrames = 8;
constexpr std::uint32_t kParam = 3;

// A hosted node whose output is a deterministic function of its input and its
// own block count, so a record observed one block late or from the wrong node
// is distinguishable. Clobber additionally writes through its input view after
// rendering, which an in-place reader is allowed to do to bytes only it reads.
class StageSlot final : public pulp::host::PluginSlot {
  public:
    enum class Mode { Shape, Clobber };
    static constexpr float kClobber = -7.0f;

    StageSlot(Mode mode, float scale, int latency = 0,
              pulp::host::ParamRate rate = pulp::host::ParamRate::ControlRate)
        : mode_(mode), scale_(scale), latency_(latency), rate_(rate) {
        info_.name = "parallel probe stage";
        info_.format = pulp::host::PluginFormat::CLAP;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }

    // Output of the block-th call (0-based) for input sample x.
    static float shape(float scale, std::uint64_t block, float x) {
        return scale * x + 0.01f * static_cast<float>(block + 1);
    }

    std::atomic<std::uint64_t> worker_blocks{0};
    std::thread::id audio_thread;

    const pulp::host::PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return true;
    }
    void release() override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::host::ParameterEventQueue&,
                 int frames) override {
        if (audio_thread != std::thread::id{} && std::this_thread::get_id() != audio_thread)
            worker_blocks.fetch_add(1, std::memory_order_relaxed);
        const float* in = input.num_channels() > 0 ? input.channel_ptr(0) : nullptr;
        for (std::size_t c = 0; c < output.num_channels(); ++c) {
            for (int f = 0; f < frames; ++f) {
                const float x = in != nullptr ? in[f] : 0.0f;
                output.channel_ptr(c)[f] = mode_ == Mode::Shape ? shape(scale_, block_, x) : x;
            }
        }
        if (mode_ == Mode::Clobber && in != nullptr)
            std::fill_n(const_cast<float*>(in), frames, kClobber);
        ++block_;
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        pulp::host::HostParamInfo parameter;
        parameter.id = kParam;
        parameter.name = "observed";
        parameter.min_value = 0.0f;
        parameter.max_value = 1.0f;
        parameter.flags.automatable = true;
        parameter.flags.modulatable = true;
        parameter.rate = rate_;
        return {parameter};
    }
    float get_parameter(std::uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return true;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return latency_;
    }
    int tail_samples() const override {
        return 0;
    }

  private:
    pulp::host::PluginInfo info_;
    Mode mode_;
    float scale_;
    int latency_;
    pulp::host::ParamRate rate_;
    std::uint64_t block_ = 0;
};

enum class Topology {
    Plain,    // input -> {source, sibling} -> output: one parallel level
    Pdc,      // as Plain with a latent sibling: the source edge is delayed
    Feedback, // a built-in gain feeds itself back, beside a parallel sibling
    Clobber,  // gain source -> in-place reader that writes through its input view
    Control,  // source audio drives a destination parameter, plus MIDI and a sibling
    Dense,    // as Control with audio-rate modulation
};

struct ProbeGraph {
    SignalGraph graph;
    NodeId input = 0, source = 0, sibling = 0, reader = 0, output = 0;
    NodeId midi_in = 0, midi_out = 0;
    StageSlot* source_slot = nullptr;
    StageSlot* sibling_slot = nullptr;
    Topology topology;

    ProbeGraph(Topology t, bool parallel) : topology(t) {
        input = graph.add_input_node(1, "input");
        output = graph.add_output_node(1, "output");
        auto add_slot = [&](StageSlot::Mode mode, float scale, int latency,
                            pulp::host::ParamRate rate, StageSlot** out) {
            auto slot = std::make_unique<StageSlot>(mode, scale, latency, rate);
            *out = slot.get();
            return graph.add_plugin_node(std::move(slot), 1, 1);
        };
        StageSlot* reader_slot = nullptr;
        const auto rate = t == Topology::Dense ? pulp::host::ParamRate::AudioRate
                                               : pulp::host::ParamRate::ControlRate;
        if (t == Topology::Feedback) {
            source = graph.add_gain_node("feedback");
            graph.set_node_gain(source, 0.5f);
            REQUIRE(graph.connect(input, 0, source, 0));
            REQUIRE(graph.connect_feedback(source, 0, source, 0));
            REQUIRE(graph.connect(source, 0, output, 0));
        } else if (t == Topology::Clobber) {
            // A built-in gain's output is not retained across blocks, so its
            // sole reader reads it in place; a hosted node's output is retained
            // and always copied.
            source = graph.add_gain_node("in-place source");
            graph.set_node_gain(source, 0.5f);
            REQUIRE(graph.connect(input, 0, source, 0));
        } else {
            source = add_slot(StageSlot::Mode::Shape, 0.5f, 0, rate, &source_slot);
            REQUIRE(graph.connect(input, 0, source, 0));
        }
        sibling = add_slot(StageSlot::Mode::Shape, 0.25f, t == Topology::Pdc ? 3 : 0, rate,
                           &sibling_slot);
        REQUIRE(graph.connect(input, 0, sibling, 0));
        REQUIRE(graph.connect(sibling, 0, output, 0));
        switch (t) {
        case Topology::Plain:
        case Topology::Pdc:
            REQUIRE(graph.connect(source, 0, output, 0));
            break;
        case Topology::Feedback:
            break;
        case Topology::Clobber:
            reader = add_slot(StageSlot::Mode::Clobber, 1.0f, 0, rate, &reader_slot);
            REQUIRE(graph.connect(source, 0, reader, 0));
            REQUIRE(graph.connect(reader, 0, output, 0));
            break;
        case Topology::Control:
        case Topology::Dense:
            REQUIRE(graph.connect(source, 0, output, 0));
            if (t == Topology::Control)
                REQUIRE(graph.connect_automation(input, 0, sibling, kParam, 0.0f, 1.0f));
            else
                REQUIRE(graph.connect_audio_rate_modulation(input, 0, sibling, kParam, 0.0f, 1.0f));
            midi_in = graph.add_midi_input_node("notes");
            midi_out = graph.add_midi_output_node("notes-out");
            REQUIRE(graph.connect_midi(midi_in, midi_out));
            break;
        }
        graph.set_canonical_executor_routing_enabled(true);
        graph.set_parallel_routing_enabled(parallel);
        REQUIRE(graph.prepare(48000.0, kMaxFrames));
        const auto status = graph.routed_execution_status(kMaxFrames);
        REQUIRE(status.parallel_selected == parallel);
        if (parallel) {
            REQUIRE(status.parallel_snapshot_valid);
            REQUIRE(status.worker_pool_running);
        }
        const auto me = std::this_thread::get_id();
        for (auto* slot : {source_slot, sibling_slot, reader_slot})
            if (slot != nullptr)
                slot->audio_thread = me;
    }

    pulp::host::PortHandle audio_source() const {
        return graph.node(source)->out("audio");
    }
    pulp::host::PortHandle audio_destination() const {
        switch (topology) {
        case Topology::Feedback:
            return graph.node(source)->in("audio");
        case Topology::Clobber:
            return graph.node(reader)->in("audio");
        default:
            return graph.node(output)->in("audio");
        }
    }
    Status attach_audio() {
        return graph.attach_audio_edge_probe(audio_source(), audio_destination());
    }
    Status attach_control() {
        return graph.attach_control_edge_probe(graph.node(input)->out("audio"), sibling, kParam);
    }
    Status attach_event() {
        return graph.attach_event_edge_probe(graph.node(midi_in)->out("audio"),
                                             graph.node(midi_out)->in("audio"));
    }
};

// Block stimulus: a ramp in [0, 1) that differs per block and per sample.
float stimulus(std::uint64_t timestamp, int frame) {
    return std::fmod(
        static_cast<float>(timestamp + static_cast<std::uint64_t>(frame) + 1) * 0.0371f, 1.0f);
}

struct BlockIo {
    std::array<float, 2 * kMaxFrames> in{};
    std::array<float, 2 * kMaxFrames> out{};
};

// One callback through the public process() entry, with no allocation of its
// own so an allocation probe around it sees only the graph.
void process_block(SignalGraph& graph, BlockIo& io, std::uint64_t timestamp, int frames) {
    for (int f = 0; f < frames; ++f)
        io.in[static_cast<std::size_t>(f)] = stimulus(timestamp, f);
    const float* ip = io.in.data();
    float* op = io.out.data();
    pulp::audio::BufferView<const float> iv(&ip, 1, static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> ov(&op, 1, static_cast<std::uint32_t>(frames));
    graph.process(ov, iv, frames);
}

template <typename Read> std::vector<GraphRuntimeProbeSample> drain(Read&& read) {
    std::vector<GraphRuntimeProbeSample> samples;
    GraphRuntimeProbeSample sample;
    while (read(sample))
        samples.push_back(sample);
    return samples;
}

void require_same_record(const GraphRuntimeProbeSample& a, const GraphRuntimeProbeSample& b) {
    REQUIRE(a.kind == b.kind);
    REQUIRE(a.sequence == b.sequence);
    REQUIRE(a.timestamp_samples == b.timestamp_samples);
    REQUIRE(a.edge_index == b.edge_index);
    REQUIRE(a.frames == b.frames);
    REQUIRE(a.peak == b.peak);
    REQUIRE(a.rms == b.rms);
    REQUIRE(a.value == b.value);
    REQUIRE(a.value_end == b.value_end);
    REQUIRE(a.payload_id == b.payload_id);
    REQUIRE(a.payload_count == b.payload_count);
    REQUIRE(a.payload_status == b.payload_status);
    REQUIRE(a.payload_offset == b.payload_offset);
    REQUIRE(a.payload_dropped == b.payload_dropped);
}

// Peak and rms of the expected observation, computed from the stimulus alone.
struct Summary {
    float peak = 0.0f;
    float rms = 0.0f;
};
template <typename F> Summary summarize(int frames, F&& value) {
    Summary s;
    double squares = 0.0;
    for (int f = 0; f < frames; ++f) {
        const float v = value(f);
        s.peak = std::max(s.peak, std::abs(v));
        squares += static_cast<double>(v) * v;
    }
    s.rms = static_cast<float>(std::sqrt(squares / frames));
    return s;
}

const std::vector<int>& partitions() {
    static const std::vector<int> p{8, 3, 5, 1, 8, 7, 2, 8, 4, 6};
    return p;
}

struct Run {
    std::vector<std::vector<float>> output;
    std::vector<GraphRuntimeProbeSample> audio, control, event;
    std::uint64_t parallel_levels = 0;
    bool allocated = false;
};

Run run_topology(Topology t, bool parallel) {
    ProbeGraph g(t, parallel);
    const bool lanes = t == Topology::Control || t == Topology::Dense;
    REQUIRE(g.attach_audio() == Status::Attached);
    if (lanes) {
        REQUIRE(g.attach_control() == Status::Attached);
        REQUIRE(g.attach_event() == Status::Attached);
    }
    const auto identity = g.graph.audio_edge_probe_identity();
    REQUIRE(identity.connection != 0);
    REQUIRE(identity.source == g.source);
    REQUIRE(pulp::test::RtAllocationProbe::self_check());
    Run run;
    BlockIo io;
    pulp::midi::MidiBuffer notes;
    notes.reserve(2);
    std::uint64_t timestamp = 0;
    std::uint64_t block = 0;
    for (const int frames : partitions()) {
        if (lanes) {
            notes.clear();
            auto note =
                pulp::midi::MidiEvent::note_on(static_cast<std::uint8_t>(block % 16),
                                               static_cast<std::uint8_t>(60 + block % 12), 100);
            note.sample_offset =
                static_cast<std::int32_t>(block % static_cast<std::uint64_t>(frames));
            REQUIRE(notes.add(note));
            REQUIRE(g.graph.inject_midi(g.midi_in, notes));
        }
        {
            pulp::test::RtAllocationProbe allocation;
            process_block(g.graph, io, timestamp, frames);
            run.allocated = run.allocated || allocation.saw_allocation();
        }
        run.output.emplace_back(io.out.begin(), io.out.begin() + frames);
        timestamp += static_cast<std::uint64_t>(frames);
        ++block;
    }
    REQUIRE(g.graph.audio_edge_probe_identity().connection == identity.connection);
    run.audio = drain([&](GraphRuntimeProbeSample& s) { return g.graph.read_audio_edge_probe(s); });
    run.control =
        drain([&](GraphRuntimeProbeSample& s) { return g.graph.read_control_edge_probe(s); });
    run.event = drain([&](GraphRuntimeProbeSample& s) { return g.graph.read_event_edge_probe(s); });
    run.parallel_levels = g.graph.routing_executor_stats().parallel_levels_dispatched;
    return run;
}

// Expected audio observation for the source edge of each topology.
void require_expected_audio(Topology t, const std::vector<GraphRuntimeProbeSample>& records) {
    REQUIRE(records.size() == partitions().size());
    std::uint64_t timestamp = 0;
    std::array<float, kMaxFrames> retained{};
    for (std::size_t block = 0; block < records.size(); ++block) {
        const int frames = partitions()[block];
        const auto& r = records[block];
        Summary expected;
        if (t == Topology::Feedback) {
            // The feedback edge carries last block's output (0.5 * (x + fb)).
            expected = summarize(frames, [&](int f) { return retained[f]; });
            for (int f = 0; f < frames; ++f)
                retained[f] = 0.5f * (stimulus(timestamp, f) + retained[f]);
        } else if (t == Topology::Clobber) {
            expected = summarize(frames, [&](int f) { return 0.5f * stimulus(timestamp, f); });
        } else {
            expected = summarize(frames, [&](int f) {
                return StageSlot::shape(0.5f, block, stimulus(timestamp, f));
            });
        }
        CAPTURE(block, frames);
        REQUIRE(r.kind == GraphRuntimeProbeKind::Audio);
        REQUIRE(r.sequence == block);
        REQUIRE(r.timestamp_samples == timestamp);
        REQUIRE(r.frames == static_cast<std::uint32_t>(frames));
        REQUIRE(r.peak == expected.peak);
        REQUIRE(r.rms == expected.rms);
        timestamp += static_cast<std::uint64_t>(frames);
    }
}

void require_equivalent(const Run& serial, const Run& parallel) {
    REQUIRE(serial.parallel_levels == 0);
    REQUIRE(parallel.parallel_levels > 0);
    REQUIRE_FALSE(serial.allocated);
    REQUIRE_FALSE(parallel.allocated);
    REQUIRE(serial.output == parallel.output);
    for (const auto lane : {&Run::audio, &Run::control, &Run::event}) {
        REQUIRE((serial.*lane).size() == (parallel.*lane).size());
        for (std::size_t i = 0; i < (serial.*lane).size(); ++i)
            require_same_record((serial.*lane)[i], (parallel.*lane)[i]);
    }
}

} // namespace

TEST_CASE("Parallel audio edge probe publishes the serial walk's records",
          "[host][graph][probe][parallel]") {
    for (const auto t : {Topology::Plain, Topology::Pdc, Topology::Feedback, Topology::Clobber}) {
        CAPTURE(static_cast<int>(t));
        const auto serial = run_topology(t, false);
        const auto parallel = run_topology(t, true);
        require_expected_audio(t, serial.audio);
        require_expected_audio(t, parallel.audio);
        require_equivalent(serial, parallel);
    }
}

TEST_CASE("Parallel PDC graph runs the parallel domain and probes the undelayed source",
          "[host][graph][probe][parallel]") {
    ProbeGraph g(Topology::Pdc, true);
    const auto status = g.graph.routed_execution_status(kMaxFrames);
    REQUIRE(status.parallel_selected);
    REQUIRE_FALSE(status.serial_selected);
    REQUIRE(g.attach_audio() == Status::Attached);
    BlockIo io;
    process_block(g.graph, io, 0, kMaxFrames);
    GraphRuntimeProbeSample sample;
    REQUIRE(g.graph.read_audio_edge_probe(sample));
    const auto expected =
        summarize(kMaxFrames, [](int f) { return StageSlot::shape(0.5f, 0, stimulus(0, f)); });
    REQUIRE(sample.peak == expected.peak);
    REQUIRE(sample.rms == expected.rms);
    // PDC delays the source edge by the sibling's latency: the output's first
    // three samples carry the sibling alone, so the probe saw the source before
    // the delay rather than the delayed signal the output mixes.
    for (int f = 0; f < kMaxFrames; ++f) {
        float want = StageSlot::shape(0.25f, 0, stimulus(0, f));
        if (f >= 3)
            want += StageSlot::shape(0.5f, 0, stimulus(0, f - 3));
        CAPTURE(f);
        REQUIRE(io.out[static_cast<std::size_t>(f)] == want);
    }
}

TEST_CASE("Parallel control and event probes publish the serial walk's records",
          "[host][graph][probe][parallel]") {
    for (const auto t : {Topology::Control, Topology::Dense}) {
        CAPTURE(static_cast<int>(t));
        const auto serial = run_topology(t, false);
        const auto parallel = run_topology(t, true);
        require_expected_audio(t, serial.audio);
        require_equivalent(serial, parallel);
        REQUIRE(parallel.control.size() == partitions().size());
        REQUIRE(parallel.event.size() == partitions().size());
        std::uint64_t timestamp = 0;
        for (std::size_t block = 0; block < partitions().size(); ++block) {
            const int frames = partitions()[block];
            const auto& c = parallel.control[block];
            const auto& e = parallel.event[block];
            CAPTURE(block, frames);
            REQUIRE(c.kind == GraphRuntimeProbeKind::Control);
            REQUIRE(c.sequence == block);
            REQUIRE(c.timestamp_samples == timestamp);
            REQUIRE(c.payload_id == kParam);
            REQUIRE(c.value == stimulus(timestamp, 0));
            REQUIRE(c.value_end == stimulus(timestamp, frames - 1));
            if (t == Topology::Dense) {
                const auto expected =
                    summarize(frames, [&](int f) { return stimulus(timestamp, f); });
                REQUIRE(c.peak == expected.peak);
                REQUIRE(c.rms == expected.rms);
                REQUIRE(c.payload_count == static_cast<std::uint32_t>(frames));
            }
            REQUIRE(e.kind == GraphRuntimeProbeKind::Event);
            REQUIRE(e.sequence == block);
            REQUIRE(e.timestamp_samples == timestamp);
            REQUIRE(e.payload_count == 1);
            REQUIRE(e.payload_offset ==
                    static_cast<std::int32_t>(block % static_cast<std::uint64_t>(frames)));
            REQUIRE(e.payload_status == 0x90 + block % 16);
            timestamp += static_cast<std::uint64_t>(frames);
        }
    }
}

TEST_CASE("Parallel probe keeps its clock and records across a backend switch",
          "[host][graph][probe][parallel]") {
    ProbeGraph g(Topology::Plain, true);
    REQUIRE(g.attach_audio() == Status::Attached);
    BlockIo io;
    std::uint64_t timestamp = 0;
    std::vector<std::uint64_t> dispatched;
    for (int phase = 0; phase < 3; ++phase) {
        g.graph.set_parallel_routing_enabled(phase != 1);
        for (int block = 0; block < 2; ++block) {
            process_block(g.graph, io, timestamp, 5);
            timestamp += 5;
        }
        dispatched.push_back(g.graph.routing_executor_stats().parallel_levels_dispatched);
    }
    REQUIRE(dispatched[0] > 0);
    REQUIRE(dispatched[1] == dispatched[0]); // the serial phase dispatched nothing
    REQUIRE(dispatched[2] > dispatched[1]);
    REQUIRE(g.graph.audio_edge_probe_status() == Status::Attached);
    const auto records =
        drain([&](GraphRuntimeProbeSample& s) { return g.graph.read_audio_edge_probe(s); });
    REQUIRE(records.size() == 6);
    for (std::size_t i = 0; i < records.size(); ++i) {
        REQUIRE(records[i].sequence == i);
        REQUIRE(records[i].timestamp_samples == i * 5);
        const auto expected =
            summarize(5, [&](int f) { return StageSlot::shape(0.5f, i, stimulus(i * 5, f)); });
        REQUIRE(records[i].peak == expected.peak);
        REQUIRE(records[i].rms == expected.rms);
    }
}

TEST_CASE("Parallel probe with a slow reader drops the newest records and counts them",
          "[host][graph][probe][parallel]") {
    ProbeGraph g(Topology::Plain, true);
    REQUIRE(g.attach_audio() == Status::Attached);
    BlockIo io;
    constexpr int kBlocks = 300;
    for (int block = 0; block < kBlocks; ++block)
        process_block(g.graph, io, static_cast<std::uint64_t>(block) * kMaxFrames, kMaxFrames);
    REQUIRE(g.graph.audio_edge_probe_dropped() == kBlocks - 256);
    const auto records =
        drain([&](GraphRuntimeProbeSample& s) { return g.graph.read_audio_edge_probe(s); });
    REQUIRE(records.size() == 256);
    for (std::size_t i = 0; i < records.size(); ++i) {
        REQUIRE(records[i].sequence == i);
        REQUIRE(records[i].timestamp_samples == i * kMaxFrames);
    }
    // Room again: the next block's record follows the clock, not the drops.
    process_block(g.graph, io, kBlocks * kMaxFrames, kMaxFrames);
    GraphRuntimeProbeSample sample;
    REQUIRE(g.graph.read_audio_edge_probe(sample));
    REQUIRE(sample.sequence == 256);
    REQUIRE(sample.timestamp_samples == kBlocks * kMaxFrames);
}

TEST_CASE("Parallel probe refusals and stops stay truthful", "[host][graph][probe][parallel]") {
    ProbeGraph g(Topology::Plain, true);
    g.graph.set_anticipation_enabled(true);
    REQUIRE(g.attach_audio() == Status::UnsupportedExecution);
    g.graph.set_anticipation_enabled(false);
    g.graph.set_canonical_executor_routing_enabled(false);
    REQUIRE(g.attach_audio() == Status::UnsupportedExecution);
    g.graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.attach_audio() == Status::Attached);

    // A block larger than the prepared maximum runs no walk: as on the serial
    // walk it yields no record and does not advance the probe clock.
    BlockIo io;
    process_block(g.graph, io, 0, kMaxFrames);
    std::array<float, 2 * kMaxFrames> big_in{}, big_out{};
    const float* ip = big_in.data();
    float* op = big_out.data();
    pulp::audio::BufferView<const float> iv(&ip, 1, 2 * kMaxFrames);
    pulp::audio::BufferView<float> ov(&op, 1, 2 * kMaxFrames);
    g.graph.process(ov, iv, 2 * kMaxFrames);
    process_block(g.graph, io, kMaxFrames, kMaxFrames);
    REQUIRE(g.graph.audio_edge_probe_status() == Status::Attached);
    const auto records =
        drain([&](GraphRuntimeProbeSample& s) { return g.graph.read_audio_edge_probe(s); });
    REQUIRE(records.size() == 2);
    REQUIRE(records[1].timestamp_samples == kMaxFrames);

    // A latent graph prepared without any routed executor runs the legacy
    // domain, which still refuses even once routing is switched back on.
    SignalGraph legacy;
    const auto in = legacy.add_input_node(1);
    const auto out = legacy.add_output_node(1);
    const auto latent =
        legacy.add_plugin_node(std::make_unique<StageSlot>(StageSlot::Mode::Shape, 1.0f, 2), 1, 1);
    REQUIRE(legacy.connect(in, 0, latent, 0));
    REQUIRE(legacy.connect(latent, 0, out, 0));
    REQUIRE(legacy.connect(in, 0, out, 0));
    legacy.set_canonical_executor_routing_enabled(false);
    legacy.set_parallel_routing_enabled(false);
    REQUIRE(legacy.prepare(48000.0, kMaxFrames));
    legacy.set_canonical_executor_routing_enabled(true);
    legacy.set_parallel_routing_enabled(true);
    REQUIRE(legacy.attach_audio_edge_probe(legacy.node(in)->out("audio"),
                                           legacy.node(out)->in("audio")) ==
            Status::UnsupportedExecution);
}

TEST_CASE("Parallel probe attach/detach hammered against running workers stays coherent",
          "[host][graph][probe][parallel][rt-safety]") {
    ProbeGraph g(Topology::Control, true);
    auto& graph = g.graph;
    REQUIRE(pulp::test::RtAllocationProbe::self_check());
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> blocks{0};
    std::atomic<bool> allocated{false};
    std::thread audio([&] {
        for (auto* slot : {g.source_slot, g.sibling_slot})
            slot->audio_thread = std::this_thread::get_id();
        BlockIo io;
        pulp::test::RtAllocationProbe allocation;
        while (!stop.load(std::memory_order_acquire)) {
            process_block(graph, io, 0, kMaxFrames);
            blocks.fetch_add(1, std::memory_order_relaxed);
            // A device callback leaves the graph between blocks; those gaps
            // are when displaced probes can be reclaimed.
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
        allocated.store(allocation.saw_allocation());
    });
    for (int spins = 0; spins < 5'000'000 && blocks.load(std::memory_order_relaxed) == 0; ++spins)
        std::this_thread::yield();
    const bool audio_started = blocks.load(std::memory_order_relaxed) > 0;
    if (!audio_started) {
        stop.store(true, std::memory_order_release);
        audio.join();
    }
    REQUIRE(audio_started);

    // Hammer until both the cycle count and enough concurrent blocks are met.
    constexpr int kCycles = 2000;
    constexpr std::uint64_t kMinBlocks = 2000;
    int attached = 0, busy = 0;
    std::size_t peak_parked = 0;
    bool coherent = true;
    std::uint64_t audio_records = 0, control_records = 0;
    const auto tally = [&](Status status) {
        if (status == Status::Attached)
            ++attached;
        else if (status == Status::Busy)
            ++busy;
    };
    int cycles = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (int cycle = 0; (cycle < kCycles || blocks.load(std::memory_order_relaxed) < kMinBlocks) &&
                        std::chrono::steady_clock::now() < deadline;
         ++cycle, ++cycles) {
        // Re-attach on a stride so attachments live across blocks and their
        // records are read while workers keep producing into them.
        if (cycle % 16 == 0)
            tally(g.attach_audio());
        if (cycle % 24 == 0)
            tally(g.attach_control());
        peak_parked = std::max(peak_parked, graph.parked_edge_probe_count());
        GraphRuntimeProbeSample sample;
        std::uint64_t last_audio_sequence = 0;
        bool first_audio = true;
        while (graph.read_audio_edge_probe(sample)) {
            // Within one attachment the records are gap-free in sequence and
            // name whole blocks; the shaped source never reads as silence.
            coherent =
                coherent && sample.kind == GraphRuntimeProbeKind::Audio &&
                sample.frames == kMaxFrames && sample.timestamp_samples % kMaxFrames == 0 &&
                sample.peak > 0.0f &&
                (first_audio || sample.sequence == last_audio_sequence + 1 || sample.sequence == 0);
            first_audio = false;
            last_audio_sequence = sample.sequence;
            ++audio_records;
        }
        while (graph.read_control_edge_probe(sample)) {
            coherent = coherent && sample.kind == GraphRuntimeProbeKind::Control &&
                       sample.frames == kMaxFrames && sample.payload_id == kParam;
            ++control_records;
        }
        if (cycle % 64 == 40)
            graph.detach_audio_edge_probe();
        if (cycle % 96 == 50)
            graph.detach_control_edge_probe();
        std::this_thread::yield();
    }
    // Keep the pool hot until a probed level has demonstrably run on a worker
    // thread, so the hammer covered a cross-thread producer.
    const bool crossed = pulp::test::pump_until(
        [&] {
            return g.source_slot->worker_blocks.load() + g.sibling_slot->worker_blocks.load() > 0;
        },
        [&] { std::this_thread::yield(); });
    stop.store(true, std::memory_order_release);
    audio.join();
    CAPTURE(cycles, attached, peak_parked, busy, audio_records, control_records, blocks.load(),
            g.source_slot->worker_blocks.load(), g.sibling_slot->worker_blocks.load());
    REQUIRE(crossed);
    REQUIRE(coherent);
    REQUIRE_FALSE(allocated.load());
    REQUIRE(cycles >= kCycles);
    REQUIRE(blocks.load() >= kMinBlocks);
    REQUIRE(attached + busy == (cycles + 15) / 16 + (cycles + 23) / 24);
    REQUIRE(attached > 0);
    REQUIRE(audio_records > 0);
    REQUIRE(control_records > 0);
    REQUIRE(peak_parked <= 2 * pulp::format::EdgeProbeCell<int>::kParkCapacity);
    REQUIRE(graph.routing_executor_stats().parallel_levels_dispatched > 0);

    graph.detach_audio_edge_probe();
    graph.detach_control_edge_probe();
    REQUIRE(graph.parked_edge_probe_count() == 0);
    REQUIRE(g.attach_audio() == Status::Attached);
    BlockIo io;
    for (int block = 0; block < 3; ++block)
        process_block(graph, io, 0, kMaxFrames);
    GraphRuntimeProbeSample sample;
    for (std::uint64_t record = 0; record < 3; ++record) {
        REQUIRE(graph.read_audio_edge_probe(sample));
        REQUIRE(sample.sequence == record);
        REQUIRE(sample.timestamp_samples == record * kMaxFrames);
    }
    REQUIRE_FALSE(graph.read_audio_edge_probe(sample));
}

TEST_CASE("Parallel probe overhead per callback", "[.][bench][host][graph][probe][parallel]") {
    for (const bool parallel : {false, true}) {
        for (const bool probed : {false, true}) {
            ProbeGraph g(Topology::Control, parallel);
            if (probed) {
                REQUIRE(g.attach_audio() == Status::Attached);
                REQUIRE(g.attach_control() == Status::Attached);
                REQUIRE(g.attach_event() == Status::Attached);
            }
            BlockIo io;
            constexpr int kWarm = 2000, kBlocks = 200000;
            GraphRuntimeProbeSample sample;
            for (int b = 0; b < kWarm; ++b)
                process_block(g.graph, io, 0, kMaxFrames);
            const auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < kBlocks; ++b) {
                process_block(g.graph, io, 0, kMaxFrames);
                if ((b & 63) == 0) {
                    while (g.graph.read_audio_edge_probe(sample)) {
                    }
                    while (g.graph.read_control_edge_probe(sample)) {
                    }
                    while (g.graph.read_event_edge_probe(sample)) {
                    }
                }
            }
            const double ns =
                std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                    .count() /
                kBlocks;
            std::printf("probe-overhead parallel=%d probed=%d ns_per_block=%.1f\n",
                        parallel ? 1 : 0, probed ? 1 : 0, ns);
        }
    }
}
